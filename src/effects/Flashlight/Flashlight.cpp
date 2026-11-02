#include "Flashlight.h"
#include "../../core/DxvkCompute.h"	// UNOFFICIAL 2026-10-25 (NVR DXVK)
#include "../../core/RtLampShadows.h"	// UNOFFICIAL 2026-10-30 RT lamp shadows Flashlight


// Settings readers for this file: the section and key exactly as the settings file names them.
static int FlashSettingI(const char* section, const char* key) { return TheSettingManager->GetSettingI(section, key); }
static float FlashSettingF(const char* section, const char* key) { return TheSettingManager->GetSettingF(section, key); }

void FlashlightEffect::RegisterConstants() {
	// Name -> memory, in the order the constants were always registered.
	struct Binding {
		const char* name;
		D3DXVECTOR4* value;
	};
	const Binding bindings[] = {
		{ "TESR_FlashLightViewProjTransform", (D3DXVECTOR4*)&Constants.FlashlightViewProj },
		{ "TESR_FlashLightPosition", &Constants.Position },
		{ "TESR_FlashLightDirection", &Constants.Direction },
		{ "TESR_FlashLightColor", &Constants.Color },
		{ "TESR_FlashLightTuning", &Constants.Tuning },
		{ "TESR_FlashLightComposite", &Constants.Composite },
	};
	for (const Binding& binding : bindings) TheShaderManager->RegisterConstant(binding.name, binding.value);
};

void FlashlightEffect::UpdateSettings() {
	const char* const mainSection = "Shaders.Flashlight.Main";
	const char* const materialSection = "Shaders.Flashlight.MaterialLight";

	Settings.attachToWeapon = FlashSettingI(mainSection, "AttachToWeapon");
	Settings.renderShadows = FlashSettingI(mainSection, "RenderShadows");
	selectedPass = static_cast<int>(Settings.renderShadows);

	Settings.Offset = NiPoint3(FlashSettingF(mainSection, "OffsetX"), FlashSettingF(mainSection, "OffsetY"), FlashSettingF(mainSection, "OffsetZ"));

	// UNOFFICIAL: the look comes in an outdoor (Main) and an indoor (Interiors) version; ApplyLook picks one.
	static const char* const lookSections[2] = { "Shaders.Flashlight.Main", "Shaders.Flashlight.Interiors" };
	for (int i = 0; i < 2; i++) {
		FlashlightLook& look = Looks[i];
		const char* section = lookSections[i];
		look.Color = NiColor(FlashSettingF(section, "ColorR"), FlashSettingF(section, "ColorG"), FlashSettingF(section, "ColorB"));
		look.Dimmer = FlashSettingF(section, "Dimmer");
		look.ConeAngle = FlashSettingF(section, "Angle");
		look.Distance = FlashSettingF(section, "Distance");
		look.NearFade = FlashSettingF(section, "NearFade");
		look.HotspotLimit = FlashSettingF(section, "HotspotLimit");
		look.CookieStrength = FlashSettingF(section, "CookieStrength");
	}
	Settings.softEdges = FlashSettingI(mainSection, "SoftEdges");
	Settings.hidePipBoyLight = FlashSettingI(mainSection, "HidePipBoyLight") != 0;
	Settings.testForceOn = FlashSettingI(mainSection, "TestForceOn") != 0;
	// UNOFFICIAL: depth-aware edges, 0 off, 1 the lit surfaces, 2 also the beam's shaft
	Settings.edgeFix = (std::max)(0, (std::min)(2, (int)FlashSettingI(mainSection, "EdgeFix")));

	Settings.MaterialLight.Enabled = FlashSettingI(materialSection, "Enabled");
	Settings.MaterialLight.Intensity = FlashSettingF(materialSection, "Intensity");
	Settings.MaterialLight.Specular = FlashSettingF(materialSection, "Specular");
	Settings.MaterialLight.NormalStrength = FlashSettingF(materialSection, "NormalStrength");
	Settings.MaterialLight.MaxGeometry = FlashSettingI(materialSection, "MaxGeometry");
	Settings.MaterialLight.DebugMode = FlashSettingI(materialSection, "DebugMode");

	// With RenderPreTonemapping the effect chain draws onto the game's HDR scene surface, whose content is already linear.
	// Combine has to know, so it can skip the sRGB decode that only suits the LDR surface after tonemapping.
	const float linearSource = TheSettingManager->SettingsMain.Main.RenderPreTonemapping ? 1.0f : 0.0f;

	// These come purely from settings, so they are published here rather than in UpdateConstants, which only runs while the effect is on.
	ApplyLook(TheShaderManager->GameState.isExterior);
	// UNOFFICIAL: y 1 = EdgeFix also on the beam's shaft (EdgeFix 2); zw one texel of the half resolution beam buffer
	// (FlashlightBeamEffect::RegisterTextures), which EdgeFix's depth-aware beam upsample addresses directly.
	const float beamWidth = (float)(std::max)(1, (int)TheRenderManager->width / 2);
	const float beamHeight = (float)(std::max)(1, (int)TheRenderManager->height / 2);
	Constants.Composite = D3DXVECTOR4(linearSource, Settings.edgeFix >= 2 ? 1.0f : 0.0f, 1.0f / beamWidth, 1.0f / beamHeight);

	if (!SpotLight) SpotLight = NiSpotLight::CreateObject();
};

