#include "SkyView.h"
#include "../../core/DxvkCompute.h"	// UNOFFICIAL 2026-10-25 (NVR DXVK)

void SkyViewEffect::RegisterTextures() {
	TheTextureManager->InitTexture("TESR_SkyViewLUT", &Textures.TableTexture, &Textures.TableSurface, 128, 64, D3DFMT_A16B16G16R16F);
	// UNOFFICIAL 2026-10-18 ExactSunlight / MultipleScattering: the static tables (SkyView.fx.hlsl techniques 1 and 2). Declared in the effect as
	// TESR_SkyTransLUT then TESR_SkyMSLUT: samplers bind by declaration order.
	TheTextureManager->InitTexture("TESR_SkyTransLUT", &Textures.TransTexture, &Textures.TransSurface, 256, 64, D3DFMT_A16B16G16R16F);
	TheTextureManager->InitTexture("TESR_SkyMSLUT", &Textures.MSTexture, &Textures.MSSurface, 32, 32, D3DFMT_A16B16G16R16F);
}

bool SkyViewEffect::ShouldRender() {
	SkyShaders* sky = TheShaderManager->Shaders.Sky;
	return Effect && Textures.TableSurface && sky && sky->PhysicalSettings.Enabled && sky->Constants.PhysSky.x > 0.0f &&
		TheShaderManager->GameState.isExterior;
}

// Into the table, then back to the frame's target.
void SkyViewEffect::RenderTable(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget) {
	SkyShaders* sky = TheShaderManager->Shaders.Sky;
	const bool ms = sky->PhysicalSettings.MultipleScattering && Textures.TransSurface && Textures.MSSurface;
	const bool exact = ms || (sky->PhysicalSettings.ExactSunlight && Textures.TransSurface);
	UINT technique = 0;		// 0: the closed-form sky, exactly as before
	if (exact) {
		const float haze = sky->Constants.PhysSky.w, albedo = sky->PhysicalSettings.GroundAlbedo;
		const bool changed = !transBuilt || haze != builtHaze || (ms && (!msBuilt || albedo != builtAlbedo));
		// fixes-a #48: no 256-frame refresh any more (it cost ~0.45 ms on that frame with MS on and bought nothing: NVR's targets are
		// D3DPOOL_DEFAULT render targets that are never released, so their contents cannot be lost in a running session; if a Reset
		// path is ever added it must clear transBuilt and msBuilt).
		if (changed) {
			static GpuTimer buildTimer("Physical sky tables (static)", 1);
			const bool firstDraw = !transBuilt || (ms && !msBuilt);	// fixes-a #50: log only when a table is first made, not on every drag step
			GpuProfileScope gpu(buildTimer, Device);
			TheShaderManager->RenderEffectToRT(Textures.TransSurface, this, false, 1);
			transBuilt = true;
			if (ms) {
				TheShaderManager->RenderEffectToRT(Textures.MSSurface, this, false, 2);
				msBuilt = true;
				builtAlbedo = albedo;
			}
			// fixes-a #49: with MS off a Haze change redraws only the transmittance table; the MS table (drawn for the old haze)
			// must be drawn again when MS is next turned on
			if (!ms && haze != builtHaze) msBuilt = false;
			builtHaze = haze;
			if (firstDraw) Logger::Log("UNOFFICIAL physical sky: drew the %s table(s) (haze %.2f, ground albedo %.2f)", ms ? "sun transmittance and multiple scattering" : "sun transmittance", haze, albedo);
		}
		technique = ms ? 4 : 3;
	}
	TheShaderManager->RenderEffectToRT(Textures.TableSurface, this, false, technique);
	Device->SetRenderTarget(0, RenderTarget);
}

// UNOFFICIAL 2026-10-25 (NVR DXVK): the push block of the five SkyView passes (src\hlsl\NewVegas\Compute\SkyView.frag and the
// others): TESR_PhysSky and TESR_PhysSkySun, then the LINEAR/CLAMP sampler's heap index (written by the extension at offset 32), then the D3D9 uv shift.
struct SkyViewPush {
	D3DXVECTOR4	physSky;
	D3DXVECTOR4	physSkySun;
	UInt32		samplerIndex;
	UInt32		pad;
	float		uvShift[2];	// 40: 0.5 / screen - 0.5 / table: the D3D9 quad (ShaderManager::CreateFrameVertex) offsets uv by half a SCREEN
							// pixel, so a D3D9 pass into the 128x64 table reads it 0.47 texel off centre; the Vulkan pass must too (same picture)
};

