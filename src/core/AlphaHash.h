#pragma once
// UNOFFICIAL 2026-11-03 SmoothCutouts ([Shaders.TAA.Cutouts]; shaders: Includes/AlphaHash.hlsl).
//
// Alpha-tested cut-outs (leaves, grass, fences, chain-link, hair cards) were made for the game's transparency multisampling, alpha to
// coverage, which needs the game's MSAA. TAA, FSR and DLSS run with MSAA off, so every texel is drawn fully or not at all: jagged cut
// edges, and the see-through fringe of a texture made for alpha to coverage drawn solid (Discord 2026-11-03: "all alpha cutout assets
// look chopped"; a tester's hair port drew a dark outline round the head). The object, parallax and grass shaders hash the alpha test
// instead (keep the texel with a probability, against noise that changes per pixel and frame); the jittered frames average it.
//
// Per object pass (Render.cpp RenderPassStandardHook / RenderPassSkinnedHook): b15 is 1 for an object of the jittered world scene whose
// alpha property tests (GREATER / GREATEREQUAL) without blending; the shaders read it (NVR_AlphaHashDraw). Back to 0 after the pass, so
// no other draw (blended glass and hair, the sorted blended list, NVR's own redraws, anything outside the world scene) ever hashes.
// Every pass of one object takes the same decision for a pixel (same alpha, same noise), so its lamp passes land on what it drew.
// Skin (SkinTemplate) is left out: its light-only passes test alpha in the fixed-function stage only and could not agree.
// Per frame (TESR_AlphaHash): x the mode (0 off), y the noise offset, z the edge width.
// Off (Enabled false at startup): ALPHA_HASH_ON is not defined, the shaders are exactly as before and nothing here runs.
#include <d3d9.h>

namespace AlphaHash {
	inline const char* kSection = "Shaders.TAA.Cutouts";
	inline int Startup = -1;				// Enabled at startup (the shaders are compiled with it)
	inline bool Live = false;				// this world scene is jittered and hashing is on
	inline bool Sent = false;				// b15 as last sent
	inline UInt32 Frame = 0;				// world scenes begun (the noise moves once per frame's first one)
	inline UInt32 Hashed = 0, HashedShown = 0;	// passes hashed this frame / last frame (the log line)
	inline D3DXVECTOR4 Constant = { 0.0f, 0.0f, 1.0f, 0.0f };	// TESR_AlphaHash

	inline bool CompiledIn() {
		if (Startup < 0) Startup = TheSettingManager->GetSettingI(kSection, "Enabled") ? 1 : 0;
		return Startup == 1;
	}

	inline void Send(IDirect3DDevice9* device, bool on) {
		if (on == Sent || !device) return;
		const BOOL value = on ? TRUE : FALSE;
		device->SetPixelShaderConstantB(15, &value, 1);
		Sent = on;
	}

	// The world scene starts (RenderWorldSceneGraphHook, after TAA's BeginJitter). jittered: TAA's jitter is on for it.
	inline void BeginWorld(IDirect3DDevice9* device, bool firstWorld, bool jittered) {
		Sent = true; Send(device, false);	// whatever the device holds: 0 until a pass says otherwise
		if (!CompiledIn()) { Live = false; return; }
		const int mode = TheSettingManager->GetSettingI(kSection, "Mode");
		Live = jittered && mode > 0;
		if (firstWorld) {
			Frame++;
			HashedShown = Hashed;
			Hashed = 0;
			if (Live && Frame % 3600 == 1) Logger::Log("SmoothCutouts: mode %d, %u alpha-tested passes hashed last frame", mode, HashedShown);
		}
		// Interleaved gradient noise moves 5.588238 pixels a frame (Jimenez 2014): 64 frames before it repeats.
		Constant.x = Live ? (float)(std::min)(mode, 2) : 0.0f;
		Constant.y = 5.588238f * (float)(Frame % 64);
		Constant.z = (std::max)(0.25f, TheSettingManager->GetSettingF(kSection, "EdgeWidth"));
	}

	inline void EndWorld(IDirect3DDevice9* device) {
		Live = false;
		Send(device, false);
	}

	// Before each object pass. world: the pass belongs to the world scene (ShaderSplit::World).
	inline void ForPass(NiGeometry* geometry, bool world, IDirect3DDevice9* device) {
		bool hash = Live && world && geometry;
		if (hash) {
			const NiAlphaProperty* alpha = geometry->propertyState.m_spAlphaProperty;
			const UInt16 func = alpha ? (UInt16)((alpha->flags >> 10) & 7) : 0;	// NiAlphaProperty::TestFunction: 4 GREATER, 6 GREATEREQUAL
			hash = alpha && (alpha->flags & NiAlphaProperty::TEST_ENABLE_MASK) && !(alpha->flags & NiAlphaProperty::ALPHA_BLEND_MASK) &&
				(func == 4 || func == 6);
		}
		if (hash) Hashed++;
		Send(device, hash);
	}

	inline void AfterPass(IDirect3DDevice9* device) { Send(device, false); }
}
