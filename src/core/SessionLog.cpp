// UNOFFICIAL 2026-11-01: the session header, live changes, heartbeat, exit / crash summary and the frame journal (SessionLog.h).
#include "SessionLog.h"
#include "FrameTimeStats.h"
#include "DxvkCompute.h"
#include "Diagnostics.h"
#include "SettingChanges.h"
#include "InstallCheck.h"
#include <intrin.h>
#include <tlhelp32.h>
#include <cstdarg>
#include <cmath>
#include <mutex>
#if __has_include("NvrBuildCommit.h")
#include "NvrBuildCommit.h"	// written by the project's pre-build step: #define NVR_BUILD_COMMIT "<git commit>[-dirty] <branch>"
#endif
#ifndef NVR_BUILD_COMMIT
#define NVR_BUILD_COMMIT "unknown"
#endif

// UNOFFICIAL 2026-11-01 (Environment): the game window (OnDevice) and the environment text last logged
static HWND GameWindow = nullptr;
static std::string LastEnvironment;

namespace {

	double Seconds() {
		static LARGE_INTEGER frequency = {}, start = {};
		LARGE_INTEGER now;
		if (!frequency.QuadPart) { QueryPerformanceFrequency(&frequency); QueryPerformanceCounter(&start); }
		QueryPerformanceCounter(&now);
		return (double)(now.QuadPart - start.QuadPart) / (double)frequency.QuadPart;
	}

	std::string Uptime(double s) {
		char text[48];
		const unsigned t = (unsigned)s;
		sprintf_s(text, "%uh%02um%02us", t / 3600, t / 60 % 60, t % 60);
		return text;
	}

	// Lines of up to ~900 characters: "<head> item, item, ..." (long lists stay readable and under the Logger's stack buffer)
	void LogList(const char* head, const std::vector<std::string>& items, const char* separator = ", ") {
		std::string line;
		for (const std::string& item : items) {
			if (!line.empty() && line.size() + item.size() > 900) { Logger::Log("%s%s", head, line.c_str()); line.clear(); }
			if (!line.empty()) line += separator;
			line += item;
		}
		if (!line.empty()) Logger::Log("%s%s", head, line.c_str());
	}

	std::string RegString(HKEY root, const char* path, const char* name) {
		char value[512] = {};
		DWORD size = sizeof(value) - 1;
		if (RegGetValueA(root, path, name, RRF_RT_REG_SZ, nullptr, value, &size) != ERROR_SUCCESS) return std::string();
		return value;
	}

	std::string FileVersion(const char* module) {
		HMODULE handle = GetModuleHandleA(module);
		char path[MAX_PATH] = {};
		if (!handle || !GetModuleFileNameA(handle, path, MAX_PATH)) return std::string();
		DWORD unused = 0;
		const DWORD size = GetFileVersionInfoSizeA(path, &unused);
		if (!size) return std::string();
		std::vector<char> data(size);
		if (!GetFileVersionInfoA(path, 0, size, data.data())) return std::string();
		struct Translation { WORD language, codePage; } *translation = nullptr;
		UINT length = 0;
		std::string result;
		if (VerQueryValueA(data.data(), "\\VarFileInfo\\Translation", (LPVOID*)&translation, &length) && length >= sizeof(Translation)) {
			char block[96];
			char* text = nullptr;
			sprintf_s(block, "\\StringFileInfo\\%04x%04x\\ProductName", translation->language, translation->codePage);
			if (VerQueryValueA(data.data(), block, (LPVOID*)&text, &length) && text) result = text;
			sprintf_s(block, "\\StringFileInfo\\%04x%04x\\ProductVersion", translation->language, translation->codePage);
			if (VerQueryValueA(data.data(), block, (LPVOID*)&text, &length) && text) result += std::string(result.empty() ? "" : " ") + text;
		}
		VS_FIXEDFILEINFO* fixed = nullptr;
		if (VerQueryValueA(data.data(), "\\", (LPVOID*)&fixed, &length) && fixed) {
			char text[64];
			sprintf_s(text, "%s(file %u.%u.%u.%u)", result.empty() ? "" : " ", HIWORD(fixed->dwFileVersionMS), LOWORD(fixed->dwFileVersionMS),
				HIWORD(fixed->dwFileVersionLS), LOWORD(fixed->dwFileVersionLS));
			result += text;
		}
		return result;
	}

	// --- live state ---
	std::mutex Lock;	// the setting changes can come from the menu and loader threads
	struct PendingChange { std::string key, before, after; double last, first; };	// first: when queued (SettingChanges.h: the batch rule)
	std::vector<PendingChange> Pending;
	constexpr size_t PendingCap = 1024;	// UNOFFICIAL 2026-11-01: a preset can change a few hundred settings in one frame (was 256)
	unsigned ChangesThisMinute = 0, ChangesSuppressed = 0, ChangesTotal = 0;
	double MinuteStart = 0.0;
	std::string LastChange = "none", LastCell = "none", LastWeather = "none";
	double StartSeconds = 0.0, InGameSeconds = 0.0;
	bool Ended = false;

	// heartbeat window
	std::vector<double> FrameMs;
	double LastFrameTime = -1.0, WindowStart = -1.0, LoadingSeconds = 0.0, WorstEver = 0.0, LoadingStart = -1.0;
	unsigned LoadingScreens = 0;
	double PeakAddressMB = 0.0;

	std::string GameClock() {
		if (!TheShaderManager) return "--:--";
		const float hour = TheShaderManager->ShaderConst.GameTime.y;
		char text[16];
		const int minutes = (int)(hour * 60.0f + 0.5f);
		sprintf_s(text, "%02d:%02d", minutes / 60 % 24, minutes % 60);
		return text;
	}

	std::string CellText(TESObjectCELL* cell) {
		char text[200];
		if (!cell) return "none";
		if (cell->IsInterior()) {
			const char* name = cell->GetEditorName();
			sprintf_s(text, "interior '%s' (%08X)", name && name[0] ? name : "?", cell->refID);
		}
		else {
			TESWorldSpace* world = cell->worldSpace;
			const char* name = world ? world->GetEditorName() : nullptr;
			if (cell->coords) sprintf_s(text, "exterior '%s' (%08X) cell %d,%d", name && name[0] ? name : "?", world ? world->refID : 0, cell->coords->x, cell->coords->y);
			else sprintf_s(text, "exterior '%s' (%08X)", name && name[0] ? name : "?", world ? world->refID : 0);
		}
		return text;
	}

