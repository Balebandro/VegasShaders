#pragma once

// UNOFFICIAL 2026-10-20 cloth round 9 (user: 'can our cloth have some sort of faux physics simulation when we walk? right now it still
// kinda just follows our player'): a small simulation of a worn coat's hem, per wearer, on the CPU. No game types here, so
// tests\cloth_walk_test.cpp runs the same code against the offline prototype (bench\deep-roadmap\evidence\cloth3\cloth_walk_check.py,
// which this mirrors line by line; evidence\cloth3-LOG.md C3-15).
//
// Why the coat 'follows': the garment's own skin weights glue the skirt to the leg bones (the Combat Ranger coat at z 45-60 is 88 %
// thigh bones, at z 15-30 78 % calf bones), so it moves like trousers; the old walking part only added one offset for the whole coat.
// ClothWind.h makes the coat HANG FROM THE HIPS (each coat vertex blended from its skinned place towards the pelvis bone's alone, by its
// cloth weight) and moves it by this ring:
//  - 16 points around the wearer's upright axis (one per 22.5 degrees), each at the coat's loosest vertices in that direction (missing
//    directions -- an open front -- are filled in from their neighbours); each hangs from a point halfway in at hip height;
//  - every frame each point follows its spot on the hips-hung coat through a damped spring (Spring Hz, Damping; Drag: how much of the
//    damping acts on the speed through the air, so the hem trails behind when walking), in steps of at most 1/120 s, then is pushed out
//    of the CURRENT legs (thigh and calf capsules, the garment's measured radius + LegMargin), keeps half-way to its rest distance from
//    the next point, and may not hang further than 3 % past its length from its hip point;
//  - the displacement of each point from its spot (at most MaxSwing units) is what the shader adds, by angle and cloth weight.
// So the knee kicks the hem forward at every step, the calf pushes it back, the hips' sway swings it, it trails, and it swings past
// and settles after a stop.
// 2026-10-20 round 9b (user's video: 'moves a bit too dramatically when we run and has a clear cut off point'; 'we should simulate
// running and jumping too'): the air drag sees the speed through a soft cap (DragCap: a run trailed 16+ units, now ~8), each point may
// swing at most MaxAngle (35) degrees off its hanging direction (the back of the coat flew out level at a run), the displacement's limit
// is soft (MaxSwing x tanh), and each vertex gets a PENDULUM share (Ring::swing: 0 at the hips rising evenly to 1 at the hem) for the
// ring's displacement instead of its cloth weight (full from the knee down: the lower coat moved as one stiff block, folded at the knee).
// Checked offline on a synthetic walk, run (350 units/s) and standing jump (cloth_walk_check.py, walk-v8).
// 2026-10-20 round 9c (user: 'can we tune cloth a little bit so it doesn't take such sharp turns maybe some kind of smoothing on those
// vertexes', 'so we don't get those sharp angles when we turn and move pretty fast'; evidence\cloth3\crease_check.py): the ring's 16
// displacements are smoothed round the hem before use (HemSmooth: the mean kept, the one-wave part -- lean and turn -- at half, finer
// waves dropped; neighbouring points 4 units apart moved 12-19 units differently and sheared the coat's big triangles over), and the
// pendulum share starts at 0 where the moving cloth starts in each direction and eases in (it began at ~0.2 on the cloth's top edge: a
// one-row step under the waist that creased when turning). Sharp folds (> 30 degrees sharper than the mesh's own) on the Combat Ranger
// coat when running / turning / jumping while turning: 6.6 / 8.0 / 7.7 % of its edges -> 3.5 / 4.6 / 4.6 % (the game's own skinned
// coat: ~9 %); the hem still moves ~85 % as far; no deeper into the legs.
#include <vector>
#include <cmath>
#include <cstdint>
#include <algorithm>

namespace ClothWalk {

	constexpr int kRing = 16;
	constexpr double kPi = 3.14159265358979323846;

