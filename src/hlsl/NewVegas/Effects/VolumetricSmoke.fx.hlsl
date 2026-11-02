// GunFX volumetric smoke (prototype).
// GunFX.dll keeps its smoke (heat smoke, after-fire trail, muzzle puffs, ejection smoke) as chains of points in world
// space and hands over one record per tube segment between neighbouring points (or per lone point, a soft sphere).
// Every segment is a Gaussian tube (density falls off with distance from its axis, radius and density blended along
// it), integrated along the view ray in closed form at the ray's closest approach, so the smoke is one continuous
// volume however far apart the points are.
// Two steps: VolumetricSmokeSegments draws each segment as a screen quad over the area it can reach and adds into
// TESR_SmokeBuffer its optical depth (r), the share of it the light reaches (g), the muzzle flash's light on it (b)
// and its age (a),
// so the cost follows the smoke's size on screen, not the number of segments; VolumetricSmoke then lights the total and
// lays it over the image. Look: noise attached to the smoke makes its outline lumpy and eats into its edges (more as
// it ages); light passing through the smoke toward the sun (indoors: from above) leaves the far side darker, and the
// smoke glows when you look toward the sun; each shot's muzzle flash lights the smoke around it; fresh smoke is whiter
// and greys with age, thin smoke has a cool tint and light through it warms slightly; it is shaded as if round and
// lumpy (lit side and top brighter, lumps catching light on one side), for a sense of depth; it takes its ambient light from
// the view, is darker deep inside, fades behind scene surfaces and close to the camera, and your own first-person gun
// stays in front of it.

float4 TESR_ReciprocalResolution;
float4 TESR_SunDirection;
float4 TESR_SunColor;
float4 TESR_SunAmbient;

float4 SmokeInfo;        // x: -1 = test mode (the smoke in plain green), y: debug (solid red, seen through walls), z: 1 outdoors
                         // (sun light), 0 indoors, w: 1 = gun in front
float4 SmokeParams;      // x: edge detail, y: brightness, z: self-shadow, w: clock (seconds)
float4 SmokeFlash;       // xyz: the muzzle flash (NVR's camera-centred space), w: its light now (0 = none)
float4 SmokeFlashColor;  // rgb: its colour, w: its reach (game units)
float4 SmokeLook;        // x: colour by thickness and age (0 = plain grey, 1 = normal), y: depth shading (0 = flat, 1 = normal),
                         // z: step 1's buffer scale (1 full resolution, 2 half, 4 quarter), w: some smoke is drawn at a lower
                         // resolution this frame (step 2 then hides smoke behind each pixel's own surface)
// Gun-smoke haze (the room filling up as you keep firing): up to 40 big, soft blobs. HazeShape: centre x, y, z (NVR's
// camera-centred space) and radius; HazeLook: optical depth straight through the middle (y, z, w spare). HazeInfo x: how
// many.
float4 HazeShape[40];
float4 HazeLook[40];
float4 HazeInfo;         // x: how many blobs, y: how many bullets (the haze pass and the composite both cut holes with them)
// Recent bullets (GunFX_GetBullets): where each was fired from (NVR's space) and seconds since; the way it flew and how hard.
float4 BulletFrom[16];
float4 BulletWay[16];
float4 BulletMove;       // x: how much you are moving (GunFX_GetMoving: 0 standing .. 1 walking or running), see CompositePS
float4 EnvInfo;          // x: how much of this frame's room light replaces the last (1 = all; less smooths it over time)

sampler2D TESR_RenderedBuffer : register(s0) = sampler_state {
	ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE;
};
sampler2D TESR_DepthBuffer : register(s1) = sampler_state {
	ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE;
};
// NVR binds the TESR_ samplers to stages in the order they are declared here (EffectRecord: the first one to stage 0,
// the next to 1 ...), whatever register() says: keep them in register order. (With TESR_SmokeBufferLow declared before
// TESR_DepthBufferViewModel, the two were swapped: the gun mask read the empty quarter buffer, and the gun's depth was
// added in as smoke, a veil over the gun and arms lit orange by every shot.)
// Blended smoothly: at half resolution, step 2 reads it between its pixels (at full resolution it lands on them).
sampler2D TESR_SmokeBuffer : register(s2) = sampler_state {
	ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE;
};
// Depth of the first-person model only (NVR clears it in third person); above 0 where the gun is drawn.
sampler2D TESR_DepthBufferViewModel : register(s3) = sampler_state {
	ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE;
};
// The big, close segments, drawn at quarter resolution (each would be a nearly full-screen pass); added to the above.
sampler2D TESR_SmokeBufferLow : register(s4) = sampler_state {
	ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE;
};
// The haze, at quarter resolution, already cut at the nearest surface of each of its pixels' blocks (step 2 adds it as is).
sampler2D TESR_SmokeHaze : register(s5) = sampler_state {
	ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE;
};
// The light of the surroundings the smoke scatters (EnvPS, one pixel, once a frame).
sampler2D TESR_SmokeEnv : register(s6) = sampler_state {
	ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE;
};
// How much of the segment smoke bullets cut, per quarter-resolution pixel (x), with that pixel's surface distance (w):
// written by the haze pass (its second target), read by CompositePS (CutAt). After the others: NVR binds TESR_ samplers in
// declaration order.
sampler2D TESR_SmokeCut : register(s7) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
// UNOFFICIAL: the general 3D noise (Textures\Effects\NoiseVolume.dds, tools/cloud_noise.cpp): r one octave, 8 cells across
// the tile; g a 4-octave fbm, 4 cells across. Last, so it takes the next stage (8), which nothing here binds by hand.
sampler3D TESR_NoiseVolume : register(s8) < string ResourceName = "Effects\NoiseVolume.dds"; > = sampler_state {
	ADDRESSU = WRAP; ADDRESSV = WRAP; ADDRESSW = WRAP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR;
};

#include "Includes/Depth.hlsl"

// All the smoke step 1 added up at a point: the normal segments and the big ones.
float4 SmokeAt(float2 uv) { return tex2D(TESR_SmokeBuffer, uv) + tex2D(TESR_SmokeBufferLow, uv); }
// The same, readable inside a real branch (the buffers have no mip levels, so the result is identical).
float4 SmokeAtLod(float2 uv) { return tex2Dlod(TESR_SmokeBuffer, float4(uv, 0, 0)) + tex2Dlod(TESR_SmokeBufferLow, float4(uv, 0, 0)); }

