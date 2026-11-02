// UNOFFICIAL 2026-10-19 cloth round 5: runs the DLL's per-vertex cloth weights (src/core/ClothWeights.h) on meshes dumped by the
// offline prototype (bench\deep-roadmap\evidence\cloth3\cloth_weights_check.py --dump), so the two can be compared vertex by vertex.
//   cloth_weights_test <in.cwin> <out.cwout>
// in:  u32 vertices, u32 triangles, f32 xyz * vertices, u16 abc * triangles, f32 legs[2][3][4], f32 hipZ, kneeZ, groundZ
// out: u32 vertices, f32 weight * vertices, u8 label * vertices, u32 welded, body, cloth, torso, cuts, tubes, pads, clothCuts, f32 crotch
// Build (x86 Native Tools prompt): cl /EHsc /O2 /std:c++17 cloth_weights_test.cpp
#include "../src/core/ClothWeights.h"
#include <cstdio>
#include <vector>

int main(int argc, char** argv) {
	if (argc < 3) { printf("usage: cloth_weights_test <in.cwin> <out.cwout>\n"); return 2; }
	FILE* f = fopen(argv[1], "rb");
	if (!f) { printf("cannot open %s\n", argv[1]); return 2; }
	uint32_t nv = 0, nt = 0;
	if (fread(&nv, 4, 1, f) != 1 || fread(&nt, 4, 1, f) != 1) return 2;
	std::vector<float> pos(nv * 3);
	std::vector<uint16_t> tris(nt * 3);
	ClothWeights::Input in;
	if (fread(pos.data(), 4, pos.size(), f) != pos.size() || fread(tris.data(), 2, tris.size(), f) != tris.size() ||
		fread(in.legs, 4, 24, f) != 24 || fread(&in.hipZ, 4, 1, f) != 1 || fread(&in.kneeZ, 4, 1, f) != 1 || fread(&in.groundZ, 4, 1, f) != 1) {
		printf("short file\n"); return 2;
	}
	fclose(f);
	in.pos = pos.data(); in.vertices = nv; in.tris = tris.data(); in.triangles = nt;
	ClothWeights::Result r;
	if (!ClothWeights::Compute(in, r)) { printf("Compute failed\n"); return 1; }
	FILE* o = fopen(argv[2], "wb");
	if (!o) return 2;
	fwrite(&nv, 4, 1, o);
	fwrite(r.weight.data(), 4, nv, o);
	fwrite(r.label.data(), 1, nv, o);
	const uint32_t counts[8] = { r.welded, r.body, r.cloth, r.torso, r.cuts, r.tubes, r.pads, r.clothCuts };
	fwrite(counts, 4, 8, o);
	fwrite(&r.crotch, 4, 1, o);
	fclose(o);
	printf("ok: %u vertices (%u welded): body %u, cloth %u, torso %u; cuts %u (tubes %u, pads %u, cloth %u); crotch %.1f\n",
		nv, r.welded, r.body, r.cloth, r.torso, r.cuts, r.tubes, r.pads, r.clothCuts, r.crotch);
	return 0;
}
