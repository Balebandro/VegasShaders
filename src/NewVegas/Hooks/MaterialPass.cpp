#include "MaterialPass.h"
#include <d3dx9shader.h>

// Everything but the four entry points (defined at the end, after this unnamed namespace) is private to this file.
namespace {
	// The flashlight owns every number this pass uses. The cone, the near fade and the hotspot
	// limit must match the screen space pass exactly, or the two disagree along the rim of the beam.
	static FlashlightEffect::FlashlightSettingsStruct& Cfg() {
		return TheShaderManager->Effects.Flashlight->Settings;
	}

	// Game RTTI class ids and the engine helpers that take them.
	static constexpr UInt32 kTypeNiNode = 0x11F4428;
	static constexpr UInt32 kTypeNiGeometry = 0x11F4ACC;
	static constexpr UInt32 kTypeNiSwitchNode = 0x11F5EB4;
	static constexpr UInt32 kTriShapeDraw = 0xE745A0;
	static constexpr UInt32 kTriStripDraw = 0xE74840;
	static constexpr UInt32 kIsKindOfFn = 0x43B300;

	static constexpr int kXRayDebugMode = 8;	// draws everything queued with no depth test
	static constexpr UInt32 kMaxSavedStreams = 8;
	static constexpr UInt32 kMaxSavedTextures = 2;
	static constexpr UInt32 kSamplerValuesPerStage = 5;
	static constexpr UInt32 kSavedVsRegs = 8;	// c0-c3 view projection, c4-c7 world
	static constexpr UInt32 kSavedPsRegs = 7;	// c0-c4 and c6

	// Render states this pass overwrites. Each is recorded twice, once as NiDX9RenderState
	// caches it and once as the device holds it, because the two can disagree (see Save).
	// The order is the save and restore order.
	static const D3DRENDERSTATETYPE kTrackedStates[] = {
		D3DRS_ZENABLE,						// 0
		D3DRS_ZWRITEENABLE,					// 1
		D3DRS_ALPHABLENDENABLE,				// 2
		D3DRS_SRCBLEND,						// 3
		D3DRS_DESTBLEND,					// 4
		D3DRS_ALPHATESTENABLE,				// 5
		D3DRS_STENCILENABLE,				// 6
		D3DRS_CULLMODE,						// 7 = kCullModeSlot
		D3DRS_COLORWRITEENABLE,				// 8
		D3DRS_FOGENABLE,					// 9
		D3DRS_ZFUNC,						// 10
		D3DRS_DEPTHBIAS,					// 11
		D3DRS_SCISSORTESTENABLE,			// 12
		D3DRS_CLIPPLANEENABLE,				// 13
		D3DRS_BLENDOP,						// 14
		D3DRS_SEPARATEALPHABLENDENABLE,		// 15
		D3DRS_MULTISAMPLEMASK,				// 16
		D3DRS_SLOPESCALEDEPTHBIAS,			// 17
		D3DRS_MULTISAMPLEANTIALIAS,			// 18
		D3DRS_SRGBWRITEENABLE,				// 19
	};
	static constexpr UInt32 kTrackedStateCount = sizeof(kTrackedStates) / sizeof(kTrackedStates[0]);
	static_assert(kTrackedStateCount == 20, "MaterialPass: tracked state list changed size");
	static constexpr UInt32 kCullModeSlot = 7;	// the cull mode goes through the engine's stencil mapping, so it is not restored twice

	// The four shader sources below are compiled at first use (EnsureShaders). Only the uniform
	// names, registers, semantics and the order of the arithmetic reach the bytecode; local names do not.

	// Flat-normal pass: vertex stage.
	static const char* kFlatVertexSrc =
		"row_major float4x4 viewProj : register(c0);\n"
		"row_major float4x4 worldMat : register(c4);\n"
		"struct VertIn { float4 pos : POSITION0; float3 normal : NORMAL0; float4 uv : TEXCOORD0; };\n"
		"struct VertOut { float4 pos : POSITION; float2 uv : TEXCOORD0; float3 worldRel : TEXCOORD1; float3 normalRel : TEXCOORD2; };\n"
		"VertOut main(VertIn vin) {\n"
		"    VertOut vout;\n"
		"    float4 objPos = float4(vin.pos.xyz, 1.0);\n"
		"    float4 camRel = mul(objPos, worldMat);\n"
		"    vout.pos = mul(float4(camRel.xyz * 0.9997, 1.0), viewProj);\n"
		"    vout.uv = vin.uv.xy;\n"
		"    vout.worldRel = camRel.xyz;\n"
		"    vout.normalRel = normalize(mul(float4(vin.normal.xyz, 0.0), worldMat).xyz);\n"
		"    return vout;\n"
		"}\n";

