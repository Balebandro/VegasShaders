#pragma once

// UNOFFICIAL: sky dither (Effects\SkyDither.fx.hlsl). The sky's smooth gradients show bands on an 8-bit screen; a faint
// noise of about one brightness step, added just before the picture goes to the screen, breaks the bands up so the eye
// sees a smooth gradient. Sky only by default (on textured surfaces it is invisible anyway and only costs a read).
class SkyDitherEffect : public EffectRecord
{
public:
	SkyDitherEffect() : EffectRecord("SkyDither") {};

	struct SettingsStruct {
		float	Strength;		// noise size, in steps of the 256 brightness levels
		bool	SkyOnly;		// only where the sky shows
		int		Pattern;		// UNOFFICIAL aastab: 0 = a new noise pattern every frame, 1 = one fixed pattern (no frame-to-frame change)
	};
	SettingsStruct	Settings;

	struct ConstantsStruct {
		D3DXVECTOR4	Data;		// x: noise size (1 = one 8-bit step), y: 1 = sky only, z: noise frame offset
	};
	ConstantsStruct	Constants;

	void	UpdateConstants();
	void	RegisterConstants();
	void	UpdateSettings();
	// UNOFFICIAL 2026-10-23 (NVR DXVK): the pass as src\hlsl\NewVegas\Compute\SkyDither.frag (EffectRecord::ExtRender)
	bool	ExtRender(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, IDirect3DSurface9* RenderedSurface, UINT techniqueIndex, bool ClearRenderTarget, IDirect3DSurface9* SourceBuffer) override;
	ExtPass	extPass;
	bool	ShouldRender() { return Settings.Strength > 0.0f; };

private:
	UINT	frame = 0;
};
