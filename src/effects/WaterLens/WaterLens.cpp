#include "WaterLens.h"
#include "../../core/DxvkCompute.h"	// UNOFFICIAL 2026-10-23 (NVR DXVK)

void WaterLensEffect::UpdateConstants() {
	Animator& fade = Constants.WaterLensAnimator;
	const bool underwater = TheShaderManager->GameState.isUnderwater;

	if (underwater && !fade.switched) {
		// just went under: hold the lens at zero
		fade.switched = true;
		fade.Start(0.f, 0);
	} else if (!underwater && fade.switched) {
		// just came up: start the droplets at full and fade them out
		fade.switched = false;
		fade.Initialize(1);
		fade.Start(0.01f, 0);
	}

	Constants.Data.w = amount * fade.GetValue();
}

void WaterLensEffect::UpdateSettings(){
	const char* sec = "Shaders.WaterLens.Main";
	Constants.Data.x = TheSettingManager->GetSettingF(sec, "TimeMultA");
	Constants.Data.y = TheSettingManager->GetSettingF(sec, "TimeMultB");
	Constants.Data.z = TheSettingManager->GetSettingF(sec, "Viscosity");
	amount = TheSettingManager->GetSettingF(sec, "Amount");
}

void WaterLensEffect::RegisterConstants(){
	TheShaderManager->RegisterConstant("TESR_WaterLensData", &Constants.Data);
}

bool WaterLensEffect::ShouldRender() {
	return Constants.Data.w > 0.0f;
};

// UNOFFICIAL 2026-10-23 (NVR DXVK): WaterLens.frag; the textures (the file texture through the WRAP sampler) and constants the technique reads.
bool WaterLensEffect::ExtRender(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, IDirect3DSurface9* RenderedSurface, UINT techniqueIndex, bool ClearRenderTarget, IDirect3DSurface9* SourceBuffer) {
	if (techniqueIndex != 0) return false;
	struct Push { D3DXVECTOR4 gameTime, waterLensData; UInt32 samplerIndex; UInt32 wrapIndex; UInt32 pad[2]; } push = {};
	push.gameTime = TheShaderManager->ShaderConst.GameTime;
	push.waterLensData = Constants.Data;
	IDirect3DBaseTexture9* textures[] = { TheTextureManager->RenderedTexture, ExtFileTexture("TESR_waterlensSampler") };
	return ExtTexturePass(Device, RenderTarget, RenderedSurface, SourceBuffer, extPass, "WaterLens", textures, _countof(textures), &push, sizeof(push), 32, false, 36);
}
