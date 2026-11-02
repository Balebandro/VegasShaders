// UNOFFICIAL: builds the noise textures of the volumetric clouds (Effects\VolumetricClouds.fx.hlsl) as DDS files, so
// they are made the same way every time instead of drawn by hand. All three tile in every direction.
//   CloudShape.dds    128^3 L8 volume, full mip chain: the clouds' big shapes. Perlin-Worley noise (Perlin fbm lifted
//                     where Worley cells are bright, so it reads as billows) eroded by a finer Worley fbm, as in
//                     "The real-time volumetric cloudscapes of Horizon Zero Dawn" (Schneider, SIGGRAPH 2015).
//   CloudDetail.dds   32^3 L8 volume, full mip chain: a Worley fbm that wears away the shapes' edges into wisps.
//   CloudWeather.dds  512^2 A8R8G8B8, full mip chain: r where clouds may form (thresholded by the weather's coverage),
//                     g cloud type (0 flat layers .. 1 tall heaps), b local density.
//   NoiseVolume.dds   64^3 A8R8G8B8, full mip chain: general smooth noise for the fog and the gun smoke. r one octave of
//                     gradient noise (8 cells across the tile, like a value noise of period 1 scaled by 1/8), g four
//                     octaves of it (fbm), b a Worley fbm (billows), a another single octave, independent of r. Each is
//                     stretched so 1 % .. 99 % of its values span 0 .. 1 (mean about 0.5), as the shaders' old noise did.
// Usage: build\cloud-noise\cloud_noise.exe <output folder>   (writes the three files into it)
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

static uint32_t Hash(uint32_t x, uint32_t y, uint32_t z, uint32_t seed) {
	uint32_t h = seed * 0x9E3779B9u + 0x7F4A7C15u;
	h ^= x * 0x85EBCA6Bu; h = (h << 13) | (h >> 19); h *= 5u; h += 0xE6546B64u;
	h ^= y * 0xC2B2AE35u; h = (h << 13) | (h >> 19); h *= 5u; h += 0xE6546B64u;
	h ^= z * 0x27D4EB2Fu; h = (h << 13) | (h >> 19); h *= 5u; h += 0xE6546B64u;
	h ^= h >> 16; h *= 0x85EBCA6Bu; h ^= h >> 13; h *= 0xC2B2AE35u; h ^= h >> 16;
	return h;
}
static float Unit(uint32_t h) { return (h & 0xFFFFFFu) / 16777216.0f; }
static int Wrap(int i, int period) { return ((i % period) + period) % period; }
static float Saturate(float v) { return std::min(std::max(v, 0.0f), 1.0f); }
static float Remap(float v, float lo, float hi, float newLo, float newHi) {
	return newLo + (v - lo) / std::max(hi - lo, 1e-6f) * (newHi - newLo);
}

// Distance to the nearest feature point (one per cell, period cells across the tile), in cells; 0 .. about 0.9.
static float Worley(float x, float y, float z, int period, uint32_t seed) {
	const float px = x * period, py = y * period, pz = z * period;
	const int cx = (int)std::floor(px), cy = (int)std::floor(py), cz = (int)std::floor(pz);
	float best = 1e9f;
	for (int dz = -1; dz <= 1; dz++) for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++) {
		const int nx = cx + dx, ny = cy + dy, nz = cz + dz;
		const uint32_t wx = Wrap(nx, period), wy = Wrap(ny, period), wz = Wrap(nz, period);
		const float fx = nx + Unit(Hash(wx, wy, wz, seed)) - px;
		const float fy = ny + Unit(Hash(wx, wy, wz, seed + 101)) - py;
		const float fz = nz + Unit(Hash(wx, wy, wz, seed + 202)) - pz;
		best = std::min(best, fx * fx + fy * fy + fz * fz);
	}
	return std::sqrt(best);
}
// Bright cell centres, dark borders: the billowy look.
static float WorleyCells(float x, float y, float z, int period, uint32_t seed) {
	return 1.0f - Saturate(Worley(x, y, z, period, seed));
}
static float WorleyFbm(float x, float y, float z, int period, uint32_t seed) {
	return WorleyCells(x, y, z, period, seed) * 0.625f + WorleyCells(x, y, z, period * 2, seed + 7) * 0.25f +
		WorleyCells(x, y, z, period * 4, seed + 13) * 0.125f;
}

