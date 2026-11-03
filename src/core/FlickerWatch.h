#pragma once

// UNOFFICIAL 2026-10-23 FlickerWatch (test only; work\bench\deep-roadmap\evidence\cloudflicker): the P82 v3 report of the ground going
// ~17% darker on single frames outdoors (DXVK, nobody could reproduce it). Each frame the finished picture (before the HUD) is shrunk to
// 160x90 and read back two frames later (no stall on the frame itself). A frame whose lower half changes by more than Threshold
// while the upper half stays nearly the same is logged with what that frame did differently from the one before: which pass applied
// the sun shadow (Path bits below), whether AmbientLight kept its history, the SunFar factor, the frame time. Bounded log.
// [Shaders.CloudShadows.Main] FlickerWatch = 0 off, 1 on; FlickerThreshold (default 0.06).
#include <cmath>
#include <cstdio>

namespace FlickerWatch {

	// Set during the frame by ShaderManager / AmbientLight; read when the frame's picture is taken.
	enum : unsigned {
		kComposite = 1,        // the shadow applied inside the fog composite
		kPair = 2,             // the shadow + AO pair pass
		kIntoAL = 4,           // the shadow planned into AmbientLight's Apply
		kALApplied = 8,        // ...and AmbientLight did apply it
		kShadowPass = 16,      // the ShadowsExteriors pass ran on its own
		kSunFar = 32,          // the SunFar factor was drawn this frame
		kALHistory = 64,       // AmbientLight kept its history (continuous)
		kLodUpdated = 128,     // the LOD sun cascade was redrawn
		kCellChanged = 256,
		kALRendered = 512,     // AmbientLight's passes ran
		kSunGlide = 1024,      // (2026-10-25) the quantized sun was gliding to a new step this frame
	};
	inline float Hour = 0.0f, SunZ = 0.0f;	// (2026-10-25) set with Flags: the game hour and the shadows' sun height this frame
	inline unsigned Flags = 0;
	// (2026-10-25, bench sweep mode: multirun\sweep.py) a general flicker score next to the ground test above: the picture in a 4x3 grid,
	// a frame whose cell jumps away from BOTH neighbouring frames in the same direction (a one-frame spike; a camera pan or a time-lapse
	// changes cells steadily, not up-and-back) by more than SpikeThreshold of its brightness counts as a spike. Forced (shot list
	// 'flickerwatch') runs it indoors and out, whatever [Shaders.CloudShadows.Main] FlickerWatch says. Per-shot counters for SettingShots.
	static const int GX = 4, GY = 3;
	inline bool Forced = false;
	inline float SpikeThreshold = 0.05f;
	inline float Grid[3][GX * GY] = {};		// the last three measured frames (consecutive)
	inline unsigned GridFrame[3] = {}, GridCount = 0;
	inline unsigned SkipFrames = 0;	// a new shot reloads the settings: its first measured frames are not scored
	inline unsigned ShotFrames = 0, ShotSpikes = 0; inline float ShotMax = 0.0f; inline int ShotMaxCell = -1;
	inline char Label[64] = "";

	struct Slot { IDirect3DSurface9* rt = nullptr; IDirect3DSurface9* mem = nullptr; bool pending = false; unsigned frame = 0, flags = 0; float dt = 0.0f, hour = 0.0f, sunZ = 0.0f; };
	inline Slot Ring[3];
	inline D3DSURFACE_DESC Desc = {};
	inline unsigned Frame = 0, Lines = 0, Flagged = 0, Measured = 0;
	inline bool HavePrev = false, Failed = false;
	inline float PrevTop = 0.0f, PrevBottom = 0.0f, MaxDrop = 0.0f;
	inline unsigned PrevFlags = 0, PrevFrame = 0;
	static const UINT W = 160, H = 90;

	inline void Release() {
		for (Slot& s : Ring) {
			if (s.rt) s.rt->Release();
			if (s.mem) s.mem->Release();
			s = Slot();
		}
		HavePrev = false;
	}

	inline float Luma8(const UInt8* p) { return (0.2126f * p[2] + 0.7152f * p[1] + 0.0722f * p[0]) / 255.0f; }

