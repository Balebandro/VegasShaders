#pragma once

class SharpeningEffect : public EffectRecord
{
public:
	SharpeningEffect() : EffectRecord("Sharpening") {};

	struct SharpeningStruct {
		D3DXVECTOR4		Data;
	};
	SharpeningStruct	Constants;
	D3DXVECTOR4			Exterior = D3DXVECTOR4(0.0f, 0.0f, 0.0f, 0.0f); // x: strength, y: clamp, z: offset
	D3DXVECTOR4			Interior = D3DXVECTOR4(0.0f, 0.0f, 0.0f, 0.0f);

	void	UpdateConstants();
	void	RegisterConstants();
	void	UpdateSettings();

	// UNOFFICIAL 2026-10-23 (NVR DXVK): the pass as src\hlsl\NewVegas\Compute\Sharpening.frag (EffectRecord::ExtRender)
	bool	ExtRender(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, IDirect3DSurface9* RenderedSurface, UINT techniqueIndex, bool ClearRenderTarget, IDirect3DSurface9* SourceBuffer) override;
private:
	ExtPass				extPass;

};