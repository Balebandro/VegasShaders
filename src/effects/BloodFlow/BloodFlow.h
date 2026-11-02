#pragma once

#include "BloodFlowBoxes.h"

// UNOFFICIAL, optional (off by default; Shaders.BloodFlow): faked liquid blood.
//  - Splats: the game's own blood splats (its simple decals, read from its decal lists) grow streams that run down the
//    surface the way gravity pulls along it (straight down a wall, slantwise across a slope), slowing as they go. A
//    stream that reaches something flat (a ledge, the floor) gathers in a small puddle there; one that reaches an edge
//    with nothing under it hangs in a swelling drop that lets go, falls and leaves a stain where it lands. Fresh blood is
//    wet and shines a little; thin trails dry first, darker and browner.
//  - Pools: a pool of blood spreads out under a body after death; on a slope it slides downhill and trickles run off
//    its low side, gathering where the ground levels out. Glossy at first, drying to a dark stain with a ring.
// Nothing new is placed in the game: every splat is the game's and goes when the game removes it; pools follow the
// bodies the game keeps loaded. Where each stream goes is worked out on the GPU from the depth buffer (BloodFlow.fx.hlsl,
// probe pass, into a small path texture that keeps each path once it has been seen whole); each item is then drawn as a
// box around it, the pixel shader finding the surface in it from the depth buffer and painting the blood onto it,
// blended straight into the current image. The cost follows how much blood is near; none at all when there is none.
class BloodFlowEffect : public EffectRecord
{
public:
	BloodFlowEffect() : EffectRecord("BloodFlow") {};

	struct BloodFlowSettingsStruct {
		bool	Splats;			// drips and drying on the game's wall splats
		bool	CheapGather;	// UNOFFICIAL 2026-10-16: once a frame, blood-only, cached skeleton walks, off-screen culled (false: the old way, for A/B)
		bool	Pools;			// pools under bodies
		float	DripLength;		// longest drip, in game units
		float	FlowSpeed;		// how fast the blood runs and creeps (1 = normal)
		float	Viscosity;		// how much the distortion noise breaks up the flow (higher = more uneven, clumpy)
		float	BloodAmount;	// 0-1: threshold on the blood's thickness, how much of it shows
		float	DryTime;		// seconds for a splat or drip to dry
		float	PoolSize;		// pool size, as a share of the body's size
		float	PoolSpreadTime;	// seconds a pool takes to spread (most of the way)
		float	PoolDryTime;	// seconds for a pool to dry
		float	Gloss;			// how glossy fresh blood is
		float	FreshColor[3];	// sRGB
		float	DryColor[3];	// sRGB
		float	MaxFall;		// how far drops falling off an edge are followed, in game units
		float	MaxDistance;	// game units
		int		MaxSplats;
		int		MaxPools;
		int		DebugView;		// 1: boxes in green, blood in red
		bool	SplatterAtlas;	// the game's blood splatter decals are there as a loose file (pools and puddles take their shape)
		// UNOFFICIAL 2026-10-13 (user: match my regular blood's colour, resize the trails):
		bool	MatchGameColor;	// the blood takes each game splat's own colour (its decal colour times MatchShade) instead of FreshColor
		float	MatchShade;		// the splatter texture's own grey (sRGB; the user's 2048 replacer measures 0.12)
		float	MatchBrightness;	// multiplies the matched colour
		float	StreamWidth;	// multiplies the trails' width
		float	Beads;			// UNOFFICIAL 2026-10-13: wet beads over the game's splats (0 off)
		float	BeadSize;		// game units a tile of the drops texture covers
		// UNOFFICIAL 2026-10-14 (user: like paint thrown at a wall): each splat's own impact shape (BloodFlow.fx.hlsl SplatPS)
		float	Splat;			// strength (0 off)
		float	SplatReach;		// how far the arms and spray reach, in splat radii
		float	SplatSpray;		// how many spray lines and how much mist (0-2)
		float	SplatDrips;		// how far its drips run on walls and slopes, in splat radii (0 none)
		bool	SplatFast;		// UNOFFICIAL 2026-10-22: the splat shapes drawn the cheaper way, same picture (technique BloodSplatFast; false: the old way, A/B)
		bool	SplatMask;		// UNOFFICIAL 2026-10-22: each splat's shape worked out once into a mask atlas and read from there (BloodFlowBoxes::SplatMask; false: SplatFast)
		int		TestSplats;		// UNOFFICIAL 2026-10-22, TEST ONLY: this many made-up splats on the ground ahead (fresh game splats cannot be made from a save)
		float	Merge;			// splats closer than this x their radii merge into one item (0 off)
		float	GibPools;		// a pool under each cut-off limb, this x the body's pool size (0 off)
		float	GibHold;		// UNOFFICIAL 2026-10-20: seconds a gib's landing splat waits (dropped if the gib bounces on; 0 off)
		int		MergeCount;		// ... when at least this many lie together
	};
	BloodFlowSettingsStruct	Settings;

