// Developer switches: wait for a debugger to attach (debug builds only) / hook the D3D9 device creation.
#define NVR_WAIT_FOR_DEBUGGER 0
#define NVR_HOOK_DEVICE 0

#include "../src/NewVegas/Hooks/Hooks.h"
#include "../src/core/Device/Hook.h"
#include "../src/NewVegas/Managers.h"
#include "../src/core/EffectSelfTest.h"
void HdrOutputNoteDesktop();	// in Render.cpp (UNOFFICIAL 2026-10-29)
#include "../src/core/InstallCheck.h"	// 2026-11-03: the install self-check (old DLL / folders / textures / presets), shown on screen
#include "../src/core/SessionLog.h"	// UNOFFICIAL 2026-11-01: session header, game events, exit summary
#include "../src/core/IOWorker2.h"	// UNOFFICIAL 2026-10-20 (EXPERIMENTAL, off by default): a second loading thread for model / texture tasks
#include "../src/core/DxvkLoader.h"	// UNOFFICIAL 2026-11-04: the DXVK fork's d3d9.dll from a mod manager's mod

namespace {
	const UInt32 kPluginVersion = 441;

	// NVR needs some plugins to run at all: without one, say so and quit the game.
	void RequirePlugin(const wchar_t* dllName, const wchar_t* missingText) {
		if (GetModuleHandle(dllName)) return;
		MessageBox(NULL, missingText, L"Vegas Shaders", MB_OK | MB_ICONERROR);
		ExitProcess(0);
	}

	// After every plugin is loaded: find the optional plugins NVR talks to.
	void LinkOptionalPlugins() {
		// JohnnyGuitar NVSE: the view-model clip distance (used by the depth combine)
		CombineDepthEffect* combine = TheShaderManager->Effects.CombineDepth;
		if (!combine->johnnyguitar) combine->johnnyguitar = GetModuleHandle(L"johnnyguitar.dll");
		if (!combine->johnnyguitar) {
			Logger::Log("JG not found");
		}
		else {
			Logger::Log("JG found, initializing functions");
			HMODULE jg = combine->johnnyguitar;
			combine->JG_SetClipDist = reinterpret_cast<bool(__cdecl*)(float)>(GetProcAddress(jg, "JGSetViewmodelClipDistance"));
			combine->JG_GetClipDist = reinterpret_cast<float(__cdecl*)()>(GetProcAddress(jg, "JGGetViewmodelClipDistance"));
		}

		// Vanilla Plus AO present: NVR's ambient occlusion knows the game's AO is replaced
		if (GetModuleHandle(L"VanillaPlusAO.dll")) TheShaderManager->Effects.AmbientOcclusion->bNVAOLoaded = true;

		// (Vanilla Plus Skin is detected earlier, in InitializeRendererHook (core/Hooks/GameCommon.cpp): the game has made all
		// its SKIN shaders by the time this message arrives, so knowing it here would be too late.)

		// Real Time Menus: asks it which menus keep the game running
		if (HMODULE realTimeMenus = GetModuleHandle(L"RealTimeMenus.dll")) {
			Logger::Log("Real Time Menus found, initializing functions");
			TheGameMenuManager->IsLiveMenu = reinterpret_cast<GameMenuManager::MenuPauseState(__cdecl*)(uint32_t, bool, bool)>(GetProcAddress(realTimeMenus, "IsLiveMenu"));
		}

		// every pass of the game's SLS vertex shaders updates the eye position
		ShadowLightShader::EnableEyePositionForAllPasses();
	}
}

namespace { typedef NVSEMessagingInterface Msg; }

