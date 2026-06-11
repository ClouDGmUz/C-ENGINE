#include "vk_context.h"
#include "imgui_layer.h"
#include "renderer.h"
#include "IconsLucide.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <vector>

#define WIDTH  1440
#define HEIGHT 810

/* ─── Default ImGui dark theme with minor polish ─── */
static void apply_dark_theme() {
    ImGui::StyleColorsDark();
    ImGuiStyle &s = ImGui::GetStyle();
    s.FrameRounding  = 4.0f;
    s.GrabRounding   = 3.0f;
    s.WindowRounding = 6.0f;
    s.FramePadding   = ImVec2(6, 4);
    s.ItemSpacing    = ImVec2(8, 6);
    s.WindowPadding  = ImVec2(12, 10);
}

/* ─── Camera ─── */
struct Camera {
    glm::vec3 pos   = {2.0f, 1.5f, 2.0f};
    float     yaw   = -135.0f;
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

static Camera g_cam;
static bool   g_captured = false;
static double g_last_mx, g_last_my;

/* ─── Gizmo state ─── */
static ToolMode  g_tool     = TOOL_MOVE;
static GizmoAxis g_hovered  = GIZMO_NONE;
static GizmoAxis g_dragging = GIZMO_NONE;
static glm::vec3 g_drag_start_pos;
static float     g_drag_start_scale;
static float     g_drag_start_rot[3];
static float     g_drag_last_t;  // last projected value along axis

/* ─── Undo system ─── */
struct UndoEntry {
    std::vector<SceneObject> objects;
    int selected;
};
static std::vector<UndoEntry> g_undo_stack;
static const int MAX_UNDO = 50;

static void undo_push(const std::vector<SceneObject> &objs, int sel) {
    if ((int)g_undo_stack.size() >= MAX_UNDO)
        g_undo_stack.erase(g_undo_stack.begin());
    g_undo_stack.push_back({objs, sel});
}

/* ─── Key state tracking (for single-press detection) ─── */
static bool g_key_d_was_pressed = false;
static bool g_key_z_was_pressed = false;

/* ─── Ray helpers ─── */
static glm::vec3 screen_to_ray(double mx, double my, int w, int h, const glm::mat4 &view, const glm::mat4 &proj) {
    float x = (2.0f * (float)mx / w - 1.0f);
    float y = (1.0f - 2.0f * (float)my / h); // flip Y for Vulkan
    glm::vec4 clip(x, y, -1.0f, 1.0f);
    glm::vec4 eye = glm::inverse(proj) * clip;
    eye = glm::vec4(eye.x, eye.y, -1.0f, 0.0f);
    glm::vec3 world = glm::vec3(glm::inverse(view) * eye);
    return glm::normalize(world);
}

static bool ray_sphere(glm::vec3 ro, glm::vec3 rd, glm::vec3 center, float radius, float &t) {
    glm::vec3 oc = ro - center;
    float b = glm::dot(oc, rd);
    float c = glm::dot(oc, oc) - radius * radius;
    float disc = b*b - c;
    if (disc < 0) return false;
    t = -b - sqrtf(disc);
    return t > 0;
}

// Distance between the pick ray and an axis segment [origin, origin+axis*len].
// Returns FLT_MAX when the closest point lies outside the segment, so callers
// can pick the nearest axis instead of testing in a fixed X/Y/Z order.
static float ray_axis_dist(glm::vec3 ro, glm::vec3 rd, glm::vec3 origin, glm::vec3 axis, float len) {
    glm::vec3 w = origin - ro;
    float b = glm::dot(rd, axis);
    float d = glm::dot(rd, w);
    float e = glm::dot(axis, w);
    float denom = 1.0f - b * b; // rd, axis normalized
    // parameter along the axis of the closest ray approach
    float s = (denom < 1e-6f) ? 0.0f : (b * d - e) / denom;
    if (s < 0.0f || s > len) return 1e30f;
    glm::vec3 p = origin + axis * s;          // closest point on the axis
    glm::vec3 to_p = p - ro;
    float along_ray = glm::dot(to_p, rd);
    if (along_ray < 0.0f) return 1e30f;       // behind the camera
    return glm::length(to_p - rd * along_ray); // perpendicular miss distance
}

// Project ray onto axis: find the parameter t along 'axis' from 'origin' 
// where the ray is closest to the axis line. Returns the world-space t value.
static float ray_project_on_axis(glm::vec3 ro, glm::vec3 rd, glm::vec3 origin, glm::vec3 axis) {
    // Find closest point between ray (ro + t*rd) and line (origin + s*axis)
    // s = (dot(w,axis)*dot(rd,rd) - dot(w,rd)*dot(rd,axis)) / (dot(rd,rd)*dot(axis,axis) - dot(rd,axis)^2)
    glm::vec3 w = origin - ro;
    float a = glm::dot(rd, rd);    // always 1 if rd normalized
    float b = glm::dot(rd, axis);
    float c = glm::dot(axis, axis); // always 1 if axis normalized
    float d = glm::dot(rd, w);
    float e = glm::dot(axis, w);
    float denom = a * c - b * b;
    if (fabsf(denom) < 1e-6f) return 0.0f;
    float s = (b * d - a * e) / denom;
    return s; // parameter along axis from origin
}

static void mouse_button_cb(GLFWwindow *w, int button, int action, int) {
    if (ImGui::GetIO().WantCaptureMouse) return;
    if (button == GLFW_MOUSE_BUTTON_RIGHT) {
        g_captured = (action == GLFW_PRESS);
        glfwSetInputMode(w, GLFW_CURSOR, g_captured ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
        if (g_captured) glfwGetCursorPos(w, &g_last_mx, &g_last_my);
    }
    if (button == GLFW_MOUSE_BUTTON_LEFT && action == GLFW_RELEASE) {
        g_dragging = GIZMO_NONE;
    }
}

static void cursor_pos_cb(GLFWwindow *, double x, double y) {
    if (!g_captured) return;
    g_cam.yaw   += (float)(x - g_last_mx) * g_cam.sensitivity;
    g_cam.pitch -= (float)(y - g_last_my) * g_cam.sensitivity;
    g_last_mx = x; g_last_my = y;
    if (g_cam.pitch >  89.0f) g_cam.pitch =  89.0f;
    if (g_cam.pitch < -89.0f) g_cam.pitch = -89.0f;
}

static void scroll_cb(GLFWwindow *, double, double yoff) {
    if (ImGui::GetIO().WantCaptureMouse) return;
    g_cam.pos += g_cam.front() * (float)yoff * 0.5f;
}

static int g_obj_counter = 1;

static SceneObject make_default_object(ShapeType shape) {
    SceneObject obj{};
    const char *names[] = {"Cube", "Sphere", "Pyramid", "Cylinder"};
    snprintf(obj.name, sizeof(obj.name), "%s.%03d", names[shape], g_obj_counter++);
    obj.shape = shape;
    float offset = (g_obj_counter - 2) * 1.5f;
    obj.pos[0] = offset; obj.pos[1] = 0; obj.pos[2] = 0;
    obj.rotation[0] = 0; obj.rotation[1] = 0; obj.rotation[2] = 0;
    obj.scale = 1.0f;

    /* default PBR material per shape */
    switch (shape) {
    case SHAPE_SPHERE:   // polished metal — shows off the metalness workflow
        obj.color[0] = 0.95f; obj.color[1] = 0.93f; obj.color[2] = 0.88f;
        obj.roughness = 0.25f; obj.metallic = 1.0f; break;
    case SHAPE_PYRAMID:  // sandstone
        obj.color[0] = 0.85f; obj.color[1] = 0.65f; obj.color[2] = 0.35f;
        obj.roughness = 0.8f; obj.metallic = 0.0f; break;
    case SHAPE_CYLINDER: // painted blue
        obj.color[0] = 0.30f; obj.color[1] = 0.50f; obj.color[2] = 0.80f;
        obj.roughness = 0.4f; obj.metallic = 0.0f; break;
    case SHAPE_CUBE:     // terracotta
    default:
        obj.color[0] = 0.80f; obj.color[1] = 0.35f; obj.color[2] = 0.25f;
        obj.roughness = 0.55f; obj.metallic = 0.0f; break;
    }
    return obj;
}

int main(int argc, char **argv) {
    if (!glfwInit()) { fprintf(stderr, "GLFW init failed\n"); return 1; }
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);

    GLFWwindow *window = glfwCreateWindow(WIDTH, HEIGHT, "Vulkan 3D Engine", nullptr, nullptr);
    if (!window) { fprintf(stderr, "Window creation failed\n"); glfwTerminate(); return 1; }

    glfwSetMouseButtonCallback(window, mouse_button_cb);
    glfwSetCursorPosCallback(window, cursor_pos_cb);
    glfwSetScrollCallback(window, scroll_cb);

    VkCtx ctx{};
    vk_init(ctx, window);
    imgui_init(window, ctx);
    apply_dark_theme();
    Renderer *renderer = renderer_create(ctx);

    /* scene objects */
    std::vector<SceneObject> objects;
    objects.push_back(make_default_object(SHAPE_CUBE));
    int selected_obj = 0;

    int   render_mode   = 2;
    int   sphere_detail = 2;
    float bg_col[3]     = {0.08f, 0.08f, 0.08f};
    float light_dir[3]  = {1.0f, 2.0f, 1.0f};
    float light_intensity = 1.2f;
    float light_ambient   = 0.25f;
    float sun_color[3]     = {1.0f, 0.96f, 0.88f};
    float shadow_softness  = 0.3f;
    float sun_size         = 0.54f;  // degrees — real sun is ~0.53°
    float exposure         = 1.0f;

    bool  shadows_on       = true;

    float xray_density     = 1.0f;

    int   bg_mode       = 0;
    float sky_top[3]    = {0.25f, 0.45f, 0.75f};
    float sky_bottom[3] = {0.75f, 0.82f, 0.9f};
    float sky_turbidity  = 2.5f;
    float cloud_coverage = 0.35f;
    float cloud_speed    = 1.0f;
    bool  fog_on        = true;
    float fog_color[3]  = {0.6f, 0.66f, 0.75f};
    float fog_density   = 0.02f;

    /* debug startup state: --bg <0..2>, --rm <0..3> */
    for (int i = 1; i < argc - 1; i++) {
        if (!strcmp(argv[i], "--bg")) bg_mode     = atoi(argv[i + 1]);
        if (!strcmp(argv[i], "--rm")) render_mode = atoi(argv[i + 1]);
    }

    double last_time = glfwGetTime();
    const float LEFT_W = 220.0f;
    const float RIGHT_W = 300.0f;

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        double now = glfwGetTime();
        float dt = (float)(now - last_time);
        last_time = now;

        if (g_captured) {
            float ms = g_cam.speed * dt;
            if (glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS) ms *= 2.5f;
            glm::vec3 fwd = g_cam.front(), rgt = g_cam.right();
            if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS) g_cam.pos += fwd * ms;
            if (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS) g_cam.pos -= fwd * ms;
            if (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS) g_cam.pos += rgt * ms;
            if (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS) g_cam.pos -= rgt * ms;
            if (glfwGetKey(window, GLFW_KEY_E) == GLFW_PRESS) g_cam.pos.y += ms;
            if (glfwGetKey(window, GLFW_KEY_Q) == GLFW_PRESS) g_cam.pos.y -= ms;
        }