// Smooth noise 0..1 with one lattice cell per unit (it was a procedural value noise: eight hashes per call), now one
// read of the 3D noise texture.
float noise3(float3 x) { return tex3Dlod(TESR_NoiseVolume, float4(x / 8.0, 0.0)).r; }
// Its 4-octave fbm, the same scale for its first octave.
float noiseFbm(float3 x) { return tex3Dlod(TESR_NoiseVolume, float4(x / 4.0, 0.0)).g; }
// The same at a coarser mip level (lod), for points seen so far off that one pixel covers several of its texels.
float noiseFbmLod(float3 x, float lod) { return tex3Dlod(TESR_NoiseVolume, float4(x / 4.0, lod)).g; }
// erfc (Abramowitz & Stegun 7.1.27, error below 0.0005).
float erfcApprox(float x) {
	float z = abs(x);
	float t = 1.0 + z * (0.278393 + z * (0.230389 + z * (0.000972 + z * 0.078108)));
	float e = 1.0 / (t * t * t * t);
	return x >= 0.0 ? e : 2.0 - e;
}
// The light the smoke is lit by: the sun outdoors, from above indoors.
float3 LightDirection() { return SmokeInfo.z > 0.5 ? normalize(TESR_SunDirection.xyz) : float3(0.0, 0.0, 1.0); }

// ---- Step 1: one quad per segment, adding its optical depth ------------------------------------------------------
// Each vertex of a segment's quad carries the whole record (the same for all four corners):
//   a: end A x, y, z (NVR's camera-centred world space), radius
//   b: end B x, y, z (the same), radius (same as A for a lone sphere)
//   c: density at A, density at B, age 0..1, end weights (A 0..15 + 16 * B 0..15) + 256 * texture (pillows 0..15 * 16 +
//      streaks 0..15)
//   d: the smoke's own noise coordinate at A and at B (it travels with the smoke), age 0..1 at A and at B
struct SEGMENT { float4 position : POSITION0; float4 a : TEXCOORD0; float4 b : TEXCOORD1; float4 c : TEXCOORD2; float4 d : TEXCOORD3; };
SEGMENT SegmentVS(SEGMENT input) { return input; }

float4 SegmentPS(SEGMENT input, float2 vpos : VPOS) : COLOR0 {
	float2 uv = (vpos + 0.5) * TESR_ReciprocalResolution.xy * max(SmokeLook.z, 1.0);   // the buffer may be half resolution
	// NVR's world space is centred on the camera (objects are drawn at their position minus TESR_CameraPosition): the
	// reconstructed position and the eye (the inverse view's origin) are in that space, and NVR hands the smoke over in it.
	float viewDepth;
	float3 world = reconstructWorldPosition(uv, viewDepth).xyz;
	float3 cam = mul(float4(0.0, 0.0, 0.0, 1.0), TESR_InvViewTransform).xyz;
	float3 toPixel = world - cam;
	float sceneDist = length(toPixel);
	float3 dir = toPixel / max(sceneDist, 0.0001);
	// At half or quarter resolution this pixel stands for a block of the screen: the smoke is kept up to the block's
	// farthest surface (its four corners). Step 2 then hides, screen pixel by screen pixel, whatever lies behind that
	// pixel's own surface (from the smoke's distance, alpha below): so the gun and the edges of walls stay exact.
	// (A real branch: at full resolution these reads are skipped. tex2Dlod, so the compiler does not flatten it.)
	float scale = max(SmokeLook.z, 1.0);
	[branch] if (scale > 1.0) {
		const float2 corners[4] = { float2(-1.0, -1.0), float2(1.0, -1.0), float2(-1.0, 1.0), float2(1.0, 1.0) };
		float2 reach = (0.5 * scale - 0.5) * TESR_ReciprocalResolution.xy;
		[unroll] for (int k = 0; k < 4; k++) {
			float cornerDepth;
			float3 corner = reconstructWorldPositionLod(uv + corners[k] * reach, cornerDepth).xyz;
			sceneDist = max(sceneDist, length(corner - cam));
		}
	}
	float debug = SmokeInfo.y;

	float4 a = input.a;
	float4 b = input.b;
	float4 c = input.c;
	float4 d = input.d;
	float3 A = a.xyz - cam;
	float3 AB = b.xyz - a.xyz;
	// Closest approach between the ray (t * dir, 0 <= t <= sceneDist) and the segment A + s * AB.
	float len2 = dot(AB, AB);
	float sRaw = 0.0;
	if (len2 > 0.0001) {
		float bb = dot(dir, AB), cc = dot(dir, A), ee = dot(AB, A);
		float denom = len2 - bb * bb;
		sRaw = denom > 0.00001 ? (bb * cc - ee) / denom : 0.0;
	}
	// Joints: an end shared with the neighbouring segment does not stop at a flat cut there (at a bend the two cuts
	// leave a thin crack between them). Instead the segment runs on one radius past the joint, fading out, while the
	// neighbour fades in: the two always add up to the whole, whatever the bend. A strand's free ends are rounded.
	// The flags (whole numbers; rounded so the interpolated value stays exact) give each end's weight: 1 for a free end,
	// less at a joint (how much of the wedge outside a bend to fill).
	float flags = floor(c.w + 0.5);
	float fillA = fmod(flags, 16.0) / 15.0;
	float fillB = fmod(floor(flags / 16.0 + 0.001), 16.0) / 15.0;
	float segLen = sqrt(len2);
	float eA = fillA < 0.999 ? min(a.w / max(segLen, 0.01), 0.5) : 0.0;   // how far it runs on past a joint (segment lengths)
	float eB = fillB < 0.999 ? min(b.w / max(segLen, 0.01), 0.5) : 0.0;
	float s = clamp(sRaw, -eA, 1.0 + eB);
	float sEnd = saturate(s);                                  // radius, density and age stop at the ends
	float along = 1.0;
	if (eA > 0.0) along *= lerp(fillA, 1.0, smoothstep(-eA, eA, s));
	if (eB > 0.0) along *= lerp(fillB, 1.0, 1.0 - smoothstep(1.0 - eB, 1.0 + eB, s));
	float3 P = A + s * AB;
	float r = max(lerp(a.w, b.w, sEnd), 0.05);
	float tc = dot(P, dir);
	float t = clamp(tc, 0.0, sceneDist + debug * 100000.0);   // the debug view sees through walls
	float3 off = P - t * dir;                                  // from the ray to the axis
	float rho2 = dot(off, off);
	// Where a segment adds nothing, the pixel is skipped (discard): no write, so a big quad's empty area costs little.
	if (rho2 > 9.0 * r * r) discard;                           // nowhere near the ray
	float3 axis = segLen > 0.01 ? AB / segLen : float3(0.0, 0.0, 1.0);
	float age = lerp(d.z, d.w, sEnd);

	// Edge detail: noise in the smoke's own frame (across the tube in radii, along it by the smoke's own coordinate, so
	// it travels and widens with the smoke and drifts slowly). A larger noise makes the outline lumpy; a finer one
	// eats into the edges, more as the smoke ages; the thick core stays whole.
	// The texture (each record's own, from GunFX: [Volume] fPillows for the puffs and ejection smoke, fStreaks for the
	// heat smoke and trail) changes what the same two noises do, at the same cost: pillows turn the lumps into round lobes
	// meeting in creases (billow noise) and put them in the thickness too (step 2's relief lights them on one side);
	// streaks stretch the finer noise along the flow and squeeze it across, into fibres like real gun smoke.
	float style = floor(flags / 256.0 + 0.001);
	float pillows = floor(style / 16.0 + 0.001) / 15.0;
	float streaks = fmod(style, 16.0) / 15.0;
	float coord = lerp(d.x, d.y, s) + SmokeParams.w * 0.1;
	float3 q = (-off / r) * 1.2 + axis * coord;
	float3 acr = -off / r - axis * dot(-off / r, axis);       // across the tube, in radii
	float3 qFine = lerp(q * 2.7 + 13.1, acr * 3.6 + axis * (coord * 0.35) + 3.3, streaks);
	float lumps = noise3(q);
	float fine = noise3(qFine);
	lumps = lerp(lumps, 1.0 - abs(2.0 * lumps - 1.0), pillows);
	float re = r * (0.85 - 0.05 * pillows + (0.35 + 0.1 * pillows) * lumps);
	if (rho2 > 6.25 * re * re) discard;                        // 2.5 radii
	float g = exp(-rho2 / (re * re));
	g *= lerp(1.0, 0.45 + 0.75 * lumps, pillows) * lerp(1.0, 0.35 + fine, streaks);
	float erode = SmokeParams.x * (0.25 + 0.75 * age) * (1.0 - fine) * (1.0 - 0.4 * pillows);
	g = saturate((g - erode) / max(1.0 - erode, 0.05));
	if (g <= 0.0) discard;

	// Length of the ray inside the tube: crossing it ~ sqrt(pi) r / sin(angle), capped by the segment length.
	float sinA = segLen > 0.01 ? length(cross(dir, AB)) / segLen : 1.0;
	float path = 1.7725 * re / max(sinA, 1.7725 * re / (segLen + 1.7725 * re));
	// Behind the scene surface: fade out over the radius (at least 2 units; not in the debug view). Close to the
	// camera: fade out too, so walking into your own smoke never fills the screen.
	float occ = max(saturate((sceneDist - tc) / max(r, 2.0) + 0.5), debug);
	float nearFade = saturate((tc - 6.0) / 18.0);
	float density = lerp(c.x, c.y, sEnd);
	float dens = density * g * path * along * occ * nearFade;
	if (dens <= 0.000001) discard;                             // hidden behind the scene or faded out near the camera

	// Light reaching this point through the tube, from the light's side: the tube's density integrated from here
	// toward the light (closed form for a Gaussian tube, capped by the segment's length when the light runs along it).
	float3 L = LightDirection();
	float3 o = -off;                                           // this point, from the axis
	float3 Lp = L - axis * dot(L, axis);                       // the light's direction across the tube
	float lp = length(Lp);
	float x = dot(o, Lp) / max(lp, 0.0001);                    // how far toward the light this point already is
	float side2 = max(dot(o, o) - x * x, 0.0);
	float lightPath = 0.8862 * re / max(lp, 0.15) * exp(-side2 / (re * re)) * erfcApprox(x / re);
	lightPath = min(lightPath, segLen + 1.7725 * re);
	float lit = exp(-density * lightPath * 3.0 * SmokeParams.z);
	// Depth: shade the tube as if it had a soft, round surface (its side and top toward the light brighter, the far side
	// darker), even where it is too thin to shadow itself. Its normal: across the tube where the ray passes, turned
	// toward you the nearer the ray passes to the axis. Measured against the middle (the part facing you), so the smoke
	// gains shape without getting darker or brighter overall.
	float3 across = o / (1.5 * re);
	float3 normal = normalize(across - dir * sqrt(saturate(1.0 - dot(across, across))) + 0.0001);
	float shape = clamp(1.0 + 0.4 * (dot(normal, L) - dot(-dir, L)), 0.7, 1.35);
	lit *= lerp(1.0, shape, SmokeLook.y);

	// Muzzle flash: it lights the smoke within about its reach.
	float3 toFlash = SmokeFlash.xyz - (cam + t * dir);
	float flash = SmokeFlash.w * exp(-dot(toFlash, toFlash) / max(SmokeFlashColor.w * SmokeFlashColor.w, 1.0));
	return float4(dens, dens * lit, dens * flash, dens * t);   // alpha: the smoke's distance along the ray (step 2)
}

