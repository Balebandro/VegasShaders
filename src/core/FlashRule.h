#pragma once

// UNOFFICIAL 2026-11-01 the flash rule of the automatic flash / flicker detector (core/Diagnostics.cpp, [Main.Main.Misc] FlashDetect).
// Pure maths on three 8 x 8 luminance summaries of the finished picture (frames t-1, t, t+1, each cell the mean luminance 0..1 of its
// part of the screen), so it is unit-tested on synthetic sequences (tests/flash_rule.cpp). The flicker bench's rule: frame t is a
// FLASH when it jumps away from BOTH neighbours in the same direction while the neighbours agree with each other:
//   per cell: d1 = I(t) - I(t-1), d2 = I(t) - I(t+1) have the same sign, the smaller of |d1|, |d2| is at least
//   max(CellAbs, CellRel x the neighbours' mean), and |I(t-1) - I(t+1)| is at most Agree x that jump;
//   the frame: at least DarkerCells cells darker (or BrighterCells brighter: game lights such as muzzle flashes and lightning are
//   brief and bright, so a brighter frame needs a larger share), or the whole picture's mean passes the same test with WholeAbs /
//   WholeRel (with at least WholeCells cells agreeing).
// Why this does not fire on what is not a flash: a camera pan or turn moves the picture continuously, so I(t+1) differs from I(t-1)
// by more than either differs from I(t) (the neighbours do not agree); a fade or a sunrise is monotonic (d1 and d2 have opposite
// signs); a cut, a loading screen or a menu opening is a step (I(t) ~ I(t+1): d2 ~ 0). The caller also skips frames around loads,
// menus, the Pip-Boy, fast turns and history resets (TemporalBackbone::HistoryValid false).
// Black / white frames: a FLASH whose frame mean is under BlackMean (neighbours over BlackNeighbour) or over WhiteMean (neighbours
// under WhiteNeighbour) is labelled so.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace FlashRule {

	constexpr int Side = 8, Cells = Side * Side;

	struct Thresholds {
		float	CellAbs = 0.04f;		// a cell's jump: at least this much luminance (0..1) ...
		float	CellRel = 0.12f;		// ... and at least this share of the neighbours' mean
		float	Agree = 0.35f;			// the neighbours may differ by at most this share of the jump
		int		DarkerCells = 16;		// 25 % of the picture darker
		int		BrighterCells = 24;		// 37.5 % brighter
		float	WholeAbs = 0.03f, WholeRel = 0.10f;
		int		WholeCells = 8;
		float	BlackMean = 0.02f, BlackNeighbour = 0.08f, WhiteMean = 0.92f, WhiteNeighbour = 0.75f;
		// Stage attribution (Attribute below): the source is the first stage whose OWN jump on the flashing cells is at least
		// StageShare of the final picture's relative jump there and at least StageMin; later stages with at least AlsoShare of the
		// source's own jump amplified it, stages at or under -AlsoShare of it damped it.
		float	StageShare = 0.3f, StageMin = 0.01f, AlsoShare = 0.25f;
	};

	struct Verdict {
		bool		flash = false;
		bool		whole = false;			// the whole picture's mean jumped
		bool		black = false, white = false;
		int			darker = 0, brighter = 0;	// cells that flashed each way
		int			sign = 0;				// -1 darker, +1 brighter (the side that decided)
		uint64_t	mask = 0;				// bit y * 8 + x: cells that flashed on the deciding side
		float		meanPrev = 0.0f, meanThis = 0.0f, meanNext = 0.0f;
		float		largest = 0.0f;			// the largest jump of a flashing cell (signed, the smaller of its two)
	};

	inline Verdict Judge(const float* prev, const float* cur, const float* next, const Thresholds& t = Thresholds()) {
		Verdict v;
		uint64_t darkMask = 0, brightMask = 0;
		float darkLargest = 0.0f, brightLargest = 0.0f;
		double sa = 0.0, sb = 0.0, sc = 0.0;
		for (int i = 0; i < Cells; i++) {
			const float a = prev[i], b = cur[i], c = next[i];
			sa += a; sb += b; sc += c;
			const float d1 = b - a, d2 = b - c;
			if ((d1 > 0.0f) != (d2 > 0.0f) || d1 == 0.0f || d2 == 0.0f) continue;
			const float jump = fminf(fabsf(d1), fabsf(d2));
			const float reference = 0.5f * (a + c);
			if (jump < fmaxf(t.CellAbs, t.CellRel * reference)) continue;
			if (fabsf(a - c) > t.Agree * jump) continue;
			if (d1 < 0.0f) { v.darker++; darkMask |= 1ull << i; if (jump > darkLargest) darkLargest = jump; }
			else { v.brighter++; brightMask |= 1ull << i; if (jump > brightLargest) brightLargest = jump; }
		}
		v.meanPrev = (float)(sa / Cells); v.meanThis = (float)(sb / Cells); v.meanNext = (float)(sc / Cells);
		const float m1 = v.meanThis - v.meanPrev, m2 = v.meanThis - v.meanNext;
		if ((m1 > 0.0f) == (m2 > 0.0f) && m1 != 0.0f && m2 != 0.0f) {
			const float jump = fminf(fabsf(m1), fabsf(m2));
			const int agreeing = m1 < 0.0f ? v.darker : v.brighter;
			v.whole = jump >= fmaxf(t.WholeAbs, t.WholeRel * 0.5f * (v.meanPrev + v.meanNext)) &&
				fabsf(v.meanPrev - v.meanNext) <= t.Agree * jump && agreeing >= t.WholeCells;
		}
		v.black = v.meanThis < t.BlackMean && v.meanPrev > t.BlackNeighbour && v.meanNext > t.BlackNeighbour;
		v.white = v.meanThis > t.WhiteMean && v.meanPrev < t.WhiteNeighbour && v.meanNext < t.WhiteNeighbour;
		const bool dark = v.darker >= t.DarkerCells || (v.whole && m1 < 0.0f) || v.black;
		const bool bright = v.brighter >= t.BrighterCells || (v.whole && m1 > 0.0f) || v.white;
		v.flash = dark || bright;
		if (dark && (!bright || v.darker >= v.brighter)) { v.sign = -1; v.mask = darkMask; v.largest = -darkLargest; }
		else if (bright) { v.sign = 1; v.mask = brightMask; v.largest = brightLargest; }
		return v;
	}

	// "..XX..../..." : the 8 x 8 cells, top row first, X = flashed (for the log line)
	inline void MaskText(uint64_t mask, char* out /* at least 72 */) {
		int k = 0;
		for (int y = 0; y < Side; y++) {
			if (y) out[k++] = '/';
			for (int x = 0; x < Side; x++) out[k++] = (mask >> (y * Side + x)) & 1 ? 'X' : '.';
		}
		out[k] = 0;
	}

	// Where on the screen the mask's cells are, in words (UNOFFICIAL 2026-11-01 step 2): "the upper half", "the left half", "the centre",
	// "the edges", "the whole picture", "the upper left" ... from the shares of the cells in each half and in the inner 4 x 4.
	inline void RegionText(uint64_t mask, char* out /* at least 48 */) {
		int n = 0, top = 0, left = 0, centre = 0;
		for (int i = 0; i < Cells; i++) {
			if (!((mask >> i) & 1)) continue;
			const int x = i % Side, y = i / Side;
			n++;
			if (y < Side / 2) top++;
			if (x < Side / 2) left++;
			if (x >= 2 && x < 6 && y >= 2 && y < 6) centre++;
		}
		if (!n) { strcpy_s(out, 48, "nowhere"); return; }
		const char* vertical = top * 4 >= n * 3 ? "upper" : (n - top) * 4 >= n * 3 ? "lower" : nullptr;
		const char* horizontal = left * 4 >= n * 3 ? "left" : (n - left) * 4 >= n * 3 ? "right" : nullptr;
		if (n >= 48) strcpy_s(out, 48, "the whole picture");
		else if (vertical && horizontal) sprintf_s(out, 48, "the %s %s", vertical, horizontal);
		else if (vertical) sprintf_s(out, 48, "the %s half", vertical);
		else if (horizontal) sprintf_s(out, 48, "the %s half", horizontal);
		else if (centre * 4 >= n * 3) strcpy_s(out, 48, "the centre");
		else if (centre == 0 && n >= 8) strcpy_s(out, 48, "the edges");
		else strcpy_s(out, 48, "spread over the picture");
	}

	// The mean of the mask's cells
	inline float MaskMean(const float* luma, uint64_t mask) {
		double sum = 0.0;
		int n = 0;
		for (int i = 0; i < Cells; i++) if ((mask >> i) & 1) { sum += luma[i]; n++; }
		return n ? (float)(sum / n) : 0.0f;
	}

	// --- stage attribution (UNOFFICIAL 2026-11-01, the stage probes of core/Diagnostics.cpp, [Main.Main.Misc] FlashStages) ---
	// The same 8 x 8 summary taken after every effect of the frame chain, in chain order, for frames t-1, t, t+1. Because the effects
	// are composited one after another onto the same picture, a stage's jump on the flashing cells contains everything upstream of
	// it: its OWN contribution is its jump minus the jump of the stage before it. The source of the flash is the first stage whose
	// own jump is large enough; stages after it that add to the jump amplified it (Bloom, Exposure a frame later), stages with a
	// negative own jump damped it (TAA). The maths is relative (jump / the neighbours' mean) so HDR stages (before the tonemap) and
	// display stages (after it) compare.
	constexpr int MaxStages = 48;

	// The mean relative spike of the mask's cells in the flash's direction: min(|b-a|, |b-c|) / max(floor, (a+c)/2) where b differs
	// from both neighbours the flash's way, else 0.
	inline float RelativeJump(const float* a, const float* b, const float* c, uint64_t mask, int sign, float floor = 0.002f) {
		double sum = 0.0;
		int n = 0;
		for (int i = 0; i < Cells; i++) {
			if (!((mask >> i) & 1)) continue;
			n++;
			const float d1 = b[i] - a[i], d2 = b[i] - c[i];
			const bool same = sign < 0 ? (d1 < 0.0f && d2 < 0.0f) : (d1 > 0.0f && d2 > 0.0f);
			if (!same) continue;
			sum += fminf(fabsf(d1), fabsf(d2)) / fmaxf(floor, 0.5f * (a[i] + c[i]));
		}
		return n ? (float)(sum / n) : 0.0f;
	}

	// nullptr: the stage was not probed in that frame. rebase: a mark where the picture changes domain (the game's tonemap: HDR before,
	// display values after): its own jump is not judged (the HDR jump compressed is not a damping) and the stages after it are measured
	// against it.
	struct StageInput { const float* prev; const float* cur; const float* next; bool rebase = false; };
	struct StageResult { float rel = 0.0f, own = 0.0f; bool valid = false; };
	struct Attribution {
		int			source = -1;					// index into the stages, -1 = none qualified
		float		finalRel = 0.0f;				// the final picture's relative jump on the mask (what the stages are measured against)
		float		largestOwn = 0.0f;				// the largest own jump seen (for the "none qualified" line)
		int			largest = -1;
		int			count = 0;
		StageResult	stage[MaxStages];
		bool		amplified[MaxStages] = {}, damped[MaxStages] = {};
	};

	inline Attribution Attribute(const StageInput* stages, int count, uint64_t mask, int sign, float finalRel, const Thresholds& t = Thresholds()) {
		Attribution r;
		r.count = count > MaxStages ? MaxStages : count;
		r.finalRel = finalRel;
		float previous = 0.0f;
		for (int k = 0; k < r.count; k++) {
			const StageInput& s = stages[k];
			if (!s.prev || !s.cur || !s.next) continue;	// not probed in one of the frames: the next stage's own jump is measured against the last probed one
			const float rel = RelativeJump(s.prev, s.cur, s.next, mask, sign);
			r.stage[k].rel = rel; r.stage[k].own = s.rebase ? 0.0f : rel - previous; r.stage[k].valid = true;
			previous = rel;
			if (s.rebase) continue;
			if (r.stage[k].own > r.largestOwn) { r.largestOwn = r.stage[k].own; r.largest = k; }
			if (r.source < 0 && r.stage[k].own >= fmaxf(t.StageMin, t.StageShare * finalRel)) r.source = k;
		}
		if (r.source >= 0) {
			const float own = r.stage[r.source].own;
			for (int k = 0; k < r.count; k++) {
				if (k == r.source || !r.stage[k].valid || stages[k].rebase) continue;
				if (r.stage[k].own >= t.AlsoShare * own) r.amplified[k] = true;
				else if (r.stage[k].own <= -t.AlsoShare * own) r.damped[k] = true;
			}
		}
		return r;
	}
}

