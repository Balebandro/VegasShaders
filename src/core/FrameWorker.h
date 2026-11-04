#pragma once

// UNOFFICIAL 2026-11-03 FrameWorker (cpu-workers; bench\deep-roadmap\cpuworkers\REPORT.md). On a slower CPU the frame waits for the
// game's main thread alone (CPU-LIMITS.md), and our own per-frame work sits on it. Draw calls and anything that reads the game's scene
// graph must stay there; pure arithmetic on values the main thread has already copied out can run beside it. This is the one thread
// that does that: the main thread hands it a job (a function and the job's own copy of its inputs), carries on with the frame, and
// waits for the job only where a result is first needed (Wait). One job at a time; Start waits for the previous one first.
// A job may split independent items over the worker and up to two helper threads (ParallelFor; each item writes only its own result,
// so the job decides the order anything is summed in, and the result does not depend on which thread did what).
// The job runs with the main thread's floating-point control words (x87 precision and rounding, SSE rounding and denormal modes,
// copied at Start): Direct3D 9 sets the x87 precision on the device thread, and the same instructions must round the same way here
// for the results to be bit for bit those of the main thread.
#include <windows.h>
#include <float.h>

namespace FrameWorker {
	typedef void (*JobFn)(void* arg);

	inline HANDLE Thread = nullptr, Go = nullptr, Done = nullptr;
	inline JobFn PendingFn = nullptr;
	inline void* PendingArg = nullptr;
	inline unsigned int ControlX87 = 0, ControlSse = 0;
	inline volatile LONG Busy = 0;		// 1 from Start until the main thread has seen the job finish (Wait)
	inline bool Failed = false;			// the thread could not be made: Start runs jobs in place

	// ParallelFor's helpers: thread t (1..HelperCount) takes items t, t + stride, ... (the worker takes 0, stride, ...).
	typedef void (*ItemFn)(void* arg, int item);
	static const int kMaxHelpers = 2;
	inline int HelperCount = 0;
	inline HANDLE HelperGo[kMaxHelpers] = {}, HelperDone[kMaxHelpers] = {};
	inline ItemFn ForFn = nullptr;
	inline void* ForArg = nullptr;
	inline int ForCount = 0, ForStride = 1;

	inline void UseControlWords() {
		unsigned int ignored;
		__control87_2(ControlX87, _MCW_EM | _MCW_RC | _MCW_PC | _MCW_IC | _MCW_DN, &ignored, nullptr);
		__control87_2(ControlSse, _MCW_EM | _MCW_RC | _MCW_DN, nullptr, &ignored);
	}

	inline DWORD WINAPI HelperLoop(void* index) {
		const int t = (int)(intptr_t)index;
		for (;;) {
			if (WaitForSingleObject(HelperGo[t - 1], INFINITE) != WAIT_OBJECT_0) return 0;
			UseControlWords();
			for (int i = t; i < ForCount; i += ForStride) ForFn(ForArg, i);
			SetEvent(HelperDone[t - 1]);
		}
	}

	// From inside a job: fn(arg, i) for every i in [0, count), spread over the worker and its helpers; returns when all are done.
	inline void ParallelFor(int count, ItemFn fn, void* arg) {
		if (!HelperCount || count < 2) { for (int i = 0; i < count; i++) fn(arg, i); return; }
		ForFn = fn; ForArg = arg; ForCount = count; ForStride = HelperCount + 1;
		for (int h = 0; h < HelperCount; h++) SetEvent(HelperGo[h]);
		for (int i = 0; i < count; i += ForStride) fn(arg, i);
		WaitForMultipleObjects(HelperCount, HelperDone, TRUE, INFINITE);
	}

	inline DWORD WINAPI Loop(void*) {
		for (;;) {
			if (WaitForSingleObject(Go, INFINITE) != WAIT_OBJECT_0) return 0;
			UseControlWords();
			PendingFn(PendingArg);
			SetEvent(Done);
		}
	}

	inline bool Ensure() {
		if (Thread || Failed) return Thread != nullptr;
		Go = CreateEventA(nullptr, FALSE, FALSE, nullptr);
		Done = CreateEventA(nullptr, FALSE, FALSE, nullptr);
		if (Go && Done) Thread = CreateThread(nullptr, 256 * 1024, &Loop, nullptr, 0, nullptr);
		if (!Thread) { Failed = true; Logger::Log("[WARNING] FrameWorker: no worker thread (error %lu): its jobs run on the main thread", GetLastError()); return false; }
		// Helpers: as many as leave the game's own thread and the worker a logical processor each, at most two.
		DWORD_PTR process = 0, system = 0;
		int logical = 0;
		if (GetProcessAffinityMask(GetCurrentProcess(), &process, &system)) for (DWORD_PTR m = process; m; m &= m - 1) logical++;
		const int want = logical - 2 < kMaxHelpers ? (logical - 2 > 0 ? logical - 2 : 0) : kMaxHelpers;
		for (int h = 0; h < want; h++) {
			HelperGo[h] = CreateEventA(nullptr, FALSE, FALSE, nullptr);
			HelperDone[h] = CreateEventA(nullptr, FALSE, FALSE, nullptr);
			if (!HelperGo[h] || !HelperDone[h] || !CreateThread(nullptr, 256 * 1024, &HelperLoop, (void*)(intptr_t)(h + 1), 0, nullptr)) break;
			HelperCount = h + 1;
		}
		Logger::Log("UNOFFICIAL FrameWorker: worker thread started, %d helper thread(s) (%d logical processors)", HelperCount, logical);
		return true;
	}

	// The main thread waits until the job has finished (nothing to wait for: returns at once).
	inline void Wait() {
		if (!Busy) return;
		WaitForSingleObject(Done, INFINITE);
		Busy = 0;
	}

	// Runs fn(arg) on the worker (in place when there is none). arg and what it points to belong to the job until Wait returns.
	inline void Start(JobFn fn, void* arg) {
		Wait();
		if (!Ensure()) { fn(arg); return; }
		unsigned int x87 = 0, sse = 0;
		__control87_2(0, 0, &x87, &sse);
		ControlX87 = x87; ControlSse = sse;
		PendingFn = fn; PendingArg = arg;
		Busy = 1;
		SetEvent(Go);
	}
}

// UNOFFICIAL 2026-11-03 SkyOnWorker: the sky's projection (effects/Sky/Sky.cpp) may be running on the worker. Its results (the sky
// irradiance and TESR_PhysSky's brightness) reach the sky's constants at Join, which every place that can read them calls first:
// ShaderRecord::SetCT (before uploading a constant of the sky's: shadow-map passes read none and do not wait), EffectRecord::SetCT,
// the start of both effect chains, Sm6Pass's constant copy, and the end of the game's render call (nothing carries over to the next
// frame). While nothing is pending each of those is one flag test.
namespace SkyProjection {
	inline bool Pending = false;
	inline const char* Begin = nullptr;	// the sky's constants (SkyShaders::Constants), set when they are registered
	inline const char* End = nullptr;
	void JoinNow();	// Sky.cpp
	inline void Join() { if (Pending) JoinNow(); }
	inline void JoinIfSky(const void* value) { if (Pending && (const char*)value >= Begin && (const char*)value < End) JoinNow(); }
}
