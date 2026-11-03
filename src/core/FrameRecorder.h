#pragma once

// UNOFFICIAL, TEST ONLY (2026-10-17): records ONE whole game frame, Present to Present, into a .nvrf file (FrameFormat.h) that
// tests/frame_replay.exe plays back on its own. Started only by the shot list's 'record' command (SettingShots.h); nothing here
// runs, and no hook is installed, in normal play.
//
// How: Detours on the device's ACTION functions (draws, Clear, StretchRect, ColorFill, UpdateSurface, UpdateTexture, BeginScene,
// EndScene, Present) and on the resources' Lock/Unlock functions, taken from the vtables of the real objects (Windows' d3d9 puts
// vtable slots back after load, so vtable patches do not stick). At each action the FULL device state is read back with Get* and
// only the words that changed since the previous action are written, so state set by state blocks (D3DX effects) is caught too.
// Each resource is described at its first reference and its contents are copied at its first use in the frame (textures,
// buffers, shaders, declarations; render targets read before written are read back from the GPU). Writes the game makes during
// the frame (Lock/Unlock on the render thread) are recorded in call order. NvAPI_D3D9_StretchRectEx, which bypasses the device,
// is recorded by RenderManager::ResolveDepthBuffer (NvStretchRect below). Names: NVR's GPU timers (begin/end markers), the
// F10 shader-family split (Render.cpp ShaderSplit marks), the game's shader names at each bind (SetShadersHook) and NVR's
// effect passes (by bytecode hash).
//
// The hooks stay installed (inert, one test) after the first recording: removing a detour while another thread may be inside it
// is not safe, and this only ever runs in an auto-test launch. Recording stalls the game for seconds; it is test-only.
#include <cstdarg>
#include <cstdio>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <string>
#include <tlhelp32.h>
#include "FrameFormat.h"
#include "EffectSelfTestCore.h"

namespace FrameRecorder {
	using namespace FrameFormat;

	enum Status { kIdle, kArmed, kRecording, kDone, kFailed };
	inline volatile Status State = kIdle;
	inline char Path[MAX_PATH] = "";
	inline DWORD Thread = 0;
	inline thread_local int Depth = 0;	// inside our own work, or inside a hooked call already: pass straight through
	inline bool Installed = false, InstallFailed = false;
	inline IDirect3DDevice9* Device = nullptr;

	struct Res {
		DWORD id = 0;
		D3DRESOURCETYPE type = D3DRTYPE_SURFACE;
		IUnknown* obj = nullptr;
		bool content = false;	// contents written (or known not needed)
		bool written = false;	// written by the GPU earlier in this frame
		DWORD parent = 0;		// sub-surfaces: the texture
		DWORD level = 0, face = 0;
	};

	struct Recorder {
		FILE* file = nullptr;
		std::unordered_map<const void*, DWORD> ids;
		std::vector<Res> res;	// [id]
		FrameState last = {}, cur = {};
		bool haveLast = false;
		std::unordered_map<unsigned long long, std::string> hashNames;	// effect pass bytecode -> name
		std::unordered_map<const void*, std::string> pendingNames;		// shader pointer -> name (before its id exists)
		std::unordered_set<DWORD> named;
		struct Lock { void* ptr = nullptr; DWORD offset = 0, size = 0, flags = 0; RECT rect = {}; bool whole = true; int pitch = 0, slice = 0; D3DBOX box = {}; };
		std::unordered_map<unsigned long long, Lock> locks;	// (object, level, face) -> open lock
		unsigned long long bytes = 0;
		unsigned draws = 0, actions = 0, updates = 0, readbacks = 0, depthUnknown = 0, unreadable = 0, marks = 0, stateRecords = 0, journaled = 0;
		unsigned kinds[8] = {};
		double startMs = 0.0;
		std::string notes;
	};
	inline Recorder* R = nullptr;

