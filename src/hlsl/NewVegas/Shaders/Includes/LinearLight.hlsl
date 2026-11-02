// UNOFFICIAL 2026-10-14 linear lighting ([Shaders.PBR.Linear], PBR.cpp). The game lights in gamma space: it multiplies and
// adds the texture and light colours as they are stored, which are gamma-encoded (about x^(1/2.2)). Light does not add up
// like that, so lit sides fall off too fast into the shade, two lamps add up too dark, and coloured light tints too little.
// With it on, the colours (textures, lamps, sun, ambient, sky) are decoded to linear first (LinColor), the light is worked
// out on those, and the result is encoded back (LinEncode), so everything after it (fog, the frame, the tonemapper) gets
// gamma-space colour as before.
//
// The strength settings ([Shaders.PBR.*] LightingScale, AmbientScale, SkylightingScale and the terrain's) were set by eye on
// the gamma-space result, so they are decoded too (LinScale): a surface lit by one light alone, or by the ambient alone, comes
// out exactly as bright as before; only where lights mix, and along the falloff, does it change. The shaders that are not
// linear (skin, hair, grass, trees) read the same settings, so they keep matching.
//
// Light passes that add on top of an object already drawn (the extra lamp passes and the separate highlight pass) cannot
// read what is there, so each is encoded alone: their sum comes out a little brighter than one linear sum would.
// The passes that multiply (the lighting-only pass, later multiplied by the texture) are exact: (a * b)^g = a^g * b^g.
//
// Compiled in only when [Shaders.PBR.Linear] Enabled is on at startup (LINEAR_LIGHTING_ON, ShaderRecord::LoadShader; the
// template then defines LINEAR_LIGHTING before it includes Object.hlsl / Terrain.hlsl). Otherwise the functions below are
// empty and the shaders are exactly what they were. Compiled in, Gamma 1 (x 1 or less) also returns every colour unchanged.
// c191: clear of PointShadowForward's c174-c190, GunFX's c171-c173, terrain blending's c176-c177 and c196-c199.
#ifndef LINEARLIGHT_INCLUDED
#define LINEARLIGHT_INCLUDED

#ifdef LINEAR_LIGHTING
float4 TESR_LinearLighting : register(c191); // x: gamma the colours are decoded with (1 or less = off), y: 1 / x

float3 LinColor(float3 c) { return TESR_LinearLighting.x > 1.0f ? pow(max(c, 0.0f), TESR_LinearLighting.x) : c; }
float3 LinEncode(float3 c) { return TESR_LinearLighting.x > 1.0f ? pow(max(c, 0.0f), TESR_LinearLighting.y) : c; }
float LinScale(float s) { return TESR_LinearLighting.x > 1.0f ? pow(max(s, 0.0f), TESR_LinearLighting.x) : s; }
// UNOFFICIAL 2026-10-20 (user: 'interiors look incredibly bright'; Rivet City's hangar): a lamp's distance falloff (vanillaAtt, 1 at
// the lamp, 0 at its radius) was made for the game's gamma-space light. Multiplied into the linear light and encoded back, a lamp
// alone came out att^(1/2.2) bright instead of att: 2x at att 0.25, 3.5x at 0.1 -- big rooms with wide lamps lit almost evenly.
// Decoded like the colours, a lamp alone falls off exactly as in the game again; lamps still add up in linear light.
// z: the exponent the falloff is decoded with ([Shaders.PBR.Linear] FalloffMatch: 1 + (gamma - 1) x match); 0 (not written) = gamma.
float LinFalloff(float a) {
    const float k = TESR_LinearLighting.z > 0.0f ? TESR_LinearLighting.z : TESR_LinearLighting.x;
    return TESR_LinearLighting.x > 1.0f ? pow(max(a, 0.0f), k) : a;
}
#else
float3 LinColor(float3 c) { return c; }
float3 LinEncode(float3 c) { return c; }
float LinScale(float s) { return s; }
float LinFalloff(float a) { return a; }
#endif

// UNOFFICIAL 2026-10-28 LinearScene ([Shaders.PBR.Linear] LinearScene, core/LinearScene.h, docs/next/LINEAR-SCENE-PLAN.md): while NVR keeps
// the world's frame in linear light (TESR_LinearLighting.w 1, set per draw), the pass's finished colour -- exactly what it gave before,
// fog and all -- is decoded, so the game's own blends add the passes in linear light; one step encodes the frame again. w 0: unchanged.
#if defined(LINEAR_LIGHTING) && defined(LINEAR_SCENE_ON) && LINEAR_SCENE_ON
float3 LinearSceneOut(float3 c) { return TESR_LinearLighting.w > 0.5f ? pow(max(c, 0.0f), TESR_LinearLighting.x) : c; }
#else
float3 LinearSceneOut(float3 c) { return c; }
#endif

// UNOFFICIAL 2026-10-21 LampFalloff: LampAtt (objects) and LampAttOther (the ground, skin, hair), with or without linear lighting.
#if defined(__INTELLISENSE__)
    #include "LampFalloff.hlsl"
#else
    #include "includes/LampFalloff.hlsl"
#endif

#endif
