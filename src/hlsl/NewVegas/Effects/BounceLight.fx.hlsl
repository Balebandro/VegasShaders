// UNOFFICIAL, optional (off by default): one bounce of indirect light, gathered from the screen.
// Written from scratch for this build, after the general idea of screen-space indirect lighting.
//
// Passes 1-4 run at half (QuarterResolution: quarter) resolution into the effect's own buffers (BounceLight.cpp).
// Pass 1 (Prepare): depth, normal and colour of each low-resolution pixel packed into one texel (see Prepare).
// Pass 2 (Gather, rgb = bounced light, a = view depth of the pixel it belongs to): for each pixel, 8 points on a disc around it in its tangent plane (lifted slightly off the
// surface), each projected to the screen. The surface found there sends the light it shows on screen back toward
// the pixel, weighted by how much each faces the other and by how close it is to the point aimed at. Rotated per
// pixel by blue noise. The disc is kept within a small part of the screen: close up, a full-size disc would read
// pixels scattered over the whole screen, which is very slow (texture cache misses) and pulls in unrelated colour.
// Passes 3-4: depth-aware blur, horizontal then vertical, to smooth out the noise.
// Pass 5 (Combine, full resolution): depth-aware upsample, then the bounced light added to the scene, tinted by
// the receiving surface's own hue (there is no albedo buffer, so the hue of the pixel stands in for it).
//
// Screen-space limits: only light from things visible on screen bounces, and it fades at the screen edges.
// The first-person weapon and anything touching the camera neither receive nor send bounce light.
//
// UNOFFICIAL additions, each switchable ([Shaders.BounceLight.Main] OffScreenLight, MultiBounce, Temporal, HalfSamples);
// with all of them off BounceLight.cpp runs the first technique, exactly as before. The other techniques have the same
// five passes, buffers and result (so Specular's SpecularBounce combine takes them unchanged):
// - BounceLightPlus: Prepare and Gather with the off-screen light and the extra bounces (below).
// - BounceLightTemporal (8 samples a frame) and BounceLightTemporalHalf (4): the same, plus last frame's bounce reused.
//   The sample pattern turns from frame to frame, and each frame's gather is blended into last frame's at the same spot
//   (found by reprojection; not where the depth there does not match: something moved in front, or came into view).
//   NVR_BounceHistory is last frame's gather then; this frame's goes to the other history buffer.
// Off-screen light: a sample that leaves the screen (or lands on the first-person weapon) used to add nothing, which
//   darkened the bounce toward the screen edges and round the weapon. Its surface is unknown: it is now taken to face
//   the pixel as the seen samples' surfaces do on average, and to send light between the ambient light's (the weather's
//   outdoors, the cell's indoors) and the seen samples' average.
// Extra bounces: in Prepare, the bounce light each surface showed last frame (what Combine added to it) joins the light
//   it sends on, so light keeps bouncing, one more time each frame.
// UNOFFICIAL NeonSpill ([Shaders.BounceLight.Main] NeonSpill, 0: off): long-range light from bright lights (neon signs,
//   lamps, fires) onto the surfaces round them, the look of the screen-space "RTGI" shaders. After ZN_FX's SDIL/DAMP RT
//   (Zenteon, GPL-3, github.com/Zenteon/ZN_FX): instead of tracing rays, each direction reads ever smaller, blurrier
//   copies of the screen at ever greater distances, so it reaches far for a few reads. Here:
//   - BounceSpillPyramid builds the copies: level 0 320 texels wide (whatever the screen's size, so the reach is the same
//     share of the screen at any resolution), each next level half the one before, seven in all. Only what is bright
//     enough to count as a light goes in (SpillLight), with its depth.
//   - GatherPlus then reads them along 8 directions round each pixel, one read per level, 1.5 texels of that level out
//     (so the reads of neighbouring directions overlap and a small sign is not missed between them), and adds the light
//     each read area sends to the pixel: its light times its size seen from the pixel (the area it stands for on screen,
//     made world size at its depth), times how much the pixel faces it, unless something nearer along the same
//     direction rises higher (a wall, a kerb, a car in between).
//   - BounceSpillGather does that per low-resolution pixel into a buffer of its own, with a memory: blended into last
//     frame's at the same spot (no flicker from a small lamp that some frames' reads hit and others miss); and while the
//     view moves, a spot's light may fade no faster than over NeonSpillMemory seconds (it may still rise at once), so the
//     light of a sign that goes off screen as the view turns lingers and fades instead of vanishing. With the view still
//     it follows the lights as they are, so a sign that blinks still blinks. GatherPlus adds the buffer.
//   - What counts as a light (SpillLight) is relative to the scene's own light: brighter than LightLevel times a white
//     surface lit by the sky and the sun or moon outdoors, or by the cell's light indoors (BounceLight.cpp, NVR_SpillMemory.w).
//     Nothing measured on screen, so it does not change as you turn or zoom: a level taken from the screen's average
//     made lights flicker in and out as the view moved, and a lamp you zoomed in on raised it until it stopped counting.
//   - NeonSpillNear and NeonSpillFar weigh the reads close to a pixel and far from it (between them in steps, read by
//     read): the close ones make the glow right round a light (strong round a small, bright sign), the far ones carry big
//     signs' light down the street. NeonSpillBrightCap limits how bright a light counts (in multiples of the light test),
//     so a tiny glaring bulb cannot outshine a big sign. Pixels that are themselves lights get no spill: a sign's own face
//     lit by its tubes washed it out.
//   - NeonSpillSeparateDepth (technique BounceSpillGatherSeparate; 0: as before): a read's light is placed at its own depth and
//     on-screen spot, not at a depth mixed with the empty texels round it (what stands in the way), and light that may lie on the
//     receiving pixel's own plane (within a quarter texel of where it is read) does not count: a sign no longer lights the flat
//     wall or panel it is painted on, the street keeps its light (SpillStepSeparate).
//   - NeonSpillMaskSpillOnly (0: as before): the test that keeps light pixels from being lit is applied to the spill alone (in
//     GatherPlus), not to all the bounce in Combine. NeonSpillCapSceneLight (BounceLight.cpp only): NeonSpillBrightCap counts in
//     multiples of the scene's own light, not of the light test, so a stricter LightLevel no longer lets very bright lights
//     spill MORE (their spill used to grow with the light test).
//   - NeonSpillHoldAfterTurn (seconds, 0: as before): the memory's hold, which only works while the view moves, keeps working that
//     long after the view stops, so the light of a sign turned off screen fades instead of draining in a few frames.
//   The result joins the short-range bounce in the gather, so the blur, temporal reuse and combine take it as it is. With
//   NeonSpill on, BounceLight.cpp always runs one of the techniques with GatherPlus and its own combine.

float4 TESR_BounceLightData; // x: strength, y: radius (world units), z: draw distance, w: blur radius (pixels)
float4 TESR_ReciprocalResolution;
float4 NVR_BounceLayout; // zw: 1 / half-resolution buffer size
float4 NVR_BounceTemporal;    // x: share of last frame's gather kept (0: none), y: extra bounces (0: none), z: frame number
float4 NVR_BounceAmbient;     // rgb: the ambient light an unknown surface sends (linear), w: off-screen light (0: none)
float4x4 NVR_BounceReproject; // this frame's view space -> last frame's clip space
// UNOFFICIAL NeonSpill. x: spill strength (0: off), y: the short-range bounce's strength in the gather (1 with the spill off:
// the combine applies it then; with the spill on the combine's strength is 1), z: reach (1: the farthest reads land 0.3 of
// the screen width away), w: unused.
float4 NVR_BounceSpill = float4(0.0f, 1.0f, 1.0f, 6.0f);
float4 NVR_SpillDebug;  // x: [Shaders.BounceLight.Main] NeonSpillDebug (0 off; 1 what counts as a light; 2 the bounce light alone)
// UNOFFICIAL NeonSpill shape. x: weight of the closest reads (NeonSpillNear), y: of the farthest (NeonSpillFar), z: the most a
// light counts, in multiples of the light test (NeonSpillBrightCap; 0: no limit).
float4 NVR_SpillShape = float4(1.0f, 1.0f, 0.0f, 0.0f);
float4 NVR_SpillTarget; // pyramid passes: xy = 1 / size of the level written, zw = 1 / size of the level read
// UNOFFICIAL NeonSpill memory. x: share of last frame's spill kept (0: none: no usable last frame), y: the share of last
// frame's light a spot keeps at least while the view moves (0: no memory), z: 1 while NeonSpillHoldAfterTurn keeps that hold
// on after the view stopped (0: off), w: how bright (linear) a pixel must be to count as a light (the scene's light times
// NeonSpillLightLevel; see the header).
float4 NVR_SpillMemory;
// UNOFFICIAL round 6, set by BounceLight.cpp with the spill on. xy: the pyramid's level 0 size in texels (NeonSpillSeparateDepth's
// BounceSpillGatherSeparate), z: 1 = NeonSpillMaskSpillOnly (the 'lights get no spill' test in GatherPlus on the spill alone,
// instead of on all the bounce in Combine; 0: as before), w: that test's level (linear): 1.5 x the scene's own light, the default
// light test, whatever NeonSpillLightLevel is (so a stricter LightLevel never takes pixels out of it and gives them spill).
float4 NVR_SpillOptions;

