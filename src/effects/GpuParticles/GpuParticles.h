#pragma once

// UNOFFICIAL 2026-10-26 (NVR DXVK 1.5-particles, docs\next\FORK-1026-PARTICLES.md): GPU-simulated rain and snow drops.
// [Shaders.GPUParticles.Main] Enabled (off by default). While it rains or snows outdoors (ForceRain / ForceSnow count),
// up to Count drops live in a storage buffer from frame to frame round the camera: a compute shader moves them, lands
// them on what they hit (the rain's top-down occlusion map: roofs, trees, ground; and the scene's depth buffer: what the
// camera sees), turns a landed rain drop into a short splash and a landed flake into a settled one, and starts them
// again at the top of the box. They are drawn as streaks / splashes / flakes over the frame after tonemapping, with a
// soft depth fade. Needs the NVR DXVK d3d9.dll with ID3D9NvrExtGeometry; without it (native D3D9, stock or older NVR
// DXVK) nothing happens and the picture is the old one. Independent of the screen-space rain (Precipitations).
class GpuParticles {
public:
	// UpdateConstants: true when the drops will be drawn this frame (asks for the ortho map).
	static bool		Wanted();
	// RenderEffects, after the weather effects. Draws onto the frame chain (Commit) when Wanted().
	static void		Render(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget);

private:
	static bool		Ready();	// the extension's geometry interface, pipelines and buffers (made once; fails once)
};
