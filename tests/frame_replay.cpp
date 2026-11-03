// UNOFFICIAL, TEST ONLY (2026-10-17): frame replayer for New Vegas Reloaded captures (.nvrf, src/core/FrameFormat.h, written by
// src/core/FrameRecorder.h when a shot list says 'record'). Recreates every resource of the recorded frame on a native D3D9 HAL
// device, plays the frame back many times with GPU timestamps around the whole frame and around named ranges, and runs a small
// script of A/B changes and diagnosis passes. See bench\deep-roadmap\replay\NOTES.md for the commands.
//
//   frame_replay.exe <capture.nvrf> [script.txt] [--warmup N] [--runs N] [--out DIR] [--list] [--no-validate] [--info] [--d3dx] [--no-preshader]
//   frame_replay.exe --compile <file.hlsl> <ps_3_0|vs_3_0|...> <out.bin> [NAME=VALUE ...]
//
// Script (one command per line, '#' comment, names with spaces in double quotes):
//   range <name> <predicate>                 time the matching actions as one range (intervals where they run)
//   ps <predicate> <file> / vs <predicate> <file>   replace the shader (file: bytecode, or .hlsl compiled like the game)
//   rs <predicate> <D3DRS name|number> <value>      override a render state on matching draws
//   ss <predicate> <stage> <D3DSAMP name|number> <value>   override a sampler state on matching draws
//   skip <predicate>                         leave the matching actions out
//   move <predicate> before <predicate>      move the matching actions (in order) before the first action of the second
//   count <predicate> [name]                 overdraw: layers per pixel of the matching draws (4 variants), maps + histograms
//   occlusion <predicate> [name]             samples passed per matching draw (occlusion queries)
//   dump <action index> <rt0|rt1|rt2|rt3|depth> <file.png>   save a target after that action
//   states                                   depth-state report (compare directions of depth-writing draws, depth clears)
// Predicates: all | draws A-B | actions A-B | ps X | vs X | marker NAME | split NAME | rt ID | tex ID | not P | P and P | P or P
//   X = shader name substring (case-insensitive), bytecode hash prefix (hex) or id:N. marker = an NVR GPU timer (begin..end),
//   split = an F10 shader-family interval ("world scene/GRASS/vanilla"; substring match).
#define NOMINMAX
#include <windows.h>
#include <d3d9.h>
#include <d3dx9.h>
#include <wincodec.h>
#include "../lib/Nvidia/nvapi.h"
#include <algorithm>
#include <cfloat>
#include <cstdarg>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>
#include "../src/core/FrameFormat.h"

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "windowscodecs.lib")

using namespace FrameFormat;

static void Fail(const char* format, ...) {
	va_list a; va_start(a, format); vfprintf(stderr, format, a); va_end(a);
	fprintf(stderr, "\n");
	exit(1);
}
static std::string Lower(std::string s) { for (char& c : s) c = (char)tolower((unsigned char)c); return s; }

// ---------------------------------------------------------------- compile (the game's path: D3DX43 preprocess, then compile, flags 0)
static std::vector<unsigned char> CompileFile(const std::string& file, const char* profile, const std::vector<std::pair<std::string, std::string>>& defines, std::string& error) {
	std::vector<D3DXMACRO> macros;
	for (auto& d : defines) macros.push_back({ d.first.c_str(), d.second.c_str() });
	macros.push_back({ nullptr, nullptr });
	ID3DXBuffer* source = nullptr; ID3DXBuffer* errors = nullptr; ID3DXBuffer* code = nullptr;
	HRESULT hr = D3DXPreprocessShaderFromFileA(file.c_str(), macros.data(), nullptr, &source, &errors);
	if (FAILED(hr)) { error = errors ? (const char*)errors->GetBufferPointer() : "preprocess failed"; if (errors) errors->Release(); return {}; }
	if (errors) { errors->Release(); errors = nullptr; }
	hr = D3DXCompileShader((const char*)source->GetBufferPointer(), source->GetBufferSize(), nullptr, nullptr, "main", profile, 0, &code, &errors, nullptr);
	source->Release();
	if (FAILED(hr)) { error = errors ? (const char*)errors->GetBufferPointer() : "compile failed"; if (errors) errors->Release(); return {}; }
	if (errors) errors->Release();
	std::vector<unsigned char> out((unsigned char*)code->GetBufferPointer(), (unsigned char*)code->GetBufferPointer() + code->GetBufferSize());
	code->Release();
	return out;
}
static std::vector<unsigned char> CompileText(const char* text, const char* profile) {
	ID3DXBuffer* code = nullptr; ID3DXBuffer* errors = nullptr;
	if (FAILED(D3DXCompileShader(text, (UINT)strlen(text), nullptr, nullptr, "main", profile, 0, &code, &errors, nullptr)))
		Fail("internal shader (%s) did not compile: %s", profile, errors ? (const char*)errors->GetBufferPointer() : "?");
	if (errors) errors->Release();
	std::vector<unsigned char> out((unsigned char*)code->GetBufferPointer(), (unsigned char*)code->GetBufferPointer() + code->GetBufferSize());
	code->Release();
	return out;
}

// ---------------------------------------------------------------- effects: EffectRecord::LoadEffect's path
// D3DXPreprocessShaderFromFile (with NVR's effect macros) -> D3DXCreateEffectCompiler on the preprocessed text -> CompileEffect
// -> D3DXCreateEffect (here on a NULLREF device) -> the pass's shader bytecode. A plain compile of the pass function does not
// give the same bytecode (registers are allocated over the whole effect).
static std::map<unsigned long long, std::string> CaptureShaderHashes(const char* capture) {
	std::map<unsigned long long, std::string> out;
	FILE* f = nullptr;
	if (!capture || fopen_s(&f, capture, "rb") || !f) return out;
	DWORD header[6]; fread(header, sizeof(header), 1, f);
	std::map<DWORD, unsigned long long> idHash;
	DWORD h[2];
	std::vector<unsigned char> buf;
	while (fread(h, 8, 1, f) == 1 && h[0] != kRecEnd) {
		if (h[0] == kRecShader || h[0] == kRecName) {
			buf.resize(h[1]); fread(buf.data(), 1, h[1], f);
			const DWORD id = *(const DWORD*)buf.data();
			if (h[0] == kRecShader) { const unsigned long long hh = Hash(buf.data() + 8, buf.size() - 8); idHash[id] = hh; out[hh]; }
			else if (idHash.count(id)) { std::string& n = out[idHash[id]]; const std::string name((const char*)buf.data() + 4); if (n.find(name) == std::string::npos) n += (n.empty() ? "" : " | ") + name; }
		}
		else _fseeki64(f, h[1], SEEK_CUR);
	}
	fclose(f);
	return out;
}

static IDirect3DDevice9* NullRefDevice() {
	static IDirect3DDevice9* device = nullptr;
	if (device) return device;
	IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
	D3DPRESENT_PARAMETERS pp = {}; pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.BackBufferWidth = 64; pp.BackBufferHeight = 64; pp.hDeviceWindow = GetDesktopWindow();
	if (!d3d || FAILED(d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_NULLREF, GetDesktopWindow(), D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &device))) Fail("no NULLREF device for effect compiles");
	return device;
}

// 2026-11-02: the game compiles effects with d3dcompiler_47 (D3DCompile, fx_2_0, flags 0, on the preprocessed source) since
// [Main.Main.Performance] NewEffectCompiler (2026-10-29, EffectCompiler47.h, on by default); --d3dx = the old D3DX effect compiler.
static bool UseCompiler47 = true;
// --no-preshader: effects compiled with D3DCOMPILE_NO_PRESHADER, for captures recorded with the test setting
// [Main.Main.Performance] EffectNoPreshaders (EffectCompiler47.h). Such a frame holds the effects' raw parameters instead of the
// results of their preshaders (CPU-side expressions), so a rewritten effect swapped in by 'ps' is fed correctly by name.
static UINT EffectFlags = 0;
typedef HRESULT(WINAPI* D3DCompileFn)(LPCVOID, SIZE_T, LPCSTR, const void*, void*, LPCSTR, LPCSTR, UINT, UINT, ID3DXBuffer**, ID3DXBuffer**);
static D3DCompileFn Compile47() {
	static const D3DCompileFn fn = []() -> D3DCompileFn {
		HMODULE module = LoadLibraryA("d3dcompiler_47.dll");
		return module ? (D3DCompileFn)GetProcAddress(module, "D3DCompile") : nullptr;
	}();
	return fn;
}

// Compiles an NVR effect the game's way; returns the chosen pass's bytecode (pixel or vertex).
static std::vector<unsigned char> CompileEffectPass(const std::string& file, const std::string& technique, int pass, bool pixel,
	const std::vector<std::pair<std::string, std::string>>& defines, std::string& error, unsigned long long* otherHash = nullptr) {
	std::vector<D3DXMACRO> macros;
	for (auto& d : defines) macros.push_back({ d.first.c_str(), d.second.c_str() });
	macros.push_back({ nullptr, nullptr });
	ID3DXBuffer *source = nullptr, *errors = nullptr, *compiled = nullptr;
	ID3DXEffectCompiler* compiler = nullptr; ID3DXEffect* fx = nullptr;
	std::vector<unsigned char> out;
	if (FAILED(D3DXPreprocessShaderFromFileA(file.c_str(), macros.data(), nullptr, &source, &errors))) { error = errors ? (const char*)errors->GetBufferPointer() : "preprocess failed"; }
	else if (errors) { errors->Release(); errors = nullptr; }
	if (source && UseCompiler47) {
		std::string name = file.substr(file.find_last_of("/\\") + 1);
		if (name.size() > 8 && name.compare(name.size() - 8, 8, ".fx.hlsl") == 0) name.resize(name.size() - 8);	// the game passes the effect's name
		if (!Compile47()) error = "no d3dcompiler_47.dll (use --d3dx)";
		else if (FAILED(Compile47()(source->GetBufferPointer(), source->GetBufferSize(), name.c_str(), nullptr, nullptr, nullptr, "fx_2_0", EffectFlags, 0, &compiled, &errors)))
			error = errors ? (const char*)errors->GetBufferPointer() : "D3DCompile failed";
	}
	else {
		if (source && FAILED(D3DXCreateEffectCompiler((const char*)source->GetBufferPointer(), source->GetBufferSize(), nullptr, nullptr, 0, &compiler, &errors))) error = errors ? (const char*)errors->GetBufferPointer() : "effect compiler failed";
		if (errors) { errors->Release(); errors = nullptr; }
		if (compiler && FAILED(compiler->CompileEffect(EffectFlags, &compiled, &errors))) error = errors ? (const char*)errors->GetBufferPointer() : "CompileEffect failed";
	}
	if (errors) { errors->Release(); errors = nullptr; }
	if (compiled && FAILED(D3DXCreateEffect(NullRefDevice(), compiled->GetBufferPointer(), compiled->GetBufferSize(), nullptr, nullptr, D3DXFX_LARGEADDRESSAWARE, nullptr, &fx, &errors))) error = errors ? (const char*)errors->GetBufferPointer() : "D3DXCreateEffect failed";
	if (fx) {
		D3DXHANDLE t = nullptr;
		const bool numeric = !technique.empty() && technique.find_first_not_of("0123456789") == std::string::npos;
		t = numeric ? fx->GetTechnique(atoi(technique.c_str())) : fx->GetTechniqueByName(technique.c_str());
		D3DXHANDLE ph = t ? fx->GetPass(t, pass) : nullptr;
		D3DXPASS_DESC pd = {};
		if (!t) error = "no technique '" + technique + "'";
		else if (!ph || FAILED(fx->GetPassDesc(ph, &pd))) error = "no pass " + std::to_string(pass);
		else {
			const DWORD* code = pixel ? pd.pPixelShaderFunction : pd.pVertexShaderFunction;
			const DWORD* other = pixel ? pd.pVertexShaderFunction : pd.pPixelShaderFunction;
			if (!code) error = pixel ? "the pass has no pixel shader" : "the pass has no vertex shader";
			else out.assign((const unsigned char*)code, (const unsigned char*)code + D3DXGetShaderSize(code));
			if (otherHash) *otherHash = other ? Hash(other, D3DXGetShaderSize(other)) : 0;
		}
	}
	if (fx) fx->Release(); if (compiled) compiled->Release(); if (compiler) compiler->Release(); if (source) source->Release(); if (errors) errors->Release();
	return out;
}

// frame_replay --compile-effect <file.fx.hlsl> <technique name|index> <pass> <out.bin> [ps|vs] [--check <capture.nvrf>] [--d3dx] [--no-preshader] [NAME=VALUE ...]
// Without FORWARD_SHADOWS / PHYSICAL_SKY_ON / REVERSED_DEPTH given, the 8 combinations NVR can use are tried and, with --check,
// the one whose bytecode is in the capture is kept (so an unchanged source shows that the path reproduces the game's bytecode).
static int CompileEffectCommand(int argc, char** argv) {
	if (argc < 6) Fail("--compile-effect <file.fx.hlsl> <technique|index> <pass> <out.bin> [ps|vs] [--check capture.nvrf] [NAME=VALUE ...]");
	const std::string file = argv[2], technique = argv[3], outPath = argv[5];
	const int pass = atoi(argv[4]);
	bool pixel = true; const char* check = nullptr;
	std::vector<std::pair<std::string, std::string>> user;
	for (int i = 6; i < argc; i++) {
		const std::string a = argv[i];
		if (a == "ps") pixel = true; else if (a == "vs") pixel = false;
		else if (a == "--check" && i + 1 < argc) check = argv[++i];
		else if (a == "--d3dx") UseCompiler47 = false;
		else if (a == "--no-preshader") EffectFlags = D3DXSHADER_NO_PRESHADER;
		else { const size_t eq = a.find('='); user.push_back({ a.substr(0, eq), eq == std::string::npos ? "" : a.substr(eq + 1) }); }
	}
	auto given = [&](const char* n) { for (auto& d : user) if (d.first == n) return true; return false; };
	const auto known = CaptureShaderHashes(check);
	std::vector<std::vector<std::pair<std::string, std::string>>> tries;
	for (int c = 0; c < 8; c++) {
		auto d = user;
		if (!given("FORWARD_SHADOWS")) d.push_back({ "FORWARD_SHADOWS", (c & 1) ? "1" : "0" }); else if (c & 1) continue;
		if (!given("PHYSICAL_SKY_ON")) d.push_back({ "PHYSICAL_SKY_ON", (c & 2) ? "1" : "0" }); else if (c & 2) continue;
		if (!given("REVERSED_DEPTH")) { if (c & 4) d.push_back({ "REVERSED_DEPTH", "" }); } else if (c & 4) continue;
		tries.push_back(d);
	}
	std::vector<unsigned char> best; std::string bestDefs; bool matched = false;
	for (auto& d : tries) {
		std::string error; unsigned long long other = 0;
		auto code = CompileEffectPass(file, technique, pass, pixel, d, error, &other);
		std::string defs; for (auto& x : d) defs += x.first + (x.second.empty() ? "" : "=" + x.second) + " ";
		if (code.empty()) { printf("  [%s] %s\n", defs.c_str(), error.c_str()); continue; }
		const unsigned long long h = Hash(code.data(), code.size());
		const bool in = known.count(h) != 0;
		printf("  [%s] %s %016llx (%u bytes)%s%s\n", defs.c_str(), pixel ? "PS" : "VS", h, (unsigned)code.size(), check ? (in ? "  IN THE CAPTURE: " : "  not in the capture") : "", in ? known.at(h).c_str() : "");
		if (best.empty() || (in && !matched)) { best = code; bestDefs = defs; matched = in; }
		if (!check && tries.size() > 1 && &d == &tries.front()) {}
	}
	if (best.empty()) return 1;
	FILE* f = nullptr;
	if (fopen_s(&f, outPath.c_str(), "wb") || !f) Fail("cannot write %s", outPath.c_str());
	fwrite(best.data(), 1, best.size(), f); fclose(f);
	printf("wrote %s: %s pass %d of '%s' with [%s] hash %016llx%s\n", outPath.c_str(), pixel ? "PS" : "VS", pass, technique.c_str(), bestDefs.c_str(),
		Hash(best.data(), best.size()), check ? (matched ? " (matches the capture)" : " (NOT in the capture: changed source, or other defines)") : "");
	return 0;
}