	inline bool Measure(Slot& s, float& top, float& bottom, D3DFORMAT format = D3DFMT_UNKNOWN, float* grid = nullptr) {
		if (format == D3DFMT_UNKNOWN) format = Desc.Format;
		if (FAILED(TheRenderManager->device->GetRenderTargetData(s.rt, s.mem))) return false;
		D3DLOCKED_RECT lock = {};
		if (FAILED(s.mem->LockRect(&lock, NULL, D3DLOCK_READONLY))) return false;
		double sumTop = 0.0, sumBottom = 0.0;
		double cells[GX * GY] = {};
		for (UINT y = 0; y < H; y++) {
			const UInt8* row = (const UInt8*)lock.pBits + (size_t)y * lock.Pitch;
			double sum = 0.0;
			for (UINT x = 0; x < W; x++) {
				float l;
				if (format == D3DFMT_A16B16G16R16F) {
					const D3DXFLOAT16* h = (const D3DXFLOAT16*)(row + x * 8);
					float rgb[3];
					D3DXFloat16To32Array(rgb, h, 3);
					l = 0.2126f * rgb[0] + 0.7152f * rgb[1] + 0.0722f * rgb[2];
				}
				else l = Luma8(row + x * 4);
				sum += l;
				if (grid) cells[(y * GY / H) * GX + x * GX / W] += l;
			}
			if (y < H / 3) sumTop += sum; else if (y >= H / 2) sumBottom += sum;
		}
		s.mem->UnlockRect();
		top = (float)(sumTop / (W * (H / 3)));
		bottom = (float)(sumBottom / (W * (H - H / 2)));
		if (grid) for (int i = 0; i < GX * GY; i++) grid[i] = (float)(cells[i] / ((W / GX) * (H / GY)));
		return true;
	}

	// (2026-10-25) Two more measuring points in the frame, so a flagged frame says WHERE it changed: 'world' = the scene as the game
	// drew it (forward sun shadows included), before NVR's post chain; 'lit' = right after the sun-shadow apply / AO / AmbientLight
	// step. Each is shrunk and read back two frames later like the final picture; its lower-half brightness is kept per frame.
	struct Probe {
		const char* name;
		Slot ring[3];
		D3DSURFACE_DESC desc = {};
		float bottom[16] = {};
		unsigned frameOf[16] = {};
		bool failed = false;
	};
	inline Probe World{ "world" }, Lit{ "lit" };

	inline void ReleaseProbe(Probe& p) {
		for (Slot& s : p.ring) {
			if (s.rt) s.rt->Release();
			if (s.mem) s.mem->Release();
			s = Slot();
		}
	}

	// Called mid-frame (before OnFrame, which ends the frame and advances Frame).
	inline void OnProbe(Probe& p, IDirect3DDevice9* device, IDirect3DSurface9* surface) {
		if (p.failed || Failed || !device || !surface) return;
		D3DSURFACE_DESC d = {};
		surface->GetDesc(&d);
		if (d.Format != D3DFMT_A8R8G8B8 && d.Format != D3DFMT_X8R8G8B8 && d.Format != D3DFMT_A16B16G16R16F) {
			p.failed = true;
			Logger::Log("FLICKER WATCH: %s picture format %d not handled, that point off", p.name, (int)d.Format);
			return;
		}
		if (!p.ring[0].rt || d.Format != p.desc.Format) {
			ReleaseProbe(p);
			p.desc = d;
			for (Slot& s : p.ring)
				if (FAILED(device->CreateRenderTarget(W, H, d.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &s.rt, NULL)) ||
					FAILED(device->CreateOffscreenPlainSurface(W, H, d.Format, D3DPOOL_SYSTEMMEM, &s.mem, NULL))) {
					ReleaseProbe(p);
					p.failed = true;
					Logger::Log("FLICKER WATCH: could not make the %s read-back surfaces, that point off", p.name);
					return;
				}
		}
		Slot& write = p.ring[Frame % 3];
		write.pending = SUCCEEDED(device->StretchRect(surface, NULL, write.rt, NULL, D3DTEXF_LINEAR)) ||
			SUCCEEDED(device->StretchRect(surface, NULL, write.rt, NULL, D3DTEXF_NONE));
		write.frame = Frame;
		Slot& read = p.ring[(Frame + 1) % 3];
		if (!read.pending) return;
		read.pending = false;
		float top, bottom;
		if (!Measure(read, top, bottom, p.desc.Format)) return;
		p.bottom[read.frame % 16] = bottom;
		p.frameOf[read.frame % 16] = read.frame;
	}