sampler2D TESR_RenderedBuffer : register(s0) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
sampler2D TESR_DepthBuffer : register(s1) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
sampler2D TESR_SourceBuffer : register(s2) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
sampler2D TESR_NormalsBuffer : register(s3) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = NONE; MINFILTER = NONE; MIPFILTER = NONE; };
sampler2D TESR_BlueNoiseSampler : register(s4) < string ResourceName = "Effects\bluenoise256.dds"; > = sampler_state { ADDRESSU = WRAP; ADDRESSV = WRAP; MAGFILTER = NONE; MINFILTER = NONE; MIPFILTER = NONE; };
sampler2D NVR_BounceBuffer : register(s5) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
sampler2D NVR_BouncePrep : register(s6) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
// Last frame's low-resolution result (rgb = bounce, a = view depth): its final result in Prepare (extra bounces), its
// gather output in the temporal Gather. Bound by BounceLight.cpp per pass. After the TESR_ samplers.
sampler2D NVR_BounceHistory : register(s7) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
// UNOFFICIAL NeonSpill: the light pyramid's seven levels (rgb = light sent, linear; a = its depth, linear 0-1, 1 = nothing
// there). Bound by BounceLight.cpp: in the pyramid passes only the level read, in the gather all seven. Read bilinearly: point
// reads left the light in blocks of a level's texels. Where a read mixes light with nothing, the depth goes toward the far
// plane and the light toward none, which together still send about the right amount.
sampler2D NVR_Spill0 : register(s8) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
sampler2D NVR_Spill1 : register(s9) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
sampler2D NVR_Spill2 : register(s10) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
sampler2D NVR_Spill3 : register(s11) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
sampler2D NVR_Spill4 : register(s12) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
sampler2D NVR_Spill5 : register(s13) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
sampler2D NVR_Spill6 : register(s14) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
// UNOFFICIAL NeonSpillSteadySeconds: last frame's running average of level 0's light (rgb, a = its depth as level 0's), read by
// SpillDown0 only (BounceLight.cpp binds it for that pass). Declared last, after every other sampler.
sampler2D NVR_SpillSteadyBuffer : register(s15) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
// x: this frame's share of the running average (0: off, the pyramid as before), y: 1 = last frame's average is usable,
// z: 1 = NeonSpillDebug 1 shows the average, w: 1 = the camera has not moved (read at the same texel, no reprojection).
float4 NVR_SpillSteady;

static const float Strength = TESR_BounceLightData.x;
static const float Radius = max(TESR_BounceLightData.y, 1.0f);
static const float DrawDistance = TESR_BounceLightData.z;
static const float BlurRadius = TESR_BounceLightData.w;
static const int SAMPLES = 8;
static const float NearFade = 40.0f;      // view depth (world units) below which nothing bounces: the weapon, the camera's own body
static const float MaxScreenRadius = 0.06f; // largest disc on screen, as a fraction of the screen width

struct VSOUT
{
	float4 vertPos : POSITION;
	float2 UVCoord : TEXCOORD0;
};

struct VSIN
{
	float4 vertPos : POSITION0;
	float2 UVCoord : TEXCOORD0;
};

VSOUT FrameVS(VSIN IN)
{
	VSOUT OUT = (VSOUT)0.0f;
	OUT.vertPos = IN.vertPos;
	OUT.UVCoord = IN.UVCoord;
	return OUT;
}

// Low-resolution passes: the CPU sets a half- or quarter-size viewport over a target of that size; move the texture
// coordinate to the centre of the low-resolution texel.
VSOUT HalfVS(VSIN IN)
{
	VSOUT OUT = FrameVS(IN);
	OUT.UVCoord += 0.5f * (NVR_BounceLayout.zw - TESR_ReciprocalResolution.xy);
	return OUT;
}

#include "Includes/Helpers.hlsl"
#include "Includes/Depth.hlsl"
#include "Includes/Blur.hlsl"

float3 NormalLod(float2 uv)
{
	return normalize(tex2Dlod(TESR_NormalsBuffer, float4(uv, 0.0f, 0.0f)).xyz * 2 - 1);
}

// View-space position from a screen position and CombineDepth's linear depth (view z / far z).
float3 PositionFromLinear(float2 uv, float linear01)
{
	uv = rectToScreen(uv);	// UNOFFICIAL 2026-10-27 RenderScale v2: callers pass texture coordinates, the ray needs the screen position
	float z = projectedDepthFromLinear(linear01);
	float4 viewSpace = mul(float4(uv.x * 2 - 1, (1 - uv.y) * 2 - 1, z, 1.0f), TESR_InvProjectionTransform);
	return viewSpace.xyz / viewSpace.w;
}

// Two values in [0, 1] as 12-bit integers in one 32-bit float (exact: the result stays below 2^24).
float Pack2(float2 v)
{
	v = floor(saturate(v) * 4095.0f + 0.5f);
	return v.x * 4096.0f + v.y;
}

float2 Unpack2(float p)
{
	float high = floor(p / 4096.0f);
	return float2(high, p - high * 4096.0f) / 4095.0f;
}

// A unit vector folded onto an octahedron and unfolded into a square: two numbers in [0, 1].
float2 OctEncode(float3 n)
{
	n /= abs(n.x) + abs(n.y) + abs(n.z);
	float2 s = n.xy >= 0.0f ? 1.0f : -1.0f;
	float2 e = n.z >= 0.0f ? n.xy : (1.0f - abs(n.yx)) * s;
	return e * 0.5f + 0.5f;
}

float3 OctDecode(float2 e)
{
	e = e * 2.0f - 1.0f;
	float3 n = float3(e, 1.0f - abs(e.x) - abs(e.y));
	float t = saturate(-n.z);
	n.xy += n.xy >= 0.0f ? -t : t;
	return normalize(n);
}

// Pass 1 (Prepare): everything the gather needs about one low-resolution pixel, packed into a single texel:
// x = linear depth, y = normal, z = colour red and green (square-root encoded), w = colour blue. The gather then
// reads one small, compact buffer per sample instead of three full-resolution ones (depth, normals, scene), which
// is what made it slow: its samples land far apart, so nearly every read was a texture cache miss.
float4 Prepare(VSOUT IN) : COLOR0
{
	float2 uv = IN.UVCoord;
	float linear01 = tex2Dlod(TESR_DepthBuffer, float4(uv, 0.0f, 0.0f)).x;
	float3 normal = NormalLod(uv);
	// Capped so small bright glints (specular highlights, lamps) do not spray light around.
	float3 light = min(linearize(tex2Dlod(TESR_SourceBuffer, float4(uv, 0.0f, 0.0f)).rgb), 1.0f);
	return float4(linear01, Pack2(OctEncode(normal)), Pack2(sqrt(light.rg)), light.b);
}

