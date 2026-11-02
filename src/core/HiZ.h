#pragma once

// UNOFFICIAL 2026-10-26 (NVR DXVK, deep/fork-hiz; docs\next\FORK-1026-HIZ.md): the hierarchical depth pyramid (Hi-Z).
// Built once per frame in ONE compute dispatch (Compute\HiZBuild.comp), only when a reader wants it: [Shaders.Reflections.Main] HiZ
// (the hierarchical ray march, Compute\ReflectionsTraceHiZ.frag). Vulkan-pass path: needs the NVR DXVK d3d9.dll and
// [Main.Main.Performance] NvrDxvkPasses; without them nothing is built and the effects run as before.
//
// v2 (2026-10-26): the pyramid is a STORAGE BUFFER (vec2 per cell, levels one after the other; layout in HiZBuild.comp). v1 wrote
// storage-image views of a 7-level G32R32F D3D9 render target: the HiZCheck readback found it ALL ZERO in game, so every coarse cell
// looked 'behind' the ray and the walk never hit anything.
// Level 0 is half the screen rounded up to a multiple of 64 (1920x1080 -> 960x576), each level half the one above (down to 15x9).
// Cell (i, j) of level k covers the full-resolution pixels [i, i+1) x [j, j+1) * 2^(k+1): x = the nearest linear depth there
// (TESR_DepthBuffer's value, depth / farZ), y = the farthest; the weapon's pixels are left out (all weapon: x 4, y 0).
// F10 timer "  Hi-Z pyramid".
//
// A failure (shader file missing, pipeline or buffer refused, dispatch failed) turns the pyramid off for the session (logged once); the
// readers then take their normal paths. It does not turn the other NVR DXVK paths off.

#include "DxvkCompute.h"

class HiZ {
public:
	static const UINT Levels = 7;
	// ShaderManager: builds this frame's pyramid when Wanted (once a frame; a second call does nothing).
	static void					Build(IDirect3DDevice9* Device, bool Wanted);
	// This frame's pyramid buffer, or null (not wanted, not built, failed): the reader takes its normal path.
	static ID3D9NvrExtBuffer*	Current();
	static bool					Failed() { return failed; }
	static int					CheckEvery;	// test only ([Shaders.Reflections.Main] HiZCheck): read back and compare every N built frames, 0 off

private:
	static void					Check(IDirect3DTexture9* depth, float invertedDepth);
	static bool					Ensure();
	static void					Release();
	static UINT					LevelOffset(UINT k);	// in cells

	static ID3D9NvrExtBuffer*	buffer;
	static ID3D9NvrExtPipeline*	pipeline;
	static UINT					width, height;		// level 0
	static UINT					groupsX, groupsY;
	static UINT					builtFrame;
	static bool					built;
	static bool					failed;
	static UINT					frames;
};