// UNOFFICIAL: the outdoor or indoor look into Settings, and the constants that come purely from it.
void FlashlightEffect::ApplyLook(bool exterior) {
	const FlashlightLook& look = Looks[exterior ? 0 : 1];
	Settings.Color = look.Color;
	Settings.Dimmer = look.Dimmer;
	Settings.ConeAngle = look.ConeAngle;
	Settings.Distance = look.Distance;
	Settings.NearFade = look.NearFade;
	Settings.HotspotLimit = look.HotspotLimit;
	Settings.CookieStrength = look.CookieStrength;
	const float softEdgeFlag = Settings.softEdges ? 1.0f : 0.0f;
	Constants.Tuning = D3DXVECTOR4(Settings.NearFade, softEdgeFlag, Settings.HotspotLimit, Settings.CookieStrength);
}

// UNOFFICIAL: index of a technique by name, -1 when the loaded Flashlight.fx has none of that name.
int FlashlightEffect::TechniqueIndex(const char* name) {
	if (!Effect) return -1;
	D3DXHANDLE wanted = Effect->GetTechniqueByName(name);
	D3DXEFFECT_DESC desc = {};
	if (!wanted || FAILED(Effect->GetDesc(&desc))) return -1;
	for (UINT i = 0; i < desc.Techniques; i++)
		if (Effect->GetTechnique(i) == wanted) return (int)i;
	return -1;
}

// UNOFFICIAL: see Flashlight.h.
void FlashlightEffect::RefreshTechniqueIndices() {
	if (techniqueGeneration == LoadGeneration) return;
	techniqueGeneration = LoadGeneration;
	edgeFixIndex = TechniqueIndex("EdgeFix");
	edgeFixShadowsIndex = TechniqueIndex("EdgeFixShadows");
}

// The weapon types the light does not ride on: melee, grenades, mines and thrown weapons (there is no sight to look down).
static bool IsMeleeWeaponType(UInt8 type) {
	switch (type) {
	case TESObjectWEAP::WeaponType::kWeapType_HandToHandMelee:
	case TESObjectWEAP::WeaponType::kWeapType_OneHandMelee:
	case TESObjectWEAP::WeaponType::kWeapType_TwoHandMelee:
	case TESObjectWEAP::WeaponType::kWeapType_OneHandGrenade:
	case TESObjectWEAP::WeaponType::kWeapType_OneHandMine:
	case TESObjectWEAP::WeaponType::kWeapType_OneHandLunchboxMine:
	case TESObjectWEAP::WeaponType::kWeapType_OneHandThrown:
		return true;
	default:
		return false;
	}
}

