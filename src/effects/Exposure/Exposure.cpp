#include "Exposure.h"
#include "../../core/DxvkCompute.h"	// UNOFFICIAL 2026-10-23 (NVR DXVK)
#include <algorithm>

void ExposureEffect::UpdateConstants() {
	TheShaderManager->avglumaRequired = true; // the screen average must be computed this frame

	bool stale = TheSettingManager->SettingsChanged || TheShaderManager->GameState.isDayTimeChanged;
	if (!stale) return;

	const ExposureSettingsStruct& s = Settings;
	ShaderManager* sm = TheShaderManager;
	Constants.Data.x = sm->GetTransitionValue(s.Main.MinBrightness, s.Night.MinBrightness, s.Interiors.MinBrightness);
	Constants.Data.y = sm->GetTransitionValue(s.Main.MaxBrightness, s.Night.MaxBrightness, s.Interiors.MaxBrightness);
	Constants.Data.z = sm->GetTransitionValue(s.Main.DarkAdaptSpeed, s.Night.DarkAdaptSpeed, s.Interiors.DarkAdaptSpeed);
	Constants.Data.w = sm->GetTransitionValue(s.Main.LightAdaptSpeed, s.Night.LightAdaptSpeed, s.Interiors.LightAdaptSpeed);
}

// Fills one brightness group from its settings section.
static void ReadExposureGroup(const char* section, ExposureEffect::ValuesStruct& out) {
	SettingManager* cfg = TheSettingManager;
	out.MinBrightness = cfg->GetSettingF(section, "MinBrightness");
	out.MaxBrightness = cfg->GetSettingF(section, "MaxBrightness");
	out.DarkAdaptSpeed = cfg->GetSettingF(section, "DarkAdaptSpeed");
	out.LightAdaptSpeed = cfg->GetSettingF(section, "LightAdaptSpeed");
}

void ExposureEffect::UpdateSettings(){
	ReadExposureGroup("Shaders.Exposure.Main", Settings.Main);
	ReadExposureGroup("Shaders.Exposure.Night", Settings.Night);
	ReadExposureGroup("Shaders.Exposure.Interiors", Settings.Interiors);

	// UNOFFICIAL 2026-10-26: histogram trimming (both percentages are capped at 90)
	const char* main = "Shaders.Exposure.Main";
	Settings.Histogram = TheSettingManager->GetSettingI(main, "Histogram") != 0;
	Settings.HistogramLowPercent = std::clamp(TheSettingManager->GetSettingF(main, "HistogramLowPercent"), 0.0f, 90.0f);
	Settings.HistogramHighPercent = std::clamp(TheSettingManager->GetSettingF(main, "HistogramHighPercent"), 0.0f, 90.0f);
}

void ExposureEffect::RegisterConstants() {
	TheShaderManager->RegisterConstant("TESR_ExposureData", &Constants.Data);
}

// UNOFFICIAL 2026-10-23 (NVR DXVK): Exposure.frag; the textures and constants the technique reads (EffectRecord::ExtTexturePass).
bool ExposureEffect::ExtRender(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, IDirect3DSurface9* RenderedSurface, UINT techniqueIndex, bool ClearRenderTarget, IDirect3DSurface9* SourceBuffer) {
	if (techniqueIndex != 0) return false;
	struct Push { D3DXVECTOR4 exposureData; UInt32 samplerIndex; UInt32 pad[3]; } push = {};
	push.exposureData = Constants.Data;
	IDirect3DBaseTexture9* textures[] = { TheTextureManager->RenderedTexture, TheShaderManager->Effects.AvgLuma ? TheShaderManager->Effects.AvgLuma->Textures.AvgLumaTexture : nullptr };
	return ExtTexturePass(Device, RenderTarget, RenderedSurface, SourceBuffer, extPass, "Exposure", textures, _countof(textures), &push, sizeof(push), 16);
}