// ---- Step 2: light the total and lay it over the image -----------------------------------------------------------
struct VSIN { float4 position : POSITION0; float2 uv : TEXCOORD0; };
struct VSOUT { float4 position : POSITION0; float2 uv : TEXCOORD0; };
VSOUT FrameVS(VSIN input) { VSOUT o; o.position = input.position; o.uv = input.uv; return o; }

// ---- The room light: once a frame, into TESR_SmokeEnv ----------------------------------------------------------------
// The smoke scatters the light of its surroundings, so it looks lighter than dark things behind it. It is taken from the
// whole view (an 8 x 8 grid of samples), so it fits interiors, whose light the sun values do not describe, and draws no
// edges of nearby objects (the gun) into the smoke. The same for every pixel, so step 2 reads it from one pixel. It
// eases to the new value over about a third of a second (EnvInfo.x, blended into the last one): with 16 samples and no
// easing, a lamp crossing one sample as you turned changed the whole haze's brightness at once (it flickered).
float4 EnvPS(VSOUT input) : COLOR0 {
	float3 env = 0.0;
	[unroll] for (int gy = 0; gy < 8; gy++)
		[unroll] for (int gx = 0; gx < 8; gx++)
			env += tex2Dlod(TESR_RenderedBuffer, float4(0.0625 + 0.125 * gx, 0.0625 + 0.125 * gy, 0, 0)).rgb;
	env = 1.8 * env / 64.0 + 0.05;
	env = lerp(dot(env, float3(0.299, 0.587, 0.114)).xxx, env, 0.5);   // smoke is grey: half the surroundings' tint
	return float4(env, saturate(EnvInfo.x));
}

// A haze blob's density along a ray, integrated from its closest point to the blob's centre out to s (kk = 1 - d^2 / R^2
// at that closest point, iR2 = 1 / R^2): the integral of (kk - s^2/R^2)^2 ds.
float BlobStretch(float s, float kk, float iR2) {
	return s * (kk * kk - s * s * iR2 * (0.6666667 * kk - 0.2 * s * s * iR2));
}

