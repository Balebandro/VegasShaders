// Flashlight composite. The spot pool is lit per pixel from depth and normals, using the cookie
// projection and the shadow lookup. A depth-only blur separates the shadow term, and the Combine
// pass adds the light (plus the FlashlightBeam shaft) to the scene.
float4x4 TESR_FlashLightViewProjTransform;
float4 TESR_CameraForward;
float4 TESR_SpotLightPosition;
float4 TESR_SpotLightColor;
float4 TESR_SpotLightDirection;
float4 TESR_SunColor;
float4 TESR_SunDirection;
float4 TESR_DebugVar;
float4 TESR_ReciprocalResolution;
float4 TESR_FlashLightTuning;		// x near fade, y soft edges, z hotspot limit, w cookie strength
float4 TESR_FlashLightComposite;	// x source buffer is already linear; UNOFFICIAL (EdgeFix): y 1 = also the beam's shaft, zw 1 / beam buffer size
float4 TESR_VolumetricControl;		// x beam strength, 0 when the FlashlightBeam effect is off

#define FL_NEARFADE			TESR_FlashLightTuning.x
#define FL_SOFTEDGE			TESR_FlashLightTuning.y
#define FL_HOTSPOTLIMIT		TESR_FlashLightTuning.z
#define FL_COOKIESTRENGTH	TESR_FlashLightTuning.w
#define FL_LINEARSOURCE		TESR_FlashLightComposite.x
#define FL_EDGEBEAM			TESR_FlashLightComposite.y
#define FL_BEAMTEXEL		TESR_FlashLightComposite.zw

sampler2D TESR_SourceBuffer : register(s0) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_RenderedBuffer : register(s1) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_DepthBuffer : register(s2) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = ANISOTROPIC; MIPFILTER = LINEAR; };
sampler2D TESR_PointShadowBuffer : register(s3)  = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_NormalsBuffer : register(s4) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_ShadowSpotlightBuffer0 : register(s5) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_SpotLightTexture : register(s6) < string ResourceName = "Effects\Flashlight.png"; > = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_VolumetricBuffer : register(s7) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };


struct VSOUT
{
	float4 clipPos : POSITION;
	float2 UVCoord : TEXCOORD0;
};

struct VSIN
{
	float4 clipPos : POSITION0;
	float2 UVCoord : TEXCOORD0;
};

VSOUT FrameVS(VSIN src)
{
	VSOUT res = (VSOUT)0.0f;
	res.clipPos = src.clipPos;
	res.UVCoord = src.UVCoord;
	return res;
}

#include "Includes/Depth.hlsl"
#include "Includes/Helpers.hlsl"
#include "Includes/Normals.hlsl"
#include "Includes/BlurDepth.hlsl"

// Preview helper: draws a texture inside a rectangle of the screen (debug overlay, not used by the passes).
float4 previewTex(float4 base, float2 p, float2 origin, float2 extent, sampler2D src){
	float2 corner = origin + extent;
	if ((p.x < origin.x || p.y < origin.y) || (p.x > corner.x || p.y > corner.y )) return base;
	return tex2D(src, float2(invlerp(origin, corner, p)))/TESR_DebugVar.y;
}


// Maps a clip space position onto texture space: perspective divide, then -1..1 becomes 0..1 with y flipped.
float4 clipToTex(float4 c){
	c.xyz /= c.w;
	c.x = c.x * 0.5f + 0.5f;
	c.y = c.y * -0.5f + 0.5f;

	return c;
}


