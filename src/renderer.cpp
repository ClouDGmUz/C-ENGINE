#include "renderer.h"
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <cstdio>
#include <cstring>
#include <cstddef>
#include <cmath>
#include <vector>

#include "vert_spv.h"
#include "frag_spv.h"

#define MAX_VERTEX_COUNT 1024
#define MAX_INDEX_COUNT  8192
#define PI 3.14159265359f
#define SHADOW_DIM 2048

/* must match the Scene UBO block in shader.vert / shader.frag (std140) */
struct SceneUBO {
    float light_space[16];
    float inv_vp[16];     // inverse(proj * view), written in renderer_draw
    float sky_top[4];     // rgb, w = cloud coverage
    float sky_bottom[4];  // rgb, w = cloud speed
    float fog[4];         // rgb, w = density
    float misc[4];        // x = time, y = shadows on, z = fog on, w = bg_mode (0/1/2)
    float sun[4];         // xyz = direction, w = intensity
    float sun_color[4];   // rgb, w = angular diameter in degrees
    float cam[4];         // xyz = position, w = exposure
    float params[4];      // x = ambient, y = shadow softness, z = xray density, w = turbidity
};

/* per-draw fragment push constants (offset 128), must match shader.frag PC */
struct FragPC {
    int32_t mode;       // render_mode + tex_mode * 16
    float   albedo[3];
    float   roughness;
    float   metallic;
};

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

struct Vertex { float x, y, z, r, g, b, nx, ny, nz, u, v; };

static Vertex vtx(float x, float y, float z, float r, float g, float b, float u = 0, float v = 0) {
    float len = sqrtf(x*x + y*y + z*z);
    if (len < 0.0001f) len = 1.0f;
    return {x, y, z, r, g, b, x/len, y/len, z/len, u, v};
}

struct ShapeData {
    std::vector<Vertex>   verts;
    std::vector<uint16_t> indices;
};

static void make_cube(ShapeData &sd) {
    float s = 0.5f;
    // 24 verts (4 per face) with proper UVs and normals
    struct FV { float p[3]; float n[3]; float uv[2]; };
    FV faces[] = {
        // Front (+Z)
        {{-s,-s, s},{0,0,1},{0,0}}, {{ s,-s, s},{0,0,1},{1,0}}, {{ s, s, s},{0,0,1},{1,1}}, {{-s, s, s},{0,0,1},{0,1}},
        // Back (-Z)
        {{ s,-s,-s},{0,0,-1},{0,0}}, {{-s,-s,-s},{0,0,-1},{1,0}}, {{-s, s,-s},{0,0,-1},{1,1}}, {{ s, s,-s},{0,0,-1},{0,1}},
        // Right (+X)
        {{ s,-s, s},{1,0,0},{0,0}}, {{ s,-s,-s},{1,0,0},{1,0}}, {{ s, s,-s},{1,0,0},{1,1}}, {{ s, s, s},{1,0,0},{0,1}},
        // Left (-X)
        {{-s,-s,-s},{-1,0,0},{0,0}}, {{-s,-s, s},{-1,0,0},{1,0}}, {{-s, s, s},{-1,0,0},{1,1}}, {{-s, s,-s},{-1,0,0},{0,1}},
        // Top (+Y)
        {{-s, s, s},{0,1,0},{0,0}}, {{ s, s, s},{0,1,0},{1,0}}, {{ s, s,-s},{0,1,0},{1,1}}, {{-s, s,-s},{0,1,0},{0,1}},
        // Bottom (-Y)
        {{-s,-s,-s},{0,-1,0},{0,0}}, {{ s,-s,-s},{0,-1,0},{1,0}}, {{ s,-s, s},{0,-1,0},{1,1}}, {{-s,-s, s},{0,-1,0},{0,1}},
    };
    for (int i = 0; i < 24; i++) {
        float c[3] = {(faces[i].p[0]+s), (faces[i].p[1]+s), (faces[i].p[2]+s)};
        Vertex v; v.x=faces[i].p[0]; v.y=faces[i].p[1]; v.z=faces[i].p[2];
        v.r=c[0]; v.g=c[1]; v.b=c[2];
        v.nx=faces[i].n[0]; v.ny=faces[i].n[1]; v.nz=faces[i].n[2];
        v.u=faces[i].uv[0]; v.v=faces[i].uv[1];
        sd.verts.push_back(v);
    }
    for (int f = 0; f < 6; f++) {
        uint16_t base = f * 4;
        sd.indices.push_back(base); sd.indices.push_back(base+1); sd.indices.push_back(base+2);
        sd.indices.push_back(base); sd.indices.push_back(base+2); sd.indices.push_back(base+3);
    }
}

