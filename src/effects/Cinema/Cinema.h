#pragma once

class CinemaEffect : public EffectRecord
{
public:
	CinemaEffect() : EffectRecord("Cinema") {};

	struct CinemaStruct {
		D3DXVECTOR4		Data;
		D3DXVECTOR4		Settings;
	};

	float	Mode;
	float	aspectRatio;
	void	UpdateConstants();
	void	RegisterConstants();
	void	UpdateSettings();


	CinemaStruct	Constants;

	// UNOFFICIAL 2026-10-25 (NVR DXVK): the pass as src\hlsl\NewVegas\Compute\Cinema.frag (EffectRecord::ExtRender). Both
	// techniques: 0 = Cinema, 1 = CinemaSkyDither (Cinema plus the sky dither, the ShaderManager's combined pass).
	bool	ExtRender(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, IDirect3DSurface9* RenderedSurface, UINT techniqueIndex, bool ClearRenderTarget, IDirect3DSurface9* SourceBuffer) override;
private:
	ExtPass				extPass;
};