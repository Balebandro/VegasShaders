#pragma once

// UNOFFICIAL 2026-10-28 LinearScene (core/LinearScene.h): the pixel shaders that can give linear light (Includes/LinearLight.hlsl
// LinearSceneOut, compiled with LINEAR_SCENE_ON: ObjectTemplate, ParallaxTemplate, SkinTemplate, TerrainTemplate). ShaderRecord notes them
// as it makes them; only these are ever drawn while the frame is kept in linear light.
#include <d3d9.h>
#include <unordered_set>

namespace LinearScene {
	inline std::unordered_set<IDirect3DPixelShader9*> Capable;
	inline void NoteCapable(IDirect3DPixelShader9* handle) { if (handle) Capable.insert(handle); }
}
