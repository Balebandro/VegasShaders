#include "Lens.h"
#include "../../core/DxvkCompute.h"	// UNOFFICIAL 2026-10-23 (NVR DXVK)

void LensEffect::UpdateConstants() {
	ShaderManager* sm = TheShaderManager;
	const LensSettings& s = Settings;
	// x is forced to 0 while the camera is underwater
	float underwaterMask = sm->GameState.isUnderwater ? 0.0f : 1.0f;
	Constants.Data.x = sm->GetTransitionValue(s.Main.strength, s.Night.strength, s.Interiors.strength) * underwaterMask;
	Constants.Data.y = sm->GetTransitionValue(s.Main.bloomExponent, s.Night.bloomExponent, s.Interiors.bloomExponent);
	Constants.Data.z = sm->GetTransitionValue(s.Main.smudginess, s.Night.smudginess, s.Interiors.smudginess);
}

// Reads one Lens group from the settings file.
static void ReadLensGroup(const char* section, LensEffect::LensSettingsStruct& out) {
	out.bloomExponent = TheSettingManager->GetSettingF(section, "BloomExponent");
	out.strength = TheSettingManager->GetSettingF(section, "Strength");
	out.smudginess = TheSettingManager->GetSettingF(section, "Smudginess");
}

void LensEffect::UpdateSettings(){
	if (!Enabled) Constants.Data.x = 0; // a disabled effect draws nothing

	ReadLensGroup("Shaders.Lens.Main", Settings.Main);
	ReadLensGroup("Shaders.Lens.Night", Settings.Night);
	ReadLensGroup("Shaders.Lens.Interiors", Settings.Interiors);
}

void LensEffect::RegisterConstants() {
	TheShaderManager->RegisterConstant("TESR_LensData", &Constants.Data);
}

bool LensEffect::ShouldRender() {
	if (TheShaderManager->GameState.isUnderwater) return false;
	return Constants.Data.x > 0;
};

// UNOFFICIAL 2026-10-23 (NVR DXVK): Lens.frag; TESR_SourceBuffer (refreshed by ExtFullScreen), the first bloom buffer and the
// dirt file texture through the WRAP sampler.
bool LensEffect::ExtRender(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, IDirect3DSurface9* RenderedSurface, UINT techniqueIndex, bool ClearRenderTarget, IDirect3DSurface9* SourceBuffer) {
	if (techniqueIndex != 0) return false;
	struct Push { D3DXVECTOR4 lensData; UInt32 samplerIndex; UInt32 wrapIndex; UInt32 pad[2]; } push = {};
	push.lensData = Constants.Data;
	IDirect3DBaseTexture9* bloom = TheShaderManager->Effects.Bloom ? TheShaderManager->Effects.Bloom->Textures.BloomTexture[0] : nullptr;
	IDirect3DBaseTexture9* textures[] = { TheTextureManager->SourceTexture, bloom, ExtFileTexture("TESR_LensSampler") };
	return ExtTexturePass(Device, RenderTarget, RenderedSurface, SourceBuffer, extPass, "Lens", textures, _countof(textures), &push, sizeof(push), 16, false, 20);
}
