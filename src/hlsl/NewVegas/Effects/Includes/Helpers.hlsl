// Small shared helpers for the post-process effects: range remaps, luma, colour constants,
// sRGB <-> linear conversion and a ten-way colour picker.

// remap between [0,1] and [-1,1]
#define	expand(x)	        (((x) - 0.5f) / 0.5f)
#define	compress(x)         (((x) * 0.5f) + 0.5f)

// clamped and saturated dot products
#define	shade(nrm, lgt)     max(dot(nrm, lgt), 0)
#define	shades(nrm, lgt)    saturate(dot(nrm, lgt))

// where t sits between lo and hi
#define invlerp(lo, hi, t)   ((t-lo)/(hi-lo))
#define invlerps(lo, hi, t)  saturate((t-lo)/(hi-lo))

// luminance, BT.709 weights
#define luma(rgbx)          dot(rgbx.rgb, float3(0.2126f, 0.7152f, 0.0722f))
// BT.601 weights, unused:
// #define luma(rgbx)          dot(rgbx, float3(0.3f, 0.59f, 0.11f))

// brighten-free colour product
#define mix(ca, cb)         ca * cb * 2
#define	weight(x)           dot(x, 1)
#define	sqr(x)              ((x) * (x))
#define blendnormals(p, q)  float3(p.xy + q.xy, p.z)
// hash of a 2D seed in [0,1)
#define rand(seed)           (frac(sin(dot(seed, float2(12.9898, 78.233))) * 43758.5453))
// pow that tolerates negative bases
#define pows(base, ex)       (pow(abs(base), ex) * sign(base))
// symmetric response curve with strength b
#define bend(a, b)           (a * (1 + b) / (1 + a * b))
// saturating curve, reaches 1 at speed b
#define scaledReinhard(a, b) ((a * b) / (1 + a * b))

#define PI 3.1415926538
#define white     float4(1.0, 1.0, 1.0, 1.0)
#define grey      float4(0.5, 0.5, 0.5, 1.0)
#define black     float4(0.0, 0.0, 0.0, 1.0)
#define red       float4(1.0, 0.0, 0.0, 1.0)
#define green     float4(0.0, 1.0, 0.0, 1.0)
#define blue      float4(0.0, 0.0, 1.0, 1.0)
#define yellow    float4(1.0, 1.0, 0.0, 1.0)
#define cyan      float4(0.0, 1.0, 1.0, 1.0)
#define magenta   float4(1.0, 0.0, 1.0, 1.0)

float3 linearize(float3 srgb) {
    float3 low = srgb / 12.92;
    float3 high = pow(abs((srgb + 0.055) / 1.055), 2.4);	// abs is free here: the branch that uses it has base > 0.09 (ends warning X3571)
    float3 lin = (srgb <= 0.04045) ? low : high;
    return lin;
}
float4 linearize(float4 srgba) {
    float3 low = srgba.rgb / 12.92;
    float3 high = pow(abs((srgba.rgb + 0.055) / 1.055), 2.4);
    float3 lin = (srgba.rgb <= 0.04045) ? low : high;
    return float4(lin, srgba.a);
}
float3 delinearize(float3 lin) {
    float3 low = lin * 12.92;
    float3 high = (pow(abs(lin), 1.0 / 2.4) * 1.055) - 0.055;
    float3 srgb = (lin <= 0.0031308) ? low : high;
    return srgb;
}
float4 delinearize(float4 lina) {
    float3 low = lina.rgb * 12.92;
    float3 high = (pow(abs(lina.rgb), 1.0 / 2.4) * 1.055) - 0.055;
    float3 srgb = (lina.rgb <= 0.0031308) ? low : high;
    return float4(srgb, lina.a);
}

// Pick one of ten colours by a 0..0.99 selector (tenths); anything else gives black.
float4 selectColor(float sel, float4 c0, float4 c1, float4 c2, float4 c3, float4 c4, float4 c5, float4 c6, float4 c7, float4 c8, float4 c9){
    if (sel == 0.0) return c0;
    if (sel >= 0.1 && sel < 0.2) return c1;
    if (sel >= 0.2 && sel < 0.3) return c2;
    if (sel >= 0.3 && sel < 0.4) return c3;
    if (sel >= 0.4 && sel < 0.5) return c4;
    if (sel >= 0.5 && sel < 0.6) return c5;
    if (sel >= 0.6 && sel < 0.7) return c6;
    if (sel >= 0.7 && sel < 0.8) return c7;
    if (sel >= 0.8 && sel < 0.9) return c8;
    if (sel >= 0.9 && sel < 1.0) return c9;
    return black;
}

float3 selectColor(float sel, float3 c0, float3 c1, float3 c2, float3 c3, float3 c4, float3 c5, float3 c6, float3 c7, float3 c8, float3 c9){
    if (sel == 0.0) return c0;
    if (sel >= 0.1 && sel < 0.2) return c1;
    if (sel >= 0.2 && sel < 0.3) return c2;
    if (sel >= 0.3 && sel < 0.4) return c3;
    if (sel >= 0.4 && sel < 0.5) return c4;
    if (sel >= 0.5 && sel < 0.6) return c5;
    if (sel >= 0.6 && sel < 0.7) return c6;
    if (sel >= 0.7 && sel < 0.8) return c7;
    if (sel >= 0.8 && sel < 0.9) return c8;
    if (sel >= 0.9 && sel < 1.0) return c9;
    return black.rgb;
}
