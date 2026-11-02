// Probe (2026-10-18 pbrport): is the sun pass' low furnace value at roughness 0.1, N.V 0.05 (hcfurnace A5c) real or under-sampling?
// White dielectric, PBR_SMITH_CORRELATED 1, coupled diffuse; uniform hemisphere grids of increasing size, spec and diffuse parts apart.
#include <cstdio>
#include <cmath>
#include <cstdlib>
#undef PBR_SMITH_CORRELATED
#define PBR_SMITH_CORRELATED 1
#include "pbr_on.h"
using namespace onpbr;
int main(int argc, char** argv) {
	const double kPi = 3.14159265358979;
	for (float r : { 0.1f, 0.15f, 0.2f }) for (float nv : { 0.03f, 0.05f, 0.08f }) for (int n : { 1536, 4096 }) {
		const float3 N(0, 0, 1), V(std::sqrt(1 - nv * nv), 0, nv), one(1.0f);
		SetDiffuseEnergyKeep(N, V, r);
		double tot = 0, spec = 0, fullSpec = 0;
		for (int i = 0; i < n; i++) { const double ct = (i + 0.5) / n, st = std::sqrt(1 - ct * ct);
			for (int j = 0; j < 2 * n; j++) { const double ph = (j + 0.5) / (2 * n) * 2 * kPi;
				const float3 L((float)(st * std::cos(ph)), (float)(st * std::sin(ph)), (float)ct);
				tot += PBRSun(0, r, one, N, V, L, one).y; spec += PBRSunSpecular(0, r, one, N, V, L, one).y; fullSpec += PBRSpecular(0, r, one, N, V, L, one).y; } }
		const double k = 2.0 / ((double)n * 2 * n);
		printf("r %.2f nv %.2f n %4d: sun total %.4f (sun spec %.4f, diffuse %.4f), full-pass spec on the same grid %.4f, keep %.3f\n", r, nv, n, tot * k, spec * k, (tot - spec) * k, fullSpec * k, PBRDiffuseKeep);
	}
}
