// The player's flashlight as a post-process spot light (wave 3, written from specs/Flashlight.md, 2026-11-02).
// Techniques (the C++ asks by index 0 / 1 and by name EdgeFix / EdgeFixShadows):
//   0 Lit               no shadow:  NoShadow, Light, MinBlur, AvgBlur, Composite
//   1 LitShadows        shadows:    ShadowMap, BlurH, BlurV, Light, MinBlur, AvgBlur, Composite
//   EdgeFix             as Lit, with the depth-aware composite (CompositeEdge)
//   EdgeFixShadows      as LitShadows, with the depth-aware composite
// Chain: each pass reads the chain's current image (TESR_RenderedBuffer) and writes the next one; Composite reads the image before
// the effect (TESR_SourceBuffer) and the blurred light (TESR_RenderedBuffer).

float4x4 TESR_FlashLightViewProjTransform;	// light view x projection (far = light radius), D3D row convention
float4 TESR_FlashLightPosition;				// registered by the C++; not read here
float4 TESR_FlashLightDirection;			// registered by the C++; not read here
float4 TESR_FlashLightColor;				// registered by the C++; not read here
float4 TESR_FlashLightTuning;				// x near fade distance (0 off), y soft edges (1 wrap light), z hotspot limit (0 off), w cookie strength
float4 TESR_FlashLightComposite;			// x source is linear (1), y EdgeFix 2 (beam upsample depth-aware), z, w 1 / beam buffer size
float4 TESR_SpotLightPosition;				// xyz light position, w radius
float4 TESR_SpotLightDirection;				// xyz axis, w outer cone angle in degrees
float4 TESR_SpotLightColor;					// rgb colour, w dimmer
float4 TESR_SunColor;						// only its luma is used
float4 TESR_ReciprocalResolution;			// 1 / frame width, 1 / frame height
float4 TESR_VolumetricControl;				// x beam strength (0 when the beam effect is off)

sampler2D TESR_SourceBuffer : register(s0) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
sampler2D TESR_RenderedBuffer : register(s1) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_DepthBuffer : register(s2) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
sampler2D TESR_PointShadowBuffer : register(s3) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };	// declared, not read
sampler2D TESR_NormalsBuffer : register(s4) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
sampler2D TESR_ShadowSpotlightBuffer0 : register(s5) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
sampler2D TESR_SpotLightTexture : register(s6) < string ResourceName = "Effects\Flashlight.png"; > = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_VolumetricBuffer : register(s7) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };

struct FlashVertexIn
{
	float4 pos : POSITION0;
	float2 uv : TEXCOORD0;
};

struct FlashVertexOut
{
	float4 pos : POSITION;
	float2 uv : TEXCOORD0;
};

#include "Includes/Helpers.hlsl"
#include "Includes/Depth.hlsl"
#include "Includes/Normals.hlsl"

FlashVertexOut ScreenVS(FlashVertexIn IN)
{
	FlashVertexOut o = (FlashVertexOut)0.0f;
	o.pos = IN.pos;
	o.uv = IN.uv;
	return o;
}

float LumaOfLight(float3 c)
{
	return dot(c, float3(0.2126f, 0.7152f, 0.0722f));
}

float3 SurfaceWorld(float2 uv, float depth)
{
	return TESR_CameraPosition.xyz + toWorld(uv) * depth;
}

float3 SurfaceNormalWorld(float2 uv)
{
	float3 v = mul(TESR_ViewTransform, float4(GetNormal(uv), 0.0f)).xyz;	// the rotation alone
	return v * rsqrt(max(dot(v, v), 1e-8f));
}

// Distance falloff: 1 at the light, 0 at and beyond the radius.
float Falloff(float dist)
{
	float d = dist / max(TESR_SpotLightPosition.w, 1e-4f);
	float s = saturate(d * d);
	return saturate((1.0f - s) * (1.0f - s) / (1.0f + 5.0f * s));
}

