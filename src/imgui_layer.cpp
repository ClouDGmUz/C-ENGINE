#include "imgui_layer.h"
#include <cstdio>

static void check_vk(VkResult r) {
    if (r != VK_SUCCESS) { fprintf(stderr, "ImGui Vulkan error: %d\n", r); }
}

void imgui_init(GLFWwindow *window, const VkCtx &ctx) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    ImGui::StyleColorsDark();

    ImGui_ImplGlfw_InitForVulkan(window, true);

    ImGui_ImplVulkan_InitInfo init_info = {};
    init_info.Instance           = ctx.instance;
    init_info.PhysicalDevice     = ctx.physical_device;
    init_info.Device             = ctx.device;
    init_info.QueueFamily        = ctx.graphics_family;
    init_info.Queue              = ctx.graphics_queue;
    init_info.PipelineCache      = VK_NULL_HANDLE;
    init_info.DescriptorPool     = VK_NULL_HANDLE;
    init_info.RenderPass         = ctx.render_pass;
    init_info.Subpass            = 0;
    init_info.MinImageCount      = ctx.swapchain_count;
    init_info.ImageCount         = ctx.swapchain_count;
    init_info.MSAASamples        = VK_SAMPLE_COUNT_1_BIT;
    init_info.DescriptorPoolSize = 100;
    init_info.Allocator          = nullptr;
    init_info.CheckVkResultFn    = check_vk;

    ImGui_ImplVulkan_Init(&init_info);
    ImGui_ImplVulkan_CreateFontsTexture();
}

void imgui_new_frame() {
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
}

void imgui_render(VkCommandBuffer cmd) {
    ImDrawData *draw_data = ImGui::GetDrawData();
    if (draw_data)
        ImGui_ImplVulkan_RenderDrawData(draw_data, cmd);
}

void imgui_shutdown() {
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
}
