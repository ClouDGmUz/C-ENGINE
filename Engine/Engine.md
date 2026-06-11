# Engine

Vulkan 3D editor written in C++ with GLFW windowing, Dear ImGui panels, and procedurally generated shapes. Single executable, no runtime asset files — shaders are compiled to SPIR-V and embedded at build time.

## Modules

- [[Main Loop]] — application entry, camera, gizmo interaction, undo, UI layout
- [[Renderer]] — pipelines, geometry tessellation, draw recording
- [[Vulkan Context]] — instance/device/swapchain bootstrap and frame lifecycle
- [[ImGui Layer]] — Dear ImGui backend glue
- [[Shaders]] — GLSL vertex/fragment shaders, all visual modes
- [[Build System]] — CMake, dependency fetching, shader embedding

## Dependency graph

```
Main Loop ──► Renderer ──► Vulkan Context
    │             │
    │             └──► Shaders (embedded SPIR-V)
    │
    ├──► ImGui Layer ──► Vulkan Context
    │
    └──► Vulkan Context
```

[[Vulkan Context]] depends on nothing internal — only volk and GLFW.
