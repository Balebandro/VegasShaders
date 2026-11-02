// UNOFFICIAL 2026-10-20 cloth round 9: runs the DLL's hem ring + walking simulation (src/core/ClothWalk.h) on a case dumped by the
// offline prototype (bench\deep-roadmap\evidence\cloth3\cloth_walk_check.py with CW_WALK_CPP=<this exe>), so both can be compared.
//   cloth_walk_test <in.cwwalk> <out.cwwalkout>
// in:  u32 n, f32 xyz * n, f32 weight * n, u8 label * n, f32 centre[2], f32 hipZ, u32 frames, per frame: f64 dt, f64 r[9], f64 t[3], f64 legs[24]
// out: u32 n, f32 u * n, f32 swing * n, f64 p[48], f64 a[48], f64 L[16], per frame f64 d[48]
// Build (x86 Native Tools prompt): cl /EHsc /O2 /std:c++17 cloth_walk_test.cpp
#include "../src/core/ClothWalk.h"
#include <cstdio>
#include <vector>

int main(int argc, char** argv) {
	if (argc < 3) { printf("usage: cloth_walk_test <in> <out>\n"); return 2; }
	FILE* f = nullptr;
	if (fopen_s(&f, argv[1], "rb") || !f) return 2;
	uint32_t n = 0;
	fread(&n, 4, 1, f);
	std::vector<float> pos(n * 3), w(n);
	std::vector<uint8_t> lab(n);
	float c[2], hipZ;
	fread(pos.data(), 4, pos.size(), f); fread(w.data(), 4, n, f); fread(lab.data(), 1, n, f); fread(c, 4, 2, f); fread(&hipZ, 4, 1, f);
	ClothWalk::Ring ring;
	if (!ClothWalk::BuildRing(pos.data(), n, w.data(), lab.data(), c, hipZ, ring)) { printf("no ring\n"); return 1; }
	uint32_t frames = 0;
	fread(&frames, 4, 1, f);
	FILE* o = nullptr;
	if (fopen_s(&o, argv[2], "wb") || !o) return 2;
	fwrite(&n, 4, 1, o); fwrite(ring.u.data(), 4, n, o); fwrite(ring.swing.data(), 4, n, o);
	fwrite(ring.p, 8, 48, o); fwrite(ring.a, 8, 48, o); fwrite(ring.L, 8, 16, o);
	ClothWalk::State st; ClothWalk::Params prm;
	for (uint32_t i = 0; i < frames; i++) {
		double dt, r[9], t[3], legs[6][4], out[16][3];
		fread(&dt, 8, 1, f); fread(r, 8, 9, f); fread(t, 8, 3, f); fread(legs, 8, 24, f);
		ClothWalk::Xf x;
		for (int a = 0; a < 3; a++) { for (int b = 0; b < 3; b++) x.r[a][b] = r[a * 3 + b]; x.t[a] = t[a]; }
		ClothWalk::Step(st, ring, x, legs, dt, prm, out);
		fwrite(out, 8, 48, o);
	}
	fclose(f); fclose(o);
	printf("ok: %u vertices, %u frames\n", n, frames);
	return 0;
}
