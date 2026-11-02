// UNOFFICIAL 2026-10-27 RenderScale v2 (docs/next/RENDERSCALE-V2.md): with [Shaders.TAA.FSR] / [Shaders.TAA.DLSS] RenderScale below 1,
// NVR's effects before tonemapping run at the lower internal resolution, in the TOP-LEFT part of every screen-shaped texture
// (TESR_RenderScale.xy of its width and height), and FSR 2 / DLSS upscale the frame after them (effects/TAAUpscale.cpp).
// Two kinds of coordinates:
//   uv  = "rect" coordinates: what a full-screen pass's TEXCOORD0 holds and what texture reads use, 0..TESR_RenderScale.xy.
//   s01 = "screen" coordinates: 0..1 across the whole picture, what NDC / projection / camera-ray maths use.
// Off (RenderScale 1, or a frame drawn at native resolution): TESR_RenderScale = (1, 1, 1, 1) and every function here does nothing.
#ifndef NVR_RENDERRECT_HLSL
#define NVR_RENDERRECT_HLSL

float4 TESR_RenderScale;	// xy: render size / screen size (1 = off), zw: 1 / xy

// a texture coordinate to the screen position it shows (for NDC: s01 * 2 - 1, y flipped as usual)
float2 rectToScreen(float2 uv) { return uv * TESR_RenderScale.zw; }
// a screen position (projectPosition's xy, a reprojected history position...) to the texture coordinate that holds it
float2 screenToRect(float2 s01) { return s01 * TESR_RenderScale.xy; }
// keeps a read inside the drawn part (half a texel in from its right and bottom edge); texel = 1 / the texture's size
float2 clampToRect(float2 uv, float2 texel) { return clamp(uv, 0.5 * texel, TESR_RenderScale.xy - 0.5 * texel); }
// clampToRect only in rect frames (a native frame stays bit-identical whatever the sampler's addressing)
float2 clampToRectIfOn(float2 uv, float2 texel) { return (TESR_RenderScale.x < 0.9999 || TESR_RenderScale.y < 0.9999) ? clampToRect(uv, texel) : uv; }
// true past the drawn part (where a ray march or a reprojection has left the picture): replaces "uv < 0 || uv > 1" tests
bool outsideRect(float2 uv) { return any(uv < 0.0) || any(uv > TESR_RenderScale.xy); }

#endif