float4 Shadows(VSOUT IN) : COLOR0
{
	float d0 = readDepth(IN.UVCoord);
	float3 eyeV = toWorld(IN.UVCoord);
	float4 wp = float4(TESR_CameraPosition.xyz + eyeV * d0, 1);
	float3 toLight = TESR_SpotLightPosition.xyz - wp.xyz;

	float rad = TESR_SpotLightPosition.w;
	float dist = length(toLight)/rad;

	float4 lsc = clipToTex(mul(wp, TESR_FlashLightViewProjTransform));
	float sDepth = tex2D(TESR_ShadowSpotlightBuffer0, lsc.xy).r;
	float occl = sDepth + 0.05 * dist > dist; // BIAS to reduce shadowing artefacts

	if (dist < 1 && lsc.x > 0.0 && lsc.x < 1.0 && lsc.y > 0.0 && lsc.y < 1.0) return float4(occl.rrr, 1);
	return float4(1.0, 1.0, 1.0, 1.0);
}

float4 NoShadow(VSOUT IN) : COLOR0
{
	return float4(1.0, 1.0, 1.0, 1.0);
}

// Procedural rings and mottling layered over the projected cookie, so a bare wall
// shows some structure in the beam instead of a flat disc. Fades to 1 at strength 0.
float cookieBreakup(float2 uv)
{
	if (FL_COOKIESTRENGTH <= 0.001) return 1.0;

	float2 q = uv - 0.5;
	float rr = saturate(length(q) * 2.0);

	float warpT = sin(uv.x * 17.0 + uv.y * 5.5) * 0.45 + sin(uv.x * -6.5 + uv.y * 19.0) * 0.35;
	float ringT = sin(rr * 36.0 + warpT) * 0.5 + 0.5;
	float mot = sin(uv.x * 41.0 + sin(uv.y * 13.0) * 2.2) * sin(uv.y * 29.0 + sin(uv.x * 11.0) * 1.7);
	mot = mot * 0.5 + 0.5;

	float rim = smoothstep(0.25, 0.95, rr);
	float brk = 1.0 + (ringT - 0.5) * (0.10 + 0.14 * rim) + (mot - 0.5) * 0.12;
	brk -= rim * (0.04 + 0.04 * mot);
	return lerp(1.0, saturate(brk), saturate(FL_COOKIESTRENGTH));
}

float4 Flashlight(VSOUT IN) : COLOR0
{
	float d0 = readDepth(IN.UVCoord);
	float3 eyeV = toWorld(IN.UVCoord);
	float3 eyeDir = normalize(eyeV);
	float3 wpos = TESR_CameraPosition.xyz + eyeV * d0;
	float3 nrm = GetWorldNormal(IN.UVCoord);

	float3 lpos = TESR_SpotLightPosition.xyz;
	float3 ldir = TESR_SpotLightDirection.xyz;
	float3 toLight = lpos - wpos;
	float3 lvec = normalize(toLight);

	// Soft edges replace the clamped N.L with wrap lighting. The normals here are
	// reconstructed from depth, which is near-random across alpha tested foliage, and the
	// clamp turns that into hard speckle because so many samples land on exactly zero.
	//
	// Squaring the wrap is what actually removes the speckle: it has to stay shallow around
	// N.L = 0, where the reconstructed normals scatter, or the noise comes straight back.
	// Cubing it is shallow enough nowhere near there and reintroduces the dithering.
	//
	// So the shape is fixed at squared, and the energy is corrected with a constant instead.
	// For ((N.L + 1) / 2)^k the mean over uniformly distributed normals is 1/(k+1); clamped
	// N.L has a mean of 1/4, so squared runs a third bright and 3/4 brings it back. That
	// costs a quarter off a surface facing the light head on, which is simply what lifting
	// the grazing angles has to be paid for with if the total is to stay put.
	float ndl = dot(lvec, nrm);
	float wrap = saturate(ndl * 0.5 + 0.5);
	float diffT = (FL_SOFTEDGE > 0.5) ? (0.75 * wrap * wrap) : max(ndl, 0.0);
	float specT = pows(shades(normalize(eyeDir + lvec * -1), nrm), 5);

	// radius based attenuation based on https://lisyarus.github.io/blog/graphics/2022/07/30/point-light-attenuation.html
	float rad = TESR_SpotLightPosition.w;
	float dRatio = length(toLight)/rad;
	float sq = saturate(dRatio * dRatio);
	float fall = saturate(((1 - sq) * (1 - sq)) / (1 + 5.0 * sq));

	float4 lsc = clipToTex(mul(float4(wpos, 1), TESR_FlashLightViewProjTransform));
	float occ = tex2D(TESR_RenderedBuffer, IN.UVCoord);

	float2 cookieUV = lsc.xy;
	float cookieT = tex2D(TESR_SpotLightTexture, cookieUV).r * cookieBreakup(cookieUV);

	float lumInv = 1 / max(0.05, luma(TESR_SunColor));
	float3 lcol = TESR_SpotLightColor.rgb * TESR_SpotLightColor.w * lumInv;

	float cosOuter = cos(radians(TESR_SpotLightDirection.w));
	float cosInner = cos(radians(TESR_SpotLightDirection.w * 0.5));
	float cone = pow(invlerps(cosOuter, cosInner, shades(ldir, lvec * -1)), 2.0);

	// Ramp the pool down point blank so a wall a foot away doesn't clip to white
	float nearF = (FL_NEARFADE > 0.0) ? smoothstep(0.0, FL_NEARFADE, length(toLight)) : 1.0;

	float3 res = (diffT + specT) * lcol * cone * fall * cookieT * occ * nearF;

	return float4(res, 1);
}


