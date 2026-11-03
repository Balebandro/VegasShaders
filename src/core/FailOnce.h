#pragma once

// UNOFFICIAL 2026-11-02 [Main.Main.Misc] RetryFailedEffects (hidden-state review, docs/next/HIDDEN-STATE-2026-11-02.md): effects
// that switch a part of themselves off after one failure (a target or texture that could not be made, a pass the driver refused)
// kept a private flag that never cleared, so a single bad moment (a load spike, a full video memory) left that part off until the
// game restarted, with one log line and nothing in the menu. A Latch here does the same on the frame it fails, then lets the code
// try again after the next cell change, up to kTries times in all; every failure is logged and listed in the menu's status bar.
// RetryFailedEffects false = as before: the first failure holds for the session (the notice is still listed).
//
// Use: `FailOnce::Latch Fail{"GodRays dedicated pass"};` in place of `bool failed`; `if (Fail.Blocked()) return;` where the flag
// was tested; `Fail.Set("could not make the target");` where it was set. The name and the reason must be literals (kept as pointers).

#include <string>
#include <vector>

namespace FailOnce {

	constexpr int kTries = 3;
	inline unsigned CellSerial = 0;	// bumped by ShaderManager on the first call of a frame with a cell change (NextCell)
	inline std::vector<std::string> Notices;	// one line per part switched off, for the status bar (ImGuiManager)

	inline void NextCell() { CellSerial++; }
	inline bool RetryOn() { return TheSettingManager->GetSettingI("Main.Main.Misc", "RetryFailedEffects") != 0; }

	inline void Notice(const char* name, const char* why, bool final) {
		char line[320];
		sprintf_s(line, "%s: %s; %s", name, why ? why : "failed", final ? "off until the game restarts" : "tried again after the next cell change");
		for (std::string& n : Notices) if (n.compare(0, strlen(name) + 1, std::string(name) + ":") == 0) { n = line; return; }
		Notices.push_back(line);
	}

	struct Latch {
		const char* name;
		mutable bool failed = false;	// mutable: Blocked() lets a retry through from const readers (CoverMap)
		mutable unsigned cell = 0;
		mutable int tries = 0;

		explicit Latch(const char* n) : name(n) {}

		// true while the part stays off: failed in this cell, out of tries, or RetryFailedEffects off.
		bool Blocked() const {
			if (!failed) return false;
			if (cell == CellSerial || tries >= kTries || !RetryOn()) return true;
			failed = false;
			Logger::Log("UNOFFICIAL RetryFailedEffects: %s tried again (attempt %d of %d) after a cell change", name, tries + 1, kTries);
			return false;
		}
		void Set(const char* why = nullptr) {
			if (failed) return;
			failed = true; cell = CellSerial; tries++;
			const bool final = tries >= kTries || !RetryOn();
			Logger::Log("[WARNING] UNOFFICIAL %s: %s; %s", name, why ? why : "failed",
				final ? "off for the session" : "off until the next cell change, then tried again ([Main.Main.Misc] RetryFailedEffects)");
			Notice(name, why, final);
		}
		void Clear() { failed = false; }	// a success elsewhere (the part works again)
	};
}
