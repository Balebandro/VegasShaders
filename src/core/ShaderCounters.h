#pragma once

// UNOFFICIAL 2026-10-26 (debugging tools, needs NVR DXVK 1.5-debug = fork patch 0016 (0015 before the 10-27 renumbering); docs\next\FORK-DEBUG-PIXEL.md).
//
// DebugReadbackRing: GPU results read back WITHOUT waiting. A ring of a few host-visible buffers (ID3D9NvrExtReadback): each frame
// a copy of a small GPU buffer is recorded into the next free one, and the oldest one is read only once the GPU has finished it
// (TryReadback returns S_FALSE otherwise and is asked again next frame). If every buffer still waits, that frame is simply not
// copied (counted as skipped). Nothing here ever calls the waiting ReadBuffer.
//
// ShaderCounters: named counters and a "printf" for one pixel, for any Vulkan pass. The pass's GLSL includes Compute\Counters.glsl and
// gets a second build with -DNVR_COUNTERS=1 (its own .spv); the effect draws that build instead of the normal one while its counter
// setting is on, with Buffer bound (Binding / Bind). Per-frame averages are logged once a second ("SHADER COUNTERS ...") and shown in
// the pixel inspector's window. Off (the setting false, or no 1.5-debug d3d9.dll): Begin returns null and the effect draws its normal
// pipeline; nothing is created.
//
// Example: effects\Reflections.cpp (the Hi-Z trace, [Main.Develop.ShaderCounters] ReflectionsHiZ).

#include <vector>
#include <string>
#include <initializer_list>
#include "DxvkCompute.h"

namespace DebugReadback {
	inline ID3D9NvrExtReadback* Ext = nullptr;
	inline bool Tried = false;

	// The 1.5-debug interface, or null (native D3D9, stock DXVK, an older NVR DXVK: logged once).
	inline ID3D9NvrExtReadback* Interface() {
		if (Tried) return Ext;
		Tried = true;
		ID3D9NvrExt* ext = DxvkCompute::Extension();
		if (!ext || !DxvkCompute::Available()) {
			Logger::Log("UNOFFICIAL debugging tools: need NVR DXVK (the NVR extension is not present: native D3D9 or stock DXVK); nothing is done.");
			return nullptr;
		}
		if (FAILED(ext->QueryInterface(__uuidof(ID3D9NvrExtReadback), (void**)&Ext)) || !Ext) {
			Ext = nullptr;
			Logger::Log("UNOFFICIAL debugging tools: need NVR DXVK 1.5-debug (fork patch 0016, non-waiting readback); this d3d9.dll has extension %u.%u without it: nothing is done.",
				DxvkCompute::Info().ExtVersionMajor, DxvkCompute::Info().ExtVersionMinor);
			return nullptr;
		}
		Logger::Log("UNOFFICIAL debugging tools: NVR DXVK 1.5-debug readback available (never waits).");
		return Ext;	// kept for the session (a device reference, like DxvkCompute's)
	}
}

class DebugReadbackRing {
public:
	static const int Slots = 4;	// read about Slots - 1 frames late; DXVK keeps at most a few frames in flight

	bool Ready() const { return Size != 0; }

	bool Init(UINT size, const char* name) {
		if (Size == size) return true;
		Release();
		ID3D9NvrExtReadback* rb = DebugReadback::Interface();
		if (!rb) return false;
		for (int i = 0; i < Slots; i++) {
			if (FAILED(rb->CreateReadbackBuffer(size, name, &Buffers[i])) || !Buffers[i]) {
				Logger::Log("UNOFFICIAL debugging tools: CreateReadbackBuffer(%s, %u bytes) failed", name ? name : "", size);
				Release();
				return false;
			}
		}
		Size = size;
		return true;
	}

	// Records a copy of Bytes bytes of Source (a TRANSFER_SRC buffer) into the next free slot: its index, or -1 when every slot
	// still waits for the GPU (nothing recorded; Skipped counts it).
	int Submit(ID3D9NvrExtBuffer* Source, UINT Bytes) {
		ID3D9NvrExtReadback* rb = DebugReadback::Interface();
		if (!rb || !Size || !Source || Bytes > Size) return -1;
		if (Count >= Slots) { Skipped++; return -1; }
		const int slot = (Oldest + Count) % Slots;
		if (FAILED(rb->CopyToReadback(Buffers[slot], 0, Source, 0, Bytes))) return -1;
		Count++;
		return slot;
	}

	// The oldest pending slot, if the GPU has finished it: true, its index and Bytes bytes in Out. Call until false.
	bool Poll(void* Out, UINT Bytes, int& slot) {
		ID3D9NvrExtReadback* rb = DebugReadback::Interface();
		if (!rb || !Count || Bytes > Size) return false;
		const HRESULT result = rb->TryReadback(Buffers[Oldest], 0, Bytes, Out);
		if (result == S_FALSE) return false;
		slot = Oldest;
		Oldest = (Oldest + 1) % Slots;
		Count--;
		return result == S_OK;	// a failed read drops that slot
	}

	void Release() {
		for (int i = 0; i < Slots; i++) DxvkCompute::Drop(Buffers[i]);
		Size = 0; Oldest = 0; Count = 0;
	}

	UINT Skipped = 0;

private:
	ID3D9NvrExtBuffer*	Buffers[Slots] = {};
	UINT				Size = 0;
	int					Oldest = 0, Count = 0;
};

class ShaderCounters {
public:
	static const UINT MaxCounters = 32, MaxProbes = 8;
	struct Block { UINT header[4]; UINT count[MaxCounters]; float probe[MaxProbes][4]; };	// Counters.glsl's layout (std430, 272 bytes)
	static_assert(sizeof(Block) == 272, "Counters.glsl layout");