// The cone at a surface point: the position of the cosine of the angle between the axis and the light-to-surface direction,
// between the outer edge (0) and the half-angle edge (1), squared.
float ConeAt(float3 pos)
{
	float3 fromLight = pos - TESR_SpotLightPosition.xyz;
	float len = max(length(fromLight), 0.001f);
	float c = dot(TESR_SpotLightDirection.xyz, fromLight / len);
	float cosOuter = cos(radians(TESR_SpotLightDirection.w));
	float cosHalf = cos(radians(TESR_SpotLightDirection.w * 0.5f));
	float t = saturate((c - cosOuter) / max(cosHalf - cosOuter, 1e-4f));
	return t * t;
}

// Light colour: colour times dimmer, over the sun's luma (keeps it visible in daylight and at night alike).
float3 LightColour()
{
	return TESR_SpotLightColor.rgb * TESR_SpotLightColor.w / max(0.05f, LumaOfLight(TESR_SunColor.rgb));
}

// Procedural breakup of the beam (CookieStrength). The ring frequency (36) and the amplitudes (0.10 centre, 0.24 edge, mottling 0.12)
// are the spec's; the warp and mottling frequencies are this version's choice.
float CookieBreakup(float2 cuv, float strength)
{
	float2 p = (cuv - 0.5f) * 2.0f;
	float r = saturate(length(p));
	float warp = 0.4f * sin(7.0f * p.x) + 0.4f * sin(5.0f * p.y);
	float ring = 0.5f + 0.5f * sin(36.0f * r + warp);
	float a = 4.0f * sin(3.0f * p.x + 1.7f * p.y);
	float b = 4.0f * sin(2.6f * p.y + 0.9f * p.x);
	float mottle = 0.5f + 0.5f * sin(a) * sin(b);
	float amp = lerp(0.10f, 0.24f, r);
	float pattern = 1.0f + amp * (2.0f * ring - 1.0f) + 0.12f * (mottle - 0.5f) - 0.08f * smoothstep(0.7f, 1.0f, r);
	return lerp(1.0f, max(pattern, 0.0f), saturate(strength));
}

// Shadow map test: 1 lit, 0 in shadow; 1 outside the reach and the light's frame.
float4 ShadowMapPS(FlashVertexOut IN) : COLOR0
{
	float2 uv = IN.uv;
	float3 pos = SurfaceWorld(uv, readDepthLod(uv));
	float radius = max(TESR_SpotLightPosition.w, 1e-4f);
	float dist = length(TESR_SpotLightPosition.xyz - pos) / radius;
	float lit = 1.0f;
	if (dist < 1.0f) {
		float4 clip = mul(float4(pos, 1.0f), TESR_FlashLightViewProjTransform);
		float2 ndc = clip.xy / max(clip.w, 1e-6f);
		if (all(abs(ndc) <= 1.0f)) {
			float2 suv = float2(ndc.x * 0.5f + 0.5f, 0.5f - 0.5f * ndc.y);
			float stored = tex2Dlod(TESR_ShadowSpotlightBuffer0, float4(suv, 0.0f, 0.0f)).r;
			lit = stored >= dist * 0.95f ? 1.0f : 0.0f;	// bias: 5% of the distance
		}
	}
	return float4(lit, lit, lit, 1.0f);
}

