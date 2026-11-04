#pragma once

// UNOFFICIAL 2026-11-02 the REFERENCE path tracer ([Main.Main.RayTracing] ReferencePT, off; docs\next\RT-PATH-TRACING.md): a ground
// truth for the raster and hybrid look, not a real-time effect. The RT helper (RtCore.h) traces PTSamples paths a pixel and frame from
// the camera (Compute\Rt\RtPathTrace.comp: PTBounces diffuse bounces, the albedo atlas, the sun, the sky, the lamps and glow maps) and
// keeps a running mean while the camera, the sun and the settings stay still; RenderDebug shows it over the finished picture
// (RtPathTraceShow.frag; PTSplit keeps the game's frame on the left for an A/B). Needs the same as every RT effect: the DXVK fork, a GPU
// with ray tracing for 64-bit programs (the 9800X3D's iGPU here; not the GTX 1070) and NvrRtHelper.exe. Off: nothing runs.

namespace RtPathTrace {
	void	Register();						// once: the core user
	void	RenderDebug(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget);	// the traced picture over the finished frame
	void	Replace(IDirect3DDevice9* Device);
	bool	Replacing();	// (2026-11-03) the traced picture replaces the scene this frame: AO / AmbientLight / Specular stay out (no light twice)	// (2026-11-03) PTReplace: the traced picture into the scene right after RtCore::Frame
	std::string	StatusText();				// the REPORT block's line: on / why not, samples so far
}