// ---- Bullet holes ------------------------------------------------------------------------------------------------------
// Bullets clear tunnels through the smoke. Each one's path (from the muzzle toward where you aimed) becomes a hole that
// opens at once to its full width (the air the bullet shoved aside; wider with a higher fBullet) and then closes again
// from its edge inward as the smoke flows back, meeting in the middle at 1.2 s. Its outline is not a perfect circle: three
// gentle waves round the path (2, 3 and 5 to a turn) with the shot's own random phases, twisting slowly along the path
// and while it closes, read where this ray passes nearest the path; and its rim is soft. The hole starts BulletBehind units
// behind the muzzle (the muzzle blast clears the air just round and behind the gun too: from first person that is the
// stretch between your eye and the gun) and runs on 1500 units; the part of a ray closer than 6 units to you is left out.
// Returns how much of the smoke goes where the ray is inside the hole (the soft rim factor times the bullet's strength, 0 for
// no hole) and the ray's stretch inside it, t0 to t1 (distances along the ray). HazePS removes the haze in exactly that
// stretch; CompositePS clears the other smoke when its distance along the ray lies in it.
// (The first version took the haze at the middle of the stretch only; looking down a tunnel that point lies far off,
// where the haze is thin, so the middle of the hole stayed full.)
static const float BulletBehind = 30.0;
float BulletHole(int j, float3 cam, float3 dir, float sceneDist, out float t0, out float t1, out float tPuff) {
	float4 from = BulletFrom[j], way = BulletWay[j];
	float since = from.w;
	// Its timing (GunFX's fBulletOpen and fBulletSeconds, in HazeInfo.z and w; older GunFX: 0.05 and 1.2 s): it widens
	// over `open` seconds, then from a tenth of its life its edge moves in until it closes at `life`.
	float open = HazeInfo.z > 0.0 ? HazeInfo.z : 0.05, life = HazeInfo.w > 0.0 ? HazeInfo.w : 1.2;
	float closeFrom = max(open, 0.1 * life);
	float closing = saturate((since - closeFrom) / max(life - closeFrom, 0.05));
	float opened = saturate(since / open);
	// The hole's radius: it widens while opening, then closes from its edge inward as the smoke flows back.
	float rt = (6.0 + 3.0 * min(way.w, 3.0)) * opened * (2.0 - opened) * (1.0 - closing * sqrt(closing));
	float3 w = cam - from.xyz;
	float wb = dot(w, way.xyz), db = dot(dir, way.xyz);
	float3 wp = w - way.xyz * wb, dp = dir - way.xyz * db;
	float A = dot(dp, dp), B = 2.0 * dot(wp, dp), C = dot(wp, wp);
	// The part of the ray that can meet the path: from 6 units in front of you to the surface, from just behind the muzzle on.
	float ta = 6.0, tb = sceneDist;
	float u0 = (-BulletBehind - wb) / (abs(db) > 0.00001 ? db : 0.00001), u1 = (1500.0 - wb) / (abs(db) > 0.00001 ? db : 0.00001);
	ta = max(ta, min(u0, u1)); tb = min(tb, max(u0, u1));
	// Where on it the ray passes nearest the path, and how near.
	float tn = clamp(-B / (2.0 * max(A, 0.000001)), ta, tb);
	float3 off = wp + dp * tn;
	float dmin = length(off);
	float hole = 0.0;
	// Where along the ray the bullet is 35 units past the muzzle (the gun's own fresh puff lies before that). Not used at
	// the moment (a try at leaving the puff uncut changed the look too much); kept for a later, separate try.
	tPuff = db > 0.00001 ? (35.0 - wb) / db : 0.0;
	t0 = 1.0e9; t1 = -1.0e9;                                   // (a single exit below: D3DX's compiler and early returns)
	[branch] if (rt >= 0.3 && way.w > 0.0 && tb > ta && dmin < 1.7 * rt) {
		// The outline: the angle round the path there, as (cos, sin), raised to 2, 3 and 5 turns.
		float3 e1 = normalize(cross(way.xyz, abs(way.z) < 0.9 ? float3(0.0, 0.0, 1.0) : float3(1.0, 0.0, 0.0)));
		float3 e2 = cross(way.xyz, e1);
		float2 z = float2(dot(off, e1), dot(off, e2)) / max(dmin, 0.001);
		float2 z2 = float2(z.x * z.x - z.y * z.y, 2.0 * z.x * z.y);
		float2 z3 = float2(z2.x * z.x - z2.y * z.y, z2.x * z.y + z2.y * z.x);
		float2 z5 = float2(z3.x * z2.x - z3.y * z2.y, z3.x * z2.y + z3.y * z2.x);
		float seed = frac((SmokeParams.w - since) * 0.6180339) * 97.0;   // the shot's own (its birth time)
		// Along the path the outline only twists slowly (about half a turn over a room): looking down a tunnel, each pixel
		// reads it at a different depth (where its ray passes nearest the path), so an outline that changed quickly along the
		// path came out as broken slivers and gaps round the far end of a young hole.
		float3 phase = seed * float3(1.0, 1.7, 2.3) + (wb + db * tn) * float3(0.0012, 0.0018, 0.0026) + since * float3(0.9, -1.3, 1.7);
		float3 ps, pc;
		sincos(phase, ps, pc);
		// The 3 and 5 waves are odd (they flip for the opposite angle): rays passing just either side of the path read opposite
		// angles, so they fade out near the path's middle, or the outline flipped there and drew a crisp line along the bullet's
		// path (a dark dotted line where the haze is thick). The 2 wave is the same both ways and stays.
		float odd = smoothstep(0.0, 0.5, dmin / max(rt, 0.001));
		float ragged = 0.42 * (z2.y * pc.x + z2.x * ps.x) + odd * (0.33 * (z3.y * pc.y + z3.x * ps.y) + 0.25 * (z5.y * pc.z + z5.x * ps.z));
		float rn = rt * (1.0 + 0.35 * ragged);
		// How much it clears for this ray: full near the path, fading out across its edge like a soft ball (1 on the path,
		// 0.14 at the outline, 0.04 at 1.25 times it). BulletField adds these up over all the bullets, so holes close together
		// melt into one bigger, smooth hole instead of overlapping circles with sharp corners where their edges cross.
		float x = dmin / rn;
		float k = exp(-2.0 * x * x) * saturate(way.w);
		// (The whole path counts, from just behind the muzzle on.)
		float ta2 = ta, tb2 = tb;
		// The ray's stretch where it counts (within 1.25 times the outline; a ray along the path: all of it, if it is inside).
		float R2 = 1.5625 * rn * rn;
		float disc = B * B - 4.0 * A * (C - R2);
		float sq = sqrt(max(disc, 0.0));
		float h0 = A > 0.000001 ? max(ta2, (-B - sq) / (2.0 * A)) : ta2;
		float h1 = A > 0.000001 ? min(tb2, (-B + sq) / (2.0 * A)) : (C < R2 ? tb2 : ta2);
		// While it is small (just opening, or nearly closed) it is also faint: it fades in as it widens and out as it closes,
		// instead of popping in or ending as a thin dotted line along the bullet's path.
		k *= smoothstep(0.3, 3.0, rt);
		[branch] if ((A <= 0.000001 || disc > 0.0) && h1 > h0 && k > 0.0) { t0 = h0; t1 = h1; hole = k; }
	}
	return hole;
}

