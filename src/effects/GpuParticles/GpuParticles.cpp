#include "GpuParticles.h"
#include "../../core/DxvkCompute.h"

// UNOFFICIAL 2026-10-26 (NVR DXVK 1.5-particles): see GpuParticles.h. Shaders: src\hlsl\NewVegas\Compute\GpuParticles*.

namespace {
	struct FrameBlock {				// GpuParticles.glsl GP_FRAME_BLOCK (std140, 288 bytes)
		D3DXMATRIX	viewProj;
		D3DXMATRIX	orthoProj;
		float		origin[4];
		float		camDelta[4];
		float		box[4];
		float		sim[4];
		float		look[4];
		float		screen[4];
		float		light[4];
		float		camRight[4];
		float		camUp[4];
		float		camFwd[4];
	};
	static_assert(sizeof(FrameBlock) == 288, "GP_FRAME_BLOCK layout");

	struct FragPush { float screen[4]; float tint[4]; float range[4]; };

	ID3D9NvrExtGeometry*	Geometry = nullptr;
	ID3D9NvrExtPipeline*	SimPipeline = nullptr;
	ID3D9NvrExtPipeline*	DrawPipeline = nullptr;
	ID3D9NvrExtBuffer*		ParticleBuffer = nullptr;
	ID3D9NvrExtBuffer*		FrameBuffer = nullptr;
	UINT					BufferCount = 0;
	bool					Tried = false;
	bool					Off = false;		// failed once: off for the session (logged)
	bool					Reset = true;
	D3DXVECTOR3				LastEye(0.0f, 0.0f, 0.0f);
	UInt32					LastFrame = 0;
	UInt32					FrameSeed = 0;
	UInt32					Frames = 0;

	void TurnOff(const char* why) {
		Off = true;
		Logger::Log("UNOFFICIAL GPU particles: off for this session: %s", why);
	}

	int SettingCount() {
		int count = TheSettingManager->GetSettingI("Shaders.GPUParticles.Main", "Count");
		if (count < 64) count = 64;
		if (count > 262144) count = 262144;
		return (count + 63) & ~63;
	}
}

bool GpuParticles::Wanted() {
	if (Off || !TheSettingManager->GetSettingI("Shaders.GPUParticles.Main", "Enabled")) return false;
	if (!DxvkCompute::Available()) return false;
	const auto& state = TheShaderManager->GameState;
	return (state.isRainy || state.isSnow) && state.isExterior && !state.isUnderwater;
}

