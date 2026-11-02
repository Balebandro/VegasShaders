#include "BloomLegacy.h"
#include "../../core/DxvkCompute.h"	// UNOFFICIAL 2026-10-23 (NVR DXVK)

void BloomLegacyEffect::UpdateConstants() {
	// exteriors use the Main group, every interior the Interiors group
	const ValuesStruct& v = TheShaderManager->GameState.isExterior ? Settings.Main : Settings.Interiors;

	Constants.Data = D3DXVECTOR4(v.Luminance, v.MiddleGray, v.WhiteCutOff, Constants.Data.w);
	Constants.Values = D3DXVECTOR4(v.BloomIntensity, v.OriginalIntensity, v.BloomSaturation, v.OriginalSaturation);
}

// Reads one bloom group from its settings section.
static void ReadBloomGroup(const char* section, BloomLegacyEffect::ValuesStruct& out) {
	SettingManager* cfg = TheSettingManager;
	out.Luminance = cfg->GetSettingF(section, "Luminance");
	out.MiddleGray = cfg->GetSettingF(section, "MiddleGray");
	out.WhiteCutOff = cfg->GetSettingF(section, "WhiteCutOff");
	out.BloomIntensity = cfg->GetSettingF(section, "BloomIntensity");
	out.OriginalIntensity = cfg->GetSettingF(section, "OriginalIntensity");
	out.BloomSaturation = cfg->GetSettingF(section, "BloomSaturation");
	out.OriginalSaturation = cfg->GetSettingF(section, "OriginalSaturation");
}

void BloomLegacyEffect::UpdateSettings() {
	ReadBloomGroup("Shaders.BloomLegacy.Exteriors", Settings.Main);
	ReadBloomGroup("Shaders.BloomLegacy.Interiors", Settings.Interiors);
}

// UNOFFICIAL 2026-10-25 (NVR DXVK): the four passes as Vulkan passes, through the frame chain as Render does it: BloomPass
// into the chain, BlurPass horizontal and vertical, CombinePass last (it may write the game target directly). Every pass reads
// the current image (TESR_RenderedBuffer); TESR_SourceBuffer is the image before the effect, copied once by the first pass.
// The pipelines are made before anything is drawn, so a refused one leaves the frame as it was (the D3D9 technique runs).
bool BloomLegacyEffect::ExtRender(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, IDirect3DSurface9* RenderedSurface, UINT techniqueIndex, bool ClearRenderTarget, IDirect3DSurface9* SourceBuffer) {
	if (techniqueIndex != 0) return false;
	if (!SourceBuffer || SourceBuffer == RenderTarget || SourceBuffer != TheTextureManager->SourceSurface || !TheTextureManager->SourceTexture) return false;

	struct BloomPush { D3DXVECTOR4 bloomData; UInt32 samplerIndex; UInt32 pad[3]; };
	struct BlurPush { D3DXVECTOR4 reciprocalResolution; D3DXVECTOR2 offsetMask; UInt32 samplerIndex; UInt32 pad; };
	struct CombinePush { D3DXVECTOR4 bloomValues; UInt32 samplerIndex; UInt32 pad[3]; };

	// Textures bind in order (set 1, binding i, slot i); the LINEAR/CLAMP sampler (slot 0) takes its heap index from the push block.
	auto make = [](ExtPass& pass, const char* spvName, UINT textureCount, UINT samplerOffset, UINT pushSize) {
		if (pass.Tried) return;
		pass.Tried = true;
		NVR_EXT_BINDING_DESC bindings[3] = {};
		for (UINT i = 0; i < textureCount; i++) {
			bindings[i].Set = 1; bindings[i].Binding = i; bindings[i].Type = NVR_EXT_BIND_SAMPLED_IMAGE; bindings[i].Slot = i; bindings[i].ViewType = NVR_EXT_VIEW_2D;
		}
		bindings[textureCount].Type = NVR_EXT_BIND_SAMPLER; bindings[textureCount].Slot = 0; bindings[textureCount].PushOffset = samplerOffset;
		pass.Pipeline = DxvkCompute::CreateGraphicsPipeline(spvName, bindings, textureCount + 1, pushSize, true);
	};
	make(extBloom, "BloomLegacyBloom", 1, 16, sizeof(BloomPush));
	make(extBlur, "BloomLegacyBlur", 1, 24, sizeof(BlurPush));
	make(extCombine, "BloomLegacyCombine", 2, 16, sizeof(CombinePush));
	ID3D9NvrExtSampler* sampler = DxvkCompute::LinearClamp();
	if (!extBloom.Pipeline || !extBlur.Pipeline || !extCombine.Pipeline || !sampler) return false;

	// One full-screen pass through the chain. Only the first pass gets SourceBuffer: ExtFullScreen copies the image into it before drawing.
	auto draw = [&](ExtPass& pass, IDirect3DBaseTexture9* const* textures, UINT textureCount, IDirect3DSurface9* source, const void* push, UINT pushSize, bool lastPass) {
		NVR_EXT_RESOURCE_BINDING resources[3] = {};
		for (UINT i = 0; i < textureCount; i++) { resources[i].Slot = i; resources[i].Type = NVR_EXT_BIND_SAMPLED_IMAGE; resources[i].pResource = textures[i]; }
		resources[textureCount].Slot = 0; resources[textureCount].Type = NVR_EXT_BIND_SAMPLER; resources[textureCount].pResource = sampler;
		return ExtFullScreen(Device, RenderTarget, TheTextureManager->RenderedSurface, source, pass.Pipeline, resources, textureCount + 1, push, pushSize, lastPass);
	};

	BloomPush bloom = {};
	bloom.bloomData = Constants.Data;
	BlurPush blur = {};
	blur.reciprocalResolution = TheShaderManager->ShaderConst.ReciprocalResolution;
	CombinePush combine = {};
	combine.bloomValues = Constants.Values;

	// BloomPass
	IDirect3DBaseTexture9* image = TheTextureManager->RenderedTexture;
	if (!draw(extBloom, &image, 1, SourceBuffer, &bloom, sizeof(bloom), false)) return false;

	// BlurPass: OffsetMaskH, then OffsetMaskV
	const D3DXVECTOR2 offsetMasks[2] = { D3DXVECTOR2(1.0f, 0.0f), D3DXVECTOR2(0.0f, 1.0f) };
	for (UINT i = 0; i < 2; i++) {
		blur.offsetMask = offsetMasks[i];
		image = TheTextureManager->RenderedTexture;
		if (!draw(extBlur, &image, 1, nullptr, &blur, sizeof(blur), false)) return false;
	}

	// CombinePass: the blurred bloom and the image before the effect
	IDirect3DBaseTexture9* combineTextures[2] = { TheTextureManager->RenderedTexture, TheTextureManager->SourceTexture };
	return draw(extCombine, combineTextures, 2, nullptr, &combine, sizeof(combine), true);
}

void BloomLegacyEffect::RegisterConstants() {
	ShaderManager* sm = TheShaderManager;
	sm->RegisterConstant("TESR_BloomLegacyData", &Constants.Data);
	sm->RegisterConstant("TESR_BloomLegacyValues", &Constants.Values);
}