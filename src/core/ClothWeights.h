#pragma once

// UNOFFICIAL 2026-10-19 cloth round 5 (user: 'how can we detect what's part of the cloth on a mesh intelligently, without hand
// tuning?'): a PER-VERTEX cloth weight for a worn mesh, worked out once on the CPU from its own vertices, triangles and skeleton
// (bind pose = the space the vertex shader's IN.position is in). Used by ClothWind.h (mask 5; the weights reach the vertex shader
// as vertex stream 1). No game types here, so tests\cloth_weights_test.cpp runs the same code offline.
//
// Why: the analytic skeleton mask (mask 4: distance off the garment's measured leg thickness) cannot tell a coat's lining (1 unit
// inside its shell) from the shell -- they drifted apart -- and lets jeans that bulge past the measured radius move. Checked on the
// NIFs (bench\deep-roadmap\evidence\cloth3\cloth_weights_check.py, the prototype this mirrors line by line; evidence\cloth3-LOG.md
// C3-11): on the user's Combat Ranger coat and the Sheriff's duster the shell and the lining are ONE connected piece (joined at the
// front edges and the hem), on the Mysterious Stranger coat and trousers are one piece (sewn at the waist).
//
//  1. weld by position (0.05 units): seams join, a vertex's copies share one value;
//  2. horizontal cuts every 2 units from the hips down; each cut's segments chained into loops / arcs:
//       - a loop around exactly ONE leg axis = a trouser leg or a boot ('body');
//       - an arc, or a loop around no leg, that keeps (80 %) within the leg's measured radius + 4 units of one leg = a knee pad, a strap,
//         a pocket on the trousers ('body');
//       - anything else (a loop around both legs = skirt / robe, arcs standing off = an open coat) = cloth;
//     the crotch = just above the highest trouser tube; above it (the pelvis band) by distance: still unless more than the hip
//     radius + 6 units off the legs; each cut's triangles vote for their corners (a vertex on any trouser / pad cut is body: where
//     a skirt is sewn onto the thigh it must not move -- the mod Purifier robe's seam moved 0.34 by majority vote); uncut vertices take their
//     neighbours' label; above the hips = torso (still), at the ground = body;
//  3. cloth weight = hang (smooth, 0 at the hips .. 1 at the knees) x freedom (smooth over 8 units of distance ALONG THE SURFACE from
//     the nearest body vertex: where a coat is sewn to the trousers) x clearance (smooth over 5 units of straight-line room from the
//     nearest body vertex: a coat resting on the trousers cannot swing into them) x ground fade (6 units);
//  4. layers: cloth vertices within 2 units of each other (a shell and its lining) share the smaller clearance and are averaged 8 times.
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <queue>
#include <cmath>
#include <cstdint>
#include <algorithm>

namespace ClothWeights {

	constexpr double kWeld = 0.05, kStep = 2.0, kArcHug = 4.0, kPelvis = 8.0, kPelvisOff = 6.0, kFree = 8.0, kClear = 5.0, kLayer = 2.0;
	constexpr int kSmooth = 8;	// (2 left the scribe robe's layers 0.072 apart, 8: 0.018; evidence\cloth3-LOG.md C3-11)
	constexpr double kMaxLegs = 400.0;	// 2026-10-20: hips more than this above the ground (or not finite) = not a body we know: no weights
	constexpr int kMaxCuts = 512;		// at most this many cut heights (400 / kStep = 200 for any real body)
	constexpr double kHangTop = 4.0;	// 2026-10-20 auto-detect: a moving piece counts when it reaches this close below the hips (or higher)

	struct Input {
		const float* pos = nullptr;				// xyz per vertex (bind pose)
		uint32_t vertices = 0;
		const uint16_t* tris = nullptr;			// 3 per triangle
		uint32_t triangles = 0;
		float legs[2][3][4] = {};				// per leg: hip, knee, ankle (xyz, measured radius)
		float hipZ = 0.0f, kneeZ = 0.0f, groundZ = 0.0f;
	};
	struct Result {
		std::vector<float> weight;				// per vertex, 0..1
		std::vector<uint8_t> label;				// per vertex: 0 torso / pelvis (still), 1 body (still), 2 cloth
		uint32_t welded = 0, body = 0, cloth = 0, torso = 0, cuts = 0, tubes = 0, pads = 0, clothCuts = 0;
		float crotch = 0.0f;
		uint32_t moving = 0;					// 2026-10-20 auto-detect: welded vertices with weight > 0.5
		float hemDrop = 0.0f;					// ...and how far the lowest of them hangs below the crotch (units)
	};