	void FlushChanges(bool all) {
		const double now = Seconds();
		std::lock_guard<std::mutex> guard(Lock);
		if (now - MinuteStart > 60.0) {
			if (ChangesSuppressed) Logger::Log("SETTING %u more changes in the last minute not listed (at most %u a minute)", ChangesSuppressed, SettingChanges::PerMinute);
			ChangesSuppressed = 0; ChangesThisMinute = 0; MinuteStart = now;
		}
		std::vector<SettingChanges::Change> ready;
		for (size_t i = 0; i < Pending.size(); ) {
			PendingChange& c = Pending[i];
			if (!all && now - c.last < 0.75) { i++; continue; }	// a slider still being dragged
			if (c.before != c.after) ready.push_back({ c.key, c.before, c.after, c.first });
			Pending.erase(Pending.begin() + i);
		}
		if (ready.empty()) return;
		// UNOFFICIAL 2026-11-01 (SettingChanges.h): changes that arrived together in numbers (a preset load, a settings file reload) are
		// one compact block, never dropped; the rest one line each under the per-minute cap
		std::vector<std::vector<SettingChanges::Change>> batches;
		std::vector<SettingChanges::Change> singles;
		SettingChanges::Split(ready, batches, singles);
		for (const std::vector<SettingChanges::Change>& batch : batches) {
			ChangesTotal += (unsigned)batch.size();
			const SettingChanges::Change& c = batch.back();
			LastChange = c.key + " " + c.before + " -> " + c.after + " (in a batch of " + std::to_string(batch.size()) + ")";
			char head[96];
			sprintf_s(head, "SETTING batch (%u changes, game time %s): ", (unsigned)batch.size(), GameClock().c_str());
			LogList(head, SettingChanges::BatchItems(batch), " | ");
		}
		for (const SettingChanges::Change& c : singles) {
			ChangesTotal++;
			LastChange = c.key + " " + c.before + " -> " + c.after;
			if (ChangesThisMinute++ < SettingChanges::PerMinute) Logger::Log("SETTING %s: %s -> %s (game time %s)", c.key.c_str(), c.before.c_str(), c.after.c_str(), GameClock().c_str());
			else ChangesSuppressed++;
		}
	}

	unsigned EffectsOn() {
		if (!TheShaderManager) return 0;
		unsigned on = 0;
		for (const auto& entry : TheShaderManager->EffectsNames) if (entry.second && *entry.second && (*entry.second)->Enabled) on++;
		return on;
	}

	double AddressSpaceMB(double* totalMB = nullptr) {
		MEMORYSTATUSEX memory = { sizeof(memory) };
		if (!GlobalMemoryStatusEx(&memory)) return 0.0;
		if (totalMB) *totalMB = memory.ullTotalVirtual / 1048576.0;
		return (memory.ullTotalVirtual - memory.ullAvailVirtual) / 1048576.0;
	}

	void Heartbeat(double now) {
		FlushChanges(false);
		const double span = now - WindowStart;
		std::vector<double> ms = FrameMs;
		const FrameTimeSummary s = SummarizeFrameTimes(ms);
		const unsigned spikes = CountFrameSpikes(FrameMs, s.p50Ms);
		double totalMB = 0.0;
		const double usedMB = AddressSpaceMB(&totalMB);
		const unsigned textureMB = TheRenderManager && TheRenderManager->device ? TheRenderManager->device->GetAvailableTextureMem() / 1048576u : 0;
		const SessionLog::Counters& c = SessionLog::Count;
		Logger::Log("HEARTBEAT %.0f s: %u frames, avg %.2f ms (%.0f fps), 1%% low %.0f fps, worst %.1f ms, %u spikes | loading %.1f s | address space "
			"%.0f of %.0f MB (peak %.0f), D3D9 texture memory free ~%u MB | effects on %u | %s, game time %s | cuts %u, history resets %u, lamp slot "
			"changes %u, cached sun cascade redraws %u, lamp pool draws %u, shadow atlas re-converts %u, reflection cube restarts %u, AL grid rebuilds %u",
			span, s.frames, s.avgMs, s.avgMs > 0 ? 1000.0 / s.avgMs : 0.0, s.low1Fps, s.maxMs, spikes, LoadingSeconds, usedMB, totalMB, PeakAddressMB, textureMB,
			EffectsOn(), LastCell.c_str(), GameClock().c_str(), c.cameraCuts, c.historyResets, c.lampSlotChanges, c.cachedCascadeRedraws, c.poolDraws,
			c.atlasReconverts, c.cubeRestarts, c.gridRebuilds);
		SessionLog::Count = SessionLog::Counters();
		Diagnostics::Heartbeat();	// UNOFFICIAL 2026-11-01: the effects' drawn / skipped counts, the flash detector, shader cache, D3D failures
		{	// UNOFFICIAL 2026-11-01: a frame-generation / capture / overlay program started or stopped, or another window came in front
			const std::string environment = SessionLog::Environment();
			if (environment != LastEnvironment) { Logger::Log("ENVIRONMENT changed: %s", environment.c_str()); LastEnvironment = environment; }
		}
		FrameMs.clear();
		LoadingSeconds = 0.0;
		WindowStart = now;
	}

