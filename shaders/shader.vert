#version 450
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aColor;
layout(location = 2) in vec3 aNormal;
layout(location = 0) out vec3 vColor;
layout(location = 1) out vec3 vNormal;
layout(location = 2) out vec3 vPos;
layout(push_constant) uniform PC { mat4 mvp; mat4 model; } pc;
void main() {
    gl_Position = pc.mvp * vec4(aPos, 1.0);
    vColor  = aColor;
    vNormal = mat3(pc.model) * aNormal;
    vPos    = (pc.model * vec4(aPos, 1.0)).xyz;
}
