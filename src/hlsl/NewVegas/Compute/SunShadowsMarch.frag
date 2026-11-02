// SunShadows contact march (techniques 0, 1, 3, 4, first pass) as a Vulkan pass: Effects/SunShadows.fx.hlsl ContactMarchPS (the
// FastPaths form: the skips are real branches; the picture is the same as the other form). Four samples toward the sun, step
// lengths scaled per pixel by blue noise, nearer samples counting more. Red: contact factor (1 lit); green: the lamps' term kept.
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "SunShadows.glsl"

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o_color;

void main() {
    float lamps = sourceAt(uv).g;
    float viewZ = viewDepthAt(uv);
    vec3 viewPos = viewPositionAt(uv, viewZ);
    vec3 viewSun = normalize(cb.viewSpaceLightDir.xyz);
    float facing = sunFacing(viewNormalAt(uv), viewSun);
    float contact = 1.0;
    if (!noContact(viewZ, facing)) {
        float noise = textureLod(sampler2D(t_noise, NOISE_WRAP), uv / (cb.reciprocalResolution.xy * 256.0), 0.0).r;
        float stepLength = 400.0 * pow(clamp(viewZ / farDistance(), 0.0, 1.0), 0.6) * mix(min(0.8, viewZ / renderDistance()), 1.0, noise);
        float occlusion = 0.0, weights = 0.0;
        for (int i = 1; i <= 4; i++) {
            vec3 rayPoint = viewPos + viewSun * (stepLength * float(i));
            float weight = 1.0 / float(i);
            occlusion += weight * rayBlocked(texCoordOf(rayPoint), rayPoint.z);
            weights += weight;
        }
        contact = contactFromOcclusion(occlusion / weights, facing, viewZ);
    }
    o_color = vec4(contact, lamps, 0.0, 1.0);
}
