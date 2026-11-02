// UNOFFICIAL 2026-10-29: compile one game-shader template variant offline with the game's D3DX (D3DXCompileShaderFromFile, ps/vs_3_0),
// print errors and the instruction slot count. Usage: template_compile.exe <template.hlsl> <profile> NAME[=VALUE] ...
//
// UNOFFICIAL 2026-10-31 (docs/next/DEFERRED-RENDERING-PLAN.md stage 0c, tests/template_defines.py): the same, with what the game does on top:
//   --flow          on X4505 (out of temp registers) compile again with D3DXSHADER_PREFER_FLOW_CONTROL, as ShaderRecord::LoadShader does
//   --device [N]    create a D3D9 device on adapter N (default: the first NVIDIA one) and CreatePixelShader / CreateVertexShader the result:
//                   the device's acceptance (a GTX 1070 refuses ~3,600+ slot pixel shaders with D3DERR_INVALIDCALL), which the slot count alone
//                   does not tell
//   --out FILE      write the bytecode (byte-identity checks: tests/template_identity.py)
//   --preprocess F  write the preprocessed source instead (D3DXPreprocessShaderFromFile, as ShaderRecord::LoadShader's cache compare sees it;
//                   a check of a define list against the game's own Cache\*.hlsl)
//   --batch LIST    one process for many records: each line of LIST is  id<TAB>profile<TAB>source.hlsl<TAB>out-or-dash<TAB>NAME=VALUE NAME ...
//                   (the define list is NAME=VALUE pairs separated by spaces; NAME alone = empty definition); a line per record is printed:
//                   id OK slots=N ms=M flow=0|1 defines=K device=ok|refused(0x...)|-   or   id FAILED <the first error line>
#include <windows.h>
#include <d3d9.h>
#include <d3dx9.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>
#include <string>
#include <fstream>
#include <sstream>

static IDirect3DDevice9* gDevice = nullptr;
static std::string gAdapterName;

static bool MakeDevice(int adapterWanted) {
	IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
	if (!d3d) { printf("DEVICE: no D3D9\n"); return false; }
	UINT adapter = D3DADAPTER_DEFAULT;
	bool found = false;
	for (UINT a = 0; a < d3d->GetAdapterCount(); a++) {
		D3DADAPTER_IDENTIFIER9 id = {}; d3d->GetAdapterIdentifier(a, 0, &id);
		if (adapterWanted >= 0 ? (int)a == adapterWanted : (!found && strstr(id.Description, "NVIDIA"))) { adapter = a; found = true; gAdapterName = id.Description; }
	}
	if (!found) { D3DADAPTER_IDENTIFIER9 id = {}; d3d->GetAdapterIdentifier(adapter, 0, &id); gAdapterName = id.Description; }
	WNDCLASSA wc = {}; wc.lpfnWndProc = DefWindowProcA; wc.lpszClassName = "tmplcompile"; wc.hInstance = GetModuleHandleA(NULL);
	RegisterClassA(&wc);
	HWND window = CreateWindowA("tmplcompile", "tmplcompile", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, NULL, NULL, wc.hInstance, NULL);
	D3DPRESENT_PARAMETERS pp = {}; pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.BackBufferFormat = D3DFMT_UNKNOWN;
	pp.hDeviceWindow = window; pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
	if (FAILED(d3d->CreateDevice(adapter, D3DDEVTYPE_HAL, window, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &gDevice))) { printf("DEVICE: CreateDevice failed on %s\n", gAdapterName.c_str()); return false; }
	printf("DEVICE: %s\n", gAdapterName.c_str());
	return true;
}

struct Result { bool ok = false; int slots = -1; DWORD ms = 0; bool flow = false; std::string error; HRESULT device = S_OK; bool deviceTried = false; std::vector<char> code; };

static Result CompileOne(const char* file, const char* profile, const std::vector<std::pair<std::string, std::string>>& defines, bool flowRetry, bool device) {
	Result r;
	std::vector<D3DXMACRO> macros;
	for (const auto& d : defines) macros.push_back({ d.first.c_str(), d.second.c_str() });
	macros.push_back({ NULL, NULL });
	ID3DXBuffer* code = nullptr; ID3DXBuffer* errors = nullptr;
	const DWORD t0 = GetTickCount();
	HRESULT hr = D3DXCompileShaderFromFileA(file, macros.data(), NULL, "main", profile, 0, &code, &errors, NULL);
	if (FAILED(hr) && flowRetry && errors && strstr((const char*)errors->GetBufferPointer(), "X4505")) {
		errors->Release(); errors = nullptr;
		if (code) { code->Release(); code = nullptr; }
		hr = D3DXCompileShaderFromFileA(file, macros.data(), NULL, "main", profile, D3DXSHADER_PREFER_FLOW_CONTROL, &code, &errors, NULL);
		r.flow = true;
	}
	r.ms = GetTickCount() - t0;
	if (errors) { r.error = (const char*)errors->GetBufferPointer(); errors->Release(); }
	if (FAILED(hr) || !code) { if (code) code->Release(); return r; }
	r.ok = true;
	r.code.assign((const char*)code->GetBufferPointer(), (const char*)code->GetBufferPointer() + code->GetBufferSize());
	ID3DXBuffer* dis = nullptr;
	if (SUCCEEDED(D3DXDisassembleShader((const DWORD*)code->GetBufferPointer(), FALSE, NULL, &dis)) && dis) {
		const char* a = strstr((const char*)dis->GetBufferPointer(), "approximately ");
		if (a) r.slots = atoi(a + 14);
		dis->Release();
	}
	if (device && gDevice) {
		r.deviceTried = true;
		if (profile[0] == 'v') { IDirect3DVertexShader9* vs = nullptr; r.device = gDevice->CreateVertexShader((const DWORD*)code->GetBufferPointer(), &vs); if (vs) vs->Release(); }
		else { IDirect3DPixelShader9* ps = nullptr; r.device = gDevice->CreatePixelShader((const DWORD*)code->GetBufferPointer(), &ps); if (ps) ps->Release(); }
	}
	code->Release();
	return r;
}

