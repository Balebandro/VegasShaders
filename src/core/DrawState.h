#pragma once
// UNOFFICIAL 2026-10-29 SharedDrawState (ROOTPLAN round 4 step 11): the device state CapturedNormals' lean hook tracks from the device's
// own Set calls inside the world's main list (core/CapturedNormals.h), in a header of its own so other per-draw hooks (LinearScene) can
// read it without including CapturedNormals (its includes reach LinearScene.h). Valid only while CapturedNormals keeps it (lean hook on,
// NVR DXVK, inside its span); anywhere else readers ask the device.
#include <d3d9.h>

namespace SharedDrawState {
	struct TrackedState { IDirect3DSurface9* rt[4]; IDirect3DPixelShader9* ps; DWORD colorWrite, blend, src, dst, colorWrite2, colorWrite3, colorWrite1; };	// colorWrite1: UNOFFICIAL 2026-10-31 AlbedoBuffer
	inline TrackedState T = {};
	inline bool Valid = false;
}
