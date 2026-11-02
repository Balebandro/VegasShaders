// Game shader A/B (UNOFFICIAL P47). Compiles game shader templates from two Shaders folders the way the game does
// (ShaderRecord::LoadShader: D3DX43 preprocess, then D3DXCompileShader with flags 0), draws old and new over varied test
// scenes into a 32-bit float target on the GPU and requires every pixel to be bit-identical, checks that the scene has
// no NaNs and that the feature under test visibly changes it (so "identical" means something), then times both at
// 2560x1440 on full-screen draws.
//  - terrain: TerrainTemplate.hlsl, TEX_COUNT 1-7, several parallax settings (roadmap 3C).
//  - objects: ObjectTemplate.hlsl's multi-light pixel shaders SLS2029-2036 with 0-6 lights in use (roadmap 3J).
//  - skin: SKIN2000/2001/2002/2003/2006, the hand-written skin pixel shaders (Includes\Skin.hlsl): PerPixelDiffusion 0 identical, 1 changes (UNOFFICIAL).
//  - terrainblend (only when named): terrain blending in the NEW folder's TerrainTemplate (TERRAIN_BLEND), against the same
//    file compiled without it: compiles everywhere, about half dithered away 5 units deep in a 10-unit band, the debug view
//    never discards; GPU time off vs on (UNOFFICIAL).
// Usage: build\shader-test\game_shaders.exe <old Shaders folder> <new Shaders folder> [terrain|objects]
//        game_shaders.exe --asm-template <template file> <ps_3_0|vs_3_0> <output file> [NAME=VALUE ...]
// tools\test-game-shaders.ps1 extracts the old folder from git HEAD and runs it.
#define NOMINMAX
#include <windows.h>
#include <d3d9.h>
#include <d3dx9.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
using Microsoft::WRL::ComPtr;

static void Check(HRESULT hr, const char* what) {
	if (FAILED(hr)) { char text[200]; sprintf_s(text, "%s failed: %08lx", what, hr); throw std::runtime_error(text); }
}

static unsigned seed = 12345;
static float Random() { seed = seed * 1664525u + 1013904223u; return (seed >> 8) / 16777216.0f; }

typedef std::vector<std::pair<std::string, std::string>> Defines;

// Same fixed defines as ShaderRecord::LoadShader. FORWARD_SHADOWS 0 for the pixel tests (no shadow atlas here); the
// code under test does not depend on it.
static ComPtr<ID3DXBuffer> Compile(const std::string& file, const char* profile, const Defines& defines, int forwardShadows,
	unsigned* slots = nullptr, std::string* disassembly = nullptr) {
	std::vector<D3DXMACRO> macros;
	for (const auto& d : defines) macros.push_back({ d.first.c_str(), d.second.c_str() });
	// The fixed defines, as ShaderRecord::LoadShader sets them. A define the caller gave (NAME=VALUE on --asm-template, or
	// INTERIOR_SHADOWS / FORWARD_SHADOWS in a test) replaces the fixed one, as the game's AppendDefine does.
	auto fixed = [&macros, &defines](const char* name, const char* value) {
		for (const auto& d : defines) if (d.first == name) return;
		macros.push_back({ name, value });
	};
	fixed("REVERSED_DEPTH", "");
	fixed("FORWARD_SHADOWS", forwardShadows ? "1" : "0");
	fixed("SKYLIGHTING_MODE", "0");
	fixed("SHADOW_FIXED_MODE", "2");
	macros.push_back({ NULL, NULL });
	ComPtr<ID3DXBuffer> source, errors, code;
	HRESULT hr = D3DXPreprocessShaderFromFileA(file.c_str(), macros.data(), NULL, &source, &errors);
	if (FAILED(hr)) throw std::runtime_error("preprocess " + file + ": " + (errors ? (const char*)errors->GetBufferPointer() : ""));
	errors.Reset();
	hr = D3DXCompileShader((const char*)source->GetBufferPointer(), source->GetBufferSize(), NULL, NULL, "main", profile, 0,
		&code, &errors, NULL);
	if (FAILED(hr)) throw std::runtime_error("compile " + file + ": " + (errors ? (const char*)errors->GetBufferPointer() : ""));
	if (slots || disassembly) {
		ComPtr<ID3DXBuffer> text;
		Check(D3DXDisassembleShader((const DWORD*)code->GetBufferPointer(), FALSE, NULL, &text), "disassemble");
		if (disassembly) *disassembly = (const char*)text->GetBufferPointer();
		if (slots) {
			*slots = 0;
			const char* found = strstr((const char*)text->GetBufferPointer(), "approximately ");
			if (found) sscanf_s(found, "approximately %u", slots);
		}
	}
	return code;
}

struct Vertex { float v[11][4]; };  // POSITION0, then TEXCOORD0-9 as declared in the vertex shaders below

// Terrain: TerrainTemplate's PS_INPUT.
static const char* TerrainVertexShader = R"(
struct VIn { float4 pos : POSITION0; float4 uv : TEXCOORD0; float4 color : TEXCOORD1; float4 lpos : TEXCOORD2; float4 t : TEXCOORD3;
             float4 b : TEXCOORD4; float4 n : TEXCOORD5; float4 b0 : TEXCOORD6; float4 b1 : TEXCOORD7; float4 proj : TEXCOORD8;
             float4 view : TEXCOORD9; };
struct VOut { float4 blend_0 : COLOR0; float4 blend_1 : COLOR1; float4 sPosition : POSITION; float2 uv : TEXCOORD0;
              float3 vertex_color : TEXCOORD1; float3 lPosition : TEXCOORD2; float3 tangent : TEXCOORD3; float3 binormal : TEXCOORD4;
              float3 normal : TEXCOORD5; float4 projectionPosition : TEXCOORD6; float3 viewPosition : TEXCOORD7; };
VOut main(VIn IN) {
    VOut OUT;
    OUT.blend_0 = IN.b0; OUT.blend_1 = IN.b1; OUT.sPosition = IN.pos; OUT.uv = IN.uv.xy; OUT.vertex_color = IN.color.xyz;
    OUT.lPosition = IN.lpos.xyz; OUT.tangent = IN.t.xyz; OUT.binormal = IN.b.xyz; OUT.normal = IN.n.xyz;
    OUT.projectionPosition = IN.proj; OUT.viewPosition = IN.view.xyz;
    return OUT;
})";

// Objects: COLOR0/1 and TEXCOORD0-7 passed straight through; each variant reads the ones it declares.
static const char* ObjectVertexShader = R"(
struct VIn { float4 pos : POSITION0; float4 t0 : TEXCOORD0; float4 t1 : TEXCOORD1; float4 t2 : TEXCOORD2; float4 t3 : TEXCOORD3;
             float4 t4 : TEXCOORD4; float4 t5 : TEXCOORD5; float4 t6 : TEXCOORD6; float4 t7 : TEXCOORD7; float4 c0 : TEXCOORD8;
             float4 c1 : TEXCOORD9; };
struct VOut { float4 c0 : COLOR0; float4 c1 : COLOR1; float4 pos : POSITION; float4 t0 : TEXCOORD0; float4 t1 : TEXCOORD1;
              float4 t2 : TEXCOORD2; float4 t3 : TEXCOORD3; float4 t4 : TEXCOORD4; float4 t5 : TEXCOORD5; float4 t6 : TEXCOORD6;
              float4 t7 : TEXCOORD7; };
VOut main(VIn IN) {
    VOut OUT;
    OUT.c0 = IN.c0; OUT.c1 = IN.c1; OUT.pos = IN.pos; OUT.t0 = IN.t0; OUT.t1 = IN.t1; OUT.t2 = IN.t2; OUT.t3 = IN.t3;
    OUT.t4 = IN.t4; OUT.t5 = IN.t5; OUT.t6 = IN.t6; OUT.t7 = IN.t7;
    return OUT;
})";

struct Scene { std::vector<Vertex> vertices; std::vector<unsigned short> indices; };

static const int Grid = 65;

static void GridIndices(Scene& scene) {
	for (int j = 0; j + 1 < Grid; j++)
		for (int i = 0; i + 1 < Grid; i++) {
			const unsigned short a = (unsigned short)(j * Grid + i), b = a + 1, c = a + Grid, d = c + 1;
			scene.indices.insert(scene.indices.end(), { a, b, c, b, d, c });
		}
}

// A reversed-depth perspective projection like RenderManager's (row vectors, unit x/y scale): clip = (x, y, A*z + B, z).
static const float TestNear = 10.0f, TestFar = 300000.0f;
static const float TestProjectionA = -(TestNear / (TestFar - TestNear)), TestProjectionB = TestNear * TestFar / (TestFar - TestNear);

static void Set(Vertex& v, int k, float x, float y, float z, float w) { v.v[k][0] = x; v.v[k][1] = y; v.v[k][2] = z; v.v[k][3] = w; }

// Terrain: ground from 150 to about 2600 units away (parallax fades out at 2048), random blend weights (some exactly 0),
// gently bent normals.
static Scene TerrainScene(int texCount) {
	Scene scene;
	const float eye[3] = { 0, -100, 150 };
	for (int j = 0; j < Grid; j++)
		for (int i = 0; i < Grid; i++) {
			const float sx = i / float(Grid - 1), sy = j / float(Grid - 1);
			Vertex v = {};
			const float wx = (sx - 0.5f) * 3000, wy = 50 + (1 - sy) * 2600, wz = Random() * 20;
			const float dx = wx - eye[0], dy = wy - eye[1], dz = wz - eye[2];
			const float dist = sqrtf(dx * dx + dy * dy + dz * dz);
			Set(v, 0, sx * 2 - 1, 1 - sy * 2, 0.5f, 1);
			Set(v, 1, sx * 7 + Random() * 0.05f, sy * 7 + Random() * 0.05f, 0, 0);
			Set(v, 2, 0.6f + 0.4f * Random(), 0.6f + 0.4f * Random(), 0.6f + 0.4f * Random(), 1);
			Set(v, 3, wx, wy, wz, 1);
			Set(v, 4, 1, 0.2f * (Random() - 0.5f), 0.2f * (Random() - 0.5f), 0);
			Set(v, 5, 0.2f * (Random() - 0.5f), 1, 0.2f * (Random() - 0.5f), 0);
			Set(v, 6, 0.4f * (Random() - 0.5f), 0.4f * (Random() - 0.5f), 1, 0);
			float blends[8] = {}, total = 0;
			for (int t = 0; t < texCount; t++) { blends[t] = Random() < 0.3f ? 0.0f : Random(); total += blends[t]; }
			if (total == 0) { blends[0] = 1; total = 1; }
			for (int t = 0; t < texCount; t++) blends[t] /= total;
			Set(v, 7, blends[0], blends[1], blends[2], blends[3]);
			Set(v, 8, blends[4], blends[5], blends[6], 0);
			// Clip position of the view-space point (x, y, dist) under TestProjection (see CommonConstants), so the
			// inverse projection there takes it back exactly, as in the game.
			Set(v, 9, (sx * 2 - 1) * dist, (1 - sy * 2) * dist, TestProjectionA * dist + TestProjectionB, dist);
			Set(v, 10, eye[0], eye[1], eye[2], 1);
			scene.vertices.push_back(v);
		}
	GridIndices(scene);
	return scene;
}

static void RandomDirection(float out[3], float minLength, float maxLength) {
	float x = Random() - 0.5f, y = Random() - 0.5f, z = Random() * 0.8f + 0.2f;
	const float length = sqrtf(x * x + y * y + z * z), scale = (minLength + (maxLength - minLength) * Random()) / length;
	out[0] = x * scale; out[1] = y * scale; out[2] = z * scale;
}

// Objects (ObjectTemplate's LIGHTS >= 4 pixel shader): tangent-space light and view directions, object-space positions for
// the per-pixel attenuation, and the camera-relative world position in the interpolator the variant uses for it
// (TEXCOORD5 at MAX_LIGHTS 3, TEXCOORD6 at 4, the .w of TEXCOORD5-7 at 6), with the "present" sentinel 1.0.
static Scene ObjectScene(int maxLights) {
	Scene scene;
	for (int j = 0; j < Grid; j++)
		for (int i = 0; i < Grid; i++) {
			const float sx = i / float(Grid - 1), sy = j / float(Grid - 1);
			Vertex v = {};
			float dir[6][3], view[3];
			for (int k = 0; k < 6; k++) RandomDirection(dir[k], 0.7f, 1.3f);
			RandomDirection(view, 1.0f, 1.0f);
			const float world[3] = { (sx - 0.5f) * 400, 150 + 200 * sy + 20 * Random(), (0.5f - sy) * 150 + 10 * Random() };
			Set(v, 0, sx * 2 - 1, 1 - sy * 2, 0.5f, 1);
			Set(v, 1, sx * 5, sy * 5, 0, 0);                                                            // uv
			Set(v, 2, (sx - 0.5f) * 300, (sy - 0.5f) * 300, 30 * Random(), 1.0f);                       // lPosition (.w sentinel at MAX 6)
			Set(v, 3, dir[0][0], dir[0][1], dir[0][2], view[0]);                                        // lightDir, view.x
			Set(v, 4, dir[1][0], dir[1][1], dir[1][2], view[1]);                                        // light2, view.y
			Set(v, 5, dir[2][0], dir[2][1], dir[2][2], view[2]);                                        // light3, view.z
			if (maxLights == 3) {
				Set(v, 6, world[0], world[1], world[2], 1.0f);                                          // shadowWorldPos
			} else if (maxLights == 4) {
				Set(v, 6, dir[3][0], dir[3][1], dir[3][2], 0.5f);                                       // light4
				Set(v, 7, world[0], world[1], world[2], 1.0f);                                          // shadowWorldPos
			} else {
				Set(v, 6, dir[3][0], dir[3][1], dir[3][2], world[0]);                                   // light4, world.x
				Set(v, 7, dir[4][0], dir[4][1], dir[4][2], world[1]);                                   // light5, world.y
				Set(v, 8, dir[5][0], dir[5][1], dir[5][2], world[2]);                                   // light6, world.z
			}
			Set(v, 9, 0.6f + 0.4f * Random(), 0.6f + 0.4f * Random(), 0.6f + 0.4f * Random(), 1);    // vertex colour
			Set(v, 10, 0.5f, 0.55f, 0.6f, 0.1f + 0.3f * Random());                                     // fog colour, amount
			scene.vertices.push_back(v);
		}
	GridIndices(scene);
	return scene;
}

