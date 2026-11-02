// Tests for src/base/LogScrub.h (UNOFFICIAL 2026-11-01): the log privacy scrubber, on representative NVR log lines, with a fake
// machine (fixed folders and names) and with this machine's own (BuildFromSystem), plus the frame number in the line prefix.
#include <cstdio>
#include <string>
#include "../src/base/LogScrub.h"
#include "../src/base/LogFiles.h"

static int failures = 0;
#define CHECK(cond, what) do { if (!(cond)) { printf("FAIL: %s\n", what); failures++; } } while (0)

static std::string Run(const LogScrub::Scrubber& s, const std::string& line) {
	std::string out;
	return s.Apply(line.data(), line.size(), out) ? out : line;
}

static void Expect(const LogScrub::Scrubber& s, const std::string& line, const std::string& want) {
	const std::string got = Run(s, line);
	if (got != want) { printf("FAIL:\n  in:   %s\n  got:  %s\n  want: %s\n", line.c_str(), got.c_str(), want.c_str()); failures++; }
}

static std::string Prefix(unsigned frame) {
	SYSTEMTIME t = {};
	t.wHour = 10; t.wMinute = 56; t.wSecond = 38; t.wMilliseconds = 243;
	char text[64];
	LogFiles::LinePrefix(text, sizeof(text), t, frame);
	return text;
}