        /* ─── Mouse picking & gizmo interaction ─── */
        int win_w, win_h;
        glfwGetWindowSize(window, &win_w, &win_h);
        double mx, my;
        glfwGetCursorPos(window, &mx, &my);

        glm::mat4 view = g_cam.view();
        glm::mat4 proj = glm::perspective(glm::radians(60.0f),
            (float)ctx.swapchain_extent.width / (float)ctx.swapchain_extent.height, 0.1f, 100.0f);
        proj[1][1] *= -1;

        bool mouse_in_viewport = !ImGui::GetIO().WantCaptureMouse;
        glm::vec3 ray_dir = screen_to_ray(mx, my, win_w, win_h, view, proj);
        glm::vec3 ray_origin = g_cam.pos;

        // Gizmo hover detection — hit targets match the drawn billboard arrows.
        // All three axes are tested and the NEAREST one wins, so clicks land
        // on the arrow under the cursor instead of favoring X then Y then Z.
        float gizmo_len = (g_tool == TOOL_SCALE) ? 0.8f : 1.2f;
        g_hovered = GIZMO_NONE;
        if (mouse_in_viewport && selected_obj >= 0 && selected_obj < (int)objects.size() && g_dragging == GIZMO_NONE) {
            glm::vec3 obj_pos(objects[selected_obj].pos[0], objects[selected_obj].pos[1], objects[selected_obj].pos[2]);
            float gdist = glm::length(g_cam.pos - obj_pos);
            float shaft_w = glm::clamp(gdist * 0.006f, 0.008f, 0.06f);
            float hit_r = shaft_w * 2.5f;            // arrow width + small grace zone
            float hit_l = gizmo_len + shaft_w * 9.0f; // include the arrowhead
            float best = hit_r;
            const glm::vec3 axes[3] = { {1,0,0}, {0,1,0}, {0,0,1} };
            for (int a = 0; a < 3; a++) {
                float dist = ray_axis_dist(ray_origin, ray_dir, obj_pos, axes[a], hit_l);
                if (dist < best) { best = dist; g_hovered = (GizmoAxis)(GIZMO_X + a); }
            }
        }

