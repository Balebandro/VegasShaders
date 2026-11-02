#include "FlashlightBeam.h"
#include "../../core/DxvkCompute.h"	// UNOFFICIAL 2026-10-25 (NVR DXVK)

void FlashlightBeamEffect::RegisterConstants() {
	ShaderManager* sm = TheShaderManager;
	sm->RegisterConstant("TESR_VolumetricControl", &Constants.Control);
}

void FlashlightBeamEffect::RegisterTextures() {
	// Half resolution: the march is the expensive part and the shaft has no fine detail.
	// The Flashlight Combine pass reads it through a bilinear sampler, which upsamples it.
	const UINT w = MaxQuality::Reduced(TheRenderManager->width, 2);
	const UINT h = MaxQuality::Reduced(TheRenderManager->height, 2);
	TheTextureManager->InitTexture("TESR_VolumetricBuffer", &Textures.VolumetricTexture, &Textures.VolumetricSurface,
		w, h, D3DFMT_A16B16G16R16F);	// UNOFFICIAL: full with Max Quality
}

void FlashlightBeamEffect::UpdateSettings() {
	// UpdateConstants only runs while this effect is enabled, so switching it off would
	// otherwise leave the last gate value standing and the Flashlight Combine pass would
	// keep compositing a stale buffer. UpdateSettings runs on every settings change
	// regardless of enabled state, and before UpdateConstants, so clearing it here both
	// closes the gate on disable and leaves the enabled path free to set it again.
	Constants.Control = D3DXVECTOR4(0.0f, 0.0f, 0.0f, 0.0f);

	const char* section = "Shaders.FlashlightBeam.Main";
	SettingManager* cfg = TheSettingManager;
	Settings.FirstPerson = cfg->GetSettingI(section, "FirstPerson");
	Settings.ThirdPerson = cfg->GetSettingI(section, "ThirdPerson");
	Settings.Strength = cfg->GetSettingF(section, "Strength");
	Settings.Anisotropy = cfg->GetSettingF(section, "Anisotropy");
	Settings.HeightFalloff = cfg->GetSettingF(section, "HeightFalloff");
	Settings.MaxDistance = cfg->GetSettingF(section, "MaxDistance");
	Settings.FirstPersonStrength = cfg->GetSettingF(section, "FirstPersonStrength");
	Settings.FirstPersonOffsetRight = cfg->GetSettingF(section, "FirstPersonOffsetRight");
	Settings.FirstPersonOffsetDown = cfg->GetSettingF(section, "FirstPersonOffsetDown");
}

void FlashlightBeamEffect::UpdateConstants() {
	// Strength folds the effect toggle, the per view toggle and the configured strength into
	// one value, so a single > 0 test gates both the march and the Combine composite. That
	// matters because the buffer persists between frames: without the gate on the composite
	// side, the last shaft rendered would keep bleeding in after the beam is switched off.
	const bool thirdPerson = Player && Player->isThirdPerson;
	const bool viewOn = thirdPerson ? Settings.ThirdPerson : Settings.FirstPerson;
	const FlashlightEffect* torch = TheShaderManager->Effects.Flashlight;
	const bool torchLit = torch->Enabled && torch->spotLightActive;
	const float gate = (Enabled && viewOn && torchLit) ? Settings.Strength : 0.0f;

	Constants.Control = D3DXVECTOR4(gate, Settings.Anisotropy, Settings.HeightFalloff, Settings.MaxDistance);
}