// UNOFFICIAL 2026-11-01 the beat rule of the report burst's "REPORT n VL pattern" line (core/Diagnostics.cpp "VL stages"): does a value
// measured once a frame (a volumetric light stage's upper-half mean over a burst) alternate with a fixed period, larger than its noise?
// Each value is compared with the mean of its two neighbours, d = v - (previous + next) / 2 (0 on any straight drift, so a pan or a
// fade does not count; the burst's mean d is taken out as well, so a steady curve does not either). For each period 2..4 the d of each
// phase (frame number mod period, at least 3 values each) are averaged; a beat is a phase whose mean stands out from the scatter left
// inside the phases: score = |phase mean| / its standard error (with a floor for half-float rounding), at least MinScore. A one-in-three
// dropout a a b a a b ... gives d = b - a on the odd frames and -(b - a) / 2 on the others: period 3, that phase, a high score. One odd
// frame alone does not make a beat. Measured on 16-frame bursts (15 values): plain noise passes 7 in 0.2 % of bursts, a one-in-three
// beat of 6 x the noise's standard deviation in 95 %, of 10 x in all. Unit-tested in tests/flash_rule.cpp.
namespace BeatRule {

	constexpr int MaxValues = 64, MinPeriod = 2, MaxPeriod = 4, MinPerPhase = 3;
	constexpr double MinScore = 7.0;

