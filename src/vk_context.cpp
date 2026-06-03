#include "vk_context.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

void vk_check(VkResult r, const char *msg) {
    if (r != VK_SUCCESS) { fprintf(stderr, "Vulkan error: %s (code %d)\n", msg, r); exit(1); }
}

uint32_t vk_find_memory_type(const VkCtx &ctx, uint32_t filter, VkMemoryPropertyFlags props) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(ctx.physical_device, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((filter & (1 << i)) && (mp.memoryTypes[i].propertyFlags & props) == props)
            return i;
    fprintf(stderr, "Failed to find suitable memory type\n");
    exit(1);
    return 0;
}

void vk_create_image(const VkCtx &ctx, uint32_t w, uint32_t h, VkFormat fmt,
                     VkImageTiling tiling, VkImageUsageFlags usage,
                     VkImage &image, VkDeviceMemory &memory) {
    VkImageCreateInfo ci = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ci.imageType     = VK_IMAGE_TYPE_2D;
    ci.extent        = { w, h, 1 };
    ci.mipLevels     = 1;
    ci.arrayLayers   = 1;
    ci.format        = fmt;
    ci.tiling        = tiling;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    ci.usage         = usage;
    ci.samples       = VK_SAMPLE_COUNT_1_BIT;
    ci.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    vk_check(vkCreateImage(ctx.device, &ci, nullptr, &image), "create image");

    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(ctx.device, image, &req);
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    ai.allocationSize  = req.size;
    ai.memoryTypeIndex = vk_find_memory_type(ctx, req.memoryTypeBits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    vk_check(vkAllocateMemory(ctx.device, &ai, nullptr, &memory), "allocate image memory");
    vkBindImageMemory(ctx.device, image, memory, 0);
}

static VkImageView create_image_view(const VkCtx &ctx, VkImage image, VkFormat fmt, VkImageAspectFlags aspects) {
    VkImageViewCreateInfo ci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    ci.image                           = image;
    ci.viewType                       = VK_IMAGE_VIEW_TYPE_2D;
    ci.format                         = fmt;
    ci.subresourceRange.aspectMask    = aspects;
    ci.subresourceRange.levelCount    = 1;
    ci.subresourceRange.layerCount    = 1;
    VkImageView view;
    vk_check(vkCreateImageView(ctx.device, &ci, nullptr, &view), "create image view");
    return view;
}

/* --- instance --- */
static void create_instance(VkCtx &ctx) {
    uint32_t ext_count = 0;
    const char **glfw_exts = glfwGetRequiredInstanceExtensions(&ext_count);

    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app.pApplicationName   = "Vulkan 3D";
    app.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    app.pEngineName        = "No Engine";
    app.apiVersion         = VK_API_VERSION_1_0;

    VkInstanceCreateInfo ci = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    ci.pApplicationInfo        = &app;
    ci.enabledExtensionCount   = ext_count;
    ci.ppEnabledExtensionNames = glfw_exts;

#ifndef NDEBUG
    const char *layers[] = { "VK_LAYER_KHRONOS_validation" };
    ci.enabledLayerCount   = 1;
    ci.ppEnabledLayerNames = layers;
#endif

    vk_check(vkCreateInstance(&ci, nullptr, &ctx.instance), "create instance");
}

static void create_surface(VkCtx &ctx, GLFWwindow *window) {
    vk_check(glfwCreateWindowSurface(ctx.instance, window, nullptr, &ctx.surface), "create surface");
}

/* --- physical device --- */
static int score_device(VkPhysicalDevice dev) {
    VkPhysicalDeviceProperties props;
    VkPhysicalDeviceFeatures   feat;
    vkGetPhysicalDeviceProperties(dev, &props);
    vkGetPhysicalDeviceFeatures(dev, &feat);
    if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)   return 1000 + props.limits.maxImageDimension2D;
    if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) return 500  + props.limits.maxImageDimension2D;
    return 100;
}

static void pick_physical_device(VkCtx &ctx) {
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(ctx.instance, &count, nullptr);
    if (count == 0) { fprintf(stderr, "No Vulkan-capable GPU\n"); exit(1); }
    auto *devices = (VkPhysicalDevice *)malloc(count * sizeof(VkPhysicalDevice));
    vkEnumeratePhysicalDevices(ctx.instance, &count, devices);

    int best = -1;
    for (uint32_t i = 0; i < count; i++) {
        int s = score_device(devices[i]);
        if (s > best) { best = s; ctx.physical_device = devices[i]; }
    }
    free(devices);

    uint32_t qf_count;
    vkGetPhysicalDeviceQueueFamilyProperties(ctx.physical_device, &qf_count, nullptr);
    auto *qfs = (VkQueueFamilyProperties *)malloc(qf_count * sizeof(VkQueueFamilyProperties));
    vkGetPhysicalDeviceQueueFamilyProperties(ctx.physical_device, &qf_count, qfs);

    int gfx = -1, pres = -1;
    for (uint32_t i = 0; i < qf_count; i++) {
        if (qfs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) gfx = i;
        VkBool32 supports_present = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(ctx.physical_device, i, ctx.surface, &supports_present);
        if (supports_present) pres = i;
    }
    free(qfs);

    if (gfx < 0 || pres < 0) { fprintf(stderr, "Missing required queue family\n"); exit(1); }
    ctx.graphics_family = (uint32_t)gfx;
    ctx.present_family  = (uint32_t)pres;
}

