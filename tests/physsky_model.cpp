// Offline check of the CPU twin of the physical sky's ExactSunlight / MultipleScattering path (src/effects/PhysSkyModel.h, the same
// header Sky.cpp uses): builds its tables, times them, and prints luminance at fixed directions and the dome-mean luminance on
// Sky.cpp's grid (16 x 32 over the upper hemisphere) for several sun elevations, to compare with the numpy models
// (bench\deep-roadmap\evidence\skyms\cpu_compare.py).
// Usage: build\physsky-model\physsky_model.exe [haze] [albedo]
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include "../src/effects/PhysSkyModel.h"
using namespace PhysSky;

static float Lum(const V3& c) { return 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z; }
static V3 Dir(double elDeg, double azDeg) {
	const double e = elDeg * Pi / 180.0, a = azDeg * Pi / 180.0;
	return V3((float)(cos(e) * cos(a)), (float)(cos(e) * sin(a)), (float)sin(e));
}

int main(int argc, char** argv) {
	const float haze = argc > 1 ? (float)atof(argv[1]) : 1.0f, albedo = argc > 2 ? (float)atof(argv[2]) : 0.3f;
	Tables t;
	LARGE_INTEGER f, a, b;
	QueryPerformanceFrequency(&f);
	QueryPerformanceCounter(&a);
	t.Build(haze, albedo, true);
	QueryPerformanceCounter(&b);
	std::printf("tables built (transmittance + multiple scattering) in %.1f ms, haze %.2f albedo %.2f\n", 1000.0 * (b.QuadPart - a.QuadPart) / f.QuadPart, haze, albedo);
	const double elev[] = { 90, 45, 15, 5, 0, -2 };
	const V3 named[5] = { Dir(90, 0), Dir(2, 90), Dir(2, 180), Dir(45, 180), Dir(2, 20) };
	const int ST = 16, SP = 32;
	for (double se : elev) {
		const V3 sun((float)cos(se * Pi / 180.0), 0.0f, (float)sin(se * Pi / 180.0));
		double domeExact = 0, domeMs = 0;
		for (int i = 0; i < ST; i++) {
			const double th = (i + 0.5) * (Pi / 2) / ST, st = sin(th), ct = cos(th), dOmega = st * (Pi / 2 / ST) * (2 * Pi / SP);
			for (int j = 0; j < SP; j++) {
				const double ph = (j + 0.5) * (2 * Pi) / SP;
				V3 d((float)(st * cos(ph)), (float)(st * sin(ph)), (float)(std::max)(ct, 0.002));
				const float n = Len(d); d = d * (1.0f / n);
				domeExact += Lum(t.Radiance(d, sun, 300.0f, haze, false)) * dOmega;
				domeMs += Lum(t.Radiance(d, sun, 300.0f, haze, true)) * dOmega;
			}
		}
		std::printf("sun %3.0f dome-mean exact %.6f ms %.6f |", se, domeExact, domeMs);
		for (int k = 0; k < 5; k++) std::printf(" %.6f %.6f", Lum(t.Radiance(named[k], sun, 300.0f, haze, false)), Lum(t.Radiance(named[k], sun, 300.0f, haze, true)));
		std::printf("\n");
	}
	return 0;
}
