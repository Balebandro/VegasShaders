#pragma once

// Lean FXAA pass (Effects\FXAA.fx.hlsl), driven by [Main.Main.ReducedQuality] FXAA rather than a
// Shaders.FXAA section. The game's own MSAA (iMultiSample) is left to the player.
class FXAAEffect : public EffectRecord
{
public:
	FXAAEffect() : EffectRecord("FXAA") {};

	// UNOFFICIAL 2026-10-23 (NVR DXVK): the pass as src\hlsl\NewVegas\Compute\FXAA.frag (EffectRecord::ExtRender)
	bool	ExtRender(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, IDirect3DSurface9* RenderedSurface, UINT techniqueIndex, bool ClearRenderTarget, IDirect3DSurface9* SourceBuffer) override;
	ExtPass	extPass;
	bool	ShouldRender();
};
