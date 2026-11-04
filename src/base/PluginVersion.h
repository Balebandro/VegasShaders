#pragma once

// "<game product> v<version> <NVR product> v<version>" from the version resources of FalloutNV.exe and of this DLL,
// built once at start-up for the log and the console.
class PluginVersion {	// all static
public:
	// 2026-11-04 (a tester: "fatal error ... while loading plugin", the log stopped after its first line, before this string):
	// the appends are bounded (strncat's count was the buffer's size, not the room left, so a long version text ran past the
	// buffer), and a file whose version resource makes Windows' reader fault (a patched or repacked FalloutNV.exe) is skipped
	// and named in the string instead of taking the plugin down.
	static void CreateVersionString() {	// once, at plugin load
		char path[MAX_PATH];

		if (GetModuleFileNameA(NULL, path, MAX_PATH))		// the game
			AddVersionGuarded(path);

		Append(" ");										// between the two

		HMODULE self = NULL;								// the module this function lives in
		const DWORD flags = GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT;
		if (GetModuleHandleExA(flags, (LPCSTR)&CreateVersionString, &self) && GetModuleFileNameA(self, path, MAX_PATH))
			AddVersionGuarded(path);
	}

	// As much of text as fits; the string stays terminated.
	static void Append(const char* text) {
		strncat_s(VersionString, sizeof(VersionString), text, _TRUNCATE);
	}

	// AddVersion, with a fault inside Windows' version reader caught (no C++ objects here, so __try is allowed).
	static void AddVersionGuarded(char* path) {
		__try {
			AddVersion(path);
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
			Append("(version unreadable)");
		}
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
			// (valueSize counts characters; a value need not end in a zero within it, so only that many are taken)
			char text[0x80];
			if (VerQueryValueA(block, "\\StringFileInfo\\040904b0\\ProductName", &value, &valueSize) && value) {
				strncpy_s(text, (const char*)value, valueSize < sizeof(text) ? (size_t)valueSize : sizeof(text) - 1);
				Append(text);
				Append(" v");								// then the version
			}
			if (VerQueryValueA(block, "\\StringFileInfo\\040904b0\\ProductVersion", &value, &valueSize) && value) {
				strncpy_s(text, (const char*)value, valueSize < sizeof(text) ? (size_t)valueSize : sizeof(text) - 1);
				Append(text);
			}
		}
		delete[] block;
	}

	static char VersionString[0x80];	// PluginVersion.cpp
};
