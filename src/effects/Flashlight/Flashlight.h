#pragma once

class FlashlightEffect : public EffectRecord
{
public:
	FlashlightEffect() : EffectRecord("Flashlight"), SpotLight(nullptr), spotLightActive(false), aimingHoldUntil(0.0) {};

	// UNOFFICIAL: the light's look, outdoors ([Shaders.Flashlight.Main]) and indoors ([Shaders.Flashlight.Interiors]);
	// ApplyLook copies the one for the current cell into Settings every frame.
	struct FlashlightLook {
		NiColor		Color;
		float		Dimmer, ConeAngle, Distance;
		float		NearFade, HotspotLimit, CookieStrength;
	};
	FlashlightLook	Looks[2]; // 0: outdoors, 1: interiors
	void			ApplyLook(bool exterior);

	struct FlashlightSettingsStruct {
		NiColor		Color;
		float		Dimmer, ConeAngle, Distance;
		bool		renderShadows;
		NiPoint3	Offset;
		bool		attachToWeapon;
		float		NearFade, HotspotLimit, CookieStrength;
		bool		softEdges;
		int			edgeFix;	// UNOFFICIAL: depth-aware edges, 0 off, 1 the lit surfaces, 2 also the beam's shaft ([Shaders.Flashlight.Main] EdgeFix)
		bool		hidePipBoyLight;	// UNOFFICIAL 2026-10-28 [Shaders.Flashlight.Main] HidePipBoyLight: the hidden Pip-Boy light also gets no brightness or reach
		bool		testForceOn;		// UNOFFICIAL 2026-10-28 [Shaders.Flashlight.Main] TestForceOn (test only): the flashlight on as if the Pip-Boy light were

		// Forward re-light of nearby static geometry, drawn by MaterialPass
		struct MaterialLightStruct {
			bool	Enabled;
			float	Intensity, Specular, NormalStrength;
			int		MaxGeometry, DebugMode;
		};
		MaterialLightStruct	MaterialLight;
	};
	FlashlightSettingsStruct	Settings;
	NiPointLight*	hiddenPipBoyLight = nullptr;	// UNOFFICIAL 2026-10-28 HidePipBoyLight: the light we darkened, and its own values
	float			hiddenPipBoyDimmer = 0.0f;
	NiColor			hiddenPipBoySpec = NiColor(0, 0, 0);

	struct FlashlightStruct {
		D3DXMATRIX	FlashlightViewProj;
		D3DXVECTOR4	Position, Direction, Color;
		D3DXVECTOR4	Tuning;		// x near fade, y soft edges, z hotspot limit, w cookie strength
		D3DXVECTOR4	Composite;	// x source buffer is already linear; UNOFFICIAL (EdgeFix): y 1 = also the beam's shaft, zw 1 / beam buffer size
	};
	FlashlightStruct	Constants;

	NiSpotLight* SpotLight;
	bool	spotLightActive;	// true while the cone is lit
	double	aimingHoldUntil;	// real time the aiming latch holds until, see UpdateConstants
	int		selectedPass;
	int		TechniqueThisFrame();	// UNOFFICIAL 2026-10-30: selectedPass, or its shadowed technique while a traced shadow is there (RT lamp shadows Flashlight)

	void	UpdateConstants();	void	RegisterConstants();
	void	UpdateSettings();	bool	ShouldRender();

	void	GetFlashlightViewProj();	void	PublishLightConstants(bool abActive);
	int		TechniqueIndex(const char* name);	// UNOFFICIAL: -1 when the loaded effect has no technique of that name
	// UNOFFICIAL: the EdgeFix technique indices, found once per loaded effect instead of by a name scan every frame.
	void	RefreshTechniqueIndices();
	unsigned techniqueGeneration = 0;
	int		edgeFixIndex = -1, edgeFixShadowsIndex = -1;

	// UNOFFICIAL 2026-10-25 (NVR DXVK): the four techniques as src\hlsl\NewVegas\Compute\Flashlight*.frag (EffectRecord::ExtRender)
	bool	ExtRender(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, IDirect3DSurface9* RenderedSurface, UINT techniqueIndex, bool ClearRenderTarget, IDirect3DSurface9* SourceBuffer) override;
private:
	ExtPass				extShadows, extDepthBlur, extLight, extBoxBlur, extCombine;
	ExtPass				extShadowsRt;		// UNOFFICIAL 2026-10-30 FlashlightShadowsRT (RT lamp shadows Flashlight)
	ExtUniformBuffer	extUniforms;
};