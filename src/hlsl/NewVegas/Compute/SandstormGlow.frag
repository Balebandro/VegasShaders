// Sandstorm technique 6 (Glow, quarter resolution) as a Vulkan pass (needs NVR DXVK): Effects/Sandstorm.fx.hlsl GlowPS line
// for line, into the glow buffer (rgb the glow, a the depth). effects/Sandstorm.cpp ExtRender.
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "Common.glsl"

layout(set = 1, binding = 0) uniform texture2D t_depth;      // TESR_DepthBuffer (POINT, CLAMP)

#define SS_DEPTH
#include "SandstormCommon.glsl"

void main() {
    vec2 vpos = floor(gl_FragCoord.xy);
    vec2 uvc = (vpos + 0.5) * QuarterTexel - 0.5 * QuarterTexel + 1.5 * TESR_ReciprocalResolution.xy;   // pixel (1, 1) of the 4 x 4 block
    float depth = readDepthLod(uvc);
    float visibility = TESR_SandstormData.z;
    float dist = depth / farZ >= 0.9961 ? visibility * 6.0 : length(toWorld(uvc) * depth);   // as ViewDistance
    vec3 dir = normalize(toWorld(uvc));
    float density = TESR_SandstormData.y * TESR_SandstormData.x;
    vec3 camera = TESR_CameraPosition.xyz;
    vec3 glow = vec3(0.0);
    if (TESR_SandstormLights.x > 0.0) {
        for (int l = 0; l < 12; l++) {
            glow += PointGlowFast(TESR_ShadowLightPosition[l], TESR_LightColor[l], camera, dir, dist, density);
            glow += PointGlowFast(TESR_LightPosition[l], TESR_LightColor[12 + l], camera, dir, dist, density);
        }
        glow *= density * TESR_SandstormColor.rgb * TESR_SandstormLights.x * 0.01;
    }
    o_color = vec4(glow, depth / farZ);
}