// All the bullets' holes for one ray, merged: their contributions (BulletHole) added up, so close holes flow together
// into one, and the hole is where the total is high (soft from 0.07 to 0.375). Its stretch along the ray is the bullets'
// stretches averaged by how much each contributes (cubed), so it moves smoothly as holes come and go (two separate stretches
// picked one way or the other drew hard seams where the choice flipped). Returns how much of the smoke in t0..t1 goes.
float BulletField(float3 cam, float3 dir, float sceneDist, out float t0, out float t1, out float tPuff) {
	float total = 0.0, weight = 0.0, from = 0.0, to = 0.0, puff = 0.0;
	[loop] for (int j = 0; j < 16; j++) {
		if (j >= HazeInfo.y) break;
		float h0, h1, hp;
		float c = BulletHole(j, cam, dir, sceneDist, h0, h1, hp);
		// (The stretch follows the bullet this ray passes closest to: weighted by c cubed, so it is mostly that one's.)
		[branch] if (c > 0.0) { float w = c * c * c; total += c; weight += w; from += w * h0; to += w * h1; puff += w * hp; }
	}
	t0 = weight > 0.0 ? from / weight : 1.0e9;
	t1 = weight > 0.0 ? to / weight : -1.0e9;
	tPuff = weight > 0.0 ? puff / weight : 0.0;
	return total > 0.0 ? smoothstep(0.07, 0.375, total) : 0.0;   // one hole: clear to 0.7 of its outline, none from 1.15
}

// ---- Haze: every blob in one pass, at quarter resolution -----------------------------------------------------------
// A blob is a soft sphere, density rho0 (1 - r^2/R^2)^2 (its edge fades out smoothly), integrated along the view ray in
// closed form from 6 units in front of you to the nearest surface in this pixel's block (so it never shows over
// something in front of it; at the edges of things it thins a little, which a haze hides). Its top and the side toward
// the light are lighter. Then noise, fixed in the world at the haze's middle distance along the ray and drifting slowly,
// makes the total lumpy and layered rather than smooth balls: once per pixel, not per blob (noise for every blob a ray
// passes through cost a millisecond in a full room).
// UNOFFICIAL 2026-10-13 (user: the haze looks swirly at some angles): the noise used to be read at the haze's middle
// distance along each ray. That distance follows the walls (the haze stops at them) and the blobs (the middle of each one's
// stretch), so the points it was read at lay on a shrunken copy of the room, or on a sphere through your eye: looking
// along a wall or a corridor, or past a blob, the noise was smeared into rings round the vanishing point, and it slid as you
// moved (tests: work\haze_swirl.cpp). Now it is read on four fixed shells round the eye (HazeShell), each counting by how
// much of this ray's haze lies in its band of distances (worked out exactly with the blobs, HazeBandEdge): the lumps keep
// their size and shape whatever the walls or blobs do, and far shells read a coarser mip so they do not shimmer.
// Writes optical depth, light share and flash (as step 1) into TESR_SmokeHaze, and in alpha the distance to the nearest surface
// of its block (what it was cut at), for step 2's depth-aware upscaling (HazeAt).
// How much of the segment smoke (TESR_SmokeBuffer and TESR_SmokeBufferLow) the bullets cut along this ray (0 none .. 1
// all), for one quarter-resolution pixel: worked out here in the haze pass and read by CompositePS (CutAt), instead of
// every bullet for every screen pixel there, which cost 3 to 5 ms in game firing a burst into thick haze at 2560x1440 (the
// smoke itself is drawn at half and an eighth of the resolution anyway). The smoke is taken as spread along the ray round
// its mean distance (6 units plus 15 % of the distance either way); each bullet's hole counts by the share of that spread
// inside its own stretch along the ray, and these are added up as in BulletField, so close holes merge.
float SegmentCut(float2 uv, float3 cam, float3 dir, float surface) {
	float cut = 0.0;
	float4 smoke = SmokeAtLod(uv);
	[branch] if (smoke.r >= 0.0005 && HazeInfo.y > 0.5) {
		float smokeDist = smoke.a / max(smoke.r, 0.000001);
		float spread = 6.0 + 0.15 * smokeDist;
		float total = 0.0, covered = 0.0, strongest = 0.0;
		[loop] for (int j = 0; j < 16; j++) {
			if (j >= HazeInfo.y) break;
			float t0, t1, tPuff;
			float c = BulletHole(j, cam, dir, surface, t0, t1, tPuff);
			[branch] if (c > 0.0) {
				total += c;
				covered += c * saturate((min(t1, smokeDist + spread) - max(t0, smokeDist - spread)) / (2.0 * spread));
				strongest = max(strongest, c);
			}
		}
		cut = total > 0.0 ? smoothstep(0.07, 0.375, total) * saturate(covered / strongest) : 0.0;
	}
	return cut;
}

struct HazeOutput {
	float4 haze : COLOR0;   // TESR_SmokeHaze
	float4 cut : COLOR1;    // TESR_SmokeCut
};

// The haze's noise shells round the eye (game units) and the band edges between them (geometric: near haze shows its lumps
// big, far haze small, as in a real fog).
static const float4 HazeShell = float4(16.0, 90.0, 355.0, 1300.0);
static const float3 HazeBandEdge = float3(45.0, 180.0, 700.0);

