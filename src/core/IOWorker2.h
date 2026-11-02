#pragma once

// UNOFFICIAL IOWorker2 (EXPERIMENTAL, OFF by default, 2026-10-20, side session ioworker; bench\deep-roadmap\evidence\ioworker-LOG.md
// STEP 2). The game loads models, textures, cells... on ONE loading thread (the IOManager's worker). The ArrivalSampler showed
// it busy 99 % of the time while the player waits in IOManager::LoadQueuedPriority after a loading screen (go-Primm 400-900 ms),
// with 600-900 tasks queued, two thirds of it CPU work in model loads (NPC FaceGen face textures, D3DX texture decoding). This
// gives the game a SECOND worker that only takes model and texture tasks (QueuedModel, QueuedTexture): work the main thread
// already does at the same time as the first worker (synchronous loads), so no new kind of concurrency; cell loading,
// references, LOD, trees, heads, audio and animations stay on worker 0, away from the shared-state races found by acidpointer's
// psycho (whose IOManager layout and hazard list this design learned from; nothing copied).
// Three patches, all byte-checked first (any mismatch: nothing written, one log line), applied at plugin load, before the game
// creates its IOManager:
//   P1 0x00C3DA7A  IOManager::IOManager 'push 1' (workers) -> 'push 2': the game creates and starts worker 1 like worker 0.
//   P2 0x010C165C  IOManager's own vtable slot +0x58 (which worker BSTaskManager::AddTask wakes; a shared 'return 0' stub): ours
//                  still returns 0 (worker 0 woken exactly as before) and, for a model / texture task, ALSO wakes worker 1 the
//                  game's way (only if it has no wake-up pending).
//   P3 0x00C41201  BSTaskManagerThread::ThreadUpdate, the 'is this task queued?' check (mov edx,[ebp-34h]; cmp [edx+0Ch],1): a
//                  jump to a stub that redoes it; on worker 1 only, a task that is not a model / texture task (or while a
//                  ModelLoader lock-free map has no thread slot left for worker 1) takes the game's own 'not runnable' path, so
//                  it stays queued for worker 0. On every other thread the stub only redoes the check.
// Worker 1 runs at below-normal priority. [Main.Main.Performance] IOWorker2 = 1 to try it; read at plugin load (restart).
#include <atomic>
#include "../base/SafeWrite.h"

namespace IOWorker2 {
	static const DWORD kCountPush = 0x00C3DA7A;		// 6A 01
	static const DWORD kSelectSlot = 0x010C165C;	// IOManager vtable +0x58
	static const DWORD kSelectStub = 0x008D0560;	// ThisCall_Ret0_OneArg
	static const DWORD kFilterSite = 0x00C41201;	// 8B 55 CC 83 7A 0C 01 (75 6A)
	static const DWORD kIOManagerPtr = 0x01202D98, kModelLoaderPtr = 0x011C3B3C;
	static const DWORD kVtQueuedModel = 0x01016890, kVtQueuedTexture = 0x01016788;
	static const DWORD kMapOffsets[] = { 0x00, 0x04, 0x08, 0x0C, 0x10, 0x14, 0x18, 0x20 };	// ModelLoader's lock-free maps (thread slots: 10 each)

	inline DWORD FilterBack = 0x00C41208, FilterSkip = 0x00C41274;	// the stub's two exits: the original 'jnz' / the 'not runnable' path
	inline bool Armed = false;
	inline volatile LONG PriorityDone = 0, MapsBad = 0, MapsLogged = 0;
	inline std::atomic<long> Woken{ 0 }, Approved{ 0 }, Refused{ 0 }, SlotRefused{ 0 };
	inline DWORD LastLogTick = 0;

	inline DWORD Peek(DWORD a) { __try { return a > 0x10000 ? *(const DWORD*)(uintptr_t)a : 0; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; } }

	// The IOManager's worker 1 (BSTaskManagerThread*), when there are two; kept once found (it lives as long as the IOManager).
	inline volatile DWORD W1 = 0;
	inline DWORD Worker1() {
		if (W1) return W1;
		const DWORD io = Peek(kIOManagerPtr);
		if (!io || Peek(io + 0x4C) < 2) return 0;
		const DWORD w = Peek(Peek(io + 0x50) + 4);
		if (w && Peek(w + 0x30) == io) W1 = w;	// its owner is the IOManager
		return W1;
	}

	// Worker 1 only: does every ModelLoader map either hold a thread slot for this thread already, or have two free?
	// (ThreadSpecificInterfaceManager at map +0x14: capacity +0, TLS index +4, slots used +0x0C; a thread past the capacity gets
	// a NULL interface, i.e. a crash.)
	inline bool SlotsOk() {
		if (MapsBad) return false;
		const DWORD ml = Peek(kModelLoaderPtr);
		if (!ml) return false;
		for (DWORD off : kMapOffsets) {
			const DWORD map = Peek(ml + off), tsim = map ? Peek(map + 0x14) : 0;
			const DWORD cap = tsim ? Peek(tsim) : 0, tls = tsim ? Peek(tsim + 4) : 0, used = tsim ? Peek(tsim + 0x0C) : 0;
			if (!tsim || cap < 1 || cap > 64 || tls >= 1088 || used > cap + 64) {	// not the layout we know: worker 1 takes nothing
				if (!InterlockedExchange(&MapsBad, 1)) Logger::Log("UNOFFICIAL IOWorker2: ModelLoader map +%02X not as expected (map %08X, slots %08X: capacity %u, used %u); worker 1 takes no tasks", off, map, tsim, cap, used);
				return false;
			}
			if (TlsGetValue(tls)) continue;	// this thread has its slot already
			if (used + 2 > cap) return false;
		}
		return true;
	}