void FlashlightEffect::UpdateConstants() {

	ApplyLook(TheShaderManager->GameState.isExterior);

	// UNOFFICIAL: EdgeFix runs the same passes from techniques of their own, so with it off the original
	// techniques (0 without shadows, 1 with) run untouched. A Flashlight.fx without them keeps the original.
	selectedPass = Settings.renderShadows ? 1 : 0;
	if (Settings.edgeFix > 0) {
		RefreshTechniqueIndices();
		const int edgeTechnique = Settings.renderShadows ? edgeFixShadowsIndex : edgeFixIndex;
		if (edgeTechnique >= 0) selectedPass = edgeTechnique;
	}

	if (SpotLight == nullptr) return;

	// Whether the game has the flashlight on: the test JIP uses (functions_jip/jip_fn_ui.h), with TestForceOn for the auto tests.
	uint8_t* const magicBase = ThisStdCall<uint8_t*>(0x93CCD0, NULL);
	void* const magicSlot = (void*)&(*(MagicItem**)(magicBase + 0x18));
	UInt32 lightIsOn = ThisStdCall<bool>(0x822B90, &Player->magicTarget, magicSlot, 1);
	if (Settings.testForceOn) lightIsOn = 1;	// UNOFFICIAL 2026-10-28 TestForceOn (auto tests)

	const bool active = Enabled && lightIsOn != 0;
	spotLightActive = active;
	// UNOFFICIAL 2026-10-28 HidePipBoyLight: the Pip-Boy light's own values back once the flashlight is off (see below).
	if (!active && hiddenPipBoyLight) {
		NiNode* playerNode = Player->GetNode();	// only if it is still the player's (the game may have let it go since)
		for (UINT32 i = 0; playerNode && i < playerNode->m_children.capacity; i++) {
			if (playerNode->m_children.data[i] == hiddenPipBoyLight) {
				hiddenPipBoyLight->Dimmer = hiddenPipBoyDimmer;
				hiddenPipBoyLight->Spec = hiddenPipBoySpec;
				break;
			}
		}
		hiddenPipBoyLight = nullptr;
	}
	if (!active) {
		// off: the light goes to zero dimmer and zero reach
		SpotLight->Dimmer = 0.0f;
		SpotLight->Spec = NiColor(0.0f, 0.0f, 0.0f);
		SpotLight->CastShadows = false;
		PublishLightConstants(false);
		return;
	}

	NiPoint3 weaponPos;
	NiMatrix33 weaponRot;
	bool meleeWeapon = false;
	if (Player->process->IsWeaponOut() && Player->ActorSkinInfo) {
		const TESObjectWEAP* weapon = Player->ActorSkinInfo->WeaponForm;
		meleeWeapon = !weapon || IsMeleeWeaponType(weapon->weaponType);
	}

	// Down the sights the light belongs on the gun, whatever AttachToWeapon says: the weapon is lined up with the view there, so
	// riding the head puts the beam behind the thing the player is looking past.
	//
	// IsAiming() cannot be used raw: it is false during the attack and follow-through states, so the light alternated between two
	// bones while firing. So it is latched: aiming takes hold at once, and releasing waits for the signal to stay false for a
	// little while (in real seconds, so the hold does not shorten as the frame rate rises). Melee is excluded, as for AttachToWeapon.
	static const double aimHoldSeconds = 0.35;
	const double frameTime = TheFrameRateManager->Time;
	if (Player->IsAiming()) aimingHoldUntil = frameTime + aimHoldSeconds;
	const bool aiming = frameTime < aimingHoldUntil;
	const bool rideWeapon = (Settings.attachToWeapon || aiming) && !meleeWeapon && Player->process->IsWeaponOut();

	if (Player->isThirdPerson) {
		if (rideWeapon && Player->ActorSkinInfo && Player->ActorSkinInfo->WeaponNode) {
			weaponPos = Player->ActorSkinInfo->WeaponNode->m_worldTransform.pos;
			weaponRot = Player->ActorSkinInfo->WeaponNode->m_worldTransform.rot;
		}
		else if (Player->ActorSkinInfo && Player->ActorSkinInfo->HeadNode) {
			// A turn of 90 degrees about Z, then X, applied in the head bone's own frame.
			static const float turnData[3][3] = { { 0.0f, -1.0f, 0.0f }, { 0.0f, 0.0f, 1.0f }, { -1.0f, 0.0f, 0.0f } };
			NiMatrix33 turn = NiMatrix33();
			for (int row = 0; row < 3; row++)
				for (int column = 0; column < 3; column++) turn.data[row][column] = turnData[row][column];

			const NiAVObject* head = Player->ActorSkinInfo->HeadNode;
			weaponPos = head->m_worldTransform.pos;
			const NiMatrix33 headRot = head->m_worldTransform.rot;
			turn = turn * headRot;	// the turn in the bone's frame
			weaponRot = headRot * turn;
		}
		else {
			weaponPos = WorldSceneGraph->camera->m_worldTransform.pos;
			weaponRot = WorldSceneGraph->camera->m_worldTransform.rot;
		}
	}
	else {
		if (rideWeapon && Player->firstPersonSkinInfo && Player->firstPersonSkinInfo->WeaponNode) {
			weaponPos = Player->firstPersonSkinInfo->WeaponNode->m_worldTransform.pos;
			weaponRot = Player->firstPersonSkinInfo->WeaponNode->m_worldTransform.rot;
		}
		else {
			weaponPos = WorldSceneGraph->camera->m_worldTransform.pos;
			weaponRot = WorldSceneGraph->camera->m_worldTransform.rot;
		}
	}

	// The offset, turned into the direction of the cone.
	const NiPoint3 offset = weaponRot * Settings.Offset;
	weaponPos.x += offset.x;
	weaponPos.y += offset.y;
	weaponPos.z += offset.z;

	if (active) {
		// The Pip-Boy light: culled, so it is not drawn. UNOFFICIAL 2026-10-28 HidePipBoyLight also takes its brightness and reach while the flashlight is on.
		NiNode* playerNode = Player->GetNode();
		for (UINT32 i = 0; i < playerNode->m_children.capacity; i++) {
			NiAVObject* child = playerNode->m_children.data[i];
			if (!child || child->GetRTTI() != (void*)0x11F4A98) continue;
			child->m_flags |= child->APP_CULLED;
			// UNOFFICIAL 2026-10-28 HidePipBoyLight (Doctor Abyss: shadows vanish after a short delay indoors): culling hid the light's node,
			// but its brightness and reach stayed, so the game could still hand it to the objects round the player as one of their few lamps.
			// Now it also gets no brightness and no reach while the flashlight is on (put back when it is off).
			if (Settings.hidePipBoyLight) {
				NiPointLight* pipLight = (NiPointLight*)child;
				if (hiddenPipBoyLight != pipLight || pipLight->Dimmer != 0.0f || pipLight->Spec.r != 0.0f) {
					if (pipLight->Dimmer != 0.0f || pipLight->Spec.r != 0.0f) { hiddenPipBoyDimmer = pipLight->Dimmer; hiddenPipBoySpec = pipLight->Spec; }
					static bool told = false;
					if (!told) { told = true; Logger::Log("UNOFFICIAL HidePipBoyLight: the Pip-Boy light (dimmer %.2f, reach %.0f) gets no light while the flashlight is on", hiddenPipBoyDimmer, hiddenPipBoySpec.r); }
					hiddenPipBoyLight = pipLight;
					pipLight->Dimmer = 0.0f;
					pipLight->Spec = NiColor(0, 0, 0);
				}
			}
		}
	}

	NiSpotLight& light = *SpotLight;
	light.CastShadows = Settings.renderShadows;
	light.Diff = Settings.Color;
	light.Dimmer = Settings.Dimmer * 10.0;
	light.m_worldTransform.pos = weaponPos;
	light.m_worldTransform.rot = weaponRot;
	light.m_worldTransform.scale = 1.0f;
	light.OuterSpotAngle = Settings.ConeAngle;
	light.Spec = NiColor(Settings.Distance, 0, 0);	// radius in the r channel

	// Packed as the shared TESR_SpotLight* constants are: w carries the radius, the cone angle and the dimmer.
	Constants.Position = D3DXVECTOR4(weaponPos.x, weaponPos.y, weaponPos.z, Settings.Distance);
	Constants.Direction = D3DXVECTOR4(weaponRot.data[0][0], weaponRot.data[1][0], weaponRot.data[2][0], Settings.ConeAngle);
	Constants.Color = D3DXVECTOR4(Settings.Color.r, Settings.Color.g, Settings.Color.b, Settings.Dimmer);

	GetFlashlightViewProj();
	PublishLightConstants(true);
};