// 512x512 maps with a full mip chain. Colour maps: random colour, smooth heights in alpha. Normal maps: bent normals,
// random gloss in alpha.
static ComPtr<IDirect3DTexture9> MakeTexture(IDirect3DDevice9* device, int index, bool normalMap) {
	const UINT size = 512;
	ComPtr<IDirect3DTexture9> texture;
	Check(device->CreateTexture(size, size, 0, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &texture, NULL), "CreateTexture");
	D3DLOCKED_RECT lock;
	Check(texture->LockRect(0, &lock, NULL, 0), "LockRect");
	const float f1 = 3.0f + index, f2 = 5.0f + 2 * index;
	for (UINT y = 0; y < size; y++) {
		DWORD* row = (DWORD*)((BYTE*)lock.pBits + y * lock.Pitch);
		for (UINT x = 0; x < size; x++) {
			const float u = x / float(size) * 6.2831853f, w = y / float(size) * 6.2831853f;
			const float h = 0.5f + 0.3f * sinf(u * f1) * cosf(w * f2) + 0.2f * (Random() - 0.5f);
			auto byte = [](float value) { return (DWORD)std::min(255.0f, std::max(0.0f, value * 255.0f + 0.5f)); };
			const float r = normalMap ? 0.5f + 0.3f * (Random() - 0.5f) : Random(), g = normalMap ? 0.5f + 0.3f * (Random() - 0.5f) : Random();
			const float b = normalMap ? 0.9f + 0.1f * Random() : Random();
			row[x] = (byte(h) << 24) | (byte(r) << 16) | (byte(g) << 8) | byte(b);
		}
	}
	Check(texture->UnlockRect(0), "UnlockRect");
	Check(D3DXFilterTexture(texture.Get(), NULL, 0, D3DX_FILTER_BOX), "D3DXFilterTexture");
	return texture;
}

struct Constants {
	std::vector<float> c = std::vector<float>(224 * 4, 0.0f);
	void Set(int reg, float x, float y, float z, float w) { c[reg * 4] = x; c[reg * 4 + 1] = y; c[reg * 4 + 2] = z; c[reg * 4 + 3] = w; }
	void Apply(IDirect3DDevice9* device) { Check(device->SetPixelShaderConstantF(0, c.data(), 224), "SetPixelShaderConstantF"); }
};

static void CommonConstants(Constants& k) {
	// Shadow.hlsl: inverse of TestProjection (clip (X, Y, Z, W) -> view (X, Y, W, (Z - A*W) / B)), identity inverse view.
	// A real camera, so GetShadowWorldPos gets back the scene's view-space points as it does in the game.
	k.Set(100, 1, 0, 0, 0);
	k.Set(101, 0, 1, 0, 0);
	k.Set(102, 0, 0, 0, 1.0f / TestProjectionB);
	k.Set(103, 0, 0, 1, -TestProjectionA / TestProjectionB);
	for (int r = 0; r < 4; r++) { k.Set(104 + r, 0, 0, 0, 0); k.c[(104 + r) * 4 + r] = 1; }
	for (int i = 0; i < 9; i++) k.Set(137 + i, 0.3f / (i + 1), 0.25f / (i + 1), 0.35f / (i + 1), 0);  // sky irradiance
	k.Set(136, 0, 0, 0, 0);                                                    // UNOFFICIAL TESR_SharpSky (c136): off, the plain sky
}

// waterline: TESR_TerrainParallaxExtraData.w, the camera-relative height below which CheapUnderwaterTerrain treats the
// ground as under water (0 here = none of the test scene, whose camera-relative height is about +1).
static Constants TerrainConstants(const float parallax[4], float waterline = 0) {
	Constants k;
	CommonConstants(k);
	k.Set(1, 0.2f, 0.2f, 0.25f, 1);                        // AmbientColor
	k.Set(3, 1.0f, 0.9f, 0.8f, 1);                         // SunColor
	k.Set(18, 0.39f, 0.52f, 0.76f, 0);                     // SunDir
	k.Set(32, 1, 0, 2, 0.5f); k.Set(33, 0, 1, 3, 0);       // LandSpec
	k.Set(34, 1, 1, 0, 1); k.Set(35, 1, 0, 1, 0);          // LandHeight: some textures without height maps
	k.Set(36, 6000, 5000, 1, 0); k.Set(37, 0.5f, 0.6f, 0.7f, 1);  // fog
	k.Set(89, 0.2f, 0.8f, 1, 1);                           // TESR_TerrainData: metal, rough, light scale, ambient scale
	k.Set(90, 1, 1, 0.7f, 2.7f);                           // TESR_TerrainExtraData: PBR on (parallax shadows only matter with PBR)
	k.Set(91, parallax[0], parallax[1], parallax[2], parallax[3]);  // TESR_TerrainParallaxData
	k.Set(92, 2048, 0.1f, 2.0f, waterline);                // TESR_TerrainParallaxExtraData: max distance, height, shadow intensity, waterline
	k.Set(135, 1.0f, 0.5f, 0, 0);                          // TESR_TerrainSkyData
	return k;
}

// lightsUsed goes where the variant reads it: EmittanceColor.a (c2) without OPT, PSLightColor[0].a (c3) with OPT.
static Constants ObjectConstants(float lightsUsed, bool opt) {
	Constants k;
	CommonConstants(k);
	unsigned saved = seed;
	seed = 777;  // the same lights for every call
	k.Set(1, 0.15f, 0.15f, 0.18f, 1);                      // AmbientColor (.a 1: no alpha test)
	k.Set(2, 0.2f, 0.1f, 0.05f, opt ? 3.0f : lightsUsed);  // EmittanceColor
	for (int i = 0; i < 10; i++) k.Set(3 + i, 0.3f + 0.7f * Random(), 0.3f + 0.7f * Random(), 0.3f + 0.7f * Random(), 0);  // PSLightColor
	if (opt) k.c[3 * 4 + 3] = lightsUsed;
	k.c[4 * 4 + 3] = 20;                                   // PSLightColor[1].w: gloss power with OPT
	for (int i = 0; i < 8; i++)                            // PSLightPosition: object space, radius
		k.Set(19 + i, (Random() - 0.5f) * 400, (Random() - 0.5f) * 400, 40 + 60 * Random(), 150 + 300 * Random());
	k.Set(27, 1, 1, 20, 0.5f);                             // Toggles: vertex colour, fog, gloss power, alpha ref
	k.Set(32, 0, 1, 1, 1);                                 // TESR_PBRData: -, roughness scale, light scale, ambient scale
	k.Set(33, 1, 1, 0.5f, 0);                              // TESR_PBRExtraData: saturation, skylight scale, directionality
	seed = saved;
	return k;
}

struct Gpu {
	ComPtr<IDirect3DDevice9> device;
	ComPtr<IDirect3DVertexShader9> terrainVS, objectVS;
	ComPtr<IDirect3DSurface9> target, readback, big;
	std::vector<ComPtr<IDirect3DTexture9>> textures;
	static const UINT Size = 1024;
};

static ComPtr<IDirect3DVertexShader9> MakeVertexShader(IDirect3DDevice9* device, const char* source) {
	ComPtr<ID3DXBuffer> code, errors;
	Check(D3DXCompileShader(source, (UINT)strlen(source), NULL, NULL, "main", "vs_3_0", 0, &code, &errors, NULL), "compile test vertex shader");
	ComPtr<IDirect3DVertexShader9> shader;
	Check(device->CreateVertexShader((const DWORD*)code->GetBufferPointer(), &shader), "CreateVertexShader");
	return shader;
}

