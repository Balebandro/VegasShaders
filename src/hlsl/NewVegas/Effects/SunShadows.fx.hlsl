// SunShadows (wave 3 rewrite, 2026-11-02): the outdoor sun's shadow term, written into the two-channel shadow buffer
// TESR_PointShadowBuffer (G16R16). Red: sun visibility (1 = lit), made of the screen-space contact shadows and, where the deferred
// path owns them, the sun cascade maps and the character shadow map. Green: the lamps' term the PointShadows passes wrote before
// this effect; carried through (blurred with red and raised to Intensity, as ShadowsExteriorEffect::FadedContactUnread describes).
//
// Driven by effects/SunShadows.cpp (ping-pong: buffer -> scratch 0 -> scratch 1 -> ... -> buffer; each pass reads its source on
// stage 3, TESR_PointShadowBuffer, never its own target) and core/ShaderManager.cpp (technique choice):
//   0  (no name)                  ContactMarch, BlurAcross, BlurDown, ApplySunMaps       ForwardShadows off, or RT sun Mode 1
//   1  ForwardContactShadows      ContactMarch, BlurAcross, BlurDownFinal                the object shaders do the cascades
//   2  ForwardContactSharp        SharpMarch, SharpFinish                                as 1 with [ScreenSpace] Sharp
//   3  ContactFast                as 0, the march's skips as real branches               [ScreenSpace] FastPaths
//   4  ForwardContactShadowsFast  as 1, likewise                                         [ScreenSpace] FastPaths
// The cascade lookup follows Shaders/Includes/Shadow.hlsl GetSunShadow (PCF, Mode 3) so the deferred and forward shadows agree:
// same atlas quadrants, quadratic B-spline 3 x 3 (SmoothFilter: cubic 4 x 4), 1.5-texel normal offset, receiver-plane slack,
// HandoverBlend bands with both cascades read at one point, and CharacterMapShadow for the player's own map.
// The sun fade (TESR_ShadowFade.x) and Darkness are the exterior apply's (ShadowsExteriors.fx.hlsl), not applied here.
// Compile macros (EffectRecord::BuildMacros): FORWARD_SHADOWS 0/1, CHARACTER_SHADOW_MAP (defined 1 when on at startup).
// Vulkan twin: Compute/SunShadows.glsl and SunShadows{March,Blur,Apply,Sharp,SharpFinish}.frag.

#ifndef FORWARD_SHADOWS
	#define FORWARD_SHADOWS 0
#endif
#ifndef CHARACTER_SHADOW_MAP
	#define CHARACTER_SHADOW_MAP 0
#endif

// ---- samplers: EffectRecord binds stage N to the N-th declared sampler and parses "register ( sN )", so the
// ---- declaration order and the register numbers must agree; all seven are declared whatever the macros say.
sampler2D TESR_DepthBuffer : register(s0) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
sampler2D TESR_ShadowAtlas : register(s1) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
sampler2D TESR_NormalsBuffer : register(s2) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
sampler2D TESR_PointShadowBuffer : register(s3) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
sampler2D TESR_NoiseSampler : register(s4) < string ResourceName = "Effects\bluenoise256.dds"; > = sampler_state { ADDRESSU = WRAP; ADDRESSV = WRAP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_CharShadowMap : register(s5) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
sampler2D TESR_BackDepthBuffer : register(s6) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };

// ---- constants (ShadowsExteriorEffect::RegisterConstants and ShaderManager's table; bound by name) ----
float4 TESR_ReciprocalResolution;    // xy: 1 / the buffers' size (one texel in texture coordinates)
float4 TESR_ViewSpaceLightDir;       // xyz: toward the sun, view space (normalised here)
float4 TESR_SmoothedSunDir;          // xyz: toward the (smoothed, quantised) sun the cascades were drawn for, world space
float4 TESR_ShadowScreenSpaceData;   // x: contact shadows on, y: BlurRadius, z: RenderDistance, w: Intensity (exponent)
float4 TESR_ShadowScreenSpaceFade;   // x: NearFade start (view depth), y: NearFade end; y = 0: no near fade
float4 TESR_ShadowFade;              // y: 1 = the sun maps are drawn ([Main] Enabled)
float4 TESR_ShadowFormatData;        // x: map mode (3 = PCF, the only one drawn), z: 1 outdoors
float4 TESR_ShadowForwardData;       // x: 1 = the object shaders' lookups are OFF (this effect does them), w: HandoverBlend (world units)
float4 TESR_ShadowBlur;              // x: 1 / atlas size, z: 1 = SmoothFilter
float4 TESR_RtSunReplace;            // x: RT sun range (view depth) while Mode 1 replaces the cascades, 0 otherwise
float4x4 TESR_ShadowCameraToLightTransformNear;     // camera-relative world -> each cascade's light clip space (ortho)
float4x4 TESR_ShadowCameraToLightTransformMiddle;
float4x4 TESR_ShadowCameraToLightTransformFar;
float4x4 TESR_ShadowCameraToLightTransformLod;
float4 TESR_ShadowNearCenter;        // xyz: centre (camera-relative world), w: radius
float4 TESR_ShadowMiddleCenter;
float4 TESR_ShadowFarCenter;
float4 TESR_ShadowLodCenter;
#if CHARACTER_SHADOW_MAP
float4x4 TESR_CharShadowCameraToLight;   // camera-relative world -> the character map's clip space (ortho)
float4 TESR_CharShadowCenter;            // xyz: box centre (camera-relative), w: half-extent (world units)
float4 TESR_CharShadowData;              // x: 1 / resolution, y: hand-over band (share of the half-extent), z: depth bias, w: 1 + debug view (0 = not drawn)
#endif