// Depth-aware blur of the shadow mask along dir (texels): 12 taps and the centre, Gaussian-like weights, taps across a depth step
// (more than 350 game units x centre depth / far) left out. Beyond the reach: black.
float4 ShadowBlur(float2 uv, float2 dir)
{
	float depth0 = readDepthLod(uv);
	float3 pos0 = SurfaceWorld(uv, depth0);
	float radius = max(TESR_SpotLightPosition.w, 1e-4f);
	if (length(TESR_SpotLightPosition.xyz - pos0) / radius >= 1.0f) return float4(0.0f, 0.0f, 0.0f, 1.0f);
	float storedC = tex2Dlod(TESR_DepthBuffer, float4(uv, 0.0f, 0.0f)).x;
	float threshold = 350.0f * storedC;
	float sum = tex2Dlod(TESR_RenderedBuffer, float4(uv, 0.0f, 0.0f)).r;
	float wsum = 1.0f;
	[unroll]
	for (int i = 1; i <= 6; i++) {
		float w = exp(-float(i * i) / 18.0f);
		[unroll]
		for (int s = -1; s <= 1; s += 2) {
			float2 tuv = clampToRectIfOn(uv + dir * (float(s * i)) * TESR_ReciprocalResolution.xy, TESR_ReciprocalResolution.xy);
			if (abs(readDepthLod(tuv) - depth0) < threshold) {
				sum += w * tex2Dlod(TESR_RenderedBuffer, float4(tuv, 0.0f, 0.0f)).r;
				wsum += w;
			}
		}
	}
	float v = sum / wsum;
	return float4(v, v, v, 1.0f);
}

float4 ShadowBlurHPS(FlashVertexOut IN) : COLOR0 { return ShadowBlur(IN.uv, float2(1.0f, 0.0f)); }
float4 ShadowBlurVPS(FlashVertexOut IN) : COLOR0 { return ShadowBlur(IN.uv, float2(0.0f, 1.0f)); }

// The no-shadow stand-in: 1 everywhere.
float4 NoShadowPS(FlashVertexOut IN) : COLOR0
{
	return float4(1.0f, 1.0f, 1.0f, 1.0f);
}

// The lit pool: RGB light to add (shadow term in the image before: 1 without shadows).
float4 LightPS(FlashVertexOut IN) : COLOR0
{
	float2 uv = IN.uv;
	float shadow = tex2D(TESR_RenderedBuffer, uv).r;
	float3 pos = SurfaceWorld(uv, readDepth(uv));
	float3 n = SurfaceNormalWorld(uv);

	float3 toLight = TESR_SpotLightPosition.xyz - pos;
	float dist = length(toLight);
	float3 L = toLight / max(dist, 1e-6f);
	float ndl = dot(n, L);
	float diffuse = lerp(saturate(ndl), 0.75f * sqr(ndl * 0.5f + 0.5f), TESR_FlashLightTuning.y);

	float3 V = TESR_CameraPosition.xyz - pos;
	V = V * rsqrt(max(dot(V, V), 1e-8f));
	float3 HV = L + V;
	float3 H = HV * rsqrt(max(dot(HV, HV), 1e-8f));
	float specular = pow(saturate(dot(n, H)), 5.0f);

	float nearFade = TESR_FlashLightTuning.x > 0.0f ? smoothstep(0.0f, TESR_FlashLightTuning.x, dist) : 1.0f;

	// The cookie, projected with the light's matrix (perspective divide, 0 to 1, y flipped), breakup on top.
	float4 clip = mul(float4(pos, 1.0f), TESR_FlashLightViewProjTransform);
	float2 ndc = clip.xy / max(clip.w, 1e-6f);
	float2 cuv = float2(ndc.x * 0.5f + 0.5f, 0.5f - 0.5f * ndc.y);
	float cookie = tex2D(TESR_SpotLightTexture, cuv).r * CookieBreakup(cuv, TESR_FlashLightTuning.w);

	float3 light = (diffuse + specular) * LightColour() * ConeAt(pos) * Falloff(dist) * cookie * shadow * nearFade;
	return float4(max(light, 0.0f), 1.0f);
}

