#pragma once

// UNOFFICIAL, testbed (2026-10-13): a police body-camera look (Shaders > Bodycam, off by default). At the very end of the chain,
// after anti-aliasing and the cinema overlay: a wide lens that bends straight lines (fisheye, scaled so no black corners
// show), colour fringes towards the edges, the camera's oversharpening halos, highlights that bloom and clip, punchy but
// washed-out colour with a faint green cast, sensor noise and video-compression blocks (mostly in the dark), a vignette, and
// a timestamp in the corner (the game's date and clock) drawn from a small texture this effect writes when the text changes.
// Also: a soft black oval round the frame, the cheap lens blurring towards its edges, overexposure, a smear when the view
// turns (from the player's turn since last frame, as MotionBlur), and lens dirt lit up by bright light.
class BodycamEffect : public EffectRecord
{
public:
	BodycamEffect() : EffectRecord("Bodycam") {};

	struct BodycamStruct {
		D3DXVECTOR4	Lens;	// x: fisheye strength, y: zoom that keeps the corners inside the image, z: colour fringe (pixels at 1080p), w: vignette
		D3DXVECTOR4	Image;	// x: sharpening, y: contrast, z: saturation, w: highlight glow
		D3DXVECTOR4	Noise;	// x: grain, y: compression blocks, z: frame number (30 a second), w: green tint
		D3DXVECTOR4	Text;	// x: text opacity, y: text texel size (pixels at 1080p), z: text width (texels), w: text height (texels)
		D3DXVECTOR4	Frame;	// x: black oval border, y: edge blur, z: exposure, w: lens dirt
		D3DXVECTOR4	Motion;	// xy: motion blur (uv the picture moved this frame, times the setting), z: motion blur setting, w: shadow lift
		D3DXVECTOR4	Grade;	// x: cool (neutralises the game's yellow grade), y: lens blur, zw: unused
		// UNOFFICIAL 2026-10-13 (user: still missing something; picked 30 fps video, slow auto exposure, breakdown when moving):
		D3DXVECTOR4	Video;	// x: compression breakdown now (setting x how fast the view moves), y/z: rolling-shutter lean across/down (uv over
							//    the picture's height), w: 1 show the held video frame (the picture is not drawn again)
		D3DXVECTOR4	Adapt;	// x: auto exposure strength, y: this frame's seconds, z: seconds the exposure takes to follow, w: 1 measured
		// UNOFFICIAL 2026-10-13 (user: the rest of the list too): the body the camera is strapped to, the gunfire, the dark, the
		// encoder and the autofocus.
		D3DXVECTOR4	Body;	// xy: the picture's shift (uv; the chest bobbing and the gun's jolt), z: its roll (radians), w: zoom that hides the edges
		D3DXVECTOR4	Flash;	// x: muzzle-flash overload now (0-1), y: exposure dip after it (0-1), z: low-light mode, w: light smear
		D3DXVECTOR4	Glitch;	// x: broken-block burst now (0-1), y: its seed, z: autofocus strength, w: seconds this frame
	};
	BodycamStruct	Constants;

	// Every effect has a value by day (Main), at night (blended with the day's through dawn and dusk) and indoors. UNOFFICIAL
	// 2026-10-13 (user: the whole suite per lighting condition, not only the grade). Kept as one array so they blend alike.
	enum LookKey { Fisheye, FitEdges, Border, Fringe, EdgeBlur, LensBlur, Vignette, Sharpen, Exposure, Contrast, Saturation, Lift, Cool,
		Glow, Grain, Blocks, MotionBlur, LensDirt, Tint, Text, VideoFps, AutoExposure, ExposureSpeed, Breakup, RollingShutter,
		Bounce, GunJolt, Overload, LowLight, LightSmear, Hiccups, Autofocus, LookKeys };
	struct LookStruct { float v[LookKeys]; };
	LookStruct		Day = {}, Night = {}, Interiors = {};
	float			LastYaw = 0.0f, LastPitch = 0.0f;	// the view's turn last frame (degrees), for the motion blur
	bool			HaveLast = false;