// Local minimum over four diagonal taps. scaleFactor limits the pass to the top left part of the screen.
float4 BoxBlurMin (VSOUT IN, uniform sampler2D buffer, uniform float scaleFactor) :COLOR0
{
	float2 io = float2(-1, 1) * 0.5;
	float2 part = scaleFactor * TESR_RenderScale.xy;	// UNOFFICIAL 2026-10-27 RenderScale v2: of the drawn part (as Includes/Blur.hlsl)
	clip((IN.UVCoord <= part) - 1);

	float2 maxuv = part - 1.5 * TESR_ReciprocalResolution.xy;
	float4 mn = tex2D(buffer, IN.UVCoord);
	mn = min(mn, tex2D(buffer, min(IN.UVCoord + io.xx * TESR_ReciprocalResolution.xy, maxuv)));
	mn = min(mn, tex2D(buffer, min(IN.UVCoord + io.xy * TESR_ReciprocalResolution.xy, maxuv)));
	mn = min(mn, tex2D(buffer, min(IN.UVCoord + io.yx * TESR_ReciprocalResolution.xy, maxuv)));
	mn = min(mn, tex2D(buffer, min(IN.UVCoord + io.yy * TESR_ReciprocalResolution.xy, maxuv)));

	return mn;
}


// Local mean over the same four taps. scaleFactor limits the pass to the top left part of the screen.
float4 BoxBlurAvg (VSOUT IN, uniform sampler2D buffer, uniform float scaleFactor) :COLOR0
{
	float2 io = float2(-1, 1) * 0.5;
	float2 part = scaleFactor * TESR_RenderScale.xy;	// UNOFFICIAL 2026-10-27 RenderScale v2: of the drawn part (as Includes/Blur.hlsl)
	clip((IN.UVCoord <= part) - 1);

	float2 maxuv = part - 1.5 * TESR_ReciprocalResolution.xy;
	float4 sum = tex2D(buffer, IN.UVCoord);
	sum += tex2D(buffer, min(IN.UVCoord + io.xx * TESR_ReciprocalResolution.xy, maxuv));
	sum += tex2D(buffer, min(IN.UVCoord + io.xy * TESR_ReciprocalResolution.xy, maxuv));
	sum += tex2D(buffer, min(IN.UVCoord + io.yx * TESR_ReciprocalResolution.xy, maxuv));
	sum += tex2D(buffer, min(IN.UVCoord + io.yy * TESR_ReciprocalResolution.xy, maxuv));

	return float4(sum.rgb/= 5, 1);
}