// Min of five taps (the centre and the four diagonal neighbours half a texel away), and their mean.
float4 MinBlurPS(FlashVertexOut IN) : COLOR0
{
	float2 uv = IN.uv;
	if (outsideRect(uv)) return float4(0.0f, 0.0f, 0.0f, 1.0f);
	float2 h = 0.5f * TESR_ReciprocalResolution.xy;
	float3 m = tex2Dlod(TESR_RenderedBuffer, float4(uv, 0.0f, 0.0f)).rgb;
	m = min(m, tex2Dlod(TESR_RenderedBuffer, float4(clampToRectIfOn(uv + float2(h.x, h.y), TESR_ReciprocalResolution.xy), 0.0f, 0.0f)).rgb);
	m = min(m, tex2Dlod(TESR_RenderedBuffer, float4(clampToRectIfOn(uv + float2(-h.x, h.y), TESR_ReciprocalResolution.xy), 0.0f, 0.0f)).rgb);
	m = min(m, tex2Dlod(TESR_RenderedBuffer, float4(clampToRectIfOn(uv + float2(h.x, -h.y), TESR_ReciprocalResolution.xy), 0.0f, 0.0f)).rgb);
	m = min(m, tex2Dlod(TESR_RenderedBuffer, float4(clampToRectIfOn(uv + float2(-h.x, -h.y), TESR_ReciprocalResolution.xy), 0.0f, 0.0f)).rgb);
	return float4(m, 1.0f);
}

float4 AvgBlurPS(FlashVertexOut IN) : COLOR0
{
	float2 uv = IN.uv;
	if (outsideRect(uv)) return float4(0.0f, 0.0f, 0.0f, 1.0f);
	float2 h = 0.5f * TESR_ReciprocalResolution.xy;
	float3 m = tex2Dlod(TESR_RenderedBuffer, float4(uv, 0.0f, 0.0f)).rgb;
	m += tex2Dlod(TESR_RenderedBuffer, float4(clampToRectIfOn(uv + float2(h.x, h.y), TESR_ReciprocalResolution.xy), 0.0f, 0.0f)).rgb;
	m += tex2Dlod(TESR_RenderedBuffer, float4(clampToRectIfOn(uv + float2(-h.x, h.y), TESR_ReciprocalResolution.xy), 0.0f, 0.0f)).rgb;
	m += tex2Dlod(TESR_RenderedBuffer, float4(clampToRectIfOn(uv + float2(h.x, -h.y), TESR_ReciprocalResolution.xy), 0.0f, 0.0f)).rgb;
	m += tex2Dlod(TESR_RenderedBuffer, float4(clampToRectIfOn(uv + float2(-h.x, -h.y), TESR_ReciprocalResolution.xy), 0.0f, 0.0f)).rgb;
	return float4(m * 0.2f, 1.0f);
}

// The scene's colour in linear light (the source is linear HDR, or sRGB to decode).
float3 LinearSource(float2 p)
{
	float3 s = tex2Dlod(TESR_SourceBuffer, float4(p, 0.0f, 0.0f)).rgb;
	return TESR_FlashLightComposite.x > 0.5f ? s : linearize(s);
}

// The added light of one pixel: the source colour times the light, dimmed on bright surfaces (exp(-3.5 x source)), and the hotspot limit.
float3 AddedLight(float3 lin, float3 light)
{
	float dim = LumaOfLight(exp(-3.5f * lin));
	float3 added = lin * max(0.0f, dim * light);
	if (TESR_FlashLightTuning.z > 0.0f) {
		float limit = 1.0f / TESR_FlashLightTuning.z;
		added *= limit / max(limit, LumaOfLight(added));
	}
	return added;
}

// The beam shaft (half resolution in TESR_VolumetricBuffer), dimmed inside the cone and soft-capped. EdgeFix 2 upsamples it depth-aware.
float3 BeamAt(float2 uv, float depth)
{
	float3 shaft = tex2Dlod(TESR_VolumetricBuffer, float4(uv, 0.0f, 0.0f)).rgb;
	if (TESR_FlashLightComposite.y > 0.5f) {
		float2 bt = TESR_FlashLightComposite.zw;
		float2 hh = uv / bt - 0.5f;
		float2 base = floor(hh);
		float2 f = hh - base;
		float3 sum = 0.0f;
		float wsum = 0.0f;
		[unroll]
		for (int j = 0; j < 4; j++) {
			float2 o = float2(fmod(float(j), 2.0f), floor(float(j) * 0.5f));
			float2 suv = (base + o + 0.5f) * bt;
			float gap = abs(readDepthLod(suv) - depth) / (1.0f + 0.01f * depth);
			float bw = lerp(1.0f - f.x, f.x, o.x) * lerp(1.0f - f.y, f.y, o.y);
			float w = bw / (1.0f + 100.0f * gap * gap);
			sum += w * tex2Dlod(TESR_VolumetricBuffer, float4(suv, 0.0f, 0.0f)).rgb;
			wsum += w;
		}
		shaft = sum / max(wsum, 1e-12f);
	}
	shaft *= (1.0f - 0.85f * ConeAt(SurfaceWorld(uv, depth)));
	return shaft / (1.0f + 2.5f * LumaOfLight(shaft));
}

