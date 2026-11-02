// UNOFFICIAL 2026-10-21 LampFalloff ([Shaders.PBR.LampFalloff], effects/PBR.cpp; included at the end of LinearLight.hlsl). P82 tester and
// user: outdoor lamps lit a flat, evenly bright disc with a hard edge ("full bright throughout the entire radius instead of naturally
// falling off"), next to the RTX Remix mod's lamps, which are bright by the lamp and dim smoothly over their reach.
//
// The game gives each lamp att = 1 - (d / r)^2 (Pointlights.hlsl vanillaAtt): flat near the lamp (0.75 at half its radius), then
// a steep drop to 0 at the radius. Two things made it flatter still with linear lighting on:
//   - FalloffMatch (LinearLight.hlsl LinFalloff) decodes att like a colour, so a lamp alone falls off as in the game -- but only in
//     the object shaders. The ground (TerrainTemplate.hlsl), skin (Skin.hlsl, SKIN2006) and hair (SM3*) used att as it is in linear
//     light, so encoded back their lamps came out att^(1/2.2): 0.88 at half the radius, 0.63 at 0.8, 0.47 at 0.9, then 0 -- the
//     flat disc with the hard edge on the ground. MatchEverywhere gives them the objects' decode.
//   - The game's shape itself. Exterior / Interior replace it with a physically shaped one: inverse-square light with a smooth
//     window to zero at the lamp's radius (Karis, "Real Shading in Unreal Engine 4", SIGGRAPH 2013 course, eq. 9),
//         shape = saturate(1 - (d/r)^4)^2 / (1 + (d / (Falloff r))^2)
//     the "+1" there (1 m^2 in his units) made a share of the lamp's radius (Falloff), so each lamp keeps the reach its maker gave it.
//     It is light, so it applies in linear light (encoded back by LinEncode) and is encoded once without linear lighting; the
//     DLL scales it (z) so that the lamp sends out as much light over its reach as the game's falloff does (see PBR.cpp
//     LampFalloffNorm): brighter right by the lamp, dimmer far from it, no edge. Where att is 0 (beyond the radius) it is 0 too,
//     so LampReachSkip, TrimLampPasses and the lamp scissor are unchanged.
//
// Compiled in only when one of the switches is on at startup (ShaderRecord::BuildDefines), so that otherwise every shader is exactly
// what it was: LAMP_FALLOFF_ON 1 = MatchEverywhere only, 2 = the shape too (Exterior or Interior; the interior lamp-shadow records
// get 1 or 0 without Interior, ShaderManager::LoadShader). Then live: x switches the shape per frame (outdoors / indoors), w
// MatchEverywhere; 0 returns exactly the old falloff.
// c205: free in every pixel shader that includes LinearLight.hlsl (object, parallax, land, skin, hair, leaves) and on every deep/ branch.
#ifndef LAMPFALLOFF_INCLUDED
#define LAMPFALLOFF_INCLUDED

#ifndef LAMP_FALLOFF_ON
    #define LAMP_FALLOFF_ON 0
#endif

#if LAMP_FALLOFF_ON
float4 TESR_LampFalloff : register(c205); // x: 1 = the physical shape this frame (Exterior outdoors, Interior indoors), y: 1 / Falloff^2,
                                          // z: its strength (the energy match x Brightness), w: 1 = MatchEverywhere (the ground, skin, hair)
#endif

// The old decode's exponent (LinFalloff): FalloffMatch's z, or the gamma; 1 = att as it is.
#ifdef LINEAR_LIGHTING
    #define LAMP_OLD_EXPONENT (TESR_LinearLighting.x > 1.0f ? (TESR_LinearLighting.z > 0.0f ? TESR_LinearLighting.z : TESR_LinearLighting.x) : 1.0f)
#else
    #define LAMP_OLD_EXPONENT 1.0f
#endif

#if LAMP_FALLOFF_ON >= 2
// The gamma the shape is encoded for (the display's, 2.2): with linear lighting on at gamma g the shaders light in x^g, so the
// shape goes in as shape^(g / 2.2) and comes out on screen as shape^(1 / 2.2) whatever the gamma (NightGamma, or 1 = off).
#ifdef LINEAR_LIGHTING
    #define LAMP_SHAPE_EXPONENT (max(TESR_LinearLighting.x, 1.0f) * (1.0f / 2.2f))
#else
    #define LAMP_SHAPE_EXPONENT (1.0f / 2.2f)
#endif

// att is the game's 1 - (d / r)^2, saturated: 0..1, so q, the window and the result are 0 or more.
float LampShape(float att) {
    const float q = 1.0f - att;                         // (d / r)^2
    float window = 1.0f - q * q;                        // 1 - (d / r)^4
    window *= window;
    return TESR_LampFalloff.z * window / (1.0f + q * TESR_LampFalloff.y);
}

// The shape on, or att raised to the old falloff's exponent. abs: free in ps_3_0 (a source modifier); both are 0 or more anyway, and
// it keeps the compiler's "pow(f, e) will not work for negative f" warning out of the log (one per shader).
float LampFalloffPow(float att, float oldExponent) {
    const bool shaped = TESR_LampFalloff.x > 0.0f;
    return pow(abs(shaped ? LampShape(att) : att), shaped ? LAMP_SHAPE_EXPONENT : oldExponent);
}
#endif

// Objects (Object.hlsl): the falloff they had (LinFalloff), or the shape.
float LampAtt(float att) {
#if LAMP_FALLOFF_ON >= 2
    return LampFalloffPow(att, LAMP_OLD_EXPONENT);
#else
    return LinFalloff(att);
#endif
}

// The ground, skin and hair: att as it is (as before), the objects' decode with MatchEverywhere, or the shape.
float LampAttOther(float att) {
#if LAMP_FALLOFF_ON >= 2
    return LampFalloffPow(att, TESR_LampFalloff.w > 0.0f ? LAMP_OLD_EXPONENT : 1.0f);
#elif LAMP_FALLOFF_ON
    return TESR_LampFalloff.w > 0.0f ? LinFalloff(att) : att;
#else
    return att;
#endif
}

#endif
