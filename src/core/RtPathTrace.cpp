// UNOFFICIAL 2026-11-02 the reference path tracer: RtPathTrace.h; the trace shader is Compute\Rt\RtPathTrace.comp (run by the helper),
// the display pass Compute\RtPathTraceShow.frag (a fork pass in the game).

#include "RtPathTrace.h"
#include "RtCore.h"
#include "DxvkCompute.h"
#include "GpuProfiler.h"
#include "RtDenoise.h"
#include "ManyLamps.h"	// (2026-11-03 rtspec) PTGameLamps 4: the game's lamp table
#include "RenderRect.h"	// (2026-11-03) RenderScale < 1: the trace over the frame's top-left rect (docs/next/RT-RENDERSCALE.md)
#include <algorithm>

namespace {
	const char* const kSection = "Main.Main.RayTracing";

	struct Settings {
		bool	enabled = false;		// ReferencePT
		int		bounces = 4;			// PTBounces [1-16]
		int		samples = 1;			// PTSamples [1-16] paths a pixel and frame
		int		maxSamples = 4096;		// PTMaxSamples: the mean stops growing (and the helper stops tracing) here
		float	sky = 1.0f;				// PTSky: the sky's light on misses outdoors
		float	ambient = 0.0f;			// PTAmbient: the interior's light on misses (x the game's ambient)
		float	glow = 1.0f;			// PTGlow: glow maps' light (needs GlowTextures)
		float	clamp = 0.0f;			// PTClamp: a path's luminance cap (0 = none: unbiased)
		float	exposure = 1.0f;		// PTExposure (display only, live without a restart of the mean)
		float	split = 0.0f;			// PTSplit: 0 the whole screen, 0.5 the right half (the game's frame on the left)
		int		debugView = 0;			// PTDebugView: 1 albedo, 2 normals, 3 the first hit's direct light only; (2026-11-03) 5 the first hit's arriving indirect light, 6 / 7 RT GI's bounce (raw / denoised) shown the same way;
										// (rtspec) 8 the first hit's direct highlight only, 9 its reflection only
		bool	primary = true;			// PTPrimary (2026-11-02 b): the first hit's colour and normal from the screen's G-buffer
		bool	replace = false;		// PTReplace (2026-11-03): the traced picture INTO the game's HDR scene (the post chain runs on it)
		bool	denoise = false;		// PTDenoise (2026-11-03): interactive: each frame's own paths through the shared RT denoiser, no mean
		bool	specular = true;		// PTSpecular (2026-11-02 e): GGX highlights + reflections (roughness / metal: the MaterialBuffer on screen)
		bool	glowLights = false;		// PTGlowLights (2026-11-03 b): glow-mapped meshes sampled as lights (RtPathTrace.comp GlowSample)
		int		gameLamps = 4;			// PTGameLamps (2026-11-03 rtspec): the lamps as the game lights them: 1 its falloff ((1 - (d/r)^2)^2.2), 2 its colour
										// (colour x dimmer decoded together), 3 both, 4 the game's own lamp table (its lamps, reach and colour) + its falloff,
										// 0 neither (the older guess, as RT GI)
	} S;

	// RtPathTrace.comp's uniform block, std140
	struct PassConstants {
		float size[4], forward[4], right[4], up[4], origin[4], accum[4], sunDir[4], sunColor[4], skyZenith[4], skyHorizon[4], ambient[4], misc[4];
		float lampPos[8][4], lampColor[8][4];
		float rect[4];	// (2026-11-03) xy the depth's jitter (screen uv)
	};
	static_assert(sizeof(PassConstants) <= 512, "RtCore: 512 bytes of constants a pass");

	int			imageA = -1, imageB = -1, imageDisplay = -1, shader = -1;
	int			imageDepth = -1, imageNormals = -1, imageAlbedo = -1, imagePano = -1, imageFrame = -1, imageMaterial = -1, imageAlbedoOut = -1, imageLamps = -1;
	constexpr int kLampsMax = 256;	// (2026-11-03) the lamp image: ShadowCubeMapsMax + TrackedLightsMax (76) fit; (rtspec) the game's table's nearest 256
	constexpr int kGlowMax = 32;	// (2026-11-03 b) PTGlowLights: at most this many glow lights, in the lamp image after the lamps
	constexpr int kLightTexels = 4 * (kLampsMax + kGlowMax);
	IDirect3DTexture9*	lampTexture = nullptr;	// (2026-11-03) the lamp list for the helper (G32R32F, 4 texels a lamp)
	struct LampData { float pos[4], colour[4]; };
	LampData	lampData[kLampsMax];
	int			lampCount = 0;	// (2026-11-02 b) PTPrimary's G-buffer, the sky panorama
	IDirect3DTexture9*	displayTexture = nullptr;
	IDirect3DSurface9*	displaySurface = nullptr;
	IDirect3DTexture9*	denoisedTexture = nullptr;	// (2026-11-03) PTDenoise's output
	IDirect3DTexture9*	albedoTexture = nullptr;	// (2026-11-03) PTDenoise: the first surface's colour (the helper's albedo out)
	IDirect3DSurface9*	albedoSurface = nullptr;
	bool		albedoThisFrame = false;
	float		tracedScale[2] = { 1.0f, 1.0f };	// (2026-11-03) the RenderScale rect the shown trace covers (the overlay after the upscale reads it at uv x this)
	UINT		lastRect[2] = {};
	IDirect3DSurface9*	denoisedSurface = nullptr;
	RtDenoise::Denoiser* denoiser = nullptr;
	IDirect3DTexture9*	parkTexture = nullptr;		// a pass may not draw into the device's current render target: park it here meanwhile
	IDirect3DSurface9*	parkSurface = nullptr;
	UINT		textureWidth = 0, textureHeight = 0;
	ID3D9NvrExtPipeline* showPipeline = nullptr;
	bool		showTried = false;
	UInt32		samples = 0;				// in the helper's mean (counted when this frame's pass and copy were recorded)
	bool		haveImage = false;			// displayTexture holds a traced mean (also after the trace stopped at PTMaxSamples)
	UInt32		resets = 0;
	// what the mean was traced with: any change starts it over
	D3DXMATRIX	lastView;
	D3DXVECTOR4	lastCamera, lastSun;
	float		lastKey[8] = {};
	bool		lastValid = false;

