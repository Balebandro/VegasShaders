#pragma once

class TonemappingShaders : public ShaderCollection
{
public:
	TonemappingShaders() : ShaderCollection("Tonemapping") {};

	struct ValuesStruct {
		float HighlightSaturation, WeatherContrast, ToneMappingColor, Linearization;
		float BloomBlend, BloomStrength, WeatherModifier, TonemapWhitePoint;
		float TonemappingMode, Exposure, Saturation, Gamma;

		float TonemapContrast, TonemapBrightness, TonemapMidpoint, TonemapShoulder;
	};

	struct TonemappingSettingsStruct { ValuesStruct Main, Night, Interiors; };
	TonemappingSettingsStruct Settings;

	struct TonemappingStruct {
		D3DXVECTOR4		ToneMapping, BloomData, SkyData, HDRData, LotteData;
		D3DXVECTOR4		Clip;			// UNOFFICIAL TESR_ToneMappingClip: x HueSafeClip
		float			PointLightMult;
	};
	TonemappingStruct Constants;

	void	UpdateConstants();
	static void	ScaleLotte(D3DXVECTOR4& LotteData, float whitePoint);	// UNOFFICIAL: the VTLottes constants (also the self-test's VTLottes shot)
	void	RegisterConstants();
	void	UpdateSettings();
};