// The shared TESR_SpotLight* constants are published here rather than by the caller,
// because UpdateConstants runs more than once per frame - at frame start, again from
// GetNearbyLights during the shadow pass, and again after the world render. Each call
// re-reads the weapon bone. Publishing from the caller left the cone constants holding an
// earlier read than TESR_FlashLightViewProjTransform, so the projected cookie tracked a
// different snapshot than the cone it is projected into and slid against the beam as the
// weapon moved. Writing both from the same read removes that by construction.
// The shared TESR_SpotLight* constants are published here rather than by the caller, because UpdateConstants runs more than once per
// frame (at frame start, again from GetNearbyLights in the shadow pass, and after the world render). Each call re-reads the weapon
// bone. Publishing from the caller left the cone constants holding an earlier read than TESR_FlashLightViewProjTransform, so the
// cookie slid against the beam as the weapon moved. Writing both from the same read removes that by construction.
void FlashlightEffect::PublishLightConstants(bool abActive) {
	if (!abActive) {
		const D3DXVECTOR4 zero(0.0f, 0.0f, 0.0f, 0.0f);
		TheShaderManager->SpotLightPosition[0] = zero;
		TheShaderManager->SpotLightDirection[0] = zero;
		TheShaderManager->SpotLightColor[0] = zero;
		TheShaderManager->VolumetricData = zero;
		return;
	}

	D3DXVECTOR4& position = TheShaderManager->SpotLightPosition[0];
	position = SpotLight->m_worldTransform.pos.toD3DXVEC4();
	position.w = SpotLight->Spec.r;	// radius
	TheShaderManager->SpotLightDirection[0] = D3DXVECTOR4(
		SpotLight->m_worldTransform.rot.data[0][0],
		SpotLight->m_worldTransform.rot.data[1][0],
		SpotLight->m_worldTransform.rot.data[2][0],
		SpotLight->OuterSpotAngle);	// outside angle of the cone
	TheShaderManager->SpotLightColor[0] = D3DXVECTOR4(SpotLight->Diff.r, SpotLight->Diff.g, SpotLight->Diff.b, SpotLight->Dimmer);

	// The beam march needs parallax to draw a visible shaft. In first person the light sits on the camera, so every view ray runs
	// down the beam and there is nothing to see: the march takes a virtual origin pushed right and down in camera space, while the
	// surface pool and the cookie keep the true position. w is the first-person strength floor, which also blends the cookie flat in
	// the march, so the parallax mismatch between the two positions does not show.
	FlashlightBeamEffect* beam = TheShaderManager->Effects.FlashlightBeam;
	TheShaderManager->VolumetricData = position;
	TheShaderManager->VolumetricData.w = 0.0f;
	if (beam && Player && !Player->isThirdPerson && WorldSceneGraph && WorldSceneGraph->camera) {
		NiMatrix33& cameraRot = WorldSceneGraph->camera->m_worldTransform.rot;
		const float right = beam->Settings.FirstPersonOffsetRight;
		const float down = beam->Settings.FirstPersonOffsetDown;
		TheShaderManager->VolumetricData.x += cameraRot.data[0][2] * right - cameraRot.data[0][1] * down;
		TheShaderManager->VolumetricData.y += cameraRot.data[1][2] * right - cameraRot.data[1][1] * down;
		TheShaderManager->VolumetricData.z += cameraRot.data[2][2] * right - cameraRot.data[2][1] * down;
		TheShaderManager->VolumetricData.w = beam->Settings.FirstPersonStrength;
	}
}

