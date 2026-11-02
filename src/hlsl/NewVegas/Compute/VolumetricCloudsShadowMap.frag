// VolumetricClouds techniques ShadowMap (4), ShadowMapTowers (10) and ShadowMapPrefilter (11) as a Vulkan pass (needs NVR DXVK): Effects/VolumetricClouds.fx.hlsl ShadowMapPS line for
// line, into TESR_CloudShadow. effects/VolumetricClouds.cpp ExtRender.
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "Common.glsl"

layout(set = 1, binding = 0) uniform texture2D t_weather;    // TESR_CloudWeather (LINEAR, WRAP, LINEAR mips)
layout(set = 1, binding = 1) uniform texture3D t_shape;      // TESR_CloudShape (LINEAR, WRAP, LINEAR mips)
layout(set = 1, binding = 2) uniform texture3D t_detail;     // TESR_CloudDetail (LINEAR, WRAP, LINEAR mips)
layout(set = 1, binding = 3) uniform texture3D t_noise;      // TESR_NoiseVolume (LINEAR, WRAP, LINEAR mips)

#define CLOUDS_DENSITY
#include "VolumetricClouds.glsl"

void main() {
    vec2 vpos = floor(gl_FragCoord.xy);
    vec2 at = ShadowCentre() + ((vpos + 0.5) / ShadowMapSize - 0.5) * ShadowRange;
    vec3 light = ShadowLight();
    float len = Thickness / light.z;
    float stepLength = len / 8.0;
    vec3 wind = vec3(cb.wind.xy, 0.0);
    float opticalDepth = 0.0;
    if (pc.prefilter != 0u) {
        // (2026-10-27 ShadowPrefilter, technique ShadowMapPrefilter: ShadowMapPrefilterPS line for line: the smooth shape only, four
        // taps round the texel on a rotated grid, ShadowSoftness (cb.tower.w) texels apart, their mean)
        float spread = cb.tower.w * ShadowRange / ShadowMapSize;
        for (int t = 0; t < 4; t++) {
            vec2 o = t == 0 ? vec2(-0.125, -0.375) : t == 1 ? vec2(0.375, -0.125) : t == 2 ? vec2(-0.375, 0.125) : vec2(0.125, 0.375);
            vec2 tap = at + o * spread;
            for (float i = 0.5; i < 8.0; i += 1.0) {
                vec3 p = vec3(tap, Base) + light * (i * stepLength);
                opticalDepth += CloudDensity(p + wind, (p.z - Base) / Thickness, 1.0, true, false) * stepLength;
            }
        }
        o_color = vec4(exp(-Extinction * opticalDepth * 0.25), 0.0, 0.0, 1.0);
        return;
    }
    if (pc.towers != 0u) {
        // (2026-10-25 Towering, ShadowMapPS(true): the .fx's real loop, the same samples)
        for (float i = 0.5; i < 8.0; i += 1.0) {
            vec3 p = vec3(at, Base) + light * (i * stepLength);
            opticalDepth += CloudDensity(p + wind, (p.z - Base) / Thickness, 1.0, false, true) * stepLength;
        }
    }
    else {
        for (int i = 0; i < 8; i++) {
            float s = (float(i) + 0.5) * stepLength;
            vec3 p = vec3(at, Base) + light * s;
            opticalDepth += CloudDensity(p + wind, (p.z - Base) / Thickness, 1.0, false, false) * stepLength;
        }
    }
    o_color = vec4(exp(-Extinction * opticalDepth), 0.0, 0.0, 1.0);
}
