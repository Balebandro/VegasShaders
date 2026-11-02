// UNOFFICIAL 2026-10-20 plant wind ([Shaders.Plants.Wind], core/PlantWind.h; OFF by default; bench\deep-roadmap\evidence\
// plantwind-LOG.md). The plants move in the grass field wind (Includes/GrassWind.hlsl, core/GrassWind.h): the same direction,
// strength source, clock and gust pattern -- fronts (0.5 + 0.5 sin(clock / 4 - P.K))^2 x a patch wave 45 degrees off the wind at
// 1.4x its length (0.75 + 0.25 sin), P the WORLD position -- so a gust rolls through the grass and the bushes together.
//
// Two parts:
// - PlantLeafGust / PlantLeafWind*: SpeedTree leaves (STLEAF001/003.vso; their sun shadow in Shadows\ShadowMap.vso gets the gust boost
//   and the lean, not the flutter). Global per
//   frame (registered constants): TESR_PlantLeafWind  xy the gust wave vector (radians per unit, world), z the grass wind's clock
//   (radians, 0..8 pi), w 1 = on (0: the callers run none of this -- the game's leaf motion exactly);
//   TESR_PlantLeafWind2 x flutter (radians), y lean (units at full height; 0 = none), z 1 / the lean's full height (model units),
//   w how much the gust's crest adds to the game's swing (x (1 + w));  TESR_PlantLeafWind3 xy the wind direction (world).
// - PlantWindMove: static plants (ObjectTemplate.hlsl, Shadows\ShadowMap.vso), compiled in only with PLANT_WIND_ON. Per draw, in the
//   cloth wind's registers c248-c253 (core/ClothWind.h; a plant draw is never a cloth draw), all in the object's own space p:
//   PlantA (c248) x -1 = a plant (else nothing runs), y the gust lean at full weight (world units), z the flutter (world units),
//                 w 1 / the plant's height H (its root's bound top over its base)
//   PlantB (c249) xyz the gust wave vector as dot(p, .), w its phase (clock / 4 - the object origin's share, wrapped on the CPU)
//   PlantC (c250) the same for the patch wave;  PlantD (c251) the same for the flutter wave (a short 3-D wave: the plant's parts
//                 quiver out of step), w 2 x clock + the plant's own random phase
//   PlantE (c252) xyz world height per object unit (s x the rotation's third row), w the object origin's height over the plant's base
//   PlantF (c253) xyz the wind direction in object units per world unit
//   Rooted: the weight is (h / H)^2, 0 at the base (and below), 1 at the top; the top drops as it leans (it bends, it does not
//   stretch), as the grass. Every pass of the plant and its shadow get the same numbers (core/PlantWind.h), so they agree.

#ifndef PLANT_WIND_ON
    #define PLANT_WIND_ON 0
#endif
#ifndef CLOTH_WIND_ON
    #define CLOTH_WIND_ON 0
#endif

// The gust at world position P (xy): 0 between fronts, 1 at a crest (the grass's front^2 x patch).
float PlantLeafGust(float2 P, float2 K, float clock) {
    const float gustPhase = clock * 0.25f;
    const float along = dot(P, K);
    const float front = 0.5f + 0.5f * sin(gustPhase - along);
    const float patch = 0.75f + 0.25f * sin(gustPhase - 0.5f * (along + P.y * K.x - P.x * K.y));
    return front * front * patch;
}

#if PLANT_WIND_ON
#ifdef CLOTH_WIND_DECLARED
    // the cloth's own declarations of c248-c253 (Includes/ClothWind.hlsl, included before this file). 2026-10-20: tested on the
    // marker ClothWind.hlsl defines, not on CLOTH_WIND_ON: the SpeedTree leaf shaders include this file without ClothWind.hlsl, and
    // with both switches on they failed to compile ('undeclared identifier ClothE'): the bush leaves were drawn without their vertex
    // shader, flat and flung about round the player (user's 13:20 screenshot).
    #define PlantA ClothA
    #define PlantB ClothB
    #define PlantC ClothC
    #define PlantD ClothD
    #define PlantE ClothE
    #define PlantF ClothF
#else
    float4 PlantA : register(c248);
    float4 PlantB : register(c249);
    float4 PlantC : register(c250);
    float4 PlantD : register(c251);
    float4 PlantE : register(c252);
    float4 PlantF : register(c253);
#endif

// The move of point p (object space, object units). Callers: [branch] if (PlantA.x < -0.5f) p += PlantWindMove(p).
float3 PlantWindMove(float3 p) {
    const float h = dot(p, PlantE.xyz) + PlantE.w;                      // world units above the plant's base
    float w = saturate(h * PlantA.w);
    w *= w;
    const float front = 0.5f + 0.5f * sin(PlantB.w - dot(p, PlantB.xyz));
    const float patch = 0.75f + 0.25f * sin(PlantC.w - dot(p, PlantC.xyz));
    float2 flutter;
    sincos(PlantD.w + dot(p, PlantD.xyz), flutter.x, flutter.y);
    const float along = (PlantA.y * front * front * patch + PlantA.z * flutter.x) * w;   // world units, along the wind
    const float across = PlantA.z * 0.5f * flutter.y * w;                                  // ...and across it
    const float s2 = dot(PlantE.xyz, PlantE.xyz);                                          // the object's scale squared
    const float3 side = cross(PlantE.xyz, PlantF.xyz) * rsqrt(s2);                        // (up x wind) in object units per world unit
    const float drop = min((along * along + across * across) * 0.5f / max(h, 1.0f), max(h, 0.0f));
    return PlantF.xyz * along + side * across - PlantE.xyz * (drop / s2);
}
#endif