	// Flat-normal pass: pixel stage.
	static const char* kFlatPixelSrc =
		"float4 lightColorIntensity : register(c0);\n"
		"float4 lightDirAngle : register(c1);\n"
		"float4 tuning : register(c2);\n"
		"float4 lightPosRadius : register(c3);\n"
		"float4 specParams : register(c4);\n"
		"float4 hotspotParams : register(c6);\n"
		"sampler2D baseMap : register(s0);\n"
		"struct PixIn { float2 uv : TEXCOORD0; float3 worldRel : TEXCOORD1; float3 normalRel : TEXCOORD2; };\n"
		"float linstep01(float lo, float hi, float x) { return saturate((x - lo) / max(0.0001, hi - lo)); }\n"
		"float lum709(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }\n"
		"float4 main(PixIn pin) : COLOR0 {\n"
		"    float dbg = 4.0 * max(lightColorIntensity.w, 0.25);\n"
		"    if (tuning.y > 0.5 && tuning.y < 1.5) return float4(dbg, 0.0, 0.0, 1.0);\n"
		"    float4 albedo = tex2D(baseMap, pin.uv);\n"
		"    float3 toLamp = lightPosRadius.xyz - pin.worldRel;\n"
		"    float dist = length(toLamp);\n"
		"    float3 L = toLamp / max(dist, 0.001);\n"
		"    float reach = max(lightPosRadius.w, 1.0);\n"
		"    float q = saturate((dist / reach) * (dist / reach));\n"
		"    float falloff = saturate(((1.0 - q) * (1.0 - q)) / (1.0 + 5.0 * q));\n"
		"    float coneDeg = max(1.0, lightDirAngle.w);\n"
		"    float outerCos = cos(radians(coneDeg));\n"
		"    float innerCos = cos(radians(coneDeg * 0.5));\n"
		"    float cone = pow(linstep01(outerCos, innerCos, dot(normalize(lightDirAngle.xyz), -L)), 2.0);\n"
		"    float fadeIn = (tuning.x > 0.0) ? smoothstep(0.0, tuning.x, dist) : 1.0;\n"
		"    float weight = falloff * cone * fadeIn;\n"
		"    float3 N = normalize(pin.normalRel);\n"
		"    float ndl = saturate(dot(N, L));\n"
		"    float3 V = normalize(-pin.worldRel);\n"
		"    float3 H = normalize(L + V);\n"
		"    float ndh = saturate(dot(N, H));\n"
		"    float gloss = smoothstep(0.45, 0.95, saturate(specParams.z));\n"
		"    float shine = specParams.y * 2.0;\n"
		"    float lobeExp = max(shine / 32.0, 1.0);\n"
		"    float lobe = pow(saturate((ndh - 0.72) / 0.28), lobeExp);\n"
		"    float spec = specParams.x * gloss * lobe;\n"
		"    spec *= (ndl <= 0.2) ? saturate(ndl + 0.5) : 1.0;\n"
		"    float3 diffLit = albedo.rgb * ndl * lightColorIntensity.rgb * lightColorIntensity.w * weight;\n"
		"    float specMask = saturate(spec);\n"
		"    float3 specLit = saturate(specMask * weight * lightColorIntensity.rgb);\n"
		"    if (tuning.y > 6.5) return float4(saturate(lobe * 8.0), gloss, saturate(lobe * gloss * 16.0), 1.0);\n"
		"    if (tuning.y > 5.5) return float4(0.0, 0.0, 0.0, 1.0);\n"
		"    if (tuning.y > 4.5) return float4(pow(ndh, 16.0) * dbg * weight, pow(ndh, 16.0) * dbg * weight, pow(ndh, 16.0) * dbg * weight, 1.0);\n"
		"    if (tuning.y > 3.5) return float4(0.0, 0.0, 0.0, 1.0);\n"
		"    if (tuning.y > 2.5) return float4(specLit * 4.0, 1.0);\n"
		"    if (tuning.y > 1.5) return float4(0.0, 0.0, dbg * weight, 1.0);\n"
		"    float3 lit = diffLit + specLit;\n"
		"    if (hotspotParams.x > 0.0) {\n"
		"        float cap = 1.0 / hotspotParams.x;\n"
		"        float y = lum709(lit);\n"
		"        lit *= cap / max(cap, y);\n"
		"    }\n"
		"    return float4(lit, 1.0);\n"
		"}\n";

	// Normal-mapped pass: vertex stage, which also passes the tangent basis through.
	static const char* kBumpVertexSrc =
		"row_major float4x4 viewProj : register(c0);\n"
		"row_major float4x4 worldMat : register(c4);\n"
		"struct VertIn { float4 pos : POSITION0; float3 tangent : TANGENT0; float3 binormal : BINORMAL0; float3 normal : NORMAL0; float4 uv : TEXCOORD0; };\n"
		"struct VertOut { float4 pos : POSITION; float2 uv : TEXCOORD0; float3 worldRel : TEXCOORD1; float3 tangentRel : TEXCOORD2; float3 binormalRel : TEXCOORD3; float3 normalRel : TEXCOORD4; };\n"
		"VertOut main(VertIn vin) {\n"
		"    VertOut vout;\n"
		"    float4 objPos = float4(vin.pos.xyz, 1.0);\n"
		"    float4 camRel = mul(objPos, worldMat);\n"
		"    vout.pos = mul(float4(camRel.xyz * 0.9997, 1.0), viewProj);\n"
		"    vout.uv = vin.uv.xy;\n"
		"    vout.worldRel = camRel.xyz;\n"
		"    vout.tangentRel = normalize(mul(float4(vin.tangent.xyz, 0.0), worldMat).xyz);\n"
		"    vout.binormalRel = normalize(mul(float4(vin.binormal.xyz, 0.0), worldMat).xyz);\n"
		"    vout.normalRel = normalize(mul(float4(vin.normal.xyz, 0.0), worldMat).xyz);\n"
		"    return vout;\n"
		"}\n";

