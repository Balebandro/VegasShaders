#pragma once

// Patching the game's code in memory: plain writes (the page is made writable just for the write), 5-byte relative
// jumps and calls, and two small helpers that remember what a patched call or vtable slot pointed to before.

DECLSPEC_NOINLINE void __fastcall SafeWrite8(SIZE_T address, SIZE_T value);
DECLSPEC_NOINLINE void __fastcall SafeWrite16(SIZE_T address, SIZE_T value);
DECLSPEC_NOINLINE void __fastcall SafeWrite32(SIZE_T address, SIZE_T value);
DECLSPEC_NOINLINE void __fastcall SafeWriteBuf(SIZE_T address, const void* bytes, SIZE_T count);

// E9 (jmp) / E8 (call) rel32 written at `site`, going to `target`.
DECLSPEC_NOINLINE void __fastcall WriteRelJump(SIZE_T site, SIZE_T target);
DECLSPEC_NOINLINE void __fastcall WriteRelCall(SIZE_T site, SIZE_T target);
template <typename T> inline void __fastcall WriteRelJump(SIZE_T site, T target) { WriteRelJump(site, (SIZE_T)target); }
template <typename T> inline void __fastcall WriteRelCall(SIZE_T site, T target) { WriteRelCall(site, (SIZE_T)target); }

// Retargets the call instruction already at `site` (only its rel32 is rewritten).
DECLSPEC_NOINLINE void __fastcall ReplaceCall(SIZE_T site, SIZE_T target);

// Destination of the rel32 jump or call at `site`.
static inline SIZE_T GetRelJumpAddr(SIZE_T site) {
	const SIZE_T next = site + 5;
	return next + *(SIZE_T*)(site + 1);
}

// Points an existing call at a hook and keeps the function it called (GetOverwrittenAddr) for the hook to call on.
class CallDetour {	// one patched call site
	SIZE_T previous = 0;

public:
	template <typename T> DECLSPEC_NOINLINE void __fastcall ReplaceCall(SIZE_T site, T hook) {
		__assume(site != 0);
		if (*reinterpret_cast<uint8_t*>(site) != 0xE8) {		// not a call instruction: refuse, loudly
			char text[72];
			sprintf_s(text, "Cannot write detour; jumpSrc is not a function call. (0x%08X)", site);
			MessageBoxA(nullptr, text, "WriteRelCall", MB_OK | MB_ICONERROR);
			return;
		}
		previous = GetRelJumpAddr(site);
		::ReplaceCall(site, (SIZE_T)hook);
	}

	[[nodiscard]] SIZE_T GetOverwrittenAddr() const { return previous; }	// the original callee
};

// The same for a vtable slot: `slot` is the address of the entry.
class VirtFuncDetour {	// one patched vtable slot
protected:
	SIZE_T previous = 0;

public:
	template <typename T> inline void __fastcall ReplaceVirtualFunc(SIZE_T slot, T hook) {
		previous = *(uint32_t*)slot;
		SafeWrite32(slot, (SIZE_T)hook);
	}

	[[nodiscard]] SIZE_T GetOverwrittenAddr() const { return previous; }	// the original function
};