#include "Includes/Depth.hlsl"      // TESR_ProjectionTransform, TESR_ViewTransform, TESR_InvViewTransform, TESR_CameraData, RenderRect
#include "Includes/BackDepth.hlsl"  // BackViewZ, TESR_BackDepthData

// ---- the frame quad ----
struct FrameVertex {
	float4 position : POSITION;
	float2 uv : TEXCOORD0;
};
FrameVertex FrameVS(FrameVertex IN) { return IN; }

// ---------------------------------------------------------------------------------------------------------------------------
// Screen and view helpers. Positions come from the stored linear depth and the projection's own terms (_11, _22, _31, _32), so
// they do not depend on the depth buffer's direction (REVERSED_DEPTH) or the inverse projection. uv is a texture ("rect")
// coordinate; the NDC maths takes the screen position it shows (RenderRect.hlsl).
// ---------------------------------------------------------------------------------------------------------------------------
static const float SkyShare = 0.9999f;          // stored depth (share of far) at and past which a pixel is sky
static const float RayThickness = 20.0f;        // world units an occluder is taken to be thick when BackDepth knows nothing

float FarDistance() { return max(TESR_CameraData.y, 1.0f); }
float RenderDistance() { return max(TESR_ShadowScreenSpaceData.z, 1.0f); }
float ContactIntensity() { return max(TESR_ShadowScreenSpaceData.w, 0.0f); }

float ViewDepthAt(float2 uv) { return tex2Dlod(TESR_DepthBuffer, float4(uv, 0.0f, 0.0f)).x * TESR_CameraData.y; }

// the texel centre under uv: a LINEAR depth read there is the texel's own depth, not a blend across a silhouette
float2 SnapToTexel(float2 uv) {
	float2 texel = TESR_ReciprocalResolution.xy;
	return (floor(uv / texel) + 0.5f) * texel;
}

float3 ViewPositionAt(float2 uv, float viewZ) {
	float2 s01 = rectToScreen(uv);
	float2 ndc = float2(s01.x * 2.0f - 1.0f, 1.0f - s01.y * 2.0f);
	return float3((ndc.x - TESR_ProjectionTransform[2][0]) * viewZ / TESR_ProjectionTransform[0][0],
	              (ndc.y - TESR_ProjectionTransform[2][1]) * viewZ / TESR_ProjectionTransform[1][1], viewZ);
}

float2 TexCoordOf(float3 viewPos) {
	float invZ = 1.0f / max(viewPos.z, 1.0e-3f);
	float2 ndc = float2(viewPos.x * TESR_ProjectionTransform[0][0] * invZ + TESR_ProjectionTransform[2][0],
	                    viewPos.y * TESR_ProjectionTransform[1][1] * invZ + TESR_ProjectionTransform[2][1]);
	return screenToRect(float2(ndc.x * 0.5f + 0.5f, 0.5f - ndc.y * 0.5f));
}

// the stored normal (view space, 0..1 encoded), unit length; facing the camera where the buffer holds nothing usable
float3 ViewNormalAt(float2 uv) {
	float3 n = tex2Dlod(TESR_NormalsBuffer, float4(uv, 0.0f, 0.0f)).xyz * 2.0f - 1.0f;
	float len2 = dot(n, n);
	return len2 > 1.0e-4f ? n * rsqrt(len2) : float3(0.0f, 0.0f, -1.0f);
}

// view space -> camera-relative world (the space the cascade and character matrices take, as Shadow.hlsl GetShadowWorldPos)
float3 WorldPositionOf(float3 viewPos) { return mul(float4(viewPos, 1.0f), TESR_InvViewTransform).xyz; }
float3 WorldDirectionOf(float3 viewDir) { return mul(TESR_ViewTransform, float4(viewDir, 0.0f)).xyz; }

