// SunShadows sharp march (technique 2, first pass) as a Vulkan pass: Effects/SunShadows.fx.hlsl SharpMarchPS. One line toward the
// sun, 16 even steps on screen, ray depth interpolated in 1 / z, nearer steps counting more; no noise.
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
        float nearPlane = max(cb.cameraData.x, 1.0e-3);
        vec3 rayEnd = viewPos + viewSun * (2.0 * 2000.0 * pow(clamp(viewZ / farDistance(), 0.0, 1.0), 0.6));
        float keep = rayEnd.z < nearPlane ? clamp((viewPos.z - nearPlane) / max(viewPos.z - rayEnd.z, 1.0e-4), 0.0, 1.0) : 1.0;
        rayEnd = mix(viewPos, rayEnd, keep);
        vec2 endUV = texCoordOf(rayEnd);
        float invStart = 1.0 / max(viewPos.z, nearPlane);
        float invEnd = 1.0 / max(rayEnd.z, nearPlane);
        float occlusion = 0.0, weights = 0.0;
        for (int k = 1; k <= 16; k++) {
            float t = float(k) / 16.0;
            float weight = 1.0 / (1.0 + 4.0 * t);
            occlusion += weight * rayBlocked(mix(uv, endUV, t), 1.0 / mix(invStart, invEnd, t));
            weights += weight;
        }
        contact = contactFromOcclusion(occlusion / weights, facing, viewZ);
    }
    o_color = vec4(contact, lamps, 0.0, 1.0);
}