	struct Ring {
		bool ok = false;
		double c[2] = {};					// the wearer's upright axis (bind pose, between the hip joints)
		double p[kRing][3] = {};			// the hem points (bind pose)
		double a[kRing][3] = {};			// what each hangs from (halfway in, at hip height)
		double L[kRing] = {};				// their lengths
		bool present[kRing] = {};
		std::vector<float> u;				// per vertex: its place around the ring (point i at u = i; 0..16)
		std::vector<float> swing;			// per vertex: its pendulum share of the ring's displacement (0 where the cloth starts .. 1 hem, at most 1.2; 0 off the cloth)
	};

	// pos: xyz per vertex (bind pose); weight / label: ClothWeights.h's (label 2 = cloth)
	inline bool BuildRing(const float* pos, uint32_t n, const float* weight, const uint8_t* label, const float center[2], float hipZ, Ring& r) {
		r = Ring();
		r.c[0] = center[0]; r.c[1] = center[1];
		r.u.resize(n);
		std::vector<int> sector(n);
		std::vector<double> ud(n);
		for (uint32_t v = 0; v < n; v++) {
			const double ang = std::atan2((double)pos[v * 3 + 1] - r.c[1], (double)pos[v * 3] - r.c[0]);
			double u = std::fmod(ang / (2.0 * kPi) * kRing - 0.5, (double)kRing); if (u < 0.0) u += kRing;
			r.u[v] = (float)u; ud[v] = u;
			double s = std::fmod(ang / (2.0 * kPi) * kRing, (double)kRing); if (s < 0.0) s += kRing;
			sector[v] = (int)std::floor(s) % kRing;
		}
		double rad[kRing] = {}, z[kRing] = {};
		for (int i = 0; i < kRing; i++) {
			double mx = -1.0; int count = 0;
			for (uint32_t v = 0; v < n; v++) if (sector[v] == i && label[v] == 2 && weight[v] > 0.3f) { count++; mx = (std::max)(mx, (double)weight[v]); }
			if (count < 3) continue;
			double q[3] = {}; int m = 0;
			for (uint32_t v = 0; v < n; v++)
				if (sector[v] == i && label[v] == 2 && weight[v] > 0.3f && weight[v] >= 0.95 * mx) { for (int k = 0; k < 3; k++) q[k] += pos[v * 3 + k]; m++; }
			for (double& x : q) x /= m;
			rad[i] = std::hypot(q[0] - r.c[0], q[1] - r.c[1]); z[i] = q[2]; r.present[i] = true;
		}
		bool any = false;
		for (int i = 0; i < kRing; i++) any = any || r.present[i];
		if (!any) return false;
		for (int i = 0; i < kRing; i++) {	// missing directions (an open front): between the nearest present ones
			if (r.present[i]) continue;
			int a = -1, b = -1, da = 99, db = 99;
			for (int j = 0; j < kRing; j++) {
				if (!r.present[j]) continue;
				const int up = ((j - i) % kRing + kRing) % kRing, down = ((i - j) % kRing + kRing) % kRing;
				if (up < da) { da = up; a = j; }
				if (down < db) { db = down; b = j; }
			}
			const double t = (double)db / (std::max)(da + db, 1);
			rad[i] = rad[b] + (rad[a] - rad[b]) * t; z[i] = z[b] + (z[a] - z[b]) * t;
		}
		for (int i = 0; i < kRing; i++) {
			const double th = (i + 0.5) * 2.0 * kPi / kRing;
			r.p[i][0] = r.c[0] + rad[i] * std::cos(th); r.p[i][1] = r.c[1] + rad[i] * std::sin(th); r.p[i][2] = z[i];
			r.a[i][0] = r.c[0] + 0.5 * rad[i] * std::cos(th); r.a[i][1] = r.c[1] + 0.5 * rad[i] * std::sin(th); r.a[i][2] = hipZ;
			const double d[3] = { r.p[i][0] - r.a[i][0], r.p[i][1] - r.a[i][1], r.p[i][2] - r.a[i][2] };
			r.L[i] = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
		}
		// round 9c: the top of the moving cloth in each direction (its highest vertex with weight > 0.05; missing directions filled as above)
		double zt[kRing]; bool hasT[kRing] = {};
		for (int i = 0; i < kRing; i++) {
			zt[i] = -1e30;
			for (uint32_t v = 0; v < n; v++) if (sector[v] == i && weight[v] > 0.05f) { zt[i] = (std::max)(zt[i], (double)pos[v * 3 + 2]); hasT[i] = true; }
		}
		bool anyT = false;
		for (int i = 0; i < kRing; i++) anyT = anyT || hasT[i];
		for (int i = 0; i < kRing; i++) {
			if (!anyT) { zt[i] = hipZ; continue; }
			if (hasT[i]) continue;
			int a = -1, b = -1, da = 99, db = 99;
			for (int j = 0; j < kRing; j++) {
				if (!hasT[j]) continue;
				const int up = ((j - i) % kRing + kRing) % kRing, down = ((i - j) % kRing + kRing) % kRing;
				if (up < da) { da = up; a = j; }
				if (down < db) { db = down; b = j; }
			}
			zt[i] = zt[b] + (zt[a] - zt[b]) * ((double)db / (std::max)(da + db, 1));
		}
		r.swing.resize(n);	// round 9b: the pendulum share (cloth only: fades in over the first 0.2 of the weight); 9c: from the cloth's top, eased in
		for (uint32_t v = 0; v < n; v++) {
			const double i0 = std::floor(ud[v]), f = ud[v] - i0;
			const int a = (int)i0 % kRing, b = (a + 1) % kRing;
			const double zr = z[a] * (1.0 - f) + z[b] * f;
			const double ztu = zt[a] * (1.0 - f) + zt[b] * f;
			double share = (ztu - pos[v * 3 + 2]) / (std::max)(ztu - zr, 1.0);
			share = share < 0.0 ? 0.0 : (share > 1.2 ? 1.2 : share);
			if (share <= 1.0) share = share * share * (2.0 - share);	// no slope at the top, slope 1 at the ring
			double wf = (double)weight[v] / 0.2; wf = wf < 0.0 ? 0.0 : (wf > 1.0 ? 1.0 : wf); wf = wf * wf * (3.0 - 2.0 * wf);
			r.swing[v] = (float)(share * wf);
		}
		r.ok = true;
		return true;
	}

