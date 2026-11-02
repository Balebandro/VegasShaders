// Shared lighting math check (2026-10-16, foundations study): scores the changed Shaders\Includes\PBR.hlsl against the limits written
// BEFORE the change (bench\foundations\REPORT.md, "Shared lighting math"). Runs the REAL shader code on the CPU:
//   pbr_now.h  = this tree's PBR.hlsl            (gen_pbr_cpp.py ..\..\src\hlsl\NewVegas\Shaders\Includes\PBR.hlsl pbr_now.h nowpbr)
//   pbr_old.h  = the PBR.hlsl before the change  (gen_pbr_cpp.py <old PBR.hlsl> pbr_old.h oldpbr)
// E1/E2/E3: white furnace of the full pass and the sun pass with EnergySplit on (the furnace grid of furnace.cpp, and for the full pass
//           also a fine 41 x 41 grid where the highlight part is integrated by GGX importance sampling, so narrow lobes are resolved).
// S2:       no NaN / Inf / negative in PBRSun and PBRSunSpecular (edge cases + 1,000,000 random inputs).
// M1:       EnergySplit off = the old code (1,000,000 random inputs, largest relative difference).
// Build: cl /nologo /O2 /EHsc /fp:precise lightmath.cpp      Run: lightmath.exe [furnace grid resolution, default 2048]
#include <cstdio>
#include <cmath>
#include <random>
#include <vector>
#include "pbr_now.h"
#include "pbr_old.h"
using namespace nowpbr;   // after pbr_old.h: each copy calls its own functions (both in a namespace, so no argument-dependent clash)

typedef float3 (*Fn)(float, float, float3, float3, float3, float3, float3);

static double RhoUniform(Fn fn, float metal, float rough, float nv, int n) {
	const float3 N(0, 0, 1), V(std::sqrt(std::fmax(0.f, 1 - nv * nv)), 0, nv);
	nowpbr::SetDiffuseEnergyKeep(N, V, rough);   // as the templates do, once per pixel (harmless for the old copy)
	double s = 0;
	for (int i = 0; i < n; i++) { const float ct = (i + 0.5f) / n, st = std::sqrt(1 - ct * ct);
		for (int j = 0; j < 2 * n; j++) { const float ph = (j + 0.5f) / (2 * n) * 2 * PI; const float3 L(st * std::cos(ph), st * std::sin(ph), ct);
			s += fn(metal, rough, float3(1.0f), N, V, L, float3(1.0f)).y; } }
	return s * 2.0 / ((double)n * 2 * n);
}
// Highlight part by GGX importance sampling (pdf of L = D(h) NdotH / (4 VdotH)), n x n stratified half-vectors.
static double RhoSpecIS(Fn fn, float rough, float nv, int n) {
	const float3 N(0, 0, 1), V(std::sqrt(std::fmax(0.f, 1 - nv * nv)), 0, nv);
	const double a = (double)rough * rough, a2 = a * a, pi = 3.14159265358979;
	double s = 0;
	for (int i = 0; i < n; i++) for (int j = 0; j < n; j++) {
		const double u1 = (i + 0.5) / n, u2 = (j + 0.5) / n, t2 = a2 * u1 / (1 - u1), ct = 1 / std::sqrt(1 + t2), st = std::sqrt(1 - ct * ct), ph = 2 * pi * u2;
		const float3 H((float)(st * std::cos(ph)), (float)(st * std::sin(ph)), (float)ct);
		const float vh = dot(V, H); if (vh <= 0) continue;
		const float3 L = 2.0f * vh * H - V; if (L.z <= 0) continue;
		const double dd = ct * ct * (a2 - 1) + 1, D = a2 / (pi * dd * dd);
		s += fn(0.0f, rough, float3(1.0f), N, V, L, float3(1.0f)).y / pi / (D * ct / (4 * vh));
	}
	return s / ((double)n * n);
}
static float3 FullDiffusePart(float m, float r, float3 a, float3 n, float3 v, float3 l, float3 c) { return PBR(m, r, a, n, v, l, c) - PBRSpecular(m, r, a, n, v, l, c); }
static float3 OldFullDiffusePart(float m, float r, float3 a, float3 n, float3 v, float3 l, float3 c) { return oldpbr::PBR(m, r, a, n, v, l, c) - oldpbr::PBRSpecular(m, r, a, n, v, l, c); }

static bool Bad(float3 c) { for (float v : { c.x, c.y, c.z }) if (std::isnan(v) || std::isinf(v) || v < 0) return true; return false; }