	inline double NowMs() { LARGE_INTEGER f, c; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c); return c.QuadPart * 1000.0 / f.QuadPart; }

	inline bool Active() { return State == kRecording && R && GetCurrentThreadId() == Thread; }

	struct Guard { Guard() { Depth++; } ~Guard() { Depth--; } };

	// ---- file ----
	inline void Write(const void* data, size_t bytes) { if (R && R->file && bytes) { fwrite(data, 1, bytes, R->file); R->bytes += bytes; } }
	inline void Begin(DWORD type, size_t bytes) { DWORD h[2] = { type, (DWORD)bytes }; Write(h, sizeof(h)); }
	inline void Note(const char* format, ...) {
		if (!R || R->notes.size() > 6000) return;
		char line[512]; va_list a; va_start(a, format); _vsnprintf_s(line, _TRUNCATE, format, a); va_end(a);
		R->notes += line; R->notes += "\n";
	}

	// ---- ids and descriptions ----
	inline DWORD Find(const void* p) { if (!p || !R) return 0; auto f = R->ids.find(p); return f == R->ids.end() ? 0 : f->second; }
	inline DWORD NewId(IUnknown* obj, D3DRESOURCETYPE type) {
		const DWORD id = (DWORD)R->res.size();
		Res r; r.id = id; r.type = type; r.obj = obj;
		obj->AddRef();	// held until the end of the frame: no address is reused while we map it
		R->res.push_back(r);
		R->ids[obj] = id;
		return id;
	}

	inline DWORD TextureId(IDirect3DBaseTexture9* t);
	inline DWORD SurfaceId(IDirect3DSurface9* s) {
		if (!s) return 0;
		if (DWORD id = Find(s)) return id;
		// A texture's level (or a cube face) is described by its texture; others by their own description.
		IDirect3DTexture9* tex = nullptr; IDirect3DCubeTexture9* cube = nullptr;
		if (SUCCEEDED(s->GetContainer(IID_IDirect3DTexture9, (void**)&tex)) && tex) {
			const DWORD parent = TextureId(tex);
			DWORD level = 0;
			for (DWORD l = 0; l < tex->GetLevelCount(); l++) { IDirect3DSurface9* x = nullptr; if (SUCCEEDED(tex->GetSurfaceLevel(l, &x)) && x) { if (x == s) level = l; x->Release(); } }
			tex->Release();
			const DWORD id = NewId(s, D3DRTYPE_SURFACE);
			R->res[id].parent = parent; R->res[id].level = level;
			FrameSubSurface d = { id, parent, level, 0 };
			Begin(kRecSubSurface, sizeof(d)); Write(&d, sizeof(d));
			return id;
		}
		if (SUCCEEDED(s->GetContainer(IID_IDirect3DCubeTexture9, (void**)&cube)) && cube) {
			const DWORD parent = TextureId(cube);
			DWORD level = 0, face = 0;
			for (DWORD l = 0; l < cube->GetLevelCount(); l++)
				for (DWORD f = 0; f < 6; f++) { IDirect3DSurface9* x = nullptr; if (SUCCEEDED(cube->GetCubeMapSurface((D3DCUBEMAP_FACES)f, l, &x)) && x) { if (x == s) { level = l; face = f; } x->Release(); } }
			cube->Release();
			const DWORD id = NewId(s, D3DRTYPE_SURFACE);
			R->res[id].parent = parent; R->res[id].level = level; R->res[id].face = face;
			FrameSubSurface d = { id, parent, level, face };
			Begin(kRecSubSurface, sizeof(d)); Write(&d, sizeof(d));
			return id;
		}
		D3DSURFACE_DESC sd = {};
		s->GetDesc(&sd);
		IDirect3DSwapChain9* chain = nullptr;
		DWORD kind = (sd.Usage & D3DUSAGE_DEPTHSTENCIL) ? kSurfDepth : (sd.Usage & D3DUSAGE_RENDERTARGET) ? kSurfRenderTarget : kSurfPlain;
		if (SUCCEEDED(s->GetContainer(IID_IDirect3DSwapChain9, (void**)&chain)) && chain) { kind = kSurfBackBuffer; chain->Release(); }
		const DWORD id = NewId(s, D3DRTYPE_SURFACE);
		FrameSurfaceDesc d = { id, kind, sd.Width, sd.Height, (DWORD)sd.Format, (DWORD)sd.MultiSampleType, sd.MultiSampleQuality, sd.Usage, (DWORD)sd.Pool };
		Begin(kRecSurface, sizeof(d)); Write(&d, sizeof(d));
		R->kinds[4]++;
		return id;
	}

	inline DWORD TextureId(IDirect3DBaseTexture9* t) {
		if (!t) return 0;
		if (DWORD id = Find(t)) return id;
		const D3DRESOURCETYPE type = t->GetType();
		const DWORD id = NewId(t, type);
		FrameTexDesc d = { id, 0, 0, 1, t->GetLevelCount(), 0, 0, 0 };
		DWORD rec = kRecTexture;
		if (type == D3DRTYPE_TEXTURE) {
			D3DSURFACE_DESC sd = {}; ((IDirect3DTexture9*)t)->GetLevelDesc(0, &sd);
			d.width = sd.Width; d.height = sd.Height; d.usage = sd.Usage; d.format = sd.Format; d.pool = sd.Pool; R->kinds[0]++;
		}
		else if (type == D3DRTYPE_CUBETEXTURE) {
			D3DSURFACE_DESC sd = {}; ((IDirect3DCubeTexture9*)t)->GetLevelDesc(0, &sd);
			d.width = d.height = sd.Width; d.usage = sd.Usage; d.format = sd.Format; d.pool = sd.Pool; rec = kRecCube; R->kinds[1]++;
		}
		else {
			D3DVOLUME_DESC vd = {}; ((IDirect3DVolumeTexture9*)t)->GetLevelDesc(0, &vd);
			d.width = vd.Width; d.height = vd.Height; d.depth = vd.Depth; d.usage = vd.Usage; d.format = vd.Format; d.pool = vd.Pool; rec = kRecVolume; R->kinds[2]++;
		}
		Begin(rec, sizeof(d)); Write(&d, sizeof(d));
		return id;
	}

	inline DWORD BufferId(IDirect3DResource9* b) {
		if (!b) return 0;
		if (DWORD id = Find(b)) return id;
		const D3DRESOURCETYPE type = b->GetType();
		const DWORD id = NewId(b, type);
		FrameBufferDesc d = { id };
		if (type == D3DRTYPE_VERTEXBUFFER) { D3DVERTEXBUFFER_DESC v = {}; ((IDirect3DVertexBuffer9*)b)->GetDesc(&v); d.length = v.Size; d.usage = v.Usage; d.format = v.FVF; d.pool = v.Pool; }
		else { D3DINDEXBUFFER_DESC v = {}; ((IDirect3DIndexBuffer9*)b)->GetDesc(&v); d.length = v.Size; d.usage = v.Usage; d.format = v.Format; d.pool = v.Pool; }
		Begin(type == D3DRTYPE_VERTEXBUFFER ? kRecVertexBuffer : kRecIndexBuffer, sizeof(d)); Write(&d, sizeof(d));
		R->kinds[3]++;
		return id;
	}

	inline void NameId(DWORD id, const char* name) {
		const size_t n = strlen(name) + 1;
		Begin(kRecName, 4 + n); Write(&id, 4); Write(name, n);
	}

	template <class T> inline DWORD ShaderId(T* s, bool pixel) {
		if (!s) return 0;
		if (DWORD id = Find(s)) return id;
		UINT size = 0;
		s->GetFunction(nullptr, &size);
		std::vector<unsigned char> code(size);
		if (size) s->GetFunction(code.data(), &size);
		const DWORD id = NewId(s, D3DRTYPE_SURFACE);
		R->res[id].content = true;
		Begin(kRecShader, 8 + code.size()); DWORD h[2] = { id, pixel ? 1u : 0u }; Write(h, 8); Write(code.data(), code.size());
		auto hn = R->hashNames.find(Hash(code.data(), code.size()));
		if (hn != R->hashNames.end()) NameId(id, hn->second.c_str());
		auto pn = R->pendingNames.find(s);
		if (pn != R->pendingNames.end()) { NameId(id, pn->second.c_str()); R->pendingNames.erase(pn); }
		R->kinds[5]++;
		return id;
	}

	inline DWORD DeclId(IDirect3DVertexDeclaration9* d) {
		if (!d) return 0;
		if (DWORD id = Find(d)) return id;
		UINT n = 0;
		d->GetDeclaration(nullptr, &n);
		std::vector<D3DVERTEXELEMENT9> e(n);
		if (n) d->GetDeclaration(e.data(), &n);
		const DWORD id = NewId(d, D3DRTYPE_SURFACE);
		R->res[id].content = true;
		Begin(kRecDecl, 4 + e.size() * sizeof(D3DVERTEXELEMENT9)); Write(&id, 4); Write(e.data(), e.size() * sizeof(D3DVERTEXELEMENT9));
		R->kinds[6]++;
		return id;
	}

	// ---- texture upload journal ----
	// NVTF (bUseDefaultPoolForTextures) puts the game's textures in the default pool, where they cannot be locked or read back.
	// Their data only ever arrives through UpdateTexture / UpdateSurface from system-memory copies, so when a shot list asks for
	// 'record' the hooks go in at device start (EarlyStart) and every such upload is copied into a journal file in %TEMP%;
	// the recording takes a default-pool texture's levels from there. Deleted when the run ends (and stale ones at the next start).
	namespace Journal {
		struct Entry { long long offset = 0; DWORD rowBytes = 0, rows = 0, width = 0, height = 0, format = 0; };
		inline bool On = false;
		inline CRITICAL_SECTION Lock;
		inline FILE* File = nullptr;
		inline char Path[MAX_PATH] = "";
		inline std::unordered_map<unsigned long long, Entry> Index;
		inline unsigned long long Bytes = 0;
		inline unsigned Uploads = 0, Partial = 0;
		inline unsigned long long Key(const void* t, DWORD level, DWORD face) { return ((unsigned long long)(uintptr_t)t << 16) ^ ((unsigned long long)level << 4) ^ face; }

		inline void Start() {
			char dir[MAX_PATH] = "";
			GetTempPathA(MAX_PATH, dir);
			WIN32_FIND_DATAA fd; char pattern[MAX_PATH];
			sprintf_s(pattern, "%sNVR-FrameRecorder-*.journal", dir);
			HANDLE h = FindFirstFileA(pattern, &fd);
			if (h != INVALID_HANDLE_VALUE) { do { char old[MAX_PATH]; sprintf_s(old, "%s%s", dir, fd.cFileName); DeleteFileA(old); } while (FindNextFileA(h, &fd)); FindClose(h); }
			sprintf_s(Path, "%sNVR-FrameRecorder-%lu.journal", dir, GetCurrentProcessId());
			InitializeCriticalSection(&Lock);
			if (fopen_s(&File, Path, "w+b") || !File) { Logger::Log("FRAME RECORDER: no upload journal (%s)", Path); return; }
			setvbuf(File, nullptr, _IOFBF, 4 << 20);
			On = true;
			Logger::Log("FRAME RECORDER: texture upload journal on (%s): default-pool textures (NVTF) become readable for 'record'", Path);
		}
		inline void Stop() {
			if (!On) return;
			EnterCriticalSection(&Lock);
			On = false;
			fclose(File); File = nullptr;
			Index.clear();
			LeaveCriticalSection(&Lock);
			DeleteFileA(Path);
			Logger::Log("FRAME RECORDER: upload journal closed and deleted (%u uploads, %.0f MB, %u partial)", Uploads, Bytes / 1048576.0, Partial);
		}
		inline bool Journaled(IDirect3DBaseTexture9* dst) {	// default pool, not a target, not dynamic
			D3DSURFACE_DESC d = {};
			const D3DRESOURCETYPE t = dst->GetType();
			if (t == D3DRTYPE_TEXTURE) ((IDirect3DTexture9*)dst)->GetLevelDesc(0, &d);
			else if (t == D3DRTYPE_CUBETEXTURE) ((IDirect3DCubeTexture9*)dst)->GetLevelDesc(0, &d);
			else return false;
			return d.Pool == D3DPOOL_DEFAULT && !(d.Usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL | D3DUSAGE_DYNAMIC));
		}
		inline void Put(const void* dst, DWORD level, DWORD face, const D3DSURFACE_DESC& sd, const D3DLOCKED_RECT& l) {
			DWORD rowBytes = 0, rows = 0;
			if (!Layout(sd.Format, sd.Width, sd.Height, rowBytes, rows)) return;
			EnterCriticalSection(&Lock);
			if (On) {
				_fseeki64(File, 0, SEEK_END);
				Entry e; e.offset = _ftelli64(File); e.rowBytes = rowBytes; e.rows = rows; e.width = sd.Width; e.height = sd.Height; e.format = sd.Format;
				bool ok = true;
				for (DWORD y = 0; y < rows && ok; y++) ok = fwrite((const unsigned char*)l.pBits + (size_t)y * l.Pitch, rowBytes, 1, File) == 1;
				if (ok) { Index[Key(dst, level, face)] = e; Bytes += (unsigned long long)rowBytes * rows; }
			}
			LeaveCriticalSection(&Lock);
		}
		inline void Forget(const void* dst, DWORD level, DWORD face) { EnterCriticalSection(&Lock); Index.erase(Key(dst, level, face)); Partial++; LeaveCriticalSection(&Lock); }

		// UpdateTexture(system memory -> default pool): the source's levels that land in the destination.
		inline void OnUpdateTexture(IDirect3DBaseTexture9* src, IDirect3DBaseTexture9* dst) {
			if (!On || !src || !dst || src->GetType() != dst->GetType() || !Journaled(dst)) return;
			const DWORD sl = src->GetLevelCount(), dl = dst->GetLevelCount();
			const DWORD skip = sl > dl ? sl - dl : 0;
			const bool cube = src->GetType() == D3DRTYPE_CUBETEXTURE;
			for (DWORD level = 0; level < dl && level + skip < sl; level++)
				for (DWORD face = 0; face < (cube ? 6u : 1u); face++) {
					D3DSURFACE_DESC sd = {}; D3DLOCKED_RECT l = {};
					if (cube) {
						IDirect3DCubeTexture9* c = (IDirect3DCubeTexture9*)src;
						c->GetLevelDesc(level + skip, &sd);
						if (sd.Pool != D3DPOOL_SYSTEMMEM || FAILED(c->LockRect((D3DCUBEMAP_FACES)face, level + skip, &l, nullptr, D3DLOCK_READONLY))) continue;
						Put(dst, level, face, sd, l);
						c->UnlockRect((D3DCUBEMAP_FACES)face, level + skip);
					}
					else {
						IDirect3DTexture9* t = (IDirect3DTexture9*)src;
						t->GetLevelDesc(level + skip, &sd);
						if (sd.Pool != D3DPOOL_SYSTEMMEM || FAILED(t->LockRect(level + skip, &l, nullptr, D3DLOCK_READONLY))) continue;
						Put(dst, level, face, sd, l);
						t->UnlockRect(level + skip);
					}
				}
			Uploads++;
		}
		// UpdateSurface(system memory surface -> a default-pool texture's level): whole-level copies only.
		inline void OnUpdateSurface(IDirect3DSurface9* src, const RECT* rect, IDirect3DSurface9* dst, const POINT* point) {
			if (!On || !src || !dst) return;
			IDirect3DTexture9* tex = nullptr;
			if (FAILED(dst->GetContainer(IID_IDirect3DTexture9, (void**)&tex)) || !tex) return;
			if (Journaled(tex)) {
				DWORD level = 0;
				for (DWORD i = 0; i < tex->GetLevelCount(); i++) { IDirect3DSurface9* x = nullptr; if (SUCCEEDED(tex->GetSurfaceLevel(i, &x)) && x) { if (x == dst) level = i; x->Release(); } }
				D3DSURFACE_DESC sd = {}, dd = {}; src->GetDesc(&sd); dst->GetDesc(&dd);
				D3DLOCKED_RECT l = {};
				if (rect || (point && (point->x || point->y)) || sd.Width != dd.Width || sd.Height != dd.Height) Forget(tex, level, 0);
				else if (sd.Pool == D3DPOOL_SYSTEMMEM && SUCCEEDED(src->LockRect(&l, nullptr, D3DLOCK_READONLY))) { Put(tex, level, 0, sd, l); src->UnlockRect(); Uploads++; }
			}
			tex->Release();
		}
		inline bool Get(const void* tex, DWORD level, DWORD face, std::vector<unsigned char>& out, Entry& e) {
			if (!On) return false;
			EnterCriticalSection(&Lock);
			auto f = Index.find(Key(tex, level, face));
			bool ok = f != Index.end();
			if (ok) {
				e = f->second;
				out.resize((size_t)e.rowBytes * e.rows);
				fflush(File);
				ok = !_fseeki64(File, e.offset, SEEK_SET) && fread(out.data(), out.size(), 1, File) == 1;
				_fseeki64(File, 0, SEEK_END);
			}
			LeaveCriticalSection(&Lock);
			return ok;
		}
	}

	// ---- contents ----
	inline void WriteRows(DWORD id, DWORD level, DWORD face, DWORD flags, DWORD w, DWORD h, DWORD depth, D3DFORMAT f, const unsigned char* bits, int pitch, int slicePitch) {
		DWORD rowBytes = 0, rows = 0;
		if (!Layout(f, w, h, rowBytes, rows)) { R->unreadable++; Note("format %u of resource %u not handled", (unsigned)f, id); return; }
		FrameDataHeader d = { id, level, face, flags, w, h, depth, rowBytes, rows };
		Begin(kRecData, sizeof(d) + (size_t)rowBytes * rows * depth);
		Write(&d, sizeof(d));
		for (DWORD z = 0; z < depth; z++)
			for (DWORD y = 0; y < rows; y++) Write(bits + (size_t)z * slicePitch + (size_t)y * pitch, rowBytes);
	}

	// A render target surface read back from the GPU (resolved first if multisampled).
	inline bool ReadBack(IDirect3DSurface9* s, DWORD id, DWORD level, DWORD face) {
		D3DSURFACE_DESC sd = {};
		s->GetDesc(&sd);
		DWORD rb = 0, rows = 0;
		if (!Layout(sd.Format, sd.Width, sd.Height, rb, rows)) { R->unreadable++; Note("render target %u format %u not readable", id, (unsigned)sd.Format); return false; }
		IDirect3DSurface9* resolve = nullptr; IDirect3DSurface9* sys = nullptr;
		IDirect3DSurface9* source = s;
		DWORD flags = kDataReadBack;
		bool ok = true;
		if (sd.MultiSampleType != D3DMULTISAMPLE_NONE) {
			ok = SUCCEEDED(Device->CreateRenderTarget(sd.Width, sd.Height, sd.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &resolve, nullptr)) &&
				SUCCEEDED(Device->StretchRect(s, nullptr, resolve, nullptr, D3DTEXF_NONE));
			source = resolve; flags |= kDataResolved;
		}
		ok = ok && SUCCEEDED(Device->CreateOffscreenPlainSurface(sd.Width, sd.Height, sd.Format, D3DPOOL_SYSTEMMEM, &sys, nullptr)) &&
			SUCCEEDED(Device->GetRenderTargetData(source, sys));
		D3DLOCKED_RECT l = {};
		if (ok && SUCCEEDED(sys->LockRect(&l, nullptr, D3DLOCK_READONLY))) {
			WriteRows(id, level, face, flags, sd.Width, sd.Height, 1, sd.Format, (const unsigned char*)l.pBits, l.Pitch, 0);
			sys->UnlockRect();
			R->readbacks++;
		}
		else { ok = false; R->unreadable++; Note("render target %u (format %u, %ux%u) could not be read back", id, (unsigned)sd.Format, sd.Width, sd.Height); }
		if (sys) sys->Release();
		if (resolve) resolve->Release();
		return ok;
	}

	// A depth texture (INTZ) read before written: sampled into an R32F target by a full-screen pass, read back, written as floats
	// (kDataDepth). The device state is kept in a state block around it. The replayer writes it back with oDepth.
	inline bool DepthReadBack(IDirect3DTexture9* tex, DWORD id) {
		static IDirect3DVertexShader9* vs = nullptr; static IDirect3DPixelShader9* ps = nullptr;
		if (!vs || !ps) {
			ID3DXBuffer* code = nullptr;
			const char* v = "void main(float4 p : POSITION, float2 t : TEXCOORD0, out float4 op : POSITION, out float2 ot : TEXCOORD0) { op = p; ot = t; }";
			const char* f = "sampler2D s0 : register(s0); float4 main(float2 t : TEXCOORD0) : COLOR0 { return tex2D(s0, t).rrrr; }";
			if (SUCCEEDED(D3DXCompileShader(v, (UINT)strlen(v), nullptr, nullptr, "main", "vs_3_0", 0, &code, nullptr, nullptr))) { Device->CreateVertexShader((const DWORD*)code->GetBufferPointer(), &vs); code->Release(); }
			if (SUCCEEDED(D3DXCompileShader(f, (UINT)strlen(f), nullptr, nullptr, "main", "ps_3_0", 0, &code, nullptr, nullptr))) { Device->CreatePixelShader((const DWORD*)code->GetBufferPointer(), &ps); code->Release(); }
			if (!vs || !ps) return false;
		}
		D3DSURFACE_DESC sd = {}; tex->GetLevelDesc(0, &sd);
		IDirect3DSurface9* rt = nullptr; IDirect3DSurface9* sys = nullptr; IDirect3DStateBlock9* saved = nullptr;
		bool ok = SUCCEEDED(Device->CreateRenderTarget(sd.Width, sd.Height, D3DFMT_R32F, D3DMULTISAMPLE_NONE, 0, FALSE, &rt, nullptr)) &&
			SUCCEEDED(Device->CreateOffscreenPlainSurface(sd.Width, sd.Height, D3DFMT_R32F, D3DPOOL_SYSTEMMEM, &sys, nullptr)) &&
			SUCCEEDED(Device->CreateStateBlock(D3DSBT_ALL, &saved));
		if (ok) {
			IDirect3DSurface9* oldRt[4] = {}; IDirect3DSurface9* oldDs = nullptr;
			for (DWORD i = 0; i < 4; i++) Device->GetRenderTarget(i, &oldRt[i]);
			Device->GetDepthStencilSurface(&oldDs);
			struct V { float x, y, z, w, u, v; };
			const float du = 0.5f / sd.Width, dv = 0.5f / sd.Height;
			V quad[4] = { { -1, 1, 0, 1, du, dv }, { 1, 1, 0, 1, 1 + du, dv }, { -1, -1, 0, 1, du, 1 + dv }, { 1, -1, 0, 1, 1 + du, 1 + dv } };
			Device->SetRenderTarget(0, rt); for (DWORD i = 1; i < 4; i++) Device->SetRenderTarget(i, nullptr);
			Device->SetDepthStencilSurface(nullptr);
			Device->SetVertexShader(vs); Device->SetPixelShader(ps); Device->SetFVF(D3DFVF_XYZW | D3DFVF_TEX1);
			Device->SetTexture(0, tex);
			Device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT); Device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT); Device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
			Device->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);
			const D3DRENDERSTATETYPE off[] = { D3DRS_ZENABLE, D3DRS_ALPHABLENDENABLE, D3DRS_ALPHATESTENABLE, D3DRS_SCISSORTESTENABLE, D3DRS_STENCILENABLE, D3DRS_SRGBWRITEENABLE, D3DRS_CLIPPLANEENABLE, D3DRS_FOGENABLE };
			for (auto r : off) Device->SetRenderState(r, FALSE);
			Device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE); Device->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
			const bool began = SUCCEEDED(Device->BeginScene());	// fails harmlessly when the game is inside a scene already
			ok = SUCCEEDED(Device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(V)));
			if (began) Device->EndScene();
			for (DWORD i = 0; i < 4; i++) { if (i == 0 || oldRt[i]) Device->SetRenderTarget(i, oldRt[i]); else Device->SetRenderTarget(i, nullptr); if (oldRt[i]) oldRt[i]->Release(); }
			Device->SetDepthStencilSurface(oldDs); if (oldDs) oldDs->Release();
			saved->Apply();	// after the targets: SetRenderTarget resets the viewport and scissor rect
			ok = ok && SUCCEEDED(Device->GetRenderTargetData(rt, sys));
		}
		D3DLOCKED_RECT l = {};
		if (ok && SUCCEEDED(sys->LockRect(&l, nullptr, D3DLOCK_READONLY))) { WriteRows(id, 0, 0, kDataReadBack | kDataDepth, sd.Width, sd.Height, 1, D3DFMT_R32F, (const unsigned char*)l.pBits, l.Pitch, 0); sys->UnlockRect(); R->readbacks++; }
		else ok = false;
		if (saved) saved->Release();
		if (sys) sys->Release();
		if (rt) rt->Release();
		return ok;
	}

	inline void Content(DWORD id) {
		if (!id || !R || id >= R->res.size()) return;
		Res& r = R->res[id];
		if (r.content || r.written) return;
		r.content = true;
		if (r.parent) { Content(r.parent); return; }
		if (r.type == D3DRTYPE_TEXTURE) {
			IDirect3DTexture9* t = (IDirect3DTexture9*)r.obj;
			D3DSURFACE_DESC sd = {}; t->GetLevelDesc(0, &sd);
			if (sd.Usage & D3DUSAGE_DEPTHSTENCIL) {
				if (DepthReadBack(t, id)) { Note("depth texture %u (%ux%u format %u) read before written: copied as R32F", id, sd.Width, sd.Height, (unsigned)sd.Format); return; }
				R->depthUnknown++; Note("depth texture %u (%ux%u format %u) read before written: contents unknown", id, sd.Width, sd.Height, (unsigned)sd.Format); return;
			}
			for (DWORD l = 0; l < t->GetLevelCount(); l++) {
				t->GetLevelDesc(l, &sd);
				if (sd.Usage & D3DUSAGE_RENDERTARGET) {
					IDirect3DSurface9* s = nullptr;
					if (SUCCEEDED(t->GetSurfaceLevel(l, &s)) && s) { ReadBack(s, id, l, 0); s->Release(); }
					continue;
				}
				if (sd.Pool == D3DPOOL_DEFAULT && !(sd.Usage & D3DUSAGE_DYNAMIC)) {
					std::vector<unsigned char> data; Journal::Entry e;
					if (Journal::Get(t, l, 0, data, e) && e.width == sd.Width && e.height == sd.Height && e.format == (DWORD)sd.Format) { WriteRows(id, l, 0, 0, sd.Width, sd.Height, 1, sd.Format, data.data(), e.rowBytes, 0); R->journaled++; }
					else { R->unreadable++; Note("default-pool texture %u level %u: not in the upload journal", id, l); }
					continue;
				}
				D3DLOCKED_RECT lr = {};
				if (SUCCEEDED(t->LockRect(l, &lr, nullptr, D3DLOCK_READONLY))) { WriteRows(id, l, 0, 0, sd.Width, sd.Height, 1, sd.Format, (const unsigned char*)lr.pBits, lr.Pitch, 0); t->UnlockRect(l); }
				else { R->unreadable++; Note("texture %u level %u: LockRect failed", id, l); }
			}
		}
		else if (r.type == D3DRTYPE_CUBETEXTURE) {
			IDirect3DCubeTexture9* t = (IDirect3DCubeTexture9*)r.obj;
			for (DWORD l = 0; l < t->GetLevelCount(); l++) {
				D3DSURFACE_DESC sd = {}; t->GetLevelDesc(l, &sd);
				for (DWORD f = 0; f < 6; f++) {
					if (sd.Usage & D3DUSAGE_RENDERTARGET) {
						IDirect3DSurface9* s = nullptr;
						if (SUCCEEDED(t->GetCubeMapSurface((D3DCUBEMAP_FACES)f, l, &s)) && s) { ReadBack(s, id, l, f); s->Release(); }
						continue;
					}
					if (sd.Pool == D3DPOOL_DEFAULT && !(sd.Usage & D3DUSAGE_DYNAMIC)) {
						std::vector<unsigned char> data; Journal::Entry e;
						if (Journal::Get(t, l, f, data, e) && e.width == sd.Width && e.height == sd.Height && e.format == (DWORD)sd.Format) { WriteRows(id, l, f, 0, sd.Width, sd.Height, 1, sd.Format, data.data(), e.rowBytes, 0); R->journaled++; }
						else { R->unreadable++; Note("default-pool cube %u level %u face %u: not in the upload journal", id, l, f); }
						continue;
					}
					D3DLOCKED_RECT lr = {};
					if (SUCCEEDED(t->LockRect((D3DCUBEMAP_FACES)f, l, &lr, nullptr, D3DLOCK_READONLY))) { WriteRows(id, l, f, 0, sd.Width, sd.Height, 1, sd.Format, (const unsigned char*)lr.pBits, lr.Pitch, 0); t->UnlockRect((D3DCUBEMAP_FACES)f, l); }
					else R->unreadable++;
				}
			}
		}
		else if (r.type == D3DRTYPE_VOLUMETEXTURE) {
			IDirect3DVolumeTexture9* t = (IDirect3DVolumeTexture9*)r.obj;
			for (DWORD l = 0; l < t->GetLevelCount(); l++) {
				D3DVOLUME_DESC vd = {}; t->GetLevelDesc(l, &vd);
				D3DLOCKED_BOX lb = {};
				if (vd.Pool != D3DPOOL_DEFAULT && SUCCEEDED(t->LockBox(l, &lb, nullptr, D3DLOCK_READONLY))) {
					WriteRows(id, l, 0, 0, vd.Width, vd.Height, vd.Depth, vd.Format, (const unsigned char*)lb.pBits, lb.RowPitch, lb.SlicePitch); t->UnlockBox(l);
				}
				else R->unreadable++;
			}
		}
		else if (r.type == D3DRTYPE_VERTEXBUFFER || r.type == D3DRTYPE_INDEXBUFFER) {
			const bool vb = r.type == D3DRTYPE_VERTEXBUFFER;
			UINT size = 0;
			if (vb) { D3DVERTEXBUFFER_DESC d = {}; ((IDirect3DVertexBuffer9*)r.obj)->GetDesc(&d); size = d.Size; }
			else { D3DINDEXBUFFER_DESC d = {}; ((IDirect3DIndexBuffer9*)r.obj)->GetDesc(&d); size = d.Size; }
			void* p = nullptr;
			const HRESULT hr = vb ? ((IDirect3DVertexBuffer9*)r.obj)->Lock(0, 0, &p, D3DLOCK_READONLY) : ((IDirect3DIndexBuffer9*)r.obj)->Lock(0, 0, &p, D3DLOCK_READONLY);
			if (SUCCEEDED(hr) && p) {
				FrameDataHeader d = { id, 0, 0, 0, size, 1, 1, size, 1 };
				Begin(kRecData, sizeof(d) + size); Write(&d, sizeof(d)); Write(p, size);
			}
			else { R->unreadable++; Note("buffer %u (%u bytes) could not be locked (%08lX)", id, size, (unsigned long)hr); }
			if (SUCCEEDED(hr)) { if (vb) ((IDirect3DVertexBuffer9*)r.obj)->Unlock(); else ((IDirect3DIndexBuffer9*)r.obj)->Unlock(); }
		}
		else if (r.type == D3DRTYPE_SURFACE) {
			IDirect3DSurface9* s = (IDirect3DSurface9*)r.obj;
			D3DSURFACE_DESC sd = {}; s->GetDesc(&sd);
			if (sd.Usage & D3DUSAGE_DEPTHSTENCIL) { R->depthUnknown++; Note("depth surface %u (%ux%u format %u) used before cleared: contents unknown", id, sd.Width, sd.Height, (unsigned)sd.Format); return; }
			if ((sd.Usage & D3DUSAGE_RENDERTARGET) || sd.Pool == D3DPOOL_DEFAULT) {
				if (sd.Usage & D3DUSAGE_RENDERTARGET) { ReadBack(s, id, 0, 0); return; }
			}
			D3DLOCKED_RECT lr = {};
			if (SUCCEEDED(s->LockRect(&lr, nullptr, D3DLOCK_READONLY))) { WriteRows(id, 0, 0, 0, sd.Width, sd.Height, 1, sd.Format, (const unsigned char*)lr.pBits, lr.Pitch, 0); s->UnlockRect(); }
			else { R->unreadable++; Note("surface %u could not be locked", id); }
		}
	}

	inline void Written(DWORD id) {
		if (!id) return;
		Res& r = R->res[id];
		r.written = true;
		if (r.parent && r.level == 0) R->res[r.parent].written = true;
	}

	// Covers the whole surface (viewport or rect list)?
	inline bool Covers(DWORD id, const D3DVIEWPORT9& v) {
		IDirect3DSurface9* s = (IDirect3DSurface9*)R->res[id].obj;
		D3DSURFACE_DESC sd = {}; s->GetDesc(&sd);
		return v.X == 0 && v.Y == 0 && v.Width >= sd.Width && v.Height >= sd.Height;
	}

	// ---- state ----
	inline void ReadState(FrameState& s) {
		IDirect3DDevice9* d = Device;
		for (unsigned char rs : RenderStates) d->GetRenderState((D3DRENDERSTATETYPE)rs, &s.rs[rs]);
		for (unsigned i = 0; i < kSamplers; i++) {
			for (unsigned st = 1; st < kSamplerStates; st++) d->GetSamplerState(SamplerOf(i), (D3DSAMPLERSTATETYPE)st, &s.ss[i][st]);
			IDirect3DBaseTexture9* t = nullptr;
			d->GetTexture(SamplerOf(i), &t);
			s.tex[i] = TextureId(t);
			if (t) t->Release();
		}
		for (unsigned i = 0; i < kStages; i++) for (unsigned char st : StageStates) d->GetTextureStageState(i, (D3DTEXTURESTAGESTATETYPE)st, &s.tss[i][st]);
		IDirect3DVertexShader9* vs = nullptr; IDirect3DPixelShader9* ps = nullptr; IDirect3DVertexDeclaration9* decl = nullptr; IDirect3DIndexBuffer9* ib = nullptr;
		d->GetVertexShader(&vs); s.vs = ShaderId(vs, false); if (vs) vs->Release();
		d->GetPixelShader(&ps); s.ps = ShaderId(ps, true); if (ps) ps->Release();
		d->GetVertexDeclaration(&decl); s.decl = DeclId(decl); if (decl) decl->Release();
		d->GetFVF(&s.fvf);
		d->GetIndices(&ib); s.ib = BufferId(ib); if (ib) ib->Release();
		for (unsigned i = 0; i < 16; i++) {
			IDirect3DVertexBuffer9* vb = nullptr; UINT offset = 0, stride = 0, freq = 0;
			d->GetStreamSource(i, &vb, &offset, &stride);
			d->GetStreamSourceFreq(i, &freq);
			s.stream[i][0] = BufferId(vb); s.stream[i][1] = offset; s.stream[i][2] = stride; s.stream[i][3] = freq;
			if (vb) vb->Release();
		}
		for (unsigned i = 0; i < 4; i++) {
			IDirect3DSurface9* rt = nullptr;
			if (SUCCEEDED(d->GetRenderTarget(i, &rt)) && rt) { s.rt[i] = SurfaceId(rt); rt->Release(); }
			else s.rt[i] = 0;
		}
		IDirect3DSurface9* ds = nullptr;
		if (SUCCEEDED(d->GetDepthStencilSurface(&ds)) && ds) { s.ds = SurfaceId(ds); ds->Release(); }
		else s.ds = 0;
		d->GetViewport(&s.viewport);
		d->GetScissorRect(&s.scissor);
		for (unsigned i = 0; i < 6; i++) d->GetClipPlane(i, s.clip[i]);
		d->GetVertexShaderConstantF(0, &s.vsf[0][0], kVSFloat);
		d->GetVertexShaderConstantI(0, &s.vsi[0][0], 16);
		d->GetVertexShaderConstantB(0, s.vsb, 16);
		d->GetPixelShaderConstantF(0, &s.psf[0][0], kPSFloat);
		d->GetPixelShaderConstantI(0, &s.psi[0][0], 16);
		d->GetPixelShaderConstantB(0, s.psb, 16);
		if (!s.vs) for (unsigned i = 0; i < kTransforms; i++) d->GetTransform(TransformOf(i), &s.xf[i]);
	}

	inline void WriteStateDiff() {
		const DWORD* a = (const DWORD*)&R->last;
		const DWORD* b = (const DWORD*)&R->cur;
		std::vector<DWORD> out;
		out.push_back(0);
		DWORD runs = 0;
		for (unsigned i = 0; i < kStateWords;) {
			if (R->haveLast && a[i] == b[i]) { i++; continue; }
			unsigned j = i + 1;
			while (j < kStateWords && (!R->haveLast || a[j] != b[j] || (j + 1 < kStateWords && a[j + 1] != b[j + 1])) && j - i < 65535) j++;	// bridge single equal words
			out.push_back((DWORD)i | ((DWORD)(j - i) << 16));
			out.insert(out.end(), b + i, b + j);
			runs++;
			i = j;
		}
		if (!runs) return;
		out[0] = runs;
		Begin(kRecState, out.size() * 4); Write(out.data(), out.size() * 4);
		R->last = R->cur;
		R->haveLast = true;
		R->stateRecords++;
	}

	inline void WriteAction(DWORD type, std::initializer_list<DWORD> args, const void* data = nullptr, DWORD bytes = 0, const void* data2 = nullptr, DWORD bytes2 = 0) {
		FrameAction a = {};
		a.type = type;
		unsigned i = 0;
		for (DWORD v : args) if (i < 7) a.a[i++] = v;
		a.dataBytes = bytes + bytes2;
		Begin(kRecAction, sizeof(a) + a.dataBytes); Write(&a, sizeof(a)); Write(data, bytes); Write(data2, bytes2);
		R->actions++;
	}

	// Before a draw: state, the resources it reads, the targets it writes.
	inline void PrepareDraw(bool indexed, bool up) {
		ReadState(R->cur);
		FrameState& s = R->cur;
		for (unsigned i = 0; i < kSamplers; i++) Content(s.tex[i]);
		if (!up) {
			for (unsigned i = 0; i < 16; i++) if (s.stream[i][0] && s.stream[i][2]) Content(s.stream[i][0]);
			if (indexed) Content(s.ib);
		}
		for (unsigned i = 0; i < 4; i++) if (s.rt[i]) { Content(s.rt[i]); Written(s.rt[i]); }	// a draw may blend: what was there counts
		if (s.ds && (s.rs[D3DRS_ZENABLE] || s.rs[D3DRS_STENCILENABLE])) { Content(s.ds); if (s.rs[D3DRS_ZWRITEENABLE] || s.rs[D3DRS_STENCILENABLE]) Written(s.ds); }
		WriteStateDiff();
		R->draws++;
	}

	inline UINT VerticesOf(D3DPRIMITIVETYPE t, UINT prims) {
		switch (t) {
		case D3DPT_POINTLIST: return prims;
		case D3DPT_LINELIST: return prims * 2;
		case D3DPT_LINESTRIP: return prims + 1;
		case D3DPT_TRIANGLELIST: return prims * 3;
		case D3DPT_TRIANGLESTRIP: case D3DPT_TRIANGLEFAN: return prims + 2;
		}
		return 0;
	}

	// ---- hooks: device ----
	typedef HRESULT(STDMETHODCALLTYPE* PresentFn)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
	typedef HRESULT(STDMETHODCALLTYPE* SceneFn)(IDirect3DDevice9*);
	typedef HRESULT(STDMETHODCALLTYPE* ClearFn)(IDirect3DDevice9*, DWORD, const D3DRECT*, DWORD, D3DCOLOR, float, DWORD);
	typedef HRESULT(STDMETHODCALLTYPE* DrawFn)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT);
	typedef HRESULT(STDMETHODCALLTYPE* DrawIndexedFn)(IDirect3DDevice9*, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
	typedef HRESULT(STDMETHODCALLTYPE* DrawUPFn)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, const void*, UINT);
	typedef HRESULT(STDMETHODCALLTYPE* DrawIndexedUPFn)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT, UINT, const void*, D3DFORMAT, const void*, UINT);
	typedef HRESULT(STDMETHODCALLTYPE* UpdateSurfaceFn)(IDirect3DDevice9*, IDirect3DSurface9*, const RECT*, IDirect3DSurface9*, const POINT*);
	typedef HRESULT(STDMETHODCALLTYPE* UpdateTextureFn)(IDirect3DDevice9*, IDirect3DBaseTexture9*, IDirect3DBaseTexture9*);
	typedef HRESULT(STDMETHODCALLTYPE* StretchRectFn)(IDirect3DDevice9*, IDirect3DSurface9*, const RECT*, IDirect3DSurface9*, const RECT*, D3DTEXTUREFILTERTYPE);
	typedef HRESULT(STDMETHODCALLTYPE* ColorFillFn)(IDirect3DDevice9*, IDirect3DSurface9*, const RECT*, D3DCOLOR);
	inline PresentFn OPresent = nullptr; inline SceneFn OBeginScene = nullptr, OEndScene = nullptr; inline ClearFn OClear = nullptr;
	inline DrawFn ODraw = nullptr; inline DrawIndexedFn ODrawIndexed = nullptr; inline DrawUPFn ODrawUP = nullptr; inline DrawIndexedUPFn ODrawIndexedUP = nullptr;
	inline UpdateSurfaceFn OUpdateSurface = nullptr; inline UpdateTextureFn OUpdateTexture = nullptr; inline StretchRectFn OStretchRect = nullptr; inline ColorFillFn OColorFill = nullptr;

	inline bool Want() { return State == kRecording && Depth == 0 && R && GetCurrentThreadId() == Thread; }

	inline void Start();
	inline void Finish();

	inline HRESULT STDMETHODCALLTYPE HPresent(IDirect3DDevice9* d, const RECT* a, const RECT* b, HWND w, const RGNDATA* r) {
		if (Want()) { Guard g; Finish(); }
		const HRESULT hr = OPresent(d, a, b, w, r);
		if (State == kArmed && Depth == 0 && GetCurrentThreadId() == Thread) { Guard g; Start(); }
		return hr;
	}
	inline HRESULT STDMETHODCALLTYPE HBeginScene(IDirect3DDevice9* d) {
		if (!Want()) return OBeginScene(d);
		Guard g; WriteAction(kActBeginScene, {}); return OBeginScene(d);
	}
	inline HRESULT STDMETHODCALLTYPE HEndScene(IDirect3DDevice9* d) {
		if (!Want()) return OEndScene(d);
		Guard g; WriteAction(kActEndScene, {}); return OEndScene(d);
	}
	inline HRESULT STDMETHODCALLTYPE HClear(IDirect3DDevice9* d, DWORD count, const D3DRECT* rects, DWORD flags, D3DCOLOR c, float z, DWORD stencil) {
		if (!Want()) return OClear(d, count, rects, flags, c, z, stencil);
		Guard g;
		ReadState(R->cur);
		FrameState& s = R->cur;
		const bool full = (!count || !rects) && !s.rs[D3DRS_SCISSORTESTENABLE];
		if (flags & D3DCLEAR_TARGET) for (unsigned i = 0; i < 4; i++) if (s.rt[i]) { if (!full || !Covers(s.rt[i], s.viewport)) Content(s.rt[i]); Written(s.rt[i]); }
		if ((flags & (D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL)) && s.ds) { if (!full || !Covers(s.ds, s.viewport)) Content(s.ds); Written(s.ds); }
		WriteStateDiff();
		DWORD zbits; memcpy(&zbits, &z, 4);
		WriteAction(kActClear, { (rects ? count : 0), flags, c, zbits, stencil }, rects, rects ? count * sizeof(D3DRECT) : 0);
		return OClear(d, count, rects, flags, c, z, stencil);
	}
	inline HRESULT STDMETHODCALLTYPE HDraw(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, UINT start, UINT prims) {
		if (!Want()) return ODraw(d, t, start, prims);
		Guard g; PrepareDraw(false, false); WriteAction(kActDraw, { (DWORD)t, start, prims });
		return ODraw(d, t, start, prims);
	}
	inline HRESULT STDMETHODCALLTYPE HDrawIndexed(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, INT base, UINT minIndex, UINT vertices, UINT start, UINT prims) {
		if (!Want()) return ODrawIndexed(d, t, base, minIndex, vertices, start, prims);
		Guard g; PrepareDraw(true, false); WriteAction(kActDrawIndexed, { (DWORD)t, (DWORD)base, minIndex, vertices, start, prims });
		return ODrawIndexed(d, t, base, minIndex, vertices, start, prims);
	}
	inline HRESULT STDMETHODCALLTYPE HDrawUP(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, UINT prims, const void* data, UINT stride) {
		if (!Want()) return ODrawUP(d, t, prims, data, stride);
		Guard g; PrepareDraw(false, true);
		WriteAction(kActDrawUP, { (DWORD)t, prims, stride }, data, VerticesOf(t, prims) * stride);
		return ODrawUP(d, t, prims, data, stride);
	}
	inline HRESULT STDMETHODCALLTYPE HDrawIndexedUP(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, UINT minIndex, UINT vertices, UINT prims, const void* indices, D3DFORMAT format, const void* data, UINT stride) {
		if (!Want()) return ODrawIndexedUP(d, t, minIndex, vertices, prims, indices, format, data, stride);
		Guard g; PrepareDraw(false, true);
		const DWORD indexBytes = VerticesOf(t, prims) * (format == D3DFMT_INDEX32 ? 4 : 2);
		WriteAction(kActDrawIndexedUP, { (DWORD)t, minIndex, vertices, prims, (DWORD)format, stride, indexBytes }, indices, indexBytes,
			(const unsigned char*)data, (minIndex + vertices) * stride);	// vertices from 0: the indices are absolute
		return ODrawIndexedUP(d, t, minIndex, vertices, prims, indices, format, data, stride);
	}
	inline HRESULT STDMETHODCALLTYPE HUpdateSurface(IDirect3DDevice9* d, IDirect3DSurface9* src, const RECT* rect, IDirect3DSurface9* dst, const POINT* point) {
		if (Journal::On && Depth == 0) { Guard g; Journal::OnUpdateSurface(src, rect, dst, point); }
		if (!Want()) return OUpdateSurface(d, src, rect, dst, point);
		Guard g;
		const DWORD s = SurfaceId(src), t = SurfaceId(dst);
		Content(s);
		if (rect || point) Content(t);
		Written(t);
		RECT r = rect ? *rect : RECT{}; POINT p = point ? *point : POINT{};
		WriteAction(kActUpdateSurface, { s, t, rect ? 1u : 0u, point ? 1u : 0u }, &r, sizeof(r), &p, sizeof(p));
		return OUpdateSurface(d, src, rect, dst, point);
	}
	inline HRESULT STDMETHODCALLTYPE HUpdateTexture(IDirect3DDevice9* d, IDirect3DBaseTexture9* src, IDirect3DBaseTexture9* dst) {
		if (Journal::On && Depth == 0) { Guard g; Journal::OnUpdateTexture(src, dst); }
		if (!Want()) return OUpdateTexture(d, src, dst);
		Guard g;
		const DWORD s = TextureId(src), t = TextureId(dst);
		Content(s); Written(t); R->res[t].content = true;
		WriteAction(kActUpdateTexture, { s, t });
		return OUpdateTexture(d, src, dst);
	}
	inline HRESULT STDMETHODCALLTYPE HStretchRect(IDirect3DDevice9* d, IDirect3DSurface9* src, const RECT* sr, IDirect3DSurface9* dst, const RECT* dr, D3DTEXTUREFILTERTYPE f) {
		if (!Want()) return OStretchRect(d, src, sr, dst, dr, f);
		Guard g;
		const DWORD s = SurfaceId(src), t = SurfaceId(dst);
		Content(s);
		if (dr) Content(t);
		Written(t);
		RECT a = sr ? *sr : RECT{}, b = dr ? *dr : RECT{};
		WriteAction(kActStretchRect, { s, t, (DWORD)f, sr ? 1u : 0u, dr ? 1u : 0u }, &a, sizeof(a), &b, sizeof(b));
		return OStretchRect(d, src, sr, dst, dr, f);
	}
	inline HRESULT STDMETHODCALLTYPE HColorFill(IDirect3DDevice9* d, IDirect3DSurface9* s, const RECT* rect, D3DCOLOR c) {
		if (!Want()) return OColorFill(d, s, rect, c);
		Guard g;
		const DWORD id = SurfaceId(s);
		if (rect) Content(id);
		Written(id);
		RECT r = rect ? *rect : RECT{};
		WriteAction(kActColorFill, { id, c, rect ? 1u : 0u }, &r, sizeof(r));
		return OColorFill(d, s, rect, c);
	}

	// ---- hooks: resource writes during the frame ----
	inline unsigned long long LockKey(const void* o, DWORD level, DWORD face) { return ((unsigned long long)(uintptr_t)o << 16) ^ ((unsigned long long)level << 4) ^ face; }

	typedef HRESULT(STDMETHODCALLTYPE* BufLockFn)(IDirect3DResource9*, UINT, UINT, void**, DWORD);
	typedef HRESULT(STDMETHODCALLTYPE* BufUnlockFn)(IDirect3DResource9*);
	typedef HRESULT(STDMETHODCALLTYPE* TexLockFn)(IDirect3DTexture9*, UINT, D3DLOCKED_RECT*, const RECT*, DWORD);
	typedef HRESULT(STDMETHODCALLTYPE* TexUnlockFn)(IDirect3DTexture9*, UINT);
	typedef HRESULT(STDMETHODCALLTYPE* CubeLockFn)(IDirect3DCubeTexture9*, D3DCUBEMAP_FACES, UINT, D3DLOCKED_RECT*, const RECT*, DWORD);
	typedef HRESULT(STDMETHODCALLTYPE* CubeUnlockFn)(IDirect3DCubeTexture9*, D3DCUBEMAP_FACES, UINT);
	typedef HRESULT(STDMETHODCALLTYPE* VolLockFn)(IDirect3DVolumeTexture9*, UINT, D3DLOCKED_BOX*, const D3DBOX*, DWORD);
	typedef HRESULT(STDMETHODCALLTYPE* VolUnlockFn)(IDirect3DVolumeTexture9*, UINT);
	typedef HRESULT(STDMETHODCALLTYPE* SurfLockFn)(IDirect3DSurface9*, D3DLOCKED_RECT*, const RECT*, DWORD);
	typedef HRESULT(STDMETHODCALLTYPE* SurfUnlockFn)(IDirect3DSurface9*);

	inline void OnBufferLock(IDirect3DResource9* b, UINT offset, UINT size, void** p, DWORD flags) {
		if (!p || !*p || (flags & D3DLOCK_READONLY)) return;
		Recorder::Lock l; l.ptr = *p; l.offset = offset; l.size = size; l.flags = flags;
		R->locks[LockKey(b, 0, 0)] = l;
	}
	inline void OnBufferUnlock(IDirect3DResource9* b) {
		auto f = R->locks.find(LockKey(b, 0, 0));
		if (f == R->locks.end()) return;
		Recorder::Lock l = f->second;
		R->locks.erase(f);
		const DWORD id = BufferId(b);
		DWORD length = 0;
		if (b->GetType() == D3DRTYPE_VERTEXBUFFER) { D3DVERTEXBUFFER_DESC d = {}; ((IDirect3DVertexBuffer9*)b)->GetDesc(&d); length = d.Size; }
		else { D3DINDEXBUFFER_DESC d = {}; ((IDirect3DIndexBuffer9*)b)->GetDesc(&d); length = d.Size; }
		DWORD size = l.size ? l.size : (length > l.offset ? length - l.offset : 0);
		if (l.offset + size > length) size = length > l.offset ? length - l.offset : 0;
		FrameUpdate u = { id, 0, 0, l.offset, 0, 0, 0, 0, size, 1, l.flags };
		Begin(kRecUpdate, sizeof(u) + size); Write(&u, sizeof(u)); Write(l.ptr, size);
		R->updates++;
	}
	inline void OnRectLock(const void* o, DWORD level, DWORD face, D3DLOCKED_RECT* lr, const RECT* rect, DWORD flags) {
		if (!lr || !lr->pBits || (flags & D3DLOCK_READONLY)) return;
		Recorder::Lock l; l.ptr = lr->pBits; l.pitch = lr->Pitch; l.flags = flags; l.whole = !rect; if (rect) l.rect = *rect;
		R->locks[LockKey(o, level, face)] = l;
	}
	inline void OnRectUnlock(unsigned long long key, DWORD id, DWORD level, DWORD face, D3DFORMAT format, DWORD w, DWORD h) {
		auto f = R->locks.find(key);
		if (f == R->locks.end()) return;
		Recorder::Lock l = f->second;
		R->locks.erase(f);
		RECT r = l.whole ? RECT{ 0, 0, (LONG)w, (LONG)h } : l.rect;
		DWORD rowBytes = 0, rows = 0;
		if (!Layout(format, r.right - r.left, r.bottom - r.top, rowBytes, rows)) { R->unreadable++; return; }
		FrameUpdate u = { id, level, face, 0, (DWORD)r.left, (DWORD)r.top, (DWORD)r.right, (DWORD)r.bottom, rowBytes, rows, l.flags };
		Begin(kRecUpdate, sizeof(u) + (size_t)rowBytes * rows); Write(&u, sizeof(u));
		for (DWORD y = 0; y < rows; y++) Write((const unsigned char*)l.ptr + (size_t)y * l.pitch, rowBytes);
		R->updates++;
	}

	template <int N> struct BufHook {
		static inline BufLockFn OLock = nullptr; static inline BufUnlockFn OUnlock = nullptr;
		static HRESULT STDMETHODCALLTYPE Lock(IDirect3DResource9* b, UINT o, UINT s, void** p, DWORD f) {
			if (!Want()) { Guard g; return OLock(b, o, s, p, f); }
			Guard g; const HRESULT hr = OLock(b, o, s, p, f); if (SUCCEEDED(hr)) OnBufferLock(b, o, s, p, f); return hr;
		}
		static HRESULT STDMETHODCALLTYPE Unlock(IDirect3DResource9* b) {
			if (!Want()) { Guard g; return OUnlock(b); }
			Guard g; OnBufferUnlock(b); return OUnlock(b);
		}
	};
	template <int N> struct TexHook {
		static inline TexLockFn OLock = nullptr; static inline TexUnlockFn OUnlock = nullptr;
		static HRESULT STDMETHODCALLTYPE Lock(IDirect3DTexture9* t, UINT level, D3DLOCKED_RECT* lr, const RECT* r, DWORD f) {
			if (!Want()) { Guard g; return OLock(t, level, lr, r, f); }
			Guard g; const HRESULT hr = OLock(t, level, lr, r, f); if (SUCCEEDED(hr)) OnRectLock(t, level, 0, lr, r, f); return hr;
		}
		static HRESULT STDMETHODCALLTYPE Unlock(IDirect3DTexture9* t, UINT level) {
			if (!Want()) { Guard g; return OUnlock(t, level); }
			Guard g;
			if (R->locks.count(LockKey(t, level, 0))) { D3DSURFACE_DESC sd = {}; t->GetLevelDesc(level, &sd); OnRectUnlock(LockKey(t, level, 0), TextureId(t), level, 0, sd.Format, sd.Width, sd.Height); }
			return OUnlock(t, level);
		}
	};
	template <int N> struct CubeHook {
		static inline CubeLockFn OLock = nullptr; static inline CubeUnlockFn OUnlock = nullptr;
		static HRESULT STDMETHODCALLTYPE Lock(IDirect3DCubeTexture9* t, D3DCUBEMAP_FACES face, UINT level, D3DLOCKED_RECT* lr, const RECT* r, DWORD f) {
			if (!Want()) { Guard g; return OLock(t, face, level, lr, r, f); }
			Guard g; const HRESULT hr = OLock(t, face, level, lr, r, f); if (SUCCEEDED(hr)) OnRectLock(t, level, face, lr, r, f); return hr;
		}
		static HRESULT STDMETHODCALLTYPE Unlock(IDirect3DCubeTexture9* t, D3DCUBEMAP_FACES face, UINT level) {
			if (!Want()) { Guard g; return OUnlock(t, face, level); }
			Guard g;
			if (R->locks.count(LockKey(t, level, face))) { D3DSURFACE_DESC sd = {}; t->GetLevelDesc(level, &sd); OnRectUnlock(LockKey(t, level, face), TextureId(t), level, face, sd.Format, sd.Width, sd.Height); }
			return OUnlock(t, face, level);
		}
	};
	template <int N> struct SurfHook {
		static inline SurfLockFn OLock = nullptr; static inline SurfUnlockFn OUnlock = nullptr;
		static HRESULT STDMETHODCALLTYPE Lock(IDirect3DSurface9* s, D3DLOCKED_RECT* lr, const RECT* r, DWORD f) {
			if (!Want()) { Guard g; return OLock(s, lr, r, f); }
			Guard g; const HRESULT hr = OLock(s, lr, r, f); if (SUCCEEDED(hr)) OnRectLock(s, 0, 0, lr, r, f); return hr;
		}
		static HRESULT STDMETHODCALLTYPE Unlock(IDirect3DSurface9* s) {
			if (!Want()) { Guard g; return OUnlock(s); }
			Guard g;
			if (R->locks.count(LockKey(s, 0, 0))) {
				D3DSURFACE_DESC sd = {}; s->GetDesc(&sd);
				const DWORD id = SurfaceId(s);
				const Res& r = R->res[id];
				if (r.parent) OnRectUnlock(LockKey(s, 0, 0), r.parent, r.level, r.face, sd.Format, sd.Width, sd.Height);	// a texture level: an update of the texture
				else OnRectUnlock(LockKey(s, 0, 0), id, 0, 0, sd.Format, sd.Width, sd.Height);
			}
			return OUnlock(s);
		}
	};
	// Volume textures: contents copied at first use only (no mid-frame volume writes are expected; LockBox not hooked).

	// ---- NvAPI depth resolve (RenderManager::ResolveDepthBuffer) ----
	inline void NvStretchRect(IDirect3DResource9* src, IDirect3DResource9* dst) {
		if (!Want() || !src || !dst) return;
		Guard g;
		auto idOf = [](IDirect3DResource9* r) -> DWORD {
			const D3DRESOURCETYPE t = r->GetType();
			if (t == D3DRTYPE_SURFACE) return SurfaceId((IDirect3DSurface9*)r);
			if (t == D3DRTYPE_TEXTURE || t == D3DRTYPE_CUBETEXTURE || t == D3DRTYPE_VOLUMETEXTURE) return TextureId((IDirect3DBaseTexture9*)r);
			return 0;
		};
		const DWORD s = idOf(src), t = idOf(dst);
		Content(s); Written(t); if (t) R->res[t].content = true;
		WriteAction(kActNvStretchRect, { s, t });
	}

	// ---- names ----
	inline void Mark(DWORD kind, const char* name) {
		if (!Want() || !name) return;
		const size_t n = strlen(name) + 1;
		Begin(kRecMark, 4 + n); Write(&kind, 4); Write(name, n);
		R->marks++;
	}
	// SetShadersHook: the shaders the game binds next and their names.
	inline void NameShader(const void* shader, const char* name) {
		if (!Want() || !shader || !name) return;
		const DWORD id = Find(shader);
		if (id) { if (R->named.insert(id).second) NameId(id, name); return; }
		R->pendingNames.emplace(shader, name);
	}

	inline void CollectEffectNames() {
		if (!TheShaderManager) return;
		for (auto& e : TheShaderManager->EffectsNames) {
			EffectRecord* rec = e.second ? *e.second : nullptr;
			if (!rec || !rec->Effect) continue;
			ID3DXEffect* fx = rec->Effect;
			D3DXEFFECT_DESC ed = {};
			if (FAILED(fx->GetDesc(&ed))) continue;
			for (UINT t = 0; t < ed.Techniques; t++) {
				D3DXHANDLE th = fx->GetTechnique(t);
				D3DXTECHNIQUE_DESC td = {};
				if (!th || FAILED(fx->GetTechniqueDesc(th, &td))) continue;
				for (UINT p = 0; p < td.Passes; p++) {
					D3DXHANDLE ph = fx->GetPass(th, p);
					D3DXPASS_DESC pd = {};
					if (!ph || FAILED(fx->GetPassDesc(ph, &pd))) continue;
					char name[256];
					sprintf_s(name, "effect %s / %s / pass %u", e.first.c_str(), td.Name ? td.Name : "?", p);
					if (pd.pVertexShaderFunction) R->hashNames[Hash(pd.pVertexShaderFunction, D3DXGetShaderSize(pd.pVertexShaderFunction))] = std::string(name) + " (VS)";
					if (pd.pPixelShaderFunction) R->hashNames[Hash(pd.pPixelShaderFunction, D3DXGetShaderSize(pd.pPixelShaderFunction))] = std::string(name) + " (PS)";
				}
			}
		}
	}

	// ---- start / finish ----
	inline void Start() {
		R = new (std::nothrow) Recorder();
		if (!R) { State = kFailed; return; }
		R->res.resize(1);
		R->startMs = NowMs();
		if (fopen_s(&R->file, Path, "wb") || !R->file) { Logger::Log("FRAME RECORDER: could not open %s", Path); delete R; R = nullptr; State = kFailed; return; }
		setvbuf(R->file, nullptr, _IOFBF, 8 << 20);
		IDirect3DSurface9* bb = nullptr;
		D3DSURFACE_DESC bd = {};
		if (SUCCEEDED(Device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb) { bb->GetDesc(&bd); bb->Release(); }
		const DWORD header[6] = { kMagic, kVersion, kStateWords, bd.Width, bd.Height, (DWORD)bd.Format };
		Write(header, sizeof(header));
		CollectEffectNames();
		State = kRecording;
		Logger::Log("FRAME RECORDER: recording the next frame into %s (%ux%u)", Path, bd.Width, bd.Height);
	}

	inline void Finish() {
		// The picture as presented: the validation reference.
		IDirect3DSurface9* bb = nullptr;
		if (SUCCEEDED(Device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb) {
			SelfTestCore::ShotReader reader;
			std::vector<unsigned> bgra; unsigned w = 0, h = 0;
			if (reader.Read(Device, bb, bgra, w, h)) {
				Begin(kRecRefImage, 8 + bgra.size() * 4); const DWORD wh[2] = { w, h }; Write(wh, 8); Write(bgra.data(), bgra.size() * 4);
			}
			else Note("back buffer not read: %s", reader.error ? reader.error : "?");
			reader.Release();
			bb->Release();
		}
		WriteAction(kActPresent, {});
		const double ms = NowMs() - R->startMs;
		char info[8192];
		sprintf_s(info, "recorded %u actions (%u draws), %u state records, %u writes during the frame, %u marks | resources: %u textures, %u cube, %u volume, %u buffers, "
			"%u surfaces, %u shaders, %u declarations | %u render targets read back, %u default-pool texture levels from the upload journal, %u depth buffers with unknown contents, %u not readable | %.1f MB, %.0f ms\n%s",
			R->actions, R->draws, R->stateRecords, R->updates, R->marks, R->kinds[0], R->kinds[1], R->kinds[2], R->kinds[3], R->kinds[4], R->kinds[5], R->kinds[6],
			R->readbacks, R->journaled, R->depthUnknown, R->unreadable, R->bytes / 1048576.0, ms, R->notes.c_str());
		const size_t n = strlen(info) + 1;
		Begin(kRecInfo, n); Write(info, n);
		Begin(kRecEnd, 0);
		const bool ok = !ferror(R->file);
		fclose(R->file);
		Logger::Log("FRAME RECORDER: %s %s: %s", Path, ok ? "written" : "WRITE FAILED", info);
		for (Res& r : R->res) if (r.obj) r.obj->Release();
		delete R;
		R = nullptr;
		State = ok ? kDone : kFailed;
	}

	// ---- install (once, from the render thread) ----
	template <class Fn> inline void AttachOnce(std::unordered_set<void*>& seen, Fn& original, void* target, void* hook, bool& ok) {
		if (!target || seen.count(target)) return;
		seen.insert(target);
		original = (Fn)target;
		if (DetourAttach(&(PVOID&)original, hook) != NO_ERROR) ok = false;
	}

	inline bool Install(IDirect3DDevice9* device) {
		if (Installed) return true;
		if (InstallFailed || !device) return false;
		Device = device;
		void** v = *(void***)device;
		// Probe objects: the classes the game's resources use (managed, default dynamic, default static, system memory).
		IDirect3DVertexBuffer9* vb[4] = {}; IDirect3DIndexBuffer9* ib[4] = {}; IDirect3DTexture9* tex[4] = {}; IDirect3DCubeTexture9* cube[2] = {};
		IDirect3DSurface9* surf[4] = {};
		device->CreateVertexBuffer(64, 0, 0, D3DPOOL_MANAGED, &vb[0], nullptr);
		device->CreateVertexBuffer(64, D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT, &vb[1], nullptr);
		device->CreateVertexBuffer(64, D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT, &vb[2], nullptr);
		device->CreateVertexBuffer(64, 0, 0, D3DPOOL_SYSTEMMEM, &vb[3], nullptr);
		device->CreateIndexBuffer(64, 0, D3DFMT_INDEX16, D3DPOOL_MANAGED, &ib[0], nullptr);
		device->CreateIndexBuffer(64, D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_DEFAULT, &ib[1], nullptr);
		device->CreateIndexBuffer(64, D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_DEFAULT, &ib[2], nullptr);
		device->CreateIndexBuffer(64, 0, D3DFMT_INDEX16, D3DPOOL_SYSTEMMEM, &ib[3], nullptr);
		device->CreateTexture(4, 4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex[0], nullptr);
		device->CreateTexture(4, 4, 1, D3DUSAGE_DYNAMIC, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &tex[1], nullptr);
		device->CreateTexture(4, 4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &tex[2], nullptr);
		device->CreateTexture(4, 4, 1, 0, D3DFMT_DXT1, D3DPOOL_MANAGED, &tex[3], nullptr);
		device->CreateCubeTexture(4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &cube[0], nullptr);
		device->CreateCubeTexture(4, 1, D3DUSAGE_DYNAMIC, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &cube[1], nullptr);
		device->CreateOffscreenPlainSurface(4, 4, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &surf[0], nullptr);
		if (tex[0]) tex[0]->GetSurfaceLevel(0, &surf[1]);
		if (tex[1]) tex[1]->GetSurfaceLevel(0, &surf[2]);
		if (tex[2]) tex[2]->GetSurfaceLevel(0, &surf[3]);

		bool ok = true;
		std::unordered_set<void*> seen;
		DetourTransactionBegin();
		DetourUpdateThread(GetCurrentThread());
		AttachOnce(seen, OPresent, v[17], (void*)&HPresent, ok);
		AttachOnce(seen, OUpdateSurface, v[30], (void*)&HUpdateSurface, ok);
		AttachOnce(seen, OUpdateTexture, v[31], (void*)&HUpdateTexture, ok);
		AttachOnce(seen, OStretchRect, v[34], (void*)&HStretchRect, ok);
		AttachOnce(seen, OColorFill, v[35], (void*)&HColorFill, ok);
		AttachOnce(seen, OBeginScene, v[41], (void*)&HBeginScene, ok);
		AttachOnce(seen, OEndScene, v[42], (void*)&HEndScene, ok);
		AttachOnce(seen, OClear, v[43], (void*)&HClear, ok);
		AttachOnce(seen, ODraw, v[81], (void*)&HDraw, ok);
		AttachOnce(seen, ODrawIndexed, v[82], (void*)&HDrawIndexed, ok);
		AttachOnce(seen, ODrawUP, v[83], (void*)&HDrawUP, ok);
		AttachOnce(seen, ODrawIndexedUP, v[84], (void*)&HDrawIndexedUP, ok);
		// Buffers (vertex and index share the hook body: it asks the object its type).
		unsigned bufSlot = 0;
		auto buf = [&](IDirect3DResource9* r) {
			if (!r) return;
			void** t = *(void***)r;
			void* lock = t[11]; void* unlock = t[12];
			if (seen.count(lock) || bufSlot >= 6) return;
			switch (bufSlot++) {
			case 0: AttachOnce(seen, BufHook<0>::OLock, lock, (void*)&BufHook<0>::Lock, ok); AttachOnce(seen, BufHook<0>::OUnlock, unlock, (void*)&BufHook<0>::Unlock, ok); break;
			case 1: AttachOnce(seen, BufHook<1>::OLock, lock, (void*)&BufHook<1>::Lock, ok); AttachOnce(seen, BufHook<1>::OUnlock, unlock, (void*)&BufHook<1>::Unlock, ok); break;
			case 2: AttachOnce(seen, BufHook<2>::OLock, lock, (void*)&BufHook<2>::Lock, ok); AttachOnce(seen, BufHook<2>::OUnlock, unlock, (void*)&BufHook<2>::Unlock, ok); break;
			case 3: AttachOnce(seen, BufHook<3>::OLock, lock, (void*)&BufHook<3>::Lock, ok); AttachOnce(seen, BufHook<3>::OUnlock, unlock, (void*)&BufHook<3>::Unlock, ok); break;
			case 4: AttachOnce(seen, BufHook<4>::OLock, lock, (void*)&BufHook<4>::Lock, ok); AttachOnce(seen, BufHook<4>::OUnlock, unlock, (void*)&BufHook<4>::Unlock, ok); break;
			case 5: AttachOnce(seen, BufHook<5>::OLock, lock, (void*)&BufHook<5>::Lock, ok); AttachOnce(seen, BufHook<5>::OUnlock, unlock, (void*)&BufHook<5>::Unlock, ok); break;
			}
		};
		for (auto* r : vb) buf(r);
		for (auto* r : ib) buf(r);
		unsigned texSlot = 0;
		for (auto* r : tex) {
			if (!r) continue;
			void** t = *(void***)r;
			if (seen.count(t[19]) || texSlot >= 3) continue;
			switch (texSlot++) {
			case 0: AttachOnce(seen, TexHook<0>::OLock, t[19], (void*)&TexHook<0>::Lock, ok); AttachOnce(seen, TexHook<0>::OUnlock, t[20], (void*)&TexHook<0>::Unlock, ok); break;
			case 1: AttachOnce(seen, TexHook<1>::OLock, t[19], (void*)&TexHook<1>::Lock, ok); AttachOnce(seen, TexHook<1>::OUnlock, t[20], (void*)&TexHook<1>::Unlock, ok); break;
			case 2: AttachOnce(seen, TexHook<2>::OLock, t[19], (void*)&TexHook<2>::Lock, ok); AttachOnce(seen, TexHook<2>::OUnlock, t[20], (void*)&TexHook<2>::Unlock, ok); break;
			}
		}
		unsigned cubeSlot = 0;
		for (auto* r : cube) {
			if (!r) continue;
			void** t = *(void***)r;
			if (seen.count(t[19]) || cubeSlot >= 2) continue;
			switch (cubeSlot++) {
			case 0: AttachOnce(seen, CubeHook<0>::OLock, t[19], (void*)&CubeHook<0>::Lock, ok); AttachOnce(seen, CubeHook<0>::OUnlock, t[20], (void*)&CubeHook<0>::Unlock, ok); break;
			case 1: AttachOnce(seen, CubeHook<1>::OLock, t[19], (void*)&CubeHook<1>::Lock, ok); AttachOnce(seen, CubeHook<1>::OUnlock, t[20], (void*)&CubeHook<1>::Unlock, ok); break;
			}
		}
		unsigned surfSlot = 0;
		for (auto* r : surf) {
			if (!r) continue;
			void** t = *(void***)r;
			if (seen.count(t[13]) || surfSlot >= 4) continue;
			switch (surfSlot++) {
			case 0: AttachOnce(seen, SurfHook<0>::OLock, t[13], (void*)&SurfHook<0>::Lock, ok); AttachOnce(seen, SurfHook<0>::OUnlock, t[14], (void*)&SurfHook<0>::Unlock, ok); break;
			case 1: AttachOnce(seen, SurfHook<1>::OLock, t[13], (void*)&SurfHook<1>::Lock, ok); AttachOnce(seen, SurfHook<1>::OUnlock, t[14], (void*)&SurfHook<1>::Unlock, ok); break;
			case 2: AttachOnce(seen, SurfHook<2>::OLock, t[13], (void*)&SurfHook<2>::Lock, ok); AttachOnce(seen, SurfHook<2>::OUnlock, t[14], (void*)&SurfHook<2>::Unlock, ok); break;
			case 3: AttachOnce(seen, SurfHook<3>::OLock, t[13], (void*)&SurfHook<3>::Lock, ok); AttachOnce(seen, SurfHook<3>::OUnlock, t[14], (void*)&SurfHook<3>::Unlock, ok); break;
			}
		}
		const LONG committed = ok ? DetourTransactionCommit() : (DetourTransactionAbort(), -1);
		for (auto* r : surf) if (r) r->Release();
		for (auto* r : vb) if (r) r->Release();
		for (auto* r : ib) if (r) r->Release();
		for (auto* r : tex) if (r) r->Release();
		for (auto* r : cube) if (r) r->Release();
		if (committed != NO_ERROR) { InstallFailed = true; Logger::Log("FRAME RECORDER: detours FAILED (%ld); nothing recorded", committed); return false; }
		Installed = true;
		Logger::Log("FRAME RECORDER: hooks installed (%u buffer, %u texture, %u cube, %u surface lock classes)", bufSlot, texSlot, cubeSlot, surfSlot);
		return true;
	}

	// RenderManager::Initialize (device start): when the shot list asks for 'record', the hooks go in now so the upload journal
	// sees the game's textures arrive (NVTF default pool). Nothing happens without the list.
	inline void EarlyStart(IDirect3DDevice9* device, const char* list) {
		FILE* f = nullptr;
		if (fopen_s(&f, list, "r") || !f) return;
		char line[512]; bool wanted = false;
		while (fgets(line, sizeof(line), f)) { const char* p = line; while (*p == ' ' || *p == '\t') p++; if (!strncmp(p, "record", 6) && (p[6] == 0 || isspace((unsigned char)p[6]))) wanted = true; }
		fclose(f);
		if (!wanted) return;
		if (Install(device)) Journal::Start();
	}

	// SettingShots: record the next whole frame (from this frame's Present to the next one) into path.
	inline bool Arm(IDirect3DDevice9* device, const char* path) {
		if (State == kArmed || State == kRecording) return false;
		D3DDEVICE_CREATION_PARAMETERS cp = {};
		// The game asks for a pure device, but Windows' d3d9 still answers the Get* calls; check that it does instead of trusting the flag.
		if (SUCCEEDED(device->GetCreationParameters(&cp)) && (cp.BehaviorFlags & D3DCREATE_PUREDEVICE)) {
			DWORD v = 0; float f[4] = {}; IDirect3DBaseTexture9* t = nullptr; D3DVIEWPORT9 vp = {};
			const HRESULT a = device->GetRenderState(D3DRS_ZFUNC, &v), b = device->GetVertexShaderConstantF(0, f, 1), c = device->GetTexture(0, &t), d = device->GetViewport(&vp);
			if (t) t->Release();
			Logger::Log("FRAME RECORDER: the device was made with D3DCREATE_PUREDEVICE (flags %08lX); Get* answers %08lX %08lX %08lX %08lX", cp.BehaviorFlags, (unsigned long)a, (unsigned long)b, (unsigned long)c, (unsigned long)d);
			if (FAILED(a) || FAILED(b) || FAILED(c) || FAILED(d)) { Logger::Log("FRAME RECORDER: the state cannot be read back; not recorded"); State = kFailed; return false; }
		}
		if (!Install(device)) { State = kFailed; return false; }
		strcpy_s(Path, path);
		Thread = GetCurrentThreadId();
		State = kArmed;
		return true;
	}
}
