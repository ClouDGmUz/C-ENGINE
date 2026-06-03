#pragma once
#include <volk.h>
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#include <vector>
#include <cstdint>

#define MAX_FRAMES_IN_FLIGHT 2

struct VkCtx {
    VkInstance       instance;
    VkSurfaceKHR     surface;
    VkPhysicalDevice physical_device;
    uint32_t         graphics_family;
    uint32_t         present_family;
    VkDevice         device;
    VkQueue          graphics_queue;
    VkQueue          present_queue;
    VkSwapchainKHR   swapchain;
    VkFormat         swapchain_format;
    VkExtent2D       swapchain_extent;
    uint32_t         swapchain_count;
    std::vector<VkImage>     swapchain_images;
    std::vector<VkImageView> swapchain_views;
    VkImage          depth_image;
    VkDeviceMemory   depth_memory;
    VkImageView      depth_view;
    VkRenderPass     render_pass;
    std::vector<VkFramebuffer> framebuffers;
    VkCommandPool    command_pool;
    std::vector<VkCommandBuffer> command_buffers;
    VkSemaphore      image_available[MAX_FRAMES_IN_FLIGHT];
    VkSemaphore      render_finished[MAX_FRAMES_IN_FLIGHT];
    VkFence          in_flight[MAX_FRAMES_IN_FLIGHT];
    uint32_t         frame_index;
};

void vk_check(VkResult r, const char *msg);
uint32_t vk_find_memory_type(const VkCtx &ctx, uint32_t filter, VkMemoryPropertyFlags props);
void vk_create_image(const VkCtx &ctx, uint32_t w, uint32_t h, VkFormat fmt,
                     VkImageTiling tiling, VkImageUsageFlags usage,
                     VkImage &image, VkDeviceMemory &memory);
void vk_init(VkCtx &ctx, GLFWwindow *window);
void vk_shutdown(VkCtx &ctx);
bool vk_begin_frame(VkCtx &ctx, GLFWwindow *window, uint32_t &out_image_index);
void vk_end_frame(VkCtx &ctx, uint32_t image_index);