static void CreateGpu(Gpu& gpu) {
	HWND window = CreateWindowExA(0, "STATIC", "NVR game shader test", WS_OVERLAPPED, 0, 0, 128, 128, NULL, NULL, GetModuleHandle(NULL), NULL);
	if (!window) throw std::runtime_error("window creation failed");
	ComPtr<IDirect3D9> d3d; d3d.Attach(Direct3DCreate9(D3D_SDK_VERSION));
	if (!d3d) throw std::runtime_error("D3D9 unavailable");
	D3DADAPTER_IDENTIFIER9 adapter = {}; Check(d3d->GetAdapterIdentifier(0, 0, &adapter), "GetAdapterIdentifier");
	printf("GPU: %s\n", adapter.Description);
	D3DDISPLAYMODE display = {}; Check(d3d->GetAdapterDisplayMode(D3DADAPTER_DEFAULT, &display), "GetAdapterDisplayMode");
	D3DPRESENT_PARAMETERS pp = {}; pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.hDeviceWindow = window;
	pp.BackBufferWidth = 128; pp.BackBufferHeight = 128; pp.BackBufferFormat = display.Format;
	pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
	Check(d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &gpu.device), "CreateDevice");
	IDirect3DDevice9* device = gpu.device.Get();
	gpu.terrainVS = MakeVertexShader(device, TerrainVertexShader);
	gpu.objectVS = MakeVertexShader(device, ObjectVertexShader);
	std::vector<D3DVERTEXELEMENT9> elements;
	elements.push_back({ 0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 });
	for (BYTE k = 0; k < 10; k++) elements.push_back({ 0, (WORD)(16 * (k + 1)), D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, k });
	elements.push_back(D3DDECL_END());
	ComPtr<IDirect3DVertexDeclaration9> declaration;
	Check(device->CreateVertexDeclaration(elements.data(), &declaration), "CreateVertexDeclaration");
	Check(device->SetVertexDeclaration(declaration.Get()), "SetVertexDeclaration");
	Check(device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE), "cull");
	Check(device->SetRenderState(D3DRS_ZENABLE, FALSE), "z");
	Check(device->SetRenderState(D3DRS_LIGHTING, FALSE), "lighting");
	for (int k = 0; k < 14; k++) {
		gpu.textures.push_back(MakeTexture(device, k % 7, k >= 7));
		device->SetSamplerState(k, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
		device->SetSamplerState(k, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
		device->SetSamplerState(k, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
		device->SetSamplerState(k, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
		device->SetSamplerState(k, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
	}
	Check(device->CreateRenderTarget(Gpu::Size, Gpu::Size, D3DFMT_A32B32G32R32F, D3DMULTISAMPLE_NONE, 0, FALSE, &gpu.target, NULL), "float target");
	Check(device->CreateOffscreenPlainSurface(Gpu::Size, Gpu::Size, D3DFMT_A32B32G32R32F, D3DPOOL_SYSTEMMEM, &gpu.readback, NULL), "readback");
	Check(device->CreateRenderTarget(2560, 1440, D3DFMT_A16B16G16R16F, D3DMULTISAMPLE_NONE, 0, FALSE, &gpu.big, NULL), "timing target");
}

// Terrain: colour maps s0-s6, normal maps s7-s13. Objects: colour map s0, normal map s1.
static void BindTextures(Gpu& gpu, bool terrain) {
	for (int k = 0; k < 14; k++) {
		IDirect3DTexture9* texture = terrain ? gpu.textures[k].Get() : k == 0 ? gpu.textures[0].Get() : k == 1 ? gpu.textures[7].Get() : nullptr;
		Check(gpu.device->SetTexture(k, texture), "SetTexture");
	}
}

static IDirect3DPixelShader9* CreatePS(IDirect3DDevice9* device, ID3DXBuffer* code, ComPtr<IDirect3DPixelShader9>& holder) {
	Check(device->CreatePixelShader((const DWORD*)code->GetBufferPointer(), &holder), "CreatePixelShader");
	return holder.Get();
}

static void Draw(IDirect3DDevice9* device, IDirect3DPixelShader9* shader, const Scene& scene) {
	Check(device->SetPixelShader(shader), "SetPixelShader");
	Check(device->DrawIndexedPrimitiveUP(D3DPT_TRIANGLELIST, 0, (UINT)scene.vertices.size(), (UINT)scene.indices.size() / 3,
		scene.indices.data(), D3DFMT_INDEX16, scene.vertices.data(), sizeof(Vertex)), "DrawIndexedPrimitiveUP");
}

static std::vector<float> Render(Gpu& gpu, IDirect3DPixelShader9* shader, const Scene& scene) {
	IDirect3DDevice9* device = gpu.device.Get();
	Check(device->SetRenderTarget(0, gpu.target.Get()), "SetRenderTarget");
	D3DVIEWPORT9 viewport = { 0, 0, Gpu::Size, Gpu::Size, 0, 1 };
	Check(device->SetViewport(&viewport), "SetViewport");
	Check(device->Clear(0, NULL, D3DCLEAR_TARGET, 0, 1, 0), "Clear");
	Check(device->BeginScene(), "BeginScene");
	Draw(device, shader, scene);
	Check(device->EndScene(), "EndScene");
	Check(device->GetRenderTargetData(gpu.target.Get(), gpu.readback.Get()), "GetRenderTargetData");
	D3DLOCKED_RECT lock;
	Check(gpu.readback->LockRect(&lock, NULL, D3DLOCK_READONLY), "LockRect readback");
	std::vector<float> pixels(Gpu::Size * Gpu::Size * 4);
	for (UINT y = 0; y < Gpu::Size; y++) memcpy(&pixels[y * Gpu::Size * 4], (BYTE*)lock.pBits + y * lock.Pitch, Gpu::Size * 16);
	gpu.readback->UnlockRect();
	return pixels;
}

// A file in the folder of this exe (build\shader-test), wherever it is run from.
static std::string NextToExe(const char* name) {
	char path[MAX_PATH];
	GetModuleFileNameA(NULL, path, MAX_PATH);
	std::string folder(path);
	return folder.substr(0, folder.find_last_of('\\') + 1) + name;
}

struct Comparison { size_t different = 0, nan = 0; double worst = 0; };

static Comparison Compare(const std::vector<float>& a, const std::vector<float>& b) {
	Comparison result;
	for (size_t p = 0; p < a.size(); p += 4) {
		if (memcmp(&a[p], &b[p], 16) != 0) {
			result.different++;
			for (int k = 0; k < 4; k++) result.worst = std::max(result.worst, (double)fabsf(a[p + k] - b[p + k]));
		}
		for (int k = 0; k < 4; k++) if (a[p + k] != a[p + k] || b[p + k] != b[p + k]) { result.nan++; break; }
	}
	return result;
}

static double ChangedShare(const std::vector<float>& a, const std::vector<float>& b) {
	size_t changed = 0;
	for (size_t p = 0; p < a.size(); p += 4)
		if (fabsf(a[p] - b[p]) + fabsf(a[p + 1] - b[p + 1]) + fabsf(a[p + 2] - b[p + 2]) > 1e-3f) changed++;
	return changed / double(a.size() / 4);
}

static double TimeDraws(Gpu& gpu, IDirect3DPixelShader9* shader, const Scene& scene) {
	IDirect3DDevice9* device = gpu.device.Get();
	ComPtr<IDirect3DQuery9> start, end, frequency, disjoint;
	Check(device->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &start), "timestamp query");
	Check(device->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &end), "timestamp query");
	Check(device->CreateQuery(D3DQUERYTYPE_TIMESTAMPFREQ, &frequency), "frequency query");
	Check(device->CreateQuery(D3DQUERYTYPE_TIMESTAMPDISJOINT, &disjoint), "disjoint query");
	Check(device->SetRenderTarget(0, gpu.big.Get()), "SetRenderTarget big");
	D3DVIEWPORT9 viewport = { 0, 0, 2560, 1440, 0, 1 };
	Check(device->SetViewport(&viewport), "SetViewport big");
	std::vector<double> samples;
	for (int run = 0; run < 25; run++) {
		Check(device->BeginScene(), "BeginScene");
		disjoint->Issue(D3DISSUE_BEGIN);
		start->Issue(D3DISSUE_END);
		for (int k = 0; k < 4; k++) Draw(device, shader, scene);
		end->Issue(D3DISSUE_END);
		disjoint->Issue(D3DISSUE_END);
		frequency->Issue(D3DISSUE_END);
		Check(device->EndScene(), "EndScene");
		UINT64 t0 = 0, t1 = 0, hz = 0; BOOL broken = TRUE;
		while (start->GetData(&t0, sizeof(t0), D3DGETDATA_FLUSH) == S_FALSE) {}
		while (end->GetData(&t1, sizeof(t1), D3DGETDATA_FLUSH) == S_FALSE) {}
		while (frequency->GetData(&hz, sizeof(hz), D3DGETDATA_FLUSH) == S_FALSE) {}
		while (disjoint->GetData(&broken, sizeof(broken), D3DGETDATA_FLUSH) == S_FALSE) {}
		if (run >= 5 && !broken && hz) samples.push_back((t1 - t0) * 1000.0 / hz / 4);
	}
	if (samples.empty()) return -1;
	std::sort(samples.begin(), samples.end());
	return samples[samples.size() / 2];
}

// Old and new alternated five times after a warm-up, so GPU clock changes hit both alike. Each round is already a
// median; the fastest round is the least disturbed one, so that is what is compared (with the slowest for spread).
static void TimeOldNew(Gpu& gpu, const char* label, IDirect3DPixelShader9* oldShader, IDirect3DPixelShader9* newShader, const Scene& scene) {
	TimeDraws(gpu, oldShader, scene);
	TimeDraws(gpu, newShader, scene);
	std::vector<double> oldMs, newMs;
	for (int round = 0; round < 5; round++) {
		oldMs.push_back(TimeDraws(gpu, oldShader, scene));
		newMs.push_back(TimeDraws(gpu, newShader, scene));
	}
	std::sort(oldMs.begin(), oldMs.end());
	std::sort(newMs.begin(), newMs.end());
	printf("  %-44s old %.3f ms (to %.3f), new %.3f ms (to %.3f), %+.1f%%\n", label, oldMs[0], oldMs[4],
		newMs[0], newMs[4], 100.0 * (newMs[0] - oldMs[0]) / oldMs[0]);
}

static int TestTerrain(Gpu& gpu, const std::string& oldFolder, const std::string& newFolder) {
	int failures = 0;
	const std::string oldFile = oldFolder + "\\TerrainTemplate.hlsl", newFile = newFolder + "\\TerrainTemplate.hlsl";
	auto defines = [](int texCount, const char* lights) {
		Defines d = { { "PS", "" }, { "TEX_COUNT", std::to_string(texCount) } };
		if (lights) d.push_back({ "NUM_PT_LIGHTS", lights });
		return d;
	};
	std::puts("TERRAIN instruction slots (static count; the parallax loop body counts once), with forward shadows:");
	for (int texCount = 1; texCount <= 7; texCount++) {
		unsigned oldSlots, newSlots;
		Compile(oldFile, "ps_3_0", defines(texCount, NULL), 1, &oldSlots);
		Compile(newFile, "ps_3_0", defines(texCount, NULL), 1, &newSlots);
		Compile(newFile, "ps_3_0", defines(texCount, "24"), 1);  // the point-light variants must compile too
		printf("  TEX_COUNT %d: old %u, new %u\n", texCount, oldSlots, newSlots);
	}
	struct Setting { const char* name; float parallax[4]; };
	const Setting settings[] = {
		{ "defaults (parallax, shadows, height blend, HQ)", { 1, 1, 1, 1 } },
		{ "8 steps", { 1, 1, 1, 0 } },
		{ "no height blend", { 1, 1, 0, 1 } },
		{ "no parallax shadows", { 1, 0, 1, 1 } },
		{ "parallax off", { 0, 1, 1, 1 } },
	};
	IDirect3DDevice9* device = gpu.device.Get();
	Check(device->SetVertexShader(gpu.terrainVS.Get()), "SetVertexShader");
	BindTextures(gpu, true);
	std::puts("TERRAIN pixels, 1024x1024 float target, every pixel must be bit-identical:");
	for (int texCount = 1; texCount <= 7; texCount++) {
		ComPtr<IDirect3DPixelShader9> oldHolder, newHolder;
		IDirect3DPixelShader9* oldShader = CreatePS(device, Compile(oldFile, "ps_3_0", defines(texCount, NULL), 0).Get(), oldHolder);
		IDirect3DPixelShader9* newShader = CreatePS(device, Compile(newFile, "ps_3_0", defines(texCount, NULL), 0).Get(), newHolder);
		const Scene scene = TerrainScene(texCount);
		std::vector<float> reference[5];
		for (int s = 0; s < 5; s++) {
			TerrainConstants(settings[s].parallax).Apply(device);
			reference[s] = Render(gpu, oldShader, scene);
			const Comparison result = Compare(reference[s], Render(gpu, newShader, scene));
			printf("  TEX_COUNT %d, %-44s %s", texCount, settings[s].name, result.different ? "DIFFERENT" : "identical");
			if (result.different) printf(" (%zu pixels, largest difference %.3g)", result.different, result.worst);
			printf("\n");
			if (result.different) failures++;
			if (result.nan) { printf("FAIL: %zu NaN pixels\n", result.nan); failures++; }
		}
		const double parallax = ChangedShare(reference[0], reference[4]), shadows = ChangedShare(reference[0], reference[3]);
		printf("  TEX_COUNT %d: parallax changes %.0f%% of the pixels, parallax shadows %.0f%%\n", texCount, 100 * parallax, 100 * shadows);
		if (parallax < 0.25 || shadows < 0.1) { std::puts("FAIL: the test scene does not exercise parallax enough"); failures++; }

		// CheapUnderwaterTerrain with the whole scene under water must equal parallax and parallax shadows switched off.
		const float off[4] = { 0, 0, 1, 1 }, on[4] = { 1, 1, 1, 1 };
		TerrainConstants(off).Apply(device);
		// Both renders with the new shader: the switch is checked against the same code, not against old-vs-new changes.
		const std::vector<float> flat = Render(gpu, newShader, scene);
		TerrainConstants(on, 1e30f).Apply(device);
		const Comparison under = Compare(flat, Render(gpu, newShader, scene));
		printf("  TEX_COUNT %d, all under water (CheapUnderwaterTerrain) = parallax off: %s", texCount, under.different ? "DIFFERENT" : "identical");
		if (under.different) printf(" (%zu pixels, largest difference %.3g)", under.different, under.worst);
		printf("\n");
		if (under.different) failures++;
	}
	std::puts("TERRAIN GPU time per full-screen draw at 2560x1440 (A16B16G16R16F), game defaults:");
	TerrainConstants(settings[0].parallax).Apply(device);
	for (int texCount = 1; texCount <= 7; texCount += 2) {
		ComPtr<IDirect3DPixelShader9> oldHolder, newHolder;
		IDirect3DPixelShader9* oldShader = CreatePS(device, Compile(oldFile, "ps_3_0", defines(texCount, NULL), 1).Get(), oldHolder);
		IDirect3DPixelShader9* newShader = CreatePS(device, Compile(newFile, "ps_3_0", defines(texCount, NULL), 1).Get(), newHolder);
		const std::string label = "TEX_COUNT " + std::to_string(texCount);
		TimeOldNew(gpu, label.c_str(), oldShader, newShader, TerrainScene(texCount));
	}
	std::puts("TERRAIN GPU time, new shader, full screen: normal vs everything under water with CheapUnderwaterTerrain:");
	for (int texCount = 1; texCount <= 7; texCount += 2) {
		ComPtr<IDirect3DPixelShader9> holder;
		IDirect3DPixelShader9* shader = CreatePS(device, Compile(newFile, "ps_3_0", defines(texCount, NULL), 1).Get(), holder);
		const Scene scene = TerrainScene(texCount);
		double normal = 1e9, under = 1e9;
		TerrainConstants(settings[0].parallax).Apply(device);
		TimeDraws(gpu, shader, scene);
		for (int round = 0; round < 5; round++) {
			TerrainConstants(settings[0].parallax).Apply(device);
			normal = std::min(normal, TimeDraws(gpu, shader, scene));
			TerrainConstants(settings[0].parallax, 1e30f).Apply(device);
			under = std::min(under, TimeDraws(gpu, shader, scene));
		}
		printf("  TEX_COUNT %d: %.3f ms -> %.3f ms under water (%+.0f%%)\n", texCount, normal, under, 100 * (under - normal) / normal);
	}

	// ParallaxLite on the terrain (ReducedQuality, lossy by design): the new shader with TESR_TerrainParallaxData.w = 2 against
	// its own defaults (w = 1) and 8 steps (w = 0). Times alternate like TimeOldNew; the picture difference is reported
	// and written as PNGs (build\shader-test\parallax-*.png) for a look.
	std::puts("TERRAIN parallax lite, new shader: GPU time full screen 2560x1440 and difference from the defaults:");
	const float full[4] = { 1, 1, 1, 1 }, eight[4] = { 1, 1, 1, 0 }, lite[4] = { 1, 1, 1, 2 };
	for (int texCount = 1; texCount <= 7; texCount += 2) {
		ComPtr<IDirect3DPixelShader9> holder;
		IDirect3DPixelShader9* shader = CreatePS(device, Compile(newFile, "ps_3_0", defines(texCount, NULL), 0).Get(), holder);
		const Scene scene = TerrainScene(texCount);
		TerrainConstants(full).Apply(device);
		const std::vector<float> reference = Render(gpu, shader, scene);
		if (texCount == 7) D3DXSaveSurfaceToFileA(NextToExe("parallax-full.png").c_str(), D3DXIFF_PNG, gpu.target.Get(), NULL, NULL);
		TerrainConstants(eight).Apply(device);
		const std::vector<float> eightSteps = Render(gpu, shader, scene);
		TerrainConstants(lite).Apply(device);
		const std::vector<float> lighter = Render(gpu, shader, scene);
		if (texCount == 7) D3DXSaveSurfaceToFileA(NextToExe("parallax-lite.png").c_str(), D3DXIFF_PNG, gpu.target.Get(), NULL, NULL);
		auto difference = [&](const std::vector<float>& other, double& mean, double& share) {
			double sum = 0; size_t visible = 0;
			for (size_t p = 0; p < reference.size(); p += 4) {
				double d = 0;
				for (int k = 0; k < 3; k++) d = std::max(d, (double)fabsf(std::min(reference[p + k], 1.0f) - std::min(other[p + k], 1.0f)));
				sum += d;
				if (d > 2.0 / 255) visible++;
			}
			mean = 255 * sum / (reference.size() / 4); share = 100.0 * visible / (reference.size() / 4);
		};
		double eightMean, eightShare, liteMean, liteShare;
		difference(eightSteps, eightMean, eightShare);
		difference(lighter, liteMean, liteShare);
		double fullMs = 1e9, eightMs = 1e9, liteMs = 1e9;
		TerrainConstants(full).Apply(device); TimeDraws(gpu, shader, scene);
		for (int round = 0; round < 5; round++) {
			TerrainConstants(full).Apply(device); fullMs = std::min(fullMs, TimeDraws(gpu, shader, scene));
			TerrainConstants(eight).Apply(device); eightMs = std::min(eightMs, TimeDraws(gpu, shader, scene));
			TerrainConstants(lite).Apply(device); liteMs = std::min(liteMs, TimeDraws(gpu, shader, scene));
		}
		printf("  TEX_COUNT %d: defaults %.3f ms, 8 steps %.3f ms (%+.0f%%), lite %.3f ms (%+.0f%%); difference from the defaults "
			"(mean /255, %% of pixels over 2/255): 8 steps %.2f, %.1f%%; lite %.2f, %.1f%%\n", texCount, fullMs, eightMs,
			100 * (eightMs - fullMs) / fullMs, liteMs, 100 * (liteMs - fullMs) / fullMs, eightMean, eightShare, liteMean, liteShare);
	}
	return failures;
}

static int TestObjects(Gpu& gpu, const std::string& oldFolder, const std::string& newFolder) {
	int failures = 0;
	const std::string oldFile = oldFolder + "\\ObjectTemplate.hlsl", newFile = newFolder + "\\ObjectTemplate.hlsl";
	struct Variant { const char* name; Defines defines; int maxLights; bool opt; };
	const Variant variants[] = {  // src/effects/PBR.h; MAX_LIGHTS as in ObjectTemplate.hlsl
		{ "SLS2029 (LIGHTS 9)", { { "PS", "" }, { "LIGHTS", "9" } }, 6, false },
		{ "SLS2030 (LIGHTS 9, SI)", { { "PS", "" }, { "LIGHTS", "9" }, { "SI", "" } }, 6, false },
		{ "SLS2031 (LIGHTS 4)", { { "PS", "" }, { "LIGHTS", "4" } }, 4, false },
		{ "SLS2032 (LIGHTS 4, OPT)", { { "PS", "" }, { "LIGHTS", "4" }, { "OPT", "" } }, 4, true },
		{ "SLS2033 (LIGHTS 4, SI)", { { "PS", "" }, { "LIGHTS", "4" }, { "SI", "" } }, 4, false },
		{ "SLS2034 (LIGHTS 4, SPECULAR)", { { "PS", "" }, { "LIGHTS", "4" }, { "SPECULAR", "" } }, 3, false },
		{ "SLS2035 (LIGHTS 4, SPECULAR, OPT)", { { "PS", "" }, { "LIGHTS", "4" }, { "SPECULAR", "" }, { "OPT", "" } }, 3, true },
		{ "SLS2036 (LIGHTS 4, SPECULAR, SI)", { { "PS", "" }, { "LIGHTS", "4" }, { "SPECULAR", "" }, { "SI", "" } }, 3, false },
	};
	std::puts("OBJECTS instruction slots, with forward shadows:");
	for (const Variant& v : variants) {
		unsigned oldSlots, newSlots;
		Compile(oldFile, "ps_3_0", v.defines, 1, &oldSlots);
		Compile(newFile, "ps_3_0", v.defines, 1, &newSlots);
		printf("  %-36s old %u, new %u\n", v.name, oldSlots, newSlots);
	}
	IDirect3DDevice9* device = gpu.device.Get();
	Check(device->SetVertexShader(gpu.objectVS.Get()), "SetVertexShader");
	BindTextures(gpu, false);
	std::puts("OBJECTS pixels with 0-6 lights in use, 1024x1024 float target, every pixel must be bit-identical:");
	for (const Variant& v : variants) {
		ComPtr<IDirect3DPixelShader9> oldHolder, newHolder;
		IDirect3DPixelShader9* oldShader = CreatePS(device, Compile(oldFile, "ps_3_0", v.defines, 0).Get(), oldHolder);
		IDirect3DPixelShader9* newShader = CreatePS(device, Compile(newFile, "ps_3_0", v.defines, 0).Get(), newHolder);
		const Scene scene = ObjectScene(v.maxLights);
		std::vector<float> fewest, most;
		std::string summary;
		for (int used = 0; used <= 6; used++) {
			ObjectConstants((float)used, v.opt).Apply(device);
			std::vector<float> a = Render(gpu, oldShader, scene);
			const Comparison result = Compare(a, Render(gpu, newShader, scene));
			summary += result.different ? " DIFFERENT" : " same";
			if (result.different) {
				printf("  %s, %d lights: %zu pixels differ, largest difference %.3g\n", v.name, used, result.different, result.worst);
				failures++;
			}
			if (result.nan) { printf("FAIL: %s, %d lights: %zu NaN pixels\n", v.name, used, result.nan); failures++; }
			if (used == 0) fewest = a;
			if (used == 6) most = a;
		}
		const double lights = ChangedShare(fewest, most);
		printf("  %-36s 0-6 lights:%s; the extra lights change %.0f%% of the pixels\n", v.name, summary.c_str(), 100 * lights);
		if (lights < 0.5) { std::puts("FAIL: the extra lights barely change the test scene"); failures++; }
	}
	std::puts("OBJECTS GPU time per full-screen draw at 2560x1440 (A16B16G16R16F), with forward shadows compiled in:");
	for (const Variant& v : variants) {
		ComPtr<IDirect3DPixelShader9> oldHolder, newHolder;
		IDirect3DPixelShader9* oldShader = CreatePS(device, Compile(oldFile, "ps_3_0", v.defines, 1).Get(), oldHolder);
		IDirect3DPixelShader9* newShader = CreatePS(device, Compile(newFile, "ps_3_0", v.defines, 1).Get(), newHolder);
		const Scene scene = ObjectScene(v.maxLights);
		for (int used : { 2, v.maxLights }) {
			ObjectConstants((float)used, v.opt).Apply(device);
			const std::string label = std::string(v.name) + ", " + std::to_string(used) + " lights";
			TimeOldNew(gpu, label.c_str(), oldShader, newShader, scene);
		}
	}
	return failures;
}

// ---- Metal (UNOFFICIAL): ObjectTemplate's LIGHTS < 4 pixel shaders, SLS2000-2027, and the LIGHTS >= 4 ones, SLS2029-2036
// (objects lit by four or more lamps at once; metal there since 2026-10-08) ----
// Their PS_INPUT: TEXCOORD0 uv (zw barrel), 1-3 the lights (.w radius; lightDir.w world up's y for the metal), 4 the
// camera-relative world position (.w sentinel), 5 squared light distances (.w up's x), 6 the view (.w barrel), 7 projected
// shadow, COLOR0 vertex colour (.a up's z, encoded), COLOR1 fog.
static Scene SmallObjectScene() {
	Scene scene;
	for (int j = 0; j < Grid; j++)
		for (int i = 0; i < Grid; i++) {
			const float sx = i / float(Grid - 1), sy = j / float(Grid - 1);
			Vertex v = {};
			float dir[3][3], view[3], up[3];
			for (int k = 0; k < 3; k++) RandomDirection(dir[k], 0.7f, 1.3f);
			RandomDirection(view, 1.0f, 1.0f);
			RandomDirection(up, 1.0f, 1.0f);
			if (Random() < 0.4f) up[2] = -up[2];      // some surfaces face down
			const float world[3] = { (sx - 0.5f) * 400, 150 + 200 * sy + 20 * Random(), (0.5f - sy) * 150 + 10 * Random() };
			Set(v, 0, sx * 2 - 1, 1 - sy * 2, 0.5f, 1);
			Set(v, 1, sx * 5, sy * 5, (sx - 0.5f) * 30, (sy - 0.5f) * 30);                           // uv, barrel local xy
			Set(v, 2, dir[0][0], dir[0][1], dir[0][2], up[1]);                                      // sun, up.y
			Set(v, 3, dir[1][0] * 100, dir[1][1] * 100, dir[1][2] * 100, 300);                      // light 2, radius
			Set(v, 4, dir[2][0] * 100, dir[2][1] * 100, dir[2][2] * 100, 300);                      // light 3, radius
			Set(v, 5, world[0], world[1], world[2], 1.0f);                                          // shadowWorldPos
			Set(v, 6, 10000 * Random(), 20000 * Random(), 20000 * Random(), up[0]);                 // lightDistSq, up.x
			Set(v, 7, view[0] * 200, view[1] * 200, view[2] * 200, 5);                              // viewDir, barrel z
			Set(v, 8, 0.5f, 0.5f, 0.5f, 0.5f);                                                       // shadowUVs
			Set(v, 9, 0.6f + 0.4f * Random(), 0.6f + 0.4f * Random(), 0.6f + 0.4f * Random(), up[2] * 0.5f + 0.5f);  // vertex colour, up.z
			Set(v, 10, 0.5f, 0.55f, 0.6f, 0.1f + 0.3f * Random());                                  // fog colour, amount
			scene.vertices.push_back(v);
		}
	GridIndices(scene);
	return scene;
}

static int TestMetal(Gpu& gpu, const std::string& oldFolder, const std::string& newFolder) {
	int failures = 0;
	const std::string oldFile = oldFolder + "\\ObjectTemplate.hlsl", newFile = newFolder + "\\ObjectTemplate.hlsl";
	// metal: has the metal code (SPECULAR, not HAIR); maxLights: 0 for the LIGHTS < 4 shaders, else the LIGHTS >= 4 shader's
	// MAX_LIGHTS (its scene, ObjectScene, and that many lights in use)
	struct Variant { const char* name; Defines defines; bool opt, metal; int maxLights; };
	const Variant variants[] = {
		{ "SLS2017 (SPECULAR)", { { "PS", "" }, { "SPECULAR", "" } }, false, true },
		{ "SLS2018 (SPECULAR, SI)", { { "PS", "" }, { "SPECULAR", "" }, { "SI", "" } }, false, true },
		{ "SLS2020 (SPECULAR, PROJ_SHADOW)", { { "PS", "" }, { "SPECULAR", "" }, { "PROJ_SHADOW", "" } }, false, true },
		{ "SLS2023 (SPECULAR, LIGHTS 2)", { { "PS", "" }, { "SPECULAR", "" }, { "LIGHTS", "2" } }, false, true },
		{ "SLS2000 (no specular)", { { "PS", "" } }, false, false },
		{ "SLS2001 (OPT)", { { "PS", "" }, { "OPT", "" } }, true, false },
		{ "SLS2019 (SPECULAR, HAIR)", { { "PS", "" }, { "SPECULAR", "" }, { "HAIR", "" } }, false, false },
		{ "SLS2034 (LIGHTS 4, SPECULAR)", { { "PS", "" }, { "LIGHTS", "4" }, { "SPECULAR", "" } }, false, true, 3 },
		{ "SLS2035 (LIGHTS 4, SPECULAR, OPT)", { { "PS", "" }, { "LIGHTS", "4" }, { "SPECULAR", "" }, { "OPT", "" } }, true, true, 3 },
		{ "SLS2036 (LIGHTS 4, SPECULAR, SI)", { { "PS", "" }, { "LIGHTS", "4" }, { "SPECULAR", "" }, { "SI", "" } }, false, true, 3 },
		{ "SLS2031 (LIGHTS 4)", { { "PS", "" }, { "LIGHTS", "4" } }, false, false, 4 },
		{ "SLS2029 (LIGHTS 9)", { { "PS", "" }, { "LIGHTS", "9" } }, false, false, 6 },
	};
	IDirect3DDevice9* device = gpu.device.Get();
	Check(device->SetVertexShader(gpu.objectVS.Get()), "SetVertexShader");
	BindTextures(gpu, false);
	const Scene smallScene = SmallObjectScene();
	std::puts("METAL (ObjectTemplate): metal off must be bit-identical to the old shader; metal on must change the scene, no NaNs:");
	for (const Variant& v : variants) {
		const Scene scene = v.maxLights ? ObjectScene(v.maxLights) : smallScene;
		ComPtr<IDirect3DPixelShader9> oldHolder, newHolder;
		IDirect3DPixelShader9* oldShader = CreatePS(device, Compile(oldFile, "ps_3_0", v.defines, 0).Get(), oldHolder);
		IDirect3DPixelShader9* newShader = CreatePS(device, Compile(newFile, "ps_3_0", v.defines, 0).Get(), newHolder);
		Constants k = ObjectConstants(v.maxLights ? (float)v.maxLights : 1.0f, v.opt);
		k.Set(34, 0, 0.6f, 0.25f, 0.55f);       // TESR_PBRMetal: off
		k.Set(36, 0.5f, 0.25f, 1, 0);           // TESR_PBRMetalLook
		k.Apply(device);
		const std::vector<float> a = Render(gpu, oldShader, scene), off = Render(gpu, newShader, scene);
		const Comparison same = Compare(a, off);
		k.Set(34, 1, 0.6f, 0.25f, 0.55f);       // on
		k.Apply(device);
		const std::vector<float> on = Render(gpu, newShader, scene);
		const Comparison changed = Compare(a, on);
		double before = 0, after = 0; size_t n = 0;
		for (size_t p = 0; p < a.size(); p += 4)
			if (fabsf(a[p] - on[p]) + fabsf(a[p + 1] - on[p + 1]) + fabsf(a[p + 2] - on[p + 2]) > 1e-3f) {
				before += a[p] + a[p + 1] + a[p + 2]; after += on[p] + on[p + 1] + on[p + 2]; n++;
			}
		printf("  %-30s off: %s (%zu pixels differ, worst %.3g) | on: %.0f%% of pixels change, their brightness %.3f -> %.3f, NaN %zu\n",
			v.name, same.different ? "DIFFERENT" : "identical", same.different, same.worst, 100.0 * ChangedShare(a, on),
			n ? before / (3 * n) : 0.0, n ? after / (3 * n) : 0.0, changed.nan);
		if (same.different) failures++;
		if (same.nan || changed.nan) { printf("FAIL: NaN pixels\n"); failures++; }
		if (v.metal && ChangedShare(a, on) < 0.05) { printf("FAIL: metal on barely changes the test scene\n"); failures++; }
		if (!v.metal && changed.different) { printf("FAIL: metal on changes a variant without the metal code\n"); failures++; }
		// UNOFFICIAL SharpSky (SkyAmbient.hlsl): the metal's sky along the reflected direction, sharper. Off (c136 0, above) is the
		// plain sky. On changes the metal of the variants with a sky term (LIGHTS < 4; the LIGHTS >= 4 ones pass none) and nothing
		// else, and makes no NaNs.
		k.Set(136, 1, 0, 0, 0);
		k.Apply(device);
		const std::vector<float> sharpened = Render(gpu, newShader, scene);
		k.Set(136, 0, 0, 0, 0);
		k.Apply(device);
		const Comparison sharp = Compare(on, sharpened);
		printf("  %-30s SharpSky on: %.0f%% of pixels change, NaN %zu\n", v.name, 100.0 * ChangedShare(on, sharpened), sharp.nan);
		if (sharp.nan) { printf("FAIL: NaN pixels with SharpSky on\n"); failures++; }
		if (v.metal && !v.maxLights && ChangedShare(on, sharpened) < 0.05) { printf("FAIL: SharpSky on barely changes the metal\n"); failures++; }
		if ((!v.metal || v.maxLights) && sharp.different) { printf("FAIL: SharpSky changes a variant with no sky term in its metal\n"); failures++; }
		if (v.metal) {
			// The distance fade (TESR_PBRExtraData.w, Object.hlsl getMetalFade; the scene is 150-450 units away): wholly short
			// of it, metal on as without the fade; wholly beyond it, metal off (to rounding: the metal code with no metal
			// rounds a little differently from the code without it, about 1e-4, a fortieth of an 8-bit step); in between,
			// part of the way.
			k.Set(33, 1, 1, 0.5f, 10000); k.Apply(device);
			const Comparison notYet = Compare(on, Render(gpu, newShader, scene));
			k.Set(33, 1, 1, 0.5f, 100); k.Apply(device);
			const Comparison fadedOut = Compare(off, Render(gpu, newShader, scene));
			k.Set(33, 1, 1, 0.5f, 500); k.Apply(device);
			const std::vector<float> partway = Render(gpu, newShader, scene);
			k.Set(33, 1, 1, 0.5f, 0); k.Apply(device);
			printf("  %-30s fade: short of it worst %.3g vs no fade | beyond it worst %.3g vs metal off | part way %.0f%% of pixels change\n",
				v.name, notYet.worst, fadedOut.worst, 100.0 * ChangedShare(a, partway));
			if (notYet.worst > 1e-4f || fadedOut.worst > 1e-3f || notYet.nan || fadedOut.nan) { printf("FAIL: the metal's distance fade\n"); failures++; }
		}
		k.Set(34, 0, 0.6f, 0.25f, 0.55f);
		k.Apply(device);
		const std::string labelOff = std::string(v.name) + ", metal off";
		TimeOldNew(gpu, labelOff.c_str(), oldShader, newShader, scene);
		k.Set(34, 1, 0.6f, 0.25f, 0.55f);
		k.Apply(device);
		const std::string labelOn = std::string(v.name) + ", metal on";
		TimeOldNew(gpu, labelOn.c_str(), oldShader, newShader, scene);
	}
	return failures;
}

// ---- Material rules (UNOFFICIAL): ObjectTemplate compiled with MATERIAL_RULES_ON (Object.hlsl, c197-c199) ----
// With the rules compiled in, an object without a rule (c197.x 0) must look exactly as the old shader, metal off and on; a
// rule must change the lit variants (roughness for every one; metal for the SPECULAR ones), and nothing may make NaNs.
static int TestMaterialRules(Gpu& gpu, const std::string& oldFolder, const std::string& newFolder) {
	int failures = 0;
	const std::string oldFile = oldFolder + "\\ObjectTemplate.hlsl", newFile = newFolder + "\\ObjectTemplate.hlsl";
	struct Variant { const char* name; Defines defines; bool opt, metal; int maxLights; };
	const Variant variants[] = {
		{ "SLS2017 (SPECULAR)", { { "PS", "" }, { "SPECULAR", "" } }, false, true },
		{ "SLS2020 (SPECULAR, PROJ_SHADOW)", { { "PS", "" }, { "SPECULAR", "" }, { "PROJ_SHADOW", "" } }, false, true },
		{ "SLS2000 (no specular)", { { "PS", "" } }, false, false },
		{ "SLS2001 (OPT)", { { "PS", "" }, { "OPT", "" } }, true, false },
		{ "SLS2034 (LIGHTS 4, SPECULAR)", { { "PS", "" }, { "LIGHTS", "4" }, { "SPECULAR", "" } }, false, true, 3 },
		{ "SLS2035 (LIGHTS 4, SPECULAR, OPT)", { { "PS", "" }, { "LIGHTS", "4" }, { "SPECULAR", "" }, { "OPT", "" } }, true, true, 3 },
	};
	IDirect3DDevice9* device = gpu.device.Get();
	Check(device->SetVertexShader(gpu.objectVS.Get()), "SetVertexShader");
	BindTextures(gpu, false);
	const Scene smallScene = SmallObjectScene();
	std::puts("MATERIAL RULES (ObjectTemplate, MATERIAL_RULES_ON 1): no rule must be bit-identical to the old shader; rules change it, no NaNs:");
	for (const Variant& v : variants) {
		const Scene scene = v.maxLights ? ObjectScene(v.maxLights) : smallScene;
		Defines rulesOn = v.defines;
		rulesOn.push_back({ "MATERIAL_RULES_ON", "1" });
		ComPtr<IDirect3DPixelShader9> oldHolder, newHolder;
		IDirect3DPixelShader9* oldShader = CreatePS(device, Compile(oldFile, "ps_3_0", v.defines, 0).Get(), oldHolder);
		IDirect3DPixelShader9* newShader = CreatePS(device, Compile(newFile, "ps_3_0", rulesOn, 0).Get(), newHolder);
		Constants k = ObjectConstants(v.maxLights ? (float)v.maxLights : 1.0f, v.opt);
		k.Set(36, 0.5f, 0.25f, 1, 0);
		k.Set(197, 0, 0, 0, 1); k.Set(198, -1, -1, 0, 0); k.Set(199, 0, 0, 0, 0);	// no rule (PBR.h defaults)
		size_t worst = 0;
		for (int metal = 0; metal < 2; metal++) {
			k.Set(34, (float)metal, 0.6f, 0.25f, 0.55f);
			k.Apply(device);
			const Comparison same = Compare(Render(gpu, oldShader, scene), Render(gpu, newShader, scene));
			worst = (std::max)(worst, same.different);
		}
		k.Set(34, 0, 0.6f, 0.25f, 0.55f);
		k.Apply(device);
		const std::vector<float> base = Render(gpu, newShader, scene);
		k.Set(197, 1, 0, 0, 1); k.Set(198, 0.75f, 1.0f, 0, 0); k.Apply(device);		// "rock": never metal, rough
		const std::vector<float> rough = Render(gpu, newShader, scene);
		k.Set(197, 1, 1, 2, 1); k.Set(198, 0.05f, 0.2f, 0, 0); k.Apply(device);		// "chrome": all metal, polished
		const std::vector<float> chrome = Render(gpu, newShader, scene);
		k.Set(197, 1, 0, 0, 1); k.Set(198, -1, -1, 1, 0); k.Set(199, 1, 0, 0, 0); k.Apply(device);	// DebugView
		const std::vector<float> debug = Render(gpu, newShader, scene);
		k.Set(197, 0, 0, 0, 1); k.Set(198, -1, -1, 0, 0); k.Set(199, 0, 0, 0, 0); k.Apply(device);
		const Comparison r = Compare(base, rough), c = Compare(base, chrome), d = Compare(base, debug);
		printf("  %-34s no rule: %s (%zu pixels differ) | rough rule %.0f%% | chrome rule %.0f%% | DebugView %.0f%% of pixels change, NaN %zu\n",
			v.name, worst ? "DIFFERENT" : "identical", worst, 100.0 * ChangedShare(base, rough), 100.0 * ChangedShare(base, chrome),
			100.0 * ChangedShare(base, debug), r.nan + c.nan + d.nan);
		if (worst) failures++;
		if (r.nan || c.nan || d.nan) { printf("FAIL: NaN pixels\n"); failures++; }
		if (ChangedShare(base, debug) < 0.5) { printf("FAIL: DebugView does not tint the picture\n"); failures++; }
		if (v.metal && ChangedShare(base, chrome) < 0.05) { printf("FAIL: the chrome rule barely changes a metal variant\n"); failures++; }
		if (v.metal && ChangedShare(base, rough) < 0.05) { printf("FAIL: the rough rule barely changes a lit variant\n"); failures++; }
		TimeOldNew(gpu, (std::string(v.name) + ", rules compiled in, no rule").c_str(), oldShader, newShader, scene);
	}
	return failures;
}

// ---- Environment maps (UNOFFICIAL): EnvTemplate.hlsl against the game's own shaders ----
// The game's SLS2050/2051 (VS) and SLS2057/2058 (PS) bytecode, from its shader package, against ours: the pixel shaders
// with the light scaling off must give the game's picture (to rounding: the game's run at partial precision); the vertex
// shaders must give the same outputs, read back one at a time by a probe pixel shader.
static std::vector<DWORD> LoadBytecode(const std::string& path) {
	FILE* f = nullptr;
	if (fopen_s(&f, path.c_str(), "rb") || !f) throw std::runtime_error("cannot open " + path);
	fseek(f, 0, SEEK_END); const long size = ftell(f); fseek(f, 0, SEEK_SET);
	std::vector<DWORD> code((size + 3) / 4);
	fread(code.data(), 1, size, f); fclose(f);
	return code;
}

static ComPtr<ID3DXBuffer> CompileSource(const std::string& file, const char* profile, const Defines& defines) {
	std::vector<D3DXMACRO> macros;
	for (const auto& d : defines) macros.push_back({ d.first.c_str(), d.second.c_str() });
	macros.push_back({ "SKYLIGHTING_MODE", "0" });
	macros.push_back({ NULL, NULL });
	ComPtr<ID3DXBuffer> source, errors, code;
	if (FAILED(D3DXPreprocessShaderFromFileA(file.c_str(), macros.data(), NULL, &source, &errors)))
		throw std::runtime_error("preprocess " + file + ": " + (errors ? (const char*)errors->GetBufferPointer() : ""));
	errors.Reset();
	if (FAILED(D3DXCompileShader((const char*)source->GetBufferPointer(), source->GetBufferSize(), NULL, NULL, "main", profile, 0, &code, &errors, NULL)))
		throw std::runtime_error("compile " + file + ": " + (errors ? (const char*)errors->GetBufferPointer() : ""));
	return code;
}

static ComPtr<ID3DXBuffer> CompileText(const char* source, const char* profile) {
	ComPtr<ID3DXBuffer> code, errors;
	if (FAILED(D3DXCompileShader(source, (UINT)strlen(source), NULL, NULL, "main", profile, 0, &code, &errors, NULL)))
		throw std::runtime_error(std::string("compile probe: ") + (errors ? (const char*)errors->GetBufferPointer() : ""));
	return code;
}

static void Normalize3(float v[3]) { const float l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); for (int k = 0; k < 3; k++) v[k] /= l; }
static void Cross3(const float a[3], const float b[3], float out[3]) { out[0] = a[1] * b[2] - a[2] * b[1]; out[1] = a[2] * b[0] - a[0] * b[2]; out[2] = a[0] * b[1] - a[1] * b[0]; }

// What the game's VS hands the env PS: uv, the cube-space frame (tangent and binormal x0.1, normal) with the view in .w,
// the fog's visibility, the vertex colour.
static Scene EnvPixelScene() {
	Scene scene;
	for (int j = 0; j < Grid; j++)
		for (int i = 0; i < Grid; i++) {
			const float sx = i / float(Grid - 1), sy = j / float(Grid - 1);
			Vertex v = {};
			float n[3], a[3], tng[3], bin[3], view[3];
			RandomDirection(n, 1, 1); if (Random() < 0.5f) n[2] = -n[2];
			RandomDirection(a, 1, 1);
			Cross3(n, a, tng); Normalize3(tng); Cross3(n, tng, bin);
			RandomDirection(view, 1, 1);
			Set(v, 0, sx * 2 - 1, 1 - sy * 2, 0.5f, 1);
			Set(v, 1, sx * 4, sy * 4, 0, 0);
			Set(v, 2, tng[0] * 0.1f, bin[0] * 0.1f, n[0], view[0]);
			Set(v, 3, tng[1] * 0.1f, bin[1] * 0.1f, n[1], view[1]);
			Set(v, 4, tng[2] * 0.1f, bin[2] * 0.1f, n[2], view[2]);
			Set(v, 5, 0.6f + 0.4f * Random(), 300, 0, 0);                                   // fog, distance to the camera
			Set(v, 9, 0.5f + 0.5f * Random(), 0.5f + 0.5f * Random(), 0.5f + 0.5f * Random(), 1);
			scene.vertices.push_back(v);
		}
	GridIndices(scene);
	return scene;
}

// A mesh for the vertex shaders: object-space positions on screen, random frames, bone weights and indices (0-17, so
// every bone's three rows fit in Bones[54]), as the game's vertex layout (slots below).
static Scene EnvVertexScene() {
	Scene scene;
	for (int j = 0; j < Grid; j++)
		for (int i = 0; i < Grid; i++) {
			const float sx = i / float(Grid - 1), sy = j / float(Grid - 1);
			Vertex v = {};
			float n[3], a[3], tng[3], bin[3];
			RandomDirection(n, 1, 1);
			RandomDirection(a, 1, 1);
			Cross3(n, a, tng); Normalize3(tng); Cross3(n, tng, bin);
			const float w0 = 0.5f * Random(), w1 = 0.3f * Random(), w2 = 0.2f * Random();
			Set(v, 0, (sx * 2 - 1) * 0.8f, (1 - sy * 2) * 0.8f, 0.3f * Random(), 1);    // POSITION
			Set(v, 1, sx * 4, sy * 4, 0, 0);                                               // TEXCOORD0
			Set(v, 2, n[0], n[1], n[2], 0);                                                // NORMAL
			Set(v, 3, tng[0], tng[1], tng[2], 0);                                          // TANGENT
			Set(v, 4, bin[0], bin[1], bin[2], 0);                                          // BINORMAL
			Set(v, 5, Random(), Random(), Random(), 1);                                    // COLOR0
			Set(v, 6, w0, w1, w2, 0);                                                      // BLENDWEIGHT
			Set(v, 7, (float)(rand() % 18) / 255, (float)(rand() % 18) / 255, (float)(rand() % 18) / 255, (float)(rand() % 18) / 255);  // BLENDINDICES
			scene.vertices.push_back(v);
		}
	GridIndices(scene);
	return scene;
}

static const char* PassThroughVS = R"(
struct VIn { float4 pos : POSITION0; float4 t0 : TEXCOORD0; float4 t1 : TEXCOORD1; float4 t2 : TEXCOORD2; float4 t3 : TEXCOORD3;
             float4 t4 : TEXCOORD4; float4 c0 : TEXCOORD8; };
struct VOut { float4 pos : POSITION; float4 t0 : TEXCOORD0; float4 t1 : TEXCOORD1; float4 t2 : TEXCOORD2; float4 t3 : TEXCOORD3;
              float4 t4 : TEXCOORD4; float4 c0 : COLOR0; };
VOut main(VIn IN) { VOut OUT; OUT.pos = IN.pos; OUT.t0 = IN.t0; OUT.t1 = IN.t1; OUT.t2 = IN.t2; OUT.t3 = IN.t3; OUT.t4 = IN.t4; OUT.c0 = IN.c0; return OUT; })";