// ---- UNOFFICIAL: depth-aware edges ([Shaders.Flashlight.Main] EdgeFix) ----
// The light is worked out from one depth sample per pixel: with MSAA the depth buffer keeps one of the
// pixel's samples (the depth resolve copies one, it does not average), while the colour is the average of
// all of them, so at the edge of a leaf the colour is part leaf and part whatever is behind it. Combine
// scales that whole blend by the light of whichever side the depth sample landed on, so the background part
// of the pixel gets the leaf's light (a bright rim in the background's colour, worst against a bright sky)
// or the leaf part gets none (a dark notch), and the choice flips from pixel to pixel along the edge: the
// light brings back the jaggies MSAA removed. At a depth edge, FindEdgeSplit splits the pixel's colour into
// a near and a far part, with a neighbour from each side as the reference; SplitLight then lights each part
// with its own side's light. With EdgeFix 2 the beam's light shaft gets the same treatment (BeamAt): its
// half resolution buffer is upsampled by depth instead of bilinearly, which spread the shaft seen past a
// leaf over the leaf's edge, and at an edge each side's shaft counts in proportion to the side's share of
// the pixel. Off edges nothing changes. (Making the two blur passes depth-aware as well was tried on the
// bench: it removed a little more of the rim but left dark outlines and speckle on grass, so they are kept.)

// Edges are breaks in the surface, not steep surfaces: on any flat surface 1 / depth is linear across
// the screen, so for two opposite neighbours a and b, dc / a + dc / b - 2 is zero however steeply the
// surface is seen (ground at a grazing angle), and it is large where one of them is on something
// else. That measure counts as an edge from 2%, fully from 5%.
static const float EdgeDepthStart = 0.02;
static const float EdgeDepthRamp = 0.03;
// The cheap test that comes first (DepthBreakAround) sees a 2% break next to the pixel as 0.5%.
static const float EdgeGate = 0.005;

float DepthBreak(float dc, float a, float b)
{
	return abs(dc / max(a, 1.0) + dc / max(b, 1.0) - 2.0);
}

// The same measure on two bilinear reads at opposite corners of the pixel. Each is the average of a
// 2x2 block of depths that includes this pixel, so two fetches see seven of the nine pixels of the 3x3
// neighbourhood (all but the top-right and bottom-left ones), and a break among them moves the pair.
// Measured on dense grass, also reading the other two corners found nothing more worth its cost.
float DepthBreakAround(float2 uv, float dc)
{
	float2 h = 0.5 * TESR_ReciprocalResolution.xy;
	float topLeft = readDepthLod(uv - h), bottomRight = readDepthLod(uv + h);
	return DepthBreak(dc, topLeft, bottomRight);
}

struct EdgeSplit
{
	float weight;           // how much of the split result to use: 0 off edges, and where both sides look alike
	float coverage;         // share of the pixel taken by the near side
	float2 uvNear, uvFar;   // the reference neighbours of the two sides
	float3 nearColor, farColor;
	float3 nearPart, farPart;   // this pixel's colour, split between the two sides
};

float3 SourceColorLod(float2 uv, bool sourceIsLinear)
{
	float4 c = tex2Dlod(TESR_SourceBuffer, float4(uv, 0, 0));
	return sourceIsLinear ? c.rgb : linearize(c.rgb);
}

// Combine's own modulation of the light by the surface colour (dimmer on already bright surfaces)
float3 ModulatedLight(float3 part, float3 surface, float3 light)
{
	return part * max(0.0, luma(exp(-surface * 3.5)) * light);
}

