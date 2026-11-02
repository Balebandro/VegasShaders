#pragma once

// UNOFFICIAL CpuSampler (TEST BUILDS ONLY, 2026-10-21, side session cpulimits; bench\deep-roadmap\CPU-LIMITS.md).
// Whose code fills each part of the CPU frame? CpuSplit (F10) says how long each part takes; this says what runs inside it.
// A sampling profiler after ArrivalSampler (2026-10-20): a thread of ours wakes every ~1 ms, suspends the main thread (and the
// two AI threads, once CpuSplit has seen them), reads its registers and copies its live stack, resumes it, and only then looks at
// the copy. Each sample is counted under (CpuSplit bucket the thread is in, owner), where the owner is the innermost code that is
// not Windows/C runtime: the game (FalloutNV.exe), NVR, another plugin, or the graphics driver / D3D9 runtime (then also who
// called it: the game, NVR or a plugin). Leaf addresses and the game's calls into the driver are counted too, for symbols offline
// (bench\deep-roadmap\cpulimits\samples.py; exe names from the IDA map, NVR names from its PDB).
// Started by the shot list's 'sampler' line (SettingShots.h); written per shot to VegasShaders-Logs\CpuSamples_<time>.txt
// when the next shot starts. While the main thread is suspended this thread takes no lock and allocates nothing.
// Cost: ~1-2 % of the main thread (ArrivalSampler's measure); its numbers come from separate runs, not from the timing runs.
#include <tlhelp32.h>
#include <intrin.h>
#include <share.h>
#include <unordered_map>
#include <vector>
#include <algorithm>
#include "TestTools.h"
#include "CpuSplit.h"

namespace CpuSampler {
#if NVR_TEST_TOOLS
	enum Owner : UInt8 { oGame, oNVR, oPlugin, oDriverGame, oDriverNVR, oDriverPlugin, oDriverOther, oOther, kOwners };
	static const char* const OwnerNames[kOwners] = { "game", "NVR", "plugin", "driver<-game", "driver<-NVR", "driver<-plugin", "driver<-other", "other" };
	enum Kind : UInt8 { kSystem, kDriver, kExe, kNVR, kPluginDll, kUnknown };
	struct Range { DWORD Start, End; int Module; };
	static const unsigned kThreads = 3;	// main, AI1 (or the combined AI thread), AI2
	static const unsigned kNoBucket = CpuSplit::kBucketCount, kRows = CpuSplit::kBucketCount + 1;

	inline int Hz = 0;
	inline bool Started = false;
	inline HANDLE Handles[kThreads] = {};
	inline DWORD Ids[kThreads] = {}, StackBase[kThreads] = {}, StackBottom[kThreads] = {};
	inline unsigned char* StackCopy = nullptr;
	static const DWORD kStackCopyMax = 128 * 1024;
	inline Range Ranges[1024];
	inline int RangeCount = 0;
	inline char ModuleNames[256][64];
	inline DWORD ModuleBase[256], ModuleSize[256];
	inline Kind ModuleKind[256];
	inline int ModuleCount = 0;
	inline FILE* Out = nullptr;
	inline LONGLONG QpcFreq = 1;

	// this shot's counts (sampler thread only)
	inline unsigned Counts[kThreads][kRows][kOwners];
	inline unsigned KernelCounts[kThreads][kRows][kOwners];	// of those, the thread was inside a Windows kernel call (waiting or a syscall)
	inline int Wow64Cpu = -1;	// module index of wow64cpu.dll (the 32-bit side of every syscall)
	// How much the thread really ran: its consumed CPU cycles (QueryThreadCycleTime, TSC-based) since the previous sample over the
	// wall time between them. The share is booked to the bucket the thread is in at the sample: 1 = computing the whole time,
	// 0 = waiting (the driver waiting for the graphics card, a lock, a semaphore). Written as W lines.
	inline double BusySum[kThreads][kRows];
	inline unsigned BusyN[kThreads][kRows];
	inline ULONG64 LastCycles[kThreads];
	inline LONGLONG LastBusyQpc[kThreads];
	inline LONGLONG CalQpc = 0;
	inline unsigned long long CalTsc = 0;
	inline unsigned Taken[kThreads];
	inline std::unordered_map<unsigned long long, unsigned> Leaves;	// (thread << 56 | bucket << 40 | owner << 32 | address) -> samples
	inline std::unordered_map<unsigned long long, unsigned> Callers;	// (bucket << 40 | caller owner << 32 | return address) -> driver samples
	inline int CurrentShot = -1;
	inline LONGLONG ShotStart = 0;
	inline double PauseSumUs = 0.0, PauseMaxUs = 0.0;
	inline unsigned long long PauseCount = 0;