// UNOFFICIAL 2026-10-25 (NVR DXVK): the constants FlashlightBeam.frag reads, in one std140 block (the two matrices are too big for
// push data). The layout is the D3DX matrices' memory as stored, so the shader's m[i][j] is the HLSL's (EffectRecord's SetMatrix
// hands the effect the same logical matrix).
struct FlashlightBeamUniforms {
	D3DXVECTOR4	volumetricControl;		// TESR_VolumetricControl
	D3DXVECTOR4	volumetricData;			// TESR_VolumetricData
	D3DXVECTOR4	spotLightPosition;		// TESR_SpotLightPosition
	D3DXVECTOR4	spotLightColor;			// TESR_SpotLightColor
	D3DXVECTOR4	spotLightDirection;		// TESR_SpotLightDirection
	D3DXVECTOR4	sunColor;				// TESR_SunColor
	D3DXVECTOR4	reciprocalResolution;	// TESR_ReciprocalResolution
	D3DXVECTOR4	cameraPosition;			// TESR_CameraPosition
	D3DXVECTOR4	cameraData;				// TESR_CameraData
	D3DXMATRIX	flashLightViewProj;		// TESR_FlashLightViewProjTransform
	D3DXMATRIX	viewTransform;			// TESR_ViewTransform
	D3DXMATRIX	projectionTransform;	// TESR_ProjectionTransform
};
static_assert(sizeof(FlashlightBeamUniforms) == 336, "FlashlightBeam.frag declares a 336-byte block");

// The two samplers' heap indices, filled in by the extension at these push offsets.
struct FlashlightBeamPush {
	UInt32	pointSamplerIndex;		// 0
	UInt32	cookieSamplerIndex;		// 4
};

static const NVR_EXT_BINDING_DESC FlashlightBeamBindings[] = {
	{ 1, 0, NVR_EXT_BIND_SAMPLED_IMAGE,  0, NVR_EXT_VIEW_2D, FALSE, 0 },	// t_depth: TESR_DepthBuffer
	{ 1, 1, NVR_EXT_BIND_SAMPLED_IMAGE,  1, NVR_EXT_VIEW_2D, FALSE, 0 },	// t_cookie: TESR_SpotLightTexture
	{ 2, 0, NVR_EXT_BIND_UNIFORM_BUFFER, 0, NVR_EXT_VIEW_2D, FALSE, 0 },	// Constants
	{ 0, 0, NVR_EXT_BIND_SAMPLER,        0, NVR_EXT_VIEW_2D, FALSE, 0 },	// POINT, CLAMP: index at push offset 0
	{ 0, 0, NVR_EXT_BIND_SAMPLER,        1, NVR_EXT_VIEW_2D, FALSE, 4 },	// LINEAR with mips, CLAMP: index at push offset 4
};

// The value of a registered constant as the D3D9 path uploads it (ConstantsTable; SetVector / SetMatrix copy it); null if unregistered.
static const D3DXVECTOR4* RegisteredConstant(const char* name) {
	auto found = TheShaderManager->ConstantsTable.find(name);
	return found == TheShaderManager->ConstantsTable.end() ? nullptr : found->second;
}