	// Normal-mapped pass: pixel stage.
	static const char* kBumpPixelSrc =
		"float4 lightColorIntensity : register(c0);\n"
		"float4 lightDirAngle : register(c1);\n"
		"float4 tuning : register(c2);\n"
		"float4 lightPosRadius : register(c3);\n"
		"float4 specParams : register(c4);\n"
		"float4 hotspotParams : register(c6);\n"
		"sampler2D baseMap : register(s0);\n"
		"sampler2D bumpMap : register(s1);\n"
		"struct PixIn { float2 uv : TEXCOORD0; float3 worldRel : TEXCOORD1; float3 tangentRel : TEXCOORD2; float3 binormalRel : TEXCOORD3; float3 normalRel : TEXCOORD4; };\n"
		"float linstep01(float lo, float hi, float x) { return saturate((x - lo) / max(0.0001, hi - lo)); }\n"
		"float lum709(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }\n"
		"float4 main(PixIn pin) : COLOR0 {\n"
		"    float dbg = 4.0 * max(lightColorIntensity.w, 0.25);\n"
		"    if (tuning.y > 0.5 && tuning.y < 1.5) return float4(dbg, 0.0, 0.0, 1.0);\n"
		"    float4 albedo = tex2D(baseMap, pin.uv);\n"
		"    float3 toLamp = lightPosRadius.xyz - pin.worldRel;\n"
		"    float dist = length(toLamp);\n"
		"    float3 L = toLamp / max(dist, 0.001);\n"
		"    float reach = max(lightPosRadius.w, 1.0);\n"
		"    float q = saturate((dist / reach) * (dist / reach));\n"
		"    float falloff = saturate(((1.0 - q) * (1.0 - q)) / (1.0 + 5.0 * q));\n"
		"    float coneDeg = max(1.0, lightDirAngle.w);\n"
		"    float outerCos = cos(radians(coneDeg));\n"
		"    float innerCos = cos(radians(coneDeg * 0.5));\n"
		"    float cone = pow(linstep01(outerCos, innerCos, dot(normalize(lightDirAngle.xyz), -L)), 2.0);\n"
		"    float fadeIn = (tuning.x > 0.0) ? smoothstep(0.0, tuning.x, dist) : 1.0;\n"
		"    float weight = falloff * cone * fadeIn;\n"
		"    float3 T = normalize(pin.tangentRel);\n"
		"    float3 B = normalize(pin.binormalRel);\n"
		"    float3 N = normalize(pin.normalRel);\n"
		"    float4 bumpTex = tex2D(bumpMap, pin.uv);\n"
		"    float3 tsN = bumpTex.xyz * 2.0 - 1.0;\n"
		"    tsN.xy *= max(specParams.w, 0.0);\n"
		"    tsN = normalize(tsN);\n"
		"    float3 Nb = normalize(tsN.x * T + tsN.y * B + tsN.z * N);\n"
		"    float ndlVert = saturate(dot(N, L));\n"
		"    float ndl = saturate(dot(Nb, L));\n"
		"    float3 V = normalize(-pin.worldRel);\n"
		"    float3 H = normalize(L + V);\n"
		"    float glossTex = saturate(bumpTex.a);\n"
		"    float gloss = smoothstep(0.45, 0.95, glossTex);\n"
		"    float ndh = saturate(dot(Nb, H));\n"
		"    float shine = specParams.y * 2.0;\n"
		"    float lobeExp = max(shine / 32.0, 1.0);\n"
		"    float lobe = pow(saturate((ndh - 0.72) / 0.28), lobeExp);\n"
		"    float spec = specParams.x * gloss * lobe;\n"
		"    spec *= (ndl <= 0.2) ? saturate(ndl + 0.5) : 1.0;\n"
		"    float3 diffLit = albedo.rgb * ndl * lightColorIntensity.rgb * lightColorIntensity.w * weight;\n"
		"    float specMask = saturate(spec);\n"
		"    float3 specLit = saturate(specMask * weight * lightColorIntensity.rgb);\n"
		"    if (tuning.y > 6.5) return float4(saturate(lobe * 8.0), gloss, saturate(lobe * gloss * 16.0), 1.0);\n"
		"    if (tuning.y > 5.5) {\n"
		"        float dN = ndl - ndlVert;\n"
		"        float up = saturate(dN * 8.0) * dbg * weight;\n"
		"        float down = saturate(-dN * 8.0) * dbg * weight;\n"
		"        return float4(up, down, down, 1.0);\n"
		"    }\n"
		"    if (tuning.y > 4.5) return float4(pow(ndh, 16.0) * dbg * weight, pow(ndh, 16.0) * dbg * weight, pow(ndh, 16.0) * dbg * weight, 1.0);\n"
		"    if (tuning.y > 3.5) return float4(glossTex * dbg * weight, glossTex * dbg * weight, glossTex * dbg * weight, 1.0);\n"
		"    if (tuning.y > 2.5) return float4(specLit * 4.0, 1.0);\n"
		"    if (tuning.y > 1.5) return float4(0.0, dbg * weight, 0.0, 1.0);\n"
		"    float3 lit = diffLit + specLit;\n"
		"    if (hotspotParams.x > 0.0) {\n"
		"        float cap = 1.0 / hotspotParams.x;\n"
		"        float y = lum709(lit);\n"
		"        lit *= cap / max(cap, y);\n"
		"    }\n"
		"    return float4(lit, 1.0);\n"
		"}\n";

	// One queued draw: the geometry, the two textures it is lit with and its specular inputs.
	struct QueuedDraw {
		NiGeometry*				geo;
		IDirect3DBaseTexture9*	baseTex;
		IDirect3DBaseTexture9*	bumpTex;
		float					specScale;
		float					specExp;
		float					glossDefault;
		bool					hasBump;
	};

	struct SavedStream {
		IDirect3DVertexBuffer9*	vb;
		UINT					byteOffset;
		UINT					vertexStride;
	};

	struct StateValue {
		D3DRENDERSTATETYPE state;
		DWORD value;
	};

	// Sampler states kept per stage, in the order they are read and written back.
	static const D3DSAMPLERSTATETYPE kSavedSamplerStates[kSamplerValuesPerStage] = {
		D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV, D3DSAMP_MAGFILTER, D3DSAMP_MINFILTER, D3DSAMP_MIPFILTER,
	};

	template <typename T> static void SafeRelease(T*& arPtr) {
		if (!arPtr) return;
		arPtr->Release();
		arPtr = nullptr;
	}

	// Snapshot of everything Save touches, put back by Restore. Render states are kept twice:
	// as NiDX9RenderState reports them and as the device reports them. Restoring both, in
	// that order, leaves a disagreement between the two exactly as the engine had it.
	struct DeviceSnapshot {
		IDirect3DVertexShader9*			vs;
		IDirect3DPixelShader9*			ps;
		IDirect3DVertexDeclaration9*	decl;
		IDirect3DBaseTexture9*			tex[kMaxSavedTextures];
		IDirect3DIndexBuffer9*			ib;
		SavedStream						vbs[kMaxSavedStreams];
		DWORD							fvfCode;
		DWORD							engineStates[kTrackedStateCount];
		DWORD							deviceStates[kTrackedStateCount];
		DWORD							samplers[kMaxSavedTextures][kSamplerValuesPerStage];
		float							vsRegs[kSavedVsRegs][4];
		float							psRegs[kSavedPsRegs][4];
		bool							regsSaved;

