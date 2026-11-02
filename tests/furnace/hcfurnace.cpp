// SmithCorrelated check (2026-10-18, helper 'pbrport'; idea from Physically-Based-Reloaded (Balebandro), GPL-3.0, commit 4e64984b).
// Scores Shaders\Includes\PBR.hlsl with PBR_SMITH_CORRELATED 1 against the limits written BEFORE the change
// (bench\deep-roadmap\evidence\pbrport-LOG.md, limit A). Runs the REAL shader code on the CPU:
//   pbr_on.h   = this tree's PBR.hlsl, PBR_SMITH_CORRELATED 1   (gen_pbr_cpp.py <PBR.hlsl> pbr_on.h onpbr, then the define on top)
//   pbr_off.h  = this tree's PBR.hlsl, PBR_SMITH_CORRELATED 0   (same, define 0)
//   pbr_base.h = PBR.hlsl of deep/candidate a3bc6d85 (before the change)
// A1  white furnace of a white metal (F0 = 1), specular lobe alone (PBRSpecular), half-vector importance sampling, against an
//     independent double-precision reference: exact GGX D, exact height-correlated Smith G2 (Heitz 2014 eq. 99, Lambda form).
//     PASS: ON within +-2% of the reference at every point of r {0.1..1} x N.V {0.1..1}, and OFF more than 5% off somewhere in
//     r 0.2-0.7. Full pass (PBR) and sun pass (PBRSun) printed too.
// A2  no NaN / Inf / negative on 1,000,000 random calls of PBR, PBRSpecular, PBRSun, PBRSunSpecular ON, plus degenerate inputs.
//     And OFF = the old code bit for bit (1,000,000 random calls x 5 functions).
// A3  (reported) firefly figure: largest single-light pi f N.L at the N.V clamp (V on or behind the tangent plane), ON vs OFF;
//     white dielectric total furnace (PBR full pass, F0 0.04, albedo 1), ON vs OFF (stop if ON > 1.15).
// Build (VS2019 x64 prompt): cl /nologo /O2 /EHsc /fp:precise hcfurnace.cpp      Run: hcfurnace.exe
#include <cstdio>
#include <cmath>
#include <random>
#include <vector>
#include <cstring>
#undef PBR_SMITH_CORRELATED
#define PBR_SMITH_CORRELATED 1
#include "pbr_on.h"
#undef PBR_SMITH_CORRELATED
#define PBR_SMITH_CORRELATED 0
#include "pbr_off.h"
#include "pbr_base.h"

typedef float3 (*Fn)(float, float, float3, float3, float3, float3, float3);
static const double kPi = 3.14159265358979;

// Independent reference: GGX D x height-correlated Smith G2 / (4 NV NL), F = 1, double precision.
static double RefSpec(double r, double nv, double nl, double nh) {
	const double a = r * r, a2 = a * a, d = nh * nh * (a2 - 1) + 1, D = a2 / (kPi * d * d);
	const double lv = (-1 + std::sqrt(1 + a2 * (1 - nv * nv) / (nv * nv))) / 2, ll = (-1 + std::sqrt(1 + a2 * (1 - nl * nl) / (nl * nl))) / 2;
	return D / (1 + lv + ll) / (4 * nv * nl);
}

