#include "DitherBuster.h"
#include "../Tonemapping/Tonemapping.h"				// UNOFFICIAL 2026-10-25 (NVR DXVK): TESR_ToneMappingClip for the stable pass
#include "../../core/DxvkCompute.h"	// UNOFFICIAL 2026-10-25 (NVR DXVK)

void DitherBusterEffect::UpdateConstants() {
	Constants.Data.x = Settings.Strength;
	Constants.Data.y = Settings.MaskPower;
	Constants.Data.z = Settings.DitherThreshold;
	Constants.Data.w = Settings.Radius;
	Constants.Gate = D3DXVECTOR4(Settings.GateSoftness, Settings.DeDither ? 1.0f : 0.0f, (float)Settings.DebugView, 0.0f);
}

void DitherBusterEffect::UpdateSettings() {
	Settings.Strength  = TheSettingManager->GetSettingF("Shaders.DitherBuster.Main", "Strength");
	Settings.MaskPower = TheSettingManager->GetSettingF("Shaders.DitherBuster.Main", "MaskPower");
	Settings.DitherThreshold = TheSettingManager->GetSettingF("Shaders.DitherBuster.Main", "DitherThreshold");
	Settings.Radius = TheSettingManager->GetSettingF("Shaders.DitherBuster.Main", "Radius");
	Settings.GateSoftness = std::clamp(TheSettingManager->GetSettingF("Shaders.DitherBuster.Main", "GateSoftness"), 0.0f, 1.0f);
	Settings.DeDither = TheSettingManager->GetSettingI("Shaders.DitherBuster.Main", "DeDither") != 0;
	Settings.DebugView = TheSettingManager->GetSettingI("Shaders.DitherBuster.Main", "DebugView");
}

void DitherBusterEffect::RegisterConstants() {
	TheShaderManager->RegisterConstant("TESR_DitherBusterData", &Constants.Data);
	TheShaderManager->RegisterConstant("TESR_DitherBusterGate", &Constants.Gate);
}

// UNOFFICIAL 2026-10-25 (NVR DXVK): the passes as src\hlsl\NewVegas\Compute\DitherBuster.frag (technique 0, DitherOnly) and
// DitherBusterStable.frag (technique 1, DitherOnlyStable); the textures and constants the technique reads (EffectRecord::ExtTexturePass).
bool DitherBusterEffect::ExtRender(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, IDirect3DSurface9* RenderedSurface, UINT techniqueIndex, bool ClearRenderTarget, IDirect3DSurface9* SourceBuffer) {
	IDirect3DBaseTexture9* textures[] = { TheTextureManager->RenderedTexture };
	if (techniqueIndex == 0) {
		struct Push { D3DXVECTOR4 reciprocalResolution; D3DXVECTOR4 ditherBusterData; UInt32 samplerIndex; UInt32 pad[3]; } push = {};
		push.reciprocalResolution = TheShaderManager->ShaderConst.ReciprocalResolution;
		push.ditherBusterData = Constants.Data;
		return ExtTexturePass(Device, RenderTarget, RenderedSurface, SourceBuffer, extPass, "DitherBuster", textures, _countof(textures), &push, sizeof(push), 32);
	}
	if (techniqueIndex == 1) {
		struct Push { D3DXVECTOR4 reciprocalResolution; D3DXVECTOR4 ditherBusterData; D3DXVECTOR4 ditherBusterGate; D3DXVECTOR4 toneMappingClip; UInt32 samplerIndex; UInt32 pad[3]; } push = {};
		push.reciprocalResolution = TheShaderManager->ShaderConst.ReciprocalResolution;
		push.ditherBusterData = Constants.Data;
		push.ditherBusterGate = Constants.Gate;
		if (TheShaderManager->Shaders.Tonemapping) push.toneMappingClip = TheShaderManager->Shaders.Tonemapping->Constants.Clip;
		return ExtTexturePass(Device, RenderTarget, RenderedSurface, SourceBuffer, extPassStable, "DitherBusterStable", textures, _countof(textures), &push, sizeof(push), 64);
	}
	return false;
}