		void Save(IDirect3DDevice9* apDevice) {
			NiDX9RenderState* engine = TheRenderManager ? TheRenderManager->renderState : nullptr;
			// Start from all zeros, so a failed Get leaves a null / zero behind as before.
			vs = nullptr; ps = nullptr; decl = nullptr; ib = nullptr;
			fvfCode = 0;
			memset(tex, 0, sizeof(tex));
			memset(vbs, 0, sizeof(vbs));
			memset(engineStates, 0, sizeof(engineStates));
			memset(deviceStates, 0, sizeof(deviceStates));
			memset(samplers, 0, sizeof(samplers));
			memset(vsRegs, 0, sizeof(vsRegs));
			memset(psRegs, 0, sizeof(psRegs));
			regsSaved = false;

			apDevice->GetVertexShader(&vs);
			apDevice->GetPixelShader(&ps);
			apDevice->GetVertexDeclaration(&decl);
			for (UInt32 stage = 0; stage < kMaxSavedTextures; ++stage)
				apDevice->GetTexture(stage, &tex[stage]);
			apDevice->GetIndices(&ib);
			apDevice->GetFVF(&fvfCode);
			for (UInt32 n = 0; n < kMaxSavedStreams; ++n)
				apDevice->GetStreamSource(n, &vbs[n].vb, &vbs[n].byteOffset, &vbs[n].vertexStride);

			// Both halves are read on purpose. Measured: STENCILENABLE is 0 on the device and 1 in the
			// engine's cache, and the engine relies on that. It skips a write its cache calls redundant,
			// so while the cache says stencil is on it never really switches it on. Restoring only one
			// half repaired that quietly, and decals that were never stencil tested suddenly were, and
			// vanished. So the disagreement is put back as found.
			for (UInt32 k = 0; k < kTrackedStateCount; ++k) {
				const D3DRENDERSTATETYPE rs = kTrackedStates[k];
				engineStates[k] = engine ? engine->GetRenderState(rs) : 0;
				apDevice->GetRenderState(rs, &deviceStates[k]);
			}

			// Gamebryo uploads per-object constants for every draw but global ones only when it thinks
			// they went stale. Overwriting a global and walking away leaves the engine believing its
			// value is still resident, so the next user of that register gets this pass's matrix.
			const bool vsOk = SUCCEEDED(apDevice->GetVertexShaderConstantF(0, &vsRegs[0][0], kSavedVsRegs));
			regsSaved = vsOk && SUCCEEDED(apDevice->GetPixelShaderConstantF(0, &psRegs[0][0], kSavedPsRegs));

			for (UInt32 stage = 0; stage < kMaxSavedTextures; ++stage)
				for (UInt32 v = 0; v < kSamplerValuesPerStage; ++v)
					apDevice->GetSamplerState(stage, kSavedSamplerStates[v], &samplers[stage][v]);
		}

		void Restore(IDirect3DDevice9* apDevice) {
			NiDX9RenderState* engine = TheRenderManager ? TheRenderManager->renderState : nullptr;

			// Shaders, then the declaration (or the FVF when there was none).
			if (!engine) {
				apDevice->SetVertexShader(vs);
				apDevice->SetPixelShader(ps);
				if (decl) apDevice->SetVertexDeclaration(decl);
				else apDevice->SetFVF(fvfCode);
			}
			else {
				engine->SetVertexShader(vs, false);
				engine->SetPixelShader(ps, false);
				if (decl) engine->SetVertexDeclaration(decl, false);
				else engine->SetFVF(fvfCode, false);
			}

			// Per stage: the texture, then its five sampler states.
			for (UInt32 stage = 0; stage < kMaxSavedTextures; ++stage) {
				if (engine) engine->SetTexture(stage, tex[stage]);
				else apDevice->SetTexture(stage, tex[stage]);
				for (UInt32 v = 0; v < kSamplerValuesPerStage; ++v) {
					if (engine) engine->SetSamplerState(stage, kSavedSamplerStates[v], samplers[stage][v], false);
					else apDevice->SetSamplerState(stage, kSavedSamplerStates[v], samplers[stage][v]);
				}
			}

			// Per state: the engine cache first (which also writes the device), then the device
			// on top. Cull mode is only written through the engine, so its device write is skipped.
			for (UInt32 k = 0; k < kTrackedStateCount; ++k) {
				if (engine)
					engine->SetRenderState(kTrackedStates[k], engineStates[k], RenderStateArgs);
				if (k != kCullModeSlot)
					apDevice->SetRenderState(kTrackedStates[k], deviceStates[k]);
			}

			if (regsSaved) {
				apDevice->SetVertexShaderConstantF(0, &vsRegs[0][0], kSavedVsRegs);
				apDevice->SetPixelShaderConstantF(0, &psRegs[0][0], kSavedPsRegs);
			}

			apDevice->SetIndices(ib);
			for (UInt32 n = 0; n < kMaxSavedStreams; ++n)
				apDevice->SetStreamSource(n, vbs[n].vb, vbs[n].byteOffset, vbs[n].vertexStride);

			// Drop the references the Get calls added, in the order they were taken.
			SafeRelease(vs);
			SafeRelease(ps);
			SafeRelease(decl);
			for (UInt32 stage = 0; stage < kMaxSavedTextures; ++stage) SafeRelease(tex[stage]);
			SafeRelease(ib);
			for (UInt32 n = 0; n < kMaxSavedStreams; ++n) SafeRelease(vbs[n].vb);
		}
	};

	static bool s_enabledThisFrame = false;
	static std::vector<QueuedDraw> s_draws;
	static IDirect3DVertexShader9* s_flatVS = nullptr;
	static IDirect3DPixelShader9* s_flatPS = nullptr;
	static IDirect3DVertexShader9* s_bumpVS = nullptr;
	static IDirect3DPixelShader9* s_bumpPS = nullptr;
	static bool s_shadersBroken = false;

	// Engine RTTI test (NiObject::IsKindOf with a class id).
	static bool IsKind(NiAVObject* apObject, UInt32 auiClassId) {
		if (!apObject) return false;
		return CdeclCall<bool>(kIsKindOfFn, auiClassId, apObject);
	}

	// The shade types this pass re-lights. Every one derives from BSShaderProperty, which is what
	// makes the cast in CaptureGeometry safe. Unlit, distant LOD and tall grass are left out on
	// purpose: an unlit mesh stays unlit, a distant LOD stand-in is hidden behind the real mesh,
	// and grass proxies are lit by their own shader.
	static bool IsRelightableShade(NiShadeProperty::ShaderPropType aeType) {
		switch (aeType) {
		case NiShadeProperty::kProp_Lighting:
		case NiShadeProperty::kProp_PPLighting:
		case NiShadeProperty::kProp_Lighting30:
		case NiShadeProperty::kProp_Hair:
		case NiShadeProperty::kProp_SpeedTreeBranch:
		case NiShadeProperty::kProp_SpeedTreeLeaf:
			return true;
		default:
			return false;
		}
	}

