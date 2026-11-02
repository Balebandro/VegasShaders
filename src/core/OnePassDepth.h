#pragma once
#include <vector>

// UNOFFICIAL 2026-10-26 LampsDepthFirst ([Main.Main.Misc] LampsDepthFirst, live, default off; asked for by the user: "make all lamps
// in one pass actually pay off").
//
// Why one pass was slower than the game's several passes (LampsInOnePass.h: +1.08 ms in the Ultra-Luxe; +0.49 ms after
// DxvkLampBranches): the game draws its opaque world by pass ID, so every object's FIRST pass is drawn before any added pass
// (ENGINE-ANSWERS.md D1: base lit passes 0x64-0x12B, then extra lamps 0x12C-0x149 with z EQUAL). The added lamp passes therefore
// run with the whole opaque depth already in the buffer: the depth test throws away every hidden pixel before it is shaded. The
// one many-lamp pass (OPT passes 0xC3-0xD6, SLS2034-2036) is a first pass: it shades every pixel of the object that is in front of
// what was drawn before it, in the game's order (not front to back), and each of those pixels pays for all its lamps. The lamp
// cost per VISIBLE pixel is about the same in both ways (the branches skip lamps out of reach just as TrimLampPasses does); the
// difference is the pixels that are shaded and then covered by something drawn later.
//
// What this does: the main opaque list's OPT passes are drawn first, in two steps. Step 1 draws each of them with depth only (a
// one-instruction pixel shader, colour writes off): the depth buffer then holds the nearest OPT surface everywhere. Step 2 draws
// the same passes again with the game's own shaders; the game's depth test (GREATEREQUAL / LESSEQUAL) passes only where a pass's
// depth equals what step 1 left, so every pixel is shaded once, by the surface that is seen. The OPT shaders have no clip(), so the
// test happens before the pixel shader (early depth). Then the rest of the main list is drawn as the game does it.
// Exactness: both steps use the same vertex shader with the same constants (the game's own setup of the pass, run twice), so the
// depths are the same bit for bit; the colour comes only from step 2. A pass is left out of step 1 (drawn normally in step 2) when
// it is alpha-tested or blended, skinned (actors: their cloth and bones are set up per draw), not a first pass, or has fewer lamps
// than LampsDepthFirstMinLamps. If any OPT pass in the list is not a first pass, nothing is moved that frame.
// How the passes are moved: GrassFirst's way (Render.cpp): the values of each batch renderer's pass-ID list are rewritten so the
// OPT IDs come first, RenderBatches(0xC3, 0xD6) draws (and uses up) them, the IDs are sorted back. During that call each pass is
// recorded and drawn depth-only; afterwards the records are drawn again through the game's own RenderPassImmediately 0x00B994F0 with
// the cull / alpha-test state of their bucket. The device state seen by the rest of the list is put back as it was (GrassFirst's
// Save / Restore, all parts).
// DXVK only (stock or NVR DXVK; it needs no fork feature): on native D3D9 the depth step's extra draws cost main-thread time in the
// driver and it measured slower, so there the setting does nothing. Meant together with LampsInOnePass (and DxvkLampBranches).
// MEASURED 2026-10-26 (slot C 1080p, GTX 1070, NVR DXVK 1.4, same-launch A/B, world scene GPU ms, medians; lists
// bench\foundations\lists\dxvklamps-*; docs/next/DXVK-FORK-PLAN.md "LampsDepthFirst"): Lucky 38 multi 3.62 / one 4.28 / one + depth
// first 3.51 (x5); casino 2.15 / 2.31 / 1.83; Ultra-Luxe multi 4.35-4.60 / one 4.28-5.50 / one + depth first 4.09-5.47 (4 launches;
// the one-pass cost itself changes from stretch to stretch). Same picture (every pair inside the same-settings noise). The one pass shades 19% fewer samples
// (occlusion counts, Ultra-Luxe: 1.50 -> 1.21 per screen pixel) and the walls and floors drawn first also hide other objects
// (their families drop 0.1-0.2 ms). Native D3D9: world +0.7 (Lucky 38) / +1.2 ms (Ultra-Luxe), frame +0.9 / +1.6 ms.
// Test aid ([Main.Main.Misc] LampsDepthFirstCount = 1, development builds, F10 only): each OPT draw of the world scene is wrapped
// in an occlusion query; 'LAMPS DEPTH FIRST' lines give the samples the OPT passes shade per frame (with step 1: about the visible
// samples; without: visible + hidden-later), i.e. the overdraw of the one many-lamp pass.
namespace OnePassDepth {

	inline int Mode = 0;			// [Main.Main.Misc] LampsDepthFirst, read each frame
	inline int MinLamps = 0;		// [Main.Main.Misc] LampsDepthFirstMinLamps: OPT passes with fewer lamps get no depth step
	inline int Count = 0;			// [Main.Main.Misc] LampsDepthFirstCount (test aid, F10 only)
	inline bool HookSeen = false;	// the per-object pass hooks (RenderPassStandardHook) are in: without them step 1 could not draw depth only
	inline bool Recording = false;	// inside step 1 (the early RenderBatches over the OPT IDs)
	inline bool InDepth = false;	// a depth-only draw of step 1 is being set up / drawn (the pass hooks take the short way)
	inline bool DepthDrawn = false;	// the pass hook drew the current step-1 pass depth-only
	inline bool Swapped = false;	// the depth-only pixel shader is on the device
	inline DWORD SavedWrite[4] = {};
	inline IDirect3DPixelShader9* DepthShader = nullptr;
	inline bool ShaderFailed = false;
	inline bool LoggedOn = false;

