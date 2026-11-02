#pragma once
// UNOFFICIAL 2026-10-31 register-clash guard: two constants pinned to overlapping registers in one shader (D3DX error X4500, "overlapping
// register semantics not yet implemented") make that shader fail to compile, and NVR falls back without saying why in a way anyone reads
// (2026-10-31: MaterialBuffer's c217 and ShadowContrast's c207 against the RT lamp shadows' c216-c217 / c206-c209 took every interior
// lamp-shadow shader down that way, unnoticed for two days). Every compile's error text goes through Check: each clash is logged once as
// an [ERROR] REGISTER CLASH line naming the shader and the source line; Summary (after the startup shader load) gives the total.
#include <set>
#include <string>
#include <mutex>

namespace RegisterClash {
	inline std::mutex Lock;
	inline std::set<std::string> Seen;
	inline unsigned Count = 0;

	inline void Check(const char* shader, const char* text) {
		if (!text || !strstr(text, "X4500")) return;
		std::lock_guard<std::mutex> guard(Lock);
		const char* p = text;
		while ((p = strstr(p, "X4500")) != nullptr) {
			const char* start = p; while (start > text && start[-1] != '\n') start--;
			const char* end = strchr(p, '\n'); if (!end) end = p + strlen(p);
			std::string line(start, end);
			while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
			Count++;
			if (Seen.insert(line).second)
				Logger::Log("[ERROR] REGISTER CLASH (X4500) in %s: %s -- two constants share a register; this shader falls back", shader ? shader : "?", line.c_str());
			p = end;
		}
	}
	inline void Summary() {
		std::lock_guard<std::mutex> guard(Lock);
		if (Count) Logger::Log("[ERROR] REGISTER CLASH summary: %u failed compiles from %u distinct clashes (see the REGISTER CLASH lines)", Count, (unsigned)Seen.size());
		else Logger::Log("Register clash check: no X4500 in any shader compiled so far");
	}
}
