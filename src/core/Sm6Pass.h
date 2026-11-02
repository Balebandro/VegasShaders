#pragma once

// UNOFFICIAL 2026-10-26 (deep/fork-sm6): NVR's own effect passes without Shader Model 3's limits on the NVR DXVK fork.
//
// The D3D9 path compiles each .fx.hlsl with fxc as ps_3_0 (224 float constants, 16 samplers, 32 temporaries, instruction slots).
// tools\nvr-sm6 compiles chosen passes of the SAME .fx.hlsl to SPIR-V (glslang's HLSL front end; `#if NVR_SM6` for what only the
// fork can afford) into Data\Shaders\VegasShaders\Compute\Sm6, one file per variant of the effects' compile-time defines, with a
// layout file (constants in one uniform buffer, textures, sampler states). An Sm6Pass draws such a pass through the NVR extension's
// full-screen passes in place of the D3D9 draw, fed exactly what the D3D9 pass would read at that moment:
//   constants: the effect's own parameter values (ID3DXEffect Get*: after the caller's SetCT and SetVector, as the D3D9 pass), or,
//              for a constant only the SM6 build declares, the registered TESR_ constant, else the source's literal default, else 0;
//   textures:  SetTexture overrides, else the device's texture on the stage the effect's compiled D3D9 pass samples that sampler from
//              (what SetCT and the caller's own SetTexture calls bound), else the effect's TESR_ texture.
// So an effect switches a pass over by replacing its DrawPrimitive with Draw (same setup code); docs\next\FORK-1026-SM6.md.
//
// [Main.Main.Performance] NvrSm6Shaders (live). Native D3D9 and stock DXVK: On() is false and nothing here runs. A pass whose build
// is missing, stale (the deployed .hlsl or an include changed since tools\nvr-sm6\build.py ran: FNV-1a hashes in the layout) or
// refused stays on its D3D9 shader, logged once; the other compute paths are not affected.

#include <map>
#include <string>
#include <vector>
#include "NvrDxvkExt.h"
#include "EffectRecord.h"

class Sm6Pass {
public:
	static bool On();

	// Loads <Effect>.<Technique>.<Pass> on first use and again after the effect is reloaded. False: keep the D3D9 pass.
	bool Prepare(EffectRecord* Effect, const char* Technique, UINT Pass);
	// Per draw (cleared by Draw): textures/vectors the D3D9 path does not hand over the usual way (SM6-only samplers, say).
	void SetTexture(const char* Name, IDirect3DBaseTexture9* Texture) { TextureOverrides[Name] = Texture; }
	void SetVector(const char* Name, const D3DXVECTOR4& Value) { VectorOverrides[Name] = Value; }
	// One full-screen draw into Targets (Count = the pass's target count; Viewport null = the whole target). Uv: as for
	// EffectRecord::ExtDrawTo (ScreenQuad for a FrameVS pass, TexelCentre for HalfVS-style vertex shaders). False (logged once,
	// the pass then stays off for the session): draw the D3D9 pass instead.
	bool Draw(IDirect3DDevice9* Device, IDirect3DSurface9* const* Targets, UINT Count, EffectRecord::ExtUv Uv, const RECT* Viewport = nullptr);
	bool Ready() const { return Pipeline != nullptr && !Failed; }
	UINT Targets() const { return TargetCount; }
	// The pass's own vertex shader is FrameVS (the frame quad) and it sets no render states (blending etc.): the generic hook in
	// EffectRecord::Render may draw it.
	bool Plain() const { return VertexShader == "FrameVS" && StateCount == 0; }
	// The device has a scissor rectangle on (PointShadows' LampMaskScissor): the extension's passes have no scissor and would draw
	// the whole target (same picture, as the pass writes 0 outside its lamps, but 0.02 -> 0.26 ms at og-night 2026-10-26), so the
	// generic hook keeps the D3D9 draw then.
	static bool Scissored(IDirect3DDevice9* Device) { DWORD on = FALSE; return Device && SUCCEEDED(Device->GetRenderState(D3DRS_SCISSORTESTENABLE, &on)) && on; }
	UInt32 Frames = 0;

private:
	struct Constant {
		std::string Name;
		UINT Offset = 0, Size = 0;
		std::string Type;			// float4, float4x4, float4[9], ...
		std::vector<float> Default;
		D3DXHANDLE Handle = NULL;	// the effect's parameter, if the D3D9 build declares it
		D3DXPARAMETER_DESC Desc = {};
	};
	struct Texture {
		std::string Name;
		UINT Binding = 0, Slot = 0;
		NVR_EXT_VIEW_TYPE View = NVR_EXT_VIEW_2D;
		int Stage = -1;				// the D3D9 pass's sampler register for it, -1 if that pass does not sample it
	};
	bool Fail(const char* Why);
	bool LoadLayout(const char* Path);
	void FillConstants();
	IDirect3DBaseTexture9* TextureFor(IDirect3DDevice9* Device, const Texture& T, IDirect3DBaseTexture9*& Held);

	EffectRecord* Owner = nullptr;
	unsigned Generation = 0;
	bool Tried = false, Failed = false;
	std::string Key;				// <Effect>.<Technique>.<Pass>
	std::vector<Constant> Constants;
	std::vector<Texture> Textures;
	std::vector<std::pair<UINT, std::string>> SamplerStates;	// slot, "FILTER ADDRESS"
	std::vector<ID3D9NvrExtSampler*> Samplers;
	std::vector<unsigned char> UboData;
	UINT UboSize = 0, PushSize = 0, TargetCount = 1;
	std::string VertexShader;
	int StateCount = 0;
	ID3D9NvrExtPipeline* Pipeline = nullptr;
	EffectRecord::ExtUniformBuffer Ubo;
	std::map<std::string, IDirect3DBaseTexture9*> TextureOverrides;
	std::map<std::string, D3DXVECTOR4> VectorOverrides;
	std::vector<std::string> Missing;	// constants with no value (logged once)
};

// An effect's SM6 passes by technique and pass (EffectRecord::Sm6Set).
struct Sm6PassSet { std::map<std::string, Sm6Pass> Passes; };