	struct Result {
		bool	beat = false;
		int		period = 0, phase = 0;		// the best period and its phase that stands out (frames with frame % period == phase)
		double	score = 0.0;				// that phase's mean d over its standard error (>= MinScore: a beat)
		double	noise = 0.0;				// the scatter of d inside the phases (standard deviation)
		double	odd = 0.0, rest = 0.0;		// the values' mean on that phase's frames, and on the others
		int		used = 0;					// values with both neighbours (consecutive frames, all three valid)
	};

	// values[i] measured in frame frames[i] (ascending), valid[i] false: none that frame.
	inline Result Find(const float* values, const bool* valid, const unsigned* frames, int count) {
		Result r;
		if (count > MaxValues) count = MaxValues;
		double d[MaxValues] = {};
		bool has[MaxValues] = {};
		double scale = 0.0, dSum = 0.0;
		int seen = 0;
		for (int i = 0; i < count; i++)
			if (valid[i] && values[i] == values[i]) { scale += fabs((double)values[i]); seen++; }
		for (int i = 1; i + 1 < count; i++) {
			if (!valid[i - 1] || !valid[i] || !valid[i + 1] || frames[i - 1] + 1 != frames[i] || frames[i] + 1 != frames[i + 1]) continue;
			const double v = (double)values[i] - 0.5 * ((double)values[i - 1] + (double)values[i + 1]);
			if (v != v || fabs(v) > 1e30) continue;	// NaN / inf: left out
			d[i] = v; has[i] = true; dSum += v; r.used++;
		}
		if (r.used < 2 * MinPeriod + 1 || !seen) return r;
		const double floor = 1e-6 + 2e-4 * (scale / seen);	// a half float's rounding near the values' size
		const double dMean = dSum / r.used;
		for (int p = MinPeriod; p <= MaxPeriod; p++) {
			double sum[MaxPeriod] = {};
			int n[MaxPeriod] = {};
			for (int i = 0; i < count; i++) if (has[i]) { sum[frames[i] % p] += d[i] - dMean; n[frames[i] % p]++; }
			bool ok = r.used > p;
			for (int c = 0; c < p; c++) if (n[c] < MinPerPhase) ok = false;
			if (!ok) continue;
			double residual = 0.0;
			for (int i = 0; i < count; i++)
				if (has[i]) { const int c = frames[i] % p; const double e = d[i] - dMean - sum[c] / n[c]; residual += e * e; }
			const double variance = residual / (r.used - p);
			for (int c = 0; c < p; c++) {
				const double score = fabs(sum[c] / n[c]) / sqrt(variance / n[c] + floor * floor);
				if (score > r.score) { r.score = score; r.period = p; r.phase = c; r.noise = sqrt(variance); }
			}
		}
		if (!r.period) return r;
		double a = 0.0, b = 0.0;
		int na = 0, nb = 0;
		for (int i = 0; i < count; i++) {
			if (!valid[i] || values[i] != values[i]) continue;
			if ((int)(frames[i] % r.period) == r.phase) { a += values[i]; na++; } else { b += values[i]; nb++; }
		}
		r.odd = na ? a / na : 0.0;
		r.rest = nb ? b / nb : 0.0;
		r.beat = r.score >= MinScore;
		return r;
	}