	struct BloodFlowStruct {
		D3DXVECTOR4	Fresh;		// rgb: fresh blood colour (linear); w: gloss
		D3DXVECTOR4	Dry;		// rgb: dried blood colour (linear); w: splat dry time (s)
		D3DXVECTOR4	Info;		// x: outdoors, y: debug view, z: pool dry time (s), w: drip speed
		D3DXVECTOR4	Pool;		// x: pool spread time (s); y: longest drip (game units); z: longest fall; w: gravity (units/s^2)
		D3DXVECTOR4	Probe;		// x: 1 / path texture rows; y: finest detail a path is kept at (units per pixel)
		D3DXVECTOR4	Flow;		// x: flow speed; y: viscosity; z: blood amount; w: clock (seconds, wraps every 1023)
		D3DXVECTOR4	Match;		// x: splatter texture grey (sRGB); y: brightness; z: trail width; w: 1 match the game's colour
		D3DXVECTOR4	DryRatio;	// rgb: dried over fresh colour (linear, from the settings), for the matched colour
		D3DXVECTOR4	Beads;		// x: wet beads' strength (0 off), y: a drops tile's size (game units)
		D3DXVECTOR4	Splat;		// UNOFFICIAL 2026-10-14: x strength, y reach, z spray, w drips (SplatPS)
	};
	BloodFlowStruct	Constants;

	void	UpdateConstants();
	void	RegisterConstants();
	void	RegisterTextures();
	void	UpdateSettings();
	bool	ShouldRender();
	void	Render(IDirect3DDevice9* device, IDirect3DSurface9* target, IDirect3DSurface9* rendered,
		UINT technique, bool clear, IDirect3DSurface9* source) override;

	typedef BloodFlowBoxes::Item Item;

	static const int	ItemMax = 96;
	static const int	PathRows = ItemMax * BloodFlowBoxes::Streams;
	Item				Items[ItemMax];
	int					Count = 0;
	int					OnScreenCount = 0;	// UNOFFICIAL 2026-10-16: items whose box touches the screen (BloodFlowBoxes::OnScreen)
	VolumetricSmokeQuads::Vertex Vertices[ItemMax * 36];
	VolumetricSmokeQuads::Vertex ProbeVertices[ItemMax * 6];
	VolumetricSmokeQuads::Vertex BoundsVertices[ItemMax * 6];
	IDirect3DVertexDeclaration9* Declaration = nullptr;
	double				Clock = 0.0;	// seconds the effect has run (pauses with the effect)

	// The path texture (BloodFlowBoxes.h), twice: the probe pass reads last frame's and writes the other.
	IDirect3DTexture9*	PathTexture[2] = {};
	IDirect3DSurface9*	PathSurface[2] = {};
	IDirect3DTexture9*	PathRead = nullptr;	// what TESR_BloodFlowPaths binds
	int					PathIndex = 0;		// the one holding the latest paths
	// The box round each item's real paths (2 x ItemMax: lowest and highest corner), from the latest paths.
	IDirect3DTexture9*	BoundsTexture = nullptr;
	IDirect3DSurface9*	BoundsSurface = nullptr;
	// UNOFFICIAL 2026-10-22 (SplatMask): the splat shapes' mask atlas (made the first time it is used: 24 MB), its cells, the splat
	// pass's vertices with each splat's cell, and the constants (TESR_BloodFlowMask, TESR_BloodFlowMaskSize).
	IDirect3DTexture9*	MaskTexture = nullptr;
	IDirect3DSurface9*	MaskSurface = nullptr;
	bool				MaskFailed = false;	// the card cannot draw into or filter R16F: the mask stays off
	BloodFlowBoxes::SplatMask Mask;
	float				MaskShape[3] = { -1.0f, -1.0f, -1.0f };	// SplatReach, SplatSpray, SplatDrips the cells were worked out with
	VolumetricSmokeQuads::Vertex MaskBakeVertices[BloodFlowBoxes::SplatMask::Cells * 6];
	VolumetricSmokeQuads::Vertex SplatVertices[ItemMax * 36];
	D3DXVECTOR4			MaskConstants = D3DXVECTOR4(0.0f, 0.0f, 0.0f, 0.0f);
	D3DXVECTOR4			MaskSizeConstants = D3DXVECTOR4(0.0f, 0.0f, 0.0f, 0.0f);

	// Each item keeps its slot (rows in the path texture) while it is gathered every frame.
	struct SlotState { UInt64 Key = 0; bool Used = false; UInt32 SeenFrame = 0; int WrittenPass = -10; };
	SlotState			Slots[ItemMax];
	UInt32				Frame = 0;			// UpdateConstants calls
	int					ProbePasses = 0;	// probe passes drawn

private:
	void	AddTestSplats();
	D3DXVECTOR3	TestAnchor = D3DXVECTOR3(0.0f, 0.0f, 0.0f);	// TestSplats: the ground point they lie round, and when they were made
	double		TestStart = -1.0;
	void	GatherSplats();
	void	GatherPools();
	int		SlotFor(UInt64 key);
};
