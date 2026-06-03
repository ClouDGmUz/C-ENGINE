#include "renderer.h"
#include <glm/gtc/type_ptr.hpp>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <vector>

#include "vert_spv.h"
#include "frag_spv.h"

#define MAX_VERTEX_COUNT 1024
#define MAX_INDEX_COUNT  8192
#define PI 3.14159265359f

static VkShaderModule create_shader(const VkCtx &ctx, const uint32_t *code, size_t size) {
    VkShaderModuleCreateInfo ci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    ci.codeSize = size;
    ci.pCode    = code;
    VkShaderModule mod;
    vk_check(vkCreateShaderModule(ctx.device, &ci, nullptr, &mod), "create shader module");
    return mod;
}

template<size_t N>
static VkShaderModule create_shader(const VkCtx &ctx, const uint32_t (&code)[N]) {
    return create_shader(ctx, code, N * sizeof(uint32_t));
}

/* TODO: use device-local buffers with staging for better performance */
static void create_buffer(const VkCtx &ctx, VkDeviceSize size, VkBufferUsageFlags usage,
                          VkBuffer &buffer, VkDeviceMemory &memory) {
    VkBufferCreateInfo ci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    ci.size        = size;
    ci.usage       = usage;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    vk_check(vkCreateBuffer(ctx.device, &ci, nullptr, &buffer), "create buffer");

    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(ctx.device, buffer, &req);
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    ai.allocationSize  = req.size;
    ai.memoryTypeIndex = vk_find_memory_type(ctx, req.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    vk_check(vkAllocateMemory(ctx.device, &ai, nullptr, &memory), "allocate buffer memory");
    vkBindBufferMemory(ctx.device, buffer, memory, 0);
}

struct Vertex { float x, y, z, r, g, b, nx, ny, nz; };

static Vertex vtx(float x, float y, float z, float r, float g, float b) {
    float len = sqrtf(x*x + y*y + z*z);
    if (len < 0.0001f) len = 1.0f;
    return {x, y, z, r, g, b, x/len, y/len, z/len};
}

struct ShapeData {
    std::vector<Vertex>   verts;
    std::vector<uint16_t> indices;
};

static void make_cube(ShapeData &sd) {
    float s = 0.5f;
    float p[8][3] = {{-s,-s,-s},{-s,-s,s},{-s,s,-s},{-s,s,s},
                     { s,-s,-s},{ s,-s,s},{ s,s,-s},{ s,s,s}};
    float c[8][3] = {{0,0,0},{0,0,1},{0,1,0},{0,1,1},
                     {1,0,0},{1,0,1},{1,1,0},{1,1,1}};
    for (int i = 0; i < 8; i++)
        sd.verts.push_back(vtx(p[i][0], p[i][1], p[i][2], c[i][0], c[i][1], c[i][2]));
    uint16_t idxs[] = {
        0,1,2, 1,3,2, 4,6,5, 5,6,7,
        0,4,1, 1,4,5, 2,3,6, 3,7,6,
        0,2,4, 2,6,4, 1,5,3, 3,5,7,
    };
    sd.indices.assign(std::begin(idxs), std::end(idxs));
}

static void make_sphere(ShapeData &sd, int detail, float radius) {
    int sectors = 8 + detail * 6;
    int stacks  = 4 + detail * 4;
    for (int i = 0; i <= stacks; i++) {
        float phi = PI * i / stacks;
        for (int j = 0; j <= sectors; j++) {
            float theta = 2.0f * PI * j / sectors;
            float x = radius * sinf(phi) * cosf(theta);
            float y = radius * cosf(phi);
            float z = radius * sinf(phi) * sinf(theta);
            sd.verts.push_back(vtx(x, y, z,
                (x/radius + 1.0f)*0.5f, (y/radius + 1.0f)*0.5f, (z/radius + 1.0f)*0.5f));
        }
    }
    for (int i = 0; i < stacks; i++) {
        for (int j = 0; j < sectors; j++) {
            uint16_t first = i * (sectors + 1) + j;
            uint16_t second = first + sectors + 1;
            sd.indices.push_back(first);  sd.indices.push_back(second);
            sd.indices.push_back(first+1); sd.indices.push_back(first+1);
            sd.indices.push_back(second); sd.indices.push_back(second+1);
        }
    }
}

static void make_pyramid(ShapeData &sd) {
    sd.verts.push_back(vtx( 0.0f,  0.5f,  0.0f,  1.0f, 1.0f, 0.0f));
    sd.verts.push_back(vtx(-0.5f, -0.5f, -0.5f,  1.0f, 0.0f, 0.0f));
    sd.verts.push_back(vtx( 0.5f, -0.5f, -0.5f,  0.0f, 1.0f, 0.0f));
    sd.verts.push_back(vtx( 0.5f, -0.5f,  0.5f,  0.0f, 0.0f, 1.0f));
    sd.verts.push_back(vtx(-0.5f, -0.5f,  0.5f,  1.0f, 0.0f, 1.0f));
    uint16_t idxs[] = {
        0,2,1, 0,3,2, 0,4,3, 0,1,4,
        1,3,4, 1,2,3,
    };
    sd.indices.assign(std::begin(idxs), std::end(idxs));
}

static void make_cylinder(ShapeData &sd, int sectors, float radius, float half_h) {
    sd.verts.push_back(vtx(0.0f,  half_h, 0.0f,  1.0f, 0.5f, 0.0f));
    sd.verts.push_back(vtx(0.0f, -half_h, 0.0f,  0.0f, 0.5f, 1.0f));
    for (int j = 0; j < sectors; j++) {
        float t = 2.0f * PI * j / sectors;
        sd.verts.push_back(vtx(radius*cosf(t),  half_h, radius*sinf(t), 1.0f, 0.5f, 0.0f));
    }
    for (int j = 0; j < sectors; j++) {
        float t = 2.0f * PI * j / sectors;
        sd.verts.push_back(vtx(radius*cosf(t), -half_h, radius*sinf(t), 0.0f, 0.5f, 1.0f));
    }
    for (int j = 0; j < sectors; j++) {
        uint16_t n = (j + 1) % sectors;
        uint16_t t0 = 2 + j, t1 = 2 + n;
        uint16_t b0 = 2 + sectors + j, b1 = 2 + sectors + n;
        sd.indices.push_back(0); sd.indices.push_back(t1); sd.indices.push_back(t0);
        sd.indices.push_back(t0); sd.indices.push_back(b0); sd.indices.push_back(t1);
        sd.indices.push_back(t1); sd.indices.push_back(b0); sd.indices.push_back(b1);
        sd.indices.push_back(1); sd.indices.push_back(b0); sd.indices.push_back(b1);
    }
}

static void make_ground(ShapeData &sd) {
    const int N = 8;
    const float size = 6.0f;
    const float y = -1.0f;
    for (int i = 0; i <= N; i++) {
        for (int j = 0; j <= N; j++) {
            float x = (i / (float)N - 0.5f) * size;
            float z = (j / (float)N - 0.5f) * size;
            bool white = (i + j) % 2 == 0;
            float c = white ? 0.35f : 0.2f;
            sd.verts.push_back(vtx(x, y, z, c, c, c));
        }
    }
    for (int i = 0; i < N; i++) {
        for (int j = 0; j < N; j++) {
            uint16_t a = i * (N+1) + j;
            uint16_t b = a + (N+1);
            sd.indices.push_back(a); sd.indices.push_back(b); sd.indices.push_back(a+1);
            sd.indices.push_back(a+1); sd.indices.push_back(b); sd.indices.push_back(b+1);
        }
    }
}

static ShapeData make_shape(ShapeType type, int sphere_detail) {
    ShapeData sd;
    switch (type) {
    case SHAPE_CUBE:     make_cube(sd);                              break;
    case SHAPE_SPHERE:   make_sphere(sd, sphere_detail, 0.5f);        break;
    case SHAPE_PYRAMID:  make_pyramid(sd);                            break;
    case SHAPE_CYLINDER: make_cylinder(sd, 20, 0.5f, 0.5f);          break;
    default:             make_cube(sd);                              break;
    }
    return sd;
}

Renderer *renderer_create(const VkCtx &ctx) {
    auto *r = new Renderer{};
    r->current_shape = SHAPE_CUBE;
    r->sphere_detail = 2;
    r->wireframe     = false;
    r->obj_scale     = 1.0f;
    r->obj_yaw       = 0.0f;

    ShapeData initial = make_shape(SHAPE_CUBE, r->sphere_detail);
    r->max_vertex_bytes = MAX_VERTEX_COUNT * (uint32_t)sizeof(Vertex);

    /* vertex buffer */
    VkDeviceSize vb_size = r->max_vertex_bytes;
    create_buffer(ctx, vb_size, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, r->vertex_buffer, r->vertex_memory);
    void *data;
    vkMapMemory(ctx.device, r->vertex_memory, 0, vb_size, 0, &data);
    memcpy(data, initial.verts.data(), initial.verts.size() * sizeof(Vertex));
    vkUnmapMemory(ctx.device, r->vertex_memory);

    /* index buffer */
    VkDeviceSize ib_size = MAX_INDEX_COUNT * sizeof(uint16_t);
    create_buffer(ctx, ib_size, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, r->index_buffer, r->index_memory);
    vkMapMemory(ctx.device, r->index_memory, 0, ib_size, 0, &data);
    memcpy(data, initial.indices.data(), initial.indices.size() * sizeof(uint16_t));
    vkUnmapMemory(ctx.device, r->index_memory);
    r->index_count = (uint32_t)initial.indices.size();

    /* ground plane */
    ShapeData ground;
    make_ground(ground);
    VkDeviceSize gv_size = ground.verts.size() * sizeof(Vertex);
    create_buffer(ctx, gv_size, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, r->ground_vb, r->ground_vm);
    vkMapMemory(ctx.device, r->ground_vm, 0, gv_size, 0, &data);
    memcpy(data, ground.verts.data(), gv_size);
    vkUnmapMemory(ctx.device, r->ground_vm);

    VkDeviceSize gi_size = ground.indices.size() * sizeof(uint16_t);
    create_buffer(ctx, gi_size, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, r->ground_ib, r->ground_im);
    vkMapMemory(ctx.device, r->ground_im, 0, gi_size, 0, &data);
    memcpy(data, ground.indices.data(), gi_size);
    vkUnmapMemory(ctx.device, r->ground_im);
    r->ground_index_count = (uint32_t)ground.indices.size();

    /* pipeline layout with push constant for mvp + model */
    VkPushConstantRange pc_range = { VK_SHADER_STAGE_VERTEX_BIT, 0, 128 };
    VkPipelineLayoutCreateInfo pl_ci = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    pl_ci.pushConstantRangeCount = 1;
    pl_ci.pPushConstantRanges    = &pc_range;
    vk_check(vkCreatePipelineLayout(ctx.device, &pl_ci, nullptr, &r->pipeline_layout), "pipeline layout");

    /* shaders */
    VkShaderModule vert_mod = create_shader(ctx, vert_spv);
    VkShaderModule frag_mod = create_shader(ctx, frag_spv);

    VkPipelineShaderStageCreateInfo stages[] = {
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
          VK_SHADER_STAGE_VERTEX_BIT, vert_mod, "main", nullptr },
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
          VK_SHADER_STAGE_FRAGMENT_BIT, frag_mod, "main", nullptr },
    };

    /* vertex input */
    VkVertexInputBindingDescription vib = { 0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX };
    VkVertexInputAttributeDescription via[] = {
        { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, x) },
        { 1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, r) },
        { 2, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, nx) },
    };
    VkPipelineVertexInputStateCreateInfo vis = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    vis.vertexBindingDescriptionCount   = 1;
    vis.pVertexBindingDescriptions      = &vib;
    vis.vertexAttributeDescriptionCount = 3;
    vis.pVertexAttributeDescriptions    = via;

    VkPipelineInputAssemblyStateCreateInfo ias = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    ias.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkViewport vp = { 0, 0, (float)ctx.swapchain_extent.width, (float)ctx.swapchain_extent.height, 0, 1 };
    VkRect2D   sc = { {0, 0}, ctx.swapchain_extent };
    VkPipelineViewportStateCreateInfo vps = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vps.viewportCount = 1;
    vps.pViewports    = &vp;
    vps.scissorCount  = 1;
    vps.pScissors     = &sc;

    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    ds.depthTestEnable       = VK_TRUE;
    ds.depthWriteEnable      = VK_TRUE;
    ds.depthCompareOp        = VK_COMPARE_OP_LESS;

    VkPipelineColorBlendAttachmentState cba = {};
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                         VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cb.attachmentCount = 1;
    cb.pAttachments    = &cba;

    /* rasterization: wireframe needs different cull mode + polygon mode */
    VkPipelineRasterizationStateCreateInfo rs_fill = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs_fill.polygonMode = VK_POLYGON_MODE_FILL;
    rs_fill.cullMode    = VK_CULL_MODE_BACK_BIT;
    rs_fill.frontFace   = VK_FRONT_FACE_CLOCKWISE;
    rs_fill.lineWidth   = 1.0f;

    VkPipelineRasterizationStateCreateInfo rs_wire = rs_fill;
    rs_wire.polygonMode = VK_POLYGON_MODE_LINE;
    rs_wire.cullMode    = VK_CULL_MODE_NONE;

    /* dynamic viewport/scissor state */
    VkDynamicState dyn_states[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dyn = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates    = dyn_states;

    VkGraphicsPipelineCreateInfo pci = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    pci.stageCount          = 2;
    pci.pStages             = stages;
    pci.pVertexInputState   = &vis;
    pci.pInputAssemblyState = &ias;
    pci.pViewportState      = &vps;
    pci.pRasterizationState = &rs_fill;
    pci.pMultisampleState   = &ms;
    pci.pDepthStencilState  = &ds;
    pci.pColorBlendState    = &cb;
    pci.pDynamicState       = &dyn;
    pci.layout              = r->pipeline_layout;
    pci.renderPass          = ctx.render_pass;
    pci.subpass             = 0;

    vk_check(vkCreateGraphicsPipelines(ctx.device, VK_NULL_HANDLE, 1, &pci, nullptr, &r->pipeline), "create pipeline");

    pci.pRasterizationState = &rs_wire;
    vk_check(vkCreateGraphicsPipelines(ctx.device, VK_NULL_HANDLE, 1, &pci, nullptr, &r->pipeline_wireframe), "create wire pipeline");

    vkDestroyShaderModule(ctx.device, frag_mod, nullptr);
    vkDestroyShaderModule(ctx.device, vert_mod, nullptr);

    return r;
}

