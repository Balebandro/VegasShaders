#pragma once

// UNOFFICIAL 2026-10-29 GlassMask ([Shaders.Reflections.Main] GlassReflect, docs/next/FORK-RT-REFLECTIONS.md "Glass"): which pixels show
// see-through glass, so the Reflections effect can reflect on windows, bottles and shop glass in any orientation (its floor guess only
// takes surfaces facing up, and glass has no material data). FNV draws glass in the world scene's blended stage (RenderBlendedHook,
// 0xB650C0) alpha blended WITH depth writes, so the depth buffer, and the normals rebuilt from it, already hold the glass surface; only
// "this pixel is glass" is missing. Here, inside that stage, each see-through draw (alpha blended, depth written, not a cut-out, not
// water: SandstormGlass::IsSeeThroughPass's rules) is drawn once more, right after itself, into a screen-sized mask target with a
// one-instruction pixel shader writing 1 (the game's own z test against the depth the glass just wrote: NVR may run inverted depth,
// so never a fixed LESSEQUAL; no blending, no depth write), so the mask holds the nearest glass where it is what the camera sees. The game's MSAA: the mask is multisampled like the scene and resolved
// after the stage. Cost: one extra draw per glass draw (glass is a few draws a frame) plus its state changes.
// Off (GlassReflect 0, the default): nothing hooked, no target, no draw.
#include <d3d9.h>
#include <d3dx9.h>
#include <vector>

namespace GlassMask {
	inline bool Failed = false;
	inline bool FrameCleared = false;			// cleared in this world scene
	inline bool Written = false;				// a glass draw wrote it this frame
	inline IDirect3DTexture9* Texture = nullptr;  inline IDirect3DSurface9* TextureSurface = nullptr;	// what Reflections reads (TESR_GlassMask)
	inline IDirect3DSurface9* MsaaSurface = nullptr;	// with the game's MSAA: what the redraws write, resolved into Texture
	inline D3DSURFACE_DESC Made = {};
	inline IDirect3DPixelShader9* PS2 = nullptr;  inline IDirect3DPixelShader9* PS3 = nullptr;	// "return 1" for vs_2_x / vs_3_0
	inline UInt32 Draws = 0, Frames = 0;

	inline bool Wanted() {
		ReflectionsEffect* reflections = TheShaderManager->Effects.Reflections;
		return !Failed && reflections && reflections->Enabled && reflections->Settings.GlassReflect > 0.0f;
	}

	inline void Release() {
		if (TextureSurface) { TextureSurface->Release(); TextureSurface = nullptr; }
		if (Texture) { Texture->Release(); Texture = nullptr; }
		if (MsaaSurface) { MsaaSurface->Release(); MsaaSurface = nullptr; }
		Made = {};
	}

	inline IDirect3DPixelShader9* Compile(IDirect3DDevice9* d, const char* profile) {
		static const char kSource[] = "float4 main() : COLOR0 { return float4(1.0, 1.0, 1.0, 1.0); }";
		ID3DXBuffer* code = nullptr;
		ID3DXBuffer* errors = nullptr;
		IDirect3DPixelShader9* ps = nullptr;
		if (SUCCEEDED(D3DXCompileShader(kSource, (UINT)strlen(kSource), NULL, NULL, "main", profile, 0, &code, &errors, NULL)) && code)
			d->CreatePixelShader((const DWORD*)code->GetBufferPointer(), &ps);
		if (code) code->Release();
		if (errors) errors->Release();
		return ps;
	}

	// The mask the size and sampling of the scene target the glass draws into (made again when that changes). False: off for the session.
	inline bool Ensure(IDirect3DDevice9* d, IDirect3DSurface9* sceneTarget) {
		D3DSURFACE_DESC t = {};
		if (!sceneTarget || FAILED(sceneTarget->GetDesc(&t))) return false;
		if (Texture && Made.Width == t.Width && Made.Height == t.Height && Made.MultiSampleType == t.MultiSampleType) return true;
		Release();
		if (!PS2) PS2 = Compile(d, "ps_2_0");
		if (!PS3) PS3 = Compile(d, "ps_3_0");
		bool ok = PS2 && PS3 &&
			SUCCEEDED(d->CreateTexture(t.Width, t.Height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &Texture, NULL)) &&
			SUCCEEDED(Texture->GetSurfaceLevel(0, &TextureSurface));
		if (ok && t.MultiSampleType != D3DMULTISAMPLE_NONE)
			ok = SUCCEEDED(d->CreateRenderTarget(t.Width, t.Height, D3DFMT_A8R8G8B8, t.MultiSampleType, t.MultiSampleQuality, FALSE, &MsaaSurface, NULL));
		if (!ok) {
			Release();
			Failed = true;
			Logger::Log("[WARNING] GlassMask: the glass mask could not be made; glass reflections are off for the session.");
			return false;
		}
		Made = t;
		return true;
	}