float4 Gather(VSOUT IN) : COLOR0
{
	float2 uv = IN.UVCoord;
	float4 center = tex2Dlod(NVR_BouncePrep, float4(uv, 0.0f, 0.0f));
	float3 origin = PositionFromLinear(uv, center.x);
	float fade = smoothstep(NearFade, NearFade * 2.0f, origin.z) * (1.0f - smoothstep(DrawDistance * 0.7f, DrawDistance, origin.z));
	[branch] if (fade <= 0.0f) return float4(0.0f, 0.0f, 0.0f, origin.z);

	float3 normal = OctDecode(Unpack2(center.y));
	float3 noise = tex2Dlod(TESR_BlueNoiseSampler, float4(uv / (256.0f * NVR_BounceLayout.zw), 0.0f, 0.0f)).xyz;
	float3 up = abs(normal.z) < 0.99f ? float3(0.0f, 0.0f, 1.0f) : float3(1.0f, 0.0f, 0.0f);
	float3 tangent = normalize(cross(up, normal));
	float3 bitangent = cross(normal, tangent);
	// A disc of r world units at depth z spans r * P11 / (2 z) of the screen width.
	float radius = min(Radius, MaxScreenRadius * 2.0f * origin.z / TESR_ProjectionTransform[0][0]);

	float3 indirect = 0.0f;
	[unroll]
	for (int i = 0; i < SAMPLES; i++) {
		float angle = (i + noise.x) * (6.2831853f / SAMPLES);
		float r = radius * (0.15f + 0.85f * frac(noise.y + i * 0.618034f));
		float3 p = origin + (tangent * cos(angle) + bitangent * sin(angle)) * r + normal * (r * 0.25f);
		float2 sampleUV = projectPosition(p).xy;
		float onScreen = step(0.0f, sampleUV.x) * step(sampleUV.x, 1.0f) * step(0.0f, sampleUV.y) * step(sampleUV.y, 1.0f);
		// Snapped to the centre of the low-resolution pixel whose depth, normal and colour are read.
		sampleUV = (floor(saturate(sampleUV) / NVR_BounceLayout.zw) + 0.5f) * NVR_BounceLayout.zw;

		float4 texel = tex2Dlod(NVR_BouncePrep, float4(sampleUV, 0.0f, 0.0f));
		float3 s = PositionFromLinear(sampleUV, texel.x);
		float3 toSample = s - origin;
		float dist = length(toSample) + 0.001f;
		float3 dir = toSample / dist;
		// The surface found must lie near the point aimed at. Something far in front of it (a pole, the weapon)
		// is another object that does not face this pixel, and would otherwise leave a coloured halo around it.
		float nearAim = saturate(1.0f - length(s - p) / radius);
		float weight = saturate(dot(normal, dir)) * saturate(dot(OctDecode(Unpack2(texel.y)), -dir)) * nearAim * onScreen * step(NearFade, s.z);
		float2 rg = Unpack2(texel.z);
		indirect += float3(rg * rg, texel.w) * weight;
	}
	return float4(indirect * (2.0f / SAMPLES) * fade, origin.z);
}

// hard: NeonSpillHardEdges (the BounceLight*HardEdges techniques; false compiles to the code from before it).
float4 Blur(VSOUT IN, uniform float2 axis, uniform bool hard) : COLOR0
{
	float4 center = tex2Dlod(NVR_BounceBuffer, float4(IN.UVCoord, 0.0f, 0.0f));
	float tolerance = max(center.a * 0.03f, 2.0f);
	// UNOFFICIAL NeonSpill (only with it on): on a surface seen edge on (the street, a wall along it) the depth changes a
	// lot from one pixel to the next, so the same-surface test let nothing through there and the light stayed grainy, in
	// steps. The tolerance now also grows with each tap's distance by how fast the depth changes here: the smaller of the
	// changes to the two neighbours, so the jump at an object's edge does not count.
	float slope = 0.0f;
	[branch] if (NVR_BounceSpill.x > 0.0f) {
		float2 texel = axis * NVR_BounceLayout.zw;
		float before = tex2Dlod(NVR_BounceBuffer, float4(IN.UVCoord - texel, 0.0f, 0.0f)).a;
		float after = tex2Dlod(NVR_BounceBuffer, float4(IN.UVCoord + texel, 0.0f, 0.0f)).a;
		// NeonSpillHardEdges: a ridge or dip (a thin thing in front of, or a gap behind, both neighbours) is not a slope.
		if (hard) slope = (center.a - before) * (after - center.a) > 0.0f ? 1.5f * BlurRadius * min(abs(center.a - before), abs(after - center.a)) : 0.0f;
		else slope = 1.5f * BlurRadius * min(abs(center.a - before), abs(after - center.a));
	}
	float3 sum = center.rgb * 0.114725602f;
	float weights = 0.114725602f;
	[unroll]
	for (int i = 0; i < cKernelSize; i++) {
		// BlurOffsets are in full-resolution pixels; this buffer is half or quarter resolution.
		float2 uv = IN.UVCoord + BlurOffsets[i] * axis * BlurRadius * (NVR_BounceLayout.zw / TESR_ReciprocalResolution.xy);
		uv = clampToRectIfOn(uv, NVR_BounceLayout.zw);	// UNOFFICIAL 2026-10-27 RenderScale v2: no stale texels past the drawn part
		float4 tap = tex2Dlod(NVR_BounceBuffer, float4(uv, 0.0f, 0.0f));
		float steps = abs(dot(BlurOffsets[i] / TESR_ReciprocalResolution.xy, axis)); // taps out, 1-6
		float weight = BlurWeights[i] * (abs(tap.a - center.a) <= tolerance + slope * steps);
		sum += tap.rgb * weight;
		weights += weight;
	}
	return float4(sum / weights, center.a);
}

// UNOFFICIAL NeonSpill: how much of a colour (linear) counts as a light: none below the threshold, all of it from twice the
// threshold up. Brightness is the luminance for greys and the brightest channel for strong colours, so a pure blue or red
// neon tube counts as bright as it looks, not as dim as its luminance.
float SpillBrightness(float3 c)
{
	float peak = max(c.r, max(c.g, c.b));
	float saturation = 1.0f - min(c.r, min(c.g, c.b)) / max(peak, 1.0e-4f);
	return lerp(dot(c, float3(0.2126f, 0.7152f, 0.0722f)), peak, saturation);
}
float SpillAmount(float3 c, float threshold)
{
	return saturate((SpillBrightness(c) - threshold) / threshold);
}

// UNOFFICIAL NeonSpillHardEdges (spill on only; Combine's upsample without it is round 5's, unchanged): the four low-resolution
// texels round the pixel, bilinearly, each ONLY if its depth is the pixel's own surface; if none is, no light is added (round 5's
// soft weights were normalised, so a thin frond whose four texels all landed on the wall behind it took the wall's full light, in
// 4 x 4 blocks). The tolerance widens with the depth's slope as round 5's, but only on a surface whose depth keeps going one way
// (a ridge or dip, a thin thing in front of or a gap behind both neighbours, is not a slope).
float3 UpsampleHardEdges(float2 uv, float depth, float2 base, float2 fraction)
{
	float tolerance = max(depth * 0.03f, 2.0f);
	float2 px = TESR_ReciprocalResolution.xy;
	float left = readDepthLod(uv - float2(px.x, 0.0f)), right = readDepthLod(uv + float2(px.x, 0.0f));
	float up = readDepthLod(uv - float2(0.0f, px.y)), down = readDepthLod(uv + float2(0.0f, px.y));
	float slopeX = (depth - left) * (right - depth) > 0.0f ? min(abs(depth - left), abs(right - depth)) : 0.0f;
	float slopeY = (depth - up) * (down - depth) > 0.0f ? min(abs(depth - up), abs(down - depth)) : 0.0f;
	tolerance += 1.5f * (slopeX + slopeY) / NVR_BounceLayout.x;
	float3 sum = 0.0f;
	float weights = 1.0e-5f;
	[unroll] for (int y = 0; y < 2; y++) {
		[unroll] for (int x = 0; x < 2; x++) {
			float4 tap = tex2Dlod(NVR_BounceBuffer, float4((base + float2(x, y) + 0.5f) * NVR_BounceLayout.zw, 0.0f, 0.0f));
			float weight = (x ? fraction.x : 1 - fraction.x) * (y ? fraction.y : 1 - fraction.y) * (abs(tap.a - depth) <= tolerance);
			sum += tap.rgb * weight;
			weights += weight;
		}
	}
	return sum / weights; // no tap on this surface: 0 / 1e-5 = no light
}

