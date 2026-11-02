// Furnace + invalid-value harness for NVR's shared shading math (2026-10-16, foundations study, Phase 1).
// It runs the REAL Shaders\Includes\PBR.hlsl (translated mechanically to C++ by gen_pbr_cpp.py into pbr_gen.h).
//
// 1. White furnace: a white surface (albedo 1) under a uniform white sky must not send back more light than it gets.
//    For each view angle and roughness: directional albedo  rho(V) = INTEGRAL f(L,V) cos(theta_L) dL  over the hemisphere,
//    where the shader functions return PI * f * NdotL * lightColor (lightColor = 1), so rho = SUM(value) * (2 PI / N) / PI.
//    rho > 1 = energy gained (a bug); rho well below 1 = energy lost (rough surfaces lose some by design: no multi-scatter).
//    Checked: PBR (full pass), PBRSun (sun pass, disc point), and the SPLIT passes PBRDiffuse + PBRSpecular (multi-pass).
// 2. Invalid values: every function on random and on degenerate inputs (V = -L, N = L, N = -L, grazing, roughness 0,
//    zero vectors); counts NaN, Inf and negative results.
#include <cstdio>
#include <cstdint>
#include <cmath>
#include <vector>
#include <random>
#include "pbr_gen.h"

typedef float3 (*Fn)(float, float, float3, float3, float3, float3, float3);
static float3 Split(float m, float r, float3 a, float3 n, float3 v, float3 l, float3 c) { return PBRDiffuse(m, r, a, n, v, l, c) + PBRSpecular(m, r, a, n, v, l, c); }
struct Named { const char* name; Fn fn; };
static const Named Fns[] = { { "PBR (full pass)", PBR }, { "PBRSun (sun pass)", PBRSun }, { "PBRDiffuse+PBRSpecular (split passes)", Split },
	{ "PBRDiffuse alone", PBRDiffuse }, { "PBRSpecular alone", PBRSpecular }, { "PBRSunSpecular alone", PBRSunSpecular } };

static float Rho(Fn fn, float metal, float rough, float nv, int nTheta, int nPhi) {
	const float3 N(0, 0, 1), V(std::sqrt(1 - nv * nv), 0, nv);
	double sum = 0;
	for (int i = 0; i < nTheta; i++) {
		const float ct = (i + 0.5f) / nTheta, st = std::sqrt(1 - ct * ct);
		for (int j = 0; j < nPhi; j++) {
			const float ph = (j + 0.5f) / nPhi * 2 * PI;
			const float3 L(st * std::cos(ph), st * std::sin(ph), ct);
			sum += fn(metal, rough, float3(1.0f), N, V, L, float3(1.0f)).y;
		}
	}
	return (float)(sum * 2.0 / ((double)nTheta * nPhi));  // (2 PI / N) / PI
}

static bool Bad(float3 c, int& nan, int& inf, int& neg) {
	bool b = false;
	for (float v : { c.x, c.y, c.z }) {
		if (std::isnan(v)) { nan++; b = true; }
		else if (std::isinf(v)) { inf++; b = true; }
		else if (v < 0) { neg++; b = true; }
	}
	return b;
}