	void ReadSettings() {
		auto f = [](const char* k, float lo, float hi) { return (std::min)((std::max)(TheSettingManager->GetSettingF(kSection, k), lo), hi); };
		auto i = [](const char* k, int lo, int hi) { return (std::min)((std::max)(TheSettingManager->GetSettingI(kSection, k), lo), hi); };
		S.enabled = TheSettingManager->GetSettingI(kSection, "ReferencePT") != 0;
		S.bounces = i("PTBounces", 1, 16);
		S.samples = i("PTSamples", 1, 16);
		S.maxSamples = i("PTMaxSamples", 1, 1 << 20);
		S.sky = f("PTSky", 0.0f, 16.0f);
		S.ambient = f("PTAmbient", 0.0f, 4.0f);
		S.glow = f("PTGlow", 0.0f, 16.0f);
		S.clamp = f("PTClamp", 0.0f, 1000.0f);
		S.exposure = f("PTExposure", 0.01f, 64.0f);
		S.split = f("PTSplit", 0.0f, 1.0f);
		S.debugView = i("PTDebugView", 0, 9);
		S.primary = TheSettingManager->GetSettingI(kSection, "PTPrimary") != 0;
		S.specular = TheSettingManager->GetSettingI(kSection, "PTSpecular") != 0;
		S.denoise = TheSettingManager->GetSettingI(kSection, "PTDenoise") != 0;
		S.replace = TheSettingManager->GetSettingI(kSection, "PTReplace") != 0;
		S.glowLights = TheSettingManager->GetSettingI(kSection, "PTGlowLights") != 0;
		S.gameLamps = std::clamp(TheSettingManager->GetSettingI(kSection, "PTGameLamps"), 0, 4);
	}

	float Lin(float v) { return powf((std::max)(v, 0.0f), 2.2f); }

	// The lamps that light the hits: the shadow slots' lamps and the tracked ones as ShaderManager listed them this frame, nearest
	// first, at most 8; scene space (world minus CameraPosition), colour linear x dimmer (as AmbientOcclusion.cpp RtGILamps).
	int Lamps(PassConstants& c) {
		struct Lamp { D3DXVECTOR4 pos, colour; float distance; };
		Lamp lamps[ShadowCubeMapsMax + TrackedLightsMax];
		int found = 0;
		const D3DXVECTOR4& eye = TheRenderManager->CameraPosition;
		auto add = [&](const D3DXVECTOR4& p, const D3DXVECTOR4& col) {
			if (p.w <= 10.0f || col.w <= 0.0f || found >= ShadowCubeMapsMax + TrackedLightsMax) return;
			const D3DXVECTOR3 rel(p.x - eye.x, p.y - eye.y, p.z - eye.z);
			for (int i = 0; i < found; i++)	// the same lamp in both lists
				if (fabsf(lamps[i].pos.x - rel.x) < 1.0f && fabsf(lamps[i].pos.y - rel.y) < 1.0f && fabsf(lamps[i].pos.z - rel.z) < 1.0f) return;
			// (2026-11-03 rtspec) PTGameLamps: the colour as the game's lamp table makes it (ManyLamps.h: diffuse x dimmer, the dimmer at most 1
			// without HDR, then decoded to linear together; the light's LOD dimmer is not in these lists); off: diffuse decoded x dimmer, as RT GI
			float dim = col.w;
			const bool gameColour = (S.gameLamps & 2) != 0;
			if (gameColour && !*(const bool*)0x011F941E && dim > 1.0f) dim = 1.0f;	// BSShaderManager::bHDR
			const D3DXVECTOR4 colour = gameColour ? D3DXVECTOR4(Lin(col.x * dim), Lin(col.y * dim), Lin(col.z * dim), 0.0f) :
				D3DXVECTOR4(Lin(col.x) * col.w, Lin(col.y) * col.w, Lin(col.z) * col.w, 0.0f);
			lamps[found++] = { D3DXVECTOR4(rel.x, rel.y, rel.z, p.w), colour,
				(std::max)(D3DXVec3Length(&rel) - p.w, 0.0f) };
		};
		ShaderManager* sm = TheShaderManager;
		// (rtspec) PTGameLamps 4: the game's lamp table's active lamps this frame (the deferred pass's lamps), nearest first, kLampsMax at most
		if (S.gameLamps == 4 && ManyLamps::TableFrame + 1 >= ManyLamps::FrameId && !ManyLamps::ActiveLamps.empty()) {
			static std::vector<Lamp> table;
			table.clear();
			for (const ManyLamps::ActiveLamp& a : ManyLamps::ActiveLamps) {
				if (a.reach <= 0.0f) continue;
				const D3DXVECTOR3 rel(a.pos[0] - eye.x, a.pos[1] - eye.y, a.pos[2] - eye.z);
				table.push_back({ D3DXVECTOR4(rel.x, rel.y, rel.z, a.reach), D3DXVECTOR4(a.colour[0], a.colour[1], a.colour[2], 0.0f),
					(std::max)(D3DXVec3Length(&rel) - a.reach, 0.0f) });
			}
			std::sort(table.begin(), table.end(), [](const Lamp& a, const Lamp& b) { return a.distance < b.distance; });
			lampCount = (std::min)((int)table.size(), kLampsMax);
			for (int i = 0; i < lampCount; i++) { memcpy(lampData[i].pos, &table[i].pos, 16); memcpy(lampData[i].colour, &table[i].colour, 16); }
			const int first = (std::min)(lampCount, 8);
			for (int i = 0; i < first; i++) { memcpy(c.lampPos[i], &table[i].pos, 16); memcpy(c.lampColor[i], &table[i].colour, 16); }
			return first;
		}
		for (int s = 0; s < ShadowCubeMapsMax; s++) add(sm->BatchLightPosition[s], sm->BatchLightColor[s]);
		for (int t = 0; t < TrackedLightsMax; t++) add(sm->LightPosition[t], sm->LightColor[ShadowCubeMapsScreen + t]);
		std::sort(lamps, lamps + found, [](const Lamp& a, const Lamp& b) { return a.distance < b.distance; });
		lampCount = (std::min)(found, kLampsMax);	// (2026-11-03) all of them for the lamp image
		for (int i = 0; i < lampCount; i++) { memcpy(lampData[i].pos, &lamps[i].pos, 16); memcpy(lampData[i].colour, &lamps[i].colour, 16); }
		found = (std::min)(found, 8);
		for (int i = 0; i < found; i++) { memcpy(c.lampPos[i], &lamps[i].pos, 16); memcpy(c.lampColor[i], &lamps[i].colour, 16); }
		return found;
	}

