#pragma once

#include <unordered_set>
#include <vector>
#include <string>
#include <chrono>
#include <cstdio>

// UNOFFICIAL ShaderWarmUp ([Main.Main.Performance] ShaderWarmUp, default OFF, 2026-10-07).
// RESULT 2026-10-07 16:25 (FNVProbe + NVR logs): 78 records drawn at the menu in 0.3 ms, no menu hitch, and every first
// use in play still hitched (85-180 ms): the driver did not compile on these draws; it compiles per drawing setup (the
// partner shader, vertex layout, bound texture types), which this 1-pixel draw does not reproduce. Kept for a second
// attempt that records and replays the real setups.
//
// On plain Direct3D 9 the driver does the last part of a shader's compile the first time the shader is drawn, not when
// it is created. NVR's replacement shaders are big, so the first draw of each one cost 50-100 ms: the hitches when new
// cells came into view (FNVProbe 2026-10-07: 75-267 ms frames matched NVR's "<shader> : Texture ... Successfully bound"
// first-use log lines to ~0.1 s; with DXVK, which compiles at creation on worker threads, the same first uses cost
// nothing). So:
//  - ShaderRecord::SetCT notes the first use of each game shader's record (NoteFirstUse) in
//    Shaders\VegasShaders\Shaders\Cache\WarmUp.txt, one name a line, kept across sessions;
//  - at the main menu, RenderInterfaceHook calls OnInterfaceFrame: every record of every shader in that list is drawn
//    once into an 8x8 target (a 1-pixel triangle; the other stage gets a small generic shader), a time budget per
//    frame, with the device state saved in a state block (+ render targets and depth, which state blocks leave out)
//    and put back, so the menu draws as before. The driver compiles them there instead of in play.
// Only shaders already used once are warmed (warming all ~560 would take tens of seconds). Skipped under DXVK, which
// does not need it. Nothing in the picture changes: the same shaders are drawn the same way, only compiled earlier.
namespace ShaderWarmUp {
	inline std::unordered_set<std::string> Known;	// in the list file, or noted this session
	inline bool ListLoaded = false;

	inline const char* ListPath() { static const std::string path = std::string(ShadersPath) + "Cache\\WarmUp.txt"; return path.c_str(); }

	inline void LoadList() {
		if (ListLoaded) return;
		ListLoaded = true;
		FILE* f = nullptr;
		if (fopen_s(&f, ListPath(), "r") || !f) return;
		char line[160];
		while (fgets(line, sizeof(line), f)) {
			size_t n = strlen(line);
			while (n && (line[n - 1] == '\n' || line[n - 1] == '\r' || line[n - 1] == ' ')) line[--n] = 0;
			if (n) Known.insert(line);
		}
		fclose(f);
	}

	// From ShaderRecord::SetCT, the first time a record is used this session (main thread).
	inline void NoteFirstUse(const char* name) {
		if (!name) return;
		const size_t n = strlen(name);
		if (n < 5 || (_stricmp(name + n - 4, ".pso") && _stricmp(name + n - 4, ".vso"))) return;	// game shaders only
		LoadList();
		if (!Known.insert(name).second) return;
		FILE* f = nullptr;
		if (!fopen_s(&f, ListPath(), "a") && f) { fprintf(f, "%s\n", name); fclose(f); }
	}

	struct Item { IDirect3DVertexShader9* VS; IDirect3DPixelShader9* PS; const char* Name; };
	inline std::vector<Item> Queue;
	inline size_t Next = 0;
	inline bool Started = false, Finished = false;
	inline int Frames = 0;
	inline double DrawMs = 0, SlowestMs = 0;
	inline const char* Slowest = nullptr;
	inline IDirect3DVertexShader9* GenericVS = nullptr;
	inline IDirect3DPixelShader9* GenericPS = nullptr;
	inline IDirect3DVertexDeclaration9* Decl = nullptr;
	inline IDirect3DSurface9* Target = nullptr;
	inline std::chrono::steady_clock::time_point FirstFrame;

	inline double Now() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

