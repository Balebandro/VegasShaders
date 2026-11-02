#pragma once

// UNOFFICIAL ArrivalSampler (TEST BUILDS ONLY, 2026-10-20, side session arrivalhitch; evidence\arrivalhitch-LOG.md).
// What is the main thread doing in the frames over 50 ms after arriving somewhere (the time outside draw calls that the shader
// preloader could not remove)? A sampling profiler, so nothing in the game is hooked or patched (FNVProbe and the other plugins
// keep their call-site hooks): a thread of ours wakes every ~1 ms (a high-resolution waitable timer; the system timer
// resolution is not changed), suspends the main (render) thread, reads its registers and copies its live stack (ESP up to the
// stack base, at most 128 KB), and resumes it. Only then, with the main thread running again, the copy is searched for return
// addresses (a value inside a loaded module's executable section, right after a call instruction): innermost 256 and outermost
// 128 are kept with the instruction pointer (all of them in nearly every sample). While the main thread is suspended this thread takes no lock and allocates nothing
// (a lock the main thread holds would deadlock both).
// The main thread's frame tick (ShaderWarmUp2::FrameTick, the 'WARMUP2 FRAME' point) hands every frame's interval and flags
// over a lock-free queue; this thread writes the samples of frames over 50 ms (not loading screens, not the main menu) and of
// every 200th ordinary frame (a reference) to VegasShaders-Logs\ArrivalSamples_<time>.txt, flushed per frame
// (the auto-test ends the game with TerminateProcess). Read by evidence\arrivalhitch\analyze_samples.py.
// 2026-10-20 ioworker (evidence\ioworker-LOG.md): in the same tick, right after the main thread and one at a time (never two
// threads suspended), the game's LOADING threads are sampled the same way: the IOManager worker(s) (BSTaskManagerThread, found
// through the IOManager singleton at 0x01202D98: count at +0x4C, thread array at +0x50, thread ID at +0x08 of each worker) and
// Obsidian's 'Serialized IO Thread' (OEI::IO::IO_Thread at 0x011F6BF0, thread ID at +0x24 of its BSThread), which does every
// archive / loose-file fread and fseek for every thread. With each one go 8 raw stack values at ESP (the arguments of a wait),
// the IOManager's queue counters and the thread's own semaphore counts (plain reads, racy, read after the resume), and for a
// worker inside a task the task's vtable (the argument above the return address of ThreadUpdate's RunTask / FinishTask call).
// The process's threads (start address, priority) are listed once found and again when new ones appear. Nothing is patched.
// Off unless [Main.Main.Performance] ArrivalSampler = 1, and compiled out of release trees (NVR_TEST_TOOLS).
#include <tlhelp32.h>
#include <intrin.h>
#include <share.h>
#include <atomic>
#include "TestTools.h"

namespace ArrivalSampler {
#if NVR_TEST_TOOLS
	static const unsigned kKeepInner = 256, kKeepOuter = 128, kKeep = kKeepInner + kKeepOuter;	// 2026-10-20 L1: 40 + 16 lost the OnIdle frames (~105 return addresses per stack, the OnIdle call 16-60 from the outer end)
	struct Sample { LONGLONG Qpc; DWORD Eip, Esp; DWORD PauseTicks; unsigned short Kept, Found; DWORD Ra[kKeep]; };
	struct FrameInfo { LONGLONG Start, End; float Ms, DrawMs, NewMs, SlowKnownMs, SinceLoad; int Shot; unsigned Index; char Kind; char Flags[6]; };
	struct Range { DWORD Start, End; int Module; };

	inline bool Enabled = false, Started = false;
	inline DWORD MainId = 0, StackBase = 0, StackBottom = 0;
	inline HANDLE MainHandle = nullptr;
	inline LONGLONG QpcFreq = 1, LastFrameQpc = 0;
	inline unsigned FrameIndex = 0;

	// main thread -> sampler: frames (single producer, single consumer)
	static const unsigned kFrameRing = 1024;
	inline FrameInfo Frames[kFrameRing];
	inline std::atomic<unsigned> FramesWritten{ 0 };
	inline volatile unsigned FramesRead = 0;	// written by the sampler only; the main thread reads it to see if the ring is full