static double MeanLuma(const std::vector<float>& a) {
	double s = 0; for (size_t p = 0; p < a.size(); p += 4) s += 0.2126 * a[p] + 0.7152 * a[p + 1] + 0.0722 * a[p + 2];
	return s / (a.size() / 4);
}

// The environment-map test cube: a bright sky, dark ground, coloured sides.
static ComPtr<IDirect3DCubeTexture9> MakeEnvCube(IDirect3DDevice9* device) {
	ComPtr<IDirect3DCubeTexture9> cube;
	Check(device->CreateCubeTexture(64, 0, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &cube, NULL), "CreateCubeTexture");
	for (int face = 0; face < 6; face++) {
		D3DLOCKED_RECT lock;
		Check(cube->LockRect((D3DCUBEMAP_FACES)face, 0, &lock, NULL, 0), "lock cube");
		for (int y = 0; y < 64; y++)
			for (int x = 0; x < 64; x++) {
				const int r = 40 + face * 35 + x, g = 60 + y * 2, b = 200 - face * 25;
				((DWORD*)((BYTE*)lock.pBits + y * lock.Pitch))[x] = D3DCOLOR_ARGB(255, r & 255, g & 255, b & 255);
			}
		cube->UnlockRect((D3DCUBEMAP_FACES)face, 0);
	}
	Check(D3DXFilterTexture(cube.Get(), NULL, 0, D3DX_FILTER_BOX), "filter cube");
	return cube;
}