	struct Params {
		double spring = 1.4, damping = 0.2, drag = 1.0, legMargin = 1.5, maxSwing = 25.0;
		double dragCap = 180.0, maxAngle = 35.0;	// round 9b
		double hemSmooth = 1.0;						// round 9c: 0 = as 9b, 1 = the ring smoothed round the hem (see the top)
		double stretch = 1.03, neighbour = 0.5;
		int iterations = 2;
	};
	struct Xf { double r[3][3]; double t[3]; };	// world = r q + t (r includes the scale)
	inline void Apply(const Xf& x, const double* q, double* o) {
		for (int i = 0; i < 3; i++) o[i] = x.r[i][0] * q[0] + x.r[i][1] * q[1] + x.r[i][2] * q[2] + x.t[i];
	}
	struct State {
		bool valid = false;
		double x[kRing][3] = {}, v[kRing][3] = {}, tPrev[kRing][3] = {};
	};

	inline void SegPush(double* x, const double* a, const double* b, double ra, double rb, double margin) {
		const double ab[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
		const double l2 = (std::max)(ab[0] * ab[0] + ab[1] * ab[1] + ab[2] * ab[2], 1e-6);
		double t = ((x[0] - a[0]) * ab[0] + (x[1] - a[1]) * ab[1] + (x[2] - a[2]) * ab[2]) / l2;
		t = t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t);
		const double q[3] = { a[0] + ab[0] * t, a[1] + ab[1] * t, a[2] + ab[2] * t };
		const double v[3] = { x[0] - q[0], x[1] - q[1], x[2] - q[2] };
		const double d = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
		const double R = ra + (rb - ra) * t + margin;
		if (d < R && d > 1e-6) for (int k = 0; k < 3; k++) x[k] = q[k] + v[k] / d * R;
	}

