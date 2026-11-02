#pragma once
#include <string>

// 2026-11-02 Vegas Shaders: every file and folder the plugin reads or writes at run time, in one place. The project was New Vegas
// Reloaded; its old names are still read as a fallback, so an install that only copies the new DLL in keeps working:
//   folders      the new one if it exists, else the old one if it exists, else the new one (decided once, logged in one line);
//   plugin files read the new name, else the old one; always write the new name. An old file is never changed or deleted;
//   logs         new folder and names only (a log is only ever written).
// Kept as they were, on purpose: the NVSE plugin name (Main.cpp, mods check for it), the console prefix NVR, the legacy .config
// file's name (only read), settings sections and keys, TESR_* names and the log line tags.
namespace Paths {

	// ---- logs (no fallback) -------------------------------------------------------------------------------------------------
	// A macro as well, so a format string can be built from it at compile time: ".\\" VS_LOGS_FOLDER "\\name_%s.png".
	#define VS_LOGS_FOLDER "VegasShaders-Logs"
	constexpr const char* LogsFolder = VS_LOGS_FOLDER;	// in the game folder
	constexpr const char* LogBaseName = "VegasShaders";	// VegasShaders_<date>.log
	// The old logs folder, only READ: other tools (FNVProbe, the auto-test plugin) still write their own logs there.
	constexpr const char* OldLogsFolder = "NVR-Unofficial-Optimized-Logs";

	// ---- folders, relative to the game folder, with a trailing backslash ------------------------------------------------------
	const char* ShaderRoot();	// Data\Shaders\VegasShaders\        (old: Data\Shaders\NewVegasReloaded\)
	const char* ShadersDir();	// <ShaderRoot>Shaders\ .
	const char* EffectsDir();	// <ShaderRoot>Effects\ .
	const char* ComputeDir();	// <ShaderRoot>Compute\ .
	const char* TextureRoot();	// Data\Textures\VegasShaders\       (old: Data\Textures\NewVegasReloaded\)
	const char* LutDir();		// <TextureRoot>LUTs\ .
	const char* DataRoot();		// Data\VegasShaders\                (old: Data\NVR\): Presets, Variants, Keywords, Skins

	// The shader folder as messages name it, without the leading "Data\" ("Shaders\VegasShaders\").
	const char* ShaderRootShort();

	// One log line with the folders in use and which of them have the old name. Call once the log is open.
	void LogChoice();

	// ---- files in the plugins folder ------------------------------------------------------------------------------------------
	enum class File {
		UserSettings,		// VegasShaders.toml                 (old: NewVegasReloaded.dll.toml)
		Defaults,			// VegasShaders.defaults.toml        (old: NewVegasReloaded.dll.defaults.toml)
		MaxQuality,			// VegasShaders.maxquality.toml      (old: NewVegasReloaded.dll.maxquality.toml)
		MaxQualityUser,		// VegasShaders.maxquality.user.toml (old: NewVegasReloaded.dll.maxquality.user.toml)
		Materials,			// VegasShaders.materials.txt        (old: NewVegasReloaded.dll.materials.txt)
		ImGuiLayout,		// VegasShaders.imgui.ini            (old: NewVegasReloaded.imgui.ini)
	};
	const char* NewName(File file);
	const char* OldName(File file);

	// folder ends with a backslash. ReadPath: the new name if that file exists, else the old name if that one exists, else the
	// new name. WritePath: always the new name. OldPathIfOnlyOld: the old file's path when only the old one exists, else "".
	std::string ReadPath(const std::string& folder, File file);
	std::string WritePath(const std::string& folder, File file);
	std::string OldPathIfOnlyOld(const std::string& folder, File file);

	// The game folder (the current folder at first use, without a trailing backslash: common dialogs can change the current
	// folder later), its Data\NVSE\Plugins\ folder, and the folder this DLL was loaded from (usually the same plugins folder).
	const std::string& GameRoot();
	std::string PluginsDir();
	std::string ModuleDir();

	// The legacy settings file of the original New Vegas Reloaded keeps its name; it is only ever read.
	constexpr const char* LegacyConfigFile = "NewVegasReloaded.dll.config";

	// The plugin DLL's file name, and the old one: both in one plugins folder would load the plugin twice (Main.cpp logs it).
	constexpr const char* DllName = "VegasShaders.dll";
	constexpr const char* OldDllName = "NewVegasReloaded.dll";
}