static std::vector<std::pair<std::string, std::string>> ParseDefines(const std::vector<std::string>& words) {
	std::vector<std::pair<std::string, std::string>> out;
	for (const std::string& a : words) { if (a.empty()) continue; const size_t e = a.find('='); out.push_back({ a.substr(0, e), e == std::string::npos ? "" : a.substr(e + 1) }); }
	return out;
}

static std::string FirstLine(const std::string& s) { const size_t n = s.find('\n'); return n == std::string::npos ? s : s.substr(0, n); }

static void WriteCode(const std::string& path, const std::vector<char>& code) {
	if (path.empty() || path == "-") return;
	std::ofstream f(path, std::ios::out | std::ios::binary);
	f.write(code.data(), code.size());
}

int main(int argc, char** argv) {
	bool flow = false, device = false; int adapter = -1; std::string out, batch, preprocess;
	std::vector<std::string> rest;
	for (int i = 1; i < argc; i++) {
		std::string a = argv[i];
		if (a == "--flow") flow = true;
		else if (a == "--device") { device = true; if (i + 1 < argc && isdigit((unsigned char)argv[i + 1][0])) adapter = atoi(argv[++i]); }
		else if (a == "--out" && i + 1 < argc) out = argv[++i];
		else if (a == "--preprocess" && i + 1 < argc) preprocess = argv[++i];
		else if (a == "--batch" && i + 1 < argc) batch = argv[++i];
		else rest.push_back(a);
	}
	if (device && !MakeDevice(adapter)) device = false;
	if (!batch.empty()) {
		std::ifstream list(batch);
		if (!list.is_open()) { printf("cannot open %s\n", batch.c_str()); return 2; }
		std::string line; int failures = 0;
		while (std::getline(list, line)) {
			if (line.empty() || line[0] == '#') continue;
			std::vector<std::string> f; std::stringstream ss(line); std::string part;
			while (std::getline(ss, part, '\t')) f.push_back(part);
			if (f.size() < 5) { printf("%s BAD LINE\n", f.empty() ? "?" : f[0].c_str()); failures++; continue; }
			std::vector<std::string> words; std::stringstream ws(f[4]); std::string w;
			while (ws >> w) words.push_back(w);
			const auto defines = ParseDefines(words);
			Result r = CompileOne(f[2].c_str(), f[1].c_str(), defines, flow, device);
			if (!r.ok) { printf("%s FAILED %s\n", f[0].c_str(), FirstLine(r.error).c_str()); failures++; continue; }
			WriteCode(f[3], r.code);
			char dev[48] = "-";
			if (r.deviceTried) { if (SUCCEEDED(r.device)) strcpy(dev, "ok"); else sprintf(dev, "refused(0x%08lX)", (unsigned long)r.device); }
			printf("%s OK slots=%d ms=%lu flow=%d defines=%u device=%s\n", f[0].c_str(), r.slots, r.ms, r.flow ? 1 : 0, (unsigned)defines.size(), dev);
			fflush(stdout);
		}
		return failures ? 1 : 0;
	}
	if (rest.size() < 2) { printf("usage: template_compile [--flow] [--device [N]] [--out FILE] <file> <profile> NAME[=VALUE]...\n       template_compile [--flow] [--device [N]] --batch <list>\n"); return 2; }
	std::vector<std::string> words(rest.begin() + 2, rest.end());
	const auto defines = ParseDefines(words);
	if (!preprocess.empty()) {
		std::vector<D3DXMACRO> macros;
		for (const auto& d : defines) macros.push_back({ d.first.c_str(), d.second.c_str() });
		macros.push_back({ NULL, NULL });
		ID3DXBuffer* src = nullptr; ID3DXBuffer* errors = nullptr;
		HRESULT hr = D3DXPreprocessShaderFromFileA(rest[0].c_str(), macros.data(), NULL, &src, &errors);
		if (errors) { printf("%s\n", (const char*)errors->GetBufferPointer()); errors->Release(); }
		if (FAILED(hr) || !src) { printf("PREPROCESS FAILED\n"); return 1; }
		std::ofstream f(preprocess, std::ios::out | std::ios::binary);
		f.write((const char*)src->GetBufferPointer(), src->GetBufferSize());
		printf("preprocessed %lu bytes, %u defines -> %s\n", (unsigned long)src->GetBufferSize(), (unsigned)defines.size(), preprocess.c_str());
		src->Release();
		return 0;
	}
	Result r = CompileOne(rest[0].c_str(), rest[1].c_str(), defines, flow, device);
	if (!r.error.empty()) printf("%s\n", r.error.c_str());
	if (!r.ok) { printf("FAILED (%lu ms)\n", r.ms); return 1; }
	WriteCode(out, r.code);
	printf("OK %d instruction slots, %lu ms, %u defines%s", r.slots, r.ms, (unsigned)defines.size(), r.flow ? ", PREFER_FLOW_CONTROL retry" : "");
	if (r.deviceTried) printf(", device %s", SUCCEEDED(r.device) ? "ok" : "REFUSED");
	printf("\n");
	return 0;
}