// ---------------------------------------------------------------- pictures
static bool SavePng(const std::string& path, unsigned width, unsigned height, const std::vector<unsigned>& bgra) {
	IWICImagingFactory* factory = nullptr; IWICStream* stream = nullptr; IWICBitmapEncoder* encoder = nullptr; IWICBitmapFrameEncode* frame = nullptr;
	wchar_t wide[MAX_PATH] = L"";
	MultiByteToWideChar(CP_ACP, 0, path.c_str(), -1, wide, MAX_PATH);
	bool ok = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) &&
		SUCCEEDED(factory->CreateStream(&stream)) && SUCCEEDED(stream->InitializeFromFilename(wide, GENERIC_WRITE)) &&
		SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) && SUCCEEDED(encoder->Initialize(stream, WICBitmapEncoderNoCache)) &&
		SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) && SUCCEEDED(frame->Initialize(nullptr)) && SUCCEEDED(frame->SetSize(width, height));
	if (ok) {
		WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
		ok = SUCCEEDED(frame->SetPixelFormat(&format)) && IsEqualGUID(format, GUID_WICPixelFormat32bppBGRA) &&
			SUCCEEDED(frame->WritePixels(height, width * 4, width * height * 4, (BYTE*)bgra.data())) && SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
	}
	if (frame) frame->Release();
	if (encoder) encoder->Release();
	if (stream) stream->Release();
	if (factory) factory->Release();
	return ok;
}
static float Half(unsigned short h) {
	const unsigned s = (h >> 15) & 1, e = (h >> 10) & 31, m = h & 1023;
	float v = e == 0 ? m / 1024.0f * powf(2, -14) : e == 31 ? (m ? NAN : INFINITY) : (1 + m / 1024.0f) * powf(2.0f, (float)e - 15);
	return s ? -v : v;
}
// One pixel of a read-back surface as RGBA floats (false: format not handled).
static bool Pixel(D3DFORMAT f, const unsigned char* p, float* c) {
	c[0] = c[1] = c[2] = 0; c[3] = 1;
	switch ((unsigned)f) {
	case D3DFMT_A8R8G8B8: case D3DFMT_X8R8G8B8: c[2] = p[0] / 255.f; c[1] = p[1] / 255.f; c[0] = p[2] / 255.f; c[3] = f == D3DFMT_A8R8G8B8 ? p[3] / 255.f : 1; return true;
	case D3DFMT_A8B8G8R8: case D3DFMT_X8B8G8R8: c[0] = p[0] / 255.f; c[1] = p[1] / 255.f; c[2] = p[2] / 255.f; c[3] = p[3] / 255.f; return true;
	case D3DFMT_A2R10G10B10: { const unsigned v = *(const unsigned*)p; c[2] = (v & 1023) / 1023.f; c[1] = ((v >> 10) & 1023) / 1023.f; c[0] = ((v >> 20) & 1023) / 1023.f; return true; }
	case D3DFMT_A2B10G10R10: { const unsigned v = *(const unsigned*)p; c[0] = (v & 1023) / 1023.f; c[1] = ((v >> 10) & 1023) / 1023.f; c[2] = ((v >> 20) & 1023) / 1023.f; return true; }
	case D3DFMT_A16B16G16R16F: { const unsigned short* h = (const unsigned short*)p; for (int i = 0; i < 4; i++) c[i] = Half(h[i]); return true; }
	case D3DFMT_A16B16G16R16: { const unsigned short* h = (const unsigned short*)p; for (int i = 0; i < 4; i++) c[i] = h[i] / 65535.f; return true; }
	case D3DFMT_G16R16F: { const unsigned short* h = (const unsigned short*)p; c[0] = Half(h[0]); c[1] = Half(h[1]); return true; }
	case D3DFMT_G16R16: { const unsigned short* h = (const unsigned short*)p; c[0] = h[0] / 65535.f; c[1] = h[1] / 65535.f; return true; }
	case D3DFMT_R16F: c[0] = c[1] = c[2] = Half(*(const unsigned short*)p); return true;
	case D3DFMT_R32F: c[0] = c[1] = c[2] = *(const float*)p; return true;
	case D3DFMT_G32R32F: c[0] = ((const float*)p)[0]; c[1] = ((const float*)p)[1]; return true;
	case D3DFMT_A32B32G32R32F: for (int i = 0; i < 4; i++) c[i] = ((const float*)p)[i]; return true;
	case D3DFMT_L8: c[0] = c[1] = c[2] = p[0] / 255.f; return true;
	case D3DFMT_A8: c[3] = p[0] / 255.f; c[0] = c[1] = c[2] = p[0] / 255.f; return true;
	}
	return false;
}
static unsigned Byte(float v) { if (!(v > 0)) return 0; if (v >= 1) return 255; return (unsigned)(v * 255.0f + 0.5f); }

// ---------------------------------------------------------------- capture contents
struct Resource {
	DWORD record = 0;	// kRecTexture ... kRecDecl
	FrameTexDesc tex = {}; FrameBufferDesc buf = {}; FrameSurfaceDesc surf = {}; FrameSubSurface sub = {};
	IDirect3DBaseTexture9* texture = nullptr; IDirect3DSurface9* surface = nullptr; IDirect3DVertexBuffer9* vb = nullptr; IDirect3DIndexBuffer9* ib = nullptr;
	IDirect3DVertexShader9* vs = nullptr; IDirect3DPixelShader9* ps = nullptr; IDirect3DVertexDeclaration9* decl = nullptr;
	std::vector<unsigned char> code;	// shaders: bytecode
	unsigned long long hash = 0;
	std::vector<std::string> names;
	bool updated = false;				// written during the frame: restored before every replay
	std::vector<unsigned char> initial;	// buffers with updates: the contents at the frame's start
	IDirect3DBaseTexture9* restoreTexture = nullptr;	// default-pool textures with updates: the contents at the frame's start
	bool nvRegistered = false;
	bool missing = false;
	bool isPixel = false;
};

enum OpType { kOpAction = 0, kOpUpdate = 1 };
struct Op {
	OpType kind = kOpAction;
	FrameAction act = {};
	std::vector<unsigned char> data;
	FrameUpdate upd = {};
	IDirect3DSurface9* staging = nullptr;	// texture updates into default-pool textures
	int state = -1;
	int index = 0;		// action index in the recorded order
	int draw = -1;		// draw number (draws only)
	bool skip = false;
	std::vector<unsigned short> ranges;	// sorted
	std::string marker, split;			// innermost marker, current split (for listings)
	bool IsDraw() const { return kind == kOpAction && act.type >= kActDraw && act.type <= kActDrawIndexedUP; }
};

struct Range { std::string name; bool user = false; std::vector<double> ms; };

// A render target's contents at the frame's start (read back in the game). Restored before every replay by drawing the GPU copy
// (tex) into it - written by the ROPs like the game's own draws, so the driver's colour compression state matches - or, with
// --restore-copy or where a texture of the format cannot be made, by StretchRect from a surface copy / UpdateSurface.
struct Restore { IDirect3DSurface9* sys = nullptr; IDirect3DSurface9* dst = nullptr; IDirect3DSurface9* copy = nullptr; IDirect3DTexture9* tex = nullptr; };
static bool RestoreByCopy = false;
struct DepthRestore { IDirect3DTexture9* values = nullptr; IDirect3DSurface9* dst = nullptr; UINT width = 0, height = 0; };	// R32F copy -> depth via oDepth
static std::vector<DepthRestore> DepthRestores;

static IDirect3DDevice9* D = nullptr;
static std::vector<Resource> Res;
static std::vector<FrameState> States;
static std::vector<Op> Ops;
static std::vector<Range> Ranges;
static std::vector<Restore> Restores;
static std::string Info;
static DWORD Width = 0, Height = 0, BBFormat = 0;
static std::vector<unsigned> RefImage;
static DWORD RefW = 0, RefH = 0;
static IDirect3DSurface9* BackBuffer = nullptr;
static bool NvOk = false;
static NvPhysicalGpuHandle NvGpu = nullptr;
// The GPU's current graphics and memory clocks in MHz (read only, NvAPI), to see whether a slow run is a clock change.
static void GpuClocks(double& graphics, double& memory) {
	graphics = memory = 0;
	if (!NvOk) return;
	if (!NvGpu) { NvPhysicalGpuHandle h[NVAPI_MAX_PHYSICAL_GPUS] = {}; NvU32 n = 0; if (NvAPI_EnumPhysicalGPUs(h, &n) != NVAPI_OK || !n) return; NvGpu = h[0]; }
	NV_GPU_CLOCK_FREQUENCIES f = {}; f.version = NV_GPU_CLOCK_FREQUENCIES_VER; f.ClockType = NV_GPU_CLOCK_FREQUENCIES_CURRENT_FREQ;
	if (NvAPI_GPU_GetAllClockFrequencies(NvGpu, &f) != NVAPI_OK) return;
	graphics = f.domain[NVAPI_GPU_PUBLIC_CLOCK_GRAPHICS].frequency / 1000.0;
	memory = f.domain[NVAPI_GPU_PUBLIC_CLOCK_MEMORY].frequency / 1000.0;
}
static unsigned Warnings = 0;
static void Warn(const char* format, ...) {
	if (Warnings++ > 60) return;
	va_list a; va_start(a, format); printf("warning: "); vprintf(format, a); printf("\n"); va_end(a);
}

static Resource& R(DWORD id) { if (id >= Res.size()) Res.resize(id + 1); return Res[id]; }

static D3DPOOL ReplayPool(DWORD pool) { return pool == D3DPOOL_MANAGED ? D3DPOOL_DEFAULT : (D3DPOOL)pool; }

static void CreateTextureRes(Resource& r) {
	const FrameTexDesc& t = r.tex;
	const D3DPOOL pool = ReplayPool(t.pool);
	HRESULT hr = E_FAIL;
	if (r.record == kRecTexture) { IDirect3DTexture9* x = nullptr; hr = D->CreateTexture(t.width, t.height, t.levels, t.usage, (D3DFORMAT)t.format, pool, &x, nullptr); r.texture = x; }
	else if (r.record == kRecCube) { IDirect3DCubeTexture9* x = nullptr; hr = D->CreateCubeTexture(t.width, t.levels, t.usage, (D3DFORMAT)t.format, pool, &x, nullptr); r.texture = x; }
	else { IDirect3DVolumeTexture9* x = nullptr; hr = D->CreateVolumeTexture(t.width, t.height, t.depth, t.levels, t.usage, (D3DFORMAT)t.format, pool, &x, nullptr); r.texture = x; }
	if (FAILED(hr)) { r.missing = true; Warn("texture %u (%ux%u format %u usage %x pool %u) not created: %08lX", t.id, t.width, t.height, t.format, t.usage, t.pool, (unsigned long)hr); }
}

// A system-memory twin of a default-pool texture (staging for UpdateTexture).
static IDirect3DBaseTexture9* SysTwin(const Resource& r) {
	const FrameTexDesc& t = r.tex;
	const DWORD usage = 0;
	if (r.record == kRecTexture) { IDirect3DTexture9* x = nullptr; D->CreateTexture(t.width, t.height, t.levels, usage, (D3DFORMAT)t.format, D3DPOOL_SYSTEMMEM, &x, nullptr); return x; }
	if (r.record == kRecCube) { IDirect3DCubeTexture9* x = nullptr; D->CreateCubeTexture(t.width, t.levels, usage, (D3DFORMAT)t.format, D3DPOOL_SYSTEMMEM, &x, nullptr); return x; }
	IDirect3DVolumeTexture9* x = nullptr; D->CreateVolumeTexture(t.width, t.height, t.depth, t.levels, usage, (D3DFORMAT)t.format, D3DPOOL_SYSTEMMEM, &x, nullptr); return x;
}

static bool CopyIntoTexture(IDirect3DBaseTexture9* tex, DWORD record, const FrameDataHeader& h, const unsigned char* rows, DWORD lockFlags) {
	if (record == kRecVolume) {
		D3DLOCKED_BOX b = {};
		if (FAILED(((IDirect3DVolumeTexture9*)tex)->LockBox(h.level, &b, nullptr, lockFlags))) return false;
		for (DWORD z = 0; z < h.depth; z++) for (DWORD y = 0; y < h.rows; y++)
			memcpy((unsigned char*)b.pBits + (size_t)z * b.SlicePitch + (size_t)y * b.RowPitch, rows + ((size_t)z * h.rows + y) * h.rowBytes, h.rowBytes);
		((IDirect3DVolumeTexture9*)tex)->UnlockBox(h.level);
		return true;
	}
	D3DLOCKED_RECT l = {};
	const HRESULT hr = record == kRecCube ? ((IDirect3DCubeTexture9*)tex)->LockRect((D3DCUBEMAP_FACES)h.face, h.level, &l, nullptr, lockFlags)
		: ((IDirect3DTexture9*)tex)->LockRect(h.level, &l, nullptr, lockFlags);
	if (FAILED(hr)) return false;
	for (DWORD y = 0; y < h.rows; y++) memcpy((unsigned char*)l.pBits + (size_t)y * l.Pitch, rows + (size_t)y * h.rowBytes, h.rowBytes);
	if (record == kRecCube) ((IDirect3DCubeTexture9*)tex)->UnlockRect((D3DCUBEMAP_FACES)h.face, h.level); else ((IDirect3DTexture9*)tex)->UnlockRect(h.level);
	return true;
}

static IDirect3DSurface9* SurfaceOf(DWORD id) {
	if (!id || id >= Res.size()) return nullptr;
	return Res[id].surface;
}
static IDirect3DResource9* ObjectOf(DWORD id) {
	if (!id || id >= Res.size()) return nullptr;
	Resource& r = Res[id];
	if (r.texture) return r.texture;
	return r.surface;
}

// ---------------------------------------------------------------- loading
struct Reader {
	FILE* f = nullptr;
	explicit Reader(const char* path) { if (fopen_s(&f, path, "rb")) f = nullptr; }
	~Reader() { if (f) fclose(f); }
	bool Read(void* p, size_t n) { return fread(p, 1, n, f) == n; }
	void Skip(DWORD n) { _fseeki64(f, n, SEEK_CUR); }
};

