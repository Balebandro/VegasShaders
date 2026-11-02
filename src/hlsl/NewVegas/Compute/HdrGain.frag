// HDR output (needs NVR DXVK 1.5-hdr; core/HdrOutput.h): the per-pixel highlight gain the fork applies at present.
// Inputs: the game's HDR scene before tonemapping (what ISHDRBLENDINSHADERCIN reads as DestBlend) and the SDR picture the
// tonemapper wrote (start of ShaderManager::RenderEffects). Output (R16F): gain >= 1 by which the SDR pixel's linear light is
// multiplied on an HDR display. Mid-tones and shadows keep gain 1 (the SDR look at paper white); only pixels the SDR tonemapper
// pushed towards white get room above paper white, up to peak = PeakNits / PaperWhiteNits.
#version 450
#extension GL_GOOGLE_include_directive : enable
#extension GL_EXT_samplerless_texture_functions : require
#include "Common.glsl"

layout(set = 1, binding = 0) uniform texture2D t_scene;   // HDR scene before tonemapping (encoded as the tonemapper reads it)
layout(set = 1, binding = 1) uniform texture2D t_sdr;     // tonemapped SDR picture (same size as the target)

layout(push_constant) uniform Push {
    float peak;          //  0: PeakNits / PaperWhiteNits (>= 1)
    float knee;          //  4: SDR level (0-1, linear, brightest channel) where the gain starts
    float strength;      //  8: 0-1, how much of the highlight headroom is used
    float sceneGamma;    // 12: TESR_ToneMapping.w (scene linearisation exponent); <= 0: sRGB curve (the gamma-space path)
    float exposure;      // 16: TESR_HDRData.y
    float outGamma;      // 20: TESR_HDRData.w (the tonemapper's output gamma); <= 0: sRGB curve
    float pad0;          // 24
    float pad1;          // 28
    uint  samplerIndex;  // 32: POINT, CLAMP
} pc;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o_gain;

void main() {
    vec3 scene = texture(sampler2D(t_scene, NVR_SAMPLER(pc.samplerIndex)), uv).rgb;
    vec3 sdr = texelFetch(t_sdr, ivec2(gl_FragCoord.xy), 0).rgb;

    vec3 sceneLin = pc.sceneGamma > 0.0 ? pows(max(scene, vec3(0.0)), pc.sceneGamma) : linearize(max(scene, vec3(0.0)));
    vec3 sdrLin = pc.outGamma > 0.0 ? pow(saturate(sdr), vec3(pc.outGamma)) : linearize(saturate(sdr));

    // The scene's brightness in the tonemapper's units (1 = what a linear tonemapper would show as white)
    float sceneY = luma(sceneLin) * pc.exposure;
    float sdrY = max(luma(sdrLin), 1e-4);
    float sdrMax = max(sdrLin.r, max(sdrLin.g, sdrLin.b));

    // Highlight target: follows the scene, rolls off smoothly towards the peak
    float hdrY = pc.peak * (1.0 - exp(-sceneY / pc.peak));
    float gain = max(hdrY / sdrY, 1.0);

    // Only where the SDR tonemapper compressed: from the knee up to white
    float t = smoothstep(pc.knee, 1.0, sdrMax) * saturate(pc.strength);
    gain = clamp(mix(1.0, gain, t), 1.0, pc.peak);

    o_gain = vec4(gain, gain, gain, 1.0);
}