	LPTOP_LEVEL_EXCEPTION_FILTER PreviousFilter = nullptr;
	LONG WINAPI CrashFilter(EXCEPTION_POINTERS* info) {
		static volatile LONG entered = 0;
		if (InterlockedExchange(&entered, 1) == 0 && info && info->ExceptionRecord) {
			const EXCEPTION_RECORD* r = info->ExceptionRecord;
			char module[MAX_PATH] = "?";
			HMODULE handle = nullptr;
			UINT_PTR offset = 0;
			if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)r->ExceptionAddress, &handle) && handle) {
				char path[MAX_PATH] = {};
				GetModuleFileNameA(handle, path, MAX_PATH);
				const char* base = strrchr(path, '\\');
				strcpy_s(module, base ? base + 1 : path);
				offset = (UINT_PTR)r->ExceptionAddress - (UINT_PTR)handle;
			}
			Logger::LogUnbuffered("CRASH: exception %08lX at %s+%lX (thread %lu) after %s | last cell %s | last setting change %s | address space %.0f MB | frame time worst %.1f ms",
				r->ExceptionCode, module, (unsigned long)offset, GetCurrentThreadId(), Uptime(Seconds() - StartSeconds).c_str(), LastCell.c_str(),
				LastChange.c_str(), AddressSpaceMB(), WorstEver);
		}
		return PreviousFilter ? PreviousFilter(info) : EXCEPTION_CONTINUE_SEARCH;
	}

	// The game INI values the header lists (as the game read them, after NVR's own adjustments in ReadSettingHook)
	const char* const IniNames[] = { "iSize W:Display", "iSize H:Display", "bFull Screen:Display", "bFull Screen", "iMultiSample:Display",
		"iPresentInterval:Display", "uGridsToLoad:General", "uExterior Cell Buffer:General", "iMaxAnisotropy:Display", "iShadowMapResolution:Display",
		"iTexMipMapSkip:Display", "b30GrassVS:Grass", "bUseThreadedAI:General", "iNumHWThreads:General", "bUseWaterReflections:Water",
		"iWaterReflectHeight:Water", "fLODFadeOutMultObjects:LOD", "uInterior Cell Buffer:General", "bAllowScreenShot:Display" };
	std::string IniValues[sizeof(IniNames) / sizeof(IniNames[0])];
}

namespace {
	// UNOFFICIAL 2026-11-01: the journal's last events, always (FrameJournal::Dump; core/Diagnostics.cpp)
	struct JournalEvent { unsigned frame; double time; char text[200]; };
	JournalEvent JournalRing[256];
	unsigned JournalNext = 0;
	std::mutex JournalLock;

	// The last frames' times for the REPORT block
	float FrameRing[1024];
	unsigned FrameRingNext = 0;
}

void FrameJournal::Note(const char* format, ...) {
	char text[600];
	va_list args;
	va_start(args, format);
	vsnprintf(text, sizeof(text), format, args);
	va_end(args);
	{
		std::lock_guard<std::mutex> guard(JournalLock);
		JournalEvent& e = JournalRing[JournalNext++ % (sizeof(JournalRing) / sizeof(JournalRing[0]))];
		e.frame = TheFrameRateManager ? TheFrameRateManager->FrameNumber : 0;
		e.time = Seconds();
		strncpy_s(e.text, text, _TRUNCATE);
	}
	if (!On || Lines >= Cap) return;
	if (++Lines == Cap) Logger::Log("FJ %s | the frame journal reached its %u lines: no more this session", text, Cap);
	else Logger::Log("FJ %s", text);
}

unsigned FrameJournal::Dump(const char* head, unsigned first, unsigned last, double maxAgeSeconds) {
	const unsigned size = sizeof(JournalRing) / sizeof(JournalRing[0]);
	std::vector<JournalEvent> events;
	{
		std::lock_guard<std::mutex> guard(JournalLock);
		const unsigned count = JournalNext < size ? JournalNext : size;
		for (unsigned i = JournalNext - count; i != JournalNext; i++) events.push_back(JournalRing[i % size]);
	}
	const double now = Seconds();
	unsigned written = 0;
	for (const JournalEvent& e : events) {
		if (e.frame < first || e.frame > last) continue;
		if (maxAgeSeconds > 0.0 && now - e.time > maxAgeSeconds) continue;
		Logger::Log("%s f%u (%.2f s ago): %s", head, e.frame, now - e.time, e.text);
		written++;
	}
	if (!written) Logger::Log("%s (no frame journal events in these frames)", head);
	return written;
}

unsigned FrameJournal::CountFrame(unsigned frame, const char* prefix) {
	const unsigned size = sizeof(JournalRing) / sizeof(JournalRing[0]);
	const size_t length = prefix ? strlen(prefix) : 0;
	std::lock_guard<std::mutex> guard(JournalLock);
	const unsigned count = JournalNext < size ? JournalNext : size;
	unsigned found = 0;
	for (unsigned i = JournalNext - count; i != JournalNext; i++) {
		const JournalEvent& e = JournalRing[i % size];
		if (e.frame == frame && (!length || !strncmp(e.text, prefix, length))) found++;
	}
	return found;
}

std::string SessionLog::Where() { return LastCell; }
std::string SessionLog::Weather() { return LastWeather; }
std::string SessionLog::Clock() { return GameClock(); }
void SessionLog::RecentFrameTimes(std::vector<float>& out, unsigned count) {
	const unsigned size = sizeof(FrameRing) / sizeof(FrameRing[0]);
	if (count > size) count = size;
	if (count > FrameRingNext) count = FrameRingNext;
	out.clear();
	for (unsigned i = FrameRingNext - count; i != FrameRingNext; i++) out.push_back(FrameRing[i % size]);
}

void SessionLog::HistoryReset(const char* effect, const char* why) {
	Count.historyResets++;
	EffectStatus::History(EffectStatus::Register(effect), why);	// UNOFFICIAL 2026-11-01 (core/EffectStatus.h)
	FRAME_JOURNAL("history reset: %s (%s)", effect, why);
}

