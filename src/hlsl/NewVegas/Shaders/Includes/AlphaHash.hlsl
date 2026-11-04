// UNOFFICIAL 2026-11-03 SmoothCutouts ([Shaders.TAA.Cutouts], core/AlphaHash.h): smooth edges for alpha-tested cut-outs (leaves, grass,
// fences, chain-link, hair cards) while TAA / FSR / DLSS anti-alias the world. Their own transparency multisampling (alpha to coverage)
// only works with the game's MSAA, which is off with them, so every texel was drawn fully or not at all: jagged cut edges, and the
// see-through fringe of a texture made for alpha to coverage drawn solid (a tester's hair: a dark outline round the head).
//
// Hashed alpha test: each pixel keeps the texel with a probability (its coverage) against noise that changes per pixel and per frame;
// the jittered frames average it back into a partial coverage, as MSAA's samples did. Every pass of an object takes the same decision
// for a pixel (same alpha, same noise), so its lamp passes stay on the pixels its first pass drew.
//   Mode 1 edges: the cut stays where the game's test puts it, softened over about EdgeWidth pixels.
//   Mode 2 like the game's MSAA: coverage = the texel's alpha (what alpha to coverage gives; the look cut-out textures were made for).
// Compiled in only with ALPHA_HASH_ON (Enabled at startup); off, the shaders are exactly as before.

#ifndef ALPHA_HASH_INCLUDED
#define ALPHA_HASH_INCLUDED

bool NVR_AlphaHashDraw : register(b15);	// per draw (AlphaHash::ForPass): an alpha-tested, unblended object of the jittered world scene
float4 TESR_AlphaHash;	// x mode (0 off, 1 edges, 2 like MSAA), y this frame's noise offset (pixels), z edge width (pixels)

#if defined(NVR_SM4) && NVR_SM4
    #define ALPHA_HASH_VPOS_PARAM , float4 alphaHashVPos4 : VPOS	// the no-limits build: SV_Position, the pixel centre
    #define ALPHA_HASH_VPOS_VALUE (alphaHashVPos4.xy - 0.5f)
#else
    #define ALPHA_HASH_VPOS_PARAM , float2 alphaHashVPos : VPOS
    #define ALPHA_HASH_VPOS_VALUE alphaHashVPos
#endif

// 1 keeps the pixel, -1 drops it, with probability coverage of keeping it. Interleaved gradient noise (Jimenez 2014), moved each frame:
// a 3x3 neighbourhood holds both outcomes, so TAA's and FSR's history clamps keep the average instead of rejecting it as flicker.
// salt shifts the pattern (grass: away from its own distance-fade dither).
float AlphaHashKeep(float coverage, float2 vpos, float salt) {
    const float2 p = vpos + TESR_AlphaHash.y + salt;
    const float noise = frac(52.9829189f * frac(dot(p, float2(0.06711056f, 0.00583715f))));
    return coverage > noise ? 1.0f : -1.0f;
}

// The alpha the game's test should see: unchanged unless this draw is hashed; then 1 (kept: passes clip(a - ref) and the fixed-function
// test against the same reference) or -1 (dropped by the clip).
float AlphaHash(float alpha, float ref, float2 vpos) {
    const float width = max(fwidth(alpha) * TESR_AlphaHash.z, 1.0f / 255.0f);	// out of the branch: no derivatives inside one
    float result = alpha;
    if (NVR_AlphaHashDraw) {
        if (TESR_AlphaHash.x > 0.5f) {
            const float coverage = TESR_AlphaHash.x > 1.5f ? alpha : saturate((alpha - ref) / width + 0.5f);
            result = AlphaHashKeep(coverage, vpos, 0.0f);
        }
    }
    return result;
}

#endif