        // LMB press: start drag on gizmo or pick object
        if (mouse_in_viewport && glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS && g_dragging == GIZMO_NONE) {
            if (g_hovered != GIZMO_NONE) {
                g_dragging = g_hovered;
                g_drag_start_pos = glm::vec3(objects[selected_obj].pos[0], objects[selected_obj].pos[1], objects[selected_obj].pos[2]);
                g_drag_start_scale = objects[selected_obj].scale;
                g_drag_start_rot[0] = objects[selected_obj].rotation[0];
                g_drag_start_rot[1] = objects[selected_obj].rotation[1];
                g_drag_start_rot[2] = objects[selected_obj].rotation[2];

                // Compute initial projection along axis
                glm::vec3 axis(0);
                if (g_dragging == GIZMO_X) axis = {1,0,0};
                if (g_dragging == GIZMO_Y) axis = {0,1,0};
                if (g_dragging == GIZMO_Z) axis = {0,0,1};
                g_drag_last_t = ray_project_on_axis(ray_origin, ray_dir, g_drag_start_pos, axis);
            } else {
                // Pick object by ray-sphere intersection
                float best_t = 1e30f;
                int best_i = -1;
                for (int i = 0; i < (int)objects.size(); i++) {
                    glm::vec3 center(objects[i].pos[0], objects[i].pos[1], objects[i].pos[2]);
                    float r = objects[i].scale * 0.5f;
                    float t;
                    if (ray_sphere(ray_origin, ray_dir, center, r, t) && t < best_t) {
                        best_t = t; best_i = i;
                    }
                }
                if (best_i >= 0) selected_obj = best_i;
            }
        }

