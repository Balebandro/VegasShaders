#pragma once

class ImageAdjustEffect : public EffectRecord
{
public:
	ImageAdjustEffect() : EffectRecord("ImageAdjust") {};

	struct ValuesStruct {
		float Brightness, Contrast, Saturation, Strength;
		float DarkColorR, DarkColorG, DarkColorB;
		float LightColorR, LightColorG, LightColorB;
	};

	struct ImageAdjustSettingsStruct { ValuesStruct Main, Night, Interiors; };
	ImageAdjustSettingsStruct Settings;

	struct ImageAdjustStruct { D3DXVECTOR4 Data, DarkColor, LightColor; };
	ImageAdjustStruct	Constants;

	void	UpdateConstants();
	void	RegisterConstants();
	void	UpdateSettings();
	bool	ShouldRender() override;

	// UNOFFICIAL 2026-10-23 (NVR DXVK): the pass as src\hlsl\NewVegas\Compute\ImageAdjust.frag (EffectRecord::ExtRender)
	bool	ExtRender(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, IDirect3DSurface9* RenderedSurface, UINT techniqueIndex, bool ClearRenderTarget, IDirect3DSurface9* SourceBuffer) override;
private:
	ExtPass				extPass;
};
