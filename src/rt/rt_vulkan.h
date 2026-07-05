// rt_vulkan.h
//
// Low-level Vulkan context and helpers shared by the renderer modules. Owns
// the instance/device/swapchain and a present path that copies a final RGBA
// image to the screen via vkCmdBlitImage (letterboxed 4:3). The path tracer
// (rt_renderer.cpp / rt_pathtrace.cpp) builds on the exposed context.

#ifndef WOLFPT_RT_VULKAN_H
#define WOLFPT_RT_VULKAN_H

#include <vulkan/vulkan.h>
#include <stdint.h>

namespace rtvk {

struct Buffer {
    VkBuffer        buffer = VK_NULL_HANDLE;
    VkDeviceMemory  memory = VK_NULL_HANDLE;
    VkDeviceSize    size   = 0;
    void           *mapped = nullptr;
    VkDeviceAddress address = 0;
};

struct Image {
    VkImage        image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView    view = VK_NULL_HANDLE;
    uint32_t       width = 0, height = 0;
    VkFormat       format = VK_FORMAT_UNDEFINED;
    VkImageLayout  layout = VK_IMAGE_LAYOUT_UNDEFINED;
};

struct Context {
    VkInstance        instance = VK_NULL_HANDLE;
    VkSurfaceKHR      surface = VK_NULL_HANDLE;
    VkPhysicalDevice  phys = VK_NULL_HANDLE;
    VkDevice          device = VK_NULL_HANDLE;
    VkQueue           queue = VK_NULL_HANDLE;
    uint32_t          queueFamily = 0;
    VkCommandPool     cmdPool = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties memProps{};

    // Swapchain
    VkSwapchainKHR    swapchain = VK_NULL_HANDLE;
    VkFormat          swapFormat = VK_FORMAT_UNDEFINED;
    VkExtent2D        swapExtent{};
    uint32_t          swapCount = 0;

    bool              rtSupported = false;   // ray query + accel structures
    int               windowW = 0, windowH = 0;
};

Context &ctx();

bool Init(int windowW, int windowH);
void Shutdown();
bool Ready();

// --- Generic helpers used by the renderer/accel code ------------------------
uint32_t FindMemoryType(uint32_t typeBits, VkMemoryPropertyFlags props);

Buffer CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                    VkMemoryPropertyFlags memProps);
void    DestroyBuffer(Buffer &b);

Image CreateImage(uint32_t w, uint32_t h, VkFormat fmt, VkImageUsageFlags usage,
                  VkImageAspectFlags aspect);
void  DestroyImage(Image &img);

// One-shot command buffer helpers.
VkCommandBuffer BeginOneShot();
void            EndOneShot(VkCommandBuffer cb);

void TransitionImage(VkCommandBuffer cb, VkImage image, VkImageLayout from,
                     VkImageLayout to, VkImageAspectFlags aspect);

// --- Final presentation -----------------------------------------------------
// Provide the final composited RGBA8 frame (renderW x renderH). It is blitted
// to the swapchain (nearest-filtered, 4:3 letterboxed) and presented.
void PresentRGBA(const uint32_t *rgba, int renderW, int renderH);

VkShaderModule LoadShader(const char *spvFile);

} // namespace rtvk

#endif // WOLFPT_RT_VULKAN_H
