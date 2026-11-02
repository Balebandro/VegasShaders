// GodRays: soft light shafts from the sun's screen position, from bright sky only (rewritten 2026-11-02, wave 3, from
// src/effects/GodRays/README.md). Techniques, as GodRays.cpp asks for them:
//   DedicatedGodRays   SkyMask, LightMask, Blur1, Blur2, Blur3, Combine     (6 passes, the main path)
//   GodRaysMaxReduce   Reduce     the 8 x 8 maximum of the light mask (SkipEmpty, SkipIdle check)
//   GodRaysIdleTest    Test       discards unless the sky maximum reaches the limit (SkipIdle; NaN counts as bright)
//   (unnamed)          the same six passes as DedicatedGodRays (the spec's packed legacy path is not reproduced: fallback only)
// Registers are explicit in the spec (s0 scene, s1 depth, s5 ray buffer, s6 maximum); the C++ binds the ping-pong to s5.
// Open: the rect-frame behaviour and the aspect factor of the blur direction (TESR_ReciprocalResolution.z) are not used here.

#include "Includes/Helpers.hlsl"

// ---- samplers (explicit slots, as the spec's section 3a) ----
sampler2D TESR_RenderedBuffer : register(s0) = sampler_state { MinFilter = LINEAR; MagFilter = LINEAR; MipFilter = NONE; AddressU = CLAMP; AddressV = CLAMP; };
sampler2D TESR_DepthBuffer : register(s1) = sampler_state { MinFilter = LINEAR; MagFilter = LINEAR; MipFilter = NONE; AddressU = CLAMP; AddressV = CLAMP; };
sampler2D NVR_GodRaysBuffer : register(s5) = sampler_state { MinFilter = LINEAR; MagFilter = LINEAR; MipFilter = NONE; AddressU = CLAMP; AddressV = CLAMP; };
sampler2D NVR_GodRaysMax : register(s6) = sampler_state { MinFilter = POINT; MagFilter = POINT; MipFilter = NONE; AddressU = CLAMP; AddressV = CLAMP; };

// ---- constants (registered by GodRaysEffect) ----
float4 TESR_SunColor;               // rgb sun colour (linearised here)
float4 TESR_GodRaysRay;             // x RayIntensity, y RayLength, w SunsetGlareFix (1 on)
float4 TESR_GodRaysRayColor;        // rgb custom ray colour, w Saturate (0..1)
float4 TESR_GodRaysData;            // y Luminance threshold, z the per-frame day/night strength, w TimeEnabled
float4 TESR_SunPosition;            // xyz sun disc position; z = its elevation (0 at the horizon, 1 at the zenith)
float4 TESR_SunDirection;           // xyz sun direction for the ray colour (its z is the elevation used there)
float4 TESR_SunAmount;              // x day factor (0 night .. 1 day)
float4 TESR_SunsetColor;            // rgb sunset colour boost
float4 TESR_ViewSpaceLightDir;      // view-space sun direction (not necessarily unit length)
float4 NVR_GodRaysSkip;             // x SkipEmpty reduction ran, y SkipIdle on, z SkipIdle limit
float4 NVR_GodRaysReduce;           // xy 1 / source size of the reduction step
float4 NVR_GodRaysLayout;           // xy ray buffer / full extent, zw 1 / ray buffer size

// Depth.hlsl: readDepth, reconstructPosition, projectPosition, TESR_CameraData, the transforms (the depth sampler must precede it).
#include "Includes/Depth.hlsl"

// ---- vertex shader: the frame quad (ShaderManager::CreateFrameVertex) ----
struct VSIN { float3 pos : POSITION; float2 uv : TEXCOORD0; };
struct VSOUT { float4 pos : POSITION; float2 uv : TEXCOORD0; };
VSOUT FrameVS(VSIN IN) {
	VSOUT o;
	o.pos = float4(IN.pos, 1.0f);
	o.uv = IN.uv;
	return o;
}

// ---- sun and view helpers ----
float GRSunH() { return saturate(TESR_SunPosition.z); }                          // 1 at the zenith, 0 at the horizon
float GRSunset() { return pow(saturate(1.0f - TESR_SunPosition.z), 8.0f); }      // 1 at the horizon, near 0 with the sun high
float3 GRViewDir(float2 uv) { return normalize(reconstructPosition(uv)); }       // the pixel's view direction
float3 GRSunView() { return TESR_ViewSpaceLightDir.xyz; }

// The sky test: 1 for sky pixels (linear depth above 0.9), 0 for the world.
float GRSky(float2 uv) { return step(0.9f, tex2D(TESR_DepthBuffer, uv).x); }