	// " world +1.2% lit -17.0%": each point's lower-half change from frame-1 to frame ('?' when not measured).
	inline void ProbeText(unsigned frame, char* out, size_t size) {
		out[0] = 0;
		for (const Probe* p : { &World, &Lit }) {
			const unsigned a = frame % 16, b = (frame - 1) % 16;
			char part[48];
			if (p->frameOf[a] == frame && p->frameOf[b] == frame - 1 && p->bottom[b] > 1e-4f)
				sprintf_s(part, " %s %+.1f%%", p->name, (p->bottom[a] / p->bottom[b] - 1.0f) * 100.0f);
			else sprintf_s(part, " %s ?", p->name);
			strcat_s(out, size, part);
		}
	}

	inline void FlagText(unsigned f, char* out, size_t size) {
		sprintf_s(out, size, "%s%s%s%s%s%s%s%s%s%s%s", f & kComposite ? " composite" : "", f & kPair ? " pair" : "", f & kIntoAL ? " intoAL" : "",
			f & kALApplied ? " ALapplied" : "", f & kShadowPass ? " shadowpass" : "", f & kSunFar ? " sunfar" : "", f & kALHistory ? " ALhistory" : "",
			f & kLodUpdated ? " lodupd" : "", f & kCellChanged ? " cellchg" : "", f & kALRendered ? " ALran" : "", f & kSunGlide ? " glide" : "");
	}