// hard: NeonSpillHardEdges (the BounceLight*HardEdges techniques; false compiles to the code from before it).
float4 Combine(VSOUT IN, uniform bool hard) : COLOR0
{
	float4 color = tex2D(TESR_SourceBuffer, IN.UVCoord);
	float depth = readDepth(IN.UVCoord);
	[branch] if (depth < NearFade || depth > DrawDistance) return color;

	// Depth-aware bilinear upsample: the four nearest half-resolution texels, the ones at another depth
	// (across an object's edge) counted less, so the bounce does not bleed across silhouettes.
	float2 position = IN.UVCoord / NVR_BounceLayout.zw - 0.5f;
	float2 base = floor(position);
	float2 fraction = position - base;
	float3 indirect;
	if (hard) indirect = UpsampleHardEdges(IN.UVCoord, depth, base, fraction);
	else {
	float tolerance = max(depth * 0.03f, 2.0f);
	// UNOFFICIAL NeonSpill (only with it on): as in Blur, more tolerance where the surface is seen edge on, by how fast the
	// depth changes across one low-resolution texel (the smaller change to either neighbour, each way).
	[branch] if (NVR_BounceSpill.x > 0.0f) {
		float2 px = TESR_ReciprocalResolution.xy;
		float left = readDepthLod(IN.UVCoord - float2(px.x, 0.0f)), right = readDepthLod(IN.UVCoord + float2(px.x, 0.0f));
		float up = readDepthLod(IN.UVCoord - float2(0.0f, px.y)), down = readDepthLod(IN.UVCoord + float2(0.0f, px.y));
		float slope = min(abs(depth - left), abs(right - depth)) + min(abs(depth - up), abs(down - depth));
		tolerance += 1.5f * slope / NVR_BounceLayout.x;
	}
	float3 sum = 0.0f;
	float weights = 1.0e-5f;
	[unroll] for (int y = 0; y < 2; y++) {
		[unroll] for (int x = 0; x < 2; x++) {
			float4 tap = tex2Dlod(NVR_BounceBuffer, float4((base + float2(x, y) + 0.5f) * NVR_BounceLayout.zw, 0.0f, 0.0f));
			float weight = (x ? fraction.x : 1 - fraction.x) * (y ? fraction.y : 1 - fraction.y);
			weight /= 1.0f + abs(tap.a - depth) / tolerance;
			sum += tap.rgb * weight;
			weights += weight;
		}
	}
	indirect = sum / weights;
	}

	float3 lin = linearize(color.rgb);
	float peak = max(max(lin.r, lin.g), max(lin.b, 0.0001f));
	float3 albedo = lin / peak * 0.6f;
	// UNOFFICIAL NeonSpill debug views: 1 = what counts as a light (the pyramid's first level) over the image darkened;
	// 2 = the light this effect adds, alone, on black.
	[branch] if (NVR_SpillDebug.x > 0.5f) {
		float3 shown = NVR_SpillDebug.x < 1.5f ? lin * 0.12f + tex2Dlod(NVR_Spill0, float4(IN.UVCoord, 0.0f, 0.0f)).rgb
			: Strength * indirect * albedo;
		return delinearize(float4(shown, color.a));
	}
	// UNOFFICIAL NeonSpill: a pixel that is itself a light (a sign's tubes) takes no bounce: its own light washed it out.
	// (NeonSpillMaskSpillOnly: done in GatherPlus on the spill alone instead, so lit surfaces keep their short bounce.)
	[branch] if (NVR_BounceSpill.x > 0.0f && NVR_SpillOptions.z <= 0.0f) indirect *= 1.0f - SpillAmount(lin, max(NVR_SpillMemory.w, 0.002f));
	lin += Strength * indirect * albedo;
	return delinearize(float4(lin, color.a));
}

// ---- UNOFFICIAL: off-screen light, extra bounces, last frame's bounce reused (BounceLightPlus, BounceLightTemporal[Half]) ----

// Where a point (this frame's view space) was last frame: xy = screen texture coordinate, z = its view depth then.
float3 LastFrame(float3 position)
{
	float4 clip = mul(float4(position, 1.0f), NVR_BounceReproject);
	float w = max(clip.w, 0.001f);
	return float3(clip.x / w * 0.5f + 0.5f, 0.5f - clip.y / w * 0.5f, clip.w);
}

// How far the depths of the low-resolution pixels round a point may stray from its own and still be the same surface:
// a few percent, and more where the surface is seen edge on (there, neighbouring pixels lie far apart in depth).
float DepthTolerance(float3 position, float3 normal)
{
	float facing = max(abs(dot(normal, normalize(position))), 0.05f);
	float texel = 2.0f * NVR_BounceLayout.z / TESR_ProjectionTransform[0][0]; // one low-resolution pixel's width at depth 1
	return position.z * (0.03f + 2.0f * texel * sqrt(1.0f - facing * facing) / facing) + 4.0f;
}

// UNOFFICIAL 2026-10-12: DepthTolerance for the neon spill's memory (SpillGather): edge on counts as at most about 63 degrees
// (two texels' depth change per texel, not twenty), and a tighter base, so held light cannot cross onto a neighbouring surface.
float SpillMemoryTolerance(float3 position, float3 normal)
{
	float facing = max(abs(dot(normal, normalize(position))), 0.45f);
	float texel = 2.0f * NVR_BounceLayout.z / TESR_ProjectionTransform[0][0];
	return position.z * (0.02f + 2.0f * texel * sqrt(1.0f - facing * facing) / facing) + 2.0f;
}

// Last frame's result at a point (from LastFrame) in NVR_BounceHistory: the four low-resolution pixels round it, bilinearly,
// each one only if its depth matches. Returns how much of it matched (0: nothing usable, off screen or another surface).
float HistoryAt(float3 last, float tolerance, out float3 history)
{
	float2 position = screenToRect(last.xy) / NVR_BounceLayout.zw - 0.5f;	// UNOFFICIAL 2026-10-27 RenderScale v2: last.xy is a screen position (the edge test below stays on it)
	float2 base = floor(position);
	float2 f = position - base;
	float4 t00 = tex2Dlod(NVR_BounceHistory, float4((base + 0.5f) * NVR_BounceLayout.zw, 0.0f, 0.0f));
	float4 t10 = tex2Dlod(NVR_BounceHistory, float4((base + float2(1.5f, 0.5f)) * NVR_BounceLayout.zw, 0.0f, 0.0f));
	float4 t01 = tex2Dlod(NVR_BounceHistory, float4((base + float2(0.5f, 1.5f)) * NVR_BounceLayout.zw, 0.0f, 0.0f));
	float4 t11 = tex2Dlod(NVR_BounceHistory, float4((base + 1.5f) * NVR_BounceLayout.zw, 0.0f, 0.0f));
	float4 w = float4((1.0f - f.x) * (1.0f - f.y), f.x * (1.0f - f.y), (1.0f - f.x) * f.y, f.x * f.y);
	w *= abs(float4(t00.a, t10.a, t01.a, t11.a) - last.z) < tolerance;
	float sum = dot(w, 1.0f);
	history = (t00.rgb * w.x + t10.rgb * w.y + t01.rgb * w.z + t11.rgb * w.w) / max(sum, 1.0e-4f);
	float inside = all(last.xy > 0.0f) && all(last.xy < 1.0f) && last.z > NearFade;
	return inside ? sum : 0.0f;
}

// ---- UNOFFICIAL NeonSpill: the light pyramid (BounceSpillPyramid) and its gather (NeonSpill, in GatherPlus) ----

static const float SpillNothing = 1.0f;  // pyramid depth where nothing sends light or stands in the way (sky, the weapon)
static const float SpillBaseWidth = 320.0f; // level 0's width in texels (BounceLight.cpp makes it so at any resolution)
static const int SpillDirections = 8;
static const float SpillAreaShare = 0.589f; // 1.5 pi / 8: the share of the ring between d / sqrt2 and d * sqrt2 one read stands for
static const float SpillPlaneSpread = 0.5f; // NeonSpillSeparateDepth: +-1/4 texel (in units of half a texel; preview: 0.5 kept the ground's light, 0.75-1 cut it)
static const float2 SpillQuad[4] = { float2(-1.0f, -1.0f), float2(1.0f, -1.0f), float2(-1.0f, 1.0f), float2(1.0f, 1.0f) };

// The part of a pixel's light (linear) that counts as coming from a light: none below the threshold, rising to all of it
// at twice the threshold. Brightness is the luminance for greys and the brightest channel for strong colours, so a pure
// blue or red neon tube counts as bright as it looks, not as dim as its luminance.
float3 SpillLight(float3 c, float threshold)
{
	float brightness = SpillBrightness(c);
	float amount = saturate((brightness - threshold) / threshold);
	// NeonSpillBrightCap: no brighter than that many times the threshold (the colour kept).
	[flatten] if (NVR_SpillShape.z > 0.0f) amount *= min(1.0f, NVR_SpillShape.z * threshold / max(brightness, 1.0e-4f));
	return c * amount;
}