// UNOFFICIAL 2026-10-25 (NVR DXVK): the five techniques as Vulkan passes, SkyView.fx.hlsl line for line (Compute\SkyView, SkyTrans,
// SkyMS, SkyViewX, SkyViewXMS). Render gives this the surface a technique draws into (the sky table, or the transmittance or
// multiple scattering table), so each pass is one ExtDrawTo into it; the frame chain is not touched. The constants are the ones the
// D3D9 pass uploads (Sky.cpp sets them before the tables are drawn). Techniques 2 and 3 read the transmittance table, 4 both.
bool SkyViewEffect::ExtRender(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, IDirect3DSurface9* RenderedSurface, UINT techniqueIndex, bool ClearRenderTarget, IDirect3DSurface9* SourceBuffer) {
	if (techniqueIndex > 4 || ClearRenderTarget || !RenderTarget) return false;
	SkyShaders* sky = TheShaderManager->Shaders.Sky;
	if (!sky) return false;

	static const char* const names[5] = { "SkyView", "SkyTrans", "SkyMS", "SkyViewX", "SkyViewXMS" };
	const UINT textures = techniqueIndex == 4 ? 2 : techniqueIndex >= 2 ? 1 : 0;
	const UINT resourceCount = textures ? textures + 1 : 0;	// the sampler after the textures, none for techniques 0 and 1
	ExtPass& pass = extPass[techniqueIndex];
	if (!pass.Tried) {
		pass.Tried = true;
		NVR_EXT_BINDING_DESC bindings[3] = {};
		for (UINT i = 0; i < textures; i++) {
			bindings[i].Set = 1; bindings[i].Binding = i; bindings[i].Type = NVR_EXT_BIND_SAMPLED_IMAGE; bindings[i].Slot = i; bindings[i].ViewType = NVR_EXT_VIEW_2D;
		}
		if (textures) { bindings[textures].Type = NVR_EXT_BIND_SAMPLER; bindings[textures].Slot = 0; bindings[textures].PushOffset = 32; }
		pass.Pipeline = DxvkCompute::CreateGraphicsPipeline(names[techniqueIndex], bindings, resourceCount, sizeof(SkyViewPush), textures != 0);
	}
	ID3D9NvrExtSampler* sampler = textures ? DxvkCompute::LinearClamp() : nullptr;
	if (!pass.Pipeline || (textures && !sampler)) return false;

	IDirect3DBaseTexture9* inputs[2] = { Textures.TransTexture, Textures.MSTexture };
	NVR_EXT_RESOURCE_BINDING resources[3] = {};
	for (UINT i = 0; i < textures; i++) {
		if (!inputs[i]) return false;
		resources[i].Slot = i; resources[i].Type = NVR_EXT_BIND_SAMPLED_IMAGE; resources[i].pResource = inputs[i];
	}
	if (textures) { resources[textures].Slot = 0; resources[textures].Type = NVR_EXT_BIND_SAMPLER; resources[textures].pResource = sampler; }

	SkyViewPush push = {};
	push.physSky = sky->Constants.PhysSky;
	push.physSkySun = sky->Constants.PhysSkySun;
	D3DSURFACE_DESC desc;
	if (FAILED(RenderTarget->GetDesc(&desc)) || !desc.Width || !desc.Height) return false;
	push.uvShift[0] = 0.5f / (float)TheRenderManager->width - 0.5f / (float)desc.Width;
	push.uvShift[1] = 0.5f / (float)TheRenderManager->height - 0.5f / (float)desc.Height;
	if (!ExtDrawTo(pass.Pipeline, &RenderTarget, 1, resources, resourceCount, &push, sizeof(push), nullptr, ExtUv::TexelCentre)) return false;	// uvShift adds it
	if (++ExtFrames == 1) Logger::Log("UNOFFICIAL NVR DXVK: %s runs as Vulkan passes (the physical sky's tables).", Name);
	return true;
}
