#pragma once

// UNOFFICIAL 2026-10-20 cloth on still objects (user, video of a Legion tent: 'here the entire tent mesh wobbles around' and 'maybe
// we could find a way to only move thin parts of static cloth objects instead of the whole thing'): a PER-VERTEX cloth weight for an
// unskinned cloth mesh (a tent, a flag on its pole, a banner, a curtain), worked out once on the CPU from its own vertices and
// triangles. Used by ClothWind.h (mask 6; the weights reach the vertex shader as vertex stream 1, like the worn coats' mask 5).
// No game types here, so tests\cloth_static_test.cpp runs the same code offline.
//
// Why: still cloth was masked by height alone -- pinned at the top of its bound, loosest at the BOTTOM -- so a tent's pole, the guy
// ropes and the canvas where it meets the ground swung the most, and a flag swung its whole pole. One mesh holds all of those parts
// (the Legion soldier tent: canvas, poles, ropes and pegs in one shape with one texture), so the choice has to be per vertex.
//
//  1. weld by position (0.05 units); split the mesh into its separate PIECES (connected triangles);
//  2. each piece is RIGID or a SHEET:
//       - a rod (a pole, a rope, a stick, a frame of a few of them): every vertex within kRodRadius of up to kRodSegments straight
//         segments (fitted greedily; two near-parallel segments side by side are a strip of cloth, not a frame);
//       - a solid (a crate, a peg, a stake): closed (almost no open edges) and thick (twice its volume over its area > kSolid);
//       - a tiny piece (no side longer than kTiny);
//       - anything else is a sheet of cloth;
//  3. sheets touching each other (within kTouch) are one CLUSTER (a drape sewn onto a canvas moves with it);
//  4. where a cluster is HELD: within kTouch of a rigid piece (a pole, a rope's end), at the ground (a mesh stands on the ground when
//     its lowest point is within kStandLow..kStandHigh of its origin; the ground is then the origin's height or the lowest point,
//     whichever is higher -- poles and pegs reach below the ground -- and the kGround units above it hold the cloth), and along its
//     top line (kTopBand) when it stands on the ground or touches nothing rigid (a banner, a curtain hangs from its top); cloth that
//     touches nothing rigid also hangs from its whole top EDGE (its edge vertices in the top kTopEdge of its height);
//  5. FREE EDGES: the open edges of the sheets that are not held and do not meet another sheet (a seam between two panels is not
//     free), and the folded rims of two-sided cloth (an edge whose two triangles face opposite ways: a banner made as two layers
//     joined round its edge) -- a flag's three outer edges, a tent's door flaps;
//  6. per sheet vertex: g = the distance ALONG THE CLOTH to where it is held, f = to the nearest free edge;
//     looseness t = g / (g + f) (0 where held .. 1 on a free edge; a panel held all round stays near 0 in its middle: taut canvas);
//     weight = smooth((t - kTaut) / (1 - kTaut)) x smooth(g / kHold); rigid pieces 0;
//     (2026-10-20, a tarp held at one corner flapped whole) a flat hanging cluster (an awning, a tarp) is tied along all its edges, and a
//     cluster with no free edge at all billows gently in its middle (kTautShare);
//  7. layers: sheet vertices within kLayer of each other (two-sided cloth) are averaged kSmooth times, so they move together;
//  8. the billow's side: the shader billows along the normal, and a two-sided cloth's two layers face opposite ways (they pushed
//     apart). In a cluster where a fifth of the vertices have an opposite-facing vertex within kLayer, each vertex gets the sign that
//     turns its normal to the cluster's main facing (the same side for both layers); single-layer cloth keeps its own normal (+1).
#include <vector>
#include <unordered_map>
#include <queue>
#include <cmath>
#include <cstdint>
#include <algorithm>

namespace ClothStatic {