// Pyramid passes: the texture coordinate to the centre of the texel written (as HalfVS does for the bounce buffers).
VSOUT SpillVS(VSIN IN)
{
	VSOUT OUT = FrameVS(IN);
	OUT.UVCoord += 0.5f * (NVR_SpillTarget.xy - TESR_ReciprocalResolution.xy);
	return OUT;
}

// Level 0: each texel stands for a block of the image (4 x 4 pixels at 1280 wide, 8 x 8 at 2560), read as 4 x 4 bilinear
// reads spread over it (each the average of up to 2 x 2 pixels) and the depth of one pixel at each. rgb = their light's
// average (SpillLight), a = their depth weighted by their light (where the light comes from); with no light there, the
// nearest depth (it may still stand in the way).
struct SpillOut0
{
	float4 color : COLOR0;
	float4 steady : COLOR1;	// NeonSpillSteadySeconds: this frame's running average (written only while that target is bound)
};

SpillOut0 SpillDown0(VSOUT IN)
{
	float threshold = max(NVR_SpillMemory.w, 0.002f); // (a floor: a pitch-black cell does not make every faint pixel a light)
	float3 light = 0.0f;
	float weightedDepth = 0.0f, weights = 0.0f, nearest = SpillNothing;
	[unroll] for (int y = 0; y < 4; y++) {
		[unroll] for (int x = 0; x < 4; x++) {
			float2 uv = IN.UVCoord + (float2(x, y) - 1.5f) * 0.25f * NVR_SpillTarget.xy;
			float linear01 = tex2Dlod(TESR_DepthBuffer, float4(uv, 0.0f, 0.0f)).x;
			float z = linear01 * farZ;
			bool counts = z > NearFade && z < DrawDistance * 1.5f; // not the weapon, the sky or the far distance
			float3 tap = counts ? SpillLight(linearize(tex2Dlod(TESR_SourceBuffer, float4(uv, 0.0f, 0.0f)).rgb), threshold) : 0.0f;
			float weight = dot(tap, 1.0f);
			light += tap;
			weightedDepth += linear01 * weight;
			weights += weight;
			nearest = counts ? min(nearest, linear01) : nearest;
		}
	}
	float4 result = float4(light * (1.0f / 16.0f), weights > 0.0f ? weightedDepth / weights : nearest);
	SpillOut0 OUT;
	OUT.color = result;
	OUT.steady = result;
	// UNOFFICIAL NeonSpillSteadySeconds (2026-10-12: muzzle flashes spilled light through walls; a flash writes no depth, so its
	// light sat on the wall behind it, and the memory held it): the light's running average over that time, followed with the
	// view (last frame's, where this texel's light was then: the 2 x 2 texels round it, bilinearly, each only at about the same
	// depth; read at their centres, so a still view reads back exactly what it wrote and a thin tube's average is not blurred
	// away frame after frame).
	// A light counts only up to 1.5 x its average: a sign that stays lit as before, a flash of a few frames hardly, a sign that
	// comes on fades in over that time.
	[branch] if (NVR_SpillSteady.x > 0.0f) {
		float3 average = result.rgb; // no usable last average (a load, a cell change): the light counts at once
		[branch] if (NVR_SpillSteady.y > 0.0f) {
			float3 last = 0.0f;
			[branch] if (result.a < 0.999f * SpillNothing) {
				float3 where = NVR_SpillSteady.w > 0.5f ? float3(IN.UVCoord, result.a * farZ) : LastFrame(PositionFromLinear(IN.UVCoord, result.a));
				[branch] if (all(where.xy > 0.0f) && all(where.xy < 1.0f)) {
					float2 position = where.xy / NVR_SpillTarget.xy - 0.5f;
					float2 base = floor(position);
					float2 f = position - base;
					float3 sum = 0.0f;
					float weights = 0.0f;
					[unroll] for (int k = 0; k < 4; k++) {
						float2 corner = float2(k == 1 || k == 3, k >= 2);
						float4 t = tex2Dlod(NVR_SpillSteadyBuffer, float4((base + 0.5f + corner) * NVR_SpillTarget.xy, 0.0f, 0.0f));
						float2 w2 = corner > 0.5f ? f : 1.0f - f;
						float w = w2.x * w2.y * (abs(t.a * farZ - where.z) < where.z * 0.1f + 20.0f);
						sum += t.rgb * w;
						weights += w;
					}
					last = weights > 1.0e-4f ? sum / weights : 0.0f;
				}
			}
			average = lerp(last, result.rgb, NVR_SpillSteady.x);
		}
		OUT.steady = float4(average, result.a);
		OUT.color.rgb = NVR_SpillSteady.z > 0.5f ? average : min(result.rgb, average * 1.5f); // z: NeonSpillDebug 1 shows the average
	}
	return OUT;
}

// Levels 1-6: the 2 x 2 texels of the level before, the same way.
float4 SpillDown(VSOUT IN, uniform sampler2D level) : COLOR0
{
	float3 light = 0.0f;
	float weightedDepth = 0.0f, weights = 0.0f, nearest = SpillNothing;
	[unroll] for (int i = 0; i < 4; i++) {
		float4 tap = tex2Dlod(level, float4(IN.UVCoord + SpillQuad[i] * 0.5f * NVR_SpillTarget.zw, 0.0f, 0.0f));
		float weight = dot(tap.rgb, 1.0f);
		light += tap.rgb;
		weightedDepth += tap.a * weight;
		weights += weight;
		nearest = min(nearest, tap.a);
	}
	return float4(light * 0.25f, weights > 0.0f ? weightedDepth / weights : nearest);
}

// One read of the pyramid: `distance` full-resolution pixels from the pixel along `direction` (one pixel along it, in
// texture coordinates). pixelWorld: the world size of one pixel at view depth 1. `horizon`: the highest rise seen so far
// along this direction (the sine of its angle over the pixel's surface); a read below it is hidden.
void SpillStep(uniform sampler2D level, uniform float share, float distance, float2 uv, float2 direction, float3 origin,
	float3 normal, float pixelWorld, inout float horizon, inout float3 light)
{
	float2 sampleUV = uv + direction * distance;
	[branch] if (any(sampleUV != saturate(sampleUV))) return;
	float4 tap = tex2Dlod(level, float4(sampleUV, 0.0f, 0.0f));
	[branch] if (tap.a >= 0.999f * SpillNothing) return;
	float z = tap.a * farZ;
	float3 s = float3((sampleUV.x * 2.0f - 1.0f) / TESR_ProjectionTransform[0][0], (1.0f - sampleUV.y * 2.0f) / TESR_ProjectionTransform[1][1], 1.0f) * z;
	float3 toSample = s - origin;
	float dist2 = dot(toSample, toSample) + 1.0f;
	float rise = dot(normal, toSample) * rsqrt(dist2);
	float visible = saturate(1.0f + (rise - horizon) * 6.0f);
	// Only something near the pixel stands in the way: a thin thing far in front of it (a pole, a fence) does not hide
	// the light behind it along that direction.
	float ring = distance * pixelWorld * origin.z;
	horizon = dist2 < 9.0f * ring * ring ? max(horizon, rise) : horizon;
	// The light the area this read stands for sends to the pixel (a disc of that area seen from the pixel, so it stays
	// finite close up): light x area x cos / (pi d^2 + area).
	float area = SpillAreaShare * distance * distance * pixelWorld * pixelWorld * z * z;
	// share: where this read lies between the closest (0) and the farthest (1): NeonSpillNear to NeonSpillFar.
	light += tap.rgb * (saturate(rise) * visible * area / (3.14159265f * dist2 + area) * lerp(NVR_SpillShape.x, NVR_SpillShape.y, share));
}