	// --- UNOFFICIAL 2026-11-01 step 2 (docs/next/FLICKER-ISOLATION-PLAN.md): the beat detector on the whole picture ---
	// The one-frame flash rule needs a cell to move 0.04 in one frame. A steady beat of 0.035 (the Strip: dark pixels +9 levels every third
	// frame) never passes it, but over a window of frames it stands out of the noise: Find above, run on every cell's series over the
	// last Window frames of a still camera. A cell beats when Find says so and the odd phase differs from the rest by at least
	// CellAmplitude (about 2 levels); the picture beats when at least MinCells cells beat with the same period and phase.
	constexpr int Window = 48;
	constexpr double CellAmplitude = 0.008;
	constexpr int MinCells = 6;

	struct CellsResult {
		bool		beat = false;
		int			period = 0, phase = 0;	// of the beating cells' majority
		int			cells = 0;				// cells beating with that period and phase
		uint64_t	mask = 0;				// those cells
		double		amplitude = 0.0;		// their mean (odd phase - the rest), signed: negative = the odd frames are darker
		double		score = 0.0;			// their mean Find score
		int			candidates = 0;			// cells that beat at any period
	};

	// Find's maths for every period at once: the best phase's score and (odd - rest) per period, so a picture can be judged per period (a
	// period-2 alternation is also a period-4 one; Find alone picks whichever scores higher by noise, so cells would split between them).
	struct PeriodScores { double score[MaxPeriod + 1] = {}; int phase[MaxPeriod + 1] = {}; double amplitude[MaxPeriod + 1] = {}; int used = 0; };
	inline PeriodScores Scores(const float* values, const bool* valid, const unsigned* frames, int count) {
		PeriodScores r;
		if (count > MaxValues) count = MaxValues;
		double d[MaxValues] = {};
		bool has[MaxValues] = {};
		double scale = 0.0, dSum = 0.0;
		int seen = 0;
		for (int i = 0; i < count; i++)
			if (valid[i] && values[i] == values[i]) { scale += fabs((double)values[i]); seen++; }
		for (int i = 1; i + 1 < count; i++) {
			if (!valid[i - 1] || !valid[i] || !valid[i + 1] || frames[i - 1] + 1 != frames[i] || frames[i] + 1 != frames[i + 1]) continue;
			const double v = (double)values[i] - 0.5 * ((double)values[i - 1] + (double)values[i + 1]);
			if (v != v || fabs(v) > 1e30) continue;
			d[i] = v; has[i] = true; dSum += v; r.used++;
		}
		if (r.used < 2 * MinPeriod + 1 || !seen) return r;
		const double floor = 1e-6 + 2e-4 * (scale / seen);
		const double dMean = dSum / r.used;
		for (int p = MinPeriod; p <= MaxPeriod; p++) {
			double sum[MaxPeriod] = {};
			int n[MaxPeriod] = {};
			for (int i = 0; i < count; i++) if (has[i]) { sum[frames[i] % p] += d[i] - dMean; n[frames[i] % p]++; }
			bool ok = r.used > p;
			for (int c = 0; c < p; c++) if (n[c] < MinPerPhase) ok = false;
			if (!ok) continue;
			double residual = 0.0;
			for (int i = 0; i < count; i++)
				if (has[i]) { const int c = frames[i] % p; const double e = d[i] - dMean - sum[c] / n[c]; residual += e * e; }
			const double variance = residual / (r.used - p);
			for (int c = 0; c < p; c++) {
				const double score = fabs(sum[c] / n[c]) / sqrt(variance / n[c] + floor * floor);
				if (score > r.score[p]) { r.score[p] = score; r.phase[p] = c; }
			}
			double a = 0.0, b = 0.0;
			int na = 0, nb = 0;
			for (int i = 0; i < count; i++) {
				if (!valid[i] || values[i] != values[i]) continue;
				if ((int)(frames[i] % p) == r.phase[p]) { a += values[i]; na++; } else { b += values[i]; nb++; }
			}
			r.amplitude[p] = (na ? a / na : 0.0) - (nb ? b / nb : 0.0);
		}
		return r;
	}