void SessionLog::OnPluginLoad(unsigned nvseVersion, unsigned gameVersion, unsigned pluginVersion) {
	StartSeconds = Seconds();
	MinuteStart = StartSeconds;
	Logger::Log("SESSION build: VegasShaders %u, commit %s, compiled %s %s (%s)", pluginVersion, NVR_BUILD_COMMIT, __DATE__, __TIME__,
#if NVR_TEST_TOOLS
		"test tools in"
#else
		"release"
#endif
	);
	// both packed as major << 24 | minor << 16 | build << 4 | type (the game's 1.4.0.525 = 040020D1)
	Logger::Log("SESSION xNVSE %u.%u.%u (%08X), game 1.%u.%u.%u (%08X)", nvseVersion >> 24, (nvseVersion >> 16) & 0xFF, (nvseVersion >> 4) & 0xFFF,
		nvseVersion, gameVersion >> 24, (gameVersion >> 16) & 0xFF, (gameVersion >> 4) & 0xFFF, gameVersion);

	// Windows: the build from RtlGetVersion (GetVersionEx lies to an exe without a manifest), the edition and release from the registry; Wine
	char windows[256] = "?";
	typedef LONG(WINAPI* RtlGetVersionFn)(OSVERSIONINFOEXW*);
	HMODULE ntdll = GetModuleHandleA("ntdll.dll");
	OSVERSIONINFOEXW version = { sizeof(version) };
	RtlGetVersionFn getVersion = ntdll ? (RtlGetVersionFn)GetProcAddress(ntdll, "RtlGetVersion") : nullptr;
	const char* key = "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
	DWORD ubr = 0, size = sizeof(ubr);
	RegGetValueA(HKEY_LOCAL_MACHINE, key, "UBR", RRF_RT_REG_DWORD, nullptr, &ubr, &size);
	std::string release = RegString(HKEY_LOCAL_MACHINE, key, "DisplayVersion");
	if (release.empty()) release = RegString(HKEY_LOCAL_MACHINE, key, "ReleaseId");
	if (getVersion && getVersion(&version) == 0)
		sprintf_s(windows, "%s %s, build %lu.%lu.%lu.%lu", RegString(HKEY_LOCAL_MACHINE, key, "ProductName").c_str(), release.c_str(),
			version.dwMajorVersion, version.dwMinorVersion, version.dwBuildNumber, ubr);
	typedef const char* (__cdecl* WineVersionFn)();
	WineVersionFn wine = ntdll ? (WineVersionFn)GetProcAddress(ntdll, "wine_get_version") : nullptr;
	BOOL wow64 = FALSE;
	IsWow64Process(GetCurrentProcess(), &wow64);
	Logger::Log("SESSION Windows: %s%s%s%s", windows, wow64 ? ", 64-bit" : ", 32-bit", wine ? ", Wine / Proton " : "", wine ? wine() : "");

	// CPU: the brand string, cores and threads
	int regs[4] = {};
	char brand[49] = {};
	__cpuid(regs, 0x80000000);
	if ((unsigned)regs[0] >= 0x80000004u) {
		for (int i = 0; i < 3; i++) { __cpuid(regs, 0x80000002 + i); memcpy(brand + 16 * i, regs, 16); }
	}
	const char* cpu = brand;
	while (*cpu == ' ') cpu++;
	unsigned cores = 0;
	DWORD bytes = 0;
	GetLogicalProcessorInformation(nullptr, &bytes);
	std::vector<SYSTEM_LOGICAL_PROCESSOR_INFORMATION> info(bytes / sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION) + 1);
	if (bytes && GetLogicalProcessorInformation(info.data(), &bytes))
		for (DWORD i = 0; i < bytes / sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION); i++) if (info[i].Relationship == RelationProcessorCore) cores++;
	SYSTEM_INFO system = {};
	GetNativeSystemInfo(&system);
	MEMORYSTATUSEX memory = { sizeof(memory) };
	GlobalMemoryStatusEx(&memory);
	Logger::Log("SESSION CPU: %s, %u cores, %lu threads | RAM %.1f GB (%.1f GB free) | address space %.0f MB (%s)", cpu[0] ? cpu : "?", cores,
		system.dwNumberOfProcessors, memory.ullTotalPhys / 1073741824.0, memory.ullAvailPhys / 1073741824.0, memory.ullTotalVirtual / 1048576.0,
		memory.ullTotalVirtual > 3ull * 1073741824ull ? "4 GB patch / large address aware" : "NOT large address aware: 2 GB, expect out-of-memory crashes");
}

void SessionLog::OnSettingsLoaded() {
	LogSettingsDiff("SESSION settings");
}

// UNOFFICIAL 2026-11-01: the live values (Config.TomlConfig: a change made in the menu, a preset loaded or a settings file reloaded
// after the start is in it), so the REPORT block's call lists what the tester has NOW, not what the SESSION header saw at the start
void SessionLog::LogSettingsDiff(const char* head) {
	unsigned differ = 0, total = 0;
	const std::vector<std::string> items = SettingsDiff(differ, total);
	if (!total) return;
	Logger::Log("%s: %u of %u differ from this build's defaults%s (key=yours (default)):", head, differ, total,
		TheSettingManager->Config.MaxQualityActive ? "; Max Quality is ON (its values are used in place of these)" : "");
	LogList((std::string(head) + ": ").c_str(), items, " | ");
}

// 2026-11-03: the walk LogSettingsDiff logged, as data (the bug report, InstallCheck.h, lists the same)
std::vector<std::string> SessionLog::SettingsDiff(unsigned& differ, unsigned& total) {
	typedef SettingManager::Configuration Config;
	std::vector<std::string> items;
	differ = total = 0;
	if (!TheSettingManager) return items;
	Config& config = TheSettingManager->Config;
	if (!config.DefaultConfig.is_table()) return items;
	auto number = [](const tomlValue& v, double& out) {
		if (v.is_floating()) { out = v.as_floating(); return true; }
		if (v.is_integer()) { out = (double)v.as_integer(); return true; }
		return false;
	};
	auto text = [](const tomlValue& v) -> std::string {
		char t[64];
		if (v.is_boolean()) return v.as_boolean() ? "true" : "false";
		if (v.is_integer()) { sprintf_s(t, "%lld", (long long)v.as_integer()); return t; }
		if (v.is_floating()) { sprintf_s(t, "%g", (double)(float)v.as_floating()); return t; }
		if (v.is_string()) { std::string s = v.as_string(); if (s.size() > 60) s = s.substr(0, 57) + "..."; return "\"" + s + "\""; }
		return "?";
	};
	auto walk = [&](auto& self, const tomlValue& defaults, const tomlValue* user, const std::string& path) -> void {
		std::vector<std::string> here;
		for (const auto& [key, value] : defaults.as_table()) {
			const tomlValue* mine = user && user->is_table() && user->contains(key) ? &user->at(key) : nullptr;
			if (value.is_table()) { self(self, value, mine, path.empty() ? (key[0] == '_' ? key.substr(1) : key) : path + "." + key); continue; }
			total++;
			if (!mine) continue;
			double a, b;
			bool same;
			if (number(value, a) && number(*mine, b)) same = fabs(a - b) <= 1e-5 * (std::max)(1.0, fabs(a));
			else if (value.is_boolean() && mine->is_boolean()) same = value.as_boolean() == mine->as_boolean();
			else if (value.is_string() && mine->is_string()) same = value.as_string().str == mine->as_string().str;	// (the text only: toml11's == also compares "basic" vs 'literal' quoting)
			else same = false;
			if (same) continue;
			differ++;
			here.push_back(key + "=" + text(*mine) + " (" + text(value) + ")");
		}
		if (!here.empty() && items.size() < 600) {
			std::string group = "[" + path + "] ";
			for (size_t i = 0; i < here.size(); i++) group += (i ? ", " : "") + here[i];
			items.push_back(group);
		}
	};
	walk(walk, config.DefaultConfig, &config.TomlConfig, "");
	return items;
}

