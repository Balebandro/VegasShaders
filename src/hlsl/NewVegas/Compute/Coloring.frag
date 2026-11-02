// Coloring as a Vulkan pass (needs NVR DXVK): Effects/Coloring.fx.hlsl line for line. lutPass 0 = Coloring (the grade alone),
// 1 = ColoringLUT (the grade, then the LUT). Bindings: 0 the picture, 1 day, 2 night, 3 interior LUT (read only when lutPass is 1).
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "Common.glsl"

layout(set = 1, binding = 0) uniform texture2D t_rendered;   // TESR_RenderedBuffer (LINEAR, CLAMP)
layout(set = 1, binding = 1) uniform texture2D t_day;        // TESR_LUTDayBuffer
layout(set = 1, binding = 2) uniform texture2D t_night;      // TESR_LUTNightBuffer
layout(set = 1, binding = 3) uniform texture2D t_interior;   // TESR_LUTInteriorBuffer

layout(push_constant) uniform Push {
    vec4 colorCurve;    // 0: TESR_ColoringColorCurve (x ColorCurve, yzw the tone mask curves)
    vec4 effectGamma;   // 16: TESR_ColoringEffectGamma (x EffectGamma, yzw per channel)
    vec4 data;          // 32: TESR_ColoringData (x Strength, y BaseGamma, z Fade, w Contrast)
    vec4 values;        // 48: TESR_ColoringValues (x Saturation, y Bleach, z BleachLuma, w Linearization)
    vec4 lutData;       // 64: TESR_LUTData (the LUT's values; zero for the grade alone)
    vec4 lutBlend;      // 80: TESR_LUTBlend
    uint samplerIndex;  // 96: LINEAR, CLAMP
    uint lutPass;       // 100: 1 = ColoringLUT
} pc;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o_color;

// a sampler from the heap at point of use (GLSL rejects a sampler built outside the call)
#define COL_SAMPLE(t, p) texture(sampler2D(t, NVR_SAMPLER(pc.samplerIndex)), p)

float sCurve(float x, float k) {
    if (abs(k) < 1e-3) return x;
    float s = 1.0 / (1.0 + exp(-k * (x - 0.5)));
    float y0 = 1.0 / (1.0 + exp(k * 0.5));
    float y1 = 1.0 / (1.0 + exp(-k * 0.5));
    return (s - y0) / (y1 - y0);
}

vec3 overlay(vec3 base, float blend) {
    vec3 dark = 2.0 * base * blend;
    vec3 light = 1.0 - 2.0 * (1.0 - base) * (1.0 - blend);
    return mix(light, dark, lessThanEqual(base, vec3(0.5)));
}

vec3 splitTone(vec3 c, vec3 mask) {
    vec3 t = clamp(mask, 0.0, 1.0);
    vec3 a = abs(c);
    vec3 darkAmount = clamp((0.5 - t) * 2.0, 0.0, 1.0);
    vec3 brightAmount = clamp((t - 0.5) * 2.0, 0.0, 1.0);
    vec3 mag = mix(mix(a, a * a, darkAmount), sqrt(a), brightAmount);
    return sign(c) * mag;
}

vec3 grade(vec3 picture) {
    float strength = pc.data.x;
    float baseGamma = pc.data.y;
    float fade = clamp(pc.data.z, 0.0, 1.0);
    float contrast = pc.data.w;
    float saturation = pc.values.x;
    float bleach = pc.values.y;
    float bleachLuma = pc.values.z;
    float linearization = max(pc.values.w, 0.01);

    // 1. Overlay with its own luma, weighted by BleachLuma in the highlights
    float l = luma(picture);
    vec3 overlaid = min(overlay(picture, l), vec3(65504.0));
    vec3 c = mix(picture, overlaid, bleachLuma * smoothstep(0.45, 1.0, l));

    // 2. Linearization
    c = pows(c, linearization);

    // 3. Contrast
    c = mix(vec3(0.01), c, contrast);

    // 4. Tone mask
    float lum = max(dot(c, vec3(-0.9689, 1.8758, 0.0415)), 0.0);
    vec3 curves = pc.colorCurve.yzw;
    vec3 mask = vec3(sCurve(lum, curves.x), sCurve(lum, curves.y), sCurve(lum, curves.z));
    mask = pow(clamp(mask, 0.0, 1.0), vec3(1.0 / max(pc.effectGamma.x, 0.01)));
    mask = mix(mask, 1.0 - mask, bleach);
    mask = pow(clamp(mask, 0.0, 1.0), 1.0 / max(pc.effectGamma.yzw, vec3(0.01)));

    // 5. Base gamma
    c = pows(c, 1.0 / max(baseGamma, 0.01));
    vec3 beforeSplit = c;

    // 6. Split tone, blended by Strength
    c = mix(c, splitTone(c, mask), strength);

    // 7. Whole-grade S-curve
    float k = pc.colorCurve.x;
    c = vec3(sCurve(c.r, k), sCurve(c.g, k), sCurve(c.b, k));

    // 8. Channel mixing
    float meanC = (c.r + c.g + c.b) / 3.0;
    c = mix(c, vec3(meanC), saturation);
    float greenBlue = 0.5 * (c.g + c.b);
    c.g = mix(c.g, greenBlue, fade);
    c.b = mix(c.b, greenBlue, fade);

    // 9. Second split tone, masked by the mixed picture's luma, then the Linearization power back
    float mixedLuma = luma(c);
    vec3 split = splitTone(c, vec3(mixedLuma));
    vec3 relinearized = pows(split, 1.0 / linearization);

    // 10. Output
    return mix(beforeSplit, relinearized, strength);
}

vec3 lutStrip(texture2D table, float cells, vec3 c) {
    float b = c.b * (cells - 1.0);
    float cellLo = floor(b);
    float cellHi = min(cellLo + 1.0, cells - 1.0);
    float blendB = b - cellLo;
    vec2 inCell = vec2(c.r * (cells - 1.0), c.g * (cells - 1.0));
    vec2 uvLo = vec2((cellLo * cells + 0.5 + inCell.x) / (cells * cells), (0.5 + inCell.y) / cells);
    vec2 uvHi = vec2((cellHi * cells + 0.5 + inCell.x) / (cells * cells), (0.5 + inCell.y) / cells);
    return mix(COL_SAMPLE(table, uvLo).rgb, COL_SAMPLE(table, uvHi).rgb, blendB);
}

vec3 lutGradeWith(texture2D table, float cells, vec3 c) {
    return (cells >= 2.0) ? lutStrip(table, cells, clamp(c, vec3(0.0), vec3(1.0))) : c;
}

vec3 lutGrade(vec3 col) {
    float scale = (pc.lutBlend.z > 0.5) ? max(1.0, max(col.r, max(col.g, col.b))) : 1.0;
    vec3 lookup = col / scale;
    vec3 day = lutGradeWith(t_day, pc.lutData.x, lookup);
    vec3 night = lutGradeWith(t_night, pc.lutData.z, lookup);
    vec3 interior = lutGradeWith(t_interior, pc.lutData.w, lookup);
    vec3 exterior = mix(night, day, pc.lutBlend.x);
    vec3 graded = mix(exterior, interior, pc.lutBlend.y) * scale;
    return mix(col, graded, pc.lutData.y);
}

void main() {
    vec3 col = grade(COL_SAMPLE(t_rendered, uv).rgb);
    if (pc.lutPass == 1u) col = lutGrade(col);
    o_color = vec4(col, 1.0);
}