	// lumas[i]: the 64 cell luminances of frame frames[i] (ascending, consecutive for a clean result); valid[i] false: no picture that frame.
	// Per period: the cells that beat at it (score >= MinScore, amplitude >= minAmplitude); the picture's period is the one with the most
	// cells, the smallest on a tie (an alternation counts at 2 and 4: 2 wins), and the phase is those cells' commonest.
	inline CellsResult JudgeCells(const float* const* lumas, const bool* valid, const unsigned* frames, int count, double minAmplitude = CellAmplitude, int minCells = MinCells) {
		CellsResult r;
		if (count > MaxValues) count = MaxValues;
		int cells[MaxPeriod + 1] = {}, phases[MaxPeriod + 1][MaxPeriod] = {};
		double amplitude[MaxPeriod + 1] = {}, score[MaxPeriod + 1] = {};
		uint64_t mask[MaxPeriod + 1] = {};
		float values[MaxValues];
		for (int c = 0; c < FlashRule::Cells; c++) {
			for (int i = 0; i < count; i++) values[i] = valid[i] && lumas[i] ? lumas[i][c] : 0.0f;
			const PeriodScores s = Scores(values, valid, frames, count);
			bool any = false;
			for (int p = MinPeriod; p <= MaxPeriod; p++) {
				if (s.score[p] < MinScore || fabs(s.amplitude[p]) < minAmplitude) continue;
				any = true;
				cells[p]++; phases[p][s.phase[p]]++;
				amplitude[p] += s.amplitude[p]; score[p] += s.score[p];
				mask[p] |= 1ull << c;
			}
			if (any) r.candidates++;
		}
		for (int p = MinPeriod; p <= MaxPeriod; p++)
			if (cells[p] > r.cells) {
				r.cells = cells[p]; r.period = p; r.mask = mask[p];
				r.amplitude = amplitude[p] / cells[p]; r.score = score[p] / cells[p];
				r.phase = 0;
				for (int ph = 1; ph < p; ph++) if (phases[p][ph] > phases[p][r.phase]) r.phase = ph;
			}
		r.beat = r.cells >= minCells;
		return r;
	}