// UNOFFICIAL NeonSpillSeparateDepth (BounceSpillGatherSeparate): SpillStep with the light's depth kept apart from what stands in
// the way. A bilinear read mixes a lit texel's depth (its lights' own, light-weighted) with its empty neighbours' depth (the
// NEAREST thing in their block, kept for the horizon test), so next to the floor, a wall at right angles or any slanted surface the
// mixed depth sits in front of the surface the light is on, and a sign lit its own flat wall. Here the four texels round the read
// are read one by one: the light is the bilinear mix as before; the light's depth and screen position come from the lit texels
// only (weighted by their light); the plain bilinear depth still raises the horizon. scale: this level's size / level 0's.
void SpillStepSeparate(uniform sampler2D level, uniform float share, uniform float scale, float distance, float2 uv, float2 direction,
	float3 origin, float3 normal, float pixelWorld, inout float horizon, inout float3 light)
{
	float2 sampleUV = uv + direction * distance;
	[branch] if (any(sampleUV != saturate(sampleUV))) return;
	float2 size = ceil(NVR_SpillOptions.xy * scale);          // each level is the one before halved, rounded up (BounceLight.cpp)
	float2 position = sampleUV * size - 0.5f;
	float2 base = floor(position);
	float2 f = position - base;
	// The four texels one by one: the bilinear mix of light and of depth (as SpillStep's read), and the light-weighted depth
	// and screen position of the lit ones.
	float3 tapLight = 0.0f;
	float depthMixed = 0.0f, litSum = 0.0f, litDepth = 0.0f;
	float2 litUV = 0.0f;
	[unroll] for (int k = 0; k < 4; k++) {
		float2 corner = float2(k == 1 || k == 3, k >= 2);
		float2 texelUV = (base + 0.5f + corner) / size;
		float4 t = tex2Dlod(level, float4(texelUV, 0.0f, 0.0f));
		float2 w2 = corner > 0.5f ? f : 1.0f - f;
		float w = w2.x * w2.y;
		float l = w * dot(t.rgb, 1.0f);
		tapLight += t.rgb * w;
		depthMixed += t.a * w;
		litSum += l;
		litDepth += t.a * l;
		litUV += texelUV * l;
	}
	[branch] if (depthMixed >= 0.999f * SpillNothing) return;
	// What stands in the way along this direction: the mixed depth at the read, as SpillStep has it.
	float3 o = float3((sampleUV.x * 2.0f - 1.0f) / TESR_ProjectionTransform[0][0], (1.0f - sampleUV.y * 2.0f) / TESR_ProjectionTransform[1][1], 1.0f) * (depthMixed * farZ) - origin;
	float oDist2 = dot(o, o) + 1.0f;
	float oRise = dot(normal, o) * rsqrt(oDist2);
	float ring = distance * pixelWorld * origin.z;
	float newHorizon = oDist2 < 9.0f * ring * ring ? max(horizon, oRise) : horizon;
	[branch] if (litSum <= 0.0f) { horizon = newHorizon; return; }
	// The light: at its own depth, where on screen its texels are.
	float z = litDepth / litSum * farZ;
	litUV /= litSum;
	float3 ray = float3((litUV.x * 2.0f - 1.0f) / TESR_ProjectionTransform[0][0], (1.0f - litUV.y * 2.0f) / TESR_ProjectionTransform[1][1], 1.0f);
	float3 toSample = ray * z - origin;
	float dist2 = dot(toSample, toSample) + 1.0f;
	float rise = dot(normal, toSample) * rsqrt(dist2);
	float visible = saturate(1.0f + (rise - horizon) * 6.0f);
	horizon = newHorizon;
	// Where on screen the light is, is only known to within its texel of this level. If one of the rays through the part of the
	// texel the light may sit in (a quarter of a texel either way round lightUV: SpillPlaneSpread) meets the pixel's own plane at the light's
	// depth, the light may lie ON that plane (a sign on the wall this pixel is on), and its rise is only that uncertainty: it does
	// not count; from twice that far off the plane it counts fully. Worked out on dot(normal, ray), which is linear across the
	// texel (no division by it: a ray running along the plane is fine): on the plane at depth z means dot(normal, ray) = onPlane / z.
	float spread = SpillPlaneSpread * (abs(normal.x) / (size.x * TESR_ProjectionTransform[0][0]) + abs(normal.y) / (size.y * TESR_ProjectionTransform[1][1]));
	float onPlane = dot(normal, origin) / z;
	float offPlane = saturate(abs(dot(normal, ray) - onPlane) / (spread + 0.002f * abs(onPlane) + 1.0e-6f) - 1.0f);
	float area = SpillAreaShare * distance * distance * pixelWorld * pixelWorld * z * z;
	light += tapLight * (saturate(rise) * visible * offPlane * area / (3.14159265f * dist2 + area) * lerp(NVR_SpillShape.x, NVR_SpillShape.y, share));
}

// The light reaching a pixel (view-space position and normal) from the pyramid, along SpillDirections directions turned by
// the pixel's noise, seven reads each, the nth on level n, 1.5 of its texels out (times reach and a little jitter).
// separate: NeonSpillSeparateDepth (SpillStepSeparate); false compiles to the code from before it.
float3 NeonSpill(float2 uv, float3 origin, float3 normal, float3 noise, uniform bool separate)
{
	float pixelWorld = 2.0f * TESR_ReciprocalResolution.x / TESR_ProjectionTransform[0][0];
	float first = NVR_BounceSpill.z * 1.5f / (SpillBaseWidth * TESR_ReciprocalResolution.x); // pixels to the first read
	float3 light = 0.0f;
	[loop] for (int i = 0; i < SpillDirections; i++) {
		float angle = (i + noise.x) * (6.2831853f / SpillDirections);
		float2 direction = float2(cos(angle), sin(angle)) * TESR_ReciprocalResolution.xy;
		float step = first * (0.75f + 0.5f * frac(noise.y + 0.5f + i * 0.618034f));
		float horizon = 0.0f;
		if (separate) {
			SpillStepSeparate(NVR_Spill0, 0.0f, 1.0f, step, uv, direction, origin, normal, pixelWorld, horizon, light);
			SpillStepSeparate(NVR_Spill1, 1.0f / 6.0f, 0.5f, step * 2.0f, uv, direction, origin, normal, pixelWorld, horizon, light);
			SpillStepSeparate(NVR_Spill2, 2.0f / 6.0f, 0.25f, step * 4.0f, uv, direction, origin, normal, pixelWorld, horizon, light);
			SpillStepSeparate(NVR_Spill3, 3.0f / 6.0f, 0.125f, step * 8.0f, uv, direction, origin, normal, pixelWorld, horizon, light);
			SpillStepSeparate(NVR_Spill4, 4.0f / 6.0f, 0.0625f, step * 16.0f, uv, direction, origin, normal, pixelWorld, horizon, light);
			SpillStepSeparate(NVR_Spill5, 5.0f / 6.0f, 0.03125f, step * 32.0f, uv, direction, origin, normal, pixelWorld, horizon, light);
			SpillStepSeparate(NVR_Spill6, 1.0f, 0.015625f, step * 64.0f, uv, direction, origin, normal, pixelWorld, horizon, light);
			continue;
		}
		SpillStep(NVR_Spill0, 0.0f, step, uv, direction, origin, normal, pixelWorld, horizon, light);
		SpillStep(NVR_Spill1, 1.0f / 6.0f, step * 2.0f, uv, direction, origin, normal, pixelWorld, horizon, light);
		SpillStep(NVR_Spill2, 2.0f / 6.0f, step * 4.0f, uv, direction, origin, normal, pixelWorld, horizon, light);
		SpillStep(NVR_Spill3, 3.0f / 6.0f, step * 8.0f, uv, direction, origin, normal, pixelWorld, horizon, light);
		SpillStep(NVR_Spill4, 4.0f / 6.0f, step * 16.0f, uv, direction, origin, normal, pixelWorld, horizon, light);
		SpillStep(NVR_Spill5, 5.0f / 6.0f, step * 32.0f, uv, direction, origin, normal, pixelWorld, horizon, light);
		SpillStep(NVR_Spill6, 1.0f, step * 64.0f, uv, direction, origin, normal, pixelWorld, horizon, light);
	}
	return light;
}