	// UNOFFICIAL 2026-11-03 b PTGlowLights: the glow lights for the lamp image (4 floats a texel pair: centre, radius; a guessed colour):
	// the frame's glow-mapped instances (RtCore::GlowLights(): world bound sphere, the record's average colour), the kGlowMax that
	// should give the camera's surroundings the most light (guessed colour x radius^2 / distance^2). The sphere only tells the trace
	// WHERE to look (RtPathTrace.comp GlowSample shoots a ray into it and takes the real mesh's glow); the colour only steers how often
	// it is picked. Bounds over 512 units (rooms, whole facades with a few lit windows) stay out: aiming at a sphere that size is no
	// better than a bounce ray, and their glow keeps coming from bounce rays. The radius is grown by 1 % + 1 unit (float slack: every
	// glowing point of the mesh must be inside its sphere).
	struct GlowData { float pos[4], colour[4]; };
	GlowData	glowData[kGlowMax];
	int Glows() {
		if (!S.glowLights || S.glow <= 0.0f) return 0;
		struct Pick { const RtCore::GlowLight* g; float weight; };
		std::vector<Pick> picks;
		for (const RtCore::GlowLight& g : RtCore::GlowLights()) {
			if (!(g.radius > 0.0f) || g.radius > 512.0f) continue;
			const float luma = 0.2126f * g.rgb[0] + 0.7152f * g.rgb[1] + 0.0722f * g.rgb[2] + 0.05f;
			const float d2 = g.center[0] * g.center[0] + g.center[1] * g.center[1] + g.center[2] * g.center[2];
			picks.push_back({ &g, luma * g.radius * g.radius / (std::max)(d2, g.radius * g.radius) });
		}
		const size_t keep = (std::min)(picks.size(), (size_t)kGlowMax);
		std::partial_sort(picks.begin(), picks.begin() + keep, picks.end(), [](const Pick& a, const Pick& b) { return a.weight > b.weight; });
		for (size_t i = 0; i < keep; i++) {
			const RtCore::GlowLight& g = *picks[i].g;
			GlowData& d = glowData[i];
			d.pos[0] = g.center[0]; d.pos[1] = g.center[1]; d.pos[2] = g.center[2]; d.pos[3] = g.radius * 1.01f + 1.0f;
			// a guess of the light it gives (only how often it is picked): its colour (floor 0.05: never 0, a listed light must be
			// pickable) x PTGlow x 0.25 (the share of a bound sphere's view a glowing mesh tends to fill)
			for (int k = 0; k < 3; k++) d.colour[k] = ((std::max)(g.rgb[k], 0.0f) + 0.05f) * S.glow * 0.25f;
			d.colour[3] = 0.0f;
		}
		return (int)keep;
	}

	bool Wants() {
		ReadSettings();
		return S.enabled;
	}