int main(int argc, char** argv) {
	const int nGrid = argc > 1 ? atoi(argv[1]) : 2048;
	int fails = 0;
	auto verdict = [&](const char* id, bool pass, const char* what) { printf("%s %s  %s\n", id, pass ? "PASS" : "FAIL", what); if (!pass) fails++; };

	// ---- E1 / E2 / E3 on the furnace grid ----
	const float roughs[] = { 0.1f, 0.2f, 0.3f, 0.5f, 0.7f, 1.0f }, nvs[] = { 0.05f, 0.1f, 0.25f, 0.5f, 0.75f, 1.0f };
	struct P { const char* name; Fn now, old; } passes[] = { { "PBR (full pass)", PBR, oldpbr::PBR }, { "PBRSun (sun pass)", PBRSun, oldpbr::PBRSun } };
	double maxOn = 0, minOn = 9, minOld = 9, metalDiff = 0;
	printf("WHITE FURNACE (furnace grid, %d x %d directions), EnergySplit ON vs the old code; rows roughness, columns N.V\n", nGrid, 2 * nGrid);
	for (const P& p : passes) {
		printf(" %s, dielectric: new (old)\n   rough\\NdotV", p.name);
		for (float nv : nvs) printf("        %4.2f", nv);
		printf("\n");
		for (float r : roughs) {
			printf("   %5.2f      ", r);
			for (float nv : nvs) {
				TESR_PBRMath.x = 1.0f; const double on = RhoUniform(p.now, 0, r, nv, nGrid);
				const double old = RhoUniform(p.old, 0, r, nv, nGrid);
				maxOn = std::fmax(maxOn, on); minOn = std::fmin(minOn, on); minOld = std::fmin(minOld, old);
				printf(" %.3f(%.3f)", on, old);
				const double mOn = RhoUniform(p.now, 1, r, nv, nGrid / 4), mOld = RhoUniform(p.old, 1, r, nv, nGrid / 4);
				metalDiff = std::fmax(metalDiff, std::fabs(mOn - mOld));
			}
			printf("\n");
		}
	}
	// fine grid, full pass: highlight part (unchanged code) by importance sampling + the diffuse part (PBR - PBRSpecular) uniformly
	double fineMax = 0, fineMaxOld = 0, fineMin = 9, fineMinOld = 9; float atR = 0, atV = 0;
	for (int i = 0; i <= 40; i++) for (int j = 0; j <= 40; j++) {
		const float r = 0.02f + 0.98f * i / 40, nv = 0.005f + 0.995f * j / 40;
		const double spec = RhoSpecIS(PBRSpecular, r, nv, 256);
		TESR_PBRMath.x = 1.0f; const double on = spec + RhoUniform(FullDiffusePart, 0, r, nv, 128);
		const double old = RhoSpecIS(oldpbr::PBRSpecular, r, nv, 256) + RhoUniform(OldFullDiffusePart, 0, r, nv, 128);
		if (on > fineMax) { fineMax = on; atR = r; atV = nv; }
		fineMaxOld = std::fmax(fineMaxOld, old); fineMin = std::fmin(fineMin, on); fineMinOld = std::fmin(fineMinOld, old);
	}
	printf(" fine grid (41 x 41, roughness 0.02-1, N.V 0.005-1), full pass: new max %.4f (at roughness %.3f N.V %.3f), old max %.4f; new min %.4f, old min %.4f\n",
		fineMax, atR, atV, fineMaxOld, fineMin, fineMinOld);
	char buf[256];
	snprintf(buf, sizeof buf, "furnace max %.4f on the furnace grid (limit 1.005; old 1.073 / 1.067), %.4f on the fine grid (old %.4f)", maxOn, fineMax, fineMaxOld);
	verdict("E1", maxOn <= 1.005 && fineMax <= 1.005, buf);
	snprintf(buf, sizeof buf, "furnace min %.4f (limit: old min %.4f - 0.01); fine grid %.4f (old %.4f)", minOn, minOld, fineMin, fineMinOld);
	verdict("E2", minOn >= minOld - 0.01 && fineMin >= fineMinOld - 0.01, buf);
	snprintf(buf, sizeof buf, "metal tables, largest change %.5f (limit 0.001)", metalDiff);
	verdict("E3", metalDiff <= 0.001, buf);

	// ---- S2: NaN guard in the sun pass ----
	struct Case { const char* what; float3 n, v, l; float rough, metal; };
	const float3 z(0, 0, 1), x(1, 0, 0);
	const Case cases[] = {
		{ "sun exactly along the normal (N = L)", z, normalize(float3(0.6f, 0, 0.8f)), z, 0.5f, 0 },
		{ "sun exactly along the normal, eye too (L = V = N)", z, z, z, 0.5f, 0 },
		{ "eye exactly opposite the sun (V = -L)", z, normalize(float3(0.6f, 0, 0.8f)), normalize(float3(-0.6f, 0, -0.8f)), 0.5f, 0 },
		{ "sun straight behind (N = -L)", z, normalize(float3(0.6f, 0, 0.8f)), -1.0f * z, 0.5f, 0 },
		{ "grazing view (N.V = 0)", z, x, normalize(float3(0.3f, 0.2f, 0.9f)), 0.5f, 0 },
		{ "grazing sun (N.L = 0)", z, normalize(float3(0.3f, 0.2f, 0.9f)), x, 0.5f, 0 },
		{ "roughness 0", z, normalize(float3(0.3f, 0, 0.95f)), normalize(float3(-0.3f, 0, 0.95f)), 0.0f, 0 },
		{ "roughness 0, metal", z, normalize(float3(0.3f, 0, 0.95f)), normalize(float3(-0.3f, 0, 0.95f)), 0.0f, 1 } };
	nowpbr::PBRDiffuseKeep = 1.0f;
	int s2bad = 0, s2badOld = 0;
	printf("\nSUN PASS EDGE CASES (bad = NaN / Inf / negative), new | old\n");
	for (const Case& c : cases) for (float on : { 0.0f, 1.0f }) {
		TESR_PBRMath.x = on;
		const bool b1 = Bad(PBRSun(c.metal, c.rough, float3(1.0f), c.n, c.v, c.l, float3(1.0f))), b2 = Bad(PBRSunSpecular(c.metal, c.rough, float3(1.0f), c.n, c.v, c.l, float3(1.0f)));
		const bool o1 = Bad(oldpbr::PBRSun(c.metal, c.rough, float3(1.0f), c.n, c.v, c.l, float3(1.0f))), o2 = Bad(oldpbr::PBRSunSpecular(c.metal, c.rough, float3(1.0f), c.n, c.v, c.l, float3(1.0f)));
		if (on == 1.0f) printf("  %-52s %s | %s\n", c.what, (b1 || b2) ? "BAD" : "ok", (o1 || o2) ? "BAD" : "ok");
		s2bad += b1 + b2; s2badOld += o1 + o2;
	}
	std::mt19937 rng(4321);
	std::uniform_real_distribution<float> u(-1, 1), u01(0, 1);
	auto randDir = [&]() { float3 d; do d = float3(u(rng), u(rng), u(rng)); while (dot(d, d) < 1e-4f || dot(d, d) > 1); return normalize(d); };
	int s2rand = 0;
	for (int i = 0; i < 1000000; i++) {
		const float3 n = randDir(), v = randDir(), l = randDir(); const float r = u01(rng), m = u01(rng) < 0.5f ? 0.0f : 1.0f, a = u01(rng);
		TESR_PBRMath.x = (i & 1) ? 1.0f : 0.0f;
		s2rand += Bad(PBRSun(m, r, float3(a), n, v, l, float3(1.0f))) + Bad(PBRSunSpecular(m, r, float3(a), n, v, l, float3(1.0f)));
	}
	snprintf(buf, sizeof buf, "sun pass: %d bad on the edge cases (old %d), %d bad on 1,000,000 random inputs", s2bad, s2badOld, s2rand);
	verdict("S2", s2bad == 0 && s2rand == 0, buf);

	// ---- M1: EnergySplit off = old ----
	struct Q { const char* name; Fn now, old; } all[] = { { "PBR", PBR, oldpbr::PBR }, { "PBRSun", PBRSun, oldpbr::PBRSun }, { "PBRSunSpecular", PBRSunSpecular, oldpbr::PBRSunSpecular },
		{ "PBRDiffuse", PBRDiffuse, oldpbr::PBRDiffuse }, { "PBRSpecular", PBRSpecular, oldpbr::PBRSpecular } };
	double worst = 0;
	printf("\nOFF = OLD (EnergySplit off, 1,000,000 random inputs; inputs where the old code gives NaN skipped)\n");
	for (const Q& q : all) {
		double w = 0; int skipped = 0;
		std::mt19937 rr(99);
		std::uniform_real_distribution<float> uu(-1, 1), uu01(0, 1);
		auto rd = [&]() { float3 d; do d = float3(uu(rr), uu(rr), uu(rr)); while (dot(d, d) < 1e-4f || dot(d, d) > 1); return normalize(d); };
		TESR_PBRMath.x = 0.0f; nowpbr::PBRDiffuseKeep = 1.0f;
		for (int i = 0; i < 1000000; i++) {
			const float3 n = rd(), v = rd(), l = rd(); const float r = uu01(rr), m = uu01(rr) < 0.5f ? 0.0f : 1.0f; const float3 a(uu01(rr), uu01(rr), uu01(rr));
			const float3 o = q.old(m, r, a, n, v, l, float3(1.0f)), c = q.now(m, r, a, n, v, l, float3(1.0f));
			if (Bad(o)) { skipped++; continue; }
			for (int k = 0; k < 3; k++) { const float ov = k == 0 ? o.x : k == 1 ? o.y : o.z, cv = k == 0 ? c.x : k == 1 ? c.y : c.z;
				w = std::fmax(w, std::fabs(cv - ov) / std::fmax(std::fabs(ov), 1e-6f)); }
		}
		printf("  %-16s largest relative difference %.2e (%d skipped)\n", q.name, w, skipped);
		worst = std::fmax(worst, w);
	}
	snprintf(buf, sizeof buf, "EnergySplit off vs old, largest relative difference %.2e (limit 1e-5)", worst);
	verdict("M1", worst <= 1e-5, buf);
	printf("\n%s\n", fails ? "SOME CPU LIMITS FAIL" : "ALL CPU LIMITS PASS");
	return fails;
}
