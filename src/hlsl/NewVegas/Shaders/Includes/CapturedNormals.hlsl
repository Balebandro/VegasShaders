// UNOFFICIAL 2026-10-28 CapturedNormals (core/CapturedNormals.h, docs/next/CAPTURED-NORMALS.md; macintroll's proposal): while the
// world's main opaque list draws, NVR's lit templates (ObjectTemplate, ParallaxTemplate, SkinTemplate, TerrainTemplate) write the surface's
// smooth interpolated vertex normal (world space, after skinning, BEFORE the normal map) to a fourth render target, COLOR3:
//   rg  the normal, octahedral (Includes/Shadow.hlsl ShadowEncodeNormal), facing the camera
//   b   the camera distance (length of the camera-relative world position), which readers compare against the depth buffer
//   a   1 = written by a template, 2 = by an actor's (bone-skinned draws: SKIN, or the skin template; 2026-10-28: snow keeps off actors)
//       (0 = cleared, or a variant with no smooth normal)
// The target is bound only inside that list and only for those templates' opaque draws (anything else masks it off, COLORWRITEENABLE3);
// anywhere else COLOR3 is discarded. Compiled in only with [Shaders.SnowAccumulation.Main] CapturedNormals on at startup
// (CAPTURED_NORMALS_ON, ShaderRecord::BuildDefines): off, the templates are exactly as before. Needs Includes/Shadow.hlsl first.
#ifndef NVR_CAPTURED_NORMALS_HLSL
#define NVR_CAPTURED_NORMALS_HLSL

#ifndef CAPTURED_NORMALS_ON
    #define CAPTURED_NORMALS_ON 0
#endif

#if CAPTURED_NORMALS_ON
    #define CAPTURED_NORMAL_INVALID float4(0.0f, 0.0f, 0.0f, 0.0f)
    // n: the smooth vertex normal (camera-relative world space, any length), worldPos: the camera-relative world position.
    float4 CapturedNormalOut(float3 n, float3 worldPos) {
        const float len2 = dot(n, n);
        [flatten] if (len2 < 1.0e-6f) return CAPTURED_NORMAL_INVALID;
        n *= rsqrt(len2);
        n *= dot(n, worldPos) > 0.0f ? -1.0f : 1.0f;	// the side the camera sees (two-sided cards, cloth)
        #if defined(SKIN) || defined(CAPTURED_ACTOR)
            return float4(ShadowEncodeNormal(n), length(worldPos), 2.0f);
        #else
            return float4(ShadowEncodeNormal(n), length(worldPos), 1.0f);
        #endif
    }

    // UNOFFICIAL 2026-10-29 MaterialBuffer (core/CapturedNormals.h; [Main.Main.Quality] MaterialBuffer, restart; CAPTURED_NORMALS_ON 2): the
    // templates' spare COLOR2 = TESR_MaterialBuffer while the main list draws: r the material class (Render.cpp per draw: 1 object, 2 worn
    // gear (skinned), 3 cloth, 4 land, 5 plants), g the drawn object's world bound radius (thickness, an upper bound), b the roughness the
    // shader lit with (-1: none), a 1. Actors' skin is not in it (its template's COLOR2 is the skin blur's): the normals' a == 2 marks it.
    // 2026-10-31: r = class + 0.49 x metal (the share of the pixel lit as metal, [Shaders.PBR.Metal]): class = floor(r), metal =
    // frac(r) / 0.49. Readers that test r > 0.5 for "written" stay right. Skin is filled in after the list (class 6, core/CapturedNormals.h).
    #if CAPTURED_NORMALS_ON >= 2
        // 2026-10-31: the draw's class and bound radius ride in NVR_SpecFade.w (c211, Includes/PBR.hlsl: class + 8 x floor(radius), set per draw
        // by Render.cpp MaterialForPass through core/PbrPop.h). They were on c217 until then, which the RT lamp shadows' NVR_PointShadowRTSlot[2]
        // (c216-c217) also holds: every interior lamp-shadow shader with POINT_SHADOW_RT failed to compile with MaterialBuffer on.
        #define NVR_MATERIAL_RADIUS floor(NVR_SpecFade.w * 0.125f)
        #define NVR_MATERIAL_CLASS (NVR_SpecFade.w - 8.0f * NVR_MATERIAL_RADIUS)
        #define CapturedMaterialOut(roughness) float4(NVR_MATERIAL_CLASS, NVR_MATERIAL_RADIUS, (roughness), 1.0f)
        #define CapturedMaterialOutClass(materialClass, roughness) float4((materialClass), NVR_MATERIAL_RADIUS, (roughness), 1.0f)
        #define CapturedMaterialOutMetal(roughness, metal) float4(NVR_MATERIAL_CLASS + 0.49f * saturate(metal), NVR_MATERIAL_RADIUS, (roughness), 1.0f)
    #else
        #define CapturedMaterialOut(roughness) 0.0f
        #define CapturedMaterialOutClass(materialClass, roughness) 0.0f
        #define CapturedMaterialOutMetal(roughness, metal) 0.0f
    #endif

    // UNOFFICIAL 2026-10-31 AlbedoBuffer (core/CapturedNormals.h; [Main.Main.Quality] AlbedoBuffer, restart, with MaterialBuffer; CAPTURED_NORMALS_ON 3;
    // docs/next/DEFERRED-RENDERING-PLAN.md rung A): the templates' spare COLOR1 = TESR_AlbedoBuffer while the main list draws: rgb the LINEAR base
    // colour the light is multiplied by (texture x vertex colour, after the glow mix, before metal: readers take diffuse = rgb x (1 - metal) and
    // F0 = lerp(0.04, rgb, metal), the metal from TESR_MaterialBuffer.r's fraction), a 1 = written. Passes with no texture of their own (the
    // DIFFUSE lamp passes, unless FoldHighlights bound the base map on s7) write a 0; the ONLY_SPECULAR highlight passes keep their highlightCopy
    // there (no writer: core/CapturedNormals.h masks them off). Below 3 the macros give the 0 the pad was written with before.
    #if CAPTURED_NORMALS_ON >= 3
        #define CapturedAlbedoOut(rgb) float4((rgb), 1.0f)
        #define CAPTURED_ALBEDO_NONE float4(0.0f, 0.0f, 0.0f, 0.0f)
    #else
        #define CapturedAlbedoOut(rgb) 0.0f
        #define CAPTURED_ALBEDO_NONE 0.0f
    #endif
#endif

#endif