static int TestEnv(Gpu& gpu, const std::string& newFolder, const std::string& vanillaFolder) {
	int failures = 0;
	IDirect3DDevice9* device = gpu.device.Get();
	const std::string file = newFolder + "\\EnvTemplate.hlsl";
	// The cube map: a bright sky, dark ground, coloured sides.
	ComPtr<IDirect3DCubeTexture9> cube = MakeEnvCube(device);
	for (int k = 0; k < 16; k++) device->SetTexture(k, nullptr);
	device->SetTexture(0, gpu.textures[7].Get());   // NormalMap (alpha: the mask)
	device->SetTexture(1, cube.Get());              // EnvironmentCubeMap
	device->SetTexture(3, gpu.textures[1].Get());   // CustomEnvMask

	// ---- pixel shaders ----
	ComPtr<IDirect3DVertexShader9> vs2, vs3;
	auto vs2code = CompileText(PassThroughVS, "vs_2_0"), vs3code = CompileText(PassThroughVS, "vs_3_0");
	Check(device->CreateVertexShader((const DWORD*)vs2code->GetBufferPointer(), &vs2), "vs2");
	Check(device->CreateVertexShader((const DWORD*)vs3code->GetBufferPointer(), &vs3), "vs3");
	const Scene pixelScene = EnvPixelScene();
	std::puts("ENV MAPS pixel shaders (EnvTemplate.hlsl vs the game's), 1024x1024 float target:");
	struct PsVariant { const char* name; const char* game; Defines defines; };
	const PsVariant psVariants[] = { { "SLS2057", "SLS2057.pso", { { "PS", "" } } }, { "SLS2058", "SLS2058.pso", { { "PS", "" }, { "NEGATE_VIEW", "" } } } };
	for (const PsVariant& v : psVariants) {
		ComPtr<IDirect3DPixelShader9> game, ours;
		const std::vector<DWORD> gameCode = LoadBytecode(vanillaFolder + "\\" + v.game);
		Check(device->CreatePixelShader(gameCode.data(), &game), "game PS");
		Check(device->CreatePixelShader((const DWORD*)CompileSource(file, "ps_3_0", v.defines)->GetBufferPointer(), &ours), "our PS");
		Constants k;
		CommonConstants(k);
		k.Set(1, 0.30f, 0.32f, 0.36f, 0.9f);     // AmbientColor
		k.Set(27, 1, 0, 1.5f, 0.35f);            // EnvToggles: vertex colour, -, strength, custom mask share
		k.Set(32, 0, 1, 1, 0.5f);                // TESR_PBRData: ambient scale
		k.Set(33, 1, 0.5f, 0.5f, 0);             // TESR_PBRExtraData: skylight scale
		k.Set(34, 0, 0.6f, 0.25f, 0.55f);        // TESR_PBRMetal: off
		k.Set(35, 0, 1.5f, 0.5f, 0);             // TESR_PBREnv: Lighting 0
		k.Apply(device);
		Check(device->SetVertexShader(vs2.Get()), "set vs2");
		const std::vector<float> a = Render(gpu, game.Get(), pixelScene);
		Check(device->SetVertexShader(vs3.Get()), "set vs3");
		const std::vector<float> b = Render(gpu, ours.Get(), pixelScene);
		const Comparison off = Compare(a, b);
		k.Set(35, 1, 1.5f, 0.5f, 0); k.Apply(device);
		const std::vector<float> lit = Render(gpu, ours.Get(), pixelScene);
		k.Set(34, 1, 0.6f, 0.25f, 0.55f); k.Apply(device);
		const std::vector<float> metal = Render(gpu, ours.Get(), pixelScene);
		printf("  %s: Lighting 0 vs the game's: %zu pixels differ, worst %.4g (mean %.4f vs %.4f) | Lighting 1: mean %.4f | metal on: mean %.4f (x%.2f) | NaN %zu\n",
			v.name, off.different, off.worst, MeanLuma(a), MeanLuma(b), MeanLuma(lit), MeanLuma(metal), MeanLuma(metal) / std::max(MeanLuma(lit), 1e-9),
			Compare(a, lit).nan + Compare(a, metal).nan);
		if (off.worst > 0.01) { printf("FAIL: %s differs from the game's with the light scaling off\n", v.name); failures++; }
		if (MeanLuma(a) < 0.01) { printf("FAIL: the test scene shows no reflection\n"); failures++; }
		if (Compare(a, lit).nan || Compare(a, metal).nan || off.nan) { printf("FAIL: NaN pixels\n"); failures++; }
		// The metal's distance fade (TESR_PBRExtraData.w; the scene is 300 units away): faded out, the reflection is as
		// without metal; well short of it, as with metal and no fade.
		k.Set(33, 1, 0.5f, 0.5f, 100); k.Apply(device);
		const Comparison fadedOut = Compare(lit, Render(gpu, ours.Get(), pixelScene));
		k.Set(33, 1, 0.5f, 0.5f, 10000); k.Apply(device);
		const Comparison notYet = Compare(metal, Render(gpu, ours.Get(), pixelScene));
		printf("  %s metal fade: beyond it worst %.3g vs no metal | short of it worst %.3g vs no fade\n", v.name, fadedOut.worst, notYet.worst);
		if (fadedOut.worst > 1e-4f || notYet.worst > 1e-4f) { printf("FAIL: the metal's distance fade\n"); failures++; }
	}

	// ---- vertex shaders ----
	std::vector<D3DVERTEXELEMENT9> elements = {
		{ 0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
		{ 0, 16, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 },
		{ 0, 32, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_NORMAL, 0 },
		{ 0, 48, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TANGENT, 0 },
		{ 0, 64, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_BINORMAL, 0 },
		{ 0, 80, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0 },
		{ 0, 96, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_BLENDWEIGHT, 0 },
		{ 0, 112, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_BLENDINDICES, 0 },
		D3DDECL_END() };
	ComPtr<IDirect3DVertexDeclaration9> gameLayout, harnessLayout;
	Check(device->GetVertexDeclaration(&harnessLayout), "get declaration");
	Check(device->CreateVertexDeclaration(elements.data(), &gameLayout), "game declaration");
	Check(device->SetVertexDeclaration(gameLayout.Get()), "set game declaration");
	const Scene vertexScene = EnvVertexScene();
	// Constants: projection-free on-screen transform, cube space a rotation plus a shift, fog, bones near identity.
	std::vector<float> vc(256 * 4, 0.0f);
	auto setv = [&](int r, float x, float y, float z, float w) { vc[r * 4] = x; vc[r * 4 + 1] = y; vc[r * 4 + 2] = z; vc[r * 4 + 3] = w; };
	const float rot[3][4] = { { 0.8f, -0.6f, 0.0f, 30.0f }, { 0.6f, 0.8f, 0.0f, -20.0f }, { 0.0f, 0.0f, 1.0f, 50.0f } };
	auto setTransforms = [&](int mvp, int cubeSpace) {
		setv(mvp + 0, 1, 0, 0, 0); setv(mvp + 1, 0, 1, 0, 0); setv(mvp + 2, 0, 0, 0.5f, 0.25f); setv(mvp + 3, 0, 0, 0, 1);
		for (int r = 0; r < 3; r++) setv(cubeSpace + r, rot[r][0], rot[r][1], rot[r][2], rot[r][3]);
	};
	setv(14, 1.2f, 2.0f, 1.5f, 0);   // FogParam: start, range, power (screen-space lengths here)
	for (int bone = 0; bone < 18; bone++) {
		const float a = 0.05f * bone, c = cosf(a), s = sinf(a);
		setv(44 + bone * 3 + 0, c, -s, 0, 0.01f * bone);
		setv(44 + bone * 3 + 1, s, c, 0, -0.01f * bone);
		setv(44 + bone * 3 + 2, 0, 0, 1, 0.02f);
	}
	const char* probe = R"(
struct PIn { float4 t0 : TEXCOORD0; float4 t1 : TEXCOORD1; float4 t2 : TEXCOORD2; float4 t3 : TEXCOORD3; float4 t4 : TEXCOORD4; float4 c0 : COLOR0; };
float4 Which : register(c0);
float4 main(PIn IN) : COLOR0 {
    return Which.x > 4.5 ? float4(IN.c0.rgb, 0) : Which.x > 3.5 ? float4(IN.t0.xy, IN.t4.x, 0) : Which.x > 2.5 ? IN.t3 : Which.x > 1.5 ? IN.t2 : IN.t1;
})";
	ComPtr<IDirect3DPixelShader9> probe2, probe3;
	Check(device->CreatePixelShader((const DWORD*)CompileText(probe, "ps_2_0")->GetBufferPointer(), &probe2), "probe2");
	Check(device->CreatePixelShader((const DWORD*)CompileText(probe, "ps_3_0")->GetBufferPointer(), &probe3), "probe3");
	const char* outputs[5] = { "frame x / view x", "frame y / view y", "frame z / view z", "uv, fog", "vertex colour" };
	std::puts("ENV MAPS vertex shaders (EnvTemplate.hlsl vs the game's), outputs read back through a probe:");
	struct VsVariant { const char* name; const char* game; Defines defines; bool skin; };
	const VsVariant vsVariants[] = { { "SLS2050", "SLS2050.vso", { { "VS", "" } }, false }, { "SLS2051 (skin)", "SLS2051.vso", { { "VS", "" }, { "SKIN", "" } }, true } };
	for (const VsVariant& v : vsVariants) {
		ComPtr<IDirect3DVertexShader9> game, ours;
		const std::vector<DWORD> gameCode = LoadBytecode(vanillaFolder + "\\" + v.game);
		Check(device->CreateVertexShader(gameCode.data(), &game), "game VS");
		Check(device->CreateVertexShader((const DWORD*)CompileSource(file, "vs_3_0", v.defines)->GetBufferPointer(), &ours), "our VS");
		std::fill(vc.begin(), vc.begin() + 44 * 4, 0.0f);
		setv(14, 1.2f, 2.0f, 1.5f, 0);
		if (v.skin) setTransforms(1, 9); else setTransforms(0, 8);
		Check(device->SetVertexShaderConstantF(0, vc.data(), 256), "VS constants");
		std::string line;
		double worstAll = 0;
		for (int o = 0; o < 5; o++) {
			const float which[4] = { (float)o + 1, 0, 0, 0 };
			Check(device->SetPixelShaderConstantF(0, which, 1), "which");
			Check(device->SetVertexShader(game.Get()), "game VS");
			const std::vector<float> a = Render(gpu, probe2.Get(), vertexScene);
			Check(device->SetVertexShader(ours.Get()), "our VS");
			const std::vector<float> b = Render(gpu, probe3.Get(), vertexScene);
			const Comparison c = Compare(a, b);
			char part[160]; sprintf_s(part, " | %s: worst %.3g", outputs[o], c.worst);
			line += part;
			worstAll = std::max(worstAll, c.worst);
			if (o == 3 && MeanLuma(a) == 0) { printf("FAIL: %s drew nothing\n", v.name); failures++; }
		}
		printf("  %s%s\n", v.name, line.c_str());
		if (worstAll > 0.01) { printf("FAIL: %s outputs differ from the game's\n", v.name); failures++; }
	}
	Check(device->SetVertexDeclaration(harnessLayout.Get()), "restore declaration");
	return failures;
}

