#pragma once

// UNOFFICIAL 2026-10-15 Max Quality ([Main.Main.Quality] MaxQuality, read once at start: it needs a restart). For people
// taking screenshots rather than playing: every effect normally worked out at half or a quarter of the screen size runs at
// the full size, and VegasShaders.maxquality.toml's settings are read in place of the user's (SettingManager's
// FillNode; never written to their file, so turning it off gives their own settings back). The effects' shaders take the
// size from the constants their C++ sets, so only the sizes below change.
namespace MaxQuality {
	inline bool Active = false;	// set by SettingManager::Configuration::Init, before any effect registers its textures

	// The divisor an effect works at: its own (2 for half size, 4 for a quarter), or 1 with Max Quality on.
	inline UINT Divisor(UINT normal) { return Active ? 1u : normal; }

	// One side of a target made at full / divisor (rounded up), or the full side with Max Quality on.
	inline UINT Reduced(UINT full, UINT divisor) { divisor = Divisor(divisor); return (full + divisor - 1) / divisor; }
}
