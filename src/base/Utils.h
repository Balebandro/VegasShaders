#pragma once

// Small string and maths helpers shared by NVR's managers and effects.

// Whitespace stripped from the front, the back, or both ends (\s as std::regex sees it).
static inline std::string ltrim(const std::string text) {
	static const char* const leading = "^\\s+";
	return std::regex_replace(text, std::regex(leading), "");
}
static inline std::string rtrim(const std::string text) {
	static const char* const trailing = "\\s+$";
	return std::regex_replace(text, std::regex(trailing), "");
}
static inline std::string trim(const std::string text) {
	const std::string right = rtrim(text);
	return ltrim(right);
}

// Component-wise std::lerp.
static inline D3DXVECTOR4 lerp(D3DXVECTOR4 from, D3DXVECTOR4 to, float t) {
	return D3DXVECTOR4(std::lerp(from.x, to.x, t), std::lerp(from.y, to.y, t), std::lerp(from.z, to.z, t), std::lerp(from.w, to.w, t));
}

// Note the argument order: x is kept within [low, high] -- clamp(low, high, x).
inline static float clamp(float low, float high, float x) {
	return min(max(low, x), high);
}

// Where x sits between from and to (0 at from, 1 at to, unclamped).
inline static float invLerp(float from, float to, float x) {
	return (x - from) / (to - from);
}

// invLerp clamped to 0..1, and its smoothstep (3s^2 - 2s^3) version.
inline static float step(float from, float to, float x) {
	const float f = invLerp(from, to, x);
	return clamp(0, 1, f);
}
inline static float smoothStep(float from, float to, float x) {
	const float s = step(from, to, x);
	return s * s * (3.0 - 2.0 * s);
}

// -1, 0 or 1.
template <typename T> inline static int sgn(T v) {
	const T zero = T(0);
	return (zero < v) - (v < zero);
}

// |base|^exponent with the sign of base.
inline static float pows(float base, float exponent) {
	return pow(abs(base), exponent) * sgn(base);
}
