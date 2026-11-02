// SkyView technique 4 (SkyViewXMS: as technique 3, with the multiple scattering table Psi_ms added along the view ray) as a
// Vulkan pass (needs NVR DXVK): Effects/SkyView.fx.hlsl SkyViewXMSPS line for line. effects/SkyView.cpp ExtRender.
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "Common.glsl"

#define SAMPLE_TRANS(coord) textureLod(sampler2D(t_trans, NVR_SAMPLER(pc.samplerIndex)), (coord), 0.0)
#define SAMPLE_MS(coord) textureLod(sampler2D(t_ms, NVR_SAMPLER(pc.samplerIndex)), (coord), 0.0)

layout(set = 1, binding = 0) uniform texture2D t_trans;   // TESR_SkyTransLUT
layout(set = 1, binding = 1) uniform texture2D t_ms;      // TESR_SkyMSLUT

layout(push_constant) uniform Push {
    vec4 physSky;      // 0: TESR_PhysSky (x share, y brightness, z weather tint, w haze)
    vec4 physSkySun;   // 16: TESR_PhysSkySun (xyz the sun's real direction)
    uint samplerIndex; // 32: LINEAR, CLAMP (the heap index, filled by the host)
    vec2 uvShift;      // 40: 0.5 / screen size - 0.5 / table size (SkyView.cpp): the D3D9 quad's half-pixel offset is the screen's, so D3D9 reads the table at uv + this
} pc;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o_color;

#include "SkyAtmosphere.glsl"
#include "SkyTables.glsl"

void main() {
    vec2 tuv = uv + pc.uvShift;   // the D3D9 pass's coordinate, not the texel centre
    float azimuth = tuv.x * AtmPi;
    float elevation = tuv.y * tuv.y * (0.5 * AtmPi);
    vec3 dir = vec3(cos(elevation) * cos(azimuth), cos(elevation) * sin(azimuth), max(sin(elevation), 0.002));
    vec3 sun = vec3(sqrt(clamp(1.0 - pc.physSkySun.z * pc.physSkySun.z, 0.0, 1.0)), 0.0, pc.physSkySun.z);
    o_color = vec4(AtmSkyX(normalize(dir), sun, 300.0, pc.physSky.w, true) * TableScale, 1.0);
}
