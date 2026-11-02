#pragma once

class WaterShaders : public ShaderCollection
{
public:
	WaterShaders() : ShaderCollection("Water") {};

	struct WaterStruct {
		D3DXVECTOR4		waterCoefficients, waveParams, waterVolume, waterSettings, shorelineParams;
	};

	struct WaterConstants {
		WaterStruct		Default, Placed;
		D3DXVECTOR4		Fog, deepColor, shallowColor, LODColor;
	};
	WaterConstants		Constants;

	float	causticsStrength;

	void	UpdateConstants();
	void	RegisterConstants();
	void	UpdateSettings();
};