	// P3, from the stub, on every step of every BSTaskManagerThread's sweep: may this thread take this task?
	inline bool __cdecl MayRun(DWORD self, DWORD task) {
		if (!self || !task || self != Worker1()) return true;	// worker 0 and every other task thread: unchanged
		if (*(const volatile LONG*)(task + 0x0C) != 1) return true;	// not queued: the game's own check skips it anyway
		const DWORD vt = *(const DWORD*)task;
		if (vt != kVtQueuedModel && vt != kVtQueuedTexture) { Refused++; return false; }
		if (!SlotsOk()) { SlotRefused++; return false; }
		Approved++;
		return true;
	}

	__declspec(naked) inline void FilterStub() {
		__asm {
			mov edx, [ebp - 0x34]			// the task (the replaced 'mov edx, [ebp-34h]')
			push edx
			push dword ptr [ebp - 0x88]		// ThreadUpdate's 'this' (the BSTaskManagerThread)
			call MayRun
			add esp, 8
			test al, al
			jz skip
			mov edx, [ebp - 0x34]
			cmp dword ptr [edx + 0x0C], 1	// the replaced 'cmp [edx+0Ch], 1'
			jmp dword ptr [FilterBack]		// the game's 'jnz' (flags kept)
		skip:
			jmp dword ptr [FilterSkip]
		}
	}

	// P2: BSTaskManager::AddTask asks which worker to wake for a new task (thiscall: ECX = the IOManager, one stack argument).
	inline UInt32 __fastcall Select(void* io, void* edx, void* task) {
		const DWORD w1 = Worker1();
		if (w1 && task) {
			const DWORD vt = *(const DWORD*)task;
			if (vt == kVtQueuedModel || vt == kVtQueuedTexture) {
				if (!PriorityDone && !InterlockedExchange(&PriorityDone, 1)) SetThreadPriority(*(HANDLE*)(w1 + 0x04), THREAD_PRIORITY_BELOW_NORMAL);
				volatile LONG* pending = (volatile LONG*)(w1 + 0x0C);
				if (*pending == 0) {	// BSTaskThread::SignalTask: wake only without a wake-up pending (BSSemaphore::Signal)
					ReleaseSemaphore(*(HANDLE*)(w1 + 0x10), 1, nullptr);
					InterlockedIncrement((LONG*)pending);
					Woken++;
				}
			}
		}
		return 0;	// worker 0, as the game always did
	}

	// NVSEPlugin_Load, after the settings are read.
	inline void Install() {
		SettingManager::Configuration::ConfigNode node;
		const bool want = TheSettingManager->Config.FillNode(&node, "Main.Main.Performance", "IOWorker2") && node.IntValue > 0;
		if (!want) return;
		static const unsigned char kPush[] = { 0x6A, 0x01, 0x6A, 0x0A };
		static const unsigned char kSite[] = { 0x8B, 0x55, 0xCC, 0x83, 0x7A, 0x0C, 0x01, 0x75, 0x6A };
		const DWORD io = Peek(kIOManagerPtr), slot = Peek(kSelectSlot);
		const bool push = !memcmp((const void*)kCountPush, kPush, sizeof(kPush)), site = !memcmp((const void*)kFilterSite, kSite, sizeof(kSite));
		if (io || !push || slot != kSelectStub || !site) {
			Logger::Log("UNOFFICIAL IOWorker2: NOT armed (IOManager already created %s, worker-count bytes %s, vtable slot %08X %s, sweep check bytes %s): the game keeps its one loading thread",
				io ? "YES" : "no", push ? "as expected" : "DIFFERENT", slot, slot == kSelectStub ? "as expected" : "DIFFERENT", site ? "as expected" : "DIFFERENT");
			return;
		}
		SafeWrite32(kSelectSlot, (SIZE_T)&Select);
		unsigned char jump[7] = { 0xE9, 0, 0, 0, 0, 0x90, 0x90 };
		*(DWORD*)(jump + 1) = (DWORD)(uintptr_t)&FilterStub - (kFilterSite + 5);
		SafeWriteBuf(kFilterSite, jump, sizeof(jump));
		SafeWrite8(kCountPush + 1, 0x02);	// last: from here on the game will create two workers
		FlushInstructionCache(GetCurrentProcess(), (void*)kFilterSite, sizeof(jump));
		Armed = true;
		Logger::Log("UNOFFICIAL IOWorker2: armed (EXPERIMENTAL): the game gets a second loading thread for model and texture tasks");
	}

	// Render thread, every frame: the first time worker 1 exists, and then once a minute, what it did.
	inline void Tick() {
		if (!Armed) return;
		const DWORD now = GetTickCount();
		static bool told = false;
		if (!told) {
			const DWORD w1 = Worker1();
			if (!w1) return;
			told = true; LastLogTick = now;
			Logger::Log("UNOFFICIAL IOWorker2: the IOManager has %u workers; worker 1 = thread %u", Peek(Peek(kIOManagerPtr) + 0x4C), Peek(w1 + 0x08));
			return;
		}
		if (now - LastLogTick < 60000) return;
		LastLogTick = now;
		Logger::Log("UNOFFICIAL IOWorker2: last minute: worker 1 woken %ld times, took %ld model / texture tasks, left %ld other tasks to worker 0, %ld refused for map slots",
			Woken.exchange(0), Approved.exchange(0), Refused.exchange(0), SlotRefused.exchange(0));
	}
}
