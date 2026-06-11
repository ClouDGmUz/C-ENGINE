# Shaders

**Source:** `shaders/shader.vert`, `shaders/shader.frag`

One vertex shader and one über fragment shader covering every visual mode. Compiled to SPIR-V by glslang at build time (see [[Build System]]) and embedded into the [[Renderer]] as C headers — no runtime shader files.

## Depends on

Nothing — but the push-constant layout is a binary contract with the [[Renderer]] (`FragPC` struct must match field-for-field).

## Used by

- [[Renderer]] — embeds `vert_spv` / `frag_spv` arrays, selects modes via push constant

## Vertex shader

Inputs: position, color, normal, UV. Push constants: `mvp` (0–63) and `model` (64–127). Outputs world-space position and normal for lighting.

## Fragment modes (`render_mode = mode % 16`, `tex_mode = mode / 16`)

| Mode | Meaning |
|------|---------|
| 0 | Wireframe / flat vertex color (else-branch) |
| 1 | Solid — vertex color with hemispheric top light |
| 2 | Rendered — GGX specular + Lambert diffuse, hemispheric ambient, fill light, rim, fake AO, exponential fog, ACES tonemap + gamma |
| 3 | Infinite ground grid — fwidth-based anti-aliased major/minor lines, red X axis, blue Z axis, distance fade |
| 4 | Selection outline — flat orange |
| 5 | Atmosphere sky — gradient + sun disk + halo, direction decoded from vertex color |
| 6 | Planar shadow — flat dark overlay |

Procedural textures (`tex_mode` 1–4): checker, brick, marble, wood — hash/value-noise/fbm based, no samplers anywhere.

## Known issues

- Atmosphere mode 5 hardcodes sky colors; the Sky Top/Bottom UI pickers in the [[Main Loop]] have no effect in Environment mode
- Mode 6 computes `shadowAlpha` then ignores it; `fColor` assigned twice
- `light_dir = (0,0,0)` from the UI normalizes to NaN