// ---------------------------------------------------------------------------------------------------------------------------
// Contact shadows (screen-space march toward the sun)
// ---------------------------------------------------------------------------------------------------------------------------

// 0 nearer than NearFade / 2, 1 from NearFade on (TESR_ShadowScreenSpaceFade); 1 everywhere when NearFade is 0
float NearFadeWeight(float viewZ) {
	float start = TESR_ShadowScreenSpaceFade.x;
	float end = max(TESR_ShadowScreenSpaceFade.y, start + 1.0e-3f);
	return TESR_ShadowScreenSpaceFade.y > 0.0f ? smoothstep(start, end, viewZ) : 1.0f;
}

// how strongly the surface faces the sun for the contact term: 0 turned away, 1 from about 7 degrees above grazing
float SunFacing(float3 viewNormal, float3 viewSun) { return saturate(dot(viewNormal, viewSun) * 8.0f); }

// One sample of the ray against the depth buffer: 1 where the surface seen there lies in front of the ray point by more than
// the bias and less than the occluder's thickness (BackDepth's back face when it drew one there, never more than 20 units).
// The two edges are short ramps instead of steps, so a sub-pixel change (TAA jitter, a slow pan) never flips a sample whole.
float RayBlocked(float2 sampleUV, float rayZ) {
	[branch] if (outsideRect(sampleUV) || rayZ <= TESR_CameraData.x) return 0.0f;
	float2 centre = SnapToTexel(sampleUV);
	float sceneZ = ViewDepthAt(centre);
	float back = BackViewZ(centre);
	float thickness = back > sceneZ ? min(RayThickness, back - sceneZ) : RayThickness;
	float bias = max(0.01f, rayZ * 0.002f);
	float gap = rayZ - sceneZ;
	return saturate((gap - bias) / bias) * saturate((thickness - gap) / (0.25f * thickness + 0.01f));
}

// occlusion share (0..1) -> contact factor: lifted by ^0.3, scaled by the facing and NearFade, faded to 1 over the last fifth of
// RenderDistance
float ContactFromOcclusion(float occlusion, float facing, float viewZ) {
	float shade = pow(saturate(occlusion), 0.3f) * facing * NearFadeWeight(viewZ);
	return lerp(1.0f - shade, 1.0f, smoothstep(0.8f * RenderDistance(), RenderDistance(), viewZ));
}

// the length of one march step: 2000 / 5 units times the depth's share of the far distance to the power 0.6 (rays grow slower
// than the distance, so they get shorter on screen far away)
float MarchStep(float viewZ) { return 400.0f * pow(saturate(viewZ / FarDistance()), 0.6f); }

// Four samples along the sun ray, step lengths scaled per pixel by blue noise (between min(0.8, depth / RenderDistance) and 1:
// near the camera the whole ray is spread out, far away only its end), nearer samples counting more (1 / step).
float ContactMarch(float2 uv, float3 viewPos, float3 viewSun, float facing) {
	float viewZ = viewPos.z;
	float noise = tex2Dlod(TESR_NoiseSampler, float4(uv / (TESR_ReciprocalResolution.xy * 256.0f), 0.0f, 0.0f)).r;
	float stepLength = MarchStep(viewZ) * lerp(min(0.8f, viewZ / RenderDistance()), 1.0f, noise);
	float occlusion = 0.0f, weights = 0.0f;
	[unroll] for (int i = 1; i <= 4; i++) {
		float3 rayPoint = viewPos + viewSun * (stepLength * i);
		float weight = 1.0f / i;
		occlusion += weight * RayBlocked(TexCoordOf(rayPoint), rayPoint.z);
		weights += weight;
	}
	return ContactFromOcclusion(occlusion / weights, facing, viewZ);
}

// true where no contact shadow can be: contacts off, past RenderDistance, sky, or turned away from the sun
bool NoContact(float viewZ, float facing) {
	return TESR_ShadowScreenSpaceData.x < 0.5f || viewZ >= RenderDistance() || viewZ >= SkyShare * TESR_CameraData.y || facing <= 0.0f;
}

// Pass 1 of techniques 0, 1, 3, 4. fast: the skips are real branches (FastPaths); otherwise the march runs everywhere and the
// skip is a select. The same picture either way.
float4 ContactMarchPS(float2 uv : TEXCOORD0, uniform bool fast) : COLOR0 {
	float lamps = tex2Dlod(TESR_PointShadowBuffer, float4(uv, 0.0f, 0.0f)).g;
	float viewZ = ViewDepthAt(uv);
	float3 viewPos = ViewPositionAt(uv, viewZ);
	float3 viewSun = normalize(TESR_ViewSpaceLightDir.xyz);
	float facing = SunFacing(ViewNormalAt(uv), viewSun);
	float contact = 1.0f;
	if (fast) {
		[branch] if (!NoContact(viewZ, facing)) contact = ContactMarch(uv, viewPos, viewSun, facing);
	}
	else {
		contact = ContactMarch(uv, viewPos, viewSun, facing);
		contact = NoContact(viewZ, facing) ? 1.0f : contact;
	}
	return float4(contact, lamps, 0.0f, 1.0f);
}