// ---- Water preview (P48) ----
// Renders WATER000.pso (outdoor water, close range) over a test lake seen from 250 units above the surface: a sky to
// reflect, a sandy bottom to refract, the water getting deeper from left to right. Three pictures stacked in one BMP:
// the classic style, the enhanced style, and the enhanced style a quarter second later (to see the waves move).
static const char* WaterVertexShader = R"(
struct VIn { float4 pos : POSITION0; float4 t0 : TEXCOORD0; float4 t1 : TEXCOORD1; float4 t2 : TEXCOORD2; float4 t3 : TEXCOORD3;
             float4 t4 : TEXCOORD4; float4 t5 : TEXCOORD5; float4 t6 : TEXCOORD6; float4 t7 : TEXCOORD7; float4 t8 : TEXCOORD8; };
struct VOut { float4 pos : POSITION; float4 t0 : TEXCOORD0; float4 t1 : TEXCOORD1; float4 t2 : TEXCOORD2; float4 t3 : TEXCOORD3;
              float4 t4 : TEXCOORD4; float4 t5 : TEXCOORD5; float4 t6 : TEXCOORD6; float4 t7 : TEXCOORD7; float4 t8 : TEXCOORD8; };
VOut main(VIn IN) {
    VOut OUT;
    OUT.pos = IN.pos; OUT.t0 = IN.t0; OUT.t1 = IN.t1; OUT.t2 = IN.t2; OUT.t3 = IN.t3; OUT.t4 = IN.t4;
    OUT.t5 = IN.t5; OUT.t6 = IN.t6; OUT.t7 = IN.t7; OUT.t8 = IN.t8;
    return OUT;
})";

static ComPtr<IDirect3DTexture9> ProceduralTexture(IDirect3DDevice9* device, UINT size, DWORD(*texel)(float u, float v)) {
	ComPtr<IDirect3DTexture9> texture;
	Check(device->CreateTexture(size, size, 0, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &texture, NULL), "CreateTexture");
	D3DLOCKED_RECT lock;
	Check(texture->LockRect(0, &lock, NULL, 0), "LockRect");
	for (UINT y = 0; y < size; y++)
		for (UINT x = 0; x < size; x++)
			((DWORD*)((BYTE*)lock.pBits + y * lock.Pitch))[x] = texel((x + 0.5f) / size, (y + 0.5f) / size);
	texture->UnlockRect(0);
	Check(D3DXFilterTexture(texture.Get(), NULL, 0, D3DX_FILTER_BOX), "D3DXFilterTexture");
	return texture;
}

static DWORD Rgb(float r, float g, float b, float a = 1) {
	auto c = [](float v) { return (DWORD)std::lround(std::min(1.0f, std::max(0.0f, v)) * 255); };
	return (c(a) << 24) | (c(r) << 16) | (c(g) << 8) | c(b);
}

// ---- Parallax objects (P61, ParallaxLite) ----
// ParallaxTemplate's pixel shader (rocks, cliffs, walls with a height map) through the objects' pass-through vertex
// shader: uv TEXCOORD0, sun direction TEXCOORD1, tangent-space view direction TEXCOORD7 with the distance in .w
// (grazing and far at the top of the screen, head-on and near at the bottom; parallax fades out at 2048).
static Scene ParallaxObjectScene() {
	Scene scene;
	for (int j = 0; j < Grid; j++)
		for (int i = 0; i < Grid; i++) {
			const float sx = i / float(Grid - 1), sy = j / float(Grid - 1);
			Vertex v = {};
			float light[3] = { 0.4f + 0.3f * (sx - 0.5f), 0.3f, 0.8f };
			float view[3] = { (sx - 0.5f) * 0.8f, 0.2f + 0.6f * (1 - sy), 0.15f + 0.85f * sy };
			for (float* d : { light, view }) {
				const float length = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
				for (int k = 0; k < 3; k++) d[k] /= length;
			}
			Set(v, 0, sx * 2 - 1, 1 - sy * 2, 0.5f, 1);
			Set(v, 1, sx * 6, sy * 6, 0, 0);                                  // uv
			Set(v, 2, light[0], light[1], light[2], 1);                        // lightDir
			Set(v, 8, view[0], view[1], view[2], 2600 - 2500 * sy);            // viewDir, distance
			Set(v, 9, 0.9f, 0.85f, 0.8f, 1);                                   // vertex colour
			Set(v, 10, 0.5f, 0.55f, 0.6f, 0);                                  // fog colour, no fog
			scene.vertices.push_back(v);
		}
	GridIndices(scene);
	return scene;
}