	void Setup() {
		const UINT w = TheRenderManager->width, h = TheRenderManager->height;
		imageA = RtCore::AddImage("NVR RT path trace mean rg", w, h, NVR_EXT_SHARED_RG32F);
		imageB = RtCore::AddImage("NVR RT path trace mean b", w, h, NVR_EXT_SHARED_RG32F);
		imageDisplay = RtCore::AddImage("NVR RT path trace", w, h, NVR_EXT_SHARED_RGBA16F);
		imageDepth = RtCore::AddImage("NVR RT path trace depth", w, h, NVR_EXT_SHARED_R32F);
		imageNormals = RtCore::AddImage("NVR RT path trace normals", w, h, NVR_EXT_SHARED_RGBA16F);
		imageAlbedo = RtCore::AddImage("NVR RT path trace albedo", w, h, NVR_EXT_SHARED_RGBA16F);
		imagePano = RtCore::AddImage("NVR RT path trace sky", 128, 128, NVR_EXT_SHARED_RGBA16F);	// (Reflections.h pano[]: 128 x 128)
		imageFrame = RtCore::AddImage("NVR RT path trace frame", w, h, NVR_EXT_SHARED_RGBA16F);
		imageMaterial = RtCore::AddImage("NVR RT path trace material", w, h, NVR_EXT_SHARED_RGBA16F);	// (2026-11-02 e) TESR_MaterialBuffer
		imageAlbedoOut = RtCore::AddImage("NVR RT path trace first albedo", w, h, NVR_EXT_SHARED_RGBA16F);	// (2026-11-03) PTDenoise's demodulation
		imageLamps = RtCore::AddImage("NVR RT path trace lamps", kLightTexels, 1, NVR_EXT_SHARED_RG32F);	// (2026-11-03) all the frame's lamps	// (2026-11-02 c) the game's frame at the core's point
		samples = 0;	// new images: the mean starts over
		DxvkCompute::Drop(showPipeline);	// (a helper restart by reloadshaders: the display pass's .spv may have changed too)
		showTried = false;
	}

	// Whether anything the mean depends on moved since the last traced frame (a still first-person camera keeps its matrix exactly;
	// the tolerances only absorb float noise). The projection's jitter (FSR / DLSS) is left out: only its scale counts.
	bool Moved(const D3DXVECTOR4& sun) {
		const D3DXMATRIX& view = TheRenderManager->viewMatrix;
		const D3DXMATRIX& proj = TheRenderManager->projMatrix;
		const D3DXVECTOR4& camera = TheRenderManager->CameraPosition;
		const float key[8] = { (float)S.bounces, S.sky, S.ambient, S.glow, S.clamp, (float)S.debugView + (S.primary ? 16.0f : 0.0f) + (S.specular ? 32.0f : 0.0f) + (S.glowLights ? 64.0f : 0.0f) + 128.0f * (float)S.gameLamps, proj._11, proj._22 };
		bool moved = !lastValid;
		for (int i = 0; i < 16 && !moved; i++) moved = fabsf(((const float*)&view)[i] - ((const float*)&lastView)[i]) > 1.0e-4f;
		if (!moved) moved = fabsf(camera.x - lastCamera.x) > 0.05f || fabsf(camera.y - lastCamera.y) > 0.05f || fabsf(camera.z - lastCamera.z) > 0.05f;
		if (!moved) moved = fabsf(sun.x - lastSun.x) + fabsf(sun.y - lastSun.y) + fabsf(sun.z - lastSun.z) > 0.002f || sun.w != lastSun.w;
		for (int i = 0; i < 8 && !moved; i++) moved = key[i] != lastKey[i];
		lastView = view; lastCamera = camera; lastSun = sun;
		memcpy(lastKey, key, sizeof(key));
		lastValid = true;
		return moved;
	}

