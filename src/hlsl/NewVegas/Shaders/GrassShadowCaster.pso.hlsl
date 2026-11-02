// UNOFFICIAL 2026-10-27 grass sun shadows ([Shaders.Grass.Main] CastShadows, core/GrassShadows.h): the pixel shader of the caster
// draws that put the grass tufts into the near sun shadow map (the grass vertex shaders compiled with GRASS_SHADOW_CASTER). Writes
// what ShadowMap.pso writes for PCF (Mode 3, the only mode this runs with): the depth in the shadow map's space.

sampler2D DiffuseMap : register(s0);   // the tuft's texture, bound by the game for its grass pass
float4 CasterParams : register(c0);    // x 1 = CastShadowsBlack (diagnostic): depth 0, nearer than anything, so the shadow is solid (set per draw by GrassShadows)

struct PS_INPUT {
    float2 uv    : TEXCOORD0;
    float4 light : TEXCOORD1;   // the position in the shadow map's space (w 1)
    float4 sun   : TEXCOORD5;   // .w = the grass distance fade (as the grass PS)
};

float4 main(PS_INPUT IN, float2 vPos : VPOS) : COLOR0 {
    // The cut-out: about where the grass PS's coverage (alpha x 1.75) and the game's alpha test turn a texel solid.
    clip(tex2D(DiffuseMap, IN.uv).a - 0.4f);
    // The tufts that thin out with distance thin out in the shadow too (the same dither as the grass PS, in shadow map pixels).
    float dither = frac(52.9829189f * frac(dot(vPos, float2(0.06711056f, 0.00583715f))));
    clip(IN.sun.w - dither);
    return float4(CasterParams.x > 0.5f ? 0.0f : IN.light.z, 0.0f, 0.0f, 1.0f);
}
