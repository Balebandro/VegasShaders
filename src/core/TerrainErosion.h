// UNOFFICIAL 2026-10-27 erosion relief, ported from Balebandro's Physically-Based-Reloaded (a5bb3379 + e8bf26c9). Off by default.
#pragma once

// Erosion relief on distant terrain ([Shaders.Terrain.Erosion], Includes/TerrainErosion.hlsl): branching gullies carved
// into the LOD terrain's shading, following its slopes downhill.
namespace TerrainErosion {
	void	Initialize();	// ShaderManager::Initialize: the constants, by name, before shaders load
	void	Update();		// RenderHook, every frame: the settings into them
}
