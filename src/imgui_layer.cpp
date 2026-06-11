#include "imgui_layer.h"
#include "IconsLucide.h"
#include <cstdio>
#include <string>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

static void check_vk(VkResult r) {
    if (r != VK_SUCCESS) { fprintf(stderr, "ImGui Vulkan error: %d\n", r); }
}

/* Resolve an asset path independent of the working directory: try the cwd,
   then the exe directory, then up out of build/<config>/ to the repo root. */
static std::string find_asset(const char *rel) {
    std::string bases[4];
    int n = 0;
    bases[n++] = "";
#ifdef _WIN32
    char buf[MAX_PATH];
    DWORD len = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    if (len > 0 && len < MAX_PATH) {
        std::string dir(buf);
        dir.resize(dir.find_last_of("\\/") + 1);
        bases[n++] = dir;             // assets copied next to the exe
        bases[n++] = dir + "..\\..\\"; // build/<config>/ -> repo root
        bases[n++] = dir + "..\\";
    }
#endif
    for (int i = 0; i < n; i++) {
        std::string p = bases[i] + rel;
        FILE *f = fopen(p.c_str(), "rb");
        if (f) { fclose(f); return p; }
    }
    return "";
}

/* Merge the Lucide icon font into the default font so ICON_LC_* strings
   render inline. Icon glyphs live in the private-use area, no collisions. */
static void load_fonts(ImGuiIO &io) {
    io.Fonts->AddFontDefault();

    std::string path = find_asset("assets/lucide.ttf");
    if (path.empty()) { fprintf(stderr, "lucide.ttf not found, icons disabled\n"); return; }

    static const ImWchar ranges[] = { ICON_MIN_LC, ICON_MAX_LC, 0 };
    ImFontConfig cfg;
    cfg.MergeMode        = true;
    cfg.PixelSnapH       = true;
    cfg.GlyphMinAdvanceX = 15.0f;                 // monospace-ish icon column
    cfg.GlyphOffset      = ImVec2(0.0f, 2.5f);    // align with ProggyClean baseline
    io.Fonts->AddFontFromFileTTF(path.c_str(), 14.0f, &cfg, ranges);
}

void imgui_init(GLFWwindow *window, const VkCtx &ctx) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    load_fonts(io);

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