float3 EncodeOut(float3 c)
{
	return TESR_FlashLightComposite.x > 0.5f ? c : delinearize(c);
}

// The plain composite.
float4 CompositePS(FlashVertexOut IN) : COLOR0
{
	float2 uv = IN.uv;
	float3 lin = LinearSource(uv);
	float3 light = tex2Dlod(TESR_RenderedBuffer, float4(uv, 0.0f, 0.0f)).rgb;
	float3 added = AddedLight(lin, light);
	if (TESR_VolumetricControl.x > 0.0f) added += BeamAt(uv, readDepthLod(uv)) * TESR_VolumetricControl.x;
	return float4(EncodeOut(lin + added), 1.0f);
}

// The depth-aware composite (EdgeFix). Only pixels at a depth break change: the light is split between the near and the far
// reference pixel by the colour, and blended back by the edge strength.
float4 CompositeEdgePS(FlashVertexOut IN) : COLOR0
{
	float2 uv = IN.uv;
	float2 rt = TESR_ReciprocalResolution.xy;
	float c = tex2Dlod(TESR_DepthBuffer, float4(uv, 0.0f, 0.0f)).x;
	float dL = tex2Dlod(TESR_DepthBuffer, float4(clampToRectIfOn(uv + float2(-rt.x, 0.0f), rt), 0.0f, 0.0f)).x;
	float dR = tex2Dlod(TESR_DepthBuffer, float4(clampToRectIfOn(uv + float2(rt.x, 0.0f), rt), 0.0f, 0.0f)).x;
	float dU = tex2Dlod(TESR_DepthBuffer, float4(clampToRectIfOn(uv + float2(0.0f, -rt.y), rt), 0.0f, 0.0f)).x;
	float dD = tex2Dlod(TESR_DepthBuffer, float4(clampToRectIfOn(uv + float2(0.0f, rt.y), rt), 0.0f, 0.0f)).x;
	float dUL = tex2Dlod(TESR_DepthBuffer, float4(clampToRectIfOn(uv + float2(-rt.x, -rt.y), rt), 0.0f, 0.0f)).x;
	float dUR = tex2Dlod(TESR_DepthBuffer, float4(clampToRectIfOn(uv + float2(rt.x, -rt.y), rt), 0.0f, 0.0f)).x;
	float dDL = tex2Dlod(TESR_DepthBuffer, float4(clampToRectIfOn(uv + float2(-rt.x, rt.y), rt), 0.0f, 0.0f)).x;
	float dDR = tex2Dlod(TESR_DepthBuffer, float4(clampToRectIfOn(uv + float2(rt.x, rt.y), rt), 0.0f, 0.0f)).x;

	// Break measure of the four opposite pairs: 0 on a flat surface, however steep.
	float c0 = max(c, 1e-6f);
	float b1 = abs(c0 / max(dL, 1e-4f) + c0 / max(dR, 1e-4f) - 2.0f);
	float b2 = abs(c0 / max(dU, 1e-4f) + c0 / max(dD, 1e-4f) - 2.0f);
	float b3 = abs(c0 / max(dUL, 1e-4f) + c0 / max(dDR, 1e-4f) - 2.0f);
	float b4 = abs(c0 / max(dUR, 1e-4f) + c0 / max(dDL, 1e-4f) - 2.0f);
	float edge = smoothstep(0.02f, 0.05f, max(max(b1, b2), max(b3, b4)));

	float3 lin = LinearSource(uv);
	float3 light = tex2Dlod(TESR_RenderedBuffer, float4(uv, 0.0f, 0.0f)).rgb;
	if (edge > 0.0f) {
		// The far reference: the farthest of the eight neighbours.
		float2 farOff = float2(-rt.x, 0.0f);
		float farDepth = dL;
		if (dR > farDepth) { farDepth = dR; farOff = float2(rt.x, 0.0f); }
		if (dU > farDepth) { farDepth = dU; farOff = float2(0.0f, -rt.y); }
		if (dD > farDepth) { farDepth = dD; farOff = float2(0.0f, rt.y); }
		if (dUL > farDepth) { farDepth = dUL; farOff = float2(-rt.x, -rt.y); }
		if (dUR > farDepth) { farDepth = dUR; farOff = float2(rt.x, -rt.y); }
		if (dDL > farDepth) { farDepth = dDL; farOff = float2(-rt.x, rt.y); }
		if (dDR > farDepth) { farDepth = dDR; farOff = float2(rt.x, rt.y); }

		// The near reference: among the four direct neighbours nearer than the midpoint, the most different in colour.
		float mid = 0.5f * (c + farDepth);
		float3 centreCol = lin;
		float bestDiff = -1.0f;
		float2 nearOff = float2(0.0f, 0.0f);
		float2 offs[4] = { float2(-rt.x, 0.0f), float2(rt.x, 0.0f), float2(0.0f, -rt.y), float2(0.0f, rt.y) };
		float dirDepth[4] = { dL, dR, dU, dD };
		[unroll]
		for (int k = 0; k < 4; k++) {
			float diff = length(LinearSource(clampToRectIfOn(uv + offs[k], rt)) - centreCol);
			if (dirDepth[k] < mid && diff > bestDiff) { bestDiff = diff; nearOff = offs[k]; }
		}
		float3 nearCol = LinearSource(clampToRectIfOn(uv + nearOff, rt));
		float3 farCol = LinearSource(clampToRectIfOn(uv + farOff, rt));
		float3 axis = nearCol - farCol;
		float contrast = length(axis);
		float weightOn = smoothstep(0.0025f, 0.01f, contrast);
		float t = saturate(dot(lin - farCol, axis) / max(dot(axis, axis), 1e-10f));
		float3 lightNear = tex2Dlod(TESR_RenderedBuffer, float4(clampToRectIfOn(uv + nearOff, rt), 0.0f, 0.0f)).rgb;
		float3 lightFar = tex2Dlod(TESR_RenderedBuffer, float4(clampToRectIfOn(uv + farOff, rt), 0.0f, 0.0f)).rgb;
		float3 split = lerp(lightFar, lightNear, t);
		light = lerp(light, split, edge * weightOn);
	}
	float3 added = AddedLight(lin, light);
	if (TESR_VolumetricControl.x > 0.0f) added += BeamAt(uv, readDepthLod(uv)) * TESR_VolumetricControl.x;
	return float4(EncodeOut(lin + added), 1.0f);
}

