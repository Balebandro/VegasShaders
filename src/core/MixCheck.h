#pragma once

// UNOFFICIAL 2026-10-30 [Main.Main.Performance] NvrDxvkMixCheck (diagnostic, off by default; read at launch). A tester's NVR DXVK runs
// with NvrDxvkKeepD3D9 lists blackened the world ("CombineDepth,Normals") or flickered ("AmbientOcclusion,SkyView,SunFar,SunShadows,
// ShadowsExteriors"); seven slot runs with the same settings never reproduced it. This makes the tester's own launch name the broken edge.
//
// Every render-target image (NVR's targets and the game's; a cube map counts as one image) is stamped with the frame, the path (D3D9 or the
// NVR DXVK extension) and the pass that last wrote it: D3D9 draws, clears, ColorFill and StretchRect (device table hooks, installed at
// launch only while this is on), the depth resolve (RenderManager::ResolveDepthBuffer), extension pass targets (EffectRecord::ExtFullScreen
// / ExtDrawTo) and storage bindings (NoteBindings, from DxvkCompute::PreLoad). Reads: a D3D9 draw's samplers that its pixel shader really
// samples (the stages SetTexture filled, checked at the draw), StretchRect sources, and an extension pass's sampled bindings (NoteBindings,
// called after the pass's own copies). The pass name is the last one SetCT (an effect, or a game shader NVR replaces) or an extension pass
// announced. One "MIXCHECK" line per image, reader and settings mode (NvrDxvkPasses + NvrDxvkKeepD3D9, so switching the list in the menu
// logs the new mode too):
//   EMPTY  a pass read an image nothing has written this session (black input);
//   STALE  a pass read an image last written N frames ago (1 is normal for history buffers: TAA, temporal AO / AL, cloud history);
//   EDGE   read on one path in the same frame it was written on the other (D3D9 -> Vulkan or Vulkan -> D3D9), the hand-overs to look at.
// Every 600 frames a "MIXCHECK intermittent" line (once per read) for reads that were stale in some frames and fresh in others: a flicker.
// EMPTY / STALE, EDGE and intermittent lines have their own caps, so the hand-overs never crowd out the empty and stale reads. File textures
// (no render-target usage) are never tracked. Depth-stencil images count as written by draws with depth writes on and by depth clears.
// No read is judged for 300 frames after the player changes cell and 120 after the mode changes (loads, arrival, the first frames of a
// new list would log what every mode logs). A normal frame still logs cached images (point-shadow cubes, atlas cascades) and ping-pong
// history buffers: the broken edge is what only the failing mode logs. tools/mixcheck_diff.py prints that from the log.
// Off (the default): nothing is hooked; one bool test at each call site.
#include <d3d9.h>
#include <unordered_map>
#include <string>

namespace MixCheck {
	enum : UInt8 { kNone = 0, kD3D9 = 1, kVulkan = 2 };
	inline bool On = false;					// read at launch (BeginFrame); the call sites test only this while off
	inline const char* Pass = "the game";	// the pass now drawing: the last SetCT (effect or replaced game shader) or extension pass

	void NoteWriteImpl(IDirect3DResource9* resource, UInt8 path, const char* writer);
	void NoteReadImpl(IDirect3DResource9* resource, UInt8 path);
	void BeginFrame(IDirect3DDevice9* d, bool shadowMapsMsaa);	// Render.cpp, once a frame (implemented there: MIXCHECK_IMPLEMENTATION)

	inline void SetPass(const char* name) { if (On && name && *name) Pass = name; }
	inline void NoteWrite(IDirect3DResource9* resource, UInt8 path, const char* writer = nullptr) { if (On) NoteWriteImpl(resource, path, writer); }
	inline void NoteRead(IDirect3DResource9* resource, UInt8 path) { if (On) NoteReadImpl(resource, path); }
	// An extension pass's bindings (NVR_EXT_RESOURCE_BINDING: Type, pResource): sampled images are its reads, storage images its writes.
	template <typename Binding> inline void NoteBindings(const Binding* bindings, UINT count, UInt32 sampledType, UInt32 storageType) {
		if (!On) return;
		for (UINT i = 0; bindings && i < count; i++) {
			IDirect3DResource9* resource = static_cast<IDirect3DResource9*>(static_cast<void*>(bindings[i].pResource));
			if (!resource) continue;
			if ((UInt32)bindings[i].Type == sampledType) NoteReadImpl(resource, kVulkan);
			else if ((UInt32)bindings[i].Type == storageType) NoteWriteImpl(resource, kVulkan, nullptr);
		}
	}
}

