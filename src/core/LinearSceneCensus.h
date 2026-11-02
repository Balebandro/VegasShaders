#pragma once

// UNOFFICIAL 2026-10-28 DIAGNOSTIC (development builds only), [Main.Main.Misc] LinearSceneCensus, live: step 1 of the linear-scene plan
// (docs/next/LINEAR-SCENE-PLAN.md). User: "seems like this bug keeps popping up over and over how can we solve this once and for all".
// Every lighter-patch report so far (Lucky 38 tiles 10-21, Novac rubble 10-25, Freeside road day and night 10-28) is one cause: the
// game adds a surface's passes (light, extra lamps, texture, highlights) in the frame, and NVR's linear lighting encodes each pass on its
// own first. The plan is to keep the world in linear light in the fp16 target and encode once; before that, this counts what draws into
// the world and how, so the plan knows which shaders are NVR's linear ones and which are the game's (they would need decoding).
//
// On: every draw (DrawIndexedPrimitive / DrawPrimitive, device vtable) while the world or the first person is drawn is counted by
// (context, inside the main opaque list RenderBatches(1, 0x25B) or not, pixel shader, blend: off / ONE+ONE add / DESTCOLOR*ZERO multiply
// / alpha / other). Pixel shaders are named from SetShadersHook's binds (NiD3DPass names, NVR or the game's); others show as a pointer.
// Every 600 frames the top entries go to the log ("LINEAR CENSUS" lines). Off: nothing hooked, nothing counted.
#include <d3d9.h>
#include <unordered_map>
#include <vector>
#include <algorithm>

namespace LinearSceneCensus {

	inline int Mode = 0;
	inline bool InMain = false;			// GrassFirst::MainListHook, around RenderBatches(1, 0x25B)
	inline UInt16 LastPass = 0xFFFF;	// the pass enum of the last SetShadersHook bind (the game binds once per run of one pass kind)

	struct Name { const char* name; bool nvr; };
	inline std::unordered_map<IDirect3DPixelShader9*, Name> Names;

	struct Key {
		IDirect3DPixelShader9* ps; UInt16 pass; UInt8 context, main, blend;
		bool operator==(const Key& o) const { return ps == o.ps && pass == o.pass && context == o.context && main == o.main && blend == o.blend; }
	};
	struct KeyHash { size_t operator()(const Key& k) const { return (size_t)k.ps * 31u + k.pass * 131u + k.context * 7u + k.main * 3u + k.blend; } };
	inline std::unordered_map<Key, UInt64, KeyHash> Counts;
	inline UInt32 Frames = 0;

	typedef HRESULT(STDMETHODCALLTYPE* DIPFn)(IDirect3DDevice9*, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
	typedef HRESULT(STDMETHODCALLTYPE* DPFn)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT);
	inline DIPFn NextDIP = nullptr;
	inline DPFn NextDP = nullptr;
	inline bool Hooked = false;
	inline int (*ContextOf)() = nullptr;	// Render.cpp: 1 world, 2 first person, 0 other

	inline UInt8 BlendKind(IDirect3DDevice9* d) {
		DWORD on = 0, src = 0, dst = 0;
		d->GetRenderState(D3DRS_ALPHABLENDENABLE, &on);
		if (!on) return 0;
		d->GetRenderState(D3DRS_SRCBLEND, &src);
		d->GetRenderState(D3DRS_DESTBLEND, &dst);
		if (src == D3DBLEND_ONE && dst == D3DBLEND_ONE) return 1;
		if ((src == D3DBLEND_DESTCOLOR && dst == D3DBLEND_ZERO) || (src == D3DBLEND_ZERO && dst == D3DBLEND_SRCCOLOR)) return 2;
		if (src == D3DBLEND_SRCALPHA && dst == D3DBLEND_INVSRCALPHA) return 3;
		return 4;
	}

	inline void Count(IDirect3DDevice9* d) {
		if (!Mode || !ContextOf) return;
		const int c = ContextOf();
		if (!c) return;
		IDirect3DPixelShader9* ps = nullptr;
		d->GetPixelShader(&ps);
		if (ps) ps->Release();	// the pointer is only a key
		Counts[{ ps, LastPass, (UInt8)c, (UInt8)(InMain ? 1 : 0), BlendKind(d) }]++;
	}

	inline HRESULT STDMETHODCALLTYPE DIPHook(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, INT b, UINT mi, UINT nv, UINT s, UINT pc) {
		Count(d);
		return NextDIP(d, t, b, mi, nv, s, pc);
	}
	inline HRESULT STDMETHODCALLTYPE DPHook(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, UINT s, UINT pc) {
		Count(d);
		return NextDP(d, t, s, pc);
	}

	// SetShadersHook: the bound pass's pixel shader by name (the handle the device gets).
	inline void OnBind(IDirect3DPixelShader9* handle, const char* name, bool nvr, UInt32 pass) {
		if (!Mode) return;
		LastPass = (UInt16)pass;
		if (!handle) return;
		Names[handle] = { name, nvr };
	}