	inline void BuildQueue() {
		LoadList();
		std::unordered_set<void*> seen;
		for (const auto& [CollectionName, Pointer] : TheShaderManager->ShaderNames) {
			ShaderCollection* Collection = Pointer ? *Pointer : nullptr;
			if (!Collection) continue;
			for (NiD3DPixelShaderEx* Shader : Collection->PixelShaderList) {
				if (!Shader || !Shader->Name || !Known.count(Shader->Name)) continue;
				for (int i = 0; i < 3; i++) {
					ShaderRecordPixel* Record = Shader->GetShaderRecord((ShaderRecordType)i);
					if (Record && Record->ShaderHandle && seen.insert(Record->ShaderHandle).second) Queue.push_back({ nullptr, Record->ShaderHandle, Shader->Name });
				}
			}
			for (NiD3DVertexShaderEx* Shader : Collection->VertexShaderList) {
				if (!Shader || !Shader->Name || !Known.count(Shader->Name)) continue;
				for (int i = 0; i < 3; i++) {
					ShaderRecordVertex* Record = Shader->GetShaderRecord((ShaderRecordType)i);
					if (Record && Record->ShaderHandle && seen.insert(Record->ShaderHandle).second) Queue.push_back({ Record->ShaderHandle, nullptr, Shader->Name });
				}
			}
		}
	}

	// The partner stage for a lone shader: a vertex shader that fills every output a pixel shader may read, and a pixel
	// shader that writes one colour. vs_3_0 allows 12 output registers but the compiler counts position among them
	// (X5622 with 12): position + 8 texcoords + 2 colours = 11.
	inline bool MakeResources(IDirect3DDevice9* Device) {
		static const char* kVS =
			"struct O { float4 p : POSITION; float4 t0 : TEXCOORD0; float4 t1 : TEXCOORD1; float4 t2 : TEXCOORD2; float4 t3 : TEXCOORD3;"
			" float4 t4 : TEXCOORD4; float4 t5 : TEXCOORD5; float4 t6 : TEXCOORD6; float4 t7 : TEXCOORD7;"
			" float4 c0 : COLOR0; float4 c1 : COLOR1; };"
			"O main(float4 p : POSITION) { O o; o.p = p; o.t0 = o.t1 = o.t2 = o.t3 = o.t4 = o.t5 = o.t6 = o.t7 = p; o.c0 = o.c1 = 1; return o; }";
		static const char* kPS = "float4 main() : COLOR0 { return 0; }";
		ID3DXBuffer* Code = nullptr;
		ID3DXBuffer* Errors = nullptr;
		if (FAILED(D3DXCompileShader(kVS, (UINT)strlen(kVS), NULL, NULL, "main", "vs_3_0", 0, &Code, &Errors, NULL)) || !Code ||
			FAILED(Device->CreateVertexShader((const DWORD*)Code->GetBufferPointer(), &GenericVS))) {
			if (Errors) { Logger::Log("Shader warm-up: generic vertex shader failed: %s", (const char*)Errors->GetBufferPointer()); Errors->Release(); }
			if (Code) Code->Release();
			return false;
		}
		Code->Release(); Code = nullptr;
		if (FAILED(D3DXCompileShader(kPS, (UINT)strlen(kPS), NULL, NULL, "main", "ps_3_0", 0, &Code, &Errors, NULL)) || !Code ||
			FAILED(Device->CreatePixelShader((const DWORD*)Code->GetBufferPointer(), &GenericPS))) {
			if (Errors) Errors->Release();
			if (Code) Code->Release();
			Logger::Log("Shader warm-up: generic pixel shader failed");
			return false;
		}
		Code->Release();
		// One float4 per vertex, offered under every usage a vertex shader may ask for (elements may share an offset).
		D3DVERTEXELEMENT9 Elements[20];
		int e = 0;
		const BYTE Usages[] = { D3DDECLUSAGE_POSITION, D3DDECLUSAGE_NORMAL, D3DDECLUSAGE_TANGENT, D3DDECLUSAGE_BINORMAL, D3DDECLUSAGE_BLENDWEIGHT };
		for (BYTE u : Usages) Elements[e++] = { 0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, u, 0 };
		Elements[e++] = { 0, 0, D3DDECLTYPE_UBYTE4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_BLENDINDICES, 0 };
		Elements[e++] = { 0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0 };
		Elements[e++] = { 0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 1 };
		for (BYTE i = 0; i < 8; i++) Elements[e++] = { 0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, i };
		Elements[e++] = D3DDECL_END();
		if (FAILED(Device->CreateVertexDeclaration(Elements, &Decl))) {
			D3DVERTEXELEMENT9 Simple[] = { { 0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 }, D3DDECL_END() };
			if (FAILED(Device->CreateVertexDeclaration(Simple, &Decl))) { Logger::Log("Shader warm-up: no vertex declaration"); return false; }
			Logger::Log("Shader warm-up: the full vertex declaration was refused, position only");
		}
		if (FAILED(Device->CreateRenderTarget(8, 8, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &Target, NULL))) {
			Logger::Log("Shader warm-up: no render target");
			return false;
		}
		return true;
	}