static void Load(const char* path) {
	// Pass 1: which resources are written during the frame.
	std::set<DWORD> updatedIds;
	{
		Reader in(path);
		if (!in.f) Fail("cannot open %s", path);
		DWORD header[6];
		if (!in.Read(header, sizeof(header)) || header[0] != kMagic) Fail("%s is not a frame capture", path);
		if (header[1] != kVersion || header[2] != kStateWords) Fail("capture version %u / state size %u, this replayer reads %u / %u", header[1], header[2], kVersion, kStateWords);
		DWORD h[2];
		while (in.Read(h, 8)) {
			if (h[0] == kRecEnd) break;
			if (h[0] == kRecUpdate) { FrameUpdate u; in.Read(&u, sizeof(u)); updatedIds.insert(u.id); in.Skip(h[1] - sizeof(u)); }
			else in.Skip(h[1]);
		}
	}
	Reader in(path);
	DWORD header[6];
	in.Read(header, sizeof(header));
	Width = header[3]; Height = header[4]; BBFormat = header[5];

	FrameState cur = {};
	bool stateDirty = true;
	int markerDepth = 0;
	std::vector<std::string> markerStack;
	std::string split;
	std::map<std::string, unsigned short> rangeOf;
	auto rangeId = [&](const std::string& name) -> unsigned short {
		auto f = rangeOf.find(name);
		if (f != rangeOf.end()) return f->second;
		Range r; r.name = name; Ranges.push_back(r);
		return rangeOf[name] = (unsigned short)(Ranges.size() - 1);
	};
	rangeId("FRAME (all actions)");
	// Staging for default-pool textures: filled level by level, uploaded when the next record is not more of the same.
	DWORD stagingId = 0; IDirect3DBaseTexture9* staging = nullptr;
	auto flushStaging = [&]() {
		if (!staging) return;
		Resource& r = Res[stagingId];
		if (r.texture && FAILED(D->UpdateTexture(staging, r.texture))) Warn("texture %u: UpdateTexture failed", stagingId);
		if (r.updated) { r.restoreTexture = staging; }
		else staging->Release();
		staging = nullptr; stagingId = 0;
	};
	int actionIndex = 0, drawIndex = 0;
	auto memberships = [&](Op& op) {
		op.ranges.push_back(0);
		for (auto& m : markerStack) op.ranges.push_back(rangeId("marker: " + m));
		if (!split.empty() && split != "(none)") op.ranges.push_back(rangeId("split: " + split));
		std::sort(op.ranges.begin(), op.ranges.end());
		op.ranges.erase(std::unique(op.ranges.begin(), op.ranges.end()), op.ranges.end());
		op.marker = markerStack.empty() ? "" : markerStack.back();
		op.split = split;
	};

	DWORD h[2];
	std::vector<unsigned char> buf;
	while (in.Read(h, 8)) {
		const DWORD type = h[0], bytes = h[1];
		if (type == kRecEnd) break;
		buf.resize(bytes);
		if (bytes && !in.Read(buf.data(), bytes)) Fail("capture truncated");
		const unsigned char* p = buf.data();
		if (type != kRecData || (bytes >= 4 && *(const DWORD*)p != stagingId)) flushStaging();
		switch (type) {
		case kRecTexture: case kRecCube: case kRecVolume: {
			const FrameTexDesc& t = *(const FrameTexDesc*)p;
			Resource& r = R(t.id); r.record = type; r.tex = t; r.updated = updatedIds.count(t.id) != 0;
			CreateTextureRes(r);
			break;
		}
		case kRecVertexBuffer: case kRecIndexBuffer: {
			const FrameBufferDesc& b = *(const FrameBufferDesc*)p;
			Resource& r = R(b.id); r.record = type; r.buf = b; r.updated = updatedIds.count(b.id) != 0;
			const D3DPOOL pool = ReplayPool(b.pool);
			HRESULT hr;
			if (type == kRecVertexBuffer) hr = D->CreateVertexBuffer(b.length, b.usage, b.format, pool, &r.vb, nullptr);
			else hr = D->CreateIndexBuffer(b.length, b.usage, (D3DFORMAT)b.format, pool, &r.ib, nullptr);
			if (FAILED(hr)) { r.missing = true; Warn("buffer %u (%u bytes) not created: %08lX", b.id, b.length, (unsigned long)hr); }
			break;
		}
		case kRecSurface: {
			const FrameSurfaceDesc& s = *(const FrameSurfaceDesc*)p;
			Resource& r = R(s.id); r.record = type; r.surf = s;
			HRESULT hr = S_OK;
			if (s.kind == kSurfBackBuffer) {
				if (s.width != Width || s.height != Height) Warn("a back buffer of another size (%ux%u)", s.width, s.height);
				r.surface = BackBuffer; BackBuffer->AddRef();
			}
			else if (s.kind == kSurfDepth) hr = D->CreateDepthStencilSurface(s.width, s.height, (D3DFORMAT)s.format, (D3DMULTISAMPLE_TYPE)s.multisample, s.quality, FALSE, &r.surface, nullptr);
			else if (s.kind == kSurfRenderTarget) hr = D->CreateRenderTarget(s.width, s.height, (D3DFORMAT)s.format, (D3DMULTISAMPLE_TYPE)s.multisample, s.quality, FALSE, &r.surface, nullptr);
			else hr = D->CreateOffscreenPlainSurface(s.width, s.height, (D3DFORMAT)s.format, s.pool == D3DPOOL_DEFAULT ? D3DPOOL_DEFAULT : D3DPOOL_SYSTEMMEM, &r.surface, nullptr);
			if (FAILED(hr)) { r.missing = true; Warn("surface %u (kind %u, %ux%u format %u) not created: %08lX", s.id, s.kind, s.width, s.height, s.format, (unsigned long)hr); }
			break;
		}
		case kRecSubSurface: {
			const FrameSubSurface& s = *(const FrameSubSurface*)p;
			Resource& r = R(s.id); r.record = type; r.sub = s;
			Resource& parent = R(s.parent);
			if (parent.texture) {
				if (parent.record == kRecCube) ((IDirect3DCubeTexture9*)parent.texture)->GetCubeMapSurface((D3DCUBEMAP_FACES)s.face, s.level, &r.surface);
				else ((IDirect3DTexture9*)parent.texture)->GetSurfaceLevel(s.level, &r.surface);
			}
			if (!r.surface) r.missing = true;
			break;
		}
		case kRecShader: {
			const DWORD id = ((const DWORD*)p)[0], pixel = ((const DWORD*)p)[1];
			Resource& r = R(id); r.record = type; r.isPixel = pixel != 0;
			r.code.assign(p + 8, p + bytes);
			r.hash = Hash(r.code.data(), r.code.size());
			HRESULT hr = pixel ? D->CreatePixelShader((const DWORD*)r.code.data(), &r.ps) : D->CreateVertexShader((const DWORD*)r.code.data(), &r.vs);
			if (FAILED(hr)) { r.missing = true; Warn("shader %u not created", id); }
			break;
		}
		case kRecDecl: {
			const DWORD id = *(const DWORD*)p;
			Resource& r = R(id); r.record = type;
			if (FAILED(D->CreateVertexDeclaration((const D3DVERTEXELEMENT9*)(p + 4), &r.decl))) { r.missing = true; Warn("declaration %u not created", id); }
			break;
		}
		case kRecName: {
			const DWORD id = *(const DWORD*)p;
			std::string name((const char*)p + 4);
			auto& names = R(id).names;
			if (std::find(names.begin(), names.end(), name) == names.end()) names.push_back(name);
			break;
		}
		case kRecData: {
			const FrameDataHeader& d = *(const FrameDataHeader*)p;
			const unsigned char* rows = p + sizeof(d);
			Resource& r = R(d.id);
			if (r.missing) break;
			if (r.record == kRecVertexBuffer || r.record == kRecIndexBuffer) {
				void* dst = nullptr;
				const DWORD flags = (r.buf.usage & D3DUSAGE_DYNAMIC) ? D3DLOCK_DISCARD : 0;
				const HRESULT hr = r.vb ? r.vb->Lock(0, 0, &dst, flags) : r.ib->Lock(0, 0, &dst, flags);
				if (SUCCEEDED(hr)) { memcpy(dst, rows, std::min<size_t>(d.rowBytes, r.buf.length)); if (r.vb) r.vb->Unlock(); else r.ib->Unlock(); }
				else Warn("buffer %u not filled", d.id);
				if (r.updated) r.initial.assign(rows, rows + d.rowBytes);
			}
			else if (r.record == kRecTexture || r.record == kRecCube || r.record == kRecVolume) {
				const bool rt = (r.tex.usage & D3DUSAGE_RENDERTARGET) != 0;
				if (d.flags & kDataDepth) {	// a depth texture read before written: its values as R32F, written back with oDepth before each replay
					IDirect3DTexture9* values = nullptr; IDirect3DTexture9* sys = nullptr;
					if (r.record == kRecTexture && SUCCEEDED(D->CreateTexture(d.width, d.height, 1, 0, D3DFMT_R32F, D3DPOOL_DEFAULT, &values, nullptr)) &&
						SUCCEEDED(D->CreateTexture(d.width, d.height, 1, 0, D3DFMT_R32F, D3DPOOL_SYSTEMMEM, &sys, nullptr))) {
						FrameDataHeader h0 = d; h0.level = 0;
						CopyIntoTexture(sys, kRecTexture, h0, rows, 0);
						D->UpdateTexture(sys, values);
						sys->Release();
						DepthRestore dr; dr.values = values; dr.width = d.width; dr.height = d.height;
						((IDirect3DTexture9*)r.texture)->GetSurfaceLevel(0, &dr.dst);
						DepthRestores.push_back(dr);
					}
					else { if (values) values->Release(); if (sys) sys->Release(); Warn("depth texture %u: values not restored", d.id); }
				}
				else if (rt) {	// read back from the GPU in the game: restored before every replay
					IDirect3DSurface9* dst = nullptr;
					if (r.record == kRecCube) ((IDirect3DCubeTexture9*)r.texture)->GetCubeMapSurface((D3DCUBEMAP_FACES)d.face, d.level, &dst);
					else if (r.record == kRecTexture) ((IDirect3DTexture9*)r.texture)->GetSurfaceLevel(d.level, &dst);
					IDirect3DSurface9* sys = nullptr;
					if (dst && SUCCEEDED(D->CreateOffscreenPlainSurface(d.width, d.height, (D3DFORMAT)r.tex.format, D3DPOOL_SYSTEMMEM, &sys, nullptr))) {
						D3DLOCKED_RECT l = {};
						if (SUCCEEDED(sys->LockRect(&l, nullptr, 0))) { for (DWORD y = 0; y < d.rows; y++) memcpy((unsigned char*)l.pBits + (size_t)y * l.Pitch, rows + (size_t)y * d.rowBytes, d.rowBytes); sys->UnlockRect(); }
						Restores.push_back({ sys, dst });
					}
					else { if (dst) dst->Release(); Warn("render target texture %u: no restore surface", d.id); }
				}
				else if (ReplayPool(r.tex.pool) == D3DPOOL_DEFAULT && !(r.tex.usage & D3DUSAGE_DYNAMIC)) {
					if (!staging) { staging = SysTwin(r); stagingId = d.id; if (!staging) { Warn("texture %u: no staging", d.id); break; } }
					if (!CopyIntoTexture(staging, r.record, d, rows, 0)) Warn("texture %u level %u: staging lock failed", d.id, d.level);
				}
				else if (!CopyIntoTexture(r.texture, r.record, d, rows, 0)) Warn("texture %u level %u not filled", d.id, d.level);
			}
			else if (r.record == kRecSurface && r.surface) {
				if (r.surf.kind == kSurfRenderTarget || r.surf.kind == kSurfBackBuffer) {
					IDirect3DSurface9* sys = nullptr;
					if (SUCCEEDED(D->CreateOffscreenPlainSurface(d.width, d.height, (D3DFORMAT)r.surf.format, D3DPOOL_SYSTEMMEM, &sys, nullptr))) {
						D3DLOCKED_RECT l = {};
						if (SUCCEEDED(sys->LockRect(&l, nullptr, 0))) { for (DWORD y = 0; y < d.rows; y++) memcpy((unsigned char*)l.pBits + (size_t)y * l.Pitch, rows + (size_t)y * d.rowBytes, d.rowBytes); sys->UnlockRect(); }
						r.surface->AddRef();
						Restores.push_back({ sys, r.surface });
					}
				}
				else {
					D3DLOCKED_RECT l = {};
					if (SUCCEEDED(r.surface->LockRect(&l, nullptr, 0))) { for (DWORD y = 0; y < d.rows; y++) memcpy((unsigned char*)l.pBits + (size_t)y * l.Pitch, rows + (size_t)y * d.rowBytes, d.rowBytes); r.surface->UnlockRect(); }
				}
			}
			break;
		}
		case kRecState: {
			const DWORD runs = *(const DWORD*)p;
			const DWORD* w = (const DWORD*)p + 1;
			DWORD* s = (DWORD*)&cur;
			for (DWORD i = 0; i < runs; i++) {
				const DWORD offset = *w & 0xFFFF, count = *w >> 16;
				w++;
				memcpy(s + offset, w, count * 4);
				w += count;
			}
			stateDirty = true;
			break;
		}
		case kRecAction: case kRecUpdate: {
			Op op;
			if (type == kRecAction) {
				op.act = *(const FrameAction*)p;
				op.data.assign(p + sizeof(FrameAction), p + bytes);
			}
			else {
				op.kind = kOpUpdate;
				op.upd = *(const FrameUpdate*)p;
				op.data.assign(p + sizeof(FrameUpdate), p + bytes);
				Resource& r = R(op.upd.id);
				if ((r.record == kRecTexture || r.record == kRecCube) && ReplayPool(r.tex.pool) == D3DPOOL_DEFAULT && !(r.tex.usage & D3DUSAGE_DYNAMIC)) {
					const DWORD w = op.upd.right - op.upd.left, hgt = op.upd.bottom - op.upd.top;
					if (SUCCEEDED(D->CreateOffscreenPlainSurface(w, hgt, (D3DFORMAT)r.tex.format, D3DPOOL_SYSTEMMEM, &op.staging, nullptr))) {
						D3DLOCKED_RECT l = {};
						if (SUCCEEDED(op.staging->LockRect(&l, nullptr, 0))) { for (DWORD y = 0; y < op.upd.rows; y++) memcpy((unsigned char*)l.pBits + (size_t)y * l.Pitch, op.data.data() + (size_t)y * op.upd.rowBytes, op.upd.rowBytes); op.staging->UnlockRect(); }
					}
				}
			}
			if (stateDirty) { States.push_back(cur); stateDirty = false; }
			op.state = (int)States.size() - 1;
			op.index = actionIndex++;
			if (op.IsDraw()) op.draw = drawIndex++;
			memberships(op);
			Ops.push_back(std::move(op));
			break;
		}
		case kRecMark: {
			const DWORD kind = *(const DWORD*)p;
			std::string name((const char*)p + 4);
			if (kind == kMarkBegin) markerStack.push_back(name);
			else if (kind == kMarkEnd) {
				for (size_t i = markerStack.size(); i-- > 0;) if (markerStack[i] == name) { markerStack.erase(markerStack.begin() + i); break; }
			}
			else split = name;
			(void)markerDepth;
			break;
		}
		case kRecRefImage: {
			RefW = ((const DWORD*)p)[0]; RefH = ((const DWORD*)p)[1];
			RefImage.assign((const unsigned*)(p + 8), (const unsigned*)(p + 8) + (size_t)RefW * RefH);
			break;
		}
		case kRecInfo: Info = (const char*)p; break;
		default: Warn("unknown record %u", type); break;
		}
	}
	flushStaging();
	if (States.empty()) States.push_back(cur);
	// The F10 split ranges are only meaningful as the game reports them: drop marker ranges nobody would read? Keep all.
}

// ---------------------------------------------------------------- state
static FrameState Shadow;
// D3D9 draws only between BeginScene and EndScene: the replayer's own draws (restores, copies) open a scene when none is open.
static bool InScene = false;
struct SceneGuard {
	bool began = false;
	SceneGuard() { if (!InScene && SUCCEEDED(D->BeginScene())) { began = true; InScene = true; } }
	~SceneGuard() { if (began) { D->EndScene(); InScene = false; } }
};
static bool ShadowValid = false;

static void SetConstantsF(bool pixel, const float (*want)[4], float (*have)[4], unsigned count, bool all) {
	for (unsigned i = 0; i < count;) {
		if (!all && !memcmp(want[i], have[i], 16)) { i++; continue; }
		unsigned j = i + 1;
		while (j < count && (all || memcmp(want[j], have[j], 16))) j++;
		if (pixel) D->SetPixelShaderConstantF(i, want[i], j - i); else D->SetVertexShaderConstantF(i, want[i], j - i);
		memcpy(have[i], want[i], (j - i) * 16);
		i = j;
	}
}

static void ApplyState(const FrameState& s) {
	FrameState& c = Shadow;
	const bool all = !ShadowValid;
	bool rtChanged = all;
	for (unsigned i = 0; i < 4; i++) {
		if (all || s.rt[i] != c.rt[i]) {
			if (i == 0 && !s.rt[0]) continue;
			D->SetRenderTarget(i, SurfaceOf(s.rt[i]));
			c.rt[i] = s.rt[i];
			rtChanged = true;
		}
	}
	if (all || s.ds != c.ds) { D->SetDepthStencilSurface(SurfaceOf(s.ds)); c.ds = s.ds; }
	if (rtChanged || memcmp(&s.viewport, &c.viewport, sizeof(D3DVIEWPORT9))) { D->SetViewport(&s.viewport); c.viewport = s.viewport; }
	if (rtChanged || memcmp(&s.scissor, &c.scissor, sizeof(RECT))) { D->SetScissorRect(&s.scissor); c.scissor = s.scissor; }
	for (unsigned char rs : RenderStates) if (all || s.rs[rs] != c.rs[rs]) { D->SetRenderState((D3DRENDERSTATETYPE)rs, s.rs[rs]); c.rs[rs] = s.rs[rs]; }
	for (unsigned i = 0; i < kSamplers; i++) {
		for (unsigned st = 1; st < kSamplerStates; st++)
			if (all || s.ss[i][st] != c.ss[i][st]) { D->SetSamplerState(SamplerOf(i), (D3DSAMPLERSTATETYPE)st, s.ss[i][st]); c.ss[i][st] = s.ss[i][st]; }
		if (all || s.tex[i] != c.tex[i]) {
			IDirect3DBaseTexture9* t = s.tex[i] && s.tex[i] < Res.size() ? Res[s.tex[i]].texture : nullptr;
			D->SetTexture(SamplerOf(i), t);
			c.tex[i] = s.tex[i];
		}
	}
	for (unsigned i = 0; i < kStages; i++) for (unsigned char st : StageStates)
		if (all || s.tss[i][st] != c.tss[i][st]) { D->SetTextureStageState(i, (D3DTEXTURESTAGESTATETYPE)st, s.tss[i][st]); c.tss[i][st] = s.tss[i][st]; }
	if (all || s.decl != c.decl || s.fvf != c.fvf) {
		if (s.decl && s.decl < Res.size() && Res[s.decl].decl) D->SetVertexDeclaration(Res[s.decl].decl);
		else D->SetFVF(s.fvf);
		c.decl = s.decl; c.fvf = s.fvf;
	}
	if (all || s.vs != c.vs) { D->SetVertexShader(s.vs < Res.size() ? Res[s.vs].vs : nullptr); c.vs = s.vs; }
	if (all || s.ps != c.ps) { D->SetPixelShader(s.ps < Res.size() ? Res[s.ps].ps : nullptr); c.ps = s.ps; }
	if (all || s.ib != c.ib) { D->SetIndices(s.ib < Res.size() ? Res[s.ib].ib : nullptr); c.ib = s.ib; }
	for (unsigned i = 0; i < 16; i++) {
		if (all || memcmp(s.stream[i], c.stream[i], 12)) {
			D->SetStreamSource(i, s.stream[i][0] < Res.size() ? Res[s.stream[i][0]].vb : nullptr, s.stream[i][1], s.stream[i][2]);
			memcpy(c.stream[i], s.stream[i], 12);
		}
		if (all || s.stream[i][3] != c.stream[i][3]) { D->SetStreamSourceFreq(i, s.stream[i][3] ? s.stream[i][3] : 1); c.stream[i][3] = s.stream[i][3]; }
	}
	for (unsigned i = 0; i < 6; i++) if (all || memcmp(s.clip[i], c.clip[i], 16)) { D->SetClipPlane(i, s.clip[i]); memcpy(c.clip[i], s.clip[i], 16); }
	SetConstantsF(false, s.vsf, c.vsf, kVSFloat, all);
	SetConstantsF(true, s.psf, c.psf, kPSFloat, all);
	if (all || memcmp(s.vsi, c.vsi, sizeof(s.vsi))) { D->SetVertexShaderConstantI(0, &s.vsi[0][0], 16); memcpy(c.vsi, s.vsi, sizeof(s.vsi)); }
	if (all || memcmp(s.vsb, c.vsb, sizeof(s.vsb))) { D->SetVertexShaderConstantB(0, s.vsb, 16); memcpy(c.vsb, s.vsb, sizeof(s.vsb)); }
	if (all || memcmp(s.psi, c.psi, sizeof(s.psi))) { D->SetPixelShaderConstantI(0, &s.psi[0][0], 16); memcpy(c.psi, s.psi, sizeof(s.psi)); }
	if (all || memcmp(s.psb, c.psb, sizeof(s.psb))) { D->SetPixelShaderConstantB(0, s.psb, 16); memcpy(c.psb, s.psb, sizeof(s.psb)); }
	if (!s.vs) for (unsigned i = 0; i < kTransforms; i++) if (all || memcmp(&s.xf[i], &c.xf[i], sizeof(D3DMATRIX))) { D->SetTransform(TransformOf(i), &s.xf[i]); c.xf[i] = s.xf[i]; }
	ShadowValid = true;
}