// UNOFFICIAL 2026-11-01 the environment (SessionLog.h): programs that take the game's frames or draw over them. A tester's 4K smear
// video came from a Lossless Scaling capture; whether such a program runs is a fact a log should carry.
std::string SessionLog::Environment() {
	struct Known { const char* exe; const char* what; };
	static const Known programs[] = {
		{ "losslessscaling.exe", "Lossless Scaling (frame generation / scaling)" }, { "rtss.exe", "RivaTuner Statistics Server (overlay, frame limit)" },
		{ "msiafterburner.exe", "MSI Afterburner" }, { "obs64.exe", "OBS Studio" }, { "obs32.exe", "OBS Studio (32-bit)" }, { "nvidia overlay.exe", "NVIDIA app overlay" },
		{ "nvidia share.exe", "NVIDIA ShadowPlay" }, { "magpie.exe", "Magpie (scaling)" }, { "skif.exe", "Special K" }, { "specialk64.exe", "Special K" },
		{ "parsecd.exe", "Parsec (streaming)" }, { "medal.exe", "Medal (capture)" }, { "overwolf.exe", "Overwolf (overlay)" }, { "streamlabs obs.exe", "Streamlabs" },
		{ "gamebar.exe", "Xbox Game Bar" }, { "xboxgamebar.exe", "Xbox Game Bar" }, { "radeonsoftware.exe", "AMD Software overlay" }, { "frameview.exe", "NVIDIA FrameView" },
		{ "capframex.exe", "CapFrameX" }, { "presentmon.exe", "PresentMon" }, { "sunshine.exe", "Sunshine (streaming)" }, { "moonlight.exe", "Moonlight (streaming)" } };
	static const char* hooks[] = { "GameOverlayRenderer.dll", "DiscordHook.dll", "RTSSHooks.dll", "nvspcap.dll", "SpecialK32.dll", "graphics-hook32.dll", "ReShade32.dll",
		"d3d9_ow.dll", "OWClient.dll", "XInput1_3.dll" };
	std::vector<std::string> found;
	struct Running { DWORD pid; const Known* k; };
	std::vector<Running> running;
	HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (snapshot != INVALID_HANDLE_VALUE) {
		PROCESSENTRY32W entry = { sizeof(entry) };
		for (BOOL more = Process32FirstW(snapshot, &entry); more; more = Process32NextW(snapshot, &entry)) {
			char name[MAX_PATH] = {};
			WideCharToMultiByte(CP_ACP, 0, entry.szExeFile, -1, name, sizeof(name), nullptr, nullptr);
			for (char& c : name) c = (char)tolower((unsigned char)c);
			for (const Known& k : programs) if (!strcmp(name, k.exe)) { running.push_back({ entry.th32ProcessID, &k }); break; }
		}
		CloseHandle(snapshot);
	}
	// Their windows over the game window (a frame-generation program shows its own window on top of the game's)
	RECT game = {};
	if (GameWindow) GetWindowRect(GameWindow, &game);
	struct Over { std::vector<Running>* running; const RECT* game; std::vector<std::string> notes; };
	Over over = { &running, &game, {} };
	if (GameWindow) EnumWindows([](HWND w, LPARAM p) -> BOOL {
		Over* o = (Over*)p;
		if (w == GameWindow || !IsWindowVisible(w)) return TRUE;
		DWORD pid = 0;
		GetWindowThreadProcessId(w, &pid);
		for (const Running& r : *o->running) {
			if (r.pid != pid) continue;
			RECT rect = {}, cut = {};
			if (GetWindowRect(w, &rect) && IntersectRect(&cut, &rect, o->game) && (cut.right - cut.left) * (cut.bottom - cut.top) > 0) {
				char t[160];
				sprintf_s(t, "%s has a %ldx%ld window over the game window%s", r.k->exe, rect.right - rect.left, rect.bottom - rect.top,
					(GetWindowLongA(w, GWL_EXSTYLE) & WS_EX_TOPMOST) ? " (topmost)" : "");
				o->notes.push_back(t);
			}
			break;
		}
		return TRUE;
	}, (LPARAM)&over);
	for (const Running& r : running) found.push_back(std::string(r.k->exe) + " = " + r.k->what);
	std::string text = found.empty() ? "no frame generation / capture / overlay program from the known list is running" : "running: ";
	for (size_t i = 0; i < found.size(); i++) text += (i ? ", " : "") + found[i];
	for (const std::string& n : over.notes) text += " | " + n;
	std::vector<std::string> inProcess;
	for (const char* h : hooks) if (GetModuleHandleA(h)) inProcess.push_back(h);
	if (!inProcess.empty()) { text += " | hook DLLs in the game: "; for (size_t i = 0; i < inProcess.size(); i++) text += (i ? ", " : "") + inProcess[i]; }
	// The foreground window and the game window
	HWND fore = GetForegroundWindow();
	DWORD forePid = 0;
	if (fore) GetWindowThreadProcessId(fore, &forePid);
	const char* foreText = !fore ? "no foreground window" : fore == GameWindow || forePid == GetCurrentProcessId() ? "the game is the foreground window" : "ANOTHER program is the foreground window";
	for (const Running& r : running) if (fore && r.pid == forePid) { foreText = r.k->exe; break; }
	text += std::string(" | ") + foreText;
	if (GameWindow) {
		char t[160];
		MONITORINFO monitor = { sizeof(monitor) };
		GetMonitorInfoA(MonitorFromWindow(GameWindow, MONITOR_DEFAULTTONEAREST), &monitor);
		sprintf_s(t, " | game window %ldx%ld at %ld,%ld on a %ldx%ld monitor%s%s", game.right - game.left, game.bottom - game.top, game.left, game.top,
			monitor.rcMonitor.right - monitor.rcMonitor.left, monitor.rcMonitor.bottom - monitor.rcMonitor.top, IsIconic(GameWindow) ? ", minimised" : "",
			IsWindowVisible(GameWindow) ? "" : ", hidden");
		text += t;
	}
	return text;
}