	void Record(IDirect3DDevice9* Device) {
		if (imageA < 0 || imageB < 0 || imageDisplay < 0 || shader < 0) return;
		const UINT w = TheRenderManager->width, h = TheRenderManager->height;
		if (!displayTexture || textureWidth != w || textureHeight != h) {
			DxvkCompute::Drop(displaySurface); DxvkCompute::Drop(displayTexture);
			TheTextureManager->InitTexture("NVR_RtPathTrace", &displayTexture, &displaySurface, w, h, D3DFMT_A16B16G16R16F);
			textureWidth = w;
			textureHeight = h;
			samples = 0;
			haveImage = false;
			if (!displayTexture) return;
		}
		ShadowsExteriorEffect* shadows = TheShaderManager->Effects.ShadowsExteriors;
		const bool exterior = TheShaderManager->GameState.isExterior;
		const bool sunUp = exterior && shadows && shadows->Constants.ShadowFade.x < 1.0f;
		const D3DXVECTOR4 sunDir = shadows ? shadows->Constants.SmoothedSunDir : D3DXVECTOR4(0.0f, 0.0f, 1.0f, 0.0f);
		const D3DXVECTOR4 sunKey(sunDir.x, sunDir.y, sunDir.z, sunUp ? 1.0f : 0.0f);
		if (Moved(sunKey)) {
			if (samples > 0) resets++;
			samples = 0;
		}
		if (S.denoise) samples = 0;	// (2026-11-03) PTDenoise: every frame its own paths (the denoiser keeps the history, through camera moves)
		else if ((int)samples >= S.maxSamples) return;	// converged: the last copy stays on show, the helper rests

		const D3DXMATRIX& view = TheRenderManager->viewMatrix;
		const D3DXMATRIX& proj = TheRenderManager->projMatrix;
		PassConstants c = {};
		// (2026-11-03) RenderScale < 1: the frame is the top-left RenderRect part of the screen-size G-buffer: trace only that (its uv
		// = pixel / rect size is the screen's), into the top-left of the full-size display texture (Replace writes it texel for texel into
		// the rect-layout scene; the overlay after the upscale reads it at uv x the scale). The mean starts over when the rect changes.
		const UINT tw = RenderRect::Width(w), th = RenderRect::Height(h);
		if (tw != lastRect[0] || th != lastRect[1]) { lastRect[0] = tw; lastRect[1] = th; samples = 0; }
		c.size[0] = (float)tw; c.size[1] = (float)th; c.size[2] = 1.0f / tw; c.size[3] = 1.0f / th;
		if (TAAEffect* taa = TheShaderManager->Effects.TAA) { const D3DXVECTOR2 j = taa->DepthJitterUV(); c.rect[0] = j.x; c.rect[1] = j.y; }
		// as RtSunShadows: the view matrix's columns are right (0), up (1) and forward (2)
		c.forward[0] = view._13; c.forward[1] = view._23; c.forward[2] = view._33;
		auto constant = [](const char* name) { auto f = TheShaderManager->ConstantsTable.find(name); return f == TheShaderManager->ConstantsTable.end() ? nullptr : f->second; };
		auto texture = [](const char* name) -> IDirect3DBaseTexture9* { auto f = TheTextureManager->TextureNames.find(name); return f == TheTextureManager->TextureNames.end() || !f->second ? nullptr : *f->second; };
		const D3DXVECTOR4* cameraData = constant("TESR_CameraData");
		c.forward[3] = cameraData ? cameraData->y : 0.0f;	// farZ: the depth buffer's scale
		c.right[0] = view._11 / proj._11; c.right[1] = view._21 / proj._11; c.right[2] = view._31 / proj._11;
		c.up[0] = view._12 / proj._22; c.up[1] = view._22 / proj._22; c.up[2] = view._32 / proj._22;
		const D3DXVECTOR3 eye = RtCore::Eye();
		c.origin[0] = eye.x; c.origin[1] = eye.y; c.origin[2] = eye.z;
		c.origin[3] = (float)((TheFrameRateManager->FrameNumber * 7919u) & 0xFFFFFu);	// the random seed (exact in a float)
		const int alphaAtlas = RtCore::AlphaAtlasImage(), albedoAtlas = RtCore::AlbedoAtlasImage(), glowAtlas = RtCore::GlowAtlasImage();
		UInt32 flags = (RtCore::AlphaTested() && alphaAtlas >= 0 ? 1u : 0u) | (albedoAtlas >= 0 ? 2u : 0u) | (glowAtlas >= 0 && albedoAtlas >= 0 ? 4u : 0u);
		// (2026-11-02 b) PTPrimary: the screen's depth, normals and albedo (CapturedNormals' AlbedoBuffer: linear base colour, the land's
		// blended layers) for the first hit, when this frame's albedo capture ran
		const D3DXVECTOR4* albedoData = constant("TESR_AlbedoBufferData");
		IDirect3DBaseTexture9* gDepth = texture("TESR_DepthBuffer");
		IDirect3DBaseTexture9* gNormals = texture("TESR_NormalsBuffer");
		IDirect3DBaseTexture9* gAlbedo = texture("TESR_AlbedoBuffer");
		if (S.primary && imageDepth >= 0 && imageNormals >= 0 && imageAlbedo >= 0 && c.forward[3] > 0.0f && albedoData && albedoData->x > 0.5f &&
			gDepth && gNormals && gAlbedo && RtCore::CopyIn(imageDepth, gDepth) && RtCore::CopyIn(imageNormals, gNormals) && RtCore::CopyIn(imageAlbedo, gAlbedo))
			flags |= 8u;
		// (2026-11-02 e) the screen's roughness / metal (CapturedNormals' MaterialBuffer) with the first hit's G-buffer
		const D3DXVECTOR4* materialData = constant("TESR_MaterialBufferData");
		IDirect3DBaseTexture9* gMaterial = texture("TESR_MaterialBuffer");
		if ((flags & 8u) && S.specular && imageMaterial >= 0 && materialData && materialData->x > 0.5f && gMaterial && RtCore::CopyIn(imageMaterial, gMaterial))
			flags |= 256u;
		if (S.specular) flags |= 128u;
		if ((S.gameLamps & 1) || S.gameLamps == 4) flags |= 2048u;	// (2026-11-03 rtspec) PTGameLamps: the game's falloff
		if (S.denoise && imageAlbedoOut >= 0) flags |= 512u;	// (2026-11-03) demodulated for the denoiser
		// (2026-11-02 b) the sky on misses: RT reflections' SkyPanorama (the sky as the screen saw it, clouds and all; needs the Reflections
		// effect with [Shaders.Reflections.RT] SkyPanorama), last frame's, while it is fresh; else the 2-colour gradient
		ReflectionsEffect* reflections = TheShaderManager->Effects.Reflections;
		IDirect3DTexture9* pano = exterior && reflections ? reflections->SkyPanoramaTexture(TheFrameRateManager->FrameNumber, 3) : nullptr;
		if (pano && imagePano >= 0 && RtCore::CopyIn(imagePano, pano))
			flags |= 16u;
		// (2026-11-02 c) the frame as drawn so far (the world, the sky; NVR's effects not yet): camera rays that leave the RT scene show it
		IDirect3DSurface9* frame = nullptr;
		if (S.primary && imageFrame >= 0 && SUCCEEDED(Device->GetRenderTarget(0, &frame)) && frame) {
			if (RtCore::CopyIn(imageFrame, frame)) flags |= 32u;
			frame->Release();
		}
		const int normalAtlas = RtCore::NormalAtlasImage();	// (2026-11-02 d) NormalTextures
		if (normalAtlas >= 0 && albedoAtlas >= 0) flags |= 64u;
		static UInt32 lastFlags = 0;
		if (flags != lastFlags) { samples = 0; lastFlags = flags; }	// a source came or went (the G-buffer, the panorama, an atlas): start over
		c.accum[0] = (float)samples; c.accum[1] = (float)S.bounces; c.accum[2] = (float)S.samples; c.accum[3] = (float)flags;
		const ShaderConstants& k = TheShaderManager->ShaderConst;
		memcpy(c.sunDir, &sunKey, 16);
		c.sunColor[0] = Lin(k.sunColor.x); c.sunColor[1] = Lin(k.sunColor.y); c.sunColor[2] = Lin(k.sunColor.z);
		c.sunColor[3] = tanf(0.5f * 0.53f * 0.0174532925f);	// the sun's disc
		c.skyZenith[0] = Lin(k.skyColor.x) * S.sky; c.skyZenith[1] = Lin(k.skyColor.y) * S.sky; c.skyZenith[2] = Lin(k.skyColor.z) * S.sky;
		c.skyZenith[3] = exterior ? 1.0f : 0.0f;
		c.skyHorizon[0] = Lin(k.horizonColor.x) * S.sky; c.skyHorizon[1] = Lin(k.horizonColor.y) * S.sky; c.skyHorizon[2] = Lin(k.horizonColor.z) * S.sky;
		c.skyHorizon[3] = S.glow;
		c.ambient[0] = Lin(k.sunAmbient.x) * S.ambient; c.ambient[1] = Lin(k.sunAmbient.y) * S.ambient; c.ambient[2] = Lin(k.sunAmbient.z) * S.ambient;
		c.ambient[3] = S.sky;	// the panorama's scale
		c.misc[0] = (float)Lamps(c);
		// (2026-11-03) all the lamps through the lamp image (G32R32F, 4 texels a lamp), when it can be filled and copied this frame
		// (2026-11-03 b) PTGlowLights: the glow lights after the lamps (rect.z = how many)
		const int glowCount = Glows();
		if (imageLamps >= 0 && (lampCount > 8 || glowCount > 0)) {
			if (!lampTexture) TheRenderManager->device->CreateTexture(kLightTexels, 1, 1, D3DUSAGE_DYNAMIC, D3DFMT_G32R32F, D3DPOOL_DEFAULT, &lampTexture, nullptr);
			D3DLOCKED_RECT r = {};
			if (lampTexture && SUCCEEDED(lampTexture->LockRect(0, &r, nullptr, D3DLOCK_DISCARD))) {
				float* t = (float*)r.pBits;
				memset(t, 0, sizeof(float) * 2 * kLightTexels);
				for (int i = 0; i < lampCount; i++) {
					const LampData& d = lampData[i];
					const float v[8] = { d.pos[0], d.pos[1], d.pos[2], d.pos[3], d.colour[0], d.colour[1], d.colour[2], 0.0f };
					memcpy(t + 8 * i, v, sizeof(v));
				}
				for (int i = 0; i < glowCount; i++) {
					const GlowData& d = glowData[i];
					const float v[8] = { d.pos[0], d.pos[1], d.pos[2], d.pos[3], d.colour[0], d.colour[1], d.colour[2], 0.0f };
					memcpy(t + 8 * (lampCount + i), v, sizeof(v));
				}
				lampTexture->UnlockRect(0);
				if (RtCore::CopyIn(imageLamps, lampTexture)) { flags |= 1024u; c.misc[0] = (float)lampCount; c.rect[2] = (float)glowCount; }
			}
		}
		c.accum[3] = (float)flags;	// (the lamp image's flag 1024 comes after the reset check: a lamp list change does not restart the mean)
		// (2026-11-03 b) FIX: these three were inside the comment above since 940b1ef2 (the lamp image), so the ray length was 0: no
		// traced hit at all (bounce rays all "missed": outdoors they took the sky, indoors nothing), PTClamp and PTDebugView did nothing
		c.misc[1] = S.clamp; c.misc[2] = 60000.0f; c.misc[3] = (float)S.debugView;

		// images: the mean (2), the display copy, then the atlases (a written image stands in for a missing one: its flag is off)
		const int images[15] = { imageA, imageB, imageDisplay, flags & 1u ? alphaAtlas : imageDisplay, albedoAtlas >= 0 ? albedoAtlas : imageDisplay,
			flags & 4u ? glowAtlas : albedoAtlas >= 0 ? albedoAtlas : imageDisplay, imageDepth, imageNormals, imageAlbedo, imagePano, imageFrame,
			flags & 64u ? normalAtlas : albedoAtlas >= 0 ? albedoAtlas : imageDisplay, imageMaterial, imageAlbedoOut, imageLamps };
		if (!RtCore::AddPass(shader, images, 15, 0x7u | (flags & 512u ? 1u << 13 : 0u), &c, sizeof(c), (tw + 7) / 8, (th + 7) / 8)) return;
		if (!RtCore::CopyOut(imageDisplay, displaySurface, tw, th)) return;
		tracedScale[0] = (float)tw / (float)w; tracedScale[1] = (float)th / (float)h;
		albedoThisFrame = false;
		if (flags & 512u) {	// (2026-11-03) the first surface's colour for the show pass (demodulation)
			if (!albedoTexture) TheTextureManager->InitTexture("NVR_RtPathTraceAlbedo", &albedoTexture, &albedoSurface, w, h, D3DFMT_A16B16G16R16F);
			albedoThisFrame = albedoSurface && RtCore::CopyOut(imageAlbedoOut, albedoSurface, tw, th);
		}
		if (!S.denoise) samples += (UInt32)S.samples;	// (2026-11-03: PTDenoise keeps 0, so leaving it starts a fresh mean; rtpt-8c)
		haveImage = true;
		static UInt32 logged = 0;
		if (samples == (UInt32)S.samples && logged < 8) {
			logged++;
			Logger::Log("UNOFFICIAL RT path trace: started (%ux%u, %d bounces, %d paths a pixel and frame, %s, %d lamps, sun %s, albedo %s, glow %s, alpha test %s, "
				"first hit %s, sky %s, %d glow lights).", w, h, S.bounces, S.samples, exterior ? "outdoors: sky on misses" : "indoors", (int)c.misc[0], sunUp ? "up" : "down / indoors",
				flags & 2u ? "atlas" : "averages", flags & 4u ? "atlas" : "none", flags & 1u ? "on" : "off", flags & 8u ? "the screen's G-buffer" : "traced",
				flags & 16u ? "the reflections' panorama" : "gradient", (int)c.rect[2]);
		}
		if (samples == 64u || samples == 256u || samples == 1024u || samples == 4096u)
			Logger::Log("UNOFFICIAL RT path trace: %u samples a pixel (camera still; %u restarts so far).", samples, resets);
	}

