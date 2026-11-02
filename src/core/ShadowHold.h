#pragma once

// UNOFFICIAL 2026-10-30 ShadowHold ([Shaders.ShadowsInteriors.Forward] ShadowHold, seconds, 0 = off; user: "shadows pop in and out of
// existence again seemingly randomly when we walk around in cells with a lot of lights like Vault 21").
// Indoors a lamp's shadow comes from a live slot (LightPoints, people included) or, with PoolLamps, from the pool's still cube map; any
// other lamp lights with no shadow at all. Walking changes the ranking, so lamps at the edges of those lists gained and lost their shadows:
//   - the pool took the first PoolLamps candidates every frame with no margin (the live slots have LampSwitchMargin): a lamp at its edge
//     flipped in and out;
//   - a pool lamp whose map was not current (it moved, its reach changed by a flicker, an exact compare) waited for a draw turn
//     (PoolDrawsPerFrame) with NO shadow meanwhile, and a just-joined one had none until its turn;
//   - FadeIn started over whenever a lamp moved between a live slot and the pool (its shadow faded out and back in).
// With ShadowHold above 0: a pool lamp stays a member for that many seconds after it last ranked inside the pool (unless the live slots or a
// cell change take it); a pool map that is not current is still used until it is redrawn (same cell, moved under 32 units); a reach /
// position change under 2 % / 1 unit counts as current; and FadeIn runs once from when a lamp's shadow appeared, live or pool.
// Always (with the F10 profile on): the SHADOW FLIPS log line, how often lamps gained and lost a shadow per second.
#include <unordered_map>
#include "GpuProfiler.h"
#include <vector>

namespace ShadowHold {

	struct State {
		UInt32 frame = 0;			// the last frame (Frame numbering) the lamp had a shadow, live or pool
		UInt32 liveFrame = 0;		// ... in a live slot
		UInt32 sinceFrame = 0;		// the frame since was decided for
		double since = 0.0;			// when its shadow appeared (FadeIn)
		double lastTop = -1e9;		// when it last ranked inside the pool's PoolLamps
	};
	inline std::unordered_map<const void*, State> Map;
	inline UInt32 Frame = 1;		// counts AfterPublish calls
	inline UInt32 StatFrames = 0, StatGained = 0, StatLost = 0, StatLiveGained = 0, StatLiveLost = 0, StatShadowed = 0, StatHeld = 0, StatStale = 0, StatNear = 0, StatCulled = 0, StatDimmed = 0, StatLiveHeld = 0;
	inline double StatStart = 0.0;
	inline std::vector<const void*> Prev, PrevLive;
	inline float (*VisiblePixels)(const void*) = nullptr;	// LampVisibility::Pixels while VisibleRanking measures (1 or 2)
	inline UInt32 StatVisGained = 0, StatVisLost = 0, StatVisShadowed = 0;

	inline double Now() {
		LARGE_INTEGER frequency, counter;
		QueryPerformanceFrequency(&frequency);
		QueryPerformanceCounter(&counter);
		return (double)counter.QuadPart / (double)frequency.QuadPart;
	}
	inline float Seconds() { return (std::max)(0.0f, TheSettingManager->GetSettingF("Shaders.ShadowsInteriors.Forward", "ShadowHold")); }

	// Had a shadow (live or pool) on the last published frame.
	inline bool Continuous(const void* light) { auto f = Map.find(light); return f != Map.end() && f->second.frame == Frame; }

	// FadeIn's start for a lamp published this frame: kept while it has a shadow frame after frame, whichever kind holds it.
	inline double Since(const void* light, double now) {
		State& s = Map[light];
		if (s.sinceFrame != Frame) { if (s.frame != Frame) s.since = now; s.sinceFrame = Frame; }	// (the first call this frame decides)
		return s.since;
	}

	// After Publish + PublishPool (ShadowManager, indoors): the lamps with a shadow this frame. lights: all published, the first liveCount live.
	inline void AfterPublish(const void* const* lights, int count, int liveCount) {
		const UInt32 next = Frame + 1;
		std::vector<const void*> now(lights, lights + count), live(lights, lights + (std::min)(liveCount, count));
		for (const void* l : now) Map[l].frame = next;
		for (const void* l : live) Map[l].liveFrame = next;
		if (GpuTimer::Enabled) {
			auto in = [](const std::vector<const void*>& set, const void* l) { for (const void* x : set) if (x == l) return true; return false; };
			for (const void* l : now) if (!in(Prev, l)) { StatGained++; if (VisiblePixels && VisiblePixels(l) > 0.5f) StatVisGained++; }
			for (const void* l : Prev) if (!in(now, l)) { StatLost++; if (VisiblePixels && VisiblePixels(l) > 0.5f) StatVisLost++; }
			if (VisiblePixels) for (const void* l : now) if (VisiblePixels(l) > 0.5f) StatVisShadowed++;
			for (const void* l : live) if (!in(PrevLive, l)) StatLiveGained++;
			for (const void* l : PrevLive) if (!in(live, l)) StatLiveLost++;
			StatShadowed += (UInt32)now.size();
			const double t = Now();
			if (!StatFrames) StatStart = t;
			if (++StatFrames >= 240) {
				const double seconds = (std::max)(t - StatStart, 1e-3);
				const float shadowed = StatShadowed / (float)StatFrames;
				Logger::Log("SHADOW FLIPS ShadowHold %.2f: %.1f lamps with a shadow per frame; per second %.2f gained / %.2f lost (%.3f flips per lamp per second), "
					"live slots %.2f gained / %.2f lost; held in the pool by ShadowHold %.1f, stale pool maps used %.2f, near-current maps not redrawn %.2f per frame; "
					"per second lamps with a shadow that the game culled %.2f / switched off or dimmed %.2f; live slots kept by ShadowHold (VisibleRanking) %.2f per frame; "
					"lamps lighting something visible: %.1f with a shadow per frame, per second %.2f gained / %.2f lost",
					Seconds(), shadowed, StatGained / seconds, StatLost / seconds, shadowed > 0.0f ? (StatGained + StatLost) / seconds / shadowed : 0.0,
					StatLiveGained / seconds, StatLiveLost / seconds, StatHeld / (float)StatFrames, StatStale / (float)StatFrames, StatNear / (float)StatFrames,
					StatCulled / seconds, StatDimmed / seconds, StatLiveHeld / (float)StatFrames,
					StatVisShadowed / (float)StatFrames, StatVisGained / seconds, StatVisLost / seconds);
				StatVisShadowed = StatVisGained = StatVisLost = 0;
				StatFrames = StatGained = StatLost = StatLiveGained = StatLiveLost = StatShadowed = StatHeld = StatStale = StatNear = StatCulled = StatDimmed = StatLiveHeld = 0;
			}
		}
		Prev.swap(now); PrevLive.swap(live);
		Frame = next;
		if ((Frame & 1023) == 0) for (auto it = Map.begin(); it != Map.end();) it = Frame - it->second.frame > 2048 ? Map.erase(it) : std::next(it);
	}
}
