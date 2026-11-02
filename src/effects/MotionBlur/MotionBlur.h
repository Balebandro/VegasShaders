#pragma once

class MotionBlurEffect : public EffectRecord
{
public:
	MotionBlurEffect() : EffectRecord("MotionBlur") { ZeroMemory(&Constants, sizeof(Constants)); };	// UNOFFICIAL 2026-10-22: zeroed (the old angles were read on the first frame before ever being set)

	struct ValuesStruct { float BlurCutOff, GaussianWeight, BlurScale, BlurOffsetMax; };

	struct MotionBlurSettings { ValuesStruct ThirdPerson, FirstPerson; };
	MotionBlurSettings Settings;

	struct MotionBlurStruct {
		D3DXVECTOR4		BlurParams, Data;
		float			oldAngleX, oldAngleZ;
		float			oldAmountX, oldAmountY;
		float			oldoldAmountX, oldoldAmountY;
	};
	MotionBlurStruct	Constants;

	void	UpdateConstants();
	void	RegisterConstants();
	void	UpdateSettings();
	// UNOFFICIAL 2026-10-23 (NVR DXVK): the pass as src\hlsl\NewVegas\Compute\MotionBlur.frag (EffectRecord::ExtRender)
	bool	ExtRender(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, IDirect3DSurface9* RenderedSurface, UINT techniqueIndex, bool ClearRenderTarget, IDirect3DSurface9* SourceBuffer) override;
	ExtPass	extPass;
	bool	ShouldRender();
};