	inline LONGLONG Qpc() { LARGE_INTEGER c; QueryPerformanceCounter(&c); return c.QuadPart; }

	inline Kind KindOf(const char* name) {
		char n[64]; strcpy_s(n, name); _strlwr_s(n);
		if (!strcmp(n, "falloutnv.exe")) return kExe;
		if (!strcmp(n, "newvegasreloaded.dll")) return kNVR;
		if (strstr(n, "d3d9") || strstr(n, "nvd3dum") || strstr(n, "nvwgf") || strstr(n, "nvgpucomp") || strstr(n, "atiumd") || strstr(n, "aticfx") ||
			strstr(n, "igdumd") || strstr(n, "igd9") || strstr(n, "dxgi") || strstr(n, "dxvk") || strstr(n, "d3dx9")) return kDriver;
		if (!strcmp(n, "ntdll.dll") || !strcmp(n, "kernel32.dll") || !strcmp(n, "kernelbase.dll") || !strcmp(n, "ucrtbase.dll") || !strcmp(n, "msvcrt.dll") ||
			strstr(n, "msvcp") || strstr(n, "vcruntime") || !strcmp(n, "user32.dll") || !strcmp(n, "win32u.dll") || !strcmp(n, "gdi32.dll") ||
			!strcmp(n, "gdi32full.dll") || !strcmp(n, "wow64.dll") || !strcmp(n, "wow64cpu.dll") || !strcmp(n, "combase.dll")) return kSystem;
		return kPluginDll;
	}

	// Sampler thread, threads RUNNING: the loaded modules and their executable sections.
	inline void ScanModules() {
		HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
		if (snap == INVALID_HANDLE_VALUE) return;
		MODULEENTRY32W me = {}; me.dwSize = sizeof(me);
		Range ranges[1024]; int rc = 0;
		for (BOOL ok = Module32FirstW(snap, &me); ok; ok = Module32NextW(snap, &me)) {
			const DWORD base = (DWORD)(uintptr_t)me.modBaseAddr, size = me.modBaseSize;
			int m = -1;
			for (int i = 0; i < ModuleCount; i++) if (ModuleBase[i] == base && ModuleSize[i] == size) { m = i; break; }
			if (m < 0 && ModuleCount < 256) {
				m = ModuleCount++;
				ModuleBase[m] = base; ModuleSize[m] = size;
				if (!WideCharToMultiByte(CP_ACP, 0, me.szModule, -1, ModuleNames[m], 64, nullptr, nullptr)) strcpy_s(ModuleNames[m], "?");
				ModuleKind[m] = KindOf(ModuleNames[m]);
				if (!_stricmp(ModuleNames[m], "wow64cpu.dll")) Wow64Cpu = m;
				if (Out) fprintf(Out, "M %08lX %08lX %s\n", (unsigned long)base, (unsigned long)size, ModuleNames[m]);
			}
			if (m < 0) continue;
			__try {
				const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)(uintptr_t)base;
				if (dos->e_magic != IMAGE_DOS_SIGNATURE) continue;
				const IMAGE_NT_HEADERS32* nt = (const IMAGE_NT_HEADERS32*)((uintptr_t)base + dos->e_lfanew);
				if (nt->Signature != IMAGE_NT_SIGNATURE) continue;
				const IMAGE_SECTION_HEADER* s = IMAGE_FIRST_SECTION(nt);
				for (unsigned i = 0; i < nt->FileHeader.NumberOfSections && rc < 1024; i++)
					if (s[i].Characteristics & IMAGE_SCN_MEM_EXECUTE)
						ranges[rc++] = { base + s[i].VirtualAddress, base + s[i].VirtualAddress + max(s[i].Misc.VirtualSize, s[i].SizeOfRawData), m };
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {}
		}
		CloseHandle(snap);
		std::sort(ranges, ranges + rc, [](const Range& a, const Range& b) { return a.Start < b.Start; });
		memcpy(Ranges, ranges, sizeof(Range) * rc);
		RangeCount = rc;
		if (Out) fflush(Out);
	}

	inline int ModuleAt(DWORD v) {
		int lo = 0, hi = RangeCount - 1;
		while (lo <= hi) {
			const int mid = (lo + hi) >> 1;
			if (v < Ranges[mid].Start) hi = mid - 1;
			else if (v >= Ranges[mid].End) lo = mid + 1;
			else return Ranges[mid].Module;
		}
		return -1;
	}