	static IDirect3DBaseTexture9* TextureOf(BSShaderPPLightingProperty* apProperty, UInt32 auiSlot) {
		if (!apProperty || auiSlot >= 6 || !apProperty->ppTextures[auiSlot])
			return nullptr;
		NiSourceTexture* source = *apProperty->ppTextures[auiSlot];
		if (!source || !source->rendererData)
			return nullptr;
		return source->rendererData->dTexture;
	}

	// True when the declaration has an element of this usage at index 0 (the list ends at stream 0xFF).
	static bool DeclHasUsage(IDirect3DVertexDeclaration9* apDecl, BYTE aucUsage) {
		if (!apDecl)
			return false;
		D3DVERTEXELEMENT9 layout[MAXD3DDECLLENGTH];
		UINT used = MAXD3DDECLLENGTH;
		if (FAILED(apDecl->GetDeclaration(layout, &used)))
			return false;
		for (const D3DVERTEXELEMENT9* el = layout; el != layout + used && el->Stream != 0xFF; ++el) {
			if (el->Usage == aucUsage && el->UsageIndex == 0)
				return true;
		}
		return false;
	}

	static bool HasTangentFrame(NiGeometry* apGeometry) {
		NiGeometryData* data = apGeometry ? apGeometry->geomData : nullptr;
		NiGeometryBufferData* buffers = data ? data->m_pkBuffData : nullptr;
		if (!buffers || !buffers->VertexDeclaration)
			return false;
		IDirect3DVertexDeclaration9* decl = buffers->VertexDeclaration;
		return DeclHasUsage(decl, D3DDECLUSAGE_TANGENT) && DeclHasUsage(decl, D3DDECLUSAGE_BINORMAL) &&
			DeclHasUsage(decl, D3DDECLUSAGE_NORMAL);
	}

	static float Clamp(float afValue, float afLo, float afHi) {
		return afValue < afLo ? afLo : (afValue > afHi ? afHi : afValue);
	}

	static NiMaterialProperty* MaterialOf(NiGeometry* apGeometry) {
		return apGeometry ? apGeometry->propertyState.m_spMaterialProperty : nullptr;
	}

	// The material's brightest specular channel, 1 without a material or with a black one.
	static float SpecularScaleOf(NiGeometry* apGeometry) {
		const NiMaterialProperty* mat = MaterialOf(apGeometry);
		if (mat == nullptr) return 1.0f;
		float top = mat->spec.r;
		if (mat->spec.g > top) top = mat->spec.g;
		if (mat->spec.b > top) top = mat->spec.b;
		return top <= 0.001f ? 1.0f : Clamp(top, 0.0f, 4.0f);
	}

	static float SpecularPowerOf(NiGeometry* apGeometry) {
		const NiMaterialProperty* mat = MaterialOf(apGeometry);
		const bool noShine = (mat == nullptr) || mat->fShine <= 0.0f;
		return noShine ? 32.0f : Clamp(mat->fShine, 4.0f, 128.0f);
	}

	// Bounding sphere against the flashlight's sphere of reach. A geometry without a bound falls
	// back to its world position with radius zero.
	static bool InsideLightReach(NiAVObject* apObject) {
		if (!apObject || !TheShaderManager)
			return false;
		const NiBound* sphere = apObject->m_kWorldBound;
		const NiPoint3 mid = sphere ? sphere->Center : apObject->m_worldTransform.pos;
		const float r = sphere ? sphere->Radius : 0.0f;
		const D3DXVECTOR4& lamp = TheShaderManager->SpotLightPosition[0];
		const float reach = lamp.w + r;
		const float dx = mid.x - lamp.x;
		const float dy = mid.y - lamp.y;
		const float dz = mid.z - lamp.z;
		return (dx * dx + dy * dy + dz * dz) <= reach * reach;
	}

	static bool HasAlphaBlendOrTest(NiGeometry* apGeometry) {
		const NiAlphaProperty* ap = apGeometry ? apGeometry->propertyState.m_spAlphaProperty : nullptr;
		const UInt32 alphaBits = NiAlphaProperty::ALPHA_BLEND_MASK | NiAlphaProperty::TEST_ENABLE_MASK;
		return ap != nullptr && (ap->flags & alphaBits) != 0;
	}

	static bool CanQueue(NiGeometry* apGeometry, BSShaderPPLightingProperty* apProperty, IDirect3DBaseTexture9** apDiffuseOut) {
		if (!s_enabledThisFrame || !apGeometry || !apProperty || !apDiffuseOut)
			return false;
		if ((int)s_draws.size() >= Cfg().MaterialLight.MaxGeometry)
			return false;
		if (apGeometry->skinInstance || HasAlphaBlendOrTest(apGeometry))
			return false;
		if ((apGeometry->m_flags & NiAVObject::APP_CULLED) != 0)
			return false;

		const char* geoName = apGeometry->m_pcName;
		if (geoName && strncmp(geoName, "Terrain LOD", 11) == 0)
			return false;

		NiGeometryData* gd = apGeometry->geomData;
		if (!gd || !gd->m_pkBuffData || !gd->m_pkBuffData->VertCount)
			return false;
		if (!InsideLightReach(apGeometry))
			return false;

		IDirect3DBaseTexture9* base = TextureOf(apProperty, 0);
		*apDiffuseOut = base;
		return base != nullptr;
	}

	static void Enqueue(NiGeometry* apGeometry, BSShaderPPLightingProperty* apProperty, IDirect3DBaseTexture9* apBump,
		bool abSpecular, float afSpecStrength, float afSpecPower, bool abBump) {
		IDirect3DBaseTexture9* base = nullptr;
		if (!CanQueue(apGeometry, apProperty, &base))
			return;
		QueuedDraw entry;
		entry.geo = apGeometry;
		entry.baseTex = base;
		entry.bumpTex = abBump ? apBump : nullptr;
		entry.specScale = abSpecular ? afSpecStrength : 0.0f;
		entry.specExp = afSpecPower;
		entry.glossDefault = 0.5f;
		entry.hasBump = abBump && apBump;
		s_draws.push_back(entry);
	}

