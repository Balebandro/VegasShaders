#include "BloodLens.h"
#include "../../core/DxvkCompute.h"	// UNOFFICIAL 2026-10-23 (NVR DXVK)

void BloodLensEffect::UpdateConstants() {
	// UNOFFICIAL fixes-b #45 (LOOK CHANGE, BloodLens is off by default): UpdateConstants runs twice a rendered frame, and the fade
	// counter used to step on both calls (the fade took half the frames the setting says). It steps once per rendered frame
	// (FrameNumber) now. Params.w (the shader's only strength input) was only set in UpdateSettings, so the fade never reached
	// the shader: it is set here, every call.
	static unsigned lastFrame = ~0u;
	const bool newFrame = TheFrameRateManager->FrameNumber != lastFrame;
	lastFrame = TheFrameRateManager->FrameNumber;

	if (TheShaderManager->GameState.isUnderwater)
		Constants.Percent = 0.0f;	// water washes the blood off

	if (Constants.Percent > 0.0f) {
		Constants.Time.z = effectTime;

		// a fresh splash (Percent still exactly 1) restarts the counter and picks a new random placement
		const bool justStarted = Constants.Percent == 1.0f;
		if (justStarted) {
			Constants.Time.w = 0.0f;
			srand(time(NULL));
			auto pick = [](float lo, float hi) { return (double)rand() / (RAND_MAX + 1) * (hi - lo) + lo; };
			Constants.Params.x = pick(0.25f, 0.75f);
			Constants.Params.y = pick(-0.1f, 0.5f);
			Constants.Params.z = pick(-2.0f, 2.0f);
		}

		if (newFrame || justStarted) Constants.Time.w += 1.0f;
		Constants.Percent = 1.0f - Constants.Time.w / Constants.Time.z;
		if (Constants.Percent < 0.0f)
			Constants.Percent = 0.0f;
	}
	Constants.Params.w = intensity * Constants.Percent;
}

void BloodLensEffect::UpdateSettings() {
	const char* sec = "Shaders.BloodLens.Main";
	SettingManager* cfg = TheSettingManager;
	effectTime = cfg->GetSettingF(sec, "Time");
	intensity = cfg->GetSettingF(sec, "Intensity");
	Constants.Params.w = intensity * Constants.Percent;
	Constants.BloodColor.x = cfg->GetSettingF(sec, "ColorR");
	Constants.BloodColor.y = cfg->GetSettingF(sec, "ColorG");
	Constants.BloodColor.z = cfg->GetSettingF(sec, "ColorB");
}

void BloodLensEffect::RegisterConstants() {
	TheShaderManager->RegisterConstant("TESR_BloodLensParams", &Constants.Params);
	TheShaderManager->RegisterConstant("TESR_BloodLensColor", &Constants.BloodColor);
}

bool BloodLensEffect::ShouldRender() {
	return Constants.Percent > 0.0f;
};

// UNOFFICIAL 2026-10-23 (NVR DXVK): BloodLens.frag; the textures (the file texture through the WRAP sampler) and constants the technique reads.
bool BloodLensEffect::ExtRender(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, IDirect3DSurface9* RenderedSurface, UINT techniqueIndex, bool ClearRenderTarget, IDirect3DSurface9* SourceBuffer) {
	if (techniqueIndex != 0) return false;
	struct Push { D3DXVECTOR4 params, bloodColor; UInt32 samplerIndex; UInt32 wrapIndex; UInt32 pad[2]; } push = {};
	push.params = Constants.Params;
	push.bloodColor = Constants.BloodColor;
	IDirect3DBaseTexture9* textures[] = { TheTextureManager->RenderedTexture, ExtFileTexture("TESR_BloodLensSampler") };
	return ExtTexturePass(Device, RenderTarget, RenderedSurface, SourceBuffer, extPass, "BloodLens", textures, _countof(textures), &push, sizeof(push), 32, false, 36);
}

