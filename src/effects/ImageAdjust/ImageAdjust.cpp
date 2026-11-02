#include "ImageAdjust.h"
#include "../../core/DxvkCompute.h"	// UNOFFICIAL 2026-10-23 (NVR DXVK)

void ImageAdjustEffect::UpdateConstants() {
	ShaderManager* sm = TheShaderManager;
	const ImageAdjustSettingsStruct& s = Settings;
	// day / night / interior blend of one field
	auto mix = [&](float ValuesStruct::* field) {
		return sm->GetTransitionValue(s.Main.*field, s.Night.*field, s.Interiors.*field);
	};

	Constants.Data = D3DXVECTOR4(mix(&ValuesStruct::Brightness), mix(&ValuesStruct::Contrast),
		mix(&ValuesStruct::Saturation), mix(&ValuesStruct::Strength));
	Constants.DarkColor.x = mix(&ValuesStruct::DarkColorR);
	Constants.DarkColor.y = mix(&ValuesStruct::DarkColorG);
	Constants.DarkColor.z = mix(&ValuesStruct::DarkColorB);
	Constants.LightColor.x = mix(&ValuesStruct::LightColorR);
	Constants.LightColor.y = mix(&ValuesStruct::LightColorG);
	Constants.LightColor.z = mix(&ValuesStruct::LightColorB);
}

static void ReadAdjustGroup(const char* section, ImageAdjustEffect::ValuesStruct& out) {
	SettingManager* cfg = TheSettingManager;
	out.Brightness = cfg->GetSettingF(section, "Brightness");
	out.Contrast = cfg->GetSettingF(section, "Contrast");
	out.Saturation = cfg->GetSettingF(section, "Saturation");
	out.Strength = cfg->GetSettingF(section, "Strength");
	out.DarkColorR = cfg->GetSettingF(section, "DarkColorR");
	out.DarkColorG = cfg->GetSettingF(section, "DarkColorG");
	out.DarkColorB = cfg->GetSettingF(section, "DarkColorB");
	out.LightColorR = cfg->GetSettingF(section, "LightColorR");
	out.LightColorG = cfg->GetSettingF(section, "LightColorG");
	out.LightColorB = cfg->GetSettingF(section, "LightColorB");
}

void ImageAdjustEffect::UpdateSettings(){
	ReadAdjustGroup("Shaders.ImageAdjust.Main", Settings.Main);
	ReadAdjustGroup("Shaders.ImageAdjust.Night", Settings.Night);
	ReadAdjustGroup("Shaders.ImageAdjust.Interiors", Settings.Interiors);
}

void ImageAdjustEffect::RegisterConstants() {
	ShaderManager* sm = TheShaderManager;
	sm->RegisterConstant("TESR_ImageAdjustData", &Constants.Data);
	sm->RegisterConstant("TESR_DarkAdjustColor", &Constants.DarkColor);
	sm->RegisterConstant("TESR_LightAdjustColor", &Constants.LightColor);
}

bool ImageAdjustEffect::ShouldRender() {
	const D3DXVECTOR4& d = Constants.Data;
	const D3DXVECTOR4& dark = Constants.DarkColor;
	const D3DXVECTOR4& light = Constants.LightColor;

	// Saturation is independent of Strength. Everything else is blended by Strength.
	if (d.z != 1.0f) return true;
	if (d.w == 0.0f) return false;
	return d.x != 1.0f || d.y != 1.0f ||
		dark.x != 1.0f || dark.y != 1.0f || dark.z != 1.0f ||
		light.x != 1.0f || light.y != 1.0f || light.z != 1.0f;
}

// UNOFFICIAL 2026-10-23 (NVR DXVK): ImageAdjust.frag; the textures and constants the technique reads (EffectRecord::ExtTexturePass).
bool ImageAdjustEffect::ExtRender(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, IDirect3DSurface9* RenderedSurface, UINT techniqueIndex, bool ClearRenderTarget, IDirect3DSurface9* SourceBuffer) {
	if (techniqueIndex != 0) return false;
	struct Push { D3DXVECTOR4 data, darkColor, lightColor; UInt32 samplerIndex; UInt32 pad[3]; } push = {};
	push.data = Constants.Data;
	push.darkColor = Constants.DarkColor;
	push.lightColor = Constants.LightColor;
	IDirect3DBaseTexture9* textures[] = { TheTextureManager->RenderedTexture };
	return ExtTexturePass(Device, RenderTarget, RenderedSurface, SourceBuffer, extPass, "ImageAdjust", textures, _countof(textures), &push, sizeof(push), 48);
}