// Pass 1 of technique 2: one straight line from the pixel toward the sun (2 x 2000 units times the depth's share of far to the
// power 0.6, pulled back to stay in front of the near plane), 16 even steps on screen with the ray depth interpolated in 1 / z
// (perspective-correct), nearer steps counting more (1 / (1 + 4t)). No noise: the finish pass smooths the steps.
float4 SharpMarchPS(float2 uv : TEXCOORD0) : COLOR0 {
	float lamps = tex2Dlod(TESR_PointShadowBuffer, float4(uv, 0.0f, 0.0f)).g;
	float viewZ = ViewDepthAt(uv);
	float3 viewPos = ViewPositionAt(uv, viewZ);
	float3 viewSun = normalize(TESR_ViewSpaceLightDir.xyz);
	float facing = SunFacing(ViewNormalAt(uv), viewSun);
	float contact = 1.0f;
	[branch] if (!NoContact(viewZ, facing)) {
		float nearPlane = max(TESR_CameraData.x, 1.0e-3f);
		float3 rayEnd = viewPos + viewSun * (2.0f * 2000.0f * pow(saturate(viewZ / FarDistance()), 0.6f));
		float keep = rayEnd.z < nearPlane ? saturate((viewPos.z - nearPlane) / max(viewPos.z - rayEnd.z, 1.0e-4f)) : 1.0f;
		rayEnd = lerp(viewPos, rayEnd, keep);
		float2 endUV = TexCoordOf(rayEnd);
		float invStart = 1.0f / max(viewPos.z, nearPlane);
		float invEnd = 1.0f / max(rayEnd.z, nearPlane);
		float occlusion = 0.0f, weights = 0.0f;
		[loop] for (int k = 1; k <= 16; k++) {
			float t = k / 16.0f;
			float weight = 1.0f / (1.0f + 4.0f * t);
			occlusion += weight * RayBlocked(lerp(uv, endUV, t), 1.0f / lerp(invStart, invEnd, t));
			weights += weight;
		}
		contact = ContactFromOcclusion(occlusion / weights, facing, viewZ);
	}
	return float4(contact, lamps, 0.0f, 1.0f);
}

// Pass 2 of technique 2: the centre's red (counted twice) with its four direct neighbours whose depth is within 2 % (at least
// one unit) of the centre's, then both channels raised to Intensity, as every technique's last pass does.
float4 SharpFinishPS(float2 uv : TEXCOORD0) : COLOR0 {
	float4 centre = tex2Dlod(TESR_PointShadowBuffer, float4(uv, 0.0f, 0.0f));
	float centreZ = ViewDepthAt(uv);
	float tolerance = max(0.02f * centreZ, 1.0f);
	float2 texel = TESR_ReciprocalResolution.xy;
	float2 offsets[4] = { float2(texel.x, 0.0f), float2(-texel.x, 0.0f), float2(0.0f, texel.y), float2(0.0f, -texel.y) };
	float sum = 2.0f * centre.r, weights = 2.0f;
	[unroll] for (int i = 0; i < 4; i++) {
		float2 tapUV = clampToRectIfOn(uv + offsets[i], texel);
		float agree = abs(ViewDepthAt(tapUV) - centreZ) <= tolerance ? 1.0f : 0.0f;
		sum += agree * tex2Dlod(TESR_PointShadowBuffer, float4(tapUV, 0.0f, 0.0f)).r;
		weights += agree;
	}
	float2 result = pow(max(float2(sum / weights, centre.g), 1.0e-6f), ContactIntensity());
	return float4(result, 0.0f, 1.0f);
}