technique Lit
{
	pass NoShadow
	{
		VertexShader = compile vs_3_0 ScreenVS();
		PixelShader = compile ps_3_0 NoShadowPS();
		ZEnable = FALSE; ZWriteEnable = FALSE; AlphaBlendEnable = FALSE; AlphaTestEnable = FALSE;
		CullMode = NONE; ColorWriteEnable = 15;
	}
	pass Light
	{
		VertexShader = compile vs_3_0 ScreenVS();
		PixelShader = compile ps_3_0 LightPS();
		ZEnable = FALSE; ZWriteEnable = FALSE; AlphaBlendEnable = FALSE; AlphaTestEnable = FALSE;
		CullMode = NONE; ColorWriteEnable = 15;
	}
	pass MinBlur
	{
		VertexShader = compile vs_3_0 ScreenVS();
		PixelShader = compile ps_3_0 MinBlurPS();
		ZEnable = FALSE; ZWriteEnable = FALSE; AlphaBlendEnable = FALSE; AlphaTestEnable = FALSE;
		CullMode = NONE; ColorWriteEnable = 15;
	}
	pass AvgBlur
	{
		VertexShader = compile vs_3_0 ScreenVS();
		PixelShader = compile ps_3_0 AvgBlurPS();
		ZEnable = FALSE; ZWriteEnable = FALSE; AlphaBlendEnable = FALSE; AlphaTestEnable = FALSE;
		CullMode = NONE; ColorWriteEnable = 15;
	}
	pass Composite
	{
		VertexShader = compile vs_3_0 ScreenVS();
		PixelShader = compile ps_3_0 CompositePS();
		ZEnable = FALSE; ZWriteEnable = FALSE; AlphaBlendEnable = FALSE; AlphaTestEnable = FALSE;
		CullMode = NONE; ColorWriteEnable = 15;
	}
}

