#pragma once
#include "vk_context.h"
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>

void imgui_init(GLFWwindow *window, const VkCtx &ctx);
void imgui_new_frame();
void imgui_render(VkCommandBuffer cmd);
void imgui_shutdown();