	// legs: L hip, knee, ankle, R hip, knee, ankle (world xyz, measured radius). out: each point's displacement (world).
	inline void Step(State& s, const Ring& r, const Xf& pelvis, const double legs[6][4], double dt, const Params& prm, double out[kRing][3]) {
		double tNow[kRing][3], A[kRing][3];
		for (int i = 0; i < kRing; i++) { Apply(pelvis, r.p[i], tNow[i]); Apply(pelvis, r.a[i], A[i]); }
		if (!s.valid || dt <= 0.0 || dt > 0.25) {
			for (int i = 0; i < kRing; i++) for (int k = 0; k < 3; k++) { s.x[i][k] = tNow[i][k]; s.v[i][k] = 0.0; s.tPrev[i][k] = tNow[i][k]; out[i][k] = 0.0; }
			s.valid = true;
			return;
		}
		const double w0 = 2.0 * kPi * prm.spring, c = 2.0 * prm.damping * w0;
		const double cmax = std::cos(prm.maxAngle * kPi / 180.0), smax = std::sin(prm.maxAngle * kPi / 180.0);
		double vt[kRing][3], vair[kRing][3], rest[kRing];
		for (int i = 0; i < kRing; i++) {
			for (int k = 0; k < 3; k++) vt[i][k] = (tNow[i][k] - s.tPrev[i][k]) / dt;
			const double vs = std::sqrt(vt[i][0] * vt[i][0] + vt[i][1] * vt[i][1] + vt[i][2] * vt[i][2]);
			const double sat = 1.0 / std::sqrt(1.0 + (vs / prm.dragCap) * (vs / prm.dragCap));	// the speed the air sees (soft cap)
			for (int k = 0; k < 3; k++) vair[i][k] = vt[i][k] * sat;
			const int j = (i + 1) % kRing;
			const double e[3] = { tNow[i][0] - tNow[j][0], tNow[i][1] - tNow[j][1], tNow[i][2] - tNow[j][2] };
			rest[i] = std::sqrt(e[0] * e[0] + e[1] * e[1] + e[2] * e[2]);
		}
		const int n = (std::min)((int)std::ceil(dt * 120.0), 8);
		const double h = dt / n;
		static const int segs[4][2] = { { 0, 1 }, { 1, 2 }, { 3, 4 }, { 4, 5 } };
		for (int st = 0; st < n; st++) {
			const double f = (double)(st + 1) / n;
			double xOld[kRing][3], ts[kRing][3];
			for (int i = 0; i < kRing; i++)
				for (int k = 0; k < 3; k++) {
					ts[i][k] = s.tPrev[i][k] + (tNow[i][k] - s.tPrev[i][k]) * f;
					s.v[i][k] += h * (w0 * w0 * (ts[i][k] - s.x[i][k]) - c * (s.v[i][k] - vt[i][k]) - c * prm.drag * vair[i][k]);
					xOld[i][k] = s.x[i][k];
					s.x[i][k] += s.v[i][k] * h;
				}
			for (int it = 0; it < prm.iterations; it++) {
				for (int i = 0; i < kRing; i++)
					for (const auto& sg : segs) SegPush(s.x[i], legs[sg[0]], legs[sg[1]], legs[sg[0]][3], legs[sg[1]][3], prm.legMargin);
				for (int i = 0; i < kRing; i++) {
					const int j = (i + 1) % kRing;
					const double e[3] = { s.x[j][0] - s.x[i][0], s.x[j][1] - s.x[i][1], s.x[j][2] - s.x[i][2] };
					const double l = std::sqrt(e[0] * e[0] + e[1] * e[1] + e[2] * e[2]);
					if (l > 1e-6) {
						const double k = (l - rest[i]) / l * 0.5 * prm.neighbour;
						for (int q = 0; q < 3; q++) { s.x[i][q] += k * e[q]; s.x[j][q] -= k * e[q]; }
					}
				}
				for (int i = 0; i < kRing; i++) {
					const double e[3] = { s.x[i][0] - A[i][0], s.x[i][1] - A[i][1], s.x[i][2] - A[i][2] };
					const double l = std::sqrt(e[0] * e[0] + e[1] * e[1] + e[2] * e[2]);
					if (l > prm.stretch * r.L[i]) for (int q = 0; q < 3; q++) s.x[i][q] = A[i][q] + e[q] * (prm.stretch * r.L[i] / l);
				}
				for (int i = 0; i < kRing; i++) {	// round 9b: at most maxAngle off its hanging direction (from its hip point)
					const double e[3] = { s.x[i][0] - A[i][0], s.x[i][1] - A[i][1], s.x[i][2] - A[i][2] };
					const double rr[3] = { ts[i][0] - A[i][0], ts[i][1] - A[i][1], ts[i][2] - A[i][2] };
					const double le = std::sqrt(e[0] * e[0] + e[1] * e[1] + e[2] * e[2]), lr = std::sqrt(rr[0] * rr[0] + rr[1] * rr[1] + rr[2] * rr[2]);
					if (le < 1e-6 || lr < 1e-6) continue;
					const double rh[3] = { rr[0] / lr, rr[1] / lr, rr[2] / lr };
					const double ca = (e[0] * rh[0] + e[1] * rh[1] + e[2] * rh[2]) / le;
					if (ca < cmax) {
						const double pp[3] = { e[0] / le - ca * rh[0], e[1] / le - ca * rh[1], e[2] / le - ca * rh[2] };
						const double pl = std::sqrt(pp[0] * pp[0] + pp[1] * pp[1] + pp[2] * pp[2]);
						if (pl > 1e-6) for (int q = 0; q < 3; q++) s.x[i][q] = A[i][q] + le * (cmax * rh[q] + smax * pp[q] / pl);
					}
				}
			}
			for (int i = 0; i < kRing; i++) for (int k = 0; k < 3; k++) s.v[i][k] = (s.x[i][k] - xOld[i][k]) / h;
		}
		for (int i = 0; i < kRing; i++) {
			for (int k = 0; k < 3; k++) { s.tPrev[i][k] = tNow[i][k]; out[i][k] = s.x[i][k] - tNow[i][k]; }
			const double l = std::sqrt(out[i][0] * out[i][0] + out[i][1] * out[i][1] + out[i][2] * out[i][2]);
			if (l > 1e-6 && prm.maxSwing > 0.0) { const double m = prm.maxSwing * std::tanh(l / prm.maxSwing) / l; for (int k = 0; k < 3; k++) out[i][k] *= m; }	// soft limit
		}
		if (prm.hemSmooth > 0.0) {	// round 9c: the mean kept, the one-wave part at half, finer waves dropped (blended by hemSmooth)
			double mean[3] = {}, A[3] = {}, B[3] = {}, cs[kRing], sn[kRing];
			for (int i = 0; i < kRing; i++) {
				cs[i] = std::cos(2.0 * kPi * i / kRing); sn[i] = std::sin(2.0 * kPi * i / kRing);
				for (int k = 0; k < 3; k++) { mean[k] += out[i][k] / kRing; A[k] += out[i][k] * cs[i]; B[k] += out[i][k] * sn[i]; }
			}
			for (int i = 0; i < kRing; i++)
				for (int k = 0; k < 3; k++) {
					const double low = mean[k] + (cs[i] * A[k] + sn[i] * B[k]) / kRing;
					out[i][k] += (low - out[i][k]) * prm.hemSmooth;
				}
		}
	}
}