	inline void Hook(IDirect3DDevice9* d) {
		if (Hooked || !d) return;
		void** table = *(void***)d;
		DWORD old = 0;
		if (!VirtualProtect(&table[81], 2 * sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) return;
		NextDP = (DPFn)table[81];  table[81] = (void*)&DPHook;
		NextDIP = (DIPFn)table[82]; table[82] = (void*)&DIPHook;
		VirtualProtect(&table[81], 2 * sizeof(void*), old, &old);
		Hooked = true;	// stays hooked for the session (other hooks chain through it); with the census off it only passes on
		Logger::Log("LINEAR CENSUS: draw hooks on");
	}

	inline void Report() {
		static const char* const blends[] = { "opaque", "add ONE+ONE", "multiply", "alpha", "other blend" };
		static const char* const contexts[] = { "other", "world", "first person" };
		std::vector<std::pair<Key, UInt64>> v(Counts.begin(), Counts.end());
		std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
		UInt64 total = 0, nvr = 0, mainAll = 0, mainNvr = 0;
		for (const auto& e : v) {
			auto it = Names.find(e.first.ps);
			const bool isNvr = it != Names.end() && it->second.nvr;
			total += e.second; if (isNvr) nvr += e.second;
			if (e.first.main) { mainAll += e.second; if (isNvr) mainNvr += e.second; }
		}
		Logger::Log("LINEAR CENSUS: %u frames, %.1f draws/frame (%.1f by NVR shaders); main opaque list %.1f/frame (%.1f NVR)", Frames,
			total / (double)Frames, nvr / (double)Frames, mainAll / (double)Frames, mainNvr / (double)Frames);
		// UNOFFICIAL 2026-10-31 (docs/next/DEFERRED-RENDERING-PLAN.md stage 0a): the world's main list by blend, NVR's templates against the game's
		// shaders, and every blended template draw inside it (what a pass at the end of the list would find already blended over the walls: the
		// land fade and the fog passes are expected; anything else would take a deferred record's per-draw handle swap).
		{
			UInt64 mainBy[2][5] = {};
			for (const auto& e : v) {
				if (!e.first.main || e.first.context != 1 || e.first.blend > 4) continue;
				auto it = Names.find(e.first.ps);
				mainBy[it != Names.end() && it->second.nvr ? 1 : 0][e.first.blend] += e.second;
			}
			const double f = 1.0 / Frames;
			Logger::Log("LINEAR CENSUS main list (world), template / game draws a frame: opaque %.1f / %.1f, add %.1f / %.1f, multiply %.1f / %.1f, alpha %.1f / %.1f, other %.1f / %.1f",
				mainBy[1][0] * f, mainBy[0][0] * f, mainBy[1][1] * f, mainBy[0][1] * f, mainBy[1][2] * f, mainBy[0][2] * f, mainBy[1][3] * f, mainBy[0][3] * f, mainBy[1][4] * f, mainBy[0][4] * f);
			unsigned shownBlend = 0;
			for (const auto& e : v) {
				if (!e.first.main || e.first.context != 1 || e.first.blend == 0) continue;
				auto it = Names.find(e.first.ps);
				if (it == Names.end() || !it->second.nvr) continue;
				if (shownBlend++ >= 40) break;
				Logger::Log("LINEAR CENSUS in-span blended template draw: %6.1f/frame  pass %03X  %-12s %s", e.second * f, (unsigned)e.first.pass, blends[e.first.blend], it->second.name ? it->second.name : "?");
			}
			if (!shownBlend) Logger::Log("LINEAR CENSUS in-span blended template draws: none");
		}
		unsigned shown = 0;
		for (const auto& e : v) {
			if (shown++ >= 60) break;
			auto it = Names.find(e.first.ps);
			char ptr[24]; sprintf_s(ptr, "%p", (void*)e.first.ps);
			Logger::Log("LINEAR CENSUS: %6.1f/frame  %-12s %-4s pass %03X  %-12s %-24s %s", e.second / (double)Frames, contexts[e.first.context],
				e.first.main ? "MAIN" : "-", (unsigned)e.first.pass, blends[e.first.blend], it != Names.end() && it->second.name ? it->second.name : ptr,
				it == Names.end() ? "(unnamed: not a pass bind)" : it->second.nvr ? "NVR" : "game");
		}
	}

	// UNOFFICIAL 2026-10-31 (the deferred ladder's round-1 batch on the native slot: every window after the first counted 0.0 draws): the native
	// D3D9 runtime puts its own entries back into the device table now and then (CapturedNormals::EnsureHooked), so a hook put in once is gone
	// for the rest of the session. The same rule as there: a slot holding exactly what this hook calls on to gets ours back; anything else
	// (another program's hook on top) is left alone. Once a frame, while the census is on.
	inline UInt32 Reinstalls = 0;
	inline void EnsureHooked(IDirect3DDevice9* d) {
		if (!Hooked || !d) return;
		void** table = *(void***)d;
		struct Slot { int i; void* ours; void* next; };
		const Slot slots[2] = { { 81, (void*)&DPHook, (void*)NextDP }, { 82, (void*)&DIPHook, (void*)NextDIP } };
		for (const Slot& s : slots) {
			if (table[s.i] == s.ours || table[s.i] != s.next) continue;
			DWORD old = 0;
			if (!VirtualProtect(&table[s.i], sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) continue;
			table[s.i] = s.ours;
			VirtualProtect(&table[s.i], sizeof(void*), old, &old);
			if (++Reinstalls <= 3) Logger::Log("LINEAR CENSUS: device table slot %d had been put back to the runtime's entry: hook put in again (logged 3 times)", s.i);
		}
	}

	// Render.cpp, once a frame.
	inline void BeginFrame(IDirect3DDevice9* d) {
		const int want = TheSettingManager->GetSettingI("Main.Main.Misc", "LinearSceneCensus");
		if (want && !Hooked) Hook(d);
		if (want) EnsureHooked(d);	// UNOFFICIAL 2026-10-31 (native D3D9 puts its own table entries back)
		if (want != Mode) { Counts.clear(); Frames = 0; }
		Mode = want;
		if (!Mode) return;
		if (++Frames >= 600) { Report(); Counts.clear(); Frames = 0; }
	}
}