void SessionLog::OnDevice(IDirect3DDevice9* device) {
	// The GPUs Windows knows (the display class in the registry: name, driver, VRAM); DXVK may show the game another name
	const char* displayClass = "SYSTEM\\CurrentControlSet\\Control\\Class\\{4d36e968-e325-11ce-bfc1-08002be10318}";
	std::vector<std::string> gpus;
	for (int i = 0; i < 16; i++) {
		char path[160];
		sprintf_s(path, "%s\\%04d", displayClass, i);
		const std::string name = RegString(HKEY_LOCAL_MACHINE, path, "DriverDesc");
		if (name.empty()) continue;
		std::string driver = RegString(HKEY_LOCAL_MACHINE, path, "DriverVersion");
		// NVIDIA's own number from the Windows one: 32.0.15.6094 -> 560.94 (the last five digits of the last two parts)
		const std::string provider = RegString(HKEY_LOCAL_MACHINE, path, "ProviderName");
		if (provider.find("NVIDIA") != std::string::npos) {
			std::string digits;
			const size_t dot = driver.find('.', driver.find('.') + 1);
			for (size_t k = dot == std::string::npos ? 0 : dot; k < driver.size(); k++) if (isdigit((unsigned char)driver[k])) digits += driver[k];
			if (digits.size() >= 5) driver += " = NVIDIA " + digits.substr(digits.size() - 5, 3) + "." + digits.substr(digits.size() - 2);
		}
		unsigned long long vram = 0;
		DWORD size = sizeof(vram);
		if (RegGetValueA(HKEY_LOCAL_MACHINE, path, "HardwareInformation.qwMemorySize", RRF_RT_QWORD, nullptr, &vram, &size) != ERROR_SUCCESS) {
			DWORD vram32 = 0;
			size = sizeof(vram32);
			if (RegGetValueA(HKEY_LOCAL_MACHINE, path, "HardwareInformation.MemorySize", RRF_RT_DWORD | RRF_RT_REG_BINARY, nullptr, &vram32, &size) == ERROR_SUCCESS) vram = vram32;
		}
		char item[400];
		sprintf_s(item, "%s (driver %s%s%s, VRAM %.1f GB)", name.c_str(), driver.empty() ? "?" : driver.c_str(), provider.empty() ? "" : ", ",
			provider.c_str(), vram / 1073741824.0);
		gpus.push_back(item);
	}
	LogList("SESSION GPU (Windows): ", gpus, " | ");
	IDirect3D9* d3d = nullptr;
	D3DADAPTER_IDENTIFIER9 adapter = {};
	D3DDISPLAYMODE desktop = {};
	if (device && SUCCEEDED(device->GetDirect3D(&d3d)) && d3d) {
		d3d->GetAdapterIdentifier(D3DADAPTER_DEFAULT, 0, &adapter);
		d3d->GetAdapterDisplayMode(D3DADAPTER_DEFAULT, &desktop);
		d3d->Release();
	}
	Logger::Log("SESSION GPU (as the game sees it): %s, vendor %04lX device %04lX, D3D9 driver %u.%u.%u.%u", adapter.Description, adapter.VendorId, adapter.DeviceId,
		HIWORD(adapter.DriverVersion.HighPart), LOWORD(adapter.DriverVersion.HighPart), HIWORD(adapter.DriverVersion.LowPart), LOWORD(adapter.DriverVersion.LowPart));
	const std::string d3d9 = FileVersion("d3d9.dll");
	const char* runtime = TheRenderManager->D3D9RuntimeDescription();
	Logger::Log("SESSION D3D9 runtime: %.*s, d3d9.dll %s | NVR DXVK extension: %s", (int)(strchr(runtime, '(') ? strchr(runtime, '(') - runtime : strlen(runtime)), runtime,
		d3d9.empty() ? "(no version info)" : d3d9.c_str(), DxvkCompute::Available() ? DxvkCompute::Status() : "not there");
	// 2026-11-03: the same facts for the install check and the bug report (InstallCheck.h)
	std::string gpuWindows;
	for (const std::string& g : gpus) gpuWindows += (gpuWindows.empty() ? "" : " | ") + g;
	char gpuGame[320];
	sprintf_s(gpuGame, "%s, vendor %04lX device %04lX", adapter.Description, adapter.VendorId, adapter.DeviceId);
	InstallCheck::OnDevice(gpuWindows, gpuGame, !strncmp(runtime, "DXVK", 4), d3d9, DxvkCompute::Available() ? DxvkCompute::Status() : "");
	const char* wrappers[] = { "dxgi.dll", "d3d11.dll", "dinput8.dll", "ReShade32.dll", "dxvk_d3d9.dll" };
	std::vector<std::string> found;
	for (const char* w : wrappers) if (GetModuleHandleA(w)) { const std::string v = FileVersion(w); found.push_back(std::string(w) + (v.empty() ? "" : " " + v)); }
	if (!found.empty()) LogList("SESSION also loaded: ", found);

	IDirect3DSwapChain9* swap = nullptr;
	D3DPRESENT_PARAMETERS present = {};
	if (device && SUCCEEDED(device->GetSwapChain(0, &swap)) && swap) { swap->GetPresentParameters(&present); swap->Release(); }
	GameWindow = present.hDeviceWindow;	// UNOFFICIAL 2026-11-01 (Environment)
	const char* mode = present.Windowed ? "windowed" : "exclusive fullscreen";
	if (present.Windowed && present.hDeviceWindow) {
		const LONG style = GetWindowLongA(present.hDeviceWindow, GWL_STYLE);
		MONITORINFO monitor = { sizeof(monitor) };
		RECT window = {};
		GetWindowRect(present.hDeviceWindow, &window);
		if (!(style & WS_CAPTION) && GetMonitorInfoA(MonitorFromWindow(present.hDeviceWindow, MONITOR_DEFAULTTONEAREST), &monitor) &&
			EqualRect(&window, &monitor.rcMonitor)) mode = "borderless fullscreen window";
	}
	Logger::Log("SESSION screen: %ux%u %s, back buffer format %u, MSAA %u, present interval %s, refresh %u Hz (desktop %ux%u at %u Hz)",
		present.BackBufferWidth, present.BackBufferHeight, mode, (unsigned)present.BackBufferFormat, (unsigned)present.MultiSampleType,
		present.PresentationInterval == D3DPRESENT_INTERVAL_IMMEDIATE ? "immediate (no vsync)" : present.PresentationInterval == D3DPRESENT_INTERVAL_ONE ? "1 (vsync)" : "other",
		present.FullScreen_RefreshRateInHz ? present.FullScreen_RefreshRateInHz : desktop.RefreshRate, desktop.Width, desktop.Height, desktop.RefreshRate);
	SettingManager* s = TheSettingManager;
	Logger::Log("SESSION anti-aliasing / upscaling: TAA %s, FSR %s (version %d, RenderScale %.2f), DLSS %s (RenderScale %.2f, Ray Reconstruction %s), SMAA %s",
		s->GetSettingI("Shaders.TAA.Status", "Enabled") ? "on" : "off", s->GetSettingI("Shaders.TAA.FSR", "Enabled") ? "on" : "off",
		s->GetSettingI("Shaders.TAA.FSR", "Version"), s->GetSettingF("Shaders.TAA.FSR", "RenderScale"), s->GetSettingI("Shaders.TAA.DLSS", "Enabled") ? "on" : "off",
		s->GetSettingF("Shaders.TAA.DLSS", "RenderScale"), s->GetSettingI("Shaders.TAA.DLSS", "RayReconstruction") ? "on" : "off",
		s->GetSettingI("Shaders.SMAA.Status", "Enabled") ? "on" : "off");
}