/* --- logical device --- */
static void create_device(VkCtx &ctx) {
    float prio = 1.0f;
    uint32_t families[] = { ctx.graphics_family, ctx.present_family };
    uint32_t n = (ctx.graphics_family == ctx.present_family) ? 1 : 2;

    VkDeviceQueueCreateInfo qcis[2];
    for (uint32_t i = 0; i < n; i++) {
        qcis[i] = VkDeviceQueueCreateInfo{};
        qcis[i].sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        qcis[i].queueFamilyIndex = families[i];
        qcis[i].queueCount       = 1;
        qcis[i].pQueuePriorities = &prio;
    }

    const char *exts[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    VkPhysicalDeviceFeatures feats = {};
    feats.fillModeNonSolid = VK_TRUE;

    VkDeviceCreateInfo ci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    ci.queueCreateInfoCount    = n;
    ci.pQueueCreateInfos       = qcis;
    ci.enabledExtensionCount   = 1;
    ci.ppEnabledExtensionNames = exts;
    ci.pEnabledFeatures        = &feats;

    vk_check(vkCreateDevice(ctx.physical_device, &ci, nullptr, &ctx.device), "create device");
    volkLoadDevice(ctx.device);
    vkGetDeviceQueue(ctx.device, ctx.graphics_family, 0, &ctx.graphics_queue);
    vkGetDeviceQueue(ctx.device, ctx.present_family, 0, &ctx.present_queue);
}

/* --- swapchain --- */
static void create_swapchain(VkCtx &ctx) {
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(ctx.physical_device, ctx.surface, &caps);

    uint32_t fmt_count;
    vkGetPhysicalDeviceSurfaceFormatsKHR(ctx.physical_device, ctx.surface, &fmt_count, nullptr);
    auto *formats = (VkSurfaceFormatKHR *)malloc(fmt_count * sizeof(VkSurfaceFormatKHR));
    vkGetPhysicalDeviceSurfaceFormatsKHR(ctx.physical_device, ctx.surface, &fmt_count, formats);

    VkSurfaceFormatKHR chosen = formats[0];
    for (uint32_t i = 0; i < fmt_count; i++) {
        if (formats[i].format == VK_FORMAT_B8G8R8A8_SRGB &&
            formats[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
            chosen = formats[i];
    }
    free(formats);
    ctx.swapchain_format = chosen.format;
    ctx.swapchain_extent = caps.currentExtent;

    uint32_t img_count = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && img_count > caps.maxImageCount) img_count = caps.maxImageCount;

    VkSwapchainCreateInfoKHR ci = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
    ci.surface          = ctx.surface;
    ci.minImageCount    = img_count;
    ci.imageFormat      = chosen.format;
    ci.imageColorSpace  = chosen.colorSpace;
    ci.imageExtent      = ctx.swapchain_extent;
    ci.imageArrayLayers = 1;
    ci.imageUsage       = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ci.preTransform     = caps.currentTransform;
    ci.compositeAlpha   = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    ci.presentMode      = VK_PRESENT_MODE_FIFO_KHR;
    ci.clipped          = VK_TRUE;

    uint32_t qf[] = { ctx.graphics_family, ctx.present_family };
    if (ctx.graphics_family != ctx.present_family) {
        ci.imageSharingMode      = VK_SHARING_MODE_CONCURRENT;
        ci.queueFamilyIndexCount = 2;
        ci.pQueueFamilyIndices   = qf;
    } else {
        ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }

    vk_check(vkCreateSwapchainKHR(ctx.device, &ci, nullptr, &ctx.swapchain), "create swapchain");
    vkGetSwapchainImagesKHR(ctx.device, ctx.swapchain, &ctx.swapchain_count, nullptr);
    ctx.swapchain_images.resize(ctx.swapchain_count);
    vkGetSwapchainImagesKHR(ctx.device, ctx.swapchain, &ctx.swapchain_count, ctx.swapchain_images.data());
}

/* --- image views --- */
static void create_image_views(VkCtx &ctx) {
    ctx.swapchain_views.resize(ctx.swapchain_count);
    for (uint32_t i = 0; i < ctx.swapchain_count; i++)
        ctx.swapchain_views[i] = create_image_view(ctx, ctx.swapchain_images[i], ctx.swapchain_format, VK_IMAGE_ASPECT_COLOR_BIT);
}

/* --- depth buffer --- */
static VkFormat find_depth_format(const VkCtx &ctx) {
    VkFormat candidates[] = { VK_FORMAT_D32_SFLOAT, VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT };
    for (auto fmt : candidates) {
        VkFormatProperties props;
        vkGetPhysicalDeviceFormatProperties(ctx.physical_device, fmt, &props);
        if (props.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)
            return fmt;
    }
    return VK_FORMAT_D32_SFLOAT;
}

static void create_depth_resources(VkCtx &ctx) {
    VkFormat depth_fmt = find_depth_format(ctx);
    vk_create_image(ctx, ctx.swapchain_extent.width, ctx.swapchain_extent.height,
        depth_fmt, VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
        ctx.depth_image, ctx.depth_memory);
    ctx.depth_view = create_image_view(ctx, ctx.depth_image, depth_fmt, VK_IMAGE_ASPECT_DEPTH_BIT);
}

/* --- render pass (color + depth) --- */
static void create_render_pass(VkCtx &ctx) {
    VkAttachmentDescription attachments[2] = {};

    attachments[0].format         = ctx.swapchain_format;
    attachments[0].samples        = VK_SAMPLE_COUNT_1_BIT;
    attachments[0].loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[0].storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[0].initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[0].finalLayout    = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    attachments[1].format         = find_depth_format(ctx);
    attachments[1].samples        = VK_SAMPLE_COUNT_1_BIT;
    attachments[1].loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[1].storeOp        = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[1].initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[1].finalLayout    = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentReference color_ref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkAttachmentReference depth_ref = { 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };

    VkSubpassDescription subpass = {};
    subpass.pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount    = 1;
    subpass.pColorAttachments       = &color_ref;
    subpass.pDepthStencilAttachment = &depth_ref;

    VkRenderPassCreateInfo ci = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    ci.attachmentCount = 2;
    ci.pAttachments    = attachments;
    ci.subpassCount    = 1;
    ci.pSubpasses      = &subpass;

    vk_check(vkCreateRenderPass(ctx.device, &ci, nullptr, &ctx.render_pass), "create render pass");
}

/* --- framebuffers --- */
static void create_framebuffers(VkCtx &ctx) {
    ctx.framebuffers.resize(ctx.swapchain_count);
    for (uint32_t i = 0; i < ctx.swapchain_count; i++) {
        VkImageView attachments[] = { ctx.swapchain_views[i], ctx.depth_view };
        VkFramebufferCreateInfo ci = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
        ci.renderPass      = ctx.render_pass;
        ci.attachmentCount = 2;
        ci.pAttachments    = attachments;
        ci.width           = ctx.swapchain_extent.width;
        ci.height          = ctx.swapchain_extent.height;
        ci.layers          = 1;
        vk_check(vkCreateFramebuffer(ctx.device, &ci, nullptr, &ctx.framebuffers[i]), "create framebuffer");
    }
}

/* --- command pool & buffers --- */
static void create_commands(VkCtx &ctx) {
    VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    pci.queueFamilyIndex = ctx.graphics_family;
    pci.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    vk_check(vkCreateCommandPool(ctx.device, &pci, nullptr, &ctx.command_pool), "create command pool");

    ctx.command_buffers.resize(ctx.swapchain_count);
    VkCommandBufferAllocateInfo ai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    ai.commandPool        = ctx.command_pool;
    ai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = ctx.swapchain_count;
    vk_check(vkAllocateCommandBuffers(ctx.device, &ai, ctx.command_buffers.data()), "allocate command buffers");
}

/* --- sync objects --- */
static void create_sync(VkCtx &ctx) {
    VkSemaphoreCreateInfo si = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    VkFenceCreateInfo     fi = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        vk_check(vkCreateSemaphore(ctx.device, &si, nullptr, &ctx.image_available[i]),  "create semaphore");
        vk_check(vkCreateSemaphore(ctx.device, &si, nullptr, &ctx.render_finished[i]), "create semaphore");
        vk_check(vkCreateFence(ctx.device, &fi, nullptr, &ctx.in_flight[i]),          "create fence");
    }
}

/* --- swapchain recreation --- */
static void cleanup_swapchain(VkCtx &ctx) {
    for (auto fb : ctx.framebuffers) vkDestroyFramebuffer(ctx.device, fb, nullptr);
    ctx.framebuffers.clear();
    vkDestroyImageView(ctx.device, ctx.depth_view, nullptr);
    vkDestroyImage(ctx.device, ctx.depth_image, nullptr);
    vkFreeMemory(ctx.device, ctx.depth_memory, nullptr);
    for (auto iv : ctx.swapchain_views) vkDestroyImageView(ctx.device, iv, nullptr);
    ctx.swapchain_views.clear();
    vkDestroySwapchainKHR(ctx.device, ctx.swapchain, nullptr);
}

static void recreate_swapchain(VkCtx &ctx) {
    vkDeviceWaitIdle(ctx.device);
    cleanup_swapchain(ctx);
    create_swapchain(ctx);
    create_image_views(ctx);
    create_depth_resources(ctx);
    create_framebuffers(ctx);

    vkFreeCommandBuffers(ctx.device, ctx.command_pool,
        (uint32_t)ctx.command_buffers.size(), ctx.command_buffers.data());
    ctx.command_buffers.clear();
    ctx.command_buffers.resize(ctx.swapchain_count);
    VkCommandBufferAllocateInfo ai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    ai.commandPool        = ctx.command_pool;
    ai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = ctx.swapchain_count;
    vk_check(vkAllocateCommandBuffers(ctx.device, &ai, ctx.command_buffers.data()), "re-allocate command buffers");
}

/* --- public API --- */
void vk_init(VkCtx &ctx, GLFWwindow *window) {
    memset(&ctx, 0, sizeof(ctx));
    volkInitialize();
    create_instance(ctx);
    volkLoadInstanceOnly(ctx.instance);
    create_surface(ctx, window);
    pick_physical_device(ctx);
    create_device(ctx);
    create_swapchain(ctx);
    create_image_views(ctx);
    create_depth_resources(ctx);
    create_render_pass(ctx);
    create_framebuffers(ctx);
    create_commands(ctx);
    create_sync(ctx);
}

void vk_shutdown(VkCtx &ctx) {
    vkDeviceWaitIdle(ctx.device);

    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        vkDestroySemaphore(ctx.device, ctx.image_available[i], nullptr);
        vkDestroySemaphore(ctx.device, ctx.render_finished[i], nullptr);
        vkDestroyFence(ctx.device, ctx.in_flight[i], nullptr);
    }
    vkDestroyCommandPool(ctx.device, ctx.command_pool, nullptr);
    cleanup_swapchain(ctx);
    vkDestroyRenderPass(ctx.device, ctx.render_pass, nullptr);
    vkDestroyDevice(ctx.device, nullptr);
    vkDestroySurfaceKHR(ctx.instance, ctx.surface, nullptr);
    vkDestroyInstance(ctx.instance, nullptr);
}

