#pragma once

// UNOFFICIAL 2026-10-30 VisibleRanking ([Shaders.ShadowsInteriors.Forward] VisibleRanking, off by default; user: "shadows pop in and out
// of existence again seemingly randomly when we walk around in cells with a lot of lights like Vault 21", also at PoolLamps 0 / 128 and
// LightPoints 64).
// The live lamp-shadow slots went to the lamps ranked first by brightness x reach / distance round the player (CameraIndependent: no view
// test at all), with no idea what is visible: Vault 21 holds hundreds of lamps within reach, most of them lighting rooms behind walls, so
// the slots went to hidden lamps while the ones lighting what the player looks at took turns.
// Here each candidate lamp's light is measured against the picture itself: after the depth combine, a 160 x 90 pass per lamp keeps the
// pixels whose surface (TESR_DepthBuffer, so walls in between hide what is behind them) lies within the lamp's reach, inside an occlusion
// query. UNOFFICIAL 2026-10-30 (user: a partly hidden lamp lost its shadow while its shadow was still in view): each pixel counts as much
// as the lamp lights it ((1 - d^2 / r^2)^2, kept against a per-pixel dither), so a lamp that lights a visible ceiling strongly ranks above
// one whose reach only grazes a large far wall; the count is then that lamp's light on the visible picture, not its reach's area. The answer (read a few frames later, without a flush: HiddenObjectCulling's way) is how much of the visible picture that lamp
// lights. ShaderManager::GetNearbyLights then ranks by brightness x that share: lamps that light nothing visible go last. LampSwitchMargin
// still favours the lamps that have a slot, and with ShadowHold above 0 a lamp keeps its slot for that many seconds after it last ranked
// inside the slots (while it still lights something visible).
// At most PerFrame lamps are measured a frame (round robin over the candidates, nearest-ranked first); an answer stays in use for MaxAge
// frames. Costs ~PerFrame tiny draws and queries a frame while on; nothing while off.
#include <d3d9.h>
#include <d3dx9.h>
#include <unordered_map>
#include "GpuProfiler.h"
#include <vector>

namespace LampVisibility {

	constexpr int Slots = 4, PerFrame = 128, W = 160, H = 90;
	constexpr UInt32 MaxAge = 30;	// frames an answer is used for (lamps are measured every few frames)

	struct Lamp {
		IDirect3DQuery9* query[Slots] = {};
		UInt32 issued[Slots] = {};
		UInt32 answerFrame = 0;		// the frame the newest answer was issued in
		DWORD pixels = 0;			// of W x H (the newest answer)
		float smooth = -1.0f;		// the answers smoothed (a quarter of each new one), -1 = none yet
		UInt32 requested = 0;		// last frame it was a candidate
		double lastInside = -1e9;	// ShadowHold: when it last ranked inside the live slots
	};
	inline std::unordered_map<const void*, Lamp> Lamps;
	struct Want { const void* light; D3DXVECTOR3 position; float radius; };
	inline std::vector<Want> Wanted;	// this frame's lamps to measure (GetNearbyLights)
	inline size_t Cursor = 0;			// round robin start in the candidate list past the first PerFrame / 2
	inline UInt32 Frame = 1;
	inline bool On = false, Failed = false;
	inline IDirect3DTexture9* Target = nullptr; inline IDirect3DSurface9* TargetSurface = nullptr;
	inline IDirect3DPixelShader9* PS = nullptr;
	inline unsigned StatFrames = 0; inline double StatMeasured = 0, StatAnswered = 0, StatVisible = 0, StatCandidates = 0;

	// 0 off, 1 on, 2 measured only (the ranking unchanged; the SHADOW FLIPS line then splits the flips of lamps that light something visible).
	inline int Setting() { return TheSettingManager->GetSettingI("Shaders.ShadowsInteriors.Forward", "VisibleRanking"); }

