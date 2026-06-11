# Vulkan Context

**Source:** `src/vk_context.h`, `src/vk_context.cpp`

Bootstraps and owns every core Vulkan object: instance, surface, device, swapchain, depth buffer, render pass, framebuffers, command buffers, sync primitives. Exposes the `VkCtx` struct that every other module reads.

## Depends on

Nothing internal. External: volk (loader), GLFW (surface + required instance extensions).

## Used by

- [[Main Loop]] — frame lifecycle (`vk_begin_frame` / `vk_end_frame`), render pass begin/end
- [[Renderer]] — device, render pass, memory-type helper, `vk_check`
- [[ImGui Layer]] — instance/device/queue handles for backend init

## Frame lifecycle

- `vk_begin_frame` — wait frame fence, acquire swapchain image, reset + begin this frame's command buffer; returns false on zero-size framebuffer or `VK_ERROR_OUT_OF_DATE_KHR` (recreates). `VK_SUBOPTIMAL_KHR` at acquire is treated as success (image and semaphore are live); recreation happens after present instead
- `vk_end_frame` — submit with `image_available[frame]` wait / `render_finished[image]` signal, present, advance `frame_index` (`MAX_FRAMES_IN_FLIGHT = 2`)
- Command buffers are per in-flight frame (guarded by that frame's fence); `render_finished` semaphores are per swapchain image
- Swapchain recreation on resize: device-wait-idle, rebuild swapchain/views/depth/framebuffers, recreate per-image `render_finished` semaphores

## Configuration

- Picks discrete GPU over integrated via simple scoring
- B8G8R8A8_SRGB format preferred, FIFO present mode (vsync)
- Validation layer `VK_LAYER_KHRONOS_validation` enabled in debug builds when available
- Single render pass: color (clear→present) + depth (clear, don't-care store), with an EXTERNAL subpass dependency ordering the shared depth image between in-flight frames
- `fillModeNonSolid` device feature enabled for the wireframe pipeline in [[Renderer]]
