#pragma once

class DitherBusterEffect : public EffectRecord
{
public:
	DitherBusterEffect() : EffectRecord("DitherBuster") {};

	struct DitherBusterSettingsStruct {
		float Strength;
		float MaskPower;
		float DitherThreshold;
		float Radius;
		float GateSoftness;	// UNOFFICIAL aastab: 0 = hard isolation gate, up to 1 = smooth ramp over DitherThreshold * (1 -+ softness)
		bool DeDither;		// UNOFFICIAL aastab: judge isolation on the picture without the tonemap's dither noise
		int DebugView;		// UNOFFICIAL aastab: 1 = the picture with the recomputed noise taken out
	};
	DitherBusterSettingsStruct Settings;

	struct DitherBusterStruct {
		D3DXVECTOR4		Data;
		D3DXVECTOR4			Gate;	// UNOFFICIAL aastab: x gate softness, y de-dither, z debug view
	};

	void	UpdateConstants();
	void	RegisterConstants();
	void	UpdateSettings();
	// UNOFFICIAL aastab: technique 1 (soft gate / de-dithered judging / debug view) only when one of its options is on, else the original pass 0.
	UINT	TechniqueIndex() const { return (Settings.GateSoftness > 0.0f || Settings.DeDither || Settings.DebugView != 0) ? 1 : 0; }

	DitherBusterStruct	Constants;

	// UNOFFICIAL 2026-10-25 (NVR DXVK): technique 0 as src\hlsl\NewVegas\Compute\DitherBuster.frag, technique 1 as DitherBusterStable.frag (EffectRecord::ExtRender)
	bool	ExtRender(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, IDirect3DSurface9* RenderedSurface, UINT techniqueIndex, bool ClearRenderTarget, IDirect3DSurface9* SourceBuffer) override;
private:
	ExtPass				extPass;
	ExtPass				extPassStable;
};
