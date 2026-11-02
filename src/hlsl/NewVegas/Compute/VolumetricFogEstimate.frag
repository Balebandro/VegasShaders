// VolumetricFog's Estimate pass (DedicatedFog / CompositeFog / CompositeFogInterior pass 0, and their Legacy and Classic forms) as a
// Vulkan pass (needs NVR DXVK; effects/VolumetricFog.cpp ExtDedicated): Effects/VolumetricFog.fx.hlsl EstimatePS, into the two
// half-size fog targets at once (multiply + depth, add). Variants: Compute/variants.txt (l c i p).
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "VolumetricFog.glsl"

layout(location = 0) out vec4 o_mulDepth;
layout(location = 1) out vec4 o_add;

void main() {
    vec2 per = max(floor(1.0 / max(pc.fogLayout.xy, vec2(0.01)) + 0.5), vec2(1.0));
    float dN;
    vec2 tex = NearestCovered(floor(gl_FragCoord.xy), per, dN);
    vec3 mulV, addV;
    FogTerms(tex, dN, V_LOOK, V_INTERIOR, mulV, addV);
    o_mulDepth = vec4(mulV, dN);
    o_add = vec4(addV, 1.0);
}