// UNOFFICIAL 2026-10-25 (NVR DXVK): FlashlightBeam.frag, one pass into the beam's own half resolution target (ExtDrawTo; the
// frame chain is not touched). The depth buffer is the texture the D3D9 technique samples (the TESR_DepthBuffer slot), the cookie
// the file texture behind TESR_SpotLightTexture through a trilinear CLAMP sampler (the technique's LINEAR mip states). The clear
// RenderEffectToRT asks for is not needed: the pass writes every pixel of the target.
bool FlashlightBeamEffect::ExtRender(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, IDirect3DSurface9* RenderedSurface, UINT techniqueIndex, bool ClearRenderTarget, IDirect3DSurface9* SourceBuffer) {
	if (techniqueIndex != 0) return false;
	std::string depthName = "TESR_DepthBuffer";
	IDirect3DBaseTexture9** depthSlot = TheTextureManager->GetTextureSlotByName(depthName);
	IDirect3DBaseTexture9* depth = depthSlot ? *depthSlot : nullptr;
	IDirect3DBaseTexture9* cookie = ExtFileTexture("TESR_SpotLightTexture");

	const D3DXVECTOR4* volumetricControl = RegisteredConstant("TESR_VolumetricControl");
	const D3DXVECTOR4* volumetricData = RegisteredConstant("TESR_VolumetricData");
	const D3DXVECTOR4* spotLightPosition = RegisteredConstant("TESR_SpotLightPosition");
	const D3DXVECTOR4* spotLightColor = RegisteredConstant("TESR_SpotLightColor");
	const D3DXVECTOR4* spotLightDirection = RegisteredConstant("TESR_SpotLightDirection");
	const D3DXVECTOR4* sunColor = RegisteredConstant("TESR_SunColor");
	const D3DXVECTOR4* reciprocalResolution = RegisteredConstant("TESR_ReciprocalResolution");
	const D3DXVECTOR4* cameraPosition = RegisteredConstant("TESR_CameraPosition");
	const D3DXVECTOR4* cameraData = RegisteredConstant("TESR_CameraData");
	const D3DXVECTOR4* lightViewProj = RegisteredConstant("TESR_FlashLightViewProjTransform");
	const D3DXVECTOR4* view = RegisteredConstant("TESR_ViewTransform");
	const D3DXVECTOR4* projection = RegisteredConstant("TESR_ProjectionTransform");
	if (!depth || !cookie || !volumetricControl || !volumetricData || !spotLightPosition || !spotLightColor || !spotLightDirection ||
		!sunColor || !reciprocalResolution || !cameraPosition || !cameraData || !lightViewProj || !view || !projection) return false;

	if (!extPass.Tried) {
		extPass.Tried = true;
		extPass.Pipeline = DxvkCompute::CreateGraphicsPipeline("FlashlightBeam", FlashlightBeamBindings, _countof(FlashlightBeamBindings), sizeof(FlashlightBeamPush), true);
	}
	ID3D9NvrExtSampler* pointSampler = DxvkCompute::PointClamp();
	ID3D9NvrExtSampler* cookieSampler = DxvkCompute::MipSampler(false, NVR_EXT_ADDRESS_CLAMP);
	if (!extPass.Pipeline || !pointSampler || !cookieSampler) return false;

	FlashlightBeamUniforms uniforms = {};
	uniforms.volumetricControl = *volumetricControl;
	uniforms.volumetricData = *volumetricData;
	uniforms.spotLightPosition = *spotLightPosition;
	uniforms.spotLightColor = *spotLightColor;
	uniforms.spotLightDirection = *spotLightDirection;
	uniforms.sunColor = *sunColor;
	uniforms.reciprocalResolution = *reciprocalResolution;
	uniforms.cameraPosition = *cameraPosition;
	uniforms.cameraData = *cameraData;
	memcpy(&uniforms.flashLightViewProj, lightViewProj, sizeof(D3DXMATRIX));
	memcpy(&uniforms.viewTransform, view, sizeof(D3DXMATRIX));
	memcpy(&uniforms.projectionTransform, projection, sizeof(D3DXMATRIX));
	ID3D9NvrExtBuffer* uniformBuffer = ExtUniforms(extUniforms, &uniforms, sizeof(uniforms));
	if (!uniformBuffer) return false;

	NVR_EXT_RESOURCE_BINDING resources[5] = {};
	resources[0].Slot = 0; resources[0].Type = NVR_EXT_BIND_SAMPLED_IMAGE; resources[0].pResource = depth;
	resources[1].Slot = 1; resources[1].Type = NVR_EXT_BIND_SAMPLED_IMAGE; resources[1].pResource = cookie;
	resources[2].Slot = 0; resources[2].Type = NVR_EXT_BIND_UNIFORM_BUFFER; resources[2].pResource = uniformBuffer;
	resources[3].Slot = 0; resources[3].Type = NVR_EXT_BIND_SAMPLER; resources[3].pResource = pointSampler;
	resources[4].Slot = 1; resources[4].Type = NVR_EXT_BIND_SAMPLER; resources[4].pResource = cookieSampler;
	FlashlightBeamPush push = {};
	IDirect3DSurface9* targets[] = { RenderTarget };
	return ExtDrawTo(extPass.Pipeline, targets, _countof(targets), resources, _countof(resources), &push, sizeof(push), nullptr, ExtUv::ScreenQuad);	// FrameVS into a smaller target
}