// The sun's projected screen position (the view-space sun direction times the far distance).
float2 GRSunScreen() { return projectPosition(GRSunView() * TESR_CameraData.y).xy; }

// ---- pass 0: SkyMask. The scene plus the sun glare, in sky pixels only (zero on the world) ----
float4 PS_SkyMask(float2 uv : TEXCOORD0) : COLOR0 {
	float3 scene = linearize(tex2D(TESR_RenderedBuffer, uv).rgb);
	float sunset = GRSunset();
	float fix = step(0.5f, TESR_GodRaysRay.w);
	float boostW = lerp(1.0f - sunset, sunset, fix);                             // SunsetGlareFix: the boost near the horizon only
	float3 glareCol = linearize(TESR_SunColor.rgb) + linearize(TESR_SunsetColor.rgb) * boostW;
	float glarePow = lerp(0.1f, 8.0f, sunset);
	float glare = pow(saturate(dot(normalize(GRSunView()), GRViewDir(uv))), 180.0f) * glarePow;
	float3 lin = scene + glare * glareCol;
	return float4(lin * GRSky(uv), 1.0f);
}

// ---- pass 1: LightMask. Five texel-centred taps (the pixel and its diagonal neighbours one half texel away), averaged; keeps the
// bright sky above a threshold that falls to 0 at the horizon, ramped over 15 x Luminance, scaled by 100 and RayIntensity ----
float4 PS_LightMask(float2 uv : TEXCOORD0) : COLOR0 {
	float2 t = NVR_GodRaysLayout.zw;
	float3 avg = (tex2D(NVR_GodRaysBuffer, uv).rgb
	            + tex2D(NVR_GodRaysBuffer, uv + float2(t.x, t.y)).rgb
	            + tex2D(NVR_GodRaysBuffer, uv + float2(-t.x, t.y)).rgb
	            + tex2D(NVR_GodRaysBuffer, uv + float2(t.x, -t.y)).rgb
	            + tex2D(NVR_GodRaysBuffer, uv + float2(-t.x, -t.y)).rgb) * 0.2f;
	float thr = 2.0f * pow(GRSunH(), 8.0f);
	float ramp = saturate((luma(avg) - thr) / max(15.0f * TESR_GodRaysData.y, 1e-4f));
	float keep = saturate(ramp * 100.0f * TESR_GodRaysRay.x);
	return float4(avg * keep, 1.0f);
}

// ---- passes 2 to 4: the radial blurs. 10 taps from the pixel toward the sun's screen position, step times RayLength; a tap counts
// when it is within the distance to the centre and inside the screen; the sum over one more than the count. Blur3 keeps the ray
// attenuation in alpha. ----
float4 GRBlur(float2 uv, float stepF, float store) {
	float2 centre = GRSunScreen();
	float2 dir = centre - uv;
	float reach = length(dir);
	float2 stepUV = dir * stepF * max(TESR_GodRaysRay.y, 0.0f);
	float3 sum = 0.0f;
	float count = 0.0f;
	[unroll] for (int i = 0; i < 10; i++) {
		float2 tuv = uv + stepUV * i;
		float along = length(stepUV) * i;
		float ok = step(along, reach) * step(0.0f, tuv.x) * step(tuv.x, 1.0f) * step(0.0f, tuv.y) * step(tuv.y, 1.0f);
		sum += ok * tex2D(NVR_GodRaysBuffer, tuv).rgb;
		count += ok;
	}
	float3 rays = sum / (count + 1.0f);
	// Ray attenuation: the sun against the view (0.5 to 1, squared-ish), times the time factor, zero at or below the horizon.
	float facing = max(dot(normalize(GRSunView()), GRViewDir(uv)), 0.0f);
	float att = pow(0.5f + 0.5f * facing, 2.5f);
	float timeF = lerp(1.0f, lerp(0.2f, 4.0f, pow(GRSunH(), 4.0f)), step(0.5f, TESR_GodRaysData.w));
	att *= timeF * step(1e-4f, TESR_SunPosition.z);
	float alpha = lerp(1.0f, att, store);
	return float4(rays, alpha);
}

float4 PS_Blur1(float2 uv : TEXCOORD0) : COLOR0 { return GRBlur(uv, 0.1f, 0.0f); }
float4 PS_Blur2(float2 uv : TEXCOORD0) : COLOR0 { return GRBlur(uv, 0.01f, 0.0f); }
float4 PS_Blur3(float2 uv : TEXCOORD0) : COLOR0 { return GRBlur(uv, 0.001f, 1.0f); }