// Half-vector importance sampling (pdf(L) = D NH / (4 VH)); value = pi f NL (the shader convention, white light).
template <class F> static double RhoIS(F f, double rough, double nv, int n) {
	const double a = rough * rough, a2 = a * a; double s = 0;
	const double vx = std::sqrt(std::fmax(0.0, 1 - nv * nv));
	for (int i = 0; i < n; i++) for (int j = 0; j < n; j++) {
		const double u1 = (i + 0.5) / n, u2 = (j + 0.5) / n, t2 = a2 * u1 / (1 - u1), ct = 1 / std::sqrt(1 + t2), st = std::sqrt(1 - ct * ct), ph = 2 * kPi * u2;
		const double hx = st * std::cos(ph), hy = st * std::sin(ph), hz = ct;
		const double vh = vx * hx + nv * hz; if (vh <= 0) continue;
		const double lx = 2 * vh * hx - vx, ly = 2 * vh * hy, lz = 2 * vh * hz - nv; if (lz <= 0) continue;
		const double dd = ct * ct * (a2 - 1) + 1, D = a2 / (kPi * dd * dd);
		s += f(lx, ly, lz, ct) / kPi / (D * ct / (4 * vh));
	}
	return s / ((double)n * n);
}
static double RhoShader(Fn fn, float metal, float albedo, float rough, float nv, int n) {
	const float3 N(0, 0, 1), V(std::sqrt(std::fmax(0.f, 1 - nv * nv)), 0, nv);
	return RhoIS([&](double lx, double ly, double lz, double) { return (double)fn(metal, rough, float3(albedo), N, V, float3((float)lx, (float)ly, (float)lz), float3(1.0f)).y; }, rough, nv, n);
}
static double RhoRef(double rough, double nv, int n) {
	const double vx = std::sqrt(std::fmax(0.0, 1 - nv * nv));
	return RhoIS([&](double lx, double ly, double lz, double nh) { return kPi * RefSpec(rough, nv, lz, nh) * lz; }, rough, nv, n);
}
// Uniform hemisphere grid (for the broad diffuse part).
template <class F> static double RhoUniform(F f, int n) {
	double s = 0;
	for (int i = 0; i < n; i++) { const double ct = (i + 0.5) / n, st = std::sqrt(1 - ct * ct);
		for (int j = 0; j < 2 * n; j++) { const double ph = (j + 0.5) / (2 * n) * 2 * kPi; s += f(st * std::cos(ph), st * std::sin(ph), ct); } }
	return s * 2.0 / ((double)n * 2 * n);
}
static bool Bad(float3 c) { for (float v : { c.x, c.y, c.z }) if (std::isnan(v) || std::isinf(v) || v < 0) return true; return false; }
static bool Same(float3 a, float3 b) { return !memcmp(&a, &b, sizeof(float3)); }

