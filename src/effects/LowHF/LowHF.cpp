#include "LowHF.h"
#include "../../core/DxvkCompute.h"	// UNOFFICIAL 2026-10-23 (NVR DXVK)

void LowHFEffect::UpdateConstants() {
	const float health = (float)Player->GetActorValue(Actor::ActorVal::kActorVal_Health) / (float)Player->GetBaseActorValue(Actor::ActorVal::kActorVal_Health);
	const float fatigue = (float)Player->GetActorValue(Actor::ActorVal::kActorVal_Stamina) / (float)Player->GetBaseActorValue(Actor::ActorVal::kActorVal_Stamina);

	Constants.Data.x = Constants.Data.y = Constants.Data.z = Constants.Data.w = 0.0f;

	// the health limit is read from the settings file every frame, not from Settings
	const float limit = TheSettingManager->GetSettingF("Shaders.LowHF.Main", "HealthLimit");
	if (!Player->IsAlive()) return;

	Constants.HealthCoeff = 1.0f - health / limit;
	Constants.FatigueCoeff = 1.0f - fatigue / Settings.FatigueLimit;
	if (health < limit) {
		const float k = Constants.HealthCoeff;
		Constants.Data.x = k * Settings.LumaMultiplier;
		Constants.Data.y = k * 0.01f * Settings.BlurMultiplier;
		Constants.Data.z = k * 20.0f * Settings.VignetteMultiplier;
		Constants.Data.w = (1.0f - k) * Settings.DarknessMultiplier;
	}
	// exhaustion only tints the luma when low health has not already
	if (Constants.Data.x == 0.0f && fatigue < Settings.FatigueLimit)
		Constants.Data.x = Constants.FatigueCoeff * Settings.LumaMultiplier;
}

void LowHFEffect::UpdateSettings() {
	const char* sec = "Shaders.LowHF.Main";
	SettingManager* cfg = TheSettingManager;
	Settings.healthLimit = cfg->GetSettingF(sec, "HealthLimit");
	Settings.FatigueLimit = cfg->GetSettingF(sec, "FatigueLimit");
	Settings.LumaMultiplier = cfg->GetSettingF(sec, "LumaMultiplier");
	Settings.BlurMultiplier = cfg->GetSettingF(sec, "BlurMultiplier");
	Settings.VignetteMultiplier = cfg->GetSettingF(sec, "VignetteMultiplier");
	Settings.DarknessMultiplier = cfg->GetSettingF(sec, "DarknessMultiplier");
}

void LowHFEffect::RegisterConstants() {
	TheShaderManager->RegisterConstant("TESR_LowHFData", &Constants.Data);
}

bool LowHFEffect::ShouldRender() {
	return Constants.Data.x != 0.0f;
};

// UNOFFICIAL 2026-10-23 (NVR DXVK): LowHF.frag; the textures and constants the technique reads (EffectRecord::ExtTexturePass).
bool LowHFEffect::ExtRender(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, IDirect3DSurface9* RenderedSurface, UINT techniqueIndex, bool ClearRenderTarget, IDirect3DSurface9* SourceBuffer) {
	if (techniqueIndex != 0) return false;
	struct Push { D3DXVECTOR4 lowHFData; UInt32 samplerIndex; UInt32 pad[3]; } push = {};
	push.lowHFData = Constants.Data;
	IDirect3DBaseTexture9* textures[] = { TheTextureManager->RenderedTexture };
	return ExtTexturePass(Device, RenderTarget, RenderedSurface, SourceBuffer, extPass, "LowHF", textures, _countof(textures), &push, sizeof(push), 16);
}
