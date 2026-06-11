#pragma once
#include "vk_context.h"
#include <glm/glm.hpp>

#define MAX_OBJECTS 64

enum ShapeType { SHAPE_CUBE = 0, SHAPE_SPHERE, SHAPE_PYRAMID, SHAPE_CYLINDER, SHAPE_COUNT };

struct SceneObject {
    char      name[32];
    ShapeType shape;
    float     pos[3];
    float     rotation[3]; // euler angles in radians
    float     scale;
    int       tex_mode;    // 0=none, 1=checker, 2=brick, 3=marble, 4=wood
    /* PBR material */
    float     color[3];    // albedo
    float     roughness;   // 0..1
    float     metallic;    // 0..1
};

/* Tool modes for gizmo */
enum ToolMode { TOOL_MOVE = 0, TOOL_SCALE, TOOL_ROTATE };

/* Which gizmo axis is hovered/active */
enum GizmoAxis { GIZMO_NONE = 0, GIZMO_X, GIZMO_Y, GIZMO_Z };

/* Where each object's mesh landed in the per-frame buffers */
struct ObjDrawInfo {
    uint32_t idx_count;
    uint32_t idx_offset;
    int32_t  vtx_offset;
};

struct Renderer {
    VkPipelineLayout    pipeline_layout;
    VkPipeline          pipeline;
    VkPipeline          pipeline_wireframe;
    VkPipeline          pipeline_xray;    // additive blend, no cull, no depth
    VkPipeline          pipeline_overlay; // gizmo: no cull, no depth, on top

    /* descriptor set: binding 0 = scene UBO, binding 1 = shadow map */
    VkDescriptorSetLayout desc_layout;
    VkDescriptorPool      desc_pool;
    VkDescriptorSet       desc_sets[MAX_FRAMES_IN_FLIGHT];
    VkBuffer              ubo_buffer[MAX_FRAMES_IN_FLIGHT];
    VkDeviceMemory        ubo_memory[MAX_FRAMES_IN_FLIGHT];
    void                 *ubo_mapped[MAX_FRAMES_IN_FLIGHT];

    /* shadow map (directional light, depth-only pass) */
    VkImage          shadow_image;
    VkDeviceMemory   shadow_memory;
    VkImageView      shadow_view;
    VkSampler        shadow_sampler;
    VkRenderPass     shadow_render_pass;
    VkFramebuffer    shadow_framebuffer;
    VkPipeline       shadow_pipeline;

    /* per-frame dynamic buffers: CPU writes frame N while GPU may still
       read frame N-1, so each in-flight frame gets its own copy.
       Persistently mapped (host-visible + coherent). */
    VkBuffer            vertex_buffer[MAX_FRAMES_IN_FLIGHT];
    VkDeviceMemory      vertex_memory[MAX_FRAMES_IN_FLIGHT];
    void               *vertex_mapped[MAX_FRAMES_IN_FLIGHT];
    VkBuffer            index_buffer[MAX_FRAMES_IN_FLIGHT];
    VkDeviceMemory      index_memory[MAX_FRAMES_IN_FLIGHT];
    void               *index_mapped[MAX_FRAMES_IN_FLIGHT];

    /* sky fullscreen tri */
    VkBuffer            sky_vb[MAX_FRAMES_IN_FLIGHT];
    VkDeviceMemory      sky_vm[MAX_FRAMES_IN_FLIGHT];
    void               *sky_mapped[MAX_FRAMES_IN_FLIGHT];

    /* gizmo: arrows (move/scale) or 48-segment rings (rotate).
       Worst case 3 axes x 48 seg x 6 verts = 864; buffer holds 1024. */
    VkBuffer            gizmo_vb[MAX_FRAMES_IN_FLIGHT];
    VkDeviceMemory      gizmo_vm[MAX_FRAMES_IN_FLIGHT];
    void               *gizmo_mapped[MAX_FRAMES_IN_FLIGHT];

    uint32_t            max_vertex_bytes;
    int                 render_mode; // 0=wireframe, 1=solid, 2=rendered, 3=x-ray
    int                 sphere_detail;

    /* draw list written by renderer_shadow_pass (upload), reused by
       renderer_draw and renderer_draw_outline */
    ObjDrawInfo         draws[MAX_OBJECTS];
    int                 draw_count;

    /* ground plane (static, uploaded once) */
    VkBuffer            ground_vb;
    VkDeviceMemory      ground_vm;
    VkBuffer            ground_ib;
    VkDeviceMemory      ground_im;
    uint32_t            ground_index_count;

    /* lighting params (for rendered mode) */
    float      light_dir[3];
    float      light_intensity;
    float      light_ambient;
    float      sun_color[3];
    float      shadow_softness;
    float      sun_size;        // angular diameter in degrees (real sun = 0.53)
    bool       shadows_enabled;
    float      exposure;        // camera exposure multiplier (pre-tonemap)

    /* x-ray */
    float      xray_density;    // absorption per surface layer

    /* environment */
    int        bg_mode; // 0=solid, 1=gradient, 2=physical atmosphere
    float      sky_top[3];
    float      sky_bottom[3];
    float      sky_turbidity;   // atmosphere haze (1=clear .. 10=hazy)
    float      cloud_coverage;  // 0 = clear sky
    float      cloud_speed;
    bool       fog_enabled;
    float      fog_color[3];
    float      fog_density;
    float      time;            // seconds, for cloud animation

    /* camera pos for specular */
    float      cam_pos[3];
};

Renderer *renderer_create(const VkCtx &ctx);
void renderer_destroy(Renderer *r, const VkCtx &ctx);

/* Must run each frame BEFORE the main render pass begins: uploads scene
   meshes into the per-frame buffers, updates the scene UBO, and (when
   enabled) renders the shadow map in its own depth-only render pass. */
void renderer_shadow_pass(Renderer *r, const VkCtx &ctx, uint32_t frame_index,
                          VkCommandBuffer cmd, const SceneObject *objects, int object_count);

void renderer_draw(Renderer *r, const VkCtx &ctx, uint32_t frame_index,
                   VkCommandBuffer cmd, const glm::mat4 &view, const glm::mat4 &proj,
                   const SceneObject *objects, int object_count);
void renderer_draw_gizmo(Renderer *r, const VkCtx &ctx, uint32_t frame_index,
                         VkCommandBuffer cmd,
                         const glm::mat4 &view, const glm::mat4 &proj,
                         const SceneObject &obj, ToolMode tool, GizmoAxis highlight);
void renderer_draw_outline(Renderer *r, const VkCtx &ctx, VkCommandBuffer cmd,
                           const glm::mat4 &view, const glm::mat4 &proj,
                           const SceneObject &obj, int obj_index, uint32_t frame_index);