	inline IDirect3DSurface9* DrawSurface() { return MsaaSurface ? MsaaSurface : TextureSurface; }

	// A world scene begins: the mask is cleared before its first glass draw (lazily, in BeforeStage).
	inline void BeginWorld() { FrameCleared = false; Written = false; }

	// The blended stage begins (world scene, world depth): make and clear the mask once per scene.
	inline bool BeforeStage(IDirect3DDevice9* d) {
		IDirect3DSurface9* rt0 = nullptr;
		if (FAILED(d->GetRenderTarget(0, &rt0)) || !rt0) return false;
		const bool ok = Ensure(d, rt0);
		if (ok && !FrameCleared) {
			d->SetRenderTarget(0, DrawSurface());
			d->Clear(0, NULL, D3DCLEAR_TARGET, D3DCOLOR_ARGB(0, 0, 0, 0), 1.0f, 0);
			d->SetRenderTarget(0, rt0);
			FrameCleared = true;
		}
		rt0->Release();
		return ok;
	}

	// Right after a see-through draw: the same draw once more into the mask (the bound vertex shader and streams as they are).
	template <typename DrawFn> inline void Redraw(IDirect3DDevice9* d, DrawFn draw) {
		IDirect3DVertexShader9* vs = nullptr;
		d->GetVertexShader(&vs);
		UINT size = 0;
		DWORD version = 0;
		if (vs) {
			vs->GetFunction(NULL, &size);
			if (size >= 4) { std::vector<DWORD> code(size / 4); vs->GetFunction(code.data(), &size); version = code[0]; }
			vs->Release();
		}
		IDirect3DPixelShader9* mine = (version & 0xFFFF) >= 0x0300 ? PS3 : PS2;	// vs_3_0 needs ps_3_0; fixed function / vs_2_x take ps_2_0
		IDirect3DSurface9* rt0 = nullptr;
		IDirect3DPixelShader9* ps = nullptr;
		d->GetRenderTarget(0, &rt0);
		d->GetPixelShader(&ps);
		DWORD blend = 0, zwrite = 0, atest = 0, mask = 0, srgb = 0;
		d->GetRenderState(D3DRS_ALPHABLENDENABLE, &blend);
		d->GetRenderState(D3DRS_ZWRITEENABLE, &zwrite);
		d->GetRenderState(D3DRS_ALPHATESTENABLE, &atest);
		d->GetRenderState(D3DRS_COLORWRITEENABLE, &mask);
		d->GetRenderState(D3DRS_SRGBWRITEENABLE, &srgb);
		d->SetRenderTarget(0, DrawSurface());
		d->SetPixelShader(mine);
		d->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
		d->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
		d->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
		d->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
		d->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
		if (SUCCEEDED(draw())) { Draws++; Written = true; }
		// Everything back as the game set it (its state cache never sees these).
		d->SetRenderTarget(0, rt0);
		d->SetPixelShader(ps);
		d->SetRenderState(D3DRS_ALPHABLENDENABLE, blend);
		d->SetRenderState(D3DRS_ZWRITEENABLE, zwrite);
		d->SetRenderState(D3DRS_ALPHATESTENABLE, atest);
		d->SetRenderState(D3DRS_COLORWRITEENABLE, mask);
		d->SetRenderState(D3DRS_SRGBWRITEENABLE, srgb);
		if (rt0) rt0->Release();
		if (ps) ps->Release();
	}

	// The blended stage ended: the multisampled mask resolved for Reflections.
	inline void AfterStage(IDirect3DDevice9* d) {
		if (MsaaSurface && Written) d->StretchRect(MsaaSurface, NULL, TextureSurface, NULL, D3DTEXF_NONE);
		static UInt32 lastDraws = 0;
		if ((++Frames % 600) == 1)
			Logger::Log("UNOFFICIAL GlassMask: %u glass redraws in the last %u blended stages (%s)", Draws - lastDraws, Frames == 1 ? 1u : 600u,
				MsaaSurface ? "multisampled, resolved" : "single-sampled");
		if ((Frames % 600) == 1) lastDraws = Draws;
	}

	// What Reflections binds this frame (null: no glass this frame, or off).
	inline IDirect3DTexture9* ThisFrame() { return Wanted() && Written ? Texture : nullptr; }
}