// ---------------------------------------------------------------- execution
static void RestoreDepth() {
	if (DepthRestores.empty()) return;
	static IDirect3DVertexShader9* vs = nullptr; static IDirect3DPixelShader9* ps = nullptr;
	static std::map<std::pair<UINT, UINT>, IDirect3DSurface9*> dummies;
	if (!vs) {
		auto v = CompileText("void main(float4 p : POSITION, float2 t : TEXCOORD0, out float4 op : POSITION, out float2 ot : TEXCOORD0) { op = p; ot = t; }", "vs_3_0");
		auto f = CompileText("sampler2D s0 : register(s0); float4 main(float2 t : TEXCOORD0, out float depth : DEPTH) : COLOR0 { depth = tex2D(s0, t).r; return 0; }", "ps_3_0");
		D->CreateVertexShader((const DWORD*)v.data(), &vs); D->CreatePixelShader((const DWORD*)f.data(), &ps);
	}
	SceneGuard scene;
	for (auto& dr : DepthRestores) {
		IDirect3DSurface9*& dummy = dummies[std::make_pair(dr.width, dr.height)];
		if (!dummy && FAILED(D->CreateRenderTarget(dr.width, dr.height, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &dummy, nullptr))) { Warn("depth restore: no target"); continue; }
		struct V { float x, y, z, w, u, v; };
		const float du = 0.5f / dr.width, dv = 0.5f / dr.height;
		V quad[4] = { { -1, 1, 0, 1, du, dv }, { 1, 1, 0, 1, 1 + du, dv }, { -1, -1, 0, 1, du, 1 + dv }, { 1, -1, 0, 1, 1 + du, 1 + dv } };
		D->SetRenderTarget(0, dummy); for (int i = 1; i < 4; i++) D->SetRenderTarget(i, nullptr);
		D->SetDepthStencilSurface(dr.dst);
		D->SetVertexShader(vs); D->SetPixelShader(ps); D->SetFVF(D3DFVF_XYZW | D3DFVF_TEX1);
		D->SetTexture(0, dr.values);
		D->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT); D->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT); D->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
		D->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);
		D->SetRenderState(D3DRS_ZENABLE, TRUE); D->SetRenderState(D3DRS_ZWRITEENABLE, TRUE); D->SetRenderState(D3DRS_ZFUNC, D3DCMP_ALWAYS);
		D->SetRenderState(D3DRS_STENCILENABLE, FALSE); D->SetRenderState(D3DRS_COLORWRITEENABLE, 0); D->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
		D->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE); D->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE); D->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
		D->SetRenderState(D3DRS_CLIPPLANEENABLE, 0); D->SetRenderState(D3DRS_DEPTHBIAS, 0); D->SetRenderState(D3DRS_SLOPESCALEDEPTHBIAS, 0);
		D->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(V));
	}
	ShadowValid = false;
}

// The render targets' start contents are kept on the GPU, so restoring them before each replay is a quick GPU copy instead of
// an upload of hundreds of MB from system memory (which made the replayer CPU-bound).
static void RestoreByDraw();
static void RestoresToGpu() {
	unsigned moved = 0, drawn = 0;
	for (auto& r : Restores) {
		D3DSURFACE_DESC d = {};
		if (!r.sys || !r.dst || FAILED(r.dst->GetDesc(&d))) continue;
		if (!RestoreByCopy) {
			IDirect3DTexture9* sysTex = nullptr; IDirect3DTexture9* tex = nullptr;
			D3DLOCKED_RECT a = {}, b = {};
			const unsigned bpp = PixelBytes(d.Format);
			if (bpp && SUCCEEDED(D->CreateTexture(d.Width, d.Height, 1, 0, d.Format, D3DPOOL_SYSTEMMEM, &sysTex, nullptr)) &&
				SUCCEEDED(D->CreateTexture(d.Width, d.Height, 1, 0, d.Format, D3DPOOL_DEFAULT, &tex, nullptr)) &&
				SUCCEEDED(r.sys->LockRect(&a, nullptr, D3DLOCK_READONLY))) {
				if (SUCCEEDED(sysTex->LockRect(0, &b, nullptr, 0))) {
					for (UINT y = 0; y < d.Height; y++) memcpy((unsigned char*)b.pBits + (size_t)y * b.Pitch, (unsigned char*)a.pBits + (size_t)y * a.Pitch, (size_t)d.Width * bpp);
					sysTex->UnlockRect(0);
				}
				r.sys->UnlockRect();
				if (SUCCEEDED(D->UpdateTexture(sysTex, tex))) { r.tex = tex; tex = nullptr; drawn++; }
			}
			if (sysTex) sysTex->Release();
			if (tex) tex->Release();
			if (r.tex) continue;
		}
		if (d.MultiSampleType != D3DMULTISAMPLE_NONE) { Warn("multisampled render target: start contents not restored (needs the default restore by drawing)"); r.sys->Release(); r.sys = nullptr; continue; }
		IDirect3DSurface9* copy = nullptr;
		if (FAILED(D->CreateRenderTarget(d.Width, d.Height, d.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &copy, nullptr))) continue;
		if (FAILED(D->UpdateSurface(r.sys, nullptr, copy, nullptr))) { copy->Release(); continue; }
		r.copy = copy; r.sys->Release(); r.sys = nullptr; moved++;
	}
	printf("render target start contents: %u of %u restored by drawing, %u by StretchRect, the rest by upload\n", drawn, (unsigned)Restores.size(), moved);
	// Check: each drawn restore must give back the bytes the game had (non-multisampled targets).
	RestoreByDraw();
	unsigned bad = 0;
	for (auto& r : Restores) {
		if (!r.tex || !r.sys) continue;
		D3DSURFACE_DESC d = {}; r.dst->GetDesc(&d);
		if (d.MultiSampleType == D3DMULTISAMPLE_NONE) {
			IDirect3DSurface9* back = nullptr;
			D3DLOCKED_RECT a = {}, b = {};
			const unsigned bpp = PixelBytes(d.Format);
			if (SUCCEEDED(D->CreateOffscreenPlainSurface(d.Width, d.Height, d.Format, D3DPOOL_SYSTEMMEM, &back, nullptr)) && SUCCEEDED(D->GetRenderTargetData(r.dst, back)) &&
				SUCCEEDED(back->LockRect(&a, nullptr, D3DLOCK_READONLY))) {
				if (SUCCEEDED(r.sys->LockRect(&b, nullptr, D3DLOCK_READONLY))) {
					unsigned long long diff = 0; long firstAt = -1;
					for (UINT y = 0; y < d.Height; y++) {
						const unsigned char* pa = (unsigned char*)a.pBits + (size_t)y * a.Pitch; const unsigned char* pb = (unsigned char*)b.pBits + (size_t)y * b.Pitch;
						if (memcmp(pa, pb, (size_t)d.Width * bpp)) { if (!diff) for (size_t i = 0; i < (size_t)d.Width * bpp; i++) if (pa[i] != pb[i]) { firstAt = (long)(y * 100000 + i); break; } diff++; }
					}
					if (diff) {
						bad++;
						const size_t row = firstAt / 100000, byte = (firstAt % 100000) / bpp * bpp;
						const unsigned char* pa = (unsigned char*)a.pBits + row * a.Pitch + byte; const unsigned char* pb = (unsigned char*)b.pBits + row * b.Pitch + byte;
						char ha[64] = "", hb[64] = "";
						for (unsigned i = 0; i < bpp && i < 16; i++) { sprintf_s(ha + i * 3, 4, "%02x ", pa[i]); sprintf_s(hb + i * 3, 4, "%02x ", pb[i]); }
						Warn("restore by drawing differs: %ux%u format %u, %llu rows differ; first at row %u pixel %u: drawn %s game %s", d.Width, d.Height, (unsigned)d.Format, diff, (unsigned)row, (unsigned)(byte / bpp), ha, hb);
					}
					r.sys->UnlockRect();
				}
				back->UnlockRect();
			}
			if (back) back->Release();
		}
		r.sys->Release(); r.sys = nullptr;
	}
	printf("restore check: %u drawn restores differ from the game's bytes\n", bad);
}

static void RestoreByDraw() {
	static IDirect3DVertexShader9* vs = nullptr; static IDirect3DPixelShader9* ps = nullptr;
	if (!vs) {
		auto v = CompileText("void main(float4 p : POSITION, float2 t : TEXCOORD0, out float4 op : POSITION, out float2 ot : TEXCOORD0) { op = p; ot = t; }", "vs_3_0");
		auto f = CompileText("sampler2D s0 : register(s0); float4 main(float2 t : TEXCOORD0) : COLOR0 { return tex2D(s0, t); }", "ps_3_0");
		D->CreateVertexShader((const DWORD*)v.data(), &vs); D->CreatePixelShader((const DWORD*)f.data(), &ps);
	}
	bool first = true;
	SceneGuard scene;
	for (auto& r : Restores) {
		if (!r.tex) continue;
		if (first) {
			first = false;
			for (int i = 1; i < 4; i++) D->SetRenderTarget(i, nullptr);
			D->SetDepthStencilSurface(nullptr);
			D->SetVertexShader(vs); D->SetPixelShader(ps); D->SetFVF(D3DFVF_XYZW | D3DFVF_TEX1);
			D->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT); D->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT); D->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
			D->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE); D->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP); D->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
			const D3DRENDERSTATETYPE off[] = { D3DRS_ZENABLE, D3DRS_ALPHABLENDENABLE, D3DRS_ALPHATESTENABLE, D3DRS_SCISSORTESTENABLE, D3DRS_STENCILENABLE, D3DRS_SRGBWRITEENABLE, D3DRS_CLIPPLANEENABLE, D3DRS_FOGENABLE };
			for (auto rs : off) D->SetRenderState(rs, FALSE);
			D->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE); D->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF); D->SetRenderState(D3DRS_MULTISAMPLEMASK, 0xFFFFFFFF);
		}
		D3DSURFACE_DESC d = {}; r.dst->GetDesc(&d);
		struct V { float x, y, z, w, u, v; };
		const float du = 0.5f / d.Width, dv = 0.5f / d.Height;
		V quad[4] = { { -1, 1, 0, 1, du, dv }, { 1, 1, 0, 1, 1 + du, dv }, { -1, -1, 0, 1, du, 1 + dv }, { 1, -1, 0, 1, 1 + du, 1 + dv } };
		D->SetRenderTarget(0, r.dst);
		D->SetTexture(0, r.tex);
		D->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(V));
	}
	D->SetTexture(0, nullptr);
	ShadowValid = false;
}

static bool RestoreOnce = false, Restored = false;	// --restore-once: timed replays keep what the previous replay left (like the game's next frame)
static void RestoreStart() {
	if (RestoreOnce && Restored) { ShadowValid = false; return; }
	Restored = true;
	RestoreByDraw();
	for (auto& r : Restores) {
		if (r.tex || (!r.copy && !r.sys)) continue;
		if (r.copy) { if (FAILED(D->StretchRect(r.copy, nullptr, r.dst, nullptr, D3DTEXF_NONE))) Warn("restore StretchRect failed"); }
		else if (FAILED(D->UpdateSurface(r.sys, nullptr, r.dst, nullptr))) Warn("restore UpdateSurface failed");
	}
	RestoreDepth();
	for (size_t id = 1; id < Res.size(); id++) {
		Resource& r = Res[id];
		if (!r.updated || r.missing) continue;
		if (!r.initial.empty() && (r.vb || r.ib)) {
			void* p = nullptr;
			const DWORD flags = (r.buf.usage & D3DUSAGE_DYNAMIC) ? D3DLOCK_DISCARD : 0;
			if (SUCCEEDED(r.vb ? r.vb->Lock(0, 0, &p, flags) : r.ib->Lock(0, 0, &p, flags))) { memcpy(p, r.initial.data(), r.initial.size()); if (r.vb) r.vb->Unlock(); else r.ib->Unlock(); }
		}
		if (r.restoreTexture && r.texture) D->UpdateTexture(r.restoreTexture, r.texture);
	}
	ShadowValid = false;
}

static UINT IndexCount(D3DPRIMITIVETYPE t, UINT prims) {
	switch (t) {
	case D3DPT_POINTLIST: return prims; case D3DPT_LINELIST: return prims * 2; case D3DPT_LINESTRIP: return prims + 1;
	case D3DPT_TRIANGLELIST: return prims * 3; case D3DPT_TRIANGLESTRIP: case D3DPT_TRIANGLEFAN: return prims + 2;
	}
	return 0;
}

static void ExecuteUpdate(const Op& op) {
	const FrameUpdate& u = op.upd;
	if (u.id >= Res.size()) return;
	Resource& r = Res[u.id];
	if (r.missing) return;
	if (r.vb || r.ib) {
		void* p = nullptr;
		DWORD flags = u.flags & (D3DLOCK_DISCARD | D3DLOCK_NOOVERWRITE);
		if (!(r.buf.usage & D3DUSAGE_DYNAMIC)) flags = 0;
		if (SUCCEEDED(r.vb ? r.vb->Lock(u.offset, u.rowBytes, &p, flags) : r.ib->Lock(u.offset, u.rowBytes, &p, flags))) { memcpy(p, op.data.data(), u.rowBytes); if (r.vb) r.vb->Unlock(); else r.ib->Unlock(); }
		return;
	}
	if (r.texture) {
		if (op.staging) {
			IDirect3DSurface9* dst = nullptr;
			if (r.record == kRecCube) ((IDirect3DCubeTexture9*)r.texture)->GetCubeMapSurface((D3DCUBEMAP_FACES)u.face, u.level, &dst); else ((IDirect3DTexture9*)r.texture)->GetSurfaceLevel(u.level, &dst);
			POINT pt = { (LONG)u.left, (LONG)u.top };
			if (dst) { D->UpdateSurface(op.staging, nullptr, dst, &pt); dst->Release(); }
			return;
		}
		RECT rect = { (LONG)u.left, (LONG)u.top, (LONG)u.right, (LONG)u.bottom };
		D3DLOCKED_RECT l = {};
		HRESULT hr = r.record == kRecCube ? ((IDirect3DCubeTexture9*)r.texture)->LockRect((D3DCUBEMAP_FACES)u.face, u.level, &l, &rect, 0) : ((IDirect3DTexture9*)r.texture)->LockRect(u.level, &l, &rect, 0);
		if (SUCCEEDED(hr)) {
			for (DWORD y = 0; y < u.rows; y++) memcpy((unsigned char*)l.pBits + (size_t)y * l.Pitch, op.data.data() + (size_t)y * u.rowBytes, u.rowBytes);
			if (r.record == kRecCube) ((IDirect3DCubeTexture9*)r.texture)->UnlockRect((D3DCUBEMAP_FACES)u.face, u.level); else ((IDirect3DTexture9*)r.texture)->UnlockRect(u.level);
		}
		return;
	}
	if (r.surface) {
		RECT rect = { (LONG)u.left, (LONG)u.top, (LONG)u.right, (LONG)u.bottom };
		D3DLOCKED_RECT l = {};
		if (SUCCEEDED(r.surface->LockRect(&l, &rect, 0))) { for (DWORD y = 0; y < u.rows; y++) memcpy((unsigned char*)l.pBits + (size_t)y * l.Pitch, op.data.data() + (size_t)y * u.rowBytes, u.rowBytes); r.surface->UnlockRect(); }
	}
}

// Runs one action with the state it was recorded with (or the state given).
static void ExecuteAction(const Op& op, const FrameState* stateOverride = nullptr) {
	if (op.kind == kOpUpdate) { ExecuteUpdate(op); return; }
	const FrameAction& a = op.act;
	const unsigned char* data = op.data.data();
	switch (a.type) {
	case kActDraw: ApplyState(stateOverride ? *stateOverride : States[op.state]); D->DrawPrimitive((D3DPRIMITIVETYPE)a.a[0], a.a[1], a.a[2]); break;
	case kActDrawIndexed: ApplyState(stateOverride ? *stateOverride : States[op.state]); D->DrawIndexedPrimitive((D3DPRIMITIVETYPE)a.a[0], (INT)a.a[1], a.a[2], a.a[3], a.a[4], a.a[5]); break;
	case kActDrawUP:
		ApplyState(stateOverride ? *stateOverride : States[op.state]);
		D->DrawPrimitiveUP((D3DPRIMITIVETYPE)a.a[0], a.a[1], data, a.a[2]);
		Shadow.stream[0][0] = 0xFFFFFFFF; Shadow.ib = 0xFFFFFFFF;	// D3D unbinds stream 0 and the indices after a UP draw
		break;
	case kActDrawIndexedUP:
		ApplyState(stateOverride ? *stateOverride : States[op.state]);
		D->DrawIndexedPrimitiveUP((D3DPRIMITIVETYPE)a.a[0], a.a[1], a.a[2], a.a[3], data, (D3DFORMAT)a.a[4], data + a.a[6], a.a[5]);
		Shadow.stream[0][0] = 0xFFFFFFFF; Shadow.ib = 0xFFFFFFFF;
		break;
	case kActClear: {
		ApplyState(stateOverride ? *stateOverride : States[op.state]);
		float z; memcpy(&z, &a.a[3], 4);
		D->Clear(a.a[0], a.a[0] ? (const D3DRECT*)data : nullptr, a.a[1], a.a[2], z, a.a[4]);
		break;
	}
	case kActStretchRect: {
		const RECT* sr = (const RECT*)data; const RECT* dr = sr + 1;
		D->StretchRect(SurfaceOf(a.a[0]), a.a[3] ? sr : nullptr, SurfaceOf(a.a[1]), a.a[4] ? dr : nullptr, (D3DTEXTUREFILTERTYPE)a.a[2]);
		break;
	}
	case kActColorFill: D->ColorFill(SurfaceOf(a.a[0]), a.a[2] ? (const RECT*)data : nullptr, a.a[1]); break;
	case kActUpdateSurface: {
		const RECT* r = (const RECT*)data; const POINT* pt = (const POINT*)(data + sizeof(RECT));
		D->UpdateSurface(SurfaceOf(a.a[0]), a.a[2] ? r : nullptr, SurfaceOf(a.a[1]), a.a[3] ? pt : nullptr);
		break;
	}
	case kActUpdateTexture:
		if (a.a[0] < Res.size() && a.a[1] < Res.size() && Res[a.a[0]].texture && Res[a.a[1]].texture) D->UpdateTexture(Res[a.a[0]].texture, Res[a.a[1]].texture);
		break;
	case kActNvStretchRect: {
		IDirect3DResource9* s = ObjectOf(a.a[0]); IDirect3DResource9* t = ObjectOf(a.a[1]);
		if (NvOk && s && t) {
			if (!Res[a.a[0]].nvRegistered) { NvAPI_D3D9_RegisterResource(s); Res[a.a[0]].nvRegistered = true; }
			if (!Res[a.a[1]].nvRegistered) { NvAPI_D3D9_RegisterResource(t); Res[a.a[1]].nvRegistered = true; }
			NvAPI_D3D9_StretchRectEx(D, s, nullptr, t, nullptr, D3DTEXF_NONE);
		}
		break;
	}
	case kActBeginScene: D->BeginScene(); InScene = true; break;
	case kActEndScene: D->EndScene(); InScene = false; break;
	case kActPresent: break;	// the caller presents
	}
}

