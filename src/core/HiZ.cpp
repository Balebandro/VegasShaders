#include "HiZ.h"
#include "GpuProfiler.h"
#include "MixCheck.h"	// UNOFFICIAL 2026-10-30 NvrDxvkMixCheck

// UNOFFICIAL 2026-10-26 (NVR DXVK, deep/fork-hiz): see HiZ.h.

ID3D9NvrExtBuffer*		HiZ::buffer = nullptr;
ID3D9NvrExtPipeline*	HiZ::pipeline = nullptr;
UINT					HiZ::width = 0;
UINT					HiZ::height = 0;
UINT					HiZ::groupsX = 0;
UINT					HiZ::groupsY = 0;
UINT					HiZ::builtFrame = 0;
bool					HiZ::built = false;
bool					HiZ::failed = false;
UINT					HiZ::frames = 0;
int						HiZ::CheckEvery = 0;

namespace {
	// HiZBuild.comp v2: set 1 = the depth, the weapon's depth (both read with texelFetch, no sampler), the pyramid buffer.
	const NVR_EXT_BINDING_DESC HiZBindings[] = {
		{ 1, 0, NVR_EXT_BIND_SAMPLED_IMAGE,  0, NVR_EXT_VIEW_2D, FALSE, 0 },	// TESR_DepthBuffer
		{ 1, 1, NVR_EXT_BIND_SAMPLED_IMAGE,  1, NVR_EXT_VIEW_2D, FALSE, 0 },	// TESR_DepthBufferViewModel
		{ 1, 2, NVR_EXT_BIND_STORAGE_BUFFER, 0, NVR_EXT_VIEW_2D, TRUE,  0 },	// the pyramid
	};
	struct HiZPush { INT32 fullW, fullH; float invertedDepth; UINT32 w0, h0; };
	static_assert(sizeof(HiZPush) == 20, "HiZBuild.comp push block");

	const D3DXVECTOR4* RegisteredConstant(const char* name) {
		auto found = TheShaderManager->ConstantsTable.find(name);
		return found == TheShaderManager->ConstantsTable.end() ? nullptr : found->second;
	}
}

void HiZ::Release() {
	DxvkCompute::Drop(buffer);
	DxvkCompute::Drop(pipeline);
}

UINT HiZ::LevelOffset(UINT k) {
	UINT o = 0;
	for (UINT i = 0; i < k; i++) o += (width >> i) * (height >> i);
	return o;
}

// The buffer and the pipeline, made on first use. False (and off for the session) when any of it fails.
bool HiZ::Ensure() {
	if (failed) return false;
	if (buffer && pipeline) return true;
	const UINT fullW = TheRenderManager->width, fullH = TheRenderManager->height;
	groupsX = (fullW + 127) / 128;		// one workgroup per 128x128 full-resolution pixels
	groupsY = (fullH + 127) / 128;
	width = groupsX * 64;
	height = groupsY * 64;
	std::vector<char> words;
	const bool shaderThere = DxvkCompute::LoadSpirv("HiZBuild", words);	// checked first: a missing file must not turn the other paths off
	const UINT bytes = LevelOffset(Levels) * 8;
	if (shaderThere) buffer = DxvkCompute::CreateBuffer(bytes, NVR_EXT_BUFFER_STORAGE | NVR_EXT_BUFFER_TRANSFER_SRC, "Hi-Z pyramid");
	if (buffer) pipeline = DxvkCompute::CreatePipeline("HiZBuild", HiZBindings, _countof(HiZBindings), sizeof(HiZPush), false);
	if (!buffer || !pipeline) {
		Logger::Log("UNOFFICIAL Hi-Z: the pyramid could not be set up (%s); Reflections keeps its normal trace.",
			!shaderThere ? "Compute\\HiZBuild.spv missing" : !buffer ? "buffer refused" : "pipeline refused");
		Release();
		failed = true;
		return false;
	}
	Logger::Log("UNOFFICIAL Hi-Z: pyramid ready (storage buffer, level 0 %ux%u, %u levels, %u bytes, %ux%u workgroups in one dispatch).", width, height, Levels, bytes, groupsX, groupsY);
	return true;
}