static float Fade(float t) { return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f); }
static float Gradient(uint32_t h, float x, float y, float z) {
	switch (h & 15u) {
	case 0: return x + y; case 1: return -x + y; case 2: return x - y; case 3: return -x - y;
	case 4: return x + z; case 5: return -x + z; case 6: return x - z; case 7: return -x - z;
	case 8: return y + z; case 9: return -y + z; case 10: return y - z; case 11: return -y - z;
	case 12: return x + y; case 13: return -x + y; case 14: return -y + z; default: return -y - z;
	}
}
// Gradient noise tiling every period cells; about -1 .. 1.
static float Perlin(float x, float y, float z, int period, uint32_t seed) {
	const float px = x * period, py = y * period, pz = z * period;
	const int ix = (int)std::floor(px), iy = (int)std::floor(py), iz = (int)std::floor(pz);
	const float fx = px - ix, fy = py - iy, fz = pz - iz;
	float corner[8];
	for (int c = 0; c < 8; c++) {
		const int ox = c & 1, oy = (c >> 1) & 1, oz = (c >> 2) & 1;
		const uint32_t h = Hash(Wrap(ix + ox, period), Wrap(iy + oy, period), Wrap(iz + oz, period), seed);
		corner[c] = Gradient(h, fx - ox, fy - oy, fz - oz);
	}
	const float u = Fade(fx), v = Fade(fy), w = Fade(fz);
	const float x00 = corner[0] + (corner[1] - corner[0]) * u, x10 = corner[2] + (corner[3] - corner[2]) * u;
	const float x01 = corner[4] + (corner[5] - corner[4]) * u, x11 = corner[6] + (corner[7] - corner[6]) * u;
	const float y0 = x00 + (x10 - x00) * v, y1 = x01 + (x11 - x01) * v;
	return y0 + (y1 - y0) * w;
}
// 0 .. 1 fbm of octaves Perlin layers, each twice the frequency and half the weight of the last.
static float PerlinFbm(float x, float y, float z, int period, int octaves, uint32_t seed) {
	float sum = 0.0f, weight = 0.5f, total = 0.0f;
	for (int o = 0; o < octaves; o++) {
		sum += Perlin(x, y, z, period << o, seed + o * 31) * weight;
		total += weight;
		weight *= 0.5f;
	}
	return Saturate(sum / total * 0.75f + 0.5f);
}

// ---- DDS writing (uncompressed; volume or 2D; full mip chain) ----
struct DdsPixelFormat { uint32_t size, flags, fourCC, bits, r, g, b, a; };
struct DdsHeader {
	uint32_t size, flags, height, width, pitch, depth, mips, reserved[11];
	DdsPixelFormat format;
	uint32_t caps, caps2, caps3, caps4, reserved2;
};

static bool WriteDds(const std::string& path, int width, int height, int depth, int bytesPerPixel,
	const std::vector<std::vector<uint8_t>>& levels) {
	DdsHeader h = {};
	h.size = 124;
	h.flags = 0x1 | 0x2 | 0x4 | 0x8 | 0x1000 | 0x20000 | (depth > 1 ? 0x800000 : 0);
	h.width = width; h.height = height; h.depth = depth > 1 ? depth : 0;
	h.pitch = width * bytesPerPixel;
	h.mips = (uint32_t)levels.size();
	h.format.size = 32;
	if (bytesPerPixel == 1) { h.format.flags = 0x20000; h.format.bits = 8; h.format.r = 0xFF; }	// luminance
	else { h.format.flags = 0x40 | 0x1; h.format.bits = 32; h.format.r = 0xFF0000; h.format.g = 0xFF00; h.format.b = 0xFF; h.format.a = 0xFF000000u; }
	h.caps = 0x1000 | 0x8 | 0x400000;
	h.caps2 = depth > 1 ? 0x200000 : 0;
	FILE* f = std::fopen(path.c_str(), "wb");
	if (!f) return false;
	const uint32_t magic = 0x20534444;	// "DDS "
	std::fwrite(&magic, 4, 1, f);
	std::fwrite(&h, sizeof(h), 1, f);
	for (const auto& level : levels) std::fwrite(level.data(), 1, level.size(), f);
	std::fclose(f);
	return true;
}

// Box-filtered mip chain of a channels-per-texel volume (depth 1 for a 2D texture).
static std::vector<std::vector<uint8_t>> MipChain(std::vector<uint8_t> top, int w, int h, int d, int channels) {
	std::vector<std::vector<uint8_t>> levels;
	levels.push_back(top);
	while (w > 1 || h > 1 || d > 1) {
		const int nw = std::max(w / 2, 1), nh = std::max(h / 2, 1), nd = std::max(d / 2, 1);
		std::vector<uint8_t> next((size_t)nw * nh * nd * channels);
		const std::vector<uint8_t>& prev = levels.back();
		for (int z = 0; z < nd; z++) for (int y = 0; y < nh; y++) for (int x = 0; x < nw; x++) for (int c = 0; c < channels; c++) {
			int sum = 0, count = 0;
			for (int dz = 0; dz < (d > 1 ? 2 : 1); dz++) for (int dy = 0; dy < (h > 1 ? 2 : 1); dy++) for (int dx = 0; dx < (w > 1 ? 2 : 1); dx++) {
				sum += prev[(((size_t)(z * 2 + dz) * h + (y * 2 + dy)) * w + (x * 2 + dx)) * channels + c];
				count++;
			}
			next[(((size_t)z * nh + y) * nw + x) * channels + c] = (uint8_t)((sum + count / 2) / count);
		}
		levels.push_back(std::move(next));
		w = nw; h = nh; d = nd;
	}
	return levels;
}

static uint8_t Byte(float v) { return (uint8_t)std::lround(Saturate(v) * 255.0f); }