#ifdef MIXCHECK_IMPLEMENTATION
namespace MixCheck {
	struct Stamp { UInt32 frame = 0; UInt8 path = kNone; bool tracked = false; const char* writer = ""; };
	struct Count {
		UInt32 freshFrames = 0, staleFrames = 0, lastFrame = 0;
		bool frameStale = false, logged = false, intermittentLogged = false;
		std::string what;
	};

	inline bool Decided = false, Hooked = false;
	inline UInt32 Frame = 1, Serial = 0, QuietUntil = 0;	// reads before QuietUntil are not judged (warm-up)
	inline void* LastCell = nullptr;
	inline std::string Mode;
	inline std::unordered_map<void*, Stamp> Stamps;
	inline std::unordered_map<std::string, Count> Reads;
	inline std::unordered_map<void*, std::string> Names;
	inline std::unordered_map<IDirect3DPixelShader9*, UInt16> SamplerMasks;	// the samplers each pixel shader samples (bit = stage)
	inline IDirect3DBaseTexture9* Bound[16] = {};	// the pixel stages as SetTexture left them (keys only, no reference held)
	inline UInt32 LinesRead = 0, LinesEdge = 0, LinesIntermittent = 0;
	static const UInt32 kMaxRead = 250, kMaxEdge = 150, kMaxIntermittent = 60;
	// {6f1c2a4e-9b7d-4c2e-a5d1-3e8b0f6a7c19}: the stamp's tag on the object, so an object created at a freed one's address starts afresh
	static const GUID kTag = { 0x6f1c2a4e, 0x9b7d, 0x4c2e, { 0xa5, 0xd1, 0x3e, 0x8b, 0x0f, 0x6a, 0x7c, 0x19 } };

	inline const char* PathName(UInt8 path) { return path == kVulkan ? "Vulkan" : path == kD3D9 ? "D3D9" : "nothing"; }

	// The image a surface or texture belongs to: a 2D texture's surfaces count as the texture, a cube map's faces as the cube (the same
	// pointer SetTexture and the extension's bindings carry). Null for anything else.
	inline void* KeyOf(IDirect3DResource9* resource) {
		if (!resource) return nullptr;
		const D3DRESOURCETYPE type = resource->GetType();
		if (type == D3DRTYPE_TEXTURE || type == D3DRTYPE_CUBETEXTURE) return resource;
		if (type != D3DRTYPE_SURFACE) return nullptr;
		IDirect3DSurface9* surface = static_cast<IDirect3DSurface9*>(resource);
		IDirect3DTexture9* texture = nullptr;
		if (SUCCEEDED(surface->GetContainer(__uuidof(IDirect3DTexture9), (void**)&texture)) && texture) { texture->Release(); return texture; }
		IDirect3DCubeTexture9* cube = nullptr;
		if (SUCCEEDED(surface->GetContainer(__uuidof(IDirect3DCubeTexture9), (void**)&cube)) && cube) { cube->Release(); return cube; }
		return resource;
	}

