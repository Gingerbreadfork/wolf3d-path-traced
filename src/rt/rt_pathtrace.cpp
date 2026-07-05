// rt_pathtrace.cpp
//
// Vulkan hardware path tracer using VK_KHR_ray_query in a compute shader.
// Builds acceleration structures from the extracted scene each frame (boxes for
// walls/doors/pushwalls, planes for floor/ceiling, camera-facing alpha-tested
// cards for sprites), uploads the classic textures as a 2D array atlas, dispatches
// the tracer, tone maps on the GPU, and reads the result back for compositing.
//
// The tracer is deterministic (fixed AA/AO offsets, hard shadows, no RNG and no
// temporal accumulation), so a static scene renders byte-identical frames.

#include "rt_pathtrace.h"
#include "rt_vulkan.h"
#include "rt_materials.h"
#include <vulkan/vulkan.h>
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <vector>
#include <chrono>

using namespace rtvk;

// Runtime-resolved shader directory (platform_files.cpp) so a downloaded build
// finds its *.spv next to the executable instead of at the build-time path.
extern "C" const char *PLAT_ShaderDir(void);

namespace rtpt {

// ---- runtime settings ------------------------------------------------------
// Render at the classic 3D-viewport aspect (320x160 -> 960x480) so the world
// framing matches the classic raycaster; the HUD strip is composited below.
static int   g_renderW = 960, g_renderH = 480;
static int   g_bounces = 1;
static float g_ambient = 0.70f;
static float g_shadowStrength = 0.92f;
static float g_exposure = 1.4f;
static bool  g_ready = false;

// ---- RT device function pointers -------------------------------------------
static PFN_vkGetAccelerationStructureBuildSizesKHR      pvkGetASBuildSizes = nullptr;
static PFN_vkCreateAccelerationStructureKHR             pvkCreateAS = nullptr;
static PFN_vkDestroyAccelerationStructureKHR            pvkDestroyAS = nullptr;
static PFN_vkCmdBuildAccelerationStructuresKHR          pvkCmdBuildAS = nullptr;
static PFN_vkGetAccelerationStructureDeviceAddressKHR   pvkGetASAddr = nullptr;

// ---- GPU resources ---------------------------------------------------------
struct BLAS {
    VkAccelerationStructureKHR as = VK_NULL_HANDLE;
    Buffer buffer;
    VkDeviceAddress address = 0;
};
static BLAS   g_cubeBlas, g_quadBlas;
static Buffer g_cubeVtx, g_cubeIdx, g_quadVtx, g_quadIdx;

static VkAccelerationStructureKHR g_tlas = VK_NULL_HANDLE;
static Buffer g_tlasBuffer, g_tlasScratch, g_instanceBuffer;

static Image   g_accumImage, g_outImage;
static Buffer  g_readback;
static Image   g_atlas;                 // 2D array texture
static VkImageView g_atlasView = VK_NULL_HANDLE;
static VkSampler   g_sampler = VK_NULL_HANDLE;
static int     g_atlasLayers = 0;
static int     g_wallPages = 0;
static int     g_atlasLevel = -999;

static Buffer  g_instData, g_lightData, g_params;

static VkDescriptorSetLayout g_descLayout = VK_NULL_HANDLE;
static VkDescriptorPool      g_descPool = VK_NULL_HANDLE;
static VkDescriptorSet       g_descSet = VK_NULL_HANDLE;
static VkPipelineLayout      g_pipeLayout = VK_NULL_HANDLE;
static VkPipeline            g_pipe = VK_NULL_HANDLE;
static VkPipeline            g_postPipe = VK_NULL_HANDLE;   // bloom + tone map

// GPU-side instance record (matches shader `Inst`)
struct InstGPU {
    int32_t type, texA, texB, flags;
    float   origin[4];
    float   axis[4];
    float   material[4];
};
struct LightGPU { float posType[4]; float color[4]; float params[4]; };
struct ParamsGPU {
    float camPos[4], camDir[4], camRight[4], camUp[4];
    int32_t counts[4];
    float floorColor[4], ceilColor[4], tune[4];
};

// ---------------------------------------------------------------------------
static void LoadRTFunctions() {
    VkDevice d = ctx().device;
    pvkGetASBuildSizes = (PFN_vkGetAccelerationStructureBuildSizesKHR)
        vkGetDeviceProcAddr(d, "vkGetAccelerationStructureBuildSizesKHR");
    pvkCreateAS = (PFN_vkCreateAccelerationStructureKHR)
        vkGetDeviceProcAddr(d, "vkCreateAccelerationStructureKHR");
    pvkDestroyAS = (PFN_vkDestroyAccelerationStructureKHR)
        vkGetDeviceProcAddr(d, "vkDestroyAccelerationStructureKHR");
    pvkCmdBuildAS = (PFN_vkCmdBuildAccelerationStructuresKHR)
        vkGetDeviceProcAddr(d, "vkCmdBuildAccelerationStructuresKHR");
    pvkGetASAddr = (PFN_vkGetAccelerationStructureDeviceAddressKHR)
        vkGetDeviceProcAddr(d, "vkGetAccelerationStructureDeviceAddressKHR");
}

static VkDeviceAddress BufAddr(const Buffer &b) { return b.address; }

// Build a bottom-level AS from a triangle mesh.
static BLAS BuildBLAS(const float *verts, uint32_t vcount,
                      const uint32_t *indices, uint32_t icount,
                      Buffer &vtxOut, Buffer &idxOut) {
    vtxOut = CreateBuffer(vcount * 3 * sizeof(float),
        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    memcpy(vtxOut.mapped, verts, vcount * 3 * sizeof(float));

    idxOut = CreateBuffer(icount * sizeof(uint32_t),
        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    memcpy(idxOut.mapped, indices, icount * sizeof(uint32_t));

    VkAccelerationStructureGeometryKHR geom{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    geom.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
    geom.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
    geom.geometry.triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
    geom.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
    geom.geometry.triangles.vertexData.deviceAddress = BufAddr(vtxOut);
    geom.geometry.triangles.vertexStride = 3 * sizeof(float);
    geom.geometry.triangles.maxVertex = vcount - 1;
    geom.geometry.triangles.indexType = VK_INDEX_TYPE_UINT32;
    geom.geometry.triangles.indexData.deviceAddress = BufAddr(idxOut);

    uint32_t primCount = icount / 3;
    VkAccelerationStructureBuildGeometryInfoKHR bi{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    bi.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    bi.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    bi.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    bi.geometryCount = 1;
    bi.pGeometries = &geom;

    VkAccelerationStructureBuildSizesInfoKHR sizes{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    pvkGetASBuildSizes(ctx().device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
                       &bi, &primCount, &sizes);

    BLAS blas;
    blas.buffer = CreateBuffer(sizes.accelerationStructureSize,
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    VkAccelerationStructureCreateInfoKHR ci{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
    ci.buffer = blas.buffer.buffer;
    ci.size = sizes.accelerationStructureSize;
    ci.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    pvkCreateAS(ctx().device, &ci, nullptr, &blas.as);

    Buffer scratch = CreateBuffer(sizes.buildScratchSize,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    bi.dstAccelerationStructure = blas.as;
    bi.scratchData.deviceAddress = BufAddr(scratch);

    VkAccelerationStructureBuildRangeInfoKHR range{};
    range.primitiveCount = primCount;
    const VkAccelerationStructureBuildRangeInfoKHR *pRange = &range;

    VkCommandBuffer cb = BeginOneShot();
    pvkCmdBuildAS(cb, 1, &bi, &pRange);
    EndOneShot(cb);
    DestroyBuffer(scratch);

    VkAccelerationStructureDeviceAddressInfoKHR ai{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
    ai.accelerationStructure = blas.as;
    blas.address = pvkGetASAddr(ctx().device, &ai);
    return blas;
}

static void BuildGeometryBLAS() {
    // Unit cube [0,1]^3
    static const float cubeV[] = {
        0,0,0, 1,0,0, 1,1,0, 0,1,0,   // z=0
        0,0,1, 1,0,1, 1,1,1, 0,1,1,   // z=1
    };
    static const uint32_t cubeI[] = {
        0,1,2, 0,2,3,       // bottom
        4,6,5, 4,7,6,       // top
        0,5,1, 0,4,5,       // y=0
        3,2,6, 3,6,7,       // y=1
        0,7,4, 0,3,7,       // x=0
        1,5,6, 1,6,2,       // x=1
    };
    g_cubeBlas = BuildBLAS(cubeV, 8, cubeI, 36, g_cubeVtx, g_cubeIdx);

    // Unit quad in XY plane, [-0.5,0.5]^2 at z=0, normal +Z
    static const float quadV[] = {
        -0.5f,-0.5f,0,  0.5f,-0.5f,0,  0.5f,0.5f,0,  -0.5f,0.5f,0,
    };
    static const uint32_t quadI[] = { 0,1,2, 0,2,3 };
    g_quadBlas = BuildBLAS(quadV, 4, quadI, 6, g_quadVtx, g_quadIdx);
}

// ---------------------------------------------------------------------------
static void BuildAtlas(int /*level*/) {
    int wall = mat::WallPageCount();
    int spr  = mat::SpritePageCount();
    g_wallPages = wall;
    g_atlasLayers = wall + spr;

    const int TS = mat::kTexSize;
    std::vector<uint32_t> pixels((size_t)TS * TS * g_atlasLayers);
    for (int p = 0; p < wall; ++p)
        mat::DecodeWall(p, &pixels[(size_t)p * TS * TS]);
    for (int s = 0; s < spr; ++s)
        mat::DecodeSprite(s, &pixels[(size_t)(wall + s) * TS * TS]);

    if (g_atlasView) { vkDestroyImageView(ctx().device, g_atlasView, nullptr); g_atlasView = VK_NULL_HANDLE; }
    if (g_atlas.image) DestroyImage(g_atlas);

    // Create a 2D array image manually (CreateImage helper is single-layer).
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_R8G8B8A8_UNORM;
    ii.extent = {(uint32_t)TS, (uint32_t)TS, 1};
    ii.mipLevels = 1;
    ii.arrayLayers = g_atlasLayers;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(ctx().device, &ii, nullptr, &g_atlas.image) != VK_SUCCESS || !g_atlas.image) {
        printf("[pt] atlas image creation failed (%d layers)\n", g_atlasLayers);
        g_atlas = Image{}; return;   // g_atlasView stays null -> Render() bails
    }

    VkMemoryRequirements req; vkGetImageMemoryRequirements(ctx().device, g_atlas.image, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(ctx().device, &ai, nullptr, &g_atlas.memory) != VK_SUCCESS) {
        printf("[pt] atlas memory allocation failed\n");
        DestroyImage(g_atlas); return;
    }
    vkBindImageMemory(ctx().device, g_atlas.image, g_atlas.memory, 0);

    VkDeviceSize sz = (VkDeviceSize)pixels.size() * 4;
    Buffer staging = CreateBuffer(sz, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    memcpy(staging.mapped, pixels.data(), sz);

    VkCommandBuffer cb = BeginOneShot();
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.image = g_atlas.image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, (uint32_t)g_atlasLayers};
    b.srcAccessMask = 0; b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &b);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, (uint32_t)g_atlasLayers};
    region.imageExtent = {(uint32_t)TS, (uint32_t)TS, 1};
    vkCmdCopyBufferToImage(cb, staging.buffer, g_atlas.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &b);
    EndOneShot(cb);
    DestroyBuffer(staging);

    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = g_atlas.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    vi.format = VK_FORMAT_R8G8B8A8_UNORM;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, (uint32_t)g_atlasLayers};
    vkCreateImageView(ctx().device, &vi, nullptr, &g_atlasView);

    printf("[pt] atlas: %d wall + %d sprite pages (%d layers)\n", wall, spr, g_atlasLayers);
}

static void CreateOutputImages(int w, int h) {
    if (g_accumImage.image) DestroyImage(g_accumImage);
    if (g_outImage.image) DestroyImage(g_outImage);
    if (g_readback.buffer) DestroyBuffer(g_readback);
    g_accumImage = CreateImage(w, h, VK_FORMAT_R32G32B32A32_SFLOAT,
        VK_IMAGE_USAGE_STORAGE_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    // Tone mapping happens on the GPU, so the output/readback are cheap RGBA8.
    g_outImage = CreateImage(w, h, VK_FORMAT_R8G8B8A8_UNORM,
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    // HOST_CACHED so the CPU read-back memcpy is fast (uncached/write-combined
    // reads were the single biggest per-frame cost).
    g_readback = CreateBuffer((VkDeviceSize)w * h * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
        | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);

    // transition storage images to GENERAL
    VkCommandBuffer cb = BeginOneShot();
    TransitionImage(cb, g_accumImage.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_ASPECT_COLOR_BIT);
    TransitionImage(cb, g_outImage.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_ASPECT_COLOR_BIT);
    EndOneShot(cb);
    g_accumImage.layout = VK_IMAGE_LAYOUT_GENERAL;
    g_outImage.layout = VK_IMAGE_LAYOUT_GENERAL;
}

static void CreatePipeline() {
    VkDescriptorSetLayoutBinding binds[7] = {};
    for (int i = 0; i < 7; ++i) { binds[i].binding = i; binds[i].descriptorCount = 1;
        binds[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT; }
    binds[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;             // accum
    binds[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;             // out
    binds[2].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;// tlas
    binds[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;    // atlas
    binds[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;            // inst
    binds[5].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;            // lights
    binds[6].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;            // params

    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 7; li.pBindings = binds;
    vkCreateDescriptorSetLayout(ctx().device, &li, nullptr, &g_descLayout);

    VkDescriptorPoolSize pools[5] = {
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2},
        {VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1},
    };
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.maxSets = 1; pi.poolSizeCount = 5; pi.pPoolSizes = pools;
    vkCreateDescriptorPool(ctx().device, &pi, nullptr, &g_descPool);

    VkDescriptorSetAllocateInfo da{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    da.descriptorPool = g_descPool; da.descriptorSetCount = 1; da.pSetLayouts = &g_descLayout;
    vkAllocateDescriptorSets(ctx().device, &da, &g_descSet);

    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 1; pl.pSetLayouts = &g_descLayout;
    vkCreatePipelineLayout(ctx().device, &pl, nullptr, &g_pipeLayout);

    char path[512];
    snprintf(path, sizeof(path), "%s/pathtrace.comp.spv", PLAT_ShaderDir());
    VkShaderModule sm = LoadShader(path);
    if (!sm) { printf("[pt] failed to load pathtrace shader\n"); return; }

    VkComputePipelineCreateInfo cpi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cpi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpi.stage.module = sm;
    cpi.stage.pName = "main";
    cpi.layout = g_pipeLayout;
    vkCreateComputePipelines(ctx().device, VK_NULL_HANDLE, 1, &cpi, nullptr, &g_pipe);
    vkDestroyShaderModule(ctx().device, sm, nullptr);

    // Post pass (bloom + tone map), same descriptor layout.
    snprintf(path, sizeof(path), "%s/post.comp.spv", PLAT_ShaderDir());
    VkShaderModule pm = LoadShader(path);
    if (pm) {
        VkComputePipelineCreateInfo ppi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ppi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        ppi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        ppi.stage.module = pm;
        ppi.stage.pName = "main";
        ppi.layout = g_pipeLayout;
        vkCreateComputePipelines(ctx().device, VK_NULL_HANDLE, 1, &ppi, nullptr, &g_postPipe);
        vkDestroyShaderModule(ctx().device, pm, nullptr);
    }

    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = VK_FILTER_NEAREST; si.minFilter = VK_FILTER_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    vkCreateSampler(ctx().device, &si, nullptr, &g_sampler);
}

// ---------------------------------------------------------------------------
void Init() {
    g_ready = false;
    if (!ctx().rtSupported) { printf("[pt] ray query unsupported; path tracer disabled\n"); return; }
    LoadRTFunctions();
    if (!pvkCmdBuildAS) { printf("[pt] RT function load failed\n"); return; }
    BuildGeometryBLAS();
    CreatePipeline();
    if (!g_pipe) { printf("[pt] pipeline creation failed\n"); return; }
    // The main pass writes only the HDR accumImage; the post pass is what tone
    // maps into the RGBA8 output we read back. Without it the readback is
    // uninitialised garbage, so treat a missing post pipeline as a hard failure.
    if (!g_postPipe) { printf("[pt] post pipeline creation failed\n"); return; }
    CreateOutputImages(g_renderW, g_renderH);
    g_ready = true;
    printf("[pt] path tracer ready (%dx%d)\n", g_renderW, g_renderH);
}

void Shutdown() {
    if (!g_ready) return;
    g_ready = false;
    VkDevice d = ctx().device;
    if (!d) return;
    vkDeviceWaitIdle(d);

    // Destroy every GPU object created by Init()/Render() before rtvk::Shutdown()
    // tears down the device (destroying a device with live children is invalid).
    if (g_pipe)       { vkDestroyPipeline(d, g_pipe, nullptr);            g_pipe = VK_NULL_HANDLE; }
    if (g_postPipe)   { vkDestroyPipeline(d, g_postPipe, nullptr);        g_postPipe = VK_NULL_HANDLE; }
    if (g_pipeLayout) { vkDestroyPipelineLayout(d, g_pipeLayout, nullptr);g_pipeLayout = VK_NULL_HANDLE; }
    if (g_descPool)   { vkDestroyDescriptorPool(d, g_descPool, nullptr);  g_descPool = VK_NULL_HANDLE; }
    if (g_descLayout) { vkDestroyDescriptorSetLayout(d, g_descLayout, nullptr); g_descLayout = VK_NULL_HANDLE; }
    if (g_sampler)    { vkDestroySampler(d, g_sampler, nullptr);          g_sampler = VK_NULL_HANDLE; }
    if (g_atlasView)  { vkDestroyImageView(d, g_atlasView, nullptr);      g_atlasView = VK_NULL_HANDLE; }

    if (g_tlas && pvkDestroyAS)         { pvkDestroyAS(d, g_tlas, nullptr);         g_tlas = VK_NULL_HANDLE; }
    if (g_cubeBlas.as && pvkDestroyAS)  { pvkDestroyAS(d, g_cubeBlas.as, nullptr);  g_cubeBlas.as = VK_NULL_HANDLE; }
    if (g_quadBlas.as && pvkDestroyAS)  { pvkDestroyAS(d, g_quadBlas.as, nullptr);  g_quadBlas.as = VK_NULL_HANDLE; }

    DestroyImage(g_atlas); DestroyImage(g_accumImage); DestroyImage(g_outImage);
    DestroyBuffer(g_cubeBlas.buffer); DestroyBuffer(g_quadBlas.buffer);
    DestroyBuffer(g_cubeVtx); DestroyBuffer(g_cubeIdx);
    DestroyBuffer(g_quadVtx); DestroyBuffer(g_quadIdx);
    DestroyBuffer(g_tlasBuffer); DestroyBuffer(g_tlasScratch); DestroyBuffer(g_instanceBuffer);
    DestroyBuffer(g_instData); DestroyBuffer(g_lightData); DestroyBuffer(g_params);
    DestroyBuffer(g_readback);
}

bool Ready() { return g_ready; }

void DesiredResolution(int *w, int *h) { *w = g_renderW; *h = g_renderH; }

void AdjustSetting(int which, int delta) {
    if (which == SET_BOUNCES) {
        g_bounces = (g_bounces + delta < 0) ? 0 : g_bounces + delta;
        if (g_bounces > 4) g_bounces = 4;
        printf("[pt] bounces=%d\n", g_bounces);
    }
}

// Convert a 3x4 row-major transform.
static void SetTransform(VkTransformMatrixKHR &m, const float t[12]) {
    memcpy(m.matrix, t, sizeof(float) * 12);
}


void Render(const rt::Scene &scene, uint32_t *out, int w, int h) {
    if (!g_ready) return;

    static bool s_fps = getenv("WOLFPT_FPS") != nullptr;
    static int s_frames = 0;
    static double s_gpuMs = 0, s_postMs = 0, s_buildMs = 0;
    static auto s_t0 = std::chrono::steady_clock::now();
    if (s_fps && ++s_frames >= 60) {
        auto now = std::chrono::steady_clock::now();
        double dt = std::chrono::duration<double>(now - s_t0).count();
        printf("[pt] %.1f fps (%.1f ms) build=%.1f gpu=%.1f post=%.1f at %dx%d\n",
               s_frames / dt, dt / s_frames * 1000.0,
               s_buildMs / s_frames, s_gpuMs / s_frames, s_postMs / s_frames, w, h);
        s_frames = 0; s_t0 = now; s_gpuMs = s_postMs = s_buildMs = 0;
    }
    auto _tbuild = std::chrono::steady_clock::now();
    #define PT_TICK(acc) do { auto _n=std::chrono::steady_clock::now(); \
        acc += std::chrono::duration<double>(_n-_tbuild).count()*1000.0; _tbuild=_n; } while(0)
    if (w != g_renderW || h != g_renderH) { g_renderW = w; g_renderH = h; CreateOutputImages(w, h); }

    // (Re)build texture atlas on level change.
    if (scene.levelNumber != g_atlasLevel) { BuildAtlas(scene.levelNumber); g_atlasLevel = scene.levelNumber; }
    if (!g_atlasView) return;

    // ---- assemble instances ------------------------------------------------
    std::vector<InstGPU> insts;
    std::vector<VkAccelerationStructureInstanceKHR> asInst;
    insts.reserve(scene.walls.size() + scene.sprites.size() + 8);

    auto pushBox = [&](float tx, float ty, int texEW, int texNS, const float xf[12],
                       float rough, float metal, float refl, float emis) {
        InstGPU g{}; g.type = 0; g.texA = texEW; g.texB = texNS;
        g.origin[0] = tx; g.origin[1] = ty; g.origin[2] = 0; g.origin[3] = 0;
        g.material[0]=rough; g.material[1]=metal; g.material[2]=refl; g.material[3]=emis;
        uint32_t idx = (uint32_t)insts.size();
        insts.push_back(g);
        VkAccelerationStructureInstanceKHR ai{};
        SetTransform(ai.transform, xf);
        ai.instanceCustomIndex = idx;
        ai.mask = 0xFF;
        ai.flags = VK_GEOMETRY_INSTANCE_FORCE_OPAQUE_BIT_KHR |
                   VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
        ai.accelerationStructureReference = g_cubeBlas.address;
        asInst.push_back(ai);
    };

    // Walls
    for (auto &wl : scene.walls) {
        float xf[12] = {1,0,0, wl.tx, 0,1,0, wl.ty, 0,0,1, 0};
        mat::WallMaterial m = mat::WallMat(wl.texEW);
        pushBox(wl.tx, wl.ty, wl.texEW, wl.texNS, xf, m.roughness, m.metallic, m.reflectivity, m.emissive);
    }
    // Pushwall
    for (auto &pw : scene.pushwalls) {
        float xf[12] = {1,0,0, pw.x, 0,1,0, pw.y, 0,0,1, 0};
        pushBox(pw.x, pw.y, pw.texEW, pw.texNS, xf, 0.85f, 0, 0.03f, 0);
    }
    // Doors (thin box slid by open amount)
    for (auto &dr : scene.doors) {
        mat::WallMaterial m = mat::WallMat(dr.texPage);
        float xf[12];
        if (dr.vertical) {
            // thin in X, spans Y, slides +Y as it opens
            float t[12] = {0.12f,0,0, dr.tx+0.44f, 0,1,0, dr.ty+dr.open, 0,0,1, 0};
            memcpy(xf, t, sizeof(xf));
        } else {
            float t[12] = {1,0,0, dr.tx+dr.open, 0,0.12f,0, dr.ty+0.44f, 0,0,1, 0};
            memcpy(xf, t, sizeof(xf));
        }
        InstGPU g{}; g.type = 4 /*T_DOOR*/; g.texA = dr.texPage; g.texB = dr.texPage;
        g.flags = dr.vertical ? 1 : 0;
        g.origin[0]=dr.tx; g.origin[1]=dr.ty; g.origin[2]=dr.open;   // open amount -> texture slide
        g.material[0]=m.roughness; g.material[1]=m.metallic; g.material[2]=m.reflectivity;
        uint32_t idx=(uint32_t)insts.size(); insts.push_back(g);
        VkAccelerationStructureInstanceKHR ai{}; SetTransform(ai.transform, xf);
        ai.instanceCustomIndex=idx; ai.mask=0xFF;
        ai.flags=VK_GEOMETRY_INSTANCE_FORCE_OPAQUE_BIT_KHR|VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
        ai.accelerationStructureReference=g_cubeBlas.address;
        asInst.push_back(ai);
    }

    // Floor + ceiling (big quads). Colours come from Params (floorColor/ceilColor
    // in the shader); the page is -1 so the shader uses those flat colours.
    auto pushPlane = [&](int type, float z, int page, float refl) {
        InstGPU g{}; g.type = type; g.texA = page; g.texB = page;
        g.material[0]=0.5f; g.material[2]=refl;
        uint32_t idx=(uint32_t)insts.size(); insts.push_back(g);
        float xf[12] = {64,0,0, 32, 0,64,0, 32, 0,0,1, z};
        VkAccelerationStructureInstanceKHR ai{}; SetTransform(ai.transform, xf);
        ai.instanceCustomIndex=idx; ai.mask=0xFF;
        ai.flags=VK_GEOMETRY_INSTANCE_FORCE_OPAQUE_BIT_KHR|VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
        ai.accelerationStructureReference=g_quadBlas.address;
        asInst.push_back(ai);
    };
    pushPlane(1, 0.0f, -1, 0.15f);   // floor (glossy)
    pushPlane(2, 1.0f, -1, 0.0f);    // ceiling

    // Sprites (camera-facing alpha-tested cards)
    float ca = cosf(scene.cam.angleRad), sa = sinf(scene.cam.angleRad);
    float rightX = sa, rightY = ca;            // camera-right (world)
    for (auto &sp : scene.sprites) {
        int layer = g_wallPages + sp.texPage;
        if (layer < 0 || layer >= g_atlasLayers) continue;
        float size = 1.0f, hs = size * 0.5f;
        float cx = sp.x, cy = sp.y, cz = 0.5f;
        InstGPU g{}; g.type = 3; g.texA = layer; g.texB = layer;
        g.origin[0]=cx; g.origin[1]=cy; g.origin[2]=cz; g.origin[3]=hs;
        g.axis[0]=rightX; g.axis[1]=rightY; g.axis[2]=0;
        g.material[3]=sp.emissive;
        uint32_t idx=(uint32_t)insts.size(); insts.push_back(g);
        // transform: col0=right*size, col1=up*size, col2=normal*size, T=center
        float nx = ca, ny = -sa;   // forward (normal points back at camera = -forward, but sign handled in shader)
        float xf[12] = {
            rightX*size, 0,           nx*0.02f, cx,
            rightY*size, 0,           ny*0.02f, cy,
            0,           1.0f*size,   0,        cz,
        };
        VkAccelerationStructureInstanceKHR ai{}; SetTransform(ai.transform, xf);
        ai.instanceCustomIndex=idx; ai.mask=0xFF;
        // FORCE_NO_OPAQUE overrides the quad BLAS's opaque bit so ray query
        // yields candidates and our alpha test (sprite transparency) runs.
        ai.flags=VK_GEOMETRY_INSTANCE_FORCE_NO_OPAQUE_BIT_KHR |
                 VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
        ai.accelerationStructureReference=g_quadBlas.address;
        asInst.push_back(ai);
    }

    uint32_t instCount = (uint32_t)asInst.size();
    if (instCount == 0) return;

    // ---- upload instance geometry + build TLAS -----------------------------
    VkDeviceSize instSz = sizeof(VkAccelerationStructureInstanceKHR) * instCount;
    if (g_instanceBuffer.size < instSz) {
        if (g_instanceBuffer.buffer) DestroyBuffer(g_instanceBuffer);
        g_instanceBuffer = CreateBuffer(instSz + 4096,
            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    }
    memcpy(g_instanceBuffer.mapped, asInst.data(), instSz);

    VkAccelerationStructureGeometryKHR tgeom{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    tgeom.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    tgeom.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    tgeom.geometry.instances.data.deviceAddress = BufAddr(g_instanceBuffer);

    VkAccelerationStructureBuildGeometryInfoKHR tbi{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    tbi.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    tbi.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    tbi.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    tbi.geometryCount = 1;
    tbi.pGeometries = &tgeom;

    VkAccelerationStructureBuildSizesInfoKHR tsz{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    pvkGetASBuildSizes(ctx().device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &tbi, &instCount, &tsz);

    if (g_tlasBuffer.size < tsz.accelerationStructureSize) {
        if (g_tlas) { pvkDestroyAS(ctx().device, g_tlas, nullptr); g_tlas = VK_NULL_HANDLE; }
        if (g_tlasBuffer.buffer) DestroyBuffer(g_tlasBuffer);
        g_tlasBuffer = CreateBuffer(tsz.accelerationStructureSize + 4096,
            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VkAccelerationStructureCreateInfoKHR ci{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
        // Size the AS to the whole backing buffer (which carries +4096 slack). The
        // reuse guard above only compares buffer size, so the AS must be at least
        // as large, or a grown instance count could build into an undersized AS.
        ci.buffer = g_tlasBuffer.buffer; ci.size = g_tlasBuffer.size;
        ci.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
        pvkCreateAS(ctx().device, &ci, nullptr, &g_tlas);
    }
    if (g_tlasScratch.size < tsz.buildScratchSize) {
        if (g_tlasScratch.buffer) DestroyBuffer(g_tlasScratch);
        g_tlasScratch = CreateBuffer(tsz.buildScratchSize + 4096,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    }
    tbi.dstAccelerationStructure = g_tlas;
    tbi.scratchData.deviceAddress = BufAddr(g_tlasScratch);
    VkAccelerationStructureBuildRangeInfoKHR trange{}; trange.primitiveCount = instCount;
    const VkAccelerationStructureBuildRangeInfoKHR *pt = &trange;

    // ---- upload instance data / lights / params ----------------------------
    VkDeviceSize idSz = sizeof(InstGPU) * insts.size();
    if (g_instData.size < idSz) { if (g_instData.buffer) DestroyBuffer(g_instData);
        g_instData = CreateBuffer(idSz + 4096, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT); }
    memcpy(g_instData.mapped, insts.data(), idSz);

    std::vector<LightGPU> lg;
    for (auto &L : scene.lights) {
        LightGPU g{}; g.posType[0]=L.x; g.posType[1]=L.y; g.posType[2]=L.z; g.posType[3]=(float)L.type;
        g.color[0]=L.r; g.color[1]=L.g; g.color[2]=L.b; g.color[3]=L.intensity;
        g.params[0]=L.radius; g.params[1]=(float)L.flicker;
        lg.push_back(g);
    }
    if (lg.empty()) { LightGPU z{}; z.posType[3]=6; z.color[0]=z.color[1]=z.color[2]=0.5f; z.color[3]=1.0f; lg.push_back(z); }
    VkDeviceSize lSz = sizeof(LightGPU) * lg.size();
    if (g_lightData.size < lSz) { if (g_lightData.buffer) DestroyBuffer(g_lightData);
        g_lightData = CreateBuffer(lSz + 4096, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT); }
    memcpy(g_lightData.mapped, lg.data(), lSz);

    ParamsGPU pp{};
    pp.camPos[0]=scene.cam.x; pp.camPos[1]=scene.cam.y; pp.camPos[2]=scene.cam.z; pp.camPos[3]=(float)scene.levelNumber;
    pp.camDir[0]=ca; pp.camDir[1]=-sa; pp.camDir[2]=0; pp.camDir[3]=(float)w/(float)h;
    float tanH = tanf(scene.cam.fovRad*0.5f);
    // The classic projection is isotropic in world space (f_x == f_y == scale),
    // so with the viewport-aspect render target this needs no fudge factor.
    float tanV = tanH * (float)h/(float)w;
    pp.camRight[0]=rightX; pp.camRight[1]=rightY; pp.camRight[2]=0; pp.camRight[3]=tanH;
    pp.camUp[0]=0; pp.camUp[1]=0; pp.camUp[2]=1; pp.camUp[3]=tanV;
    pp.counts[0]=(int)insts.size(); pp.counts[1]=(int)lg.size(); pp.counts[2]=0; pp.counts[3]=g_bounces;
    // Polished floor with a subtle sheen (dielectric F0 ~0.06, not a mirror).
    pp.floorColor[0]=0.13f; pp.floorColor[1]=0.13f; pp.floorColor[2]=0.14f; pp.floorColor[3]=0.06f;
    pp.ceilColor[0]=0.11f; pp.ceilColor[1]=0.11f; pp.ceilColor[2]=0.14f; pp.ceilColor[3]=0.0f;
    pp.tune[0]=g_ambient; pp.tune[1]=g_shadowStrength; pp.tune[2]=g_exposure; pp.tune[3]=0.0f;
    if (g_params.size < sizeof(ParamsGPU)) { if (g_params.buffer) DestroyBuffer(g_params);
        g_params = CreateBuffer(sizeof(ParamsGPU), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT); }
    memcpy(g_params.mapped, &pp, sizeof(pp));

    // ---- descriptor updates ------------------------------------------------
    VkWriteDescriptorSet writes[7] = {};
    VkDescriptorImageInfo accumI{VK_NULL_HANDLE, g_accumImage.view, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo outI{VK_NULL_HANDLE, g_outImage.view, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo atlasI{g_sampler, g_atlasView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSetAccelerationStructureKHR asWrite{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
    asWrite.accelerationStructureCount = 1; asWrite.pAccelerationStructures = &g_tlas;
    VkDescriptorBufferInfo instB{g_instData.buffer, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo lightB{g_lightData.buffer, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo parB{g_params.buffer, 0, VK_WHOLE_SIZE};

    for (int i=0;i<7;i++){ writes[i].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet=g_descSet; writes[i].dstBinding=i; writes[i].descriptorCount=1; }
    writes[0].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; writes[0].pImageInfo=&accumI;
    writes[1].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; writes[1].pImageInfo=&outI;
    writes[2].descriptorType=VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR; writes[2].pNext=&asWrite;
    writes[3].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; writes[3].pImageInfo=&atlasI;
    writes[4].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; writes[4].pBufferInfo=&instB;
    writes[5].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; writes[5].pBufferInfo=&lightB;
    writes[6].descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; writes[6].pBufferInfo=&parB;
    vkUpdateDescriptorSets(ctx().device, 7, writes, 0, nullptr);

    // ---- record: build TLAS, dispatch, copy out ----------------------------
    if (s_fps) PT_TICK(s_buildMs);
    VkCommandBuffer cb = BeginOneShot();
    pvkCmdBuildAS(cb, 1, &tbi, &pt);
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    mb.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);

    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_pipeLayout, 0, 1, &g_descSet, 0, nullptr);
    vkCmdDispatch(cb, (w + 7) / 8, (h + 7) / 8, 1);

    // Post pass: bloom + tone map (reads the HDR accumImage, writes outImage).
    if (g_postPipe) {
        VkMemoryBarrier hb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        hb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        hb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &hb, 0, nullptr, 0, nullptr);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_postPipe);
        vkCmdDispatch(cb, (w + 7) / 8, (h + 7) / 8, 1);
    }

    VkImageMemoryBarrier ib{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    ib.oldLayout = VK_IMAGE_LAYOUT_GENERAL; ib.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    ib.image = g_outImage.image; ib.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    ib.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT; ib.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0,0,nullptr,0,nullptr,1,&ib);
    VkBufferImageCopy rc{}; rc.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
    rc.imageExtent={(uint32_t)w,(uint32_t)h,1};
    vkCmdCopyImageToBuffer(cb, g_outImage.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_readback.buffer, 1, &rc);
    ib.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; ib.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    ib.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT; ib.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0,0,nullptr,0,nullptr,1,&ib);
    EndOneShot(cb);
    if (s_fps) PT_TICK(s_gpuMs);

    // The GPU already tone-mapped to RGBA8; one bulk sequential copy out of
    // write-combined memory (fast) is all that's left.
    memcpy(out, g_readback.mapped, (size_t)w * h * 4);
    if (s_fps) PT_TICK(s_postMs);
    #undef PT_TICK
}

} // namespace rtpt