bool vk_begin_frame(VkCtx &ctx, GLFWwindow *window, uint32_t &out_image_index) {
    int fb_w, fb_h;
    glfwGetFramebufferSize(window, &fb_w, &fb_h);
    if (fb_w == 0 || fb_h == 0) return false;

    vkWaitForFences(ctx.device, 1, &ctx.in_flight[ctx.frame_index], VK_TRUE, UINT64_MAX);

    VkResult r = vkAcquireNextImageKHR(ctx.device, ctx.swapchain, UINT64_MAX,
        ctx.image_available[ctx.frame_index], VK_NULL_HANDLE, &out_image_index);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) {
        recreate_swapchain(ctx);
        return false;
    }
    vk_check(r, "acquire next image");

    vkResetFences(ctx.device, 1, &ctx.in_flight[ctx.frame_index]);

    vkResetCommandBuffer(ctx.command_buffers[out_image_index], 0);

    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    vkBeginCommandBuffer(ctx.command_buffers[out_image_index], &bi);
    return true;
}

void vk_end_frame(VkCtx &ctx, uint32_t image_index) {
    vkEndCommandBuffer(ctx.command_buffers[image_index]);

    VkPipelineStageFlags wait_stages[] = { VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT };
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.waitSemaphoreCount   = 1;
    si.pWaitSemaphores      = &ctx.image_available[ctx.frame_index];
    si.pWaitDstStageMask    = wait_stages;
    si.commandBufferCount   = 1;
    si.pCommandBuffers      = &ctx.command_buffers[image_index];
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores    = &ctx.render_finished[ctx.frame_index];

    vk_check(vkQueueSubmit(ctx.graphics_queue, 1, &si, ctx.in_flight[ctx.frame_index]), "queue submit");

    VkPresentInfoKHR pi = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores    = &ctx.render_finished[ctx.frame_index];
    pi.swapchainCount     = 1;
    pi.pSwapchains        = &ctx.swapchain;
    pi.pImageIndices      = &image_index;

    VkResult r = vkQueuePresentKHR(ctx.present_queue, &pi);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) {
        recreate_swapchain(ctx);
    } else {
        vk_check(r, "present");
    }

    ctx.frame_index = (ctx.frame_index + 1) % MAX_FRAMES_IN_FLIGHT;
}