	// A return address follows a call: E8 rel32 (5), FF /2 with modrm forms of 2, 3, 4 (SIB + disp8), 6 (disp32), 7 (SIB + disp32).
	inline bool AfterCall(DWORD v) {
		__try {
			const unsigned char* p = (const unsigned char*)(uintptr_t)v;
			if (p[-5] == 0xE8) return true;
			if (p[-2] == 0xFF && (p[-1] & 0x38) == 0x10) return true;
			if (p[-3] == 0xFF && (p[-2] & 0x38) == 0x10) return true;
			if (p[-6] == 0xFF && (p[-5] & 0x38) == 0x10) return true;
			if (p[-4] == 0xFF && (p[-3] & 0x38) == 0x10) return true;
			if (p[-7] == 0xFF && (p[-6] & 0x38) == 0x10) return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {}
		return false;
	}

	inline Kind KindAt(DWORD v, int* module = nullptr) {
		const int m = ModuleAt(v);
		if (module) *module = m;
		return m < 0 ? kUnknown : ModuleKind[m];
	}

	// The CpuSplit bucket a thread is in now (its list's last stamp), read while it is suspended.
	inline unsigned BucketNow(unsigned thread) {
		using namespace CpuSplit;
		unsigned list = kMain;
		if (thread == 1) list = Lists[kAICombined].count > 0 && Lists[kAI1].count == 0 ? kAICombined : kAI1;
		else if (thread == 2) list = kAI2;
		const LONG n = Lists[list].count;
		if (n <= 0 || (UInt32)n > Lists[list].cap) return kNoBucket;
		const UInt8 b = BucketOf(Lists[list].items[n - 1].id);
		return b < kBucketCount ? b : kNoBucket;
	}

	inline void TakeSample(unsigned thread) {
		if (!Handles[thread]) return;
		CONTEXT ctx = {}; ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
		DWORD len = 0;
		unsigned bucket = kNoBucket;
		const LONGLONG t0 = Qpc();
		if (SuspendThread(Handles[thread]) == (DWORD)-1) return;
		// ---- suspended: no lock, no allocation, no logging until ResumeThread ----
		const BOOL got = GetThreadContext(Handles[thread], &ctx);
		if (got && ctx.Esp >= StackBottom[thread] && ctx.Esp < StackBase[thread]) {
			len = min(StackBase[thread] - ctx.Esp, kStackCopyMax);
			memcpy(StackCopy, (const void*)(uintptr_t)ctx.Esp, len);
		}
		bucket = BucketNow(thread);
		ULONG64 cycles = 0;
		const BOOL gotCycles = QueryThreadCycleTime(Handles[thread], &cycles);
		const LONGLONG cycleQpc = Qpc();
		ResumeThread(Handles[thread]);
		// ---- running ----
		if (gotCycles) {
			const double tscPerQpc = cycleQpc > CalQpc ? (double)(__rdtsc() - CalTsc) / (double)(Qpc() - CalQpc) : 0.0;
			if (LastBusyQpc[thread] && tscPerQpc > 0.0 && cycleQpc > LastBusyQpc[thread]) {
				double busy = (double)(cycles - LastCycles[thread]) / ((double)(cycleQpc - LastBusyQpc[thread]) * tscPerQpc);
				busy = busy < 0.0 ? 0.0 : busy > 1.0 ? 1.0 : busy;
				BusySum[thread][bucket] += busy;
				BusyN[thread][bucket]++;
			}
			LastCycles[thread] = cycles;
			LastBusyQpc[thread] = cycleQpc;
		}
		const double us = (double)(Qpc() - t0) * 1e6 / (double)QpcFreq;
		PauseSumUs += us; PauseCount++; if (us > PauseMaxUs) PauseMaxUs = us;
		if (!got) return;
		// owner: the innermost frame that is not Windows / C runtime; a driver frame then also names its caller
		int module = -1;
		Kind leafKind = KindAt(ctx.Eip, &module);
		DWORD leaf = ctx.Eip;
		Kind ownerKind = leafKind;
		DWORD caller = 0; Kind callerKind = kUnknown;
		const DWORD* w = (const DWORD*)StackCopy;
		for (DWORD i = 0; i < len / 4; i++) {
			if (ownerKind != kSystem && ownerKind != kUnknown && ownerKind != kDriver) break;
			const DWORD v = w[i];
			if (v < 0x10000) continue;
			const Kind k = KindAt(v);
			if (k == kUnknown || !AfterCall(v)) continue;
			if (ownerKind == kSystem || ownerKind == kUnknown) { ownerKind = k; leaf = v; continue; }
			// ownerKind == kDriver: the first non-driver, non-system frame is who called the driver
			if (k == kExe || k == kNVR || k == kPluginDll) { caller = v; callerKind = k; break; }
		}
		Owner o = oOther;
		switch (ownerKind) {
		case kExe: o = oGame; break;
		case kNVR: o = oNVR; break;
		case kPluginDll: o = oPlugin; break;
		case kDriver: o = callerKind == kExe ? oDriverGame : callerKind == kNVR ? oDriverNVR : callerKind == kPluginDll ? oDriverPlugin : oDriverOther; break;
		default: break;
		}
		Counts[thread][bucket][o]++;
		// In the kernel: stopped outside every 32-bit module (the 64-bit syscall layer, wow64cpu.dll, which the 32-bit module list
		// does not show), or in Windows code right after a call instruction (a syscall stub's return point). Hook stubs in allocated
		// memory are outside every module too, but a sample rarely lands in their few instructions.
		if (module < 0 || module == Wow64Cpu || (leafKind == kSystem && AfterCall(ctx.Eip))) KernelCounts[thread][bucket][o]++;
		Taken[thread]++;
		Leaves[((unsigned long long)thread << 56) | ((unsigned long long)bucket << 40) | ((unsigned long long)o << 32) | leaf]++;
		if (caller) Callers[((unsigned long long)thread << 56) | ((unsigned long long)bucket << 40) | ((unsigned long long)o << 32) | caller]++;
	}

	inline void WriteAddress(DWORD v) {
		const int m = ModuleAt(v);
		if (m < 0) fprintf(Out, " ?+%08lX", (unsigned long)v);
		else fprintf(Out, " %s+%lX", ModuleNames[m], (unsigned long)(v - ModuleBase[m]));
	}

	// The finished shot's counts.
	inline void WriteShot(int shot, const char* slug) {
		if (!Out) return;
		const double ms = (double)(Qpc() - ShotStart) * 1000.0 / (double)QpcFreq;
		fprintf(Out, "SHOT %d %s ms %.0f samples main %u ai1 %u ai2 %u pause_us mean %.1f max %.1f\n", shot, slug, ms, Taken[0], Taken[1], Taken[2],
			PauseCount ? PauseSumUs / PauseCount : 0.0, PauseMaxUs);
		for (unsigned t = 0; t < kThreads; t++)
			for (unsigned b = 0; b < kRows; b++)
				for (unsigned o = 0; o < kOwners; o++)
					if (Counts[t][b][o]) fprintf(Out, "B %u %s|%s %u %u\n", t, b < CpuSplit::kBucketCount ? CpuSplit::BucketNames[b] : "(none)", OwnerNames[o], Counts[t][b][o], KernelCounts[t][b][o]);
		for (unsigned t = 0; t < kThreads; t++)
			for (unsigned b = 0; b < kRows; b++)
				if (BusyN[t][b]) fprintf(Out, "W %u %s|busy %.1f %u\n", t, b < CpuSplit::kBucketCount ? CpuSplit::BucketNames[b] : "(none)", BusySum[t][b], BusyN[t][b]);
		auto dump = [](const char* tag, std::unordered_map<unsigned long long, unsigned>& map, size_t keep) {
			std::vector<std::pair<unsigned long long, unsigned>> v(map.begin(), map.end());
			std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
			if (v.size() > keep) v.resize(keep);
			for (const auto& e : v) {
				const unsigned t = (unsigned)(e.first >> 56), b = (unsigned)((e.first >> 40) & 0xFF), o = (unsigned)((e.first >> 32) & 0xFF);
				fprintf(Out, "%s %u %s|%s %u", tag, t, b < CpuSplit::kBucketCount ? CpuSplit::BucketNames[b] : "(none)", OwnerNames[o], e.second);
				WriteAddress((DWORD)e.first);
				fputc('\n', Out);
			}
		};
		dump("L", Leaves, 1500);
		dump("C", Callers, 600);
		fflush(Out);
		memset(Counts, 0, sizeof(Counts));
		memset(KernelCounts, 0, sizeof(KernelCounts));
		memset(BusySum, 0, sizeof(BusySum));
		memset(BusyN, 0, sizeof(BusyN));
		memset(Taken, 0, sizeof(Taken));
		Leaves.clear(); Callers.clear();
		PauseSumUs = PauseMaxUs = 0.0; PauseCount = 0;
	}

	inline int ShotNow();
	inline const char* SlugOf(int shot);

	inline void OpenAIThreads() {
		using namespace CpuSplit;
		for (unsigned t = 1; t < kThreads; t++) {
			if (Handles[t]) continue;
			const DWORD id = t == 1 ? (AIThreadIds[kAI1] ? AIThreadIds[kAI1] : AIThreadIds[kAICombined]) : AIThreadIds[kAI2];
			if (!id) continue;
			HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, id);
			if (!h) continue;
			// the thread's stack range from its TEB (NtTib.StackBase, DeallocationStack)
			typedef LONG (NTAPI* QueryFn)(HANDLE, int, PVOID, ULONG, PULONG);
			static QueryFn query = (QueryFn)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryInformationThread");
			ThreadBasicInformation info = {};
			if (!query || query(h, 0, &info, sizeof(info), NULL) < 0 || !info.Teb) { CloseHandle(h); continue; }
			__try { StackBase[t] = *(DWORD*)((UInt8*)info.Teb + 0x04); StackBottom[t] = *(DWORD*)((UInt8*)info.Teb + 0xE0C); }
			__except (EXCEPTION_EXECUTE_HANDLER) { CloseHandle(h); continue; }
			Ids[t] = id; Handles[t] = h;
			if (Out) { fprintf(Out, "I AI thread %u = %lu, stack %08lX..%08lX\n", t, (unsigned long)id, (unsigned long)StackBottom[t], (unsigned long)StackBase[t]); fflush(Out); }
		}
	}

	inline DWORD WINAPI ThreadMain(void*) {
		SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
		HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002 /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION */, TIMER_ALL_ACCESS);
		const LONGLONG period = Hz > 0 ? -10000000LL / Hz : -10000;
		ScanModules();
		LONGLONG lastScan = Qpc();
		for (;;) {
			if (timer) {
				LARGE_INTEGER due; due.QuadPart = period;
				if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) WaitForSingleObject(timer, 50);
				else Sleep(1);
			}
			else Sleep(1);
			const int shot = ShotNow();
			if (shot != CurrentShot) {
				if (CurrentShot >= 0) WriteShot(CurrentShot, SlugOf(CurrentShot));
				CurrentShot = shot;
				ShotStart = Qpc();
				OpenAIThreads();
			}
			if (shot < 0) continue;
			const LONGLONG now = Qpc();
			if (now - lastScan > 10 * QpcFreq) { ScanModules(); lastScan = now; }
			for (unsigned t = 0; t < kThreads; t++) TakeSample(t);
		}
		return 0;
	}

	// Main thread (SettingShots, when its run starts).
	inline void Start() {
		if (Started || Hz <= 0) return;
		Started = true;
		LARGE_INTEGER f; QueryPerformanceFrequency(&f); QpcFreq = f.QuadPart;
		CalQpc = Qpc(); CalTsc = __rdtsc();
		Ids[0] = GetCurrentThreadId();
		StackBase[0] = __readfsdword(0x04);
		StackBottom[0] = __readfsdword(0xE0C);
		Handles[0] = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, Ids[0]);
		StackCopy = (unsigned char*)VirtualAlloc(nullptr, kStackCopyMax, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
		SYSTEMTIME t; GetLocalTime(&t);
		char path[MAX_PATH];
		sprintf_s(path, ".\\" VS_LOGS_FOLDER "\\CpuSamples_%04u-%02u-%02u_%02u-%02u-%02u.txt", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
		Out = _fsopen(path, "w", _SH_DENYWR);
		if (!Handles[0] || !StackCopy || !Out) { Logger::Log("UNOFFICIAL CpuSampler (test build): could not start; off"); Hz = 0; return; }
		setvbuf(Out, nullptr, _IOFBF, 1 << 18);
		fprintf(Out, "I CpuSampler 1 | %d Hz | main thread %lu | qpc %lld per s\n", Hz, (unsigned long)Ids[0], QpcFreq);
		HANDLE th = CreateThread(nullptr, 256 * 1024, &ThreadMain, nullptr, 0, nullptr);
		if (!th) { Logger::Log("UNOFFICIAL CpuSampler (test build): no thread; off"); Hz = 0; return; }
		CloseHandle(th);
		Logger::Log("UNOFFICIAL CpuSampler (test build): sampling the main and AI threads at %d Hz -> %s", Hz, path);
	}
#else
	inline int Hz = 0;
	inline void Start() {}
#endif
}
