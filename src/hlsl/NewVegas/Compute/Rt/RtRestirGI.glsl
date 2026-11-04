// UNOFFICIAL 2026-11-04 PTRestirGI (docs/next/RT-PATH-TRACING.md section 29): what RtPathTrace.comp's variant RtPathTraceGI.spv
// (-DPT_RESTIR_GI) and the reuse pass RtRestirGI.comp share: unit vectors packed into one float as whole numbers (exact in float32,
// so they survive RG32F images bit for bit: no NaN or denormal patterns).

// Octahedral: a unit vector to [0, 1]^2
vec2 rgiOct(vec3 n) {
	n /= abs(n.x) + abs(n.y) + abs(n.z);
	vec2 e = n.z >= 0.0 ? n.xy : (1.0 - abs(n.yx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
	return e * 0.5 + 0.5;
}
vec3 rgiUnOct(vec2 e) {
	e = e * 2.0 - 1.0;
	vec3 n = vec3(e, 1.0 - abs(e.x) - abs(e.y));
	const float t = max(-n.z, 0.0);
	n.xy += vec2(n.x >= 0.0 ? -t : t, n.y >= 0.0 ? -t : t);
	return normalize(n);
}

// 12 + 12 bits: a direction or a normal at ~0.1 degree
float rgiPack24(vec3 n) { const uvec2 q = uvec2(clamp(rgiOct(n), 0.0, 1.0) * 4095.0 + 0.5); return float(q.x * 4096u + q.y); }
vec3 rgiUnpack24(float f) { const uint v = uint(max(f, 0.0) + 0.5); return rgiUnOct(vec2(float(v >> 12), float(v & 4095u)) / 4095.0); }

// 8 + 8 bits of a normal and 8 bits of a count (a reservoir's M, at most 255): ~1 degree, enough for the reuse's cosines
float rgiPackNormalM(vec3 n, float m) {
	const uvec2 q = uvec2(clamp(rgiOct(n), 0.0, 1.0) * 255.0 + 0.5);
	return float(uint(clamp(m, 0.0, 255.0) + 0.5) * 65536u + q.x * 256u + q.y);
}
vec3 rgiUnpackNormal(float f) { const uint v = uint(max(f, 0.0) + 0.5); return rgiUnOct(vec2(float((v >> 8) & 255u), float(v & 255u)) / 255.0); }
float rgiUnpackM(float f) { return float(uint(max(f, 0.0) + 0.5) >> 16); }

float rgiLum(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }
