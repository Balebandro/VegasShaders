// Storage for the pointers declared in Managers.h: all null until startup fills them.

DIHookControl*						g_DIHookCtrl = nullptr;
NVSETogglePlayerControlsInterface*	g_PlayerControls = nullptr;
NVSEConsoleInterface*				g_ConsoleInterface = nullptr;

CommandManager*		TheCommandManager = nullptr;
SettingManager*		TheSettingManager = nullptr;
ShaderManager*		TheShaderManager = nullptr;
TextureManager*		TheTextureManager = nullptr;
RenderManager*		TheRenderManager = nullptr;
FrameRateManager*	TheFrameRateManager = nullptr;
GameEventManager*	TheGameEventManager = nullptr;
GameMenuManager*	TheGameMenuManager = nullptr;
ShadowManager*		TheShadowManager = nullptr;
CameraManager*		TheCameraManager = nullptr;
BinkManager*		TheBinkManager = nullptr;

Main*					Global = nullptr;
TES*					Tes = nullptr;
PlayerCharacter*		Player = nullptr;
SceneGraph*				WorldSceneGraph = nullptr;
MainDataHandler*		DataHandler = nullptr;
MenuInterfaceManager*	InterfaceManager = nullptr;
QueuedModelLoader*		ModelLoader = nullptr;
ShadowSceneNode*		SceneNode = nullptr;

// Called once the game's renderer exists (InitializeRendererHook). Textures come first (the shader manager binds them),
// the bink player last (it loads a shader).
void InitializeManagers() {
	static void (* const startOrder[])() = {
		&TextureManager::Initialize,
		&ShaderManager::Initialize,
		&FrameRateManager::Initialize,
		&GameMenuManager::Initialize,
		&GameEventManager::Initialize,
		&ShadowManager::Initialize,
		&CameraManager::Initialize,
		&BinkManager::Initialize,
	};
	for (auto start : startOrder) start();
}