	constexpr double kWeld = 0.05;
	constexpr double kRodRadius = 3.5;		// units: a pole or rope's vertices all lie this close to its axis (Legion tent poles 3.0, Dead Money 3.7)
	constexpr double kRodMinLength = 10.0;	// a rod's first segment is at least this long
	constexpr int    kRodSegments = 6;		// a frame of up to this many straight pieces (a sagging rope, a T-shaped flagpole)
	constexpr double kSolid = 2.5;			// a closed piece thicker than this (2 x volume / area, units) is solid
	constexpr double kTiny = 6.0;			// a piece no longer than this on every side is hardware (rigid)
	constexpr double kTouch = 2.5;			// a sheet vertex this close to a rigid vertex / a rod's surface is held there (canvas layers overlap 2-3 apart)
	constexpr double kGround = 3.0;			// the lowest units of a standing mesh are held by the ground
	constexpr double kStandLow = -40.0, kStandHigh = 8.0;	// a mesh stands on the ground when its lowest point is this near its origin
	constexpr double kTopBand = 3.0;		// a cluster's top units are held (a banner, a curtain, a tent's ridge)
	constexpr double kTopEdge = 0.15;		// cloth touching nothing rigid: its edge in the top share of its height is held too
	constexpr double kHold = 12.0;			// units along the cloth over which motion fades in from where it is held
	constexpr double kTaut = 0.25;			// looseness below this does not move at all (canvas held all round)
	constexpr double kFlat = 0.35;			// a hanging sheet less tall than this share of its width is an awning / canopy (tied all round)
	constexpr double kTautShare = 0.5, kTautReach = 40.0;	// a sheet with no free edge: its middle moves this much, reached this far in from where it is held
	constexpr double kLayer = 2.0;
	constexpr int    kSmooth = 4;

	struct Input {
		const float* pos = nullptr;			// xyz per vertex (the mesh's own space)
		const float* nrm = nullptr;			// optional: normal xyz per vertex (for the billow's side, step 8; none: +1 everywhere)
		uint32_t vertices = 0;
		const uint16_t* tris = nullptr;		// 3 per triangle
		uint32_t triangles = 0;
	};
	struct Result {
		std::vector<float> weight;			// per vertex, 0..1
		std::vector<uint8_t> kind;			// per vertex: 0 sheet, 1 rod, 2 solid, 3 tiny
		std::vector<uint8_t> flag;			// per vertex (pictures): 0 sheet, 1 held by a rigid part, 2 by the ground, 3 top line, 4 free edge
		std::vector<float> side;			// per vertex: +1 / -1, the billow goes along normal x side (step 8)
		uint32_t layered = 0;				// clusters found two-sided
		uint32_t canopies = 0;				// flat hanging clusters tied along all their edges
		uint32_t welded = 0, pieces = 0, rods = 0, solids = 0, tiny = 0, sheets = 0, clusters = 0;
		uint32_t heldRigid = 0, heldGround = 0, heldTop = 0, freeEdges = 0, moving = 0;	// sheet vertices (welded); moving = weight > 0.5
		float groundZ = 0.0f; bool standing = false;	// groundZ: where the ground was taken (standing meshes)
	};

