#pragma once

// UNOFFICIAL ShaderWarmUp2 ([Main.Main.Performance] ShaderWarmUp2, default 0 = off; 2026-10-18, helper warmup2, roadmap 3.12).
// A shader preloader for native Direct3D 9, to do what DXVK does for free (compile shaders before they are needed).
//
// Why v1 (ShaderWarmUp.h) did nothing: the native D3D9 driver finishes a shader's compile at the first DRAW, and it compiles
// one version per drawing setup (see the lab, evidence\warmup2\lab: which states start a new compile), so v1's one stand-in
// draw per shader (generic partner stage, no textures, a tiny A8R8G8B8 target) compiled versions the game never uses.
// v2 records the REAL setups and replays exactly those:
//  - RECORD (mode 1 and 2): detours on the d3d9 device's function bodies (as ConstantFilter.h; the native runtime rewrites its
//    vtable) keep a copy of the drawing setup on the render thread: vertex and pixel shader, vertex declaration, the format and
//    type of the texture on every sampler the shaders declare (+ sRGB read), the render-target and depth formats with their
//    MSAA, and the render states that can change the compiled code. Every new setup is timed at its first draw and appended to
//    Shaders\VegasShaders\Shaders\Cache\WarmUp2.txt (shaders by a hash of their bytecode, so the list is valid in the next
//    session), with what is needed to rebuild them. 2026-10-19: EVERY new setup, not only slow first draws: with NVIDIA's
//    threaded driver a draw call only queues (~0.4 us) and the compile runs on the driver's thread, so its cost lands on some
//    later call (test B1: 0.3 s in first draws, 2.0 s in draws of known setups, most of the rest outside draw calls).
//  - REPLAY (mode 2), at the main menu: every listed setup whose first draw took >= MinMs (default 0 = all) is rebuilt with stand-ins (the real
//    shaders found by bytecode hash; a vertex buffer of zeros; 4x4 textures and 16x16 targets of the recorded types, formats and
//    MSAA; the recorded states) and drawn once (a degenerate triangle into a hidden target), slowest first, BudgetMs per menu
//    frame; the device state is saved in a state block (+ render targets, depth, viewport, scissor) and put back. The driver
//    then compiles at the menu what it used to compile in the world. Nothing in the picture changes (no world draw is touched).
//  - Skipped under DXVK (it compiles ahead anyway). Mode 0 installs nothing.
// Log lines: 'WARMUP2 FRAME' (frames over 25 ms: time in new-setup draws), 'WARMUP2 NEW' (slow first draws), 'WARMUP2 TOTALS'
// (every 600 frames), 'Shader warm-up 2 ...' (the replay).
#include <intrin.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include "TestTools.h"	// NVR_TEST_TOOLS
#include "GpuProfiler.h"
#include "ConstantFilter.h"	// its state-block / reset / pixel-shader hooks report to OnStateLost and OnSetPixelShader
#include "ArrivalSampler.h"	// UNOFFICIAL 2026-10-20 (test builds only, off by default): main-thread samples of the frames over 50 ms, fed from FrameTick

namespace ShaderWarmUp2 {

	// ---- settings (read once, when the device hooks go in) ----
	inline int Mode = 0;				// 0 off, 1 record only, 2 record + warm up at the main menu
	inline double BudgetMs = 150.0;		// warm-up time per main-menu frame
	inline double MinMs = 0.0;			// warm up the setups whose first draw took at least this long when recorded (0 = all)
	inline const double SaveMinMs = 0.0;	// every new setup is written (2026-10-19: was 0.5 ms; first-draw time does not show a compile on a threaded driver)
	inline bool LogRecords = NVR_TEST_TOOLS;	// test builds: every list line also goes to the NVR log ('WARMUP2 REC'; the test's restore moves the list file away)

	inline bool Installed = false, Broken = false;
	inline DWORD Thread = 0;			// the render thread
	inline bool Replaying = false;		// our own warm-up calls: not tracked
	inline volatile bool Unknown = true;	// the copy of the device state is not trusted: read it back at the next draw
	inline bool Dirty = true;			// the setup changed since the last draw
	inline double TicksPerMs = 0.0;
	inline unsigned long long CalTsc = 0;
	inline double CalMs = 0.0;

