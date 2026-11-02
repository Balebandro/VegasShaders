#pragma once

// UNOFFICIAL 2026-10-19 [Shaders.Grass.Collision]: the player and the people and creatures near them push the grass aside as they
// walk through it, and it springs back behind them. After Community Shaders' "Grass Collision"
// (github.com/community-shaders/skyrim-community-shaders, features/Grass Collision, GPL-3.0 with the Modding and Linking exceptions:
// actors' bounds flatten the grass round them, the grass vertex shader bends away from them, a spring brings it back). This build's
// own DirectX 9 version: the grass vertex shaders have no constant register left (the game's 228 instances fill c20-c247), so the
// push is a small texture read in the vertex shader (vertex texture fetch).
//
// A world-locked grid round the camera (Range each way, 128 x 128 cells) holds the sideways push at a blade's tip (world units).
// Every frame it fades toward nothing over Recovery seconds, then each actor in range stamps its footprint (pushed straight out
// from its centre, strongest at its feet), keeping whichever push is stronger. The grid is resampled into a camera-centred
// A32B32G32R32F texture (vertex textures need 32-bit float on DirectX 9), bound on vertex sampler 0 for NVR's grass vertex
// shaders (GRASS23x000-003.vso), which find their spot from the camera-relative position they already rebuild for the shadows.
// TESR_Translucency.w carries 1 / the texture's width in world units (0: off).
namespace GrassCollision {
	void	ReadSettings();
	void	Update();                                 // once a frame (GrassShaders::UpdateConstants)
	void	Bind(IDirect3DDevice9* Device);           // per grass draw with NVR's vertex shader (GrassShaders::SetForDraw)
	float	FieldScale();                             // 1 / the texture's width (world units), 0 while off
}