// ---- pass 5: Combine. Linear scene times (1 + 5 rays) plus 0.2 rays; the ray colour is the sun colour (brightened with elevation,
// plus the sunset boost by the day factor) or the custom colour, by Saturate; the rays carry the strength and the Blur3 attenuation.
// Where the rays are exactly zero and SkipIdle is on, the scene is returned as stored. ----
float4 PS_Combine(float2 uv : TEXCOORD0) : COLOR0 {
	float4 stored = tex2D(TESR_RenderedBuffer, uv);
	float4 r = tex2D(NVR_GodRaysBuffer, uv);
	float3 sunCol = linearize(TESR_SunColor.rgb) * (1.0f + saturate(TESR_SunDirection.z)) + linearize(TESR_SunsetColor.rgb) * GRSunset() * TESR_SunAmount.x;
	float3 rayCol = lerp(sunCol, linearize(TESR_GodRaysRayColor.rgb), saturate(TESR_GodRaysRayColor.w));
	float3 rays = max(r.rgb, 0.0f) * rayCol * TESR_GodRaysData.z * r.a;
	float3 scene = linearize(stored.rgb);
	float3 lin = scene * (1.0f + 5.0f * rays) + 0.2f * rays;
	float zero = step(max(max(r.r, r.g), r.b), 0.0f) * step(0.5f, NVR_GodRaysSkip.y);
	float3 outc = lerp(delinearize(max(lin, 0.0f)), stored.rgb, zero);
	return float4(outc, stored.a);
}

// ---- reduction: the 8 x 8 maximum of the three colour channels, from the step's source ----
float4 PS_Reduce(float2 uv : TEXCOORD0) : COLOR0 {
	float2 t = NVR_GodRaysReduce.xy;
	float m = 0.0f;
	[unroll] for (int j = 0; j < 8; j++) {
		[unroll] for (int i = 0; i < 8; i++) {
			float2 o = float2(i - 3.5f, j - 3.5f) * t;
			float3 c = tex2D(NVR_GodRaysBuffer, uv + o).rgb;
			m = max(m, max(c.r, max(c.g, c.b)));
		}
	}
	return float4(m, m, m, 1.0f);
}

// ---- the idle test: discard unless the sky maximum reaches the limit (NaN is not discarded) ----
float4 PS_IdleTest(float2 uv : TEXCOORD0) : COLOR0 {
	float v = tex2D(NVR_GodRaysMax, uv).r;
	clip(v - NVR_GodRaysSkip.z);
	return float4(0.0f, 0.0f, 0.0f, 0.0f);
}

// ---- techniques ----
technique
{
	pass SkyMask { VertexShader = compile vs_3_0 FrameVS(); PixelShader = compile ps_3_0 PS_SkyMask(); }
	pass LightMask { VertexShader = compile vs_3_0 FrameVS(); PixelShader = compile ps_3_0 PS_LightMask(); }
	pass Blur1 { VertexShader = compile vs_3_0 FrameVS(); PixelShader = compile ps_3_0 PS_Blur1(); }
	pass Blur2 { VertexShader = compile vs_3_0 FrameVS(); PixelShader = compile ps_3_0 PS_Blur2(); }
	pass Blur3 { VertexShader = compile vs_3_0 FrameVS(); PixelShader = compile ps_3_0 PS_Blur3(); }
	pass Combine { VertexShader = compile vs_3_0 FrameVS(); PixelShader = compile ps_3_0 PS_Combine(); }
}

technique DedicatedGodRays
{
	pass SkyMask { VertexShader = compile vs_3_0 FrameVS(); PixelShader = compile ps_3_0 PS_SkyMask(); }
	pass LightMask { VertexShader = compile vs_3_0 FrameVS(); PixelShader = compile ps_3_0 PS_LightMask(); }
	pass Blur1 { VertexShader = compile vs_3_0 FrameVS(); PixelShader = compile ps_3_0 PS_Blur1(); }
	pass Blur2 { VertexShader = compile vs_3_0 FrameVS(); PixelShader = compile ps_3_0 PS_Blur2(); }
	pass Blur3 { VertexShader = compile vs_3_0 FrameVS(); PixelShader = compile ps_3_0 PS_Blur3(); }
	pass Combine { VertexShader = compile vs_3_0 FrameVS(); PixelShader = compile ps_3_0 PS_Combine(); }
}

technique GodRaysMaxReduce
{
	pass Reduce { VertexShader = compile vs_3_0 FrameVS(); PixelShader = compile ps_3_0 PS_Reduce(); }
}

technique GodRaysIdleTest
{
	pass Test { VertexShader = compile vs_3_0 FrameVS(); PixelShader = compile ps_3_0 PS_IdleTest(); }
}