// ---------------------------------------------------------------- read back
static bool ReadSurface(IDirect3DSurface9* s, std::vector<float>& rgba, DWORD& w, DWORD& h, D3DFORMAT& format) {
	D3DSURFACE_DESC d = {};
	if (!s || FAILED(s->GetDesc(&d))) return false;
	IDirect3DSurface9* sys = nullptr; IDirect3DSurface9* resolved = nullptr;
	if (d.MultiSampleType != D3DMULTISAMPLE_NONE) {	// multisampled: resolved first (the samples are averaged)
		if (FAILED(D->CreateRenderTarget(d.Width, d.Height, d.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &resolved, nullptr)) ||
			FAILED(D->StretchRect(s, nullptr, resolved, nullptr, D3DTEXF_NONE))) { if (resolved) resolved->Release(); return false; }
		s = resolved;
	}
	if (FAILED(D->CreateOffscreenPlainSurface(d.Width, d.Height, d.Format, D3DPOOL_SYSTEMMEM, &sys, nullptr))) { if (resolved) resolved->Release(); return false; }
	bool ok = SUCCEEDED(D->GetRenderTargetData(s, sys));
	if (resolved) resolved->Release();
	D3DLOCKED_RECT l = {};
	const unsigned bpp = PixelBytes(d.Format);
	if (ok && bpp && SUCCEEDED(sys->LockRect(&l, nullptr, D3DLOCK_READONLY))) {
		w = d.Width; h = d.Height; format = d.Format;
		rgba.resize((size_t)w * h * 4);
		for (DWORD y = 0; y < h && ok; y++) for (DWORD x = 0; x < w && ok; x++) ok = Pixel(d.Format, (const unsigned char*)l.pBits + (size_t)y * l.Pitch + (size_t)x * bpp, &rgba[((size_t)y * w + x) * 4]);
		sys->UnlockRect();
	}
	else ok = false;
	sys->Release();
	return ok;
}
static std::vector<unsigned> ToBgra(const std::vector<float>& rgba) {
	std::vector<unsigned> out(rgba.size() / 4);
	for (size_t i = 0; i < out.size(); i++) out[i] = 0xFF000000u | (Byte(rgba[i * 4]) << 16) | (Byte(rgba[i * 4 + 1]) << 8) | Byte(rgba[i * 4 + 2]);
	return out;
}

// ---------------------------------------------------------------- predicates and script
struct Pred {
	enum Kind { All, Draws, Actions, PS, VS, Marker, Split, RT, Tex, Not, And, Or } kind = All;
	int a = 0, b = 0;
	std::string text;
	std::vector<DWORD> ids;	// shaders matching PS/VS
	std::unique_ptr<Pred> l, r;
};

static std::vector<std::string> Tokenize(const std::string& line) {
	std::vector<std::string> t;
	size_t i = 0;
	while (i < line.size()) {
		while (i < line.size() && isspace((unsigned char)line[i])) i++;
		if (i >= line.size() || line[i] == '#') break;
		if (line[i] == '"') { size_t j = line.find('"', i + 1); if (j == std::string::npos) j = line.size(); t.push_back(line.substr(i + 1, j - i - 1)); i = j + 1; }
		else { size_t j = i; while (j < line.size() && !isspace((unsigned char)line[j])) j++; t.push_back(line.substr(i, j - i)); i = j; }
	}
	return t;
}

static std::string ShaderLabel(DWORD id) {
	if (!id || id >= Res.size()) return "(none)";
	const Resource& r = Res[id];
	char h[24]; sprintf_s(h, "%016llx", r.hash);
	std::string s = h;
	for (auto& n : r.names) s += " | " + n;
	return s;
}

static std::vector<DWORD> MatchShaders(const std::string& x, bool pixel) {
	std::vector<DWORD> out;
	const std::string lx = Lower(x);
	const bool hex = x.size() >= 6 && x.find_first_not_of("0123456789abcdefABCDEF") == std::string::npos;
	for (DWORD id = 1; id < Res.size(); id++) {
		const Resource& r = Res[id];
		if (r.record != kRecShader || r.isPixel != pixel) continue;
		bool m = false;
		if (!lx.compare(0, 3, "id:")) m = (DWORD)atoi(lx.c_str() + 3) == id;
		else {
			char h[24]; sprintf_s(h, "%016llx", r.hash);
			if (hex && !strncmp(h, lx.c_str(), lx.size())) m = true;
			for (auto& n : r.names) if (Lower(n).find(lx) != std::string::npos) m = true;
		}
		if (m) out.push_back(id);
	}
	return out;
}

static std::unique_ptr<Pred> ParsePred(const std::vector<std::string>& t, size_t& i);
static std::unique_ptr<Pred> ParseTerm(const std::vector<std::string>& t, size_t& i) {
	if (i >= t.size()) Fail("script: predicate expected");
	auto p = std::make_unique<Pred>();
	const std::string w = Lower(t[i++]);
	auto rangeArg = [&](int& a, int& b) {
		if (i >= t.size()) Fail("script: range A-B expected after '%s'", w.c_str());
		const std::string& s = t[i++];
		const size_t dash = s.find('-');
		a = atoi(s.c_str()); b = dash == std::string::npos ? a : atoi(s.c_str() + dash + 1);
	};
	auto textArg = [&]() { if (i >= t.size()) Fail("script: argument expected after '%s'", w.c_str()); return t[i++]; };
	if (w == "all") p->kind = Pred::All;
	else if (w == "draws") { p->kind = Pred::Draws; rangeArg(p->a, p->b); }
	else if (w == "actions") { p->kind = Pred::Actions; rangeArg(p->a, p->b); }
	else if (w == "ps" || w == "vs") {
		p->kind = w == "ps" ? Pred::PS : Pred::VS; p->text = textArg(); p->ids = MatchShaders(p->text, w == "ps");
		if (p->ids.empty()) Warn("predicate %s '%s' matches no shader", w.c_str(), p->text.c_str());
	}
	else if (w == "marker") { p->kind = Pred::Marker; p->text = textArg(); }
	else if (w == "split") { p->kind = Pred::Split; p->text = Lower(textArg()); }
	else if (w == "rt") { p->kind = Pred::RT; p->a = atoi(textArg().c_str()); }
	else if (w == "tex") { p->kind = Pred::Tex; p->a = atoi(textArg().c_str()); }
	else if (w == "not") { p->kind = Pred::Not; p->l = ParseTerm(t, i); }
	else if (w == "(") { p = ParsePred(t, i); if (i < t.size() && t[i] == ")") i++; }
	else Fail("script: unknown predicate '%s'", w.c_str());
	return p;
}
static std::unique_ptr<Pred> ParsePred(const std::vector<std::string>& t, size_t& i) {
	auto left = ParseTerm(t, i);
	while (i < t.size()) {
		const std::string w = Lower(t[i]);
		if (w != "and" && w != "or") break;
		i++;
		auto p = std::make_unique<Pred>();
		p->kind = w == "and" ? Pred::And : Pred::Or;
		p->l = std::move(left);
		p->r = ParseTerm(t, i);
		left = std::move(p);
	}
	return left;
}

static bool Match(const Pred& p, const Op& op) {
	const FrameState& s = States[op.state];
	switch (p.kind) {
	case Pred::All: return true;
	case Pred::Draws: return op.draw >= p.a && op.draw <= p.b;
	case Pred::Actions: return op.index >= p.a && op.index <= p.b;
	case Pred::PS: return op.IsDraw() && std::find(p.ids.begin(), p.ids.end(), s.ps) != p.ids.end();
	case Pred::VS: return op.IsDraw() && std::find(p.ids.begin(), p.ids.end(), s.vs) != p.ids.end();
	case Pred::Marker: {
		for (unsigned short r : op.ranges) if (Ranges[r].name == "marker: " + p.text) return true;
		return false;
	}
	case Pred::Split: return Lower(op.split).find(p.text) != std::string::npos && op.split != "(none)";
	case Pred::RT: return op.IsDraw() && (s.rt[0] == (DWORD)p.a || s.rt[1] == (DWORD)p.a || s.rt[2] == (DWORD)p.a || s.rt[3] == (DWORD)p.a);
	case Pred::Tex: { if (!op.IsDraw()) return false; for (unsigned i = 0; i < kSamplers; i++) if (s.tex[i] == (DWORD)p.a) return true; return false; }
	case Pred::Not: return !Match(*p.l, op);
	case Pred::And: return Match(*p.l, op) && Match(*p.r, op);
	case Pred::Or: return Match(*p.l, op) || Match(*p.r, op);
	}
	return false;
}

struct NameValue { const char* name; DWORD value; };
static const NameValue RenderStateNames[] = {
	{ "ZENABLE", 7 }, { "FILLMODE", 8 }, { "SHADEMODE", 9 }, { "ZWRITEENABLE", 14 }, { "ALPHATESTENABLE", 15 }, { "LASTPIXEL", 16 }, { "SRCBLEND", 19 },
	{ "DESTBLEND", 20 }, { "CULLMODE", 22 }, { "ZFUNC", 23 }, { "ALPHAREF", 24 }, { "ALPHAFUNC", 25 }, { "DITHERENABLE", 26 }, { "ALPHABLENDENABLE", 27 },
	{ "FOGENABLE", 28 }, { "SPECULARENABLE", 29 }, { "STENCILENABLE", 52 }, { "STENCILFAIL", 53 }, { "STENCILZFAIL", 54 }, { "STENCILPASS", 55 },
	{ "STENCILFUNC", 56 }, { "STENCILREF", 57 }, { "STENCILMASK", 58 }, { "STENCILWRITEMASK", 59 }, { "CLIPPING", 136 }, { "LIGHTING", 137 },
	{ "CLIPPLANEENABLE", 152 }, { "MULTISAMPLEANTIALIAS", 161 }, { "MULTISAMPLEMASK", 162 }, { "COLORWRITEENABLE", 168 }, { "BLENDOP", 171 },
	{ "SCISSORTESTENABLE", 174 }, { "SLOPESCALEDEPTHBIAS", 175 }, { "ADAPTIVETESS_Y", 181 }, { "TWOSIDEDSTENCILMODE", 185 }, { "COLORWRITEENABLE1", 190 },
	{ "COLORWRITEENABLE2", 191 }, { "COLORWRITEENABLE3", 192 }, { "SRGBWRITEENABLE", 194 }, { "DEPTHBIAS", 195 }, { "SEPARATEALPHABLENDENABLE", 206 },
	{ "SRCBLENDALPHA", 207 }, { "DESTBLENDALPHA", 208 }, { "BLENDOPALPHA", 209 } };
static const NameValue SamplerStateNames[] = { { "ADDRESSU", 1 }, { "ADDRESSV", 2 }, { "ADDRESSW", 3 }, { "BORDERCOLOR", 4 }, { "MAGFILTER", 5 }, { "MINFILTER", 6 },
	{ "MIPFILTER", 7 }, { "MIPMAPLODBIAS", 8 }, { "MAXMIPLEVEL", 9 }, { "MAXANISOTROPY", 10 }, { "SRGBTEXTURE", 11 }, { "ELEMENTINDEX", 12 }, { "DMAPOFFSET", 13 } };
static const NameValue ValueNames[] = { { "TRUE", 1 }, { "FALSE", 0 }, { "D3DCMP_NEVER", 1 }, { "D3DCMP_LESS", 2 }, { "D3DCMP_EQUAL", 3 }, { "D3DCMP_LESSEQUAL", 4 },
	{ "D3DCMP_GREATER", 5 }, { "D3DCMP_NOTEQUAL", 6 }, { "D3DCMP_GREATEREQUAL", 7 }, { "D3DCMP_ALWAYS", 8 }, { "D3DCULL_NONE", 1 }, { "D3DCULL_CW", 2 }, { "D3DCULL_CCW", 3 },
	{ "D3DBLEND_ZERO", 1 }, { "D3DBLEND_ONE", 2 }, { "D3DBLEND_SRCCOLOR", 3 }, { "D3DBLEND_INVSRCCOLOR", 4 }, { "D3DBLEND_SRCALPHA", 5 }, { "D3DBLEND_INVSRCALPHA", 6 },
	{ "D3DBLEND_DESTALPHA", 7 }, { "D3DBLEND_INVDESTALPHA", 8 }, { "D3DBLEND_DESTCOLOR", 9 }, { "D3DBLEND_INVDESTCOLOR", 10 }, { "D3DTEXF_NONE", 0 }, { "D3DTEXF_POINT", 1 },
	{ "D3DTEXF_LINEAR", 2 }, { "D3DTEXF_ANISOTROPIC", 3 }, { "D3DZB_TRUE", 1 }, { "D3DZB_FALSE", 0 } };
static DWORD Lookup(const std::string& s, const NameValue* table, size_t n, const char* prefix) {
	std::string u = s; for (char& c : u) c = (char)toupper((unsigned char)c);
	if (!u.compare(0, strlen(prefix), prefix)) u = u.substr(strlen(prefix));
	for (size_t i = 0; i < n; i++) if (u == table[i].name) return table[i].value;
	char* end = nullptr;
	const unsigned long v = strtoul(s.c_str(), &end, 0);
	if (end && *end == 0 && !s.empty()) return (DWORD)v;
	Fail("script: unknown name '%s'", s.c_str());
	return 0;
}
static DWORD ValueOf(const std::string& s) {
	std::string u = s; for (char& c : u) c = (char)toupper((unsigned char)c);
	for (auto& v : ValueNames) if (u == v.name) return v.value;
	char* end = nullptr;
	if (s.find('.') != std::string::npos) { float f = (float)atof(s.c_str()); DWORD d; memcpy(&d, &f, 4); return d; }
	const unsigned long v = strtoul(s.c_str(), &end, 0);
	if (end && *end == 0 && !s.empty()) return (DWORD)v;
	Fail("script: unknown value '%s'", s.c_str());
	return 0;
}

static void OwnState(Op& op) { States.push_back(States[op.state]); op.state = (int)States.size() - 1; }	// copy on write

// 2026-11-02 (wave 3 rewrites): a replacement pixel shader may keep its constants and samplers in other registers than the recorded
// one (a rewritten effect: same TESR_ names, other allocation). The swapped draw's state then gets every named constant / sampler
// of the new shader filled from where the recorded shader had it (its own copy of the state, so later draws keep the recorded
// registers). Returns the names the recording did not set (left as they were: usually zero), for a warning.
static std::string RemapPixelConstants(FrameState& st, const std::vector<unsigned char>& oldCode, const std::vector<unsigned char>& newCode) {
	ID3DXConstantTable *ot = nullptr, *nt = nullptr;
	std::string missing;
	if (oldCode.empty() || newCode.empty() || FAILED(D3DXGetShaderConstantTable((const DWORD*)oldCode.data(), &ot)) || !ot ||
		FAILED(D3DXGetShaderConstantTable((const DWORD*)newCode.data(), &nt)) || !nt) {
		if (ot) ot->Release();
		if (nt) nt->Release();
		return "(no constant table: not remapped)";
	}
	const FrameState was = st;
	D3DXCONSTANTTABLE_DESC nd = {};
	nt->GetDesc(&nd);
	for (UINT i = 0; i < nd.Constants; i++) {
		D3DXHANDLE h = nt->GetConstant(nullptr, i);
		D3DXCONSTANT_DESC n = {}; UINT count = 1;
		if (!h || FAILED(nt->GetConstantDesc(h, &n, &count))) continue;
		D3DXHANDLE oh = ot->GetConstantByName(nullptr, n.Name);
		D3DXCONSTANT_DESC o = {}; count = 1;
		if (!oh || FAILED(ot->GetConstantDesc(oh, &o, &count)) || o.RegisterSet != n.RegisterSet) { missing += std::string(missing.empty() ? "" : ", ") + n.Name; continue; }
		const UINT regs = (std::min)(o.RegisterCount, n.RegisterCount);
		if (getenv("REPLAY_REMAP_DEBUG")) printf("  remap %-40s set %u  %u x%u -> %u x%u  first %g %g %g %g\n", n.Name, (unsigned)n.RegisterSet, o.RegisterIndex, o.RegisterCount, n.RegisterIndex, n.RegisterCount,
			n.RegisterSet == D3DXRS_FLOAT4 && o.RegisterIndex < kPSFloat ? was.psf[o.RegisterIndex][0] : 0.f, n.RegisterSet == D3DXRS_FLOAT4 && o.RegisterIndex < kPSFloat ? was.psf[o.RegisterIndex][1] : 0.f,
			n.RegisterSet == D3DXRS_FLOAT4 && o.RegisterIndex < kPSFloat ? was.psf[o.RegisterIndex][2] : 0.f, n.RegisterSet == D3DXRS_FLOAT4 && o.RegisterIndex < kPSFloat ? was.psf[o.RegisterIndex][3] : 0.f);
		for (UINT r = 0; r < regs; r++) {
			const UINT from = o.RegisterIndex + r, to = n.RegisterIndex + r;
			if (n.RegisterSet == D3DXRS_FLOAT4 && from < kPSFloat && to < kPSFloat) memcpy(st.psf[to], was.psf[from], sizeof(st.psf[to]));
			else if (n.RegisterSet == D3DXRS_SAMPLER && from < 16 && to < 16) { st.tex[to] = was.tex[from]; memcpy(st.ss[to], was.ss[from], sizeof(st.ss[to])); }
			else if (n.RegisterSet == D3DXRS_INT4 && from < 16 && to < 16) memcpy(st.psi[to], was.psi[from], sizeof(st.psi[to]));
			else if (n.RegisterSet == D3DXRS_BOOL && from < 16 && to < 16) st.psb[to] = was.psb[from];
		}
	}
	static bool told = false;
	if (!told) { told = true; printf("script: constants remapped by name: %u in the new shader's table\n", (unsigned)nd.Constants); }
	ot->Release();
	nt->Release();
	return missing;
}

