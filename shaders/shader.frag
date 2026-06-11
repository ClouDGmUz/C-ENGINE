#version 450
layout(location = 0) in vec3 vColor;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in vec3 vPos;
layout(location = 3) in vec2 vUV;
layout(location = 4) in vec4 vShadowCoord;
layout(location = 0) out vec4 fColor;

/* Per-draw material. mode = render_mode + tex_mode * 16.
   Render modes: 0 flat albedo (wireframe), 1 solid studio, 2 PBR,
   3 ground grid, 4 outline, 5 physical sky, 6 x-ray, 7 flat vertex color. */
layout(push_constant) uniform PC {
    layout(offset = 128) int mode;
    float albedo_r;
    float albedo_g;
    float albedo_b;
    float roughness;
    float metallic;
} pc;

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
layout(set = 0, binding = 1) uniform sampler2DShadow shadow_map;

const float PI = 3.14159265359;

// --- Noise helpers ---
float hash(vec2 p) {
    return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453);
}

float noise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(hash(i), hash(i + vec2(1,0)), f.x),
               mix(hash(i + vec2(0,1)), hash(i + vec2(1,1)), f.x), f.y);
}

float fbm(vec2 p) {
    float v = 0.0;
    v += noise(p) * 0.5;
    v += noise(p * 2.0) * 0.25;
    v += noise(p * 4.0) * 0.125;
    v += noise(p * 8.0) * 0.0625;
    return v;
}

// --- Procedural textures ---
vec3 tex_checker(vec2 uv) {
    vec2 c = floor(uv * 8.0);
    float m = mod(c.x + c.y, 2.0);
    return mix(vec3(0.9), vec3(0.15), m);
}

vec3 tex_brick(vec2 uv) {
    vec2 p = uv * vec2(6.0, 12.0);
    float row = floor(p.y);
    if (mod(row, 2.0) > 0.5) p.x += 0.5;
    vec2 brick = fract(p);
    float mortar = 0.06;
    float bx = step(mortar, brick.x) * step(mortar, brick.y);
    vec3 brickCol = mix(vec3(0.6, 0.2, 0.1), vec3(0.7, 0.3, 0.15), noise(floor(p)));
    return mix(vec3(0.5, 0.5, 0.45), brickCol, bx);
}

vec3 tex_marble(vec2 uv) {
    float n = fbm(uv * 4.0);
    float v = sin(uv.x * 10.0 + n * 8.0) * 0.5 + 0.5;
    return mix(vec3(0.9, 0.9, 0.95), vec3(0.2, 0.25, 0.3), v * v);
}

vec3 tex_wood(vec2 uv) {
    vec2 p = uv * 4.0;
    float r = length(p - vec2(2.0)) + fbm(p * 0.5) * 2.0;
    float ring = fract(r * 4.0);
    ring = smoothstep(0.0, 0.05, ring) * smoothstep(0.3, 0.25, ring);
    vec3 light_wood = vec3(0.7, 0.5, 0.3);
    vec3 dark_wood = vec3(0.4, 0.25, 0.1);
    float n = fbm(p * 2.0);
    return mix(light_wood, dark_wood, ring * 0.7 + n * 0.3);
}

// --- GGX/PBR helpers ---
float D_GGX(float NdotH, float roughness) {
    float a = roughness * roughness;
    float a2 = a * a;
    float d = (NdotH * NdotH) * (a2 - 1.0) + 1.0;
    return a2 / (PI * d * d + 0.0001);
}
float G_SchlickGGX(float NdotV, float roughness) {
    float k = (roughness + 1.0) * (roughness + 1.0) / 8.0;
    return NdotV / (NdotV * (1.0 - k) + k);
}
float G_Smith(float NdotV, float NdotL, float roughness) {
    return G_SchlickGGX(NdotV, roughness) * G_SchlickGGX(NdotL, roughness);
}
vec3 F_Schlick(float cosTheta, vec3 F0) {
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

// --- Shadow map sampling (9-tap Poisson PCF) ---
float sample_shadow() {
    if (ubo.misc.y < 0.5) return 1.0;
    vec3 p = vShadowCoord.xyz / vShadowCoord.w;
    vec2 uv = p.xy * 0.5 + 0.5;
    if (p.z > 1.0 || p.z < 0.0) return 1.0;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) return 1.0;

    const vec2 poisson[9] = vec2[](
        vec2( 0.000,  0.000), vec2(-0.942, -0.399), vec2( 0.946, -0.769),
        vec2(-0.094, -0.929), vec2( 0.345,  0.294), vec2(-0.916,  0.458),
        vec2(-0.815, -0.879), vec2(-0.382,  0.276), vec2( 0.975,  0.756));

    float radius = (0.5 + ubo.params.y * 3.0) / 2048.0;
    float sum = 0.0;
    for (int i = 0; i < 9; i++)
        sum += texture(shadow_map, vec3(uv + poisson[i] * radius, p.z));
    return sum / 9.0;
}