EdgeSplit FindEdgeSplit(float2 uv, float dc, float3 C, bool sourceIsLinear)
{
	EdgeSplit e = (EdgeSplit)0;
	float2 px = TESR_ReciprocalResolution.xy;
	float2 uvFar = uv;
	float dNear = 1e30, dFar = -1.0;
	float2 offsets[8] = { float2(-px.x, 0), float2(px.x, 0), float2(0, -px.y), float2(0, px.y),
		float2(-px.x, -px.y), float2(px.x, -px.y), float2(-px.x, px.y), float2(px.x, px.y) };
	float depths[8];
	[unroll] for (int i = 0; i < 8; i++) {
		depths[i] = readDepthLod(uv + offsets[i]);
		dNear = min(dNear, depths[i]);
		if (depths[i] > dFar) { dFar = depths[i]; uvFar = uv + offsets[i]; }
	}
	// Opposite pairs: left/right, up/down and the two diagonals
	float breakSize = max(max(DepthBreak(dc, depths[0], depths[1]), DepthBreak(dc, depths[2], depths[3])),
		max(DepthBreak(dc, depths[4], depths[7]), DepthBreak(dc, depths[5], depths[6])));
	float edge = saturate((breakSize - EdgeDepthStart) / EdgeDepthRamp);
	[branch] if (edge <= 0.0) return e;

	// The far side's reference is the farthest neighbour. For the near side, thin things such as
	// grass blades rarely have a neighbour that is all leaf, and a part-background reference makes
	// the background part look like leaf; so of the four direct neighbours on the near side, the one
	// whose colour is least like the far side's is taken as the purest. (None: weight 0 below.)
	float3 Cf = SourceColorLod(uvFar, sourceIsLinear);
	float3 Cn = Cf;
	float2 uvNear = uvFar;
	float nearSplit = lerp(dNear, dFar, 0.5);
	float purest = -1.0;
	[unroll] for (int k = 0; k < 4; k++) {
		[branch] if (depths[k] < nearSplit) {
			float2 t = uv + offsets[k];
			float3 c = SourceColorLod(t, sourceIsLinear);
			float3 diff = c - Cf;
			float distance = dot(diff, diff);
			if (distance > purest) { purest = distance; Cn = c; uvNear = t; }
		}
	}

	// Coverage of the near side: where this pixel's colour sits between the two references
	float3 dC = Cn - Cf;
	float dd = dot(dC, dC);
	float a = saturate(dot(C - Cf, dC) / max(dd, 1e-10));

	// Split the pixel's own colour, so its texture detail is kept: the near part is a * C plus
	// the share of the difference the near side accounts for, the far part is the rest.
	e.nearPart = max(a * C + a * (1.0 - a) * dC, 0.0);
	e.farPart = max(C - e.nearPart, 0.0);

	// Two sides of nearly the same colour say nothing about coverage: keep the old result there
	float contrast = dd / max(max(dot(Cn, Cn), dot(Cf, Cf)), 1e-8);
	e.weight = edge * saturate((contrast - 0.0025) / 0.0075);
	e.coverage = a;
	e.uvNear = uvNear;
	e.uvFar = uvFar;
	e.nearColor = Cn;
	e.farColor = Cf;
	return e;
}

float3 SplitLight(EdgeSplit e)
{
	float3 Ln = tex2Dlod(TESR_RenderedBuffer, float4(e.uvNear, 0, 0)).rgb;
	float3 Lf = tex2Dlod(TESR_RenderedBuffer, float4(e.uvFar, 0, 0)).rgb;
	return ModulatedLight(e.nearPart, e.nearColor, Ln) + ModulatedLight(e.farPart, e.farColor, Lf);
}

