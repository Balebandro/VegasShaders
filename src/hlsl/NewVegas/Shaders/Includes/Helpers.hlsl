// Small maths helpers and debug colours for the object/game shaders (the Effects folder has its own copy for the post effects).
// Guarded so SkyAmbient.hlsl can pull it in for Sky.hlsl's linearize() whatever order the shader includes things in.
#ifndef HELPERS_INCLUDED
#define HELPERS_INCLUDED

#define	expand(x)	        (((x) - 0.5) / 0.5)	// [0, 1] -> [-1, 1]
#define	compress(x)         (((x) * 0.5) + 0.5)	// [-1, 1] -> [0, 1]
#define	shade(a, b)         max(dot(a, b), 0)	// clamped cosine
#define	shades(a, b)        saturate(dot(a, b))	// clamped cosine, also capped at 1
#define invlerp(lo, hi, x)  ((x-lo)/(hi-lo))	// where x sits between lo and hi
#define invlerps(lo, hi, x) saturate((x-lo)/(hi-lo))
#define luma(c)             dot(c.rgb, float3(0.2126f, 0.7152f, 0.0722f))	// Rec.709 luminance
#define mix(x, y)           x * y * 2	// overlay-style product that keeps mid-grey neutral (no outer brackets: callers rely on the plain expansion)
#define	weight(x)           dot(x, 1)	// sum of the components
#define	sqr(x)              ((x) * (x))
#define blendnormals(n1, n2) float3(n1.xy + n2.xy, n1.z)	// add two tangent-space normals' slopes
#define rand(p)             (frac(sin(dot(p, float2(12.9898, 78.233))) * 43758.5453))	// the usual sine hash
#define pows(x, e)          (pow(abs(x), e) * sign(x))	// signed power, no negative-base warning
#define reconstructZ(n)     (normalize(float3(n.xy, sqrt(1 - saturate(dot(n.xy, n.xy))))))	// rebuild z of a two-channel normal
#define bend(x, k)          (x * (1 + k) / (1 + x * k))	// symmetric curve bend
#define scaledReinhard(x, k) ((x * k) / (1 + x * k))	// rises to 1 at speed k (UNOFFICIAL 2026-10-16: the upstream bracket mismatch fixed)

#define PI 3.1415926538

// debug colours
#define white     float4(1.0, 1.0, 1.0, 1.0)
#define grey      float4(0.5, 0.5, 0.5, 1.0)
#define black     float4(0.0, 0.0, 0.0, 1.0)
#define red       float4(1.0, 0.0, 0.0, 1.0)
#define green     float4(0.0, 1.0, 0.0, 1.0)
#define blue      float4(0.0, 0.0, 1.0, 1.0)
#define yellow    float4(1.0, 1.0, 0.0, 1.0)
#define cyan      float4(0.0, 1.0, 1.0, 1.0)
#define magenta   float4(1.0, 0.0, 1.0, 1.0)

// sRGB transfer curve, both ways (the exact piecewise form, not a gamma 2.2 power).
float3 linearize(float3 srgb) {
    float3 toe = srgb / 12.92;
    float3 curve = pow(abs((srgb + 0.055) / 1.055), 2.4);	// abs is free; the select below only takes this side for values > 0.04
    return (srgb <= 0.04045) ? toe : curve;
}
float4 linearize(float4 srgb) {
    float3 toe = srgb.rgb / 12.92;
    float3 curve = pow(abs((srgb.rgb + 0.055) / 1.055), 2.4);
    return float4((srgb.rgb <= 0.04045) ? toe : curve, srgb.a);
}
float3 delinearize(float3 lin) {
    float3 toe = lin * 12.92;
    float3 curve = (pow(abs(lin), 1.0 / 2.4) * 1.055) - 0.055;
    return (lin <= 0.0031308) ? toe : curve;
}
float4 delinearize(float4 lin) {
    float3 toe = lin.rgb * 12.92;
    float3 curve = (pow(abs(lin.rgb), 1.0 / 2.4) * 1.055) - 0.055;
    return float4((lin.rgb <= 0.0031308) ? toe : curve, lin.a);
}

// Debug picker: 0 gives the first colour, [0.1, 0.2) the second ... [0.9, 1.0) the tenth, anything else black.
#define HELPERS_PICK(lo, hi, c) if (selector >= lo && selector < hi) return c;
float4 selectColor(float selector, float4 c0, float4 c1, float4 c2, float4 c3, float4 c4, float4 c5, float4 c6, float4 c7, float4 c8, float4 c9){
    if (selector == 0.0) return c0;
    HELPERS_PICK(0.1, 0.2, c1) HELPERS_PICK(0.2, 0.3, c2) HELPERS_PICK(0.3, 0.4, c3)
    HELPERS_PICK(0.4, 0.5, c4) HELPERS_PICK(0.5, 0.6, c5) HELPERS_PICK(0.6, 0.7, c6)
    HELPERS_PICK(0.7, 0.8, c7) HELPERS_PICK(0.8, 0.9, c8) HELPERS_PICK(0.9, 1.0, c9)
    return black;
}

float3 selectColor(float selector, float3 c0, float3 c1, float3 c2, float3 c3, float3 c4, float3 c5, float3 c6, float3 c7, float3 c8, float3 c9){
    if (selector == 0.0) return c0;
    HELPERS_PICK(0.1, 0.2, c1) HELPERS_PICK(0.2, 0.3, c2) HELPERS_PICK(0.3, 0.4, c3)
    HELPERS_PICK(0.4, 0.5, c4) HELPERS_PICK(0.5, 0.6, c5) HELPERS_PICK(0.6, 0.7, c6)
    HELPERS_PICK(0.7, 0.8, c7) HELPERS_PICK(0.8, 0.9, c8) HELPERS_PICK(0.9, 1.0, c9)
    return black.rgb;
}
#undef HELPERS_PICK

#endif
