/* SPDX-License-Identifier: Apache-2.0
 * Ported from Blender's Cycles renderer and adapted for Vulkan GLSL.
 * Source: https://github.com/blender/blender
 * - Perlin noise (snoise 1D-4D): adapted from OSL
 * - Voronoi: Inigo Quilez + Blender smooth Voronoi
 * - Wave texture: Blender Cycles kernel/svm/wave.h
 * - Gradient texture: Blender Cycles kernel/svm/gradient.h
 */

/* ─── Perlin / Simplex noise (1D-4D) ───
   Adapted from Blender Cycles kernel/svm/noise.h,
   originally from Open Shading Language. */

float noise_hash(float n) {
    return fract(sin(n) * 43758.5453);
}

float noise_hash2(vec2 p) {
    return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453);
}

float noise_hash3(vec3 p) {
    return fract(sin(dot(p, vec3(127.1, 311.7, 74.7))) * 43758.5453);
}

float noise_hash4(vec4 p) {
    return fract(sin(dot(p, vec4(127.1, 311.7, 74.7, 269.5))) * 43758.5453);
}

float snoise_1d(float p) {
    float i = floor(p);
    float f = fract(p);
    float u = f * f * (3.0 - 2.0 * f);
    return mix(noise_hash(i), noise_hash(i + 1.0), u) * 2.0 - 1.0;
}

float snoise_2d(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    return mix(mix(noise_hash2(i), noise_hash2(i + vec2(1.0, 0.0)), u.x),
               mix(noise_hash2(i + vec2(0.0, 1.0)), noise_hash2(i + vec2(1.0, 1.0)), u.x),
               u.y) * 2.0 - 1.0;
}

float snoise_3d(vec3 p) {
    vec3 i = floor(p);
    vec3 f = fract(p);
    vec3 u = f * f * (3.0 - 2.0 * f);
    return mix(mix(mix(noise_hash3(i), noise_hash3(i + vec3(1.0, 0.0, 0.0)), u.x),
                   mix(noise_hash3(i + vec3(0.0, 1.0, 0.0)), noise_hash3(i + vec3(1.0, 1.0, 0.0)), u.x), u.y),
               mix(mix(noise_hash3(i + vec3(0.0, 0.0, 1.0)), noise_hash3(i + vec3(1.0, 0.0, 1.0)), u.x),
                   mix(noise_hash3(i + vec3(0.0, 1.0, 1.0)), noise_hash3(i + vec3(1.0, 1.0, 1.0)), u.x), u.y),
               u.z) * 2.0 - 1.0;
}

float snoise_4d(vec4 p) {
    vec4 i = floor(p);
    vec4 f = fract(p);
    vec4 u = f * f * (3.0 - 2.0 * f);
    return mix(mix(mix(mix(noise_hash4(i), noise_hash4(i + vec4(1.0, 0.0, 0.0, 0.0)), u.x),
                        mix(noise_hash4(i + vec4(0.0, 1.0, 0.0, 0.0)), noise_hash4(i + vec4(1.0, 1.0, 0.0, 0.0)), u.x), u.y),
                    mix(mix(noise_hash4(i + vec4(0.0, 0.0, 1.0, 0.0)), noise_hash4(i + vec4(1.0, 0.0, 1.0, 0.0)), u.x),
                        mix(noise_hash4(i + vec4(0.0, 1.0, 1.0, 0.0)), noise_hash4(i + vec4(1.0, 1.0, 1.0, 0.0)), u.x), u.y), u.z),
               mix(mix(mix(noise_hash4(i + vec4(0.0, 0.0, 0.0, 1.0)), noise_hash4(i + vec4(1.0, 0.0, 0.0, 1.0)), u.x),
                        mix(noise_hash4(i + vec4(0.0, 1.0, 0.0, 1.0)), noise_hash4(i + vec4(1.0, 1.0, 0.0, 1.0)), u.x), u.y),
                    mix(mix(noise_hash4(i + vec4(0.0, 0.0, 1.0, 1.0)), noise_hash4(i + vec4(1.0, 0.0, 1.0, 1.0)), u.x),
                        mix(noise_hash4(i + vec4(0.0, 1.0, 1.0, 1.0)), noise_hash4(i + vec4(1.0, 1.0, 1.0, 1.0)), u.x), u.y), u.z),
               u.w) * 2.0 - 1.0;
}

/* ─── Fractal Brownian Motion (FBM) ───
   From Blender Cycles kernel/svm/fractal_noise.h */

