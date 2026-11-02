#pragma once

class BloodLensEffect : public EffectRecord
{
public:
	BloodLensEffect() : EffectRecord("BloodLens") {
		Constants.Percent = 0.0f;
	};

	struct BloodLensStruct {
		D3DXVECTOR4		Params, BloodColor, Time;
		float			Percent;
	};
	BloodLensStruct	Constants;

	float effectTime;
	float intensity = 0.0f;	// UNOFFICIAL fixes-b #45: [Shaders.BloodLens.Main] Intensity, applied to Params.w every frame

	void	UpdateConstants();
	void	RegisterConstants();
	void	UpdateSettings();
	// UNOFFICIAL 2026-10-23 (NVR DXVK): the pass as src\hlsl\NewVegas\Compute\BloodLens.frag (EffectRecord::ExtRender)
	bool	ExtRender(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, IDirect3DSurface9* RenderedSurface, UINT techniqueIndex, bool ClearRenderTarget, IDirect3DSurface9* SourceBuffer) override;
	ExtPass	extPass;
	bool	ShouldRender();
};
