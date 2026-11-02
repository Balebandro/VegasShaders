#pragma once

// UNOFFICIAL 2026-10-26 BloodFluid ("BloodFlow v2"; [Shaders.BloodFlow.Fluid], off by default; needs NVR DXVK, the NVR extension
// on DXVK's d3d9.dll, core/DxvkCompute.h). Fresh blood becomes a real fluid: a particle simulation run as Vulkan compute work in
// DXVK's own command stream (src/hlsl/NewVegas/Compute/BloodFluidSim.comp), painted with BloodFlow's look
// (BloodFluidDraw.frag). Drips run down walls and gather under ledges and drop off them, pools under bodies spread and settle,
// then everything stops and dries into stains.
//  - Where blood comes from: what BloodFlow already reads from the game. A fresh blood splat (on screen when it appears) is
//    thrown at its surface along the way the game threw it; a body that has just died bleeds a pool on the ground under it
//    (BloodFlow then does not draw its own pool for that body). With ReplaceStreams BloodFlow's painted streams and splat drips
//    are off while the fluid is on (the fluid's runs replace them); the game's decal and BloodFlow's splat shape stay.
//  - Without NVR DXVK (native D3D9, stock DXVK) or with the setting off, nothing here runs and BloodFlow is exactly as before.
// Ideas from gwater2 (meetric, GPL-3.0; no code copied) and NVIDIA's "Screen Space Fluid Rendering" (2009); the 2026-10-22
// spike's D3D9 prototype (work\bench\deep-roadmap\evidence\fluidsim) is the model: its numbers are kept.
//
// Kept out of BloodFlow.h (a widely included header): BloodFlow.cpp includes this and calls TheBloodFluid().

#include "BloodFlowBoxes.h"
#include "../../core/NvrDxvkExt.h"

class BloodFlowEffect;

class BloodFluid {
public:
	struct SettingsStruct {
		bool	Enabled = false;
		bool	Splats = true;			// fresh splats throw fluid blood at their surface
		bool	Pools = true;			// fresh deaths bleed a fluid pool (BloodFlow's own pool is not drawn for that body)
		bool	ReplaceStreams = true;	// BloodFlow's painted streams and splat drips off while the fluid runs
		float	Amount = 1.0f;			// particles per splat or pool, x
		int		MaxParticles = 16384;
		float	SettleTime = 12.0f;		// seconds (real time) blood keeps moving; it slows from 40 % of this
		float	BleedTime = 8.0f;		// seconds a body bleeds its pool
		float	Viscosity = 8.0f;
		float	Cling = 300.0f;
		float	Friction = 40.0f;
		float	Lifetime = 900.0f;		// seconds before the stains fade (10 s)
		float	StainSize = 3.2f;		// game units: the stain one particle paints round it
		float	Threshold = 0.3f;		// how thick the field must be to show
		float	Bump = 0.6f;			// how much the field's slope bends the normal (rounded edges)
		// UNOFFICIAL 2026-10-27 (user: "the giant amount of 'orbs' that come out of npcs when they get hit"): see BloodFluid.cpp Offer.
		float	Drops = 0.3f;			// share of the blood in the air drawn as drops (the rest still falls and lands as stain)
		float	DropSize = 0.55f;		// a drop's radius, x the particle's (1.2 units)
		bool	OldThrow = false;		// TEST: the throws, pools and drops as before 2026-10-27 (to compare)
		bool	OldColour = false;		// TEST: the fluid's colour as before 2026-10-27 evening (BloodFluidDraw.frag, look4)
		float	TextureRelief = 1.0f;	// UNOFFICIAL 2026-10-27: how much thin blood follows the ground's texture (look4.y)
		int		DebugView = 0;			// 1 stains red / drops white, 2 the particles as dots by state
		float	TestThrow = 0.0f;		// TEST: a throw from the camera at the view centre every this many seconds (0 off)
		bool	TestPour = false;		// TEST: a pool bleeding on the ground ahead (once, when turned on)
		int		StatsEvery = 0;			// TEST: log the particles' state every this many frames (waits for the GPU)
		bool	SteadyTiles = true;		// UNOFFICIAL 2026-11-01: the tiles list a fixed subset when full (BloodFluidSim.comp TilesFill)
		bool	SteadyContact = true;	// UNOFFICIAL 2026-11-01: a particle that touched a surface in the last 3 steps is drawn lying there (BloodFluid.glsl StainOf)
		bool	SteadyFloor = true;		// UNOFFICIAL 2026-11-01: the floor rebuilt on the jittered depth's own rays, contact normals smoothed, slow blood rests
		float	RestSpeed = 3.0f;		// UNOFFICIAL 2026-11-01: with SteadyFloor, blood on a floor slower than this (units/s) settles early
		int		TileCap = 256;			// UNOFFICIAL 2026-11-01: particles a tile lists (was 128)
	};
	SettingsStruct Settings;

