// UNOFFICIAL 2026-10-27 displacement shadows ([Shaders.Terrain.Displacement] CastShadows, core/DisplaceShadows.h): the pixel shader of
// the land drawn tessellated into the sun's shadow cascades (ShadowMapLand.vso). Loaded as ShadowMapLand-t1.pso .. -t7.pso, one per
// land layer count (TEX_COUNT), like the land's own pixel shaders.
//
// It writes what ShadowMapOpaque.pso writes with the PCF maps (SHADOW_FIXED_MODE 3, always): the depth, from the shadow map's clip
// position that the tessellation evaluation shader moved with the bumps (TEXCOORD6).
//
// The land's base maps (height in alpha) at s0.. and, below 7 layers, last frame's depth copy at s6 are here only for that
// tessellation shader: NVR DXVK 1.7-displace lets a TES read only textures its pixel shader uses (s0-s7). The branch that reads them
// is never taken (the clip w of the shadow map's projection is 1), but it keeps them in this shader's samplers.

#ifndef TEX_COUNT
    #define TEX_COUNT 1
#endif

sampler2D BaseMap[TEX_COUNT] : register(s0);
#if TEX_COUNT < 7
sampler2D DisplaceDepth : register(s6);
#endif

// UNOFFICIAL 2026-10-31 ShadowSlack ([Shaders.Terrain.Displacement], core/DisplaceShadows.h): x the depth pushed onto the displaced land, away
// from the sun (this cascade's depth units for ShadowSlack world units); 0 = as before.
float4 LandShadowSlack : register(c0);

struct PS_INPUT {
    float4 projectionPosition : TEXCOORD6;
};

float4 main(PS_INPUT IN) : COLOR0 {
    float4 color = float4(IN.projectionPosition.z / IN.projectionPosition.w + LandShadowSlack.x, 0.0f, 0.0f, 1.0f);
    [branch] if (IN.projectionPosition.w < -1.0e30f) {
        float unused = 0.0f;
        [unroll] for (int i = 0; i < TEX_COUNT; i++) unused += tex2Dlod(BaseMap[i], float4(0.5f, 0.5f, 0.0f, 0.0f)).a;
#if TEX_COUNT < 7
        unused += tex2Dlod(DisplaceDepth, float4(0.5f, 0.5f, 0.0f, 0.0f)).x;
#endif
        color.y += unused;
    }
    return color;
}