bool FlashlightEffect::ShouldRender() { return SpotLight && spotLightActive; }


void FlashlightEffect::GetFlashlightViewProj() {
	if (!SpotLight) return;

	// A perspective from the light, looking along its direction, out to its radius.
	const D3DXVECTOR3 up(0.0f, 0.0f, 1.0f);
	const D3DXVECTOR3 eye = SpotLight->m_worldTransform.pos.toD3DXVEC3();
	const D3DXVECTOR3 forward(SpotLight->m_worldTransform.rot.data[0][0], SpotLight->m_worldTransform.rot.data[1][0], SpotLight->m_worldTransform.rot.data[2][0]);
	const D3DXVECTOR3 target = eye + forward;

	const float radius = SpotLight->Spec.r;
	D3DXMATRIX view, projection;
	D3DXMatrixPerspectiveFovRH(&projection, D3DXToRadian(SpotLight->OuterSpotAngle * 2), 1.0f, 0.1f, radius);
	D3DXMatrixLookAtRH(&view, &eye, &target, &up);

	Constants.FlashlightViewProj = view * projection;
}

// --- The Vulkan passes (NVR DXVK) -----------------------------------------------------------------------------------
// UNOFFICIAL 2026-10-25 (NVR DXVK): the four techniques as Compute\Flashlight*.frag (shared part Flashlight.glsl), line for line.
// The passes go through the frame chain in the technique's order, each reading the image the one before left (TESR_RenderedBuffer);
// TESR_SourceBuffer is the image before the effect, copied once by the first pass as Render does. Without shadows the technique's
// first pass (NoShadow) only fills the image with 1 for the light pass to read back: it is not drawn, the light pass takes that 1
// itself (exact in any target format). Render clears every pass's target (ClearRenderTarget): every pass here writes every pixel,
// and the two clip()s (DepthBlur's, BoxBlur's) write the clear colour instead. The constants are the registered values SetCT
// uploads, in one std140 block (the D3DX matrices as stored: mul(v, M) is M * v, mul(M, v) is v * M). Samplers as the .fx declares
// them: LINEAR, CLAMP; the cookie with its LINEAR mips (MipSampler, extension 1.3); the depth buffer's MINFILTER ANISOTROPIC runs at
// the effect samplers' MAXANISOTROPY 1 on a texture without mips, which is LINEAR.
namespace {
	// The Constants block of Flashlight.glsl (std140).
	struct FlashlightUniforms {
		D3DXMATRIX	flashLightViewProj;		// TESR_FlashLightViewProjTransform
		D3DXMATRIX	viewTransform;			// TESR_ViewTransform
		D3DXMATRIX	projectionTransform;	// TESR_ProjectionTransform
		D3DXVECTOR4	cameraPosition;			// TESR_CameraPosition
		D3DXVECTOR4	cameraData;				// TESR_CameraData
		D3DXVECTOR4	spotLightPosition;		// TESR_SpotLightPosition
		D3DXVECTOR4	spotLightColor;			// TESR_SpotLightColor
		D3DXVECTOR4	spotLightDirection;		// TESR_SpotLightDirection
		D3DXVECTOR4	sunColor;				// TESR_SunColor
		D3DXVECTOR4	reciprocalResolution;	// TESR_ReciprocalResolution
		D3DXVECTOR4	flashLightTuning;		// TESR_FlashLightTuning
		D3DXVECTOR4	flashLightComposite;	// TESR_FlashLightComposite
		D3DXVECTOR4	volumetricControl;		// TESR_VolumetricControl
	};
	static_assert(sizeof(FlashlightUniforms) == 352, "Flashlight.glsl declares a 352-byte block");

