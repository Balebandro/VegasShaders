// BarrelHaze as a Vulkan pass (needs NVR DXVK): Effects/BarrelHaze.fx.hlsl (technique HeatHaze, its one pass) line for line.
// effects/BarrelHaze.cpp ExtRender. readDepth: linear depth / farZ in TESR_DepthBuffer's first channel, times farZ.
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "Common.glsl"

layout(set = 1, binding = 0) uniform texture2D t_rendered;   // TESR_RenderedBuffer (LINEAR, CLAMP)
layout(set = 1, binding = 1) uniform texture2D t_depth;      // TESR_DepthBuffer (POINT, CLAMP)

layout(push_constant) uniform Push {
    vec4 reciprocalResolution;  // 0: TESR_ReciprocalResolution
    vec4 hazeLine;              // 16: HazeLine (projected muzzle and rear end, UV)
    vec4 hazeData;              // 32: HazeData (heat, displacement in pixels, plume height and width in pixels)
    vec4 hazeAnimation;         // 48: HazeAnimation (seconds, speed, debug mask, gun clip W)
    vec4 hazeBlast;             // 64: HazeBlast (muzzle blast: displacement in pixels now, radius in pixels)
    vec4 cameraData;            // 80: TESR_CameraData (y farZ)
    uint linearIndex;           // 96: LINEAR, CLAMP
    uint pointIndex;            // 100: POINT, CLAMP
    uint pad[2];                // 104
} pc;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o_color;

float readDepth(vec2 coord) {
    return texture(sampler2D(t_depth, NVR_SAMPLER(pc.pointIndex)), coord).x * pc.cameraData.y;
}

void main() {
    vec2 pixel = (uv - pc.hazeLine.xy) / pc.reciprocalResolution.xy;
    vec2 barrel = (pc.hazeLine.zw - pc.hazeLine.xy) / pc.reciprocalResolution.xy;
    float along = clamp(dot(pixel, barrel) / max(dot(barrel, barrel), 1.0), 0.0, 1.0);
    vec2 relative = pixel - barrel * along;
    float height = max(pc.hazeData.z, 1.0), width = max(pc.hazeData.w, 1.0);
    float rise = -relative.y / height;

    // A sleeve of shimmer all around the barrel (fading with distance from it, rounded at the ends), plus the plume
    // rising above it, since hot air goes up.
    float sleeve = exp(-dot(relative, relative) / (width * width));
    float plume = smoothstep(0.0, 0.12, rise) * (1.0 - smoothstep(0.65, 1.0, rise)) * exp(-relative.x * relative.x / (width * width));
    float mask = max(sleeve, plume) * pc.hazeData.x;
    // Do not distort surfaces substantially closer than the drawn gun.
    // (skipped while the debug mask is shown, so the mask always shows the whole plume)
    if (pc.hazeAnimation.z < 0.5) mask *= step(pc.hazeAnimation.w * 0.8, readDepth(uv));
    float phase = pc.hazeAnimation.x * pc.hazeAnimation.y;
    vec2 wave = vec2(sin(relative.x / width * 2.7 + rise * 14.0 - phase * 4.1),
                     sin(relative.x / width * 4.3 - rise * 19.0 + phase * 2.9));
    vec2 offset = wave * vec2(1.0, 0.35) * pc.hazeData.y * mask * pc.reciprocalResolution.xy;

    // Muzzle blast: a ring of shimmer pushing outward from the muzzle, wider than the barrel haze, only for an instant
    // after each shot (HazeBlast.x fades to 0), whatever the barrel heat.
    float radius = max(pc.hazeBlast.y, 1.0);
    float dist = length(pixel);
    float blastMask = exp(-dist * dist / (radius * radius)) * step(0.01, pc.hazeBlast.x);
    if (pc.hazeAnimation.z < 0.5) blastMask *= step(pc.hazeAnimation.w * 0.8, readDepth(uv));
    float ripple = sin(dist / radius * 9.0 - phase * 40.0) + 0.5 * sin(dist / radius * 17.0 + phase * 23.0);
    offset += (pixel / max(dist, 1.0)) * ripple * pc.hazeBlast.x * blastMask * pc.reciprocalResolution.xy;

    vec4 color = texture(sampler2D(t_rendered, NVR_SAMPLER(pc.linearIndex)), clamp(uv + offset, 0.0, 1.0));
    if (pc.hazeAnimation.z > 0.5) color.rgb = mix(color.rgb, vec3(1.0, 0.2, 0.0), clamp(mask, 0.0, 1.0) * 0.65);
    if (pc.hazeAnimation.z > 0.5) color.rgb = mix(color.rgb, vec3(0.0, 0.6, 1.0), clamp(blastMask * pc.hazeBlast.x / 6.0, 0.0, 1.0) * 0.6);
    o_color = color;
}