// Smooth heights in red, as a rock's height map.
static DWORD HeightTexel(float u, float v) {
	const float h = 0.5f + 0.3f * sinf(u * 25.1327f) * cosf(v * 31.4159f) + 0.15f * sinf((u + v) * 62.8318f);
	return Rgb(h, h, h, 1);
}

static Constants ParallaxObjectConstants(float heightScale, float lite) {
	Constants k = ObjectConstants(1, false);
	k.Set(35, heightScale, 1, 0, lite);                                    // TESR_ParallaxData: scale, PBR, -, lite
	k.Set(134, 0, 1, 1, 1);                                                // TESR_PBRData
	k.Set(135, 1, 1, 0.5f, 0);                                             // TESR_PBRExtraData
	return k;
}

static int TestParallaxObjects(Gpu& gpu, const std::string& oldFolder, const std::string& newFolder) {
	int failures = 0;
	const std::string oldFile = oldFolder + "\\ParallaxTemplate.hlsl", newFile = newFolder + "\\ParallaxTemplate.hlsl";
	IDirect3DDevice9* device = gpu.device.Get();
	ComPtr<IDirect3DTexture9> heightMap = ProceduralTexture(device, 512, HeightTexel);
	Check(device->SetVertexShader(gpu.objectVS.Get()), "SetVertexShader");
	for (int k = 0; k < 14; k++) Check(device->SetTexture(k, nullptr), "SetTexture");
	Check(device->SetTexture(0, gpu.textures[0].Get()), "SetTexture base");
	Check(device->SetTexture(1, gpu.textures[7].Get()), "SetTexture normal");
	Check(device->SetTexture(3, heightMap.Get()), "SetTexture height");
	struct Variant { const char* name; Defines defines; };
	const Variant variants[] = {  // src/effects/POM.h
		{ "PAR2000", { { "PS", "" } } },
		{ "PAR2009 (SPECULAR)", { { "PS", "" }, { "SPECULAR", "" } } },
	};
	const Scene scene = ParallaxObjectScene();
	std::puts("PARALLAX OBJECTS pixels, 1024x1024 float target: ParallaxLite off must be bit-identical:");
	for (const Variant& v : variants) {
		ComPtr<IDirect3DPixelShader9> oldHolder, newHolder;
		IDirect3DPixelShader9* oldShader = CreatePS(device, Compile(oldFile, "ps_3_0", v.defines, 0).Get(), oldHolder);
		IDirect3DPixelShader9* newShader = CreatePS(device, Compile(newFile, "ps_3_0", v.defines, 0).Get(), newHolder);
		ParallaxObjectConstants(0.4f, 0).Apply(device);
		const std::vector<float> reference = Render(gpu, oldShader, scene);
		const Comparison off = Compare(reference, Render(gpu, newShader, scene));
		printf("  %-20s lite off: %s", v.name, off.different ? "DIFFERENT" : "identical");
		if (off.different) { printf(" (%zu pixels, largest difference %.3g)", off.different, off.worst); failures++; }
		printf("\n");
		if (off.nan) { printf("FAIL: %zu NaN pixels\n", off.nan); failures++; }
		ParallaxObjectConstants(0.0f, 0).Apply(device);
		const double parallax = ChangedShare(reference, Render(gpu, newShader, scene));
		ParallaxObjectConstants(0.4f, 1).Apply(device);
		const std::vector<float> lite = Render(gpu, newShader, scene);
		if (v.defines.size() == 1) {
			ParallaxObjectConstants(0.4f, 0).Apply(device);
			Render(gpu, newShader, scene);
			D3DXSaveSurfaceToFileA(NextToExe("parallax-objects-full.png").c_str(), D3DXIFF_PNG, gpu.target.Get(), NULL, NULL);
			ParallaxObjectConstants(0.4f, 1).Apply(device);
			Render(gpu, newShader, scene);
			D3DXSaveSurfaceToFileA(NextToExe("parallax-objects-lite.png").c_str(), D3DXIFF_PNG, gpu.target.Get(), NULL, NULL);
		}
		double sum = 0; size_t visible = 0;
		for (size_t p = 0; p < reference.size(); p += 4) {
			double d = 0;
			for (int k = 0; k < 3; k++) d = std::max(d, (double)fabsf(std::min(reference[p + k], 1.0f) - std::min(lite[p + k], 1.0f)));
			sum += d;
			if (d > 2.0 / 255) visible++;
		}
		const size_t pixels = reference.size() / 4;
		printf("  %-20s parallax changes %.0f%% of the pixels; lite vs off: mean difference %.2f/255, %.1f%% of pixels over 2/255\n",
			v.name, 100 * parallax, 255 * sum / pixels, 100.0 * visible / pixels);
		if (parallax < 0.25) { std::puts("FAIL: the test scene does not exercise parallax enough"); failures++; }
	}
	std::puts("PARALLAX OBJECTS GPU time per full-screen draw at 2560x1440, forward shadows compiled in:");
	for (const Variant& v : variants) {
		ComPtr<IDirect3DPixelShader9> oldHolder, newHolder;
		IDirect3DPixelShader9* oldShader = CreatePS(device, Compile(oldFile, "ps_3_0", v.defines, 1).Get(), oldHolder);
		IDirect3DPixelShader9* newShader = CreatePS(device, Compile(newFile, "ps_3_0", v.defines, 1).Get(), newHolder);
		ParallaxObjectConstants(0.4f, 0).Apply(device);
		TimeOldNew(gpu, (std::string(v.name) + ", lite off").c_str(), oldShader, newShader, scene);
		double offMs = 1e9, liteMs = 1e9;
		for (int round = 0; round < 5; round++) {
			ParallaxObjectConstants(0.4f, 0).Apply(device); offMs = std::min(offMs, TimeDraws(gpu, newShader, scene));
			ParallaxObjectConstants(0.4f, 1).Apply(device); liteMs = std::min(liteMs, TimeDraws(gpu, newShader, scene));
		}
		printf("  %-20s new shader: off %.3f ms, lite %.3f ms (%+.0f%%)\n", v.name, offMs, liteMs, 100 * (liteMs - offMs) / offMs);
	}
	return failures;
}


// ---- Environment maps, SharpSky (UNOFFICIAL): EnvTemplate.hlsl, the old folder against the new one. No game files needed. ----
static int TestEnvSharp(Gpu& gpu, const std::string& oldFolder, const std::string& newFolder) {
	int failures = 0;
	IDirect3DDevice9* device = gpu.device.Get();
	const std::string oldFile = oldFolder + "\\EnvTemplate.hlsl", newFile = newFolder + "\\EnvTemplate.hlsl";
	ComPtr<IDirect3DCubeTexture9> cube = MakeEnvCube(device);
	for (int t = 0; t < 16; t++) device->SetTexture(t, nullptr);
	device->SetTexture(0, gpu.textures[7].Get());   // NormalMap (alpha: the mask)
	device->SetTexture(1, cube.Get());              // EnvironmentCubeMap
	device->SetTexture(3, gpu.textures[1].Get());   // CustomEnvMask
	ComPtr<IDirect3DVertexShader9> vs3;
	Check(device->CreateVertexShader((const DWORD*)CompileText(PassThroughVS, "vs_3_0")->GetBufferPointer(), &vs3), "vs3");
	Check(device->SetVertexShader(vs3.Get()), "set vs3");
	const Scene pixelScene = EnvPixelScene();
	std::puts("ENV MAPS SharpSky (EnvTemplate.hlsl, old folder vs new): off must match the old shader, on must change the reflection, no NaNs:");
	struct PsVariant { const char* name; Defines defines; };
	const PsVariant psVariants[] = { { "SLS2057", { { "PS", "" } } }, { "SLS2058", { { "PS", "" }, { "NEGATE_VIEW", "" } } } };
	for (const PsVariant& v : psVariants) {
		ComPtr<IDirect3DPixelShader9> oldHolder, newHolder;
		IDirect3DPixelShader9* before = CreatePS(device, CompileSource(oldFile, "ps_3_0", v.defines).Get(), oldHolder);
		IDirect3DPixelShader9* after = CreatePS(device, CompileSource(newFile, "ps_3_0", v.defines).Get(), newHolder);
		Constants k;
		CommonConstants(k);                        // TESR_SharpSky (c136) off
		k.Set(1, 0.30f, 0.32f, 0.36f, 0.9f);     // AmbientColor
		k.Set(27, 1, 0, 1.5f, 0.35f);            // EnvToggles: vertex colour, -, strength, custom mask share
		k.Set(32, 0, 1, 1, 0.5f);                // TESR_PBRData: ambient scale
		k.Set(33, 1, 0.5f, 0.5f, 0);             // TESR_PBRExtraData: skylight scale
		k.Set(34, 0, 0.6f, 0.25f, 0.55f);        // TESR_PBRMetal: off
		k.Set(35, 1, 1.5f, 0.5f, 0);             // TESR_PBREnv: Lighting 1, so the sky term is in use
		k.Apply(device);
		const std::vector<float> a = Render(gpu, before, pixelScene);
		const Comparison off = Compare(a, Render(gpu, after, pixelScene));
		k.Set(136, 1, 0, 0, 0);                  // TESR_SharpSky: on
		k.Apply(device);
		const std::vector<float> sharpened = Render(gpu, after, pixelScene);
		const Comparison on = Compare(a, sharpened);
		printf("  %s: off vs old: %zu pixels differ (worst %g) | on: %.0f%% of pixels change, mean %.4f -> %.4f, NaN %zu\n", v.name, off.different, off.worst,
			100.0 * ChangedShare(a, sharpened), MeanLuma(a), MeanLuma(sharpened), on.nan);
		if (off.different) { printf("FAIL: %s changes with SharpSky off\n", v.name); failures++; }
		if (on.nan || off.nan) { printf("FAIL: NaN pixels\n"); failures++; }
		if (ChangedShare(a, sharpened) < 0.05) { printf("FAIL: SharpSky on barely changes the reflection\n"); failures++; }
	}
	return failures;
}

// ---- Skin (UNOFFICIAL): SKIN2000/2001/2002/2003/2006, the hand-written skin pixel shaders and their sun term (Includes\Skin.hlsl).
// They read what the object vertex shader passes through: sun, point light and eye directions in tangent space (unit length),
// attenuation UVs, and shadowWorldPos with a sentinel w (0.1-0.9 here, never 1, so no forward sun shadow is applied; the
// shadowed code is still compiled and must match). ----
static void UnitDirection(float out[3]) {
	const float v[3] = { Random() * 2 - 1, Random() * 2 - 1, Random() * 2 - 1 };
	const float length = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]) + 1e-6f;
	for (int k = 0; k < 3; k++) out[k] = v[k] / length;
}

static Scene SkinScene() {
	Scene scene;
	for (int j = 0; j < Grid; j++)
		for (int i = 0; i < Grid; i++) {
			const float sx = i / float(Grid - 1), sy = j / float(Grid - 1);
			Vertex v = {};
			float sun[3], point1[3], point2[3], eye[3];
			UnitDirection(sun); UnitDirection(point1); UnitDirection(point2); UnitDirection(eye);
			Set(v, 0, sx * 2 - 1, 1 - sy * 2, 0.5f, 1);
			Set(v, 1, sx * 5, sy * 5, 0, 0);                                                                // TEXCOORD0 uv
			Set(v, 2, sun[0], sun[1], sun[2], 0);                                                           // TEXCOORD1 sun direction
			Set(v, 3, point1[0], point1[1], point1[2], 0);                                                  // TEXCOORD2 point light 1 direction
			Set(v, 4, point2[0], point2[1], point2[2], 0);                                                  // TEXCOORD3 point light 2 direction (SKIN2002: shadowWorldPos)
			Set(v, 5, 0.1f + 0.8f * sx, 0.1f + 0.8f * sy, 0.1f + 0.8f * (1 - sx), 0.1f + 0.8f * (1 - sy));  // TEXCOORD4 attenuation UVs (SKIN2000/2001: shadowWorldPos)
			Set(v, 6, 0.1f + 0.8f * (1 - sy), 0.1f + 0.8f * sx, 0.1f + 0.8f * sy, 0.1f + 0.8f * (1 - sx));  // TEXCOORD5 attenuation UVs (SKIN2006)
			Set(v, 7, eye[0], eye[1], eye[2], 0);                                                           // TEXCOORD6 eye direction (SKIN2006: shadowWorldPos)
			Set(v, 8, eye[0], eye[1], eye[2], 0);                                                           // TEXCOORD7 eye direction (SKIN2006)
			Set(v, 9, 0.6f + 0.4f * Random(), 0.6f + 0.4f * Random(), 0.6f + 0.4f * Random(), 1);            // COLOR0 vertex colour
			Set(v, 10, 0.5f, 0.55f, 0.6f, 0.1f + 0.3f * Random());                                          // COLOR1 fog colour and amount
			scene.vertices.push_back(v);
		}
	GridIndices(scene);
	return scene;
}

// Skin: base map s0, normal map s1, face-gen maps s2/s3, glow and attenuation maps s4/s5. s6 and s7 are shadow maps the
// pixel shaders declare but do not sample; the shadow atlas (s9) stays unbound.
static void BindSkinTextures(Gpu& gpu) {
	const int textures[8] = { 0, 7, 2, 3, 4, 5, 6, 1 };
	for (int k = 0; k < 14; k++) Check(gpu.device->SetTexture(k, k < 8 ? gpu.textures[textures[k]].Get() : nullptr), "SetTexture");
}

// The registers the skin shaders read. Sun at c3 (PSLightColor[0] or [1]), points at c4 and c5, TESR_SkinDiffusion at c169.
static Constants SkinConstants(float diffusion, float translucency) {
	Constants k;
	CommonConstants(k);
	k.Set(1, 0.15f, 0.15f, 0.18f, 1);                      // AmbientColor (.a 1: no alpha test)
	k.Set(3, 1.0f, 0.9f, 0.75f, 1);                        // sun
	k.Set(4, 0.35f, 0.6f, 1.0f, 1);                        // point light 1
	k.Set(5, 1.0f, 0.35f, 0.2f, 1);                        // point light 2
	k.Set(27, 1, 1, 0, 0.5f);                              // Toggles: vertex colour, fog, specularity, alpha ref
	k.Set(134, 0, 0, 1, 1);                                // TESR_PBRData: light scale 1, ambient scale 1
	k.Set(135, 1, 0.5f, 0.5f, 0);                          // TESR_PBRExtraData: skylight scale, directionality
	k.Set(169, diffusion, translucency, 0, 0);             // TESR_SkinDiffusion: x PerPixelDiffusion, y Translucency
	return k;
}