	inline double NowMs() { LARGE_INTEGER f, c; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c); return (double)c.QuadPart * 1000.0 / (double)f.QuadPart; }

	inline uint64_t Hash64(const void* data, size_t size, uint64_t h = 1469598103934665603ULL) {
		const unsigned char* p = (const unsigned char*)data;
		size_t i = 0;
		for (; i + 8 <= size; i += 8) { uint64_t w; memcpy(&w, p + i, 8); h ^= w; h *= 1099511628211ULL; h ^= h >> 29; }
		for (; i < size; i++) { h ^= p[i]; h *= 1099511628211ULL; }
		h ^= h >> 33; h *= 0xff51afd7ed558ccdULL; h ^= h >> 33; h *= 0xc4ceb9fe1a85ec53ULL; h ^= h >> 33;
		return h;
	}

	// The per-draw key: 32-bit arithmetic only (64-bit multiplies are library calls in a 32-bit build), two lanes.
	inline uint64_t KeyHash(const uint32_t* w, size_t n) {
		uint32_t a = 0x9E3779B9u, b = 0x85EBCA6Bu;
		for (size_t i = 0; i < n; i++) { a = (a ^ w[i]) * 0x01000193u; b = (b + w[i]) * 0xC2B2AE35u; b ^= b >> 15; }
		a ^= a >> 16; a *= 0x85EBCA6Bu; a ^= a >> 13; b ^= b >> 16; b *= 0x27D4EB2Fu; b ^= b >> 13;
		return ((uint64_t)a << 32) | b;
	}

	// ---- shaders: bytecode hash + the samplers they declare ----
	struct ShaderInfo { uint64_t Hash = 0; uint32_t Samplers = 0; };	// Samplers: bits 0-15 pixel samplers, 16-19 vertex texture samplers
	inline SRWLOCK InfoLock = SRWLOCK_INIT;
	inline std::unordered_map<const void*, ShaderInfo> Infos;
	inline std::unordered_map<uint64_t, std::vector<IUnknown*>> Objects;	// mode 2: shader objects per hash, held (AddRef) until the warm-up is over
	inline bool KeepObjects = false;

	inline ShaderInfo Describe(const DWORD* code, UINT bytes) {
		ShaderInfo info;
		if (!code || bytes < 8) return info;
		const size_t n = bytes / 4;
		info.Hash = Hash64(code, n * 4);
		const bool pixel = (code[0] & 0xFFFF0000) == 0xFFFF0000;
		const DWORD major = (code[0] >> 8) & 0xFF;
		if (major < 2) return info;	// SM1: no instruction lengths, no sampler declarations to read
		size_t i = 1;
		while (i < n) {
			const DWORD t = code[i];
			if (t == 0x0000FFFF) break;
			const DWORD op = t & 0xFFFF;
			if (op == 0xFFFE) { i += 1 + ((t >> 16) & 0x7FFF); continue; }
			if (op == 31 && i + 2 < n) {	// dcl
				const DWORD reg = code[i + 2];
				const DWORD type = ((reg >> 28) & 7) | ((reg >> 8) & 0x18), num = reg & 0x7FF;
				if (type == 10) {	// D3DSPR_SAMPLER
					if (pixel && num < 16) info.Samplers |= 1u << num;
					else if (!pixel && num < 4) info.Samplers |= 1u << (16 + num);
				}
			}
			i += 1 + ((t >> 24) & 0x0F);
		}
		return info;
	}

	inline void NoteShader(IUnknown* object, const DWORD* code, UINT bytes) {
		const ShaderInfo info = Describe(code, bytes);
		AcquireSRWLockExclusive(&InfoLock);
		Infos[object] = info;
		if (KeepObjects) { object->AddRef(); Objects[info.Hash].push_back(object); }
		ReleaseSRWLockExclusive(&InfoLock);
	}

	// The info of a shader bound now (created before the hooks went in: read its bytecode back once).
	template <class T> inline ShaderInfo InfoOf(T* shader) {
		if (!shader) return ShaderInfo();
		AcquireSRWLockShared(&InfoLock);
		auto it = Infos.find(shader);
		const bool found = it != Infos.end();
		ShaderInfo info = found ? it->second : ShaderInfo();
		ReleaseSRWLockShared(&InfoLock);
		if (found) return info;
		UINT bytes = 0;
		if (FAILED(shader->GetFunction(nullptr, &bytes)) || !bytes || bytes > (1u << 22)) return info;
		std::vector<DWORD> code((bytes + 3) / 4);
		if (FAILED(shader->GetFunction(code.data(), &bytes))) return info;
		info = Describe(code.data(), bytes);
		AcquireSRWLockExclusive(&InfoLock);
		Infos[shader] = info;
		ReleaseSRWLockExclusive(&InfoLock);
		return info;
	}

	// ---- textures: format and type per object (direct-mapped cache; creation overwrites the slot of a new object) ----
	struct TexSlot { const void* Ptr; DWORD Format; BYTE Type; };
	inline TexSlot TexCache[4096] = {};
	inline unsigned SlotOf(const void* p) { return (unsigned)(((uintptr_t)p >> 4) ^ ((uintptr_t)p >> 16)) & 4095; }
	inline void PutTex(const void* p, DWORD format, BYTE type) {
		TexSlot& s = TexCache[SlotOf(p)];
		s.Ptr = nullptr; _ReadWriteBarrier(); s.Format = format; s.Type = type; _ReadWriteBarrier(); s.Ptr = p;
	}
	// type: 0 none, 3 texture, 4 volume, 5 cube (D3DRESOURCETYPE)
	inline void TexOf(IDirect3DBaseTexture9* t, DWORD& format, BYTE& type) {
		if (!t) { format = 0; type = 0; return; }
		const TexSlot& s = TexCache[SlotOf(t)];
		if (s.Ptr == t) { format = s.Format; type = s.Type; _ReadWriteBarrier(); if (s.Ptr == t) return; }
		const D3DRESOURCETYPE rt = t->GetType();
		format = 0; type = (BYTE)rt;
		if (rt == D3DRTYPE_TEXTURE) { D3DSURFACE_DESC d; if (SUCCEEDED(((IDirect3DTexture9*)t)->GetLevelDesc(0, &d))) format = d.Format; }
		else if (rt == D3DRTYPE_CUBETEXTURE) { D3DSURFACE_DESC d; if (SUCCEEDED(((IDirect3DCubeTexture9*)t)->GetLevelDesc(0, &d))) format = d.Format; }
		else if (rt == D3DRTYPE_VOLUMETEXTURE) { D3DVOLUME_DESC d; if (SUCCEEDED(((IDirect3DVolumeTexture9*)t)->GetLevelDesc(0, &d))) format = d.Format; }
		PutTex(t, format, type);
	}

	// ---- the drawing setup as the driver may compile per it (render thread copy) ----
	inline const D3DRENDERSTATETYPE KeyStates[] = {
		D3DRS_ALPHATESTENABLE, D3DRS_ALPHAFUNC, D3DRS_ALPHABLENDENABLE, D3DRS_SRCBLEND, D3DRS_DESTBLEND, D3DRS_BLENDOP,
		D3DRS_SEPARATEALPHABLENDENABLE, D3DRS_COLORWRITEENABLE, D3DRS_COLORWRITEENABLE1, D3DRS_COLORWRITEENABLE2, D3DRS_COLORWRITEENABLE3,
		D3DRS_SRGBWRITEENABLE, D3DRS_CLIPPLANEENABLE, D3DRS_FOGENABLE, D3DRS_FOGTABLEMODE, D3DRS_FOGVERTEXMODE, D3DRS_SHADEMODE,
		D3DRS_FILLMODE, D3DRS_POINTSPRITEENABLE, D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_ZFUNC, D3DRS_STENCILENABLE,
		D3DRS_MULTISAMPLEANTIALIAS, D3DRS_ADAPTIVETESS_Y, D3DRS_CULLMODE };
	enum { kStates = sizeof(KeyStates) / sizeof(KeyStates[0]), kStages = 20 };	// stages 0-15 pixel, 16-19 vertex texture samplers
	inline signed char StateSlot[256];

	struct Setup {
		const void* VS; const void* PS; const void* Decl; DWORD FVF;
		DWORD Rt[4]; DWORD RtMs; DWORD Ds; DWORD DsMs;
		DWORD State[kStates];
		DWORD TexFmt[kStages];
		BYTE TexType[kStages];
		BYTE Srgb[kStages];
	};
	inline Setup Cur = {};
	inline uint32_t VsSamplers = 0, PsSamplers = 0;
	inline uint64_t LastKey = 0;

	inline int StageIndex(DWORD stage) {
		if (stage < 16) return (int)stage;
		if (stage >= D3DVERTEXTEXTURESAMPLER0 && stage <= D3DVERTEXTEXTURESAMPLER3) return 16 + (int)(stage - D3DVERTEXTEXTURESAMPLER0);
		return -1;
	}
	inline DWORD DeviceStage(int index) { return index < 16 ? (DWORD)index : D3DVERTEXTEXTURESAMPLER0 + (DWORD)(index - 16); }
	inline void TargetOf(IDirect3DSurface9* s, DWORD& format, DWORD* ms) {
		D3DSURFACE_DESC d;
		if (s && SUCCEEDED(s->GetDesc(&d))) { format = d.Format; if (ms) *ms = (DWORD)d.MultiSampleType | (d.MultiSampleQuality << 8); }
		else { format = 0; if (ms) *ms = 0; }
	}

	inline unsigned long long StatRequeries = 0, StatApplies = 0;

	// Everything read back from the device (after a state block, a reset, or a call from another thread).
	inline void Requery(IDirect3DDevice9* d) {
		Unknown = false; Dirty = true; StatRequeries++;
		IDirect3DVertexShader9* vs = nullptr; d->GetVertexShader(&vs); Cur.VS = vs; VsSamplers = InfoOf(vs).Samplers; if (vs) vs->Release();
		IDirect3DPixelShader9* ps = nullptr; d->GetPixelShader(&ps); Cur.PS = ps; PsSamplers = InfoOf(ps).Samplers; if (ps) ps->Release();
		IDirect3DVertexDeclaration9* decl = nullptr; d->GetVertexDeclaration(&decl); Cur.Decl = decl; Cur.FVF = 0; if (decl) decl->Release();
		for (int i = 0; i < kStages; i++) {
			IDirect3DBaseTexture9* t = nullptr; d->GetTexture(DeviceStage(i), &t);
			TexOf(t, Cur.TexFmt[i], Cur.TexType[i]); if (t) t->Release();
			DWORD v = 0; d->GetSamplerState(DeviceStage(i), D3DSAMP_SRGBTEXTURE, &v); Cur.Srgb[i] = v ? 1 : 0;
		}
		for (int k = 0; k < kStates; k++) { DWORD v = 0; d->GetRenderState(KeyStates[k], &v); Cur.State[k] = v; }
		for (DWORD i = 0; i < 4; i++) {
			IDirect3DSurface9* s = nullptr;
			if (FAILED(d->GetRenderTarget(i, &s))) s = nullptr;
			TargetOf(s, Cur.Rt[i], i == 0 ? &Cur.RtMs : nullptr); if (s) s->Release();
		}
		IDirect3DSurface9* z = nullptr; if (FAILED(d->GetDepthStencilSurface(&z))) z = nullptr;
		TargetOf(z, Cur.Ds, &Cur.DsMs); if (z) z->Release();
	}

	// ---- records (what the list file holds) ----
	struct Record {
		uint64_t Id = 0;			// stable over sessions (shader hashes, declaration elements, the rest of the setup)
		float Ms = 0.0f;			// its first draw when recorded (the largest over the merged lists)
		uint64_t VsHash = 0, PsHash = 0;
		std::vector<D3DVERTEXELEMENT9> Decl;
		DWORD FVF = 0;
		WORD Stride[4] = {};
		DWORD Rt[4] = {}, RtMs = 0, Ds = 0, DsMs = 0;
		DWORD State[kStates] = {};
		DWORD TexFmt[kStages] = {};
		BYTE TexType[kStages] = {};
		BYTE Srgb[kStages] = {};
		const void* VsObj = nullptr; const void* PsObj = nullptr;	// this session (log names)
	};
	inline std::unordered_set<uint64_t> Seen;			// this session's setups (pointer keys)
	inline std::unordered_set<uint64_t> KnownIds;		// ids already in the list file (not written again)
	inline std::vector<Record> Pending;					// new slow setups not written yet
	inline unsigned NewLogged = 0;

	inline std::string ListPath() { return std::string(ShadersPath) + "Cache\\WarmUp2.txt"; }
	inline std::string SeedPath() { return std::string(ShadersPath) + "WarmUp2Seed.txt"; }

	inline uint64_t StableId(const Record& r) {
		uint64_t h = Hash64(&r.VsHash, 8);
		h = Hash64(&r.PsHash, 8, h);
		if (!r.Decl.empty()) h = Hash64(r.Decl.data(), r.Decl.size() * sizeof(D3DVERTEXELEMENT9), h);
		h = Hash64(&r.FVF, 4, h);
		h = Hash64(r.Rt, sizeof(r.Rt), h); h = Hash64(&r.RtMs, 4, h); h = Hash64(&r.Ds, 4, h); h = Hash64(&r.DsMs, 4, h);
		h = Hash64(r.State, sizeof(r.State), h);
		h = Hash64(r.TexFmt, sizeof(r.TexFmt), h); h = Hash64(r.TexType, sizeof(r.TexType), h); h = Hash64(r.Srgb, sizeof(r.Srgb), h);
		return h;
	}

	// One list line (without the line end).
	inline std::string FormatRecord(const Record& r) {
		char b[2048]; int n = 0;
		auto add = [&](const char* format, auto... args) { if (n < (int)sizeof(b) - 64) { const int k = _snprintf_s(b + n, sizeof(b) - n, _TRUNCATE, format, args...); if (k > 0) n += k; } };
		add("W2 %016llX %.2f %016llX %016llX %lX %lX %lX %lX %lX %lX %lX %lX %u %u %u %u s", (unsigned long long)r.Id, (double)r.Ms,
			(unsigned long long)r.VsHash, (unsigned long long)r.PsHash, r.FVF, r.Rt[0], r.Rt[1], r.Rt[2], r.Rt[3], r.RtMs, r.Ds, r.DsMs,
			(unsigned)r.Stride[0], (unsigned)r.Stride[1], (unsigned)r.Stride[2], (unsigned)r.Stride[3]);
		for (int k = 0; k < kStates; k++) add("%c%lX", k ? ',' : ':', r.State[k]);
		add(" t");
		bool first = true;
		for (int i = 0; i < kStages; i++) if (r.TexType[i]) { add("%c%d/%u/%lX/%u", first ? ':' : ',', i, (unsigned)r.TexType[i], r.TexFmt[i], (unsigned)r.Srgb[i]); first = false; }
		if (first) add(":-");
		add(" d");
		first = true;
		for (const D3DVERTEXELEMENT9& e : r.Decl) { add("%c%u/%u/%u/%u/%u/%u", first ? ':' : ',', (unsigned)e.Stream, (unsigned)e.Offset, (unsigned)e.Type, (unsigned)e.Method, (unsigned)e.Usage, (unsigned)e.UsageIndex); first = false; }
		if (first) add(":-");
		return std::string(b, n);
	}

	inline bool ParseRecord(const char* line, Record& r) {
		unsigned long long id, vs, ps; float ms; unsigned long v[8]; unsigned st[4]; int used = 0;
		if (sscanf_s(line, "W2 %llX %f %llX %llX %lX %lX %lX %lX %lX %lX %lX %lX %u %u %u %u s%n", &id, &ms, &vs, &ps, &r.FVF,
			&v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &st[0], &st[1], &st[2], &st[3], &used) < 16 || !used) return false;
		r.Id = id; r.Ms = ms; r.VsHash = vs; r.PsHash = ps;
		for (int i = 0; i < 4; i++) { r.Rt[i] = v[i]; r.Stride[i] = (WORD)st[i]; }
		r.RtMs = v[4]; r.Ds = v[5]; r.DsMs = v[6];
		const char* p = line + used;
		if (*p != ':') return false;
		for (int k = 0; k < kStates; k++) {
			if (*p != ':' && *p != ',') return false;
			char* end = nullptr; r.State[k] = strtoul(p + 1, &end, 16); p = end;
		}
		while (*p == ' ') p++;
		if (*p++ != 't') return false;
		if (p[0] == ':' && p[1] == '-') p += 2;
		else while (*p == ':' || *p == ',') {
			char* end = nullptr;
			const long stage = strtol(p + 1, &end, 10); if (*end != '/') return false;
			const unsigned long type = strtoul(end + 1, &end, 10); if (*end != '/') return false;
			const unsigned long fmt = strtoul(end + 1, &end, 16); if (*end != '/') return false;
			const unsigned long srgb = strtoul(end + 1, &end, 10);
			if (stage < 0 || stage >= kStages) return false;
			r.TexType[stage] = (BYTE)type; r.TexFmt[stage] = fmt; r.Srgb[stage] = (BYTE)srgb;
			p = end;
		}
		while (*p == ' ') p++;
		if (*p++ != 'd') return false;
		if (p[0] == ':' && p[1] == '-') p += 2;
		else while (*p == ':' || *p == ',') {
			unsigned long f[6]; char* end = (char*)p;
			for (int k = 0; k < 6; k++) { f[k] = strtoul(end + 1, &end, 10); if (k < 5 && *end != '/') return false; }
			D3DVERTEXELEMENT9 e = { (WORD)f[0], (WORD)f[1], (BYTE)f[2], (BYTE)f[3], (BYTE)f[4], (BYTE)f[5] };
			r.Decl.push_back(e);
			p = end;
		}
		if (!r.Decl.empty()) r.Decl.push_back(D3DDECL_END());
		return true;
	}

	inline void FlushPending() {
		if (Pending.empty()) return;
		FILE* f = nullptr;
		if (!fopen_s(&f, ListPath().c_str(), "a") && f) {
			for (const Record& r : Pending) { const std::string line = FormatRecord(r); fprintf(f, "%s\n", line.c_str()); if (LogRecords) Logger::Log("WARMUP2 REC %s", line.c_str()); }
			fclose(f);
		}
		Pending.clear();
	}

	// ---- names for the log (NVR's game shader records; effects and others print their hash) ----
	inline std::unordered_map<const void*, std::string> Names;
	inline bool NamesBuilt = false;
	inline void BuildNames() {
		NamesBuilt = true;
		if (!TheShaderManager) return;
		static const char* const kind[3] = { "", "(ext)", "(int)" };
		for (const auto& [CollectionName, Pointer] : TheShaderManager->ShaderNames) {
			ShaderCollection* Collection = Pointer ? *Pointer : nullptr;
			if (!Collection) continue;
			for (NiD3DPixelShaderEx* Shader : Collection->PixelShaderList) {
				if (!Shader || !Shader->Name) continue;
				if (Shader->ShaderHandleBackup) Names[Shader->ShaderHandleBackup] = std::string(Shader->Name) + "(vanilla)";
				for (int i = 0; i < 3; i++) { ShaderRecordPixel* R = Shader->GetShaderRecord((ShaderRecordType)i); if (R && R->ShaderHandle) Names[R->ShaderHandle] = std::string(Shader->Name) + kind[i]; }
			}
			for (NiD3DVertexShaderEx* Shader : Collection->VertexShaderList) {
				if (!Shader || !Shader->Name) continue;
				if (Shader->ShaderHandleBackup) Names[Shader->ShaderHandleBackup] = std::string(Shader->Name) + "(vanilla)";
				for (int i = 0; i < 3; i++) { ShaderRecordVertex* R = Shader->GetShaderRecord((ShaderRecordType)i); if (R && R->ShaderHandle) Names[R->ShaderHandle] = std::string(Shader->Name) + kind[i]; }
			}
		}
	}
	inline std::string NameOf(const void* shader, uint64_t hash) {
		if (!shader) return "none";
		if (!NamesBuilt) BuildNames();
		auto it = Names.find(shader);
		if (it != Names.end()) return it->second;
		char text[24]; sprintf_s(text, "#%08llX", (unsigned long long)(hash >> 32));
		return text;
	}

	// ---- frame accounting ----
	struct FrameCounters { unsigned Draws = 0, New = 0, SlowKnown = 0; double DrawMs = 0, NewMs = 0, NewMax = 0, SlowKnownMs = 0, HookMs = 0; };
	inline FrameCounters Frame;
	inline double LastTick = 0.0, LoadEnd = 0.0;
	inline int LastShot = -1;
	inline bool LoadingSeen = false;
	inline unsigned FrameLines = 0, TotalFrames = 0;
	struct Totals { unsigned Frames = 0, Over50 = 0, Over100 = 0, Over250 = 0, Over50Play = 0, Over100Play = 0, NewSetups = 0, NewSlow = 0, SlowKnown = 0;
		double NewMs = 0, SlowKnownMs = 0, HookMs = 0, MaxPlay = 0; };
	inline Totals Total;

	inline double TscMs(unsigned long long ticks) { return TicksPerMs > 0.0 ? (double)ticks / TicksPerMs : 0.0; }

	// ---- a new setup: build its record (render thread, at its first draw) ----
	inline void RecordNew(IDirect3DDevice9* d, double ms, UINT upStride) {
		Total.NewSetups++; Total.NewMs += ms; Frame.New++; Frame.NewMs += ms; if (ms > Frame.NewMax) Frame.NewMax = ms;
		if (ms >= 0.5) Total.NewSlow++;	// 'slow' in the TOTALS line: first draw >= 0.5 ms
		if (ms < SaveMinMs) return;
		Record r;
		r.VsObj = Cur.VS; r.PsObj = Cur.PS;
		r.VsHash = InfoOf((IDirect3DVertexShader9*)Cur.VS).Hash;
		r.PsHash = InfoOf((IDirect3DPixelShader9*)Cur.PS).Hash;
		r.FVF = Cur.FVF;
		IDirect3DVertexDeclaration9* decl = nullptr;
		if (!Cur.FVF && SUCCEEDED(d->GetVertexDeclaration(&decl)) && decl) {
			UINT count = 0;
			if (SUCCEEDED(decl->GetDeclaration(nullptr, &count)) && count && count < 64) { r.Decl.resize(count); if (FAILED(decl->GetDeclaration(r.Decl.data(), &count))) r.Decl.clear(); }
			decl->Release();
		}
		if (!r.Decl.empty() && r.Decl.back().Stream == 0xFF) r.Decl.pop_back();	// stored without the end marker
		for (UINT s = 0; s < 4; s++) {
			IDirect3DVertexBuffer9* vb = nullptr; UINT offset = 0, stride = 0;
			if (SUCCEEDED(d->GetStreamSource(s, &vb, &offset, &stride))) { if (vb) { r.Stride[s] = (WORD)stride; vb->Release(); } }
		}
		if (upStride) r.Stride[0] = (WORD)upStride;
		memcpy(r.Rt, Cur.Rt, sizeof(r.Rt)); r.RtMs = Cur.RtMs; r.Ds = Cur.Ds; r.DsMs = Cur.DsMs;
		memcpy(r.State, Cur.State, sizeof(r.State));
		const uint32_t used = PsSamplers | VsSamplers;
		for (int i = 0; i < kStages; i++) if (used & (1u << i)) { r.TexFmt[i] = Cur.TexFmt[i]; r.TexType[i] = Cur.TexType[i]; r.Srgb[i] = Cur.Srgb[i]; }
		r.Ms = (float)ms;
		r.Id = StableId(r);
		if (NewLogged < 300 && ms >= 2.0) {
			NewLogged++;
			char tex[160] = ""; size_t used2 = 0;
			for (int i = 0; i < kStages && used2 < sizeof(tex) - 24; i++) if (r.TexType[i]) used2 += sprintf_s(tex + used2, sizeof(tex) - used2, " s%d:%lX%s", i, r.TexFmt[i], r.Srgb[i] ? "s" : "");
			Logger::Log("WARMUP2 NEW %.1f ms: vs %s ps %s | rt %lX%s%s ms %lu depth %lX | alphatest %lu blend %lu clip %lu colorwrite %lX srgbw %lu | decl %u el fvf %lX |%s",
				ms, NameOf(Cur.VS, r.VsHash).c_str(), NameOf(Cur.PS, r.PsHash).c_str(), r.Rt[0], r.Rt[1] ? "+mrt" : "", r.Rt[2] ? "+" : "", r.RtMs & 0xFF, r.Ds,
				r.State[0], r.State[2], r.State[12], r.State[7], r.State[11], (unsigned)r.Decl.size(), r.FVF, tex);
		}
		if (KnownIds.insert(r.Id).second) Pending.push_back(std::move(r));
	}

	// ---- detours ----
	typedef HRESULT(STDMETHODCALLTYPE* CreateVSFn)(IDirect3DDevice9*, const DWORD*, IDirect3DVertexShader9**);
	typedef HRESULT(STDMETHODCALLTYPE* CreatePSFn)(IDirect3DDevice9*, const DWORD*, IDirect3DPixelShader9**);
	typedef HRESULT(STDMETHODCALLTYPE* CreateTexFn)(IDirect3DDevice9*, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DTexture9**, HANDLE*);
	typedef HRESULT(STDMETHODCALLTYPE* CreateVolFn)(IDirect3DDevice9*, UINT, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DVolumeTexture9**, HANDLE*);
	typedef HRESULT(STDMETHODCALLTYPE* CreateCubeFn)(IDirect3DDevice9*, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DCubeTexture9**, HANDLE*);
	typedef HRESULT(STDMETHODCALLTYPE* SetRTFn)(IDirect3DDevice9*, DWORD, IDirect3DSurface9*);
	typedef HRESULT(STDMETHODCALLTYPE* SetDSFn)(IDirect3DDevice9*, IDirect3DSurface9*);
	typedef HRESULT(STDMETHODCALLTYPE* SetRSFn)(IDirect3DDevice9*, D3DRENDERSTATETYPE, DWORD);
	typedef HRESULT(STDMETHODCALLTYPE* SetTexFn)(IDirect3DDevice9*, DWORD, IDirect3DBaseTexture9*);
	typedef HRESULT(STDMETHODCALLTYPE* SetSSFn)(IDirect3DDevice9*, DWORD, D3DSAMPLERSTATETYPE, DWORD);
	typedef HRESULT(STDMETHODCALLTYPE* DrawPFn)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT);
	typedef HRESULT(STDMETHODCALLTYPE* DrawIPFn)(IDirect3DDevice9*, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
	typedef HRESULT(STDMETHODCALLTYPE* DrawPUPFn)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, CONST void*, UINT);
	typedef HRESULT(STDMETHODCALLTYPE* DrawIPUPFn)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT, UINT, CONST void*, D3DFORMAT, CONST void*, UINT);
	typedef HRESULT(STDMETHODCALLTYPE* SetDeclFn)(IDirect3DDevice9*, IDirect3DVertexDeclaration9*);
	typedef HRESULT(STDMETHODCALLTYPE* SetFVFFn)(IDirect3DDevice9*, DWORD);
	typedef HRESULT(STDMETHODCALLTYPE* SetVSFn)(IDirect3DDevice9*, IDirect3DVertexShader9*);
	inline CreateVSFn OrigCreateVS = nullptr; inline CreatePSFn OrigCreatePS = nullptr;
	inline CreateTexFn OrigCreateTex = nullptr; inline CreateVolFn OrigCreateVol = nullptr; inline CreateCubeFn OrigCreateCube = nullptr;
	inline SetRTFn OrigSetRT = nullptr; inline SetDSFn OrigSetDS = nullptr; inline SetRSFn OrigSetRS = nullptr;
	inline SetTexFn OrigSetTex = nullptr; inline SetSSFn OrigSetSS = nullptr;
	inline DrawPFn OrigDrawP = nullptr; inline DrawIPFn OrigDrawIP = nullptr; inline DrawPUPFn OrigDrawPUP = nullptr; inline DrawIPUPFn OrigDrawIPUP = nullptr;
	inline SetDeclFn OrigSetDecl = nullptr; inline SetFVFFn OrigSetFVF = nullptr; inline SetVSFn OrigSetVS = nullptr;

	// Track a state call? Only on the render thread, outside state-block recording and our own warm-up; a call from another
	// thread makes the copy untrusted (read back at the next draw).
	inline bool Track() {
		if (Replaying) return false;
		const DWORD t = GetCurrentThreadId();
		if (t != Thread) { if (Thread) Unknown = true; return false; }
		return !ConstantFilter::Recording;
	}

	inline HRESULT STDMETHODCALLTYPE HookCreateVS(IDirect3DDevice9* self, const DWORD* code, IDirect3DVertexShader9** out) {
		const HRESULT r = OrigCreateVS(self, code, out);
		if (SUCCEEDED(r) && out && *out) { UINT bytes = 0; if (SUCCEEDED((*out)->GetFunction(nullptr, &bytes))) NoteShader(*out, code, bytes); }
		return r;
	}
	inline HRESULT STDMETHODCALLTYPE HookCreatePS(IDirect3DDevice9* self, const DWORD* code, IDirect3DPixelShader9** out) {
		const HRESULT r = OrigCreatePS(self, code, out);
		if (SUCCEEDED(r) && out && *out) { UINT bytes = 0; if (SUCCEEDED((*out)->GetFunction(nullptr, &bytes))) NoteShader(*out, code, bytes); }
		return r;
	}
	inline HRESULT STDMETHODCALLTYPE HookCreateTex(IDirect3DDevice9* self, UINT w, UINT h, UINT levels, DWORD usage, D3DFORMAT format, D3DPOOL pool, IDirect3DTexture9** out, HANDLE* shared) {
		const HRESULT r = OrigCreateTex(self, w, h, levels, usage, format, pool, out, shared);
		if (SUCCEEDED(r) && out && *out) PutTex(*out, format, D3DRTYPE_TEXTURE);
		return r;
	}
	inline HRESULT STDMETHODCALLTYPE HookCreateVol(IDirect3DDevice9* self, UINT w, UINT h, UINT depth, UINT levels, DWORD usage, D3DFORMAT format, D3DPOOL pool, IDirect3DVolumeTexture9** out, HANDLE* shared) {
		const HRESULT r = OrigCreateVol(self, w, h, depth, levels, usage, format, pool, out, shared);
		if (SUCCEEDED(r) && out && *out) PutTex(*out, format, D3DRTYPE_VOLUMETEXTURE);
		return r;
	}
	inline HRESULT STDMETHODCALLTYPE HookCreateCube(IDirect3DDevice9* self, UINT size, UINT levels, DWORD usage, D3DFORMAT format, D3DPOOL pool, IDirect3DCubeTexture9** out, HANDLE* shared) {
		const HRESULT r = OrigCreateCube(self, size, levels, usage, format, pool, out, shared);
		if (SUCCEEDED(r) && out && *out) PutTex(*out, format, D3DRTYPE_CUBETEXTURE);
		return r;
	}
	inline HRESULT STDMETHODCALLTYPE HookSetRT(IDirect3DDevice9* self, DWORD index, IDirect3DSurface9* surface) {
		const HRESULT r = OrigSetRT(self, index, surface);
		if (SUCCEEDED(r) && index < 4 && Track()) { TargetOf(surface, Cur.Rt[index], index == 0 ? &Cur.RtMs : nullptr); Dirty = true; }
		return r;
	}
	inline HRESULT STDMETHODCALLTYPE HookSetDS(IDirect3DDevice9* self, IDirect3DSurface9* surface) {
		const HRESULT r = OrigSetDS(self, surface);
		if (SUCCEEDED(r) && Track()) { TargetOf(surface, Cur.Ds, &Cur.DsMs); Dirty = true; }
		return r;
	}
	inline HRESULT STDMETHODCALLTYPE HookSetRS(IDirect3DDevice9* self, D3DRENDERSTATETYPE state, DWORD value) {
		const HRESULT r = OrigSetRS(self, state, value);
		if ((unsigned)state < 256) {
			const int slot = StateSlot[state];
			if (slot >= 0 && Cur.State[slot] != value && Track()) { Cur.State[slot] = value; Dirty = true; }
		}
		return r;
	}
	inline HRESULT STDMETHODCALLTYPE HookSetTex(IDirect3DDevice9* self, DWORD stage, IDirect3DBaseTexture9* texture) {
		const HRESULT r = OrigSetTex(self, stage, texture);
		const int i = StageIndex(stage);
		if (i >= 0 && Track()) {
			DWORD format; BYTE type; TexOf(texture, format, type);
			if (Cur.TexFmt[i] != format || Cur.TexType[i] != type) { Cur.TexFmt[i] = format; Cur.TexType[i] = type; Dirty = true; }
		}
		return r;
	}
	inline HRESULT STDMETHODCALLTYPE HookSetSS(IDirect3DDevice9* self, DWORD stage, D3DSAMPLERSTATETYPE type, DWORD value) {
		const HRESULT r = OrigSetSS(self, stage, type, value);
		if (type == D3DSAMP_SRGBTEXTURE) {
			const int i = StageIndex(stage);
			if (i >= 0 && Track() && Cur.Srgb[i] != (value ? 1 : 0)) { Cur.Srgb[i] = value ? 1 : 0; Dirty = true; }
		}
		return r;
	}
	inline HRESULT STDMETHODCALLTYPE HookSetDecl(IDirect3DDevice9* self, IDirect3DVertexDeclaration9* decl) {
		const HRESULT r = OrigSetDecl(self, decl);
		if (Track() && (Cur.Decl != decl || Cur.FVF)) { Cur.Decl = decl; Cur.FVF = 0; Dirty = true; }
		return r;
	}
	inline HRESULT STDMETHODCALLTYPE HookSetFVF(IDirect3DDevice9* self, DWORD fvf) {
		const HRESULT r = OrigSetFVF(self, fvf);
		if (Track() && (Cur.FVF != fvf || Cur.Decl)) { Cur.FVF = fvf; Cur.Decl = nullptr; Dirty = true; }
		return r;
	}
	inline HRESULT STDMETHODCALLTYPE HookSetVS(IDirect3DDevice9* self, IDirect3DVertexShader9* shader) {
		const HRESULT r = OrigSetVS(self, shader);
		if (Track() && Cur.VS != shader) { Cur.VS = shader; VsSamplers = InfoOf(shader).Samplers; Dirty = true; }
		return r;
	}
	// From ConstantFilter::HookSetPixelShader (that body is detoured there already).
	inline void OnSetPixelShader(IDirect3DPixelShader9* shader) {
		if (Track() && Cur.PS != shader) { Cur.PS = shader; PsSamplers = InfoOf(shader).Samplers; Dirty = true; }
	}
	// From ConstantFilter's state-block Apply, EndStateBlock and Reset hooks.
	inline void OnStateLost() { StatApplies++; Unknown = true; }

	// Before a draw: is this setup new? (render thread only)
	inline bool BeforeDraw(IDirect3DDevice9* d, unsigned long long& t0) {
		t0 = __rdtsc();
		if (Unknown) Requery(d);
		if (!ConstantFilter::Installed || ConstantFilter::Broken) {	// no pixel shader calls seen: read it
			IDirect3DPixelShader9* ps = nullptr; d->GetPixelShader(&ps);
			if (ps != Cur.PS) { Cur.PS = ps; PsSamplers = InfoOf(ps).Samplers; Dirty = true; }
			if (ps) ps->Release();
		}
		bool isNew = false;
		if (Dirty) {
			Dirty = false;
			Setup key = Cur;
			const uint32_t used = PsSamplers | VsSamplers;
			for (int i = 0; i < kStages; i++) if (!(used & (1u << i))) { key.TexFmt[i] = 0; key.TexType[i] = 0; key.Srgb[i] = 0; }
			static_assert(sizeof(Setup) % 4 == 0, "the key is hashed as 32-bit words");
			LastKey = KeyHash((const uint32_t*)&key, sizeof(key) / 4);
			isNew = Seen.insert(LastKey).second;
		}
		const unsigned long long t1 = __rdtsc();
		Frame.HookMs += TscMs(t1 - t0);
		t0 = t1;
		return isNew;
	}
	inline void AfterDraw(IDirect3DDevice9* d, unsigned long long t0, bool isNew, UINT upStride) {
		const double ms = TscMs(__rdtsc() - t0);
		Frame.Draws++; Frame.DrawMs += ms;
		if (isNew) RecordNew(d, ms, upStride);
		else if (ms > 2.0) { Frame.SlowKnown++; Frame.SlowKnownMs += ms; }
	}
	inline bool Watch() { return !Replaying && GetCurrentThreadId() == Thread; }

	inline HRESULT STDMETHODCALLTYPE HookDrawP(IDirect3DDevice9* self, D3DPRIMITIVETYPE type, UINT start, UINT count) {
		if (!Watch()) return OrigDrawP(self, type, start, count);
		unsigned long long t0; const bool isNew = BeforeDraw(self, t0);
		const HRESULT r = OrigDrawP(self, type, start, count);
		AfterDraw(self, t0, isNew, 0);
		return r;
	}
	inline HRESULT STDMETHODCALLTYPE HookDrawIP(IDirect3DDevice9* self, D3DPRIMITIVETYPE type, INT base, UINT minIndex, UINT vertices, UINT start, UINT count) {
		if (!Watch()) return OrigDrawIP(self, type, base, minIndex, vertices, start, count);
		unsigned long long t0; const bool isNew = BeforeDraw(self, t0);
		const HRESULT r = OrigDrawIP(self, type, base, minIndex, vertices, start, count);
		AfterDraw(self, t0, isNew, 0);
		return r;
	}
	inline HRESULT STDMETHODCALLTYPE HookDrawPUP(IDirect3DDevice9* self, D3DPRIMITIVETYPE type, UINT count, CONST void* data, UINT stride) {
		if (!Watch()) return OrigDrawPUP(self, type, count, data, stride);
		unsigned long long t0; const bool isNew = BeforeDraw(self, t0);
		const HRESULT r = OrigDrawPUP(self, type, count, data, stride);
		AfterDraw(self, t0, isNew, stride);
		return r;
	}
	inline HRESULT STDMETHODCALLTYPE HookDrawIPUP(IDirect3DDevice9* self, D3DPRIMITIVETYPE type, UINT minIndex, UINT vertices, UINT count, CONST void* indices, D3DFORMAT indexFormat, CONST void* data, UINT stride) {
		if (!Watch()) return OrigDrawIPUP(self, type, minIndex, vertices, count, indices, indexFormat, data, stride);
		unsigned long long t0; const bool isNew = BeforeDraw(self, t0);
		const HRESULT r = OrigDrawIPUP(self, type, minIndex, vertices, count, indices, indexFormat, data, stride);
		AfterDraw(self, t0, isNew, stride);
		return r;
	}

	// RenderManager::Initialize (the device exists, the game's and NVR's shaders are not created yet).
	inline void Install(IDirect3DDevice9* device) {
		if (Installed || Broken || !device) return;
		{
			SettingManager::Configuration::ConfigNode node;
			const char* section = "Main.Main.Performance";
			Mode = TheSettingManager->Config.FillNode(&node, section, "ShaderWarmUp2") ? max(0, min(node.IntValue, 2)) : 0;
			if (TheSettingManager->Config.FillNode(&node, section, "ShaderWarmUp2BudgetMs")) BudgetMs = std::clamp((double)node.FloatValue, 5.0, 2000.0);
			if (TheSettingManager->Config.FillNode(&node, section, "ShaderWarmUp2MinMs")) MinMs = std::clamp((double)node.FloatValue, 0.0, 1000.0);
		}
		if (Mode <= 0) return;
		ArrivalSampler::Start();	// test builds, ArrivalSampler = 1 (else nothing): sampling starts here, before the first world frame
		if (!strncmp(TheRenderManager->D3D9RuntimeDescription(), "DXVK", 4)) {
#if NVR_TEST_TOOLS
			// 2026-10-19: test builds keep the frame log under DXVK (no device detours, no list, no warm-up), so a DXVK run of a test
			// walk gives the same WARMUP2 FRAME / TOTALS lines as a native one (native vs DXVK arrival hitches).
			Installed = true; Mode = 1;
			CalTsc = __rdtsc(); CalMs = NowMs();
			Logger::Log("UNOFFICIAL ShaderWarmUp2: DXVK compiles shaders ahead; test build: frame log only (no recording, no warm-up)");
#else
			Logger::Log("UNOFFICIAL ShaderWarmUp2: DXVK compiles shaders ahead; not needed, off"); Mode = 0;
#endif
			return;
		}
		memset(StateSlot, -1, sizeof(StateSlot));
		for (int k = 0; k < kStates; k++) StateSlot[KeyStates[k]] = (signed char)k;
		KeepObjects = Mode >= 2;
		Seen.reserve(1 << 14);
		void** vt = *(void***)device;
		OrigSetRT = (SetRTFn)vt[37]; OrigSetDS = (SetDSFn)vt[39]; OrigSetRS = (SetRSFn)vt[57];
		OrigCreateTex = (CreateTexFn)vt[23]; OrigCreateVol = (CreateVolFn)vt[24]; OrigCreateCube = (CreateCubeFn)vt[25];
		OrigSetTex = (SetTexFn)vt[65]; OrigSetSS = (SetSSFn)vt[69];
		OrigDrawP = (DrawPFn)vt[81]; OrigDrawIP = (DrawIPFn)vt[82]; OrigDrawPUP = (DrawPUPFn)vt[83]; OrigDrawIPUP = (DrawIPUPFn)vt[84];
		OrigSetDecl = (SetDeclFn)vt[87]; OrigSetFVF = (SetFVFFn)vt[89];
		OrigCreateVS = (CreateVSFn)vt[91]; OrigSetVS = (SetVSFn)vt[92]; OrigCreatePS = (CreatePSFn)vt[106];
		DetourTransactionBegin();
		DetourUpdateThread(GetCurrentThread());
		DetourAttach(&(PVOID&)OrigSetRT, (PVOID)&HookSetRT);
		DetourAttach(&(PVOID&)OrigSetDS, (PVOID)&HookSetDS);
		DetourAttach(&(PVOID&)OrigSetRS, (PVOID)&HookSetRS);
		DetourAttach(&(PVOID&)OrigCreateTex, (PVOID)&HookCreateTex);
		DetourAttach(&(PVOID&)OrigCreateVol, (PVOID)&HookCreateVol);
		DetourAttach(&(PVOID&)OrigCreateCube, (PVOID)&HookCreateCube);
		DetourAttach(&(PVOID&)OrigSetTex, (PVOID)&HookSetTex);
		DetourAttach(&(PVOID&)OrigSetSS, (PVOID)&HookSetSS);
		DetourAttach(&(PVOID&)OrigDrawP, (PVOID)&HookDrawP);
		DetourAttach(&(PVOID&)OrigDrawIP, (PVOID)&HookDrawIP);
		DetourAttach(&(PVOID&)OrigDrawPUP, (PVOID)&HookDrawPUP);
		DetourAttach(&(PVOID&)OrigDrawIPUP, (PVOID)&HookDrawIPUP);
		DetourAttach(&(PVOID&)OrigSetDecl, (PVOID)&HookSetDecl);
		DetourAttach(&(PVOID&)OrigSetFVF, (PVOID)&HookSetFVF);
		DetourAttach(&(PVOID&)OrigCreateVS, (PVOID)&HookCreateVS);
		DetourAttach(&(PVOID&)OrigSetVS, (PVOID)&HookSetVS);
		DetourAttach(&(PVOID&)OrigCreatePS, (PVOID)&HookCreatePS);
		const LONG result = DetourTransactionCommit();
		if (result != NO_ERROR) { Broken = true; Mode = 0; Logger::Log("UNOFFICIAL ShaderWarmUp2: device detours failed (%ld); off", result); return; }
		Installed = true;
		ConstantFilter::PixelShaderObserver = &OnSetPixelShader;
		ConstantFilter::StateLostObserver = &OnStateLost;
		CalTsc = __rdtsc(); CalMs = NowMs();
		{ const unsigned long long t = __rdtsc(); const double m = NowMs(); while (NowMs() - m < 2.0) {} TicksPerMs = (double)(__rdtsc() - t) / (NowMs() - m); }
		// The ids already in the list, so a setup is written once.
		for (const std::string& path : { ListPath() }) {
			FILE* f = nullptr;
			if (fopen_s(&f, path.c_str(), "r") || !f) continue;
			char line[1024];
			while (fgets(line, sizeof(line), f)) { unsigned long long id = 0; if (sscanf_s(line, "W2 %llX", &id) == 1) KnownIds.insert(id); }
			fclose(f);
		}
		Logger::Log("UNOFFICIAL ShaderWarmUp2 mode %d (%s): drawing setups recorded at their first draw (list %s, %u setups in it)%s",
			Mode, Mode >= 2 ? "record + warm up at the main menu" : "record only", ListPath().c_str(), (unsigned)KnownIds.size(),
			Mode >= 2 ? "; the listed setups are drawn once at the main menu" : "");
	}

	// ---- the warm-up (mode 2, main menu) ----
	inline std::vector<Record> Queue;
	inline size_t Next = 0;
	inline bool WarmStarted = false, WarmDone = false;
	inline unsigned WarmExtra = 0;	// extra draws for other objects with the same bytecode
	inline unsigned WarmDrawFailed = 0, WarmOwnScenes = 0;	// draws the device refused; frames that needed their own BeginScene
	inline unsigned WarmFrames = 0, WarmDrawn = 0, WarmSkipShader = 0, WarmSkipTarget = 0, WarmSkipDecl = 0, WarmTexMissing = 0, WarmListed = 0, WarmBelow = 0;
	inline double WarmMs = 0, WarmCallMs = 0, WarmSlowest = 0, WarmFlushMs = 0;
	inline uint64_t WarmSlowestPs = 0;
	inline IDirect3DVertexBuffer9* ZeroVB = nullptr;
	inline IDirect3DQuery9* Event = nullptr;
	inline std::unordered_map<uint64_t, IDirect3DSurface9*> Targets;
	inline std::unordered_map<uint64_t, IDirect3DBaseTexture9*> Textures;
	inline std::unordered_map<uint64_t, IDirect3DVertexDeclaration9*> Decls;

	inline void LoadLists() {
		std::unordered_map<uint64_t, size_t> at;
		for (const std::string& path : { ListPath(), SeedPath() }) {
			FILE* f = nullptr;
			if (fopen_s(&f, path.c_str(), "r") || !f) continue;
			char line[1024];
			while (fgets(line, sizeof(line), f)) {
				Record r;
				if (!ParseRecord(line, r)) continue;
				WarmListed++;
				auto it = at.find(r.Id);
				if (it != at.end()) { if (r.Ms > Queue[it->second].Ms) Queue[it->second].Ms = r.Ms; continue; }
				at[r.Id] = Queue.size();
				Queue.push_back(std::move(r));
			}
			fclose(f);
		}
		const size_t all = Queue.size();
		Queue.erase(std::remove_if(Queue.begin(), Queue.end(), [](const Record& r) { return r.Ms < MinMs; }), Queue.end());
		WarmBelow = (unsigned)(all - Queue.size());
		std::stable_sort(Queue.begin(), Queue.end(), [](const Record& a, const Record& b) { return a.Ms > b.Ms; });
	}

	inline IUnknown* ObjectOf(uint64_t hash) {
		if (!hash) return nullptr;
		AcquireSRWLockShared(&InfoLock);
		auto it = Objects.find(hash);
		IUnknown* o = it != Objects.end() && !it->second.empty() ? it->second.front() : nullptr;
		ReleaseSRWLockShared(&InfoLock);
		return o;
	}
	inline std::vector<IUnknown*> ObjectsOf(uint64_t hash) {
		std::vector<IUnknown*> list;
		if (!hash) return list;
		AcquireSRWLockShared(&InfoLock);
		auto it = Objects.find(hash);
		if (it != Objects.end()) list = it->second;
		ReleaseSRWLockShared(&InfoLock);
		return list;
	}
	inline IDirect3DSurface9* TargetFor(IDirect3DDevice9* d, DWORD format, DWORD ms, bool depth) {
		const uint64_t key = ((uint64_t)format << 32) | ((uint64_t)ms << 1) | (depth ? 1 : 0);
		auto it = Targets.find(key);
		if (it != Targets.end()) return it->second;
		IDirect3DSurface9* s = nullptr;
		const D3DMULTISAMPLE_TYPE type = (D3DMULTISAMPLE_TYPE)(ms & 0xFF);
		const DWORD quality = ms >> 8;
		if (depth) { if (FAILED(d->CreateDepthStencilSurface(16, 16, (D3DFORMAT)format, type, quality, FALSE, &s, nullptr))) s = nullptr; }
		else if (FAILED(d->CreateRenderTarget(16, 16, (D3DFORMAT)format, type, quality, FALSE, &s, nullptr))) s = nullptr;
		// 2026-10-19: formats that only exist as textures (INTZ / DF24 depth, some float targets): level 0 of a 16x16 texture
		// (no MSAA there). The surface keeps its texture alive.
		if (!s && type == D3DMULTISAMPLE_NONE) {
			IDirect3DTexture9* t = nullptr;
			if (SUCCEEDED(d->CreateTexture(16, 16, 1, depth ? D3DUSAGE_DEPTHSTENCIL : D3DUSAGE_RENDERTARGET, (D3DFORMAT)format, D3DPOOL_DEFAULT, &t, nullptr)) && t) {
				if (FAILED(t->GetSurfaceLevel(0, &s))) s = nullptr;
				t->Release();
			}
		}
		if (!s) Logger::Log("Shader warm-up 2: no %s stand-in for format %lX, MSAA %lu/%lu (its setups are skipped)", depth ? "depth" : "render-target", format, (unsigned long)type, quality);
		Targets[key] = s;
		return s;
	}
	inline IDirect3DBaseTexture9* TextureFor(IDirect3DDevice9* d, BYTE type, DWORD format) {
		const uint64_t key = ((uint64_t)format << 8) | type;
		auto it = Textures.find(key);
		if (it != Textures.end()) return it->second;
		IDirect3DBaseTexture9* t = nullptr;
		const DWORD usages[3] = { 0, D3DUSAGE_DEPTHSTENCIL, D3DUSAGE_RENDERTARGET };
		for (DWORD usage : usages) {
			HRESULT hr = E_FAIL;
			if (type == D3DRTYPE_CUBETEXTURE) { IDirect3DCubeTexture9* c = nullptr; hr = d->CreateCubeTexture(4, 1, usage, (D3DFORMAT)format, D3DPOOL_DEFAULT, &c, nullptr); t = c; }
			else if (type == D3DRTYPE_VOLUMETEXTURE) { IDirect3DVolumeTexture9* v = nullptr; hr = d->CreateVolumeTexture(4, 4, 4, 1, usage, (D3DFORMAT)format, D3DPOOL_DEFAULT, &v, nullptr); t = v; }
			else { IDirect3DTexture9* x = nullptr; hr = d->CreateTexture(4, 4, 1, usage, (D3DFORMAT)format, D3DPOOL_DEFAULT, &x, nullptr); t = x; }
			if (SUCCEEDED(hr) && t) break;
			t = nullptr;
			if (type == D3DRTYPE_VOLUMETEXTURE) break;
		}
		Textures[key] = t;
		return t;
	}
	inline IDirect3DVertexDeclaration9* DeclFor(IDirect3DDevice9* d, const std::vector<D3DVERTEXELEMENT9>& elements) {
		const uint64_t key = Hash64(elements.data(), elements.size() * sizeof(D3DVERTEXELEMENT9));
		auto it = Decls.find(key);
		if (it != Decls.end()) return it->second;
		IDirect3DVertexDeclaration9* decl = nullptr;
		if (FAILED(d->CreateVertexDeclaration(elements.data(), &decl))) decl = nullptr;
		Decls[key] = decl;
		return decl;
	}

	// One listed setup, rebuilt on the device (inside the saved state). False: it cannot be rebuilt this session.
	inline bool Rebuild(IDirect3DDevice9* d, const Record& r) {
		IUnknown* vs = ObjectOf(r.VsHash);
		IUnknown* ps = ObjectOf(r.PsHash);
		if ((r.VsHash && !vs) || (r.PsHash && !ps)) {
			if (WarmSkipShader++ < 20) Logger::Log("Shader warm-up 2: setup %016llX (%.1f ms) skipped: %s shader %016llX not created this session", (unsigned long long)r.Id, (double)r.Ms, (r.VsHash && !vs) ? "vertex" : "pixel", (unsigned long long)((r.VsHash && !vs) ? r.VsHash : r.PsHash));
			return false;
		}
		IDirect3DSurface9* rt0 = r.Rt[0] ? TargetFor(d, r.Rt[0], r.RtMs, false) : nullptr;
		if (!rt0) { WarmSkipTarget++; return false; }
		IDirect3DSurface9* more[3] = {};
		for (int i = 1; i < 4; i++) if (r.Rt[i]) { more[i - 1] = TargetFor(d, r.Rt[i], r.RtMs, false); if (!more[i - 1]) { WarmSkipTarget++; return false; } }
		IDirect3DSurface9* depth = r.Ds ? TargetFor(d, r.Ds, r.DsMs, true) : nullptr;
		if (r.Ds && !depth) { WarmSkipTarget++; return false; }
		IDirect3DVertexDeclaration9* decl = nullptr;
		if (!r.FVF) {
			if (r.Decl.empty()) { WarmSkipDecl++; return false; }
			decl = DeclFor(d, r.Decl);
			if (!decl) { WarmSkipDecl++; return false; }
		}
		d->SetRenderTarget(0, rt0);
		for (int i = 1; i < 4; i++) d->SetRenderTarget(i, more[i - 1]);
		d->SetDepthStencilSurface(depth);
		D3DVIEWPORT9 vp = { 0, 0, 16, 16, 0.0f, 1.0f };
		d->SetViewport(&vp);
		d->SetVertexShader((IDirect3DVertexShader9*)vs);
		d->SetPixelShader((IDirect3DPixelShader9*)ps);
		if (r.FVF) d->SetFVF(r.FVF); else d->SetVertexDeclaration(decl);
		// Every stream the declaration reads gets the zero buffer (a stride of 64 when none was recorded), the others nothing.
		bool reads[4] = { r.FVF != 0, false, false, false };
		for (const D3DVERTEXELEMENT9& e : r.Decl) if (e.Stream < 4) reads[e.Stream] = true;
		for (UINT s = 0; s < 4; s++) {
			const UINT stride = reads[s] ? (r.Stride[s] ? r.Stride[s] : 64) : 0;
			d->SetStreamSource(s, stride ? ZeroVB : nullptr, 0, stride);
		}
		for (int i = 0; i < kStages; i++) {
			IDirect3DBaseTexture9* t = nullptr;
			if (r.TexType[i]) { t = TextureFor(d, r.TexType[i], r.TexFmt[i]); if (!t) WarmTexMissing++; }
			d->SetTexture(DeviceStage(i), t);
			d->SetSamplerState(DeviceStage(i), D3DSAMP_SRGBTEXTURE, r.Srgb[i]);
		}
		for (int k = 0; k < kStates; k++) d->SetRenderState(KeyStates[k], r.State[k]);
		if (r.State[12]) { const float keep[4] = { 0.0f, 0.0f, 0.0f, 1.0f }; for (DWORD p = 0; p < 6; p++) if (r.State[12] & (1u << p)) d->SetClipPlane(p, keep); }
		return true;
	}

	inline void ReleaseWarm() {
		for (auto& [k, s] : Targets) if (s) s->Release();
		for (auto& [k, t] : Textures) if (t) t->Release();
		for (auto& [k, v] : Decls) if (v) v->Release();
		Targets.clear(); Textures.clear(); Decls.clear();
		if (ZeroVB) ZeroVB->Release(); ZeroVB = nullptr;
		if (Event) Event->Release(); Event = nullptr;
		Queue.clear(); Queue.shrink_to_fit();
		AcquireSRWLockExclusive(&InfoLock);
		KeepObjects = false;
		for (auto& [h, list] : Objects) for (IUnknown* o : list) o->Release();
		Objects.clear();
		ReleaseSRWLockExclusive(&InfoLock);
	}

	inline void FinishWarm(const char* why) {
		WarmDone = true;
		if (WarmStarted)
			Logger::Log("Shader warm-up 2 %s: %u of %u setups drawn in %u main-menu frames (+%u draws for duplicate shader objects), %.0f ms in all (draw calls %.0f ms, waiting for the driver %.0f ms; slowest %.1f ms, pixel shader #%08llX) | not rebuilt: %u shader not found, %u target refused, %u declaration | %u texture stand-ins refused | list: %u lines, %u setups under %.1f ms left out | draws refused %u, own BeginScene in %u frames",
				why, WarmDrawn, (unsigned)Queue.size(), WarmFrames, WarmExtra, WarmMs, WarmCallMs, WarmFlushMs, WarmSlowest, (unsigned long long)(WarmSlowestPs >> 32),
				WarmSkipShader, WarmSkipTarget, WarmSkipDecl, WarmTexMissing, WarmListed, WarmBelow, MinMs, WarmDrawFailed, WarmOwnScenes);
		ReleaseWarm();
	}

	inline void WarmFrame(IDirect3DDevice9* d) {
		IDirect3DStateBlock9* saved = nullptr;
		if (FAILED(d->CreateStateBlock(D3DSBT_ALL, &saved)) || !saved) { FinishWarm("gave up (no state block)"); return; }
		IDirect3DSurface9* oldTargets[4] = {};
		IDirect3DSurface9* oldDepth = nullptr;
		for (DWORD i = 0; i < 4; i++) if (FAILED(d->GetRenderTarget(i, &oldTargets[i]))) oldTargets[i] = nullptr;
		if (FAILED(d->GetDepthStencilSurface(&oldDepth))) oldDepth = nullptr;
		D3DVIEWPORT9 oldViewport = {}; d->GetViewport(&oldViewport);
		RECT oldScissor = {}; d->GetScissorRect(&oldScissor);
		WarmFrames++;
		Replaying = true;
		// 2026-10-19: a draw outside BeginScene/EndScene is refused (and compiles nothing). Inside the game's scene BeginScene fails
		// harmlessly; when it succeeds we were outside one and close ours below.
		const bool ownScene = SUCCEEDED(d->BeginScene());
		if (ownScene) WarmOwnScenes++;
		const double start = NowMs();
		while (Next < Queue.size() && NowMs() - start < BudgetMs) {
			const Record& r = Queue[Next++];
			if (!Rebuild(d, r)) continue;
			const double t0 = NowMs();
			const HRESULT hr = d->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 1);
			if (FAILED(hr) && WarmDrawFailed++ < 5) Logger::Log("Shader warm-up 2: draw of setup %016llX refused (%08lX)", (unsigned long long)r.Id, (unsigned long)hr);
			// The same bytecode in more than one shader object (NVR's default / exterior / interior records can be identical):
			// each object once, in case the driver keeps compiled code per object.
			const std::vector<IUnknown*> moreVs = ObjectsOf(r.VsHash), morePs = ObjectsOf(r.PsHash);
			for (size_t k = 1; k < morePs.size(); k++) { d->SetPixelShader((IDirect3DPixelShader9*)morePs[k]); d->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 1); WarmExtra++; }
			if (morePs.size() > 1) d->SetPixelShader((IDirect3DPixelShader9*)morePs[0]);
			for (size_t k = 1; k < moreVs.size(); k++) { d->SetVertexShader((IDirect3DVertexShader9*)moreVs[k]); d->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 1); WarmExtra++; }
			const double ms = NowMs() - t0;
			WarmCallMs += ms; WarmDrawn++;
			if (ms > WarmSlowest) { WarmSlowest = ms; WarmSlowestPs = r.PsHash; }
		}
		if (ownScene) d->EndScene();
		// Wait until the driver has done what these draws asked (a compile on its own thread is then counted here, not in play).
		const double f0 = NowMs();
		if (Event && SUCCEEDED(Event->Issue(D3DISSUE_END))) { while (Event->GetData(nullptr, 0, D3DGETDATA_FLUSH) == S_FALSE && NowMs() - f0 < 2000.0) {} }
		WarmFlushMs += NowMs() - f0;
		for (DWORD i = 0; i < 4; i++) { d->SetRenderTarget(i, oldTargets[i]); if (oldTargets[i]) oldTargets[i]->Release(); }
		d->SetDepthStencilSurface(oldDepth);
		if (oldDepth) oldDepth->Release();
		Replaying = false;
		saved->Apply();
		saved->Release();
		d->SetViewport(&oldViewport);
		d->SetScissorRect(&oldScissor);
		Unknown = true;
		WarmMs += NowMs() - start;
		if (Next >= Queue.size()) FinishWarm("done");
	}

	// RenderInterfaceHook, every frame (menus are drawn here).
	inline void OnInterfaceFrame() {
		if (!Installed) return;
		if (InterfaceManager->IsActive(Menu::MenuType::kMenuType_Loading)) LoadingSeen = true;
		if (Mode < 2 || WarmDone) return;
		IDirect3DDevice9* d = TheRenderManager->device;
		if (!d) return;
		if (!Thread) Thread = GetCurrentThreadId();
		if (GetCurrentThreadId() != Thread) return;
		if (!InterfaceManager->IsActive(Menu::MenuType::kMenuType_Main)) { if (WarmStarted) FinishWarm("stopped (left the main menu)"); return; }
		if (!WarmStarted) {
			WarmStarted = true;
			LoadLists();
			if (Queue.empty()) { Logger::Log("Shader warm-up 2: nothing to warm up yet (%s fills as you play; %u listed setups, %u under %.1f ms)", ListPath().c_str(), WarmListed, WarmBelow, MinMs); FinishWarm("done"); return; }
			if (FAILED(d->CreateVertexBuffer(65536, D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT, &ZeroVB, nullptr)) || !ZeroVB) { FinishWarm("gave up (no vertex buffer)"); return; }
			void* p = nullptr;
			if (SUCCEEDED(ZeroVB->Lock(0, 0, &p, 0)) && p) { memset(p, 0, 65536); ZeroVB->Unlock(); }
			if (FAILED(d->CreateQuery(D3DQUERYTYPE_EVENT, &Event))) Event = nullptr;
			Logger::Log("Shader warm-up 2: %u setups to draw (first draw >= %.1f ms when recorded, slowest first), up to %.0f ms per main-menu frame",
				(unsigned)Queue.size(), MinMs, BudgetMs);
		}
		WarmFrame(d);
	}

	// RenderHook, at the start of each frame (render thread). shot = the test's shot index (-1 without a shot list).
	inline void FrameTick(int shot) {
		if (!Installed) return;
		Thread = GetCurrentThreadId();
		const double now = NowMs();
		if (now - CalMs > 200.0) TicksPerMs = (double)(__rdtsc() - CalTsc) / (now - CalMs);
		if (LastTick <= 0.0) ArrivalSampler::FirstTick();
		if (LastTick > 0.0) {
			const double interval = now - LastTick;
			const bool loading = interval > 1500.0;		// the interval held a loading screen (the game does not render then)
			const bool menu = InterfaceManager->IsActive(Menu::MenuType::kMenuType_Main);
			const bool picture = shot != LastShot;
			if (loading || LoadingSeen) LoadEnd = now;	// 'after load' counts from the last frame the loading menu was up
			TotalFrames++;
			if (!menu) {
				Total.Frames++;
				if (!loading) {
					if (interval > 50.0) Total.Over50++;
					if (interval > 100.0) Total.Over100++;
					if (interval > 250.0) Total.Over250++;
					if (!picture && now - LoadEnd > 3000.0) {
						if (interval > 50.0) Total.Over50Play++;
						if (interval > 100.0) Total.Over100Play++;
						if (interval > Total.MaxPlay) Total.MaxPlay = interval;
					}
				}
				Total.SlowKnown += Frame.SlowKnown; Total.SlowKnownMs += Frame.SlowKnownMs; Total.HookMs += Frame.HookMs;
			}
			if (interval > 25.0 && FrameLines < 3000) {
				FrameLines++;
				Logger::Log("WARMUP2 FRAME %.1f ms | new setups %u in %.1f ms (slowest %.1f) | known-setup draws over 2 ms %u in %.1f ms | draws %u in %.1f ms, recorder %.2f ms | %s%s%s | %.1f s after load | shot %d",
					interval, Frame.New, Frame.NewMs, Frame.NewMax, Frame.SlowKnown, Frame.SlowKnownMs, Frame.Draws, Frame.DrawMs, Frame.HookMs,
					loading ? "[loading] " : LoadingSeen ? "[loadmenu] " : "", menu ? "[menu] " : "", picture ? "[picture] " : "", (now - LoadEnd) / 1000.0, shot);
			}
			ArrivalSampler::OnFrame(interval, loading, menu, picture, LoadingSeen, shot, (now - LoadEnd) / 1000.0, Frame.DrawMs, Frame.NewMs, Frame.SlowKnownMs);	// test builds: ArrivalSampler = 1
			// 2026-10-19: also once when a test's shot list ends (shot index back to -1), so a run's analysis gets exact totals.
			if (TotalFrames % 600 == 0 || (shot < 0 && LastShot >= 0))
				Logger::Log("WARMUP2 TOTALS%s %u frames out of menus | over 50 ms %u, over 100 ms %u, over 250 ms %u (loading screens left out) | in play (3 s after a load, no picture frames): over 50 ms %u, over 100 ms %u, worst %.1f ms | new setups %u (%u slow, %.0f ms in their first draws) | known-setup draws over 2 ms %u (%.0f ms) | recorder %.3f ms per frame, state read back %llu times, state blocks %llu",
					(shot < 0 && LastShot >= 0) ? " (end of shots)" : "", Total.Frames, Total.Over50, Total.Over100, Total.Over250, Total.Over50Play, Total.Over100Play, Total.MaxPlay, Total.NewSetups, Total.NewSlow, Total.NewMs,
					Total.SlowKnown, Total.SlowKnownMs, Total.Frames ? Total.HookMs / Total.Frames : 0.0, StatRequeries, StatApplies);
			if (TotalFrames % 120 == 0 || (shot < 0 && LastShot >= 0)) FlushPending();	// (end of shots: the test ends the game soon after)
		}
		Frame = FrameCounters();
		LastTick = now; LastShot = shot; LoadingSeen = false;
	}
}