HazeOutput HazePS(VSOUT input, float2 vpos : VPOS) {
	float scale = max(SmokeLook.z, 1.0);
	float2 uv = (vpos + 0.5) * TESR_ReciprocalResolution.xy * scale;
	float viewDepth;
	float3 world = reconstructWorldPositionLod(uv, viewDepth).xyz;
	float3 cam = mul(float4(0.0, 0.0, 0.0, 1.0), TESR_InvViewTransform).xyz;
	float3 toPixel = world - cam;
	float sceneDist = length(toPixel);
	float3 dir = toPixel / max(sceneDist, 0.0001);
	const float2 corners[4] = { float2(-1.0, -1.0), float2(1.0, -1.0), float2(-1.0, 1.0), float2(1.0, 1.0) };
	float2 reach = (0.5 * scale - 0.5) * TESR_ReciprocalResolution.xy;
	[unroll] for (int k = 0; k < 4; k++) {
		float cornerDepth;
		float3 corner = reconstructWorldPositionLod(uv + corners[k] * reach, cornerDepth).xyz;
		sceneDist = min(sceneDist, length(corner - cam));
	}
	float3 L = LightDirection();
	float flashReach2 = max(SmokeFlashColor.w * SmokeFlashColor.w, 1.0);
	float3 sum = 0.0;
	float tNear = 1e9, tFar = 0.0;    // UNOFFICIAL 2026-10-13: where along the ray the haze starts and ends (the noise below)
	[loop] for (int i = 0; i < 40; i++) {
		if (i >= HazeInfo.x) break;
		float4 shape = HazeShape[i];
		float3 oc = shape.xyz - cam;
		float R2 = shape.w * shape.w;
		float tc = dot(oc, dir);
		float d2 = dot(oc, oc) - tc * tc;
		[branch] if (d2 < R2) {
			float h = sqrt(R2 - d2);
			float s0 = max(-h, 6.0 - tc), s1 = min(h, sceneDist - tc);
			[branch] if (s1 > s0) {
				// Integral of (kk - s^2/R^2)^2 ds = kk^2 s - 2 kk s^3 / (3 R^2) + s^5 / (5 R^4); through the middle it comes to
				// 16 R / 15, so rho0 = (optical depth through the middle) * 15 / (16 R).
				float iR2 = 1.0 / R2, kk = 1.0 - d2 * iR2;
				float F1 = s1 * (kk * kk - s1 * s1 * iR2 * (0.6666667 * kk - 0.2 * s1 * s1 * iR2));
				float F0 = s0 * (kk * kk - s0 * s0 * iR2 * (0.6666667 * kk - 0.2 * s0 * s0 * iR2));
				float rho = HazeLook[i].x * 0.9375 / shape.w;
				float tau = rho * (F1 - F0);
				tNear = min(tNear, tc + s0);
				tFar = max(tFar, tc + s1);
				float sm = 0.5 * (s0 + s1);
				float3 rel = (dir * (tc + sm) - oc) / shape.w;     // the middle of the ray's stretch, from the centre, in radii
				float lit = 0.75 + 0.25 * dot(rel, L);
				float3 toFlash = SmokeFlash.xyz - shape.xyz;
				float flash = SmokeFlash.w * exp(-dot(toFlash, toFlash) / (flashReach2 + 0.25 * R2));
				sum += tau * float3(1.0, lit, flash);
			}
		}
	}
	// Bullets clear tunnels through the haze (BulletField, above): the haze in the merged holes' stretch along the ray goes.
	[branch] if (sum.x > 0.00001 && HazeInfo.y > 0.5) {
		float h0, h1, hp;
		float k = BulletField(cam, dir, sceneDist, h0, h1, hp);
		[branch] if (k > 0.0 && h1 > h0) {
			float inside = 0.0;
			[loop] for (int i = 0; i < 40; i++) {
				if (i >= HazeInfo.x) break;
				float4 shape = HazeShape[i];
				float3 oc = shape.xyz - cam;
				float R2 = shape.w * shape.w;
				float tc = dot(oc, dir);
				float d2 = dot(oc, oc) - tc * tc;
				[branch] if (d2 < R2) {
					float h = sqrt(R2 - d2);
					float iR2 = 1.0 / R2, kk = 1.0 - d2 * iR2;
					float s0 = max(-h, h0 - tc), s1 = min(h, h1 - tc);
					[branch] if (s1 > s0) inside += HazeLook[i].x * 0.9375 / shape.w * (BlobStretch(s1, kk, iR2) - BlobStretch(s0, kk, iR2));
				}
			}
			// Softly by how see-through it looks, not by how much haze goes (see the same in CompositePS).
			float seen = lerp(exp(-sum.x), exp(-max(sum.x - inside, 0.0)), k);
			sum *= saturate(-log(max(seen, 0.000001)) / sum.x);
		}
	}
	[branch] if (sum.x > 0.00001) {
		// UNOFFICIAL 2026-10-13 (user: the haze shifts about as you walk toward or away from it; it should keep moving on its
		// own instead): the shells round the eye moved with you, so walking slid the noise through the world. Now it is read
		// where this ray crosses planes fixed in the world (across the axis the ray runs most along, every D units, D doubling
		// for long stretches so there are at most 8), inside the haze's own stretch along the ray: the lumps stay put as you
		// move, a plane coming into the stretch fades in rather than popping. The noise itself drifts all the time (a slow
		// breeze, rising a little), so the haze churns while you stand still.
		float pixelAngle = 2.0 * scale * TESR_ReciprocalResolution.y / TESR_ProjectionTransform[1][1];
		float3 drift = float3(0.35, 0.2, 0.25) * SmokeParams.w * 0.05;
		float3 ad = abs(dir);
		float3 axis = ad.x > ad.y && ad.x > ad.z ? float3(1.0, 0.0, 0.0) : (ad.y > ad.z ? float3(0.0, 1.0, 0.0) : float3(0.0, 0.0, 1.0));
		float dirA = dot(dir, axis);
		float camA = dot(TESR_CameraPosition.xyz, axis);
		float extent = max(tFar - tNear, 0.0);
		float D = 30.0 * exp2(ceil(log2(max(extent / 210.0, 1.0))));
		float side = dirA >= 0.0 ? 1.0 : -1.0;
		float atNear = camA + dirA * tNear;
		float first = (side > 0.0 ? ceil(atNear / D) : floor(atNear / D)) * D;
		float total = 0.0, total2 = 0.0, acc = 0.0;
		[loop] for (int k = 0; k < 8; k++) {
			float t = (first + side * k * D - camA) / (abs(dirA) > 0.0001 ? dirA : 0.0001);
			if (t > tFar) break;
			float w = saturate((t - tNear) / (0.5 * D)) * saturate((tFar - t) / (0.5 * D));
			[branch] if (w > 0.0) {
				float3 p = (dir * t + TESR_CameraPosition.xyz) / 45.0 + drift;
				acc += w * noiseFbmLod(p, log2(max(t * pixelAngle / 2.8125, 1.0)));
				total += w;
				total2 += w * w;
			}
		}
		// (A stretch thinner than the planes' spacing: its middle, once.)
		[branch] if (total < 0.001) {
			float t = 0.5 * (tNear + tFar);
			acc = noiseFbmLod((dir * t + TESR_CameraPosition.xyz) / 45.0 + drift, log2(max(t * pixelAngle / 2.8125, 1.0)));
			total = 1.0;
			total2 = 1.0;
		}
		// Averaging several reads flattens the contrast by about the square root of how many count: restored by that.
		float spread = clamp(sqrt(total * total / max(total2, 0.000001)), 1.0, 2.5);
		float n = saturate((acc / total - 0.5) * 1.6 * spread + 0.5);
		sum *= lerp(0.25, 1.7, n);
	}
	HazeOutput o;
	o.haze = float4(sum.xyz, sceneDist);
	o.cut = float4(SegmentCut(uv, cam, dir, sceneDist), 0.0, 0.0, sceneDist);
	return o;
}