technique LitShadows
{
	pass ShadowMap
	{
		VertexShader = compile vs_3_0 ScreenVS();
		PixelShader = compile ps_3_0 ShadowMapPS();
		ZEnable = FALSE; ZWriteEnable = FALSE; AlphaBlendEnable = FALSE; AlphaTestEnable = FALSE;
		CullMode = NONE; ColorWriteEnable = 15;
	}
	pass BlurH
	{
		VertexShader = compile vs_3_0 ScreenVS();
		PixelShader = compile ps_3_0 ShadowBlurHPS();
		ZEnable = FALSE; ZWriteEnable = FALSE; AlphaBlendEnable = FALSE; AlphaTestEnable = FALSE;
		CullMode = NONE; ColorWriteEnable = 15;
	}
	pass BlurV
	{
		VertexShader = compile vs_3_0 ScreenVS();
		PixelShader = compile ps_3_0 ShadowBlurVPS();
		ZEnable = FALSE; ZWriteEnable = FALSE; AlphaBlendEnable = FALSE; AlphaTestEnable = FALSE;
		CullMode = NONE; ColorWriteEnable = 15;
	}
	pass Light
	{
		VertexShader = compile vs_3_0 ScreenVS();
		PixelShader = compile ps_3_0 LightPS();
		ZEnable = FALSE; ZWriteEnable = FALSE; AlphaBlendEnable = FALSE; AlphaTestEnable = FALSE;
		CullMode = NONE; ColorWriteEnable = 15;
	}
	pass MinBlur
	{
		VertexShader = compile vs_3_0 ScreenVS();
		PixelShader = compile ps_3_0 MinBlurPS();
		ZEnable = FALSE; ZWriteEnable = FALSE; AlphaBlendEnable = FALSE; AlphaTestEnable = FALSE;
		CullMode = NONE; ColorWriteEnable = 15;
	}
	pass AvgBlur
	{
		VertexShader = compile vs_3_0 ScreenVS();
		PixelShader = compile ps_3_0 AvgBlurPS();
		ZEnable = FALSE; ZWriteEnable = FALSE; AlphaBlendEnable = FALSE; AlphaTestEnable = FALSE;
		CullMode = NONE; ColorWriteEnable = 15;
	}
	pass Composite
	{
		VertexShader = compile vs_3_0 ScreenVS();
		PixelShader = compile ps_3_0 CompositePS();
		ZEnable = FALSE; ZWriteEnable = FALSE; AlphaBlendEnable = FALSE; AlphaTestEnable = FALSE;
		CullMode = NONE; ColorWriteEnable = 15;
	}
}