bool GpuParticles::Ready() {
	if (Off) return false;
	if (!Tried) {
		Tried = true;
		ID3D9NvrExt* ext = DxvkCompute::Extension();
		if (!ext || FAILED(ext->QueryInterface(__uuidof(ID3D9NvrExtGeometry), (void**)&Geometry)) || !Geometry) {
			Geometry = nullptr;
			TurnOff("this d3d9.dll has no geometry passes (needs NVR DXVK 1.5-particles)");
			return false;
		}

		// simulation: depth + ortho map (texelFetch, no samplers), the particles (written), the frame block
		const NVR_EXT_BINDING_DESC simBindings[] = {
			{ 1, 0, NVR_EXT_BIND_SAMPLED_IMAGE,  0, NVR_EXT_VIEW_2D, FALSE, 0 },
			{ 1, 1, NVR_EXT_BIND_SAMPLED_IMAGE,  1, NVR_EXT_VIEW_2D, FALSE, 0 },
			{ 1, 2, NVR_EXT_BIND_STORAGE_BUFFER, 0, NVR_EXT_VIEW_2D, TRUE,  0 },
			{ 1, 3, NVR_EXT_BIND_UNIFORM_BUFFER, 1, NVR_EXT_VIEW_2D, FALSE, 0 },
		};
		std::vector<char> words;
		if (!DxvkCompute::LoadSpirv("GpuParticlesSim", words)) { TurnOff("GpuParticlesSim.spv missing"); return false; }
		NVR_EXT_COMPUTE_PIPELINE_DESC sim = {};
		sim.pSpirv = words.data();
		sim.SpirvSize = (UINT)words.size();
		sim.BindingCount = _countof(simBindings);
		sim.pBindings = simBindings;
		sim.pDebugName = "GpuParticlesSim";
		if (FAILED(ext->CreateComputePipeline(&sim, &SimPipeline)) || !SimPipeline) { TurnOff("the simulation pipeline was refused (DXVK's log says why)"); return false; }

		// drawing: the vertex shader reads the particles and the frame block; the fragment shader depth + the frame
		std::vector<char> vs, fs;
		if (!DxvkCompute::LoadSpirv("GpuParticlesVS", vs) || !DxvkCompute::LoadSpirv("GpuParticlesFS", fs)) { TurnOff("GpuParticlesVS/FS.spv missing"); return false; }
		const NVR_EXT_BINDING_DESC vsBindings[] = {
			{ 0, 1, NVR_EXT_BIND_STORAGE_BUFFER, 0, NVR_EXT_VIEW_2D, FALSE, 0 },
			{ 0, 2, NVR_EXT_BIND_UNIFORM_BUFFER, 1, NVR_EXT_VIEW_2D, FALSE, 0 },
		};
		const NVR_EXT_BINDING_DESC fsBindings[] = {
			{ 1, 0, NVR_EXT_BIND_SAMPLED_IMAGE, 0, NVR_EXT_VIEW_2D, FALSE, 0 },
			{ 1, 1, NVR_EXT_BIND_SAMPLED_IMAGE, 2, NVR_EXT_VIEW_2D, FALSE, 0 },
		};
		NVR_EXT_GEOMETRY_PIPELINE_DESC draw = {};
		draw.Graphics.pVertexSpirv = vs.data();
		draw.Graphics.VertexSpirvSize = (UINT)vs.size();
		draw.Graphics.pFragmentSpirv = fs.data();
		draw.Graphics.FragmentSpirvSize = (UINT)fs.size();
		draw.Graphics.BindingCount = _countof(fsBindings);
		draw.Graphics.pBindings = fsBindings;
		draw.Graphics.PushConstantSize = sizeof(FragPush);
		draw.Graphics.ColorTargetCount = 1;
		draw.Graphics.Blend = NVR_EXT_BLEND_ALPHA;
		draw.Graphics.ColorWriteMask = 1 | 2 | 4;	// the frame's alpha stays as it was
		draw.Graphics.pDebugName = "GpuParticles";
		draw.VertexBindingCount = _countof(vsBindings);
		draw.pVertexBindings = vsBindings;
		if (FAILED(Geometry->CreateGeometryPipeline(&draw, &DrawPipeline)) || !DrawPipeline) { TurnOff("the drawing pipeline was refused (DXVK's log says why)"); return false; }

		NVR_EXT_BUFFER_DESC frame = { sizeof(FrameBlock), NVR_EXT_BUFFER_UNIFORM | NVR_EXT_BUFFER_TRANSFER_DST, "GpuParticles frame" };
		if (FAILED(ext->CreateBuffer(&frame, &FrameBuffer)) || !FrameBuffer) { TurnOff("the frame buffer could not be made"); return false; }
		Logger::Log("UNOFFICIAL GPU particles: ready (NVR DXVK geometry passes)");
	}

	const UINT count = (UINT)SettingCount();
	if (count != BufferCount) {
		DxvkCompute::Drop(ParticleBuffer);
		NVR_EXT_BUFFER_DESC desc = { count * 32, NVR_EXT_BUFFER_STORAGE | NVR_EXT_BUFFER_TRANSFER_DST, "GpuParticles particles" };
		if (FAILED(DxvkCompute::Extension()->CreateBuffer(&desc, &ParticleBuffer)) || !ParticleBuffer) { TurnOff("the particle buffer could not be made"); return false; }
		DxvkCompute::Extension()->ClearBuffer(ParticleBuffer, 0, 0, 0);	// seed 0 = start anywhere in the box
		BufferCount = count;
		Reset = true;
		Logger::Log("UNOFFICIAL GPU particles: %u particles (%u KB)", count, count * 32 / 1024);
	}
	return true;
}

