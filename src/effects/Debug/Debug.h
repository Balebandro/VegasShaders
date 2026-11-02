#pragma once

class DebugEffect : public EffectRecord
{
public:
	DebugEffect() : EffectRecord("Debug") {};

	struct DebugStruct {
		D3DXVECTOR4				DebugVar;
		D3DXVECTOR4				ClipView;	// UNOFFICIAL: x the clipping view's mode ([Shaders.Debug.Main] ClipView)
	};
	DebugStruct	Constants;

	// UNOFFICIAL: the clipping view. Drawn by the effect's ClipView technique after every other post effect,
	// whether or not the Debug effect itself is switched on. Modes 2 and 3 also need the picture as the post
	// chain received it (straight out of the tonemapper), copied at the chain's start into ClipStartTexture.
	int                     ClipViewMode = 0;
	IDirect3DTexture9*      ClipStartTexture = nullptr;
	IDirect3DSurface9*      ClipStartSurface = nullptr;
	int                     ClipTechnique();        // index of the ClipView technique, -1 when the loaded Debug.fx has none
	bool                    ClipViewActive();       // the view will draw this frame
	void                    CaptureClipStart(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget);
	void                    RenderClipView(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget);
	void                    ReleaseClipStart();

	void	UpdateConstants();
	void	RegisterConstants();
	void	RegisterTextures();
	void	UpdateSettings();

};