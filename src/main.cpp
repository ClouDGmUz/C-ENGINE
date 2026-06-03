#include "vk_context.h"
#include "imgui_layer.h"
#include "renderer.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <cstdio>
#include <cstring>
#include <cmath>

#define WIDTH  1280
#define HEIGHT 720

struct Camera {
    glm::vec3 pos   = {2.0f, 1.5f, 2.0f};
    float     yaw   = -135.0f;  // degrees
    float     pitch = -20.0f;
    float     speed = 3.0f;
    float     sensitivity = 0.15f;

    glm::vec3 front() const {
        float ry = glm::radians(yaw), rp = glm::radians(pitch);
        return glm::normalize(glm::vec3(cosf(rp)*cosf(ry), sinf(rp), cosf(rp)*sinf(ry)));
    }
    glm::vec3 right() const { return glm::normalize(glm::cross(front(), {0,1,0})); }
    glm::mat4 view()  const { return glm::lookAt(pos, pos + front(), {0,1,0}); }
};

static Camera   g_cam;
static bool     g_captured = false;
static double   g_last_mx, g_last_my;

static void mouse_button_cb(GLFWwindow *w, int button, int action, int) {
    if (ImGui::GetIO().WantCaptureMouse) return;
    if (button == GLFW_MOUSE_BUTTON_RIGHT) {
        g_captured = (action == GLFW_PRESS);
        glfwSetInputMode(w, GLFW_CURSOR, g_captured ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
        if (g_captured) glfwGetCursorPos(w, &g_last_mx, &g_last_my);
    }
}

static void cursor_pos_cb(GLFWwindow *, double x, double y) {
    if (!g_captured) return;
    float dx = (float)(x - g_last_mx);
    float dy = (float)(y - g_last_my);
    g_last_mx = x; g_last_my = y;
    g_cam.yaw   += dx * g_cam.sensitivity;
    g_cam.pitch -= dy * g_cam.sensitivity;
    if (g_cam.pitch >  89.0f) g_cam.pitch =  89.0f;
    if (g_cam.pitch < -89.0f) g_cam.pitch = -89.0f;
}

static void scroll_cb(GLFWwindow *, double, double yoff) {
    if (ImGui::GetIO().WantCaptureMouse) return;
    g_cam.pos += g_cam.front() * (float)yoff * 0.5f;
}

int main() {
    if (!glfwInit()) { fprintf(stderr, "GLFW init failed\n"); return 1; }
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);

    GLFWwindow *window = glfwCreateWindow(WIDTH, HEIGHT, "Vulkan 3D", nullptr, nullptr);
    if (!window) { fprintf(stderr, "Window creation failed\n"); glfwTerminate(); return 1; }

    glfwSetMouseButtonCallback(window, mouse_button_cb);
    glfwSetCursorPosCallback(window, cursor_pos_cb);
    glfwSetScrollCallback(window, scroll_cb);

    VkCtx ctx{};
    vk_init(ctx, window);
    imgui_init(window, ctx);
    Renderer *renderer = renderer_create(ctx);

    int   current_shape = 0;
    int   pending_shape = -1;
    bool  wireframe     = false;
    int   sphere_detail = 2;
    float bg_col[3]     = {0.1f, 0.15f, 0.2f};
    float obj_yaw       = 0.0f;
    float obj_scale     = 1.0f;
    bool  auto_rotate   = true;
    float rot_speed     = 0.5f;

    double last_time = glfwGetTime();

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        double now = glfwGetTime();
        float dt = (float)(now - last_time);
        last_time = now;

        /* WASD + QE movement while RMB held */
        if (g_captured) {
            float move_speed = g_cam.speed * dt;
            if (glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS) move_speed *= 2.5f;
            glm::vec3 fwd = g_cam.front();
            glm::vec3 rgt = g_cam.right();
            if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS) g_cam.pos += fwd * move_speed;
            if (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS) g_cam.pos -= fwd * move_speed;
            if (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS) g_cam.pos += rgt * move_speed;
            if (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS) g_cam.pos -= rgt * move_speed;
            if (glfwGetKey(window, GLFW_KEY_E) == GLFW_PRESS) g_cam.pos.y += move_speed;
            if (glfwGetKey(window, GLFW_KEY_Q) == GLFW_PRESS) g_cam.pos.y -= move_speed;
        }

        uint32_t img_idx;
        if (!vk_begin_frame(ctx, window, img_idx)) continue;

        if (pending_shape >= 0) {
            renderer_set_shape(renderer, ctx, (ShapeType)pending_shape);
            current_shape = pending_shape;
            pending_shape = -1;
        }

        renderer->wireframe = wireframe;
        renderer->obj_scale = obj_scale;

        if (auto_rotate) obj_yaw = (float)now * rot_speed;
        renderer->obj_yaw = obj_yaw;

        glm::mat4 view = g_cam.view();
        glm::mat4 proj = glm::perspective(glm::radians(60.0f),
            (float)ctx.swapchain_extent.width / (float)ctx.swapchain_extent.height, 0.1f, 100.0f);
        proj[1][1] *= -1;

        VkClearValue clears[2];
        memcpy(clears[0].color.float32, bg_col, 3 * sizeof(float));
        clears[0].color.float32[3] = 1.0f;
        clears[1].depthStencil = { 1.0f, 0 };

        VkRenderPassBeginInfo rpi = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
        rpi.renderPass      = ctx.render_pass;
        rpi.framebuffer     = ctx.framebuffers[img_idx];
        rpi.renderArea      = VkRect2D{ {0, 0}, ctx.swapchain_extent };
        rpi.clearValueCount = 2;
        rpi.pClearValues    = clears;
        vkCmdBeginRenderPass(ctx.command_buffers[img_idx], &rpi, VK_SUBPASS_CONTENTS_INLINE);

        renderer_draw(renderer, ctx, ctx.frame_index, ctx.command_buffers[img_idx], view, proj);

        imgui_new_frame();
        {
            ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_FirstUseEver);
            ImGui::Begin("Controls");

            ImGui::Text("Shape");
            ImGui::SameLine();
            const char *names[] = {"Cube", "Sphere", "Pyramid", "Cylinder"};
            for (int i = 0; i < SHAPE_COUNT; i++) {
                if (i > 0) ImGui::SameLine();
                if (i == current_shape)
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.5f, 0.8f, 1.0f));
                if (ImGui::Button(names[i])) {
                    if (i != current_shape) pending_shape = i;
                }
                if (i == current_shape) ImGui::PopStyleColor();
            }

            ImGui::Separator();
            ImGui::Text("Transform");
            ImGui::Checkbox("Auto Rotate", &auto_rotate);
            if (!auto_rotate)
                ImGui::SliderFloat("Yaw", &obj_yaw, -3.1416f, 3.1416f, "%.2f");
            ImGui::SliderFloat("Rot Speed", &rot_speed, 0.0f, 3.0f, "%.2f");
            ImGui::SliderFloat("Scale", &obj_scale, 0.1f, 3.0f, "%.2f");

            ImGui::Separator();
            ImGui::Text("Camera (RMB + WASD/QE)");
            ImGui::SliderFloat("Speed", &g_cam.speed, 0.5f, 10.0f, "%.1f");
            ImGui::SliderFloat("Sensitivity", &g_cam.sensitivity, 0.05f, 0.5f, "%.2f");
            ImGui::ColorEdit3("Background", bg_col);

            ImGui::Separator();
            if (ImGui::SliderInt("Sphere Detail", &sphere_detail, 1, 3))
                renderer_set_sphere_detail(renderer, ctx, sphere_detail);
            ImGui::Checkbox("Wireframe", &wireframe);
            ImGui::End();
        }
        ImGui::Render();
        imgui_render(ctx.command_buffers[img_idx]);

        vkCmdEndRenderPass(ctx.command_buffers[img_idx]);
        vk_end_frame(ctx, img_idx);
    }

    vkDeviceWaitIdle(ctx.device);
    renderer_destroy(renderer, ctx);
    imgui_shutdown();
    vk_shutdown(ctx);
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
