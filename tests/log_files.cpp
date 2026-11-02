// Tests for src/base/LogFiles.h (UNOFFICIAL P46): log names, which files may be deleted, the oldest-first
// clean-up against a real temporary folder, the move of the old single log, and the timestamped line format.
#include <cstdio>
#include <set>
#include <string>
#include <vector>
#include "../src/base/LogFiles.h"

static int failures = 0;
#define CHECK(cond, what) do { if (!(cond)) { printf("FAIL: %s\n", what); failures++; } } while (0)

static SYSTEMTIME Time(unsigned y, unsigned mo, unsigned d, unsigned h, unsigned mi, unsigned s, unsigned ms = 0) {
	SYSTEMTIME t = {};
	t.wYear = (WORD)y; t.wMonth = (WORD)mo; t.wDay = (WORD)d; t.wHour = (WORD)h; t.wMinute = (WORD)mi; t.wSecond = (WORD)s; t.wMilliseconds = (WORD)ms;
	return t;
}

static std::string Line(char* stack, size_t size, std::string& heap, size_t& length, size_t& prefix, const char* format, ...) {
	va_list args;
	va_start(args, format);
	const char* line = LogFiles::FormatLine(stack, size, heap, Time(2026, 9, 29, 7, 5, 9, 42), format, args, length, prefix);
	va_end(args);
	return std::string(line, length);
}

static void Touch(const std::string& path) {
	FILE* f = nullptr;
	fopen_s(&f, path.c_str(), "w");
	if (f) { fputs("x", f); fclose(f); }
}

static void SetWriteTime(const std::string& path, SYSTEMTIME local) {
	HANDLE file = CreateFileA(path.c_str(), FILE_WRITE_ATTRIBUTES, 0, NULL, OPEN_EXISTING, 0, NULL);
	SYSTEMTIME utc;
	FILETIME written;
	TzSpecificLocalTimeToSystemTime(nullptr, &local, &utc);
	SystemTimeToFileTime(&utc, &written);
	SetFileTime(file, NULL, NULL, &written);
	CloseHandle(file);
}

static std::set<std::string> List(const std::string& folder) {
	std::set<std::string> names;
	WIN32_FIND_DATAA found;
	HANDLE search = FindFirstFileA((folder + "\\*").c_str(), &found);
	if (search == INVALID_HANDLE_VALUE) return names;
	do { if (strcmp(found.cFileName, ".") && strcmp(found.cFileName, "..")) names.insert(found.cFileName); } while (FindNextFileA(search, &found));
	FindClose(search);
	return names;
}

static void RemoveTree(const std::string& folder) {
	for (const std::string& name : List(folder)) {
		const std::string path = folder + "\\" + name;
		if (GetFileAttributesA(path.c_str()) & FILE_ATTRIBUTE_DIRECTORY) RemoveTree(path);
		else DeleteFileA(path.c_str());
	}
	RemoveDirectoryA(folder.c_str());
}