// --- Exposure + ACES tonemap + gamma ---
vec3 tonemap(vec3 c) {
    c *= ubo.cam.w;
    c = c * (c * 2.51 + 0.03) / (c * (c * 2.43 + 0.59) + 0.14);
    return pow(clamp(c, 0.0, 1.0), vec3(1.0 / 2.2));
}

// --- Fog: exponential extinction + single-scatter glow toward the sun ---
vec3 physical_sky(vec3 dir, vec3 sun_dir); // fwd decl, defined below

vec3 apply_fog(vec3 col, vec3 frag_pos) {
    if (ubo.misc.z < 0.5) return col;
    vec3 to_frag = frag_pos - ubo.cam.xyz;
    float dist = length(to_frag);
    vec3 view_dir = to_frag / max(dist, 1e-4);
    float f = 1.0 - exp(-dist * ubo.fog.w);
    vec3 fog_col;
    if (ubo.misc.w > 1.5) {
        // atmosphere mode: haze takes the horizon sky color in the view
        // direction, so distant geometry dissolves into the sky seamlessly
        vec3 h = normalize(vec3(view_dir.x, 0.06, view_dir.z));
        fog_col = physical_sky(h, normalize(ubo.sun.xyz)) * 0.9;
    } else {
        float sun_amt = pow(max(dot(view_dir, normalize(ubo.sun.xyz)), 0.0), 8.0);
        fog_col = mix(ubo.fog.rgb, ubo.sun_color.rgb, sun_amt * 0.6);
    }
    return mix(col, fog_col, f);
}

// --- Physical atmosphere: single-scatter Rayleigh + Mie ---
// Flat-atmosphere air-mass model. Sun elevation drives color: low sun
// reddens (long path absorbs short wavelengths), zenith stays blue.
float phase_rayleigh(float mu) { return 3.0 / (16.0 * PI) * (1.0 + mu * mu); }
float phase_mie(float mu) {
    const float g = 0.76, g2 = g * g;
    return (1.0 - g2) / (4.0 * PI * pow(1.0 + g2 - 2.0 * g * mu, 1.5));
}

vec3 physical_sky(vec3 dir, vec3 sun_dir) {
    float turbidity = max(ubo.params.w, 1.0);
    vec3  betaR = vec3(0.058, 0.135, 0.331);   // Rayleigh ∝ 1/λ⁴ (r,g,b)
    vec3  betaM = vec3(0.012 * turbidity);     // Mie haze, wavelength-flat

    float mu = clamp(dot(dir, sun_dir), -1.0, 1.0);
    float m_view = 1.0 / (max(dir.y, 0.0) + 0.025);      // view-path air mass
    float m_sun  = 1.0 / (max(sun_dir.y, 0.01) + 0.025); // sun-path air mass

    vec3 ext_view = exp(-(betaR + betaM) * m_view);
    vec3 ext_sun  = exp(-(betaR + betaM) * m_sun);       // sun reddening

    vec3 E = ubo.sun_color.rgb * ubo.sun.w * 16.0;
    vec3 scatter = (phase_rayleigh(mu) * betaR + phase_mie(mu) * betaM) / (betaR + betaM);
    vec3 sky = E * ext_sun * scatter * (1.0 - ext_view);

    // sun disk at its true angular size, attenuated by the atmosphere
    float cos_r = cos(radians(ubo.sun_color.w) * 0.5);
    float eps = max((1.0 - cos_r) * 0.5, 2e-6);
    float disk = smoothstep(cos_r - eps, cos_r + eps, mu);
    sky += E * ext_sun * disk;
    // lens glare so the tiny real-size disk still reads on screen
    sky += E * ext_sun * pow(max(mu, 0.0), 350.0) * 0.08;

    // clouds: planar-projected fbm, lit by atmosphere-filtered sunlight
    float coverage = ubo.sky_top.w;
    if (coverage > 0.01 && dir.y > 0.02) {
        vec2 cuv = dir.xz / (dir.y + 0.15) * 0.6;
        cuv += ubo.misc.x * ubo.sky_bottom.w * 0.01 * vec2(1.0, 0.35);
        float c = fbm(cuv) + 0.35 * fbm(cuv * 3.1 + 17.0);
        float cloud = smoothstep(1.0 - coverage, 1.0 - coverage + 0.35, c);
        cloud *= smoothstep(0.02, 0.2, dir.y); // fade at horizon
        float shade = mix(0.6, 1.05, fbm(cuv * 2.0 + 4.2)); // self-shading
        vec3 sun_tint = clamp(ext_sun * 1.4, 0.0, 1.0);
        vec3 cloud_col = mix(vec3(0.9), sun_tint, 0.55) * shade
                       * (0.25 + 0.75 * clamp(ubo.sun.w, 0.0, 1.5));
        sky = mix(sky, cloud_col, cloud * 0.85);
    }

    // dim below the horizon line
    sky *= exp(min(dir.y, 0.0) * 6.0);
    return sky;
}

