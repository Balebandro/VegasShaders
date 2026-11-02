#include "Paths.h"

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
			data = Choose("Data\\VegasShaders\\", "Data\\NVR\\");
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

void LogChoice() {
	const State& s = S();
	auto tag = [](const Folder& f) { return f.old ? " (old name)" : f.found ? "" : " (not there yet)"; };
	Logger::Log("PATHS folders: shaders %s%s, textures %s%s, data %s%s; logs %s",
		s.shaders.path.c_str(), tag(s.shaders), s.textures.path.c_str(), tag(s.textures), s.data.path.c_str(), tag(s.data), LogsFolder);
}

const char* NewName(File file) { return FileNames[(int)file].now; }
const char* OldName(File file) { return FileNames[(int)file].old; }

std::string ReadPath(const std::string& folder, File file) {
	const std::string now = folder + NewName(file);
	if (FileExists(now)) return now;
	const std::string old = folder + OldName(file);
	if (FileExists(old)) return old;
	return now;
}

std::string WritePath(const std::string& folder, File file) { return folder + NewName(file); }

std::string OldPathIfOnlyOld(const std::string& folder, File file) {
	const std::string now = folder + NewName(file), old = folder + OldName(file);
	return !FileExists(now) && FileExists(old) ? old : std::string();
}

}