	// sampler-thread state only
	static const unsigned kSampleRing = 8192;	// ~16 s at one sample per 2 ms
	inline Sample* Samples = nullptr;
	inline unsigned SampleW = 0, SampleR = 0;	// SampleW - SampleR = samples held (oldest dropped when full)
	inline unsigned char* StackCopy = nullptr;
	static const DWORD kStackCopyMax = 128 * 1024;
	inline Range Ranges[1024];
	inline int RangeCount = 0;
	inline char ModuleNames[256][64];
	inline DWORD ModuleBase[256], ModuleSize[256];
	inline int ModuleCount = 0;
	inline FILE* Out = nullptr;
	inline unsigned long long WrittenSamples = 0, TakenSamples = 0, SkippedStacks = 0, DroppedSamples = 0;
	inline double PauseSumUs = 0.0, PauseMaxUs = 0.0;
	inline LONGLONG LastModuleScan = 0, LastTotals = 0;
	inline bool UnknownEip = false;
	static const unsigned long long kMaxWritten = 60000;	// ~60 MB at most per launch

	// 2026-10-20 ioworker: the game's loading threads ('aux' threads), sampled in the same tick after the main thread.
	struct AuxThread { DWORD Tid, StackBase, StackBottom, Obj; HANDLE Handle; char Role; int Index; };
	static const unsigned kAuxMax = 4, kAuxKeepInner = 96, kAuxKeepOuter = 32, kAuxKeep = kAuxKeepInner + kAuxKeepOuter, kAuxRaw = 8, kAuxState = 8;
	struct AuxSample { LONGLONG Qpc; DWORD Eip, Esp, PauseTicks, TaskVt; unsigned short Kept, Found; unsigned Slot; DWORD State[kAuxState]; DWORD Raw[kAuxRaw]; DWORD Ra[kAuxKeep]; };
	static const unsigned kAuxRing = 16384;		// ~16 s at two loading threads per tick
	static const DWORD kAuxStackCopyMax = 64 * 1024;
	// Game addresses (FalloutNV.exe 1.4.0.525, checked in the dumped exe; the IOManager and thread layouts were first published by
	// acidpointer's psycho, re-checked in BSTaskManager::BSTaskManager 0x00C3E4F0 / BSTaskThread::BSTaskThread 0x00C42DD0 / BSThread::Initialize 0x00AA6430).
	static const DWORD kIOManagerPtr = 0x01202D98, kOeiThreadPtr = 0x011F6BF0, kOeiEventSema = 0x011F6BF4;
	static const DWORD kTaskThreadProc = 0x00C42DA0, kBSThreadProc = 0x00AA64D0, kThreadUpdate = 0x00C410B0;
	static const DWORD kRetRunTask = 0x00C41257, kRetFinishTask = 0x00C41274;	// BSTaskManagerThread::ThreadUpdate: 'call eax' (owner vtable +0x4C / +0x50), task pointer pushed just before
	typedef LONG (WINAPI* NtQueryInformationThreadFn)(HANDLE, int, void*, ULONG, ULONG*);
	inline NtQueryInformationThreadFn NtQIT = nullptr;
	inline AuxThread Aux[kAuxMax];
	inline unsigned AuxCount = 0;
	inline bool AuxDone = false, TaskVtOk = false;
	inline DWORD IoManager = 0;
	inline AuxSample* AuxSamples = nullptr;
	inline unsigned AuxW = 0, AuxR = 0;
	inline unsigned long long AuxTaken = 0, AuxDropped = 0, AuxSkipped = 0;
	inline double AuxPauseSumUs = 0.0, AuxPauseMaxUs = 0.0;
	inline LONGLONG LastAuxTry = 0;
	inline DWORD KnownTids[512];
	inline unsigned KnownTidCount = 0;

	inline LONGLONG Qpc() { LARGE_INTEGER c; QueryPerformanceCounter(&c); return c.QuadPart; }
	inline double TicksToMs(LONGLONG t) { return (double)t * 1000.0 / (double)QpcFreq; }

