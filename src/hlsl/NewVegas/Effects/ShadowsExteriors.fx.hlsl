// ShadowsExteriors: the final apply of the outdoor sun-shadow term (rewritten 2026-11-02, wave 3, from
// src/effects/ShadowsExteriors/README.md). One unnamed technique, one pass, pixel shader Shadow, as
// ShadowsExterior.cpp requests it (technique index 0). Multiplies the frame by the sun-shadow factor (red of the shadow buffer,
// with the lamp term in green), with shadows tinted toward the sky colour, after the SunFar factor.
// Open: the Vulkan pass (Compute/ShadowsExteriors.frag) is not written in this wave; the sun-producer's sky value is assumed.

#include "Includes/Helpers.hlsl"

// ---- samplers: declaration order is the binding order ----
sampler2D TESR_RenderedBuffer : register(s0) = sampler_state { MinFilter = LINEAR; MagFilter = LINEAR; MipFilter = NONE; AddressU = CLAMP; AddressV = CLAMP; };
sampler2D TESR_DepthBuffer : register(s1) = sampler_state { MinFilter = LINEAR; MagFilter = LINEAR; MipFilter = NONE; AddressU = CLAMP; AddressV = CLAMP; };
sampler2D TESR_PointShadowBuffer : register(s2) = sampler_state { MinFilter = LINEAR; MagFilter = LINEAR; MipFilter = NONE; AddressU = CLAMP; AddressV = CLAMP; };
sampler2D TESR_NormalsBuffer : register(s3) = sampler_state { MinFilter = LINEAR; MagFilter = LINEAR; MipFilter = NONE; AddressU = CLAMP; AddressV = CLAMP; };
sampler2D TESR_SunFarBuffer : register(s4) = sampler_state { MinFilter = POINT; MagFilter = POINT; MipFilter = NONE; AddressU = CLAMP; AddressV = CLAMP; };

// ---- constants (registered by ShadowsExteriorEffect) ----
float4 TESR_WaterSettings;      // x water height, z 1 when the camera is underwater
float4 TESR_ShadowData;         // y Darkness (the darkness is 1 - y at the shadow floor)
float4 TESR_ShadowFade;         // x sun fade (1 = no shadow), z point-light shadows on
float4 TESR_SkyColor;           // rgb sky colour (gamma encoded)
float4 TESR_SunAmbient;         // rgb sun ambient colour (only the luminance is used)
float4 TESR_SunFarApply;        // x 1 when the SunFar factor was written this frame

// Depth.hlsl: readDepth, reconstructWorldPosition, TESR_CameraData, TESR_CameraPosition, the transforms. Normals.hlsl: GetNormal.
#include "Includes/Depth.hlsl"
#include "Includes/Normals.hlsl"

// ---- vertex shader: the frame quad (ShaderManager::CreateFrameVertex) ----
struct VSIN { float3 pos : POSITION; float2 uv : TEXCOORD0; };
struct VSOUT { float4 pos : POSITION; float2 uv : TEXCOORD0; };
VSOUT FrameVS(VSIN IN) {
	VSOUT o;
	o.pos = float4(IN.pos, 1.0f);
	o.uv = IN.uv;
	return o;
}

// Shadow (the one pass).
float4 Shadow(float2 uv : TEXCOORD0) : COLOR0 {
	float3 frame = tex2D(TESR_RenderedBuffer, uv).rgb;

	// SunFar factor first: a per-channel multiply, a lerp from 1 by SunFarApply.x.
	float3 sunFar = tex2D(TESR_SunFarBuffer, uv).rgb;
	frame *= lerp(float3(1, 1, 1), sunFar, saturate(TESR_SunFarApply.x));

	// Sun factor: red of the shadow buffer, faded toward 1 by the sun fade; green is the point-light term.
	float2 sb = tex2D(TESR_PointShadowBuffer, uv).rg;
	float sun = lerp(sb.r, 1.0f, saturate(TESR_ShadowFade.x));
	float floorLevel = max(0.0f, 1.0f - TESR_ShadowData.y);       // the darkness floor
	float lamp = sb.g * saturate(TESR_ShadowFade.z);
	float lit = saturate(sun + lamp);
	float f = lerp(floorLevel, 1.0f, lit);                        // from the floor (no sun, no lamp) to 1 (full sun)

	// Water exception (underwater only): the water plane keeps its light. The surface is the one whose world normal points
	// along -Z (as the spec states), at the water height.
	float underwater = step(0.5f, TESR_WaterSettings.z);
	float dN = readDepth(uv);
	float3 rel = reconstructWorldPosition(uv, dN).xyz;
	float3 worldN = normalize(GetNormal(uv));
	float h = TESR_CameraPosition.z + rel.z;
	float onWater = step(abs(h - TESR_WaterSettings.x), 2.0f) * step(0.999f, dot(worldN, float3(0, 0, -1)));
	f = lerp(f, 1.0f, underwater * onWater);

	// Shadow colour: linearised (gamma 2.2); a tint toward the sky (luminance x factor x sky) and a plain darkening (frame x
	// factor), blended by saturate(factor + 0.5) so that as the factor falls the result moves toward the tint.
	float3 lin = pow(max(frame, 0.0f), 2.2f);
	float3 skyLin = pow(max(TESR_SkyColor.rgb, 0.0f), 2.2f);
	float3 tint = luma(lin) * f * skyLin;
	float3 plain = lin * f;
	float w = saturate(f + 0.5f);
	float3 col = lerp(tint, plain, w);
	return float4(pow(max(col, 0.0f), 1.0f / 2.2f), 1.0f);
}

technique
{
	pass Shadow
	{
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 Shadow();
	}
}