	ID3D9NvrExtPipeline* ShowPipeline() {
		if (showTried) return showPipeline;
		showTried = true;
		std::vector<char> spirv;
		ID3D9NvrExt* ext = DxvkCompute::Extension();
		if (!ext || !DxvkCompute::LoadSpirv("RtPathTraceShow", spirv)) return nullptr;
		NVR_EXT_BINDING_DESC bindings[4] = {
			{ 1, 0, NVR_EXT_BIND_SAMPLED_IMAGE, 0, NVR_EXT_VIEW_2D, FALSE, 0 },
			{ 1, 1, NVR_EXT_BIND_SAMPLED_IMAGE, 1, NVR_EXT_VIEW_2D, FALSE, 0 },
			{ 1, 2, NVR_EXT_BIND_SAMPLED_IMAGE, 2, NVR_EXT_VIEW_2D, FALSE, 0 },
			{ 0, 0, NVR_EXT_BIND_SAMPLER, 0, NVR_EXT_VIEW_2D, FALSE, 4 },
		};
		NVR_EXT_GRAPHICS_PIPELINE_DESC desc = {};
		desc.pFragmentSpirv = spirv.data();
		desc.FragmentSpirvSize = (UINT)spirv.size();
		desc.BindingCount = 4;
		desc.pBindings = bindings;
		desc.PushConstantSize = 32;
		desc.UsesSamplerHeap = TRUE;
		desc.SamplerHeapSet = 0;
		desc.SamplerHeapBinding = 0;
		desc.ColorTargetCount = 1;
		desc.Blend = NVR_EXT_BLEND_NONE;
		desc.ColorWriteMask = 0xFu;
		desc.pDebugName = "RtPathTraceShow";
		if (FAILED(ext->CreateGraphicsPipeline(&desc, &showPipeline))) {
			showPipeline = nullptr;
			Logger::Log("UNOFFICIAL RT path trace: the display pass could not be made (Compute\\RtPathTraceShow.spv; the DXVK log says why)");
		}
		return showPipeline;
	}