int main() {
	const char* base = "NewVegasReloaded";

	// 1. Names.
	CHECK(LogFiles::FileName(base, Time(2026, 9, 29, 14, 5, 3)) == "NewVegasReloaded_2026-09-29_14-05-03.log", "file name format");
	CHECK(LogFiles::FileName(base, Time(2026, 9, 29, 14, 5, 3), 2) == "NewVegasReloaded_2026-09-29_14-05-03-2.log", "same-second suffix");
	CHECK(LogFiles::IsLogName("NewVegasReloaded_2026-09-29_14-05-03.log", base), "accepts a log name");
	CHECK(LogFiles::IsLogName("NewVegasReloaded_2026-09-29_14-05-03-12.log", base), "accepts a suffixed log name");
	CHECK(LogFiles::IsLogName("newvegasreloaded_2026-09-29_14-05-03.LOG", base), "letters compared like Windows does");
	const char* rejected[] = {
		"NewVegasReloaded.log", "NewVegasReloaded_backup.log", "NewVegasReloaded_2026-09-29_14-05-3a.log",
		"NewVegasReloaded_2026-09-29_14-05-03.log.bak", "NewVegasReloaded_2026-09-29_14-05-03.txt",
		"NewVegasReloaded_2026-09-29_14-05-03-.log", "NewVegasReloaded_2026-09-29_14-05-03x.log",
		"NewVegasReloadedX_2026-09-29_14-05-03.log", "OblivionReloaded_2026-09-29_14-05-03.log",
		"NewVegasReloaded_2026-09-29-14-05-03.log", "NewVegasReloaded_2026-09-29_14-05-03-2-3.log",
	};
	for (const char* name : rejected) {
		if (LogFiles::IsLogName(name, base)) { printf("FAIL: accepted %s\n", name); failures++; }
	}

	// 2. Which logs go: the oldest, so that 25 remain once the new one exists; year boundaries sort correctly.
	std::vector<std::string> names;
	for (unsigned i = 0; i < 30; i++) names.push_back(LogFiles::FileName(base, Time(2026, 12, 1 + i, 23, 0, 0)));
	names.push_back(LogFiles::FileName(base, Time(2027, 1, 1, 0, 0, 0)));
	std::vector<std::string> shuffled;
	for (size_t i = 0; i < names.size(); i++) shuffled.push_back(names[(i * 7) % names.size()]);
	std::vector<std::string> remove = LogFiles::OldestToRemove(shuffled, 25, strlen(base));
	CHECK(remove.size() == 7, "31 logs, keep 25: the 7 oldest go");
	CHECK(remove.size() == 7 && remove.front() == names[0] && remove.back() == names[6], "exactly the 7 oldest, oldest first");
	CHECK(LogFiles::OldestToRemove(std::vector<std::string>(names.begin(), names.begin() + 24), 25, strlen(base)).empty(), "24 logs: none go");
	CHECK(LogFiles::OldestToRemove(std::vector<std::string>(names.begin(), names.begin() + 25), 25, strlen(base)).size() == 1, "25 logs: the oldest goes");
	CHECK(LogFiles::OldestToRemove(names, 0, strlen(base)).empty(), "keep 0 never deletes");

	// 3. Lines: time prefix, long lines, the exact stack-buffer boundary.
	char stack[64];
	std::string heap;
	size_t length = 0, prefix = 0;
	CHECK(Line(stack, sizeof(stack), heap, length, prefix, "value %d", 5) == "[07:05:09.042] value 5\n", "short line");
	CHECK(prefix == 15, "prefix is 15 characters");
	std::string longText(5000, 'a');
	longText += "end";
	CHECK(Line(stack, sizeof(stack), heap, length, prefix, "%s", longText.c_str()) == "[07:05:09.042] " + longText + "\n", "5003-character line kept whole");
	for (size_t n = sizeof(stack) - 20; n <= sizeof(stack) + 5; n++) {
		const std::string text(n, 'b');
		if (Line(stack, sizeof(stack), heap, length, prefix, "%s", text.c_str()) != "[07:05:09.042] " + text + "\n") {
			printf("FAIL: %u-character message around the buffer size\n", (unsigned)n);
			failures++;
		}
	}

	// 4. A real folder: clean-up, same-second names, and the old single log.
	char temp[MAX_PATH];
	GetTempPathA(MAX_PATH, temp);
	const std::string root = std::string(temp) + "nvr_log_files_test";
	RemoveTree(root);
	CreateDirectoryA(root.c_str(), NULL);
	const std::string folder = root + "\\NVR-Unofficial-Optimized-Logs";
	CreateDirectoryA(folder.c_str(), NULL);
	for (const std::string& name : names) Touch(folder + "\\" + name); // 31 logs
	const char* others[] = { "notes.txt", "NewVegasReloaded.log.bak", "OblivionReloaded_2020-01-01_00-00-00.log",
		"NewVegasReloaded_backup.log", "NewVegasReloaded_2000-01-01_00-00-00.log.txt" };
	for (const char* name : others) Touch(folder + "\\" + name);
	CreateDirectoryA((folder + "\\NewVegasReloaded_1999-01-01_00-00-00.log").c_str(), NULL); // a folder with a log's name
	CHECK(LogFiles::PruneOldLogs(folder, base, 25) == 7, "7 deleted from a real folder");
	std::set<std::string> left = List(folder);
	for (size_t i = 0; i < names.size(); i++)
		if ((left.count(names[i]) != 0) != (i >= 7)) { printf("FAIL: wrong log kept/deleted: %s\n", names[i].c_str()); failures++; }
	for (const char* name : others) CHECK(left.count(name) == 1, "other files are never deleted");
	CHECK(left.count("NewVegasReloaded_1999-01-01_00-00-00.log") == 1, "folders are never deleted");

	const SYSTEMTIME now = Time(2027, 1, 1, 0, 0, 0);
	CHECK(LogFiles::NewLogPath(folder, base, now) == folder + "\\NewVegasReloaded_2027-01-01_00-00-00-2.log", "same second gets -2");
	Touch(folder + "\\NewVegasReloaded_2027-01-01_00-00-00-2.log");
	CHECK(LogFiles::NewLogPath(folder, base, now) == folder + "\\NewVegasReloaded_2027-01-01_00-00-00-3.log", "then -3");

	const std::string oldLog = root + "\\NewVegasReloaded.log";
	Touch(oldLog);
	SetWriteTime(oldLog, Time(2026, 7, 4, 12, 30, 15));
	CHECK(LogFiles::AdoptOldLog(oldLog, folder, base), "old single log moved");
	CHECK(GetFileAttributesA(oldLog.c_str()) == INVALID_FILE_ATTRIBUTES, "old single log no longer in the game folder");
	CHECK(List(folder).count("NewVegasReloaded_2026-07-04_12-30-15.log") == 1, "old single log named after its last write time");
	CHECK(!LogFiles::AdoptOldLog(oldLog, folder, base), "nothing to move the second time");
	RemoveTree(root);

	// 5. Thirty game launches into a fake game folder whose first choice cannot be used, as the game does it.
	const std::string game = std::string(temp) + "nvr_log_files_game\\";
	RemoveTree(game.substr(0, game.size() - 1));
	CreateDirectoryA(game.c_str(), NULL);
	Touch(game + "NewVegasReloaded.log");
	SetWriteTime(game + "NewVegasReloaded.log", Time(2026, 9, 1, 12, 0, 0)); // older than every launch below
	const std::vector<std::string> gameFolders = { game + "bad?name\\", game };
	std::vector<std::string> opened;
	for (unsigned launch = 0; launch < 30; launch++) {
		const LogFiles::NewLog log = LogFiles::OpenNewLog(gameFolders, "NVR-Unofficial-Optimized-Logs", base, 25, Time(2026, 10, 1 + launch, 20, 0, 0));
		CHECK(log.file != nullptr, "each launch opens a log");
		if (!log.file) break;
		CHECK(log.adopted == (launch == 0), "the old single log is moved on the first launch only");
		CHECK(log.deleted == (launch >= 24 ? 1u : 0u), "one old log deleted per launch once 25 exist"); // 24 + the adopted one
		CHECK(log.name.find(temp) == std::string::npos && log.name.rfind("NVR-Unofficial-Optimized-Logs\\", 0) == 0, "logged name has no game path");
		fputs("written\n", log.file);
		fclose(log.file);
		opened.push_back(log.name.substr(strlen("NVR-Unofficial-Optimized-Logs\\")));
	}
	std::set<std::string> kept = List(game + "NVR-Unofficial-Optimized-Logs");
	CHECK(kept.size() == 25, "25 logs after 30 launches");
	for (size_t i = 0; i < opened.size(); i++)
		if ((kept.count(opened[i]) != 0) != (i >= 5)) { printf("FAIL: launch %u log kept/deleted wrongly\n", (unsigned)i); failures++; }
	CHECK(GetFileAttributesA((game + "NewVegasReloaded.log").c_str()) == INVALID_FILE_ATTRIBUTES, "no single log left in the game folder");
	RemoveTree(game.substr(0, game.size() - 1));

	// 6. The prefix really is 15 characters for every time of day.
	char prefixText[32];
	CHECK(LogFiles::LinePrefix(prefixText, sizeof(prefixText), Time(2026, 1, 1, 23, 59, 59, 999)) == 15 && std::string(prefixText) == "[23:59:59.999] ", "late prefix");

	if (failures) { printf("%d log file test(s) FAILED\n", failures); return 1; }
	printf("All log file tests passed.\n");
	return 0;
}