	static void Walk(NiAVObject* apObject) {
		if (!apObject || *(void**)apObject == nullptr)
			return;
		if ((apObject->m_flags & NiAVObject::APP_CULLED) != 0)
			return;
		// No subtree culling by world bound: the scene root does not keep a bound that encloses
		// its children (it reads as centre zero, radius one), so testing it dropped the whole
		// scene. Each geometry is bound tested in CanQueue instead.

		if (IsKind(apObject, kTypeNiGeometry)) {
			MaterialPass::CaptureGeometry(static_cast<NiGeometry*>(apObject));
			return;
		}
		if (!IsKind(apObject, kTypeNiNode))
			return;

		NiNode* node = static_cast<NiNode*>(apObject);
		const UInt32 childCount = node->m_children.numObjs;
		if (node->m_children.data == nullptr || childCount == 0)
			return;

		// A switch node shows one child only: follow just that one (none for a negative index).
		if (IsKind(apObject, kTypeNiSwitchNode)) {
			const int shown = static_cast<NiSwitchNode*>(apObject)->m_iIndex;
			if (shown >= 0 && (UInt32)shown < childCount)
				Walk(node->m_children.data[shown]);
			return;
		}

		for (UInt32 c = 0; c < childCount; ++c)
			Walk(node->m_children.data[c]);
	}

	static bool Compile(const char* apSource, const char* apEntry, const char* apProfile, ID3DXBuffer** apOut) {
		ID3DXBuffer* errors = nullptr;
		const HRESULT result = D3DXCompileShader(apSource, (UINT)strlen(apSource), nullptr, nullptr, apEntry, apProfile, 0, apOut, &errors, nullptr);
		SafeRelease(errors);
		return SUCCEEDED(result);
	}

	static bool EnsureShaders(IDirect3DDevice9* apDevice) {
		if (s_flatVS && s_flatPS && s_bumpVS && s_bumpPS)
			return true;
		if (s_shadersBroken || !apDevice)
			return false;

		ID3DXBuffer* code[4] = {};
		// Short circuit on purpose: after the first failure the rest are not attempted.
		const bool built =
			Compile(kFlatVertexSrc, "main", "vs_3_0", &code[0]) &&
			Compile(kFlatPixelSrc, "main", "ps_3_0", &code[1]) &&
			Compile(kBumpVertexSrc, "main", "vs_3_0", &code[2]) &&
			Compile(kBumpPixelSrc, "main", "ps_3_0", &code[3]);
		if (!built) {
			s_shadersBroken = true;
			Logger::Log("[MaterialPass] shader COMPILE failed");
			for (ID3DXBuffer*& c : code) SafeRelease(c);
			return false;
		}

		auto words = [&](int i) { return (const DWORD*)code[i]->GetBufferPointer(); };
		const HRESULT r0 = apDevice->CreateVertexShader(words(0), &s_flatVS);
		const HRESULT r1 = apDevice->CreatePixelShader(words(1), &s_flatPS);
		const HRESULT r2 = apDevice->CreateVertexShader(words(2), &s_bumpVS);
		const HRESULT r3 = apDevice->CreatePixelShader(words(3), &s_bumpPS);
		for (ID3DXBuffer*& c : code) SafeRelease(c);

		if (SUCCEEDED(r0) && SUCCEEDED(r1) && SUCCEEDED(r2) && SUCCEEDED(r3))
			return true;
		SafeRelease(s_flatVS);
		SafeRelease(s_flatPS);
		SafeRelease(s_bumpVS);
		SafeRelease(s_bumpPS);
		s_shadersBroken = true;
		return false;
	}

	static void SetupEngineState() {
		// Set through NiDX9RenderState, not the device. The engine's draw path re-applies its own
		// cache, so device-only writes get overwritten part way through the pass. Measured: with the
		// depth compare left on the device-side value the pass drew through walls, and dropping the
		// stage 1 normal map flattened the highlights. DeviceSnapshot is what makes this safe.
		NiDX9RenderState* engine = TheRenderManager->renderState;

		// The pass redraws geometry the engine already drew, so its fragments land on depths that
		// should compare equal. Only an inclusive compare lets them through. The compare is set
		// explicitly because the engine leaves a different one behind after sky and particles.
		//
		// LESSEQUAL is the measured choice and contradicts what the surrounding code predicts: on
		// the setup this was built on, IsReversedDepth() is true, the Z clear is 0, and the world
		// render leaves GREATEREQUAL, which by the ShadowManager derivation would be right. With
		// GREATEREQUAL this pass draws occluded geometry through walls; LESSEQUAL respects occlusion.
		// The disagreement is not understood, so the measured value stands. Untested on a
		// non-inverted buffer. If nothing draws there, flip this line first.
		//
		// DebugMode 8 drops the depth test entirely, so every queued object is drawn through whatever
		// is in front of it. That tells "never captured" apart from "captured and then failed depth".
		const bool xray = (Cfg().MaterialLight.DebugMode == kXRayDebugMode);
		const DWORD zeroBias = 0;	// float 0.0 has the bit pattern zero
		engine->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL, RenderStateArgs);
		engine->SetRenderState(D3DRS_DEPTHBIAS, zeroBias, RenderStateArgs);

		engine->SetVertexShader(s_flatVS, false);
		engine->SetPixelShader(s_flatPS, false);