	// The traced picture (denoised with PTDenoise) into Target: over the finished frame (hdr false: clamped, gamma, PTExposure) or into
	// the HDR scene (hdr true, PTReplace: the stored curve x^(1/2.2), not clamped above 1)
	void Show(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, bool hdr) {
		if (!S.enabled || !haveImage || !displayTexture || !RenderTarget || !RtCore::Running()) return;
		IDirect3DTexture9* shown = displayTexture;
		// UNOFFICIAL 2026-11-03 (rtgi-cal) PTDebugView 6 / 7: RT GI's bounce light ([Shaders.AmbientOcclusion.RT] GI: NVR_RTGIRaw / the shared
		// RT denoiser's), shown through this same pass (x PTExposure, gamma): the same units and encoding as PTDebugView 5, the
		// reference's arriving one-bounce light, so the two compare pixel for pixel. Nothing shown while RT GI is not traced.
		if (S.debugView == 6 || S.debugView == 7) {
			AmbientOcclusionEffect* ao = TheShaderManager->Effects.AmbientOcclusion;
			IDirect3DTexture9* gi = ao ? (S.debugView == 7 ? ao->RtDenoisedGI(Device) : nullptr) : nullptr;
			if (!gi && ao) gi = ao->RtGI();
			if (!gi) return;
			shown = gi;
		}
		else if (S.denoise) {	// (2026-11-03) PTDenoise: the frame's noisy paths through the shared RT denoiser (temporal + a-trous, RGB)
			D3DSURFACE_DESC desc = {};
			displayTexture->GetLevelDesc(0, &desc);
			static UINT dw = 0, dh = 0;
			if (!denoisedTexture || dw != desc.Width || dh != desc.Height) {
				dw = desc.Width; dh = desc.Height;
				DxvkCompute::Drop(denoisedSurface); DxvkCompute::Drop(denoisedTexture);
				Device->CreateTexture(dw, dh, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A16B16G16R16F, D3DPOOL_DEFAULT, &denoisedTexture, nullptr);
				if (denoisedTexture) denoisedTexture->GetSurfaceLevel(0, &denoisedSurface);
			}
			if (!denoiser) denoiser = RtDenoise::Create("RT path trace");
			RtDenoise::Settings settings;
			settings.Channels = 3;
			settings.IgnoreRr = true;
			settings.MaxHistory = 32.0f;
			settings.Iterations = 4;
			if (denoisedSurface && RtDenoise::Run(denoiser, Device, displayTexture, denoisedSurface, settings)) shown = denoisedTexture;
		}
		ID3D9NvrExtPipeline* pipeline = ShowPipeline();
		ID3D9NvrExtSampler* point = DxvkCompute::PointClamp();
		if (!pipeline || !point) return;
		if (!parkSurface) {
			Device->CreateTexture(16, 16, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &parkTexture, nullptr);
			if (parkTexture) parkTexture->GetSurfaceLevel(0, &parkSurface);
			if (!parkSurface) return;
		}
		static GpuTimer timer("  RT path trace show");
		GpuProfileScope gpu(timer, Device);
		IDirect3DSurface9* current = nullptr;
		Device->GetRenderTarget(0, &current);
		Device->SetRenderTarget(0, parkSurface);
		const bool modulate = shown == denoisedTexture && albedoThisFrame && albedoTexture;	// (2026-11-03) the denoised light x the first surface's colour
		struct { float exposure; UInt32 samplerIndex; float split, gamma; UInt32 modulate, pad[3]; } push = { S.exposure, 0, S.split,
			S.debugView == 1 || S.debugView == 2 || S.debugView == 4 ? 1.0f : 1.0f / 2.2f, (modulate ? 1u : 0u) | (hdr ? 2u : 0u), { 0, 0, 0 } };
		// (2026-11-03) the overlay after a RenderScale upscale reads the rect-layout trace at uv x scale (into the scene: texel for texel)
		const float uvScale[2] = { hdr ? 1.0f : tracedScale[0], hdr ? 1.0f : tracedScale[1] };
		memcpy(push.pad, uvScale, sizeof(uvScale));
		NVR_EXT_RESOURCE_BINDING bindings[4] = {};
		bindings[0].Slot = 0; bindings[0].Type = NVR_EXT_BIND_SAMPLED_IMAGE; bindings[0].pResource = shown;
		bindings[1].Slot = 1; bindings[1].Type = NVR_EXT_BIND_SAMPLED_IMAGE; bindings[1].pResource = modulate ? albedoTexture : shown;
		bindings[2].Slot = 2; bindings[2].Type = NVR_EXT_BIND_SAMPLED_IMAGE; bindings[2].pResource = displayTexture;	// (the raw trace: sky / frame pixels)
		bindings[3].Slot = 0; bindings[3].Type = NVR_EXT_BIND_SAMPLER; bindings[3].pResource = point;
		NVR_EXT_DRAW_DESC draw = {};
		draw.pPipeline = pipeline;
		draw.TargetCount = 1;
		draw.Targets[0].pResource = RenderTarget;
		draw.BindingCount = 4;
		draw.pBindings = bindings;
		draw.pPushData = &push;
		draw.PushDataSize = sizeof(push);
		DxvkCompute::Extension()->Draw(&draw);
		Device->SetRenderTarget(0, current);
		if (current) current->Release();
	}
}