void HiZ::Build(IDirect3DDevice9* Device, bool Wanted) {
	built = false;
	if (!Wanted || failed || !DxvkCompute::Available()) return;
	const UINT now = TheFrameRateManager->FrameNumber;
	if (frames && builtFrame == now) { built = true; return; }
	if (!Ensure()) return;
	IDirect3DTexture9* depth = TheShaderManager->Effects.CombineDepth ? TheShaderManager->Effects.CombineDepth->Textures.CombinedDepthTexture : nullptr;
	IDirect3DSurface9* depthSurface = TheShaderManager->Effects.CombineDepth ? TheShaderManager->Effects.CombineDepth->Textures.CombinedDepthSurface : nullptr;
	IDirect3DTexture9* viewModel = TheTextureManager->DepthTextureViewModel;
	const D3DXVECTOR4* depthConstants = RegisteredConstant("TESR_DepthConstants");
	if (!depth || !depthSurface || !viewModel || !depthConstants) return;
	// The extension must not read a texture that is the device's render target: never the case here, but checked (costs nothing).
	for (DWORD i = 0; i < 4; i++) {
		IDirect3DSurface9* target = nullptr;
		if (SUCCEEDED(Device->GetRenderTarget(i, &target)) && target) {
			const bool same = target == depthSurface;
			target->Release();
			if (same) return;
		}
	}

	HiZPush push = { (INT32)TheRenderManager->width, (INT32)TheRenderManager->height, depthConstants->z, width, height };
	NVR_EXT_RESOURCE_BINDING bindings[_countof(HiZBindings)] = {};
	bindings[0] = { 0, NVR_EXT_BIND_SAMPLED_IMAGE,  depth,     0, 0, 0, FALSE };
	bindings[1] = { 1, NVR_EXT_BIND_SAMPLED_IMAGE,  viewModel, 0, 0, 0, FALSE };
	bindings[2] = { 0, NVR_EXT_BIND_STORAGE_BUFFER, buffer,    0, 0, 0, FALSE };
	NVR_EXT_DISPATCH_DESC dispatch = {};
	dispatch.pPipeline = pipeline;
	dispatch.BindingCount = _countof(bindings);
	dispatch.pBindings = bindings;
	dispatch.pPushData = &push;
	dispatch.PushDataSize = sizeof(push);
	dispatch.GroupsX = groupsX;
	dispatch.GroupsY = groupsY;
	dispatch.GroupsZ = 1;
	HRESULT result;
	{
		static GpuTimer timer("  Hi-Z pyramid");
		GpuProfileScope gpu(timer, Device);
		MixCheck::SetPass("HiZ");	// UNOFFICIAL 2026-10-30 NvrDxvkMixCheck
		DxvkCompute::PreLoad(bindings, _countof(bindings));	// UNOFFICIAL 2026-10-27 (merge with the alflicker fix): managed textures on the GPU
		result = DxvkCompute::Extension()->Dispatch(&dispatch);
	}
	if (FAILED(result)) {
		Logger::Log("UNOFFICIAL Hi-Z: the dispatch failed (%08lX); the pyramid is off for this session, Reflections keeps its normal trace.", result);
		Release();
		failed = true;
		return;
	}
	built = true;
	builtFrame = now;
	if (++frames == 1) Logger::Log("UNOFFICIAL Hi-Z: pyramid built (first frame).");
	if (CheckEvery > 0 && frames % (UINT)CheckEvery == 0) Check(depth, depthConstants->z);
}

ID3D9NvrExtBuffer* HiZ::Current() {
	return built && !failed && builtFrame == TheFrameRateManager->FrameNumber ? buffer : nullptr;
}