void renderer_destroy(Renderer *r, const VkCtx &ctx) {
    vkDestroyPipeline(ctx.device, r->pipeline, nullptr);
    vkDestroyPipeline(ctx.device, r->pipeline_wireframe, nullptr);
    vkDestroyPipelineLayout(ctx.device, r->pipeline_layout, nullptr);

    vkDestroyBuffer(ctx.device, r->vertex_buffer, nullptr);
    vkFreeMemory(ctx.device, r->vertex_memory, nullptr);
    vkDestroyBuffer(ctx.device, r->index_buffer, nullptr);
    vkFreeMemory(ctx.device, r->index_memory, nullptr);

    vkDestroyBuffer(ctx.device, r->ground_vb, nullptr);
    vkFreeMemory(ctx.device, r->ground_vm, nullptr);
    vkDestroyBuffer(ctx.device, r->ground_ib, nullptr);
    vkFreeMemory(ctx.device, r->ground_im, nullptr);

    delete r;
}

void renderer_draw(Renderer *r, const VkCtx &ctx, uint32_t frame_index,
                   VkCommandBuffer cmd, const glm::mat4 &view, const glm::mat4 &proj) {
    VkViewport vp = { 0, 0, (float)ctx.swapchain_extent.width, (float)ctx.swapchain_extent.height, 0, 1 };
    VkRect2D   sc = { {0, 0}, ctx.swapchain_extent };
    VkDeviceSize off = 0;

    glm::mat4 model = glm::rotate(glm::mat4(1.0f), r->obj_yaw, glm::vec3(0, 1, 0));
    model = glm::scale(model, glm::vec3(r->obj_scale));
    glm::mat4 mvp = proj * view * model;

    /* --- draw shape --- */
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
        r->wireframe ? r->pipeline_wireframe : r->pipeline);
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);
    vkCmdPushConstants(cmd, r->pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 64, glm::value_ptr(mvp));
    vkCmdPushConstants(cmd, r->pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT, 64, 64, glm::value_ptr(model));
    vkCmdBindVertexBuffers(cmd, 0, 1, &r->vertex_buffer, &off);
    vkCmdBindIndexBuffer(cmd, r->index_buffer, 0, VK_INDEX_TYPE_UINT16);
    vkCmdDrawIndexed(cmd, r->index_count, 1, 0, 0, 0);

    /* --- draw ground --- */
    glm::mat4 ground_model = glm::mat4(1.0f);
    glm::mat4 ground_mvp = proj * view * ground_model;
    if (r->wireframe) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->pipeline);
    }
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);
    vkCmdPushConstants(cmd, r->pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 64, glm::value_ptr(ground_mvp));
    vkCmdPushConstants(cmd, r->pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT, 64, 64, glm::value_ptr(ground_model));
    vkCmdBindVertexBuffers(cmd, 0, 1, &r->ground_vb, &off);
    vkCmdBindIndexBuffer(cmd, r->ground_ib, 0, VK_INDEX_TYPE_UINT16);
    vkCmdDrawIndexed(cmd, r->ground_index_count, 1, 0, 0, 0);
}

