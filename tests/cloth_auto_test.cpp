// UNOFFICIAL 2026-10-20 cloth auto-detect: runs the DLL's worn-garment measures (src/core/ClothWeights.h: FreeShare, then Compute's
// moving count and hem drop) on the meshes dumped by bench\deep-roadmap\evidence\clothauto\survey.py, for the survey table.
//   cloth_auto_test <folder>      every <name>.cwin in it -> one line of numbers each
// in: the cloth_weights_test format (u32 vertices, u32 triangles, f32 xyz * vertices, u16 abc * triangles, f32 legs[2][3][4], f32 hipZ,
//     kneeZ, groundZ)
// Build (x86 Native Tools prompt): cl /EHsc /O2 /std:c++17 cloth_auto_test.cpp
#include "../src/core/ClothWeights.h"
#include <cstdio>
#include <vector>
#include <string>
#include <chrono>
#include <windows.h>

static void RunOne(const std::string& dir, const std::string& name) {
	FILE* f = fopen((dir + "\\" + name).c_str(), "rb");
	if (!f) return;
	uint32_t nv = 0, nt = 0;
	if (fread(&nv, 4, 1, f) != 1 || fread(&nt, 4, 1, f) != 1) { fclose(f); return; }
	std::vector<float> pos(nv * 3);
	std::vector<uint16_t> tris(nt * 3);
	ClothWeights::Input in;
	const bool ok = fread(pos.data(), 4, pos.size(), f) == pos.size() && fread(tris.data(), 2, tris.size(), f) == tris.size() &&
		fread(in.legs, 4, 24, f) == 24 && fread(&in.hipZ, 4, 1, f) == 1 && fread(&in.kneeZ, 4, 1, f) == 1 && fread(&in.groundZ, 4, 1, f) == 1;
	fclose(f);
	if (!ok) { printf("%s short\n", name.c_str()); return; }
	in.pos = pos.data(); in.vertices = nv; in.tris = tris.data(); in.triangles = nt;
	const auto t0 = std::chrono::high_resolution_clock::now();
	const float free = ClothWeights::FreeShare(in);
	const auto t1 = std::chrono::high_resolution_clock::now();
	ClothWeights::Result r;
	const bool done = ClothWeights::Compute(in, r);
	const auto t2 = std::chrono::high_resolution_clock::now();
	if (done) {	// the weights and labels, for the pictures (<name>.cwout: u32 vertices, f32 weight * vertices, u8 label * vertices)
		FILE* o = fopen((dir + "\\" + name.substr(0, name.size() - 5) + ".cwout").c_str(), "wb");
		if (o) { fwrite(&nv, 4, 1, o); fwrite(r.weight.data(), 4, nv, o); fwrite(r.label.data(), 1, nv, o); fclose(o); }
	}
	printf("%s nv %u welded %u body %u cloth %u torso %u crotch %.1f moving %u hemdrop %.1f free %.4f done %d hangs %d free_us %.0f compute_ms %.2f\n",
		name.c_str(), nv, r.welded, r.body, r.cloth, r.torso, r.crotch, r.moving, r.hemDrop, free, done ? 1 : 0, done && free >= ClothWeights::kFreeFirst && ClothWeights::HangsFree(r, 0.02) ? 1 : 0,
		std::chrono::duration<double, std::micro>(t1 - t0).count(), std::chrono::duration<double, std::milli>(t2 - t1).count());
}

int main(int argc, char** argv) {
	if (argc < 2) { printf("usage: cloth_auto_test <folder>\n"); return 2; }
	WIN32_FIND_DATAA fd;
	HANDLE h = FindFirstFileA((std::string(argv[1]) + "\\*.cwin").c_str(), &fd);
	if (h == INVALID_HANDLE_VALUE) return 1;
	do RunOne(argv[1], fd.cFileName); while (FindNextFileA(h, &fd));
	FindClose(h);
	return 0;
}
