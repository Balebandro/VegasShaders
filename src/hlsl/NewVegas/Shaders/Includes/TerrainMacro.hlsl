// UNOFFICIAL 2026-11-04 macro variation ([Shaders.Terrain.Macro], Terrain.cpp), off by default and compiled in only when on at startup
// (TERRAIN_MACRO, ShaderRecord::LoadShader). Terrain Variation ([Shaders.Terrain.Variation]) breaks the repeat of one ground texture
// a few metres across; a big slope can still read as one even colour from far away. Open-world games (Horizon, The Witcher 3) lay
// a large, slow variation over the ground: paler sand here, darker gravel there, a warmer or greyer stretch. This is that: a
// brightness and a warm/cool tint from value noise in world space (two octaves for the brightness, one wider for the tint), the
// same on the near land, the far land and the fade band between them, so no seam shows where they meet.
//
// Registers: c179 is clear in all three land shaders; c180 is TESR_CameraPosition as TerrainErosion.hlsl pins it (the far land
// includes both, so it is declared once).

float4 TESR_TerrainMacroData : register(c179);	// x strength (0: skipped; 2: diagnostic), y 1 / pattern size (game units), z brightness and w tint, in stops
#ifndef TESR_CAMERA_POSITION_C180
#define TESR_CAMERA_POSITION_C180
float4 TESR_CameraPosition : register(c180);
#endif

// A hash without sin (after Dave Hoskins' hash12, MIT): steady for the large cell numbers of world positions.
float MacroHash(float2 p) {
    float3 p3 = frac(p.xyx * 0.1031f);
    p3 += dot(p3, p3.yzx + 33.33f);
    return frac((p3.x + p3.y) * p3.z);
}

// Smooth value noise, 0..1.
float MacroNoise(float2 p) {
    const float2 i = floor(p);
    const float2 f = frac(p);
    const float2 u = f * f * (3.0f - 2.0f * f);
    return lerp(lerp(MacroHash(i), MacroHash(i + float2(1.0f, 0.0f)), u.x),
        lerp(MacroHash(i + float2(0.0f, 1.0f)), MacroHash(i + 1.0f), u.x), u.y);
}

// color: the ground's albedo as it goes into the lighting; cameraRelative: the pixel's camera-relative world position.
float3 MacroVary(float3 color, float3 cameraRelative) {
    [branch] if (TESR_TerrainMacroData.x <= 0.0f) return color;
    const float2 w = (cameraRelative.xy + TESR_CameraPosition.xy) * TESR_TerrainMacroData.y;
    // -1..1 (the noise's mean is 0.5 and it rarely reaches its ends: stretched so the settings mean what they say).
    // 2026-11-04 bench: as a plain +-0.5 times the old 0.12 range the patches moved about 0.015 stops: invisible.
    const float brightness = clamp(((MacroNoise(w) * 0.65f + MacroNoise(w * 3.7f + 17.3f) * 0.35f) - 0.5f) * 3.0f, -1.0f, 1.0f);
    const float tint = clamp((MacroNoise(w * 0.37f + 5.1f) - 0.5f) * 2.5f, -1.0f, 1.0f);
    [branch] if (TESR_TerrainMacroData.x > 1.5f) return (0.25f + 0.2f * brightness).xxx;	// Strength 2: the pattern as grey (diagnostic; dark, so a sunlit view does not clip it)
    // lighter or darker by up to z stops; warmer (redder, less blue) or cooler (greyer-blue) by up to w stops per channel
    const float3 varied = color * exp2(brightness * TESR_TerrainMacroData.z + tint * TESR_TerrainMacroData.w * float3(0.6f, 0.05f, -0.6f));
    return lerp(color, max(varied, 0.0f), saturate(TESR_TerrainMacroData.x));
}
