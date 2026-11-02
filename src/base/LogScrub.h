#pragma once
// UNOFFICIAL 2026-11-01: the log privacy scrubber. Players post their VegasShaders_*.log files in public (Discord, GitHub), so
// every line Logger writes (file and in-game log window) passes through Scrubber::Apply first. It replaces:
//   - the game folder (the exe's folder, its 8.3 short form, the current folder if different)      -> <GAME>
//   - the Windows user profile folder (and its 8.3 short form)                                     -> %USERPROFILE%
//   - Documents / AppData / LocalAppData / Temp when they live outside the profile                 -> %DOCUMENTS% etc.
//   - any other X:\Users\<name>, X:\Documents and Settings\<name>, Z:\home\<name>, /home/<name>,
//     /Users/<name> (another account, a moved profile, Wine / Proton / CrossOver)                  -> %USERPROFILE%
//   - Mod Organizer profile folders: ...\profiles\<name>                                           -> ...\profiles\<PROFILE>
//   - Steam account ids: userdata\<digits> and 17-digit SteamID64s (7656119...)                    -> <STEAMID>
//   - the Windows user name and the computer name anywhere in a line, as a whole word              -> <USER> / <PC>
// Paths compare case-insensitively and '\' matches '/', so "C:/Users/Name/..." is caught by the same pattern. Relative paths
// (Data\Textures\...) are left as they are. The patterns are built once (Build), Apply is const (any thread) and costs one pass
// over the line; a line with nothing to replace is not copied. Unit-tested in tests/log_scrub.cpp. Free of engine types.
#include <windows.h>
#include <shlobj.h>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>
#include <vector>

namespace LogScrub {

