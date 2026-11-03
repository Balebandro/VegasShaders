// Coloring as a Vulkan pass (needs NVR DXVK): Effects/Coloring.fx.hlsl line for line. effects/Coloring.cpp ExtRender.
// lutPass 0 is the technique Coloring (ColorPass); lutPass 1 is ColoringLUT (ColorLUTPass): the colouring, then the LUT
// (LUT.frag's SampleLUT and LUTGrade) in the same pass. The LUT textures are only read when lutPass is 1.
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "Common.glsl"

layout(set = 1, binding = 0) uniform texture2D t_rendered;   // TESR_RenderedBuffer
layout(set = 1, binding = 1) uniform texture2D t_day;        // TESR_LUTDayBuffer (ColoringLUT only)
layout(set = 1, binding = 2) uniform texture2D t_night;      // TESR_LUTNightBuffer (ColoringLUT only)
layout(set = 1, binding = 3) uniform texture2D t_interior;   // TESR_LUTInteriorBuffer (ColoringLUT only)

layout(push_constant) uniform Push {
    vec4 colorCurve;     // 0: TESR_ColoringColorCurve
    vec4 effectGamma;    // 16: TESR_ColoringEffectGamma
    vec4 coloringData;   // 32: TESR_ColoringData
    vec4 coloringValues; // 48: TESR_ColoringValues
    vec4 lutData;        // 64: TESR_LUTData (x day N, y strength, z night N, w interior N)
    vec4 lutBlend;       // 80: TESR_LUTBlend (x day/night, y interior, z hdr compat)
    uint samplerIndex;   // 96: LINEAR, CLAMP (no mips)
    uint lutPass;        // 100: 1 = ColoringLUT, 0 = Coloring
} pc;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o_color;

// XYZ_RGB[1] of the .fx.hlsl: the row GetLuminance takes the dot product with
const vec3 xyzGreenRow = vec3(0.212671, 0.715160, 0.072169);

float getLuminance(vec3 rgb) { return dot(xyzGreenRow, rgb); }

// the S-curve, normalised so 0 and 1 stay put; k = 0 is no change
float coloringCurve(float x, float k) {
    float y0 = 1.0 / (1.0 + exp(k / 2.0));
    return abs(k) < 1e-4 ? x : (1.0 / (1.0 + exp(-k * (x - 0.5))) - y0) / (1.0 - 2.0 * y0);
}