	struct Record { void* pass; UInt32 passId, a3, a4, a5, cull, alphaTest; bool depth; };
	inline std::vector<Record> Records;

	inline bool IsOpt(UInt32 id) { return id >= 0xC3 && id <= 0xD6; }

	// statistics (F10 only), one line every 240 frames or when the switch changes
	inline unsigned StatFrames = 0;
	inline int StatMode = -1;
	inline double StatPasses = 0, StatDepth = 0, StatSkinned = 0, StatAlpha = 0, StatFewLamps = 0, StatBailNotFirst = 0, StatLists = 0, StatCpuMs = 0;

	inline bool MakeShader(IDirect3DDevice9* device) {
		if (DepthShader) return true;
		if (ShaderFailed) return false;
		// alpha 0: should the game turn the alpha test on for a pass after all, the test removes the pixel (no depth written)
		static const char* src = "float4 main() : COLOR0 { return float4(0.0, 0.0, 0.0, 0.0); }";
		ID3DXBuffer* code = nullptr; ID3DXBuffer* errors = nullptr;
		if (FAILED(D3DXCompileShader(src, (UINT)strlen(src), NULL, NULL, "main", "ps_3_0", 0, &code, &errors, NULL)) || !code ||
			FAILED(device->CreatePixelShader((const DWORD*)code->GetBufferPointer(), &DepthShader))) {
			Logger::Log("UNOFFICIAL LampsDepthFirst: the depth-only shader failed (%s); off", errors ? (const char*)errors->GetBufferPointer() : "?");
			if (errors) errors->Release();
			if (code) code->Release();
			DepthShader = nullptr;
			ShaderFailed = true;
			return false;
		}
		code->Release();
		return true;
	}

	// ---- test aid: occlusion counts of the OPT draws (F10 + LampsDepthFirstCount) ----
	namespace Counts {
		enum { kRing = 4, kMax = 2048 };
		inline std::vector<IDirect3DQuery9*> Pool[kRing];
		inline unsigned Used[kRing] = {};
		inline bool Issued[kRing] = {};
		inline unsigned Slot = 0;
		inline IDirect3DQuery9* Open = nullptr;
		inline double Samples = 0, Draws = 0, Frames = 0, NotReady = 0;

		// Once a frame: read the ring slot about to be reused (issued kRing frames ago), without flushing.
		inline void BeginFrame(IDirect3DDevice9* device, bool on) {
			Slot = (Slot + 1) % kRing;
			if (Issued[Slot]) {
				unsigned long long sum = 0;
				bool ready = true;
				for (unsigned i = 0; i < Used[Slot]; i++) {
					DWORD n = 0;
					if (Pool[Slot][i]->GetData(&n, sizeof(n), 0) != S_OK) { ready = false; break; }
					sum += n;
				}
				if (ready) { Samples += (double)sum; Draws += Used[Slot]; Frames += 1; }
				else NotReady += 1;
			}
			Issued[Slot] = false;
			Used[Slot] = 0;
			if (!on) return;
			Issued[Slot] = true;
		}
		inline void Before(IDirect3DDevice9* device) {
			Open = nullptr;
			if (!Issued[Slot] || Used[Slot] >= kMax) return;
			std::vector<IDirect3DQuery9*>& pool = Pool[Slot];
			if (Used[Slot] == pool.size()) {
				IDirect3DQuery9* q = nullptr;
				if (FAILED(device->CreateQuery(D3DQUERYTYPE_OCCLUSION, &q)) || !q) return;
				pool.push_back(q);
			}
			Open = pool[Used[Slot]];
			if (FAILED(Open->Issue(D3DISSUE_BEGIN))) { Open = nullptr; return; }
		}
		inline void After() {
			if (!Open) return;
			Open->Issue(D3DISSUE_END);
			Open = nullptr;
			Used[Slot]++;
		}
	}

	inline void Report(int mode) {
		const double f = StatFrames;
		if (f < 30) return;
		const double screen = (double)TheRenderManager->width * TheRenderManager->height;
		Logger::Log("LAMPS DEPTH FIRST %s (min lamps %d), %.0f frames, per frame: lists moved %.2f, OPT passes %.1f, depth-first %.1f, left out: skinned %.1f, alpha %.1f, few lamps %.1f; frames not moved (an OPT pass not a first pass) %.2f; CPU of the move + both steps %.3f ms",
			mode ? "on" : "off", MinLamps, f, StatLists / f, StatPasses / f, StatDepth / f, StatSkinned / f, StatAlpha / f, StatFewLamps / f, StatBailNotFirst / f, StatCpuMs / f);
		if (Counts::Frames > 0)
			Logger::Log("LAMPS DEPTH FIRST counts (%s): OPT draws %.1f a frame shade %.0f samples a frame (%.3f per screen pixel; with MSAA, samples per pixel count); %.0f frames read, %.0f not ready",
				mode ? "on" : "off", Counts::Draws / Counts::Frames, Counts::Samples / Counts::Frames, Counts::Samples / Counts::Frames / screen, Counts::Frames, Counts::NotReady);
		Counts::Samples = Counts::Draws = Counts::Frames = Counts::NotReady = 0;
		StatFrames = 0;
		StatPasses = StatDepth = StatSkinned = StatAlpha = StatFewLamps = StatBailNotFirst = StatLists = StatCpuMs = 0;
	}

