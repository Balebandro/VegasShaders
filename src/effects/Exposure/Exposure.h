#pragma once

class ExposureEffect : public EffectRecord
{
public:
	ExposureEffect() : EffectRecord("Exposure") {};

	struct ValuesStruct { float MinBrightness, MaxBrightness, DarkAdaptSpeed, LightAdaptSpeed; };

	struct ExposureSettingsStruct {
		ValuesStruct Main, Night, Interiors;
		// UNOFFICIAL 2026-10-26 [Shaders.Exposure.Main] Histogram (needs NVR DXVK): the screen average leaves out the darkest LowPercent
		// and the brightest HighPercent of the pixels (AvgLuma.cpp RenderCompute, Compute\AvgLumaHistogram.comp). Live.
		bool	Histogram;
		float	HistogramLowPercent;
		float	HistogramHighPercent;
	};
	ExposureSettingsStruct	Settings;

	struct ExposureStruct { D3DXVECTOR4 Data; };
	ExposureStruct	Constants;

	void	UpdateConstants();
	void	RegisterConstants();
	void	UpdateSettings();

	// UNOFFICIAL 2026-10-23 (NVR DXVK): the pass as src\hlsl\NewVegas\Compute\Exposure.frag (EffectRecord::ExtRender)
	bool	ExtRender(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, IDirect3DSurface9* RenderedSurface, UINT techniqueIndex, bool ClearRenderTarget, IDirect3DSurface9* SourceBuffer) override;
private:
	ExtPass				extPass;
};
