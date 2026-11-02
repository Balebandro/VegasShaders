#pragma once
#include <cstdio>

// UNOFFICIAL 2026-10-18 (framefix): what the temporal effects that keep last frame's result (VolumetricLight's march, Reflections)
// did with their history, counted for the log: one line at each settings change, and every 240 drawn frames with the F10 profile on.
// Their frame counter is TheFrameRateManager->FrameNumber (advanced once per rendered frame, in UpdatePerformance). It used to be a
// `frame++` in UpdateConstants, which runs twice a frame, so 'lastRenderedFrame + 1 == frame' could never pass and the history was
// never used. A frame is counted once, under the first reason that applies, in the order of the checks in Note.
struct TemporalHistoryStats {
	unsigned int drawn = 0, used = 0, off = 0, noHistory = 0, notLastFrame = 0, jumped = 0;

	void Note(bool temporalOn, bool haveHistory, bool drewLastFrame, bool usable) {
		drawn++;
		if (usable) used++;
		else if (!temporalOn) off++;
		else if (!haveHistory) noHistory++;			// the first frame, or the history was dropped (another path ran, a switch)
		else if (!drewLastFrame) notLastFrame++;	// the effect did not draw last frame (shadows faded, a menu, a loading screen)
		else jumped++;								// the camera moved too far since last frame (load, fast travel, a cut)
	}
	void Reset() { drawn = used = off = noHistory = notLastFrame = jumped = 0; }
	// "N frames drawn, history used on K (P%); not used: ..." (no leading name)
	void Describe(char* text, size_t size) const {
		sprintf_s(text, size, "%u frames drawn, history used on %u (%.0f%%); not used: %u temporal off, %u no history yet, %u not drawn last frame, %u camera jump",
			drawn, used, drawn ? 100.0 * used / drawn : 0.0, off, noHistory, notLastFrame, jumped);
	}
};
