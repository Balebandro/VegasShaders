// Cinema as a Vulkan pass (needs NVR DXVK): Effects/Cinema.fx.hlsl line for line, both techniques (push dither 1 = CinemaSkyDither).
// effects/Cinema.cpp ExtRender. Native frames only (the Vulkan passes are not used in render-rectangle frames).
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "Common.glsl"

layout(set = 1, binding = 0) uniform texture2D t_rendered;   // TESR_RenderedBuffer (LINEAR, CLAMP)
layout(set = 1, binding = 1) uniform texture2D t_depth;      // TESR_DepthBuffer (linear depth / farZ)
layout(set = 1, binding = 2) uniform texture2D t_noise;      // TESR_BlueNoiseSampler (POINT, WRAP)

layout(push_constant) uniform Push {
    vec4 cinemaData;           // 0: TESR_CinemaData
    vec4 cinemaSettings;       // 16: TESR_CinemaSettings
    vec4 gameTime;             // 32: TESR_GameTime
    vec4 reciprocalResolution; // 48: TESR_ReciprocalResolution (z: width / height)
    vec4 cameraData;           // 64: TESR_CameraData (x near Z, y far Z)
    vec4 skyDitherData;        // 80: TESR_SkyDitherData
    uint samplerIndex;         // 96: LINEAR, CLAMP
    uint noiseSamplerIndex;    // 100: POINT, WRAP
    uint dither;               // 104: 1 for technique 1 (CinemaSkyDither)
    uint pad;                  // 108
} pc;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o_color;

const float RAW_SKY_LIMIT = 0.996;

float overlayChannel(float b, float l) { return b < 0.5 ? 2.0 * b * l : 1.0 - 2.0 * (1.0 - b) * (1.0 - l); }
vec3 overlayColour(vec3 b, vec3 l) { return vec3(overlayChannel(b.r, l.r), overlayChannel(b.g, l.g), overlayChannel(b.b, l.b)); }

float interleavedGradient(vec2 p) { return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715)))); }

float readDepthLinear01(vec2 coord) { return textureLod(sampler2D(t_depth, NVR_SAMPLER(pc.samplerIndex)), coord, 0.0).x; }

// Depth.hlsl projectedDepthFromLinear: the raw post-projection depth from linear depth / farZ.
float projectedDepthFromLinear(float linear01) {
    float nearZ = pc.cameraData.x;
    float farZ = pc.cameraData.y;
    float viewZ = max(linear01 * farZ, 1.0e-6);
    return nearZ * (farZ - viewZ) / (viewZ * (farZ - nearZ));
}

vec4 cinemaCore(vec2 uv, bool skyDither) {
    vec2 pixel = floor(uv / pc.reciprocalResolution.xy);

    float wanted = pc.cinemaData.x;
    if (wanted > 0.0 && wanted != 1.0) {
        float edge = 0.5 * (1.0 - pc.reciprocalResolution.z / wanted);
        if ((uv.y < edge || uv.y > 1.0 - edge) && readDepthLinear01(uv) * pc.cameraData.y > pc.cinemaSettings.w)
            return vec4(0.0, 0.0, 0.0, 1.0);
    }

    vec2 shift = (uv - 0.5) * 2.0 * pc.cinemaSettings.z * pc.reciprocalResolution.xy;
    vec3 c = vec3(
        textureLod(sampler2D(t_rendered, NVR_SAMPLER(pc.samplerIndex)), uv - shift, 0.0).r,
        textureLod(sampler2D(t_rendered, NVR_SAMPLER(pc.samplerIndex)), uv, 0.0).g,
        textureLod(sampler2D(t_rendered, NVR_SAMPLER(pc.samplerIndex)), uv + shift, 0.0).b);

    c = mix(c, overlayColour(c, c), pc.cinemaData.w);

    float frame = floor(pc.gameTime.z * 25.0);
    vec2 noiseUV = (pixel + 0.5) / 256.0;
    float n1 = textureLod(sampler2D(t_noise, NVR_SAMPLER(pc.noiseSamplerIndex)), noiseUV + frame * vec2(0.6180339, 0.3819660), 0.0).r;
    float n2 = textureLod(sampler2D(t_noise, NVR_SAMPLER(pc.noiseSamplerIndex)), noiseUV + frame * vec2(0.7548777, 0.5698403), 0.0).g;
    float grain = 2.0 * n1 * n2;
    c = mix(c, overlayColour(c, vec3(grain)), pc.cinemaSettings.y * 0.01);

    float dist = length(uv - 0.5);
    float edge0 = min(1.0 - pc.cinemaData.y, 0.999);
    float falloff = 1.0 - smoothstep(edge0, 1.0, dist);
    c *= 1.0 - pc.cinemaData.z * (1.0 - falloff);
    c = max(c, vec3(0.0));

    vec4 result = vec4(c, 1.0);

    if (skyDither) {
        float raw = projectedDepthFromLinear(readDepthLinear01(uv));
        if (!(pc.skyDitherData.y > 0.5 && raw < RAW_SKY_LIMIT)) {
            vec2 offset = pc.skyDitherData.z * vec2(5.588238);
            float noise = interleavedGradient(pixel + offset) + interleavedGradient(pixel.yx + offset) - 1.0;
            result.rgb += pc.skyDitherData.x * noise;
        }
    }
    return result;
}

void main() {
    o_color = cinemaCore(uv, pc.dither == 1u);
}
