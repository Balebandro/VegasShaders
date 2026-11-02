// VolumetricFog's full-resolution passes as one Vulkan pass (needs NVR DXVK; effects/VolumetricFog.cpp ExtApplyPass), push.mode:
// 0 the Reconstruct of DedicatedFog, 1 of CompositeFog, 2 of CompositeFogInterior (Effects/VolumetricFog.fx.hlsl ReconstructPS),
// 3 ApplyShadowAO (ShadowAOPS), 4 the full-resolution techniques VolumetricFog / VolumetricFogLegacy / VolumetricFogClassic (FullPS).
// Variants: Compute/variants.txt (m l c p).
#version 450
#extension GL_GOOGLE_include_directive : enable
#define FOG_APPLY
#include "VolumetricFog.glsl"

layout(location = 0) out vec4 o_color;

void main() {
    vec4 src = LINEAR_LOD(t_scene, uv);
    int mode = V_MODE;
    if (mode == 3) {
        float alpha = src.a;
        vec3 c = CompositeScene(uv, src.rgb, COMP_EXTERIOR, alpha);
        o_color = vec4(c, alpha);
        return;
    }
    if (mode == 4) {
        float dN = POINT_LOD(t_depth, uv).x;
        vec3 mulV, addV;
        FogTerms(uv, dN, V_LOOK, false, mulV, addV);
        vec3 linC = linearize(src.rgb) * mulV + addV;
        o_color = vec4(delinearize(max(linC, vec3(0.0))), mix(src.a, 1.0, NightShare(false)));
        return;
    }
    float alpha = src.a;
    vec3 stored = src.rgb;
    if (mode != COMP_NONE) stored = CompositeScene(uv, stored, mode, alpha);
    float dP = POINT_LOD(t_depth, uv).x;
    vec3 mulV, addV;
    UpsampleFog(uv, floor(gl_FragCoord.xy), dP, V_LOOK, mode == COMP_INTERIOR, mulV, addV);
    vec3 linC = linearize(stored) * mulV + addV;
    if (pc.gridFlags.y > 0.5) linC = LightAirComposite(uv, dP, linC);
    o_color = vec4(delinearize(max(linC, vec3(0.0))), mix(alpha, 1.0, NightShare(mode == COMP_INTERIOR)));
}
