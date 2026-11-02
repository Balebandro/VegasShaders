// VolumetricClouds techniques March (1) and MarchFixedExit (6) as a Vulkan pass (needs NVR DXVK): Effects/VolumetricClouds.fx.hlsl
// MarchPS and MarchRay line for line, one texel per 2 x 2 block, into TESR_CloudCurrent. effects/VolumetricClouds.cpp ExtRender.
// pc.fixedExit picks MarchPS(true); pc.physicalSky stands for PHYSICAL_SKY_ON.
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "Common.glsl"
#include "SkyAtmosphere.glsl"

#include "VolumetricCloudsMarchRay.glsl"   // (2026-10-30: the bindings and MarchRay, shared with VolumetricCloudsSkyProbe.frag)

void main() {
    vec2 vpos = floor(gl_FragCoord.xy);
    vec2 pixel = vpos * 2.0 + cb.blocks.zw;
    vec2 coord = (pixel + 0.5) * LowTexel;
    // (2026-10-27 Towering, as the .fx) the storm techniques move each pixel's offset on by the golden ratio once per march (each pixel
    // is marched every fourth frame), spread evenly over the step: about a third less grain after the history.
    float frameStep = pc.towers != 0u ? floor(cb.march.z * 0.25) : cb.march.z;
    float jitter = fract(textureLod(sampler2D(t_blueNoise, NVR_SAMPLER(pc.pointWrapIndex)), pixel / 256.0, 0.0).r + frameStep * 0.61803398875);
    o_color = MarchRay(normalize(toWorld(coord)), jitter, 1.0, pc.fixedExit != 0u, pc.towers != 0u);
}