namespace RtPathTrace {
	void Register() {
		shader = RtCore::AddShader("RtPathTrace");
		RtCore::AddUser({ "RT reference path tracer", Wants, Setup, Record });
	}

	void RenderDebug(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget) {
		if (S.replace) return;	// (2026-11-03) PTReplace: shown in the scene already (Replace)
		Show(Device, RenderTarget, false);
	}

	// (2026-11-03) PTReplace: the traced picture into the game's scene right after the RT core's point (HDR, the stored curve), so
	// NVR's and the game's later passes (tonemap, bloom, grading) treat it as the frame
	void Replace(IDirect3DDevice9* Device) {
		if (!S.enabled || !S.replace) return;
		IDirect3DSurface9* scene = nullptr;
		if (FAILED(Device->GetRenderTarget(0, &scene)) || !scene) return;
		Show(Device, scene, true);
		scene->Release();
	}

	bool Replacing() { return S.enabled && S.replace && haveImage && RtCore::Running(); }

	std::string StatusText() {
		if (!S.enabled) return "RT reference path tracer off ([Main.Main.RayTracing] ReferencePT false)";
		char t[300];
		sprintf_s(t, "RT reference path tracer on | %s | %ux%u, %u samples a pixel (of PTMaxSamples %d), %u restarts, %d bounces, %d paths a frame, PTDebugView %d",
			RtCore::Running() ? "the core runs" : "the RT core is not running: nothing traced", textureWidth, textureHeight, samples, S.maxSamples, resets,
			S.bounces, S.samples, S.debugView);
		return t;
	}
}