// ---------------------------------------------------------------------------------------------------------------------------
// Depth-aware blur: 12 taps (six each side, BlurRadius texels apart) with Gaussian weights; a tap counts by how close it lies to
// the centre's surface plane (its distance along the centre's normal, against 1.2 % of the depth, at least one unit), so contact
// shadows do not bleed across silhouettes and grazing ground still blurs along itself. Past RenderDistance and on the sky the
// centre is kept as it is.
// ---------------------------------------------------------------------------------------------------------------------------
float4 DepthAwareBlur(float2 uv, float2 direction) {
	float4 centre = tex2Dlod(TESR_PointShadowBuffer, float4(uv, 0.0f, 0.0f));
	float centreZ = ViewDepthAt(uv);
	float4 result = centre;
	[branch] if (centreZ < RenderDistance() && centreZ < SkyShare * TESR_CameraData.y) {
		float3 centrePos = ViewPositionAt(uv, centreZ);
		float3 centreNormal = ViewNormalAt(uv);
		float tolerance = max(1.0f, 0.012f * centreZ);
		float2 stride = direction * max(TESR_ShadowScreenSpaceData.y, 0.0f) * TESR_ReciprocalResolution.xy;
		float4 sum = centre;
		float weights = 1.0f;
		[unroll] for (int i = 0; i < 12; i++) {
			float k = i < 6 ? (float)(i - 6) : (float)(i - 5);
			float2 tapUV = clampToRectIfOn(uv + k * stride, TESR_ReciprocalResolution.xy);
			float tapZ = ViewDepthAt(tapUV);
			float planeGap = abs(dot(ViewPositionAt(tapUV, tapZ) - centrePos, centreNormal));
			float weight = exp(-k * k / 18.0f) * saturate(1.0f - planeGap / tolerance);
			sum += weight * tex2Dlod(TESR_PointShadowBuffer, float4(tapUV, 0.0f, 0.0f));
			weights += weight;
		}
		result = sum / weights;
	}
	return result;
}

float4 BlurAcrossPS(float2 uv : TEXCOORD0) : COLOR0 { return float4(DepthAwareBlur(uv, float2(1.0f, 0.0f)).rg, 0.0f, 1.0f); }
float4 BlurDownPS(float2 uv : TEXCOORD0) : COLOR0 { return float4(DepthAwareBlur(uv, float2(0.0f, 1.0f)).rg, 0.0f, 1.0f); }

// the forward techniques' last pass: the vertical blur, then both channels raised to Intensity
float4 BlurDownFinalPS(float2 uv : TEXCOORD0) : COLOR0 {
	float2 blurred = DepthAwareBlur(uv, float2(0.0f, 1.0f)).rg;
	return float4(pow(max(blurred, 1.0e-6f), ContactIntensity()), 0.0f, 1.0f);
}

// ---------------------------------------------------------------------------------------------------------------------------
// Sun cascade maps (Shaders/Includes/Shadow.hlsl GetSunShadow, Mode 3, with both cascades looked up in a hand-over band)
// ---------------------------------------------------------------------------------------------------------------------------

// The atlas texels round uv compared with depth and weighed with a quadratic B-spline centred on the point (3 x 3), or with
// SmoothFilter a cubic one (4 x 4, half a texel more slack). Taps stay half a texel inside the cascade's quadrant.
float AtlasFilter(float2 uv, float depth, float2 quadrant, float slopeBias) {
	float texel = max(TESR_ShadowBlur.x, 1.0f / 16384.0f);
	float2 lo = quadrant + 0.5f * texel;
	float2 hi = quadrant + 0.5f - 0.5f * texel;
	float lit = 0.0f;
	[branch] if (TESR_ShadowBlur.z > 0.0f) {
		float2 grid = uv / texel - 0.5f;
		float2 base = floor(grid);
		float2 t = grid - base;
		float2 t2 = t * t, t3 = t2 * t;
		float4 cx = float4((1.0f - t.x) * (1.0f - t.x) * (1.0f - t.x), 3.0f * t3.x - 6.0f * t2.x + 4.0f, -3.0f * t3.x + 3.0f * t2.x + 3.0f * t.x + 1.0f, t3.x) / 6.0f;
		float4 cy = float4((1.0f - t.y) * (1.0f - t.y) * (1.0f - t.y), 3.0f * t3.y - 6.0f * t2.y + 4.0f, -3.0f * t3.y + 3.0f * t2.y + 3.0f * t.y + 1.0f, t3.y) / 6.0f;
		float cubicDepth = depth - 0.5f * slopeBias;
		[unroll] for (int row = 0; row < 4; row++) {
			[unroll] for (int col = 0; col < 4; col++) {
				float2 tap = clamp((base + float2(col - 1, row - 1) + 0.5f) * texel, lo, hi);
				lit += (cubicDepth <= tex2Dlod(TESR_ShadowAtlas, float4(tap, 0.0f, 0.0f)).x ? 1.0f : 0.0f) * cx[col] * cy[row];
			}
		}
	}
	else {
		float2 grid = uv / texel - 0.5f;
		float2 nearest = floor(grid + 0.5f);
		float2 f = grid - nearest;
		float3 wx = float3(0.5f * (0.5f - f.x) * (0.5f - f.x), 0.75f - f.x * f.x, 0.5f * (0.5f + f.x) * (0.5f + f.x));
		float3 wy = float3(0.5f * (0.5f - f.y) * (0.5f - f.y), 0.75f - f.y * f.y, 0.5f * (0.5f + f.y) * (0.5f + f.y));
		[unroll] for (int row = 0; row < 3; row++) {
			[unroll] for (int col = 0; col < 3; col++) {
				float2 tap = clamp((nearest + float2(col - 1, row - 1) + 0.5f) * texel, lo, hi);
				lit += (depth <= tex2Dlod(TESR_ShadowAtlas, float4(tap, 0.0f, 0.0f)).x ? 1.0f : 0.0f) * wx[col] * wy[row];
			}
		}
	}
	return lit;
}