	void	ReadSettings();				// BloodFlowEffect::UpdateSettings
	bool	On() const;					// Enabled, and the extension is there and has not failed
	bool	Active() const;				// On, and there is blood alive (BloodFlowEffect::ShouldRender)
	bool	ReplacesStreams() const { return On() && Settings.ReplaceStreams; }
	// Each item BloodFlow gathered this frame (on its gather call, after the on-screen test), with its slot key: true when the
	// fluid takes it over (a fresh pool, and that body's pool for as long as the fluid's lasts): BloodFlow then leaves it out.
	bool	Offer(UInt64 Key, const BloodFlowBoxes::Item& Item);
	// BloodFlowEffect::Render, after its paint: simulate and paint into Image (the current image); Source is
	// TESR_SourceBuffer's surface (the image is copied there first).
	// True when it drew into Image.
	bool	Render(IDirect3DDevice9* Device, IDirect3DSurface9* Image, IDirect3DSurface9* Source, BloodFlowEffect* Owner);

private:
	struct Emitter {
		float	Origin[4];	// xyz anchor space, w fallback ground z
		float	Dir[4];		// xyz throw direction, w speed
		float	Shape[4];	// spread radius, cone, back-off, birth window
		UInt32	Data[4];	// first slot, count, seed, colour | kind << 24
	};
	static const int		EmittersMax = 64;
	UInt32					TileCap = 128;		// particles a 16 x 16 tile lists
	Emitter					Pending[EmittersMax];
	int						PendingCount = 0;
	UInt32					PendingParticles = 0;

	bool	Ready(IDirect3DDevice9* Device);
	void	Release();
	void	Clear(const char* Why);
	void	Queue(const D3DXVECTOR3& Origin, float GroundZ, const D3DXVECTOR3& Dir, float Speed, float Spread, float Cone, float BackOff,
				float Window, int Count, UInt32 Seed, UInt32 Colour, UInt32 Kind);
	void	Stats(UInt32 Frame);

	ID3D9NvrExtPipeline*	SimPipeline = nullptr;
	ID3D9NvrExtPipeline*	DrawPipeline = nullptr;
	ID3D9NvrExtBuffer*		Particles = nullptr;
	ID3D9NvrExtBuffer*		Grid = nullptr;
	ID3D9NvrExtBuffer*		Emitters = nullptr;
	ID3D9NvrExtBuffer*		Draws = nullptr;
	ID3D9NvrExtBuffer*		TileCounts = nullptr;
	ID3D9NvrExtBuffer*		TileLists = nullptr;
	ID3D9NvrExtBuffer*		FrameBuffer = nullptr;
	bool					Failed = false;		// a pipeline or buffer was refused: off for the session (logged)
	bool					PipelinesTried = false;
	UInt32					Capacity = 0;		// particles the buffers hold
	UInt32					AllocCapacity = 0;	// the ring size slots are given out in (Settings.MaxParticles; a change clears the blood)
	bool					LastExterior = true;
	UInt32					Width = 0, Height = 0, TilesX = 0, TilesY = 0;

	UInt32					Head = 0;			// next slot to spawn into (a ring: the oldest blood goes first)
	UInt32					Used = 0;			// slots ever spawned into (up to Capacity)
	bool					HaveAnchor = false;
	double					Anchor[3] = {};		// the particles' origin (world), near the camera
	float					PendingShift[3] = {};
	bool					ShiftPending = false;
	double					LastTime = -1.0;	// TheFrameRateManager->Time at the last Render
	double					StepDebt = 0.0;		// simulated time owed (fixed steps)
	double					AliveUntil = -1.0;	// real time after which nothing is left alive (Lifetime past the last birth)
	double					LastThrow = -1e9;
	bool					PourDone = false;
	D3DXVECTOR3				LastEye = D3DXVECTOR3(0.0f, 0.0f, 0.0f);
	UInt32					Frames = 0;
	float					LastFarZ = 0.0f;
	int						LoggedEmits = 0;
	UInt64					Emitted = 0;		// particles spawned this session (log)
};

BloodFluid& TheBloodFluid();
