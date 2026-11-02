// UNOFFICIAL 2026-10-18 NewGrass pixel shader (core/NewGrass.h; pairs with NewGrass.vso.hlsl). Opaque blades: no alpha test, no
// blend, so early-Z works and the game's MSAA smooths the edges. Lit as NVR's grass is (GRASS23x000TMS.pso: sun, ambient, sky
// ambient, NVR's linear-lighting rules), with the sun shadowed by the cascades (FORWARD_SHADOWS; NEWGRASS_SHADOWS 0 = the variant
// without the lookups, picked live by [Shaders.Grass.NewGrass] ReceiveShadows). The blade's colour comes from the grass type's own
// card texture (a 4-step ramp root -> tip, alpha-weighted means read once per texture by NewGrass.h), so regions and mods match.
//
// Constants: Shadow.hlsl / PBRScale.hlsl by name (c100-c133, c134-c145, c191); ours at c20-c24 (free: the game's grass PS reads c0-c3).

#include "includes/Shadow.hlsl"
#include "includes/PBRScale.hlsl"

#ifndef NEWGRASS_SHADOWS
    #define NEWGRASS_SHADOWS 1
#endif

float4 NG_Ramp0 : register(c20);   // root colour (as stored in the texture, gamma)
float4 NG_Ramp1 : register(c21);
float4 NG_Ramp2 : register(c22);
float4 NG_Ramp3 : register(c23);   // tip colour
float4 NG_PS    : register(c24);   // x shadow lift (units above the blade the cascade lookup is taken: no acne from the land), y light wrap, z tip lift, w per-blade brightness variation
float4 NG_PS2   : register(c25);   // x NormalBlend (0 the blade's normal, 1 the ground normal the cards are lit with), y SkyLight (NVR's sky ambient share; the cards get none), z GameLighting (1 = lit exactly as the game's own grass pixel shader lights the cards)

struct PS_INPUT {
    float4 clip     : TEXCOORD0_centroid;
    float4 params   : TEXCOORD1_centroid;   // x height along the blade, y side, z tint seed, w the tuft's light scale
    float4 normal   : TEXCOORD2_centroid;   // xyz world normal, w grass dimmer
    float4 sunDir   : TEXCOORD3;
    float4 ambient  : TEXCOORD4_centroid;
    float4 sun      : TEXCOORD5_centroid;
    float4 up       : TEXCOORD6_centroid;   // xyz the tuft's ground normal
    float4 fog      : COLOR0;
};

struct PS_OUTPUT {
    float4 color : COLOR0;
};

PS_OUTPUT main(PS_INPUT IN) {
    PS_OUTPUT OUT;

    float t = saturate(IN.params.x);
    float s = t * 3.0f;
    float3 albedo = s < 1.0f ? lerp(NG_Ramp0.rgb, NG_Ramp1.rgb, s)
                  : s < 2.0f ? lerp(NG_Ramp1.rgb, NG_Ramp2.rgb, s - 1.0f)
                             : lerp(NG_Ramp2.rgb, NG_Ramp3.rgb, s - 2.0f);
    albedo *= 1.0f + (IN.params.z - 0.5f) * 2.0f * NG_PS.w;                 // each blade a little lighter or darker
    albedo += (NG_PS.z * t * t * t) * float3(0.10f, 0.08f, 0.0f);            // a warm lift toward the tip

    // Camera-relative world position (the frame every NVR shadow and sky term expects), the blade's normal facing the eye.
    float3 camRel = GetShadowWorldPos(IN.clip);
    float3 N = normalize(IN.normal.xyz);
    N = dot(N, -camRel) < 0.0f ? -N : N;

    // The sun term: the cards are lit by their ground normal (about up: every card in full sun at noon); a blade's own normal faces
    // sideways and would get half of that. Blend toward the ground normal (NormalBlend), then a little wrap (thin leaves let light round).
    float3 Nlit = normalize(lerp(N, normalize(IN.up.xyz), NG_PS2.x));
    float ndl = saturate((dot(Nlit, IN.sunDir.xyz) + NG_PS.y) / (1.0f + NG_PS.y));
    float sunAmount = ndl * IN.params.w * IN.normal.w;
#if FORWARD_SHADOWS && NEWGRASS_SHADOWS
    // Looked up a little above the blade: the land is a caster in the cascades at its own height, so a point on a blade's foot is in it.
    sunAmount *= GetSunShadow(camRel + float3(0.0f, 0.0f, NG_PS.x), N);
#endif

    // As GRASS23x000TMS.pso: the colours decoded (PBRLight / PBRAmbient), the plain amounts outside; the sky's light by the normal only
    // as much as asked (the game's card path has none of it).
    float3 lighting = PBRLight(IN.sun.rgb) * sunAmount + PBRAmbient(IN.ambient.rgb) * IN.params.w + SkyAmbient(N, 1.0f) * NG_PS2.y;
    float3 lit = LinEncode(lighting * LinColor(albedo));
    // GameLighting (round 6): the cards are drawn by the game's own grass pixel shader, (ambient x light scale + sun x NdotL x dimmer x
    // light scale) x texture in gamma space, no PBR scales and no sky term; lit the same way the strands take the bush's own colour.
    float3 litGame = (IN.ambient.rgb * IN.params.w + IN.sun.rgb * sunAmount) * albedo;
    lit = NG_PS2.z > 0.5f ? litGame : lit;

    OUT.color.rgb = lerp(lit, IN.fog.rgb, IN.fog.w);
    OUT.color.a = 1.0f;
    return OUT;
};