	inline char Lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }
	inline bool IsSep(char c) { return c == '\\' || c == '/'; }
	inline bool IsWordChar(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || (unsigned char)c >= 0x80; }
	inline bool SameChar(char a, char b) { return Lower(a) == Lower(b) || (IsSep(a) && IsSep(b)); }

	// A path segment (a folder name, which may hold spaces and apostrophes) ends at a separator, a double quote, a bracket, a
	// comma or the end. (A name in single quotes takes the closing quote and what follows up to one of those with it: too much
	// is scrubbed, never too little.)
	inline size_t SegmentEnd(const char* s, size_t i, size_t n) {
		while (i < n) {
			const char c = s[i];
			if (IsSep(c) || c == '"' || c == ')' || c == '(' || c == ']' || c == '[' || c == ',' || c == ';' || c == '|' ||
				c == '<' || c == '>' || c == '\n' || c == '\r' || c == '\t') break;
			i++;
		}
		return i;
	}

	// Case-insensitive compare of `word` (lower case) at s[i..]; separators match each other.
	inline bool MatchAt(const char* s, size_t i, size_t n, const char* word, size_t length) {
		if (i + length > n) return false;
		for (size_t k = 0; k < length; k++) if (!SameChar(s[i + k], word[k])) return false;
		return true;
	}

	class Scrubber {
	public:
		// A folder to replace with `token` wherever it appears (case-insensitive, either separator). Trailing separators are dropped;
		// anything shorter than 4 characters (a bare "C:\") is ignored.
		void AddPath(std::string path, const char* token) {
			while (!path.empty() && IsSep(path.back())) path.pop_back();
			if (path.size() < 4) return;
			for (char& c : path) c = IsSep(c) ? '\\' : Lower(c);
			for (const Pattern& p : Paths) if (p.text == path) return;
			Paths.push_back({ path, token });
		}
		// A name (user, computer) to replace with `token` as a whole word anywhere. Names under 3 characters and generic account
		// names (which identify no one, and are ordinary words in the log) are left to the path rules.
		void AddWord(std::string word, const char* token) {
			if (word.size() < 3) return;
			for (char& c : word) c = Lower(c);
			static const char* const generic[] = { "user", "users", "admin", "administrator", "owner", "default", "guest", "public",
				"home", "steamuser", "deck", "root", "desktop", "localhost", "player", "gamer", "windows", "local", "system" };
			for (const char* g : generic) if (word == g) return;
			for (const Pattern& p : Words) if (p.text == word) return;
			Words.push_back({ word, token });
		}
		// Longest first, so the game folder inside the profile becomes <GAME>, not %USERPROFILE%\...
		void Finish() {
			auto longer = [](const Pattern& a, const Pattern& b) { return a.text.size() > b.text.size(); };
			std::sort(Paths.begin(), Paths.end(), longer);
			std::sort(Words.begin(), Words.end(), longer);
			memset(First, 0, sizeof(First));
			for (const Pattern& p : Paths) { First[(unsigned char)p.text[0]] = 1; First[(unsigned char)(p.text[0] - 32 * (p.text[0] >= 'a' && p.text[0] <= 'z'))] = 1; }
			for (const Pattern& p : Words) { First[(unsigned char)p.text[0]] = 1; First[(unsigned char)(p.text[0] - 32 * (p.text[0] >= 'a' && p.text[0] <= 'z'))] = 1; }
			for (const char c : { '\\', '/', ':', '7', 'u', 'U' }) First[(unsigned char)c] = 1;	// the generic rules
			Ready = true;
		}
		bool Empty() const { return Paths.empty() && Words.empty(); }

		// Scrubs s[0..n). Returns false (and leaves `out` alone) when nothing needed replacing; else `out` holds the scrubbed text.
		bool Apply(const char* s, size_t n, std::string& out) const {
			bool changed = false;
			size_t copied = 0;	// s[0..copied) is in out already (once changed)
			for (size_t i = 0; i < n; ) {
				const unsigned char c = (unsigned char)s[i];
				// A drive letter "X:" may start a path rule (the test is on the letter, so check the next character too)
				const bool drive = i + 2 < n && s[i + 1] == ':' && IsSep(s[i + 2]) && ((c | 32) >= 'a' && (c | 32) <= 'z');
				if (!drive && !(Ready ? First[c] : 1)) { i++; continue; }
				size_t end = 0;
				const char* token = Match(s, i, n, end);
				if (!token) { i++; continue; }
				if (!changed) { out.clear(); out.reserve(n + 16); changed = true; }
				out.append(s + copied, i - copied);
				out.append(token);
				copied = i = end;
			}
			if (changed) out.append(s + copied, n - copied);
			return changed;
		}

		// Patterns from this machine: the exe's folder, the current folder, the profile and known folders, the user and computer names.
		void BuildFromSystem() {
			char buffer[MAX_PATH * 2];
			auto addWithShort = [&](const std::string& path, const char* token) {
				if (path.empty()) return;
				AddPath(path, token);
				char shortPath[MAX_PATH * 2] = {};
				const DWORD length = GetShortPathNameA(path.c_str(), shortPath, sizeof(shortPath));
				if (length && length < sizeof(shortPath)) AddPath(shortPath, token);
				char longPath[MAX_PATH * 2] = {};
				const DWORD longLength = GetLongPathNameA(path.c_str(), longPath, sizeof(longPath));
				if (longLength && longLength < sizeof(longPath)) AddPath(longPath, token);
			};
			const DWORD exeLength = GetModuleFileNameA(NULL, buffer, MAX_PATH);
			if (exeLength && exeLength < MAX_PATH) {
				std::string exe(buffer, exeLength);
				const size_t slash = exe.find_last_of("\\/");
				if (slash != std::string::npos) addWithShort(exe.substr(0, slash), "<GAME>");
			}
			const DWORD cwdLength = GetCurrentDirectoryA(MAX_PATH, buffer);
			if (cwdLength && cwdLength < MAX_PATH) addWithShort(std::string(buffer, cwdLength), "<GAME>");
			struct Known { int csidl; const char* token; };
			const Known known[] = { { CSIDL_PROFILE, "%USERPROFILE%" }, { CSIDL_PERSONAL, "%DOCUMENTS%" }, { CSIDL_APPDATA, "%APPDATA%" },
				{ CSIDL_LOCAL_APPDATA, "%LOCALAPPDATA%" }, { CSIDL_DESKTOPDIRECTORY, "%DESKTOP%" } };
			for (const Known& k : known)
				if (SUCCEEDED(SHGetFolderPathA(NULL, k.csidl, NULL, SHGFP_TYPE_CURRENT, buffer))) addWithShort(buffer, k.token);
			for (const char* variable : { "USERPROFILE", "HOMEPATH", "OneDrive", "OneDriveConsumer", "OneDriveCommercial" }) {
				const DWORD length = GetEnvironmentVariableA(variable, buffer, MAX_PATH);
				if (length && length < MAX_PATH) addWithShort(std::string(buffer, length), !strcmp(variable, "USERPROFILE") || !strcmp(variable, "HOMEPATH") ? "%USERPROFILE%" : "%ONEDRIVE%");
			}
			const DWORD tempLength = GetTempPathA(MAX_PATH, buffer);
			if (tempLength && tempLength < MAX_PATH) addWithShort(std::string(buffer, tempLength), "%TEMP%");
			DWORD size = sizeof(buffer);
			if (GetUserNameA(buffer, &size)) AddWord(buffer, "<USER>");
			for (const char* variable : { "USERNAME", "USER", "LOGNAME" }) {
				const DWORD length = GetEnvironmentVariableA(variable, buffer, MAX_PATH);
				if (length && length < MAX_PATH) AddWord(std::string(buffer, length), "<USER>");
			}
			size = sizeof(buffer);
			if (GetComputerNameA(buffer, &size)) AddWord(buffer, "<PC>");
			for (COMPUTER_NAME_FORMAT format : { ComputerNameDnsHostname, ComputerNameDnsFullyQualified, ComputerNamePhysicalDnsHostname }) {
				size = sizeof(buffer);
				if (GetComputerNameExA(format, buffer, &size)) AddWord(buffer, "<PC>");
			}
			const DWORD domainLength = GetEnvironmentVariableA("USERDOMAIN", buffer, MAX_PATH);
			if (domainLength && domainLength < MAX_PATH) AddWord(std::string(buffer, domainLength), "<PC>");
			// The profile folder's own name (it can differ from the logon name: "John" logs on, the folder is "john.smith")
			char profile[MAX_PATH] = {};
			if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_PROFILE, NULL, SHGFP_TYPE_CURRENT, profile))) {
				const char* name = strrchr(profile, '\\');
				if (name && name[1]) AddWord(name + 1, "<USER>");
			}
			Finish();
		}

	private:
		struct Pattern { std::string text; const char* token; };
		std::vector<Pattern> Paths, Words;
		unsigned char First[256] = {};
		bool Ready = false;

		// The rule matching at s[i], if any: its token, and `end` = where the matched text ends.
		const char* Match(const char* s, size_t i, size_t n, size_t& end) const {
			// 1. This machine's folders, longest first. A match must end at a path boundary (not "C:\Games\FNV2" for "C:\Games\FNV").
			for (const Pattern& p : Paths) {
				const size_t length = p.text.size();
				if (!MatchAt(s, i, n, p.text.c_str(), length)) continue;
				if (i + length < n && !IsSep(s[i + length]) && IsWordChar(s[i + length])) continue;
				if (i > 0 && IsWordChar(s[i - 1]) && IsWordChar(s[i])) continue;	// "XC:\..." is no path start
				end = i + length;
				return p.token;
			}
			// 2. Any user folder: X:\Users\<name>, X:\Documents and Settings\<name>, X:\home\<name> (Wine maps the Linux home to Z:)
			const bool drive = i + 2 < n && s[i + 1] == ':' && IsSep(s[i + 2]) && IsWordChar(s[i]) && (i == 0 || !IsWordChar(s[i - 1]));
			if (drive) {
				for (const char* folder : { "users", "documents and settings", "home" }) {
					const size_t length = strlen(folder);
					const size_t at = i + 3;
					if (!MatchAt(s, at, n, folder, length) || at + length >= n || !IsSep(s[at + length])) continue;
					const size_t nameStart = at + length + 1;
					const size_t nameEnd = SegmentEnd(s, nameStart, n);
					if (nameEnd == nameStart) continue;
					end = nameEnd;
					return "%USERPROFILE%";
				}
			}
			// 3. A Unix home: /home/<name>, /Users/<name> (at the start of a path, not inside a relative one)
			if (s[i] == '/' && (i == 0 || !(IsWordChar(s[i - 1]) || s[i - 1] == '.' || s[i - 1] == '_' || s[i - 1] == '-' || IsSep(s[i - 1])))) {
				for (const char* folder : { "/home/", "/users/" }) {
					const size_t length = strlen(folder);
					if (!MatchAt(s, i, n, folder, length)) continue;
					const size_t nameEnd = SegmentEnd(s, i + length, n);
					if (nameEnd == i + length) continue;
					end = nameEnd;
					return "%USERPROFILE%";
				}
			}
			// 4. Mod Organizer profiles: <sep>profiles<sep><name>
			if (IsSep(s[i]) && MatchAt(s, i + 1, n, "profiles", 8) && i + 9 < n && IsSep(s[i + 9])) {
				const size_t nameEnd = SegmentEnd(s, i + 10, n);
				if (nameEnd > i + 10) {
					end = nameEnd;
					return "\\profiles\\<PROFILE>";
				}
			}
			// 5. Steam: userdata<sep><digits> and SteamID64 (17 digits starting 7656119)
			if ((s[i] == 'u' || s[i] == 'U') && (i == 0 || !IsWordChar(s[i - 1])) && MatchAt(s, i, n, "userdata", 8) && i + 9 < n && IsSep(s[i + 8]) && isdigit((unsigned char)s[i + 9])) {
				size_t k = i + 9;
				while (k < n && isdigit((unsigned char)s[k])) k++;
				end = k;
				return "userdata\\<STEAMID>";
			}
			if (s[i] == '7' && (i == 0 || !isdigit((unsigned char)s[i - 1])) && i + 17 <= n && !strncmp(s + i, "7656119", 7)) {
				size_t k = i;
				while (k < n && isdigit((unsigned char)s[k])) k++;
				if (k - i == 17) { end = k; return "<STEAMID>"; }
			}
			// 6. The user and computer names, as whole words
			if (i == 0 || !IsWordChar(s[i - 1])) {
				for (const Pattern& p : Words) {
					const size_t length = p.text.size();
					if (!MatchAt(s, i, n, p.text.c_str(), length)) continue;
					if (i + length < n && IsWordChar(s[i + length])) continue;
					end = i + length;
					return p.token;
				}
			}
			return nullptr;
		}
	};

}