static void make_sphere(ShapeData &sd, int detail, float radius) {
    int sectors = 8 + detail * 6;
    int stacks  = 4 + detail * 4;
    for (int i = 0; i <= stacks; i++) {
        float phi = PI * i / stacks;
        float v = (float)i / stacks;
        for (int j = 0; j <= sectors; j++) {
            float theta = 2.0f * PI * j / sectors;
            float u = (float)j / sectors;
            float x = radius * sinf(phi) * cosf(theta);
            float y = radius * cosf(phi);
            float z = radius * sinf(phi) * sinf(theta);
            Vertex vt;
            vt.x=x; vt.y=y; vt.z=z;
            vt.r=(x/radius+1.0f)*0.5f; vt.g=(y/radius+1.0f)*0.5f; vt.b=(z/radius+1.0f)*0.5f;
            vt.nx=x/radius; vt.ny=y/radius; vt.nz=z/radius;
            vt.u=u; vt.v=v;
            sd.verts.push_back(vt);
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
    sd.verts.push_back(vtx( 0.0f,  0.5f,  0.0f,  1.0f, 1.0f, 0.0f, 0.5f, 1.0f));
    sd.verts.push_back(vtx(-0.5f, -0.5f, -0.5f,  1.0f, 0.0f, 0.0f, 0.0f, 0.0f));
    sd.verts.push_back(vtx( 0.5f, -0.5f, -0.5f,  0.0f, 1.0f, 0.0f, 1.0f, 0.0f));
    sd.verts.push_back(vtx( 0.5f, -0.5f,  0.5f,  0.0f, 0.0f, 1.0f, 1.0f, 1.0f));
    sd.verts.push_back(vtx(-0.5f, -0.5f,  0.5f,  1.0f, 0.0f, 1.0f, 0.0f, 1.0f));
    uint16_t idxs[] = { 0,2,1, 0,3,2, 0,4,3, 0,1,4, 1,3,4, 1,2,3 };
    sd.indices.assign(std::begin(idxs), std::end(idxs));
}

static void make_cylinder(ShapeData &sd, int sectors, float radius, float half_h) {
    sd.verts.push_back(vtx(0.0f,  half_h, 0.0f,  1.0f, 0.5f, 0.0f, 0.5f, 0.5f));
    sd.verts.push_back(vtx(0.0f, -half_h, 0.0f,  0.0f, 0.5f, 1.0f, 0.5f, 0.5f));
    for (int j = 0; j < sectors; j++) {
        float t = 2.0f * PI * j / sectors;
        float u = (float)j / sectors;
        sd.verts.push_back(vtx(radius*cosf(t),  half_h, radius*sinf(t), 1.0f, 0.5f, 0.0f, u, 0.0f));
    }
    for (int j = 0; j < sectors; j++) {
        float t = 2.0f * PI * j / sectors;
        float u = (float)j / sectors;
        sd.verts.push_back(vtx(radius*cosf(t), -half_h, radius*sinf(t), 0.0f, 0.5f, 1.0f, u, 1.0f));
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
    // Large flat quad — grid lines are drawn procedurally in the fragment shader
    const float size = 50.0f;
    const float y = -1.0f;
    sd.verts.push_back({-size, y, -size, 0,0,0, 0,1,0});
    sd.verts.push_back({ size, y, -size, 0,0,0, 0,1,0});
    sd.verts.push_back({ size, y,  size, 0,0,0, 0,1,0});
    sd.verts.push_back({-size, y,  size, 0,0,0, 0,1,0});
    /* wound so the +Y face is front — the old order faced down and the
       whole grid was silently back-face culled */
    sd.indices = {0, 2, 1, 0, 3, 2};
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
    r->sphere_detail = 2;
    r->render_mode   = 2;
    r->exposure      = 1.0f;
    r->xray_density  = 1.0f;
    r->sky_turbidity = 2.5f;

    r->max_vertex_bytes = MAX_OBJECTS * MAX_VERTEX_COUNT * (uint32_t)sizeof(Vertex);

    /* per-frame dynamic buffers, persistently mapped */
    VkDeviceSize vb_size = r->max_vertex_bytes;
    VkDeviceSize ib_size = (VkDeviceSize)MAX_OBJECTS * MAX_INDEX_COUNT * sizeof(uint16_t);
    for (int f = 0; f < MAX_FRAMES_IN_FLIGHT; f++) {
        create_buffer(ctx, vb_size, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, r->vertex_buffer[f], r->vertex_memory[f]);
        vkMapMemory(ctx.device, r->vertex_memory[f], 0, vb_size, 0, &r->vertex_mapped[f]);

        create_buffer(ctx, ib_size, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, r->index_buffer[f], r->index_memory[f]);
        vkMapMemory(ctx.device, r->index_memory[f], 0, ib_size, 0, &r->index_mapped[f]);

        create_buffer(ctx, 3 * sizeof(Vertex), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, r->sky_vb[f], r->sky_vm[f]);
        vkMapMemory(ctx.device, r->sky_vm[f], 0, 3 * sizeof(Vertex), 0, &r->sky_mapped[f]);

        create_buffer(ctx, 27 * sizeof(Vertex), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, r->gizmo_vb[f], r->gizmo_vm[f]);
        vkMapMemory(ctx.device, r->gizmo_vm[f], 0, 27 * sizeof(Vertex), 0, &r->gizmo_mapped[f]);

        create_buffer(ctx, sizeof(SceneUBO), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, r->ubo_buffer[f], r->ubo_memory[f]);
        vkMapMemory(ctx.device, r->ubo_memory[f], 0, sizeof(SceneUBO), 0, &r->ubo_mapped[f]);
    }

    /* --- shadow map resources --- */
    vk_create_image(ctx, SHADOW_DIM, SHADOW_DIM, VK_FORMAT_D32_SFLOAT,
        VK_IMAGE_TILING_OPTIMAL,
        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        r->shadow_image, r->shadow_memory);

    {
        VkImageViewCreateInfo vci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vci.image                       = r->shadow_image;
        vci.viewType                    = VK_IMAGE_VIEW_TYPE_2D;
        vci.format                      = VK_FORMAT_D32_SFLOAT;
        vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        vci.subresourceRange.levelCount = 1;
        vci.subresourceRange.layerCount = 1;
        vk_check(vkCreateImageView(ctx.device, &vci, nullptr, &r->shadow_view), "shadow view");

        VkSamplerCreateInfo sci = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        sci.magFilter     = VK_FILTER_LINEAR;
        sci.minFilter     = VK_FILTER_LINEAR;
        sci.addressModeU  = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        sci.addressModeV  = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        sci.addressModeW  = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        sci.borderColor   = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE; // outside map = lit
        sci.compareEnable = VK_TRUE;                            // hardware PCF
        sci.compareOp     = VK_COMPARE_OP_LESS_OR_EQUAL;
        vk_check(vkCreateSampler(ctx.device, &sci, nullptr, &r->shadow_sampler), "shadow sampler");
    }

    /* shadow render pass: depth-only, ends ready for sampling */
    {
        VkAttachmentDescription att = {};
        att.format        = VK_FORMAT_D32_SFLOAT;
        att.samples       = VK_SAMPLE_COUNT_1_BIT;
        att.loadOp        = VK_ATTACHMENT_LOAD_OP_CLEAR;
        att.storeOp       = VK_ATTACHMENT_STORE_OP_STORE;
        att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        att.finalLayout   = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkAttachmentReference depth_ref = { 0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
        VkSubpassDescription subpass = {};
        subpass.pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.pDepthStencilAttachment = &depth_ref;

        VkSubpassDependency deps[2] = {};
        deps[0].srcSubpass    = VK_SUBPASS_EXTERNAL;            // prior frame's reads finish
        deps[0].dstSubpass    = 0;
        deps[0].srcStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        deps[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        deps[0].dstStageMask  = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        deps[0].dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        deps[1].srcSubpass    = 0;                              // writes finish before main pass samples
        deps[1].dstSubpass    = VK_SUBPASS_EXTERNAL;
        deps[1].srcStageMask  = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        deps[1].srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        deps[1].dstStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

        VkRenderPassCreateInfo rpci = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
        rpci.attachmentCount = 1;
        rpci.pAttachments    = &att;
        rpci.subpassCount    = 1;
        rpci.pSubpasses      = &subpass;
        rpci.dependencyCount = 2;
        rpci.pDependencies   = deps;
        vk_check(vkCreateRenderPass(ctx.device, &rpci, nullptr, &r->shadow_render_pass), "shadow render pass");

        VkFramebufferCreateInfo fci = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
        fci.renderPass      = r->shadow_render_pass;
        fci.attachmentCount = 1;
        fci.pAttachments    = &r->shadow_view;
        fci.width           = SHADOW_DIM;
        fci.height          = SHADOW_DIM;
        fci.layers          = 1;
        vk_check(vkCreateFramebuffer(ctx.device, &fci, nullptr, &r->shadow_framebuffer), "shadow framebuffer");
    }

    /* --- descriptor set: scene UBO + shadow sampler --- */
    {
        VkDescriptorSetLayoutBinding bindings[2] = {};
        bindings[0].binding         = 0;
        bindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        bindings[0].descriptorCount = 1;
        bindings[0].stageFlags      = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        bindings[1].binding         = 1;
        bindings[1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[1].descriptorCount = 1;
        bindings[1].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

        VkDescriptorSetLayoutCreateInfo lci = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        lci.bindingCount = 2;
        lci.pBindings    = bindings;
        vk_check(vkCreateDescriptorSetLayout(ctx.device, &lci, nullptr, &r->desc_layout), "descriptor layout");

        VkDescriptorPoolSize sizes[2] = {
            { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         MAX_FRAMES_IN_FLIGHT },
            { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, MAX_FRAMES_IN_FLIGHT },
        };
        VkDescriptorPoolCreateInfo dpci = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        dpci.maxSets       = MAX_FRAMES_IN_FLIGHT;
        dpci.poolSizeCount = 2;
        dpci.pPoolSizes    = sizes;
        vk_check(vkCreateDescriptorPool(ctx.device, &dpci, nullptr, &r->desc_pool), "descriptor pool");

        VkDescriptorSetLayout layouts[MAX_FRAMES_IN_FLIGHT];
        for (int f = 0; f < MAX_FRAMES_IN_FLIGHT; f++) layouts[f] = r->desc_layout;
        VkDescriptorSetAllocateInfo dai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        dai.descriptorPool     = r->desc_pool;
        dai.descriptorSetCount = MAX_FRAMES_IN_FLIGHT;
        dai.pSetLayouts        = layouts;
        vk_check(vkAllocateDescriptorSets(ctx.device, &dai, r->desc_sets), "allocate descriptor sets");

        for (int f = 0; f < MAX_FRAMES_IN_FLIGHT; f++) {
            VkDescriptorBufferInfo bi = { r->ubo_buffer[f], 0, sizeof(SceneUBO) };
            VkDescriptorImageInfo  ii = { r->shadow_sampler, r->shadow_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            VkWriteDescriptorSet writes[2] = {};
            writes[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[0].dstSet          = r->desc_sets[f];
            writes[0].dstBinding      = 0;
            writes[0].descriptorCount = 1;
            writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            writes[0].pBufferInfo     = &bi;
            writes[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[1].dstSet          = r->desc_sets[f];
            writes[1].dstBinding      = 1;
            writes[1].descriptorCount = 1;
            writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[1].pImageInfo      = &ii;
            vkUpdateDescriptorSets(ctx.device, 2, writes, 0, nullptr);
        }
    }

    /* ground plane */
    void *data;
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

    /* pipeline layout with push constants: vertex (mvp+model) + fragment (lighting) */
    VkPushConstantRange pc_ranges[] = {
        { VK_SHADER_STAGE_VERTEX_BIT,   0,   128 },
        { VK_SHADER_STAGE_FRAGMENT_BIT, 128, 64  },
    };
    VkPipelineLayoutCreateInfo pl_ci = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    pl_ci.setLayoutCount         = 1;
    pl_ci.pSetLayouts            = &r->desc_layout;
    pl_ci.pushConstantRangeCount = 2;
    pl_ci.pPushConstantRanges    = pc_ranges;
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
        { 3, 0, VK_FORMAT_R32G32_SFLOAT,    offsetof(Vertex, u) },
    };
    VkPipelineVertexInputStateCreateInfo vis = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    vis.vertexBindingDescriptionCount   = 1;
    vis.pVertexBindingDescriptions      = &vib;
    vis.vertexAttributeDescriptionCount = 4;
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
    rs_fill.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
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

    /* x-ray pipeline: additive blend so each surface layer accumulates like
       a radiograph; no culling (back faces add thickness), no depth test or
       write (radiation passes through everything) */
    VkPipelineColorBlendAttachmentState cba_add = cba;
    cba_add.blendEnable         = VK_TRUE;
    cba_add.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    cba_add.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
    cba_add.colorBlendOp        = VK_BLEND_OP_ADD;
    cba_add.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    cba_add.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    cba_add.alphaBlendOp        = VK_BLEND_OP_ADD;
    VkPipelineColorBlendStateCreateInfo cb_add = cb;
    cb_add.pAttachments = &cba_add;

    VkPipelineRasterizationStateCreateInfo rs_xray = rs_fill;
    rs_xray.cullMode = VK_CULL_MODE_NONE;

    VkPipelineDepthStencilStateCreateInfo ds_xray = ds;
    ds_xray.depthTestEnable  = VK_FALSE;
    ds_xray.depthWriteEnable = VK_FALSE;

    pci.pRasterizationState = &rs_xray;
    pci.pDepthStencilState  = &ds_xray;
    pci.pColorBlendState    = &cb_add;
    vk_check(vkCreateGraphicsPipelines(ctx.device, VK_NULL_HANDLE, 1, &pci, nullptr, &r->pipeline_xray), "create xray pipeline");

    /* overlay pipeline (gizmo): always on top, double-sided, opaque */
    VkPipelineDepthStencilStateCreateInfo ds_overlay = ds;
    ds_overlay.depthTestEnable  = VK_FALSE;
    ds_overlay.depthWriteEnable = VK_FALSE;

    pci.pRasterizationState = &rs_xray; // same: fill, no cull
    pci.pDepthStencilState  = &ds_overlay;
    pci.pColorBlendState    = &cb;
    vk_check(vkCreateGraphicsPipelines(ctx.device, VK_NULL_HANDLE, 1, &pci, nullptr, &r->pipeline_overlay), "create overlay pipeline");

    /* restore opaque state for the shadow pipeline below */
    pci.pDepthStencilState = &ds;
    pci.pColorBlendState   = &cb;

    /* shadow pipeline: vertex stage only, depth bias, no color attachment */
    VkPipelineRasterizationStateCreateInfo rs_shadow = rs_fill;
    rs_shadow.depthBiasEnable         = VK_TRUE;
    rs_shadow.depthBiasConstantFactor = 1.25f;
    rs_shadow.depthBiasSlopeFactor    = 1.75f;

    VkPipelineColorBlendStateCreateInfo cb_none = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };

    pci.stageCount          = 1; // vertex only — depth comes from rasterization
    pci.pRasterizationState = &rs_shadow;
    pci.pColorBlendState    = &cb_none;
    pci.renderPass          = r->shadow_render_pass;
    vk_check(vkCreateGraphicsPipelines(ctx.device, VK_NULL_HANDLE, 1, &pci, nullptr, &r->shadow_pipeline), "create shadow pipeline");

    vkDestroyShaderModule(ctx.device, frag_mod, nullptr);
    vkDestroyShaderModule(ctx.device, vert_mod, nullptr);

    return r;
}

void renderer_destroy(Renderer *r, const VkCtx &ctx) {
    vkDestroyPipeline(ctx.device, r->pipeline, nullptr);
    vkDestroyPipeline(ctx.device, r->pipeline_wireframe, nullptr);
    vkDestroyPipeline(ctx.device, r->pipeline_xray, nullptr);
    vkDestroyPipeline(ctx.device, r->pipeline_overlay, nullptr);
    vkDestroyPipeline(ctx.device, r->shadow_pipeline, nullptr);
    vkDestroyPipelineLayout(ctx.device, r->pipeline_layout, nullptr);

    vkDestroyDescriptorPool(ctx.device, r->desc_pool, nullptr);
    vkDestroyDescriptorSetLayout(ctx.device, r->desc_layout, nullptr);

    vkDestroyFramebuffer(ctx.device, r->shadow_framebuffer, nullptr);
    vkDestroyRenderPass(ctx.device, r->shadow_render_pass, nullptr);
    vkDestroySampler(ctx.device, r->shadow_sampler, nullptr);
    vkDestroyImageView(ctx.device, r->shadow_view, nullptr);
    vkDestroyImage(ctx.device, r->shadow_image, nullptr);
    vkFreeMemory(ctx.device, r->shadow_memory, nullptr);

    for (int f = 0; f < MAX_FRAMES_IN_FLIGHT; f++) {
        vkDestroyBuffer(ctx.device, r->vertex_buffer[f], nullptr);
        vkFreeMemory(ctx.device, r->vertex_memory[f], nullptr);
        vkDestroyBuffer(ctx.device, r->index_buffer[f], nullptr);
        vkFreeMemory(ctx.device, r->index_memory[f], nullptr);
        vkDestroyBuffer(ctx.device, r->sky_vb[f], nullptr);
        vkFreeMemory(ctx.device, r->sky_vm[f], nullptr);
        vkDestroyBuffer(ctx.device, r->gizmo_vb[f], nullptr);
        vkFreeMemory(ctx.device, r->gizmo_vm[f], nullptr);
        vkDestroyBuffer(ctx.device, r->ubo_buffer[f], nullptr);
        vkFreeMemory(ctx.device, r->ubo_memory[f], nullptr);
    }

    vkDestroyBuffer(ctx.device, r->ground_vb, nullptr);
    vkFreeMemory(ctx.device, r->ground_vm, nullptr);
    vkDestroyBuffer(ctx.device, r->ground_ib, nullptr);
    vkFreeMemory(ctx.device, r->ground_im, nullptr);

    delete r;
}

void renderer_shadow_pass(Renderer *r, const VkCtx &ctx, uint32_t frame_index,
                          VkCommandBuffer cmd, const SceneObject *objects, int object_count) {
    /* --- light-space matrix + scene UBO update --- */
    glm::vec3 L(r->light_dir[0], r->light_dir[1], r->light_dir[2]);
    if (glm::dot(L, L) < 1e-6f) L = glm::vec3(0, 1, 0);
    L = glm::normalize(L);
    glm::vec3 up = fabsf(L.y) > 0.95f ? glm::vec3(1, 0, 0) : glm::vec3(0, 1, 0);
    glm::mat4 lview = glm::lookAt(L * 18.0f, glm::vec3(0.0f), up);
    glm::mat4 lproj = glm::ortho(-14.0f, 14.0f, -14.0f, 14.0f, 0.5f, 40.0f);
    lproj[1][1] *= -1;
    glm::mat4 light_space = lproj * lview;

    SceneUBO ubo{};
    memcpy(ubo.light_space, glm::value_ptr(light_space), 16 * sizeof(float));
    /* inv_vp is filled by renderer_draw once view/proj are known */
    ubo.sky_top[0] = r->sky_top[0]; ubo.sky_top[1] = r->sky_top[1]; ubo.sky_top[2] = r->sky_top[2];
    ubo.sky_top[3] = r->cloud_coverage;
    ubo.sky_bottom[0] = r->sky_bottom[0]; ubo.sky_bottom[1] = r->sky_bottom[1]; ubo.sky_bottom[2] = r->sky_bottom[2];
    ubo.sky_bottom[3] = r->cloud_speed;
    ubo.fog[0] = r->fog_color[0]; ubo.fog[1] = r->fog_color[1]; ubo.fog[2] = r->fog_color[2];
    ubo.fog[3] = r->fog_density;
    ubo.misc[0] = r->time;
    ubo.misc[1] = (r->shadows_enabled && r->render_mode == 2) ? 1.0f : 0.0f;
    ubo.misc[2] = r->fog_enabled ? 1.0f : 0.0f;
    ubo.misc[3] = (float)r->bg_mode;
    ubo.sun[0] = r->light_dir[0]; ubo.sun[1] = r->light_dir[1]; ubo.sun[2] = r->light_dir[2];
    ubo.sun[3] = r->light_intensity;
    ubo.sun_color[0] = r->sun_color[0]; ubo.sun_color[1] = r->sun_color[1]; ubo.sun_color[2] = r->sun_color[2];
    ubo.sun_color[3] = r->sun_size;
    ubo.cam[0] = r->cam_pos[0]; ubo.cam[1] = r->cam_pos[1]; ubo.cam[2] = r->cam_pos[2];
    ubo.cam[3] = r->exposure;
    ubo.params[0] = r->light_ambient;
    ubo.params[1] = r->shadow_softness;
    ubo.params[2] = r->xray_density;
    ubo.params[3] = r->sky_turbidity;
    memcpy(r->ubo_mapped[frame_index], &ubo, sizeof(ubo));

    /* --- upload scene meshes into this frame's buffers --- */
    r->draw_count = 0;
    if (object_count > 0) {
        int count = object_count < MAX_OBJECTS ? object_count : MAX_OBJECTS;
        r->draw_count = count;

        Vertex   *vdata = (Vertex *)r->vertex_mapped[frame_index];
        uint16_t *idata = (uint16_t *)r->index_mapped[frame_index];

        uint32_t vtx_cursor = 0, idx_cursor = 0;
        for (int i = 0; i < count; i++) {
            ShapeData sd = make_shape(objects[i].shape, r->sphere_detail);
            memcpy(vdata + vtx_cursor, sd.verts.data(), sd.verts.size() * sizeof(Vertex));
            memcpy(idata + idx_cursor, sd.indices.data(), sd.indices.size() * sizeof(uint16_t));
            r->draws[i].idx_count  = (uint32_t)sd.indices.size();
            r->draws[i].idx_offset = idx_cursor;
            r->draws[i].vtx_offset = (int32_t)vtx_cursor;
            vtx_cursor += (uint32_t)sd.verts.size();
            idx_cursor += (uint32_t)sd.indices.size();
        }
    }

    /* --- depth-only pass into the shadow map ---
       Always run it (even empty) so the image lands in SHADER_READ_ONLY
       layout for the main pass sampler. */
    VkClearValue clear;
    clear.depthStencil = { 1.0f, 0 };
    VkRenderPassBeginInfo rpi = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    rpi.renderPass      = r->shadow_render_pass;
    rpi.framebuffer     = r->shadow_framebuffer;
    rpi.renderArea      = VkRect2D{ {0, 0}, {SHADOW_DIM, SHADOW_DIM} };
    rpi.clearValueCount = 1;
    rpi.pClearValues    = &clear;
    vkCmdBeginRenderPass(cmd, &rpi, VK_SUBPASS_CONTENTS_INLINE);

    if (r->shadows_enabled && r->render_mode == 2 && r->draw_count > 0) {
        VkViewport vp = { 0, 0, (float)SHADOW_DIM, (float)SHADOW_DIM, 0, 1 };
        VkRect2D   sc = { {0, 0}, {SHADOW_DIM, SHADOW_DIM} };
        VkDeviceSize off = 0;
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->shadow_pipeline);
        vkCmdSetViewport(cmd, 0, 1, &vp);
        vkCmdSetScissor(cmd, 0, 1, &sc);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->pipeline_layout,
                                0, 1, &r->desc_sets[frame_index], 0, nullptr);
        vkCmdBindVertexBuffers(cmd, 0, 1, &r->vertex_buffer[frame_index], &off);
        vkCmdBindIndexBuffer(cmd, r->index_buffer[frame_index], 0, VK_INDEX_TYPE_UINT16);

        for (int i = 0; i < r->draw_count; i++) {
            const SceneObject &obj = objects[i];
            glm::mat4 model = glm::translate(glm::mat4(1.0f), glm::vec3(obj.pos[0], obj.pos[1], obj.pos[2]));
            model = glm::rotate(model, obj.rotation[1], glm::vec3(0, 1, 0));
            model = glm::rotate(model, obj.rotation[0], glm::vec3(1, 0, 0));
            model = glm::rotate(model, obj.rotation[2], glm::vec3(0, 0, 1));
            model = glm::scale(model, glm::vec3(obj.scale));
            glm::mat4 light_mvp = light_space * model;

            vkCmdPushConstants(cmd, r->pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 64, glm::value_ptr(light_mvp));
            vkCmdPushConstants(cmd, r->pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT, 64, 64, glm::value_ptr(model));
            vkCmdDrawIndexed(cmd, r->draws[i].idx_count, 1, r->draws[i].idx_offset, r->draws[i].vtx_offset, 0);
        }
    }

    vkCmdEndRenderPass(cmd);
}

void renderer_draw(Renderer *r, const VkCtx &ctx, uint32_t frame_index,
                   VkCommandBuffer cmd, const glm::mat4 &view, const glm::mat4 &proj,
                   const SceneObject *objects, int object_count) {
    VkViewport vp = { 0, 0, (float)ctx.swapchain_extent.width, (float)ctx.swapchain_extent.height, 0, 1 };
    VkRect2D   sc = { {0, 0}, ctx.swapchain_extent };
    VkDeviceSize off = 0;

    /* inv_vp lands in the UBO here — view/proj are unknown during the
       shadow pass. Buffer is persistently mapped and fence-guarded, and the
       GPU reads only at submit, so writing before vkQueueSubmit is safe. */
    glm::mat4 inv_vp = glm::inverse(proj * view);
    memcpy((char *)r->ubo_mapped[frame_index] + offsetof(SceneUBO, inv_vp),
           glm::value_ptr(inv_vp), 16 * sizeof(float));

    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->pipeline_layout,
                            0, 1, &r->desc_sets[frame_index], 0, nullptr);

    FragPC fpc{};
    fpc.albedo[0] = 1.0f; fpc.albedo[1] = 1.0f; fpc.albedo[2] = 1.0f;
    fpc.roughness = 1.0f;

    /* --- draw sky background (gradient / physical atmosphere) --- */
    if (r->bg_mode > 0) {
        Vertex sky_verts[3];
        float *top = r->sky_top, *bot = r->sky_bottom;

        if (r->bg_mode == 2) {
            // Physical atmosphere: shader rebuilds the world ray per pixel
            // from inv_vp, vertex colors are unused
            sky_verts[0] = {-1.0f, -1.0f, 0.9999f, 0,0,0, 0,1,0};
            sky_verts[1] = {-1.0f,  3.0f, 0.9999f, 0,0,0, 0,1,0};
            sky_verts[2] = { 3.0f, -1.0f, 0.9999f, 0,0,0, 0,1,0};
        } else {
            // Gradient mode: vertex colors are sky top/bottom colors
            sky_verts[0] = {-1.0f, -1.0f, 0.9999f, top[0], top[1], top[2], 0,1,0};
            sky_verts[1] = {-1.0f,  3.0f, 0.9999f, bot[0], bot[1], bot[2], 0,1,0};
            sky_verts[2] = { 3.0f, -1.0f, 0.9999f, top[0], top[1], top[2], 0,1,0};
        }

        memcpy(r->sky_mapped[frame_index], sky_verts, 3 * sizeof(Vertex));

        glm::mat4 ident(1.0f);
        FragPC sky_fpc = fpc;
        sky_fpc.mode = (r->bg_mode == 2) ? 5 : 7; // 5 = physical sky, 7 = flat gradient
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->pipeline);
        vkCmdSetViewport(cmd, 0, 1, &vp);
        vkCmdSetScissor(cmd, 0, 1, &sc);
        vkCmdPushConstants(cmd, r->pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 64, glm::value_ptr(ident));
        vkCmdPushConstants(cmd, r->pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT, 64, 64, glm::value_ptr(ident));
        vkCmdPushConstants(cmd, r->pipeline_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 128, sizeof(FragPC), &sky_fpc);
        vkCmdBindVertexBuffers(cmd, 0, 1, &r->sky_vb[frame_index], &off);
        vkCmdDraw(cmd, 3, 1, 0, 0);
    }

    /* --- draw ground (infinite grid) ---
       Drawn before the objects: the x-ray pipeline writes no depth, so
       anything drawn after it would overpaint the accumulated layers. */
    glm::mat4 ground_model = glm::mat4(1.0f);
    glm::mat4 ground_mvp = proj * view * ground_model;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->pipeline);
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);
    vkCmdPushConstants(cmd, r->pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 64, glm::value_ptr(ground_mvp));
    vkCmdPushConstants(cmd, r->pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT, 64, 64, glm::value_ptr(ground_model));
    FragPC gfpc = fpc;
    gfpc.mode = 3; // infinite grid mode
    vkCmdPushConstants(cmd, r->pipeline_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 128, sizeof(FragPC), &gfpc);
    vkCmdBindVertexBuffers(cmd, 0, 1, &r->ground_vb, &off);
    vkCmdBindIndexBuffer(cmd, r->ground_ib, 0, VK_INDEX_TYPE_UINT16);
    vkCmdDrawIndexed(cmd, r->ground_index_count, 1, 0, 0, 0);

    /* --- draw scene objects --- */
    VkPipeline obj_pipeline = r->pipeline;
    if (r->render_mode == 0) obj_pipeline = r->pipeline_wireframe;
    if (r->render_mode == 3) obj_pipeline = r->pipeline_xray;
    int obj_mode = (r->render_mode == 3) ? 6 : r->render_mode; // 6 = x-ray in shader

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, obj_pipeline);
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);

    /* scene meshes were uploaded by renderer_shadow_pass; reuse r->draws */
    if (r->draw_count > 0) {
        const ObjDrawInfo *draws = r->draws;
        int count = r->draw_count;

        vkCmdBindVertexBuffers(cmd, 0, 1, &r->vertex_buffer[frame_index], &off);
        vkCmdBindIndexBuffer(cmd, r->index_buffer[frame_index], 0, VK_INDEX_TYPE_UINT16);

        for (int i = 0; i < count; i++) {
            const SceneObject &obj = objects[i];
            glm::mat4 model = glm::translate(glm::mat4(1.0f), glm::vec3(obj.pos[0], obj.pos[1], obj.pos[2]));
            model = glm::rotate(model, obj.rotation[1], glm::vec3(0, 1, 0));
            model = glm::rotate(model, obj.rotation[0], glm::vec3(1, 0, 0));
            model = glm::rotate(model, obj.rotation[2], glm::vec3(0, 0, 1));
            model = glm::scale(model, glm::vec3(obj.scale));
            glm::mat4 mvp = proj * view * model;

            FragPC obj_fpc{};
            obj_fpc.mode = obj_mode + obj.tex_mode * 16;
            obj_fpc.albedo[0] = obj.color[0];
            obj_fpc.albedo[1] = obj.color[1];
            obj_fpc.albedo[2] = obj.color[2];
            obj_fpc.roughness = obj.roughness;
            obj_fpc.metallic  = obj.metallic;

            vkCmdPushConstants(cmd, r->pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 64, glm::value_ptr(mvp));
            vkCmdPushConstants(cmd, r->pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT, 64, 64, glm::value_ptr(model));
            vkCmdPushConstants(cmd, r->pipeline_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 128, sizeof(FragPC), &obj_fpc);
            vkCmdDrawIndexed(cmd, draws[i].idx_count, 1, draws[i].idx_offset, draws[i].vtx_offset, 0);
        }
    }
}