extern "C" {

	// xNVSE's own messages.
	static void MessageHandler(NVSEMessagingInterface::Message* message) {
		switch (message->type) {
		case Msg::kMessage_PostLoad:
			RequirePlugin(L"LODFlickerFix.dll", L"LOD Flicker Fix not found.\nVegas Shaders cannot be used without it, please install it.");
			RequirePlugin(L"VanillaPlusTerrain.dll", L"Vanilla Plus Terrain not found.\nVegas Shaders cannot be used without it, please install it.");
#if NVR_TEST_TOOLS
			EffectSelfTest::RegisterLink();	// UNOFFICIAL, TEST ONLY: after every plugin is loaded (critic-notes 14)
#endif
			return;
#if NVR_TEST_TOOLS
		case Msg::kMessage_MainGameLoop:
			SettingShots::MainLoop();	// UNOFFICIAL, TEST ONLY: the auto-test's console commands, between frames (SettingShots.h)
			return;
#endif
		case Msg::kMessage_PreLoadGame:
			TheSettingManager->GameLoading = true;
			SessionLog::OnEvent("loading a save");	// UNOFFICIAL 2026-11-01 (SessionLog.h; the save's name and path are never logged)
			return;
		case Msg::kMessage_PostLoadGame: {
			TheSettingManager->GameLoading = false;
			TheShaderManager->InitializeConstants();
			const bool loaded = ((UInt32)message->data & 0xFF) != 0;	// (xNVSE passes the bool as the pointer's value)
			SessionLog::OnEvent(loaded ? "save loaded" : "save load FAILED");
			return;
		}
		// UNOFFICIAL 2026-11-01: the other game events for the log (SessionLog.h)
		case Msg::kMessage_SaveGame:
			SessionLog::OnEvent("game saved");
			return;
		case Msg::kMessage_NewGame:
			SessionLog::OnEvent("new game");
			return;
		case Msg::kMessage_ExitToMainMenu:
			SessionLog::OnEvent("exit to the main menu");
			return;
		case Msg::kMessage_ExitGame:
			SessionLog::OnExit("exit to Windows");
			return;
		case Msg::kMessage_ExitGame_Console:
			SessionLog::OnExit("exit with the qqq console command");
			return;
		case Msg::kMessage_DeferredInit:
			SessionLog::OnDeferredInit();	// UNOFFICIAL 2026-11-01: game INI values, load order, plugin DLLs
			LinkOptionalPlugins();
			return;
		}
	}

	// The Shader Loader plugin's message 0: it replaced shaders, so refresh the eye-position flags and reload every effect.
	static void ShaderLoaderHandler(NVSEMessagingInterface::Message* message) {
		if (message->type != 0) return;
		ShadowLightShader::EnableEyePositionForAllPasses();
		TheShaderManager->EffectReloadQueued = true;
	}

	bool NVSEPlugin_Query(const PluginInterface* nvse, PluginInfo* info) {
		info->InfoVersion = PluginInfo::kInfoVersion;
		info->Name = "NewVegasReloaded";	// kept after the Vegas Shaders rename: mods look the plugin up by this name
		info->Version = kPluginVersion;
		return true;
	}

	bool NVSEPlugin_Load(const PluginInterface* nvse) {
#if _DEBUG && NVR_WAIT_FOR_DEBUGGER
		while (IsDebuggerPresent() == FALSE) Sleep(10);	// attach now
#endif
#if NVR_HOOK_DEVICE
		AttachDeviceHooks();
#endif

		Logger::InitializeRotating(Paths::LogsFolder, Paths::LogBaseName, 25);	// VegasShaders-Logs\VegasShaders_<date>.log

		PluginVersion::CreateVersionString();
		Logger::Log("%s", PluginVersion::VersionString);	// (not the format: a '%' in a version string would read random arguments)
		Paths::LogChoice();	// 2026-11-02: the folders in use (new Vegas Shaders names, or the old ones of an older install)
		{
			// An old-named DLL left next to this one (or this one under the old name next to a new one) loads the plugin twice.
			char self[MAX_PATH] = {};
			HMODULE module = nullptr;
			GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)&NVSEPlugin_Load, &module);
			GetModuleFileNameA(module, self, MAX_PATH);
			const char* selfName = strrchr(self, '\\') ? strrchr(self, '\\') + 1 : self;
			const std::string other = Paths::ModuleDir() + (_stricmp(selfName, Paths::DllName) == 0 ? Paths::OldDllName : Paths::DllName);
			if (GetFileAttributesA(other.c_str()) != INVALID_FILE_ATTRIBUTES) {
				Logger::Log("[ERROR] PATHS: %s is also in the plugins folder; two copies of the plugin may load. Keep only %s.", other.c_str(), Paths::DllName);
				Paths::TwinDll = other;	// 2026-11-03: the large warning on the main menu and in game (ImGuiManager RenderTwinDllWarning)
			}
		}
		if (!nvse->IsEditor) DxvkLoader::Load();	// before anything asks which d3d9.dll this is, and before the game makes its Direct3D
		InstallCheck::OnPluginLoad(nvse->Version, nvse->GameVersion);
		SessionLog::OnPluginLoad(nvse->Version, nvse->GameVersion, kPluginVersion);	// UNOFFICIAL 2026-11-01: the session header (SessionLog.h)

		CommandManager::Initialize(nvse);
		if (nvse->IsEditor) return true;	// nothing else in the editor

		const PluginHandle self = nvse->GetPluginHandle();
		NVSEMessagingInterface* messaging = (NVSEMessagingInterface*)nvse->QueryInterface(kInterface_Messaging);
		messaging->RegisterListener(self, "NVSE", MessageHandler);
		messaging->RegisterListener(self, "Shader Loader", ShaderLoaderHandler);
		// UNOFFICIAL, TEST ONLY: the auto-test plugin's messages (EffectSelfTest.h); without that plugin nothing is ever sent.
#if NVR_TEST_TOOLS
		EffectSelfTest::LinkMessaging = messaging;
		EffectSelfTest::LinkHandle = self;
#endif
		// (The listener itself is registered at kMessage_PostLoad: xNVSE refuses a listener for a plugin it has not loaded yet,
		// and the load order between this plugin and NVRAutoTest follows the DLL names. critic-notes 14, after the calibration's exit 33.)

		// xNVSE's services: DirectInput control, player controls, console
		if (NVSEDataInterface* data = (NVSEDataInterface*)nvse->QueryInterface(kInterface_Data))
			g_DIHookCtrl = (DIHookControl*)data->GetSingleton(NVSEDataInterface::kNVSEData_DIHookControl);
		g_PlayerControls = (NVSETogglePlayerControlsInterface*)nvse->QueryInterface(kInterface_PlayerControls);
		g_ConsoleInterface = (NVSEConsoleInterface*)nvse->QueryInterface(kInterface_Console);

		// settings first (hooks read them), then presets, then the hooks
		SettingManager::Initialize();
		TheSettingManager->LoadSettings();
		SessionLog::OnSettingsLoaded();	// UNOFFICIAL 2026-11-01: every setting that is not its default
		PresetManager::Initialize();
		AttachHooks();
		HdrOutputNoteDesktop();	// UNOFFICIAL 2026-10-29: Windows' HDR state before the game's fullscreen window changes it (HdrOutput.h)
		IOWorker2::Install();	// UNOFFICIAL 2026-10-20: IOWorker2 = 1 only; must run before the game creates its IOManager
		return true;
	}

};
