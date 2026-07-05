// rt_vulkan.cpp
//
// Vulkan context: instance, surface (from SDL), physical/logical device,
// swapchain, command pool, and a robust presentation path that blits a final
// RGBA image to the swapchain (nearest-filtered, 4:3 letterboxed). Also
// provides the generic buffer/image/command helpers the path tracer uses.
//
// Presentation deliberately uses vkCmdBlitImage instead of a graphics pipeline
// so the hot path stays tiny; the path tracer runs in compute (ray query) and
// the compositor combines classic + path-traced frames before this blit.

#include "rt_vulkan.h"
#include "../platform/platform.h"

#include <SDL.h>
#include <SDL_vulkan.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include <algorithm>

namespace rtvk {

#define VK_CHECK(x) do { VkResult _r = (x); if (_r != VK_SUCCESS) { \
    printf("[vk] %s failed: %d (%s:%d)\n", #x, _r, __FILE__, __LINE__); } } while(0)

static Context g_ctx;
Context &ctx() { return g_ctx; }

// ---- Swapchain resources ----------------------------------------------------
static std::vector<VkImage>     g_swapImages;
static std::vector<VkImageView> g_swapViews;

// ---- Per-present source image (final composited frame) ----------------------
static Image    g_srcImage;
static Buffer   g_srcStaging;
static int      g_srcW = 0, g_srcH = 0;

// ---- Sync -------------------------------------------------------------------
static VkSemaphore g_semAcquire = VK_NULL_HANDLE;
static VkSemaphore g_semRender  = VK_NULL_HANDLE;
static VkFence     g_fence      = VK_NULL_HANDLE;

static bool g_ready = false;

// ---------------------------------------------------------------------------
uint32_t FindMemoryType(uint32_t typeBits, VkMemoryPropertyFlags props) {
    for (uint32_t i = 0; i < g_ctx.memProps.memoryTypeCount; ++i) {
        if ((typeBits & (1u << i)) &&
            (g_ctx.memProps.memoryTypes[i].propertyFlags & props) == props)
            return i;
    }
    printf("[vk] no suitable memory type (bits=%x props=%x)\n", typeBits, props);
    return 0;
}

Buffer CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                    VkMemoryPropertyFlags memProps) {
    Buffer b{};
    b.size = size;
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = size;
    bi.usage = usage;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VK_CHECK(vkCreateBuffer(g_ctx.device, &bi, nullptr, &b.buffer));

    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(g_ctx.device, b.buffer, &req);

    VkMemoryAllocateFlagsInfo flagsInfo{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
    flagsInfo.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;

    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = FindMemoryType(req.memoryTypeBits, memProps);
    if (usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT)
        ai.pNext = &flagsInfo;
    VK_CHECK(vkAllocateMemory(g_ctx.device, &ai, nullptr, &b.memory));
    VK_CHECK(vkBindBufferMemory(g_ctx.device, b.buffer, b.memory, 0));

    if (memProps & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
        vkMapMemory(g_ctx.device, b.memory, 0, size, 0, &b.mapped);

    if (usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) {
        VkBufferDeviceAddressInfo di{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
        di.buffer = b.buffer;
        b.address = vkGetBufferDeviceAddress(g_ctx.device, &di);
    }
    return b;
}

void DestroyBuffer(Buffer &b) {
    if (b.mapped) { vkUnmapMemory(g_ctx.device, b.memory); b.mapped = nullptr; }
    if (b.buffer) vkDestroyBuffer(g_ctx.device, b.buffer, nullptr);
    if (b.memory) vkFreeMemory(g_ctx.device, b.memory, nullptr);
    b = Buffer{};
}

Image CreateImage(uint32_t w, uint32_t h, VkFormat fmt, VkImageUsageFlags usage,
                  VkImageAspectFlags aspect) {
    Image img{};
    img.width = w; img.height = h; img.format = fmt;
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = fmt;
    ii.extent = {w, h, 1};
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = usage;
    ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VK_CHECK(vkCreateImage(g_ctx.device, &ii, nullptr, &img.image));

    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(g_ctx.device, img.image, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VK_CHECK(vkAllocateMemory(g_ctx.device, &ai, nullptr, &img.memory));
    VK_CHECK(vkBindImageMemory(g_ctx.device, img.image, img.memory, 0));

    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = img.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = fmt;
    vi.subresourceRange = {aspect, 0, 1, 0, 1};
    VK_CHECK(vkCreateImageView(g_ctx.device, &vi, nullptr, &img.view));
    return img;
}

void DestroyImage(Image &img) {
    if (img.view) vkDestroyImageView(g_ctx.device, img.view, nullptr);
    if (img.image) vkDestroyImage(g_ctx.device, img.image, nullptr);
    if (img.memory) vkFreeMemory(g_ctx.device, img.memory, nullptr);
    img = Image{};
}

VkCommandBuffer BeginOneShot() {
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = g_ctx.cmdPool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cb;
    vkAllocateCommandBuffers(g_ctx.device, &ai, &cb);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &bi);
    return cb;
}

void EndOneShot(VkCommandBuffer cb) {
    vkEndCommandBuffer(cb);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    vkQueueSubmit(g_ctx.queue, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(g_ctx.queue);
    vkFreeCommandBuffers(g_ctx.device, g_ctx.cmdPool, 1, &cb);
}

void TransitionImage(VkCommandBuffer cb, VkImage image, VkImageLayout from,
                     VkImageLayout to, VkImageAspectFlags aspect) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {aspect, 0, 1, 0, 1};
    b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT
                    | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
}

VkShaderModule LoadShader(const char *spvFile) {
    FILE *f = fopen(spvFile, "rb");
    if (!f) { printf("[vk] cannot open shader %s\n", spvFile); return VK_NULL_HANDLE; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    std::vector<char> code(sz);
    if (fread(code.data(), 1, sz, f) != (size_t)sz) { fclose(f); return VK_NULL_HANDLE; }
    fclose(f);
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = sz;
    ci.pCode = (const uint32_t *)code.data();
    VkShaderModule m;
    VK_CHECK(vkCreateShaderModule(g_ctx.device, &ci, nullptr, &m));
    return m;
}

// ---------------------------------------------------------------------------
static bool CreateInstance() {
    unsigned extCount = 0;
    if (!SDL_Vulkan_GetInstanceExtensions(PLAT_Window(), &extCount, nullptr)) {
        printf("[vk] SDL_Vulkan_GetInstanceExtensions failed: %s\n", SDL_GetError());
        return false;
    }
    std::vector<const char *> exts(extCount);
    SDL_Vulkan_GetInstanceExtensions(PLAT_Window(), &extCount, exts.data());

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "Wolfenstein 3D: Path Traced";
    app.apiVersion = VK_API_VERSION_1_2;

    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &app;
    ci.enabledExtensionCount = (uint32_t)exts.size();
    ci.ppEnabledExtensionNames = exts.data();
    VK_CHECK(vkCreateInstance(&ci, nullptr, &g_ctx.instance));
    return g_ctx.instance != VK_NULL_HANDLE;
}

static bool DeviceHasExtension(VkPhysicalDevice d, const char *name) {
    uint32_t n = 0; vkEnumerateDeviceExtensionProperties(d, nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> props(n);
    vkEnumerateDeviceExtensionProperties(d, nullptr, &n, props.data());
    for (auto &p : props) if (!strcmp(p.extensionName, name)) return true;
    return false;
}

static bool PickPhysicalDevice() {
    uint32_t n = 0; vkEnumeratePhysicalDevices(g_ctx.instance, &n, nullptr);
    if (!n) { printf("[vk] no Vulkan devices\n"); return false; }
    std::vector<VkPhysicalDevice> devs(n);
    vkEnumeratePhysicalDevices(g_ctx.instance, &n, devs.data());

    VkPhysicalDevice best = VK_NULL_HANDLE;
    int bestScore = -1;
    for (auto d : devs) {
        VkPhysicalDeviceProperties p; vkGetPhysicalDeviceProperties(d, &p);
        bool rt = DeviceHasExtension(d, VK_KHR_RAY_QUERY_EXTENSION_NAME) &&
                  DeviceHasExtension(d, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME);
        int score = 0;
        if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) score += 1000;
        if (rt) score += 5000;
        printf("[vk] device: %s  rt=%d  score=%d\n", p.deviceName, rt, score);
        if (score > bestScore) { bestScore = score; best = d; }
    }
    g_ctx.phys = best;
    g_ctx.rtSupported = DeviceHasExtension(best, VK_KHR_RAY_QUERY_EXTENSION_NAME) &&
                        DeviceHasExtension(best, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME);
    vkGetPhysicalDeviceMemoryProperties(best, &g_ctx.memProps);
    VkPhysicalDeviceProperties p; vkGetPhysicalDeviceProperties(best, &p);
    printf("[vk] selected: %s  (ray query %s)\n", p.deviceName,
           g_ctx.rtSupported ? "AVAILABLE" : "unavailable");
    return best != VK_NULL_HANDLE;
}

static bool CreateDevice() {
    uint32_t n = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(g_ctx.phys, &n, nullptr);
    std::vector<VkQueueFamilyProperties> fams(n);
    vkGetPhysicalDeviceQueueFamilyProperties(g_ctx.phys, &n, fams.data());
    uint32_t family = UINT32_MAX;
    for (uint32_t i = 0; i < n; ++i) {
        VkBool32 present = false;
        vkGetPhysicalDeviceSurfaceSupportKHR(g_ctx.phys, i, g_ctx.surface, &present);
        if ((fams[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) &&
            (fams[i].queueFlags & VK_QUEUE_COMPUTE_BIT) && present) {
            family = i; break;
        }
    }
    if (family == UINT32_MAX) { printf("[vk] no graphics+compute+present queue\n"); return false; }
    g_ctx.queueFamily = family;

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qi.queueFamilyIndex = family;
    qi.queueCount = 1;
    qi.pQueuePriorities = &prio;

    std::vector<const char *> devExts = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };

    // Feature chain
    VkPhysicalDeviceBufferDeviceAddressFeatures bda{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES};
    bda.bufferDeviceAddress = VK_TRUE;

    VkPhysicalDeviceAccelerationStructureFeaturesKHR accel{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
    accel.accelerationStructure = VK_TRUE;

    VkPhysicalDeviceRayQueryFeaturesKHR rq{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
    rq.rayQuery = VK_TRUE;

    // Only the ray-tracing features are actually required (buffer device address
    // for AS build inputs, acceleration structures, ray query). The shaders use
    // no descriptor indexing or 64-bit ints, so we don't request those and stay
    // creatable on more devices.
    void *pNext = nullptr;
    if (g_ctx.rtSupported) {
        devExts.push_back(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME);
        devExts.push_back(VK_KHR_RAY_QUERY_EXTENSION_NAME);
        devExts.push_back(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
        bda.pNext = &accel; accel.pNext = &rq;
        pNext = &bda;
    }

    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    f2.pNext = pNext;

    VkDeviceCreateInfo ci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    ci.pNext = &f2;
    ci.queueCreateInfoCount = 1;
    ci.pQueueCreateInfos = &qi;
    ci.enabledExtensionCount = (uint32_t)devExts.size();
    ci.ppEnabledExtensionNames = devExts.data();
    VK_CHECK(vkCreateDevice(g_ctx.phys, &ci, nullptr, &g_ctx.device));
    if (!g_ctx.device) return false;

    vkGetDeviceQueue(g_ctx.device, family, 0, &g_ctx.queue);

    VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pi.queueFamilyIndex = family;
    VK_CHECK(vkCreateCommandPool(g_ctx.device, &pi, nullptr, &g_ctx.cmdPool));
    return true;
}

static void DestroySwapchain() {
    for (auto v : g_swapViews) vkDestroyImageView(g_ctx.device, v, nullptr);
    g_swapViews.clear();
    g_swapImages.clear();
    if (g_ctx.swapchain) vkDestroySwapchainKHR(g_ctx.device, g_ctx.swapchain, nullptr);
    g_ctx.swapchain = VK_NULL_HANDLE;
}

static bool CreateSwapchain() {
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g_ctx.phys, g_ctx.surface, &caps);

    uint32_t fmtCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(g_ctx.phys, g_ctx.surface, &fmtCount, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(fmtCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(g_ctx.phys, g_ctx.surface, &fmtCount, formats.data());
    if (fmtCount == 0) { printf("[vk] surface reports no formats\n"); return false; }
    VkSurfaceFormatKHR chosen = formats[0];
    for (auto &f : formats)
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM &&
            f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) { chosen = f; break; }
    g_ctx.swapFormat = chosen.format;

    VkExtent2D extent = caps.currentExtent;
    if (extent.width == 0xFFFFFFFF) {
        int w, h; PLAT_WindowSize(&w, &h);
        extent.width = w; extent.height = h;
    }
    g_ctx.swapExtent = extent;
    // A minimised window reports a 0x0 extent, which is invalid for a swapchain.
    // Leave the swapchain null; PresentRGBA skips and retries when it grows back.
    if (extent.width == 0 || extent.height == 0) return false;

    uint32_t imgCount = caps.minImageCount + 1;
    if (caps.maxImageCount && imgCount > caps.maxImageCount) imgCount = caps.maxImageCount;

    VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.surface = g_ctx.surface;
    ci.minImageCount = imgCount;
    ci.imageFormat = chosen.format;
    ci.imageColorSpace = chosen.colorSpace;
    ci.imageExtent = extent;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    ci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    ci.clipped = VK_TRUE;
    VK_CHECK(vkCreateSwapchainKHR(g_ctx.device, &ci, nullptr, &g_ctx.swapchain));

    vkGetSwapchainImagesKHR(g_ctx.device, g_ctx.swapchain, &g_ctx.swapCount, nullptr);
    g_swapImages.resize(g_ctx.swapCount);
    vkGetSwapchainImagesKHR(g_ctx.device, g_ctx.swapchain, &g_ctx.swapCount, g_swapImages.data());
    g_swapViews.resize(g_ctx.swapCount);
    for (uint32_t i = 0; i < g_ctx.swapCount; ++i) {
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = g_swapImages[i];
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = chosen.format;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VK_CHECK(vkCreateImageView(g_ctx.device, &vi, nullptr, &g_swapViews[i]));
    }
    return true;
}

bool Init(int windowW, int windowH) {
    g_ctx.windowW = windowW; g_ctx.windowH = windowH;

    if (!CreateInstance()) return false;
    if (!SDL_Vulkan_CreateSurface(PLAT_Window(), g_ctx.instance, &g_ctx.surface)) {
        printf("[vk] SDL_Vulkan_CreateSurface failed: %s\n", SDL_GetError());
        return false;
    }
    if (!PickPhysicalDevice()) return false;
    if (!CreateDevice()) return false;
    if (!CreateSwapchain()) return false;

    VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    vkCreateSemaphore(g_ctx.device, &si, nullptr, &g_semAcquire);
    vkCreateSemaphore(g_ctx.device, &si, nullptr, &g_semRender);
    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    vkCreateFence(g_ctx.device, &fi, nullptr, &g_fence);

    g_ready = true;
    printf("[vk] init complete: %ux%u swapchain, %u images\n",
           g_ctx.swapExtent.width, g_ctx.swapExtent.height, g_ctx.swapCount);
    return true;
}

void Shutdown() {
    if (!g_ctx.device) return;
    vkDeviceWaitIdle(g_ctx.device);
    DestroyImage(g_srcImage);
    DestroyBuffer(g_srcStaging);
    if (g_semAcquire) vkDestroySemaphore(g_ctx.device, g_semAcquire, nullptr);
    if (g_semRender)  vkDestroySemaphore(g_ctx.device, g_semRender, nullptr);
    if (g_fence)      vkDestroyFence(g_ctx.device, g_fence, nullptr);
    DestroySwapchain();
    if (g_ctx.cmdPool) vkDestroyCommandPool(g_ctx.device, g_ctx.cmdPool, nullptr);
    vkDestroyDevice(g_ctx.device, nullptr);
    if (g_ctx.surface) vkDestroySurfaceKHR(g_ctx.instance, g_ctx.surface, nullptr);
    if (g_ctx.instance) vkDestroyInstance(g_ctx.instance, nullptr);
    g_ctx = Context{};
    g_ready = false;
}

bool Ready() { return g_ready; }

static void EnsureSrcImage(int w, int h) {
    if (g_srcW == w && g_srcH == h && g_srcImage.image) return;
    if (g_srcImage.image) DestroyImage(g_srcImage);
    if (g_srcStaging.buffer) DestroyBuffer(g_srcStaging);
    g_srcImage = CreateImage(w, h, VK_FORMAT_R8G8B8A8_UNORM,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        VK_IMAGE_ASPECT_COLOR_BIT);
    g_srcStaging = CreateBuffer((VkDeviceSize)w * h * 4,
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    g_srcW = w; g_srcH = h;
}

static void RecreateSwapchain() {
    vkDeviceWaitIdle(g_ctx.device);
    DestroySwapchain();
    CreateSwapchain();
}

void PresentRGBA(const uint32_t *rgba, int renderW, int renderH) {
    if (!g_ready) return;
    if (g_ctx.swapchain == VK_NULL_HANDLE) {
        RecreateSwapchain();
        if (g_ctx.swapchain == VK_NULL_HANDLE) return;   // window minimised / zero extent
    }

    EnsureSrcImage(renderW, renderH);
    memcpy(g_srcStaging.mapped, rgba, (size_t)renderW * renderH * 4);

    vkWaitForFences(g_ctx.device, 1, &g_fence, VK_TRUE, UINT64_MAX);

    uint32_t idx = 0;
    VkResult acq = vkAcquireNextImageKHR(g_ctx.device, g_ctx.swapchain, UINT64_MAX,
                                         g_semAcquire, VK_NULL_HANDLE, &idx);
    if (acq == VK_ERROR_OUT_OF_DATE_KHR) { RecreateSwapchain(); return; }
    // Any other failure (surface/device lost) leaves g_semAcquire unsignalled;
    // bail before the submit so its semaphore wait can't deadlock the fence.
    if (acq != VK_SUCCESS && acq != VK_SUBOPTIMAL_KHR) return;

    vkResetFences(g_ctx.device, 1, &g_fence);

    VkCommandBuffer cb = BeginOneShot(); // simple: allocate per-frame (fine at these rates)

    // upload src
    TransitionImage(cb, g_srcImage.image, VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
    VkBufferImageCopy bic{};
    bic.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    bic.imageExtent = {(uint32_t)renderW, (uint32_t)renderH, 1};
    vkCmdCopyBufferToImage(cb, g_srcStaging.buffer, g_srcImage.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bic);
    TransitionImage(cb, g_srcImage.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);

    // clear swap image to black, then blit letterboxed 4:3
    TransitionImage(cb, g_swapImages[idx], VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
    VkClearColorValue black{{0, 0, 0, 1}};
    VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cb, g_swapImages[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         &black, 1, &range);

    // compute 4:3 letterbox rect within swap extent
    int sw = g_ctx.swapExtent.width, sh = g_ctx.swapExtent.height;
    float targetAspect = 4.0f / 3.0f;
    int dw = sw, dh = (int)(sw / targetAspect);
    if (dh > sh) { dh = sh; dw = (int)(sh * targetAspect); }
    int ox = (sw - dw) / 2, oy = (sh - dh) / 2;

    VkImageBlit blit{};
    blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.srcOffsets[0] = {0, 0, 0};
    blit.srcOffsets[1] = {renderW, renderH, 1};
    blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.dstOffsets[0] = {ox, oy, 0};
    blit.dstOffsets[1] = {ox + dw, oy + dh, 1};
    vkCmdBlitImage(cb, g_srcImage.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   g_swapImages[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   1, &blit, VK_FILTER_NEAREST);

    TransitionImage(cb, g_swapImages[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_ASPECT_COLOR_BIT);

    vkEndCommandBuffer(cb);

    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &g_semAcquire;
    submit.pWaitDstStageMask = &waitStage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cb;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &g_semRender;
    vkQueueSubmit(g_ctx.queue, 1, &submit, g_fence);

    VkPresentInfoKHR pres{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pres.waitSemaphoreCount = 1;
    pres.pWaitSemaphores = &g_semRender;
    pres.swapchainCount = 1;
    pres.pSwapchains = &g_ctx.swapchain;
    pres.pImageIndices = &idx;
    VkResult pr = vkQueuePresentKHR(g_ctx.queue, &pres);
    if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR)
        RecreateSwapchain();

    // free the per-frame command buffer once done
    vkWaitForFences(g_ctx.device, 1, &g_fence, VK_TRUE, UINT64_MAX);
    vkFreeCommandBuffers(g_ctx.device, g_ctx.cmdPool, 1, &cb);
}

} // namespace rtvk