// Depth-aware upsample of the half resolution beam buffer for the pixel at uv (EdgeFix 2). Texel t of
// that buffer marched the view ray of full resolution texel t * size ratio (the frame quad carries the
// full resolution half pixel offset into the half resolution pass), so that is the depth it stands
// for, and the pixel sits between texels at those positions - all of them inside its 3x3
// neighbourhood. Where those texels share the pixel's depth (flat, or the four depths agree) this
// is one bilinear read; otherwise each is weighted by distance and by how close its depth is to the
// pixel's, and when none is close the closest one is used.
float3 BeamUpsampled(float2 uv, float dc, bool flat)
{
	float2 px = TESR_ReciprocalResolution.xy;
	float2 halfPx = FL_BEAMTEXEL;
	float2 pos = floor(uv / px) * (px / halfPx);
	[branch] if (flat) return tex2Dlod(TESR_VolumetricBuffer, float4((pos + 0.5) * halfPx, 0, 0)).rgb;

	float2 base = floor(pos);
	float2 f = pos - base;
	float2 corners[4] = { float2(0, 0), float2(1, 0), float2(0, 1), float2(1, 1) };
	float4 rel;
	[unroll] for (int i = 0; i < 4; i++)
		rel[i] = abs(readDepthLod(0.5 * px + (base + corners[i]) * halfPx) - dc) / max(dc, 1.0);
	[branch] if (max(max(rel.x, rel.y), max(rel.z, rel.w)) < 0.04)
		return tex2Dlod(TESR_VolumetricBuffer, float4((pos + 0.5) * halfPx, 0, 0)).rgb;

	// Tolerant of the few percent a steep but continuous surface (ground seen at a grazing angle)
	// changes over two pixels - weighting those down shows the march's dither as speckle - while a
	// leaf against what is behind it (tens of percent) is weighted down 15 to 150 times.
	float3 sum = 0;
	float wsum = 0;
	float3 closest = 0;
	float closestRel = 1e30;
	[unroll] for (int k = 0; k < 4; k++) {
		float3 v = tex2Dlod(TESR_VolumetricBuffer, float4((base + corners[k] + 0.5) * halfPx, 0, 0)).rgb;
		float2 wb2 = lerp(1.0 - f, f, corners[k]);
		float wb = wb2.x * wb2.y;
		float w = wb / (1.0 + sqr(rel[k] / 0.08));
		sum += v * w;
		wsum += w;
		if (wb > 0.0 && rel[k] < closestRel) { closestRel = rel[k]; closest = v; }
	}
	return wsum > 1e-4 ? sum / wsum : closest;
}

// The shaft in front of the pixel at uv, with the same yield and soft ceiling Combine applies
float3 BeamAt(float2 uv, float depth, bool flat)
{
	float3 vol = BeamUpsampled(uv, depth, flat);
	float3 surfPos = TESR_CameraPosition.xyz + toWorld(uv) * depth;
	float3 surfToL = TESR_SpotLightPosition.xyz - surfPos;
	float  surfDist = length(surfToL);
	float surfCone = pow(invlerps(
		cos(radians(TESR_SpotLightDirection.w)),
		cos(radians(TESR_SpotLightDirection.w * 0.5)),
		shades(TESR_SpotLightDirection.xyz, -surfToL / max(surfDist, 0.001))), 2.0);
	float sSurf = saturate(sqr(surfDist / TESR_SpotLightPosition.w));
	float surfAtten = saturate(sqr(1.0 - sSurf) / (1.0 + 5.0 * sSurf));
	vol *= 1.0 - 0.85 * saturate(surfCone * surfAtten * 4.0);
	return vol / (1.0 + luma(vol) * 2.5);
}

