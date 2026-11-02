#include "Grass.h"
#include "../../core/NewGrass.h"	// UNOFFICIAL 2026-10-18 NewGrass ([Shaders.Grass.NewGrass]): blade geometry in place of the near grass cards
#include "GrassCollision.h"	// UNOFFICIAL 2026-10-19 [Shaders.Grass.Collision]
#include "../../core/GrassWind.h"	// UNOFFICIAL 2026-10-20 [Shaders.Grass.Wind]: gusts rolling across the field along the weather's wind
#include "../../core/PlantWind.h"	// UNOFFICIAL 2026-10-20 [Shaders.Plants.Wind]: the plants in the same wind
#include "GpuGrass.h"	// UNOFFICIAL 2026-10-27 [Shaders.Grass.GPUGrass] (NVR DXVK 1.7-grass)
#include "../../core/GrassShadows.h"	// UNOFFICIAL 2026-10-27 [Shaders.Grass.Main] CastShadows

void GrassShaders::RegisterConstants() {
	TheShaderManager->RegisterConstant("TESR_GrassScale", &Constants.Scale);
	PlantWind::RegisterConstants();	// UNOFFICIAL 2026-10-20: the SpeedTree leaves' share of the plant wind (TESR_PlantLeafWind*)
}

// iMinGrassSize, fTexturePctThreshold, fGrass*Distance and fGrassWindMagnitude* are consumed
// at cell load: writing them affects only cells loaded afterwards.
void GrassShaders::UpdateSettings() {
	if (!Enabled) return;

	Constants.Scale.x = TheSettingManager->GetSettingF("Shaders.Grass.Main", "ScaleX");
	Constants.Scale.y = TheSettingManager->GetSettingF("Shaders.Grass.Main", "ScaleY");
	Constants.Scale.z = TheSettingManager->GetSettingF("Shaders.Grass.Main", "ScaleZ");
	Constants.Scale.w = (TheSettingManager->GetSettingI("Shaders.Grass.Main", "ShadowFix") ? 1.0f : 0.0f)	// UNOFFICIAL 2026-10-27 (GRASS23x000TMS.pso)
		+ (TheSettingManager->GetSettingI("Shaders.Grass.Main", "SkyLightScale") ? 2.0f : 0.0f);
	GameShaderFix = TheSettingManager->GetSettingI("Shaders.Grass.Main", "GameShaderFix");
	DrawFirst = TheSettingManager->GetSettingI("Shaders.Grass.Main", "DrawFirst");
	DrawFirstRestore = TheSettingManager->GetSettingI("Shaders.Grass.Main", "DrawFirstRestore");
	DrawFirstCull = TheSettingManager->GetSettingI("Shaders.Grass.Main", "DrawFirstCull");
	NewGrass::ReadSettings();	// UNOFFICIAL 2026-10-18 NewGrass (core/NewGrass.h): live
	GrassCollision::ReadSettings();	// UNOFFICIAL 2026-10-19
	GrassWind::ReadSettings();	// UNOFFICIAL 2026-10-20 (live)
	GpuGrass::ReadSettings();	// UNOFFICIAL 2026-10-27 GPU grass (live)
	GrassShadows::ReadSettings();	// UNOFFICIAL 2026-10-27 grass sun shadows (live)
}

void GrassShaders::UpdateConstants() {
	GrassCollision::Update();	// UNOFFICIAL 2026-10-19: the push round the camera, once a frame
	// UNOFFICIAL 2026-10-20 grass field wind: the weather's wind and the clock, once a rendered frame. While a sandstorm blows, the
	// clock's rate blends to the storm's (its grass path shares c19.z).
	const SandstormEffect* Sandstorm = TheShaderManager->Effects.Sandstorm;
	const float storm = Sandstorm && Sandstorm->Enabled && Sandstorm->GrassStorm[0] > 0.0f ? Sandstorm->Intensity : 0.0f;
	GrassWind::Update(Tes ? Tes->sky : nullptr, TheShaderManager->GameState.isExterior, storm);
}

// UNOFFICIAL 2026-10-17 grass study: every pass that sets grass shaders up (SetShadersHook; the game does it once for a run of
// grass draws). Tells NVR's grass vertex shaders (c19, GrassPairing in GRASS23x00*.vso) which pixel shader reads them: NVR's
// GRASS23x000TMS.pso (transparency multisampling on) takes the light colours and amounts apart; the game's own GRASS23x000.pso
// (the INI default, no transparency multisampling) needs them multiplied together as the game wrote them. GameShaderFix = false
// gives the P80 behaviour (the bare colours to the game's shader: too bright) for A/B tests.
void GrassShaders::SetForDraw(NiD3DVertexShaderEx* VertexShader, NiD3DPixelShaderEx* PixelShader) {
	if (!Enabled || !VertexShader || !VertexShader->Name || memcmp(VertexShader->Name, "GRASS", 5)) return;
	// UNOFFICIAL 2026-10-18 NewGrass: which grass VS variant this pass binds ('GRASS23x00N': N = 0 LF, 1 LV, 2 LFS, 3 LVS; the card
	// variants are 0 and 2, the others light real meshes by their normals and are never replaced by blades).
	NewGrass::LastVariant = strlen(VertexShader->Name) >= 11 ? VertexShader->Name[10] : 0;
	BindSerial++;	// UNOFFICIAL 2026-10-28 (GrassFirst: whether this frame's early run saw a grass bind of its own)
	LastBindName = VertexShader->Name;
	LastBindNVR = false;
	if (!VertexShader->ShaderHandleBackup || VertexShader->ShaderHandle == VertexShader->ShaderHandleBackup) return;	// the game's own VS
	LastBindNVR = true;

	const bool NVRPixel = PixelShader && PixelShader->ShaderHandleBackup && PixelShader->ShaderHandle != PixelShader->ShaderHandleBackup;
	// w: GrassFirst's part (DrawPart; Render.cpp namespace GrassFirst): 0 every tuft, 1 the near tufts, 2 the fading ones.
	// UNOFFICIAL 2026-10-18 sandport: y, z the sandstorm's wind (SandstormEffect::GrassStorm: the extra sway and the gust clock;
	// 0, 0 with no storm or the effect off, and the grass VS then sways exactly as the game's). GrassFirst's re-writes of c19 keep
	// them (LastPairing).
	const SandstormEffect* Sandstorm = TheShaderManager->Effects.Sandstorm;
	// UNOFFICIAL 2026-10-20 grass field wind ([Shaders.Grass.Wind], core/GrassWind.h): z is its clock while it is on (the storm's
	// gusts then run on it too, at a rate blended to theirs), and its lanes go into the 4th column of c252-c255 (the inverse view
	// matrix SetCT has just put there). Off: z the storm's clock as before, c252-c255 as SetCT wrote them.
	const float Pairing[4] = { NVRPixel || !GameShaderFix ? 1.0f : 0.0f, Sandstorm ? Sandstorm->GrassStorm[0] : 0.0f,
		GrassWind::On ? GrassWind::ClockNow : Sandstorm ? Sandstorm->GrassStorm[1] : 0.0f, (float)DrawPart };
	TheRenderManager->device->SetVertexShaderConstantF(19, Pairing, 1);
	for (int i = 0; i < 4; i++) LastPairing[i] = Pairing[i];
	GrassCollision::Bind(TheRenderManager->device);	// UNOFFICIAL 2026-10-19: the push texture on vertex sampler 0
	GrassWind::BindCards(TheRenderManager->device);
	GrassWind::LogState(TheRenderManager->device);	// diagnostic (Log 1)
}