	// Name: the log/window title. Counters / Probes: names of counter i and probe slot j (as the GLSL uses them).
	ShaderCounters(const char* Name, std::initializer_list<const char*> Counters, std::initializer_list<const char*> Probes)
		: name(Name), counterNames(Counters), probeNames(Probes) { All().push_back(this); }

	// The binding to add to the counting pipeline: Counters.glsl's block (set 3, binding 0 unless moved) as a written storage buffer.
	static NVR_EXT_BINDING_DESC Binding(UINT Slot, UINT Set = 3, UINT BindingIndex = 0) {
		NVR_EXT_BINDING_DESC b = { Set, BindingIndex, NVR_EXT_BIND_STORAGE_BUFFER, Slot, NVR_EXT_VIEW_2D, TRUE, 0 };
		return b;
	}
	static NVR_EXT_RESOURCE_BINDING Bind(UINT Slot, ID3D9NvrExtBuffer* Buffer) {
		NVR_EXT_RESOURCE_BINDING r = {};
		r.Slot = Slot; r.Type = NVR_EXT_BIND_STORAGE_BUFFER; r.pResource = Buffer;
		return r;
	}

	// Before the pass, each frame it draws with counters: reads what has finished (never waits), resets the block (ProbeX/Y: the
	// probe pixel in the PASS's target pixels, negative = none) and returns the buffer to bind. Null: off or unavailable (draw the
	// normal pipeline).
	ID3D9NvrExtBuffer* Begin(int ProbeX, int ProbeY) {
		if (failed) return nullptr;
		if (!buffer) {
			if (!DebugReadback::Interface()) { failed = true; return nullptr; }
			buffer = DxvkCompute::CreateBufferNoFail(sizeof(Block), NVR_EXT_BUFFER_STORAGE | NVR_EXT_BUFFER_TRANSFER_SRC | NVR_EXT_BUFFER_TRANSFER_DST, name);
			if (!buffer || !ring.Init(sizeof(Block), name)) {
				Logger::Log("UNOFFICIAL SHADER COUNTERS %s: could not be set up; off for the session.", name);
				failed = true;
				DxvkCompute::Drop(buffer);
				return nullptr;
			}
			Logger::Log("UNOFFICIAL SHADER COUNTERS %s: on (read back without waiting, logged once a second).", name);
		}
		Collect();
		Block reset = {};
		reset.header[0] = ProbeX >= 0 ? (UINT)ProbeX : 0xFFFFFFFFu;
		reset.header[1] = ProbeY >= 0 ? (UINT)ProbeY : 0xFFFFFFFFu;
		reset.header[2] = ++frame;
		probeX = ProbeX; probeY = ProbeY;
		if (FAILED(DxvkCompute::Extension()->UpdateBuffer(buffer, 0, sizeof(Block), &reset))) return nullptr;
		begun = true;
		return buffer;
	}

	// Right after the pass: queues the block for readback (never waits).
	void End() {
		if (!begun) return;
		begun = false;
		ring.Submit(buffer, sizeof(Block));
	}

	// One line: the last second's per-frame averages and the last probe (window and log).
	const std::string& Summary() const { return summary; }
	const char* Name() const { return name; }
	static std::vector<ShaderCounters*>& All() { static std::vector<ShaderCounters*> all; return all; }

private:
	void Collect() {
		Block block;
		int slot = 0;
		while (ring.Poll(&block, sizeof(Block), slot)) {
			for (UINT i = 0; i < MaxCounters; i++) sums[i] += block.count[i];
			memcpy(lastProbe, block.probe, sizeof(lastProbe));
			lastProbeX = (int)block.header[0]; lastProbeY = (int)block.header[1];
			framesRead++;
		}
		const ULONGLONG now = GetTickCount64();
		if (!windowStart) windowStart = now;
		if (now - windowStart < 1000 || !framesRead) return;
		char line[2048];
		int n = _snprintf_s(line, sizeof(line), _TRUNCATE, "%u frames read (%u skipped, readback ring full)", framesRead, ring.Skipped);
		for (size_t i = 0; i < counterNames.size() && i < MaxCounters && n > 0 && n < (int)sizeof(line); i++)
			n += _snprintf_s(line + n, sizeof(line) - n, _TRUNCATE, " | %s %.1f", counterNames[i], (double)sums[i] / framesRead);
		if (lastProbeX != -1 && n > 0 && n < (int)sizeof(line)) {
			n += _snprintf_s(line + n, sizeof(line) - n, _TRUNCATE, " || probe pixel (%d, %d):", lastProbeX, lastProbeY);
			for (size_t j = 0; j < probeNames.size() && j < MaxProbes && n > 0 && n < (int)sizeof(line); j++)
				n += _snprintf_s(line + n, sizeof(line) - n, _TRUNCATE, " %s (%.4g %.4g %.4g %.4g)", probeNames[j], lastProbe[j][0], lastProbe[j][1], lastProbe[j][2], lastProbe[j][3]);
		}
		summary = line;
		Logger::Log("SHADER COUNTERS %s (per frame): %s", name, line);
		memset(sums, 0, sizeof(sums));
		framesRead = 0;
		ring.Skipped = 0;
		windowStart = now;
	}

	const char*					name;
	std::vector<const char*>	counterNames, probeNames;
	ID3D9NvrExtBuffer*			buffer = nullptr;
	DebugReadbackRing			ring;
	bool						failed = false, begun = false;
	UINT						frame = 0, framesRead = 0;
	unsigned long long			sums[MaxCounters] = {};
	float						lastProbe[MaxProbes][4] = {};
	int							probeX = -1, probeY = -1, lastProbeX = -1, lastProbeY = -1;
	ULONGLONG					windowStart = 0;
	std::string					summary;
};