int main(int argc, char** argv) {
	const int nTheta = argc > 1 ? atoi(argv[1]) : 1024, nPhi = 2 * nTheta;
	printf("NVR shading math furnace (PBR.hlsl via pbr_gen.h), %d x %d directions per point\n\n", nTheta, nPhi);
	const float roughs[] = { 0.1f, 0.2f, 0.3f, 0.5f, 0.7f, 1.0f };
	const float nvs[] = { 0.05f, 0.1f, 0.25f, 0.5f, 0.75f, 1.0f };
	for (int f = 0; f < 3; f++) {
		for (float metal : { 0.0f, 1.0f }) {
			printf("WHITE FURNACE  %s, %s (albedo 1): directional albedo rho, rows roughness, columns N.V\n", Fns[f].name, metal ? "metal (F0 = 1)" : "dielectric (F0 0.04)");
			printf("  rough \\ NdotV ");
			for (float nv : nvs) printf("%7.2f", nv);
			printf("\n");
			float worst = 0;
			for (float r : roughs) {
				printf("  %5.2f         ", r);
				for (float nv : nvs) { const float rho = Rho(Fns[f].fn, metal, r, nv, nTheta, nPhi); worst = rho > worst ? rho : worst; printf("%7.3f", rho); }
				printf("\n");
			}
			printf("  max %.3f %s\n\n", worst, worst > 1.005f ? "<-- MORE LIGHT OUT THAN IN" : "(no gain)");
		}
	}

	// Invalid values
	std::mt19937 rng(1234);
	std::uniform_real_distribution<float> u(-1, 1), u01(0, 1);
	auto randDir = [&]() { float3 d; do d = float3(u(rng), u(rng), u(rng)); while (dot(d, d) < 1e-4f || dot(d, d) > 1); return normalize(d); };
	struct Case { const char* what; float3 n, v, l; float rough, metal, albedo; };
	std::vector<Case> cases;
	const float3 z(0, 0, 1), x(1, 0, 0);
	cases.push_back({ "V = -L exactly (eye opposite the light)", z, normalize(float3(0.6f, 0, 0.8f)), normalize(float3(-0.6f, 0, -0.8f)), 0.5f, 0, 1 });
	cases.push_back({ "N = L exactly (light straight on)", z, normalize(float3(0.6f, 0, 0.8f)), z, 0.5f, 0, 1 });
	cases.push_back({ "N = -L exactly (light straight behind)", z, normalize(float3(0.6f, 0, 0.8f)), -z, 0.5f, 0, 1 });
	cases.push_back({ "grazing view (N.V = 0)", z, x, normalize(float3(0.3f, 0.2f, 0.9f)), 0.5f, 0, 1 });
	cases.push_back({ "grazing light (N.L = 0)", z, normalize(float3(0.3f, 0.2f, 0.9f)), x, 0.5f, 0, 1 });
	cases.push_back({ "roughness 0", z, normalize(float3(0.3f, 0, 0.95f)), normalize(float3(-0.3f, 0, 0.95f)), 0.0f, 0, 1 });
	cases.push_back({ "roughness 0, metal 1", z, normalize(float3(0.3f, 0, 0.95f)), normalize(float3(-0.3f, 0, 0.95f)), 0.0f, 1, 1 });
	cases.push_back({ "zero eye vector", z, float3(0.0f), normalize(float3(0.3f, 0, 0.95f)), 0.5f, 0, 1 });
	cases.push_back({ "zero normal", float3(0.0f), z, z, 0.5f, 0, 1 });
	cases.push_back({ "L = V = N", z, z, z, 0.5f, 0, 1 });
	printf("INVALID VALUES on degenerate inputs (NaN / Inf / negative per function)\n");
	for (const Case& c : cases) {
		printf("  %-42s", c.what);
		for (const Named& fn : Fns) {
			int nan = 0, inf = 0, neg = 0;
			Bad(fn.fn(c.metal, c.rough, float3(c.albedo), c.n, c.v, c.l, float3(1.0f)), nan, inf, neg);
			if (nan || inf || neg) printf(" | %s:%s%s%s", fn.name, nan ? " NaN" : "", inf ? " Inf" : "", neg ? " NEG" : "");
		}
		printf("\n");
	}
	printf("\nINVALID VALUES on 1,000,000 random inputs per function (unit N, V, L anywhere on the sphere, roughness 0..1, metal 0/1)\n");
	for (const Named& fn : Fns) {
		int nan = 0, inf = 0, neg = 0, bad = 0;
		float maxv = 0;
		for (int i = 0; i < 1000000; i++) {
			const float3 n = randDir(), v = randDir(), l = randDir();
			const float3 c = fn.fn(u01(rng) < 0.5f ? 0.0f : 1.0f, u01(rng), float3(u01(rng)), n, v, l, float3(1.0f));
			if (Bad(c, nan, inf, neg)) bad++;
			else maxv = c.y > maxv ? c.y : maxv;
		}
		printf("  %-40s bad %d (NaN %d, Inf %d, negative %d), largest finite %.1f\n", fn.name, bad, nan, inf, neg, maxv);
	}
	return 0;
}
