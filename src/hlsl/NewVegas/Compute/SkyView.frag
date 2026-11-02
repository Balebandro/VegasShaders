// SkyView (technique 0, the sky-view table) as a Vulkan pass (needs NVR DXVK): Effects/SkyView.fx.hlsl SkyViewPS line for line.
// effects/SkyView.cpp ExtRender. The push block is the same in all five SkyView passes (the sampler index is unused here).
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "SkyAtmosphere.glsl"

layout(push_constant) uniform Push {
    vec4 physSky;      // 0: TESR_PhysSky (x share, y brightness, z weather tint, w haze)
    vec4 physSkySun;   // 16: TESR_PhysSkySun (xyz the sun's real direction, w the ground albedo)
    uint samplerIndex; // 32: unused here
    vec2 uvShift;      // 40: 0.5 / screen size - 0.5 / table size (SkyView.cpp): the D3D9 quad's half-pixel offset is the screen's, so D3D9 reads the table at uv + this
} pc;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o_color;

void main() {
    vec2 tuv = uv + pc.uvShift;   // the D3D9 pass's coordinate, not the texel centre
    float azimuth = tuv.x * AtmPi;
    float elevation = tuv.y * tuv.y * (0.5 * AtmPi);
    // In the sun's own frame: the sun along +x at its height, the view at this bearing from it.
    vec3 dir = vec3(cos(elevation) * cos(azimuth), cos(elevation) * sin(azimuth), max(sin(elevation), 0.002));
    vec3 sun = vec3(sqrt(clamp(1.0 - pc.physSkySun.z * pc.physSkySun.z, 0.0, 1.0)), 0.0, pc.physSkySun.z);
    o_color = vec4(AtmSky(normalize(dir), sun, 300.0, pc.physSky.w) * TableScale, 1.0);
}