void main() {
    // Decode mode: render_mode = mode % 16, tex_mode = mode / 16
    int mode_packed = pc.mode;
    int render_mode = mode_packed % 16;
    int tex_mode = mode_packed / 16;

    vec3 N = normalize(vNormal);

    // Material albedo, tinted by optional procedural pattern
    vec3 albedo = vec3(pc.albedo_r, pc.albedo_g, pc.albedo_b);
    if      (tex_mode == 1) albedo *= tex_checker(vUV);
    else if (tex_mode == 2) albedo *= tex_brick(vUV);
    else if (tex_mode == 3) albedo *= tex_marble(vUV);
    else if (tex_mode == 4) albedo *= tex_wood(vUV);

    if (render_mode == 2) {
        // --- PBR: Cook-Torrance, metalness workflow ---
        vec3 L = normalize(ubo.sun.xyz);
        vec3 V = normalize(ubo.cam.xyz - vPos);
        vec3 H = normalize(L + V);
        vec3 sunE = ubo.sun_color.rgb * ubo.sun.w * 3.2;

        float NdotL = max(dot(N, L), 0.0);
        float NdotV = max(dot(N, V), 0.001);
        float NdotH = max(dot(N, H), 0.0);
        float HdotV = max(dot(H, V), 0.0);

        float rough = clamp(pc.roughness, 0.03, 1.0);
        float metal = clamp(pc.metallic, 0.0, 1.0);
        vec3 F0 = mix(vec3(0.04), albedo, metal);

        float D = D_GGX(NdotH, rough);
        float G = G_Smith(NdotV, NdotL, rough);
        vec3 F = F_Schlick(HdotV, F0);
        vec3 specular = (D * G * F) / (4.0 * NdotV * NdotL + 0.0001);

        vec3 kD = (vec3(1.0) - F) * (1.0 - metal); // metals have no diffuse
        vec3 diffuse = kD * albedo / PI;

        float shadow = sample_shadow();
        vec3 direct = (diffuse + specular) * sunE * NdotL * shadow;

        // hemispheric ambient sampled from the active sky model
        float hemi = 0.5 + 0.5 * N.y;
        vec3 amb_up, amb_down;
        if (ubo.misc.w > 1.5) {            // physical atmosphere
            amb_up   = physical_sky(vec3(0.0, 1.0, 0.0), L) * 0.8;
            amb_down = physical_sky(normalize(vec3(L.x, 0.05, L.z)), L) * 0.2;
        } else if (ubo.misc.w > 0.5) {     // gradient sky
            amb_up   = ubo.sky_top.rgb * 0.6;
            amb_down = ubo.sky_bottom.rgb * 0.25;
        } else {                           // solid background: neutral studio
            amb_up   = sunE * 0.08 + vec3(0.05);
            amb_down = vec3(0.03);
        }
        vec3 amb_col = mix(amb_down, amb_up, hemi) * ubo.params.x * 2.0;
        vec3 ambient = amb_col * mix(albedo, F0 * 0.8, metal);

        vec3 lit = direct + ambient;
        lit = apply_fog(lit, vPos);
        fColor = vec4(tonemap(lit), 1.0);

    } else if (render_mode == 1) {
        // --- Solid: hemispheric studio shading + subtle view rim ---
        vec3 V = normalize(ubo.cam.xyz - vPos);
        float top = 0.5 + 0.5 * N.y;
        float ndv = max(dot(N, V), 0.0);
        vec3 c = albedo * mix(0.45, 1.0, top);
        c += vec3(0.10) * pow(1.0 - ndv, 3.0);            // rim
        c += vec3(0.06) * pow(ndv, 16.0);                 // headlight glint
        fColor = vec4(c, 1.0);

    } else if (render_mode == 3) {
        // --- Ground: infinite grid, receives shadows + fog ---
        vec2 coord = vPos.xz;
        vec2 dv = fwidth(coord);
        vec2 grid1 = abs(fract(coord - 0.5) - 0.5) / dv;
        float line1 = min(grid1.x, grid1.y);
        vec2 grid2 = abs(fract(coord * 4.0 - 0.5) - 0.5) / (dv * 4.0);
        float line2 = min(grid2.x, grid2.y);
        float minor = 1.0 - min(line2, 1.0);
        float major = 1.0 - min(line1, 1.0);

        vec3 base = vec3(0.08);
        if (ubo.misc.w > 1.5)      base = vec3(0.085, 0.082, 0.078);
        else if (ubo.misc.w > 0.5) base = ubo.sky_bottom.rgb * 0.22;
        vec3 col = base + vec3(0.04);
        col = mix(col, base + vec3(0.22), minor * 0.5);
        col = mix(col, base + vec3(0.37), major);
        float xAxis = 1.0 - min(abs(coord.y) / dv.y, 1.0);
        float zAxis = 1.0 - min(abs(coord.x) / dv.x, 1.0);
        col = mix(col, vec3(0.8, 0.2, 0.2), xAxis * 0.8);
        col = mix(col, vec3(0.2, 0.3, 0.8), zAxis * 0.8);

        vec3 camP = ubo.cam.xyz;
        float dist = length(vPos.xz - camP.xz);
        float fade = 1.0 - smoothstep(5.0, 25.0, dist);
        float alpha = max(minor * 0.3, major * 0.7);
        alpha = max(alpha, max(xAxis, zAxis) * 0.9);
        alpha *= fade;
        vec3 ground = mix(base, col, alpha);

        // shadow darkens the ground toward a cool tone
        float shadow = sample_shadow();
        ground *= mix(0.35, 1.0, shadow);

        ground = apply_fog(ground, vPos);
        // atmosphere fog carries HDR sky values — run the same tonemap as
        // the sky so the grid fades into the horizon without a seam
        if (ubo.misc.w > 1.5) ground = tonemap(ground);
        fColor = vec4(ground, 1.0);

    } else if (render_mode == 4) {
        fColor = vec4(1.0, 0.5, 0.0, 1.0);

    } else if (render_mode == 5) {
        // --- Physical sky: world ray from inverse view-projection ---
        // Sky tri is drawn with identity matrices, so vPos.xy = NDC.
        vec4 far_w = ubo.inv_vp * vec4(vPos.xy, 1.0, 1.0);
        vec3 dir = normalize(far_w.xyz / far_w.w - ubo.cam.xyz);
        vec3 sun_dir = normalize(ubo.sun.xyz);
        vec3 sky = physical_sky(dir, sun_dir);
        fColor = vec4(tonemap(sky), 1.0);

    } else if (render_mode == 6) {
        // --- X-ray: Beer-Lambert-style surface accumulation ---
        // Additive pipeline (no cull, no depth) sums every surface layer:
        // grazing angles traverse more material -> brighter, and stacked
        // objects accumulate like a real radiograph.
        vec3 V = normalize(ubo.cam.xyz - vPos);
        float ndv = abs(dot(N, V));
        float thickness = 0.12 + pow(1.0 - ndv, 1.6);   // path length proxy
        float a = thickness * ubo.params.z;
        vec3 film = vec3(0.45, 0.75, 1.05);             // radiograph film tint
        fColor = vec4(film * a, 1.0);

    } else if (render_mode == 7) {
        // --- Flat vertex color: gizmo + gradient sky ---
        fColor = vec4(vColor, 1.0);

    } else {
        fColor = vec4(albedo, 1.0);
    }
}
