// Flashlight's shadow pass (Shadows) as a Vulkan pass (needs NVR DXVK): Effects/Flashlight.fx.hlsl line for line.
// effects/Flashlight.cpp ExtRender.
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "Flashlight.glsl"

void main() {
    float depth = readDepth(uv);
    vec3 eyeVector = toWorld(uv);
    vec4 worldPos = vec4(cb.cameraPosition.xyz + eyeVector * depth, 1.0);
    vec3 lightToWorld = cb.spotLightPosition.xyz - worldPos.xyz;

    float radius = cb.spotLightPosition.w;
    float Distance = length(lightToWorld) / radius;

    vec4 lightSpaceCoord = ScreenCoordToTexCoord(cb.flashLightViewProj * worldPos);
    float shadowDepth = LINEAR_LOD(t_shadow, lightSpaceCoord.xy).r;
    float isShadow = (shadowDepth + 0.05 * Distance > Distance) ? 1.0 : 0.0; // BIAS to reduce shadowing artefacts

    if (Distance < 1.0 && lightSpaceCoord.x > 0.0 && lightSpaceCoord.x < 1.0 && lightSpaceCoord.y > 0.0 && lightSpaceCoord.y < 1.0) {
        o_color = vec4(vec3(isShadow), 1.0);
        return;
    }
    o_color = vec4(1.0, 1.0, 1.0, 1.0);
}