	inline double Smooth(double x) { x = x < 0.0 ? 0.0 : (x > 1.0 ? 1.0 : x); return x * x * (3.0 - 2.0 * x); }
	struct V3 { double x, y, z; };
	inline V3 Sub(const V3& a, const V3& b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
	inline double Dot(const V3& a, const V3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
	inline V3 Cross(const V3& a, const V3& b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
	inline double Len(const V3& a) { return std::sqrt(Dot(a, a)); }
	struct CellKey { int x, y, z; bool operator==(const CellKey& o) const { return x == o.x && y == o.y && z == o.z; } };
	struct CellHash { size_t operator()(const CellKey& k) const { return ((size_t)(uint32_t)k.x * 73856093u) ^ ((size_t)(uint32_t)k.y * 19349663u) ^ ((size_t)(uint32_t)k.z * 83492791u); } };
	inline CellKey CellOf(const V3& p, double s) { return { (int)std::floor(p.x / s), (int)std::floor(p.y / s), (int)std::floor(p.z / s) }; }

	// The principal axis of a point set (power iteration on the covariance).
	inline void Axis(const std::vector<V3>& P, const std::vector<uint32_t>& ids, V3& mean, V3& dir) {
		mean = { 0, 0, 0 };
		for (uint32_t i : ids) { mean.x += P[i].x; mean.y += P[i].y; mean.z += P[i].z; }
		const double n = (double)(std::max)((size_t)1, ids.size());
		mean = { mean.x / n, mean.y / n, mean.z / n };
		double c[3][3] = {};
		for (uint32_t i : ids) {
			const double d[3] = { P[i].x - mean.x, P[i].y - mean.y, P[i].z - mean.z };
			for (int a = 0; a < 3; a++) for (int b = 0; b < 3; b++) c[a][b] += d[a] * d[b];
		}
		double v[3] = { 0.577, 0.577, 0.577 };
		// start from the largest diagonal axis (a vertical pole starts upright)
		if (c[2][2] >= c[0][0] && c[2][2] >= c[1][1]) { v[0] = 0.1; v[1] = 0.1; v[2] = 1.0; }
		else if (c[0][0] >= c[1][1]) { v[0] = 1.0; v[1] = 0.1; v[2] = 0.1; }
		else { v[0] = 0.1; v[1] = 1.0; v[2] = 0.1; }
		for (int it = 0; it < 40; it++) {
			double w[3] = {};
			for (int a = 0; a < 3; a++) for (int b = 0; b < 3; b++) w[a] += c[a][b] * v[b];
			const double l = std::sqrt(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]);
			if (l < 1e-12) break;
			for (int a = 0; a < 3; a++) v[a] = w[a] / l;
		}
		dir = { v[0], v[1], v[2] };
	}
	inline double LineDistance(const V3& p, const V3& m, const V3& e) {
		const V3 d = Sub(p, m);
		const double t = Dot(d, e);
		return Len({ d.x - e.x * t, d.y - e.y * t, d.z - e.z * t });
	}

	struct Seg { V3 a, b; double r; };
	// Is this piece a rod / a frame of rods? Fills segs.
	inline bool RodOf(const std::vector<V3>& P, const std::vector<uint32_t>& piece, std::vector<Seg>& segs) {
		segs.clear();
		std::vector<uint32_t> rem = piece;
		for (int s = 0; s < kRodSegments && !rem.empty(); s++) {
			if (rem.size() < 2) { rem.clear(); break; }
			V3 m, e;
			Axis(P, rem, m, e);
			std::vector<uint32_t> in;
			for (double band : { 8.0, 4.0, kRodRadius }) {	// refine the line on its own inliers (an L-shape pulls the first mean off the pole)
				in.clear();
				for (uint32_t i : rem) if (LineDistance(P[i], m, e) <= band) in.push_back(i);
				if (in.size() < 2) break;
				Axis(P, in, m, e);
			}
			in.clear();
			double r = 0.0, t0 = 1e30, t1 = -1e30;
			for (uint32_t i : rem) {
				const double d = LineDistance(P[i], m, e);
				if (d > kRodRadius) continue;
				in.push_back(i); r = (std::max)(r, d);
				const double t = Dot(Sub(P[i], m), e); t0 = (std::min)(t0, t); t1 = (std::max)(t1, t);
			}
			if (in.size() < 2 || (in.size() < piece.size() / 10 && in.size() < 8)) return false;
			if (segs.empty() && t1 - t0 < kRodMinLength) return false;
			const Seg sg{ { m.x + e.x * t0, m.y + e.y * t0, m.z + e.z * t0 }, { m.x + e.x * t1, m.y + e.y * t1, m.z + e.z * t1 }, r };
			// a strip of cloth: a second segment side by side with an earlier one (nearly parallel and close) is not a frame
			for (const Seg& o : segs) {
				const V3 oe = Sub(o.b, o.a); const double ol = Len(oe);
				if (ol < 1e-6) continue;
				const V3 oeu = { oe.x / ol, oe.y / ol, oe.z / ol };
				const V3 mid = { (sg.a.x + sg.b.x) * 0.5, (sg.a.y + sg.b.y) * 0.5, (sg.a.z + sg.b.z) * 0.5 };
				if (std::fabs(Dot(oeu, e)) > 0.94 && LineDistance(mid, o.a, oeu) < 3.0 * kRodRadius) return false;
			}
			segs.push_back(sg);
			std::vector<uint32_t> keep;
			for (uint32_t i : rem) if (LineDistance(P[i], m, e) > kRodRadius) keep.push_back(i);
			if (keep.size() == rem.size()) return false;
			rem.swap(keep);
		}
		return rem.empty() && !segs.empty();
	}
	inline double SegDistance(const V3& p, const Seg& s) {
		const V3 ab = Sub(s.b, s.a), ap = Sub(p, s.a);
		const double l2 = (std::max)(Dot(ab, ab), 1e-9);
		double t = Dot(ap, ab) / l2; t = t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t);
		return Len({ ap.x - ab.x * t, ap.y - ab.y * t, ap.z - ab.z * t });
	}

	inline bool Compute(const Input& in, Result& out) {
		out = Result();
		if (!in.pos || !in.tris || in.vertices < 3 || in.triangles < 1) return false;
		const uint32_t n = in.vertices;
		// 1. weld
		std::vector<uint32_t> wid(n);
		std::vector<V3> P;
		{
			std::unordered_map<CellKey, uint32_t, CellHash> key;
			key.reserve(n * 2);
			for (uint32_t i = 0; i < n; i++) {
				const double x = in.pos[i * 3], y = in.pos[i * 3 + 1], z = in.pos[i * 3 + 2];
				const CellKey q{ (int)std::nearbyint(x / kWeld), (int)std::nearbyint(y / kWeld), (int)std::nearbyint(z / kWeld) };
				auto it = key.find(q);
				if (it == key.end()) { wid[i] = (uint32_t)P.size(); key.emplace(q, wid[i]); P.push_back({ x, y, z }); }
				else wid[i] = it->second;
			}
		}
		const uint32_t nw = (uint32_t)P.size();
		out.welded = nw;
		std::vector<uint32_t> T;
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
		// edges: neighbours, and how many triangles share each edge (1 = an open edge)
		std::unordered_map<uint64_t, uint32_t> edgeUse;
		edgeUse.reserve(nt * 3);
		std::vector<std::vector<uint32_t>> nbr(nw);
		for (uint32_t t = 0; t < nt; t++)
			for (int e = 0; e < 3; e++) {
				const uint32_t u = T[t * 3 + e], v = T[t * 3 + (e + 1) % 3];
				edgeUse[((uint64_t)(std::min)(u, v) << 32) | (std::max)(u, v)]++;
				nbr[u].push_back(v); nbr[v].push_back(u);
			}
		for (auto& l : nbr) { std::sort(l.begin(), l.end()); l.erase(std::unique(l.begin(), l.end()), l.end()); }
		// pieces (union-find over the welded triangles)
		std::vector<uint32_t> parent(nw);
		for (uint32_t i = 0; i < nw; i++) parent[i] = i;
		auto find = [&](uint32_t x) { while (parent[x] != x) { parent[x] = parent[parent[x]]; x = parent[x]; } return x; };
		for (uint32_t t = 0; t < nt; t++) {
			const uint32_t a = find(T[t * 3]), b = find(T[t * 3 + 1]), c = find(T[t * 3 + 2]);
			parent[b] = a; parent[find(c)] = a;
		}
		std::unordered_map<uint32_t, uint32_t> pieceId;
		std::vector<std::vector<uint32_t>> pieces;
		std::vector<uint32_t> pieceOf(nw, UINT32_MAX);
		for (uint32_t v = 0; v < nw; v++) {
			if (nbr[v].empty()) continue;	// in no triangle
			const uint32_t r = find(v);
			auto it = pieceId.find(r);
			if (it == pieceId.end()) { it = pieceId.emplace(r, (uint32_t)pieces.size()).first; pieces.emplace_back(); }
			pieces[it->second].push_back(v); pieceOf[v] = it->second;
		}
		out.pieces = (uint32_t)pieces.size();
		// per piece: open edges, area, volume (closed solids)
		std::vector<double> area(pieces.size(), 0.0), vol(pieces.size(), 0.0);
		std::vector<uint32_t> edges(pieces.size(), 0), open(pieces.size(), 0);
		for (uint32_t t = 0; t < nt; t++) {
			const V3& a = P[T[t * 3]]; const V3& b = P[T[t * 3 + 1]]; const V3& c = P[T[t * 3 + 2]];
			const uint32_t pc = pieceOf[T[t * 3]];
			area[pc] += 0.5 * Len(Cross(Sub(b, a), Sub(c, a)));
			vol[pc] += Dot(a, Cross(b, c)) / 6.0;
		}
		for (const auto& [k, use] : edgeUse) {
			const uint32_t pc = pieceOf[(uint32_t)(k >> 32)];
			edges[pc]++; if (use == 1) open[pc]++;
		}
		// 2. rigid or sheet
		std::vector<uint8_t> kindW(nw, 3);	// welded vertices in no triangle: 'tiny'
		std::vector<Seg> rodSegs;
		std::vector<std::vector<Seg>> segsOf(pieces.size());
		for (uint32_t pc = 0; pc < pieces.size(); pc++) {
			const auto& pv = pieces[pc];
			double lo[3] = { 1e30, 1e30, 1e30 }, hi[3] = { -1e30, -1e30, -1e30 };
			for (uint32_t v : pv) { const double q[3] = { P[v].x, P[v].y, P[v].z }; for (int i = 0; i < 3; i++) { lo[i] = (std::min)(lo[i], q[i]); hi[i] = (std::max)(hi[i], q[i]); } }
			uint8_t kind = 0;
			if (hi[0] - lo[0] <= kTiny && hi[1] - lo[1] <= kTiny && hi[2] - lo[2] <= kTiny) { kind = 3; out.tiny++; }
			else if (RodOf(P, pv, segsOf[pc])) { kind = 1; out.rods++; }
			else if (edges[pc] && open[pc] * 50 <= edges[pc] && area[pc] > 1e-6 && 2.0 * std::fabs(vol[pc]) / area[pc] > kSolid) { kind = 2; out.solids++; }
			else out.sheets++;
			for (uint32_t v : pv) kindW[v] = kind;
			if (kind == 1) for (const Seg& s : segsOf[pc]) rodSegs.push_back(s);
		}
		// the ground
		double zmin = 1e30;
		for (uint32_t v = 0; v < nw; v++) if (!nbr[v].empty()) zmin = (std::min)(zmin, P[v].z);
		out.standing = zmin >= kStandLow && zmin <= kStandHigh;
		const double groundZ = out.standing ? (std::max)(zmin, 0.0) : zmin;
		out.groundZ = (float)groundZ;
		std::vector<uint32_t> sheet;
		for (uint32_t v = 0; v < nw; v++) if (kindW[v] == 0) sheet.push_back(v);
		std::vector<double> w(nw, 0.0);
		std::vector<uint8_t> flagW(nw, 0);
		std::vector<float> sideW;	// per ORIGINAL vertex (step 8)
		if (!sheet.empty()) {
			// 3. clusters: sheet edges + contacts between sheet vertices of different pieces
			std::vector<std::vector<std::pair<uint32_t, double>>> g(nw);
			std::vector<char> seam(nw, 0);	// touches another sheet piece
			for (uint32_t v : sheet) for (uint32_t u : nbr[v]) if (kindW[u] == 0) g[v].push_back({ u, Len(Sub(P[u], P[v])) });
			std::unordered_map<CellKey, std::vector<uint32_t>, CellHash> grid;
			for (uint32_t v : sheet) grid[CellOf(P[v], kTouch)].push_back(v);
			for (uint32_t v : sheet) {
				const CellKey c = CellOf(P[v], kTouch);
				for (int dx = -1; dx <= 1; dx++) for (int dy = -1; dy <= 1; dy++) for (int dz = -1; dz <= 1; dz++) {
					auto it = grid.find({ c.x + dx, c.y + dy, c.z + dz });
					if (it == grid.end()) continue;
					for (uint32_t u : it->second) {
						if (u == v || pieceOf[u] == pieceOf[v]) continue;
						const double d = Len(Sub(P[u], P[v]));
						if (d <= kTouch) { g[v].push_back({ u, d }); seam[v] = 1; }
					}
				}
			}
			std::vector<uint32_t> cl(nw, UINT32_MAX);
			std::vector<std::vector<uint32_t>> clusters;
			for (uint32_t s : sheet) {
				if (cl[s] != UINT32_MAX) continue;
				const uint32_t id = (uint32_t)clusters.size();
				clusters.emplace_back();
				std::vector<uint32_t> stack{ s }; cl[s] = id;
				while (!stack.empty()) {
					const uint32_t v = stack.back(); stack.pop_back(); clusters[id].push_back(v);
					for (const auto& [u, d] : g[v]) if (cl[u] == UINT32_MAX) { cl[u] = id; stack.push_back(u); }
				}
			}
			out.clusters = (uint32_t)clusters.size();
			// 4. held: rigid contact, ground, top line
			std::vector<char> held(nw, 0);
			{
				std::unordered_map<CellKey, std::vector<uint32_t>, CellHash> rg;
				for (uint32_t v = 0; v < nw; v++) if (kindW[v] != 0 && !nbr[v].empty()) rg[CellOf(P[v], kTouch)].push_back(v);
				for (uint32_t v : sheet) {
					bool touch = false;
					const CellKey c = CellOf(P[v], kTouch);
					for (int dx = -1; dx <= 1 && !touch; dx++) for (int dy = -1; dy <= 1 && !touch; dy++) for (int dz = -1; dz <= 1 && !touch; dz++) {
						auto it = rg.find({ c.x + dx, c.y + dy, c.z + dz });
						if (it != rg.end()) for (uint32_t u : it->second) if (Len(Sub(P[u], P[v])) <= kTouch) { touch = true; break; }
					}
					for (const Seg& s : rodSegs) if (!touch && SegDistance(P[v], s) <= s.r + kTouch) touch = true;
					if (touch) { held[v] = 1; out.heldRigid++; }
				}
			}
			// the cloth's edges: open edges, and the folded rims of two-sided cloth (an edge whose two triangles face opposite ways)
			std::vector<uint64_t> rims;
			{
				std::unordered_map<uint64_t, V3> firstNormal;
				for (uint32_t t = 0; t < nt; t++) {
					if (kindW[T[t * 3]] != 0) continue;
					const V3& a = P[T[t * 3]]; const V3& b = P[T[t * 3 + 1]]; const V3& c = P[T[t * 3 + 2]];
					V3 nn = Cross(Sub(b, a), Sub(c, a)); const double l = Len(nn);
					if (l < 1e-12) continue;
					nn = { nn.x / l, nn.y / l, nn.z / l };
					for (int e = 0; e < 3; e++) {
						const uint32_t u = T[t * 3 + e], v = T[t * 3 + (e + 1) % 3];
						const uint64_t k = ((uint64_t)(std::min)(u, v) << 32) | (std::max)(u, v);
						const uint32_t use = edgeUse[k];
						if (use == 1) { rims.push_back(k); continue; }
						if (use != 2) continue;
						auto it = firstNormal.find(k);
						if (it == firstNormal.end()) firstNormal.emplace(k, nn);
						else if (Dot(it->second, nn) < -0.7) rims.push_back(k);
					}
				}
			}
			std::vector<char> rimV(nw, 0);
			for (uint64_t k : rims) { rimV[(uint32_t)(k >> 32)] = 1; rimV[(uint32_t)(k & 0xFFFFFFFFu)] = 1; }
			for (const auto& c : clusters) {
				bool rigidContact = false, ground = false;
				double top = -1e30, bottom = 1e30;
				for (uint32_t v : c) { if (held[v]) rigidContact = true; top = (std::max)(top, P[v].z); bottom = (std::min)(bottom, P[v].z); }
				if (out.standing)
					for (uint32_t v : c) if (P[v].z <= groundZ + kGround) { if (!held[v]) out.heldGround++; held[v] = 2; ground = true; }
				if (ground || !rigidContact)
					for (uint32_t v : c) if (P[v].z >= top - kTopBand && !held[v]) { held[v] = 3; out.heldTop++; }
				// cloth that touches nothing rigid hangs from its whole top edge (a banner's top edge sags between its corners); a FLAT one
				// (an awning, a tarp over a stall: much wider than tall) is tied along all its edges instead
				double lo[2] = { 1e30, 1e30 }, hi[2] = { -1e30, -1e30 };
				for (uint32_t v : c) { lo[0] = (std::min)(lo[0], P[v].x); hi[0] = (std::max)(hi[0], P[v].x); lo[1] = (std::min)(lo[1], P[v].y); hi[1] = (std::max)(hi[1], P[v].y); }
				const bool flat = top - bottom < kFlat * (std::max)(hi[0] - lo[0], hi[1] - lo[1]);
				if (!rigidContact && !ground && flat) { for (uint32_t v : c) if (rimV[v] && !held[v]) { held[v] = 3; out.heldTop++; } out.canopies++; }
				else if (!rigidContact)
					for (uint32_t v : c) if (rimV[v] && P[v].z >= top - (std::max)(kTopBand, kTopEdge * (top - bottom)) && !held[v]) { held[v] = 3; out.heldTop++; }
			}
			// 5. free edges: the cloth's edges whose ends are not held and meet no other sheet (a seam between two panels is not free)
			std::vector<char> freeV(nw, 0);
			for (uint64_t k : rims) {
				const uint32_t a = (uint32_t)(k >> 32), b = (uint32_t)(k & 0xFFFFFFFFu);
				if (held[a] || held[b] || seam[a] || seam[b]) continue;
				freeV[a] = freeV[b] = 1;
			}
			for (uint32_t v : sheet) if (freeV[v]) out.freeEdges++;
			// 6. distances along the cloth from where it is held (gh) and from the free edges (gf)
			auto dijkstra = [&](const std::vector<char>& src, std::vector<double>& d) {
				d.assign(nw, 1e30);
				typedef std::pair<double, uint32_t> QE;
				std::priority_queue<QE, std::vector<QE>, std::greater<QE>> pq;
				for (uint32_t v : sheet) if (src[v]) { d[v] = 0.0; pq.push({ 0.0, v }); }
				while (!pq.empty()) {
					const auto [dv, v] = pq.top(); pq.pop();
					if (dv > d[v]) continue;
					for (const auto& [u, len] : g[v]) { const double nd = dv + len; if (nd < d[u]) { d[u] = nd; pq.push({ nd, u }); } }
				}
			};
			std::vector<char> heldSrc(nw, 0);
			for (uint32_t v : sheet) heldSrc[v] = held[v] != 0;
			std::vector<double> gh, gf;
			dijkstra(heldSrc, gh);
			dijkstra(freeV, gf);
			// a cluster with no free edge at all (held all round: a closed tent wall, a canopy tied at its edges) billows gently in its middle
			std::vector<char> clusterFree(clusters.size(), 0);
			for (uint32_t v : sheet) if (freeV[v]) clusterFree[cl[v]] = 1;
			for (uint32_t v : sheet) {
				if (held[v] || gh[v] >= 1e29) { w[v] = 0.0; continue; }
				if (!clusterFree[cl[v]]) { w[v] = kTautShare * Smooth(gh[v] / kTautReach); continue; }
				const double f = gf[v] >= 1e29 ? 1e6 : gf[v];
				const double t = gh[v] / (gh[v] + f);
				w[v] = Smooth((t - kTaut) / (1.0 - kTaut)) * Smooth(gh[v] / kHold);
			}
			// 7. layers move together
			std::vector<std::vector<uint32_t>> lay(nw);
			{
				std::unordered_map<CellKey, std::vector<uint32_t>, CellHash> lg;
				for (uint32_t v : sheet) lg[CellOf(P[v], kLayer)].push_back(v);
				for (uint32_t v : sheet) {
					const CellKey c = CellOf(P[v], kLayer);
					for (int dx = -1; dx <= 1; dx++) for (int dy = -1; dy <= 1; dy++) for (int dz = -1; dz <= 1; dz++) {
						auto it = lg.find({ c.x + dx, c.y + dy, c.z + dz });
						if (it != lg.end()) for (uint32_t u : it->second) if (u != v && Len(Sub(P[u], P[v])) <= kLayer) lay[v].push_back(u);
					}
				}
			}
			for (int it = 0; it < kSmooth; it++) {
				std::vector<double> nwv(w);
				for (uint32_t v : sheet) {
					if (held[v]) continue;
					double s = w[v];
					for (uint32_t u : lay[v]) s += w[u];
					nwv[v] = s / (1.0 + lay[v].size());
				}
				w.swap(nwv);
			}
			for (uint32_t v : sheet) if (w[v] > 0.5) out.moving++;
			flagW.assign(nw, 0);
			for (uint32_t v : sheet) flagW[v] = held[v] ? (uint8_t)held[v] : (freeV[v] ? 4 : 0);
			// 8. the billow's side for two-sided cloth (per original vertex: a welded vertex's copies on the two layers face apart)
			if (in.nrm) {
				std::vector<std::vector<uint32_t>> copies(nw);
				for (uint32_t i = 0; i < n; i++) if (kindW[wid[i]] == 0) copies[wid[i]].push_back(i);
				auto nrmOf = [&](uint32_t i) { V3 q{ in.nrm[i * 3], in.nrm[i * 3 + 1], in.nrm[i * 3 + 2] }; const double l = Len(q); return l > 1e-9 ? V3{ q.x / l, q.y / l, q.z / l } : V3{ 0, 0, 0 }; };
				std::unordered_map<CellKey, std::vector<uint32_t>, CellHash> lg;
				for (uint32_t i = 0; i < n; i++) if (kindW[wid[i]] == 0) lg[CellOf(P[wid[i]], kLayer)].push_back(i);
				sideW.assign(n, 1.0f);
				for (const auto& c : clusters) {
					uint32_t paired = 0, total = 0;
					double M[3][3] = {};
					for (uint32_t v : c) for (uint32_t i : copies[v]) {
						const V3 a = nrmOf(i);
						const double q[3] = { a.x, a.y, a.z };
						for (int r = 0; r < 3; r++) for (int k = 0; k < 3; k++) M[r][k] += q[r] * q[k];
						total++;
						bool opp = false;
						const CellKey ck = CellOf(P[v], kLayer);
						for (int dx = -1; dx <= 1 && !opp; dx++) for (int dy = -1; dy <= 1 && !opp; dy++) for (int dz = -1; dz <= 1 && !opp; dz++) {
							auto it = lg.find({ ck.x + dx, ck.y + dy, ck.z + dz });
							if (it == lg.end()) continue;
							for (uint32_t j : it->second) if (j != i && Len(Sub(P[wid[j]], P[v])) <= kLayer && Dot(a, nrmOf(j)) < -0.5) { opp = true; break; }
						}
						if (opp) paired++;
					}
					if (!total || paired * 5 < total) continue;
					out.layered++;
					double R[3] = { 0.3, 0.5, 0.8 };	// the main facing: the dominant axis of the normals (sign-free)
					for (int it = 0; it < 40; it++) {
						double t[3] = {};
						for (int r = 0; r < 3; r++) for (int k = 0; k < 3; k++) t[r] += M[r][k] * R[k];
						const double l = std::sqrt(t[0] * t[0] + t[1] * t[1] + t[2] * t[2]);
						if (l < 1e-12) break;
						for (int r = 0; r < 3; r++) R[r] = t[r] / l;
					}
					for (uint32_t v : c) for (uint32_t i : copies[v]) {
						const V3 a = nrmOf(i);
						sideW[i] = a.x * R[0] + a.y * R[1] + a.z * R[2] < 0.0 ? -1.0f : 1.0f;
					}
				}
			}
		}
		out.side = sideW.size() == n ? sideW : std::vector<float>(n, 1.0f);
		out.weight.resize(n); out.kind.resize(n); out.flag.resize(n);
		for (uint32_t i = 0; i < n; i++) { out.weight[i] = (float)w[wid[i]]; out.kind[i] = kindW[wid[i]]; out.flag[i] = flagW[wid[i]]; }
		return true;
	}
}