	// The image's stamp. A key whose object lacks the tag (first sight, or a new object at a freed one's address) starts afresh.
	inline Stamp& StampOf(void* key) {
		IDirect3DResource9* object = static_cast<IDirect3DResource9*>(key);
		Stamp& s = Stamps[key];
		UInt32 tag = 0;
		DWORD size = sizeof(tag);
		if (FAILED(object->GetPrivateData(kTag, &tag, &size))) {
			s = Stamp();
			Names.erase(key);
			DWORD usage = 0;
			const D3DRESOURCETYPE type = object->GetType();
			D3DSURFACE_DESC d = {};
			if (type == D3DRTYPE_TEXTURE && SUCCEEDED(static_cast<IDirect3DTexture9*>(object)->GetLevelDesc(0, &d))) usage = d.Usage;
			else if (type == D3DRTYPE_CUBETEXTURE && SUCCEEDED(static_cast<IDirect3DCubeTexture9*>(object)->GetLevelDesc(0, &d))) usage = d.Usage;
			else if (type == D3DRTYPE_SURFACE && SUCCEEDED(static_cast<IDirect3DSurface9*>(object)->GetDesc(&d))) usage = d.Usage;
			s.tracked = (usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL)) != 0;
			tag = ++Serial;
			object->SetPrivateData(kTag, &tag, sizeof(tag), 0);
		}
		return s;
	}

	// The TESR_ name the image is registered under (the frame chain's images move between names: looked up again when not found).
	inline const std::string& NameOf(void* key) {
		auto found = Names.find(key);
		if (found != Names.end()) return found->second;
		for (const auto& entry : TheTextureManager->TextureNames)
			if (entry.second && *entry.second == key) return Names[key] = entry.first;
		char text[32];
		_snprintf_s(text, sizeof(text), _TRUNCATE, "image %p", key);
		return Names[key] = text;	// (an image of the game or one NVR does not register by name)
	}

	inline void Log(UInt32& lines, UInt32 cap, const char* kind, const std::string& line) {
		if (lines >= cap) return;
		if (++lines == cap) Logger::Log("MIXCHECK %s: %u lines, no more of this kind this session", kind, cap);
		else Logger::Log("MIXCHECK %s %s", kind, line.c_str());
	}

	void NoteWriteImpl(IDirect3DResource9* resource, UInt8 path, const char* writer) {
		void* key = KeyOf(resource);
		if (!key) return;
		Stamp& s = StampOf(key);
		s.frame = Frame;
		s.path = path;
		s.writer = writer && *writer ? writer : Pass;
	}

	void NoteReadImpl(IDirect3DResource9* resource, UInt8 path) {
		if (Frame < QuietUntil) return;
		void* key = KeyOf(resource);
		if (!key) return;
		const Stamp& s = StampOf(key);
		if (!s.tracked) return;
		char id[64];
		_snprintf_s(id, sizeof(id), _TRUNCATE, "%p|%p|%u|", key, (const void*)Pass, (unsigned)path);
		Count& c = Reads[id + Mode];
		if (c.what.empty()) c.what = NameOf(key) + " read by " + Pass + " (" + PathName(path) + ")";
		const bool empty = s.path == kNone, stale = !empty && s.frame != Frame, edge = !empty && !stale && s.path != path;
		// Judged per frame: a frame counts as stale when every read of it this frame was stale or empty.
		if (c.lastFrame != Frame) {
			if (c.lastFrame) { if (c.frameStale) c.staleFrames++; else c.freshFrames++; }
			c.lastFrame = Frame;
			c.frameStale = empty || stale;
		}
		else c.frameStale = c.frameStale && (empty || stale);
		if (c.logged || !(empty || stale || edge)) return;
		c.logged = true;
		char tail[160];
		if (empty) _snprintf_s(tail, sizeof(tail), _TRUNCATE, ": nothing wrote it this session (frame %u) [%s]", Frame, Mode.c_str());
		else if (stale) _snprintf_s(tail, sizeof(tail), _TRUNCATE, ": last written %u frame(s) ago by %s (%s) [%s]", Frame - s.frame, s.writer, PathName(s.path), Mode.c_str());
		else _snprintf_s(tail, sizeof(tail), _TRUNCATE, ": written this frame by %s (%s) [%s]", s.writer, PathName(s.path), Mode.c_str());
		if (edge) Log(LinesEdge, kMaxEdge, "EDGE ", c.what + tail);
		else Log(LinesRead, kMaxRead, empty ? "EMPTY" : "STALE", c.what + tail);
	}

	// ---- device table hooks (installed once, at launch, only while on) ----
	typedef HRESULT(STDMETHODCALLTYPE* DPFn)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT);
	typedef HRESULT(STDMETHODCALLTYPE* DIPFn)(IDirect3DDevice9*, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
	typedef HRESULT(STDMETHODCALLTYPE* DPUPFn)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, const void*, UINT);
	typedef HRESULT(STDMETHODCALLTYPE* DIPUPFn)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT, UINT, const void*, D3DFORMAT, const void*, UINT);
	typedef HRESULT(STDMETHODCALLTYPE* ClearFn)(IDirect3DDevice9*, DWORD, const D3DRECT*, DWORD, D3DCOLOR, float, DWORD);
	typedef HRESULT(STDMETHODCALLTYPE* ColorFillFn)(IDirect3DDevice9*, IDirect3DSurface9*, const RECT*, D3DCOLOR);
	typedef HRESULT(STDMETHODCALLTYPE* StretchRectFn)(IDirect3DDevice9*, IDirect3DSurface9*, const RECT*, IDirect3DSurface9*, const RECT*, D3DTEXTUREFILTERTYPE);
	typedef HRESULT(STDMETHODCALLTYPE* SetTextureFn)(IDirect3DDevice9*, DWORD, IDirect3DBaseTexture9*);
	inline DPFn NextDP = nullptr;  inline DIPFn NextDIP = nullptr;  inline DPUPFn NextDPUP = nullptr;  inline DIPUPFn NextDIPUP = nullptr;
	inline ClearFn NextClear = nullptr;  inline ColorFillFn NextColorFill = nullptr;  inline StretchRectFn NextStretchRect = nullptr;
	inline SetTextureFn NextSetTexture = nullptr;

	// The samplers a pixel shader samples, from its constant table (made once per shader).
	inline UInt16 SamplerMask(IDirect3DPixelShader9* shader) {
		auto found = SamplerMasks.find(shader);
		if (found != SamplerMasks.end()) return found->second;
		UInt16 mask = 0;
		UINT size = 0;
		if (SUCCEEDED(shader->GetFunction(nullptr, &size)) && size) {
			std::string code(size, '\0');
			ID3DXConstantTable* table = nullptr;
			if (SUCCEEDED(shader->GetFunction(&code[0], &size)) && SUCCEEDED(D3DXGetShaderConstantTable((const DWORD*)code.data(), &table)) && table) {
				D3DXCONSTANTTABLE_DESC desc = {};
				table->GetDesc(&desc);
				for (UINT i = 0; i < desc.Constants; i++) {
					D3DXCONSTANT_DESC c = {};
					UINT n = 1;
					if (FAILED(table->GetConstantDesc(table->GetConstant(NULL, i), &c, &n)) || c.RegisterSet != D3DXRS_SAMPLER) continue;
					for (UINT r = c.RegisterIndex; r < c.RegisterIndex + max(c.RegisterCount, 1u) && r < 16; r++) mask |= (UInt16)(1u << r);
				}
				table->Release();
			}
			else mask = 0xFFFF;	// no constant table (stripped): every bound stage counts
		}
		return SamplerMasks[shader] = mask;
	}

	// A draw: the colour targets it writes (render targets 0-3 that are set) and the bound textures its pixel shader samples.
	inline void BeforeDraw(IDirect3DDevice9* d) {
		IDirect3DPixelShader9* ps = nullptr;
		if (SUCCEEDED(d->GetPixelShader(&ps)) && ps) {
			const UInt16 mask = SamplerMask(ps);
			ps->Release();
			for (DWORD stage = 0; stage < 16; stage++) if ((mask >> stage) & 1 && Bound[stage]) NoteReadImpl(Bound[stage], kD3D9);
		}
		DWORD zEnable = 0, zWrite = 0;
		if (SUCCEEDED(d->GetRenderState(D3DRS_ZENABLE, &zEnable)) && zEnable && SUCCEEDED(d->GetRenderState(D3DRS_ZWRITEENABLE, &zWrite)) && zWrite) {
			IDirect3DSurface9* depth = nullptr;
			if (SUCCEEDED(d->GetDepthStencilSurface(&depth)) && depth) { NoteWriteImpl(depth, kD3D9, nullptr); depth->Release(); }	// shadow maps drawn as depth
		}
		DWORD write = 0xF;
		if (SUCCEEDED(d->GetRenderState(D3DRS_COLORWRITEENABLE, &write)) && !(write & 0xF)) return;	// depth only
		for (DWORD i = 0; i < 4; i++) {
			IDirect3DSurface9* target = nullptr;
			if (FAILED(d->GetRenderTarget(i, &target)) || !target) continue;
			NoteWriteImpl(target, kD3D9, nullptr);
			target->Release();
		}
	}
	inline HRESULT STDMETHODCALLTYPE DPHook(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, UINT s, UINT n) { BeforeDraw(d); return NextDP(d, t, s, n); }
	inline HRESULT STDMETHODCALLTYPE DIPHook(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, INT b, UINT mi, UINT nv, UINT s, UINT n) { BeforeDraw(d); return NextDIP(d, t, b, mi, nv, s, n); }
	inline HRESULT STDMETHODCALLTYPE DPUPHook(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, UINT n, const void* v, UINT st) { BeforeDraw(d); return NextDPUP(d, t, n, v, st); }
	inline HRESULT STDMETHODCALLTYPE DIPUPHook(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, UINT mi, UINT nv, UINT n, const void* i, D3DFORMAT f, const void* v, UINT st) {
		BeforeDraw(d); return NextDIPUP(d, t, mi, nv, n, i, f, v, st); }
	inline HRESULT STDMETHODCALLTYPE ClearHook(IDirect3DDevice9* d, DWORD count, const D3DRECT* rects, DWORD flags, D3DCOLOR color, float z, DWORD stencil) {
		if (flags & (D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL)) {
			IDirect3DSurface9* depth = nullptr;
			if (SUCCEEDED(d->GetDepthStencilSurface(&depth)) && depth) { NoteWriteImpl(depth, kD3D9, nullptr); depth->Release(); }
		}
		if (flags & D3DCLEAR_TARGET) {
			for (DWORD i = 0; i < 4; i++) {
				IDirect3DSurface9* target = nullptr;
				if (FAILED(d->GetRenderTarget(i, &target)) || !target) continue;
				NoteWriteImpl(target, kD3D9, nullptr);
				target->Release();
			}
		}
		return NextClear(d, count, rects, flags, color, z, stencil);
	}
	inline HRESULT STDMETHODCALLTYPE ColorFillHook(IDirect3DDevice9* d, IDirect3DSurface9* surface, const RECT* rect, D3DCOLOR color) {
		NoteWriteImpl(surface, kD3D9, nullptr);
		return NextColorFill(d, surface, rect, color);
	}
	inline HRESULT STDMETHODCALLTYPE StretchRectHook(IDirect3DDevice9* d, IDirect3DSurface9* source, const RECT* sourceRect, IDirect3DSurface9* dest, const RECT* destRect, D3DTEXTUREFILTERTYPE filter) {
		NoteReadImpl(source, kD3D9);
		NoteWriteImpl(dest, kD3D9, nullptr);
		return NextStretchRect(d, source, sourceRect, dest, destRect, filter);
	}
	inline HRESULT STDMETHODCALLTYPE SetTextureHook(IDirect3DDevice9* d, DWORD stage, IDirect3DBaseTexture9* texture) {
		if (stage < 16) Bound[stage] = texture;	// checked when a draw samples it (BeforeDraw)
		return NextSetTexture(d, stage, texture);
	}

	inline void Hook(IDirect3DDevice9* d) {
		void** table = *(void***)d;
		struct Slot { int index; void* hook; void** next; };
		const Slot slots[] = { { 34, (void*)&StretchRectHook, (void**)&NextStretchRect }, { 35, (void*)&ColorFillHook, (void**)&NextColorFill },
			{ 43, (void*)&ClearHook, (void**)&NextClear }, { 65, (void*)&SetTextureHook, (void**)&NextSetTexture },
			{ 81, (void*)&DPHook, (void**)&NextDP }, { 82, (void*)&DIPHook, (void**)&NextDIP }, { 83, (void*)&DPUPHook, (void**)&NextDPUP },
			{ 84, (void*)&DIPUPHook, (void**)&NextDIPUP } };
		for (const Slot& s : slots) {
			DWORD old = 0;
			if (!VirtualProtect(&table[s.index], sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) { Logger::Log("MIXCHECK: device slot %d could not be hooked: off", s.index); On = false; return; }
			*s.next = table[s.index];
			table[s.index] = s.hook;
			VirtualProtect(&table[s.index], sizeof(void*), old, &old);
		}
		for (DWORD stage = 0; stage < 16; stage++) {	// what is bound now (set before the hook went in)
			IDirect3DBaseTexture9* texture = nullptr;
			if (SUCCEEDED(d->GetTexture(stage, &texture)) && texture) { Bound[stage] = texture; texture->Release(); }
		}
		Hooked = true;
	}

	// Render.cpp, once a frame (after LinearScene / CapturedNormals hook the same slots: their hooks run inside these).
	void BeginFrame(IDirect3DDevice9* d, bool shadowMapsMsaa) {
		if (!Decided) {
			Decided = true;
			// The setup the tester's reports depend on, one line, whether or not the check is on.
			// (A toml's old [Main.Main.Misc] InvertedDepth key is not in the defaults, so the settings cannot see it: the line says only that
			// NVR goes by the game's depth clear.)
			IDirect3DSurface9* depth = nullptr;
			D3DSURFACE_DESC depthDesc = {};
			if (SUCCEEDED(d->GetDepthStencilSurface(&depth)) && depth) { depth->GetDesc(&depthDesc); depth->Release(); }
			const bool on = TheSettingManager->GetSettingI("Main.Main.Performance", "NvrDxvkMixCheck") != 0;
			Logger::Log("UNOFFICIAL NVR DXVK setup: %s; NvrDxvkPasses %s, NvrDxvkKeepD3D9 \"%s\"; reversed depth %s (from the game's depth clear %.1f; an "
				"InvertedDepth key in the toml is not read by this build); game depth buffer %ux%u multisample %u; exterior shadow maps MSAA %s; NvrDxvkMixCheck %s",
				DxvkCompute::Status(), TheSettingManager->SettingsMain.Main.NvrDxvkPasses ? "on" : "off", TheSettingManager->SettingsMain.Main.NvrDxvkKeepD3D9,
				TheRenderManager->IsReversedDepth() ? "yes" : "no", NiDX9Renderer::GetSingleton()->m_fZClear,
				depthDesc.Width, depthDesc.Height, (unsigned)depthDesc.MultiSampleType, shadowMapsMsaa ? "on" : "off", on ? "on" : "off");
			On = on;
			if (On) Hook(d);
			if (On) Logger::Log("MIXCHECK: on (one line per image, reader and mode: EMPTY = nothing wrote it, STALE = written N frames ago, EDGE = written on the other path this frame)");
		}
		if (!On) return;
		Frame++;
		char mode[192];
		_snprintf_s(mode, sizeof(mode), _TRUNCATE, "passes %s, keep \"%s\"", DxvkCompute::PassesOn() ? "Vulkan" : "D3D9", TheSettingManager->SettingsMain.Main.NvrDxvkKeepD3D9);
		if (Mode != mode) QuietUntil = (std::max)(QuietUntil, Frame + 120);	// a new list: its first frames settle
		Mode = mode;
		Pass = "the game";
		void* cell = Player ? (void*)Player->parentCell : nullptr;
		if (cell != LastCell) { LastCell = cell; QuietUntil = (std::max)(QuietUntil, Frame + 300); }	// a load or a door: the arrival settles
		if (Frame % 600 == 0) {
			for (auto& entry : Reads) {
				Count& c = entry.second;
				if (!c.intermittentLogged && c.staleFrames && c.freshFrames) {
					c.intermittentLogged = true;
					char line[112];
					_snprintf_s(line, sizeof(line), _TRUNCATE, ": stale or empty in %u of %u frames (600-frame window)", c.staleFrames, c.staleFrames + c.freshFrames);
					Log(LinesIntermittent, kMaxIntermittent, "intermittent", c.what + line);
				}
				c.staleFrames = c.freshFrames = 0;
			}
		}
	}
}
#endif