// UNOFFICIAL: edgeFix is a compile-time switch. The original techniques compile Combine(false), which is the
// unchanged composite; the EdgeFix techniques compile Combine(true).
float4 Combine (VSOUT IN, uniform bool edgeFix) : COLOR0
{
	// With RenderPreTonemapping, which is the default, this pass draws onto the game's HDR
	// scene surface and TESR_SourceBuffer is already linear. Decoding it as sRGB and
	// re-encoding on the way out is then a second transform. It almost cancels where no
	// light is added, but it also feeds the exp() rolloff below a value divided by 12.92
	// in the dark end, so the rolloff stops rolling off and close surfaces blow out.
	// Only apply the transform when this really is the post tonemapping LDR surface.
	bool sourceIsLinear = FL_LINEARSOURCE > 0.5;
	float4 source = tex2D(TESR_SourceBuffer, IN.UVCoord);
	float4 color = sourceIsLinear ? source : linearize(source);
	float4 light = tex2D(TESR_RenderedBuffer, IN.UVCoord);

	float3 addLight = color.rgb * max(0.0, luma(exp(-color.rgb * 3.5)) * light.rgb); // modulate light with base color brightness to compensate for the post process aspect

	// UNOFFICIAL: depth-aware edges, see FindEdgeSplit
	EdgeSplit edge = (EdgeSplit)0;
	float depthHere = 0.0;
	bool flat = true;
	bool edgeBeam = edgeFix && FL_EDGEBEAM > 0.5 && TESR_VolumetricControl.x > 0.0;
	if (edgeFix) {
		// Nothing to fix where neither the pool nor (EdgeFix 2) the shaft reaches: there the blurred
		// light is 0 across the pixel's whole 3x3 neighbourhood, and the bilinear beam read is 0 on every
		// half resolution texel the depth-aware upsample would use. Most of the screen ends here.
		float3 shaftHere = edgeBeam ? tex2Dlod(TESR_VolumetricBuffer, float4(IN.UVCoord, 0, 0)).rgb : 0.0;
		[branch] if (luma(light.rgb) > 0.0 || luma(shaftHere) > 0.0) {
			depthHere = readDepthLod(IN.UVCoord);
			flat = DepthBreakAround(IN.UVCoord, depthHere) < EdgeGate;
			[branch] if (!flat) {
				edge = FindEdgeSplit(IN.UVCoord, depthHere, color.rgb, sourceIsLinear);
				[branch] if (edge.weight > 0.0) addLight = lerp(addLight, SplitLight(edge), edge.weight);
			}
		}
	}

	// In-air light shaft from the FlashlightBeam effect. Gated on the same value the march
	// is gated on, so the half res buffer can never bleed in once the beam is switched off.
	if (TESR_VolumetricControl.x > 0.0) {
		// The s7 sampler is bilinear, so this upsamples the half res buffer for free
		float3 vol = tex2D(TESR_VolumetricBuffer, IN.UVCoord).rgb;

		// The shaft yields wherever this pixel's surface is already inside the beam. That
		// surface gets the lit pool, and stacking the air glow on top of it just reads as a
		// hotter cast. The test is geometric - the cone and attenuation evaluated at the
		// surface - so the pool keeps its normal brightness at any shaft strength, while
		// surfaces outside the beam still get the full shaft in front of them.
		float3 surfPos = TESR_CameraPosition.xyz + toWorld(IN.UVCoord) * readDepth(IN.UVCoord);
		float3 surfToL = TESR_SpotLightPosition.xyz - surfPos;
		float  surfDist = length(surfToL);
		float surfCone = pow(invlerps(
			cos(radians(TESR_SpotLightDirection.w)),
			cos(radians(TESR_SpotLightDirection.w * 0.5)),
			shades(TESR_SpotLightDirection.xyz, -surfToL / max(surfDist, 0.001))), 2.0);
		float sSurf = saturate(sqr(surfDist / TESR_SpotLightPosition.w));
		float surfAtten = saturate(sqr(1.0 - sSurf) / (1.0 + 5.0 * sSurf));
		vol *= 1.0 - 0.85 * saturate(surfCone * surfAtten * 4.0);

		// Soft ceiling: a ray looking straight down the beam integrates the whole lit
		// column and would otherwise fill the screen. Dim shafts pass nearly unchanged.
		vol /= 1.0 + luma(vol) * 2.5;
		// UNOFFICIAL (EdgeFix 2): depth-aware upsample, and at an edge each side's own shaft by its share
		// of the pixel, see BeamAt. A side's shaft is one bilinear read at its reference neighbour: measured
		// against a depth-aware read there it looked the same and cost a third of EdgeFix's time.
		if (edgeBeam) {
			vol = BeamAt(IN.UVCoord, depthHere, flat);
			[branch] if (edge.weight > 0.0) {
				float3 nearVol = BeamAt(edge.uvNear, readDepthLod(edge.uvNear), true);
				float3 farVol = BeamAt(edge.uvFar, readDepthLod(edge.uvFar), true);
				vol = lerp(vol, edge.coverage * nearVol + (1.0 - edge.coverage) * farVol, edge.weight);
			}
		}
		addLight += vol;
	}

	// Reinhard style roll off on the added light only, so the centre of the pool stops
	// short of clipping without dimming the falloff around it
	if (FL_HOTSPOTLIMIT > 0.0) {
		float limit = 1.0 / FL_HOTSPOTLIMIT;
		addLight *= limit / max(limit, luma(addLight));
	}

	color.rgb += addLight;

    return sourceIsLinear ? color : delinearize(color);
}

