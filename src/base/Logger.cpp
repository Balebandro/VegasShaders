#include "Logger.h"
#include "LogFiles.h"
#include "LogScrub.h"	// UNOFFICIAL 2026-11-01: every line is scrubbed of user / machine names and folders (players post their logs)

FILE* Logger::LogFile = nullptr;
std::string Logger::FilePath;

//#define debugmode
//#define logperf

namespace {

// Render-state names that the trace output can refer to, with their D3DRS_* numbers.
struct NamedRenderState {
	const char* name;
	int id;
};

constexpr NamedRenderState kNamedRenderStates[] = {
	{"D3DRS_ZENABLE", 7},
	{"D3DRS_FILLMODE", 8},
	{"D3DRS_SHADEMODE", 9},
	{"D3DRS_ZWRITEENABLE", 14},
	{"D3DRS_ALPHATESTENABLE", 15},
	{"D3DRS_LASTPIXEL", 16},
	{"D3DRS_SRCBLEND", 19},
	{"D3DRS_DESTBLEND", 20},
	{"D3DRS_CULLMODE", 22},
	{"D3DRS_ZFUNC", 23},
	{"D3DRS_ALPHAREF", 24},
	{"D3DRS_ALPHAFUNC", 25},
	{"D3DRS_DITHERENABLE", 26},
	{"D3DRS_ALPHABLENDENABLE", 27},
	{"D3DRS_FOGENABLE", 28},
	{"D3DRS_SPECULARENABLE", 29},
	{"D3DRS_FOGCOLOR", 34},
	{"D3DRS_FOGTABLEMODE", 35},
	{"D3DRS_FOGSTART", 36},
	{"D3DRS_FOGEND", 37},
	{"D3DRS_FOGDENSITY", 38},
	{"D3DRS_RANGEFOGENABLE", 48},
	{"D3DRS_STENCILENABLE", 52},
	{"D3DRS_STENCILFAIL", 53},
	{"D3DRS_STENCILZFAIL", 54},
	{"D3DRS_STENCILPASS", 55},
	{"D3DRS_STENCILFUNC", 56},
	{"D3DRS_STENCILREF", 57},
	{"D3DRS_STENCILMASK", 58},
	{"D3DRS_STENCILWRITEMASK", 59},
	{"D3DRS_TEXTUREFACTOR", 60},
	{"D3DRS_WRAP0", 128},
	{"D3DRS_WRAP1", 129},
	{"D3DRS_WRAP2", 130},
	{"D3DRS_WRAP3", 131},
	{"D3DRS_WRAP4", 132},
	{"D3DRS_WRAP5", 133},
	{"D3DRS_WRAP6", 134},
	{"D3DRS_WRAP7", 135},
	{"D3DRS_CLIPPING", 136},
	{"D3DRS_LIGHTING", 137},
	{"D3DRS_AMBIENT", 139},
	{"D3DRS_FOGVERTEXMODE", 140},
	{"D3DRS_COLORVERTEX", 141},
	{"D3DRS_LOCALVIEWER", 142},
	{"D3DRS_NORMALIZENORMALS", 143},
	{"D3DRS_DIFFUSEMATERIALSOURCE", 145},
	{"D3DRS_SPECULARMATERIALSOURCE", 146},
	{"D3DRS_AMBIENTMATERIALSOURCE", 147},
	{"D3DRS_EMISSIVEMATERIALSOURCE", 148},
	{"D3DRS_VERTEXBLEND", 151},
	{"D3DRS_CLIPPLANEENABLE", 152},
	{"D3DRS_POINTSIZE", 154},
	{"D3DRS_POINTSIZE_MIN", 155},
	{"D3DRS_POINTSPRITEENABLE", 156},
	{"D3DRS_POINTSCALEENABLE", 157},
	{"D3DRS_POINTSCALE_A", 158},
	{"D3DRS_POINTSCALE_B", 159},
	{"D3DRS_POINTSCALE_C", 160},
	{"D3DRS_MULTISAMPLEANTIALIAS", 161},
	{"D3DRS_MULTISAMPLEMASK", 162},
	{"D3DRS_PATCHEDGESTYLE", 163},
	{"D3DRS_DEBUGMONITORTOKEN", 165},
	{"D3DRS_POINTSIZE_MAX", 166},
	{"D3DRS_INDEXEDVERTEXBLENDENABLE", 167},
	{"D3DRS_COLORWRITEENABLE", 168},
	{"D3DRS_TWEENFACTOR", 170},
	{"D3DRS_BLENDOP", 171},
	{"D3DRS_POSITIONDEGREE", 172},
	{"D3DRS_NORMALDEGREE", 173},
	{"D3DRS_SCISSORTESTENABLE", 174},
	{"D3DRS_SLOPESCALEDEPTHBIAS", 175},
	{"D3DRS_ANTIALIASEDLINEENABLE", 176},
	{"D3DRS_MINTESSELLATIONLEVEL", 178},
	{"D3DRS_MAXTESSELLATIONLEVEL", 179},
	{"D3DRS_ADAPTIVETESS_X", 180},
	{"D3DRS_ADAPTIVETESS_Y", 181},
	{"D3DRS_ADAPTIVETESS_Z", 182},
	{"D3DRS_ADAPTIVETESS_W", 183},
	{"D3DRS_ENABLEADAPTIVETESSELLATION", 184},
	{"D3DRS_TWOSIDEDSTENCILMODE", 185},
	{"D3DRS_CCW_STENCILFAIL", 186},
	{"D3DRS_CCW_STENCILZFAIL", 187},
	{"D3DRS_CCW_STENCILPASS", 188},
	{"D3DRS_CCW_STENCILFUNC", 189},
	{"D3DRS_COLORWRITEENABLE1", 190},
	{"D3DRS_COLORWRITEENABLE2", 191},
	{"D3DRS_COLORWRITEENABLE3", 192},
	{"D3DRS_BLENDFACTOR", 193},
	{"D3DRS_SRGBWRITEENABLE", 194},
	{"D3DRS_DEPTHBIAS", 195},
	{"D3DRS_WRAP8", 198},
	{"D3DRS_WRAP9", 199},
	{"D3DRS_WRAP10", 200},
	{"D3DRS_WRAP11", 201},
	{"D3DRS_WRAP12", 202},
	{"D3DRS_WRAP13", 203},
	{"D3DRS_WRAP14", 204},
	{"D3DRS_WRAP15", 205},
	{"D3DRS_SEPARATEALPHABLENDENABLE", 206},
	{"D3DRS_SRCBLENDALPHA", 207},
	{"D3DRS_DESTBLENDALPHA", 208},
	{"D3DRS_BLENDOPALPHA", 209},
	{"D3DRS_FORCE_DWORD", 0x7fffffff},
};

// Name -> id map used by TraceRenderState, filled from the table above.
stateMap s_renderStateIds;

void FillRenderStateIds() {
	for (const NamedRenderState& named : kNamedRenderStates)
		s_renderStateIds[named.name] = named.id;
}

// UNOFFICIAL 2026-11-01: built once, before the first line is written (Initialize / InitializeRotating); read-only afterwards.
LogScrub::Scrubber s_scrubber;

void BuildScrubberOnce() {
	static bool built = false;
	if (built) return;
	built = true;
	s_scrubber.BuildFromSystem();
}

} // namespace

