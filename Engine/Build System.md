# Build System

**Source:** `CMakeLists.txt`

Single CMake project, target `glfw_vulkan`. Everything fetched at configure time via `FetchContent` — no system dependencies beyond a Vulkan-capable driver.

## Fetched dependencies

| Dependency | Version | Role |
|-----------|---------|------|
| GLFW | 3.4 | Window + surface, used by [[Main Loop]] and [[Vulkan Context]] |
| Vulkan-Headers | sdk-1.4.350.0 | API headers |
| volk | sdk-1.4.350.0 | Function loader, compiled into the target |
| GLM | 1.0.1 | Math, used by [[Main Loop]] and [[Renderer]] |
| Dear ImGui | v1.91.6 | UI core + GLFW/Vulkan backends, wrapped by [[ImGui Layer]] |
| glslang | 16.3.0 | Build-time GLSL→SPIR-V compiler |

## Shader embedding

Custom command runs `glslang-standalone -V --variable-name` on the [[Shaders]] and emits `vert_spv.h` / `frag_spv.h` into the build tree; the [[Renderer]] includes them directly. The `shaders` custom target makes the executable depend on regeneration when GLSL changes.

Defines `IMGUI_IMPL_VULKAN_USE_VOLK` so the ImGui Vulkan backend goes through volk.

## Known issues

- No `CMAKE_CXX_STANDARD` pinned — MSVC and CI GCC fall back to different defaults
- `imgui_demo.cpp` compiled but `ShowDemoWindow` is never called
- `cmake/embed_spv.cmake` is tracked but unreferenced — superseded by the glslang `--variable-name` approach
- Building glslang from source dominates clean-build time