	// How much a series' odd phase (frames with frame % period == phase) differs from the rest, relative to the rest's level: for the
	// stage attribution of a beat (the stages' series at the picture's period and phase; signed).
	inline float PhaseAmplitude(const float* values, const bool* valid, const unsigned* frames, int count, int period, int phase, float floor = 0.002f) {
		if (period <= 0) return 0.0f;
		double a = 0.0, b = 0.0;
		int na = 0, nb = 0;
		for (int i = 0; i < count; i++) {
			if (!valid[i] || values[i] != values[i]) continue;
			if ((int)(frames[i] % period) == phase) { a += values[i]; na++; } else { b += values[i]; nb++; }
		}
		if (!na || !nb) return 0.0f;
		const double odd = a / na, rest = b / nb;
		return (float)((odd - rest) / fmax((double)floor, fabs(rest)));
	}
}

// --- UNOFFICIAL 2026-11-01 the cycle rule (Diagnostics.h part 5, the history watch) ---
// A temporal effect whose output repeats EXACTLY every P frames while the camera is still, cycling through the same few pictures in a
// fixed order, has a history that no longer converges on the picture: its output is a function of the jitter step and the history
// ping-pong alone (a tester's 4K FSR 2 smear 2026-11-01: 5 pictures in a 16-frame cycle = 8 jitter steps x 2 history buffers). A
// converged still picture repeats every frame (one picture: normal), and a live one never repeats exactly (noise, animation, clouds).
// Period: the smallest P in 2..MaxPeriod such that the last Window hashes satisfy h[i] == h[i - P], with at least 2 distinct hashes
// among them; 0 otherwise. Unit-tested in tests/flash_rule.cpp.
namespace CycleRule {
	constexpr int Window = 48, MaxPeriod = 32;