// One cascade's lookup (c: 0 Near, 1 Middle, 2 Far, 3 Lod) at an already offset camera-relative world point.
float CascadeLookup(float c, float3 worldPos, float bias, float slopeBias) {
	float4x4 toLight = c < 0.5f ? TESR_ShadowCameraToLightTransformNear : c < 1.5f ? TESR_ShadowCameraToLightTransformMiddle :
	                   c < 2.5f ? TESR_ShadowCameraToLightTransformFar : TESR_ShadowCameraToLightTransformLod;
	float2 quadrant = c < 0.5f ? float2(0.0f, 0.0f) : c < 1.5f ? float2(0.5f, 0.0f) : c < 2.5f ? float2(0.0f, 0.5f) : float2(0.5f, 0.5f);
	float4 light = mul(float4(worldPos, 1.0f), toLight);
	light.xyz /= light.w;
	float2 atlasUV = float2(light.x * 0.5f + 0.5f, light.y * -0.5f + 0.5f) * 0.5f + quadrant;
	return AtlasFilter(atlasUV, light.z - bias - 1.5f * slopeBias, quadrant, slopeBias);
}

// 1 lit .. 0 shadowed by the cascades (camera-relative world position and unit world normal); 1 past the Lod cascade.
float CascadeVisibility(float3 worldPos, float3 worldNormal) {
	float nDotL = dot(worldNormal, TESR_SmoothedSunDir.xyz);
	float offsetScale = saturate(1.0f - nDotL);
	float4 radii = float4(TESR_ShadowNearCenter.w, TESR_ShadowMiddleCenter.w, TESR_ShadowFarCenter.w, TESR_ShadowLodCenter.w);
	float4 texelWorld = 4.0f * radii * max(TESR_ShadowBlur.x, 1.0f / 16384.0f);   // each cascade is 2R wide on half the atlas
	float4 normalOffset = offsetScale * 1.5f * texelWorld;                         // 1.5 of the cascade's texels at a grazing sun
	// receiver-plane slack: the surface's fall-off from the sun per texel (a texel is 2 x TESR_ShadowBlur.x of depth), tan capped at 5
	float slopeBias = 2.0f * TESR_ShadowBlur.x * min(sqrt(saturate(1.0f - nDotL * nDotL)) / max(nDotL, 0.05f), 5.0f);
	float bias = 0.0002f * (1.0f + offsetScale);
	float4 bandStart = radii - max(0.1f * radii, min(TESR_ShadowForwardData.w, radii));   // HandoverBlend, never under a tenth
	float4 distances = float4(length(worldPos - TESR_ShadowNearCenter.xyz), length(worldPos - TESR_ShadowMiddleCenter.xyz),
	                          length(worldPos - TESR_ShadowFarCenter.xyz), length(worldPos - TESR_ShadowLodCenter.xyz));
	float4 inside = distances < radii ? 1.0f : 0.0f;
	float4 fades = smoothstep(bandStart, max(radii, bandStart + 1.0e-3f), distances);
	// the innermost cascade holding the point, and how far into its outer band it is
	float cascade = inside.x > 0.5f ? 0.0f : inside.y > 0.5f ? 1.0f : inside.z > 0.5f ? 2.0f : inside.w > 0.5f ? 3.0f : -1.0f;
	float fade = cascade < 0.5f ? fades.x : cascade < 1.5f ? fades.y : cascade < 2.5f ? fades.z : fades.w;
	// in the band both cascades read the same point: the normal offset goes from this cascade's to the next one's
	float offsetHere = cascade < 0.5f ? lerp(normalOffset.x, normalOffset.y, fade) : cascade < 1.5f ? lerp(normalOffset.y, normalOffset.z, fade) :
	                   cascade < 2.5f ? lerp(normalOffset.z, normalOffset.w, fade) : normalOffset.w;
	// a Middle lookup inside Near's band keeps Near's slack in world units at the band's start (Shadow.hlsl slackScale)
	float slackScale = cascade < 0.5f ? lerp(radii.x / max(radii.y, 0.001f), 1.0f, fade) : 1.0f;
	float3 lookupPos = worldPos + offsetHere * worldNormal;
	float visibility = 1.0f;
	[branch] if (cascade >= 0.0f) {
		visibility = CascadeLookup(cascade, lookupPos, bias, slopeBias);
		[branch] if (fade > 0.0f) {
			float next = 1.0f;   // past the Lod cascade: lit
			[branch] if (cascade < 2.5f) next = CascadeLookup(cascade + 1.0f, lookupPos, bias, slopeBias * slackScale);
			visibility = lerp(visibility, next, fade);
		}
	}
	return saturate(visibility);
}