float noise_fbm(vec3 p, float detail, float roughness, float lacunarity, bool normalize) {
    float fscale = 1.0;
    float amp = 1.0;
    float maxamp = 0.0;
    float sum = 0.0;
    int idetail = int(detail);
    for (int i = 0; i <= idetail; i++) {
        float t = snoise_3d(fscale * p);
        sum += t * amp;
        maxamp += amp;
        amp *= roughness;
        fscale *= lacunarity;
    }
    float rmd = detail - float(idetail);
    if (rmd > 0.0) {
        float t = snoise_3d(fscale * p);
        float sum2 = sum + t * amp;
        return normalize ? mix(0.5 * sum / maxamp + 0.5, 0.5 * sum2 / (maxamp + amp) + 0.5, rmd) : mix(sum, sum2, rmd);
    }
    return normalize ? 0.5 * sum / maxamp + 0.5 : sum;
}

float noise_multi_fractal(vec3 p, float detail, float roughness, float lacunarity) {
    float value = 1.0;
    float pwr = 1.0;
    int idetail = int(detail);
    for (int i = 0; i <= idetail; i++) {
        value *= (pwr * snoise_3d(p) + 1.0);
        pwr *= roughness;
        p *= lacunarity;
    }
    float rmd = detail - float(idetail);
    if (rmd > 0.0) value *= (rmd * pwr * snoise_3d(p) + 1.0);
    return value;
}

float noise_hetero_terrain(vec3 p, float detail, float roughness, float lacunarity, float offset) {
    float pwr = roughness;
    float value = offset + snoise_3d(p);
    p *= lacunarity;
    int idetail = int(detail);
    for (int i = 1; i <= idetail; i++) {
        float increment = (snoise_3d(p) + offset) * pwr * value;
        value += increment;
        pwr *= roughness;
        p *= lacunarity;
    }
    float rmd = detail - float(idetail);
    if (rmd > 0.0) {
        float increment = (snoise_3d(p) + offset) * pwr * value;
        value += rmd * increment;
    }
    return value;
}

float noise_hybrid_multi_fractal(vec3 p, float detail, float roughness, float lacunarity, float offset, float gain) {
    float pwr = 1.0;
    float value = 0.0;
    float weight = 1.0;
    int idetail = int(detail);
    for (int i = 0; (weight > 0.001) && (i <= idetail); i++) {
        weight = min(weight, 1.0);
        float signal = (snoise_3d(p) + offset) * pwr;
        pwr *= roughness;
        value += weight * signal;
        weight *= gain * signal;
        p *= lacunarity;
    }
    float rmd = detail - float(idetail);
    if (rmd > 0.0 && weight > 0.001) {
        float signal = (snoise_3d(p) + offset) * pwr;
        value += rmd * weight * signal;
    }
    return value;
}

float noise_ridged_multi_fractal(vec3 p, float detail, float roughness, float lacunarity, float offset, float gain) {
    float pwr = roughness;
    float signal = offset - abs(snoise_3d(p));
    signal *= signal;
    float value = signal;
    int idetail = int(detail);
    for (int i = 1; i <= idetail; i++) {
        p *= lacunarity;
        float weight = clamp(signal * gain, 0.0, 1.0);
        signal = offset - abs(snoise_3d(p));
        signal *= signal;
        signal *= weight;
        value += signal * pwr;
        pwr *= roughness;
    }
    return value;
}

/* ─── Voronoi texture ───
   Adapted from Blender Cycles kernel/svm/voronoi.h
   Original smooth Voronoi by Inigo Quilez. */

#define VORONOI_F1      0
#define VORONOI_F2      1
#define VORONOI_SMOOTH  2
#define VORONOI_DIST_EUCLIDEAN     0
#define VORONOI_DIST_MANHATTAN     1
#define VORONOI_DIST_CHEBYCHEV     2
#define VORONOI_DIST_MINKOWSKI     3

float voronoi_distance(vec3 a, vec3 b, int metric, float exponent) {
    vec3 d = abs(a - b);
    if (metric == VORONOI_DIST_EUCLIDEAN) return length(d);
    if (metric == VORONOI_DIST_MANHATTAN) return d.x + d.y + d.z;
    if (metric == VORONOI_DIST_CHEBYCHEV) return max(max(d.x, d.y), d.z);
    // Minkowski
    return pow(pow(d.x, exponent) + pow(d.y, exponent) + pow(d.z, exponent), 1.0 / max(exponent, 1e-2));
}

