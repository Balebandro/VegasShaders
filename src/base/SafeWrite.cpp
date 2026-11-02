#include "SafeWrite.h"
#include <memoryapi.h>	// VirtualProtect

#pragma optimize("y", on)	// frame pointers omitted in the patch helpers

namespace {
	// Makes [address, address + bytes) writable (and executable) for the lifetime of the object.
	class WritableScope {
	public:
		WritableScope(SIZE_T address, SIZE_T bytes = sizeof(SIZE_T)) : start(address), length(bytes) {
			VirtualProtect((void*)start, length, PAGE_EXECUTE_READWRITE, &previous);
		}
		~WritableScope() {
			VirtualProtect((void*)start, length, previous, &previous);
		}

	private:
		const SIZE_T start;
		const SIZE_T length;
		SIZE_T previous;
	};

	// rel32 operand of a jump / call whose rel32 sits at `operand` (the instruction ends 4 bytes later).
	inline UInt32 RelativeTo(SIZE_T operand, SIZE_T target) { return target - (operand + 4); }
}

void __fastcall SafeWrite8(SIZE_T address, SIZE_T value) {
	WritableScope scope(address);
	*(uint8_t*)address = value;
}

void __fastcall SafeWrite16(SIZE_T address, SIZE_T value) {
	WritableScope scope(address);
	*(uint16_t*)address = value;
}

void __fastcall SafeWrite32(SIZE_T address, SIZE_T value) {
	WritableScope scope(address);
	*(uint32_t*)address = value;
}

void __fastcall SafeWriteBuf(SIZE_T address, const void* bytes, SIZE_T count) {
	WritableScope scope(address, count);
	memcpy((void*)address, bytes, count);
}

void __fastcall WriteRelJump(SIZE_T site, SIZE_T target) {
	WritableScope scope(site, 5);
	*(uint8_t*)site = 0xE9;
	*(uint32_t*)(site + 1) = RelativeTo(site + 1, target);
}

void __fastcall WriteRelCall(SIZE_T site, SIZE_T target) {
	WritableScope scope(site, 5);
	*(uint8_t*)site = 0xE8;
	*(uint32_t*)(site + 1) = RelativeTo(site + 1, target);
}

void __fastcall ReplaceCall(SIZE_T site, SIZE_T target) {
	SafeWrite32(site + 1, RelativeTo(site + 1, target));
}

#pragma optimize("y", off)	// back to the project setting