// UNOFFICIAL 2026-10-26 (deep/fork-hiz) test only, [Shaders.Reflections.Main] HiZCheck = N: every N built frames the depth buffer and
// pyramid levels 0 and 3 are read back (ReadBuffer: waits for the GPU, stalls that frame) and compared with min/max worked out on the
// CPU from the depth (the weapon mask is not re-checked: a cell whose stored nearest is 4 counts as 'weapon'). One log line per level.
void HiZ::Check(IDirect3DTexture9* depth, float invertedDepth) {
	ID3D9NvrExt* ext = DxvkCompute::Extension();
	D3DSURFACE_DESC dd = {};
	if (!ext || FAILED(depth->GetLevelDesc(0, &dd))) return;
	const UINT texel = dd.Format == D3DFMT_R32F ? 4 : dd.Format == D3DFMT_G32R32F ? 8 : 0;
	if (!texel) { Logger::Log("UNOFFICIAL Hi-Z check: depth format %u not readable here", (UINT)dd.Format); return; }
	const UINT w3 = width >> 3, h3 = height >> 3;
	const UINT depthBytes = dd.Width * dd.Height * texel, l0Bytes = width * height * 8, l3Bytes = w3 * h3 * 8;
	ID3D9NvrExtBuffer* staging = DxvkCompute::CreateBuffer(depthBytes, NVR_EXT_BUFFER_TRANSFER_DST | NVR_EXT_BUFFER_TRANSFER_SRC, "Hi-Z check depth");
	if (!staging) return;
	std::vector<float> dv(depthBytes / 4), l0(l0Bytes / 4), l3(l3Bytes / 4);
	bool ok = SUCCEEDED(ext->CopyTextureToBuffer(staging, 0, depth, 0, 0)) && SUCCEEDED(ext->ReadBuffer(staging, 0, depthBytes, dv.data())) &&
		SUCCEEDED(ext->ReadBuffer(buffer, 0, l0Bytes, l0.data())) && SUCCEEDED(ext->ReadBuffer(buffer, LevelOffset(3) * 8, l3Bytes, l3.data()));
	staging->Release();
	if (!ok) { Logger::Log("UNOFFICIAL Hi-Z check: readback failed"); return; }
	const UINT stride = texel / 4;
	auto at = [&](UINT x, UINT y) { x = (std::min)(x, dd.Width - 1); y = (std::min)(y, dd.Height - 1); return dv[(y * dd.Width + x) * stride]; };
	auto compare = [&](const std::vector<float>& lv, UINT w, UINT h, UINT cellPx, const char* name) {
		UINT cells = 0, weapon = 0, badMin = 0, badMax = 0, zeroMax = 0, swapped = 0; char example[160] = "";
		for (UINT y = 0; y < h; y += (h > 64 ? 7 : 1))
			for (UINT x = 0; x < w; x += (w > 64 ? 7 : 1)) {
				float mn = 1e30f, mx = -1e30f;
				for (UINT j = 0; j < cellPx; j++) for (UINT i = 0; i < cellPx; i++) { float v = at(x * cellPx + i, y * cellPx + j); mn = (std::min)(mn, v); mx = (std::max)(mx, v); }
				const float r = lv[(y * w + x) * 2], g = lv[(y * w + x) * 2 + 1];
				cells++;
				if (r >= 3.0f) { weapon++; continue; }
				const bool wrongMin = fabsf(r - mn) > 1e-6f + 1e-4f * mn, wrongMax = fabsf(g - mx) > 1e-6f + 1e-4f * mx;
				if (wrongMin) badMin++;
				if (wrongMax) badMax++;
				if (g == 0.0f) zeroMax++;
				if (fabsf(r - mx) <= 1e-6f + 1e-4f * mx && fabsf(g - mn) <= 1e-6f + 1e-4f * mn && mn != mx) swapped++;
				if (!example[0] && (wrongMin || wrongMax))
					_snprintf_s(example, sizeof(example), _TRUNCATE, " first bad cell (%u,%u): stored %.6f / %.6f, depth min %.6f max %.6f", x, y, r, g, mn, mx);
			}
		Logger::Log("UNOFFICIAL Hi-Z check %s (frame %u): %u cells, %u 'weapon' (nearest 4), nearest wrong %u, farthest wrong %u (of them 0: %u), swapped %u.%s",
			name, TheFrameRateManager->FrameNumber, cells, weapon, badMin, badMax, zeroMax, swapped, example);
	};
	compare(l0, width, height, 2, "level 0");
	compare(l3, w3, h3, 16, "level 3");
	Logger::Log("UNOFFICIAL Hi-Z check: depth %ux%u (%s), inverted depth %.0f, centre depth %.6f", dd.Width, dd.Height, texel == 4 ? "R32F" : "G32R32F",
		invertedDepth, at(dd.Width / 2, dd.Height / 2));
}