        // Dragging: move or scale along axis using ray-axis projection
        if (g_dragging != GIZMO_NONE && selected_obj >= 0 && selected_obj < (int)objects.size()) {
            glm::vec3 axis(0);
            if (g_dragging == GIZMO_X) axis = {1,0,0};
            if (g_dragging == GIZMO_Y) axis = {0,1,0};
            if (g_dragging == GIZMO_Z) axis = {0,0,1};

            // Project current ray onto the axis
            glm::vec3 current_obj_pos(objects[selected_obj].pos[0], objects[selected_obj].pos[1], objects[selected_obj].pos[2]);
            float current_t = ray_project_on_axis(ray_origin, ray_dir, g_drag_start_pos, axis);
            float delta = current_t - g_drag_last_t;

            if (g_tool == TOOL_MOVE) {
                objects[selected_obj].pos[0] += axis.x * delta;
                objects[selected_obj].pos[1] += axis.y * delta;
                objects[selected_obj].pos[2] += axis.z * delta;
            } else if (g_tool == TOOL_SCALE) {
                objects[selected_obj].scale += delta;
                if (objects[selected_obj].scale < 0.01f) objects[selected_obj].scale = 0.01f;
            } else if (g_tool == TOOL_ROTATE) {
                float angle = delta * 2.0f;
                if (g_dragging == GIZMO_X) objects[selected_obj].rotation[0] += angle;
                if (g_dragging == GIZMO_Y) objects[selected_obj].rotation[1] += angle;
                if (g_dragging == GIZMO_Z) objects[selected_obj].rotation[2] += angle;
            }
            g_drag_last_t = current_t;
        }

        // Release drag — push undo when transform ends
        if (glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_RELEASE) {
            if (g_dragging != GIZMO_NONE) {
                undo_push(objects, selected_obj);
            }
            g_dragging = GIZMO_NONE;
        }

        /* ─── Shift+D: Duplicate selected object ─── */
        {
            bool d_pressed = glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS;
            bool shift_held = glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS ||
                              glfwGetKey(window, GLFW_KEY_RIGHT_SHIFT) == GLFW_PRESS;
            if (d_pressed && shift_held && !g_key_d_was_pressed && !ImGui::GetIO().WantCaptureKeyboard) {
                if (selected_obj >= 0 && selected_obj < (int)objects.size()) {
                    undo_push(objects, selected_obj);
                    SceneObject dup = objects[selected_obj];
                    snprintf(dup.name, sizeof(dup.name), "%s.dup", objects[selected_obj].name);
                    dup.pos[0] += 1.0f; // offset so it's visible
                    objects.push_back(dup);
                    selected_obj = (int)objects.size() - 1;
                }
            }
            g_key_d_was_pressed = d_pressed && shift_held;
        }

