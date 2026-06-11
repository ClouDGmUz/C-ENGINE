# Main Loop

**Source:** `src/main.cpp`

Application entry point. Owns the window, the frame loop, all editor state, and the entire ImGui UI.

## Depends on

- [[Vulkan Context]] — `vk_init`, `vk_begin_frame`, `vk_end_frame`, `vk_shutdown`; reads `swapchain_extent`, `render_pass`, `framebuffers`, `command_buffers`
- [[Renderer]] — `renderer_create`, `renderer_draw`, `renderer_draw_outline`, `renderer_draw_gizmo`, `renderer_destroy`; sets all lighting/background fields each frame
- [[ImGui Layer]] — `imgui_init`, `imgui_new_frame`, `imgui_render`, `imgui_shutdown`

## Responsibilities

- **Camera** — fly camera (struct `Camera`): RMB captures mouse, WASD/QE move, Shift sprint, scroll dollies. Yaw/pitch from cursor delta, pitch clamped to ±89°.
- **Picking** — `screen_to_ray` unprojects cursor, `ray_sphere` picks objects (radius ≈ `scale * 0.5`).
- **Gizmo interaction** — `ray_axis` hover test against thin cylinders along X/Y/Z; `ray_project_on_axis` converts mouse movement to a parameter along the dragged axis. Drives move/scale/rotate depending on `ToolMode`.
- **Undo** — snapshot stack of the whole object vector (max 50). Pushed on drag end, duplicate, delete. No redo; property-panel edits are not tracked.
- **Scene state** — `std::vector<SceneObject>`, selected index, render mode, lighting parameters, background mode. All plain locals fed into the [[Renderer]] each frame.
- **UI** — fixed left panel (tools + outliner) and right panel (properties, render, camera, lighting, background, ray tracing) built with ImGui.

## Known issues

- Shift+D duplicate fires while flying (Shift = sprint, D = strafe) — no `g_captured` guard.
- "Ray Tracing" panel values (`rt_bounces`, `rt_shadows`) are passed to the [[Renderer]] but never reach the [[Shaders]] — placebo controls.
- Picking ray uses window size while projection uses swapchain extent; can disagree mid-resize.