	// The pixels of the visible picture within this lamp's reach, of W x H, smoothed over its last answers; -1 = no answer yet (or too old).
	inline float Pixels(const void* light) {
		auto f = Lamps.find(light);
		if (f == Lamps.end() || !f->second.answerFrame || Frame - f->second.answerFrame > MaxAge || f->second.smooth < 0.0f) return -1.0f;
		return f->second.smooth;
	}

	// GetNearbyLights: the candidates in ranking order (it calls this once a frame, before choosing). Collects newest answers.
	inline void Request(const std::vector<Want>& candidates) {
		Frame++;
		Wanted.clear();
		const size_t first = (std::min)(candidates.size(), (size_t)PerFrame / 2);
		for (size_t i = 0; i < first; i++) Wanted.push_back(candidates[i]);
		const size_t rest = candidates.size() - first;
		for (size_t k = 0; k < (std::min)(rest, (size_t)(PerFrame - first)); k++) Wanted.push_back(candidates[first + (Cursor + k) % rest]);
		if (rest) Cursor = (Cursor + (PerFrame - first)) % rest;
		if (GpuTimer::Enabled) StatCandidates += candidates.size();
		// the newest answers that are in (no flush), for every lamp still around
		for (auto it = Lamps.begin(); it != Lamps.end();) {
			Lamp& l = it->second;
			for (UInt32 age = 1; age < Slots; age++) {
				const int slot = (Frame - age) % Slots;
				if (!l.query[slot] || l.issued[slot] != Frame - age || l.issued[slot] <= l.answerFrame) continue;
				DWORD pixels = 0;
				if (l.query[slot]->GetData(&pixels, sizeof(pixels), 0) != S_OK) continue;
				l.pixels = pixels; l.answerFrame = l.issued[slot];
				l.smooth = l.smooth < 0.0f ? (float)pixels : l.smooth + 0.25f * ((float)pixels - l.smooth);
				if (GpuTimer::Enabled) { StatAnswered += 1; if (pixels) StatVisible += 1; }
				break;
			}
			if (Frame - l.requested > 600) {
				for (IDirect3DQuery9*& q : l.query) if (q) { q->Release(); q = nullptr; }
				it = Lamps.erase(it);
			}
			else ++it;
		}
		for (const Want& w : candidates) Lamps[w.light].requested = Frame;
	}

	inline bool Compile(IDirect3DDevice9* d) {
		if (PS) return true;
		// c0 xyz the lamp (camera relative), w its reach; c1 xyz camera right, w 1 / P11; c2 xyz camera up, w 1 / P22; c3 xyz forward,
		// w far; c4 xy RenderScale's rect (the depth texture's share drawn).
		static const char* kPS =
			"sampler2D Z : register(s0); float4 L : register(c0); float4 R : register(c1); float4 U : register(c2); float4 F : register(c3); float4 S : register(c4);"
			"float4 main(float2 uv : TEXCOORD0) : COLOR0 {"
			"  float z = tex2D(Z, uv * S.xy).x * F.w;"
			"  float3 p = (F.xyz + (R.w * (2.0f * uv.x - 1.0f)) * R.xyz + (-U.w * (2.0f * uv.y - 1.0f)) * U.xyz) * z;"
			"  float3 d = p - L.xyz;"
			"  float a = saturate(1.0f - dot(d, d) / (L.w * L.w)); a *= a;"
			"  float2 q = uv * float2(160.0f, 90.0f);"
			"  float n = frac(52.9829189f * frac(dot(q, float2(0.06711056f, 0.00583715f))));"
			"  clip(z < F.w * 0.995f && a > n ? 1.0f : -1.0f);"
			"  return 0.0f; }";
		ID3DXBuffer* code = nullptr; ID3DXBuffer* errors = nullptr;
		const bool ok = SUCCEEDED(D3DXCompileShader(kPS, (UINT)strlen(kPS), NULL, NULL, "main", "ps_2_0", 0, &code, &errors, NULL)) && code &&
			SUCCEEDED(d->CreatePixelShader((const DWORD*)code->GetBufferPointer(), &PS));
		if (!ok) Logger::Log("[ERROR] VisibleRanking: the shader failed (%s): off for the session", errors ? (const char*)errors->GetBufferPointer() : "?");
		if (code) code->Release();
		if (errors) errors->Release();
		if (!ok) Failed = true;
		return ok;
	}