static int TestSkin(Gpu& gpu, const std::string& oldFolder, const std::string& newFolder) {
	int failures = 0;
	IDirect3DDevice9* device = gpu.device.Get();
	struct Variant { const char* name; const char* file; Defines defines; bool interior; };
	const Variant variants[] = {
		{ "SKIN2000", "SKIN2000.pso.hlsl", {}, false },
		{ "SKIN2001", "SKIN2001.pso.hlsl", {}, false },
		{ "SKIN2002", "SKIN2002.pso.hlsl", {}, false },
		{ "SKIN2003", "SKIN2003.pso.hlsl", {}, false },
		{ "SKIN2006", "SKIN2006.pso.hlsl", {}, false },
		// Interior builds of the two that take point-light shadows (ShaderManager: INTERIOR_SHADOWS, no forward sun shadow).
		{ "SKIN2002 interior", "SKIN2002.pso.hlsl", { { "INTERIOR_SHADOWS", "1" }, { "FORWARD_SHADOWS", "0" } }, true },
		{ "SKIN2006 interior", "SKIN2006.pso.hlsl", { { "INTERIOR_SHADOWS", "1" }, { "FORWARD_SHADOWS", "0" } }, true },
	};
	Check(device->SetVertexShader(gpu.objectVS.Get()), "SetVertexShader");
	BindSkinTextures(gpu);
	std::puts("SKIN pixels, 1024x1024 float target: PerPixelDiffusion 0 must be bit-identical to the old shader, 1 must change the scene, no NaNs:");
	for (const Variant& v : variants)
		for (int forwardShadows = 0; forwardShadows <= (v.interior ? 0 : 1); forwardShadows++) {
			const std::string oldFile = oldFolder + "\\" + v.file, newFile = newFolder + "\\" + v.file;
			ComPtr<IDirect3DPixelShader9> oldHolder, newHolder;
			IDirect3DPixelShader9* oldShader = CreatePS(device, Compile(oldFile, "ps_3_0", v.defines, forwardShadows).Get(), oldHolder);
			IDirect3DPixelShader9* newShader = CreatePS(device, Compile(newFile, "ps_3_0", v.defines, forwardShadows).Get(), newHolder);
			const Scene scene = SkinScene();
			SkinConstants(0, 0).Apply(device);
			const std::vector<float> reference = Render(gpu, oldShader, scene);
			const Comparison off = Compare(reference, Render(gpu, newShader, scene));
			SkinConstants(1, 0.5f).Apply(device);
			const std::vector<float> on = Render(gpu, newShader, scene);
			const Comparison onResult = Compare(reference, on);
			const double changed = ChangedShare(reference, on);
			printf("  %-18s forward shadows %d: off %s (%zu pixels differ, largest %.3g); on: %.1f%% of pixels change\n",
				v.name, forwardShadows, off.different ? "DIFFERENT" : "identical", off.different, off.worst, 100 * changed);
			if (off.different) failures++;
			if (off.nan || onResult.nan) { printf("FAIL: %s, forward shadows %d: NaN pixels\n", v.name, forwardShadows); failures++; }
			if (changed < 0.05) { printf("FAIL: %s, forward shadows %d: the diffusion barely changes the test scene\n", v.name, forwardShadows); failures++; }
		}
	return failures;
}

// UNOFFICIAL terrain blending (TerrainTemplate.hlsl TERRAIN_BLEND, [Shaders.Terrain.Blending]). The "scene depth" the land
// compares against is made by drawing the same land with a pixel shader that writes the reversed depth of a point
// `behind` units further along the view ray: 0 = nothing behind (the sky, depth 0), else an object that close behind.
static ComPtr<IDirect3DTexture9> BlendDepthTexture(Gpu& gpu, const Scene& scene, UINT width, UINT height, float behind) {
	IDirect3DDevice9* device = gpu.device.Get();
	ComPtr<IDirect3DTexture9> texture;
	Check(device->CreateTexture(width, height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_R32F, D3DPOOL_DEFAULT, &texture, NULL), "blend depth texture");
	ComPtr<IDirect3DSurface9> surface, previous;
	Check(texture->GetSurfaceLevel(0, &surface), "blend depth surface");
	Check(device->GetRenderTarget(0, &previous), "GetRenderTarget");
	Check(device->SetRenderTarget(0, surface.Get()), "SetRenderTarget blend depth");
	D3DVIEWPORT9 viewport = { 0, 0, width, height, 0, 1 };
	Check(device->SetViewport(&viewport), "SetViewport blend depth");
	Check(device->Clear(0, NULL, D3DCLEAR_TARGET, 0, 1, 0), "Clear blend depth");
	if (behind > 0) {
		char source[512];
		sprintf_s(source, "float4 main(float4 proj : TEXCOORD6) : COLOR0 { float w = proj.w + %.6f; return (%.9g * w + %.9g) / w; }",
			behind, TestProjectionA, TestProjectionB);
		ComPtr<ID3DXBuffer> code, errors;
		Check(D3DXCompileShader(source, (UINT)strlen(source), NULL, NULL, "main", "ps_3_0", 0, &code, &errors, NULL), "compile blend depth shader");
		ComPtr<IDirect3DPixelShader9> holder;
		Check(device->BeginScene(), "BeginScene");
		Draw(device, CreatePS(device, code.Get(), holder), scene);
		Check(device->EndScene(), "EndScene");
	}
	Check(device->SetRenderTarget(0, previous.Get()), "SetRenderTarget restore");
	return texture;
}

static size_t ClearedPixels(const std::vector<float>& pixels) {
	size_t cleared = 0;
	for (size_t p = 0; p < pixels.size(); p += 4) if (pixels[p] == 0 && pixels[p + 1] == 0 && pixels[p + 2] == 0 && pixels[p + 3] == 0) cleared++;
	return cleared;
}

static int TestTerrainBlend(Gpu& gpu, const std::string& newFolder) {
	int failures = 0;
	const std::string file = newFolder + "\\TerrainTemplate.hlsl";
	auto defines = [](int texCount, const char* lights, bool blend) {
		Defines d = { { "PS", "" }, { "TEX_COUNT", std::to_string(texCount) } };
		if (lights) d.push_back({ "NUM_PT_LIGHTS", lights });
		if (blend) d.push_back({ "TERRAIN_BLEND", "1" });
		return d;
	};
	std::puts("TERRAIN BLEND instruction slots, off -> compiled in (forward shadows):");
	for (int texCount = 1; texCount <= 7; texCount++) {
		unsigned off, on;
		Compile(file, "ps_3_0", defines(texCount, NULL, false), 1, &off);
		Compile(file, "ps_3_0", defines(texCount, NULL, true), 1, &on);
		Compile(file, "ps_3_0", defines(texCount, "24", true), 1);
		printf("  TEX_COUNT %d: %u -> %u\n", texCount, off, on);
	}
	IDirect3DDevice9* device = gpu.device.Get();
	Check(device->SetVertexShader(gpu.terrainVS.Get()), "SetVertexShader");
	BindTextures(gpu, true);
	device->SetSamplerState(15, D3DSAMP_MINFILTER, D3DTEXF_POINT);
	device->SetSamplerState(15, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
	device->SetSamplerState(15, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
	device->SetSamplerState(15, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
	device->SetSamplerState(15, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
	const float parallax[4] = { 1, 1, 1, 1 };
	// on: TESR_TerrainBlendData.x; debug: .w. Blend distance 10, max distance 4096; 1 / resolution; dither frame 0.
	auto constants = [&](float on, float debug, UINT width, UINT height) {
		Constants k = TerrainConstants(parallax);
		k.Set(176, on, 1.0f / 10.0f, 4096.0f, debug);
		k.Set(177, 1.0f / width, 1.0f / height, 0, 0);
		k.Apply(device);
	};
	std::puts("TERRAIN BLEND pixels, 1024x1024 float target (against the same file without TERRAIN_BLEND):");
	for (int texCount = 1; texCount <= 7; texCount += 3) {
		ComPtr<IDirect3DPixelShader9> offHolder, onHolder;
		IDirect3DPixelShader9* offShader = CreatePS(device, Compile(file, "ps_3_0", defines(texCount, NULL, false), 0).Get(), offHolder);
		IDirect3DPixelShader9* onShader = CreatePS(device, Compile(file, "ps_3_0", defines(texCount, NULL, true), 0).Get(), onHolder);
		const Scene scene = TerrainScene(texCount);
		ComPtr<IDirect3DTexture9> sky = BlendDepthTexture(gpu, scene, Gpu::Size, Gpu::Size, 0);
		ComPtr<IDirect3DTexture9> near5 = BlendDepthTexture(gpu, scene, Gpu::Size, Gpu::Size, 5);
		ComPtr<IDirect3DTexture9> far50 = BlendDepthTexture(gpu, scene, Gpu::Size, Gpu::Size, 50);
		constants(0, 0, Gpu::Size, Gpu::Size);
		const std::vector<float> reference = Render(gpu, offShader, scene);
		auto report = [&](const char* name, IDirect3DTexture9* depth, float on, float debug, bool mustMatch) {
			Check(device->SetTexture(15, depth), "SetTexture s15");
			constants(on, debug, Gpu::Size, Gpu::Size);
			const std::vector<float> pixels = Render(gpu, onShader, scene);
			const Comparison result = Compare(reference, pixels);
			const double gone = ClearedPixels(pixels) / double(Gpu::Size * Gpu::Size);
			printf("  TEX_COUNT %d, %-46s %s", texCount, name, result.different ? "different" : "identical");
			if (result.different) printf(" (%.1f%% of pixels, largest %.3g)", 100.0 * result.different / (Gpu::Size * Gpu::Size), result.worst);
			printf(", dithered away %.1f%%\n", 100 * gone);
			if (mustMatch && result.different) { std::puts("FAIL: expected identical"); failures++; }
			if (result.nan) { printf("FAIL: %zu NaN pixels\n", result.nan); failures++; }
			return gone;
		};
		report("compiled in, off (Data.x 0)", sky.Get(), 0, 0, false);
		report("on, nothing behind the land (sky)", sky.Get(), 1, 0, false);
		report("on, objects 50 units behind (> BlendDistance)", far50.Get(), 1, 0, false);
		const double half = report("on, objects 5 units behind (half the band)", near5.Get(), 1, 0, false);
		if (half < 0.35 || half > 0.65) { std::puts("FAIL: about half the land should dither away 5 units over an object"); failures++; }
		const double debugGone = report("debug view, objects 5 units behind", near5.Get(), 1, 1, false);
		if (debugGone > 0) { std::puts("FAIL: the debug view must not discard"); failures++; }
	}
	std::puts("TERRAIN BLEND GPU time per full-screen draw at 2560x1440 (A16B16G16R16F), forward shadows compiled in:");
	for (int texCount = 1; texCount <= 7; texCount += 3) {
		ComPtr<IDirect3DPixelShader9> offHolder, onHolder;
		IDirect3DPixelShader9* offShader = CreatePS(device, Compile(file, "ps_3_0", defines(texCount, NULL, false), 1).Get(), offHolder);
		IDirect3DPixelShader9* onShader = CreatePS(device, Compile(file, "ps_3_0", defines(texCount, NULL, true), 1).Get(), onHolder);
		const Scene scene = TerrainScene(texCount);
		ComPtr<IDirect3DTexture9> sky = BlendDepthTexture(gpu, scene, 2560, 1440, 0);
		ComPtr<IDirect3DTexture9> near5 = BlendDepthTexture(gpu, scene, 2560, 1440, 5);
		const std::string label = "TEX_COUNT " + std::to_string(texCount);
		Check(device->SetTexture(15, sky.Get()), "SetTexture s15");
		constants(0, 0, 2560, 1440);
		TimeOldNew(gpu, (label + ", not compiled vs compiled, off").c_str(), offShader, onShader, scene);
		constants(1, 0, 2560, 1440);
		TimeOldNew(gpu, (label + ", not compiled vs on, sky behind").c_str(), offShader, onShader, scene);
		Check(device->SetTexture(15, near5.Get()), "SetTexture s15");
		TimeOldNew(gpu, (label + ", not compiled vs on, all in band").c_str(), offShader, onShader, scene);
	}
	Check(device->SetTexture(15, nullptr), "SetTexture s15");
	return failures;
}


int main(int argc, char** argv) {
	try {
		if (argc >= 5 && strcmp(argv[1], "--asm-template") == 0) {
			Defines defines;
			for (int k = 5; k < argc; k++) {
				const char* eq = strchr(argv[k], '=');
				defines.push_back({ eq ? std::string(argv[k], (size_t)(eq - argv[k])) : std::string(argv[k]), eq ? std::string(eq + 1) : std::string() });
			}
			std::string text;
			Compile(argv[2], argv[3], defines, 1, nullptr, &text);
			FILE* out = nullptr;
			if (fopen_s(&out, argv[4], "w") || !out) return 1;
			fputs(text.c_str(), out);
			fclose(out);
			return 0;
		}
		if (argc == 5 && strcmp(argv[3], "env") == 0) {   // game_shaders <old> <new> env <folder with the game's SLS2050/2051.vso, SLS2057/2058.pso>
			Gpu gpu;
			CreateGpu(gpu);
			const int failures = TestEnv(gpu, argv[2], argv[4]);
			if (failures) { printf("%d game shader check(s) FAILED\n", failures); return 1; }
			std::puts("All game shader checks passed.");
			return 0;
		}
		if (argc != 3 && argc != 4) { std::puts("usage: game_shaders <old Shaders folder> <new Shaders folder> [terrain|objects]"); return 2; }
		const std::string which = argc == 4 ? argv[3] : "";
		Gpu gpu;
		CreateGpu(gpu);
		int failures = 0;
		if (which.empty() || which == "terrain") failures += TestTerrain(gpu, argv[1], argv[2]);
		if (which.empty() || which == "objects") failures += TestObjects(gpu, argv[1], argv[2]);
		if (which.empty() || which == "parallax") failures += TestParallaxObjects(gpu, argv[1], argv[2]);
		if (which.empty() || which == "metal") failures += TestMetal(gpu, argv[1], argv[2]);
		if (which.empty() || which == "materials") failures += TestMaterialRules(gpu, argv[1], argv[2]);
		if (which.empty() || which == "envsharp") failures += TestEnvSharp(gpu, argv[1], argv[2]);
		if (which.empty() || which == "skin") failures += TestSkin(gpu, argv[1], argv[2]);
		if (which == "terrainblend") failures += TestTerrainBlend(gpu, argv[2]);	// UNOFFICIAL: needs a TerrainTemplate with TERRAIN_BLEND
		if (failures) { printf("%d game shader check(s) FAILED\n", failures); return 1; }
		std::puts("All game shader checks passed.");
		return 0;
	}
	catch (const std::exception& e) {
		printf("ERROR: %s\n", e.what());
		return 1;
	}
}
