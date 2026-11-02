#include "Coloring.h"
#include "../LUT/LUT.h"	// UNOFFICIAL 2026-10-25: the ColoringLUT technique reads the LUT's textures and constants
#include "../../core/DxvkCompute.h"	// UNOFFICIAL 2026-10-25 (NVR DXVK)

void ColoringEffect::UpdateConstants() {
	// Colouring preset: the current cell's, else the worldspace's (exteriors only), else "Default"
	SettingManager* cfg = TheSettingManager;
	SettingsColoringStruct* preset = cfg->GetSettingsColoring(Player->parentCell->GetEditorName());
	if (!preset && TheShaderManager->GameState.isExterior)
		preset = cfg->GetSettingsColoring(Player->GetWorldSpace()->GetEditorName());
	if (!preset)
		preset = cfg->GetSettingsColoring("Default");

	Constants.Data = D3DXVECTOR4(preset->Strength, preset->BaseGamma, preset->Fade, preset->Contrast);
	Constants.Values = D3DXVECTOR4(preset->Saturation, preset->Bleach, preset->BleachLuma, preset->Linearization);
	Constants.ColorCurve = D3DXVECTOR4(preset->ColorCurve, preset->ColorCurveR, preset->ColorCurveG, preset->ColorCurveB);
	Constants.EffectGamma = D3DXVECTOR4(preset->EffectGamma, preset->EffectGammaR, preset->EffectGammaG, preset->EffectGammaB);
}

void ColoringEffect::UpdateSettings() {}

void ColoringEffect::RegisterConstants() {
	ShaderManager* sm = TheShaderManager;
	sm->RegisterConstant("TESR_ColoringColorCurve", &Constants.ColorCurve);
	sm->RegisterConstant("TESR_ColoringEffectGamma", &Constants.EffectGamma);
	sm->RegisterConstant("TESR_ColoringData", &Constants.Data);
	sm->RegisterConstant("TESR_ColoringValues", &Constants.Values);
}

// UNOFFICIAL 2026-10-25 (NVR DXVK): Coloring.frag. Technique 1 (ColoringLUT) is the colouring and then the LUT in one pass, with
// the LUT's textures and constants as the LUT effect holds them (the D3D9 constant table reads the same ones). Any LUT slot
// without a texture is bound to the current image, which the shader passes through as the D3D9 pass does for an unusable LUT.
bool ColoringEffect::ExtRender(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, IDirect3DSurface9* RenderedSurface, UINT techniqueIndex, bool ClearRenderTarget, IDirect3DSurface9* SourceBuffer) {
	if (techniqueIndex > 1) return false;
	const bool withLUT = techniqueIndex == 1;
	LUTEffect* lut = TheShaderManager->Effects.LUT;
	struct Push { D3DXVECTOR4 colorCurve, effectGamma, data, values, lutData, lutBlend; UInt32 samplerIndex; UInt32 lutPass; } push = {};
	push.colorCurve = Constants.ColorCurve;
	push.effectGamma = Constants.EffectGamma;
	push.data = Constants.Data;
	push.values = Constants.Values;
	push.lutData = withLUT ? lut->Constants.Data : D3DXVECTOR4(0.0f, 0.0f, 0.0f, 0.0f);
	push.lutBlend = withLUT ? lut->Constants.Blend : D3DXVECTOR4(0.0f, 0.0f, 0.0f, 0.0f);
	push.lutPass = withLUT ? 1 : 0;
	IDirect3DBaseTexture9* current = TheTextureManager->RenderedTexture;
	IDirect3DBaseTexture9* textures[] = { current,
		(withLUT && lut->DayTexture) ? lut->DayTexture : current,
		(withLUT && lut->NightTexture) ? lut->NightTexture : current,
		(withLUT && lut->InteriorTexture) ? lut->InteriorTexture : current };
	return ExtTexturePass(Device, RenderTarget, RenderedSurface, SourceBuffer, extPass, "Coloring", textures, _countof(textures), &push, sizeof(push), 96);
}