void SessionLog::NoteIni(const GameSetting* setting) {
	if (!setting || !setting->Name) return;
	for (size_t i = 0; i < sizeof(IniNames) / sizeof(IniNames[0]); i++) {
		if (_stricmp(setting->Name, IniNames[i])) continue;
		char text[64];
		const char type = (char)tolower((unsigned char)setting->Name[0]);
		if (type == 'f') sprintf_s(text, "%g", setting->fValue);
		else if (type == 'b') strcpy_s(text, setting->iValue ? "1" : "0");
		else if (type == 'u') sprintf_s(text, "%u", (unsigned)setting->iValue);
		else if (type == 's') strcpy_s(text, "(text)");
		else sprintf_s(text, "%d", setting->iValue);
		IniValues[i] = text;
		return;
	}
}

void SessionLog::OnDeferredInit() {
	std::vector<std::string> ini;
	for (size_t i = 0; i < sizeof(IniNames) / sizeof(IniNames[0]); i++)
		if (!IniValues[i].empty()) { std::string name = IniNames[i]; ini.push_back(name.substr(0, name.find(':')) + "=" + IniValues[i]); }
	LogList("SESSION game INI (as read): ", ini);

	// The load order: count, a hash that tells two players' orders apart at a glance, and the names (not personal; one or two lines)
	if (DataHandler) {
		const ModList& mods = DataHandler->modList;
		const unsigned count = (std::min)((unsigned)mods.loadedModCount, 255u);
		unsigned hash = 2166136261u;
		std::vector<std::string> names;
		for (unsigned i = 0; i < count; i++) {
			const ModInfo* mod = mods.loadedMods[i];
			if (!mod) continue;
			char name[0x105] = {};
			memcpy(name, mod->name, 0x104);
			for (const char* c = name; *c; c++) { hash ^= (unsigned char)tolower((unsigned char)*c); hash *= 16777619u; }
			hash ^= '\n'; hash *= 16777619u;
			names.push_back(name);
		}
		Logger::Log("SESSION load order: %u plugins, hash %08X", (unsigned)names.size(), hash);
		InstallCheck::OnLoadOrder(names, hash);	// 2026-11-03: weather / lighting mods (InstallCheck.h)
		LogList("SESSION load order: ", names);
	}

	// The xNVSE plugin DLLs and anything else loaded from the game folder (wrappers, injectors): names only
	std::vector<std::string> plugins, gameFolder;
	char exe[MAX_PATH] = {};
	GetModuleFileNameA(NULL, exe, MAX_PATH);
	std::string root = exe;
	root = root.substr(0, root.find_last_of("\\/") + 1);
	for (char& c : root) c = (char)tolower((unsigned char)c);
	HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, 0);
	if (snapshot != INVALID_HANDLE_VALUE) {
		MODULEENTRY32W entry = { sizeof(entry) };
		for (BOOL more = Module32FirstW(snapshot, &entry); more; more = Module32NextW(snapshot, &entry)) {
			char path[MAX_PATH * 2] = {};
			WideCharToMultiByte(CP_ACP, 0, entry.szExePath, -1, path, sizeof(path), nullptr, nullptr);
			char name[MAX_PATH] = {};
			WideCharToMultiByte(CP_ACP, 0, entry.szModule, -1, name, sizeof(name), nullptr, nullptr);
			std::string lower = path;
			for (char& c : lower) c = (char)tolower((unsigned char)c);
			if (lower.find("\\nvse\\plugins\\") != std::string::npos) plugins.push_back(name);
			else if (!root.empty() && lower.rfind(root, 0) == 0 && lower.find('\\', root.size()) == std::string::npos && lower.size() > 4 &&
				lower.compare(lower.size() - 4, 4, ".exe")) gameFolder.push_back(name);
		}
		CloseHandle(snapshot);
	}
	Logger::Log("SESSION xNVSE plugins: %u loaded", (unsigned)plugins.size());
	LogList("SESSION xNVSE plugins: ", plugins);
	LogList("SESSION DLLs from the game folder: ", gameFolder);
	LastEnvironment = SessionLog::Environment();	// UNOFFICIAL 2026-11-01
	Logger::Log("SESSION environment: %s", LastEnvironment.c_str());
}