void renderer_draw_outline(Renderer *r, const VkCtx &ctx, VkCommandBuffer cmd,
                           const glm::mat4 &view, const glm::mat4 &proj,
                           const SceneObject &obj, int obj_index, uint32_t frame_index) {
    // Mesh already uploaded by renderer_draw this frame — reuse its offsets
    // instead of re-uploading (re-upload would clobber object 0's data).
    if (obj_index < 0 || obj_index >= r->draw_count) return;
    const ObjDrawInfo &draw = r->draws[obj_index];

    VkViewport vp = { 0, 0, (float)ctx.swapchain_extent.width, (float)ctx.swapchain_extent.height, 0, 1 };
    VkRect2D   sc = { {0, 0}, ctx.swapchain_extent };
    VkDeviceSize off = 0;

    float outline_scale = obj.scale * 1.05f; // slightly bigger
    glm::mat4 model = glm::translate(glm::mat4(1.0f), glm::vec3(obj.pos[0], obj.pos[1], obj.pos[2]));
    model = glm::rotate(model, obj.rotation[1], glm::vec3(0, 1, 0));
    model = glm::rotate(model, obj.rotation[0], glm::vec3(1, 0, 0));
    model = glm::rotate(model, obj.rotation[2], glm::vec3(0, 0, 1));
    model = glm::scale(model, glm::vec3(outline_scale));
    glm::mat4 mvp = proj * view * model;

    FragPC fpc{};
    fpc.mode = 4; // orange outline mode

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->pipeline_wireframe);
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);
    vkCmdPushConstants(cmd, r->pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 64, glm::value_ptr(mvp));
    vkCmdPushConstants(cmd, r->pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT, 64, 64, glm::value_ptr(model));
    vkCmdPushConstants(cmd, r->pipeline_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 128, sizeof(FragPC), &fpc);
    vkCmdBindVertexBuffers(cmd, 0, 1, &r->vertex_buffer[frame_index], &off);
    vkCmdBindIndexBuffer(cmd, r->index_buffer[frame_index], 0, VK_INDEX_TYPE_UINT16);
    vkCmdDrawIndexed(cmd, draw.idx_count, 1, draw.idx_offset, draw.vtx_offset, 0);
}

