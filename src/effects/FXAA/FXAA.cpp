#include "FXAA.h"
#include "../../core/DxvkCompute.h"	// UNOFFICIAL 2026-10-23 (NVR DXVK)

bool FXAAEffect::ShouldRender() {
	return TheSettingManager->SettingsMain.Main.FXAA;
}

// UNOFFICIAL 2026-10-23 (NVR DXVK): FXAA.frag; the textures and constants the technique reads (EffectRecord::ExtTexturePass).
bool FXAAEffect::ExtRender(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, IDirect3DSurface9* RenderedSurface, UINT techniqueIndex, bool ClearRenderTarget, IDirect3DSurface9* SourceBuffer) {
	if (techniqueIndex != 0) return false;
	struct Push { D3DXVECTOR4 reciprocalResolution; UInt32 samplerIndex; UInt32 pad[3]; } push = {};
	push.reciprocalResolution = TheShaderManager->ShaderConst.ReciprocalResolution;
	IDirect3DBaseTexture9* textures[] = { TheTextureManager->RenderedTexture };
	return ExtTexturePass(Device, RenderTarget, RenderedSurface, SourceBuffer, extPass, "FXAA", textures, _countof(textures), &push, sizeof(push), 16);
}