        /* ─── Ctrl+Z: Undo ─── */
        {
            bool z_pressed = glfwGetKey(window, GLFW_KEY_Z) == GLFW_PRESS;
            bool ctrl_held = glfwGetKey(window, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS ||
                             glfwGetKey(window, GLFW_KEY_RIGHT_CONTROL) == GLFW_PRESS;
            if (z_pressed && ctrl_held && !g_key_z_was_pressed && !ImGui::GetIO().WantCaptureKeyboard) {
                if (!g_undo_stack.empty()) {
                    UndoEntry &e = g_undo_stack.back();
                    objects = e.objects;
                    selected_obj = e.selected;
                    g_undo_stack.pop_back();
                }
            }
            g_key_z_was_pressed = z_pressed && ctrl_held;
        }

        uint32_t img_idx;
        if (!vk_begin_frame(ctx, window, img_idx)) continue;
        VkCommandBuffer cmd = ctx.command_buffers[ctx.frame_index];

        renderer->render_mode = render_mode;
        renderer->light_intensity = light_intensity;
        renderer->light_ambient = light_ambient;
        renderer->light_dir[0] = light_dir[0];
        renderer->light_dir[1] = light_dir[1];
        renderer->light_dir[2] = light_dir[2];
        renderer->sun_color[0] = sun_color[0]; renderer->sun_color[1] = sun_color[1]; renderer->sun_color[2] = sun_color[2];
        renderer->shadow_softness = shadow_softness;
        renderer->sun_size = sun_size;
        renderer->exposure = exposure;
        renderer->xray_density = xray_density;
        renderer->bg_mode = bg_mode;
        renderer->sky_top[0] = sky_top[0]; renderer->sky_top[1] = sky_top[1]; renderer->sky_top[2] = sky_top[2];
        renderer->sky_bottom[0] = sky_bottom[0]; renderer->sky_bottom[1] = sky_bottom[1]; renderer->sky_bottom[2] = sky_bottom[2];
        renderer->sky_turbidity = sky_turbidity;
        renderer->shadows_enabled = shadows_on;
        renderer->cloud_coverage  = cloud_coverage;
        renderer->cloud_speed     = cloud_speed;
        renderer->fog_enabled     = fog_on;
        renderer->fog_color[0] = fog_color[0]; renderer->fog_color[1] = fog_color[1]; renderer->fog_color[2] = fog_color[2];
        renderer->fog_density     = fog_density;
        renderer->time            = (float)now;
        renderer->cam_pos[0] = g_cam.pos.x; renderer->cam_pos[1] = g_cam.pos.y; renderer->cam_pos[2] = g_cam.pos.z;

        /* shadow map pass + scene upload — must precede the main render pass */
        renderer_shadow_pass(renderer, ctx, ctx.frame_index, cmd,
                             objects.data(), (int)objects.size());

        VkClearValue clears[2];
        if (bg_mode > 0) {
            memcpy(clears[0].color.float32, sky_bottom, 3 * sizeof(float));
        } else {
            memcpy(clears[0].color.float32, bg_col, 3 * sizeof(float));
        }
        clears[0].color.float32[3] = 1.0f;
        clears[1].depthStencil = { 1.0f, 0 };

        VkRenderPassBeginInfo rpi = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
        rpi.renderPass      = ctx.render_pass;
        rpi.framebuffer     = ctx.framebuffers[img_idx];
        rpi.renderArea      = VkRect2D{ {0, 0}, ctx.swapchain_extent };
        rpi.clearValueCount = 2;
        rpi.pClearValues    = clears;
        vkCmdBeginRenderPass(cmd, &rpi, VK_SUBPASS_CONTENTS_INLINE);

        renderer_draw(renderer, ctx, ctx.frame_index, cmd, view, proj,
                      objects.data(), (int)objects.size());

        /* Draw selection outline + gizmo on selected object */
        if (selected_obj >= 0 && selected_obj < (int)objects.size()) {
            renderer_draw_outline(renderer, ctx, cmd, view, proj,
                                  objects[selected_obj], selected_obj, ctx.frame_index);
            renderer_draw_gizmo(renderer, ctx, ctx.frame_index, cmd, view, proj,
                                objects[selected_obj], g_tool, g_dragging != GIZMO_NONE ? g_dragging : g_hovered);
        }

        /* ─── ImGui ─── */
        imgui_new_frame();

        /* ── Left panel: Tools + Outliner ── */
        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
        ImGui::SetNextWindowSize(ImVec2(LEFT_W, (float)win_h));
        ImGui::Begin("Tools", nullptr,
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar);

        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.8f, 1.0f, 1.0f));
        ImGui::Text(ICON_LC_WRENCH "  TOOLS");
        ImGui::PopStyleColor();
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        /* Tool selector */
        bool move_sel   = (g_tool == TOOL_MOVE);
        bool scale_sel  = (g_tool == TOOL_SCALE);
        bool rotate_sel = (g_tool == TOOL_ROTATE);

        if (move_sel) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.3f, 0.5f, 0.8f, 1.0f));
        if (ImGui::Button(ICON_LC_MOVE "  Move (G)", ImVec2(-1, 0))) g_tool = TOOL_MOVE;
        if (move_sel) ImGui::PopStyleColor();

        if (scale_sel) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.3f, 0.5f, 0.8f, 1.0f));
        if (ImGui::Button(ICON_LC_SCALE_3D "  Scale (S)", ImVec2(-1, 0))) g_tool = TOOL_SCALE;
        if (scale_sel) ImGui::PopStyleColor();

        if (rotate_sel) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.3f, 0.5f, 0.8f, 1.0f));
        if (ImGui::Button(ICON_LC_ROTATE_3D "  Rotate (R)", ImVec2(-1, 0))) g_tool = TOOL_ROTATE;
        if (rotate_sel) ImGui::PopStyleColor();

        /* Keyboard shortcuts for tools */
        if (!ImGui::GetIO().WantCaptureKeyboard) {
            if (glfwGetKey(window, GLFW_KEY_G) == GLFW_PRESS) g_tool = TOOL_MOVE;
            if (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS && !g_captured) g_tool = TOOL_SCALE;
            if (glfwGetKey(window, GLFW_KEY_R) == GLFW_PRESS) g_tool = TOOL_ROTATE;
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.8f, 1.0f, 1.0f));
        ImGui::Text(ICON_LC_LIST_TREE "  OUTLINER");
        ImGui::PopStyleColor();
        ImGui::Spacing();

        /* Add object buttons */
        const char *shape_names[] = {"Cube", "Sphere", "Pyramid", "Cylinder"};
        const char *shape_icons[] = {ICON_LC_BOX, ICON_LC_CIRCLE, ICON_LC_PYRAMID, ICON_LC_CYLINDER};
        if (ImGui::Button(ICON_LC_PLUS "  Add", ImVec2(-1, 0)))
            ImGui::OpenPopup("AddObject");
        if (ImGui::BeginPopup("AddObject")) {
            for (int i = 0; i < SHAPE_COUNT; i++) {
                char item[64];
                snprintf(item, sizeof(item), "%s  %s", shape_icons[i], shape_names[i]);
                if (ImGui::MenuItem(item)) {
                    objects.push_back(make_default_object((ShapeType)i));
                    selected_obj = (int)objects.size() - 1;
                }
            }
            ImGui::EndPopup();
        }

        ImGui::Spacing();

        /* Object list */
        for (int i = 0; i < (int)objects.size(); i++) {
            bool is_selected = (i == selected_obj);
            char row[80];
            snprintf(row, sizeof(row), "%s %s", shape_icons[objects[i].shape], objects[i].name);
            if (ImGui::Selectable(row, is_selected))
                selected_obj = i;
        }

        ImGui::Spacing();
        if (selected_obj >= 0 && selected_obj < (int)objects.size()) {
            if (ImGui::Button(ICON_LC_TRASH_2 "  Delete", ImVec2(-1, 0))) {
                undo_push(objects, selected_obj);
                objects.erase(objects.begin() + selected_obj);
                if (selected_obj >= (int)objects.size())
                    selected_obj = (int)objects.size() - 1;
            }
        }

        ImGui::End();

        /* ── Right panel: Properties ── */
        ImGui::SetNextWindowPos(ImVec2((float)win_w - RIGHT_W, 0.0f));
        ImGui::SetNextWindowSize(ImVec2(RIGHT_W, (float)win_h));
        ImGui::Begin("Properties", nullptr,
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar);

        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.8f, 1.0f, 1.0f));
        ImGui::Text(ICON_LC_SLIDERS_HORIZONTAL "  PROPERTIES");
        ImGui::PopStyleColor();
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        /* ── Object properties (selected) ── */
        if (selected_obj >= 0 && selected_obj < (int)objects.size()) {
            SceneObject &obj = objects[selected_obj];
            if (ImGui::CollapsingHeader(ICON_LC_BOX "  Object", ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::Spacing();
                ImGui::InputText("Name", obj.name, sizeof(obj.name));
                int shape_idx = (int)obj.shape;
                if (ImGui::Combo("Shape", &shape_idx, shape_names, SHAPE_COUNT))
                    obj.shape = (ShapeType)shape_idx;
                ImGui::Spacing();
            }

            if (ImGui::CollapsingHeader(ICON_LC_AXIS_3D "  Transform", ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::Spacing();
                ImGui::DragFloat3("Position", obj.pos, 0.05f);
                ImGui::DragFloat3("Rotation", obj.rotation, 0.02f);
                ImGui::DragFloat("Scale", &obj.scale, 0.02f, 0.01f, 10.0f);
                ImGui::Spacing();
            }

            if (ImGui::CollapsingHeader(ICON_LC_PALETTE "  Material", ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::Spacing();
                ImGui::ColorEdit3("Color", obj.color);
                ImGui::SliderFloat("Roughness", &obj.roughness, 0.02f, 1.0f, "%.2f");
                ImGui::SliderFloat("Metallic", &obj.metallic, 0.0f, 1.0f, "%.2f");
                const char *tex_names[] = {"None", "Checker", "Brick", "Marble", "Wood"};
                ImGui::Combo("Pattern", &obj.tex_mode, tex_names, 5);
                ImGui::Spacing();
            }
        }

        /* ── Render Mode ── */
        if (ImGui::CollapsingHeader(ICON_LC_MONITOR "  Render", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Spacing();
            ImGui::RadioButton("Wireframe", &render_mode, 0); ImGui::SameLine();
            ImGui::RadioButton("Solid",     &render_mode, 1);
            ImGui::RadioButton("Rendered",  &render_mode, 2); ImGui::SameLine();
            ImGui::RadioButton("X-Ray",     &render_mode, 3);
            if (render_mode == 3) {
                ImGui::SliderFloat("Density", &xray_density, 0.1f, 4.0f, "%.2f");
                ImGui::TextDisabled("Layers accumulate like a radiograph");
            }
            ImGui::SliderInt("Subdivisions", &sphere_detail, 1, 3);
            renderer->sphere_detail = sphere_detail;
            ImGui::Spacing();
        }

        /* ── Camera ── */
        if (ImGui::CollapsingHeader(ICON_LC_VIDEO "  Camera")) {
            ImGui::Spacing();
            ImGui::TextDisabled("RMB + WASD/QE  |  Scroll to dolly");
            ImGui::SliderFloat("Move Speed", &g_cam.speed, 0.5f, 10.0f, "%.1f");
            ImGui::SliderFloat("Sensitivity", &g_cam.sensitivity, 0.05f, 0.5f, "%.2f");
            ImGui::Text("Pos: %.1f, %.1f, %.1f", g_cam.pos.x, g_cam.pos.y, g_cam.pos.z);
            ImGui::Spacing();
        }

        /* ── Lighting ── */
        if (ImGui::CollapsingHeader(ICON_LC_SUN "  Lighting", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Spacing();
            const char *presets[] = {"Custom", "Daylight", "Sunset", "Moonlight", "Studio", "Overcast"};
            static int preset = 0;
            if (ImGui::Combo("Preset", &preset, presets, 6)) {
                switch (preset) {
                case 1: // Daylight: high sun, warm white, crisp shadows
                    light_dir[0]=1; light_dir[1]=2; light_dir[2]=0.5f;
                    light_intensity=1.5f; light_ambient=0.35f;
                    sun_color[0]=1; sun_color[1]=0.96f; sun_color[2]=0.88f;
                    shadow_softness=0.2f; sun_size=0.54f; exposure=1.0f; break;
                case 2: // Sunset: low sun, deep orange, long soft shadows
                    light_dir[0]=2; light_dir[1]=0.25f; light_dir[2]=-1;
                    light_intensity=1.2f; light_ambient=0.15f;
                    sun_color[0]=1; sun_color[1]=0.45f; sun_color[2]=0.15f;
                    shadow_softness=0.5f; sun_size=0.9f; exposure=1.1f; break;
                case 3: // Moonlight: dim cool light, dark ambient
                    light_dir[0]=-0.5f; light_dir[1]=1.5f; light_dir[2]=0.5f;
                    light_intensity=0.25f; light_ambient=0.08f;
                    sun_color[0]=0.55f; sun_color[1]=0.65f; sun_color[2]=1.0f;
                    shadow_softness=0.4f; sun_size=0.52f; exposure=1.6f; break;
                case 4: // Studio: big neutral softbox
                    light_dir[0]=0.3f; light_dir[1]=2; light_dir[2]=1;
                    light_intensity=1.8f; light_ambient=0.5f;
                    sun_color[0]=1; sun_color[1]=1; sun_color[2]=1;
                    shadow_softness=0.15f; sun_size=3.0f; exposure=0.9f; break;
                case 5: // Overcast: top-down diffuse, huge soft source
                    light_dir[0]=0; light_dir[1]=1; light_dir[2]=0;
                    light_intensity=0.6f; light_ambient=0.55f;
                    sun_color[0]=0.8f; sun_color[1]=0.84f; sun_color[2]=0.9f;
                    shadow_softness=0.9f; sun_size=6.0f; exposure=1.0f; break;
                }
            }
            ImGui::Separator();
            ImGui::Spacing();
            ImGui::Text("Sun");
            ImGui::ColorEdit3("Sun Color", sun_color, ImGuiColorEditFlags_NoInputs);
            ImGui::SliderFloat3("Direction", light_dir, -3.0f, 3.0f, "%.1f");
            ImGui::SliderFloat("Intensity", &light_intensity, 0.0f, 3.0f, "%.2f");
            ImGui::SliderFloat("Sun Size", &sun_size, 0.1f, 10.0f, "%.2f deg");
            ImGui::Spacing();
            ImGui::Text("Shadows");
            ImGui::Checkbox("Enabled##shadows", &shadows_on);
            ImGui::SliderFloat("Softness", &shadow_softness, 0.0f, 1.0f, "%.2f");
            ImGui::Spacing();
            ImGui::Text("Camera");
            ImGui::SliderFloat("Ambient", &light_ambient, 0.0f, 1.0f, "%.2f");
            ImGui::SliderFloat("Exposure", &exposure, 0.1f, 4.0f, "%.2f");
            ImGui::Spacing();
        }

        /* ── Environment ── */
        if (ImGui::CollapsingHeader(ICON_LC_CLOUD_SUN "  Environment", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Spacing();
            const char *bg_names[] = {"Solid", "Gradient Sky", "Atmosphere"};
            ImGui::Combo("Mode", &bg_mode, bg_names, 3);
            if (bg_mode == 0) {
                ImGui::ColorEdit3("Color", bg_col, ImGuiColorEditFlags_NoInputs);
            } else if (bg_mode == 1) {
                ImGui::ColorEdit3("Sky Zenith", sky_top, ImGuiColorEditFlags_NoInputs);
                ImGui::SameLine();
                ImGui::ColorEdit3("Horizon", sky_bottom, ImGuiColorEditFlags_NoInputs);
            }
            if (bg_mode == 2) {
                // Physical scattering: color comes from sun position + haze
                ImGui::SliderFloat("Turbidity", &sky_turbidity, 1.0f, 10.0f, "%.1f");
                ImGui::TextDisabled("Sky color follows the sun direction");
                ImGui::Spacing();
                ImGui::Text("Clouds");
                ImGui::SliderFloat("Coverage", &cloud_coverage, 0.0f, 1.0f, "%.2f");
                ImGui::SliderFloat("Speed", &cloud_speed, 0.0f, 5.0f, "%.1f");
            }
            ImGui::Spacing();
            ImGui::Text("Fog");
            ImGui::Checkbox("Enabled##fog", &fog_on);
            if (fog_on) {
                ImGui::ColorEdit3("Fog Color", fog_color, ImGuiColorEditFlags_NoInputs);
                ImGui::SameLine();
                if (ImGui::SmallButton("Match Sky"))
                    memcpy(fog_color, sky_bottom, 3 * sizeof(float));
                ImGui::SliderFloat("Density", &fog_density, 0.0f, 0.15f, "%.3f");
            }
            ImGui::Spacing();
        }

        /* ── Footer ── */
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::TextDisabled(ICON_LC_GAUGE " %.1f FPS | %d objects", ImGui::GetIO().Framerate, (int)objects.size());

        ImGui::End();

        ImGui::Render();
        imgui_render(cmd);

        vkCmdEndRenderPass(cmd);
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
