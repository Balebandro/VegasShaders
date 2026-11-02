// Bloom's Vulkan passes (needs NVR DXVK): the same maths as Effects/Bloom.fx.hlsl, shared by the four pass files.
// The push block is the one effects/Bloom.cpp (BloomExtPush) fills: TESR_BloomData, TESR_BloomResolution, TESR_BloomGlow,
// TESR_BloomGlowExtra, then the LINEAR/CLAMP sampler's heap index. The chain has no RenderScale rect steps here.
#include "Common.glsl"

layout(push_constant) uniform Push {
    vec4 data;          // 0: TESR_BloomData (xy tent radius, z PassBlending)
    vec4 resolution;    // 16: TESR_BloomResolution (level written: xy size, zw 1 / size)
    vec4 glow;          // 32: TESR_BloomGlow (x spread, y last scale, z GlowBoost, w glow light level)
    vec4 glowExtra;     // 48: TESR_BloomGlowExtra (x AntiFlicker, y LinearLight)
    uint samplerIndex;  // 64: LINEAR, CLAMP
    uint pad[3];        // 68..79
} pc;

// a sampler from the heap at point of use (GLSL rejects a sampler built outside the call)
#define BLOOM_SAMPLE(t, p) texture(sampler2D(t, NVR_SAMPLER(pc.samplerIndex)), p)

vec3 toLinear(vec3 c) { c = max(c, vec3(0.0)); return c * c; }
vec3 toDisplay(vec3 c) { return sqrt(max(c, vec3(0.0))); }
float luma709(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }

// First pass only: GlowBoost on the bright taps, then display again unless LinearLight (the D3D9 GlowTap).
vec3 glowTap(vec3 c) {
    c = toLinear(c);
    float boost = pc.glow.z;
    if (boost > 0.0) {
        float threshold = max(pc.glow.w, 0.002);
        float peak = max(c.r, max(c.g, c.b));
        float low = min(c.r, min(c.g, c.b));
        float saturation = 1.0 - low / max(peak, 1e-6);
        float brightness = luma709(c) + (peak - luma709(c)) * saturation;
        float amount = clamp((brightness - threshold) / threshold, 0.0, 1.0);
        c *= 1.0 + boost * amount;
    }
    return (pc.glowExtra.y > 0.5) ? c : toDisplay(c);
}

// texel = 1 / the source level's size (the source is twice the size of the level written)
vec3 fetchTap(texture2D src, vec2 uv, vec2 texel, vec2 o, bool convert) {
    vec3 c = BLOOM_SAMPLE(src, uv + o * texel).rgb;
    return convert ? glowTap(c) : c;
}

vec3 downsample13(texture2D src, vec2 uv, bool first) {
    vec2 texel = 0.5 * pc.resolution.zw;
    bool convert = first && (pc.glow.z > 0.0 || pc.glowExtra.y > 0.5);
    vec3 a = fetchTap(src, uv, texel, vec2(-2.0,  2.0), convert);
    vec3 b = fetchTap(src, uv, texel, vec2( 0.0,  2.0), convert);
    vec3 c = fetchTap(src, uv, texel, vec2( 2.0,  2.0), convert);
    vec3 d = fetchTap(src, uv, texel, vec2(-2.0,  0.0), convert);
    vec3 e = fetchTap(src, uv, texel, vec2( 0.0,  0.0), convert);
    vec3 f = fetchTap(src, uv, texel, vec2( 2.0,  0.0), convert);
    vec3 g = fetchTap(src, uv, texel, vec2(-2.0, -2.0), convert);
    vec3 h = fetchTap(src, uv, texel, vec2( 0.0, -2.0), convert);
    vec3 i = fetchTap(src, uv, texel, vec2( 2.0, -2.0), convert);
    vec3 j = fetchTap(src, uv, texel, vec2(-1.0,  1.0), convert);
    vec3 k = fetchTap(src, uv, texel, vec2( 1.0,  1.0), convert);
    vec3 l = fetchTap(src, uv, texel, vec2(-1.0, -1.0), convert);
    vec3 m = fetchTap(src, uv, texel, vec2( 1.0, -1.0), convert);

    vec3 boxCentre = (j + k + l + m) * 0.25;
    vec3 boxTL = (a + b + d + e) * 0.25;
    vec3 boxTR = (b + c + e + f) * 0.25;
    vec3 boxBL = (d + e + g + h) * 0.25;
    vec3 boxBR = (e + f + h + i) * 0.25;

    if (first && pc.glowExtra.x > 0.5) {
        float wC  = 0.5   / (1.0 + max(luma709(boxCentre), 0.0));
        float wTL = 0.125 / (1.0 + max(luma709(boxTL), 0.0));
        float wTR = 0.125 / (1.0 + max(luma709(boxTR), 0.0));
        float wBL = 0.125 / (1.0 + max(luma709(boxBL), 0.0));
        float wBR = 0.125 / (1.0 + max(luma709(boxBR), 0.0));
        float wSum = wC + wTL + wTR + wBL + wBR;
        return (boxCentre * wC + boxTL * wTL + boxTR * wTR + boxBL * wBL + boxBR * wBR) / wSum;
    }
    return 0.5 * boxCentre + 0.125 * (boxTL + boxTR + boxBL + boxBR);
}

vec3 tent(texture2D src, vec2 uv, vec2 r) {
    vec3 sum = 4.0 * BLOOM_SAMPLE(src, uv).rgb;
    sum += 2.0 * BLOOM_SAMPLE(src, uv + vec2( r.x, 0.0)).rgb;
    sum += 2.0 * BLOOM_SAMPLE(src, uv + vec2(-r.x, 0.0)).rgb;
    sum += 2.0 * BLOOM_SAMPLE(src, uv + vec2(0.0,  r.y)).rgb;
    sum += 2.0 * BLOOM_SAMPLE(src, uv + vec2(0.0, -r.y)).rgb;
    sum += BLOOM_SAMPLE(src, uv + vec2( r.x,  r.y)).rgb;
    sum += BLOOM_SAMPLE(src, uv + vec2(-r.x,  r.y)).rgb;
    sum += BLOOM_SAMPLE(src, uv + vec2( r.x, -r.y)).rgb;
    sum += BLOOM_SAMPLE(src, uv + vec2(-r.x, -r.y)).rgb;
    return sum / 16.0;
}

vec3 upsample(texture2D above, texture2D own, vec2 uv, bool last) {
    vec3 up = tent(above, uv, pc.data.xy);
    vec3 self = BLOOM_SAMPLE(own, uv).rgb;
    bool blend = pc.data.z > 0.0;
    vec3 result = blend ? mix(self, up, pc.data.z) : self + pc.glow.x * up;
    if (last && !blend) result *= pc.glow.y;
    if (last && pc.glowExtra.y > 0.5) result = toDisplay(result);
    return result;
}
