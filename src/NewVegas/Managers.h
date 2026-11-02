#pragma once

// NVR's managers (made at startup, never freed)
class CommandManager; class SettingManager; class ShaderManager; class TextureManager; class RenderManager; class FrameRateManager;
class GameMenuManager; class GameEventManager; class ShadowManager; class CameraManager; class BinkManager;
extern CommandManager		*TheCommandManager;
extern SettingManager		*TheSettingManager;
extern ShaderManager		*TheShaderManager;
extern TextureManager		*TheTextureManager;
extern RenderManager		*TheRenderManager;
extern FrameRateManager		*TheFrameRateManager;
extern GameMenuManager		*TheGameMenuManager;
extern GameEventManager		*TheGameEventManager;
extern ShadowManager		*TheShadowManager;
extern CameraManager		*TheCameraManager;
extern BinkManager			*TheBinkManager;

// xNVSE interfaces (set in NVSEPlugin_Load)
class DIHookControl; struct NVSETogglePlayerControlsInterface; struct NVSEConsoleInterface;
extern DIHookControl						*g_DIHookCtrl;
extern NVSETogglePlayerControlsInterface	*g_PlayerControls;
extern NVSEConsoleInterface					*g_ConsoleInterface;

// the game's singletons (caught as their constructors run: Hooks/Game.cpp, core/Hooks/GameCommon.cpp)
class Main; class TES; class PlayerCharacter; class SceneGraph; class MainDataHandler; class MenuInterfaceManager;
class QueuedModelLoader; class ShadowSceneNode;
extern Main					*Global;
extern TES					*Tes;
extern PlayerCharacter		*Player;
extern SceneGraph			*WorldSceneGraph;
extern MainDataHandler		*DataHandler;
extern MenuInterfaceManager	*InterfaceManager;
extern QueuedModelLoader	*ModelLoader;
extern ShadowSceneNode		*SceneNode;

// the managers' declarations (this order: later ones use earlier ones)
#include "../core/RenderManager.h"
#include "../core/MaxQuality.h"
#include "../core/SettingManager.h"
#include "../core/CommandManager.h"
#include "../core/PresetManager.h"
#include "../core/TextureRecord.h"
#include "../core/ShaderManager.h"
#include "../core/TextureManager.h"
#include "../core/FrameRateManager.h"
#include "../core/GameEventManager.h"
#include "../core/GameMenuManager.h"
#include "../core/ImGuiManager.h"
#include "nvse/DIHookControl.h"
#include "../core/RenderPass.h"
#include "../core/ShadowManager.h"
#include "../core/CameraManager.h"
#include "../core/BinkManager.h"

void InitializeManagers();	// all managers, in dependency order (after the renderer exists)
