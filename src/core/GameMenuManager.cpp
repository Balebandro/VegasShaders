// The old D3DX text menu was replaced by the ImGui overlay (ImGuiManager); its drawing, key handling and layout code is gone.
// See GameMenuManager.h for what remains.

void GameMenuManager::Initialize() {
	Logger::Log("Starting the menu manager...");
	TheGameMenuManager = new GameMenuManager;	// IsLiveMenu starts null; Main.cpp fills it in when the runtime module has it
}