void GpuParticles::Render(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget) {
	if (!Wanted() || !Ready()) return;
	FrameChain& chain = TheShaderManager->Chain;
	if (!chain.Owns(RenderTarget, TheTextureManager->RenderedSurface)) {
		static bool said = false;
		if (!said) Logger::Log("UNOFFICIAL GPU particles: the effect chain is not active this frame (legacy copies): not drawn");
		said = true;
		return;
	}
	CombineDepthEffect* combine = TheShaderManager->Effects.CombineDepth;
	IDirect3DBaseTexture9* depth = combine ? combine->Textures.CombinedDepthTexture : nullptr;
	if (!depth) return;
	ShadowsExteriorEffect* shadows = TheShaderManager->Effects.ShadowsExteriors;
	IDirect3DBaseTexture9* ortho = shadows ? shadows->ShadowMapOrthoTexture : nullptr;

	const auto& state = TheShaderManager->GameState;
	const bool snow = state.isSnow;
	const float radius = (std::max)(200.0f, TheSettingManager->GetSettingF("Shaders.GPUParticles.Main", "Radius"));
	const float speed = (std::max)(0.05f, TheSettingManager->GetSettingF("Shaders.GPUParticles.Main", "Speed"));
	const float size = (std::max)(0.05f, TheSettingManager->GetSettingF("Shaders.GPUParticles.Main", "Size"));
	const float opacity = TheSettingManager->GetSettingF("Shaders.GPUParticles.Main", "Opacity");
	const bool splashes = TheSettingManager->GetSettingI("Shaders.GPUParticles.Main", "Splashes") != 0;

	// how far the camera moved since the last frame drawn; a jump (load, fast travel) or a pause starts the drops afresh
	const D3DXVECTOR3 eye(TheRenderManager->CameraPosition.x, TheRenderManager->CameraPosition.y, TheRenderManager->CameraPosition.z);
	D3DXVECTOR3 delta = eye - LastEye;
	const UInt32 frame = TheFrameRateManager->FrameNumber;
	if (frame - LastFrame > 2 || D3DXVec3Length(&delta) > radius) Reset = true;
	if (Reset) delta = D3DXVECTOR3(0.0f, 0.0f, 0.0f);
	LastEye = eye;
	LastFrame = frame;
	float dt = (float)TheFrameRateManager->ElapsedTime;
	if (!(dt >= 0.0f)) dt = 0.0f;
	if (dt > 0.1f) dt = 0.1f;

	FrameBlock f = {};
	f.viewProj = TheRenderManager->ViewProjMatrix;
	f.orthoProj = shadows ? shadows->ShadowMaps[ShadowsExteriorEffect::MapOrtho].ShadowCameraToLight : TheRenderManager->ViewProjMatrix;
	const D3DXMATRIX& inv = TheRenderManager->InvViewMatrix;
	const float origin[4] = { inv._41, inv._42, inv._43, TheRenderManager->CameraData.y };
	const float camDelta[4] = { delta.x, delta.y, delta.z, dt };
	const float fall = snow ? 70.0f * speed : 650.0f * speed;
	const float box[4] = { radius, 900.0f, 500.0f, fall };
	const float sim[4] = { (float)BufferCount, (float)(FrameSeed++ & 0xFFFFF), snow ? 4.0f : 0.4f, snow ? 1.0f : 0.0f };
	const float flags = (splashes ? 1.0f : 0.0f) + (ortho ? 2.0f : 0.0f) + (Reset ? 4.0f : 0.0f);
	const float look[4] = { size, 0.045f, opacity, flags };
	const float screen[4] = { (float)TheRenderManager->width, (float)TheRenderManager->height, TheRenderManager->projMatrix._11, TheRenderManager->projMatrix._22 };
	const D3DXVECTOR4& fog = TheShaderManager->ShaderConst.fogColor;
	auto lift = [](float v) { v = v * 1.2f + 0.06f; return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); };
	const float light[4] = { lift(fog.x), lift(fog.y), lift(fog.z), (float)fmod(TheShaderManager->ShaderConst.GameTime.z, 1000.0) };
	const float right[4] = { inv._11, inv._12, inv._13, 0.0f };
	const float up[4] = { inv._21, inv._22, inv._23, 0.0f };
	const float fwd[4] = { inv._31, inv._32, inv._33, TheRenderManager->CameraData.x };
	memcpy(f.origin, origin, 16); memcpy(f.camDelta, camDelta, 16); memcpy(f.box, box, 16); memcpy(f.sim, sim, 16);
	memcpy(f.look, look, 16); memcpy(f.screen, screen, 16); memcpy(f.light, light, 16);
	memcpy(f.camRight, right, 16); memcpy(f.camUp, up, 16); memcpy(f.camFwd, fwd, 16);
	Reset = false;

	ID3D9NvrExt* ext = DxvkCompute::Extension();
	if (FAILED(ext->UpdateBuffer(FrameBuffer, 0, sizeof(f), &f))) { TurnOff("UpdateBuffer failed"); return; }

	// 1. simulate
	const NVR_EXT_RESOURCE_BINDING simRes[] = {
		{ 0, NVR_EXT_BIND_SAMPLED_IMAGE,  depth, 0, 0, 0, FALSE },
		{ 1, NVR_EXT_BIND_SAMPLED_IMAGE,  ortho ? ortho : depth, 0, 0, 0, FALSE },
		{ 0, NVR_EXT_BIND_STORAGE_BUFFER, ParticleBuffer, 0, 0, 0, FALSE },
		{ 1, NVR_EXT_BIND_UNIFORM_BUFFER, FrameBuffer, 0, 0, 0, FALSE },
	};
	NVR_EXT_DISPATCH_DESC dispatch = {};
	dispatch.pPipeline = SimPipeline;
	dispatch.BindingCount = _countof(simRes);
	dispatch.pBindings = simRes;
	dispatch.GroupsX = BufferCount / 64;
	dispatch.GroupsY = dispatch.GroupsZ = 1;
	DxvkCompute::PreLoad(simRes, _countof(simRes));	// UNOFFICIAL 2026-10-27 (merge with the alflicker fix): managed textures on the GPU
	if (FAILED(ext->Dispatch(&dispatch))) { TurnOff("the simulation dispatch failed (DXVK's log says why)"); return; }

	// 2. draw over a copy of the frame (the shader reads the frame behind each drop), then that copy is the frame
	IDirect3DSurface9* out = chain.Output();
	Device->StretchRect(TheTextureManager->RenderedSurface, NULL, out, NULL, D3DTEXF_NONE);
	FragPush push = { { screen[0], screen[1], TheRenderManager->CameraData.y, opacity }, { light[0], light[1], light[2], snow ? 6.0f : 15.0f },
		{ radius * 1.1f, snow ? 1.0f : 0.0f, 0.0f, 0.0f } };
	const NVR_EXT_RESOURCE_BINDING drawRes[] = {
		{ 0, NVR_EXT_BIND_SAMPLED_IMAGE,  depth, 0, 0, 0, FALSE },
		{ 2, NVR_EXT_BIND_SAMPLED_IMAGE,  TheTextureManager->RenderedTexture, 0, 0, 0, FALSE },
		{ 0, NVR_EXT_BIND_STORAGE_BUFFER, ParticleBuffer, 0, 0, 0, FALSE },
		{ 1, NVR_EXT_BIND_UNIFORM_BUFFER, FrameBuffer, 0, 0, 0, FALSE },
	};
	NVR_EXT_GEOMETRY_DRAW_DESC draw = {};
	draw.Draw.pPipeline = DrawPipeline;
	draw.Draw.TargetCount = 1;
	draw.Draw.Targets[0].pResource = out;
	draw.Draw.BindingCount = _countof(drawRes);
	draw.Draw.pBindings = drawRes;
	draw.Draw.pPushData = &push;
	draw.Draw.PushDataSize = sizeof(push);
	draw.VertexCount = 6;
	draw.InstanceCount = BufferCount;
	DxvkCompute::PreLoad(drawRes, _countof(drawRes));
	if (FAILED(Geometry->DrawGeometry(&draw))) { TurnOff("the drawing pass failed (DXVK's log says why)"); return; }
	chain.Commit();
	if (++Frames == 1) Logger::Log("UNOFFICIAL GPU particles: running (%s, %u particles%s)", snow ? "snow" : "rain", BufferCount, ortho ? ", ortho map" : ", no ortho map");
}
