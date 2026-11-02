#pragma once

// UNOFFICIAL 2026-10-30 LandAllLamps ([Main.Main.Misc] LandAllLamps, restart, off; docs/next/ROOT-ASSUMPTIONS-PLAN.md C: the land onto
// the one lamp list). The LAMP CAPS census (run 20261030-195707-slotA) found the land the one shader family that leaves lamps out: every
// other lit object gets all of its active lamps (one pass, ManyLamps' block or the table for the shiny many-lamp shaders), but in
// Freeside at night half the land pieces near the lamps (own lamps 7.25 a piece) were drawn by the 6-lamp land shader with ~7 of their
// lamps a frame left out. The land's lamps come from Vanilla Plus Terrain (VPT hooks the land passes and writes c39 colours, c63
// positions (the piece's own space, as IN.lPosition), c88 the count, and picks the 0/6/12/24-lamp shader; memory nvr-land-lamps-vpt).
// The land cannot read the lamp table (no free sampler: s15 is its last, terrain blending's), so the list goes in the same registers:
//   - with it on at startup, every land shader with lamps is compiled for 24 (LAND_ALL_LAMPS; its loop runs to the count anyway);
//   - before each land draw (ForPass) the piece's own active lamps are listed (its lighting property's list +0x60, filtered as
//     GetNumberOfActiveNonShadowLights, as ManyLamps reads it) with its world -> object matrix;
//   - after the pixel-constant write that sets the count (c88) while a land shader is bound (ConstantFilter's pixel hook: VPT's writes),
//     the lamps VPT did not write (no entry within 1 unit) are added after its own, up to 24, and the count raised. (v1 acted on every
//     write into c39-c88: VPT writes the colours, positions and count separately, so it compared against the last piece's positions,
//     46 % matched, run 20261030-201640-slotA; the self-check refused it.) The first land draw's writes are logged once (their order).
// Self-check (F10 profile): VPT's own entries are matched to the piece's lamps by position, and their colours compared with the ones this
// code would write; a LAND LAMPS line gives the matched share and the largest colour difference. Under 90 % matched: nothing is added
// (the positions are not in the space assumed here), logged once.
#include "GpuProfiler.h"
#include "PointShadowForward.h"
#include "ManyLamps.h"
#include <vector>

namespace LandLamps {

	enum { MaxLamps = 24, ColourReg = 39, PositionReg = 63, CountReg = 88 };
	inline int Wanted = -1;
	inline bool Bound = false;			// one of NVR's land shaders with lamps is bound
	inline bool OwnUpload = false;
	inline const void* Pass = nullptr;	// the land pass being drawn
	inline D3DXMATRIX ToObject;
	struct Lamp { D3DXVECTOR3 local; float reach; float colour[4]; };
	inline std::vector<Lamp> Own;
	inline bool Refused = false;		// the self-check failed: nothing added for the session
	inline UInt32 Frame = 0;
	inline unsigned StatFrames = 0;
	inline unsigned long long StatDraws = 0, StatAdded = 0, StatVpt = 0, StatMatched = 0, StatFull = 0;
	inline float StatColourDiff = 0.0f;
	inline int WritesLogged = 0;		// the first land draw's constant writes, logged once

	inline bool CompiledIn() {
		if (Wanted < 0) Wanted = TheSettingManager->GetSettingI("Main.Main.Misc", "LandAllLamps") != 0;
		return Wanted != 0;
	}
	// NVR's land shaders with lamps (effects/Terrain.h: SLS2092-SLS2146, every second, +2/+4/+6 = 6/12/24 lamps).
	inline bool IsLand(const char* name) {
		if (!name || strncmp(name, "SLS2", 4) || strcmp(name + 7, ".pso")) return false;
		const int n = atoi(name + 4);
		return n >= 92 && n <= 146 && !(n & 1) && ((n - 92) % 8) != 0;
	}

	inline void OnBind(const char* pixelShaderName, bool nvrRecord) {
		Bound = CompiledIn() && !Refused && nvrRecord && IsLand(pixelShaderName);
	}

	// Before each land draw (Render.cpp ForwardShadowsForPass): the piece's own active lamps in its space.
	inline void ForPass(const PointShadowForward::RenderPassView* pass) {
		if (!Bound || !pass || !pass->geometry) return;
		Pass = pass;
		Own.clear();
		NiGeometry* geometry = pass->geometry;
		BSShaderProperty* shade = (BSShaderProperty*)geometry->GetProperty(NiProperty::PropertyType::kType_Shade);
		if (!shade || !shade->IsLightingProperty()) return;
		const NiTransform& t = geometry->m_worldTransform;
		ManyLamps::GetInverseWorldMatrix(&t, geometry->skinInstance, &ToObject);	// (BSUtilities::GetInverseWorldMatrix, as the game makes the lamps' space)
		const float reachScale = t.scale > 0.0f ? 1.0f / t.scale : 1.0f;
		for (const UInt32* node = *(const UInt32* const*)((const UInt8*)shade + 0x60); node; node = (const UInt32*)node[0]) {
			const ShadowSceneLight* light = (const ShadowSceneLight*)node[2];
			const UInt8* raw = (const UInt8*)light;
			if (!light || *(const UInt16*)(raw + 0x110) == 0xFF || raw[0xEC]) continue;
			const NiPointLight* source = light->sourceLight;
			if (!source || (((const UInt8*)source)[0x30] & 1) || !light->bPointLight) continue;
			Lamp l;
			const D3DXVECTOR3 at(source->m_worldTransform.pos.x, source->m_worldTransform.pos.y, source->m_worldTransform.pos.z);
			D3DXVec3TransformCoord(&l.local, &at, &ToObject);
			l.reach = source->Spec.r * reachScale;
			float dimmer = source->Dimmer;
			if (!*(const bool*)0x011F941E && dimmer > 1.0f) dimmer = 1.0f;	// BSShaderManager::bHDR (as ManyLamps::Put)
			l.colour[0] = source->Diff.r * dimmer * light->fLODDimmer;
			l.colour[1] = source->Diff.g * dimmer * light->fLODDimmer;
			l.colour[2] = source->Diff.b * dimmer * light->fLODDimmer;
			l.colour[3] = 0.0f;
			Own.push_back(l);
		}
		if (GpuTimer::Enabled) StatDraws++;
	}