// Linear stretch so the 1st .. 99th percentiles of values span 0 .. 1.
static void Stretch(std::vector<float>& values) {
	std::vector<float> sorted = values;
	std::sort(sorted.begin(), sorted.end());
	const float lo = sorted[sorted.size() / 100], hi = sorted[sorted.size() * 99 / 100];
	for (float& v : values) v = Saturate((v - lo) / std::max(hi - lo, 1e-6f));
}

int main(int argc, char** argv) {
	if (argc != 2) { std::puts("usage: cloud_noise <output folder>"); return 2; }
	const std::string out = argv[1];

	// Shape: Perlin-Worley, then eroded by a lower-frequency Worley fbm.
	{
		const int n = 128;
		std::vector<uint8_t> voxels((size_t)n * n * n);
		double sum = 0.0;
		for (int z = 0; z < n; z++) {
			for (int y = 0; y < n; y++) for (int x = 0; x < n; x++) {
				const float u = (x + 0.5f) / n, v = (y + 0.5f) / n, w = (z + 0.5f) / n;
				const float perlin = PerlinFbm(u, v, w, 4, 3, 11);
				const float worley = WorleyFbm(u, v, w, 4, 23);
				const float perlinWorley = Saturate(Remap(perlin, 0.0f, 1.0f, worley, 1.0f));
				const float erosion = WorleyFbm(u, v, w, 4, 37);
				const float shape = Saturate(Remap(perlinWorley, erosion - 1.0f, 1.0f, 0.0f, 1.0f));
				voxels[((size_t)z * n + y) * n + x] = Byte(shape);
				sum += shape;
			}
			if (z % 16 == 15) std::printf("shape %d/%d\n", z + 1, n);
		}
		std::printf("shape mean %.3f\n", sum / ((double)n * n * n));
		if (!WriteDds(out + "\\CloudShape.dds", n, n, n, 1, MipChain(voxels, n, n, n, 1))) { std::puts("write failed"); return 1; }
	}
	// Detail: Worley fbm.
	{
		const int n = 32;
		std::vector<uint8_t> voxels((size_t)n * n * n);
		for (int z = 0; z < n; z++) for (int y = 0; y < n; y++) for (int x = 0; x < n; x++) {
			const float u = (x + 0.5f) / n, v = (y + 0.5f) / n, w = (z + 0.5f) / n;
			voxels[((size_t)z * n + y) * n + x] = Byte(WorleyFbm(u, v, w, 2, 53));
		}
		if (!WriteDds(out + "\\CloudDetail.dds", n, n, n, 1, MipChain(voxels, n, n, n, 1))) { std::puts("write failed"); return 1; }
	}
	// Weather map: coverage pattern, cloud type, density. BGRA byte order (A8R8G8B8).
	{
		const int n = 512;
		std::vector<uint8_t> texels((size_t)n * n * 4);
		for (int y = 0; y < n; y++) for (int x = 0; x < n; x++) {
			const float u = (x + 0.5f) / n, v = (y + 0.5f) / n;
			const float perlin = PerlinFbm(u, v, 0.37f, 4, 6, 71);
			const float cells = WorleyFbm(u, v, 0.61f, 3, 83);
			const float coverage = Saturate(Remap(perlin, 0.0f, 1.0f, cells * 0.6f, 1.0f));
			const float type = PerlinFbm(u, v, 0.13f, 2, 3, 97);
			const float density = PerlinFbm(u, v, 0.83f, 8, 3, 109);
			uint8_t* t = &texels[((size_t)y * n + x) * 4];
			t[0] = Byte(density); t[1] = Byte(type); t[2] = Byte(coverage); t[3] = 255;
		}
		if (!WriteDds(out + "\\CloudWeather.dds", n, n, 1, 4, MipChain(texels, n, n, 1, 4))) { std::puts("write failed"); return 1; }
	}
	// General noise volume for the fog and the smoke.
	{
		const int n = 64;
		const size_t count = (size_t)n * n * n;
		std::vector<float> smooth(count), fbm(count), billows(count), second(count);
		for (int z = 0; z < n; z++) for (int y = 0; y < n; y++) for (int x = 0; x < n; x++) {
			const float u = (float)x / n, v = (float)y / n, w = (float)z / n;
			const size_t i = ((size_t)z * n + y) * n + x;
			smooth[i] = Perlin(u, v, w, 8, 131);
			fbm[i] = PerlinFbm(u, v, w, 4, 4, 137);
			billows[i] = WorleyFbm(u, v, w, 4, 149);
			second[i] = Perlin(u, v, w, 8, 151);
		}
		Stretch(smooth); Stretch(fbm); Stretch(billows); Stretch(second);
		std::vector<uint8_t> texels(count * 4);
		for (size_t i = 0; i < count; i++) {
			texels[i * 4 + 0] = Byte(billows[i]);	// b
			texels[i * 4 + 1] = Byte(fbm[i]);		// g
			texels[i * 4 + 2] = Byte(smooth[i]);	// r
			texels[i * 4 + 3] = Byte(second[i]);	// a
		}
		if (!WriteDds(out + "\\NoiseVolume.dds", n, n, n, 4, MipChain(texels, n, n, n, 4))) { std::puts("write failed"); return 1; }
	}
	std::puts("done");
	return 0;
}