#if CHARACTER_SHADOW_MAP
// The player's own sun map (Shaders/Includes/Shadow.hlsl CharacterMapShadow): inside its box the darker of the cascades' term and
// the map's, fading back to the cascades over the box's outer band. Quadratic B-spline 3 x 3 on the map (R32F plain depth, cleared
// to 1), receivers past the far plane compared just under 1. debugView: set when TESR_CharShadowData.w asks for a diagnostic view
// inside the box (2: the map's verdict, 3: the map's depth, 4: the point's depth), which then replaces the whole sun term.
float CharacterVisibility(float3 worldPos, float3 worldNormal, float nDotL, float cascadeVisibility, out bool debugView) {
	debugView = false;
	float texel = max(TESR_CharShadowData.x, 1.0f / 16384.0f);
	float halfExtent = max(TESR_CharShadowCenter.w, 1.0f);
	float offsetScale = saturate(1.0f - nDotL);
	float3 position = worldPos + worldNormal * (offsetScale * 1.5f * 2.0f * halfExtent * texel);
	float4 light = mul(float4(position, 1.0f), TESR_CharShadowCameraToLight);
	float edge = max(abs(light.x), abs(light.y));
	float weight = 1.0f - smoothstep(1.0f - max(TESR_CharShadowData.y, 0.01f), 1.0f, edge);
	float result = cascadeVisibility;
	[branch] if (weight > 0.0f) {
		float2 uv = float2(light.x * 0.5f + 0.5f, light.y * -0.5f + 0.5f);
		float slope = min(sqrt(saturate(1.0f - nDotL * nDotL)) / max(nDotL, 0.05f), 5.0f);
		float depth = min(light.z, 0.9999f) - TESR_CharShadowData.z - 1.5f * texel * slope;
		float2 lo = 0.5f * texel;
		float2 hi = 1.0f - 0.5f * texel;
		float2 grid = uv / texel - 0.5f;
		float2 nearest = floor(grid + 0.5f);
		float2 f = grid - nearest;
		float3 wx = float3(0.5f * (0.5f - f.x) * (0.5f - f.x), 0.75f - f.x * f.x, 0.5f * (0.5f + f.x) * (0.5f + f.x));
		float3 wy = float3(0.5f * (0.5f - f.y) * (0.5f - f.y), 0.75f - f.y * f.y, 0.5f * (0.5f + f.y) * (0.5f + f.y));
		float lit = 0.0f;
		[unroll] for (int row = 0; row < 3; row++) {
			[unroll] for (int col = 0; col < 3; col++) {
				float2 tap = clamp((nearest + float2(col - 1, row - 1) + 0.5f) * texel, lo, hi);
				lit += (depth <= tex2Dlod(TESR_CharShadowMap, float4(tap, 0.0f, 0.0f)).x ? 1.0f : 0.0f) * wx[col] * wy[row];
			}
		}
		result = lerp(cascadeVisibility, min(cascadeVisibility, lit), weight);
		[branch] if (TESR_CharShadowData.w > 1.5f) {
			debugView = true;
			result = TESR_CharShadowData.w > 3.5f ? saturate(light.z) :
			         TESR_CharShadowData.w > 2.5f ? tex2Dlod(TESR_CharShadowMap, float4(clamp(uv, lo, hi), 0.0f, 0.0f)).x : lit;
		}
	}
	return result;
}
#endif

