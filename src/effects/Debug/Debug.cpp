#include "Debug.h"

void DebugEffect::UpdateConstants() {}

void DebugEffect::UpdateSettings() {
	Constants.DebugVar.x = TheSettingManager->GetSettingF("Main.Develop.Main", "DebugVar1");
	Constants.DebugVar.y = TheSettingManager->GetSettingF("Main.Develop.Main", "DebugVar2");
	Constants.DebugVar.z = TheSettingManager->GetSettingF("Main.Develop.Main", "DebugVar3");
	Constants.DebugVar.w = TheSettingManager->GetSettingF("Main.Develop.Main", "DebugVar4");

	// UNOFFICIAL: clipping view, 0 off, 1 finished picture, 2 tonemapper output, 3 both compared
	ClipViewMode = TheSettingManager->GetSettingI("Shaders.Debug.Main", "ClipView");
	if (ClipViewMode < 0 || ClipViewMode > 3) ClipViewMode = 0;
	Constants.ClipView = D3DXVECTOR4((float)ClipViewMode, 0.0f, 0.0f, 0.0f);
	if (ClipViewMode < 2) ReleaseClipStart(); // only modes 2 and 3 need the copy
}

void DebugEffect::RegisterConstants() {
	TheShaderManager->RegisterConstant("TESR_DebugVar", &Constants.DebugVar);
	TheShaderManager->RegisterConstant("TESR_ClipView", &Constants.ClipView);
}

// UNOFFICIAL: the slot is registered empty; the texture is created on first use by CaptureClipStart (samplers follow
// the slot, TextureRecord::TextureRef), so nothing is allocated unless the clipping view needs it.
void DebugEffect::RegisterTextures() {
	TheTextureManager->RegisterTexture("TESR_ClipStartBuffer", (IDirect3DBaseTexture9**)&ClipStartTexture);
}

int DebugEffect::ClipTechnique() {
	if (!Effect) return -1;
	D3DXHANDLE wanted = Effect->GetTechniqueByName("ClipView");
	if (!wanted) return -1;
	D3DXEFFECT_DESC desc = {};
	if (FAILED(Effect->GetDesc(&desc))) return -1;
	for (UINT i = 0; i < desc.Techniques; i++)
		if (Effect->GetTechnique(i) == wanted) return (int)i;
	return -1;
}

bool DebugEffect::ClipViewActive() {
	if (ClipViewMode <= 0 || ClipTechnique() < 0) return false;
	return ClipViewMode == 1 || ClipStartTexture != nullptr;
}

void DebugEffect::ReleaseClipStart() {
	if (ClipStartSurface) { ClipStartSurface->Release(); ClipStartSurface = nullptr; }
	if (ClipStartTexture) { ClipStartTexture->Release(); ClipStartTexture = nullptr; }
}

// Called at the start of the post chain, while the game target still holds the picture the tonemapper produced.
void DebugEffect::CaptureClipStart(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget) {
	if (ClipViewMode < 2 || !RenderTarget || ClipTechnique() < 0) return;
	D3DSURFACE_DESC desc = {};
	if (FAILED(RenderTarget->GetDesc(&desc))) return;
	if (ClipStartSurface) {
		D3DSURFACE_DESC have = {};
		if (FAILED(ClipStartSurface->GetDesc(&have)) || have.Width != desc.Width || have.Height != desc.Height || have.Format != desc.Format)
			ReleaseClipStart();
	}
	if (!ClipStartTexture) {
		if (FAILED(Device->CreateTexture(desc.Width, desc.Height, 1, D3DUSAGE_RENDERTARGET, desc.Format, D3DPOOL_DEFAULT, &ClipStartTexture, NULL)) ||
			FAILED(ClipStartTexture->GetSurfaceLevel(0, &ClipStartSurface))) {
			ReleaseClipStart();
			static bool reported = false;
			if (!reported) { Logger::Log("[ERROR] Clipping view: could not create its %ux%u copy (format %u); modes 2 and 3 are unavailable.", desc.Width, desc.Height, desc.Format); reported = true; }
			return;
		}
	}
	if (FAILED(Device->StretchRect(RenderTarget, NULL, ClipStartSurface, NULL, D3DTEXF_NONE))) ReleaseClipStart();
}

// Called after every other post effect. Renders like any effect (frame chain included); the Debug effect's own
// switch is overridden for this one call so the view does not need the debug overlays turned on.
void DebugEffect::RenderClipView(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget) {
	if (!ClipViewActive()) return;
	const bool wasEnabled = Enabled;
	Enabled = true;
	Render(Device, RenderTarget, TheTextureManager->RenderedSurface, (UINT)ClipTechnique(), false, nullptr);
	Enabled = wasEnabled;
}