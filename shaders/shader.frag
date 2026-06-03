#version 450
layout(location = 0) in vec3 vColor;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in vec3 vPos;
layout(location = 0) out vec4 fColor;
void main() {
    vec3 lightDir = normalize(vec3(1.0, 2.0, 1.0));
    vec3 lightCol = vec3(1.0, 0.95, 0.85);
    vec3 N        = normalize(vNormal);
    float diff    = max(dot(N, lightDir), 0.0);
    float ambient = 0.25;
    vec3 lit      = vColor * (ambient + diff) * lightCol;
    fColor = vec4(lit, 1.0);
}