	struct FlashlightPush {
		D3DXVECTOR2	offsetMask;		// 0: DepthBlur's OffsetMaskH / OffsetMaskV
		UInt32		mode;			// 8: see Flashlight.glsl
		UInt32		linearSampler;	// 12: LINEAR, CLAMP (filled in by the binding)
		UInt32		cookieSampler;	// 16: LINEAR with mips, CLAMP (filled in by the binding)
		UInt32		pad[3];
	};

	// Set 1: the textures in the .fx's sampler order (TESR_PointShadowBuffer, which no pass reads, left out); set 2: the constants.
	const NVR_EXT_BINDING_DESC FlashlightBindings[] = {
		{ 1, 0, NVR_EXT_BIND_SAMPLED_IMAGE,  0, NVR_EXT_VIEW_2D, FALSE, 0 },	// t_source: TESR_SourceBuffer
		{ 1, 1, NVR_EXT_BIND_SAMPLED_IMAGE,  1, NVR_EXT_VIEW_2D, FALSE, 0 },	// t_rendered: TESR_RenderedBuffer
		{ 1, 2, NVR_EXT_BIND_SAMPLED_IMAGE,  2, NVR_EXT_VIEW_2D, FALSE, 0 },	// t_depth: TESR_DepthBuffer
		{ 1, 3, NVR_EXT_BIND_SAMPLED_IMAGE,  3, NVR_EXT_VIEW_2D, FALSE, 0 },	// t_normals: TESR_NormalsBuffer
		{ 1, 4, NVR_EXT_BIND_SAMPLED_IMAGE,  4, NVR_EXT_VIEW_2D, FALSE, 0 },	// t_shadow: TESR_ShadowSpotlightBuffer0
		{ 1, 5, NVR_EXT_BIND_SAMPLED_IMAGE,  5, NVR_EXT_VIEW_2D, FALSE, 0 },	// t_cookie: TESR_SpotLightTexture
		{ 1, 6, NVR_EXT_BIND_SAMPLED_IMAGE,  6, NVR_EXT_VIEW_2D, FALSE, 0 },	// t_volumetric: TESR_VolumetricBuffer
		{ 2, 0, NVR_EXT_BIND_UNIFORM_BUFFER, 0, NVR_EXT_VIEW_2D, FALSE, 0 },	// Constants
		{ 0, 0, NVR_EXT_BIND_SAMPLER,        0, NVR_EXT_VIEW_2D, FALSE, 12 },	// LINEAR, CLAMP
		{ 0, 0, NVR_EXT_BIND_SAMPLER,        1, NVR_EXT_VIEW_2D, FALSE, 16 },	// LINEAR with mips, CLAMP
	};
	const UINT FlashlightTextureCount = 7;

	IDirect3DBaseTexture9* FlashlightSlotTexture(const char* name) {
		std::string slotName = name;
		IDirect3DBaseTexture9** slot = TheTextureManager->GetTextureSlotByName(slotName);
		return slot ? *slot : nullptr;
	}
	// The constants from the same registered memory the effect's SetCT uploads.
	const D3DXVECTOR4* FlashlightRegistered(const char* name) {
		auto found = TheShaderManager->ConstantsTable.find(name);
		return found == TheShaderManager->ConstantsTable.end() ? nullptr : found->second;
	}
}

// UNOFFICIAL 2026-10-30 RT lamp shadows Flashlight: with a traced shadow this frame the shadowed technique runs (its NVR DXVK passes read
// the trace, ExtRender), RenderShadows on or not; otherwise the technique UpdateConstants chose.
int FlashlightEffect::TechniqueThisFrame() {
	if (!RtLampShadows::FlashlightShadow()) return selectedPass;
	if (Settings.edgeFix > 0 && edgeFixShadowsIndex >= 0) return edgeFixShadowsIndex;
	return selectedPass == 0 ? 1 : selectedPass;
}