	// ShaderManager::RenderEffectsPreTonemapping, after the depth combine: one query'd pass per wanted lamp.
	inline void Render(IDirect3DDevice9* d, IDirect3DTexture9* depth) {
		if (!On || Failed || !depth || Wanted.empty()) return;
		if (!Target) {
			if (FAILED(d->CreateTexture(W, H, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &Target, NULL)) || FAILED(Target->GetSurfaceLevel(0, &TargetSurface))) {
				Logger::Log("[ERROR] VisibleRanking: could not make its %dx%d target: off for the session", W, H);
				if (Target) { Target->Release(); Target = nullptr; }
				Failed = true;
				return;
			}
		}
		if (!Compile(d)) return;
		auto constant = [](const char* name) -> const D3DXVECTOR4* {
			auto found = TheShaderManager->ConstantsTable.find(name);
			return found == TheShaderManager->ConstantsTable.end() ? nullptr : found->second;
		};
		const D3DXVECTOR4* scale = constant("TESR_RenderScale");
		const D3DXVECTOR4* camera = constant("TESR_CameraData");
		const float farZ = camera ? camera->y : TheRenderManager->CameraData.y;

		NiDX9RenderState* state = TheRenderManager->renderState;
		IDirect3DVertexShader9* vs = nullptr; IDirect3DPixelShader9* ps = nullptr; IDirect3DVertexDeclaration9* decl = nullptr; DWORD fvf = 0;
		IDirect3DSurface9* rt0 = nullptr; IDirect3DSurface9* zs = nullptr; IDirect3DBaseTexture9* tex0 = nullptr; D3DVIEWPORT9 vp = {};
		d->GetVertexShader(&vs); d->GetPixelShader(&ps); d->GetVertexDeclaration(&decl); d->GetFVF(&fvf);
		d->GetRenderTarget(0, &rt0); d->GetDepthStencilSurface(&zs); d->GetTexture(0, &tex0); d->GetViewport(&vp);
		static const D3DRENDERSTATETYPE kRS[] = { D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_ALPHABLENDENABLE, D3DRS_ALPHATESTENABLE, D3DRS_STENCILENABLE,
			D3DRS_SCISSORTESTENABLE, D3DRS_CULLMODE, D3DRS_COLORWRITEENABLE, D3DRS_FILLMODE, D3DRS_SRGBWRITEENABLE, D3DRS_FOGENABLE, D3DRS_CLIPPLANEENABLE };
		const DWORD want[] = { FALSE, FALSE, FALSE, FALSE, FALSE, FALSE, D3DCULL_NONE, 0u, D3DFILL_SOLID, FALSE, FALSE, 0 };
		DWORD savedRS[sizeof(kRS) / sizeof(kRS[0])];
		for (unsigned i = 0; i < sizeof(kRS) / sizeof(kRS[0]); i++) { if (FAILED(d->GetRenderState(kRS[i], &savedRS[i]))) savedRS[i] = want[i]; if (savedRS[i] != want[i]) d->SetRenderState(kRS[i], want[i]); }
		static const D3DSAMPLERSTATETYPE kSS[] = { D3DSAMP_MINFILTER, D3DSAMP_MAGFILTER, D3DSAMP_MIPFILTER, D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV, D3DSAMP_SRGBTEXTURE };
		const DWORD wantS[] = { D3DTEXF_POINT, D3DTEXF_POINT, D3DTEXF_NONE, D3DTADDRESS_CLAMP, D3DTADDRESS_CLAMP, FALSE };
		DWORD savedS[sizeof(kSS) / sizeof(kSS[0])];
		for (unsigned i = 0; i < sizeof(kSS) / sizeof(kSS[0]); i++) { if (FAILED(d->GetSamplerState(0, kSS[i], &savedS[i]))) savedS[i] = wantS[i]; if (savedS[i] != wantS[i]) d->SetSamplerState(0, kSS[i], wantS[i]); }
		float savedPS[5][4] = {};
		d->GetPixelShaderConstantF(0, &savedPS[0][0], 5);

		d->SetRenderTarget(0, TargetSurface);
		d->SetDepthStencilSurface(NULL);
		const D3DVIEWPORT9 full = { 0, 0, W, H, 0.0f, 1.0f };
		d->SetViewport(&full);
		state->SetVertexShader(NULL, false);
		state->SetPixelShader(PS, false);
		d->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
		d->SetTexture(0, depth);
		const D3DXMATRIX& inv = TheRenderManager->InvViewMatrix;
		const float invP11 = fabsf(TheRenderManager->InvProjMatrix._11), invP22 = fabsf(TheRenderManager->InvProjMatrix._22);
		const float c[4][4] = {
			{ inv._11, inv._12, inv._13, invP11 }, { inv._21, inv._22, inv._23, invP22 }, { inv._31, inv._32, inv._33, farZ },
			{ scale ? scale->x : 1.0f, scale ? scale->y : 1.0f, 0.0f, 0.0f } };
		d->SetPixelShaderConstantF(1, &c[0][0], 4);
		struct V { float x, y, z, w, u, v; };
		const V quad[4] = { { -0.5f, -0.5f, 0, 1, 0, 0 }, { W - 0.5f, -0.5f, 0, 1, 1, 0 }, { -0.5f, H - 0.5f, 0, 1, 0, 1 }, { W - 0.5f, H - 0.5f, 0, 1, 1, 1 } };
		const D3DXVECTOR4& cam = TheRenderManager->CameraPosition;
		const int slot = Frame % Slots;
		for (const Want& w : Wanted) {
			Lamp& l = Lamps[w.light];
			if (!l.query[slot] && FAILED(d->CreateQuery(D3DQUERYTYPE_OCCLUSION, &l.query[slot]))) { l.query[slot] = nullptr; continue; }
			const float lc[4] = { w.position.x - cam.x, w.position.y - cam.y, w.position.z - cam.z, w.radius };
			d->SetPixelShaderConstantF(0, lc, 1);
			l.query[slot]->Issue(D3DISSUE_BEGIN);
			d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(V));
			l.query[slot]->Issue(D3DISSUE_END);
			l.issued[slot] = Frame;
			if (GpuTimer::Enabled) StatMeasured += 1;
		}

