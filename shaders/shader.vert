#version 450
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aColor;
layout(location = 2) in vec3 aNormal;
layout(location = 3) in vec2 aUV;
layout(location = 0) out vec3 vColor;
layout(location = 1) out vec3 vNormal;
layout(location = 2) out vec3 vPos;
layout(location = 3) out vec2 vUV;
layout(location = 4) out vec4 vShadowCoord;
layout(push_constant) uniform PC { mat4 mvp; mat4 model; } pc;
layout(set = 0, binding = 0) uniform Scene {
    mat4 light_space;
    mat4 inv_vp;      // inverse(proj * view) — world ray reconstruction
    vec4 sky_top;     // rgb, w = cloud coverage
    vec4 sky_bottom;  // rgb, w = cloud speed
    vec4 fog;         // rgb, w = density
    vec4 misc;        // x = time, y = shadows on, z = fog on, w = bg_mode (0/1/2)
    vec4 sun;         // xyz = direction (unnormalized), w = intensity
    vec4 sun_color;   // rgb, w = angular diameter in degrees
    vec4 cam;         // xyz = position, w = exposure
    vec4 params;      // x = ambient, y = shadow softness, z = xray density, w = turbidity
} ubo;
void main() {
    gl_Position = pc.mvp * vec4(aPos, 1.0);
    vColor  = aColor;
    vNormal = mat3(pc.model) * aNormal;
    vPos    = (pc.model * vec4(aPos, 1.0)).xyz;
    vUV     = aUV;
    vShadowCoord = ubo.light_space * pc.model * vec4(aPos, 1.0);
}