void renderer_set_shape(Renderer *r, const VkCtx &ctx, ShapeType shape) {
    if (shape == r->current_shape) return;
    ShapeData sd = make_shape(shape, r->sphere_detail);

    void *data;
    vkMapMemory(ctx.device, r->vertex_memory, 0, r->max_vertex_bytes, 0, &data);
    memcpy(data, sd.verts.data(), sd.verts.size() * sizeof(Vertex));
    vkUnmapMemory(ctx.device, r->vertex_memory);

    vkMapMemory(ctx.device, r->index_memory, 0, MAX_INDEX_COUNT * sizeof(uint16_t), 0, &data);
    memcpy(data, sd.indices.data(), sd.indices.size() * sizeof(uint16_t));
    vkUnmapMemory(ctx.device, r->index_memory);

    r->index_count = (uint32_t)sd.indices.size();
    r->current_shape = shape;
}

void renderer_set_sphere_detail(Renderer *r, const VkCtx &ctx, int detail) {
    if (detail == r->sphere_detail) return;
    r->sphere_detail = detail;
    if (r->current_shape != SHAPE_SPHERE) return;
    ShapeData sd = make_shape(SHAPE_SPHERE, detail);

    void *data;
    vkMapMemory(ctx.device, r->vertex_memory, 0, r->max_vertex_bytes, 0, &data);
    memcpy(data, sd.verts.data(), sd.verts.size() * sizeof(Vertex));
    vkUnmapMemory(ctx.device, r->vertex_memory);

    vkMapMemory(ctx.device, r->index_memory, 0, MAX_INDEX_COUNT * sizeof(uint16_t), 0, &data);
    memcpy(data, sd.indices.data(), sd.indices.size() * sizeof(uint16_t));
    vkUnmapMemory(ctx.device, r->index_memory);

    r->index_count = (uint32_t)sd.indices.size();
}
