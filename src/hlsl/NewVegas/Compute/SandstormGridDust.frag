// Sandstorm technique 1 (GridDust, the fast path's dust grid) as a Vulkan pass (needs NVR DXVK): Effects/Sandstorm.fx.hlsl
// GridDustPS line for line, into the grid atlas. effects/Sandstorm.cpp ExtRender.
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "Common.glsl"

layout(set = 1, binding = 0) uniform texture2D t_ortho;      // TESR_OrthoMapBuffer (POINT, CLAMP)
layout(set = 1, binding = 1) uniform texture2D t_atlas;      // TESR_ShadowAtlas (LINEAR, CLAMP)

#define SS_ORTHO
#define SS_SHADOWS
#include "SandstormCommon.glsl"

void main() {
    vec2 vpos = floor(gl_FragCoord.xy);
    vec2 tile = floor(vpos / GridSize);
    vec2 cell = vpos - tile * GridSize;          // the column, 0 .. GridSize - 1
    float z = tile.y * 8.0 + tile.x;
    float t0 = GridDistance(z), t1 = GridDistance(z + 1.0);
    vec3 dir = normalize(toWorld((cell + 0.5) / GridSize));

    float intensity = TESR_SandstormData.x;
    float density = TESR_SandstormData.y * intensity;
    float heightFalloff = TESR_SandstormData.w;
    float time = TESR_SandstormWind.w;
    vec2 wind = TESR_SandstormWind.xy;
    float speed = TESR_SandstormWind.z;
    vec3 camera = TESR_CameraPosition.xyz;
    vec3 dustAmbient, dustSun;
    DustLighting(dir, dustAmbient, dustSun);

    // One step of the reference march, at the cell's middle.
    vec3 drift = vec3(wind * speed * time, -speed * 0.05 * time);
    float middle = 0.5 * (t0 + t1);
    vec3 p = camera + dir * middle;
    float height = exp(-max(p.z - camera.z + 200.0, 0.0) / heightFalloff);
    float clouds = fbm((p - drift) / TESR_SandstormShape.y);
    float local = density * height * mix(1.0, saturate(clouds * 2.2 - 0.35) * 1.8, TESR_SandstormShape.x) * (t1 - t0);
    // The sun's shadow and the shelter upwind are read at four points spread over the cell and averaged (see Sandstorm.fx.hlsl).
    vec2 spread = 0.25 / GridSize;
    vec2 centre = (cell + 0.5) / GridSize;
    vec3 sub[4] = vec3[4](normalize(toWorld(centre + vec2(-spread.x, -spread.y))) * mix(t0, t1, 0.125),
                          normalize(toWorld(centre + vec2(spread.x, -spread.y))) * mix(t0, t1, 0.625),
                          normalize(toWorld(centre + vec2(-spread.x, spread.y))) * mix(t0, t1, 0.875),
                          normalize(toWorld(centre + vec2(spread.x, spread.y))) * mix(t0, t1, 0.375));
    if (middle < 500.0) {
        float openShare = 0.25 * (InTheWindFast(sub[0], wind, false) + InTheWindFast(sub[1], wind, false) + InTheWindFast(sub[2], wind, false) + InTheWindFast(sub[3], wind, false));
        local *= mix(mix(0.35, 1.0, openShare), 1.0, smoothstep(350.0, 500.0, middle));
    }
    float absorbed = 1.0 - exp(-local);
    float sun = 0.25 * (SunVisibility(sub[0]) + SunVisibility(sub[1]) + SunVisibility(sub[2]) + SunVisibility(sub[3]));
    vec3 light = dustAmbient + dustSun * sun;
    o_color = vec4(absorbed * light * (0.8 + 0.4 * clouds), local);
}
