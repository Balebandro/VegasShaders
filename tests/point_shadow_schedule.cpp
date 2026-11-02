// Checks the point-light cubemap refresh schedule (src/core/PointShadowSchedule.h).
#include <cstdio>
#include <vector>
#include "../src/core/PointShadowSchedule.h"
#include "../src/core/PointShadowSlots.h"

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { std::printf("FAIL: "); std::printf(__VA_ARGS__); std::printf("\n"); failures++; } else { std::printf("PASS: "); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

static PointShadowSlotState State(const void* light, float x = 1, float y = 2, float z = 3, float r = 500)
{
	PointShadowSlotState s;
	static int cell, texture;
	s.light = light; s.texture = &texture; s.cell = &cell; s.x = x; s.y = y; s.z = z; s.radius = r; s.valid = true;
	return s;
}

static PointShadowSlotState StaticState(const void* light, unsigned long long hash)
{
	PointShadowSlotState s = State(light);
	s.staticCasters = true;
	s.casterHash = hash;
	return s;
}

int main()
{
	int lightA, lightB;
	const PointShadowSlotState a = State(&lightA);

	// Interval 1 (the default) redraws everything every frame, whatever the state.
	bool always = true;
	for (unsigned frame = 0; frame < 100; frame++) for (unsigned slot = 0; slot < 12; slot++) always &= PointShadowNeedsRedraw(a, a, frame, slot, 1);
	CHECK(always, "interval 1 redraws every slot on every frame");
	CHECK(PointShadowNeedsRedraw(PointShadowSlotState(), a, 5, 0, 1), "interval 1 redraws a never-drawn slot");

	// Stable slot: exactly one redraw per `interval` frames, for every slot, over long runs (including 3, which does not divide the old 8-frame counter).
	for (unsigned interval = 2; interval <= 4; interval++) {
		bool exactlyOnce = true;
		for (unsigned slot = 0; slot < 12; slot++)
			for (unsigned start = 0; start < 50; start++) {
				unsigned redraws = 0;
				for (unsigned f = start; f < start + interval; f++) redraws += PointShadowNeedsRedraw(a, a, f, slot, interval);
				exactlyOnce &= redraws == 1;
			}
		CHECK(exactlyOnce, "interval %u: a stable slot is redrawn exactly once in any %u consecutive frames", interval, interval);
	}

	// Staggering: with 12 slots at interval 2 or 3 the per-frame redraw count is spread evenly.
	for (unsigned interval = 2; interval <= 4; interval++) {
		unsigned minPer = 99, maxPer = 0;
		for (unsigned f = 0; f < 60; f++) {
			unsigned n = 0;
			for (unsigned slot = 0; slot < 12; slot++) n += PointShadowNeedsRedraw(a, a, f, slot, interval);
			minPer = n < minPer ? n : minPer; maxPer = n > maxPer ? n : maxPer;
		}
		CHECK(maxPer - minPer <= 0, "interval %u: 12 slots redraw %u per frame every frame (staggered evenly)", interval, minPer);
	}

	// Anything that changes what the slot describes forces an immediate redraw, on a frame it would otherwise skip.
	const unsigned skipFrame = 1, slot = 0, interval = 4; // (1 + 0) % 4 != 0
	CHECK(!PointShadowNeedsRedraw(a, a, skipFrame, slot, interval), "the test frame is one where a stable slot is skipped");
	CHECK(PointShadowNeedsRedraw(a, State(&lightB), skipFrame, slot, interval), "a different light in the slot is redrawn at once");
	CHECK(PointShadowNeedsRedraw(a, State(&lightA, 1.001f), skipFrame, slot, interval), "a moved light (x) is redrawn at once");
	CHECK(PointShadowNeedsRedraw(a, State(&lightA, 1, 2.5f), skipFrame, slot, interval), "a moved light (y) is redrawn at once");
	CHECK(PointShadowNeedsRedraw(a, State(&lightA, 1, 2, 3.5f), skipFrame, slot, interval), "a moved light (z) is redrawn at once");
	CHECK(PointShadowNeedsRedraw(a, State(&lightA, 1, 2, 3, 501), skipFrame, slot, interval), "a changed radius is redrawn at once");
	PointShadowSlotState otherCell = a; int cell2; otherCell.cell = &cell2;
	CHECK(PointShadowNeedsRedraw(a, otherCell, skipFrame, slot, interval), "a different cell is redrawn at once");
	PointShadowSlotState newTexture = a; int texture2; newTexture.texture = &texture2;
	CHECK(PointShadowNeedsRedraw(a, newTexture, skipFrame, slot, interval), "a recreated cubemap texture (device reset) is redrawn at once");
	CHECK(PointShadowNeedsRedraw(PointShadowSlotState(), a, skipFrame, slot, interval), "a slot that was never drawn is redrawn at once");

	// A complete static caster set is reusable indefinitely, even at interval 1. Any caster-state
	// change, or the arrival/removal of dynamic geometry, invalidates it immediately.
	const PointShadowSlotState staticA = StaticState(&lightA, 0x1234);
	bool staticReused = true;
	for (unsigned frame = 0; frame < 100; frame++) staticReused &= !PointShadowNeedsRedraw(staticA, staticA, frame, 0, 1);
	CHECK(staticReused, "an unchanged all-static cubemap is reused indefinitely at interval 1");
	PointShadowSlotState changedCasters = staticA; changedCasters.casterHash++;
	CHECK(PointShadowRedrawReason(staticA, changedCasters, 1, 0, 4) == PointShadowRedraw::CastersChanged,
		"a static caster transform/material/visibility change redraws at once");
	PointShadowSlotState becameDynamic = staticA; becameDynamic.staticCasters = false;
	CHECK(PointShadowRedrawReason(staticA, becameDynamic, 1, 0, 4) == PointShadowRedraw::CastersChanged,
		"the arrival of dynamic geometry redraws at once");
	CHECK(PointShadowRedrawReason(becameDynamic, staticA, 1, 0, 4) == PointShadowRedraw::CastersChanged,
		"a dynamic set becoming fully static redraws and starts a cache");

	// A light that moves every frame (carried torch) is redrawn every frame at any interval.
	bool moving = true;
	PointShadowSlotState last = State(&lightA, 0);
	for (unsigned f = 0; f < 40; f++) {
		PointShadowSlotState now = State(&lightA, (float)f + 1);
		moving &= PointShadowNeedsRedraw(last, now, f, 5, 4);
		last = now;
	}
	CHECK(moving, "a light that moves every frame is redrawn every frame at interval 4");

	// Redraw reasons: what a frame counts as. Each single change is reported as its own reason, a plain refresh as Scheduled.
	CHECK(PointShadowRedrawReason(PointShadowSlotState(), a, 0, 0, 2) == PointShadowRedraw::NewSlot, "reason: a never-drawn slot is NewSlot");
	CHECK(PointShadowRedrawReason(a, State(&lightB), 1, 0, 4) == PointShadowRedraw::OtherLight, "reason: a different light is OtherLight");
	CHECK(PointShadowRedrawReason(a, newTexture, 1, 0, 4) == PointShadowRedraw::OtherTexture, "reason: a new cubemap texture is OtherTexture");
	CHECK(PointShadowRedrawReason(a, otherCell, 1, 0, 4) == PointShadowRedraw::OtherCell, "reason: a different cell is OtherCell");
	CHECK(PointShadowRedrawReason(a, State(&lightA, 9), 1, 0, 4) == PointShadowRedraw::Moved, "reason: a moved light is Moved");
	CHECK(PointShadowRedrawReason(a, State(&lightA, 1, 2, 3, 400), 1, 0, 4) == PointShadowRedraw::Resized, "reason: a changed radius alone is Resized");
	CHECK(PointShadowRedrawReason(a, a, 4, 0, 4) == PointShadowRedraw::Scheduled, "reason: an unchanged slot on its turn is Scheduled");
	CHECK(PointShadowRedrawReason(a, a, 1, 0, 4) == PointShadowRedraw::None, "reason: an unchanged slot off its turn is None");
	CHECK(PointShadowRedrawReason(a, a, 3, 5, 1) == PointShadowRedraw::Scheduled, "reason: interval 1 is Scheduled on every frame");
	CHECK(!PointShadowSlotChanged(a, a) && PointShadowSlotChanged(a, State(&lightB)) && PointShadowSlotChanged(PointShadowSlotState(), a),
		"PointShadowSlotChanged ignores the schedule and sees every real change");
	const unsigned long long setSeed = 1469598103934665603ULL;
	const unsigned long long hashAB = PointShadowAddCasterHash(PointShadowAddCasterHash(setSeed, 0x1234), 0x5678);
	const unsigned long long hashBA = PointShadowAddCasterHash(PointShadowAddCasterHash(setSeed, 0x5678), 0x1234);
	CHECK(hashAB == hashBA, "caster-set hash ignores geometry-list traversal order");
	CHECK(hashAB != PointShadowAddCasterHash(PointShadowAddCasterHash(setSeed, 0x1234), 0x5679),
		"caster-set hash changes when caster state changes");

	// Stable slot assignment (PointShadowSlots.h).
	int L[12];
	auto assign = [&](const void** previous, std::vector<const void*> ranked, int slots, const void** out) {
		AssignStablePointShadowSlots(previous, ranked.data(), (int)ranked.size(), slots, out);
	};
	auto slotOf = [&](const void** out, int slots, const void* light) { for (int s = 0; s < slots; s++) if (out[s] == light) return s; return -1; };
	{
		const void* none[11] = {};
		const void* out[11];
		assign(none, { &L[0], &L[1], &L[2], &L[3] }, 11, out);
		CHECK(out[0] == &L[0] && out[1] == &L[1] && out[2] == &L[2] && out[3] == &L[3] && !out[4] && !out[10],
			"stable slots: with nothing assigned before, rank r takes slot r");

		// The same lights in a different distance order keep their slots.
		const void* first[11]; for (int s = 0; s < 11; s++) first[s] = out[s];
		const void* again[11];
		assign(first, { &L[2], &L[0], &L[3], &L[1] }, 11, again);
		bool same = true; for (int s = 0; s < 11; s++) same &= again[s] == first[s];
		CHECK(same, "stable slots: lights that only changed rank keep their slots");

		// A light that leaves frees its slot; the newcomer takes the lowest free one, nobody else moves.
		const void* left[11];
		assign(first, { &L[0], &L[2], &L[3], &L[4] }, 11, left); // L[1] left, L[4] arrived
		CHECK(left[0] == &L[0] && left[2] == &L[2] && left[3] == &L[3] && left[1] == &L[4] && !left[4],
			"stable slots: a newcomer takes the slot freed by the light that left, the others stay");

		// A hole in the middle is filled before slots at the end, and a second newcomer goes to the next free slot.
		const void* filled[11];
		assign(left, { &L[0], &L[2], &L[3], &L[4], &L[5], &L[6] }, 11, filled);
		CHECK(filled[0] == &L[0] && filled[1] == &L[4] && filled[2] == &L[2] && filled[3] == &L[3] && filled[4] == &L[5] && filled[5] == &L[6],
			"stable slots: newcomers fill free slots in rank order without moving anyone");
	}
	{
		// Every ranked light gets exactly one slot, and no slot holds two, over a long random walk of the ranking.
		const void* previous[11] = {};
		unsigned seed = 12345;
		auto next = [&]() { seed = seed * 1664525u + 1013904223u; return seed >> 16; };
		bool valid = true, moved = false;
		int totalMoves = 0, naiveMoves = 0;
		const void* naivePrevious[11] = {};
		for (int frame = 0; frame < 3000; frame++) {
			std::vector<const void*> ranked;
			bool present[12] = {};
			int count = 3 + (int)(next() % 9); // 3..11 lights
			while ((int)ranked.size() < count) { int l = (int)(next() % 12); if (!present[l]) { present[l] = true; ranked.push_back(&L[l]); } }
			const void* out[11];
			assign(previous, ranked, 11, out);
			for (const void* light : ranked) valid &= slotOf(out, 11, light) >= 0;
			int used = 0; for (int s = 0; s < 11; s++) used += out[s] != nullptr;
			valid &= used == count;
			for (int s = 0; s < 11; s++) for (int t = s + 1; t < 11; t++) valid &= !(out[s] && out[s] == out[t]);
			for (const void* light : ranked) { int was = slotOf(previous, 11, light); int now = slotOf(out, 11, light); if (was >= 0 && was != now) moved = true; if (was >= 0 && was != now) totalMoves++; }
			// what plain rank order would have done
			for (size_t r = 0; r < ranked.size(); r++) { int was = slotOf(naivePrevious, 11, ranked[r]); if (was >= 0 && was != (int)r) naiveMoves++; }
			for (int s = 0; s < 11; s++) naivePrevious[s] = (size_t)s < ranked.size() ? ranked[s] : nullptr;
			for (int s = 0; s < 11; s++) previous[s] = out[s];
		}
		CHECK(valid, "stable slots: every light gets exactly one slot and no slot holds two (3000 random frames)");
		CHECK(!moved, "stable slots: a light that was in a slot and is still ranked never changes slot");
		CHECK(naiveMoves > 0 && totalMoves == 0, "stable slots: plain rank order would have moved lights %d times where stable slots move none", naiveMoves);
	}
	// UNOFFICIAL (2026-10-08): forward shadows can use all 32 slots (PointShadowSlotsMax): a full set lands one per slot, and a
	// light that stays ranked keeps its slot past the old 16.
	{
		static int lamps[40];
		const void* ranked[32]; const void* previous[32] = {}; const void* out[32];
		for (int r = 0; r < 32; r++) ranked[r] = &lamps[r];
		AssignStablePointShadowSlots(previous, ranked, 32, 32, out);
		bool full = true;
		for (int s = 0; s < 32; s++) full &= out[s] == ranked[s];
		CHECK(full, "32 slots: 32 lights fill them in rank order");
		for (int s = 0; s < 32; s++) previous[s] = out[s];
		const void* reranked[32];
		for (int r = 0; r < 32; r++) reranked[r] = r < 2 ? (const void*)&lamps[32 + r] : ranked[33 - r];	// two newcomers, the rest reversed
		AssignStablePointShadowSlots(previous, reranked, 32, 32, out);
		bool kept = true, newcomersIn = slotOf(out, 32, &lamps[32]) >= 0 && slotOf(out, 32, &lamps[33]) >= 0;
		for (int r = 2; r < 32; r++) kept &= slotOf(out, 32, reranked[r]) == slotOf(previous, 32, reranked[r]);
		CHECK(kept && newcomersIn, "32 slots: lights still ranked keep their slots (incl. slots 16-31), newcomers take the freed ones");
	}

	// 2026-10-08: 64 slots (ShadowCubeMapsMax): a full set fills them in rank order, and a re-ranked set keeps its slots.
	{
		static int lamps[70];
		const void* ranked[64]; const void* previous[64] = {}; const void* out[64];
		for (int r = 0; r < 64; r++) ranked[r] = &lamps[r];
		AssignStablePointShadowSlots(previous, ranked, 64, 64, out);
		bool full = true;
		for (int s = 0; s < 64; s++) full &= out[s] == ranked[s];
		CHECK(full, "64 slots: 64 lights fill them in rank order");
		for (int s = 0; s < 64; s++) previous[s] = out[s];
		const void* reranked[64];
		for (int r = 0; r < 64; r++) reranked[r] = r < 3 ? (const void*)&lamps[64 + r] : ranked[66 - r];	// three newcomers, the rest reversed
		AssignStablePointShadowSlots(previous, reranked, 64, 64, out);
		bool kept = true, newcomersIn = true;
		for (int r = 0; r < 3; r++) newcomersIn &= slotOf(out, 64, &lamps[64 + r]) >= 0;
		for (int r = 3; r < 64; r++) kept &= slotOf(out, 64, reranked[r]) == slotOf(previous, 64, reranked[r]);
		CHECK(kept && newcomersIn, "64 slots: lights still ranked keep their slots (incl. slots 32-63), newcomers take the freed ones");
	}

	// RedrawActorsOnly (2026-10-08): how a redrawn slot is drawn (PointShadowDrawPlan).
	{
		static int staticCube, otherCube;
		auto staticOf = [](const void* light, unsigned long long hash, const void* cube) {
			PointShadowSlotState s = StaticState(light, hash); s.texture = cube; return s;
		};
		const PointShadowSlotState drawn = staticOf(&lightA, 7, &staticCube);
		PointShadowSlotState none = drawn; none.texture = nullptr;
		PointShadowSlotState movedLamp = drawn; movedLamp.x += 1;
		CHECK(PointShadowDrawPlan(false, drawn, drawn, true) == PointShadowDraw::All, "split off: the whole list, as before");
		CHECK(PointShadowDrawPlan(true, PointShadowSlotState(), none, true) == PointShadowDraw::All, "no static cube map could be made: the whole list");
		CHECK(PointShadowDrawPlan(true, PointShadowSlotState(), drawn, true) == PointShadowDraw::StaticThenMoving, "people in reach, static cube map never drawn: static first, then the people");
		CHECK(PointShadowDrawPlan(true, drawn, drawn, true) == PointShadowDraw::MovingOnly, "people in reach, static cube map current: the copy and the people only");
		CHECK(PointShadowDrawPlan(true, drawn, staticOf(&lightA, 8, &staticCube), true) == PointShadowDraw::StaticThenMoving, "a static caster changed (a door opened): static redrawn");
		CHECK(PointShadowDrawPlan(true, drawn, staticOf(&lightB, 7, &staticCube), true) == PointShadowDraw::StaticThenMoving, "another lamp in the slot, its static cube map not here: static redrawn");
		CHECK(PointShadowDrawPlan(true, drawn, staticOf(&lightA, 7, &otherCube), true) == PointShadowDraw::StaticThenMoving, "the static cube map texture was recreated: static redrawn");
		CHECK(PointShadowDrawPlan(true, drawn, movedLamp, true) == PointShadowDraw::StaticThenMoving, "the lamp moved: static redrawn");
		CHECK(PointShadowDrawPlan(true, drawn, drawn, false) == PointShadowDraw::MovingOnly, "the people left, static cube map current: the copy alone");
		CHECK(PointShadowDrawPlan(true, drawn, staticOf(&lightA, 8, &staticCube), false) == PointShadowDraw::All, "all static and the static cube map stale: straight into the live cube map");
		CHECK(PointShadowDrawPlan(true, PointShadowSlotState(), none, false) == PointShadowDraw::All, "a lamp that never had people near it: as before");
	}

	std::printf(failures ? "\n%d check(s) FAILED\n" : "\nAll point shadow schedule checks passed\n", failures);
	return failures ? 1 : 0;
}
