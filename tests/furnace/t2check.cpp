// SmithCorrelated TEST mode check (2026-10-18 pbrport): PBR_SMITH_CORRELATED 2 with SmithCorrelatedTest 0 (TESR_PBRMath.x) must give the
// OFF code's results bit for bit (so the in-game 'off' shots of the test mode ARE the old picture), and with 1 the mode-1 results
// within float rounding. 1,000,000 random inputs x PBR / PBRSpecular / PBRSun / PBRSunSpecular, with SetDiffuseEnergyKeep per input.
#include <cstdio>
#include <cmath>
#include <cstring>
#include <random>
#undef PBR_SMITH_CORRELATED
#define PBR_SMITH_CORRELATED 2
#include "pbr_t2.h"
#undef PBR_SMITH_CORRELATED
#define PBR_SMITH_CORRELATED 1
#include "pbr_on.h"
#undef PBR_SMITH_CORRELATED
#define PBR_SMITH_CORRELATED 0
#include "pbr_off.h"
typedef float3 (*Fn)(float, float, float3, float3, float3, float3, float3);
int main() {
	std::mt19937 rng(777); std::uniform_real_distribution<float> U(-1, 1), U01(0, 1);
	auto dir = [&]() { for (;;) { float3 d(U(rng), U(rng), U(rng)); const float l = dot(d, d); if (l > 1e-4f && l <= 1) return d * (1.0f / std::sqrt(l)); } };
	Fn t2[] = { t2pbr::PBR, t2pbr::PBRSpecular, t2pbr::PBRSun, t2pbr::PBRSunSpecular }, on[] = { onpbr::PBR, onpbr::PBRSpecular, onpbr::PBRSun, onpbr::PBRSunSpecular },
	   off[] = { offpbr::PBR, offpbr::PBRSpecular, offpbr::PBRSun, offpbr::PBRSunSpecular };
	long long diff0 = 0, n = 0; double worst1 = 0;
	for (int k = 0; k < 1000000; k++) {
		const float3 N = dir(), V = dir(), L = dir(), alb(U01(rng), U01(rng), U01(rng)); const float r = 0.02f + 0.98f * U01(rng), m = U01(rng);
		for (int f = 0; f < 4; f++) {
			t2pbr::TESR_PBRMath.x = 0; t2pbr::SetDiffuseEnergyKeep(N, V, r); offpbr::SetDiffuseEnergyKeep(N, V, r);
			const float3 a = t2[f](m, r, alb, N, V, L, float3(1.0f)), b = off[f](m, r, alb, N, V, L, float3(1.0f));
			n++; if (memcmp(&a, &b, sizeof a) && !(std::isnan(a.x) && std::isnan(b.x))) diff0++;
			t2pbr::TESR_PBRMath.x = 1; t2pbr::SetDiffuseEnergyKeep(N, V, r); onpbr::SetDiffuseEnergyKeep(N, V, r);
			const float3 c = t2[f](m, r, alb, N, V, L, float3(1.0f)), d = on[f](m, r, alb, N, V, L, float3(1.0f));
			for (float q : { (c.x - d.x) / std::fmax(std::fabs(d.x), 1e-3f), (c.y - d.y) / std::fmax(std::fabs(d.y), 1e-3f) }) if (!std::isnan(q)) worst1 = std::fmax(worst1, std::fabs(q));
		}
	}
	printf("A2c test mode, strength 0 vs OFF: %lld of %lld results differ (must be 0) -> %s\n", diff0, n, diff0 ? "FAIL" : "PASS");
	printf("A2c test mode, strength 1 vs mode 1: largest relative difference %.2e (float rounding expected)\n", worst1);
	return diff0 ? 1 : 0;
}
