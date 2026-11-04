#pragma once
#include <chrono>	// TimeLogger
#include <cstdarg>
#include <cstdio>
#include <deque>	// the recent lines for the log window
#include <map>		// stateMap
#include <mutex>	// guards the recent lines
#include <string>	// FilePath

using stateMap = std::map<const char*, int>;	// render state name -> D3DRENDERSTATETYPE (TraceRenderState)

// NVR's log file (implemented in Logger.cpp). Every entry point takes a printf format.
class Logger {	// all static
public:
	static void Initialize(const char* fileName);
	// UNOFFICIAL P46: a new "<BaseName>_<date>_<time>.log" per launch in <game folder>\<FolderName>, keeping the newest
	// Keep logs there. Falls back to Initialize("<BaseName>.log") if the folder or file cannot be created.
	static void InitializeRotating(const char* folderName, const char* baseName, unsigned keep);

	static void Log(char* format, ...);
	static void Log(const char* format, ...);
	// UNOFFICIAL 2026-11-01: as Log, but not into the in-game window's ring buffer (no mutex): for the crash filter (SessionLog.cpp)
	static void LogUnbuffered(const char* format, ...);
	static void Debug(char* format, ...);			// only when debug logging is on
	static void Debug(const char* format, ...);
	static void TraceRenderState();				// every D3D render state of the device, to the log

	// Copy of the last lines written via Log() (not Debug()), for the in-game log window. Log() can be called from any
	// thread, so the copy is taken under a mutex and handed back by value: the caller never holds the lock while drawing.
	static void GetRecentLines(std::deque<std::string>& lines);
	// 2026-11-03: text scrubbed the way every log line is (LogScrub.h: <GAME>, no user / computer names), for the bug report
	static std::string Scrub(const std::string& text);

	static FILE*		LogFile;	// null until Initialize
	static std::string	FilePath;	// UNOFFICIAL 2026-11-01: the open log's path (game folder relative or absolute; the report folder copies it)

private:
	// Writes "[HH:MM:SS.mmm] <message>\n" with one fwrite, so two threads never interleave inside a line.
	static void Write(bool toRingBuffer, const char* format, va_list args);
	static void PushRecent(const char* line, size_t length);

	static constexpr size_t kRecentLimit = 500;
	static std::deque<std::string>	s_recentLines;
	static std::mutex				s_recentMutex;
};

// Wall-clock stopwatch: LogTime logs "<name> ran in <ms> ms" since construction or the previous LogTime.
class TimeLogger {	// e.g. around a load step
public:
	TimeLogger(); ~TimeLogger();

	float LogTime(const char* name);

private:
	std::chrono::system_clock::time_point start, end;
};