static DWORD AddShader(const std::vector<unsigned char>& code, bool pixel, const std::string& name) {
	const DWORD id = (DWORD)Res.size();
	Resource& r = R(id);
	r.record = kRecShader; r.isPixel = pixel; r.code = code; r.hash = Hash(code.data(), code.size()); r.names.push_back(name);
	const HRESULT hr = pixel ? D->CreatePixelShader((const DWORD*)code.data(), &r.ps) : D->CreateVertexShader((const DWORD*)code.data(), &r.vs);
	if (FAILED(hr)) Fail("shader %s could not be created (%08lX)", name.c_str(), (unsigned long)hr);
	return id;
}

static std::vector<unsigned char> LoadShaderFile(const std::string& file, bool pixel) {
	std::string ext = Lower(file.substr(file.find_last_of('.') + 1));
	if (ext == "hlsl" || ext == "fx" || ext == "txt") {
		std::string error;
		auto code = CompileFile(file, pixel ? "ps_3_0" : "vs_3_0", {}, error);
		if (code.empty()) Fail("%s: %s", file.c_str(), error.c_str());
		return code;
	}
	FILE* f = nullptr;
	if (fopen_s(&f, file.c_str(), "rb") || !f) Fail("cannot read %s", file.c_str());
	std::vector<unsigned char> code;
	unsigned char b[65536]; size_t n;
	while ((n = fread(b, 1, sizeof(b), f)) > 0) code.insert(code.end(), b, b + n);
	fclose(f);
	return code;
}

struct CountJob { std::unique_ptr<Pred> pred; std::string name; };
struct OcclusionJob { std::unique_ptr<Pred> pred; std::string name; };
struct DumpJob { int action; std::string target, file; };
static std::vector<CountJob> CountJobs;
static std::vector<OcclusionJob> OcclusionJobs;
static std::vector<DumpJob> DumpJobs;
static bool StatesReport = false;
static std::vector<std::unique_ptr<Pred>> RangePreds;	// parallel to user ranges (index into Ranges)
static std::vector<unsigned short> RangePredIds;

static void RunScript(const char* path) {
	FILE* f = nullptr;
	if (fopen_s(&f, path, "r") || !f) Fail("cannot read script %s", path);
	char line[2048];
	int number = 0;
	struct Move { std::unique_ptr<Pred> what, where; };
	std::vector<Move> moves;
	while (fgets(line, sizeof(line), f)) {
		number++;
		auto t = Tokenize(line);
		if (t.empty()) continue;
		const std::string c = Lower(t[0]);
		size_t i = 1;
		if (c == "range") {
			if (t.size() < 3) Fail("script line %d: range <name> <predicate>", number);
			i = 2;
			auto p = ParsePred(t, i);
			Range r; r.name = t[1]; r.user = true; Ranges.push_back(r);
			RangePredIds.push_back((unsigned short)(Ranges.size() - 1));
			RangePreds.push_back(std::move(p));
		}
		else if (c == "ps" || c == "vs") {
			auto p = ParsePred(t, i);
			if (i >= t.size()) Fail("script line %d: shader file expected", number);
			const bool pixel = c == "ps";
			const std::string file = t[i];
			const DWORD id = AddShader(LoadShaderFile(file, pixel), pixel, "replacement " + file);
			unsigned n = 0;
			std::set<std::string> unset;
			for (Op& op : Ops) if (op.IsDraw() && Match(*p, op)) {
				OwnState(op);
				if (pixel) {	// 2026-11-02: constants and samplers by name (RemapPixelConstants)
					const DWORD was = States[op.state].ps;
					if (was && was < Res.size() && id < Res.size()) { const std::string m = RemapPixelConstants(States[op.state], Res[was].code, Res[id].code); if (!m.empty()) unset.insert(m); }
				}
				(pixel ? States[op.state].ps : States[op.state].vs) = id; n++;
			}
			printf("script: %s on %u draws -> %s\n", c.c_str(), n, file.c_str());
			for (const std::string& m : unset) printf("script: WARNING the recording does not set %s for %s (left as recorded in those registers)\n", m.c_str(), file.c_str());
		}
		else if (c == "rs") {
			auto p = ParsePred(t, i);
			if (i + 1 >= t.size()) Fail("script line %d: rs <predicate> <state> <value>", number);
			const DWORD state = Lookup(t[i], RenderStateNames, sizeof(RenderStateNames) / sizeof(RenderStateNames[0]), "D3DRS_");
			const DWORD value = ValueOf(t[i + 1]);
			if (state >= kRenderStates) Fail("script line %d: render state %u out of range", number, state);
			unsigned n = 0;
			for (Op& op : Ops) if (op.IsDraw() && Match(*p, op)) { OwnState(op); States[op.state].rs[state] = value; n++; }
			printf("script: rs %u = %u on %u draws\n", state, value, n);
		}
		else if (c == "ss") {
			auto p = ParsePred(t, i);
			if (i + 2 >= t.size()) Fail("script line %d: ss <predicate> <stage> <state> <value>", number);
			const DWORD stage = (DWORD)atoi(t[i].c_str());
			const DWORD state = Lookup(t[i + 1], SamplerStateNames, sizeof(SamplerStateNames) / sizeof(SamplerStateNames[0]), "D3DSAMP_");
			const DWORD value = ValueOf(t[i + 2]);
			if (stage >= kSamplers || state >= kSamplerStates) Fail("script line %d: stage or state out of range", number);
			unsigned n = 0;
			for (Op& op : Ops) if (op.IsDraw() && Match(*p, op)) { OwnState(op); States[op.state].ss[stage][state] = value; n++; }
			printf("script: ss stage %u state %u = %u on %u draws\n", stage, state, value, n);
		}
		else if (c == "skip") {
			auto p = ParsePred(t, i);
			unsigned n = 0;
			for (Op& op : Ops) if (op.kind == kOpAction && op.act.type != kActPresent && Match(*p, op)) { op.skip = true; n++; }
			printf("script: skip %u actions\n", n);
		}
		else if (c == "move") {
			Move m;
			m.what = ParsePred(t, i);
			if (i >= t.size() || Lower(t[i]) != "before") Fail("script line %d: move <predicate> before <predicate>", number);
			i++;
			m.where = ParsePred(t, i);
			moves.push_back(std::move(m));
		}
		else if (c == "count") { CountJob j; j.pred = ParsePred(t, i); j.name = i < t.size() ? t[i] : "count" + std::to_string(CountJobs.size()); CountJobs.push_back(std::move(j)); }
		else if (c == "occlusion") { OcclusionJob j; j.pred = ParsePred(t, i); j.name = i < t.size() ? t[i] : "occlusion" + std::to_string(OcclusionJobs.size()); OcclusionJobs.push_back(std::move(j)); }
		else if (c == "dump") {
			if (t.size() < 4) Fail("script line %d: dump <action index> <rt0|rt1|rt2|rt3|depth> <file>", number);
			DumpJobs.push_back({ atoi(t[1].c_str()), Lower(t[2]), t[3] });
		}
		else if (c == "states") StatesReport = true;
		else Fail("script line %d: unknown command '%s'", number, c.c_str());
	}
	fclose(f);
	for (Move& m : moves) {
		std::vector<Op> moved, rest;
		for (Op& op : Ops) (op.kind == kOpAction && op.act.type != kActPresent && Match(*m.what, op) ? moved : rest).push_back(std::move(op));
		size_t at = rest.size();
		for (size_t k = 0; k < rest.size(); k++) if (Match(*m.where, rest[k])) { at = k; break; }
		if (at == rest.size()) { Warn("move: no action matches the 'before' predicate; nothing moved"); for (Op& op : moved) rest.push_back(std::move(op)); std::stable_sort(rest.begin(), rest.end(), [](const Op& a, const Op& b) { return a.index < b.index; }); Ops = std::move(rest); continue; }
		printf("script: move %u actions before action %d\n", (unsigned)moved.size(), rest[at].index);
		rest.insert(rest.begin() + at, std::make_move_iterator(moved.begin()), std::make_move_iterator(moved.end()));
		Ops = std::move(rest);
	}
	// User ranges: membership from the predicate (evaluated on each action, wherever it now is).
	for (size_t k = 0; k < RangePreds.size(); k++)
		for (Op& op : Ops) if (Match(*RangePreds[k], op)) { op.ranges.push_back(RangePredIds[k]); std::sort(op.ranges.begin(), op.ranges.end()); }
}

// ---------------------------------------------------------------- timing
struct QuerySet {
	IDirect3DQuery9* disjoint = nullptr; IDirect3DQuery9* freq = nullptr;
	std::vector<IDirect3DQuery9*> ts;
	bool pending = false;
};

static void IssueTs(QuerySet& q, size_t& used) {
	if (used == q.ts.size()) { IDirect3DQuery9* x = nullptr; if (FAILED(D->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &x))) Fail("timestamp queries are not supported"); q.ts.push_back(x); }
	q.ts[used++]->Issue(D3DISSUE_END);
}

// The executed actions and where the timestamps go (an action whose range set differs from the previous one starts an interval).
static std::vector<size_t> Executed;
static std::vector<size_t> BoundaryAt;	// positions into Executed
static void PlanTiming() {
	Executed.clear(); BoundaryAt.clear();
	for (size_t i = 0; i < Ops.size(); i++) if (!Ops[i].skip && !(Ops[i].kind == kOpAction && Ops[i].act.type == kActPresent)) Executed.push_back(i);
	for (size_t k = 0; k < Executed.size(); k++)
		if (k == 0 || Ops[Executed[k]].ranges != Ops[Executed[k - 1]].ranges) BoundaryAt.push_back(k);
}

static double LastRestoreMs = 0;
// --kick <n> (default 512): every n actions the replayer hands the commands it has queued to the GPU (an event query's
// GetData with D3DGETDATA_FLUSH, which does not wait). The D3D9 driver otherwise keeps commands in its buffer until it is full or
// the frame is presented, and the GPU, which is not a frame behind as in the game, waits in the middle of the frame.
static int KickEvery = 512;	// 2026-10-17 noon capture: 0 = two speed modes; 8 / 32 / 128 / 512 = deterministic, 16.1 ms at 512 (fastest tried), 16.5 at 32, 19.4 at 8
static void Kick() {
	static IDirect3DQuery9* q = nullptr;
	if (!q && FAILED(D->CreateQuery(D3DQUERYTYPE_EVENT, &q))) { KickEvery = 0; return; }
	q->Issue(D3DISSUE_END);
	q->GetData(nullptr, 0, D3DGETDATA_FLUSH);
}
// --lead <ms> (default off): before each timed frame the GPU gets a few ms of throw-away work, so the CPU has submitted a good part
// of the frame before the GPU reaches it. Without it the GPU sometimes catches up with the replayer's CPU in the middle of
// the frame (the driver lets the CPU run only one frame ahead) and those idle gaps land in the ranges: the timing showed two
// modes, one at the game's numbers and one ~3 ms slower (2026-10-17 noon capture).
static double LeadMs = 0.0;	// measured 2026-10-17: 6 and 12 ms leads changed nothing (the slow mode is not CPU starvation); kept as an option
static void Lead() {
	if (LeadMs <= 0) return;
	static IDirect3DSurface9* target = nullptr; static IDirect3DVertexShader9* vs = nullptr; static IDirect3DPixelShader9* ps = nullptr;
	static int draws = 0;
	struct V { float x, y, z, w, u, v; };
	static const V quad[4] = { { -1, 1, 0, 1, 0, 0 }, { 1, 1, 0, 1, 1, 0 }, { -1, -1, 0, 1, 0, 1 }, { 1, -1, 0, 1, 1, 1 } };
	auto setup = [&]() {
		for (int i = 1; i < 4; i++) D->SetRenderTarget(i, nullptr);
		D->SetRenderTarget(0, target); D->SetDepthStencilSurface(nullptr);
		D->SetVertexShader(vs); D->SetPixelShader(ps); D->SetFVF(D3DFVF_XYZW | D3DFVF_TEX1);
		const D3DRENDERSTATETYPE off[] = { D3DRS_ZENABLE, D3DRS_ALPHABLENDENABLE, D3DRS_ALPHATESTENABLE, D3DRS_SCISSORTESTENABLE, D3DRS_STENCILENABLE, D3DRS_CLIPPLANEENABLE };
		for (auto rs : off) D->SetRenderState(rs, FALSE);
		D->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE); D->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
	};
	if (!target) {
		auto v = CompileText("void main(float4 p : POSITION, float2 t : TEXCOORD0, out float4 op : POSITION, out float2 ot : TEXCOORD0) { op = p; ot = t; }", "vs_3_0");
		auto f = CompileText("float4 main(float2 t : TEXCOORD0) : COLOR0 { float x = t.x + t.y; [loop] for (int i = 0; i < 250; i++) x = frac(sin(x * 12.9898) * 43758.5453); return x; }", "ps_3_0");
		D->CreateVertexShader((const DWORD*)v.data(), &vs); D->CreatePixelShader((const DWORD*)f.data(), &ps);
		if (FAILED(D->CreateRenderTarget(512, 512, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &target, nullptr))) { LeadMs = 0; return; }
		// Calibrate: one draw's GPU time.
		IDirect3DQuery9 *t0 = nullptr, *t1 = nullptr, *fq = nullptr;
		D->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &t0); D->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &t1); D->CreateQuery(D3DQUERYTYPE_TIMESTAMPFREQ, &fq);
		double one = 0.5;
		if (t0 && t1 && fq) {
			SceneGuard scene; setup();
			for (int w = 0; w < 4; w++) D->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(V));
			t0->Issue(D3DISSUE_END);
			for (int k = 0; k < 8; k++) D->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(V));
			t1->Issue(D3DISSUE_END); fq->Issue(D3DISSUE_END);
			UINT64 a = 0, b = 0, hz = 0;
			while (t1->GetData(&b, 8, D3DGETDATA_FLUSH) == S_FALSE) {}
			t0->GetData(&a, 8, D3DGETDATA_FLUSH); while (fq->GetData(&hz, 8, D3DGETDATA_FLUSH) == S_FALSE) {}
			if (hz && b > a) one = (double)(b - a) * 1000.0 / hz / 8;
		}
		if (t0) t0->Release(); if (t1) t1->Release(); if (fq) fq->Release();
		draws = std::max(1, (int)(LeadMs / std::max(one, 0.01) + 0.5));
		printf("lead: %d throw-away draws of %.3f ms before each timed frame (--lead %.1f)\n", draws, one, LeadMs);
		ShadowValid = false;
	}
	SceneGuard scene;
	setup();
	for (int k = 0; k < draws; k++) D->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(V));
	ShadowValid = false;
}
static void RunFrame(QuerySet* q, std::vector<IDirect3DQuery9*>* occlusion = nullptr, const Pred* occPred = nullptr) {
	LARGE_INTEGER f0, r0, r1; QueryPerformanceFrequency(&f0); QueryPerformanceCounter(&r0);
	RestoreStart();
	if (q) Lead();
	if (q && KickEvery > 0) Kick();
	QueryPerformanceCounter(&r1); LastRestoreMs = (r1.QuadPart - r0.QuadPart) * 1000.0 / f0.QuadPart;
	size_t used = 0, b = 0;
	if (q) { if (q->disjoint) q->disjoint->Issue(D3DISSUE_BEGIN); }
	for (size_t k = 0; k < Executed.size(); k++) {
		if (q && b < BoundaryAt.size() && BoundaryAt[b] == k) { IssueTs(*q, used); b++; }
		if (q && KickEvery > 0 && k && k % KickEvery == 0) Kick();
		const Op& op = Ops[Executed[k]];
		IDirect3DQuery9* oq = nullptr;
		if (occlusion && occPred && op.IsDraw() && Match(*occPred, op)) {
			if (FAILED(D->CreateQuery(D3DQUERYTYPE_OCCLUSION, &oq))) oq = nullptr;
			if (oq) oq->Issue(D3DISSUE_BEGIN);
		}
		ExecuteAction(op);
		if (oq) { oq->Issue(D3DISSUE_END); occlusion->push_back(oq); }
		else if (occlusion && occPred && op.IsDraw() && Match(*occPred, op)) occlusion->push_back(nullptr);
	}
	if (q) { IssueTs(*q, used); if (q->freq) q->freq->Issue(D3DISSUE_END); if (q->disjoint) q->disjoint->Issue(D3DISSUE_END); q->pending = true; }
}

static bool Collect(QuerySet& q, std::vector<double>& rangeMs, double& frameMs) {
	if (!q.pending) return false;
	q.pending = false;
	BOOL disjoint = FALSE; UINT64 freq = 0;
	if (q.disjoint) while (q.disjoint->GetData(&disjoint, sizeof(disjoint), D3DGETDATA_FLUSH) == S_FALSE) {}
	if (q.freq) while (q.freq->GetData(&freq, sizeof(freq), D3DGETDATA_FLUSH) == S_FALSE) {}
	if (!freq) freq = 1000000000ull;
	const size_t n = BoundaryAt.size() + 1;
	std::vector<UINT64> t(n);
	for (size_t i = 0; i < n; i++) while (q.ts[i]->GetData(&t[i], sizeof(UINT64), D3DGETDATA_FLUSH) == S_FALSE) {}
	if (disjoint) return false;
	rangeMs.assign(Ranges.size(), 0.0);
	for (size_t i = 0; i + 1 < n; i++) {
		const double ms = (double)(t[i + 1] - t[i]) * 1000.0 / (double)freq;
		for (unsigned short r : Ops[Executed[BoundaryAt[i]]].ranges) rangeMs[r] += ms;
	}
	frameMs = (double)(t[n - 1] - t[0]) * 1000.0 / (double)freq;
	return true;
}

static double Percentile(std::vector<double> v, double p) {
	if (v.empty()) return 0;
	std::sort(v.begin(), v.end());
	return v[std::min(v.size() - 1, (size_t)(p * (v.size() - 1) + 0.5))];
}