	// ConstantFilter's pixel hook, after every upload (Render.cpp's PixelWriteAfter dispatcher).
	inline void AfterPixelWrite(UINT start, UINT count) {
		if (OwnUpload || !Bound || !Pass || start > CountReg || start + count <= ColourReg) return;
		if (WritesLogged < 12) Logger::Log("LandAllLamps: land write %d: c%u-c%u (%u own lamps listed)", ++WritesLogged, start, start + count - 1, (unsigned)Own.size());
		if (Own.empty() || start + count <= CountReg) return;	// only once the count is written
		IDirect3DDevice9* d = TheRenderManager->device;
		float n4[4] = {};
		if (FAILED(d->GetPixelShaderConstantF(CountReg, n4, 1))) return;
		const int vpt = (std::max)(0, (std::min)((int)(n4[0] + 0.5f), (int)MaxLamps));
		float colours[MaxLamps][4] = {}, positions[MaxLamps][4] = {};
		if (vpt && (FAILED(d->GetPixelShaderConstantF(ColourReg, &colours[0][0], vpt)) || FAILED(d->GetPixelShaderConstantF(PositionReg, &positions[0][0], vpt)))) return;
		// VPT's entries matched to the piece's lamps by position (and the self-check)
		std::vector<bool> written(Own.size(), false);
		int matched = 0;
		for (int k = 0; k < vpt; k++)
			for (size_t i = 0; i < Own.size(); i++) {
				const D3DXVECTOR3 dv(positions[k][0] - Own[i].local.x, positions[k][1] - Own[i].local.y, positions[k][2] - Own[i].local.z);
				if (D3DXVec3LengthSq(&dv) > 1.0f) continue;
				if (!written[i]) { written[i] = true; matched++; }
				if (GpuTimer::Enabled)
					for (int c = 0; c < 3; c++) StatColourDiff = (std::max)(StatColourDiff, fabsf(colours[k][c] - Own[i].colour[c]));
				break;
			}
		if (GpuTimer::Enabled) { StatVpt += vpt; StatMatched += matched; }
		int total = vpt;
		for (size_t i = 0; i < Own.size() && total < MaxLamps; i++) {
			if (written[i]) continue;
			memcpy(colours[total], Own[i].colour, sizeof(colours[0]));
			positions[total][0] = Own[i].local.x; positions[total][1] = Own[i].local.y; positions[total][2] = Own[i].local.z; positions[total][3] = Own[i].reach;
			total++;
		}
		if (total == vpt) return;
		OwnUpload = true;
		d->SetPixelShaderConstantF(ColourReg + vpt, &colours[vpt][0], total - vpt);
		d->SetPixelShaderConstantF(PositionReg + vpt, &positions[vpt][0], total - vpt);
		const float n[4] = { (float)total, n4[1], n4[2], n4[3] };
		d->SetPixelShaderConstantF(CountReg, n, 1);
		OwnUpload = false;
		if (GpuTimer::Enabled) { StatAdded += total - vpt; if (total == MaxLamps) StatFull++; }
	}

	// Once a frame (Render.cpp, with ManyLamps::BeginFrame): the statistics, and the self-check's verdict.
	inline void BeginFrame() {
		Pass = nullptr;
		if (!CompiledIn() || !GpuTimer::Enabled) return;
		if (++StatFrames < 240) return;
		const double f = 1.0 / StatFrames;
		const double share = StatVpt ? 100.0 * StatMatched / StatVpt : 100.0;
		Logger::Log("LAND LAMPS per frame: %.1f land draws with lamps, %.1f lamps from VPT (%.1f%% matched to the piece's own by position, largest colour "
			"difference %.4f), %.1f lamps added, %.2f draws at the 24 limit%s",
			StatDraws * f, StatVpt * f, share, StatColourDiff, StatAdded * f, StatFull * f, Refused ? " (REFUSED: nothing added)" : "");
		if (StatVpt > 100 && share < 90.0 && !Refused) {
			Refused = true;
			Logger::Log("[ERROR] LandAllLamps: VPT's lamp positions do not match the space assumed here (%.1f%% matched): nothing added for the session", share);
		}
		StatFrames = 0; StatDraws = StatAdded = StatVpt = StatMatched = StatFull = 0; StatColourDiff = 0.0f;
	}
}
