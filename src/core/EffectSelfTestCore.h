#pragma once

// UNOFFICIAL, DIAGNOSTIC (2026-10-12): the device-side and number-side half of the in-game effect self-test (EffectSelfTest.h
// has the game half: the setting, the configurations, the hooks and the log text). No game types here, so
// tests\effect_selftest.cpp runs it on a real D3D9 device with made-up frames. Grown out of the merged-lamps self-test
// (MergeSelfTest.h, parked in work\parked\merged-lamps).
// - Reader: a render target -> a quarter-size float picture (rgb). The GPU halves it twice with linear StretchRects (a 4x4 box
//   average for even sizes) and only the quarter-size picture is read back; if that is not possible, the whole target is read
//   and averaged 4x4 on the CPU. Multisampled targets are resolved first.
// - Series / BlockStats: one block's consecutive frames at one capture point: mean picture and per-pixel mean frame-to-frame
//   change of luma (the flicker).
// - PointData / Compare / FinishRun: each configuration against the mean of the reference (spill off) blocks before and after
//   it (critic-review-4: linear drift cancels), per 8 x 8 region, so the figures can leave out regions that changed by
//   themselves; flicker against the reference's brightness; light pixels and the ring round them; the noise floor.
// - Pictures: built on the CPU from small per-pixel maps made during the run, written as PNG with WIC (no device), on the
//   game's worker thread.
// - Shots (critic-review-14 part 1): ShotReader reads the final picture at FULL size as 8-bit BGRA for a PNG per configuration;
//   FrameOverride sets values for one frame and puts them back bit for bit; BounceValues / Read / Write are the Bounce Light
//   members a configuration sets (templates, so the test can check them on a stand-in).
#include <d3d9.h>
#include <d3dx9.h>
#include <wincodec.h>
#include <vector>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "windowscodecs.lib")

namespace SelfTestCore {

	enum { Scale = 4, Grid = 8, MaxConfigs = 16, RingRadius = 8 };	// 16: critic-review-9's 13 measured configurations + 3 picture-only ones (critic-review-14);
																	// the light sheet holds the measured ones + 2 tiles in 16