void renderer_draw_gizmo(Renderer *r, const VkCtx &ctx, uint32_t frame_index,
                         VkCommandBuffer cmd,
                         const glm::mat4 &view, const glm::mat4 &proj,
                         const SceneObject &obj, ToolMode tool, GizmoAxis highlight) {
    float len = (tool == TOOL_SCALE) ? 0.8f : 1.2f;
    glm::vec3 o(obj.pos[0], obj.pos[1], obj.pos[2]);
    glm::vec3 cam(r->cam_pos[0], r->cam_pos[1], r->cam_pos[2]);
    glm::vec3 to_cam = cam - o;
    float dist = glm::length(to_cam);
    if (dist < 1e-4f) return;
    glm::vec3 vdir = to_cam / dist;

    /* shaft width scales with distance -> roughly constant on screen */
    float w      = glm::clamp(dist * 0.006f, 0.008f, 0.06f);
    float head_w = w * 3.2f;
    float head_l = w * 9.0f;

    const glm::vec3 axes[3] = { {1,0,0}, {0,1,0}, {0,0,1} };
    Vertex gv[27];
    int n = 0;

    for (int a = 0; a < 3; a++) {
        glm::vec3 axis = axes[a];
        bool hot = (highlight == GIZMO_X + a);
        float b = hot ? 1.0f : 0.75f;
        /* axis colors: X red, Y green, Z blue; hot axis pulls toward white */
        glm::vec3 col = axis * b;
        if (hot) col += glm::vec3(0.35f);

        /* billboard: expand the shaft perpendicular to both axis and view */
        glm::vec3 side = glm::cross(axis, vdir);
        float sl = glm::length(side);
        if (sl < 1e-4f) {
            /* axis points at the camera — pick any perpendicular */
            side = glm::abs(axis.y) < 0.9f ? glm::cross(axis, glm::vec3(0,1,0))
                                           : glm::cross(axis, glm::vec3(1,0,0));
            sl = glm::length(side);
        }
        side /= sl;

        glm::vec3 tip = o + axis * len;
        glm::vec3 s = side * w;
        auto put = [&](glm::vec3 p) {
            gv[n++] = { p.x, p.y, p.z, col.r, col.g, col.b, 0, 1, 0, 0, 0 };
        };
        /* shaft quad (two tris) */
        put(o - s); put(tip - s); put(tip + s);
        put(o - s); put(tip + s); put(o + s);
        /* arrowhead tri (square tip for scale tool reads as a block) */
        glm::vec3 hs = side * head_w;
        put(tip - hs); put(tip + hs); put(tip + axis * head_l);
    }

    memcpy(r->gizmo_mapped[frame_index], gv, n * sizeof(Vertex));

    VkViewport vp = { 0, 0, (float)ctx.swapchain_extent.width, (float)ctx.swapchain_extent.height, 0, 1 };
    VkRect2D   sc = { {0, 0}, ctx.swapchain_extent };
    VkDeviceSize off = 0;

    /* overlay pipeline: no depth test, never hidden inside/behind the object */
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->pipeline_overlay);
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);

    glm::mat4 ident(1.0f);
    glm::mat4 mvp = proj * view * ident;

    FragPC fpc{};
    fpc.mode = 7; // flat vertex color

    vkCmdPushConstants(cmd, r->pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 64, glm::value_ptr(mvp));
    vkCmdPushConstants(cmd, r->pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT, 64, 64, glm::value_ptr(ident));
    vkCmdPushConstants(cmd, r->pipeline_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 128, sizeof(FragPC), &fpc);
    vkCmdBindVertexBuffers(cmd, 0, 1, &r->gizmo_vb[frame_index], &off);
    vkCmdDraw(cmd, (uint32_t)n, 1, 0, 0);
}