TimeLogger::TimeLogger() {
	// One clock read, not two. system_clock::now() is a real cost on Windows and these are
	// constructed in hot paths; end is only a scratch value until LogTime overwrites it.
	start = end = std::chrono::system_clock::now();
}

TimeLogger::~TimeLogger() = default;

// Returns the milliseconds since the last call (or since construction) and restarts the interval.
float TimeLogger::LogTime(const char* Name) {
	end = std::chrono::system_clock::now();
	const auto span = (end - start) * 1000;
	std::chrono::duration<double> seconds = span;
#ifdef logperf
	Logger::Log("%s ran in %f ms", Name, seconds.count());
#endif
	start = end;
	return static_cast<float>(seconds.count());
}

void Logger::Initialize(const char* FileName) {

	BuildScrubberOnce();
	FILE* opened = _fsopen(FileName, "w", _SH_DENYWR);
	LogFile = opened;
	FilePath = FileName ? FileName : "";
	FillRenderStateIds();
}

void Logger::InitializeRotating(const char* FolderName, const char* BaseName, unsigned Keep) {

	BuildScrubberOnce();

	// The game folder is the exe's folder. Its "A" path turns characters outside the ANSI code page into '?', so the
	// current folder (where the single log always went) is the second choice.
	char exePath[MAX_PATH] = {};
	const DWORD length = GetModuleFileNameA(NULL, exePath, MAX_PATH);
	std::string exeFolder(exePath, length);
	exeFolder.resize(length && length < MAX_PATH ? exeFolder.find_last_of("\\/") + 1 : 0); // keeps the trailing slash

	SYSTEMTIME now;
	GetLocalTime(&now);
	const LogFiles::NewLog log = LogFiles::OpenNewLog({ exeFolder, std::string() }, FolderName, BaseName, Keep, now);
	if (log.file) {
		LogFile = log.file;
		FilePath = exeFolder + log.name;
		FillRenderStateIds();
		Log("Log started %04u-%02u-%02u %02u:%02u:%02u: %s (this folder keeps the newest %u logs; %u older deleted%s)",
			now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond, log.name.c_str(), Keep, log.deleted,
			log.adopted ? "; the old single log from the game folder was moved here" : "");
		return;
	}
	Initialize((std::string(BaseName) + ".log").c_str());
	Log("Could not create a log in the %s folder; logging to %s.log in the game folder instead.", FolderName, BaseName);
}

