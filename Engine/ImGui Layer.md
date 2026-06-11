# ImGui Layer

**Source:** `src/imgui_layer.h`, `src/imgui_layer.cpp`

Thin glue around the Dear ImGui GLFW + Vulkan backends. Four functions: init, new-frame, render, shutdown. All actual UI widgets live in the [[Main Loop]].

## Depends on

- [[Vulkan Context]] — `VkCtx` handles (instance, physical device, device, graphics queue/family, render pass, swapchain image count) passed to `ImGui_ImplVulkan_Init`

## Used by

- [[Main Loop]] — only caller

## Notes

- Built with `IMGUI_IMPL_VULKAN_USE_VOLK` (defined by the [[Build System]]) so the backend resolves Vulkan functions through volk
- `DescriptorPool = VK_NULL_HANDLE` with `DescriptorPoolSize = 100` — backend creates its own pool (ImGui ≥ 1.91 feature)
- Renders into the main render pass, subpass 0, after scene geometry
- `MinImageCount` is set to the actual swapchain image count; swapchain recreation does not call `ImGui_ImplVulkan_SetMinImageCount` (harmless while the count never changes)
