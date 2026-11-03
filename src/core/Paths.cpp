#include "Paths.h"
#include <cstdarg>

// Paths.h explains the rules. The folder choice is made once, at first use (the plugin's load, Main.cpp), and kept for the session.

namespace Paths {

namespace {

	bool Exists(const std::string& relative) {
		const std::string full = GameRoot() + "\\" + relative;
		return GetFileAttributesA(full.c_str()) != INVALID_FILE_ATTRIBUTES;
	}

	bool FileExists(const std::string& path) {
		const DWORD attributes = GetFileAttributesA(path.c_str());
		return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
	}

	// The last write time, 0 if the file is missing.
	ULONGLONG WriteTime(const std::string& path) {
		WIN32_FILE_ATTRIBUTE_DATA data;
		if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &data)) return 0;
		return (ULONGLONG(data.ftLastWriteTime.dwHighDateTime) << 32) | data.ftLastWriteTime.dwLowDateTime;
	}

	// The user's own files: a newer old-named one is read over ours (a preset made for New Vegas Reloaded, copied in later).
	bool NewerOldWins(File file) { return file == File::UserSettings || file == File::MaxQualityUser; }

	struct Folder {
		std::string path;	// relative to the game folder, trailing backslash
		bool old = false;	// the old name is in use
		bool found = true;	// false: neither exists (the new name is used, and made where the code makes folders)
	};

	Folder Choose(const char* newPath, const char* oldPath) {
		Folder f;
		if (Exists(newPath)) f.path = newPath;
		else if (Exists(oldPath)) { f.path = oldPath; f.old = true; }
		else { f.path = newPath; f.found = false; }
		return f;
	}

	struct State {
		Folder shaders, textures, data;
		std::string shadersDir, effectsDir, computeDir, lutDir, shaderShort;
		State() {
			shaders = Choose("Data\\Shaders\\VegasShaders\\", "Data\\Shaders\\NewVegasReloaded\\");
			textures = Choose("Data\\Textures\\VegasShaders\\", "Data\\Textures\\NewVegasReloaded\\");
			data = Choose(NewDataRoot, OldDataRoot);
			shadersDir = shaders.path + "Shaders\\";
			effectsDir = shaders.path + "Effects\\";
			computeDir = shaders.path + "Compute\\";
			lutDir = textures.path + "LUTs\\";
			shaderShort = shaders.path.substr(5);	// without "Data\"
		}
	};

	const State& S() {
		static const State state;
		return state;
	}

	struct Names { const char* now; const char* old; };
	const Names FileNames[] = {
		{ "VegasShaders.toml", "NewVegasReloaded.dll.toml" },
		{ "VegasShaders.defaults.toml", "NewVegasReloaded.dll.defaults.toml" },
		{ "VegasShaders.maxquality.toml", "NewVegasReloaded.dll.maxquality.toml" },
		{ "VegasShaders.maxquality.user.toml", "NewVegasReloaded.dll.maxquality.user.toml" },
		{ "VegasShaders.materials.txt", "NewVegasReloaded.dll.materials.txt" },
		{ "VegasShaders.imgui.ini", "NewVegasReloaded.imgui.ini" },
	};
}

const std::string& GameRoot() {
	static const std::string root = [] {
		char buffer[MAX_PATH] = {};
		GetCurrentDirectoryA(MAX_PATH, buffer);
		std::string r(buffer);
		while (!r.empty() && (r.back() == '\\' || r.back() == '/')) r.pop_back();
		return r;
	}();
	return root;
}

std::string PluginsDir() { return GameRoot() + "\\Data\\NVSE\\Plugins\\"; }

std::string ModuleDir() {
	HMODULE self = nullptr;
	GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)&ModuleDir, &self);
	char path[MAX_PATH] = {};
	const DWORD length = GetModuleFileNameA(self, path, MAX_PATH);
	if (!length || length >= MAX_PATH) return PluginsDir();
	const std::string file(path, length);
	const size_t slash = file.find_last_of("\\/");
	return slash == std::string::npos ? PluginsDir() : file.substr(0, slash + 1);
}

const char* ShaderRoot() { return S().shaders.path.c_str(); }
const char* ShadersDir() { return S().shadersDir.c_str(); }
const char* EffectsDir() { return S().effectsDir.c_str(); }
const char* ComputeDir() { return S().computeDir.c_str(); }
const char* TextureRoot() { return S().textures.path.c_str(); }
const char* LutDir() { return S().lutDir.c_str(); }
const char* DataRoot() { return S().data.path.c_str(); }
const char* ShaderRootShort() { return S().shaderShort.c_str(); }

namespace {

	std::vector<std::string> gNotices;