	inline int OnDxvk = -1;			// the runtime is DXVK (stock or NVR's): -1 not known yet
	inline bool LoggedNative = false;

	inline void BeginFrame(IDirect3DDevice9* device) {
		if (OnDxvk < 0 && TheRenderManager) OnDxvk = !strncmp(TheRenderManager->D3D9RuntimeDescription(), "DXVK", 4) ? 1 : 0;
		const int want = TheSettingManager->GetSettingI("Main.Main.Misc", "LampsDepthFirst");
		// DXVK only: on native D3D9 the depth step's extra draws cost main-thread time in the driver (measured 2026-10-26, slot C:
		// world +0.7 to +1.2 ms, frame +0.9 to +1.6 ms); DXVK does that work on its own thread.
		Mode = OnDxvk == 1 ? want : 0;
		if (want && OnDxvk == 0 && !LoggedNative) {
			LoggedNative = true;
			Logger::Log("UNOFFICIAL LampsDepthFirst: does nothing on native D3D9 (measured slower there: the extra draws cost the driver's main-thread time); needs DXVK (stock or NVR DXVK)");
		}
		MinLamps = TheSettingManager->GetSettingI("Main.Main.Misc", "LampsDepthFirstMinLamps");
#if NVR_TEST_TOOLS
		Count = TheSettingManager->GetSettingI("Main.Main.Misc", "LampsDepthFirstCount");
#else
		Count = 0;
#endif
		if (Mode && !LoggedOn) {
			LoggedOn = true;
			Logger::Log("UNOFFICIAL LampsDepthFirst: on (the one many-lamp passes drawn depth first, then shaded once per visible pixel)%s",
				HookSeen ? "" : "; waiting for the per-object pass hooks");
		}
		Recording = InDepth = DepthDrawn = false;
		const bool counting = Count != 0 && GpuTimer::Enabled;
		Counts::BeginFrame(device, counting);
		if (!GpuTimer::Enabled) { StatFrames = 0; return; }
		if (StatMode != Mode || StatFrames >= 240) { Report(StatMode); StatMode = Mode; StatFrames = 0; }
		StatFrames++;
	}

	// The pass hooks, step 1: the depth-only pixel shader and no colour writes for this draw (device only; After puts back what
	// the game's state cache holds). SetShadersHook calls Reapply if the game binds inside the draw.
	inline void BeginDepth(IDirect3DDevice9* device) {
		Swapped = false;
		if (!MakeShader(device)) return;
		for (int i = 0; i < 4; i++) device->GetRenderState((D3DRENDERSTATETYPE)(i ? D3DRS_COLORWRITEENABLE1 + i - 1 : D3DRS_COLORWRITEENABLE), &SavedWrite[i]);
		for (int i = 0; i < 4; i++) device->SetRenderState((D3DRENDERSTATETYPE)(i ? D3DRS_COLORWRITEENABLE1 + i - 1 : D3DRS_COLORWRITEENABLE), 0);
		device->SetPixelShader(DepthShader);
		Swapped = true;
	}
	inline void Reapply(IDirect3DDevice9* device) {
		if (Swapped && InDepth) device->SetPixelShader(DepthShader);
	}
	inline void EndDepth(IDirect3DDevice9* device) {
		if (!Swapped) return;
		Swapped = false;
		device->SetPixelShader(TheRenderManager->renderState->GetPixelShader());
		for (int i = 0; i < 4; i++) device->SetRenderState((D3DRENDERSTATETYPE)(i ? D3DRS_COLORWRITEENABLE1 + i - 1 : D3DRS_COLORWRITEENABLE), SavedWrite[i]);
	}

	// RenderPassImmediatelyHook during step 1: may this pass be drawn depth-only? (counted for the statistics)
	inline bool Eligible(const void* passPtr, DWORD alphaTest, DWORD alphaBlend) {
		const UInt8* pass = (const UInt8*)passPtr;
		const NiGeometry* geometry = *(NiGeometry* const*)pass;
		const bool stats = GpuTimer::Enabled;
		if (stats) StatPasses += 1;
		const NiAlphaProperty* alpha = geometry ? geometry->propertyState.m_spAlphaProperty : nullptr;
		if (alphaTest || alphaBlend || (alpha && (alpha->flags & (1 | 0x200)))) { if (stats) StatAlpha += 1; return false; }
		if ((int)pass[9] < MinLamps) { if (stats) StatFewLamps += 1; return false; }
		return true;
	}
}
