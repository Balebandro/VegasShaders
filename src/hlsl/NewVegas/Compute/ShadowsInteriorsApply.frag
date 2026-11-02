// Interior shadows, apply pass as a Vulkan pass (needs NVR DXVK). The D3D9 DedicatedApply of Effects/ShadowsInteriors.fx.hlsl
// (Darken), line for line: the scene (binding 0, TESR_RenderedBuffer) darkened by the blurred term (binding 1) with depth (binding 2).
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "Common.glsl"

layout(set = 1, binding = 0) uniform texture2D t_scene;   // TESR_RenderedBuffer (the untouched scene)
layout(set = 1, binding = 1) uniform texture2D t_term;    // the blurred term (scratch B, or the raw mask in skip-blur mode)
layout(set = 1, binding = 2) uniform texture2D t_depth;   // TESR_DepthBuffer

layout(push_constant) uniform Push {
    vec4 cameraData;            // 0: y = far distance
    vec4 shadowData;            // 16: y = darkness
    vec4 shadowFade;            // 32: y = interior shadows enabled (0 or 1), w = draw distance
    mat4 viewTransform;         // 48: as the renderer stores it
    mat4 projectionTransform;   // 112
    uint samplerIndex;          // 176: LINEAR, CLAMP
    uint pad0, pad1, pad2;
} pc;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o_color;

// Depth.hlsl toWorld(): the world-space view direction through this pixel (unnormalised, view depth 1 along forward).
vec3 toWorldDir(vec2 tex) {
    vec3 v = vec3(pc.viewTransform[0][2], pc.viewTransform[1][2], pc.viewTransform[2][2]);
    v += (1.0 / pc.projectionTransform[0][0] * (2.0 * tex.x - 1.0)) * vec3(pc.viewTransform[0][0], pc.viewTransform[1][0], pc.viewTransform[2][0]);
    v += (-1.0 / pc.projectionTransform[1][1] * (2.0 * tex.y - 1.0)) * vec3(pc.viewTransform[0][1], pc.viewTransform[1][1], pc.viewTransform[2][1]);
    return v;
}

void main() {
    float farZ = pc.cameraData.y;
    vec3 scene = texture(sampler2D(t_scene, NVR_SAMPLER(pc.samplerIndex)), uv).rgb;
    float depth = textureLod(sampler2D(t_depth, NVR_SAMPLER(pc.samplerIndex)), uv, 0.0).r * farZ;
    float term = texture(sampler2D(t_term, NVR_SAMPLER(pc.samplerIndex)), uv).r;

    // Gamma 2.2 to linear; the highlights above 1 are kept.
    vec3 colour = pow(max(scene, vec3(0.0)), vec3(2.2));
    vec3 original = colour;

    float dist = depth * length(toWorldDir(uv));

    // Fully lit beyond the draw distance; fades in from 300 units.
    float shadow = clamp(term, 0.0, 1.0);
    float fadeEnd = max(pc.shadowFade.w, 301.0);
    shadow = mix(shadow, 1.0, clamp((dist - 300.0) / (fadeEnd - 300.0), 0.0, 1.0));

    float d = pc.shadowData.y;
    float curve = (5.0 * d) / (1.0 + 5.0 * d);
    float darkness = clamp(1.0 - curve * pc.shadowFade.y, 0.0, 1.0);
    shadow = clamp(mix(darkness, 1.0, shadow), 0.0, 1.0);
    float power = 1.0 - shadow;

    colour = clamp(shadow * pow(max(colour, vec3(0.0)), vec3(1.0 - power * 0.2)), 0.0, 1.0);
    float l = dot(colour, vec3(0.2126, 0.7152, 0.0722));
    colour = max(mix(vec3(l), colour, 1.0 + power * 0.3), vec3(0.0));

    colour += max(original - 1.0, vec3(0.0));
    o_color = vec4(pow(max(colour, vec3(0.0)), vec3(1.0 / 2.2)), 1.0);
}
