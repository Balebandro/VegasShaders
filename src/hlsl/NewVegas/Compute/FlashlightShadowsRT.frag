// UNOFFICIAL 2026-10-30 RT lamp shadows Flashlight: Flashlight's shadow pass (FlashlightShadows.frag) with the traced shadow
// (RtFlashTrace.comp, screen space, 1 lit / 0 blocked) in place of the shadow map lookup; the same reach and cone test around it.
// effects/Flashlight.cpp ExtRender binds the traced image to t_shadow.
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
    float isShadow = LINEAR_LOD(t_shadow, uv).r;

    if (Distance < 1.0 && lightSpaceCoord.x > 0.0 && lightSpaceCoord.x < 1.0 && lightSpaceCoord.y > 0.0 && lightSpaceCoord.y < 1.0) {
        o_color = vec4(vec3(isShadow), 1.0);
        return;
    }
    o_color = vec4(1.0, 1.0, 1.0, 1.0);
}