	inline void Release() {
		if (GenericVS) GenericVS->Release();
		if (GenericPS) GenericPS->Release();
		if (Decl) Decl->Release();
		if (Target) Target->Release();
		GenericVS = nullptr; GenericPS = nullptr; Decl = nullptr; Target = nullptr;
		Queue.clear(); Queue.shrink_to_fit();
	}

	inline void Finish(const char* Why) {
		Finished = true;
		if (Started)
			Logger::Log("Shader warm-up %s: %u of %u shader records drawn once in %d main-menu frames (draw calls %.1f ms; slowest %s %.1f ms)",
				Why, (unsigned)Next, (unsigned)Queue.size(), Frames, DrawMs, Slowest ? Slowest : "-", SlowestMs);
		Release();
	}

	// RenderInterfaceHook, every frame (main thread, inside the frame's scene).
	inline void OnInterfaceFrame() {
		if (Finished) return;
		if (!TheSettingManager->SettingsMain.Main.ShaderWarmUp || !strncmp(TheRenderManager->D3D9RuntimeDescription(), "DXVK", 4)) { Finished = true; return; }
		if (!InterfaceManager->IsActive(Menu::MenuType::kMenuType_Main)) {
			if (Started) Finish("stopped (left the main menu)");
			return;
		}
		IDirect3DDevice9* Device = TheRenderManager->device;
		if (!Device) return;
		if (!Started) {
			Started = true;
			BuildQueue();
			if (Queue.empty()) { Logger::Log("Shader warm-up: nothing to warm yet (%s lists the shaders used in play; it fills as you play)", ListPath()); Finish("done"); return; }
			if (!MakeResources(Device)) { Finish("gave up"); return; }
			Logger::Log("Shader warm-up: %u shader records from %u names in %s", (unsigned)Queue.size(), (unsigned)Known.size(), ListPath());
		}
		Frames++;

		IDirect3DStateBlock9* Saved = nullptr;
		if (FAILED(Device->CreateStateBlock(D3DSBT_ALL, &Saved)) || !Saved) { Finish("gave up (no state block)"); return; }
		IDirect3DSurface9* OldTargets[4] = {};
		IDirect3DSurface9* OldDepth = nullptr;
		for (DWORD i = 0; i < 4; i++) Device->GetRenderTarget(i, &OldTargets[i]);
		Device->GetDepthStencilSurface(&OldDepth);

		Device->SetRenderTarget(0, Target);
		for (DWORD i = 1; i < 4; i++) Device->SetRenderTarget(i, NULL);
		Device->SetDepthStencilSurface(NULL);
		D3DVIEWPORT9 Viewport = { 0, 0, 8, 8, 0.0f, 1.0f };
		Device->SetViewport(&Viewport);
		Device->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
		Device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
		Device->SetRenderState(D3DRS_STENCILENABLE, FALSE);
		Device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
		Device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
		Device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
		Device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
		Device->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
		for (DWORD i = 0; i < 16; i++) Device->SetTexture(i, NULL);
		for (DWORD i = 0; i < 4; i++) Device->SetTexture(D3DVERTEXTEXTURESAMPLER0 + i, NULL);
		Device->SetVertexDeclaration(Decl);

		static const float Triangle[3][4] = { { -1.0f, -1.0f, 0.5f, 1.0f }, { -0.75f, -1.0f, 0.5f, 1.0f }, { -1.0f, -0.75f, 0.5f, 1.0f } };
		const double Start = Now();
		while (Next < Queue.size() && Now() - Start < 40.0) {
			const Item& It = Queue[Next++];
			const double T0 = Now();
			Device->SetVertexShader(It.VS ? It.VS : GenericVS);
			Device->SetPixelShader(It.PS ? It.PS : GenericPS);
			Device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, Triangle, sizeof(Triangle[0]));
			const double Ms = Now() - T0;
			DrawMs += Ms;
			if (Ms > SlowestMs) { SlowestMs = Ms; Slowest = It.Name; }
		}

		for (DWORD i = 0; i < 4; i++) { Device->SetRenderTarget(i, OldTargets[i]); if (OldTargets[i]) OldTargets[i]->Release(); }
		Device->SetDepthStencilSurface(OldDepth);
		if (OldDepth) OldDepth->Release();
		Saved->Apply();
		Saved->Release();

		if (Next >= Queue.size()) Finish("done");
	}
}
