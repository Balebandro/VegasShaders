// WetWorld: rain wetness on open ground, puddles in hollows, rain splashes, wall streaks and puddle reflections.
// Written from the spec (rw-tools/specs/WetWorld.md). Techniques, sampler registers and constant names match WetWorld.cpp.
// Technique 0 and 2 keep the original puddle trace (no reflection, as the source describes it); 3, 4 and 6 use the fixed trace.

float4 TESR_ReciprocalResolution;
float4 TESR_GameTime;
float4 TESR_SunDirection;
float4 TESR_SunPosition;
float4 TESR_SunAmount;
float4 TESR_SunColor;
float4 TESR_SkyData;
float4 TESR_SkyColor;
float4 TESR_SkyLowColor;
float4 TESR_HorizonColor;
float4 TESR_OrthoData;
float4 TESR_WaterSettings;
float4 TESR_WetWorldData;	// x rain, y rainy flag (unused: see the rain fade note), z puddle amount, w darkness
float4 TESR_WetWorldLook;	// x KeepHighlights
float4x4 TESR_ShadowCameraToLightTransformOrtho;
float4 TESR_LightPosition[12];
float4 TESR_ShadowLightPosition[12];
float4 TESR_LightColor[24];

sampler2D TESR_RenderedBuffer : register(s0) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
sampler2D TESR_DepthBuffer : register(s1) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
sampler2D TESR_SourceBuffer : register(s2) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
sampler2D TESR_RippleSampler : register(s3) < string ResourceName = "Precipitations\ripples.dds"; > = sampler_state { ADDRESSU = WRAP; ADDRESSV = WRAP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_OrthoMapBuffer : register(s4) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
sampler2D TESR_NormalsBuffer : register(s5) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
sampler2D TESR_PointShadowBuffer : register(s6) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
sampler2D TESR_DepthBufferViewModel : register(s7) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
sampler2D TESR_SplashSampler : register(s8) < string ResourceName = "Precipitations\splash.dds"; > = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_SplashNormalSampler : register(s9) < string ResourceName = "Precipitations\splash_n.dds"; > = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_PuddleCloudSampler : register(s10) < string ResourceName = "water\water_d.dds"; > = sampler_state { ADDRESSU = WRAP; ADDRESSV = WRAP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_PuddleNormalSampler : register(s11) < string ResourceName = "water\water_nrm.dds"; > = sampler_state { ADDRESSU = WRAP; ADDRESSV = WRAP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_WaterStreakSampler : register(s12) < string ResourceName = "Precipitations\water-streaks_n.dds"; > = sampler_state { ADDRESSU = WRAP; ADDRESSV = WRAP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_CoverMapBuffer : register(s13) = sampler_state { ADDRESSU = WRAP; ADDRESSV = WRAP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };

#include "Includes/Helpers.hlsl"
#include "Includes/Depth.hlsl"
#include "Includes/Normals.hlsl"
#include "Includes/Sky.hlsl"
#include "Includes/CoverMap.hlsl"

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

// ---------------------------------------------------------------------------------------------------------------------
// Helpers

float WetHash(float2 p)
{
	return frac(sin(dot(p, float2(12.9898f, 78.233f))) * 43758.5453f);
}

float DotLum(float3 c)
{
	return dot(c, float3(0.2126f, 0.7152f, 0.0722f));
}

// Far limit: 0.9 of the ortho map's half width (TESR_OrthoData.x is the full width: ShadowManager sets it to 2 x Distance).
float WetFarLimit()
{
	return 0.9f * max(TESR_OrthoData.x, 1.0f) * 0.5f;
}

// Vertical view-model test: a first-person pixel is nearer than the normal depth test (reversed depth: above 0.01).
bool WetFirstPerson(float vm)
{
	return invertedDepth > 0.5f ? (vm > 0.01f) : (vm < 0.9f);
}

struct WetOrtho
{
	float2 uv;
	float z;
	float inside;
	float top;
	float open;	// 1 when nothing lies between the point and the sun (or the map's top is the point)
};

// rel: camera-relative world position. Outside the ortho map the point counts as covered (open 0).
WetOrtho WetOrthoAt(float3 rel)
{
	WetOrtho r;
	float4 o = mul(float4(rel, 1.0f), TESR_ShadowCameraToLightTransformOrtho);
	o.xyz /= max(abs(o.w), 1.0e-6f) * sign(o.w + 1.0e-12f);
	r.uv = float2(o.x * 0.5f + 0.5f, o.y * -0.5f + 0.5f);
	r.z = o.z;
	r.inside = (abs(o.x) <= 1.0f && abs(o.y) <= 1.0f && o.z >= 0.0f && o.z <= 1.0f) ? 1.0f : 0.0f;
	r.top = tex2Dlod(TESR_OrthoMapBuffer, float4(r.uv, 0.0f, 0.0f)).r;
	r.open = r.inside * step(o.z - 0.0001f, r.top);
	return r;
}

// Ortho map scales: uv per game unit (x), and depth per game unit (z), from the sun's ortho matrix.
void WetOrthoScales(out float uvPerUnit, out float depthPerUnit)
{
	float s = max(abs(TESR_ShadowCameraToLightTransformOrtho[3][3]), 1.0e-6f);
	float3 xAxis = float3(TESR_ShadowCameraToLightTransformOrtho[0][0], TESR_ShadowCameraToLightTransformOrtho[1][0], TESR_ShadowCameraToLightTransformOrtho[2][0]);
	float3 zAxis = float3(TESR_ShadowCameraToLightTransformOrtho[0][2], TESR_ShadowCameraToLightTransformOrtho[1][2], TESR_ShadowCameraToLightTransformOrtho[2][2]);
	uvPerUnit = 0.5f * length(xAxis) / s;
	depthPerUnit = length(zAxis) / s;
}

// Rain ripples: four layers of the ripple texture (mask, two encoded direction values, phase), built up with the rain.
float2 WetRipples(float2 w, float clock, float rain, out float mask)
{
	float2 slope = float2(0.0f, 0.0f);
	mask = 0.0f;
	float strength = saturate(rain * 4.0f);
	[unroll] for (int i = 0; i < 4; i++)
	{
		float scale = i == 0 ? 1.0f : (i == 1 ? 1.1f : (i == 2 ? 1.3f : 1.5f));
		float layer = i == 0 ? 1.0f : (i == 1 ? 0.75f : (i == 2 ? 0.5f : 0.25f));
		float rate = 0.96f + 0.01f * i;
		float4 rip = tex2Dlod(TESR_RippleSampler, float4(w / (120.0f * scale), 0.0f, 0.0f));
		float pulse = sin(frac(rip.a + clock * rate) * PI);
		float amp = strength * layer * rip.r * pulse;
		slope += (rip.gb * 2.0f - 1.0f) * amp;
		mask = max(mask, amp);
	}
	return slope;
}

// One streak projection on a wall: two flow groups per lane (8 units wide), with their own random speeds.
float4 WetStreakProj(float2 p, float seed, float clock)
{
	float4 acc = float4(0.0f, 0.0f, 0.0f, 0.0f);
	float lane = floor(p.x / 8.0f);
	[unroll] for (int g = 0; g < 2; g++)
	{
		float h0 = WetHash(float2(lane, seed + 3.0f * g));
		float h1 = WetHash(float2(lane + 0.37f, seed + 7.0f * g + 1.0f));
		float speed = lerp(0.45f, 1.65f, h0) * 12.0f;
		float2 uvS = float2(p.x / 64.0f, (p.y + h1 * 90.0f - clock * speed) / 90.0f);
		acc += 0.5f * tex2Dlod(TESR_WaterStreakSampler, float4(uvS, 0.0f, 0.0f));
	}
	return acc;
}

// ---------------------------------------------------------------------------------------------------------------------
// Pass 0 (wet map): red puddle coverage in hollows on near-flat ground, green flat wetness, blue very flat wetness.

static const float2 WetRing[8] =
{
	float2(1.0f, 0.0f), float2(0.7071f, 0.7071f), float2(0.0f, 1.0f), float2(-0.7071f, 0.7071f),
	float2(-1.0f, 0.0f), float2(-0.7071f, -0.7071f), float2(0.0f, -1.0f), float2(0.7071f, -0.7071f)
};

float4 WetMapCore(float2 uv)
{
	float d = tex2Dlod(TESR_DepthBuffer, float4(uv, 0.0f, 0.0f)).x;
	if (d >= 0.9999f) return float4(0.0f, 0.0f, 0.0f, 1.0f);
	float viewZ;
	float3 wp = reconstructWorldPosition(uv, viewZ).xyz;
	float3 rel = wp - TESR_CameraPosition.xyz;
	float farLim = WetFarLimit();
	if (length(rel.xy) >= farLim) return float4(0.0f, 0.0f, 0.0f, 1.0f);
	float3 n = normalize(GetWorldNormal(uv));
	if (n.z > 0.99f && abs(wp.z - TESR_WaterSettings.x) < d * 500.0f) return float4(0.0f, 0.0f, 0.0f, 1.0f);
	if (WetFirstPerson(tex2Dlod(TESR_DepthBufferViewModel, float4(uv, 0.0f, 0.0f)).x)) return float4(0.0f, 0.0f, 0.0f, 1.0f);

	WetOrtho o = WetOrthoAt(rel);
	if (o.open < 0.5f) return float4(0.0f, 0.0f, 0.0f, 1.0f);

	float c = n.z;
	float g = smoothstep(0.65f, 0.92f, c);
	float b = smoothstep(0.90f, 0.97f, c);

	float uvPerUnit, depthPerUnit;
	WetOrthoScales(uvPerUnit, depthPerUnit);
	float r = 100.0f * uvPerUnit;
	float ringSum = 0.0f;
	float maxJump = 0.0f;
	[unroll] for (int k = 0; k < 8; k++)
	{
		float t = tex2Dlod(TESR_OrthoMapBuffer, float4(o.uv + WetRing[k] * r, 0.0f, 0.0f)).r;
		ringSum += t;
		maxJump = max(maxJump, abs(t - o.z));
	}
	float curvature = (o.z - ringSum / 8.0f) / max(depthPerUnit, 1.0e-6f);
	float concave = smoothstep(0.25f, 2.0f, curvature);
	float edge = 1.0f - smoothstep(40.0f, 80.0f, maxJump / max(depthPerUnit, 1.0e-6f));
	float tilt = smoothstep(0.707f, 0.906f, c);
	float onSurface = 1.0f - smoothstep(0.0003f, 0.0005f, abs(o.top - o.z));
	float puddle = concave * edge * tilt * onSurface;

	return float4(puddle, g, b, 1.0f);
}

float4 WetMapPS(VSOUT IN) : COLOR0
{
	return WetMapCore(IN.UVCoord);
}

// Passes 1 and 2: five-tap depth-aware blur of the wet map (weights 0.375, 0.25, 0.0625), across then down.
float BlurKeep(float2 p, float dC, float tol)
{
	return abs(tex2Dlod(TESR_DepthBuffer, float4(p, 0.0f, 0.0f)).x - dC) <= tol ? 1.0f : 0.0f;
}

float4 BlurCore(float2 uv, float2 dir)
{
	float dC = tex2D(TESR_DepthBuffer, uv).x;
	float tol = 0.02f * dC + 0.5f / max(farZ, 1.0f);
	float2 o1 = dir * TESR_ReciprocalResolution.xy;
	float2 o2 = 2.0f * o1;
	float4 c = tex2D(TESR_RenderedBuffer, uv);
	float4 p1 = tex2D(TESR_RenderedBuffer, uv + o1);
	float4 m1 = tex2D(TESR_RenderedBuffer, uv - o1);
	float4 p2 = tex2D(TESR_RenderedBuffer, uv + o2);
	float4 m2 = tex2D(TESR_RenderedBuffer, uv - o2);
	float w1p = 0.25f * BlurKeep(uv + o1, dC, tol);
	float w1m = 0.25f * BlurKeep(uv - o1, dC, tol);
	float w2p = 0.0625f * BlurKeep(uv + o2, dC, tol);
	float w2m = 0.0625f * BlurKeep(uv - o2, dC, tol);
	float wsum = 0.375f + w1p + w1m + w2p + w2m;
	float4 sum = c * 0.375f + p1 * w1p + m1 * w1m + p2 * w2p + m2 * w2m;
	return float4(sum.rgb / wsum, 1.0f);
}

float4 BlurAcrossPS(VSOUT IN) : COLOR0
{
	return BlurCore(IN.UVCoord, float2(1.0f, 0.0f));
}

float4 BlurDownPS(VSOUT IN) : COLOR0
{
	return BlurCore(IN.UVCoord, float2(0.0f, 1.0f));
}

// ---------------------------------------------------------------------------------------------------------------------
// Puddle reflection trace (fixed): 8 steps in view space, squared spacing to 1800 units, first depth crossing decides.
// Returns linear colour (rgb) and the hit visibility (a); a = 0 on a miss.
float4 WetTrace(float2 uv, float3 pv, float3 rv, float3 nv)
{
	float4 res = float4(0.0f, 0.0f, 0.0f, 0.0f);
	float3 start = pv + nv * 3.0f;
	float prevT = 0.0f;
	float2 texel = max(TESR_ReciprocalResolution.xy, 1.0e-6f);
	[loop] for (int i = 0; i < 8; i++)
	{
		float s = (i + 1) / 8.0f;
		float t = 1800.0f * s * s;
		float3 q = start + rv * t;
		float3 pr = projectPosition(q);
		if (outsideRect(pr.xy)) break;
		float sceneZ = readDepthLod(pr.xy);
		float behind = q.z - sceneZ;
		if (behind > 0.0f)
		{
			float reach = 24.0f + 0.3f * (t - prevT);
			float pixelsAway = length((pr.xy - uv) / texel);
			bool hit = behind < reach && pixelsAway >= 4.0f && !WetFirstPerson(tex2Dlod(TESR_DepthBufferViewModel, float4(pr.xy, 0.0f, 0.0f)).x);
			if (hit)
			{
				float vis = lerp(1.0f, 0.25f, smoothstep(1170.0f, 1800.0f, t));
				float2 es = rectToScreen(pr.xy);
				float edge = smoothstep(0.0f, 0.05f, es.x) * smoothstep(0.0f, 0.05f, 1.0f - es.x) * smoothstep(0.0f, 0.05f, es.y) * smoothstep(0.0f, 0.05f, 1.0f - es.y);
				float3 hitCol = linearize(max(tex2Dlod(TESR_SourceBuffer, float4(pr.xy, 0.0f, 0.0f)).rgb, 0.0f));
				res = float4(hitCol, vis * edge);
			}
			break;
		}
		prevT = t;
	}
	return res;
}

// ---------------------------------------------------------------------------------------------------------------------
// Wet pass. uniform flags pick the technique: fast (early exits), fixedTrace (corrected puddle trace), persistent (cover map).

// One lamp's highlight on the ground (ng), the puddle (np) and a streak (nS): attenuation (1 - distance / radius)^2, exponent 30.
void LampTerms(float4 lp, float3 colour, float shadowMask, float3 wp, float3 V, float3 ng, float3 np, float3 nS,
	inout float3 lampG, inout float3 lampP, inout float3 lampS)
{
	if (lp.w <= 0.0f) return;
	float3 Ld = lp.xyz - wp;
	float dist = length(Ld);
	float att = saturate(1.0f - dist / lp.w);
	att *= att;
	if (att <= 0.0f) return;
	Ld /= max(dist, 1.0e-4f);
	float3 lc = colour * att * shadowMask;
	float3 Hl = normalize(Ld + V);
	lampG += lc * pow(saturate(dot(ng, Hl)), 30.0f);
	lampP += lc * pow(saturate(dot(np, Hl)), 30.0f);
	lampS += lc * pow(saturate(dot(nS, Hl)), 30.0f);
}

float4 WetPassCore(float2 uv, uniform bool fast, uniform bool fixedTrace, uniform bool persistent)
{
	float4 base = tex2D(TESR_SourceBuffer, uv);
	float rawDepth = tex2D(TESR_DepthBuffer, uv).x;
	if (rawDepth >= 0.9999f) return base;
	float viewZ;
	float3 wp = reconstructWorldPosition(uv, viewZ).xyz;
	float3 cam = TESR_CameraPosition.xyz;
	float3 rel = wp - cam;
	float dh = length(rel.xy);
	float farLim = WetFarLimit();
	if (dh >= farLim) return base;
	float3 n = normalize(GetWorldNormal(uv));
	if (n.z > 0.99f && abs(wp.z - TESR_WaterSettings.x) < rawDepth * 500.0f) return base;

	float clock = TESR_GameTime.z;
	float rain = TESR_WetWorldData.x;
	float rainFade = smoothstep(0.05f, 0.35f, rain);
	float farFade = 1.0f - smoothstep(0.5f * farLim, farLim, dh);

	float rippleMask;
	float2 rippleSlope = WetRipples(wp.xy, clock, rain, rippleMask);

	bool fp = WetFirstPerson(tex2Dlod(TESR_DepthBufferViewModel, float4(uv, 0.0f, 0.0f)).x);
	if (fp)
	{
		// First-person pixels: the base picture, refracted slightly by the ripples, blended by the ripple mask.
		float3 baseL = linearize(max(base.rgb, 0.0f));
		float3 srcL = linearize(max(tex2D(TESR_SourceBuffer, uv + rippleSlope * 0.004f).rgb, 0.0f));
		float blend = saturate(rippleMask * 2.0f) * rainFade;
		return float4(delinearize(lerp(baseL, srcL, blend)), base.a);
	}

	float4 m = tex2D(TESR_RenderedBuffer, uv);	// blurred wet map: r puddle, g flat wet, b very flat wet
	float groundW, sheenW, fillP;
	float4 cm = float4(0.0f, 0.0f, 0.0f, 0.0f);
	if (persistent)
	{
		cm = CoverMapRead(rel.xy);
		groundW = cm.x * m.g;
		sheenW = cm.x * m.b;
		fillP = smoothstep(0.08f, 1.0f, cm.y);
	}
	else
	{
		groundW = rainFade * m.g;
		sheenW = rainFade * m.b;
		fillP = smoothstep(0.08f, 1.0f, TESR_WetWorldData.z);
	}
	groundW *= farFade;
	sheenW *= farFade;
	float pm = saturate(m.r * fillP) * farFade;

	WetOrtho o = WetOrthoAt(rel);
	float open = o.open;
	float rainVis = rainFade * open;
	float flatMask = smoothstep(0.65f, 0.92f, n.z);

	if (fast && groundW <= 0.0f && pm <= 0.0f && rainVis <= 0.0f) return base;

	// Normals: ripples on flat rain-reached ground, puddle swell and puddle texture slopes.
	float2 bendXY = rippleSlope * 0.35f * flatMask * rainVis;
	float3 nR = normalize(float3(n.xy + bendXY, n.z));

	float2 pslope = float2(0.0f, 0.0f);
	float2 swell = float2(0.0f, 0.0f);
	if (groundW > 0.0f || pm > 0.0f)
	{
		float swellFade = 1.0f - smoothstep(400.0f, 1800.0f, dh);
		swell = float2(sin(wp.x * 0.2618f + clock * 1.1f), sin(wp.y * 0.2027f - clock * 0.9f)) * 0.003f * pow(max(pm, 0.0f), 1.35f) * swellFade;
		float2 pa = tex2Dlod(TESR_PuddleNormalSampler, float4(wp.xy / 256.0f + float2(0.004f, 0.002f) * clock, 0.0f, 0.0f)).xy * 2.0f - 1.0f;
		float2 pb = tex2Dlod(TESR_PuddleNormalSampler, float4(wp.xy / 350.72f - float2(0.003f, 0.0015f) * clock, 0.0f, 0.0f)).xy * 2.0f - 1.0f;
		pslope = (0.65f * pa + 0.35f * pb) * farFade;
	}
	float3 ng = normalize(float3(nR.xy + pslope * 0.06f, nR.z));
	float3 np = normalize(float3(swell + pslope * 0.25f + rippleSlope * 0.35f * rainVis, 1.0f));

	// Splashes: one per 8-unit cell at most, density rain x roof x 0.395; the sheet's alpha is the strength.
	float splashSt = 0.0f;
	float splashAdd = 0.0f;
	float2 splashN = float2(0.0f, 0.0f);
	if (rain > 0.0f && open > 0.0f && flatMask > 0.0f && dh < 2000.0f)
	{
		float2 cf = wp.xy / 8.0f;
		float2 cell = floor(cf);
		float2 lf = frac(cf);
		if (WetHash(cell) < rain * open * 0.395f)
		{
			float2 cc = 0.25f + 0.5f * float2(WetHash(cell + 17.3f), WetHash(cell + 41.7f));
			float2 sp = (lf - cc) / 0.5f + 0.5f;
			if (all(sp >= 0.0f) && all(sp <= 1.0f))
			{
				float cyc = frac(clock * 1.35f + WetHash(cell + 93.1f));
				float fr = floor(cyc * 16.0f);
				float fy = floor(fr / 4.0f);
				float fx = fr - 4.0f * fy;
				float fade = sin(cyc * PI);
				float2 sheet = (float2(fx, fy) + sp) / 4.0f;
				float4 sT = tex2Dlod(TESR_SplashSampler, float4(sheet, 0.0f, 0.0f));
				float4 sN = tex2Dlod(TESR_SplashNormalSampler, float4(sheet, 0.0f, 0.0f));
				float fadeFar = 1.0f - smoothstep(1200.0f, 2000.0f, dh);
				splashSt = sT.a * fade * flatMask * rainVis * fadeFar;
				float br = saturate((luma(sT) - 0.5f) * 1.5f + 0.5f);
				br = lerp(br, 1.0f, 0.35f);
				splashAdd = br * 0.17f * 0.6f * splashSt;
				splashN = (sN.xy * 2.0f - 1.0f) * 2.2f * splashSt;
			}
		}
	}
	ng = normalize(float3(ng.xy + splashN, ng.z));

	// Wall streaks: near-vertical surfaces, rain-reached, fading out between 800 and 1000 units.
	float streakM = 0.0f;
	float2 streakN = float2(0.0f, 0.0f);
	float wall = 1.0f - smoothstep(0.55f, 0.8f, abs(n.z));
	float streakOn = wall * open * rainFade * (1.0f - smoothstep(800.0f, 1000.0f, dh));
	if (streakOn > 0.0f)
	{
		float ax = abs(n.x), ay = abs(n.y);
		float wx = ax / max(ax + ay, 1.0e-4f);
		float4 sx = WetStreakProj(float2(wp.y, wp.z), 1.0f, clock);
		float4 sy = WetStreakProj(float2(wp.x, wp.z), 2.0f, clock);
		float4 s = wx * sx + (1.0f - wx) * sy;
		float ma = saturate(s.a);
		streakM = ma * streakOn;
		float3 sn = s.rgb / max(ma, 1.0e-4f);
		streakN = (sn.xy * 2.0f - 1.0f) * 0.25f * ma;
	}
	float3 nS = normalize(float3(n.xy + streakN, n.z));

	// Lighting terms.
	float3 eyeW = normalize(rel);
	float3 V = -eyeW;
	float3 L = normalize(TESR_SunDirection.xyz);
	float sunH = saturate(TESR_SunPosition.z);
	float sunF = smoothstep(0.25f, 0.70f, sunH);
	float sunS = tex2Dlod(TESR_PointShadowBuffer, float4(uv, 0.0f, 0.0f)).r;
	float shG = tex2Dlod(TESR_PointShadowBuffer, float4(uv, 0.0f, 0.0f)).g;
	float3 sunCol = linearize(max(TESR_SunColor.rgb, 0.0f));

	float3 Hg = normalize(L + V);
	float gExp = lerp(80.0f, 200.0f, saturate(sheenW));
	float3 sunGround = pow(saturate(dot(ng, Hg)), gExp) * sunF * sunS * sunCol * groundW;
	sunGround = min(sunGround, 1.95f);

	float3 Hp = Hg;
	float3 sunPud = pow(saturate(dot(np, Hp)), 700.0f) * sunF * sunS * sunCol * pm * 2.25f;
	sunPud = min(sunPud, 1.9f);

	float3 sunStreak = pow(saturate(dot(nS, Hg)), 80.0f) * sunF * sunS * sunCol;
	float3 lampG = float3(0.0f, 0.0f, 0.0f);
	float3 lampP = float3(0.0f, 0.0f, 0.0f);
	float3 lampS = float3(0.0f, 0.0f, 0.0f);
	if (groundW > 0.0f || pm > 0.0f || streakM > 0.0f)
	{
		// The shadow-casting lamps (colours 0..11), then the others (colours 12..23).
		[loop] for (int li = 0; li < 12; li++)
		{
			LampTerms(TESR_ShadowLightPosition[li], TESR_LightColor[li].rgb, shG, wp, V, ng, np, nS, lampG, lampP, lampS);
		}
		[loop] for (int lj = 0; lj < 12; lj++)
		{
			LampTerms(TESR_LightPosition[lj], TESR_LightColor[12 + lj].rgb, 1.0f, wp, V, ng, np, nS, lampG, lampP, lampS);
		}
	}
	lampG = min(lampG * groundW, 0.9f);
	lampP = min(lampP * pm * 2.5f, 1.8f);
	float3 streakSpec = min(1.2f * (sunStreak + lampS), 0.25f * streakM);

	// The base picture, refracted by the splashes, linear.
	float2 splashUV = splashN * float2(0.5f * TESR_ProjectionTransform[0][0], 0.5f * TESR_ProjectionTransform[1][1]) * 17.5f / max(viewZ, 1.0f);
	float3 baseL = linearize(max(tex2D(TESR_SourceBuffer, uv + splashUV * splashSt).rgb, 0.0f));
	float3 col = baseL * (1.0f - 0.3f * groundW);
	col += sunGround + lampG;

	// Puddles: refracted body, depth and rim, sky and cloud reflection, optional fixed trace, highlights.
	if (pm > 0.0f)
	{
		float fac = lerp(1.0f, 0.5f, saturate(V.z));
		float2 off = np.xy * 50.0f * 0.35f * fac;
		off *= min(1.0f, 37.5f / max(length(off), 1.0e-4f));
		float3 vo = mul(float4(off.x, off.y, 0.0f, 0.0f), TESR_ViewTransform).xyz;
		float2 refrUV = uv + float2(vo.x * TESR_ProjectionTransform[0][0], -vo.y * TESR_ProjectionTransform[1][1]) * 0.5f / max(viewZ, 1.0f);
		float3 refr = linearize(max(tex2D(TESR_SourceBuffer, refrUV).rgb, 0.0f));
		float3 body = refr * lerp(1.0f, 0.55f, saturate(TESR_WetWorldData.w));
		float depthM = pow(saturate(pm * 1.15f), 2.0f);
		body = lerp(body, DotLum(body) * float3(0.6f, 0.8f, 1.0f), 0.25f * depthM);
		body *= 1.0f - 0.18f * depthM;
		float rim = saturate(4.0f * pm * (1.0f - pm));
		body *= 1.0f - 0.15f * rim;

		float3 R = reflect(eyeW, np);
		float sunInf = saturate(dot(R, L));
		float3 sky = GetSkyColor(saturate(R.z), 1.0f, sunH, sunInf, TESR_SkyData.z, TESR_SkyColor.rgb, TESR_SkyLowColor.rgb, TESR_HorizonColor.rgb, sunCol);
		float cl = tex2Dlod(TESR_PuddleCloudSampler, float4(wp.xy / 256.0f + float2(0.002f, 0.001f) * clock, 0.0f, 0.0f)).r;
		float cloudM = saturate((cl - 0.5f) * 15.0f + 0.5f);
		sky *= lerp(1.0f, 1.65f, cloudM * 0.38f);
		float fres = 0.12f + 0.88f * pow(1.0f - saturate(dot(np, V)), 5.0f);
		float skyW = min(fres * 0.55f, 0.28f) + 0.08f * rim;
		float3 skyScaled = sky * 0.55f;
		float cap = max(DotLum(body) * 0.65f, 0.015f);
		skyScaled *= min(1.0f, cap / max(DotLum(skyScaled), 1.0e-4f));
		float3 refl = skyScaled;

		if (fixedTrace)
		{
			float3 pv = reconstructPosition(uv);
			float3 rv = normalize(mul(float4(R, 0.0f), TESR_ViewTransform).xyz);
			float3 nv = normalize(mul(float4(np, 0.0f), TESR_ViewTransform).xyz);
			float4 tr = WetTrace(uv, pv, rv, nv);
			float fadeA = 1.0f - smoothstep(0.2f, 0.4f, V.z);
			float fadeV = smoothstep(0.3333f, 0.6667f, rectToScreen(uv).y);
			float tw = 0.55f * tr.a * fadeA * fadeV;
			refl = lerp(refl, tr.rgb, tw);
		}

		float3 puddleCol = lerp(body, refl, skyW);
		puddleCol += sunPud + lampP;
		col = lerp(col, puddleCol, pm);
	}

	// Streaks darken the wall and add a clipped highlight.
	col *= 1.0f - 0.45f * streakM;
	col += streakSpec;

	// Splash brightness.
	col += splashAdd;

	float3 outc = delinearize(max(col, 0.0f));
	if (TESR_WetWorldLook.x > 0.5f) outc = max(outc, 0.0f);
	else outc = saturate(outc);
	return float4(outc, 1.0f);
}

float4 WetOriginalPS(VSOUT IN) : COLOR0 { return WetPassCore(IN.UVCoord, false, false, false); }
float4 WetFastPS(VSOUT IN) : COLOR0 { return WetPassCore(IN.UVCoord, true, false, false); }
float4 WetFixedPS(VSOUT IN) : COLOR0 { return WetPassCore(IN.UVCoord, false, true, false); }
float4 WetFixedFastPS(VSOUT IN) : COLOR0 { return WetPassCore(IN.UVCoord, true, true, false); }
float4 WetPersistentPS(VSOUT IN) : COLOR0 { return WetPassCore(IN.UVCoord, true, false, true); }
float4 WetPersistentFixedPS(VSOUT IN) : COLOR0 { return WetPassCore(IN.UVCoord, true, true, true); }

// Cover test: one pixel of a 1x1 target, kept (white) when something lies above a point 48 units over the camera.
float4 CoverTestPS(VSOUT IN) : COLOR0
{
	WetOrtho o = WetOrthoAt(float3(0.0f, 0.0f, 48.0f));
	if (o.inside < 0.5f || !(o.top < o.z - 0.003f)) discard;
	return float4(1.0f, 1.0f, 1.0f, 1.0f);
}

// ---------------------------------------------------------------------------------------------------------------------
// Techniques, in the index order WetWorld.cpp uses.

technique
{
	pass { VertexShader = compile vs_3_0 FrameVS(); PixelShader = compile ps_3_0 WetMapPS(); }
	pass { VertexShader = compile vs_3_0 FrameVS(); PixelShader = compile ps_3_0 BlurAcrossPS(); }
	pass { VertexShader = compile vs_3_0 FrameVS(); PixelShader = compile ps_3_0 BlurDownPS(); }
	pass { VertexShader = compile vs_3_0 FrameVS(); PixelShader = compile ps_3_0 WetOriginalPS(); }
}

technique CoverTestTechnique
{
	pass { VertexShader = compile vs_3_0 FrameVS(); PixelShader = compile ps_3_0 CoverTestPS(); }
}

technique FastPathsTechnique
{
	pass { VertexShader = compile vs_3_0 FrameVS(); PixelShader = compile ps_3_0 WetMapPS(); }
	pass { VertexShader = compile vs_3_0 FrameVS(); PixelShader = compile ps_3_0 BlurAcrossPS(); }
	pass { VertexShader = compile vs_3_0 FrameVS(); PixelShader = compile ps_3_0 BlurDownPS(); }
	pass { VertexShader = compile vs_3_0 FrameVS(); PixelShader = compile ps_3_0 WetFastPS(); }
}

technique FixedTraceTechnique
{
	pass { VertexShader = compile vs_3_0 FrameVS(); PixelShader = compile ps_3_0 WetMapPS(); }
	pass { VertexShader = compile vs_3_0 FrameVS(); PixelShader = compile ps_3_0 BlurAcrossPS(); }
	pass { VertexShader = compile vs_3_0 FrameVS(); PixelShader = compile ps_3_0 BlurDownPS(); }
	pass { VertexShader = compile vs_3_0 FrameVS(); PixelShader = compile ps_3_0 WetFixedPS(); }
}

technique FixedTraceFastPathsTechnique
{
	pass { VertexShader = compile vs_3_0 FrameVS(); PixelShader = compile ps_3_0 WetMapPS(); }
	pass { VertexShader = compile vs_3_0 FrameVS(); PixelShader = compile ps_3_0 BlurAcrossPS(); }
	pass { VertexShader = compile vs_3_0 FrameVS(); PixelShader = compile ps_3_0 BlurDownPS(); }
	pass { VertexShader = compile vs_3_0 FrameVS(); PixelShader = compile ps_3_0 WetFixedFastPS(); }
}

technique PersistentTechnique
{
	pass { VertexShader = compile vs_3_0 FrameVS(); PixelShader = compile ps_3_0 WetMapPS(); }
	pass { VertexShader = compile vs_3_0 FrameVS(); PixelShader = compile ps_3_0 BlurAcrossPS(); }
	pass { VertexShader = compile vs_3_0 FrameVS(); PixelShader = compile ps_3_0 BlurDownPS(); }
	pass { VertexShader = compile vs_3_0 FrameVS(); PixelShader = compile ps_3_0 WetPersistentPS(); }
}

technique PersistentFixedTraceTechnique
{
	pass { VertexShader = compile vs_3_0 FrameVS(); PixelShader = compile ps_3_0 WetMapPS(); }
	pass { VertexShader = compile vs_3_0 FrameVS(); PixelShader = compile ps_3_0 BlurAcrossPS(); }
	pass { VertexShader = compile vs_3_0 FrameVS(); PixelShader = compile ps_3_0 BlurDownPS(); }
	pass { VertexShader = compile vs_3_0 FrameVS(); PixelShader = compile ps_3_0 WetPersistentFixedPS(); }
}
