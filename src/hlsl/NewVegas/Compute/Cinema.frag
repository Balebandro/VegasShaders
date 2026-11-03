// Cinema as a Vulkan pass (needs NVR DXVK): Effects/Cinema.fx.hlsl line for line. effects/Cinema.cpp ExtRender.
// dither 0 = technique Cinema, dither 1 = technique CinemaSkyDither (Cinema, then the sky dither).
// t_noise is the file texture Effects\bluenoise256.dds: POINT and WRAP (the second sampler slot). Depth: CombineDepth's.
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "Common.glsl"

layout(set = 1, binding = 0) uniform texture2D t_rendered;   // TESR_RenderedBuffer (LINEAR, CLAMP)
layout(set = 1, binding = 1) uniform texture2D t_depth;      // TESR_DepthBuffer (LINEAR, CLAMP)
layout(set = 1, binding = 2) uniform texture2D t_noise;      // TESR_BlueNoiseSampler (POINT, WRAP)

layout(push_constant) uniform Push {
    vec4 cinemaData;             // 0: TESR_CinemaData (x aspect ratio, y vignette softness, z vignette intensity, w overlay strength)
    vec4 cinemaSettings;         // 16: TESR_CinemaSettings (y grain, z chromatic aberration, w letter box depth)
    vec4 gameTime;               // 32: TESR_GameTime
    vec4 reciprocalResolution;   // 48: TESR_ReciprocalResolution (xy 1/size, z aspect)
    vec4 cameraData;             // 64: TESR_CameraData (y far Z)
    vec4 skyDitherData;          // 80: TESR_SkyDitherData (x noise size, y sky only, z frame offset)
    uint samplerIndex;           // 96: LINEAR, CLAMP
    uint noiseSamplerIndex;      // 100: POINT, WRAP
    uint dither;                 // 104: 1 = CinemaSkyDither
} pc;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o_color;

// Helpers.hlsl and Depth.hlsl
vec2 expand2(vec2 v) { return ((v) - 0.5) / 0.5; }

float readDepth(vec2 coord) {
    return texture(sampler2D(t_depth, NVR_SAMPLER(pc.samplerIndex)), coord).x * pc.cameraData.y; // farZ
}

// Blending.hlsl's BlendMode_Overlay(float4 base, float4 blend)
float overlayChannel(float base, float blend) {
    return (base <= 0.5) ? 2.0 * base * blend : 1.0 - 2.0 * (1.0 - base) * (1.0 - blend);
}

vec4 blendModeOverlay(vec4 base, vec4 blend) {
    vec4 result = vec4(0.0);
    result.r = overlayChannel(base.r, blend.r);
    result.g = overlayChannel(base.g, blend.g);
    result.b = overlayChannel(base.b, blend.b);
    result.a = blend.a;
    return result * result.a + (1.0 - result.a) * base; // alphaBlend(result, base)
}

vec3 random(vec2 seed) {
    return texture(sampler2D(t_noise, NVR_SAMPLER(pc.noiseSamplerIndex)), (seed / 256.0 + 0.5) / pc.reciprocalResolution.xy).xyz;
}

vec4 cinema(vec2 uv) {
    // Letter box
    float aspectRatio = pc.cinemaData.x; // Ratio of the visible image (Width/Height)
    if (aspectRatio != 1.0) {
        float letterboxHeight = (1.0 - pc.reciprocalResolution.z / aspectRatio) / 2.0;
        float depth = readDepth(uv);
        if ((uv.y < letterboxHeight || uv.y > 1.0 - letterboxHeight) && depth > pc.cinemaSettings.w)
            return vec4(0.0, 0.0, 0.0, 1.0);
    }

    // Chromatic aberration
    float time = pc.gameTime.z * 25.0;
    vec2 chromaShift = pc.reciprocalResolution.xy * pc.cinemaSettings.z;
    vec2 posToCenter = vec2(0.0) - expand2(uv);

    vec4 color = texture(sampler2D(t_rendered, NVR_SAMPLER(pc.samplerIndex)), uv);
    color.r = texture(sampler2D(t_rendered, NVR_SAMPLER(pc.samplerIndex)), uv + posToCenter * chromaShift).r;
    color.b = texture(sampler2D(t_rendered, NVR_SAMPLER(pc.samplerIndex)), uv - posToCenter * chromaShift).b;

    // Film grain
    float grainAmount = pc.cinemaSettings.y * 0.01;
    vec3 noise1 = random(uv + time * pc.reciprocalResolution.xy);
    vec3 noise2 = random(uv - time * pc.reciprocalResolution.xy);
    color = blendModeOverlay(vec4(0.5 + grainAmount * ((noise1.r * noise2.b * 2.0 - 0.5) / 0.5)), color);

    // Photoshop overlay
    float overlayStrength = pc.cinemaData.w;
    color = mix(color, blendModeOverlay(color, color), overlayStrength);

    // Vignette: HLSL smoothstep(1, 1 - softness, dist), written out so that edge0 > edge1 is the same arithmetic
    float softness = pc.cinemaData.y;
    float intensity = pc.cinemaData.z;
    float dist = length(posToCenter) / 2.0;
    float e0 = 1.0;
    float e1 = 1.0 - softness;
    float s = clamp((dist - e0) / (e1 - e0), 0.0, 1.0);
    float vignette = s * s * (3.0 - 2.0 * s);

    color.rgb *= vignette * intensity + (1.0 - intensity);

    return vec4(color.rgb, 1.0);
}

// Dither.hlsl / SkyDither.frag
float gradientNoise(vec2 pixel) {
    return fract(52.9829189 * fract(dot(pixel, vec2(0.06711056, 0.00583715))));
}

void main() {
    vec4 color = cinema(uv);
    if (pc.dither != 0) {
        if (pc.skyDitherData.y > 0.0 && texture(sampler2D(t_depth, NVR_SAMPLER(pc.samplerIndex)), uv).x < 0.996) {
            o_color = color;
            return;
        }
        vec2 pixel = floor(gl_FragCoord.xy) + pc.skyDitherData.z * vec2(5.588238, 3.235);
        float noise = gradientNoise(pixel) + gradientNoise(pixel + vec2(47.0, 17.0)) - 1.0;
        color.rgb += noise * pc.skyDitherData.x;
    }
    o_color = color;
}