	// A picture's hash: its 64 cell luminances quantised to 1/256 (FNV-1a), so pictures equal to within rounding hash alike;
	// NaN cells hash as their own value.
	inline uint64_t Hash(const float* luma, int cells = 64) {
		uint64_t h = 1469598103934665603ull;
		for (int i = 0; i < cells; i++) {
			const float v = luma[i];
			int q = v != v ? -1 : (int)floorf(v * 256.0f + 0.5f);
			if (q < -1) q = -1;
			if (q > 1023) q = 1023;
			h ^= (uint64_t)(unsigned)(q + 1);
			h *= 1099511628211ull;
		}
		return h;
	}

	struct Result { int period = 0; int distinct = 0; };

	// hashes[0..count) oldest first; valid[i] false: that frame is missing (no cycle is judged through a gap)
	inline Result Period(const uint64_t* hashes, const bool* valid, int count) {
		Result r;
		if (count < Window) return r;
		const int first = count - Window;
		for (int i = first; i < count; i++) if (!valid[i]) return r;
		uint64_t seen[Window];
		int n = 0;
		for (int i = first; i < count; i++) {
			bool dup = false;
			for (int k = 0; k < n && !dup; k++) dup = seen[k] == hashes[i];
			if (!dup) seen[n++] = hashes[i];
		}
		r.distinct = n;
		if (n < 2) return r;
		for (int p = 2; p <= MaxPeriod; p++) {
			bool ok = true;
			for (int i = first + p; i < count && ok; i++) ok = hashes[i] == hashes[i - p];
			if (ok) { r.period = p; return r; }
		}
		return r;
	}
}