	static const int TextWidth = 256;	// the text texture: two lines of up to 64 characters, 4 x 6 texels each
	static const int TextHeight = 12;
	IDirect3DTexture9*	TextTexture = nullptr;
	// UNOFFICIAL 2026-10-13: the last video frame (VideoFps), shown again until the next one is due; and the measured brightness
	// (1 x 1, r: now, g: what the camera's exposure has followed to), two of them: one read, one written each frame.
	IDirect3DTexture9*	HoldTexture = nullptr;
	IDirect3DSurface9*	HoldSurface = nullptr;
	D3DSURFACE_DESC		HoldDesc = {};
	bool				HoldValid = false;
	double				HoldTick = -1.0;	// the video frame the held picture is
	double				LastRender = -1.0;	// Clock at the last draw (a pause or a menu makes the held picture stale)
	IDirect3DTexture9*	MeasureTexture[2] = {};
	IDirect3DSurface9*	MeasureSurface[2] = {};
	IDirect3DTexture9*	MeasureRead = nullptr;	// what TESR_BodycamExposureBuffer binds: the latest
	int					MeasureIndex = 0;
	bool				MeasureValid = false;
	D3DXVECTOR3			LastPos = D3DXVECTOR3(0.0f, 0.0f, 0.0f);
	float				MotionLevel = 0.0f;	// how fast the view moves, 0-1.5 (turning and running), smoothed
	float				VideoFrameRate = 0.0f;	// VideoFps now (0: every frame drawn)
	char				Shown[2][65] = {};	// the text in the texture now
	double				Clock = 0.0;		// real seconds, for the frame number and the timestamp's seconds
	// UNOFFICIAL 2026-10-13: the chest mount's bob (step phase, smoothed amount), the gun's jolt (a spring), shots seen from the
	// ammo count, the flash and the exposure dip after it, encoder hiccups (a frozen picture or a burst of broken blocks).
	float				StepPhase = 0.0f, StepAmount = 0.0f;
	D3DXVECTOR3			Jolt = D3DXVECTOR3(0.0f, 0.0f, 0.0f), JoltSpeed = D3DXVECTOR3(0.0f, 0.0f, 0.0f);	// x, y (uv), z roll
	float				TurnRoll = 0.0f;
	const void*			AmmoType = nullptr;
	int					AmmoCount = -1;
	float				FlashNow = 0.0f, DipNow = 0.0f;
	float				HiccupWait = 3.0f, GlitchLeft = 0.0f, FreezeLeft = 0.0f;
	bool				Frozen = false;
	float				WideView = 0.0f;	// [Shaders.Bodycam.Main] WideView: the world's field of view while on (degrees, 0 = the game's)
	static void			ApplyWideView();	// once a frame (CameraManager::SetSceneGraph)

	void	UpdateConstants();
	void	RegisterConstants();
	void	RegisterTextures();
	void	UpdateSettings();
	bool	ShouldRender() { return true; };
	void	Render(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, IDirect3DSurface9* RenderedSurface, UINT techniqueIndex,
		bool ClearRenderTarget, IDirect3DSurface9* SourceBuffer) override;
	// UNOFFICIAL 2026-10-25 (NVR DXVK): the picture as src\hlsl\NewVegas\Compute\Bodycam.frag (technique 0) and the brightness
	// measure as BodycamMeasure.frag (technique 1); the held frame, the text and the copies stay on the D3D9 side.
	bool	ExtRender(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, IDirect3DSurface9* RenderedSurface, UINT techniqueIndex, bool ClearRenderTarget, IDirect3DSurface9* SourceBuffer) override;

private:
	void	WriteText(const char* line1, const char* line2);
	// Two samplers in one pass (ExtTexturePass has one): slot 0 LINEAR/CLAMP, slot 1 POINT/CLAMP, heap indices at samplerPushOffset
	// and samplerPushOffset + 4 of the push block; the textures bind in order (set 1, binding i).
	bool	ExtTwoSamplers(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, IDirect3DSurface9* RenderedSurface, IDirect3DSurface9* SourceBuffer,
				ExtPass& pass, const char* spvName, IDirect3DBaseTexture9* const* textures, UINT count, const void* push, UINT pushSize, UINT samplerPushOffset);
	ExtPass	extPass, extMeasure;
};
