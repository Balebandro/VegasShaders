#pragma once

// UNOFFICIAL 2026-11-02 the post-process chain as two ordered tables of steps (EffectChain.cpp: the order of the whole frame,
// in one place). ShaderManager::RenderEffectsPreTonemapping and ShaderManager::RenderEffects keep their early returns, fill a
// ChainContext and run their stage's table. Each step is a ShaderManager member (ShaderManager::PreStep_* / PostStep_*) holding
// one block of what those two functions did, unchanged; adding or moving an effect is a line in a table.
//
// ChainContext holds what the steps share: what were the two functions' locals (the device and surfaces, the flags one step
// computes and later ones read) and their scope guards, which run when the context goes out of scope at the end of the call,
// in the order the locals' destructors ran (members are destroyed in reverse order: bounceGuard, aoGuard, then chainGuard).

class ShaderManager;
class EffectRecord;
class GpuTimer;
class AmbientOcclusionEffect;
class VolumetricFogEffect;

struct ChainContext {
	// The scope guards (they were locals of the two functions). Declared first, so they are destroyed last, in this order:
	// bounceGuard, aoGuard, chainGuard -- as the locals were (chainGuard was declared before aoGuard and bounceGuard).
	struct ChainGuard { bool armed = false; ~ChainGuard(); };	// Chain.End() once ChainBegin ran (was: struct ChainGuard chainGuard)
	struct EnabledGuard { EffectRecord* effect = nullptr; bool was = false; ~EnabledGuard(); };	// effect->Enabled = was (if effect)
	ChainGuard				chainGuard;
	EnabledGuard			aoGuard;		// pre: AmbientLightTakeover (AmbientLight replaces the AO for the rest of the call)
	EnabledGuard			bounceGuard;	// pre: AmbientLightTakeover (... and the bounce light)

	ShaderManager*			Manager = nullptr;			// the manager running the chain (this in the two functions)
	bool					stop = false;				// a step sets it to end the whole chain (as a return from the old function did); none does today

	// both stages: the locals every block used
	IDirect3DDevice9*		Device = nullptr;			// TheRenderManager->device
	IDirect3DSurface9*		RenderTarget = nullptr;		// the functions' argument
	IDirect3DSurface9*		SourceSurface = nullptr;	// TheTextureManager->SourceSurface at the start of the call
	IDirect3DSurface9*		RenderedSurface = nullptr;	// TheTextureManager->RenderedSurface at the start of the call (ChainBegin's legacy copy)

	// pre-tonemap: flags one step computes and later steps read
	bool					forwardInterior = false;	// ShadowPlan -> ShadowPasses, CompositePlan, InteriorApplyPass
	bool					keepDarkening = false;		// ShadowPlan -> ShadowPasses, CompositePlan, ShadowApply
	bool					ambientLight = false;		// AmbientLightTakeover -> HiZ, CompositePlan, AmbientLight
	AmbientOcclusionEffect*	AO = nullptr;				// CompositePlan (Effects.AmbientOcclusion) -> AmbientOcclusion, AmbientLight, VolumetricFog
	VolumetricFogEffect*	Fog = nullptr;				// CompositePlan (Effects.VolumetricFog) -> AmbientOcclusion, VolumetricLightGrid, VolumetricFog, VolumetricLight
	bool					shadowApplies = false;		// CompositePlan -> AmbientOcclusion, VolumetricFog, ShadowApply
	bool					aoApplies = false;			// CompositePlan -> AmbientOcclusion, VolumetricFog
	bool					foldCheckFrame = false;		// CompositePlan -> ShadowApply, VolumetricFog (InteriorShadowFoldCheck, test builds)
	bool					interiorApplies = false;	// CompositePlan -> ShadowApply, AmbientOcclusion, VolumetricFog
	bool					composite = false;			// CompositePlan; AmbientOcclusion clears it when the AO cannot defer -> VolumetricFog
	bool					pairApply = false;			// CompositePlan -> ShadowApply, AmbientOcclusion
	bool					shadowIntoAmbientLight = false;	// CompositePlan -> ShadowApply, AmbientLight
	bool					specularRenders = false;	// SpecularBouncePlan -> WetFlashlightSpecularUnderwater
	bool					bounceIntoSpecular = false;	// SpecularBouncePlan -> BounceLight
	bool					lightGrid = false;			// VolumetricLightGrid -> VolumetricFog, VolumetricLight

	// post-tonemap
	bool					colorLUT = false;			// FusedPassPlan -> FinalEffect, ColoringLUT
	bool					cinemaDither = false;		// FusedPassPlan -> FinalEffect, Cinema, SkyDither
	GpuTimer*				imageAdjustTimer = nullptr;	// "Image adjust + debug": one timer, two steps (ImageAdjust, Debug)
};

namespace EffectChain {
	enum Stage { PreTonemap, PostTonemap };
	typedef void (ShaderManager::*StepFunction)(ChainContext& ctx);
	struct Step {
		const char*		name;
		Stage			stage;
		StepFunction	function;
	};
	// Runs the stage's table in order (stops early only if a step sets ctx.stop).
	void Run(ShaderManager* manager, Stage stage, ChainContext& ctx);
}