vec3 colorGrade(vec3 color) {
    vec3 H = vec3(0.01);

    vec3 lumCoeff = vec3(0.2126, 0.7152, 0.0722);
    float lum = clamp(dot(lumCoeff, color), 0.0, 1.0);
    float L = clamp(10.0 * (lum - 0.45), 0.0, 1.0);
    vec3 result1 = min(2.0 * color * lum, vec3(65504.0));
    vec3 result2 = 1.0 - 2.0 * (1.0 - lum) * (1.0 - color);
    vec3 newColor = mix(result1, result2, L);
    color = mix(color, newColor, pc.coloringValues.z);

    color = pows(color, pc.coloringValues.w);
    color = mix(H, color, pc.coloringData.w);

    vec3 luminance = vec3(getLuminance(color));
    luminance.r = coloringCurve(luminance.r, pc.colorCurve.y);
    luminance.g = coloringCurve(luminance.g, pc.colorCurve.z);
    luminance.b = coloringCurve(luminance.b, pc.colorCurve.w);
    luminance = pow(max(luminance, vec3(0.0)), vec3(1.0 / pc.effectGamma.x));

    vec3 luminancei = 1.0 - luminance;
    luminance = mix(luminance, luminancei, pc.coloringValues.y);
    luminance.r = pow(abs(luminance.r), 1.0 / pc.effectGamma.y);
    luminance.g = pow(abs(luminance.g), 1.0 / pc.effectGamma.z);
    luminance.b = pow(abs(luminance.b), 1.0 / pc.effectGamma.w);

    color = pows(color, 1.0 / pc.coloringData.y);
    vec3 color2 = color;

    if (luminance.r < 0.5)
        color2.r = (2.0 * luminance.r - 1.0) * (color.r - color.r * color.r) + color.r;
    else
        color2.r = (2.0 * luminance.r - 1.0) * (sqrt(max(color.r, 0.0)) - color.r) + color.r;

    if (luminance.g < 0.5)
        color2.g = (2.0 * luminance.g - 1.0) * (color.g - color.g * color.g) + color.g;
    else
        color2.g = (2.0 * luminance.g - 1.0) * (sqrt(max(color.g, 0.0)) - color.g) + color.g;

    if (luminance.b < 0.5)
        color2.b = (2.0 * luminance.b - 1.0) * (color.b - color.b * color.b) + color.b;
    else
        color2.b = (2.0 * luminance.b - 1.0) * (sqrt(max(color.b, 0.0)) - color.b) + color.b;

    vec3 color3 = mix(color, color2, pc.coloringData.x);
    color3 = vec3(coloringCurve(color3.r, pc.colorCurve.x), coloringCurve(color3.g, pc.colorCurve.x), coloringCurve(color3.b, pc.colorCurve.x));

    float r2R = 1.0 - pc.coloringValues.x;
    float g2R = 0.0 + pc.coloringValues.x;
    float b2R = 0.0 + pc.coloringValues.x;

    float r2G = 0.0 + pc.coloringValues.x;
    float g2G = (1.0 - pc.coloringData.z) - pc.coloringValues.x;
    float b2G = (0.0 + pc.coloringData.z) + pc.coloringValues.x;

    float r2B = 0.0 + pc.coloringValues.x;
    float g2B = (0.0 + pc.coloringData.z) + pc.coloringValues.x;
    float b2B = (1.0 - pc.coloringData.z) - pc.coloringValues.x;

    vec3 mixIn = color3;
    color3.r = (mixIn.r * r2R + mixIn.g * g2R + mixIn.b * b2R);
    color3.g = (mixIn.r * r2G + mixIn.g * g2G + mixIn.b * b2G);
    color3.b = (mixIn.r * r2B + mixIn.g * g2B + mixIn.b * b2B);

    float luminance2 = getLuminance(color3);
    vec3 color4 = color3;

    if (luminance2 < 0.5)
        color4 = (2.0 * luminance2 - 1.0) * (color3 - color3 * color3) + color3;
    else
        color4 = (2.0 * luminance2 - 1.0) * (sqrt(max(color3, vec3(0.0))) - color3) + color3;

    color4 = pows(color4, 1.0 / pc.coloringValues.w);

    return mix(color, color4, pc.coloringData.x);
}

// Sample a horizontal-strip LUT texture (LUT.frag's SampleLUT). N below 2: the colour passes through.
vec3 sampleLUT(texture2D lut, vec3 color, float slotN) {
    float N = max(slotN, 2.0);
    float b = color.b * (N - 1.0);
    float bCell = floor(b);
    float bFrac = fract(b);

    float invW = 1.0 / (N * N);
    float invH = 1.0 / N;

    vec2 uv1, uv2;
    uv1.x = (bCell * N + color.r * (N - 1.0) + 0.5) * invW;
    uv1.y = (color.g * (N - 1.0) + 0.5) * invH;
    uv2.x = ((bCell + 1.0) * N + color.r * (N - 1.0) + 0.5) * invW;
    uv2.y = uv1.y;

    vec3 graded = mix(texture(sampler2D(lut, NVR_SAMPLER(pc.samplerIndex)), uv1).rgb, texture(sampler2D(lut, NVR_SAMPLER(pc.samplerIndex)), uv2).rgb, bFrac);
    return slotN >= 2.0 ? graded : color;
}

vec3 lutGrade(vec3 color) {
    float strength = pc.lutData.y;
    float hdrCompat = pc.lutBlend.z;

    float scale = max(max(color.r, color.g), max(color.b, 1.0));
    vec3 lutInput = color / mix(1.0, scale, hdrCompat);

    vec3 dayColor = sampleLUT(t_day, lutInput, pc.lutData.x);
    vec3 nightColor = sampleLUT(t_night, lutInput, pc.lutData.z);
    vec3 interiorColor = sampleLUT(t_interior, lutInput, pc.lutData.w);

    vec3 exteriorColor = mix(nightColor, dayColor, pc.lutBlend.x);
    vec3 graded = mix(exteriorColor, interiorColor, pc.lutBlend.y);

    graded *= mix(1.0, scale, hdrCompat);

    return mix(color, graded, strength);
}

void main() {
    vec3 graded = colorGrade(texture(sampler2D(t_rendered, NVR_SAMPLER(pc.samplerIndex)), uv).rgb);
    if (pc.lutPass != 0u) graded = lutGrade(graded);
    o_color = vec4(graded, 1.0);
}