technique {

	pass {
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 NoShadow();
	}

	pass {
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 Flashlight();
	}

	pass {
		VertexShader = compile vs_3_0 FrameVS();
	 	PixelShader = compile ps_3_0 BoxBlurMin(TESR_RenderedBuffer, 1.0);
	}
	pass {
		VertexShader = compile vs_3_0 FrameVS();
	 	PixelShader = compile ps_3_0 BoxBlurAvg(TESR_RenderedBuffer, 1.0);
	}

	pass {
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 Combine(false);
	}
}


technique {

	pass {
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 Shadows();
	}
	
	pass {
		VertexShader = compile vs_3_0 FrameVS();
	 	PixelShader = compile ps_3_0 DepthBlur(TESR_RenderedBuffer, OffsetMaskH, 1.0, 350, TESR_SpotLightPosition.w * 2);
	}

	pass {
		VertexShader = compile vs_3_0 FrameVS();
	 	PixelShader = compile ps_3_0 DepthBlur(TESR_RenderedBuffer, OffsetMaskV, 1.0, 350, TESR_SpotLightPosition.w * 2);
	}
	
	pass {
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 Flashlight();
	}

	pass {
		VertexShader = compile vs_3_0 FrameVS();
	 	PixelShader = compile ps_3_0 BoxBlurMin(TESR_RenderedBuffer, 1.0);
	}
	pass {
		VertexShader = compile vs_3_0 FrameVS();
	 	PixelShader = compile ps_3_0 BoxBlurAvg(TESR_RenderedBuffer, 1.0);
	}

	pass {
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 Combine(false);
	}

}

// UNOFFICIAL: the two techniques above with depth-aware edges ([Shaders.Flashlight.Main] EdgeFix). Kept as
// techniques of their own so that with EdgeFix off the original ones run exactly as before.
technique EdgeFix {

	pass {
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 NoShadow();
	}

	pass {
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 Flashlight();
	}

	pass {
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 BoxBlurMin(TESR_RenderedBuffer, 1.0);
	}
	pass {
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 BoxBlurAvg(TESR_RenderedBuffer, 1.0);
	}

	pass {
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 Combine(true);
	}
}

technique EdgeFixShadows {

	pass {
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 Shadows();
	}

	pass {
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 DepthBlur(TESR_RenderedBuffer, OffsetMaskH, 1.0, 350, TESR_SpotLightPosition.w * 2);
	}

	pass {
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 DepthBlur(TESR_RenderedBuffer, OffsetMaskV, 1.0, 350, TESR_SpotLightPosition.w * 2);
	}

	pass {
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 Flashlight();
	}

	pass {
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 BoxBlurMin(TESR_RenderedBuffer, 1.0);
	}
	pass {
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 BoxBlurAvg(TESR_RenderedBuffer, 1.0);
	}

	pass {
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 Combine(true);
	}

}
 