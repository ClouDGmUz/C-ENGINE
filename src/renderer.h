#pragma once
#include "vk_context.h"
#include <glm/glm.hpp>

enum ShapeType { SHAPE_CUBE = 0, SHAPE_SPHERE, SHAPE_PYRAMID, SHAPE_CYLINDER, SHAPE_COUNT };

struct Renderer {
    VkPipelineLayout    pipeline_layout;
    VkPipeline          pipeline;
    VkPipeline          pipeline_wireframe;
    VkBuffer            vertex_buffer;
    VkDeviceMemory      vertex_memory;
    VkBuffer            index_buffer;
    VkDeviceMemory      index_memory;
    uint32_t            index_count;
    uint32_t            max_vertex_bytes;
    ShapeType           current_shape;
    bool                wireframe;
    int                 sphere_detail;

    /* ground plane */
    VkBuffer            ground_vb;
    VkDeviceMemory      ground_vm;
    VkBuffer            ground_ib;
    VkDeviceMemory      ground_im;
    uint32_t            ground_index_count;

    /* per-frame params (set before draw) */
    float      obj_scale;
    float      obj_yaw;
};

Renderer *renderer_create(const VkCtx &ctx);
void renderer_destroy(Renderer *r, const VkCtx &ctx);
void renderer_draw(Renderer *r, const VkCtx &ctx, uint32_t frame_index,
                   VkCommandBuffer cmd, const glm::mat4 &view, const glm::mat4 &proj);
void renderer_set_shape(Renderer *r, const VkCtx &ctx, ShapeType shape);
void renderer_set_sphere_detail(Renderer *r, const VkCtx &ctx, int detail);