	inline double Smooth(double x) { x = x < 0.0 ? 0.0 : (x > 1.0 ? 1.0 : x); return x * x * (3.0 - 2.0 * x); }

	struct V3 { double x, y, z; };
	inline double Dist(const V3& a, const V3& b) { const double dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z; return std::sqrt(dx * dx + dy * dy + dz * dz); }

	// the leg's axis at height z: thigh above the knee, calf down to the ankle, the ankle below (the foot is not upright)
	inline void AxisPoint(const Input& in, int leg, double z, double& x, double& y, double& r) {
		const float* hip = in.legs[leg][0]; const float* knee = in.legs[leg][1]; const float* ankle = in.legs[leg][2];
		if (z >= knee[2]) {
			double t = (z - knee[2]) / (std::max)((double)hip[2] - knee[2], 1e-3); t = (std::min)(t, 1.0);
			x = knee[0] + ((double)hip[0] - knee[0]) * t; y = knee[1] + ((double)hip[1] - knee[1]) * t; r = knee[3] + ((double)hip[3] - knee[3]) * t;
		}
		else if (z >= ankle[2]) {
			const double t = (z - ankle[2]) / (std::max)((double)knee[2] - ankle[2], 1e-3);
			x = ankle[0] + ((double)knee[0] - ankle[0]) * t; y = ankle[1] + ((double)knee[1] - ankle[1]) * t; r = ankle[3] + ((double)knee[3] - ankle[3]) * t;
		}
		else { x = ankle[0]; y = ankle[1]; r = ankle[3]; }
	}