// The haze at a screen pixel, from the quarter-resolution buffer: of the four haze pixels round it, those whose surface
// (alpha, see HazePS) lies at about this pixel's own distance count most, so the haze neither bleeds across the edges of
// things nor steps along them in blocks (smooth blending of the quarter-resolution pixels did both: hard, jagged edges where
// the haze or a bullet's hole met something nearer or farther).
float CutAt(float2 suv, float surface) {
	// As HazeAt, from TESR_SmokeCut (its own surface distances in w).
	float2 hres = ceil(1.0 / (4.0 * TESR_ReciprocalResolution.xy));
	float2 p = suv * hres - 0.5;
	float2 f = frac(p);
	float2 at = (floor(p) + 0.5) / hres, step = 1.0 / hres;
	float4 c00 = tex2Dlod(TESR_SmokeCut, float4(at, 0, 0));
	float4 c10 = tex2Dlod(TESR_SmokeCut, float4(at + float2(step.x, 0.0), 0, 0));
	float4 c01 = tex2Dlod(TESR_SmokeCut, float4(at + float2(0.0, step.y), 0, 0));
	float4 c11 = tex2Dlod(TESR_SmokeCut, float4(at + step, 0, 0));
	float4 w = float4((1.0 - f.x) * (1.0 - f.y), f.x * (1.0 - f.y), (1.0 - f.x) * f.y, f.x * f.y);
	float4 off = abs(float4(c00.a, c10.a, c01.a, c11.a) - surface) / (2.0 + 0.04 * surface);
	w *= 1.0 / (1.0 + off * off * off) + 0.0001;
	return dot(float4(c00.x, c10.x, c01.x, c11.x), w) / dot(w, 1.0);
}

float4 HazeAt(float2 suv, float surface) {
	float2 hres = ceil(1.0 / (4.0 * TESR_ReciprocalResolution.xy));
	float2 p = suv * hres - 0.5;
	float2 f = frac(p);
	float2 at = (floor(p) + 0.5) / hres, step = 1.0 / hres;
	float4 h00 = tex2Dlod(TESR_SmokeHaze, float4(at, 0, 0));
	float4 h10 = tex2Dlod(TESR_SmokeHaze, float4(at + float2(step.x, 0.0), 0, 0));
	float4 h01 = tex2Dlod(TESR_SmokeHaze, float4(at + float2(0.0, step.y), 0, 0));
	float4 h11 = tex2Dlod(TESR_SmokeHaze, float4(at + step, 0, 0));
	float4 w = float4((1.0 - f.x) * (1.0 - f.y), f.x * (1.0 - f.y), (1.0 - f.x) * f.y, f.x * f.y);
	float4 off = abs(float4(h00.a, h10.a, h01.a, h11.a) - surface) / (2.0 + 0.04 * surface);
	w *= 1.0 / (1.0 + off * off * off) + 0.0001;
	return (h00 * w.x + h10 * w.y + h01 * w.z + h11 * w.w) / dot(w, 1.0);
}