int main(int argc, char** argv) {
	const int nIS = argc > 1 ? atoi(argv[1]) : 768, nUni = argc > 2 ? atoi(argv[2]) : 1024;
	int fails = 0;
	auto verdict = [&](const char* id, bool pass, const char* what) { printf("%s %s  %s\n", id, pass ? "PASS" : "FAIL", what); if (!pass) fails++; };
	const float roughs[] = { 0.1f, 0.15f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f, 0.7f, 0.85f, 1.0f };
	const float nvs[] = { 0.1f, 0.15f, 0.2f, 0.3f, 0.4f, 0.5f, 0.75f, 1.0f };

	// ---- A1 ----
	printf("A1 WHITE FURNACE, white metal (F0 = 1), specular lobe (PBRSpecular): ON / reference - 1 [OFF / reference - 1], reference = exact GGX + height-correlated Smith\n   rough\\NdotV");
	for (float nv : nvs) printf("            %4.2f", nv);
	printf("\n");
	double onWorst = 0, offWorstMid = 0; char buf[256];
	for (float r : roughs) {
		printf("   %5.2f     ", r);
		for (float nv : nvs) {
			const double ref = RhoRef(r, nv, nIS);
			const double on = RhoShader(onpbr::PBRSpecular, 1, 1, r, nv, nIS), off = RhoShader(offpbr::PBRSpecular, 1, 1, r, nv, nIS);
			const double eOn = on / ref - 1, eOff = off / ref - 1;
			onWorst = std::fmax(onWorst, std::fabs(eOn));
			if (r >= 0.2f && r <= 0.7f) offWorstMid = std::fmax(offWorstMid, std::fabs(eOff));
			printf(" %+6.1f%% [%+5.0f%%]", 100 * eOn, 100 * eOff);
		}
		printf("\n");
	}
	printf("   reference albedo E (F0 = 1) for the record:\n");
	for (float r : { 0.1f, 0.3f, 0.5f, 0.7f, 1.0f }) { printf("   %5.2f     ", r); for (float nv : nvs) printf("  %6.3f         ", RhoRef(r, nv, nIS)); printf("\n"); }
	snprintf(buf, sizeof buf, "ON within +-2%% of the reference everywhere (worst %.2f%%)", 100 * onWorst); verdict("A1a", onWorst <= 0.02, buf);
	snprintf(buf, sizeof buf, "OFF misses the reference by more than 5%% somewhere in r 0.2-0.7 (worst %.1f%%)", 100 * offWorstMid); verdict("A1b", offWorstMid > 0.05, buf);
	printf("   info: full pass PBR / sun pass PBRSun, white metal, ON [OFF] (sun disc point: not energy-normalised, research notes 10.1)\n");
	for (float r : { 0.2f, 0.3f, 0.5f, 0.65f, 1.0f }) {
		printf("   %5.2f     ", r);
		for (float nv : { 0.1f, 0.3f, 0.5f, 1.0f }) printf(" PBR %.3f [%.3f]", RhoShader(onpbr::PBR, 1, 1, r, nv, nIS), RhoShader(offpbr::PBR, 1, 1, r, nv, nIS));
		printf("\n");
	}

	// ---- A2 ----
	std::mt19937 rng(12345); std::uniform_real_distribution<float> U(-1, 1), U01(0, 1);
	auto randDir = [&]() { for (;;) { float3 d(U(rng), U(rng), U(rng)); const float l = dot(d, d); if (l > 1e-4f && l <= 1) return d * (1.0f / std::sqrt(l)); } };
	Fn onF[] = { onpbr::PBR, onpbr::PBRSpecular, onpbr::PBRSun, onpbr::PBRSunSpecular, onpbr::PBRDiffuse };
	Fn offF[] = { offpbr::PBR, offpbr::PBRSpecular, offpbr::PBRSun, offpbr::PBRSunSpecular, offpbr::PBRDiffuse };
	Fn baseF[] = { basepbr::PBR, basepbr::PBRSpecular, basepbr::PBRSun, basepbr::PBRSunSpecular, basepbr::PBRDiffuse };
	long long badOnNew = 0, badOff = 0, diffOffBase = 0, calls = 0;
	for (int k = 0; k < 1000000; k++) {
		float3 N = randDir(), V = randDir(), L = randDir();
		const int mode = k % 8;	// degenerate geometries the shaders meet
		if (mode == 1) V = L * -1.0f;                                  // light straight behind the surface point
		if (mode == 2) V = normalize(V - N * dot(V, N));               // view exactly in the tangent plane (N.V clamp)
		if (mode == 3) { V = normalize(V - N * dot(V, N)); L = normalize(V * -1.0f + N * 1e-3f); }   // the grazing mirror corner
		if (mode == 4) L = N;
		const float r = mode == 5 ? 0.02f : 0.02f + 0.98f * U01(rng), m = U01(rng) < 0.5f ? 1.0f : U01(rng);
		const float3 alb(U01(rng), U01(rng), U01(rng)), col(1.0f);
		for (int f = 0; f < 5; f++) {
			const float3 on = onF[f](m, r, alb, N, V, L, col), off = offF[f](m, r, alb, N, V, L, col), base = baseF[f](m, r, alb, N, V, L, col);
			calls++;
			if (Bad(on) && !Bad(off)) badOnNew++;
			if (Bad(off)) badOff++;
			if (!Same(off, base) && !(std::isnan(off.x) && std::isnan(base.x))) diffOffBase++;
		}
	}
	snprintf(buf, sizeof buf, "ON: %lld new NaN/Inf/negative results in %lld calls (OFF itself: %lld, the old code's own V = -L singularity)", badOnNew, calls, badOff);
	verdict("A2a", badOnNew == 0, buf);
	snprintf(buf, sizeof buf, "OFF = the old code bit for bit: %lld of %lld results differ", diffOffBase, calls); verdict("A2b", diffOffBase == 0, buf);

	// ---- A3 ----
	printf("A3 firefly figure: largest pi f N.L of one white light at the N.V clamp (V in the tangent plane), white metal, ON [OFF]\n");
	for (float r : { 0.043f, 0.1f, 0.2f, 0.3f, 0.5f, 1.0f }) {
		const float3 N(0, 0, 1), V(1, 0, 0);
		double mOn = 0, mOff = 0; float atT = 0, atP = 0;
		for (int i = 1; i <= 4000; i++) for (int j = 0; j <= 400; j++) {
			// L near the grazing mirror corner -V: polar angle up to 90 deg, azimuth around -x, finer close to it
			const float t = (float)(kPi / 2) * (1 - std::pow(i / 4000.0f, 3.0f)), p = (float)kPi + (float)kPi * std::pow(j / 400.0f, 3.0f);
			const float3 L(std::sin(t) * std::cos(p), std::sin(t) * std::sin(p), std::cos(t));
			if (L.z <= 0) continue;
			const double on = onpbr::PBRSpecular(1, r, float3(1.0f), N, V, L, float3(1.0f)).y, off = offpbr::PBRSpecular(1, r, float3(1.0f), N, V, L, float3(1.0f)).y;
			if (on > mOn) { mOn = on; atT = t; atP = p; }
			mOff = std::fmax(mOff, off);
		}
		printf("   r %.3f: ON %.4g (at N.L %.2g)  [OFF %.4g]   ratio %.0fx\n", r, mOn, std::cos(atT), mOff, mOn / std::fmax(mOff, 1e-30));
	}
	printf("A3 white DIELECTRIC total furnace (PBR full pass, F0 0.04, albedo 1): diffuse part uniform grid + specular lobe importance sampled, ON [OFF]\n   rough\\NdotV");
	const float nv2[] = { 0.06f, 0.1f, 0.2f, 0.3f, 0.5f, 0.75f, 1.0f };
	for (float nv : nv2) printf("          %4.2f", nv);
	printf("\n");
	double worstTot = 0, worstTotOff = 0;
	for (float r : { 0.05f, 0.1f, 0.2f, 0.3f, 0.45f, 0.6f, 0.8f, 1.0f }) {
		printf("   %5.2f     ", r);
		for (float nv : nv2) {
			const float3 N(0, 0, 1), V(std::sqrt(1 - nv * nv), 0, nv);
			double tot[2];
			Fn full[2] = { onpbr::PBR, offpbr::PBR }, spec[2] = { onpbr::PBRSpecular, offpbr::PBRSpecular };
			onpbr::PBRDiffuseKeep = 1.0f; offpbr::PBRDiffuseKeep = 1.0f;	// A3: the masking term alone, no coupled diffuse
			onpbr::PBRDiffuseKeep = 1.0f; offpbr::PBRDiffuseKeep = 1.0f;	// A3: the masking term alone, no coupled diffuse
			for (int w = 0; w < 2; w++) {
				const double diff = RhoUniform([&](double x, double y, double z) { const float3 L((float)x, (float)y, (float)z);
					return (double)(full[w](0, r, float3(1.0f), N, V, L, float3(1.0f)).y - spec[w](0, r, float3(1.0f), N, V, L, float3(1.0f)).y); }, nUni);
				tot[w] = diff + RhoShader(spec[w], 0, 1, r, nv, nIS);
			}
			worstTot = std::fmax(worstTot, tot[0]); worstTotOff = std::fmax(worstTotOff, tot[1]);
			printf(" %.3f [%.3f]", tot[0], tot[1]);
		}
		printf("\n");
	}
	snprintf(buf, sizeof buf, "white dielectric total: ON max %.3f, OFF max %.3f (stop rule: ON > 1.15)", worstTot, worstTotOff);
	printf("A3 %s  %s   (the 01:55 stop; answered by the coupled diffuse, A5)\n", worstTot <= 1.15 ? "PASS" : "FAIL (expected, not counted)", buf);

	// ---- A5: the same furnace with the coupled diffuse (SetDiffuseEnergyKeep, as the object shaders call it once per pixel) ----
	auto total = [&](bool on, float r, float nv, int nu, int ni) {
		const float3 N(0, 0, 1), V(std::sqrt(std::fmax(0.f, 1 - nv * nv)), 0, nv);
		Fn full = on ? onpbr::PBR : offpbr::PBR, spec = on ? onpbr::PBRSpecular : offpbr::PBRSpecular;
		if (on) onpbr::SetDiffuseEnergyKeep(N, V, r); else offpbr::SetDiffuseEnergyKeep(N, V, r);
		const double diff = RhoUniform([&](double x, double y, double z) { const float3 L((float)x, (float)y, (float)z);
			return (double)(full(0, r, float3(1.0f), N, V, L, float3(1.0f)).y - spec(0, r, float3(1.0f), N, V, L, float3(1.0f)).y); }, nu);
		return diff + RhoShader(spec, 0, 1, r, nv, ni);
	};
	{
		const int nG = 31; double mxOn = 0, mnOn = 9, mxOff = 0, mnOff = 9; float atR = 0, atV = 0, atRm = 0, atVm = 0;
		for (int ir = 0; ir < nG; ir++) for (int iv = 0; iv < nG; iv++) {
			const float r = 0.05f + 0.95f * ir / (nG - 1), nv = 0.05f + 0.95f * iv / (nG - 1);
			const double on = total(true, r, nv, 384, 256), off = total(false, r, nv, 384, 256);
			if (on > mxOn) { mxOn = on; atR = r; atV = nv; }
			if (on < mnOn) { mnOn = on; atRm = r; atVm = nv; }
			mxOff = std::fmax(mxOff, off); mnOff = std::fmin(mnOff, off);
		}
		snprintf(buf, sizeof buf, "full pass, white dielectric, coupled diffuse, %dx%d grid r 0.05-1 x N.V 0.05-1: ON max %.4f (r %.3f N.V %.3f) <= 1.02", nG, nG, mxOn, atR, atV);
		verdict("A5a", mxOn <= 1.02, buf);
		snprintf(buf, sizeof buf, "ON min %.4f (r %.3f N.V %.3f) >= OFF min %.4f - 0.01  (OFF max %.4f; OFF = EnergySplit's live switch at 0)", mnOn, atRm, atVm, mnOff, mxOff);
		verdict("A5b", mnOn >= mnOff - 0.01, buf);
		printf("   below the A5 grid (information): r 0.02/0.03/0.043 x N.V 0.02/0.05/0.2/1:");
		for (float r : { 0.02f, 0.03f, 0.043f }) for (float nv : { 0.02f, 0.05f, 0.2f, 1.0f }) printf(" %.3f", total(true, r, nv, 384, 256));
		printf("\n");
	}
	{
		// the sun pass: diffuse x N.L + highlight x N.S (disc point), uniform grid only (the disc point moves the lobe), r >= 0.1
		double mxOn = 0, mnOn = 9, mnOff = 9, mxOff = 0;
		printf("A5 sun pass (PBRSun), white dielectric, coupled diffuse, ON [OFF], N.V 0.05 0.1 0.2 0.3 0.5 0.75:\n");
		for (float r : { 0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f, 0.8f, 1.0f }) {
			printf("   r %.2f", r);
			for (float nv : { 0.05f, 0.1f, 0.2f, 0.3f, 0.5f, 0.75f }) {
				const float3 N(0, 0, 1), V(std::sqrt(1 - nv * nv), 0, nv);
				double v[2];
				for (int w = 0; w < 2; w++) {
					Fn sun = w == 0 ? onpbr::PBRSun : offpbr::PBRSun;
					if (w == 0) onpbr::SetDiffuseEnergyKeep(N, V, r); else offpbr::SetDiffuseEnergyKeep(N, V, r);
					v[w] = RhoUniform([&](double x, double y, double z) { return (double)sun(0, r, float3(1.0f), N, V, float3((float)x, (float)y, (float)z), float3(1.0f)).y; }, r <= 0.2f ? 4096 : 1536);	// 4096 for the narrow lobes: 1536 under-sampled r 0.1 at N.V 0.05 (0.825 vs 0.983, sunprobe.cpp)
				}
				mxOn = std::fmax(mxOn, v[0]); mnOn = std::fmin(mnOn, v[0]); mnOff = std::fmin(mnOff, v[1]); mxOff = std::fmax(mxOff, v[1]);
				printf("  %.3f [%.3f]", v[0], v[1]);
			}
			printf("\n");
		}
		snprintf(buf, sizeof buf, "sun pass r 0.1-1 x N.V 0.05-0.75: ON max %.4f <= 1.02, min %.4f >= OFF min %.4f - 0.01 (OFF max %.4f)", mxOn, mnOn, mnOff, mxOff);
		verdict("A5c", mxOn <= 1.02 && mnOn >= mnOff - 0.01, buf);
	}
	{
		// A6: metal tables unchanged by the keep (the keep only scales the (1 - metal) diffuse share)
		double worst = 0;
		for (float r : { 0.1f, 0.3f, 0.5f, 0.7f, 1.0f }) for (float nv : { 0.1f, 0.3f, 0.6f, 1.0f }) {
			const float3 N(0, 0, 1), V(std::sqrt(1 - nv * nv), 0, nv);
			onpbr::PBRDiffuseKeep = 1.0f; const double a = RhoShader(onpbr::PBR, 1, 1, r, nv, 256);
			onpbr::SetDiffuseEnergyKeep(N, V, r); const double b = RhoShader(onpbr::PBR, 1, 1, r, nv, 256);
			worst = std::fmax(worst, std::fabs(a - b));
		}
		snprintf(buf, sizeof buf, "white metal full pass with vs without the keep: largest change %.6f (<= 0.001)", worst);
		verdict("A6", worst <= 0.001, buf);
	}
	{
		// A8: split passes (PBRDiffuse + PBRSpecular): no coupling possible there (reported)
		printf("A8 split passes (PBRDiffuse + PBRSpecular), white dielectric, ON [OFF] (reported, not a limit), N.V 0.06 0.1 0.2 0.3 0.5 1:\n");
		for (float r : { 0.05f, 0.1f, 0.2f, 0.3f, 0.5f, 0.8f }) {
			printf("   r %.2f", r);
			for (float nv : { 0.06f, 0.1f, 0.2f, 0.3f, 0.5f, 1.0f }) {
				const float3 N(0, 0, 1), V(std::sqrt(1 - nv * nv), 0, nv);
				double v[2];
				for (int w = 0; w < 2; w++) {
					Fn dif = w == 0 ? onpbr::PBRDiffuse : offpbr::PBRDiffuse, spec = w == 0 ? onpbr::PBRSpecular : offpbr::PBRSpecular;
					v[w] = RhoUniform([&](double x, double y, double z) { return (double)dif(0, r, float3(1.0f), N, V, float3((float)x, (float)y, (float)z), float3(1.0f)).y; }, 256)
						+ RhoShader(spec, 0, 1, r, nv, 256);
				}
				printf("  %.3f [%.3f]", v[0], v[1]);
			}
			printf("\n");
		}
	}
	printf("\n%s: %d limit(s) failed\n", fails ? "FAIL" : "ALL PASS", fails);
	return fails;
}