bool FlashlightEffect::ExtRender(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, IDirect3DSurface9* RenderedSurface, UINT techniqueIndex, bool ClearRenderTarget, IDirect3DSurface9* SourceBuffer) {
	// 0 / 1: without / with shadows; the EdgeFix techniques run the same passes with Combine(true).
	const bool edgeFixShadows = edgeFixShadowsIndex >= 0 && techniqueIndex == (UINT)edgeFixShadowsIndex;
	const bool edgeFix = edgeFixShadows || (edgeFixIndex >= 0 && techniqueIndex == (UINT)edgeFixIndex);
	const bool shadows = techniqueIndex == 1 || edgeFixShadows;
	if ((techniqueIndex != 0 && !shadows && !edgeFix) || !ClearRenderTarget) return false;
	if (!SourceBuffer || SourceBuffer == RenderTarget || SourceBuffer != TheTextureManager->SourceSurface || !TheTextureManager->SourceTexture ||
		!TheTextureManager->RenderedTexture) return false;

	const D3DXVECTOR4* lightViewProj = FlashlightRegistered("TESR_FlashLightViewProjTransform");
	const D3DXVECTOR4* view = FlashlightRegistered("TESR_ViewTransform");
	const D3DXVECTOR4* projection = FlashlightRegistered("TESR_ProjectionTransform");
	const D3DXVECTOR4* cameraPosition = FlashlightRegistered("TESR_CameraPosition");
	const D3DXVECTOR4* cameraData = FlashlightRegistered("TESR_CameraData");
	const D3DXVECTOR4* spotLightPosition = FlashlightRegistered("TESR_SpotLightPosition");
	const D3DXVECTOR4* spotLightColor = FlashlightRegistered("TESR_SpotLightColor");
	const D3DXVECTOR4* spotLightDirection = FlashlightRegistered("TESR_SpotLightDirection");
	const D3DXVECTOR4* sunColor = FlashlightRegistered("TESR_SunColor");
	const D3DXVECTOR4* reciprocalResolution = FlashlightRegistered("TESR_ReciprocalResolution");
	const D3DXVECTOR4* tuning = FlashlightRegistered("TESR_FlashLightTuning");
	const D3DXVECTOR4* composite = FlashlightRegistered("TESR_FlashLightComposite");
	const D3DXVECTOR4* volumetricControl = FlashlightRegistered("TESR_VolumetricControl");
	if (!lightViewProj || !view || !projection || !cameraPosition || !cameraData || !spotLightPosition || !spotLightColor || !spotLightDirection ||
		!sunColor || !reciprocalResolution || !tuning || !composite || !volumetricControl) return false;

	// The textures as SetCT binds them. The shadow map is read only with shadows, the beam's buffer only with the beam on (the
	// Combine gate): otherwise they take a placeholder (the depth buffer), which is never read.
	IDirect3DBaseTexture9* depth = FlashlightSlotTexture("TESR_DepthBuffer");
	IDirect3DBaseTexture9* normals = FlashlightSlotTexture("TESR_NormalsBuffer");
	IDirect3DBaseTexture9* shadowMap = FlashlightSlotTexture("TESR_ShadowSpotlightBuffer0");
	IDirect3DBaseTexture9* cookie = ExtFileTexture("TESR_SpotLightTexture");
	IDirect3DBaseTexture9* volumetric = FlashlightSlotTexture("TESR_VolumetricBuffer");
	const bool beamOn = volumetricControl->x > 0.0f;
	// UNOFFICIAL 2026-10-30 RT lamp shadows Flashlight: the traced shadow (screen space) in the shadow map's place, read by FlashlightShadowsRT
	IDirect3DBaseTexture9* traced = shadows ? RtLampShadows::FlashlightShadow() : nullptr;
	if (traced) shadowMap = traced;
	if (!depth || !normals || !cookie || (shadows && !shadowMap) || (beamOn && !volumetric)) return false;

	// Every pipeline this technique needs, and the samplers, before anything is drawn: a refusal leaves the frame as it was.
	auto make = [](ExtPass& pass, const char* spvName) {
		if (pass.Tried) return;
		pass.Tried = true;
		pass.Pipeline = DxvkCompute::CreateGraphicsPipeline(spvName, FlashlightBindings, _countof(FlashlightBindings), sizeof(FlashlightPush), true);
	};
	if (shadows) {
		make(traced ? extShadowsRt : extShadows, traced ? "FlashlightShadowsRT" : "FlashlightShadows");
		make(extDepthBlur, "FlashlightDepthBlur");
		if (!(traced ? extShadowsRt : extShadows).Pipeline || !extDepthBlur.Pipeline) return false;
	}
	make(extLight, "FlashlightLight");
	make(extBoxBlur, "FlashlightBoxBlur");
	make(extCombine, "FlashlightCombine");
	ID3D9NvrExtSampler* linear = DxvkCompute::LinearClamp();
	ID3D9NvrExtSampler* mip = DxvkCompute::MipSampler(false, NVR_EXT_ADDRESS_CLAMP);
	if (!extLight.Pipeline || !extBoxBlur.Pipeline || !extCombine.Pipeline || !linear || !mip) return false;

	FlashlightUniforms uniforms = {};
	memcpy(&uniforms.flashLightViewProj, lightViewProj, sizeof(D3DXMATRIX));
	memcpy(&uniforms.viewTransform, view, sizeof(D3DXMATRIX));
	memcpy(&uniforms.projectionTransform, projection, sizeof(D3DXMATRIX));
	uniforms.cameraPosition = *cameraPosition;
	uniforms.cameraData = *cameraData;
	uniforms.spotLightPosition = *spotLightPosition;
	uniforms.spotLightColor = *spotLightColor;
	uniforms.spotLightDirection = *spotLightDirection;
	uniforms.sunColor = *sunColor;
	uniforms.reciprocalResolution = *reciprocalResolution;
	uniforms.flashLightTuning = *tuning;
	uniforms.flashLightComposite = *composite;
	uniforms.volumetricControl = *volumetricControl;
	ID3D9NvrExtBuffer* uniformBuffer = ExtUniforms(extUniforms, &uniforms, sizeof(uniforms));
	if (!uniformBuffer) return false;

	IDirect3DBaseTexture9* textures[FlashlightTextureCount] = { TheTextureManager->SourceTexture, nullptr, depth, normals,
		shadows ? shadowMap : depth, cookie, volumetric ? volumetric : depth };
	bool first = true;
	// One pass through the chain. The first one copies the image into SourceBuffer and gets the surface Render was given; the later
	// ones read the image the pass before committed, with the surface the chain publishes then.
	auto draw = [&](ExtPass& pass, UInt32 mode, const D3DXVECTOR2& offsetMask, bool last) {
		textures[1] = TheTextureManager->RenderedTexture;
		NVR_EXT_RESOURCE_BINDING resources[FlashlightTextureCount + 3] = {};
		for (UINT i = 0; i < FlashlightTextureCount; i++) {
			resources[i].Slot = i; resources[i].Type = NVR_EXT_BIND_SAMPLED_IMAGE; resources[i].pResource = textures[i];
		}
		resources[FlashlightTextureCount].Slot = 0; resources[FlashlightTextureCount].Type = NVR_EXT_BIND_UNIFORM_BUFFER; resources[FlashlightTextureCount].pResource = uniformBuffer;
		resources[FlashlightTextureCount + 1].Slot = 0; resources[FlashlightTextureCount + 1].Type = NVR_EXT_BIND_SAMPLER; resources[FlashlightTextureCount + 1].pResource = linear;
		resources[FlashlightTextureCount + 2].Slot = 1; resources[FlashlightTextureCount + 2].Type = NVR_EXT_BIND_SAMPLER; resources[FlashlightTextureCount + 2].pResource = mip;
		FlashlightPush push = {};
		push.offsetMask = offsetMask;
		push.mode = mode;
		const bool drawn = ExtFullScreen(Device, RenderTarget, first ? RenderedSurface : TheTextureManager->RenderedSurface, first ? SourceBuffer : nullptr,
			pass.Pipeline, resources, _countof(resources), &push, sizeof(push), last);
		if (!drawn && !first) {
			// A later pass failed (ExtFailed is set): the image is one of the effect's intermediate buffers now. The scene goes back
			// from SourceBuffer, so the D3D9 technique that runs next starts from the image it would have had.
			FrameChain& chain = TheShaderManager->Chain;
			if (chain.Owns(RenderTarget, TheTextureManager->RenderedSurface)) {
				Device->StretchRect(SourceBuffer, NULL, chain.Output(), NULL, D3DTEXF_NONE);
				chain.Commit();
			}
			else
				Device->StretchRect(SourceBuffer, NULL, RenderTarget, NULL, D3DTEXF_NONE);
		}
		first = false;
		return drawn;
	};

	const D3DXVECTOR2 none(0.0f, 0.0f);
	if (shadows) {
		if (!draw(traced ? extShadowsRt : extShadows, 0, none, false)) return false;			// Shadows (traced: RT lamp shadows Flashlight)
		if (!draw(extDepthBlur, 0, D3DXVECTOR2(1.0f, 0.0f), false)) return false;				// DepthBlur, OffsetMaskH
		if (!draw(extDepthBlur, 0, D3DXVECTOR2(0.0f, 1.0f), false)) return false;				// DepthBlur, OffsetMaskV
	}
	if (!draw(extLight, shadows ? 0 : 1, none, false)) return false;							// Flashlight (NoShadow's 1 without shadows)
	if (!draw(extBoxBlur, 0, none, false)) return false;										// BoxBlurMin
	if (!draw(extBoxBlur, 1, none, false)) return false;										// BoxBlurAvg
	return draw(extCombine, edgeFix ? 1 : 0, none, true);										// Combine(edgeFix)
}