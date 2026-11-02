#pragma once
// UNOFFICIAL 2026-11-01 [Shaders.POM.Displacement] SameHeight (tester report: interior rubble turns white with object displacement on, depending
// on position; gone with it off). The displaced parallax objects (core/DxvkCompute.cpp RegisterDisplaceObject) read their height in the
// tessellation from the pixel shader's HeightMap slot: s3 in the main pass, s2 in the lamp, highlight and texture passes. Indoors those later
// passes often find something else in s2 (run 20261101-013940-slotA, VRuinedRubblePileLg01/02: the main pass's height map 2048 DXT1 in some
// lamp passes, a 1024 DXT5 left there by an earlier draw in others: the game never sets that stage for them). A pass with another height lands
// on another depth, fails its EQUAL depth test or wins where it should not, and the floor comes out as untextured light: white patches that
// move with the draw order. The fix: each object's main pass (depth writes on) notes its height map; the object's later passes this frame get
// that texture in their height slot before they draw (their own parallax reads that slot as HeightMap too, so they read the right map).
// Off = as before.
#include <d3d9.h>
#include <unordered_map>

namespace DisplaceHeight {
	inline std::unordered_map<IDirect3DPixelShader9*, int> Slots;	// the displaced object pixel shaders and their HeightMap sampler
	inline std::unordered_map<const void*, IDirect3DBaseTexture9*> Kept;	// this frame: object -> its main pass's height map (one reference held)
	inline const void* Geometry = nullptr;	// the object being drawn (Render.cpp's pass hook)
	inline bool On = false;
	inline unsigned Fixed = 0, Frames = 0;

	inline void Note(IDirect3DPixelShader9* ps, int slot) { if (ps && slot >= 0 && slot < 16) Slots[ps] = slot; }

	inline void Release() {
		for (auto& k : Kept) if (k.second) k.second->Release();
		Kept.clear();
	}

	// Render.cpp, once a frame
	inline void BeginFrame() {
		Release();
		On = !Slots.empty() && TheSettingManager->GetSettingI("Shaders.POM.Displacement", "Enabled") &&
			TheSettingManager->GetSettingI("Shaders.POM.Displacement", "SameHeight");
		if (On && ++Frames >= 600) {
			Logger::Log("UNOFFICIAL displacement SameHeight: per frame %.1f object passes given their main pass's height map", Fixed / (double)Frames);
			Fixed = 0; Frames = 0;
		}
	}

	// CapturedNormals' draw hook, before every draw
	inline void BeforeDraw(IDirect3DDevice9* d) {
		if (!On || !Geometry) return;
		IDirect3DPixelShader9* ps = nullptr;
		d->GetPixelShader(&ps);
		if (ps) ps->Release();
		const auto s = Slots.find(ps);
		if (s == Slots.end()) return;
		IDirect3DBaseTexture9* tex = nullptr;
		d->GetTexture(s->second, &tex);
		DWORD zwrite = FALSE;
		d->GetRenderState(D3DRS_ZWRITEENABLE, &zwrite);
		auto k = Kept.find(Geometry);
		if (zwrite) {	// the main pass: its height map is the object's
			if (k != Kept.end()) { if (k->second) k->second->Release(); k->second = tex; }
			else Kept.emplace(Geometry, tex);
			return;	// (the reference GetTexture took is the one kept)
		}
		if (k != Kept.end() && k->second && k->second != tex) { d->SetTexture(s->second, k->second); Fixed++; }
		if (tex) tex->Release();
	}
}