float4 CompositePS(VSOUT input, float2 vpos : VPOS) : COLOR0 {
	float4 color = tex2D(TESR_RenderedBuffer, input.uv);
	// The smoke buffers are read at this pixel's exact centre, so the full-resolution one is not blurred by the smooth
	// sampling the half- and quarter-resolution ones need.
	const float2 suv = (vpos + 0.5) * TESR_ReciprocalResolution.xy;
	float4 smokeBuffer = SmokeAt(suv);
	float4 haze = tex2D(TESR_SmokeHaze, suv);                // the gun-smoke haze (HazePS), added in below
	float tau = smokeBuffer.r + haze.r;
	if (tau < 0.0005) return color;
	// Your own gun (the first-person model) is always in front of the smoke, so smoke never shows inside it. (It is
	// drawn with its own field of view, so comparing its depth with the smoke's is never exact near the gun.) Not in
	// the debug view, which shows all the smoke.
	if (SmokeInfo.w > 0.5 && SmokeInfo.y < 0.5 && tex2D(TESR_DepthBufferViewModel, input.uv).x > 0.0) return color;
	float transmittance = exp(-tau);
	if (SmokeInfo.y > 0.5) {
		color.rgb = lerp(color.rgb, float3(1.0, 0.05, 0.05), saturate(1.0 - exp(-4.0 * tau)));
		return color;
	}
	// Behind this pixel's own surface (the gun, a wall's edge): hidden. Step 1 kept smoke up to the farthest surface of its
	// block (at half or quarter resolution); here each screen pixel compares the smoke's distance (density-weighted along
	// the ray) with its own surface, softly (over 4 units, more far away).
	float viewDepth;
	float3 world = reconstructWorldPosition(input.uv, viewDepth).xyz;
	float3 camera = mul(float4(0.0, 0.0, 0.0, 1.0), TESR_InvViewTransform).xyz;
	float surface = length(world - camera);
	haze = HazeAt(suv, surface);                               // (sharper than the smooth read above, which only decided to start)
	// (The haze is already cut at the nearest surface of its blocks, so this is for the smoke only.)
	float smokeDist = smokeBuffer.a / max(smokeBuffer.r, 0.000001);
	// Bullets cut the segment smoke too (HazePS cut the haze): a dense puff in front of a clean tunnel through the haze behind it
	// left the hole showing through the puff as a dark, blurry blob that closed with the hole. The buffer holds the smoke's
	// optical depth and its mean distance along the ray (smokeDist), not where along the ray it is, so each bullet's hole is
	// worked out for this pixel's own ray (BulletHole, as in HazePS) and the smoke is taken as spread along the ray round its
	// mean distance (6 units plus 15 % of the distance either way: a strand is thin, a cloud the ray crosses deep). How much
	// goes: the merged holes' strength (as BulletField) times the share of that spread inside the bullets' own stretches
	// along the ray, added up (each weighted by its hole, relative to the strongest) so tunnels one after another along the
	// ray cover the cloud together. (All or nothing by the mean distance alone drew hard, straight-edged wedges where a hole's
	// edge crossed a big strand or cloud: just inside the edge the stretch is short. One stretch averaged over all the
	// bullets left a grey wash in a thick cloud hit by a spread burst: the ray runs through several of their tunnels one
	// after another, and the average covered only part of the cloud.) All four channels go together, so the distance stays
	// right. Only where there is smoke and bullets.
	[branch] if (smokeBuffer.r >= 0.0005 && HazeInfo.y > 0.5) {
		// The cut, worked out at a quarter of the resolution by the haze pass (SegmentCut), read as the haze is (CutAt).
		float cut = CutAt(suv, surface);
		// While you move (walking or running as you fire), your gun's own puff and trail round you stream past the tunnels,
		// which stay where the bullets were fired: cut there, they flickered on and off in your face instead of showing a
		// hole. So then the smoke within 60 units of you is left alone, the cut coming back fully by 150 units (holes further
		// out, and the haze's, stay). Standing still: unchanged.
		cut *= 1.0 - saturate(BulletMove.x) * (1.0 - smoothstep(60.0, 150.0, smokeDist));
		// The edge softens by how see-through the smoke looks, not by how much of it goes: a thick cloud stays nearly solid
		// until almost all of it is cut, so cutting it in even steps put the whole visible change in a thin crisp line at the
		// very edge of the hole. Now the light let through rises evenly across the hole's soft edge, thick smoke or thin.
		[branch] if (cut > 0.0) {                          // (no hole: untouched, not even by rounding)
			float seen = lerp(exp(-smokeBuffer.r), 1.0, cut);
			smokeBuffer *= saturate(-log(max(seen, 0.000001)) / max(smokeBuffer.r, 0.000001));
		}
	}
	if (SmokeLook.w > 0.5) smokeBuffer *= saturate((surface - smokeDist) / max(4.0, 0.08 * smokeDist) + 0.5);   // only with low-resolution smoke
	float tauSmoke = smokeBuffer.r, tauHaze = haze.r;
	tau = tauSmoke + tauHaze;
	if (tau < 0.0005) return color;
	transmittance = exp(-tau);
	const float age = 0.35;                                  // a mid-life tint (the buffer's alpha holds distance now)
	// Ambient light: the light of the surroundings (EnvPS, worked out once a frame).
	float3 env = tex2Dlod(TESR_SmokeEnv, float4(0.5, 0.5, 0, 0)).rgb;
	float3 dir = normalize(world - camera);
	// Direct light, scattered mostly forward (Henyey-Greenstein, g 0.6, 1 straight toward the light): the smoke glows
	// when you look toward the sun, and its far side (less light reaches it) is darker.
	float cosView = dot(dir, LightDirection());
	float forward = 0.064 / pow(1.36 - 1.2 * cosView, 1.5);
	float outdoors = SmokeInfo.z;
	// Outdoors the sun is the main light (the sky's ambient a smaller part), so the lit and shaded sides differ clearly.
	float3 ambient = lerp(env, 0.72 * TESR_SunAmbient.rgb, 0.5 * outdoors) * lerp(0.75, 0.8, outdoors);
	float3 direct = outdoors > 0.5 ? 0.5 * TESR_SunColor.rgb * (0.4 + 1.1 * forward) : 0.45 * env;   // indoors: soft, from above
	// The smoke and the haze are lit each by itself (darker deep inside each; the share the light reaches and the flash
	// from each one's own buffer), then mixed by how much of each there is: lit together, a strand in thick haze came out
	// darker than the haze round it, like a shadow.
	float deepSmoke = 1.0 - 0.5 * SmokeParams.z * (1.0 - exp(-tauSmoke));
	float deepHaze = 1.0 - 0.5 * SmokeParams.z * (1.0 - exp(-tauHaze));
	float3 smokeLight = ambient * deepSmoke + direct * saturate(smokeBuffer.g / max(tauSmoke, 0.000001)) +
		SmokeFlashColor.rgb * (smokeBuffer.b / max(tauSmoke, 0.000001));
	float3 hazeLight = ambient * deepHaze + direct * saturate(haze.g / max(tauHaze, 0.000001)) +
		SmokeFlashColor.rgb * (haze.b / max(tauHaze, 0.000001));
	// Relief: the smoke's thickness read like a height map a couple of pixels to each side, lit from the light's
	// direction on screen, so lumps and overlaps get a lit and a shaded edge (a flat area stays as it is). Only where there
	// is smoke (not where there is only haze, which has no edges to light).
	[branch] if (SmokeLook.y > 0.001 && smokeBuffer.r >= 0.0005) {
		float2 px = TESR_ReciprocalResolution.xy * 2.0;
		float hL = 1.0 - exp(-SmokeAtLod(suv - float2(px.x, 0.0)).r);
		float hR = 1.0 - exp(-SmokeAtLod(suv + float2(px.x, 0.0)).r);
		float hU = 1.0 - exp(-SmokeAtLod(suv - float2(0.0, px.y)).r);
		float hD = 1.0 - exp(-SmokeAtLod(suv + float2(0.0, px.y)).r);
		float3 bump = normalize(float3(hL - hR, hD - hU, 0.3));                   // screen: x right, y up, z toward you
		float3 lightView = mul((float3x3)TESR_InvViewTransform, LightDirection());  // x right, y up, z forward
		float3 lightScreen = normalize(float3(lightView.x, lightView.y, 0.3 - lightView.z));
		float relief = (dot(bump, lightScreen) * 0.5 + 0.5) / max(lightScreen.z * 0.5 + 0.5, 0.2);
		smokeLight *= lerp(1.0, clamp(relief, 0.8, 1.25), SmokeLook.y);   // the smoke's edges only, not the haze behind it
	}
	float3 smoke = (smokeLight * tauSmoke + hazeLight * tauHaze) / tau * SmokeParams.y;
	// Colour: fresh smoke is whiter and a little brighter, greying as it ages; thin smoke scatters blue a little more
	// (a cool, bluish grey), thick smoke is neutral; light coming through the smoke loses a little blue (the scene
	// behind it warms slightly).
	float tint = SmokeLook.x;
	float thin = exp(-1.5 * tau);
	smoke *= lerp(1.0.xxx, float3(0.86, 0.93, 1.06), thin * tint) * lerp(1.0, 1.12 - 0.24 * age, tint);
	float3 through = exp(-tau * lerp(1.0.xxx, float3(0.93, 1.0, 1.08), tint));
	if (SmokeInfo.x < -0.5) { smoke = float3(0.0, 1.0, 0.0); through = exp(-tau).xxx; }   // test mode: the smoke in plain green
	color.rgb = color.rgb * through + smoke * (1.0 - through);
	return color;
}

// First technique: the one NVR's effect chain draws (step 2). VolumetricSmoke.h runs step 1 itself just before.
technique VolumetricSmoke {
	pass P0 {
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 CompositePS();
		ZEnable = FALSE; ZWriteEnable = FALSE; AlphaBlendEnable = FALSE; AlphaTestEnable = FALSE;
		CullMode = NONE; ColorWriteEnable = 15;
	}
}

technique VolumetricSmokeEnv {
	pass P0 {
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 EnvPS();
		ZEnable = FALSE; ZWriteEnable = FALSE; AlphaTestEnable = FALSE; StencilEnable = FALSE; CullMode = NONE; ColorWriteEnable = 15;
		AlphaBlendEnable = TRUE; SrcBlend = SRCALPHA; DestBlend = INVSRCALPHA; BlendOp = ADD;   // eased into the last value
	}
}

technique VolumetricSmokeHaze {
	pass P0 {
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 HazePS();
		ZEnable = FALSE; ZWriteEnable = FALSE; AlphaBlendEnable = FALSE; AlphaTestEnable = FALSE; StencilEnable = FALSE;
		CullMode = NONE; ColorWriteEnable = 15;
	}
}

technique VolumetricSmokeSegments {
	pass P0 {
		VertexShader = compile vs_3_0 SegmentVS();
		PixelShader = compile ps_3_0 SegmentPS();
		ZEnable = FALSE; ZWriteEnable = FALSE; AlphaTestEnable = FALSE; StencilEnable = FALSE; CullMode = NONE;
		AlphaBlendEnable = TRUE; SrcBlend = ONE; DestBlend = ONE; BlendOp = ADD; ColorWriteEnable = 15;
	}
}