// ---------------------------------------------------------------- diagnosis passes
static DWORD AddSurfaceRes(IDirect3DSurface9* s) { const DWORD id = (DWORD)Res.size(); Resource& r = R(id); r.record = kRecSurface; r.surface = s; return id; }

static void CountPass(const CountJob& job, const std::string& outDir) {
	// Counter targets, one per render target size the matching draws use.
	const bool fp32 = SUCCEEDED([] { IDirect3D9* d3d = nullptr; D->GetDirect3D(&d3d); D3DDISPLAYMODE m; d3d->GetAdapterDisplayMode(0, &m);
		HRESULT hr = d3d->CheckDeviceFormat(0, D3DDEVTYPE_HAL, m.Format, D3DUSAGE_QUERY_POSTPIXELSHADER_BLENDING | D3DUSAGE_RENDERTARGET, D3DRTYPE_SURFACE, D3DFMT_R32F); d3d->Release(); return hr; }());
	const D3DFORMAT counterFormat = fp32 ? D3DFMT_R32F : D3DFMT_R16F;
	static DWORD psAlpha3 = 0, psAlpha2 = 0, psOne3 = 0, psOne2 = 0;
	if (!psAlpha3) {
		const char* alpha = "sampler2D s0 : register(s0); float4 main(float2 uv : TEXCOORD0) : COLOR0 { return float4(1, 0, 0, tex2D(s0, uv).a); }";
		const char* one = "float4 main() : COLOR0 { return float4(1, 0, 0, 1); }";
		psAlpha3 = AddShader(CompileText(alpha, "ps_3_0"), true, "count (alpha) ps_3_0");
		psAlpha2 = AddShader(CompileText(alpha, "ps_2_0"), true, "count (alpha) ps_2_0");
		psOne3 = AddShader(CompileText(one, "ps_3_0"), true, "count ps_3_0");
		psOne2 = AddShader(CompileText(one, "ps_2_0"), true, "count ps_2_0");
	}
	std::map<std::pair<UINT, UINT>, DWORD> counters;	// by size; multisampled like the draw's target (the depth buffer must match)
	unsigned matching = 0;
	for (size_t k : Executed) {
		const Op& op = Ops[k];
		if (!op.IsDraw() || !Match(*job.pred, op)) continue;
		matching++;
		D3DSURFACE_DESC d = {}; IDirect3DSurface9* rt = SurfaceOf(States[op.state].rt[0]);
		if (!rt) continue;
		rt->GetDesc(&d);
		auto key = std::make_pair(d.Width, d.Height);
		if (!counters.count(key)) {
			IDirect3DSurface9* s = nullptr;
			if (FAILED(D->CreateRenderTarget(d.Width, d.Height, counterFormat, d.MultiSampleType, d.MultiSampleQuality, FALSE, &s, nullptr))) Fail("count: no counter target");
			if (d.MultiSampleType != D3DMULTISAMPLE_NONE) printf("count: counter %ux%u multisampled (type %u) like the scene: counts are averaged over the samples (edges fractional)\n", d.Width, d.Height, (unsigned)d.MultiSampleType);
			counters[key] = AddSurfaceRes(s);
		}
	}
	printf("count '%s': %u matching draws, counter format %s\n", job.name.c_str(), matching, fp32 ? "R32F" : "R16F");
	if (!matching) return;
	FILE* csv = nullptr;
	fopen_s(&csv, (outDir + "\\" + job.name + "_histogram.csv").c_str(), "w");
	if (csv) fprintf(csv, "variant,layers,pixels\n");
	const char* variants[4] = { "depth-same_alpha-kept", "depth-same_no-alpha", "depth-off_alpha-kept", "depth-off_no-alpha" };
	for (int v = 0; v < 4; v++) {
		const bool depthOff = v >= 2, alphaKept = (v % 2) == 0;
		RestoreStart();
		for (auto& c : counters) { D->SetRenderTarget(0, Res[c.second].surface); D->SetDepthStencilSurface(nullptr); D->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE); D->Clear(0, nullptr, D3DCLEAR_TARGET, 0, 1.0f, 0); }
		ShadowValid = false;
		for (size_t k : Executed) {
			const Op& op = Ops[k];
			if (op.IsDraw() && Match(*job.pred, op)) {
				FrameState s = States[op.state];
				IDirect3DSurface9* rt = SurfaceOf(s.rt[0]);
				if (rt) {
					D3DSURFACE_DESC d = {}; rt->GetDesc(&d);
					s.rt[0] = counters[std::make_pair(d.Width, d.Height)]; s.rt[1] = s.rt[2] = s.rt[3] = 0;
					const bool vs3 = s.vs && s.vs < Res.size() && Res[s.vs].code.size() >= 4 && *(const DWORD*)Res[s.vs].code.data() == 0xFFFE0300;
					s.ps = alphaKept ? (vs3 ? psAlpha3 : psAlpha2) : (vs3 ? psOne3 : psOne2);
					s.rs[D3DRS_ALPHABLENDENABLE] = TRUE; s.rs[D3DRS_SRCBLEND] = D3DBLEND_ONE; s.rs[D3DRS_DESTBLEND] = D3DBLEND_ONE; s.rs[D3DRS_BLENDOP] = D3DBLENDOP_ADD;
					s.rs[D3DRS_SEPARATEALPHABLENDENABLE] = FALSE; s.rs[D3DRS_COLORWRITEENABLE] = 0xF; s.rs[D3DRS_SRGBWRITEENABLE] = FALSE; s.rs[D3DRS_FOGENABLE] = FALSE;
					s.rs[D3DRS_ZWRITEENABLE] = FALSE; s.rs[D3DRS_STENCILWRITEMASK] = 0; s.rs[D3DRS_ADAPTIVETESS_Y] = 0;
					if (depthOff) { s.rs[D3DRS_ZENABLE] = FALSE; s.rs[D3DRS_STENCILENABLE] = FALSE; }
					if (!alphaKept) s.rs[D3DRS_ALPHATESTENABLE] = FALSE;
					ExecuteAction(op, &s);
				}
			}
			ExecuteAction(op);
		}
		for (auto& c : counters) {
			std::vector<float> rgba; DWORD w = 0, h = 0; D3DFORMAT f;
			if (!ReadSurface(Res[c.second].surface, rgba, w, h, f)) { Warn("count: counter not read"); continue; }
			std::vector<unsigned long long> hist(65, 0);
			double sum = 0; unsigned long long covered = 0; unsigned maxLayers = 0;
			for (size_t i = 0; i < (size_t)w * h; i++) {
				const unsigned n = (unsigned)(rgba[i * 4] + 0.5f);
				hist[std::min(n, 64u)]++;
				if (n) { covered++; sum += rgba[i * 4]; }
				maxLayers = std::max(maxLayers, n);
			}
			printf("  %-24s %ux%u: covered %.1f%% of pixels, mean %.2f layers where covered, %.3f per screen pixel, max %u | 1:%.1f%% 2:%.1f%% 3:%.1f%% 4:%.1f%% 5-8:%.1f%% 9+:%.1f%%\n",
				variants[v], w, h, 100.0 * covered / ((double)w * h), covered ? sum / covered : 0.0, sum / ((double)w * h), maxLayers,
				100.0 * hist[1] / ((double)w * h), 100.0 * hist[2] / ((double)w * h), 100.0 * hist[3] / ((double)w * h), 100.0 * hist[4] / ((double)w * h),
				100.0 * (hist[5] + hist[6] + hist[7] + hist[8]) / ((double)w * h), [&] { unsigned long long s9 = 0; for (int i = 9; i <= 64; i++) s9 += hist[i]; return 100.0 * s9 / ((double)w * h); }());
			if (csv) for (int i = 0; i <= 64; i++) if (hist[i]) fprintf(csv, "%s,%d%s,%llu\n", variants[v], i, i == 64 ? "+" : "", hist[i]);
			// Heat map: 0 black, 1 blue, 2 green, 3 yellow, 4 orange, 5-7 red, 8+ white.
			std::vector<unsigned> img((size_t)w * h);
			for (size_t i = 0; i < img.size(); i++) {
				const unsigned n = (unsigned)(rgba[i * 4] + 0.5f);
				static const unsigned ramp[9] = { 0xFF000000, 0xFF2040FF, 0xFF20C040, 0xFFFFFF00, 0xFFFF9000, 0xFFFF2000, 0xFFC00000, 0xFF900000, 0xFFFFFFFF };
				img[i] = ramp[std::min(n, 8u)];
			}
			char name[64]; sprintf_s(name, "_%ux%u", w, h);
			SavePng(outDir + "\\" + job.name + "_" + variants[v] + (counters.size() > 1 ? name : "") + ".png", w, h, img);
		}
	}
	if (csv) fclose(csv);
}

static void OcclusionPass(const OcclusionJob& job, const std::string& outDir) {
	std::vector<IDirect3DQuery9*> queries;
	RunFrame(nullptr, &queries, job.pred.get());
	FILE* csv = nullptr;
	fopen_s(&csv, (outDir + "\\" + job.name + ".csv").c_str(), "w");
	if (csv) fprintf(csv, "action,draw,samples,ps,vs\n");
	unsigned long long total = 0; unsigned draws = 0, zero = 0;
	size_t qi = 0;
	for (size_t k : Executed) {
		const Op& op = Ops[k];
		if (!op.IsDraw() || !Match(*job.pred, op)) continue;
		IDirect3DQuery9* q = qi < queries.size() ? queries[qi++] : nullptr;
		DWORD samples = 0;
		if (q) { while (q->GetData(&samples, sizeof(samples), D3DGETDATA_FLUSH) == S_FALSE) {} q->Release(); }
		total += samples; draws++; if (!samples) zero++;
		if (csv) fprintf(csv, "%d,%d,%lu,\"%s\",\"%s\"\n", op.index, op.draw, samples, ShaderLabel(States[op.state].ps).c_str(), ShaderLabel(States[op.state].vs).c_str());
	}
	if (csv) fclose(csv);
	printf("occlusion '%s': %u draws, %llu samples passed in all (%.2f screens), %u draws with none\n", job.name.c_str(), draws, total, (double)total / ((double)Width * Height), zero);
}

static IDirect3DSurface9* DepthCopy(IDirect3DSurface9* ds, DWORD dsId) {
	// INTZ (or another depth texture): sampled into an R32F target by a full-screen pass.
	Resource& r = Res[dsId];
	IDirect3DBaseTexture9* tex = r.record == kRecSubSurface && r.sub.parent < Res.size() ? Res[r.sub.parent].texture : nullptr;
	if (!tex) { Warn("dump depth: the depth buffer is not a texture (contents not readable)"); return nullptr; }
	D3DSURFACE_DESC d = {}; ds->GetDesc(&d);
	IDirect3DSurface9* out = nullptr;
	if (FAILED(D->CreateRenderTarget(d.Width, d.Height, D3DFMT_R32F, D3DMULTISAMPLE_NONE, 0, FALSE, &out, nullptr))) return nullptr;
	static IDirect3DVertexShader9* vs = nullptr; static IDirect3DPixelShader9* ps = nullptr;
	if (!vs) {
		auto v = CompileText("void main(float4 p : POSITION, float2 t : TEXCOORD0, out float4 op : POSITION, out float2 ot : TEXCOORD0) { op = p; ot = t; }", "vs_3_0");
		auto f = CompileText("sampler2D s0 : register(s0); float4 main(float2 t : TEXCOORD0) : COLOR0 { return tex2D(s0, t).rrrr; }", "ps_3_0");
		D->CreateVertexShader((const DWORD*)v.data(), &vs); D->CreatePixelShader((const DWORD*)f.data(), &ps);
	}
	struct V { float x, y, z, w, u, v; };
	const float du = 0.5f / d.Width, dv = 0.5f / d.Height;
	V quad[4] = { { -1, 1, 0, 1, 0 + du, 0 + dv }, { 1, 1, 0, 1, 1 + du, 0 + dv }, { -1, -1, 0, 1, 0 + du, 1 + dv }, { 1, -1, 0, 1, 1 + du, 1 + dv } };
	IDirect3DStateBlock9* saved = nullptr;
	D->CreateStateBlock(D3DSBT_ALL, &saved);
	D->SetRenderTarget(0, out); for (int i = 1; i < 4; i++) D->SetRenderTarget(i, nullptr);
	D->SetDepthStencilSurface(nullptr);
	D->SetVertexShader(vs); D->SetPixelShader(ps); D->SetFVF(D3DFVF_XYZW | D3DFVF_TEX1);
	D->SetTexture(0, tex);
	D->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT); D->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT); D->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
	D->SetRenderState(D3DRS_ZENABLE, FALSE); D->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE); D->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
	D->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE); D->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF); D->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
	D->SetRenderState(D3DRS_STENCILENABLE, FALSE); D->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
	{ SceneGuard scene; D->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(V)); }
	if (saved) { saved->Apply(); saved->Release(); }
	ShadowValid = false;
	return out;
}

static void DumpAfter(const Op& op, const std::string& outDir) {
	for (const DumpJob& j : DumpJobs) {
		if (j.action != op.index) continue;
		const FrameState& s = States[op.state];
		std::string file = j.file.find(':') == std::string::npos && j.file[0] != '\\' ? outDir + "\\" + j.file : j.file;
		IDirect3DSurface9* target = nullptr;
		bool depth = false;
		if (j.target == "depth") { depth = true; IDirect3DSurface9* ds = SurfaceOf(s.ds); if (ds) target = DepthCopy(ds, s.ds); }
		else if (j.target.size() == 3 && j.target.compare(0, 2, "rt") == 0) { const int i = j.target[2] - '0'; if (i >= 0 && i < 4) { target = SurfaceOf(s.rt[i]); if (target) target->AddRef(); } }
		if (!target) { Warn("dump %d %s: no such target", j.action, j.target.c_str()); continue; }
		std::vector<float> rgba; DWORD w = 0, h = 0; D3DFORMAT f;
		if (!ReadSurface(target, rgba, w, h, f)) { Warn("dump %d %s: not readable", j.action, j.target.c_str()); target->Release(); continue; }
		target->Release();
		if (depth) {	// stretched to the range present, so the picture shows something
			float lo = FLT_MAX, hi = -FLT_MAX;
			for (size_t i = 0; i < (size_t)w * h; i++) { lo = std::min(lo, rgba[i * 4]); hi = std::max(hi, rgba[i * 4]); }
			for (size_t i = 0; i < (size_t)w * h; i++) { const float v = hi > lo ? (rgba[i * 4] - lo) / (hi - lo) : 0; rgba[i * 4] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = v; }
			printf("dump %d depth: range %.6f .. %.6f -> %s\n", j.action, lo, hi, file.c_str());
		}
		else printf("dump %d %s (format %u, %ux%u) -> %s\n", j.action, j.target.c_str(), (unsigned)f, w, h, file.c_str());
		SavePng(file, w, h, ToBgra(rgba));
	}
}

static void StatesPass(const std::string& outDir) {
	FILE* out = nullptr;
	fopen_s(&out, (outDir + "\\states.txt").c_str(), "w");
	auto line = [&](const char* format, ...) { char b[1024]; va_list a; va_start(a, format); _vsnprintf_s(b, _TRUNCATE, format, a); va_end(a); printf("%s\n", b); if (out) fprintf(out, "%s\n", b); };
	static const char* cmp[9] = { "?", "NEVER", "LESS", "EQUAL", "LESSEQUAL", "GREATER", "NOTEQUAL", "GREATEREQUAL", "ALWAYS" };
	line("STATES: depth state of draws in order (a new line where a depth-testing draw's (ZENABLE, ZWRITE, ZFUNC, depth buffer) changes), and every depth clear");
	std::set<std::tuple<DWORD, DWORD, DWORD>> seenWriting;
	std::tuple<DWORD, DWORD, DWORD, DWORD> last = { 99, 99, 99, 99 };
	for (size_t k : Executed) {
		const Op& op = Ops[k];
		const FrameState& s = States[op.state];
		if (op.kind == kOpAction && op.act.type == kActClear && (op.act.a[1] & (D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL))) {
			float z; memcpy(&z, &op.act.a[3], 4);
			line("  action %6d CLEAR depth buffer %u: flags %s%s%s z %.3f stencil %u | %s | %s", op.index, s.ds, (op.act.a[1] & D3DCLEAR_TARGET) ? "T" : "", (op.act.a[1] & D3DCLEAR_ZBUFFER) ? "Z" : "",
				(op.act.a[1] & D3DCLEAR_STENCIL) ? "S" : "", z, op.act.a[4], op.marker.c_str(), op.split.c_str());
			last = { 99, 99, 99, 99 };
			continue;
		}
		if (!op.IsDraw() || !s.rs[D3DRS_ZENABLE] || !s.ds) continue;
		const DWORD zw = s.rs[D3DRS_ZWRITEENABLE], zf = s.rs[D3DRS_ZFUNC];
		auto now = std::make_tuple(s.rs[D3DRS_ZENABLE], zw, zf, s.ds);
		if (now != last) {
			const bool firstWriting = zw && seenWriting.insert(std::make_tuple(s.rs[D3DRS_ZENABLE], zw, zf)).second;
			line("  action %6d draw %5d: ZENABLE %u ZWRITE %u ZFUNC %-12s depth buffer %u%s | %s | %s | ps %s", op.index, op.draw, s.rs[D3DRS_ZENABLE], zw, zf <= 8 ? cmp[zf] : "?", s.ds,
				firstWriting ? "  <-- first depth-WRITING draw with this compare" : "", op.marker.c_str(), op.split.c_str(), ShaderLabel(s.ps).c_str());
			last = now;
		}
	}
	line("STATES: distinct (ZENABLE, ZWRITE, ZFUNC) among depth-writing draws: %u", (unsigned)seenWriting.size());
	for (auto& t : seenWriting) line("  ZENABLE %u ZWRITE %u ZFUNC %s", std::get<0>(t), std::get<1>(t), std::get<2>(t) <= 8 ? cmp[std::get<2>(t)] : "?");
	if (out) fclose(out);
}