// The sun maps' term at a pixel for the deferred apply: 1 where the object shaders do the cascades (FORWARD_SHADOWS compiled in
// and TESR_ShadowForwardData.x 0), the maps are not drawn, indoors, a mode other than PCF, or sky. With RT sun shadows replacing
// the cascades (TESR_RtSunReplace.x = range R), the cascades are 1 out to 0.85 R and come back linearly over its last 15 %, where
// the traced visibility fades to 1 (Rt/RtSunShadow.comp), so the product RtSunShadows::Apply makes is one or the other.
float SunMapsVisibility(float2 uv, out bool debugView) {
	debugView = false;
	float viewZ = ViewDepthAt(uv);
	bool objectShadersDoIt = FORWARD_SHADOWS != 0 && TESR_ShadowForwardData.x < 0.5f;
	float visibility = 1.0f;
	[branch] if (!objectShadersDoIt && TESR_ShadowFade.y > 0.5f && TESR_ShadowFormatData.z > 0.5f && abs(TESR_ShadowFormatData.x - 3.0f) < 0.5f &&
	             viewZ < SkyShare * TESR_CameraData.y) {
		float3 viewPos = ViewPositionAt(uv, viewZ);
		float3 worldPos = WorldPositionOf(viewPos);
		float3 worldNormal = normalize(WorldDirectionOf(ViewNormalAt(uv)));
		visibility = CascadeVisibility(worldPos, worldNormal);
		float rtRange = TESR_RtSunReplace.x;
		visibility = rtRange > 0.0f ? lerp(1.0f, visibility, saturate((viewZ - 0.85f * rtRange) / (0.15f * rtRange))) : visibility;
#if CHARACTER_SHADOW_MAP
		// after the RT fade: the rays do not trace actors, so the player's own self-shadow stays in Mode 1 too
		[branch] if (TESR_CharShadowData.w > 0.0f)
			visibility = CharacterVisibility(worldPos, worldNormal, dot(worldNormal, TESR_SmoothedSunDir.xyz), visibility, debugView);
#endif
	}
	return visibility;
}

// Last pass of techniques 0 and 3: both channels raised to Intensity, then red takes the darker of the contact term and the
// sun maps' (a character map debug view replaces red outright).
float4 ApplySunMapsPS(float2 uv : TEXCOORD0) : COLOR0 {
	float2 contact = pow(max(tex2Dlod(TESR_PointShadowBuffer, float4(uv, 0.0f, 0.0f)).rg, 1.0e-6f), ContactIntensity());
	bool debugView;
	float sun = SunMapsVisibility(uv, debugView);
	contact.r = debugView ? sun : min(contact.r, sun);
	return float4(contact, 0.0f, 1.0f);
}

// ---- techniques: indices, names and pass counts as SunShadows.cpp and ShaderManager.cpp expect ----
technique
{
	pass ContactMarch { VertexShader = compile vs_3_0 FrameVS(); PixelShader = compile ps_3_0 ContactMarchPS(false); }
	pass BlurAcross { VertexShader = compile vs_3_0 FrameVS(); PixelShader = compile ps_3_0 BlurAcrossPS(); }
	pass BlurDown { VertexShader = compile vs_3_0 FrameVS(); PixelShader = compile ps_3_0 BlurDownPS(); }
	pass ApplySunMaps { VertexShader = compile vs_3_0 FrameVS(); PixelShader = compile ps_3_0 ApplySunMapsPS(); }
}

technique ForwardContactShadows
{
	pass ContactMarch { VertexShader = compile vs_3_0 FrameVS(); PixelShader = compile ps_3_0 ContactMarchPS(false); }
	pass BlurAcross { VertexShader = compile vs_3_0 FrameVS(); PixelShader = compile ps_3_0 BlurAcrossPS(); }
	pass BlurDownFinal { VertexShader = compile vs_3_0 FrameVS(); PixelShader = compile ps_3_0 BlurDownFinalPS(); }
}

technique ForwardContactSharp
{
	pass SharpMarch { VertexShader = compile vs_3_0 FrameVS(); PixelShader = compile ps_3_0 SharpMarchPS(); }
	pass SharpFinish { VertexShader = compile vs_3_0 FrameVS(); PixelShader = compile ps_3_0 SharpFinishPS(); }
}

technique ContactFast
{
	pass ContactMarch { VertexShader = compile vs_3_0 FrameVS(); PixelShader = compile ps_3_0 ContactMarchPS(true); }
	pass BlurAcross { VertexShader = compile vs_3_0 FrameVS(); PixelShader = compile ps_3_0 BlurAcrossPS(); }
	pass BlurDown { VertexShader = compile vs_3_0 FrameVS(); PixelShader = compile ps_3_0 BlurDownPS(); }
	pass ApplySunMaps { VertexShader = compile vs_3_0 FrameVS(); PixelShader = compile ps_3_0 ApplySunMapsPS(); }
}

technique ForwardContactShadowsFast
{
	pass ContactMarch { VertexShader = compile vs_3_0 FrameVS(); PixelShader = compile ps_3_0 ContactMarchPS(true); }
	pass BlurAcross { VertexShader = compile vs_3_0 FrameVS(); PixelShader = compile ps_3_0 BlurAcrossPS(); }
	pass BlurDownFinal { VertexShader = compile vs_3_0 FrameVS(); PixelShader = compile ps_3_0 BlurDownFinalPS(); }
}