	FILETIME WriteFileTime(const std::string& path) {
		WIN32_FILE_ATTRIBUTE_DATA data;
		if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &data)) return FILETIME{};
		return data.ftLastWriteTime;
	}

	// Shader files in the old folder (Data\Shaders\NewVegasReloaded\) newer than ours, or than this DLL where we have no such
	// file: put there by a New Vegas Reloaded preset after this mod was installed (an older install's own files are older). They
	// are made for that mod's code and would not work with ours, so they stay unused; the notice says so and names them.
	void CheckOldShaderFiles() {
		const State& s = S();
		const char* oldRoot = "Data\\Shaders\\NewVegasReloaded\\";
		if (s.shaders.old || !s.shaders.found) return;	// the old folder is the one in use, or neither exists
		const std::string oldFull = GameRoot() + "\\" + oldRoot, newFull = GameRoot() + "\\" + s.shaders.path;
		char dll[MAX_PATH] = {};
		HMODULE self = nullptr;
		GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)&CheckOldShaderFiles, &self);
		GetModuleFileNameA(self, dll, MAX_PATH);
		const FILETIME installed = WriteFileTime(dll);
		std::vector<std::string> found;
		std::vector<std::string> pending = { "" };	// sub-folders, relative, with a trailing backslash
		while (!pending.empty()) {
			const std::string sub = pending.back();
			pending.pop_back();
			WIN32_FIND_DATAA entry;
			const HANDLE search = FindFirstFileA((oldFull + sub + "*").c_str(), &entry);
			if (search == INVALID_HANDLE_VALUE) continue;
			do {
				const std::string name = entry.cFileName;
				if (name == "." || name == "..") continue;
				if (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
					if (_stricmp(name.c_str(), "Cache")) pending.push_back(sub + name + "\\");
					continue;
				}
				const size_t dot = name.find_last_of('.');
				if (dot == std::string::npos || _stricmp(name.c_str() + dot, ".hlsl")) continue;
				const std::string ours = newFull + sub + name;
				const FILETIME reference = FileExists(ours) ? WriteFileTime(ours) : installed;
				if (CompareFileTime(&entry.ftLastWriteTime, &reference) > 0) found.push_back(sub + name);
			} while (FindNextFileA(search, &entry));
			FindClose(search);
		}
		if (found.empty()) return;
		std::string list;
		for (const std::string& f : found) list += (list.empty() ? "" : ", ") + f;
		Logger::Log("PATHS %d shader file(s) in %s are newer than this mod's (from a preset made for New Vegas Reloaded?) and are not "
			"used: they are written for that mod's code. %s", (int)found.size(), oldRoot, list.c_str());
		AddOldFilesNotice("%d shader file(s) from a New Vegas Reloaded preset not used (made for NVR's code): %s", (int)found.size(), list.c_str());
	}
}

void LogChoice() {
	const State& s = S();
	auto tag = [](const Folder& f) { return f.old ? " (old name)" : f.found ? "" : " (not there yet)"; };
	Logger::Log("PATHS folders: shaders %s%s, textures %s%s, data %s%s; logs %s",
		s.shaders.path.c_str(), tag(s.shaders), s.textures.path.c_str(), tag(s.textures), s.data.path.c_str(), tag(s.data), LogsFolder);
	CheckOldShaderFiles();
}

void AddOldFilesNotice(const char* format, ...) {
	char text[1024];
	va_list args;
	va_start(args, format);
	vsnprintf_s(text, _TRUNCATE, format, args);
	va_end(args);
	for (const std::string& known : gNotices) if (known == text) return;	// the settings are read again on every reload
	gNotices.emplace_back(text);
}

const std::vector<std::string>& OldFilesNotices() { return gNotices; }

const char* NewName(File file) { return FileNames[(int)file].now; }
const char* OldName(File file) { return FileNames[(int)file].old; }

bool OldIsNewer(const std::string& folder, File file) {
	if (!NewerOldWins(file)) return false;
	const std::string now = folder + NewName(file), old = folder + OldName(file);
	return FileExists(now) && FileExists(old) && WriteTime(old) > WriteTime(now);
}

std::string ReadPath(const std::string& folder, File file) {
	const std::string now = folder + NewName(file);
	const std::string old = folder + OldName(file);
	if (FileExists(now)) return OldIsNewer(folder, file) ? old : now;
	if (FileExists(old)) return old;
	return now;
}

std::string WritePath(const std::string& folder, File file) { return folder + NewName(file); }

std::string OldPathIfRead(const std::string& folder, File file) {
	const std::string old = folder + OldName(file);
	return ReadPath(folder, file) == old ? old : std::string();
}

}