technique EdgeFix
{
	pass NoShadow
	{
		VertexShader = compile vs_3_0 ScreenVS();
		PixelShader = compile ps_3_0 NoShadowPS();
		ZEnable = FALSE; ZWriteEnable = FALSE; AlphaBlendEnable = FALSE; AlphaTestEnable = FALSE;
		CullMode = NONE; ColorWriteEnable = 15;
	}
	pass Light
	{
		VertexShader = compile vs_3_0 ScreenVS();
		PixelShader = compile ps_3_0 LightPS();
		ZEnable = FALSE; ZWriteEnable = FALSE; AlphaBlendEnable = FALSE; AlphaTestEnable = FALSE;
		CullMode = NONE; ColorWriteEnable = 15;
	}
	pass MinBlur
	{
		VertexShader = compile vs_3_0 ScreenVS();
		PixelShader = compile ps_3_0 MinBlurPS();
		ZEnable = FALSE; ZWriteEnable = FALSE; AlphaBlendEnable = FALSE; AlphaTestEnable = FALSE;
		CullMode = NONE; ColorWriteEnable = 15;
	}
	pass AvgBlur
	{
		VertexShader = compile vs_3_0 ScreenVS();
		PixelShader = compile ps_3_0 AvgBlurPS();
		ZEnable = FALSE; ZWriteEnable = FALSE; AlphaBlendEnable = FALSE; AlphaTestEnable = FALSE;
		CullMode = NONE; ColorWriteEnable = 15;
	}
	pass Composite
	{
		VertexShader = compile vs_3_0 ScreenVS();
		PixelShader = compile ps_3_0 CompositeEdgePS();
		ZEnable = FALSE; ZWriteEnable = FALSE; AlphaBlendEnable = FALSE; AlphaTestEnable = FALSE;
		CullMode = NONE; ColorWriteEnable = 15;
	}
}

technique EdgeFixShadows
{
	pass ShadowMap
	{
		VertexShader = compile vs_3_0 ScreenVS();
		PixelShader = compile ps_3_0 ShadowMapPS();
		ZEnable = FALSE; ZWriteEnable = FALSE; AlphaBlendEnable = FALSE; AlphaTestEnable = FALSE;
		CullMode = NONE; ColorWriteEnable = 15;
	}
	pass BlurH
	{
		VertexShader = compile vs_3_0 ScreenVS();
		PixelShader = compile ps_3_0 ShadowBlurHPS();
		ZEnable = FALSE; ZWriteEnable = FALSE; AlphaBlendEnable = FALSE; AlphaTestEnable = FALSE;
		CullMode = NONE; ColorWriteEnable = 15;
	}
	pass BlurV
	{
		VertexShader = compile vs_3_0 ScreenVS();
		PixelShader = compile ps_3_0 ShadowBlurVPS();
		ZEnable = FALSE; ZWriteEnable = FALSE; AlphaBlendEnable = FALSE; AlphaTestEnable = FALSE;
		CullMode = NONE; ColorWriteEnable = 15;
	}
	pass Light
	{
		VertexShader = compile vs_3_0 ScreenVS();
		PixelShader = compile ps_3_0 LightPS();
		ZEnable = FALSE; ZWriteEnable = FALSE; AlphaBlendEnable = FALSE; AlphaTestEnable = FALSE;
		CullMode = NONE; ColorWriteEnable = 15;
	}
	pass MinBlur
	{
		VertexShader = compile vs_3_0 ScreenVS();
		PixelShader = compile ps_3_0 MinBlurPS();
		ZEnable = FALSE; ZWriteEnable = FALSE; AlphaBlendEnable = FALSE; AlphaTestEnable = FALSE;
		CullMode = NONE; ColorWriteEnable = 15;
	}
	pass AvgBlur
	{
		VertexShader = compile vs_3_0 ScreenVS();
		PixelShader = compile ps_3_0 AvgBlurPS();
		ZEnable = FALSE; ZWriteEnable = FALSE; AlphaBlendEnable = FALSE; AlphaTestEnable = FALSE;
		CullMode = NONE; ColorWriteEnable = 15;
	}
	pass Composite
	{
		VertexShader = compile vs_3_0 ScreenVS();
		PixelShader = compile ps_3_0 CompositeEdgePS();
		ZEnable = FALSE; ZWriteEnable = FALSE; AlphaBlendEnable = FALSE; AlphaTestEnable = FALSE;
		CullMode = NONE; ColorWriteEnable = 15;
	}
}