	// Once a frame, on the finished picture (before the HUD).
	inline void OnFrame(IDirect3DDevice9* device, IDirect3DSurface9* target, float threshold) {
		const unsigned flags = Flags;
		Flags = 0;
		if (Failed || !device || !target) return;
		D3DSURFACE_DESC d = {};
		target->GetDesc(&d);
		if (d.Format != D3DFMT_A8R8G8B8 && d.Format != D3DFMT_X8R8G8B8 && d.Format != D3DFMT_A16B16G16R16F) {
			Failed = true;
			Logger::Log("FLICKER WATCH: picture format %d not handled, off", (int)d.Format);
			return;
		}
		if (!Ring[0].rt || d.Format != Desc.Format) {
			Release();
			Desc = d;
			for (Slot& s : Ring)
				if (FAILED(device->CreateRenderTarget(W, H, d.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &s.rt, NULL)) ||
					FAILED(device->CreateOffscreenPlainSurface(W, H, d.Format, D3DPOOL_SYSTEMMEM, &s.mem, NULL))) {
					Release();
					Failed = true;
					Logger::Log("FLICKER WATCH: could not make the read-back surfaces, off");
					return;
				}
			Logger::Log("FLICKER WATCH: on (picture %ux%u format %d, threshold %.3f)", d.Width, d.Height, (int)d.Format, threshold);
		}
		static double lastTime = 0.0;
		const double now = TheFrameRateManager->Time;
		Slot& write = Ring[Frame % 3];
		if (SUCCEEDED(device->StretchRect(target, NULL, write.rt, NULL, D3DTEXF_LINEAR)) || SUCCEEDED(device->StretchRect(target, NULL, write.rt, NULL, D3DTEXF_NONE))) {
			write.pending = true;
			write.frame = Frame;
			write.flags = flags;
			write.dt = (float)((now - lastTime) * 1000.0);
			write.hour = Hour;
			write.sunZ = SunZ;
		}
		lastTime = now;
		Slot& read = Ring[(Frame + 1) % 3];	// written two frames ago
		Frame++;
		if (!read.pending) return;
		read.pending = false;
		float top, bottom, grid[GX * GY];
		if (!Measure(read, top, bottom, D3DFMT_UNKNOWN, grid)) { HavePrev = false; GridCount = 0; return; }
		Measured++;
		{	// (2026-10-25) the spike score: frame m = the middle of the last three consecutive measured frames
			if (GridCount && read.frame != GridFrame[(GridCount - 1) % 3] + 1) GridCount = 0;
			memcpy(Grid[GridCount % 3], grid, sizeof(grid)); GridFrame[GridCount % 3] = read.frame; GridCount++;
			ShotFrames++;
			if (SkipFrames) SkipFrames--;
			else if (GridCount >= 3) {
				const float* a = Grid[(GridCount - 3) % 3]; const float* m = Grid[(GridCount - 2) % 3]; const float* b = Grid[(GridCount - 1) % 3];
				float spike = 0.0f; int cell = -1; float signedSpike = 0.0f;
				for (int i = 0; i < GX * GY; i++) {
					const float da = m[i] - a[i], db = m[i] - b[i], ref = (a[i] > b[i] ? a[i] : b[i]);
					if (ref < 0.01f || da * db <= 0.0f) continue;
					const float s = (fabsf(da) < fabsf(db) ? fabsf(da) : fabsf(db)) / ref;
					if (s > spike) { spike = s; cell = i; signedSpike = da > 0.0f ? s : -s; }
				}
				if (spike > ShotMax) { ShotMax = spike; ShotMaxCell = cell; }
				if (spike > SpikeThreshold) {
					ShotSpikes++;
					if (Lines < 300) {
						Lines++;
						Logger::Log("FLICKER SPIKE frame %u: cell %d (col %d row %d) %+.1f%% for one frame; %.1f ms; hour %.2f; shot '%s'",
							GridFrame[(GridCount - 2) % 3], cell, cell % GX, cell / GX, signedSpike * 100.0f, read.dt, read.hour, Label);
					}
				}
			}
		}
		if (HavePrev && read.frame == PrevFrame + 1 && PrevBottom > 0.01f && PrevTop > 0.01f) {
			const float change = bottom / PrevBottom - 1.0f, topChange = top / PrevTop - 1.0f;
			if (fabsf(change) > threshold && fabsf(topChange) < threshold * 0.5f) {
				Flagged++;
				if (-change > MaxDrop) MaxDrop = -change;
				if (Lines < 300) {
					Lines++;
					char was[160], is[160];
					FlagText(PrevFlags, was, sizeof(was));
					FlagText(read.flags, is, sizeof(is));
					char where[128];
					ProbeText(read.frame, where, sizeof(where));
					Logger::Log("FLICKER frame %u: lower half %+.1f%% (%.4f -> %.4f), upper %+.1f%%; %.1f ms; hour %.2f sun z %.3f; where:%s; before:%s | this:%s",
						read.frame, change * 100.0f, PrevBottom, bottom, topChange * 100.0f, read.dt, read.hour, read.sunZ, where, was, is);
				}
			}
			else if ((read.flags & ~(kLodUpdated | kCellChanged | kSunGlide)) != (PrevFlags & ~(kLodUpdated | kCellChanged | kSunGlide)) && Lines < 300) {
				// A change of path without a visible jump: logged too (bounded), it says the path change is harmless.
				Lines++;
				char was[160], is[160];
				FlagText(PrevFlags, was, sizeof(was));
				FlagText(read.flags, is, sizeof(is));
				Logger::Log("FLICKER WATCH path change frame %u (lower half %+.1f%%, upper %+.1f%%; %.1f ms): before:%s | this:%s",
					read.frame, change * 100.0f, topChange * 100.0f, read.dt, was, is);
			}
		}
		HavePrev = true;
		PrevTop = top; PrevBottom = bottom; PrevFlags = read.flags; PrevFrame = read.frame;
		if (Measured % 600 == 0)
			Logger::Log("FLICKER WATCH summary: %u frames measured, %u flagged, largest drop %.1f%%; hour %.2f sun z %.3f", Measured, Flagged, MaxDrop * 100.0f, read.hour, read.sunZ);
	}
}