float voronoi(vec3 p, float smoothness, int feature, int metric, float exponent) {
    vec3 i = floor(p);
    vec3 f = fract(p);

    float da[4];
    vec3 pa[4];
    float db[4];
    vec3 pb[4];

    for (int c = 0; c < 4; c++) { da[c] = 1e10; db[c] = 1e10; }

    for (int x = -1; x <= 1; x++)
    for (int y = -1; y <= 1; y++)
    for (int z = -1; z <= 1; z++) {
        vec3 cell = vec3(float(x), float(y), float(z));
        vec3 rnd = vec3(noise_hash3(i + cell),
                        noise_hash3(i + cell + vec3(11.0, 47.0, 83.0)),
                        noise_hash3(i + cell + vec3(19.0, 71.0, 113.0)));
        vec3 pos = cell + rnd - f;
        float d = voronoi_distance(vec3(0.0), pos, metric, exponent);

        for (int c = 0; c < 4; c++) {
            if (d < da[c]) {
                for (int k = 3; k > c; k--) {
                    da[k] = da[k-1]; pa[k] = pa[k-1];
                    db[k] = db[k-1]; pb[k] = pb[k-1];
                }
                da[c] = d; pa[c] = pos;
                break;
            }
        }
        for (int c = 0; c < 4; c++) {
            float cell_hash = noise_hash3(i + cell + vec3(37.0));
            if (cell_hash < db[c] && d > da[0] + 1e-5) {
                for (int k = 3; k > c; k--) db[k] = db[k-1];
                db[c] = cell_hash; pb[c] = pos;
                break;
            }
        }
    }

    if (feature == VORONOI_SMOOTH && smoothness > 0.0) {
        float h = clamp(smoothness, 0.0, 1.0);
        float sum = 0.0, wsum = 0.0;
        for (int c = 0; c < 4; c++) {
            float w = exp(-da[c] * (h * 10.0 + 0.01));
            sum += da[c] * w;
            wsum += w;
        }
        return sum / max(wsum, 1e-8);
    }

    int idx = (feature == VORONOI_F2) ? 1 : 0;
    return da[idx];
}

/* ─── Wave texture ───
   Adapted from Blender Cycles kernel/svm/wave.h */

#define WAVE_BANDS 0
#define WAVE_RINGS 1
#define WAVE_SIN   0
#define WAVE_SAW   1
#define WAVE_TRI   2

float wave_texture(vec3 p, int wave_type, int bands_dir, int rings_dir,
                   int profile, float distortion, float detail, float dscale,
                   float droughness, float phase) {
    p = (p + 0.000001) * 0.999999;
    float n;

    if (wave_type == WAVE_BANDS) {
        if (bands_dir == 0) n = p.x * 20.0;           // X
        else if (bands_dir == 1) n = p.y * 20.0;       // Y
        else if (bands_dir == 2) n = p.z * 20.0;       // Z
        else n = (p.x + p.y + p.z) * 10.0;              // Diagonal
    } else {
        vec3 rp = p;
        if (rings_dir == 0) rp *= vec3(0.0, 1.0, 1.0);      // X
        else if (rings_dir == 1) rp *= vec3(1.0, 0.0, 1.0);  // Y
        else if (rings_dir == 2) rp *= vec3(1.0, 1.0, 0.0);  // Z
        n = length(rp) * 20.0;
    }
    n += phase;

    if (distortion != 0.0) {
        n += distortion * (noise_fbm(p * dscale, detail, droughness, 2.0, true) * 2.0 - 1.0);
    }

    if (profile == WAVE_SIN) return 0.5 + 0.5 * sin(n);
    if (profile == WAVE_SAW) { n /= (3.14159265359 * 2.0); return n - floor(n); }
    // Triangle
    n /= (3.14159265359 * 2.0);
    return abs(n - floor(n + 0.5)) * 2.0;
}

/* ─── Gradient texture ───
   Adapted from Blender Cycles kernel/svm/gradient.h */

#define GRAD_LINEAR           0
#define GRAD_QUADRATIC        1
#define GRAD_EASING           2
#define GRAD_DIAGONAL         3
#define GRAD_RADIAL           4
#define GRAD_QUADRATIC_SPHERE 5
#define GRAD_SPHERICAL        6

float gradient_texture(vec3 p, int gradient_type) {
    float x = p.x, y = p.y, z = p.z;
    if (gradient_type == GRAD_LINEAR) return x;
    if (gradient_type == GRAD_QUADRATIC) { float r = max(x, 0.0); return r * r; }
    if (gradient_type == GRAD_EASING) {
        float r = clamp(x, 0.0, 1.0);
        float t = r * r;
        return 3.0 * t - 2.0 * t * r;
    }
    if (gradient_type == GRAD_DIAGONAL) return (x + y) * 0.5;
    if (gradient_type == GRAD_RADIAL) return atan(y, x) / (3.14159265359 * 2.0) + 0.5;
    float r = max(0.999999 - sqrt(x*x + y*y + z*z), 0.0);
    if (gradient_type == GRAD_QUADRATIC_SPHERE) return r * r;
    return r;
}
