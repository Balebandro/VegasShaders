#pragma once

// "<game product> v<version> <NVR product> v<version>" from the version resources of FalloutNV.exe and of this DLL,
// built once at start-up for the log and the console.
class PluginVersion {	// all static
public:
	static void CreateVersionString() {	// once, at plugin load
		char path[MAX_PATH];

		if (GetModuleFileNameA(NULL, path, MAX_PATH))		// the game
			AddVersion(path);

		strncat(VersionString, " ", sizeof(VersionString));	// between the two

		HMODULE self = NULL;								// the module this function lives in
		const DWORD flags = GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT;
		if (GetModuleHandleExA(flags, (LPCSTR)&CreateVersionString, &self) && GetModuleFileNameA(self, path, MAX_PATH))
			AddVersion(path);
	}

	// Appends "<ProductName> v<ProductVersion>" (US English block) of the file, as far as the file has them.
	static void AddVersion(char* path) {
		DWORD handle = NULL;
		const DWORD size = GetFileVersionInfoSizeA(path, &handle);
		if (!size) return;

		char* block = new char[size];
		if (GetFileVersionInfoA(path, handle, size, block)) {
			void* value = NULL;
			UINT valueSize = 0;
			if (VerQueryValueA(block, "\\StringFileInfo\\040904b0\\ProductName", &value, &valueSize)) {
				strncat(VersionString, (char*)value, sizeof(VersionString));
				strncat(VersionString, " v", sizeof(VersionString));	// then the version
			}
			if (VerQueryValueA(block, "\\StringFileInfo\\040904b0\\ProductVersion", &value, &valueSize))
				strncat(VersionString, (char*)value, sizeof(VersionString));
		}
		delete[] block;
	}

	static char VersionString[0x80];	// PluginVersion.cpp
};
