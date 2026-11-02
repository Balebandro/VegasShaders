// Colour-space constants and conversions (CIE XYZ <-> linear sRGB primaries).

static const float3 WhitePointE = {0.959456f, 1.00f, 1.088754f};	// reference white (illuminant E, adapted)

static const float3 BlueShift = {1.05f, 0.97f, 1.27f};	// tint used for night-time blue shift

// linear RGB -> XYZ
static const float3x3 XYZ_RGB = { 0.412453,  0.357580,  0.180423,
                      0.212671,  0.715160,  0.072169,
                      0.019334,  0.119193,  0.950227 };

// XYZ -> linear RGB
static const float3x3 RGB_XYZ = { 3.240479, -1.537150, -0.498535,
                     -0.969256,  1.875992,  0.041556,
                      0.055648, -0.204043,  1.057311 };

// Y row of the RGB->XYZ matrix, kept away from zero.
float GetLuminance(const float3 c) { return max(dot(XYZ_RGB[1], c), 0.001); }

float3 GetXYZfromRGB(const float3 c) { return mul(XYZ_RGB, c); }

float3 GetRGBfromXYZ(const float3 v) { return mul(RGB_XYZ, v); }

float3 GetWhitePoint() { return mul(RGB_XYZ, WhitePointE); }