		// Every state here can reject a fragment or change how the blend combines, so the pass owns
		// all of them rather than inheriting whatever the engine left. Colour write is limited to
		// RGB: the engine masks alpha off during the world render, and forcing it on made this pass
		// add 1.0 to alpha at every touched pixel, about 126 draws deep, into a non-clamping float target.
		const DWORD zEnable = xray ? (DWORD)D3DZB_FALSE : (DWORD)D3DZB_TRUE;
		const DWORD rgbOnly = D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE;
		const StateValue states[] = {
			{ D3DRS_ZENABLE, zEnable },
			{ D3DRS_ZWRITEENABLE, D3DZB_FALSE },
			{ D3DRS_ALPHABLENDENABLE, TRUE },
			{ D3DRS_SRCBLEND, D3DBLEND_ONE },
			{ D3DRS_DESTBLEND, D3DBLEND_ONE },
			{ D3DRS_ALPHATESTENABLE, FALSE },
			{ D3DRS_STENCILENABLE, FALSE },
			{ D3DRS_COLORWRITEENABLE, rgbOnly },
			{ D3DRS_FOGENABLE, FALSE },
			{ D3DRS_SCISSORTESTENABLE, FALSE },
			{ D3DRS_CLIPPLANEENABLE, 0 },
			// Setting only SRC and DEST describes an additive blend if the operator is ADD and no
			// separate alpha path is armed. Both were inherited before, and MULTISAMPLEMASK decides
			// which samples of the 8x target this pass may touch.
			{ D3DRS_BLENDOP, D3DBLENDOP_ADD },
			{ D3DRS_SEPARATEALPHABLENDENABLE, FALSE },
			{ D3DRS_MULTISAMPLEMASK, 0xFFFFFFFF },
			{ D3DRS_SLOPESCALEDEPTHBIAS, 0 },
		};
		for (const StateValue& entry : states)
			engine->SetRenderState(entry.state, entry.value, RenderStateArgs);

		// Wrap addressing and linear filtering on both stages the pass samples.
		const DWORD samplerValues[kSamplerValuesPerStage] = { D3DTADDRESS_WRAP, D3DTADDRESS_WRAP, D3DTEXF_LINEAR, D3DTEXF_LINEAR, D3DTEXF_LINEAR };
		for (UInt32 stage = 0; stage < 2; ++stage)
			for (UInt32 v = 0; v < kSamplerValuesPerStage; ++v)
				engine->SetSamplerState(stage, kSavedSamplerStates[v], samplerValues[v], false);
	}

	// The geometry's own cull mode (from its stencil property), counter-clockwise without one.
	static void ApplyCullFor(NiGeometry* apGeometry) {
		NiDX9RenderState* rs = TheRenderManager->renderState;
		NiStencilProperty* sp = apGeometry ? apGeometry->propertyState.m_spStencilProperty : nullptr;
		if (sp == nullptr)
			rs->SetRenderState(D3DRS_CULLMODE, D3DCULL_CCW, RenderStateArgs);
		else
			rs->SetCullMode(sp->GetDrawMode());
	}

	static bool Draw(NiGeometry* apGeometry) {
		NiGeometryData* gd = apGeometry ? apGeometry->geomData : nullptr;
		if (gd == nullptr)
			return false;
		NiGeometryBufferData* buffers = gd->m_pkBuffData;
		if (!buffers || !buffers->VertCount)
			return false;
		if (buffers->StreamCount > kMaxSavedStreams || !buffers->VertexStride)
			return false;

		IDirect3DDevice9* dev = TheRenderManager->device;
		for (UInt32 n = 0; n < buffers->StreamCount; ++n) {
			if (!buffers->VBChip || !buffers->VBChip[n] || !buffers->VBChip[n]->VB)
				return false;
			dev->SetStreamSource(n, buffers->VBChip[n]->VB, 0, buffers->VertexStride[n]);
		}

		if (buffers->IB)
			dev->SetIndices(buffers->IB);
		NiDX9RenderState* rs = TheRenderManager->renderState;
		if (buffers->FVF)
			rs->SetFVF(buffers->FVF, false);
		else if (buffers->VertexDeclaration)
			rs->SetVertexDeclaration(buffers->VertexDeclaration, false);
		else
			return false;

		// The engine's draw routine may touch the dirty flags, so they are put back afterwards.
		const UInt16 dirtyBefore = gd->m_usDirtyFlags;
		const UInt32 drawFn = gd->IsStripsData() ? kTriStripDraw : kTriShapeDraw;
		ThisCall(drawFn, NiDX9Renderer::GetSingleton(), apGeometry);
		gd->m_usDirtyFlags = dirtyBefore;
		return true;
	}
}

void MaterialPass::CaptureGeometry(NiGeometry* apGeometry) {
	if (!s_enabledThisFrame || !apGeometry)
		return;
	if (*(void**)apGeometry == nullptr || !IsKind(static_cast<NiAVObject*>(apGeometry), kTypeNiGeometry))
		return;

	NiShadeProperty* shade = apGeometry->propertyState.m_spShadeProperty;
	if (shade == nullptr || !IsRelightableShade(shade->m_eShaderType))
		return;
	// Only PPLighting geometry is ever queued (CanQueue needs its diffuse texture slot), so
	// anything else has nothing to add here.
	if (shade->m_eShaderType != NiShadeProperty::kProp_PPLighting)
		return;

	BSShaderProperty* bsProp = static_cast<BSShaderProperty*>(shade);
	const UInt32 flagsLo = bsProp->ulFlags[0];
	const UInt32 flagsHi = bsProp->ulFlags[1];
	const bool hasSpecular = (flagsLo & BSShaderProperty::Specular) != 0;
	const bool bumpAllowed = (flagsHi & BSShaderProperty::Skip_Normal_Maps) == 0;
	const bool objectSpaceNormals = (flagsLo & BSShaderProperty::Model_Space_Normals) != 0;
	const float specScale = hasSpecular ? SpecularScaleOf(apGeometry) : 0.0f;
	const float specExp = SpecularPowerOf(apGeometry);

	BSShaderPPLightingProperty* lit = static_cast<BSShaderPPLightingProperty*>(bsProp);
	IDirect3DBaseTexture9* bumpTex = TextureOf(lit, 1);
	const bool withBump = bumpAllowed && !objectSpaceNormals && HasTangentFrame(apGeometry) && bumpTex != nullptr;

	Enqueue(apGeometry, lit, bumpTex, hasSpecular, specScale, specExp, withBump);
}

void MaterialPass::CaptureScene(SceneGraph* apSceneGraph) {
	if (s_enabledThisFrame && apSceneGraph)
		Walk(apSceneGraph);
}

void MaterialPass::BeginFrame(bool abActive) {
	s_draws.clear();
	const auto& ml = Cfg().MaterialLight;
	s_enabledThisFrame = abActive && ml.Enabled && ml.Intensity > 0.0f && ml.MaxGeometry > 0;
}

