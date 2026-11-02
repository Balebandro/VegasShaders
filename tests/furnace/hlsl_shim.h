// Minimal HLSL-in-C++ shim for the furnace harness (2026-10-16): just what Shaders\Includes\PBR.hlsl and the
// Helpers.hlsl macros it uses need. float math, IEEE behaviour (normalize of a zero vector gives NaN, as on the GPU).
#pragma once
#include <cmath>
#include <algorithm>

struct float3 {
	float x, y, z;
	float3() : x(0), y(0), z(0) {}
	float3(float v) : x(v), y(v), z(v) {}
	float3(float a, float b, float c) : x(a), y(b), z(c) {}
};
inline float3 operator+(float3 a, float3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
inline float3 operator-(float3 a, float3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
inline float3 operator*(float3 a, float3 b) { return { a.x * b.x, a.y * b.y, a.z * b.z }; }
inline float3 operator/(float3 a, float3 b) { return { a.x / b.x, a.y / b.y, a.z / b.z }; }
inline float3 operator*(float3 a, float s) { return { a.x * s, a.y * s, a.z * s }; }
inline float3 operator*(float s, float3 a) { return { a.x * s, a.y * s, a.z * s }; }
inline float3 operator/(float3 a, float s) { return { a.x / s, a.y / s, a.z / s }; }
inline float3 operator+(float3 a, float s) { return { a.x + s, a.y + s, a.z + s }; }
inline float3 operator+(float s, float3 a) { return { a.x + s, a.y + s, a.z + s }; }
inline float3 operator-(float s, float3 a) { return { s - a.x, s - a.y, s - a.z }; }
inline float3 operator-(float3 a, float s) { return { a.x - s, a.y - s, a.z - s }; }
inline float3 operator-(float3 a) { return { -a.x, -a.y, -a.z }; }
// PBR.hlsl compares a float3 that holds one value in all lanes (LdotR = dot(...)): the x lane decides.
inline bool operator<(float3 a, float b) { return a.x < b; }

struct float4 { float x, y, z, w; };   // shader constants (TESR_PBRMath): the harness sets them
inline float rsqrt(float v) { return 1.0f / std::sqrt(v); }

inline float dot(float3 a, float3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline float length(float3 a) { return std::sqrt(dot(a, a)); }
inline float3 normalize(float3 a) { const float r = 1.0f / std::sqrt(dot(a, a)); return a * r; }  // rsq(0) = inf -> 0*inf = NaN
inline float3 reflect(float3 i, float3 n) { return i - 2.0f * dot(n, i) * n; }
inline float saturate(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
inline float3 saturate(float3 v) { return { saturate(v.x), saturate(v.y), saturate(v.z) }; }
inline float max(float a, float b) { return a > b ? a : b; }
inline float min(float a, float b) { return a < b ? a : b; }
inline float clamp(float v, float a, float b) { return v < a ? a : v > b ? b : v; }	// 2026-10-18 pbrport: SmithCorrelatedKeep
inline float3 pow(float3 a, float b) { return { std::pow(a.x, b), std::pow(a.y, b), std::pow(a.z, b) }; }
inline float3 lerp(float3 a, float3 b, float t) { return a + (b - a) * t; }
inline float lerp(float a, float b, float t) { return a + (b - a) * t; }
inline float3 ddx(float3) { return float3(0.0f); }  // no screen derivatives on the CPU: SpecularAA is not exercised
inline float3 ddy(float3) { return float3(0.0f); }

#define shades(n, l) saturate(dot(n, l))
#define PI 3.1415926538f