	// Sampler thread, main thread RUNNING: the loaded modules and their executable sections.
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

	inline bool InCode(DWORD v) {
		int lo = 0, hi = RangeCount - 1;
		while (lo <= hi) {
			const int mid = (lo + hi) >> 1;
			if (v < Ranges[mid].Start) hi = mid - 1;
			else if (v >= Ranges[mid].End) lo = mid + 1;
			else return true;
		}
		return false;
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

	// ---- 2026-10-20 ioworker: the loading threads (sampler thread; plain reads of game memory under __try) ----
	inline DWORD Peek(DWORD a) { __try { return a > 0x10000 ? *(const DWORD*)(uintptr_t)a : 0; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; } }
	inline bool Bytes(DWORD a, const unsigned char* b, unsigned n) { __try { return !memcmp((const void*)(uintptr_t)a, b, n); } __except (EXCEPTION_EXECUTE_HANDLER) { return false; } }

	inline void Where(DWORD a, char* out, size_t n) {
		for (int i = 0; i < ModuleCount; i++)
			if (a >= ModuleBase[i] && a < ModuleBase[i] + ModuleSize[i]) { sprintf_s(out, n, "%s+%lX", ModuleNames[i], (unsigned long)(a - ModuleBase[i])); return; }
		sprintf_s(out, n, "?");
	}

	// A thread's start address (ThreadQuerySetWin32StartAddress) and its stack (from its 32-bit TEB: StackBase, DeallocationStack).
	inline DWORD StartOf(HANDLE h) { DWORD a = 0; if (NtQIT && NtQIT(h, 9, &a, sizeof(a), nullptr) < 0) a = 0; return a; }
	inline bool StackOf(HANDLE h, DWORD& base, DWORD& bottom) {
		struct { LONG ExitStatus; DWORD Teb, Pid, Tid, Affinity; LONG Priority, BasePriority; } tbi = {};	// THREAD_BASIC_INFORMATION (32-bit)
		if (!NtQIT || NtQIT(h, 0, &tbi, sizeof(tbi), nullptr) < 0 || !tbi.Teb) return false;
		DWORD teb = tbi.Teb;
		if (Peek(teb + 0x18) != teb && Peek(teb + 0x2000 + 0x18) == teb + 0x2000) teb += 0x2000;	// a 64-bit TEB: the 32-bit one follows it
		if (Peek(teb + 0x18) != teb) return false;	// NtTib.Self
		base = Peek(teb + 0x04); bottom = Peek(teb + 0xE0C);
		return bottom && base > bottom;
	}

	// Every thread of the process with its start address and priority; only the ones not listed before.
	inline void ListThreads() {
		HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
		if (snap == INVALID_HANDLE_VALUE || !Out) return;
		THREADENTRY32 te = {}; te.dwSize = sizeof(te);
		const DWORD pid = GetCurrentProcessId(), self = GetCurrentThreadId();
		for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
			if (te.th32OwnerProcessID != pid) continue;
			bool known = false;
			for (unsigned i = 0; i < KnownTidCount; i++) if (KnownTids[i] == te.th32ThreadID) { known = true; break; }
			if (known) continue;
			if (KnownTidCount < 512) KnownTids[KnownTidCount++] = te.th32ThreadID;
			HANDLE h = OpenThread(THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
			const DWORD start = h ? StartOf(h) : 0; const int prio = h ? GetThreadPriority(h) : 0;
			if (h) CloseHandle(h);
			char where[96]; Where(start, where, sizeof(where));
			const char* role = te.th32ThreadID == MainId ? "main" : te.th32ThreadID == self ? "sampler" : "-";
			for (unsigned k = 0; k < AuxCount; k++) if (Aux[k].Tid == te.th32ThreadID) role = Aux[k].Role == 'W' ? "IOManager-worker" : "Serialized-IO-Thread";
			fprintf(Out, "TH %lu start %08lX %s prio %d %s\n", (unsigned long)te.th32ThreadID, (unsigned long)start, where, prio, role);
		}
		CloseHandle(snap);
		fflush(Out);
	}

	inline void AddAux(char role, int index, DWORD tid, DWORD obj, DWORD expectStart) {
		if (AuxCount >= kAuxMax || !tid || !Out) return;
		for (unsigned k = 0; k < AuxCount; k++) if (Aux[k].Tid == tid) return;
		HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
		DWORD base = 0, bottom = 0;
		if (!h || GetProcessIdOfThread(h) != GetCurrentProcessId() || !StackOf(h, base, bottom)) {
			fprintf(Out, "I %c%d thread %lu NOT sampled (handle %p, error %lu)\n", role, index, (unsigned long)tid, h, GetLastError());
			if (h) CloseHandle(h);
			return;
		}
		const DWORD start = StartOf(h);
		char where[96]; Where(start, where, sizeof(where));
		Aux[AuxCount] = { tid, base, bottom, obj, h, role, index };
		fprintf(Out, "X %u %c%d tid %lu start %08lX %s (%s) stack %08lX..%08lX obj %08lX vtable %08lX prio %d\n", AuxCount, role, index, (unsigned long)tid,
			(unsigned long)start, where, start == expectStart ? "as expected" : "UNEXPECTED", (unsigned long)bottom, (unsigned long)base, (unsigned long)obj,
			(unsigned long)Peek(obj), GetThreadPriority(h));
		AuxCount++;
	}

	// Find the IOManager worker(s) and the Serialized IO Thread (again every second until both are there).
	inline void FindAux() {
		static const unsigned char kThreadUpdateHead[] = { 0x55, 0x8B, 0xEC, 0x6A, 0xFF, 0x68, 0xA0, 0xE0, 0xF2, 0x00 };
		static const unsigned char kCallEax[] = { 0xFF, 0xD0 };
		if (!Out || !AuxSamples) { AuxDone = true; return; }
		const DWORD io = Peek(kIOManagerPtr), oei = Peek(kOeiThreadPtr);
		if (io && !IoManager) {
			IoManager = io;
			const bool head = Bytes(kThreadUpdate, kThreadUpdateHead, sizeof(kThreadUpdateHead));
			TaskVtOk = head && Bytes(kRetRunTask - 2, kCallEax, 2) && Bytes(kRetFinishTask - 2, kCallEax, 2);
			const DWORD count = Peek(io + 0x4C), arr = Peek(io + 0x50), push = Peek(0x00C3DA7A);
			fprintf(Out, "I IOManager %08lX: workers %lu (array %08lX), tasks in map %lu, bucket total %lu, loading priority %lu, post-process queue %lu | "
				"worker-count push at 00C3DA7A: %02lX %02lX (6A 01 = vanilla, one worker; 6A 02 = two) | ThreadUpdate bytes %s, task vtables %s\n",
				(unsigned long)io, (unsigned long)count, (unsigned long)arr, (unsigned long)Peek(io + 0x18), (unsigned long)Peek(io + 0x58), (unsigned long)Peek(io + 0x68),
				(unsigned long)Peek(Peek(io + 0x64) + 0x0C), (unsigned long)(push & 0xFF), (unsigned long)((push >> 8) & 0xFF), head ? "as expected" : "DIFFERENT", TaskVtOk ? "recorded" : "not recorded");
			if (count >= 1 && count <= 8 && arr)
				for (DWORD k = 0; k < count && k < 2; k++) {
					const DWORD t = Peek(arr + 4 * k);
					if (!t) continue;
					if (Peek(t + 0x30) != io) fprintf(Out, "I worker %lu: owner %08lX is not the IOManager\n", (unsigned long)k, (unsigned long)Peek(t + 0x30));
					AddAux('W', (int)k, Peek(t + 0x08), t, kTaskThreadProc);
				}
			else fprintf(Out, "I IOManager layout not as expected: workers not sampled\n");
		}
		static bool oeiDone = false;
		if (oei && !oeiDone && Peek(oei + 0x24)) { oeiDone = true; AddAux('O', 0, Peek(oei + 0x24), oei, kBSThreadProc); }
		AuxDone = IoManager && oeiDone;
		if (AuxDone) fprintf(Out, "I loading threads sampled: %u\n", AuxCount);
		fflush(Out);
	}

	// One loading thread, the same way as the main thread (suspend, context, stack copy, resume; nothing else while suspended).
	inline void TakeAux(unsigned k) {
		const AuxThread& a = Aux[k];
		AuxSample& s = AuxSamples[AuxW % kAuxRing];
		CONTEXT ctx = {}; ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
		DWORD len = 0;
		const LONGLONG t0 = Qpc();
		if (SuspendThread(a.Handle) == (DWORD)-1) return;
		// ---- this thread suspended: no lock, no allocation, no logging until ResumeThread ----
		const BOOL got = GetThreadContext(a.Handle, &ctx);
		if (got && ctx.Esp >= a.StackBottom && ctx.Esp < a.StackBase) {
			len = min(a.StackBase - ctx.Esp, kAuxStackCopyMax);
			memcpy(StackCopy, (const void*)(uintptr_t)ctx.Esp, len);
		}
		ResumeThread(a.Handle);
		// ---- running ----
		const LONGLONG t1 = Qpc();
		if (!got) return;
		if (!len) AuxSkipped++;
		s.Qpc = t0; s.Slot = k; s.Eip = ctx.Eip; s.Esp = ctx.Esp; s.PauseTicks = (DWORD)(t1 - t0); s.TaskVt = 0;
		const DWORD* w = (const DWORD*)StackCopy;
		const DWORD nw = len / 4;
		for (unsigned i = 0; i < kAuxRaw; i++) s.Raw[i] = i < nw ? w[i] : 0;
		DWORD found[1024]; unsigned n = 0;
		for (DWORD i = 0; i < nw && n < 1024; i++) {
			const DWORD v = w[i];
			if (v > 0x10000 && InCode(v) && AfterCall(v)) {
				found[n++] = v;
				// the live RunTask / FinishTask call is the outermost one (a stale copy can only sit deeper in the stack)
				if (TaskVtOk && a.Role == 'W' && (v == kRetRunTask || v == kRetFinishTask) && i + 1 < nw) s.TaskVt = Peek(w[i + 1]);
			}
		}
		s.Found = (unsigned short)n;
		if (n <= kAuxKeep) { memcpy(s.Ra, found, n * 4); s.Kept = (unsigned short)n; }
		else { memcpy(s.Ra, found, kAuxKeepInner * 4); memcpy(s.Ra + kAuxKeepInner, found + n - kAuxKeepOuter, kAuxKeepOuter * 4); s.Kept = kAuxKeep; }
		if (!InCode(ctx.Eip)) UnknownEip = true;
		// the queue state (plain reads, may be torn: the threads are running)
		const DWORD io = IoManager;
		s.State[0] = io ? Peek(io + 0x18) : 0;					// tasks in the IOManager's map (queued, running, or done and not yet finished by the main thread)
		s.State[1] = io ? Peek(io + 0x58) : 0;					// bucket count total
		s.State[2] = io ? Peek(io + 0x68) : 0;					// the priority the main thread waits for in LoadQueuedPriority (6 = none)
		s.State[3] = io ? Peek(io + 0x44) : 0;					// workers paused
		s.State[4] = io ? Peek(io + 0x48) : 0;					// pause requests
		s.State[5] = io ? Peek(Peek(io + 0x64) + 0x0C) : 0;	// done tasks waiting for the main thread (post-process queue)
		if (a.Role == 'W') { s.State[6] = Peek(a.Obj + 0x0C); s.State[7] = Peek(a.Obj + 0x18); }	// its task-semaphore count; its pause-semaphore count (1 = not paused)
		else { s.State[6] = Peek(Peek(kOeiEventSema)); s.State[7] = 0; }	// read / seek requests waiting for the Serialized IO Thread
		AuxW++;
		if (AuxW - AuxR > kAuxRing) { AuxR = AuxW - kAuxRing; AuxDropped++; }
		AuxTaken++;
		const double us = TicksToMs(t1 - t0) * 1000.0;
		AuxPauseSumUs += us; if (us > AuxPauseMaxUs) AuxPauseMaxUs = us;
	}

	inline void TakeSample() {
		Sample& s = Samples[SampleW % kSampleRing];
		CONTEXT ctx = {}; ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
		DWORD len = 0;
		const LONGLONG t0 = Qpc();
		if (SuspendThread(MainHandle) == (DWORD)-1) return;
		// ---- main thread suspended: no lock, no allocation, no logging until ResumeThread ----
		const BOOL got = GetThreadContext(MainHandle, &ctx);
		if (got && ctx.Esp >= StackBottom && ctx.Esp < StackBase) {	// the live stack (ESP .. base) is always committed
			len = min(StackBase - ctx.Esp, kStackCopyMax);
			memcpy(StackCopy, (const void*)(uintptr_t)ctx.Esp, len);
		}
		ResumeThread(MainHandle);
		// ---- main thread running ----
		const LONGLONG t1 = Qpc();
		if (!got) return;
		if (!len) SkippedStacks++;
		s.Qpc = t0; s.Eip = ctx.Eip; s.Esp = ctx.Esp; s.PauseTicks = (DWORD)(t1 - t0);
		DWORD found[4096]; unsigned n = 0;
		const DWORD* w = (const DWORD*)StackCopy;
		for (DWORD i = 0; i < len / 4 && n < 4096; i++) {
			const DWORD v = w[i];
			if (v > 0x10000 && InCode(v) && AfterCall(v)) found[n++] = v;
		}
		s.Found = (unsigned short)n;
		if (n <= kKeep) { memcpy(s.Ra, found, n * 4); s.Kept = (unsigned short)n; }
		else { memcpy(s.Ra, found, kKeepInner * 4); memcpy(s.Ra + kKeepInner, found + n - kKeepOuter, kKeepOuter * 4); s.Kept = kKeep; }
		if (!InCode(ctx.Eip)) UnknownEip = true;
		SampleW++;
		if (SampleW - SampleR > kSampleRing) { SampleR = SampleW - kSampleRing; DroppedSamples++; }
		TakenSamples++;
		const double us = TicksToMs(t1 - t0) * 1000.0;
		PauseSumUs += us; if (us > PauseMaxUs) PauseMaxUs = us;
	}

	// Sampler thread: the frames the main thread finished; write the selected ones, drop the others' samples.
	inline void DrainFrames() {
		const unsigned written = FramesWritten.load(std::memory_order_acquire);
		while (FramesRead != written) {
			const FrameInfo& f = Frames[FramesRead % kFrameRing];
			while (SampleR != SampleW && Samples[SampleR % kSampleRing].Qpc < f.Start) SampleR++;
			unsigned first = SampleR, last = SampleR;
			while (last != SampleW && Samples[last % kSampleRing].Qpc < f.End) last++;
			while (AuxR != AuxW && AuxSamples[AuxR % kAuxRing].Qpc < f.Start) AuxR++;
			unsigned auxLast = AuxR;
			while (auxLast != AuxW && AuxSamples[auxLast % kAuxRing].Qpc < f.End) auxLast++;
			if (f.Kind && Out && WrittenSamples < kMaxWritten) {
				double pause = 0.0, pauseMax = 0.0;
				for (unsigned i = first; i != last; i++) { const double us = TicksToMs(Samples[i % kSampleRing].PauseTicks) * 1000.0; pause += us; if (us > pauseMax) pauseMax = us; }
				const unsigned count = last - first;
				fprintf(Out, "F %u %c %.2f draws %.2f new %.2f slowknown %.2f since %.2f shot %d flags %s samples %u pause_us %.1f max %.1f aux %u\n",
					f.Index, f.Kind, f.Ms, f.DrawMs, f.NewMs, f.SlowKnownMs, f.SinceLoad, f.Shot, f.Flags[0] ? f.Flags : "-", count, count ? pause / count : 0.0, pauseMax, auxLast - AuxR);
				for (unsigned i = first; i != last; i++) {
					const Sample& s = Samples[i % kSampleRing];
					fprintf(Out, "S %.3f %08lX %u", TicksToMs(s.Qpc - f.Start), (unsigned long)s.Eip, (unsigned)s.Found);
					for (unsigned k = 0; k < s.Kept; k++) fprintf(Out, " %08lX", (unsigned long)s.Ra[k]);
					fputc('\n', Out);
				}
				// A <slot> <ms into the frame> <eip> <return addresses found> <task vtable> <8 queue-state values> <8 raw stack values> | <return addresses>
				for (unsigned i = AuxR; i != auxLast; i++) {
					const AuxSample& s = AuxSamples[i % kAuxRing];
					fprintf(Out, "A %u %.3f %08lX %u %08lX", s.Slot, TicksToMs(s.Qpc - f.Start), (unsigned long)s.Eip, (unsigned)s.Found, (unsigned long)s.TaskVt);
					for (unsigned k = 0; k < kAuxState; k++) fprintf(Out, " %lX", (unsigned long)s.State[k]);
					for (unsigned k = 0; k < kAuxRaw; k++) fprintf(Out, " %08lX", (unsigned long)s.Raw[k]);
					fputs(" |", Out);
					for (unsigned k = 0; k < s.Kept; k++) fprintf(Out, " %08lX", (unsigned long)s.Ra[k]);
					fputc('\n', Out);
				}
				WrittenSamples += count;
				fflush(Out);
			}
			SampleR = last;
			AuxR = auxLast;
			FramesRead++;
		}
	}

	inline DWORD WINAPI ThreadMain(void*) {
		SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
		HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002 /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION */, TIMER_ALL_ACCESS);
		if (Out) fprintf(Out, "I timer %s\n", timer ? "high-resolution waitable timer, 1 ms" : "NONE (Sleep(1) at the system timer resolution)");
		ScanModules();
		LastModuleScan = LastTotals = Qpc();
		for (;;) {
			if (timer) {
				LARGE_INTEGER due; due.QuadPart = -10000;	// 1 ms, relative
				if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) WaitForSingleObject(timer, 50);
				else Sleep(1);
			}
			else Sleep(1);
			DrainFrames();
			const LONGLONG now = Qpc();
			if (UnknownEip ? now - LastModuleScan > QpcFreq / 2 : now - LastModuleScan > 10 * QpcFreq) { ScanModules(); LastModuleScan = now; UnknownEip = false; }
			if (!AuxDone && now - LastAuxTry > QpcFreq) { FindAux(); LastAuxTry = now; if (AuxDone) ListThreads(); }
			if (now - LastTotals > 10 * QpcFreq && Out) {
				fprintf(Out, "T samples %llu, written %llu, no stack %llu, ring overflows %llu, main-thread pause mean %.1f us max %.1f us | loading threads %u: samples %llu, no stack %llu, ring overflows %llu, pause mean %.1f us max %.1f us\n",
					TakenSamples, WrittenSamples, SkippedStacks, DroppedSamples, TakenSamples ? PauseSumUs / TakenSamples : 0.0, PauseMaxUs,
					AuxCount, AuxTaken, AuxSkipped, AuxDropped, AuxTaken ? AuxPauseSumUs / AuxTaken : 0.0, AuxPauseMaxUs);
				fflush(Out);
				ListThreads();	// threads started since the last list
				LastTotals = now;
			}
			TakeSample();
			for (unsigned k = 0; k < AuxCount; k++) TakeAux(k);
		}
		return 0;
	}

	// Main (render) thread, from ShaderWarmUp2's frame tick.
	inline void Start() {
		if (Started) return;
		Started = true;
		SettingManager::Configuration::ConfigNode node;
		Enabled = TheSettingManager->Config.FillNode(&node, "Main.Main.Performance", "ArrivalSampler") && node.IntValue > 0;
		if (!Enabled) return;
		LARGE_INTEGER f; QueryPerformanceFrequency(&f); QpcFreq = f.QuadPart;
		MainId = GetCurrentThreadId();
		StackBase = __readfsdword(0x04);	// TEB NtTib.StackBase
		StackBottom = __readfsdword(0xE0C);	// TEB DeallocationStack (lowest reserved address of this stack)
		MainHandle = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, MainId);
		Samples = (Sample*)VirtualAlloc(nullptr, sizeof(Sample) * kSampleRing, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
		StackCopy = (unsigned char*)VirtualAlloc(nullptr, kStackCopyMax, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
		AuxSamples = (AuxSample*)VirtualAlloc(nullptr, sizeof(AuxSample) * kAuxRing, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);	// null = the loading threads are not sampled
		NtQIT = (NtQueryInformationThreadFn)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryInformationThread");
		SYSTEMTIME t; GetLocalTime(&t);
		char path[MAX_PATH];
		sprintf_s(path, ".\\" VS_LOGS_FOLDER "\\ArrivalSamples_%04u-%02u-%02u_%02u-%02u-%02u.txt", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
		Out = _fsopen(path, "w", _SH_DENYWR);
		if (!MainHandle || !Samples || !StackCopy || !Out) {
			Logger::Log("UNOFFICIAL ArrivalSampler (test build): could not start (thread handle %p, buffers %p %p, file %p '%s'); off", MainHandle, Samples, StackCopy, Out, path);
			Enabled = false; return;
		}
		setvbuf(Out, nullptr, _IOFBF, 1 << 18);
		fprintf(Out, "I ArrivalSampler 2 (+ loading threads%s) | main thread %lu, stack %08lX..%08lX | qpc %lld per s | frames over 50 ms and every 200th frame | started %02u:%02u:%02u.%03u\n",
			AuxSamples && NtQIT ? "" : ": NOT AVAILABLE", (unsigned long)MainId, (unsigned long)StackBottom, (unsigned long)StackBase, QpcFreq, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
		if (!NtQIT) AuxSamples = nullptr;
		HANDLE th = CreateThread(nullptr, 64 * 1024, &ThreadMain, nullptr, 0, nullptr);
		if (!th) { Logger::Log("UNOFFICIAL ArrivalSampler (test build): no thread; off"); Enabled = false; return; }
		CloseHandle(th);
		LastFrameQpc = Qpc();
		Logger::Log("UNOFFICIAL ArrivalSampler (test build): sampling the main thread every ~1 ms; frames over 50 ms -> %s", path);
	}

	// The first world frame's tick (ShaderWarmUp2::FrameTick): the first frame starts here. Start() normally ran earlier, at the
	// device hooks (ShaderWarmUp2::Install), so the long first frame after the save load is sampled too.
	inline void FirstTick() {
		if (!Started) Start();
		if (!Enabled) return;
		if (GetCurrentThreadId() != MainId) Logger::Log("UNOFFICIAL ArrivalSampler (test build): WARNING the frame tick runs on thread %lu, the sampled thread is %lu", (unsigned long)GetCurrentThreadId(), (unsigned long)MainId);
		LastFrameQpc = Qpc();
	}

	inline void OnFrame(double ms, bool loading, bool menu, bool picture, bool loadMenu, int shot, double sinceLoad, double drawMs, double newMs, double slowKnownMs) {
		if (!Started) Start();
		if (!Enabled) return;
		const LONGLONG now = Qpc();
		FrameIndex++;
		FrameInfo f = {};
		f.Start = LastFrameQpc; f.End = now; f.Index = FrameIndex;
		f.Ms = (float)ms; f.DrawMs = (float)drawMs; f.NewMs = (float)newMs; f.SlowKnownMs = (float)slowKnownMs; f.SinceLoad = (float)sinceLoad; f.Shot = shot;
		f.Kind = (!loading && !menu) ? (ms > 50.0 ? 'H' : (FrameIndex % 200 == 0 ? 'R' : 0)) : 0;
		int k = 0;
		if (picture) f.Flags[k++] = 'p';
		if (loadMenu) f.Flags[k++] = 'l';
		f.Flags[k] = 0;
		LastFrameQpc = now;
		const unsigned w = FramesWritten.load(std::memory_order_relaxed);
		if (w - FramesRead >= kFrameRing) return;	// the sampler is behind (writing a big frame): this frame is not written
		Frames[w % kFrameRing] = f;
		FramesWritten.store(w + 1, std::memory_order_release);
	}
#else
	inline void Start() {}
	inline void FirstTick() {}
	inline void OnFrame(double, bool, bool, bool, bool, int, double, double, double, double) {}
#endif
}
