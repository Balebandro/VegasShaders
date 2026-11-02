#pragma once

// The settings menu's tool windows, opened from its Tools menu: Presets, Lighting, Dev Tools and the Retro-Encabulator.
// Each is its own ImGui window, drawn after the main menu window every frame the menu is open.
namespace MenuTools {
	// Which windows are open (the Tools menu flips these)
	extern bool PresetsOpen;
	extern bool LightingOpen;
	extern bool DevToolsOpen;
	extern bool EncabOpen;
	extern bool AboutOpen;		// Help > About: the project, its version and the credits

	// Dev Tools > Screenshot mode: the menu hides and the game gets its input back until the menu key is pressed
	extern bool ScreenshotMode;

	bool EncabActive();					// the Retro-Encabulator is driving its settings
	bool FreecamOn();					// Dev Tools turned the free camera on
	void FrameUpdate();					// every frame the menu is open, before anything is drawn (encabulator drift)
	void TrackCell();					// every drawn menu frame: remembers interiors for the Dev Tools jump list
	void DrawWindows(float stepSize);	// after the main window: every open tool window and the preset confirm popup
	void OnMenuClosed();				// undo what the tools left switched on (encabulator, HUD, freecam)
	void OnDevToolsClosed();			// the Dev Tools window was closed from the menu
	bool HandleWindowMessage(UINT msg, LPARAM lParam);	// WndProc: true when msg was a deferred console command (handled)
}