		for (unsigned i = 0; i < sizeof(kSS) / sizeof(kSS[0]); i++) if (savedS[i] != wantS[i]) d->SetSamplerState(0, kSS[i], savedS[i]);
		for (unsigned i = 0; i < sizeof(kRS) / sizeof(kRS[0]); i++) if (savedRS[i] != want[i]) d->SetRenderState(kRS[i], savedRS[i]);
		d->SetPixelShaderConstantF(0, &savedPS[0][0], 5);
		if (rt0) { d->SetRenderTarget(0, rt0); rt0->Release(); }
		d->SetDepthStencilSurface(zs); if (zs) zs->Release();
		d->SetViewport(&vp);
		d->SetTexture(0, tex0); if (tex0) tex0->Release();
		if (decl) { d->SetVertexDeclaration(decl); decl->Release(); } else d->SetFVF(fvf);
		state->SetVertexShader(vs, false); if (vs) vs->Release();
		state->SetPixelShader(ps, false); if (ps) ps->Release();

		if (GpuTimer::Enabled && ++StatFrames >= 240) {
			const double f = 1.0 / StatFrames;
			Logger::Log("VISIBLE RANKING per frame: %.1f candidate lamps, %.1f measured, %.1f answers in (%.1f lighting something visible)",
				StatCandidates * f, StatMeasured * f, StatAnswered * f, StatVisible * f);
			StatFrames = 0; StatCandidates = StatMeasured = StatAnswered = StatVisible = 0;
		}
	}
}