void SessionLog::OnSettingChanged(const char* section, const char* key, const char* before, const char* after) {
	if (!section || !key || !before || !after) return;
	const std::string name = std::string(section) + "." + key;
	const double now = Seconds();
	std::lock_guard<std::mutex> guard(Lock);
	for (PendingChange& c : Pending)
		if (c.key == name) { c.after = after; c.last = now; return; }
	if (!strcmp(before, after)) return;
	if (Pending.size() < PendingCap) Pending.push_back({ name, before, after, now, now });
	else ChangesSuppressed++;
}

void SessionLog::OnEvent(const char* what) {
	FlushChanges(true);
	Logger::Log("GAME %s (game time %s, %s)", what, GameClock().c_str(), LastCell.c_str());
}

void SessionLog::OnExit(const char* how) {
	if (Ended) return;
	Ended = true;
	FlushChanges(true);
	Logger::Log("SESSION END (%s): up %s, %s in game, %u loading screens, %u setting changes (last: %s), last cell %s, weather %s, address space peak %.0f MB, worst frame %.1f ms",
		how, Uptime(Seconds() - StartSeconds).c_str(), Uptime(InGameSeconds).c_str(), LoadingScreens, ChangesTotal, LastChange.c_str(), LastCell.c_str(),
		LastWeather.c_str(), PeakAddressMB, WorstEver);
}

void SessionLog::OnFrame() {
	const double now = Seconds();
	const double frameMs = LastFrameTime >= 0.0 ? (now - LastFrameTime) * 1000.0 : 0.0;
	LastFrameTime = now;
	static double lastSecond = -10.0;
	if (now - lastSecond >= 1.0) {	// once a second: the journal switch, pending setting changes, the address space peak
		lastSecond = now;
		const bool on = TheSettingManager->GetSettingI("Main.Develop.Main", "FrameJournal") != 0;
		if (on != FrameJournal::On) Logger::Log("FJ frame journal %s (%u of %u lines used)", on ? "ON" : "off", FrameJournal::Lines, FrameJournal::Cap);
		FrameJournal::On = on;
		FlushChanges(false);
		PeakAddressMB = (std::max)(PeakAddressMB, AddressSpaceMB());
		if (!PreviousFilter && Player && Player->parentCell) {	// late, so a crash logger set up at load comes first and is chained
			static bool installed = false;
			if (!installed) { installed = true; PreviousFilter = SetUnhandledExceptionFilter(CrashFilter); }
		}
	}

	// Loading screens
	const bool loading = InterfaceManager && InterfaceManager->IsActive(Menu::kMenuType_Loading);
	if (loading && LoadingStart < 0.0) { LoadingStart = now; LoadingScreens++; FlushChanges(true); Logger::Log("LOADING screen"); }
	else if (!loading && LoadingStart >= 0.0) {
		LoadingSeconds += now - LoadingStart;
		Logger::Log("LOADING done after %.1f s", now - LoadingStart);
		LoadingStart = -1.0;
	}

	TESObjectCELL* cell = Player ? Player->parentCell : nullptr;
	if (!cell) return;

	// Cell and worldspace changes (exterior cells: only when the worldspace changes or from an interior; the heartbeat says where)
	static TESObjectCELL* lastCell = nullptr;
	static TESWorldSpace* lastWorld = nullptr;
	static bool lastInterior = false;
	if (cell != lastCell) {
		const bool interior = cell->IsInterior();
		TESWorldSpace* world = interior ? nullptr : cell->worldSpace;
		const bool report = interior || lastInterior || world != lastWorld || !lastCell;
		LastCell = CellText(cell);
		if (report) { FlushChanges(true); Logger::Log("CELL %s (game time %s)", LastCell.c_str(), GameClock().c_str()); }
		lastCell = cell; lastWorld = world; lastInterior = interior;
	}
	// Weather changes
	static TESWeather* lastWeather = nullptr;
	TESWeather* weather = Tes && Tes->sky ? Tes->sky->firstWeather : nullptr;
	if (weather != lastWeather) {
		lastWeather = weather;
		char text[160];
		const char* name = weather ? weather->GetEditorName() : nullptr;
		sprintf_s(text, "%08X '%s'", weather ? weather->refID : 0, name && name[0] ? name : "?");
		LastWeather = text;
		Logger::Log("WEATHER %s (game time %s)", text, GameClock().c_str());
	}

	if (TheShaderManager->GameState.isCameraCut) Count.cameraCuts++;
	if (FrameJournal::On) {	// per-frame counts: one line when they change by more than the frame-to-frame noise
		static unsigned lastDrops = 0;
		const unsigned drops = FrameJournal::FadeDrops;
		if (drops > lastDrops + (std::max)(4u, lastDrops / 8) || drops + (std::max)(4u, lastDrops / 8) < lastDrops) {
			FRAME_JOURNAL("shadow map casters dropped by CasterFadeMin (LOD fade): %u (was %u)", drops, lastDrops);
			lastDrops = drops;
		}
	}
	FrameJournal::FadeDrops = 0;

	// The heartbeat's frame times: frames in game, loading screens left out
	if (WindowStart < 0.0) WindowStart = now;
	if (!loading && frameMs > 0.0 && frameMs < 60000.0) {
		FrameMs.push_back(frameMs);
		FrameRing[FrameRingNext++ % (sizeof(FrameRing) / sizeof(FrameRing[0]))] = (float)frameMs;	// UNOFFICIAL 2026-11-01 (REPORT)
		InGameSeconds += frameMs / 1000.0;
		if (frameMs > WorstEver && now - StartSeconds > 1.0) WorstEver = frameMs;
	}
	if (now - WindowStart >= 60.0) Heartbeat(now);
}