// BounceSpillGather (low resolution, before the bounce's own passes): the spill at each pixel with its memory (see the
// header); last frame's in NVR_BounceHistory. rgb = spill (draw distance fade included), a = view depth.
// separate: NeonSpillSeparateDepth (technique BounceSpillGatherSeparate).
float4 SpillGather(VSOUT IN, uniform bool separate) : COLOR0
{
	float2 uv = IN.UVCoord;
	float3 origin = PositionFromLinear(uv, tex2Dlod(TESR_DepthBuffer, float4(uv, 0.0f, 0.0f)).x);
	float fade = smoothstep(NearFade, NearFade * 2.0f, origin.z) * (1.0f - smoothstep(DrawDistance * 0.7f, DrawDistance, origin.z));
	[branch] if (fade <= 0.0f) return float4(0.0f, 0.0f, 0.0f, origin.z);
	float3 normal = NormalLod(uv);
	float3 noise = tex2Dlod(TESR_BlueNoiseSampler, float4(uv / (256.0f * NVR_BounceLayout.zw), 0.0f, 0.0f)).xyz;
	noise.xy = frac(noise.xy + NVR_BounceTemporal.z * float2(0.618034f, 0.754878f)); // a new pattern every frame
	float3 where = LastFrame(origin);
	float3 spill = NeonSpill(uv, origin, normal, noise, separate) * fade;
	[branch] if (NVR_SpillMemory.x > 0.0f) {
		float3 last;
		// UNOFFICIAL 2026-10-12 (the user's neon light 'through the roof, following the camera', Gomorrah canopy and Lucky 38):
		// last frame's spill is taken only from the same surface, by a tolerance that grows far less on surfaces seen edge on
		// than the bounce's DepthTolerance (up to 20 low-resolution texels there: a roof's underside next to a lit neon strip
		// took the strip's held light). And the hold (the max below) only where all four texels round the point are that
		// surface: a partial match is an edge, and the held light there formed blobs on the memory's texel grid that moved
		// with the view.
		float matched = HistoryAt(where, SpillMemoryTolerance(origin, normal), last);
		float found = saturate(2.0f * matched);
		float whole = saturate((matched - 0.95f) * 20.0f);
		// While the view moves (fully from 2 low-resolution pixels a frame), no faster fall than the memory allows. UNOFFICIAL
		// NeonSpillHoldAfterTurn: NVR_SpillMemory.z is 1 for that many seconds after the view stops (BounceLight.cpp's timer), so
		// the light of a sign turned off screen keeps fading at the memory's pace instead of draining in a few frames (0: off).
		float moving = max(saturate(0.5f * length((where.xy - uv) / NVR_BounceLayout.zw)), NVR_SpillMemory.z);
		spill = max(lerp(spill, last, NVR_SpillMemory.x * found), last * (NVR_SpillMemory.y * moving * whole));
	}
	return float4(spill, origin.z);
}

// Prepare, plus the extra bounces: the bounce light this surface showed last frame (NVR_BounceHistory: last frame's
// final result, rgb as Combine adds it) is light it sends on as well.
float4 PreparePlus(VSOUT IN) : COLOR0
{
	float2 uv = IN.UVCoord;
	float linear01 = tex2Dlod(TESR_DepthBuffer, float4(uv, 0.0f, 0.0f)).x;
	float3 normal = NormalLod(uv);
	float3 scene = linearize(tex2Dlod(TESR_SourceBuffer, float4(uv, 0.0f, 0.0f)).rgb);
	float3 light = min(scene, 1.0f);
	[branch] if (NVR_BounceTemporal.y > 0.0f) {
		float3 position = PositionFromLinear(uv, linear01);
		float3 last;
		float found = HistoryAt(LastFrame(position), DepthTolerance(position, normal), last);
		[branch] if (found > 0.0f) {
			// As Combine adds it: Strength * bounce * the surface's hue.
			float peak = max(max(scene.r, scene.g), max(scene.b, 0.0001f));
			light = min(light + (NVR_BounceTemporal.y * saturate(found * 2.0f) * Strength * 0.6f / peak) * last * scene, 1.0f);
		}
	}
	return float4(linear01, Pack2(OctEncode(normal)), Pack2(sqrt(light.rg)), light.b);
}

// Gather, plus the off-screen light; with temporal, a sample pattern that turns from frame to frame, blended into last
// frame's result.
float4 GatherPlus(VSOUT IN, uniform int samples, uniform bool temporal, uniform float motionPixels) : COLOR0
{
	float2 uv = IN.UVCoord;
	float4 center = tex2Dlod(NVR_BouncePrep, float4(uv, 0.0f, 0.0f));
	float3 origin = PositionFromLinear(uv, center.x);
	float fade = smoothstep(NearFade, NearFade * 2.0f, origin.z) * (1.0f - smoothstep(DrawDistance * 0.7f, DrawDistance, origin.z));
	[branch] if (fade <= 0.0f) return float4(0.0f, 0.0f, 0.0f, origin.z);

	float3 normal = OctDecode(Unpack2(center.y));
	float3 noise = tex2Dlod(TESR_BlueNoiseSampler, float4(uv / (256.0f * NVR_BounceLayout.zw), 0.0f, 0.0f)).xyz;
	// A new pattern every frame (golden-ratio steps keep each frame's pattern blue noise), so the frames average out.
	if (temporal) noise.xy = frac(noise.xy + NVR_BounceTemporal.z * float2(0.618034f, 0.754878f));
	float3 up = abs(normal.z) < 0.99f ? float3(0.0f, 0.0f, 1.0f) : float3(1.0f, 0.0f, 0.0f);
	float3 tangent = normalize(cross(up, normal));
	float3 bitangent = cross(normal, tangent);
	float radius = min(Radius, MaxScreenRadius * 2.0f * origin.z / TESR_ProjectionTransform[0][0]);

	float3 indirect = 0.0f;
	float seenWeight = 0.0f, seen = 0.0f; // the seen samples' weights added up, and how many were seen
	[unroll]
	for (int i = 0; i < samples; i++) {
		float angle = (i + noise.x) * (6.2831853f / samples);
		float r = radius * (0.15f + 0.85f * frac(noise.y + i * 0.618034f));
		float3 p = origin + (tangent * cos(angle) + bitangent * sin(angle)) * r + normal * (r * 0.25f);
		float2 sampleUV = projectPosition(p).xy;
		// UNOFFICIAL 2026-10-27 RenderScale v2: on screen = inside the drawn part (1 off: as before)
		float onScreen = step(0.0f, sampleUV.x) * step(sampleUV.x, TESR_RenderScale.x) * step(0.0f, sampleUV.y) * step(sampleUV.y, TESR_RenderScale.y);
		sampleUV = (floor(clamp(sampleUV, 0.0f, TESR_RenderScale.xy) / NVR_BounceLayout.zw) + 0.5f) * NVR_BounceLayout.zw;

		float4 texel = tex2Dlod(NVR_BouncePrep, float4(sampleUV, 0.0f, 0.0f));
		float3 s = PositionFromLinear(sampleUV, texel.x);
		float3 toSample = s - origin;
		float dist = length(toSample) + 0.001f;
		float3 dir = toSample / dist;
		float nearAim = saturate(1.0f - length(s - p) / radius);
		float visible = onScreen * step(NearFade, s.z); // a surface on screen, not the weapon in front of it
		float weight = saturate(dot(normal, dir)) * saturate(dot(OctDecode(Unpack2(texel.y)), -dir)) * nearAim * visible;
		float2 rg = Unpack2(texel.z);
		indirect += float3(rg * rg, texel.w) * weight;
		seenWeight += weight;
		seen += visible;
	}
	// The samples whose surface is unknown (off screen, behind the weapon): weighted as the seen ones on average, with
	// light between the ambient's and the seen ones' average, the more of them seen the nearer theirs.
	float3 seenLight = indirect / max(seenWeight, 1.0e-4f);
	float unknownWeight = (samples - seen) * seenWeight / max(seen, 1.0f);
	indirect += lerp(NVR_BounceAmbient.rgb, seenLight, seen / samples) * (unknownWeight * NVR_BounceAmbient.w);
	float3 result = indirect * (2.0f / samples * NVR_BounceSpill.y) * fade;
	// UNOFFICIAL NeonSpill: the long-range light from bright lights (BounceSpillGather's buffer, bound at NVR_Spill0's stage
	// for this pass, the same size as this one) joins the bounce (NVR_BounceSpill.y = 1 with it off).
	[branch] if (NVR_BounceSpill.x > 0.0f) {
		float3 spill = tex2Dlod(NVR_Spill0, float4(uv, 0.0f, 0.0f)).rgb;
		// UNOFFICIAL NeonSpillMaskSpillOnly: a pixel that is itself a light takes no spill (its colour from the full-resolution
		// image, not Prepare's capped one); its short-range bounce stays, unlike Combine's test on all of it (round 5).
		[branch] if (NVR_SpillOptions.z > 0.0f) spill *= 1.0f - SpillAmount(linearize(tex2Dlod(TESR_SourceBuffer, float4(uv, 0.0f, 0.0f)).rgb), max(NVR_SpillOptions.w, 0.002f));
		result += spill * NVR_BounceSpill.x;
	}
	if (temporal) {
		// Last frame's at the same spot. Less of it is kept the further the spot moved on screen: half as much for one that
		// moved motionPixels low-resolution pixels. What the screen shows round a spot, and so its bounce, changes as the
		// view moves, and a long memory would lag behind.
		float3 last;
		float3 where = LastFrame(origin);
		float found = HistoryAt(where, DepthTolerance(origin, normal), last);
		float moved = length((screenToRect(where.xy) - uv) / NVR_BounceLayout.zw);	// (RenderScale v2)
		result = lerp(result, last, NVR_BounceTemporal.x / (1.0f + moved / motionPixels) * saturate(found * 2.0f));
	}
	return float4(result, origin.z);
}