	inline float Luma(const float* c) { return 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2]; }
	inline float Saturation(const float* c) {
		const float peak = (std::max)(c[0], (std::max)(c[1], c[2])), low = (std::min)(c[0], (std::min)(c[1], c[2]));
		return peak > 1.0e-4f ? (peak - (std::max)(low, 0.0f)) / peak : 0.0f;
	}
	inline float Linear(float c) { c = (std::max)(c, 0.0f); return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f); }
	// BounceLight.fx.hlsl's SpillBrightness on linearize(colour): luminance blended to the brightest channel by saturation.
	inline float SpillBrightness(const float* encoded) {
		const float c[3] = { Linear(encoded[0]), Linear(encoded[1]), Linear(encoded[2]) };
		const float peak = (std::max)(c[0], (std::max)(c[1], c[2]));
		const float saturation = 1.0f - (std::min)(c[0], (std::min)(c[1], c[2])) / (std::max)(peak, 1.0e-4f);
		const float luma = Luma(c);
		return luma + (peak - luma) * saturation;
	}

	inline bool KnownFormat(D3DFORMAT f) {
		return f == D3DFMT_A16B16G16R16F || f == D3DFMT_A32B32G32R32F || f == D3DFMT_A8R8G8B8 || f == D3DFMT_X8R8G8B8 ||
			f == D3DFMT_A2R10G10B10 || f == D3DFMT_A2B10G10R10;
	}

	// One row of a locked surface -> rgba floats.
	inline void ConvertRow(D3DFORMAT format, const unsigned char* line, unsigned width, float* row) {
		switch (format) {
		case D3DFMT_A16B16G16R16F: D3DXFloat16To32Array(row, (const D3DXFLOAT16*)line, width * 4); break;
		case D3DFMT_A32B32G32R32F: memcpy(row, line, (size_t)width * 16); break;
		case D3DFMT_A8R8G8B8: case D3DFMT_X8R8G8B8:
			for (unsigned x = 0; x < width; x++) {
				row[x * 4] = line[x * 4 + 2] / 255.0f; row[x * 4 + 1] = line[x * 4 + 1] / 255.0f; row[x * 4 + 2] = line[x * 4] / 255.0f; row[x * 4 + 3] = 1.0f;
			}
			break;
		default: {	// 10-bit
			const unsigned* p = (const unsigned*)line;
			const bool rFirst = format == D3DFMT_A2B10G10R10;
			for (unsigned x = 0; x < width; x++) {
				const float a = (p[x] & 1023) / 1023.0f, b = ((p[x] >> 10) & 1023) / 1023.0f, c = ((p[x] >> 20) & 1023) / 1023.0f;
				row[x * 4] = rFirst ? a : c; row[x * 4 + 1] = b; row[x * 4 + 2] = rFirst ? c : a; row[x * 4 + 3] = 1.0f;
			}
		}
		}
	}

	struct Reader {
		IDirect3DSurface9* resolve = nullptr;	// multisampled source: resolved here first (full size)
		IDirect3DSurface9* half = nullptr;		// GPU path: half and quarter size render targets
		IDirect3DSurface9* quarter = nullptr;
		IDirect3DSurface9* readback = nullptr;	// system memory: quarter size (GPU path) or full size (CPU path)
		D3DSURFACE_DESC desc = {};				// the source these were made for
		bool cpu = false;						// the CPU path (set if the GPU halving fails; forceCpu for the test)
		bool forceCpu = false;
		unsigned width = 0, height = 0;			// the picture read: source size / Scale
		const char* error = nullptr;			// why the last Read failed

		void Release() {
			if (resolve) { resolve->Release(); resolve = nullptr; }
			if (half) { half->Release(); half = nullptr; }
			if (quarter) { quarter->Release(); quarter = nullptr; }
			if (readback) { readback->Release(); readback = nullptr; }
			desc = {};
		}

		bool Make(IDirect3DDevice9* device, const D3DSURFACE_DESC& d) {
			Release();
			if (d.MultiSampleType != D3DMULTISAMPLE_NONE &&
				FAILED(device->CreateRenderTarget(d.Width, d.Height, d.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &resolve, NULL))) { error = "could not make the resolve target"; return false; }
			if (!cpu && (FAILED(device->CreateRenderTarget(d.Width / 2, d.Height / 2, d.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &half, NULL)) ||
				FAILED(device->CreateRenderTarget(d.Width / Scale, d.Height / Scale, d.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &quarter, NULL))))
				cpu = true;
			const unsigned rw = cpu ? d.Width : d.Width / Scale, rh = cpu ? d.Height : d.Height / Scale;
			if (FAILED(device->CreateOffscreenPlainSurface(rw, rh, d.Format, D3DPOOL_SYSTEMMEM, &readback, NULL))) { Release(); error = "could not make the readback surface"; return false; }
			desc = d;
			return true;
		}

		// The target's current picture into rgb (width x height x 3, quarter size). False (and error) if it cannot be read.
		bool Read(IDirect3DDevice9* device, IDirect3DSurface9* target, std::vector<float>& rgb) {
			error = nullptr;
			D3DSURFACE_DESC d = {};
			if (!target || FAILED(target->GetDesc(&d))) { error = "no target"; return false; }
			if (!KnownFormat(d.Format)) { error = "render target format not handled"; return false; }
			if (d.Width < Scale * 2 || d.Height < Scale * 2) { error = "render target too small"; return false; }
			if (forceCpu) cpu = true;
			if (!readback || d.Width != desc.Width || d.Height != desc.Height || d.Format != desc.Format || d.MultiSampleType != desc.MultiSampleType)
				if (!Make(device, d)) return false;
			IDirect3DSurface9* source = target;
			if (resolve) {
				if (FAILED(device->StretchRect(target, NULL, resolve, NULL, D3DTEXF_NONE))) { error = "resolve failed"; return false; }
				source = resolve;
			}
			if (!cpu) {
				if (FAILED(device->StretchRect(source, NULL, half, NULL, D3DTEXF_LINEAR)) || FAILED(device->StretchRect(half, NULL, quarter, NULL, D3DTEXF_LINEAR))) {
					cpu = true;	// this driver cannot filter this format: read it whole from now on
					if (!Make(device, d)) return false;
					if (resolve && FAILED(device->StretchRect(target, NULL, resolve, NULL, D3DTEXF_NONE))) { error = "resolve failed"; return false; }
				}
				else source = quarter;
			}
			if (FAILED(device->GetRenderTargetData(source, readback))) { error = "GetRenderTargetData failed"; return false; }
			D3DLOCKED_RECT lock;
			if (FAILED(readback->LockRect(&lock, NULL, D3DLOCK_READONLY))) { error = "lock failed"; return false; }
			width = d.Width / Scale; height = d.Height / Scale;
			rgb.assign((size_t)width * height * 3, 0.0f);
			const unsigned rowWidth = cpu ? d.Width : width;
			std::vector<float> row((size_t)rowWidth * 4);
			const unsigned rows = cpu ? height * Scale : height;
			const float inv = cpu ? 1.0f / (Scale * Scale) : 1.0f;
			for (unsigned y = 0; y < rows; y++) {
				ConvertRow(d.Format, (const unsigned char*)lock.pBits + (size_t)y * lock.Pitch, rowWidth, row.data());
				float* out = &rgb[(size_t)(cpu ? y / Scale : y) * width * 3];
				const unsigned columns = cpu ? width * Scale : width;
				for (unsigned x = 0; x < columns; x++) {
					float* o = out + (size_t)(cpu ? x / Scale : x) * 3;
					for (int c = 0; c < 3; c++) {
						const float v = row[(size_t)x * 4 + c];
						if (v == v && v < 65504.0f && v > -65504.0f) o[c] += v * inv;	// NaN / inf counted as 0
					}
				}
			}
			readback->UnlockRect();
			return true;
		}
	};

	// One block's frames at one point (a block = Settle frames not read, then Measure frames read, under one configuration).
	struct Series {
		unsigned frames = 0, width = 0, height = 0;
		std::vector<float> rgbSum, previous, change;

		void Reset() { frames = width = height = 0; for (auto* v : { &rgbSum, &previous, &change }) { v->clear(); v->shrink_to_fit(); } }

		// False if the size changed (the run must stop).
		bool Add(const std::vector<float>& rgb, unsigned w, unsigned h) {
			const size_t n = (size_t)w * h;
			if (!frames) { width = w; height = h; rgbSum.assign(n * 3, 0.0f); previous.resize(n); change.assign(n, 0.0f); }
			else if (w != width || h != height) return false;
			for (size_t i = 0; i < n * 3; i++) rgbSum[i] += rgb[i];
			for (size_t i = 0; i < n; i++) {
				const float l = Luma(&rgb[i * 3]);
				if (frames) change[i] += fabsf(l - previous[i]);
				previous[i] = l;
			}
			frames++;
			return true;
		}
	};

	// A block's means: picture (rgb), luma, and the mean frame-to-frame change of luma (the flicker), per pixel.
	struct BlockStats {
		unsigned frames = 0, width = 0, height = 0;
		std::vector<float> rgb, luma, flicker;
		void Clear() { frames = width = height = 0; for (auto* v : { &rgb, &luma, &flicker }) { v->clear(); v->shrink_to_fit(); } }
		void From(const Series& s) {
			frames = s.frames; width = s.width; height = s.height;
			const size_t n = (size_t)width * height;
			rgb.assign(n * 3, 0.0f); luma.assign(n, 0.0f); flicker.assign(n, 0.0f);
			if (!frames) return;
			const float inv = 1.0f / frames;
			for (size_t i = 0; i < n * 3; i++) rgb[i] = s.rgbSum[i] * inv;
			for (size_t i = 0; i < n; i++) { luma[i] = Luma(&rgb[i * 3]); flicker[i] = frames > 1 ? s.change[i] / (frames - 1) : 0.0f; }
		}
		bool Valid(unsigned w, unsigned h) const { return frames && width == w && height == h && luma.size() == (size_t)w * h; }
	};

	enum { Regions = Grid * Grid, MaxReferences = 2 * MaxConfigs + 2, MaxBlocks = 4 * MaxConfigs };

	// One comparison's sums per region (8 x 8), so its figures can be taken over all regions, or only over the regions that did
	// not change by themselves between the reference blocks (animated things, the sky).
	struct RegionSums {
		double luma0 = 0, luma1 = 0;				// all pixels: the reference, the configuration
		double lit = 0, b10 = 0, b25 = 0, b50 = 0, d10 = 0;	// counts among the reference's lit pixels
		double flicker = 0, refFlicker = 0, level = 0;	// lit pixels: the configuration's and the reference's flicker, the reference's brightness
		double flickering = 0, refFlickering = 0;	// lit pixels flickering (configuration, reference)
		double mask[3][5] = {};						// per mask class (0 none, 1 lights, 2 ring): count, luma0, luma1, saturation0, saturation1
	};

	struct Figures {
		double whole = 0;							// sum of luma / the reference's
		float brighter10 = 0, brighter25 = 0, brighter50 = 0, darker10 = 0;	// % of the reference's lit pixels
		float flicker = 0, refFlicker = 0;			// mean frame-to-frame change over the lit pixels, % of the REFERENCE's brightness
		float flickerPixels = 0, refFlickerPixels = 0;	// % of the lit pixels flickering (> 1% of the reference's brightness a frame; > 1/255 on a display picture)
		bool masks = false;
		float lightsShare = 0, ringShare = 0, lightsRatio = 0, lightsColour = 0, ringRatio = 0, ringColour = 0;
		int regions = 0;							// regions counted
	};

	struct Result {
		bool valid = false;
		unsigned frames = 0;
		bool moved = false;
		RegionSums region[Regions];
		Figures all, stable;						// over all regions; over the regions that did not change by themselves
		float ratio[Grid][Grid] = {};				// luma ratio per region
		int top[3] = { -1, -1, -1 };				// the three regions with the highest ratio (row * Grid + column)
	};

	// Everything kept per capture point. Blocks run reference, configuration 1, reference, configuration 2, ... reference; each
	// configuration is compared with the mean of the reference blocks just before and after it, so drift that is linear in
	// time (the time of day, a weather change) cancels.
	struct PointData {
		unsigned width = 0, height = 0;
		bool hdr = true;							// before tonemapping, or a display picture (flickering threshold 1/255)
		float lit = 0;								// "lit" = brighter than 5% of the first reference block's mean
		std::vector<float> refRgb, refLuma;			// the first reference block: the picture, the masks, the display scale
		std::vector<unsigned char> mask;			// 0 nothing, 1 a light (as the spill's light test counts it), 2 the ring round the lights
		bool masks = false;
		float threshold = 0;						// the light test used for the mask (linear)
		BlockStats previous, config;				// running: the last reference block; the configuration block waiting for the next one
		int references = 0;							// reference blocks finished
		double refRegion[MaxReferences][Regions] = {};	// each reference block's luma per region (noise floor, drift)
		Result results[MaxConfigs];					// [0]: the first reference block against itself
		std::vector<signed char> heat[MaxConfigs];	// per configuration: (ratio - 1) x 3 x 127, clamped (pictures)
		std::vector<unsigned char> flick[MaxConfigs];	// per configuration ([0] the first reference): flicker / 3% of the reference brightness x 255
		std::vector<unsigned char> refFlick[MaxConfigs];	// the same for the reference blocks round each configuration (intrusion check)
		Result firstTry[MaxConfigs];				// a configuration run again (critic-review-6): its first, disturbed result
		std::vector<signed char> drift;				// the last reference block against the first, as heat
		bool excluded[Regions] = {};				// regions that changed by themselves (reference to reference outside 1 +- 0.05)
		int excludedCount = 0;

		void Reset() {
			width = height = 0; hdr = true; lit = 0; masks = false; threshold = 0; references = 0; excludedCount = 0;
			for (auto* v : { &refRgb, &refLuma }) { v->clear(); v->shrink_to_fit(); }
			mask.clear(); mask.shrink_to_fit(); drift.clear(); drift.shrink_to_fit();
			previous.Clear(); config.Clear();
			memset(refRegion, 0, sizeof(refRegion));
			for (int c = 0; c < MaxConfigs; c++) {
				results[c] = firstTry[c] = Result();
				for (auto* v : { &flick[c], &refFlick[c] }) { v->clear(); v->shrink_to_fit(); }
				heat[c].clear(); heat[c].shrink_to_fit();
			}
			for (bool& e : excluded) e = false;
		}
		int RegionOf(unsigned x, unsigned y) const { return (int)(y * Grid / height) * Grid + (int)(x * Grid / width); }
	};

	inline signed char HeatCode(float ratio) { const float s = (std::max)(-1.0f, (std::min)(1.0f, (ratio - 1.0f) * 3.0f)); return (signed char)(s * 127.0f); }

	// The light pixels (SpillBrightness of the first reference block above threshold) and the ring round them (within RingRadius
	// pixels, lights excluded).
	inline void BuildMasks(PointData& p, float threshold) {
		const unsigned w = p.width, h = p.height;
		if (p.refRgb.size() != (size_t)w * h * 3 || threshold <= 0.0f) { p.masks = false; return; }
		p.threshold = threshold;
		std::vector<unsigned char> lights((size_t)w * h), rows((size_t)w * h);
		for (size_t i = 0; i < lights.size(); i++) lights[i] = SpillBrightness(&p.refRgb[i * 3]) > threshold;
		for (unsigned y = 0; y < h; y++)	// dilate: rows, then columns (a square of side 2 RingRadius + 1)
			for (unsigned x = 0; x < w; x++) {
				unsigned char any = 0;
				for (int k = -RingRadius; k <= RingRadius && !any; k++) { const int xx = (int)x + k; if (xx >= 0 && xx < (int)w) any = lights[(size_t)y * w + xx]; }
				rows[(size_t)y * w + x] = any;
			}
		p.mask.assign((size_t)w * h, 0);
		for (unsigned y = 0; y < h; y++)
			for (unsigned x = 0; x < w; x++) {
				unsigned char any = 0;
				for (int k = -RingRadius; k <= RingRadius && !any; k++) { const int yy = (int)y + k; if (yy >= 0 && yy < (int)h) any = rows[(size_t)yy * w + x]; }
				const size_t i = (size_t)y * w + x;
				p.mask[i] = lights[i] ? 1 : any ? 2 : 0;
			}
		p.masks = true;
	}

	// The figures of a comparison over all regions, or without the excluded ones. O(regions).
	inline Figures Sum(const PointData& p, const Result& r, bool stableOnly) {
		Figures f;
		RegionSums s;
		for (int g = 0; g < Regions; g++) {
			if (stableOnly && p.excluded[g]) continue;
			const RegionSums& q = r.region[g];
			f.regions++;
			s.luma0 += q.luma0; s.luma1 += q.luma1; s.lit += q.lit; s.b10 += q.b10; s.b25 += q.b25; s.b50 += q.b50; s.d10 += q.d10;
			s.flicker += q.flicker; s.refFlicker += q.refFlicker; s.level += q.level; s.flickering += q.flickering; s.refFlickering += q.refFlickering;
			for (int m = 0; m < 3; m++) for (int k = 0; k < 5; k++) s.mask[m][k] += q.mask[m][k];
		}
		const double share = s.lit > 0 ? 100.0 / s.lit : 0.0;
		f.whole = s.luma0 > 0 ? s.luma1 / s.luma0 : 0.0;
		f.brighter10 = (float)(s.b10 * share); f.brighter25 = (float)(s.b25 * share); f.brighter50 = (float)(s.b50 * share); f.darker10 = (float)(s.d10 * share);
		f.flicker = s.level > 0 ? (float)(100.0 * s.flicker / s.level) : 0.0f;
		f.refFlicker = s.level > 0 ? (float)(100.0 * s.refFlicker / s.level) : 0.0f;
		f.flickerPixels = (float)(s.flickering * share); f.refFlickerPixels = (float)(s.refFlickering * share);
		const double pixels = s.mask[0][0] + s.mask[1][0] + s.mask[2][0];
		if (p.masks && pixels > 0) {
			f.masks = true;
			f.lightsShare = (float)(100.0 * s.mask[1][0] / pixels); f.ringShare = (float)(100.0 * s.mask[2][0] / pixels);
			f.lightsRatio = s.mask[1][1] > 0 ? (float)(s.mask[1][2] / s.mask[1][1]) : 0.0f;
			f.ringRatio = s.mask[2][1] > 0 ? (float)(s.mask[2][2] / s.mask[2][1]) : 0.0f;
			f.lightsColour = s.mask[1][3] > 0 ? (float)(s.mask[1][4] / s.mask[1][3]) : 0.0f;
			f.ringColour = s.mask[2][3] > 0 ? (float)(s.mask[2][4] / s.mask[2][3]) : 0.0f;
		}
		return f;
	}

	// A configuration block against the mean of two reference blocks (a == b for the first reference against itself). Writes
	// the configuration's picture maps (heat, flicker). Every per-picture value is taken before the pixel loop. O(pixels).
	inline Result Compare(PointData& p, const BlockStats& a, const BlockStats& b, const BlockStats& c, int config) {
		Result r;
		const unsigned w = p.width, h = p.height;
		const size_t n = (size_t)w * h;
		if (!a.Valid(w, h) || !b.Valid(w, h) || !c.Valid(w, h) || config < 0 || config >= MaxConfigs) return r;
		r.valid = true;
		r.frames = c.frames;
		const float lit = p.lit;
		const bool masks = p.masks && p.mask.size() == n;
		if (config) p.heat[config].assign(n, 0);
		p.flick[config].assign(n, 0);
		p.refFlick[config].assign(n, 0);
		for (unsigned y = 0; y < h; y++)
			for (unsigned x = 0; x < w; x++) {
				const size_t i = (size_t)y * w + x;
				RegionSums& q = r.region[p.RegionOf(x, y)];
				const float l0 = 0.5f * (a.luma[i] + b.luma[i]), l1 = c.luma[i];
				q.luma0 += l0; q.luma1 += l1;
				if (masks) {
					float rgb0[3];
					for (int k = 0; k < 3; k++) rgb0[k] = 0.5f * (a.rgb[i * 3 + k] + b.rgb[i * 3 + k]);
					double* m = q.mask[p.mask[i]];
					m[0] += 1; m[1] += l0; m[2] += l1; m[3] += Saturation(rgb0); m[4] += Saturation(&c.rgb[i * 3]);
				}
				const float level = (std::max)(l0, lit);	// the REFERENCE's brightness: added steady light is not "less flicker"
				const float refFlicker = 0.5f * (a.flicker[i] + b.flicker[i]);
				const float scale = 255.0f / (p.hdr ? 0.03f * level : 3.0f / 255.0f);
				p.flick[config][i] = (unsigned char)(std::min)(255.0f, c.flicker[i] * scale);
				p.refFlick[config][i] = (unsigned char)(std::min)(255.0f, refFlicker * scale);
				if (config) p.heat[config][i] = (l0 > lit || l1 > lit) ? HeatCode(l1 / (std::max)(l0, 1.0e-6f)) : 0;
				if (l0 <= lit) continue;
				q.lit += 1;
				const float ratio = l1 / l0;
				if (ratio > 1.1f) q.b10 += 1;
				if (ratio > 1.25f) q.b25 += 1;
				if (ratio > 1.5f) q.b50 += 1;
				if (ratio < 0.9f) q.d10 += 1;
				q.flicker += c.flicker[i]; q.refFlicker += refFlicker; q.level += level;
				const float limit = p.hdr ? 0.01f * level : 1.0f / 255.0f;
				if (c.flicker[i] > limit) q.flickering += 1;
				if (refFlicker > limit) q.refFlickering += 1;
			}
		for (int g = 0; g < Regions; g++) r.ratio[g / Grid][g % Grid] = r.region[g].luma0 > 0 ? (float)(r.region[g].luma1 / r.region[g].luma0) : 0.0f;
		for (int k = 0; k < 3; k++) {
			int best = -1;
			for (int g = 0; g < Regions; g++) {
				if (g == r.top[0] || g == r.top[1]) continue;
				if (best < 0 || r.ratio[g / Grid][g % Grid] > r.ratio[best / Grid][best % Grid]) best = g;
			}
			r.top[k] = best;
		}
		return r;
	}

	inline void AddReferenceRegions(PointData& p, const BlockStats& s) {
		if (p.references >= MaxReferences || !s.Valid(p.width, p.height)) return;
		double* region = p.refRegion[p.references];
		for (unsigned y = 0; y < p.height; y++)
			for (unsigned x = 0; x < p.width; x++) region[p.RegionOf(x, y)] += s.luma[(size_t)y * p.width + x];
		p.references++;
	}

	// The first reference block: the picture, the "lit" level, its own flicker (configuration 0's result, masks added later).
	inline bool FirstReference(PointData& p, BlockStats&& s, bool hdr) {
		if (!s.frames) return false;
		p.width = s.width; p.height = s.height; p.hdr = hdr;
		p.refRgb = s.rgb; p.refLuma = s.luma;
		double total = 0;
		for (float l : s.luma) total += l;
		p.lit = (std::max)(1.0e-4f, (float)(total / s.luma.size()) * 0.05f);
		p.results[0] = Compare(p, s, s, s, 0);
		AddReferenceRegions(p, s);
		p.previous = std::move(s);
		return true;
	}

	// A configuration block: kept until the next reference block.
	inline void ConfigurationBlock(PointData& p, BlockStats&& s) { p.config = std::move(s); }

	// A later reference block: the waiting configuration against it and the one before; then it is the one before.
	// countRegions false: a reference of another kind (the bloom-off references) stays out of the noise floor and the drift.
	inline void NextReference(PointData& p, BlockStats&& s, int config, bool countRegions = true) {
		if (p.config.Valid(p.width, p.height)) p.results[config] = Compare(p, p.previous, s, p.config, config);
		if (countRegions) AddReferenceRegions(p, s);
		p.config.Clear();
		p.previous = std::move(s);
	}

	// The end of the run: the regions that changed by themselves (any pair of reference blocks in a row outside 1 +- 0.05, in
	// regions with light), the drift picture (last reference against the first), the first reference's mask figures, and every
	// result's figures over all and over the steady regions.
	inline void FinishRun(PointData& p) {
		const size_t n = (size_t)p.width * p.height;
		p.excludedCount = 0;
		const double pixelsPerRegion = (double)n / Regions;
		for (int g = 0; g < Regions; g++) {
			p.excluded[g] = false;
			for (int k = 0; k + 1 < p.references; k++) {
				const double a = p.refRegion[k][g], b = p.refRegion[k + 1][g];
				if (a < p.lit * pixelsPerRegion * 0.5 && b < p.lit * pixelsPerRegion * 0.5) continue;	// a dark region: its ratio means nothing
				const double ratio = a > 0 ? b / a : 2.0;
				if (ratio < 0.95 || ratio > 1.05) { p.excluded[g] = true; break; }
			}
			if (p.excluded[g]) p.excludedCount++;
		}
		if (p.previous.Valid(p.width, p.height) && p.refLuma.size() == n) {
			p.drift.assign(n, 0);
			for (size_t i = 0; i < n; i++)
				if (p.refLuma[i] > p.lit || p.previous.luma[i] > p.lit) p.drift[i] = HeatCode(p.previous.luma[i] / (std::max)(p.refLuma[i], 1.0e-6f));
		}
		if (p.masks && p.mask.size() == n && p.refRgb.size() == n * 3) {	// the first reference against itself
			Result& r = p.results[0];
			for (unsigned y = 0; y < p.height; y++)
				for (unsigned x = 0; x < p.width; x++) {
					const size_t i = (size_t)y * p.width + x;
					double* m = r.region[p.RegionOf(x, y)].mask[p.mask[i]];
					const float s = Saturation(&p.refRgb[i * 3]);
					m[0] += 1; m[1] += p.refLuma[i]; m[2] += p.refLuma[i]; m[3] += s; m[4] += s;
				}
		}
		for (int c = 0; c < MaxConfigs; c++) {
			Result& r = p.results[c];
			if (!r.valid) continue;
			r.all = Sum(p, r, false);
			r.stable = Sum(p, r, true);
		}
		p.previous.Clear(); p.config.Clear();
	}

	// The noise floor: an inner reference block against the mean of its neighbours (whole picture, or the steady regions).
	inline double ReferenceRatio(const PointData& p, int k, bool stableOnly) {
		if (k < 1 || k + 1 >= p.references) return 0.0;
		double own = 0, around = 0;
		for (int g = 0; g < Regions; g++) {
			if (stableOnly && p.excluded[g]) continue;
			own += p.refRegion[k][g]; around += 0.5 * (p.refRegion[k - 1][g] + p.refRegion[k + 1][g]);
		}
		return around > 0 ? own / around : 0.0;
	}

	// Was a configuration block disturbed (someone walking past, critic-review-6)? Over all regions: more than 3% of the lit
	// pixels 10%+ darker than the references round it (the spill can only darken through round 5's light-pixel mask, a few
	// tenths of a percent), or, in the pixels the spill does not light (less than 2% added in every other configuration), more
	// than twice the flicker of the references round it. Uses the picture maps; O(pixels x configurations).
	struct Intrusion { bool valid = false, flagged = false; float darker = 0, flickerRatio = 0; double unlitShare = 0; };
	inline Intrusion CheckIntrusion(const PointData& p, int config, int configs) {
		Intrusion in;
		const size_t n = (size_t)p.width * p.height;
		const Result& r = p.results[config];
		if (!r.valid || config <= 0 || config >= MaxConfigs || p.flick[config].size() != n || p.refFlick[config].size() != n) return in;
		in.valid = true;
		in.darker = Sum(p, r, false).darker10;
		const signed char lit2 = (signed char)(0.02f * 3.0f * 127.0f + 0.5f);	// HeatCode of a ratio of 1.02
		double flicker = 0, reference = 0;
		size_t count = 0;
		for (size_t i = 0; i < n; i++) {
			bool unlit = true;
			for (int c = 1; c < configs && unlit; c++)
				if (c != config && p.heat[c].size() == n && p.heat[c][i] >= lit2) unlit = false;
			if (!unlit) continue;
			flicker += p.flick[config][i]; reference += p.refFlick[config][i]; count++;
		}
		in.unlitShare = 100.0 * count / n;
		in.flickerRatio = (float)((flicker + count) / (reference + count));	// +1 code a pixel: a steady reference does not divide by 0
		in.flagged = in.darker > 3.0f || (count > n / 50 && in.flickerRatio > 2.0f);
		return in;
	}

	// The noise of the run: the largest steady-region deviation of an inner reference block from the mean of its neighbours.
	inline double NoiseLevel(const PointData& p) {
		double worst = 0.0;
		for (int k = 1; k + 1 < p.references; k++) { const double r = ReferenceRatio(p, k, true); if (r > 0) worst = (std::max)(worst, fabs(r - 1.0)); }
		return worst;
	}

	// ---- pictures ----
	inline unsigned Byte(float v) { return (unsigned)((std::max)(0.0f, (std::min)(1.0f, v)) * 255.0f + 0.5f); }

	// The reference picture as a viewable grey/colour value: before tonemapping squeezed round its average, else as it is.
	struct Display {
		float exposure = 1.0f; bool hdr = true;
		void Map(const float* c, float* out) const {
			for (int k = 0; k < 3; k++) {
				const float v = (std::max)(0.0f, c[k]);
				out[k] = hdr ? (v * exposure) / (1.0f + v * exposure) : v;
			}
		}
	};

	inline Display DisplayFor(const PointData& p) {
		Display d; d.hdr = p.hdr;
		double total = 0;
		for (float l : p.refLuma) total += l;
		const float mean = p.refLuma.empty() ? 0.0f : (float)(total / p.refLuma.size());
		d.exposure = mean > 0 ? 0.5f / mean : 1.0f;
		return d;
	}

	// A cols x rows sheet of width x height tiles as 32-bit BGRA (opaque), tile(index, x, y, rgb out) per pixel. Plain CPU work,
	// no device: it runs on a worker thread in the game (EffectSelfTest.h). Stops (false) if it takes longer than limitSeconds.
	// (2026-10-12 hang: the light sheet called PointData::LitThreshold(), a loop over the whole picture, for every pixel:
	// 7 tiles x 230,400 pixels x 2 x 230,400 at 2560x1440, minutes on the render thread. Every per-picture value is now taken
	// once, before the pixel loops; the per-pixel maps are made during the run.)
	template <class Tile>
	inline bool BuildSheet(std::vector<unsigned>& out, unsigned width, unsigned height, unsigned cols, unsigned rows, double limitSeconds, Tile tile) {
		out.assign((size_t)width * cols * height * rows, 0xFF000000u);
		const ULONGLONG start = GetTickCount64();
		for (unsigned t = 0; t < cols * rows; t++) {
			const unsigned ox = (t % cols) * width, oy = (t / cols) * height;
			for (unsigned y = 0; y < height; y++) {
				if ((GetTickCount64() - start) > (ULONGLONG)(limitSeconds * 1000.0)) return false;
				unsigned* line = &out[(size_t)(oy + y) * width * cols + ox];
				for (unsigned x = 0; x < width; x++) {
					float c[3] = { 0, 0, 0 };
					tile(t, x, y, c);
					line[x] = 0xFF000000u | (Byte(c[0]) << 16) | (Byte(c[1]) << 8) | Byte(c[2]);
				}
			}
		}
		return true;
	}

	// A BGRA picture to a PNG file with the Windows Imaging Component (no device, any thread). False if not written.
	inline bool SavePng(const char* path, unsigned width, unsigned height, const std::vector<unsigned>& bgra) {
		if (bgra.size() != (size_t)width * height) return false;
		const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
		IWICImagingFactory* factory = nullptr; IWICStream* stream = nullptr; IWICBitmapEncoder* encoder = nullptr; IWICBitmapFrameEncode* frame = nullptr;
		wchar_t wide[MAX_PATH] = L"";
		MultiByteToWideChar(CP_ACP, 0, path, -1, wide, MAX_PATH);
		bool ok = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) &&
			SUCCEEDED(factory->CreateStream(&stream)) && SUCCEEDED(stream->InitializeFromFilename(wide, GENERIC_WRITE)) &&
			SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) && SUCCEEDED(encoder->Initialize(stream, WICBitmapEncoderNoCache)) &&
			SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) && SUCCEEDED(frame->Initialize(nullptr)) && SUCCEEDED(frame->SetSize(width, height));
		if (ok) {
			WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
			ok = SUCCEEDED(frame->SetPixelFormat(&format)) && IsEqualGUID(format, GUID_WICPixelFormat32bppBGRA) &&
				SUCCEEDED(frame->WritePixels(height, width * 4, width * height * 4, (BYTE*)bgra.data())) && SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
		}
		if (frame) frame->Release();
		if (encoder) encoder->Release();
		if (stream) stream->Release();
		if (factory) factory->Release();
		if (SUCCEEDED(init)) CoUninitialize();
		return ok;
	}

	inline bool SheetData(const PointData& p) {
		return p.width && p.height && p.refRgb.size() == (size_t)p.width * p.height * 3 && p.refLuma.size() == (size_t)p.width * p.height;
	}
	inline void HeatColour(signed char code, float grey, float* c) {
		const float s = fabsf(code / 127.0f);
		if (code < 0) { c[0] = grey * (1 - s); c[1] = grey * (1 - s); c[2] = grey * (1 - s) + s; }
		else { c[0] = grey * (1 - s) + s; c[1] = grey * (1 - s); c[2] = grey * (1 - s); }
	}
	// The regions left out of the summary, as a dark red edge round each (used on the last tile).
	inline bool ExcludedEdge(const PointData& p, unsigned x, unsigned y) {
		const int g = p.RegionOf(x, y);
		if (!p.excluded[g]) return false;
		const unsigned gx = g % Grid, gy = g / Grid;
		const unsigned x0 = gx * p.width / Grid, x1 = (gx + 1) * p.width / Grid - 1, y0 = gy * p.height / Grid, y1 = (gy + 1) * p.height / Grid - 1;
		return x < x0 + 2 || x + 2 > x1 || y < y0 + 2 || y + 2 > y1;
	}

	// 4 x 4 tiles: the first reference picture; configurations 1..configs-1 against their references (blue darker, red brighter,
	// full colour = 33%); the last reference against the first (what changed by itself); the light pixels red and the ring round
	// them yellow, with the regions left out of the summary edged in dark red.
	inline bool BuildLightSheet(std::vector<unsigned>& out, const PointData& p, int configs, double limitSeconds) {
		if (!SheetData(p)) return false;
		const Display display = DisplayFor(p);
		const size_t n = (size_t)p.width * p.height;
		return BuildSheet(out, p.width, p.height, 4, 4, limitSeconds, [&](unsigned t, unsigned x, unsigned y, float* c) {
			const size_t i = (size_t)y * p.width + x;
			float picture[3];
			display.Map(&p.refRgb[i * 3], picture);
			const float grey = Luma(picture) * 0.6f;
			if (t == 0) { c[0] = picture[0]; c[1] = picture[1]; c[2] = picture[2]; return; }
			c[0] = c[1] = c[2] = grey;
			const int last = configs;			// the drift tile, then the mask tile
			if ((int)t < configs) { if (p.heat[t].size() == n) HeatColour(p.heat[t][i], grey, c); return; }
			if ((int)t == last) { if (p.drift.size() == n) HeatColour(p.drift[i], grey, c); return; }
			if ((int)t == last + 1) {
				if (p.masks && p.mask.size() == n) {
					if (p.mask[i] == 1) { c[0] = 1.0f; c[1] = 0.1f; c[2] = 0.1f; }
					else if (p.mask[i] == 2) { c[0] = 0.5f + grey; c[1] = 0.45f + grey; c[2] = 0.0f; }
				}
				if (ExcludedEdge(p, x, y)) { c[0] = 0.6f; c[1] = 0.0f; c[2] = 0.0f; }
			}
		});
	}

	// 4 x 4 tiles: the first reference picture, then the flicker of configurations 0..configs-1 (black steady, yellow = 3% of the
	// reference's brightness a frame before tonemapping, 3/255 a frame on a display picture), then the regions left out.
	inline bool BuildFlickerSheet(std::vector<unsigned>& out, const PointData& p, int configs, double limitSeconds) {
		if (!SheetData(p)) return false;
		const Display display = DisplayFor(p);
		const size_t n = (size_t)p.width * p.height;
		return BuildSheet(out, p.width, p.height, 4, 4, limitSeconds, [&](unsigned t, unsigned x, unsigned y, float* c) {
			const size_t i = (size_t)y * p.width + x;
			float picture[3];
			display.Map(&p.refRgb[i * 3], picture);
			if (t == 0) { c[0] = picture[0]; c[1] = picture[1]; c[2] = picture[2]; return; }
			const float grey = Luma(picture) * 0.35f;
			c[0] = c[1] = c[2] = grey;
			const int config = (int)t - 1;
			if (config < configs) {
				if (p.flick[config].size() != n) return;
				const float s = p.flick[config][i] / 255.0f;
				c[0] = grey * (1 - s) + s; c[1] = grey * (1 - s) + s * 0.85f; c[2] = grey * (1 - s);
			}
			else if (config == configs && ExcludedEdge(p, x, y)) { c[0] = 0.6f; c[1] = 0.0f; c[2] = 0.0f; }
		});
	}

	inline bool SaveLightSheet(const char* path, const PointData& p, int configs, double limitSeconds = 20.0) {
		std::vector<unsigned> sheet;
		return BuildLightSheet(sheet, p, configs, limitSeconds) && SavePng(path, p.width * 4, p.height * 4, sheet);
	}
	inline bool SaveFlickerSheet(const char* path, const PointData& p, int configs, double limitSeconds = 20.0) {
		std::vector<unsigned> sheet;
		return BuildFlickerSheet(sheet, p, configs, limitSeconds) && SavePng(path, p.width * 4, p.height * 4, sheet);
	}

	// The run: blocks of Settle frames (not read: the last block's bounce history and spill memory drain) then Measure frames (read
	// at every point), each under one configuration (0 = the reference, spill off). Standard(): reference, 1, reference, 2, ...,
	// reference; Add() appends blocks (the re-runs of disturbed configurations: configuration, reference).
	struct Schedule {
		int settle = 0, measure = 0, step = 0, count = 0;
		int configs[MaxBlocks] = {};
		void Standard(int configCount) { count = 0; step = 0; for (int c = 1; c < configCount; c++) { Add(0); Add(c); } Add(0); }
		void Add(int config) { if (count < MaxBlocks) configs[count++] = config; }
		int Blocks() const { return count; }
		int PerBlock() const { return settle + measure; }
		int Block() const { return step / PerBlock(); }
		int ConfigOf(int block) const { return block >= 0 && block < count ? configs[block] : 0; }
		int Config() const { return ConfigOf(Block()); }
		bool Measuring() const { return step % PerBlock() >= settle; }
		bool FirstMeasure() const { return step % PerBlock() == settle; }
		bool LastMeasure() const { return step % PerBlock() == PerBlock() - 1; }
		bool Done() const { return step >= count * PerBlock(); }
	};

	// ---- critic-review-14 part 1: full-size shots, and the overrides they need ----

	// The Bounce Light members a configuration may set: one field per member, read and written only through Read / Write (new
	// keys: add the field, its line in Read, Write and EffectSelfTest.h's ReadDefaults, and the configurations that use it).
	struct BounceValues {
		float neonSpill = 0, spillReach = 0, spillLightLevel = 0, spillNear = 0, spillFar = 0, spillCap = 0, spillMemorySeconds = 0;
		// round 6 (WORKER-B, critic-review-9): 0 = off, as round 5
		float spillSeparateDepth = 0, spillHardEdges = 0, spillHoldAfterTurn = 0, spillCapSceneLight = 0, spillMaskSpillOnly = 0;
		int spillDebug = 0;
		bool operator==(const BounceValues& o) const {
			return neonSpill == o.neonSpill && spillReach == o.spillReach && spillLightLevel == o.spillLightLevel && spillNear == o.spillNear &&
				spillFar == o.spillFar && spillCap == o.spillCap && spillMemorySeconds == o.spillMemorySeconds && spillDebug == o.spillDebug &&
				spillSeparateDepth == o.spillSeparateDepth && spillHardEdges == o.spillHardEdges && spillHoldAfterTurn == o.spillHoldAfterTurn &&
				spillCapSceneLight == o.spillCapSceneLight && spillMaskSpillOnly == o.spillMaskSpillOnly;
		}
	};
	template <class Effect> inline void Read(const Effect* b, BounceValues& v) {
		v.neonSpill = b->neonSpill; v.spillReach = b->spillReach; v.spillLightLevel = b->spillLightLevel; v.spillNear = b->spillNear;
		v.spillFar = b->spillFar; v.spillCap = b->spillCap; v.spillMemorySeconds = b->spillMemorySeconds; v.spillDebug = b->spillDebug;
		v.spillSeparateDepth = b->spillSeparateDepth; v.spillHardEdges = b->spillHardEdges; v.spillHoldAfterTurn = b->spillHoldAfterTurn;
		v.spillCapSceneLight = b->spillCapSceneLight; v.spillMaskSpillOnly = b->spillMaskSpillOnly;
	}
	template <class Effect> inline void Write(Effect* b, const BounceValues& v) {
		b->neonSpill = v.neonSpill; b->spillReach = v.spillReach; b->spillLightLevel = v.spillLightLevel; b->spillNear = v.spillNear;
		b->spillFar = v.spillFar; b->spillCap = v.spillCap; b->spillMemorySeconds = v.spillMemorySeconds; b->spillDebug = v.spillDebug;
		b->spillSeparateDepth = v.spillSeparateDepth; b->spillHardEdges = v.spillHardEdges; b->spillHoldAfterTurn = v.spillHoldAfterTurn;
		b->spillCapSceneLight = v.spillCapSceneLight; b->spillMaskSpillOnly = v.spillMaskSpillOnly;
	}

	// Values set for one frame (shader constants the self-test does not own: TESR_HDRBloomData.x, TESR_HDRData.x, TESR_ToneMapping.x,
	// TESR_LotteData): Set keeps the frame's value and writes the test's; Restore puts them back in reverse order, so a value set
	// twice comes back as it was before the first Set. A plain copy back: bit for bit.
	struct FrameOverride {
		struct Slot { float* where; float saved; };
		Slot slots[16] = {};
		int count = 0;
		bool Set(float* where, float value) {
			if (!where || count >= 16) return false;
			slots[count].where = where; slots[count].saved = *where; count++;
			*where = value;
			return true;
		}
		void Restore() { while (count > 0) { count--; *slots[count].where = slots[count].saved; } }
		bool Active() const { return count > 0; }
	};

	// The picture as shown, at full size, as 8-bit BGRA for a PNG: a multisampled target is resolved first (as Reader does); 8-bit
	// and 10-bit formats are taken as they are, float formats clamped to 0-1 (the final picture is display-ready). The readback is
	// one SYSTEMMEM surface, made again only when the size or format changes.
	struct ShotReader {
		IDirect3DSurface9* resolve = nullptr;
		IDirect3DSurface9* readback = nullptr;
		D3DSURFACE_DESC desc = {};
		const char* error = nullptr;
		void Release() {
			if (resolve) { resolve->Release(); resolve = nullptr; }
			if (readback) { readback->Release(); readback = nullptr; }
			desc = {};
		}
		bool Read(IDirect3DDevice9* device, IDirect3DSurface9* target, std::vector<unsigned>& bgra, unsigned& width, unsigned& height) {
			error = nullptr;
			D3DSURFACE_DESC d = {};
			if (!target || FAILED(target->GetDesc(&d))) { error = "no target"; return false; }
			if (!KnownFormat(d.Format)) { error = "render target format not handled"; return false; }
			if (!readback || d.Width != desc.Width || d.Height != desc.Height || d.Format != desc.Format || d.MultiSampleType != desc.MultiSampleType) {
				Release();
				if (d.MultiSampleType != D3DMULTISAMPLE_NONE &&
					FAILED(device->CreateRenderTarget(d.Width, d.Height, d.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &resolve, NULL))) { error = "could not make the resolve target"; return false; }
				if (FAILED(device->CreateOffscreenPlainSurface(d.Width, d.Height, d.Format, D3DPOOL_SYSTEMMEM, &readback, NULL))) { Release(); error = "could not make the readback surface"; return false; }
				desc = d;
			}
			IDirect3DSurface9* source = target;
			if (resolve) {
				if (FAILED(device->StretchRect(target, NULL, resolve, NULL, D3DTEXF_NONE))) { error = "resolve failed"; return false; }
				source = resolve;
			}
			if (FAILED(device->GetRenderTargetData(source, readback))) { error = "GetRenderTargetData failed"; return false; }
			D3DLOCKED_RECT lock;
			if (FAILED(readback->LockRect(&lock, NULL, D3DLOCK_READONLY))) { error = "lock failed"; return false; }
			width = d.Width; height = d.Height;
			bgra.resize((size_t)width * height);
			const bool eightBit = d.Format == D3DFMT_A8R8G8B8 || d.Format == D3DFMT_X8R8G8B8;
			std::vector<float> row(eightBit ? 0 : (size_t)width * 4);
			for (unsigned y = 0; y < height; y++) {
				const unsigned char* line = (const unsigned char*)lock.pBits + (size_t)y * lock.Pitch;
				unsigned* out = &bgra[(size_t)y * width];
				if (eightBit) { memcpy(out, line, (size_t)width * 4); for (unsigned x = 0; x < width; x++) out[x] |= 0xFF000000u; continue; }
				ConvertRow(d.Format, line, width, row.data());
				for (unsigned x = 0; x < width; x++) out[x] = 0xFF000000u | (Byte(row[x * 4]) << 16) | (Byte(row[x * 4 + 1]) << 8) | Byte(row[x * 4 + 2]);
			}
			readback->UnlockRect();
			return true;
		}
	};
	inline const char* FormatName(D3DFORMAT f) {
		switch (f) {
		case D3DFMT_A8R8G8B8: return "A8R8G8B8"; case D3DFMT_X8R8G8B8: return "X8R8G8B8"; case D3DFMT_A2R10G10B10: return "A2R10G10B10";
		case D3DFMT_A2B10G10R10: return "A2B10G10R10"; case D3DFMT_A16B16G16R16F: return "A16B16G16R16F"; case D3DFMT_A32B32G32R32F: return "A32B32G32R32F";
		default: return "other";
		}
	}
}
