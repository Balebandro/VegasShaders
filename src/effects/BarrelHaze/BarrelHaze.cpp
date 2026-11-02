#include "BarrelHaze.h"
#include "../../core/DxvkCompute.h"	// UNOFFICIAL 2026-10-25 (NVR DXVK)

// UNOFFICIAL 2026-10-25 (NVR DXVK): BarrelHaze.frag; the frame (LINEAR, CLAMP), the depth (POINT, CLAMP) and the haze values the
// technique reads. Two samplers, so this is ExtFullScreen with its own bindings (ExtTexturePass takes one sampler).
bool BarrelHazeEffect::ExtRender(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, IDirect3DSurface9* RenderedSurface, UINT techniqueIndex, bool ClearRenderTarget, IDirect3DSurface9* SourceBuffer) {
	if (techniqueIndex != 0) return false;
	// The TESR_ float4s in the order the shader reads them, then the two sampler heap indices (LINEAR at 96, POINT at 100).
	struct Push { D3DXVECTOR4 reciprocalResolution, line, data, animation, blast, cameraData; UInt32 linearIndex, pointIndex; UInt32 pad[2]; } push = {};
	push.reciprocalResolution = TheShaderManager->ShaderConst.ReciprocalResolution;
	push.line = BarrelHaze::Line;
	push.data = BarrelHaze::Data;
	push.animation = BarrelHaze::Animation;
	push.blast = BarrelHaze::Blast;
	push.cameraData = TheRenderManager->CameraData;
	IDirect3DBaseTexture9* textures[] = { TheTextureManager->RenderedTexture, TheShaderManager->Effects.CombineDepth ? TheShaderManager->Effects.CombineDepth->Textures.CombinedDepthTexture : nullptr };
	if (!textures[0] || !textures[1]) return false;
	if (!extPass.Tried) {
		extPass.Tried = true;
		NVR_EXT_BINDING_DESC bindings[4] = {};
		for (UINT i = 0; i < 2; i++) {
			bindings[i].Set = 1; bindings[i].Binding = i; bindings[i].Type = NVR_EXT_BIND_SAMPLED_IMAGE; bindings[i].Slot = i; bindings[i].ViewType = NVR_EXT_VIEW_2D;
		}
		bindings[2].Type = NVR_EXT_BIND_SAMPLER; bindings[2].Slot = 0; bindings[2].PushOffset = 96;
		bindings[3].Type = NVR_EXT_BIND_SAMPLER; bindings[3].Slot = 1; bindings[3].PushOffset = 100;
		extPass.Pipeline = DxvkCompute::CreateGraphicsPipeline("BarrelHaze", bindings, 4, sizeof(push), true);
	}
	ID3D9NvrExtSampler* linear = DxvkCompute::LinearClamp();
	ID3D9NvrExtSampler* point = DxvkCompute::PointClamp();
	if (!extPass.Pipeline || !linear || !point) return false;
	NVR_EXT_RESOURCE_BINDING resources[4] = {};
	for (UINT i = 0; i < 2; i++) { resources[i].Slot = i; resources[i].Type = NVR_EXT_BIND_SAMPLED_IMAGE; resources[i].pResource = textures[i]; }
	resources[2].Slot = 0; resources[2].Type = NVR_EXT_BIND_SAMPLER; resources[2].pResource = linear;
	resources[3].Slot = 1; resources[3].Type = NVR_EXT_BIND_SAMPLER; resources[3].pResource = point;
	return ExtFullScreen(Device, RenderTarget, RenderedSurface, SourceBuffer, extPass.Pipeline, resources, 4, &push, sizeof(push));
}
