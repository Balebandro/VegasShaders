#include "SkyDither.h"
#include "../../core/TemporalBackbone.h"	// UNOFFICIAL 2026-11-01 SharedNoise
#include "../../core/DxvkCompute.h"	// UNOFFICIAL 2026-10-23 (NVR DXVK)

void SkyDitherEffect::UpdateSettings() {
	Settings.Strength = std::clamp(TheSettingManager->GetSettingF("Shaders.SkyDither.Main", "Strength"), 0.0f, 4.0f);
	Settings.SkyOnly = TheSettingManager->GetSettingI("Shaders.SkyDither.Main", "SkyOnly") != 0;
	Settings.Pattern = TheSettingManager->GetSettingI("Shaders.SkyDither.Main", "Pattern") != 0 ? 1 : 0;
}

void SkyDitherEffect::RegisterConstants() {
	TheShaderManager->RegisterConstant("TESR_SkyDitherData", &Constants.Data);
}

void SkyDitherEffect::UpdateConstants() {
	// A new noise pattern every frame: at the frame rate the eye averages it into a smooth gradient, where a still
	// pattern could itself be seen as a fine grain stuck to the screen.
	// UNOFFICIAL aastab: Pattern 1 keeps one pattern (frame 0): nothing for the anti-aliasing to twinkle on with a still camera.
	// fixes-a #35: the end dither takes the fixed frame 32, the tonemap dither (Tonemapping.cpp) frame 0: with both on frame 0 they were
	// the same noise field and added up in the sky to about 2 steps instead of 1.
	// UNOFFICIAL 2026-11-01 SharedNoise ([Main.Main.Quality], core/TemporalBackbone.h): moves only while TAA / FSR / DLSS averages frames
	// (the shared index), else stands still at frame 32 (Pattern 1's; the tonemap dither's still frame is 0). Off: +1 a call, as before.
	frame = Settings.Pattern == 1 ? 32 : TemporalBackbone::Shared() ? (UINT)((TemporalBackbone::NoiseIndex() + TemporalBackbone::OffsetSkyDither) % 64) :
		(frame + 1) % 64;
	Constants.Data = D3DXVECTOR4(Settings.Strength / 255.0f, Settings.SkyOnly ? 1.0f : 0.0f, (float)frame, 0.0f);
}

// UNOFFICIAL 2026-10-23 (NVR DXVK): SkyDither.frag; the textures and constants the technique reads (EffectRecord::ExtTexturePass).
bool SkyDitherEffect::ExtRender(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, IDirect3DSurface9* RenderedSurface, UINT techniqueIndex, bool ClearRenderTarget, IDirect3DSurface9* SourceBuffer) {
	if (techniqueIndex != 0) return false;
	struct Push { D3DXVECTOR4 skyDitherData; UInt32 samplerIndex; UInt32 pad[3]; } push = {};
	push.skyDitherData = Constants.Data;
	IDirect3DBaseTexture9* textures[] = { TheTextureManager->RenderedTexture, TheShaderManager->Effects.CombineDepth ? TheShaderManager->Effects.CombineDepth->Textures.CombinedDepthTexture : nullptr };
	return ExtTexturePass(Device, RenderTarget, RenderedSurface, SourceBuffer, extPass, "SkyDither", textures, _countof(textures), &push, sizeof(push), 16, true);
}