technique
{
	pass Prepare { VertexShader = compile vs_3_0 HalfVS(); PixelShader = compile ps_3_0 Prepare(); }
	pass Gather { VertexShader = compile vs_3_0 HalfVS(); PixelShader = compile ps_3_0 Gather(); }
	pass BlurX { VertexShader = compile vs_3_0 HalfVS(); PixelShader = compile ps_3_0 Blur(float2(1.0f, 0.0f), false); }
	pass BlurY { VertexShader = compile vs_3_0 HalfVS(); PixelShader = compile ps_3_0 Blur(float2(0.0f, 1.0f), false); }
	pass Combine { VertexShader = compile vs_3_0 FrameVS(); PixelShader = compile ps_3_0 Combine(false); }
}

// UNOFFICIAL: OffScreenLight and/or MultiBounce on, Temporal off.
technique BounceLightPlus
{
	pass Prepare { VertexShader = compile vs_3_0 HalfVS(); PixelShader = compile ps_3_0 PreparePlus(); }
	pass Gather { VertexShader = compile vs_3_0 HalfVS(); PixelShader = compile ps_3_0 GatherPlus(8, false, 1.0f); }
	pass BlurX { VertexShader = compile vs_3_0 HalfVS(); PixelShader = compile ps_3_0 Blur(float2(1.0f, 0.0f), false); }
	pass BlurY { VertexShader = compile vs_3_0 HalfVS(); PixelShader = compile ps_3_0 Blur(float2(0.0f, 1.0f), false); }
	pass Combine { VertexShader = compile vs_3_0 FrameVS(); PixelShader = compile ps_3_0 Combine(false); }
}

// UNOFFICIAL: Temporal on.
technique BounceLightTemporal
{
	pass Prepare { VertexShader = compile vs_3_0 HalfVS(); PixelShader = compile ps_3_0 PreparePlus(); }
	pass Gather { VertexShader = compile vs_3_0 HalfVS(); PixelShader = compile ps_3_0 GatherPlus(8, true, 4.0f); }
	pass BlurX { VertexShader = compile vs_3_0 HalfVS(); PixelShader = compile ps_3_0 Blur(float2(1.0f, 0.0f), false); }
	pass BlurY { VertexShader = compile vs_3_0 HalfVS(); PixelShader = compile ps_3_0 Blur(float2(0.0f, 1.0f), false); }
	pass Combine { VertexShader = compile vs_3_0 FrameVS(); PixelShader = compile ps_3_0 Combine(false); }
}

// UNOFFICIAL: Temporal and HalfSamples on: 4 samples a frame instead of 8.
technique BounceLightTemporalHalf
{
	pass Prepare { VertexShader = compile vs_3_0 HalfVS(); PixelShader = compile ps_3_0 PreparePlus(); }
	pass Gather { VertexShader = compile vs_3_0 HalfVS(); PixelShader = compile ps_3_0 GatherPlus(4, true, 8.0f); }
	pass BlurX { VertexShader = compile vs_3_0 HalfVS(); PixelShader = compile ps_3_0 Blur(float2(1.0f, 0.0f), false); }
	pass BlurY { VertexShader = compile vs_3_0 HalfVS(); PixelShader = compile ps_3_0 Blur(float2(0.0f, 1.0f), false); }
	pass Combine { VertexShader = compile vs_3_0 FrameVS(); PixelShader = compile ps_3_0 Combine(false); }
}

// UNOFFICIAL NeonSpillHardEdges (with the spill on only; BounceLight.cpp picks them): the three techniques above, with Blur's and
// Combine's hard depth test (no wall light on thin things in front of it, in 4 x 4 blocks).
technique BounceLightPlusHardEdges
{
	pass Prepare { VertexShader = compile vs_3_0 HalfVS(); PixelShader = compile ps_3_0 PreparePlus(); }
	pass Gather { VertexShader = compile vs_3_0 HalfVS(); PixelShader = compile ps_3_0 GatherPlus(8, false, 1.0f); }
	pass BlurX { VertexShader = compile vs_3_0 HalfVS(); PixelShader = compile ps_3_0 Blur(float2(1.0f, 0.0f), true); }
	pass BlurY { VertexShader = compile vs_3_0 HalfVS(); PixelShader = compile ps_3_0 Blur(float2(0.0f, 1.0f), true); }
	pass Combine { VertexShader = compile vs_3_0 FrameVS(); PixelShader = compile ps_3_0 Combine(true); }
}

technique BounceLightTemporalHardEdges
{
	pass Prepare { VertexShader = compile vs_3_0 HalfVS(); PixelShader = compile ps_3_0 PreparePlus(); }
	pass Gather { VertexShader = compile vs_3_0 HalfVS(); PixelShader = compile ps_3_0 GatherPlus(8, true, 4.0f); }
	pass BlurX { VertexShader = compile vs_3_0 HalfVS(); PixelShader = compile ps_3_0 Blur(float2(1.0f, 0.0f), true); }
	pass BlurY { VertexShader = compile vs_3_0 HalfVS(); PixelShader = compile ps_3_0 Blur(float2(0.0f, 1.0f), true); }
	pass Combine { VertexShader = compile vs_3_0 FrameVS(); PixelShader = compile ps_3_0 Combine(true); }
}

technique BounceLightTemporalHalfHardEdges
{
	pass Prepare { VertexShader = compile vs_3_0 HalfVS(); PixelShader = compile ps_3_0 PreparePlus(); }
	pass Gather { VertexShader = compile vs_3_0 HalfVS(); PixelShader = compile ps_3_0 GatherPlus(4, true, 8.0f); }
	pass BlurX { VertexShader = compile vs_3_0 HalfVS(); PixelShader = compile ps_3_0 Blur(float2(1.0f, 0.0f), true); }
	pass BlurY { VertexShader = compile vs_3_0 HalfVS(); PixelShader = compile ps_3_0 Blur(float2(0.0f, 1.0f), true); }
	pass Combine { VertexShader = compile vs_3_0 FrameVS(); PixelShader = compile ps_3_0 Combine(true); }
}

// UNOFFICIAL NeonSpill: the light pyramid, seven levels, each pass into its own buffer (BounceLight.cpp binds the level read).
technique BounceSpillPyramid
{
	pass Level0 { VertexShader = compile vs_3_0 SpillVS(); PixelShader = compile ps_3_0 SpillDown0(); }
	pass Level1 { VertexShader = compile vs_3_0 SpillVS(); PixelShader = compile ps_3_0 SpillDown(NVR_Spill0); }
	pass Level2 { VertexShader = compile vs_3_0 SpillVS(); PixelShader = compile ps_3_0 SpillDown(NVR_Spill1); }
	pass Level3 { VertexShader = compile vs_3_0 SpillVS(); PixelShader = compile ps_3_0 SpillDown(NVR_Spill2); }
	pass Level4 { VertexShader = compile vs_3_0 SpillVS(); PixelShader = compile ps_3_0 SpillDown(NVR_Spill3); }
	pass Level5 { VertexShader = compile vs_3_0 SpillVS(); PixelShader = compile ps_3_0 SpillDown(NVR_Spill4); }
	pass Level6 { VertexShader = compile vs_3_0 SpillVS(); PixelShader = compile ps_3_0 SpillDown(NVR_Spill5); }
}

// UNOFFICIAL NeonSpill: the spill with its memory (low resolution); see the header.
technique BounceSpillGather
{
	pass Gather { VertexShader = compile vs_3_0 HalfVS(); PixelShader = compile ps_3_0 SpillGather(false); }
}

// UNOFFICIAL NeonSpillSeparateDepth: the same with the light's depth kept apart from what stands in the way (SpillStepSeparate).
technique BounceSpillGatherSeparate
{
	pass Gather { VertexShader = compile vs_3_0 HalfVS(); PixelShader = compile ps_3_0 SpillGather(true); }
}
