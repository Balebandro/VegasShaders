#pragma once

class LowHFEffect : public EffectRecord
{
public:
	LowHFEffect() : EffectRecord("LowHF") {};

	struct LowHFSettingsStruct {
		float healthLimit, FatigueLimit;
		float LumaMultiplier, BlurMultiplier, VignetteMultiplier, DarknessMultiplier;
	};
	LowHFSettingsStruct Settings;

	struct LowHFStruct {
		D3DXVECTOR4		Data;
		float			HealthCoeff, FatigueCoeff;
	};
	LowHFStruct	Constants;

	void	UpdateConstants();
	void	RegisterConstants();
	void	UpdateSettings();
	// UNOFFICIAL 2026-10-23 (NVR DXVK): the pass as src\hlsl\NewVegas\Compute\LowHF.frag (EffectRecord::ExtRender)
	bool	ExtRender(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, IDirect3DSurface9* RenderedSurface, UINT techniqueIndex, bool ClearRenderTarget, IDirect3DSurface9* SourceBuffer) override;
	ExtPass	extPass;
	bool	ShouldRender();
};
