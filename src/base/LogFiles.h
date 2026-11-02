#pragma once
// UNOFFICIAL P46: one log file per game launch, kept in its own folder next to the game's exe.
// Names are "<base>_YYYY-MM-DD_HH-MM-SS.log" (local time); a launch deletes the oldest logs so that `keep`
// remain including its own. Only files whose names match that pattern exactly are ever deleted or counted.
// Unit-tested in tests/log_files.cpp, including against a real folder.
#include <windows.h>
#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <share.h>
#include <string>
#include <vector>

namespace LogFiles {

	// "<base>_YYYY-MM-DD_HH-MM-SS.log"; n >= 2 gives "..._HH-MM-SS-<n>.log" for a second log started in the same second.
	inline std::string FileName(const char* base, const SYSTEMTIME& t, unsigned n = 1) {
		char name[160];
		if (n < 2)
			snprintf(name, sizeof(name), "%s_%04u-%02u-%02u_%02u-%02u-%02u.log", base, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
		else
			snprintf(name, sizeof(name), "%s_%04u-%02u-%02u_%02u-%02u-%02u-%u.log", base, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, n);
		return name;
	}

	// True only for names FileName() can produce for this base (letters compared case-insensitively, like Windows does).
	inline bool IsLogName(const std::string& name, const char* base) {
		const size_t baseLength = strlen(base);
		static const char stamp[] = "_0000-00-00_00-00-00"; // '0' = any digit, other characters must match
		const size_t stampLength = sizeof(stamp) - 1;
		if (name.size() < baseLength + stampLength + 4) return false;
		for (size_t i = 0; i < baseLength; i++)
			if (tolower((unsigned char)name[i]) != tolower((unsigned char)base[i])) return false;
		for (size_t i = 0; i < stampLength; i++) {
			const char c = name[baseLength + i];
			if (stamp[i] == '0' ? !isdigit((unsigned char)c) : c != stamp[i]) return false;
		}
		size_t pos = baseLength + stampLength;
		if (name[pos] == '-') {
			const size_t digits = ++pos;
			while (pos < name.size() && isdigit((unsigned char)name[pos])) pos++;
			if (pos == digits) return false;
		}
		return name.size() - pos == 4 && _stricmp(name.c_str() + pos, ".log") == 0;
	}

	// The oldest of these log names that must go so that `keep` logs remain once one more is created.
	// The date-time stamp sorts in time order, so the names are compared from the stamp on.
	inline std::vector<std::string> OldestToRemove(std::vector<std::string> names, size_t keep, size_t baseLength) {
		std::vector<std::string> remove;
		if (keep == 0 || names.size() < keep) return remove;
		std::sort(names.begin(), names.end(), [baseLength](const std::string& a, const std::string& b) {
			return _stricmp(a.c_str() + baseLength, b.c_str() + baseLength) < 0;
		});
		remove.assign(names.begin(), names.begin() + (names.size() - (keep - 1)));
		return remove;
	}

	// "[HH:MM:SS.mmm] " at the start of every line; returns its length (15). UNOFFICIAL 2026-11-01: with a frame number,
	// "[HH:MM:SS.mmm f12345] " (the rendered frame the line was written in, TheFrameRateManager->FrameNumber), so a line can be
	// matched with a bench screenshot of that frame. NoFrame = no frame number (before the renderer exists, and the tests).
	constexpr unsigned NoFrame = 0xFFFFFFFFu;
	inline int LinePrefix(char* out, size_t size, const SYSTEMTIME& t, unsigned frame = NoFrame) {
		if (frame == NoFrame) return snprintf(out, size, "[%02u:%02u:%02u.%03u] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
		return snprintf(out, size, "[%02u:%02u:%02u.%03u f%u] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, frame);
	}

	// Formats "[HH:MM:SS.mmm] <message>\n" (no terminating zero) into `stack`, or into `heap` when it does not fit.
	// Returns the start of the line; `length` gets its length including the '\n', `prefix` the length of the time prefix.
	inline char* FormatLine(char* stack, size_t stackSize, std::string& heap, const SYSTEMTIME& t, const char* message, va_list args,
		size_t& length, size_t& prefix, unsigned frame = NoFrame) {
		prefix = (size_t)LinePrefix(stack, stackSize, t, frame);
		const size_t room = stackSize - prefix; // for the message and its zero, which becomes the '\n'
		va_list copy;
		va_copy(copy, args);
		int written = vsnprintf(stack + prefix, room, message, copy);
		va_end(copy);
		const size_t messageLength = written > 0 ? (size_t)written : 0;
		char* line = stack;
		if (messageLength >= room) {
			heap.assign(stack, prefix);
			heap.resize(prefix + messageLength + 1);
			vsnprintf(&heap[prefix], messageLength + 1, message, args); // its zero lands on the last byte, which becomes the '\n'
			line = &heap[0];
		}
		line[prefix + messageLength] = '\n';
		length = prefix + messageLength + 1;
		return line;
	}

	inline bool Exists(const std::string& path) {
		return GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES;
	}

	// A path in `folder` for a new log started at `now` that does not exist yet.
	inline std::string NewLogPath(const std::string& folder, const char* base, const SYSTEMTIME& now) {
		std::string path = folder + "\\" + FileName(base, now);
		for (unsigned n = 2; Exists(path) && n < 100; n++) path = folder + "\\" + FileName(base, now, n);
		return path;
	}

	// Moves the single log written by builds before P46 (`oldPath`) into `folder`, named after its last write time,
	// so it is neither lost nor mistaken for the current log. Returns true if it was moved.
	inline bool AdoptOldLog(const std::string& oldPath, const std::string& folder, const char* base) {
		WIN32_FILE_ATTRIBUTE_DATA data;
		if (!GetFileAttributesExA(oldPath.c_str(), GetFileExInfoStandard, &data) || (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return false;
		SYSTEMTIME utc, local;
		if (!FileTimeToSystemTime(&data.ftLastWriteTime, &utc) || !SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local)) return false;
		return MoveFileExA(oldPath.c_str(), NewLogPath(folder, base, local).c_str(), MOVEFILE_COPY_ALLOWED) != 0;
	}

	// Deletes the oldest logs in `folder` so that `keep` remain once one more is created. Returns how many were deleted.
	inline unsigned PruneOldLogs(const std::string& folder, const char* base, size_t keep) {
		std::vector<std::string> names;
		WIN32_FIND_DATAA found;
		HANDLE search = FindFirstFileA((folder + "\\" + base + "_*.log").c_str(), &found);
		if (search != INVALID_HANDLE_VALUE) {
			do {
				if (!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && IsLogName(found.cFileName, base)) names.push_back(found.cFileName);
			} while (FindNextFileA(search, &found));
			FindClose(search);
		}
		unsigned deleted = 0;
		for (const std::string& name : OldestToRemove(names, keep, strlen(base)))
			if (DeleteFileA((folder + "\\" + name).c_str())) deleted++; // a log still open by another running game stays
		return deleted;
	}

	struct NewLog {
		FILE* file = nullptr;
		std::string name;      // "<folderName>\<file name>", without the game folder (it can hold the player's user name)
		bool adopted = false;  // the old single log was moved into the folder
		unsigned deleted = 0;  // old logs deleted to make room
	};

	// Tries each game folder in turn (ending in a slash; "" = the current folder): creates <game folder><folderName>,
	// moves the old single <game folder><base>.log into it, deletes the oldest logs and opens a new one.
	// NewLog::file is null if no game folder worked.
	inline NewLog OpenNewLog(const std::vector<std::string>& gameFolders, const char* folderName, const char* base, size_t keep, const SYSTEMTIME& now) {
		NewLog log;
		for (const std::string& gameFolder : gameFolders) {
			const std::string folder = gameFolder + folderName;
			if (!CreateDirectoryA(folder.c_str(), NULL) && GetLastError() != ERROR_ALREADY_EXISTS) continue;
			log.adopted = AdoptOldLog(gameFolder + base + ".log", folder, base);
			log.deleted = PruneOldLogs(folder, base, keep);
			const std::string path = NewLogPath(folder, base, now);
			log.file = _fsopen(path.c_str(), "w", _SH_DENYWR);
			if (!log.file) continue;
			log.name = std::string(folderName) + "\\" + path.substr(folder.size() + 1);
			return log;
		}
		return NewLog();
	}

}
