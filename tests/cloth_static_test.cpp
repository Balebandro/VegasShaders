// UNOFFICIAL 2026-10-20 cloth on still objects: runs the DLL's per-vertex weights of still cloth (src/core/ClothStatic.h) on meshes
// dumped by bench\deep-roadmap\evidence\clothauto\survey_static.py, for the pictures and the survey table.
//   cloth_static_test <folder>      every <name>.csin in it -> <name>.csout + one line of counts each
// in:  u32 vertices, u32 triangles, f32 xyz * vertices, f32 normal xyz * vertices, u16 abc * triangles
// out: u32 vertices, f32 weight * vertices, u8 kind * vertices (0 sheet, 1 rod, 2 solid, 3 tiny), u8 flag * vertices (ClothStatic::Result::flag), f32 side * vertices
// Build (x86 Native Tools prompt): cl /EHsc /O2 /std:c++17 cloth_static_test.cpp
#include "../src/core/ClothStatic.h"
#include <cstdio>
#include <vector>
#include <string>
#include <chrono>
#include <windows.h>

static bool RunOne(const std::string& in, const std::string& outPath) {
	FILE* f = fopen(in.c_str(), "rb");
	if (!f) return false;
	uint32_t nv = 0, nt = 0;
	if (fread(&nv, 4, 1, f) != 1 || fread(&nt, 4, 1, f) != 1) { fclose(f); return false; }
	std::vector<float> pos(nv * 3), nrm(nv * 3);
	std::vector<uint16_t> tris(nt * 3);
	const bool ok = fread(pos.data(), 4, pos.size(), f) == pos.size() && fread(nrm.data(), 4, nrm.size(), f) == nrm.size() && fread(tris.data(), 2, tris.size(), f) == tris.size();
	fclose(f);
	if (!ok) { printf("%s: short file\n", in.c_str()); return false; }
	ClothStatic::Input ci;
	ci.pos = pos.data(); ci.nrm = nrm.data(); ci.vertices = nv; ci.tris = tris.data(); ci.triangles = nt;
	ClothStatic::Result r;
	const auto t0 = std::chrono::high_resolution_clock::now();
	const bool done = ClothStatic::Compute(ci, r);
	const double ms = std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - t0).count();
	if (!done) { printf("%s: Compute failed\n", in.c_str()); return false; }
	FILE* o = fopen(outPath.c_str(), "wb");
	if (!o) return false;
	fwrite(&nv, 4, 1, o); fwrite(r.weight.data(), 4, nv, o); fwrite(r.kind.data(), 1, nv, o); fwrite(r.flag.data(), 1, nv, o); fwrite(r.side.data(), 4, nv, o);
	fclose(o);
	printf("%s %u welded %u pieces %u rods %u solids %u tiny %u sheets %u clusters %u heldRigid %u heldGround %u heldTop %u free %u moving %u layered %u groundZ %.1f standing %d ms %.2f\n",
		in.c_str(), nv, r.welded, r.pieces, r.rods, r.solids, r.tiny, r.sheets, r.clusters, r.heldRigid, r.heldGround, r.heldTop, r.freeEdges, r.moving, r.layered, r.groundZ, r.standing ? 1 : 0, ms);
	return true;
}

int main(int argc, char** argv) {
	if (argc < 2) { printf("usage: cloth_static_test <folder>\n"); return 2; }
	const std::string dir = argv[1];
	WIN32_FIND_DATAA fd;
	HANDLE h = FindFirstFileA((dir + "\\*.csin").c_str(), &fd);
	if (h == INVALID_HANDLE_VALUE) { printf("no .csin in %s\n", dir.c_str()); return 1; }
	int n = 0;
	do {
		const std::string name = fd.cFileName;
		RunOne(dir + "\\" + name, dir + "\\" + name.substr(0, name.size() - 5) + ".csout");
		n++;
	} while (FindNextFileA(h, &fd));
	FindClose(h);
	fprintf(stderr, "%d meshes\n", n);
	return 0;
}
