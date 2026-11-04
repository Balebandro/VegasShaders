// Checks the flash rule of the automatic flash / flicker detector (src/core/FlashRule.h) on synthetic 8 x 8 sequences: a dropout, a
// brighter flash and black / white frames fire; a camera pan, a fade, a cut, a muzzle-flash-sized blob and sensor-like noise do not.
#include <cstdio>
#include <cmath>
#include <cstdint>
#include "../src/core/FlashRule.h"

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { std::printf("FAIL: "); std::printf(__VA_ARGS__); std::printf("\n"); failures++; } else { std::printf("PASS: "); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

using namespace FlashRule;

// A smooth scene: a sky gradient over a darker ground with soft blobs; x0 shifts it (in cells, fractions allowed) for pans.
static void Scene(float* out, float x0 = 0.0f, float gain = 1.0f) {
	for (int y = 0; y < Side; y++)
		for (int x = 0; x < Side; x++) {
			const float u = (float)x + x0;
			float l = y < 3 ? 0.75f - 0.05f * y : 0.30f + 0.04f * (y - 3);
			l += 0.12f * sinf(u * 0.9f) * cosf(y * 0.7f) + 0.06f * sinf(u * 2.1f + 1.3f);
			out[y * Side + x] = fmaxf(0.0f, fminf(1.0f, l * gain));
		}
}

static uint32_t rng = 12345u;
static float Noise() { rng = rng * 1664525u + 1013904223u; return ((rng >> 8) / 16777216.0f - 0.5f); }

int main() {
	float a[Cells], b[Cells], c[Cells];

	// 1. A dropout: the lower half (32 cells) 20 % darker for one frame.
	Scene(a); Scene(b); Scene(c);
	for (int i = Cells / 2; i < Cells; i++) b[i] *= 0.80f;
	{ Verdict v = Judge(a, b, c); CHECK(v.flash && v.sign < 0 && v.darker >= 16, "a one-frame 20%% dropout of the lower half fires darker (%d cells)", v.darker); }

	// 2. The whole picture 15 % darker for one frame.
	Scene(a); Scene(b); Scene(c);
	for (int i = 0; i < Cells; i++) b[i] *= 0.85f;
	{ Verdict v = Judge(a, b, c); CHECK(v.flash && v.sign < 0, "a whole-picture 15%% dip fires (whole %d, %d cells)", (int)v.whole, v.darker); }

	// 3. A black frame and a white frame.
	Scene(a); Scene(c);
	for (int i = 0; i < Cells; i++) b[i] = 0.0f;
	{ Verdict v = Judge(a, b, c); CHECK(v.flash && v.black, "a black frame between two normal ones fires and is labelled black"); }
	for (int i = 0; i < Cells; i++) b[i] = 1.0f;
	{ Verdict v = Judge(a, b, c); CHECK(v.flash && v.white && v.sign > 0, "a white frame fires and is labelled white"); }

	// 4. A brighter flash over half the picture.
	Scene(a); Scene(b); Scene(c);
	for (int i = 0; i < Cells / 2; i++) b[i] = fminf(1.0f, b[i] * 1.3f + 0.05f);
	{ Verdict v = Judge(a, b, c); CHECK(v.flash && v.sign > 0, "a brighter flash over half the picture fires (%d cells)", v.brighter); }

	// 5. A camera pan: the scene moves 0.4, 0.8, 1.6 cells a frame (a fast turn), no flash.
	const float speeds[] = { 0.4f, 0.8f, 1.6f };
	for (float speed : speeds) {
		bool fired = false;
		for (int f = 1; f < 40; f++) {
			Scene(a, (f - 1) * speed); Scene(b, f * speed); Scene(c, (f + 1) * speed);
			if (Judge(a, b, c).flash) fired = true;
		}
		CHECK(!fired, "a camera pan at %.1f cells a frame never fires", speed);
	}

	// 6. A fade to black over 10 frames and a sunrise (brightening), no flash.
	{
		bool fired = false;
		for (int f = 1; f < 10; f++) {
			Scene(a, 0, 1.0f - 0.1f * (f - 1)); Scene(b, 0, 1.0f - 0.1f * f); Scene(c, 0, 1.0f - 0.1f * (f + 1));
			if (Judge(a, b, c).flash) fired = true;
			Scene(a, 0, 0.3f + 0.1f * (f - 1)); Scene(b, 0, 0.3f + 0.1f * f); Scene(c, 0, 0.3f + 0.1f * (f + 1));
			if (Judge(a, b, c).flash) fired = true;
		}
		CHECK(!fired, "a fade out and a brightening never fire");
	}

	// 7. A cut (another place from frame t on) and a cut back after one frame of loading black (that one IS a black frame).
	Scene(a); Scene(b, 3.0f, 0.5f); Scene(c, 3.0f, 0.5f);
	{ Verdict v = Judge(a, b, c); CHECK(!v.flash, "a camera cut to another scene does not fire"); }
	Scene(a); Scene(b); Scene(c, 3.0f, 0.5f);
	{ Verdict v = Judge(a, b, c); CHECK(!v.flash, "the frame before a cut does not fire"); }

	// 8. A muzzle flash: two cells much brighter for one frame.
	Scene(a); Scene(b); Scene(c);
	b[5 * Side + 3] = 1.0f; b[5 * Side + 4] = 1.0f;
	{ Verdict v = Judge(a, b, c); CHECK(!v.flash, "a two-cell muzzle flash does not fire"); }

	// 9. Noise of +-1.5 % on every cell (grain, dithers), 200 frames.
	{
		bool fired = false;
		for (int f = 0; f < 200; f++) {
			Scene(a); Scene(b); Scene(c);
			for (int i = 0; i < Cells; i++) { a[i] += 0.03f * Noise(); b[i] += 0.03f * Noise(); c[i] += 0.03f * Noise(); }
			if (Judge(a, b, c).flash) fired = true;
		}
		CHECK(!fired, "per-cell noise of +-1.5%% never fires");
	}

	// 10. A small dip (5 %) over the whole picture stays under the thresholds (the conservative side).
	Scene(a); Scene(b); Scene(c);
	for (int i = 0; i < Cells; i++) b[i] *= 0.95f;
	{ Verdict v = Judge(a, b, c); CHECK(!v.flash, "a 5%% dip does not fire"); }

	// 11. A two-frame dropout: frames t and t+1 dark. Frame t compares with t+1 (also dark): no flash on t, none on t+1 either.
	Scene(a); Scene(b, 0, 0.7f); Scene(c, 0, 0.7f);
	{ Verdict v = Judge(a, b, c); CHECK(!v.flash, "the first frame of a two-frame dip does not fire (a step at that frame)"); }

	// 12. The mask text.
	{
		char text[80];
		MaskText(0xFFull, text);
		CHECK(text[0] == 'X' && text[7] == 'X' && text[8] == '/' && text[9] == '.', "the mask text marks the top row (%s)", text);
	}

	// --- stage attribution (2026-11-01, Attribute / RelativeJump) ---
	// A chain of four stages: scene, Fog, VolumetricLight, Bloom. The pictures after each stage of frames t-1, t, t+1.
	{
		float scene[3][Cells], fog[3][Cells], vl[3][Cells], bloom[3][Cells];
		for (int j = 0; j < 3; j++) {
			Scene(scene[j]);
			for (int i = 0; i < Cells; i++) {
				fog[j][i] = scene[j][i] * 0.9f + 0.05f;			// fog: steady
				vl[j][i] = fog[j][i] + (i < Cells / 2 ? 0.10f : 0.0f);	// volumetric light brightens the upper half
				bloom[j][i] = fminf(1.0f, vl[j][i] * 1.1f);		// bloom: a gain
			}
		}
		// 13. The volumetric light drops out of the upper half in frame t: the final flashes darker there; the source is VolumetricLight,
		//     Bloom (a gain on it) amplifies a little, Fog and the scene stay at 0.
		for (int i = 0; i < Cells / 2; i++) { vl[1][i] = fog[1][i]; bloom[1][i] = fminf(1.0f, vl[1][i] * 1.1f); }
		Verdict v = Judge(bloom[0], bloom[1], bloom[2]);
		CHECK(v.flash && v.sign < 0, "the final picture flashes darker when the volumetric light drops out (%d cells)", v.darker);
		const StageInput in[4] = { { scene[0], scene[1], scene[2] }, { fog[0], fog[1], fog[2] }, { vl[0], vl[1], vl[2] }, { bloom[0], bloom[1], bloom[2] } };
		const float finalRel = RelativeJump(bloom[0], bloom[1], bloom[2], v.mask, v.sign);
		Attribution r = Attribute(in, 4, v.mask, v.sign, finalRel);
		CHECK(r.source == 2, "the source is VolumetricLight (stage 2), got %d (own jumps: %.3f %.3f %.3f %.3f, final %.3f)", r.source, r.stage[0].own, r.stage[1].own,
			r.stage[2].own, r.stage[3].own, finalRel);
		CHECK(r.stage[0].own < 0.001f && r.stage[1].own < 0.001f, "the scene and the fog have no own jump (%.4f, %.4f)", r.stage[0].own, r.stage[1].own);
		CHECK(!r.amplified[0] && !r.amplified[1] && !r.damped[2], "nothing before the source amplified it");
		CHECK(r.stage[3].own < 0.3f * r.stage[2].own, "a 10 %% gain after the source adds less than the source itself (%.3f vs %.3f)", r.stage[3].own, r.stage[2].own);

		// 14. The same drop, but a TAA-like stage after Bloom halves every change: it is reported as damping.
		float taa[3][Cells];
		for (int j = 0; j < 3; j++) for (int i = 0; i < Cells; i++) taa[j][i] = 0.5f * bloom[j][i] + 0.5f * bloom[0][i];
		const StageInput in5[5] = { in[0], in[1], in[2], in[3], { taa[0], taa[1], taa[2] } };
		r = Attribute(in5, 5, v.mask, v.sign, RelativeJump(taa[0], taa[1], taa[2], v.mask, v.sign));
		CHECK(r.source == 2 && r.damped[4], "with a history blend after it the source is still VolumetricLight and the blend damped it (source %d, damped %d, own %.3f)",
			r.source, (int)r.damped[4], r.stage[4].own);

		// 15. The flash is in the game's own frame: the scene itself dips 20 %, every NVR stage carries it. The source is stage 0.
		for (int j = 0; j < 3; j++) {
			Scene(scene[j], 0.0f, j == 1 ? 0.8f : 1.0f);
			for (int i = 0; i < Cells; i++) { fog[j][i] = scene[j][i] * 0.9f + 0.05f; vl[j][i] = fog[j][i] + (i < Cells / 2 ? 0.10f : 0.0f); bloom[j][i] = fminf(1.0f, vl[j][i] * 1.1f); }
		}
		v = Judge(bloom[0], bloom[1], bloom[2]);
		r = Attribute(in, 4, v.mask, v.sign, RelativeJump(bloom[0], bloom[1], bloom[2], v.mask, v.sign));
		CHECK(v.flash && r.source == 0, "a dip in the game's own frame is attributed to the scene before Vegas Shaders (source %d)", r.source);

		// 16. A stage missing from one frame (a late read-back) is skipped and the next stage is measured against the last probed one.
		const StageInput gap[4] = { in[0], { fog[0], nullptr, fog[2] }, in[2], in[3] };
		r = Attribute(gap, 4, v.mask, v.sign, RelativeJump(bloom[0], bloom[1], bloom[2], v.mask, v.sign));
		CHECK(!r.stage[1].valid && r.stage[2].valid && r.source == 0, "a stage missing in one frame is skipped (source %d)", r.source);

		// 17. No stage jumps (the flash is after NVR's effects): no source, the largest named.
		for (int j = 0; j < 3; j++) { Scene(scene[j]); for (int i = 0; i < Cells; i++) { fog[j][i] = scene[j][i]; vl[j][i] = fog[j][i]; bloom[j][i] = vl[j][i]; } }
		r = Attribute(in, 4, v.mask, v.sign, 0.2f);
		CHECK(r.source < 0, "steady stages under a flashing final give no source (%d)", r.source);
	}

	// ---- the beat rule (BeatRule::Find, the report burst's "REPORT n VL pattern" line) ----
	// A burst: 16 frames from f1000, the first without a value (the report key is read after its passes), as the burst logs it.
	// (noise x Noise() is uniform over a range of `noise`: its standard deviation is 0.29 x noise.)
	{
		const int count = 16;
		unsigned frames[count];
		bool valid[count];
		float v[count];
		for (int i = 0; i < count; i++) { frames[i] = 1000u + i; valid[i] = i > 0; }
		auto fill = [&](float base, float noise, auto shape) { for (int i = 0; i < count; i++) v[i] = base + shape(i, frames[i]) + noise * Noise(); };

		// 18. A one-in-three dropout (the Strip's symptom: the darkening drops every third frame), 100 x its noise.
		fill(0.05f, 0.001f, [](int, unsigned f) { return f % 3 == 1 ? -0.03f : 0.0f; });
		{ BeatRule::Result r = BeatRule::Find(v, valid, frames, count);
		  CHECK(r.beat && r.period == 3 && r.phase == 1 && r.odd < r.rest - 0.02, "a one-in-three dropout is a period-3 beat on its phase (p%d phase %d, score %.1f, %.4f vs %.4f)", r.period, r.phase, r.score, r.odd, r.rest); }
		// the same with no noise at all (exact half floats)
		fill(0.05f, 0.0f, [](int, unsigned f) { return f % 3 == 2 ? 0.02f : 0.0f; });
		{ BeatRule::Result r = BeatRule::Find(v, valid, frames, count);
		  CHECK(r.beat && r.period == 3 && r.phase == 2, "an exact one-in-three rise is a period-3 beat (p%d phase %d)", r.period, r.phase); }

		// 19. Every other frame (period 2).
		fill(0.05f, 0.0005f, [](int, unsigned f) { return f % 2 ? 0.004f : 0.0f; });
		{ BeatRule::Result r = BeatRule::Find(v, valid, frames, count);
		  CHECK(r.beat && r.period == 2, "an every-other-frame alternation is a period-2 beat (p%d, score %.1f)", r.period, r.score); }

		// 20. A beat with a gap (a frame that did not reach the final picture) and the last value missing: still found.
		{
			unsigned gapped[count];
			for (int i = 0; i < count; i++) gapped[i] = frames[i] + (i >= 9 ? 1u : 0u);
			for (int i = 0; i < count; i++) v[i] = 0.05f + (gapped[i] % 3 == 0 ? -0.02f : 0.0f) + 0.001f * Noise();
			bool partial[count];
			for (int i = 0; i < count; i++) partial[i] = valid[i] && i != count - 1;
			BeatRule::Result r = BeatRule::Find(v, partial, gapped, count);
			CHECK(r.beat && r.period == 3 && r.phase == 0, "a period-3 beat across a dropped frame and a missing value is found (p%d phase %d, %d used)", r.period, r.phase, r.used);
		}

		// 21. Not beats: noise alone, a straight drift (a pan, a fade), a steady curve, a constant, one odd frame (symptom B).
		int noiseBeats = 0, driftBeats = 0, curveBeats = 0, spikeBeats = 0, found = 0;
		for (int run = 0; run < 400; run++) {
			fill(0.05f, 0.002f, [](int, unsigned) { return 0.0f; });
			if (BeatRule::Find(v, valid, frames, count).beat) noiseBeats++;
			fill(0.05f, 0.001f, [](int i, unsigned) { return 0.002f * i; });
			if (BeatRule::Find(v, valid, frames, count).beat) driftBeats++;
			fill(0.05f, 0.001f, [](int i, unsigned) { return 0.0004f * (i - 8) * (i - 8); });
			if (BeatRule::Find(v, valid, frames, count).beat) curveBeats++;
			fill(0.05f, 0.001f, [](int i, unsigned) { return i == 7 ? -0.03f : 0.0f; });
			if (BeatRule::Find(v, valid, frames, count).beat) spikeBeats++;
			fill(0.05f, 0.001f, [](int, unsigned f) { return f % 3 == 1 ? -0.003f : 0.0f; });	// a beat of 10 x the noise
			if (BeatRule::Find(v, valid, frames, count).beat) found++;
		}
		CHECK(noiseBeats <= 8, "noise alone is a beat in at most 2%% of 400 bursts (%d)", noiseBeats);
		CHECK(driftBeats <= 8, "a straight drift is a beat in at most 2%% of 400 bursts (%d)", driftBeats);
		CHECK(curveBeats <= 8, "a steady curve is a beat in at most 2%% of 400 bursts (%d)", curveBeats);
		CHECK(spikeBeats == 0, "one odd frame is never a beat in 400 bursts (%d)", spikeBeats);
		CHECK(found >= 392, "a one-in-three beat of 10 x its noise is found in at least 98%% of 400 bursts (%d)", found);
		fill(0.05f, 0.0f, [](int, unsigned) { return 0.0f; });
		{ BeatRule::Result r = BeatRule::Find(v, valid, frames, count); CHECK(!r.beat, "a constant is no beat (score %.1f)", r.score); }

		// 22. Too few values: no verdict.
		bool few[count] = {};
		few[3] = few[4] = few[5] = true;
		{ BeatRule::Result r = BeatRule::Find(v, few, frames, count); CHECK(!r.beat && r.period == 0, "three values give no verdict (%d used)", r.used); }
	}

	// 23. The tonemap mark re-bases the chain: an HDR dropout (VL) followed by a tonemap that compresses every change by half must still
	//     name VL, and the tonemap stage is neither the source nor "damping"; a post stage after the tonemap is measured against it.
	{
		float scene[3][Cells], vl[3][Cells], tm[3][Cells], post[3][Cells];
		for (int j = 0; j < 3; j++) {
			Scene(scene[j]);
			for (int i = 0; i < Cells; i++) {
				vl[j][i] = scene[j][i] + (j == 1 ? 0.0f : 0.20f);	// VL adds 0.2 everywhere except in frame t (a dropout)
				tm[j][i] = 0.5f * vl[j][i];						// the game's tonemap: half
				post[j][i] = tm[j][i] + 0.02f;						// a post effect: steady
			}
		}
		Verdict v = Judge(post[0], post[1], post[2]);
		StageInput in[4] = { { scene[0], scene[1], scene[2] }, { vl[0], vl[1], vl[2] }, { tm[0], tm[1], tm[2], true }, { post[0], post[1], post[2] } };
		Attribution r = Attribute(in, 4, v.mask, v.sign, RelativeJump(post[0], post[1], post[2], v.mask, v.sign));
		CHECK(v.flash && r.source == 1 && !r.damped[2] && !r.amplified[2] && r.stage[2].own == 0.0f && !r.damped[3] && !r.amplified[3],
			"a dropout before the tonemap names the HDR stage, the tonemap mark is neither damping nor amplifying (source %d, tm own %.3f, post own %.3f)",
			r.source, r.stage[2].own, r.stage[3].own);
	}

	// --- step 2 (2026-11-01): the region text, the beat detector on the whole picture, the stages' phase amplitude ---
	{
		char region[48];
		RegionText(0x00000000FFFFFFFFull, region); CHECK(!strcmp(region, "the upper half"), "the top four rows are 'the upper half' (%s)", region);
		RegionText(0xFFFFFFFF00000000ull, region); CHECK(!strcmp(region, "the lower half"), "the bottom four rows are 'the lower half' (%s)", region);
		RegionText(0x0F0F0F0F0F0F0F0Full, region); CHECK(!strcmp(region, "the left half"), "the left four columns are 'the left half' (%s)", region);
		RegionText(0x000000000000000Full, region); CHECK(!strcmp(region, "the upper left"), "four cells top left are 'the upper left' (%s)", region);
		RegionText(~0ull, region); CHECK(!strcmp(region, "the whole picture"), "all 64 cells are 'the whole picture' (%s)", region);
		uint64_t centre = 0; for (int y = 2; y < 6; y++) for (int x = 2; x < 6; x++) centre |= 1ull << (y * Side + x);
		RegionText(centre, region); CHECK(!strcmp(region, "the centre"), "the inner 4 x 4 is 'the centre' (%s)", region);
	}
	{
		// 24. The Strip beat: 48 still frames, the upper half 0.035 darker every third frame (frame % 3 == 1), noise +-0.004 everywhere.
		static float pics[BeatRule::Window][Cells];
		const float* lumas[BeatRule::Window]; bool valid[BeatRule::Window]; unsigned frames[BeatRule::Window];
		auto make = [&](float amplitude, int period, int phase, int half, float noise, float ramp) {
			for (int i = 0; i < BeatRule::Window; i++) {
				const unsigned f = 1000 + i;
				Scene(pics[i]);
				for (int c = 0; c < Cells; c++) {
					pics[i][c] += noise * 2.0f * Noise() + ramp * i;
					const bool in = half == 0 || (half == 1 && c < Cells / 2) || (half == 2 && c >= Cells / 2);
					if (period && in && (int)(f % period) == phase) pics[i][c] -= amplitude;
				}
				lumas[i] = pics[i]; valid[i] = true; frames[i] = f;
			}
		};
		make(0.035f, 3, 1, 1, 0.004f, 0.0f);
		BeatRule::CellsResult b = BeatRule::JudgeCells(lumas, valid, frames, BeatRule::Window);
		CHECK(b.beat && b.period == 3 && b.phase == 1 && b.cells >= 24 && b.amplitude < -0.02 && (b.mask & 0xFFFFFFFF00000000ull) == 0,
			"a 0.035 dip of the upper half every third frame is a BEAT (period %d phase %d, %d cells, amplitude %.3f)", b.period, b.phase, b.cells, b.amplitude);
		// 25. The same beat is under the one-frame rule: no FLASH on any frame.
		bool flashed = false;
		for (int i = 1; i + 1 < BeatRule::Window; i++) if (Judge(pics[i - 1], pics[i], pics[i + 1]).flash) flashed = true;
		CHECK(!flashed, "the 0.035 beat never fires the one-frame rule (that is what the beat detector is for)");
		// 26. Noise alone, and a slow brightening, give no beat.
		make(0.0f, 0, 0, 0, 0.004f, 0.0f);
		b = BeatRule::JudgeCells(lumas, valid, frames, BeatRule::Window);
		CHECK(!b.beat, "noise alone gives no beat (%d cells)", b.cells);
		make(0.0f, 0, 0, 0, 0.004f, 0.002f);
		b = BeatRule::JudgeCells(lumas, valid, frames, BeatRule::Window);
		CHECK(!b.beat, "a steady brightening gives no beat (%d cells)", b.cells);
		// 27. A 2-level pulse (0.008) every other frame, whole picture: at the amplitude floor, still a beat with low noise.
		make(0.010f, 2, 0, 0, 0.001f, 0.0f);
		b = BeatRule::JudgeCells(lumas, valid, frames, BeatRule::Window);
		CHECK(b.beat && b.period == 2 && b.cells >= 48, "a 0.010 pulse every other frame over the whole picture is a BEAT (period %d, %d cells)", b.period, b.cells);
		// 28. Phase amplitude: a series 10 % lower on the odd phase is -10 %; the stage before it (steady) is 0.
		float series[BeatRule::Window], steady[BeatRule::Window];
		for (int i = 0; i < BeatRule::Window; i++) { series[i] = (frames[i] % 3 == 1) ? 0.45f : 0.50f; steady[i] = 0.5f; }
		const float pa = BeatRule::PhaseAmplitude(series, valid, frames, BeatRule::Window, 3, 1), ps = BeatRule::PhaseAmplitude(steady, valid, frames, BeatRule::Window, 3, 1);
		CHECK(fabsf(pa + 0.10f) < 0.002f && fabsf(ps) < 1e-6f, "the phase amplitude of a 10 %% odd-phase dip is -10 %% (%.3f), of a steady series 0 (%.4f)", pa, ps);
	}

	// --- UNOFFICIAL 2026-11-01 the cycle rule (the history watch) ---
	{
		const int N = CycleRule::Window + 10;
		uint64_t hashes[N];
		bool valid[N];
		float pic[5][Cells];
		for (int k = 0; k < 5; k++) Scene(pic[k], 0.3f * k, 1.0f + 0.1f * k);
		for (int i = 0; i < N; i++) valid[i] = true;
		// 29. One converged picture every frame: no cycle (period 1 is normal).
		for (int i = 0; i < N; i++) hashes[i] = CycleRule::Hash(pic[0]);
		CycleRule::Result c = CycleRule::Period(hashes, valid, N);
		CHECK(c.period == 0 && c.distinct == 1, "a still converged picture is no cycle (period %d, %d distinct)", c.period, c.distinct);
		// 30. The 2026-11-01 smear: 5 pictures in a fixed 16-frame order.
		const int order[16] = { 0, 1, 0, 2, 3, 3, 4, 3, 1, 0, 2, 3, 2, 4, 3, 4 };
		for (int i = 0; i < N; i++) hashes[i] = CycleRule::Hash(pic[order[i % 16]]);
		c = CycleRule::Period(hashes, valid, N);
		CHECK(c.period == 16 && c.distinct == 5, "5 pictures in a fixed 16-frame order are a cycle of period 16 (period %d, %d distinct)", c.period, c.distinct);
		// 31. The same with one frame that differs: no exact cycle.
		hashes[N - 7] ^= 1;
		c = CycleRule::Period(hashes, valid, N);
		CHECK(c.period == 0, "one differing frame breaks the cycle (period %d)", c.period);
		// 32. A gap in the window: not judged.
		for (int i = 0; i < N; i++) hashes[i] = CycleRule::Hash(pic[order[i % 16]]);
		valid[N - 20] = false;
		c = CycleRule::Period(hashes, valid, N);
		CHECK(c.period == 0, "a missing frame inside the window gives no verdict (period %d)", c.period);
		valid[N - 20] = true;
		// 33. Every other frame: period 2.
		for (int i = 0; i < N; i++) hashes[i] = CycleRule::Hash(pic[i & 1]);
		c = CycleRule::Period(hashes, valid, N);
		CHECK(c.period == 2 && c.distinct == 2, "an a b a b alternation is period 2 (period %d)", c.period);
		// 34. A live picture (noise every frame): no cycle.
		for (int i = 0; i < N; i++) { float p[Cells]; Scene(p); for (int k = 0; k < Cells; k++) p[k] += 0.02f * Noise(); hashes[i] = CycleRule::Hash(p); }
		c = CycleRule::Period(hashes, valid, N);
		CHECK(c.period == 0 && c.distinct > 40, "a live noisy picture is no cycle (period %d, %d distinct)", c.period, c.distinct);
		// 35. Fewer frames than the window: no verdict.
		for (int i = 0; i < N; i++) hashes[i] = CycleRule::Hash(pic[i & 1]);
		c = CycleRule::Period(hashes, valid, CycleRule::Window - 1);
		CHECK(c.period == 0, "fewer frames than the window give no verdict (period %d)", c.period);
	}

	std::printf("%s (%d failures)\n", failures ? "FAILED" : "ALL PASSED", failures);
	return failures ? 1 : 0;
}