int main() {
	LogScrub::Scrubber s;
	s.AddPath("C:\\Users\\John Smith\\Games\\Fallout New Vegas\\", "<GAME>");
	s.AddPath("C:\\Users\\JOHNSM~1\\Games\\FALLOU~1", "<GAME>");
	s.AddPath("C:\\Users\\John Smith", "%USERPROFILE%");
	s.AddPath("C:\\Users\\JOHNSM~1", "%USERPROFILE%");
	s.AddPath("D:\\Docs", "%DOCUMENTS%");
	s.AddWord("JohnS", "<USER>");
	s.AddWord("John Smith", "<USER>");
	s.AddWord("DESKTOP-K3J9Q2", "<PC>");
	s.AddWord("user", "<USER>");	// generic: ignored as a word
	s.AddWord("Al", "<USER>");		// too short: ignored as a word
	s.Finish();

	// The game folder, every spelling
	Expect(s, "Loading settings from file C:\\Users\\John Smith\\Games\\Fallout New Vegas\\Data\\NVSE\\Plugins\\NewVegasReloaded.dll.toml",
		"Loading settings from file <GAME>\\Data\\NVSE\\Plugins\\NewVegasReloaded.dll.toml");
	Expect(s, "D3D9 runtime: DXVK (c:/users/john smith/games/fallout new vegas/d3d9.dll)", "D3D9 runtime: DXVK (<GAME>/d3d9.dll)");
	Expect(s, "Shader compiled: C:\\Users\\JOHNSM~1\\Games\\FALLOU~1\\Shaders\\x.pso", "Shader compiled: <GAME>\\Shaders\\x.pso");
	Expect(s, "C:\\Users\\John Smith\\Games\\Fallout New Vegas", "<GAME>");
	// The profile, and folders in it
	Expect(s, "NEON SELF TEST a: shots to C:\\Users\\John Smith\\AppData\\Local\\Temp\\x", "NEON SELF TEST a: shots to %USERPROFILE%\\AppData\\Local\\Temp\\x");
	Expect(s, "path C:\\Users\\John Smith\\Games\\Fallout New Vegas2\\x", "path %USERPROFILE%\\Games\\Fallout New Vegas2\\x");	// not the game folder
	Expect(s, "Documents: D:\\Docs\\My Games\\FalloutNV\\FalloutPrefs.ini", "Documents: %DOCUMENTS%\\My Games\\FalloutNV\\FalloutPrefs.ini");
	Expect(s, "not a match: D:\\Docs2\\x", "not a match: D:\\Docs2\\x");
	// Other accounts and other systems
	Expect(s, "MaterialRules: E:\\Users\\Someone Else\\nvr\\NewVegasReloaded.dll.materials.txt not found",
		"MaterialRules: %USERPROFILE%\\nvr\\NewVegasReloaded.dll.materials.txt not found");
	Expect(s, "old: C:\\Documents and Settings\\bob\\x.txt", "old: %USERPROFILE%\\x.txt");
	Expect(s, "Proton: Z:\\home\\alice\\.steam\\steam\\steamapps\\common\\x", "Proton: %USERPROFILE%\\.steam\\steam\\steamapps\\common\\x");
	Expect(s, "Linux: /home/alice/Games/fnv and mac /Users/bob/fnv", "Linux: %USERPROFILE%/Games/fnv and mac %USERPROFILE%/fnv");
	Expect(s, "a texture: Data\\Textures\\home\\wall.dds", "a texture: Data\\Textures\\home\\wall.dds");	// relative: kept
	Expect(s, "relative Data/home/users/x", "relative Data/home/users/x");
	// Mod Organizer, Steam
	Expect(s, "ini F:\\MO2\\profiles\\John's Main\\FalloutPrefs.ini", "ini F:\\MO2\\profiles\\<PROFILE>\\FalloutPrefs.ini");
	Expect(s, "G:\\Steam\\userdata\\123456789\\config", "G:\\Steam\\userdata\\<STEAMID>\\config");
	Expect(s, "id 76561198012345678 seen", "id <STEAMID> seen");
	Expect(s, "frames 765611980123456789 (18 digits)", "frames 765611980123456789 (18 digits)");
	// Names as words
	Expect(s, "window of JohnS on DESKTOP-K3J9Q2: ok", "window of <USER> on <PC>: ok");
	Expect(s, "johns_backup.toml", "<USER>_backup.toml");
	Expect(s, "JohnSmith stays, JohnS2 stays", "JohnSmith stays, JohnS2 stays");
	Expect(s, "John Smith's save", "<USER>'s save");
	Expect(s, "the user toml and the AL flicker", "the user toml and the AL flicker");
	// Ordinary lines untouched (and not copied)
	{
		const std::string line = "UNOFFICIAL AmbientLight: blur compute path ready (1920x1080, 3.2 ms) Data\\NVSE\\Plugins\\x.toml";
		std::string out = "unchanged";
		CHECK(!s.Apply(line.data(), line.size(), out) && out == "unchanged", "an ordinary line is left alone");
	}
	Expect(s, "", "");

	// This machine: whatever its folders and names are, none survive in a line that holds them.
	LogScrub::Scrubber real;
	real.BuildFromSystem();
	char buffer[MAX_PATH * 2];
	DWORD size = sizeof(buffer);
	std::string name = GetUserNameA(buffer, &size) ? buffer : "";
	size = sizeof(buffer);
	std::string pc = GetComputerNameA(buffer, &size) ? buffer : "";
	SHGetFolderPathA(NULL, CSIDL_PROFILE, NULL, SHGFP_TYPE_CURRENT, buffer);
	std::string profile = buffer;
	GetModuleFileNameA(NULL, buffer, MAX_PATH);
	std::string exe = buffer;
	GetCurrentDirectoryA(MAX_PATH, buffer);
	std::string cwd = buffer;
	auto lower = [](std::string t) { for (char& c : t) c = LogScrub::Lower(c); return t; };
	const std::string line = "exe " + exe + " cwd " + cwd + "\\x profile " + profile + "\\Documents user " + name + " pc " + pc + " end";
	const std::string got = lower(Run(real, line));
	printf("this machine: %s\n", Run(real, line).c_str());
	if (name.size() >= 3 && name != "user") CHECK(got.find(lower(name)) == std::string::npos, "this machine's user name scrubbed");
	if (pc.size() >= 3) CHECK(got.find(lower(pc)) == std::string::npos, "this machine's computer name scrubbed");
	CHECK(got.find(lower(profile)) == std::string::npos, "this machine's profile scrubbed");
	CHECK(got.find("<game>") != std::string::npos, "the exe's folder became <GAME>");

	// Frame numbers in the prefix
	CHECK(Prefix(LogFiles::NoFrame) == "[10:56:38.243] ", "prefix without a frame");
	CHECK(Prefix(12345) == "[10:56:38.243 f12345] ", "prefix with a frame");

	// Cost: a typical line, many times
	{
		const std::string typical = "UNOFFICIAL ShadowAtlas: 12 lamps, 3 tiles re-converted, slot 4 handed from 0x1A2B3C to 0x4D5E6F (frame 123456)";
		LARGE_INTEGER f, a, b;
		QueryPerformanceFrequency(&f);
		QueryPerformanceCounter(&a);
		std::string out;
		unsigned changed = 0;
		for (int k = 0; k < 100000; k++) changed += real.Apply(typical.data(), typical.size(), out) ? 1 : 0;
		QueryPerformanceCounter(&b);
		printf("scrub cost: %.2f us per typical line (%u changed)\n", (b.QuadPart - a.QuadPart) * 1e6 / f.QuadPart / 100000.0, changed);
	}

	if (failures) { printf("%d log scrub test(s) FAILED\n", failures); return 1; }
	printf("All log scrub tests passed.\n");
	return 0;
}
