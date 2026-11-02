// Table for the SmithCorrelated diffuse keep fit (2026-10-18, helper 'pbrport'): the REAL PBR.hlsl with PBR_SMITH_CORRELATED 1
// (pbr_on.h), white dielectric (F0 0.04, albedo 1), keep = 1. Per (roughness, N.V): the full pass' diffuse part D (PBR - PBRSpecular,
// uniform hemisphere grid) and specular part S (PBRSpecular, half-vector importance sampling). The keep that makes the total 1 is
// (1 - S) / D where D + S > 1. Output CSV: r, nv, D, S.     Build: cl /nologo /O2 /EHsc /fp:precise hckeep_table.cpp
#include <cstdio>
#include <cmath>
#include <cstdlib>
#undef PBR_SMITH_CORRELATED
#define PBR_SMITH_CORRELATED 1
#include "pbr_on.h"
using namespace onpbr;
static const double kPi = 3.14159265358979;

int main(int argc, char** argv) {
	const int nIS = argc > 1 ? atoi(argv[1]) : 512, nUni = argc > 2 ? atoi(argv[2]) : 768, nR = argc > 3 ? atoi(argv[3]) : 41, nV = argc > 4 ? atoi(argv[4]) : 41;
	printf("r,nv,D,S\n");
	for (int ir = 0; ir < nR; ir++) for (int iv = 0; iv < nV; iv++) {
		const float r = 0.02f + 0.98f * ir / (nR - 1), nv = 0.02f + 0.98f * iv / (nV - 1);
		const float3 N(0, 0, 1), V(std::sqrt(std::fmax(0.f, 1 - nv * nv)), 0, nv), one(1.0f);
		double D = 0;
		for (int i = 0; i < nUni; i++) { const double ct = (i + 0.5) / nUni, st = std::sqrt(1 - ct * ct);
			for (int j = 0; j < 2 * nUni; j++) { const double ph = (j + 0.5) / (2 * nUni) * 2 * kPi;
				const float3 L((float)(st * std::cos(ph)), (float)(st * std::sin(ph)), (float)ct);
				D += PBR(0, r, one, N, V, L, one).y - PBRSpecular(0, r, one, N, V, L, one).y; } }
		D *= 2.0 / ((double)nUni * 2 * nUni);
		const double a = (double)r * r, a2 = a * a; double S = 0;
		for (int i = 0; i < nIS; i++) for (int j = 0; j < nIS; j++) {
			const double u1 = (i + 0.5) / nIS, u2 = (j + 0.5) / nIS, t2 = a2 * u1 / (1 - u1), ct = 1 / std::sqrt(1 + t2), st = std::sqrt(1 - ct * ct), ph = 2 * kPi * u2;
			const float3 H((float)(st * std::cos(ph)), (float)(st * std::sin(ph)), (float)ct);
			const float vh = dot(V, H); if (vh <= 0) continue;
			const float3 L = 2.0f * vh * H - V; if (L.z <= 0) continue;
			const double dd = ct * ct * (a2 - 1) + 1, Dg = a2 / (kPi * dd * dd);
			S += PBRSpecular(0, r, one, N, V, L, one).y / kPi / (Dg * ct / (4 * vh));
		}
		S /= (double)nIS * nIS;
		printf("%.4f,%.4f,%.6f,%.6f\n", r, nv, D, S);
	}
	return 0;
}