void MaterialPass::RenderWorld() {
	if (!s_enabledThisFrame || s_draws.empty() || !TheRenderManager || !TheShaderManager)
		return;

	IDirect3DDevice9* dev = TheRenderManager->device;
	if (!EnsureShaders(dev))
		return;

	DeviceSnapshot saved{};
	saved.Save(dev);
	SetupEngineState();

	// Light position and direction, relative to the camera like the vertex stage expects.
	const D3DXVECTOR4& cam = TheRenderManager->CameraPosition;
	D3DXVECTOR4 lampPos = TheShaderManager->SpotLightPosition[0];
	lampPos.x -= cam.x;
	lampPos.y -= cam.y;
	lampPos.z -= cam.z;

	const D3DXVECTOR4& rawDir = TheShaderManager->SpotLightDirection[0];
	D3DXVECTOR4 lampDir = rawDir;
	D3DXVec3Normalize((D3DXVECTOR3*)&lampDir, (D3DXVECTOR3*)&lampDir);
	lampDir.w = rawDir.w;

	// The light's dimmer and the night boost go into the colour here, as in the screen space
	// pass, so this pass follows the flashlight without a separate rebalance. The dimmer goes
	// in rgb, not w, because w only reaches the diffuse term and specular reads rgb alone.
	// MaterialLight.Intensity stays in w as this pass's own diffuse strength.
	const D3DXVECTOR4& sunCol = TheShaderManager->ShaderConst.sunColor;
	const float invSunLuma = 1.0f / max(0.05f, sunCol.x * 0.2126f + sunCol.y * 0.7152f + sunCol.z * 0.0722f);
	const D3DXVECTOR4& rawColor = TheShaderManager->SpotLightColor[0];
	const float tint = rawColor.w * invSunLuma;
	const D3DXVECTOR4 lampColor(rawColor.x * tint, rawColor.y * tint, rawColor.z * tint, Cfg().MaterialLight.Intensity);

	// X-ray reuses debug mode 1's flat colour, so the shaders need no branch for it.
	const int mode = Cfg().MaterialLight.DebugMode;
	const float shaderMode = (float)(mode == kXRayDebugMode ? 1 : mode);
	const D3DXVECTOR4 tuningRegister(Cfg().NearFade, shaderMode, 0.0f, 0.0f);

	// Shared by every item, so set once. The vertex stage pulls each position a little toward
	// the camera before projecting it. The pass redraws geometry the engine already drew, and
	// our projection's low bits disagree with the engine's, so the depth tie is lost about half
	// the time. A D3D depth bias is the wrong tool: it is constant in non-linear depth, so a
	// value big enough to help up close reaches through walls far away. The positions here are
	// camera relative, so scaling them is a proportional pull along the view ray: small at every
	// range, and indifferent to which way the depth buffer runs. Only the projected position
	// moves; worldRel keeps the true position so the lighting is unchanged.
	//
	// 0.9997 (a 0.0003 pull) is measured, not derived. Arithmetic says 0.00001 is far above the
	// float error, and it is not. Swept with MaterialLight DebugMode 2 across a wall:
	//
	//     0.00001  flickers          0.0001  clean
	//     0.00003  flickers          0.001   clean
	//
	// The threshold sits between 0.00003 and 0.0001, about ten times the arithmetic, so the
	// error is bigger than the two step matrix multiply accounts for. 0.0003 is three times
	// that threshold. At the beam's MaxDistance of 1500 it moves geometry 0.45 units, against
	// 1.5 units at the 0.001 this replaced.
	//
	// Do not lower it on arithmetic alone: that is how 0.00001 shipped, and its flicker is
	// faint enough to pass a quick look.
	//
	// A wall poster in one interior once looked tied to this offset, because changing its size
	// changed how widely the poster vanished. It was not the cause. SavedDeviceState read render
	// states from the device and restored them into NiDX9RenderState, which repaired a
	// STENCILENABLE mismatch the engine relied on and started real stencil tests on decals.
	// That is why the pull was ten thousand times larger than it needed to be.
	//
	// Whatever value it takes, the offset must stay: removing it brings the flicker back, and so
	// does matching the engine's single concatenated world-view-projection exactly. The
	// camera-relative form used here is equivalent to the engine's absolute one, not bit identical.
	dev->SetVertexShaderConstantF(0, (const float*)&TheRenderManager->ViewProjMatrix, 4);

	// Pixel registers c0-c3 and c6 (c4 is per item, c5 unused).
	const D3DXVECTOR4 hotspotRegister(Cfg().HotspotLimit, 0.0f, 0.0f, 0.0f);
	dev->SetPixelShaderConstantF(0, (const float*)&lampColor, 1);
	dev->SetPixelShaderConstantF(1, (const float*)&lampDir, 1);
	dev->SetPixelShaderConstantF(2, (const float*)&tuningRegister, 1);
	dev->SetPixelShaderConstantF(3, (const float*)&lampPos, 1);
	dev->SetPixelShaderConstantF(6, (const float*)&hotspotRegister, 1);

	// Constant for the whole call, so read once instead of once per queued item.
	const float specMul = Cfg().MaterialLight.Specular;
	const float bumpStrength = Cfg().MaterialLight.NormalStrength;
	NiDX9RenderState* rs = TheRenderManager->renderState;

	for (const QueuedDraw& d : s_draws) {
		if (d.geo == nullptr || d.baseTex == nullptr)
			continue;
		const bool bumped = d.hasBump && d.bumpTex;

		D3DXMATRIX world;
		TheRenderManager->CreateD3DMatrix(&world, &d.geo->m_worldTransform);
		dev->SetVertexShaderConstantF(4, (const float*)&world, 4);

		const D3DXVECTOR4 specRegister(d.specScale * specMul, d.specExp, d.glossDefault, bumpStrength);
		dev->SetPixelShaderConstantF(4, (const float*)&specRegister, 1);

		rs->SetVertexShader(bumped ? s_bumpVS : s_flatVS, false);
		rs->SetPixelShader(bumped ? s_bumpPS : s_flatPS, false);
		rs->SetTexture(0, d.baseTex);
		rs->SetTexture(1, bumped ? d.bumpTex : nullptr);
		ApplyCullFor(d.geo);
		Draw(d.geo);
	}

	saved.Restore(dev);
}