static void ListDraws(const std::string& outDir) {
	FILE* csv = nullptr;
	if (fopen_s(&csv, (outDir + "\\draws.csv").c_str(), "w") || !csv) return;
	fprintf(csv, "action,draw,type,prims,ps,vs,rt0,ds,zenable,zwrite,zfunc,alphatest,alpharef,alphafunc,blend,src,dst,cull,colorwrite,tex0,minfilter,magfilter,mipfilter,maxaniso,marker,split\n");
	for (const Op& op : Ops) {
		if (op.kind != kOpAction) continue;
		const FrameState& s = States[op.state];
		const DWORD prims = op.act.type == kActDraw ? op.act.a[2] : op.act.type == kActDrawIndexed ? op.act.a[5] : op.act.type == kActDrawUP ? op.act.a[1] : op.act.type == kActDrawIndexedUP ? op.act.a[3] : 0;
		static const char* types[] = { "?", "Draw", "DrawIndexed", "DrawUP", "DrawIndexedUP", "Clear", "StretchRect", "ColorFill", "UpdateSurface", "UpdateTexture", "NvStretchRect", "BeginScene", "EndScene", "Present" };
		const bool draw = op.IsDraw();
		fprintf(csv, "%d,%d,%s,%lu,\"%s\",\"%s\",%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,0x%lX,%lu,%lu,%lu,%lu,%lu,\"%s\",\"%s\"\n", op.index, op.draw,
			op.act.type < 14 ? types[op.act.type] : "?", prims, draw ? ShaderLabel(s.ps).c_str() : "", draw ? ShaderLabel(s.vs).c_str() : "", s.rt[0], s.ds,
			s.rs[D3DRS_ZENABLE], s.rs[D3DRS_ZWRITEENABLE], s.rs[D3DRS_ZFUNC], s.rs[D3DRS_ALPHATESTENABLE], s.rs[D3DRS_ALPHAREF], s.rs[D3DRS_ALPHAFUNC],
			s.rs[D3DRS_ALPHABLENDENABLE], s.rs[D3DRS_SRCBLEND], s.rs[D3DRS_DESTBLEND], s.rs[D3DRS_CULLMODE], s.rs[D3DRS_COLORWRITEENABLE], s.tex[0],
			s.ss[0][D3DSAMP_MINFILTER], s.ss[0][D3DSAMP_MAGFILTER], s.ss[0][D3DSAMP_MIPFILTER], s.ss[0][D3DSAMP_MAXANISOTROPY], op.marker.c_str(), op.split.c_str());
	}
	fclose(csv);
	printf("per-action list: %s\\draws.csv\n", outDir.c_str());
}

// ---------------------------------------------------------------- main
static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcA(h, m, w, l); }

int main(int argc, char** argv) {
	if (argc >= 2 && !strcmp(argv[1], "--compile-effect")) return CompileEffectCommand(argc, argv);
	if (argc >= 5 && !strcmp(argv[1], "--compile")) {
		std::vector<std::pair<std::string, std::string>> defines;
		for (int i = 5; i < argc; i++) { std::string d = argv[i]; const size_t eq = d.find('='); defines.push_back({ d.substr(0, eq), eq == std::string::npos ? "1" : d.substr(eq + 1) }); }
		std::string error;
		auto code = CompileFile(argv[2], argv[3], defines, error);
		if (code.empty()) { fprintf(stderr, "%s\n", error.c_str()); return 1; }
		FILE* f = nullptr;
		if (fopen_s(&f, argv[4], "wb") || !f) Fail("cannot write %s", argv[4]);
		fwrite(code.data(), 1, code.size(), f); fclose(f);
		printf("compiled %s (%s) -> %s, %u bytes, hash %016llx\n", argv[2], argv[3], argv[4], (unsigned)code.size(), Hash(code.data(), code.size()));
		return 0;
	}
	if (argc < 2) {
		printf("frame_replay <capture.nvrf> [script.txt] [--warmup N] [--runs N] [--out DIR] [--list] [--no-validate] [--info] [--d3dx] [--no-preshader]\n"
			"frame_replay --compile <file.hlsl> <profile> <out.bin> [NAME=VALUE ...]\n");
		return 1;
	}
	const char* capture = argv[1];
	const char* script = nullptr;
	int warmup = 10, runs = 50, gapMs = 0, showWindow = 1;
	bool validate = true, list = false, infoOnly = false;
	std::string outDir = std::string(capture) + ".replay";
	for (int i = 2; i < argc; i++) {
		const std::string a = argv[i];
		if (a == "--warmup" && i + 1 < argc) warmup = atoi(argv[++i]);
		else if (a == "--runs" && i + 1 < argc) runs = std::max(1, atoi(argv[++i]));
		else if (a == "--out" && i + 1 < argc) outDir = argv[++i];
		else if (a == "--no-validate") validate = false;
		else if (a == "--list") list = true;
		else if (a == "--info") infoOnly = true;
		else if (a == "--restore-copy") RestoreByCopy = true;
		else if (a == "--gap" && i + 1 < argc) gapMs = atoi(argv[++i]);
		else if (a == "--lead" && i + 1 < argc) LeadMs = atof(argv[++i]);
		else if (a == "--restore-once") RestoreOnce = true;
		else if (a == "--d3dx") UseCompiler47 = false;
		else if (a == "--no-preshader") EffectFlags = D3DXSHADER_NO_PRESHADER;
		else if (a == "--kick" && i + 1 < argc) KickEvery = atoi(argv[++i]);
		else if (a == "--window" && i + 1 < argc) { const std::string w = argv[++i]; showWindow = w == "hidden" ? 0 : w == "full" ? 2 : 1; }
		else if (a[0] != '-' && !script) script = argv[i];
		else Fail("unknown option %s", a.c_str());
	}
	CreateDirectoryA(outDir.c_str(), nullptr);
	CoInitializeEx(nullptr, COINIT_MULTITHREADED);

	// Header first: the device is made at the capture's size.
	{
		FILE* f = nullptr;
		if (fopen_s(&f, capture, "rb") || !f) Fail("cannot open %s", capture);
		DWORD header[6] = {};
		fread(header, sizeof(header), 1, f); fclose(f);
		if (header[0] != kMagic) Fail("%s is not a frame capture", capture);
		Width = header[3]; Height = header[4]; BBFormat = header[5];
	}
	WNDCLASSA wc = {}; wc.lpfnWndProc = WndProc; wc.hInstance = GetModuleHandleA(nullptr); wc.lpszClassName = "NVRFrameReplay";
	RegisterClassA(&wc);
	HWND window = CreateWindowExA(0, wc.lpszClassName, "NVR frame replay", WS_OVERLAPPEDWINDOW, 0, 0, 640, 360, nullptr, nullptr, wc.hInstance, nullptr);
	if (showWindow == 1) ShowWindow(window, SW_SHOWNOACTIVATE);
	else if (showWindow == 2) { SetWindowLongA(window, GWL_STYLE, WS_POPUP | WS_VISIBLE); SetWindowPos(window, HWND_TOP, 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN), SWP_NOACTIVATE | SWP_SHOWWINDOW); }
	IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
	if (!d3d) Fail("no Direct3D 9");
	D3DPRESENT_PARAMETERS pp = {};
	pp.BackBufferWidth = Width; pp.BackBufferHeight = Height; pp.BackBufferFormat = (D3DFORMAT)(BBFormat ? BBFormat : D3DFMT_X8R8G8B8); pp.BackBufferCount = 1;
	pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.hDeviceWindow = window; pp.Windowed = TRUE; pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
	if (FAILED(d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window, D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE, &pp, &D)))
		Fail("CreateDevice %ux%u failed", Width, Height);
	D3DADAPTER_IDENTIFIER9 id = {}; d3d->GetAdapterIdentifier(0, 0, &id);
	D->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &BackBuffer);
	NvOk = NvAPI_Initialize() == NVAPI_OK;
	printf("device: %s, %ux%u, NvAPI %s\n", id.Description, Width, Height, NvOk ? "ok" : "NOT available (depth resolves skipped)");

	const DWORD loadStart = GetTickCount();
	Load(capture);
	unsigned draws = 0; for (auto& op : Ops) if (op.IsDraw()) draws++;
	printf("loaded %s in %.1f s: %u actions (%u draws), %u states, %u resources, %u restores\n", capture, (GetTickCount() - loadStart) / 1000.0, (unsigned)Ops.size(), draws,
		(unsigned)States.size(), (unsigned)Res.size() - 1, (unsigned)Restores.size());
	printf("recorder: %s\n", Info.c_str());
	if (NvOk) {
		double g, m; GpuClocks(g, m);
		NV_DISPLAY_DRIVER_MEMORY_INFO mi = {}; mi.version = NV_DISPLAY_DRIVER_MEMORY_INFO_VER;
		if (NvGpu && NvAPI_GPU_GetMemoryInfo(NvGpu, &mi) == NVAPI_OK)
			printf("GPU memory: %u MB dedicated, %u MB of it free after loading (clocks now %.0f / %.0f MHz)\n", mi.dedicatedVideoMemory / 1024, mi.curAvailableDedicatedVideoMemory / 1024, g, m);
	}
	if (infoOnly) return 0;
	RestoresToGpu();
	if (script) RunScript(script);
	PlanTiming();

	// Validation: one replay, the back buffer against the game's picture of the same frame.
	char validation[256] = "not checked";
	if (validate) {
		RunFrame(nullptr);
		std::vector<float> rgba; DWORD w = 0, h = 0; D3DFORMAT f;
		if (ReadSurface(BackBuffer, rgba, w, h, f)) {
			std::vector<unsigned> replay = ToBgra(rgba);
			SavePng(outDir + "\\replay.png", w, h, replay);
			if (RefImage.size() == (size_t)w * h) {
				SavePng(outDir + "\\reference.png", RefW, RefH, RefImage);
				double sum = 0; unsigned long long over = 0; unsigned maxDiff = 0;
				std::vector<unsigned> diff(replay.size());
				for (size_t i = 0; i < replay.size(); i++) {
					unsigned worst = 0;
					for (int c = 0; c < 3; c++) {
						const int a = (replay[i] >> (c * 8)) & 255, b = (RefImage[i] >> (c * 8)) & 255;
						const unsigned d = (unsigned)abs(a - b);
						sum += d; worst = std::max(worst, d);
					}
					if (worst > 2) over++;
					maxDiff = std::max(maxDiff, worst);
					const unsigned v = std::min(255u, worst * 8);
					diff[i] = 0xFF000000u | (v << 16) | (v << 8) | v;
				}
				SavePng(outDir + "\\difference_x8.png", w, h, diff);
				sprintf_s(validation, "mean abs diff %.3f/255, %.3f%% pixels > 2/255, max %u", sum / (replay.size() * 3.0), 100.0 * over / replay.size(), maxDiff);
			}
			else sprintf_s(validation, "no reference picture of this size in the capture");
		}
		else sprintf_s(validation, "back buffer not readable");
		D->Present(nullptr, nullptr, nullptr, nullptr);
		printf("validation: %s (pictures in %s)\n", validation, outDir.c_str());
	}

	// Timing.
	const int ring = 3;
	QuerySet sets[ring];
	for (auto& q : sets) {
		if (FAILED(D->CreateQuery(D3DQUERYTYPE_TIMESTAMPDISJOINT, &q.disjoint))) q.disjoint = nullptr;
		if (FAILED(D->CreateQuery(D3DQUERYTYPE_TIMESTAMPFREQ, &q.freq))) q.freq = nullptr;
	}
	std::vector<double> frameMs, cpuMs, restoreMs, clockG, clockM;
	LARGE_INTEGER qpf; QueryPerformanceFrequency(&qpf);
	const int total = warmup + runs;
	for (int it = 0; it < total + ring; it++) {
		QuerySet& q = sets[it % ring];
		if (q.pending) {
			std::vector<double> rangeMs; double ms = 0;
			const int which = it - ring;
			if (Collect(q, rangeMs, ms) && which >= warmup) {
				frameMs.push_back(ms);
				for (size_t r = 0; r < Ranges.size(); r++) Ranges[r].ms.push_back(rangeMs[r]);
			}
		}
		if (it < total) {
			LARGE_INTEGER c0, c1; QueryPerformanceCounter(&c0);
			RunFrame(&q);
			QueryPerformanceCounter(&c1);
			if (it >= warmup) { cpuMs.push_back((c1.QuadPart - c0.QuadPart) * 1000.0 / qpf.QuadPart - LastRestoreMs); restoreMs.push_back(LastRestoreMs); }
			D->Present(nullptr, nullptr, nullptr, nullptr);
			MSG msg; while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageA(&msg);
			if (gapMs > 0) Sleep(gapMs);
			if (it >= warmup) { double g, m; GpuClocks(g, m); clockG.push_back(g); clockM.push_back(m); }	// --gap: idle time between frames, like a game that is not 100 % GPU-bound
		}
	}
	FILE* csv = nullptr;
	{	// every timed run: shows warm-up drift or two speed modes
		FILE* runsCsv = nullptr;
		if (!fopen_s(&runsCsv, (outDir + "\\runs.csv").c_str(), "w") && runsCsv) {
			fprintf(runsCsv, "run,frame_ms,cpu_submit_ms,gpu_mhz,mem_mhz");
			for (size_t r = 1; r < Ranges.size(); r++) if (Ranges[r].user || Percentile(Ranges[r].ms, 0.5) > 0.2) fprintf(runsCsv, ",\"%s\"", Ranges[r].name.c_str());
			fprintf(runsCsv, "\n");
			for (size_t i = 0; i < frameMs.size(); i++) {
				fprintf(runsCsv, "%u,%.4f,%.4f,%.0f,%.0f", (unsigned)i, frameMs[i], i < cpuMs.size() ? cpuMs[i] : 0.0, i < clockG.size() ? clockG[i] : 0.0, i < clockM.size() ? clockM[i] : 0.0);
				for (size_t r = 1; r < Ranges.size(); r++) if (Ranges[r].user || Percentile(Ranges[r].ms, 0.5) > 0.2) fprintf(runsCsv, ",%.4f", i < Ranges[r].ms.size() ? Ranges[r].ms[i] : 0.0);
				fprintf(runsCsv, "\n");
			}
			fclose(runsCsv);
		}
	}
	fopen_s(&csv, (outDir + "\\timing.csv").c_str(), "w");
	if (csv) fprintf(csv, "range,median_ms,p10_ms,p90_ms,samples,user\n");
	printf("\nGPU time per range, %d runs after %d warm-up (median, p10-p90 ms):\n", (int)frameMs.size(), warmup);
	printf("  %-56s %8.4f  (%.4f-%.4f)\n", "FRAME (first action to last, before Present)", Percentile(frameMs, 0.5), Percentile(frameMs, 0.1), Percentile(frameMs, 0.9));
	printf("  %-56s %8.4f  (%.4f-%.4f)  (the replayer's own CPU time to submit the frame's actions)\n", "CPU submit", Percentile(cpuMs, 0.5), Percentile(cpuMs, 0.1), Percentile(cpuMs, 0.9));
	printf("  %-56s %8.4f  (%.4f-%.4f)  (CPU, before the frame's first timestamp)\n", "CPU restore of start contents", Percentile(restoreMs, 0.5), Percentile(restoreMs, 0.1), Percentile(restoreMs, 0.9));
	if (!clockG.empty()) printf("  %-56s %8.0f  (%.0f-%.0f) MHz, memory %.0f MHz\n", "GPU graphics clock (sampled after each Present)", Percentile(clockG, 0.5), Percentile(clockG, 0.1), Percentile(clockG, 0.9), Percentile(clockM, 0.5));
	if (csv) fprintf(csv, "\"FRAME\",%.5f,%.5f,%.5f,%u,0\n", Percentile(frameMs, 0.5), Percentile(frameMs, 0.1), Percentile(frameMs, 0.9), (unsigned)frameMs.size());
	std::vector<size_t> order;
	for (size_t r = 1; r < Ranges.size(); r++) order.push_back(r);
	std::stable_sort(order.begin(), order.end(), [](size_t a, size_t b) { if (Ranges[a].user != Ranges[b].user) return Ranges[a].user; return Percentile(Ranges[a].ms, 0.5) > Percentile(Ranges[b].ms, 0.5); });
	for (size_t r : order) {
		const double med = Percentile(Ranges[r].ms, 0.5);
		if (!Ranges[r].user && med < 0.001) continue;
		printf("  %-56s %8.4f  (%.4f-%.4f)%s\n", Ranges[r].name.c_str(), med, Percentile(Ranges[r].ms, 0.1), Percentile(Ranges[r].ms, 0.9), Ranges[r].user ? "  [script range]" : "");
		if (csv) fprintf(csv, "\"%s\",%.5f,%.5f,%.5f,%u,%d\n", Ranges[r].name.c_str(), med, Percentile(Ranges[r].ms, 0.1), Percentile(Ranges[r].ms, 0.9), (unsigned)Ranges[r].ms.size(), Ranges[r].user ? 1 : 0);
	}
	if (csv) fclose(csv);

	// Diagnosis passes.
	for (auto& j : CountJobs) CountPass(j, outDir);
	for (auto& j : OcclusionJobs) OcclusionPass(j, outDir);
	if (!DumpJobs.empty()) {
		RestoreStart();
		for (size_t k : Executed) { ExecuteAction(Ops[k]); DumpAfter(Ops[k], outDir); }
		D->Present(nullptr, nullptr, nullptr, nullptr);
	}
	if (StatesReport) StatesPass(outDir);
	if (list) ListDraws(outDir);

	printf("\nREPLAY: frame %.3f ms median (%.3f-%.3f), validation %s, %u timestamps a frame, %u warnings\n", Percentile(frameMs, 0.5), Percentile(frameMs, 0.1), Percentile(frameMs, 0.9),
		validation, (unsigned)BoundaryAt.size() + 1, Warnings);
	return 0;
}