	// distance to the nearest leg segment AXIS (thigh, calf of each leg; no radius)
	inline double LegAxisDistance(const Input& in, const V3& p) {
		double best = 1e30;
		for (int leg = 0; leg < 2; leg++)
			for (int s = 0; s < 2; s++) {
				const float* a = in.legs[leg][s]; const float* b = in.legs[leg][s + 1];
				const double ab[3] = { (double)b[0] - a[0], (double)b[1] - a[1], (double)b[2] - a[2] };
				const double ap[3] = { p.x - a[0], p.y - a[1], p.z - a[2] };
				const double l2 = (std::max)(ab[0] * ab[0] + ab[1] * ab[1] + ab[2] * ab[2], 1e-4);
				double t = (ap[0] * ab[0] + ap[1] * ab[1] + ap[2] * ab[2]) / l2; t = t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t);
				const double d[3] = { ap[0] - ab[0] * t, ap[1] - ab[1] * t, ap[2] - ab[2] * t };
				best = (std::min)(best, std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]));
			}
		return best;
	}

	inline double Winding(const std::vector<double>& xy, double cx, double cy) {
		double w = 0.0;
		const size_t n = xy.size() / 2;
		for (size_t i = 0; i < n; i++) {
			const size_t j = (i + 1) % n;
			const double ax = xy[i * 2] - cx, ay = xy[i * 2 + 1] - cy, bx = xy[j * 2] - cx, by = xy[j * 2 + 1] - cy;
			w += std::atan2(ax * by - ay * bx, ax * bx + ay * by);
		}
		return w / 6.283185307179586;
	}

	struct CellKey { int x, y, z; bool operator==(const CellKey& o) const { return x == o.x && y == o.y && z == o.z; } };
	struct CellHash { size_t operator()(const CellKey& k) const { return ((size_t)(uint32_t)k.x * 73856093u) ^ ((size_t)(uint32_t)k.y * 19349663u) ^ ((size_t)(uint32_t)k.z * 83492791u); } };

	inline bool Compute(const Input& in, Result& out) {
		out = Result();
		if (!in.pos || !in.tris || in.vertices < 3 || in.triangles < 1) return false;
		const uint32_t n = in.vertices;
		// 1. weld (round half to even, as the prototype's numpy round)
		std::vector<uint32_t> wid(n);
		std::vector<V3> wp;
		{
			std::unordered_map<CellKey, uint32_t, CellHash> key;
			key.reserve(n * 2);
			for (uint32_t i = 0; i < n; i++) {
				const double x = in.pos[i * 3], y = in.pos[i * 3 + 1], z = in.pos[i * 3 + 2];
				const CellKey q{ (int)std::nearbyint(x / kWeld), (int)std::nearbyint(y / kWeld), (int)std::nearbyint(z / kWeld) };
				auto it = key.find(q);
				if (it == key.end()) { wid[i] = (uint32_t)wp.size(); key.emplace(q, wid[i]); wp.push_back({ x, y, z }); }
				else wid[i] = it->second;
			}
		}
		const uint32_t nw = (uint32_t)wp.size();
		out.welded = nw;
		std::vector<uint32_t> T;	// welded triangles, degenerate ones dropped
		T.reserve(in.triangles * 3);
		for (uint32_t t = 0; t < in.triangles; t++) {
			const uint16_t* f = in.tris + t * 3;
			if (f[0] >= n || f[1] >= n || f[2] >= n) return false;
			const uint32_t a = wid[f[0]], b = wid[f[1]], c = wid[f[2]];
			if (a == b || b == c || a == c) continue;
			T.push_back(a); T.push_back(b); T.push_back(c);
		}
		const uint32_t nt = (uint32_t)T.size() / 3;
		if (!nt) return false;
		std::vector<std::vector<uint32_t>> nbr(nw);
		for (uint32_t t = 0; t < nt; t++)
			for (int e = 0; e < 3; e++) {
				const uint32_t u = T[t * 3 + e], v = T[t * 3 + (e + 1) % 3];
				nbr[u].push_back(v); nbr[v].push_back(u);
			}
		for (auto& l : nbr) { std::sort(l.begin(), l.end()); l.erase(std::unique(l.begin(), l.end()), l.end()); }
		const double hip = in.hipZ, knee = in.kneeZ, ground = in.groundZ;
		// 2026-10-20 (C3-18, a frozen test): a hip at +inf or a ground at -inf made the cut loop below endless; never trust the heights
		if (!std::isfinite(hip) || !std::isfinite(knee) || !std::isfinite(ground) || hip <= ground || hip - ground > kMaxLegs) return false;
		std::vector<double> dax(nw);
		for (uint32_t v = 0; v < nw; v++) dax[v] = LegAxisDistance(in, wp[v]);
		// 2. cuts
		struct Cut { uint64_t key; double x, y; };
		struct Seg { Cut c0, c1; uint32_t tri; };
		struct Found { double z; int kind; std::vector<uint32_t> tris; };	// kind 0 cloth, 1 tube, 2 pad
		std::vector<Found> found;
		std::vector<Seg> segs;
		std::unordered_map<uint64_t, std::vector<uint32_t>> at;
		std::vector<double> poly;
		int cutSteps = 0;
		for (double z0 = hip - 1.0 + 1e-3; z0 > ground + 1.0 && cutSteps < kMaxCuts; z0 -= kStep, cutSteps++) {
			segs.clear();
			for (uint32_t t = 0; t < nt; t++) {
				const uint32_t vv[3] = { T[t * 3], T[t * 3 + 1], T[t * 3 + 2] };
				Cut cuts[3]; int nc = 0;
				for (int e = 0; e < 3; e++) {
					const uint32_t u = vv[e], v = vv[(e + 1) % 3];
					const double zu = wp[u].z - z0, zv = wp[v].z - z0;
					if (zu * zv < 0.0) {
						const double tt = zu / (zu - zv);
						cuts[nc++] = { ((uint64_t)(std::min)(u, v) << 32) | (uint64_t)(std::max)(u, v), wp[u].x + (wp[v].x - wp[u].x) * tt, wp[u].y + (wp[v].y - wp[u].y) * tt };
					}
				}
				if (nc == 2) segs.push_back({ cuts[0], cuts[1], t });
			}
			if (segs.empty()) continue;
			out.cuts++;
			at.clear();
			for (uint32_t s = 0; s < (uint32_t)segs.size(); s++) { at[segs[s].c0.key].push_back(s); at[segs[s].c1.key].push_back(s); }
			std::vector<char> used(segs.size(), 0);
			double ax[2], ay[2], ar[2];
			for (int leg = 0; leg < 2; leg++) AxisPoint(in, leg, z0, ax[leg], ay[leg], ar[leg]);
			for (uint32_t s0 = 0; s0 < (uint32_t)segs.size(); s0++) {
				if (used[s0]) continue;
				std::vector<uint32_t> chain{ s0 };
				used[s0] = 1;
				const uint64_t ends[2] = { segs[s0].c1.key, segs[s0].c0.key };
				std::vector<double> front, back;	// points appended after (front) and inserted before (back, reversed)
				front.push_back(segs[s0].c0.x); front.push_back(segs[s0].c0.y); front.push_back(segs[s0].c1.x); front.push_back(segs[s0].c1.y);
				bool closed = false;
				for (int dir = 0; dir < 2 && !closed; dir++) {
					uint64_t cur = ends[dir];
					for (;;) {
						auto ai = at.find(cur);
						uint32_t next = UINT32_MAX;
						if (ai != at.end()) for (uint32_t s : ai->second) if (!used[s]) { next = s; break; }
						if (next == UINT32_MAX) break;
						used[next] = 1; chain.push_back(next);
						const Seg& sg = segs[next];
						const Cut& other = sg.c0.key == cur ? sg.c1 : sg.c0;
						if (dir == 0) { front.push_back(other.x); front.push_back(other.y); }
						else { back.push_back(other.y); back.push_back(other.x); }	// reversed later
						cur = other.key;
						if (cur == (dir == 0 ? ends[1] : ends[0])) { closed = true; break; }
					}
				}
				poly.assign(back.rbegin(), back.rend());
				poly.insert(poly.end(), front.begin(), front.end());
				const size_t np = poly.size() / 2;
				int kind = 0;
				auto hugs = [&](int leg) {
					size_t inside = 0;
					for (size_t i = 0; i < np; i++) {
						const double dx = poly[i * 2] - ax[leg], dy = poly[i * 2 + 1] - ay[leg];
						if (std::sqrt(dx * dx + dy * dy) <= ar[leg] + kArcHug) inside++;
					}
					return (double)inside / (double)np >= 0.8;
				};
				if (closed && np >= 3) {
					const double w0 = std::fabs(Winding(poly, ax[0], ay[0])), w1 = std::fabs(Winding(poly, ax[1], ay[1]));
					if ((w0 > 0.5) != (w1 > 0.5)) kind = 1;
					else if (w0 < 0.5 && w1 < 0.5) { for (int leg = 0; leg < 2; leg++) if (hugs(leg)) kind = 2; }
				}
				else if (!closed) { for (int leg = 0; leg < 2; leg++) if (hugs(leg)) kind = 2; }
				Found f{ z0, kind, {} };
				f.tris.reserve(chain.size());
				for (uint32_t s : chain) f.tris.push_back(segs[s].tri);
				found.push_back(std::move(f));
			}
		}
		// the crotch: just above the highest cut with a trouser tube
		double topTube = -1e30; bool anyTube = false;
		for (const Found& f : found) if (f.kind == 1) { anyTube = true; topTube = (std::max)(topTube, f.z); }
		const double crotch = anyTube ? (std::min)(topTube + 0.5 * kStep, hip - 2.0) : hip - kPelvis;
		out.crotch = (float)crotch;
		std::vector<int> bodyVotes(nw, 0), clothVotes(nw, 0);
		for (const Found& f : found) {
			if (f.z > crotch) continue;
			if (f.kind == 1) out.tubes++; else if (f.kind == 2) out.pads++; else out.clothCuts++;
			for (uint32_t t : f.tris)
				for (int e = 0; e < 3; e++) { if (f.kind) bodyVotes[T[t * 3 + e]]++; else clothVotes[T[t * 3 + e]]++; }
		}
		// labels: 0 torso, 1 body, 2 cloth (-1 not yet)
		std::vector<int> label(nw, -1);
		const double rhip = 0.5 * ((double)in.legs[0][0][3] + in.legs[1][0][3]);
		for (uint32_t v = 0; v < nw; v++) {
			const double z = wp[v].z;
			if (z >= hip) label[v] = 0;
			else if (z <= ground + 1.0) label[v] = 1;
			else if (z > crotch) label[v] = dax[v] <= rhip + kPelvisOff ? 0 : 2;
			else if (bodyVotes[v] + clothVotes[v] > 0) label[v] = bodyVotes[v] > 0 ? 1 : 2;	// a seam (cloth sewn onto the trousers) stays still
		}
		for (int round = 0; round < 20; round++) {
			std::vector<std::pair<uint32_t, int>> upd;
			for (uint32_t v = 0; v < nw; v++) {
				if (label[v] >= 0) continue;
				int c1 = 0, c2 = 0, any = 0;
				for (uint32_t u : nbr[v]) { if (label[u] < 0) continue; any++; if (label[u] == 1) c1++; else if (label[u] == 2) c2++; }
				if (any) upd.push_back({ v, (c1 && c1 >= c2) ? 1 : (c2 ? 2 : 0) });
			}
			if (upd.empty()) break;
			for (auto& [v, l] : upd) label[v] = l;
		}
		for (int& l : label) if (l < 0) l = 1;
		// 3. hang x freedom x clearance x ground
		std::vector<double> g(nw, 1e30);
		{
			typedef std::pair<double, uint32_t> QE;
			std::priority_queue<QE, std::vector<QE>, std::greater<QE>> pq;
			for (uint32_t v = 0; v < nw; v++) if (label[v] == 1) { g[v] = 0.0; pq.push({ 0.0, v }); }
			while (!pq.empty()) {
				const auto [dv, v] = pq.top(); pq.pop();
				if (dv > g[v]) continue;
				for (uint32_t u : nbr[v]) {
					const double nd = dv + Dist(wp[u], wp[v]);
					if (nd < g[u]) { g[u] = nd; pq.push({ nd, u }); }
				}
			}
		}
		std::vector<uint32_t> cloth;
		for (uint32_t v = 0; v < nw; v++) if (label[v] == 2) cloth.push_back(v);
		auto cellOf = [](const V3& p, double size) { return CellKey{ (int)std::floor(p.x / size), (int)std::floor(p.y / size), (int)std::floor(p.z / size) }; };
		std::vector<double> clear(nw, 1e30);
		{
			const double cell = kClear + 1.0;
			std::unordered_map<CellKey, std::vector<uint32_t>, CellHash> grid;
			for (uint32_t v = 0; v < nw; v++) if (label[v] == 1) grid[cellOf(wp[v], cell)].push_back(v);
			for (uint32_t v : cloth) {
				const CellKey c = cellOf(wp[v], cell);
				double best = 1e30;
				for (int dx = -1; dx <= 1; dx++) for (int dy = -1; dy <= 1; dy++) for (int dz = -1; dz <= 1; dz++) {
					auto it = grid.find({ c.x + dx, c.y + dy, c.z + dz });
					if (it != grid.end()) for (uint32_t u : it->second) best = (std::min)(best, Dist(wp[u], wp[v]));
				}
				clear[v] = best;
			}
		}
		std::vector<std::vector<uint32_t>> lay(nw);
		{
			std::unordered_map<CellKey, std::vector<uint32_t>, CellHash> grid;
			for (uint32_t v : cloth) grid[cellOf(wp[v], kLayer)].push_back(v);
			for (uint32_t v : cloth) {
				const CellKey c = cellOf(wp[v], kLayer);
				for (int dx = -1; dx <= 1; dx++) for (int dy = -1; dy <= 1; dy++) for (int dz = -1; dz <= 1; dz++) {
					auto it = grid.find({ c.x + dx, c.y + dy, c.z + dz });
					if (it != grid.end()) for (uint32_t u : it->second) if (u != v && Dist(wp[u], wp[v]) <= kLayer) lay[v].push_back(u);
				}
			}
		}
		std::vector<double> w(nw, 0.0);
		for (uint32_t v : cloth) {
			double ce = clear[v];
			for (uint32_t u : lay[v]) ce = (std::min)(ce, clear[u]);
			const double h = Smooth((hip - wp[v].z) / (std::max)(hip - knee, 1.0)) * Smooth((wp[v].z - ground) / 6.0);
			w[v] = h * Smooth(g[v] / kFree) * Smooth((ce - 0.5) / kClear);
		}
		for (int it = 0; it < kSmooth; it++) {
			std::vector<double> nwv(w);
			for (uint32_t v : cloth) {
				double s = w[v];
				for (uint32_t u : lay[v]) s += w[u];
				nwv[v] = s / (1.0 + lay[v].size());
			}
			w.swap(nwv);
		}
		out.weight.resize(n); out.label.resize(n);
		for (uint32_t i = 0; i < n; i++) { out.weight[i] = (float)w[wid[i]]; out.label[i] = (uint8_t)label[wid[i]]; }
		for (uint32_t v = 0; v < nw; v++) { if (label[v] == 0) out.torso++; else if (label[v] == 1) out.body++; else out.cloth++; }
		// 2026-10-20 auto-detect (ClothWind.h AutoVerdict): how much of the garment hangs free, on the welded vertices -- only in a piece
		// that reaches up to the waist (kHangTop below the hips or higher): a coat, a robe, a skirt hangs from there; loose knee plates,
		// bolts and buckles do not (the NCR salvaged power armour's knee plates measured 52 % 'moving' on their own)
		std::vector<uint32_t> piece(nw, UINT32_MAX);
		std::vector<double> pieceTop;
		for (uint32_t s = 0; s < nw; s++) {
			if (piece[s] != UINT32_MAX) continue;
			const uint32_t id = (uint32_t)pieceTop.size();
			double top = wp[s].z;
			std::vector<uint32_t> stack{ s }; piece[s] = id;
			while (!stack.empty()) {
				const uint32_t v = stack.back(); stack.pop_back();
				top = (std::max)(top, wp[v].z);
				for (uint32_t u : nbr[v]) if (piece[u] == UINT32_MAX) { piece[u] = id; stack.push_back(u); }
			}
			pieceTop.push_back(top);
		}
		for (uint32_t v = 0; v < nw; v++) {
			if (w[v] <= 0.5 || pieceTop[piece[v]] < hip - kHangTop) continue;
			out.moving++;
			out.hemDrop = (std::max)(out.hemDrop, (float)(crotch - wp[v].z));
		}
		return true;
	}

	// 2026-10-20 auto-detect, the verdict after Compute ([Shaders.Cloth.Main] AutoDetect, ClothWind.h): the garment hangs free -- a coat,
	// a duster, a robe, a dress -- when at least kAutoMoving of its (welded) vertices in pieces hanging from the waist move more than half,
	// they are at least minShare of the garment, and they reach kAutoHem units below the crotch (not a belt or a pouch at the hips).
	// Survey of every vanilla + DLC + TTW armour: evidence\clothauto (SURVEY.md).
	constexpr uint32_t kAutoMoving = 24;
	constexpr double kAutoHem = 10.0, kFreeFirst = 0.03;	// (0.15 left out the long tight dresses: Formal Wear 5 %; stockings and jeans that the weights wrongly let move measure 0)
	inline bool HangsFree(const Result& r, double minShare) {
		return r.welded && r.moving >= kAutoMoving && (double)r.moving >= minShare * (double)r.welded && r.hemDrop >= kAutoHem;
	}

	// 2026-10-20 auto-detect, the cheap first look (before Compute, ~20 us on a 2000-vertex mesh): the share of the vertices between the
	// knees' height and kFreeTop below the hips that stand more than kFreeGap off BOTH legs' surfaces (the measured radius at that height):
	// a coat's skirt, a robe, a duster's tails. Trousers, power armour and tight suits stay near 0 (their surface hugs the legs, and the
	// measured radius is their own thickness); only garments above kFreeFirst get the full weights (Compute) and a verdict.
	constexpr double kFreeTop = 6.0, kFreeGap = 4.0;
	inline float FreeShare(const Input& in) {
		const double hip = in.hipZ, knee = in.kneeZ;
		uint32_t band = 0, free = 0;
		for (uint32_t i = 0; i < in.vertices; i++) {
			const double x = in.pos[i * 3], y = in.pos[i * 3 + 1], z = in.pos[i * 3 + 2];
			if (z > hip - kFreeTop || z < knee) continue;
			band++;
			double off = 1e30;
			for (int leg = 0; leg < 2; leg++) {
				double ax, ay, r;
				AxisPoint(in, leg, z, ax, ay, r);
				off = (std::min)(off, std::sqrt((x - ax) * (x - ax) + (y - ay) * (y - ay)) - r);
			}
			if (off > kFreeGap) free++;
		}
		return band ? (float)free / (float)band : 0.0f;
	}
}