std::deque<std::string> Logger::s_recentLines;
std::mutex Logger::s_recentMutex;

void Logger::PushRecent(const char* Line, size_t Length) {
	std::lock_guard<std::mutex> guard(s_recentMutex);
	s_recentLines.emplace_back(Line, Length);
	while (s_recentLines.size() > kRecentLimit)
		s_recentLines.pop_front();
}

void Logger::GetRecentLines(std::deque<std::string>& OutLines) {
	std::lock_guard<std::mutex> guard(s_recentMutex);
	OutLines.assign(s_recentLines.begin(), s_recentLines.end()); // copy out; the caller never renders while holding the lock
}

void Logger::Write(bool ToRingBuffer, const char* Message, va_list Args) {

	// Formatted once; lines longer than the stack buffer go to the heap.
	char stackLine[2048];
	std::string heapLine;
	SYSTEMTIME now;
	GetLocalTime(&now);
	size_t length, prefix;
	// UNOFFICIAL 2026-11-01: the rendered frame number after the time ("[10:56:38.243 f12345]"), to match lines with bench shots
	const unsigned frame = TheFrameRateManager ? TheFrameRateManager->FrameNumber : LogFiles::NoFrame;
	const char* line = LogFiles::FormatLine(stackLine, sizeof(stackLine), heapLine, now, Message, Args, length, prefix, frame);
	// UNOFFICIAL 2026-11-01: privacy scrub of the message (LogScrub.h), always on; a line with nothing to replace is not copied
	std::string scrubbed;
	if (s_scrubber.Apply(line + prefix, length - prefix, scrubbed)) {
		scrubbed.insert(0, line, prefix);
		line = scrubbed.c_str();
		length = scrubbed.size();
	}

	if (FILE* out = LogFile) {
		fwrite(line, 1, length, out);
		fflush(out);
	}

	// Mirror into the in-memory ring buffer for the in-game log window
	// (docs/preset-manager-design.md § "Debug/authoring tooling"), without the time prefix and the newline.
	if (ToRingBuffer) PushRecent(line + prefix, length - prefix - 1);
}

void Logger::Log(char* Message, ...) {
	std::va_list args;
	va_start(args, Message);
	Write(true, Message, args);
	va_end(args);
}

void Logger::Log(const char* Message, ...) {
	std::va_list args;
	va_start(args, Message);
	Write(true, Message, args);
	va_end(args);
}

void Logger::LogUnbuffered(const char* Message, ...) {
	std::va_list args;
	va_start(args, Message);
	Write(false, Message, args);
	va_end(args);
}

void Logger::Debug(char* Message, ...) {
#ifdef debugmode
	if (LogFile) {
		std::va_list args;
		va_start(args, Message);
		Write(false, Message, args);
		va_end(args);
	}
#endif // debugmode
}

void Logger::Debug(const char* Message, ...) {
#ifdef debugmode
	if (LogFile) {
		std::va_list args;
		va_start(args, Message);
		Write(false, Message, args);
		va_end(args);
	}
#endif // debugmode
}

NiDX9RenderState::NiRenderStateSetting* RenderStateSettings;

// Logs every render state whose value changed since the previous call. The first call only takes a snapshot.
void Logger::TraceRenderState() {
	NiDX9RenderState* live = TheRenderManager->renderState;

	if (RenderStateSettings == nullptr) {
		const size_t bytes = sizeof(NiDX9RenderState::NiRenderStateSetting) * 256;
		RenderStateSettings = (NiDX9RenderState::NiRenderStateSetting*)malloc(bytes);
		memcpy(RenderStateSettings, live->RenderStateSettings, bytes);
		return;
	}

	bool changed = false;
	for (const auto& entry : s_renderStateIds) {
		const char* settingName = entry.first;
		const int index = entry.second;
		auto& previous = RenderStateSettings[index];
		const auto& current = live->RenderStateSettings[index];
		if (previous.CurrentValue != current.CurrentValue) {
			Logger::Log("Different state between iterations: State %s was %u is %u", settingName, previous.CurrentValue, current.CurrentValue);
			previous.CurrentValue = current.CurrentValue;
			changed = true;
		}
	}
	if (changed) Logger::Log("End");
}
