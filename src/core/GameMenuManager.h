#pragma once

// What is left of the pre-ImGui text menu: the settings overlay is ImGuiManager now.
// This object only carries the engine's "is this menu live" query (resolved by Main.cpp
// from the runtime module, may stay null) and the selection hook SettingManager calls
// after a settings reload. One instance, created at startup, lives for the whole session.
class GameMenuManager {
public:
	static void Initialize();

	// Called by SettingManager::LoadSettings after a reload. The text menu clamped its row and page cursors here; the ImGui
	// overlay keeps its own state, so there is nothing left to check.
	void ValidateSelection() {}

	// How a game menu behaves while open, as the engine reports it
	enum MenuPauseState : uint32_t {
		MENU_PAUSED = 0, // game stops; only MenuMode scripts run
		MENU_LIVE = 1,   // game keeps going; GameMode and MenuMode both run
		MENU_MIXED = 2,  // game keeps going; only MenuMode runs
	};

	typedef MenuPauseState(__cdecl* LiveMenuQuery)(uint32_t menuType, bool checkInstances, bool gameModeCheck);

	LiveMenuQuery IsLiveMenu = nullptr;
};
