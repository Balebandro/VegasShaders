// Shader to compute the complete point-light shadow pass in one screen draw.

float4 TESR_ShadowLightPosition[12];
float4 TESR_LightPosition[12];
float4 TESR_LightColor[24];
// UNOFFICIAL (2026-10-08): the next 16 lamps past the 24 above, with forward shadows indoors (ShaderManager::
// GetNearbyLights, ExtraLightsMax). One register each (this shader has room for little more): xyz position, w = radius
// (whole units) x 8 + the weight luma(colour) x dimmer. w 0 ends the list.
float4 TESR_ExtraLights[16];
float4 TESR_ShadowFade;
float4 TESR_PointShadowNear; // UNOFFICIAL x: NearFade, world units (0 off); y: EdgeFade (0 off); z: LampReachMask, 1 / LightRadiusMult (0 off); w: 1 MaskSkipFar
float4 TESR_SpotLightPosition;
float4 TESR_SpotLightDirection;
float4 TESR_SpotLightColor;
// UNOFFICIAL 2026-10-16 lamp contact shadows ([Shaders.ShadowsInteriors.Main] Contact*; LampContactPS below):
float4 TESR_LampContact;		// x strength (0 off), y ray length, z thickness (world units), w normal offset (units)
float4 TESR_LampContactFade;	// x fade-in start (units from the camera, past the weapon), y 1 / its range, z fade-out end, w 1 / its range
float4 TESR_LampContactBias;	// x depth bias, y bias per unit of depth, z normal offset per unit of depth, w least share of the lamp light the traced lamp must give (0.2)
float4 TESR_LampContactNoise;	// xy interleaved gradient noise's factors times the screen size, z its multiplier, w 1 noise on (only
							// with the darkening blurred: [Shaders.ShadowsInteriors.Forward] DarkeningBlur), 0 even steps


//sampler_state removed to avoid a artifact. TODO investigate
sampler2D TESR_DepthBuffer : register(s0) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_NormalsBuffer : register(s1) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = NONE; MINFILTER = NONE; MIPFILTER = NONE; };
samplerCUBE TESR_ShadowCubeMapBuffer0 : register(s2) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; ADDRESSW = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
samplerCUBE TESR_ShadowCubeMapBuffer1 : register(s3) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; ADDRESSW = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
samplerCUBE TESR_ShadowCubeMapBuffer2 : register(s4) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; ADDRESSW = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
samplerCUBE TESR_ShadowCubeMapBuffer3 : register(s5) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; ADDRESSW = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
samplerCUBE TESR_ShadowCubeMapBuffer4 : register(s6) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; ADDRESSW = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
samplerCUBE TESR_ShadowCubeMapBuffer5 : register(s7) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; ADDRESSW = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
samplerCUBE TESR_ShadowCubeMapBuffer6 : register(s8) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; ADDRESSW = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
samplerCUBE TESR_ShadowCubeMapBuffer7 : register(s9) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; ADDRESSW = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
samplerCUBE TESR_ShadowCubeMapBuffer8 : register(s10) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; ADDRESSW = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
samplerCUBE TESR_ShadowCubeMapBuffer9 : register(s11) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; ADDRESSW = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
samplerCUBE TESR_ShadowCubeMapBuffer10 : register(s12) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; ADDRESSW = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };

// UNOFFICIAL 2026-10-16 MaskHalfRes (LampMaskHalf/LampMaskUpsample below): the half-resolution mask (R) and its depth (G), bound
// by ShaderManager to stage 13 (an NVR_ name: not bound by declaration order).
sampler2D NVR_LampMaskHalf : register(s13) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
float4 TESR_ReciprocalResolution;

#include "Includes/Helpers.hlsl"
#include "Includes/Depth.hlsl"
#include "Includes/Normals.hlsl"
#include "Includes/Shadows.hlsl"

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


float GetSpotLightAmount(float4 worldPos, float4 spotLightPosition, float4 spotLightDirection, float4 normal){
    float3 lightToWorld = spotLightPosition.rgb - worldPos.xyz;
    float3 lightVector = normalize(lightToWorld);

	// radius based attenuation based on https://lisyarus.github.io/blog/graphics/2022/07/30/point-light-attenuation.html
    float radius = spotLightPosition.w;
    float Distance = length(lightToWorld)/radius;
	float s = saturate(Distance * Distance); 
	float atten = saturate(((1 - s) * (1 - s)) / (1 + 5.0 * s));

	float angleCosMax = cos(radians(spotLightDirection.w));
	float angleCosMin = cos(radians(spotLightDirection.w * 0.5));
	float cone = pow(invlerps(angleCosMax, angleCosMin, shades(spotLightDirection.xyz, lightVector * -1)), 2.0);

    float diffuse = shade(lightVector, normal.xyz);
	return diffuse * cone * atten;
}


// One shadow-casting light: GetPointLightAmount() * weight, with the cubemap read behind an exact test.
// GetPointLightAtten() is exactly 0 once the normalised distance reaches 1 (s = 1 -> (1 - s)^2 = 0), and
// GetPointLightAmount() is 0 for a light without a radius (distance is then INF or NaN, which
// !(distance < 1) also catches), so in those cases the shadow lookup cannot change the result and is
// skipped. The test depends on the pixel, so it is a dynamic branch: a test on constants alone would
// use one of ps_3_0's 16 boolean registers per light and run out. The branch needs an explicit-LOD
// read (gradients are illegal in dynamic flow control); the cubemaps have a single level, so it
// returns what texCUBE did.
// UNOFFICIAL near: NearFade / the light's radius. A blocker within NearFade of the lamp shadows less, none at the lamp
// itself: a real lamp is no point, and a blocker that close casts a shadow too blurred to see, where a point casts it
// sharp, as large as the room, and on every side when the point is inside the blocker (a lamp at the player's head).
// Shaders\Includes\PointShadowForward.hlsl PointShadowNear does the same for the forward shadows.
float PointLightAmountValueLod(samplerCUBE cube, float3 lightDir, float distance, float near) {
	if (TESR_ShadowFade.z == 0) return 1;

	float lightDepth = texCUBElod(cube, float4(lightDir, 0)).r;
	float Shadow = lightDepth + BIAS * distance > distance;
	float blocker = near > 0.0f ? smoothstep(0.0f, near, lightDepth) : 1.0f;
	return lerp(1, Shadow, (lightDepth > 0.0f && lightDepth < 1.0f) * blocker);
}

// UNOFFICIAL EdgeFade (TESR_PointShadowNear.y, a fraction of the lamp's radius): a lamp's light amount is attenuation
// times colour luma times intensity, clamped to 1; a bright lamp keeps that at 1 across most of its radius and drops only
// in the last few percent, so the un-darkened disc round it, and every shadow inside it, ended on a hard circle. Each
// lamp's amount is windowed to 0 over the outer EdgeFade of its radius instead.
float EdgeWindow(float distance) {
	return TESR_PointShadowNear.y > 0.0f ? 1.0f - smoothstep(1.0f - TESR_PointShadowNear.y, 1.0f, distance) : 1.0f;
}

// UNOFFICIAL LampReachMask ([Shaders.ShadowsInteriors.Main]; TESR_PointShadowNear.z = 1 / LightRadiusMult, 0 = off). With
// forward shadows on, this pass's sum is only the room darkening, applied on top of the game's own lamp lighting, which
// already fades with distance and angle; applying a falloff here as well darkened the floor a second time, so a shadow
// on it seemed to stop at the circle where the lamp's clamped amount began to drop. With the switch on a lamp counts in
// full out to its game radius and fades over the rest of the cube map's radius: the game's lighting does the falloff,
// and the lamp's shadows (forward, or this pass's own with forward off) show in full.
float LampReach(float distance) {
	return 1.0f - smoothstep(TESR_PointShadowNear.z, 1.0f, distance);
}

// The light amount this pass gives a lamp at a normalised distance: the old attenuation, or LampReach with the switch.
// UNOFFICIAL 2026-10-25 ForwardReachMask 2 (TESR_PointShadowNear.z < 0): the old attenuation without its angle term. The angle came
// from TESR_NormalsBuffer, rebuilt from depth (one flat normal per triangle): near a lamp every triangle of a character got its own
// darkening (the facets). The distance falloff stays, so the room keeps its old brightness (LampReach lightens it).
float LampAmount(float3 lightDir, float distance, float4 normal) {
	// The normal along the lamp makes GetPointLightAtten's angle term 1 (no new literals: this effect sits at the constant limit).
	if (TESR_PointShadowNear.z < 0.0f) return GetPointLightAtten(lightDir, distance, float4(normalize(lightDir), 0));
	return TESR_PointShadowNear.z > 0.0f ? LampReach(distance) : GetPointLightAtten(lightDir, distance, normal);
}

float ShadowedLight(samplerCUBE cube, float4 worldPos, float4 lightPos, float4 normal, float weight) {
	float3 lightDir = lightPos.xyz - worldPos.xyz;
	float distance = length(lightDir) / lightPos.w; // normalize distance over light range
	[branch] if (!(distance < 1.0) || weight == 0) return 0;

	float amount = PointLightAmountValueLod(cube, lightDir * float3(-1, -1, 1), distance, TESR_PointShadowNear.x / lightPos.w) * LampAmount(lightDir, distance, normal);
	return saturate(saturate(amount) * weight) * EdgeWindow(distance);
}


float4 Shadow( VSOUT IN, uniform bool batch ) : COLOR0 {

	float2 uv = IN.UVCoord;
	float depth = readDepth(uv);
    float3 camera_vector = toWorld(uv) * depth;
    float4 world_pos = float4(TESR_CameraPosition.xyz + camera_vector, 1.0f);	
	float4 normal = float4(GetWorldNormal(uv), 1);

	float Shadow = ShadowedLight(TESR_ShadowCubeMapBuffer0, world_pos, TESR_ShadowLightPosition[0], normal, luma(TESR_LightColor[0].rgb) * TESR_LightColor[0].w);
	Shadow += ShadowedLight(TESR_ShadowCubeMapBuffer1, world_pos, TESR_ShadowLightPosition[1], normal, luma(TESR_LightColor[1].rgb) * TESR_LightColor[1].w);
	Shadow += ShadowedLight(TESR_ShadowCubeMapBuffer2, world_pos, TESR_ShadowLightPosition[2], normal, luma(TESR_LightColor[2].rgb) * TESR_LightColor[2].w);
	Shadow += ShadowedLight(TESR_ShadowCubeMapBuffer3, world_pos, TESR_ShadowLightPosition[3], normal, luma(TESR_LightColor[3].rgb) * TESR_LightColor[3].w);
	Shadow += ShadowedLight(TESR_ShadowCubeMapBuffer4, world_pos, TESR_ShadowLightPosition[4], normal, luma(TESR_LightColor[4].rgb) * TESR_LightColor[4].w);
	Shadow += ShadowedLight(TESR_ShadowCubeMapBuffer5, world_pos, TESR_ShadowLightPosition[5], normal, luma(TESR_LightColor[5].rgb) * TESR_LightColor[5].w);
	Shadow += ShadowedLight(TESR_ShadowCubeMapBuffer6, world_pos, TESR_ShadowLightPosition[6], normal, luma(TESR_LightColor[6].rgb) * TESR_LightColor[6].w);
	Shadow += ShadowedLight(TESR_ShadowCubeMapBuffer7, world_pos, TESR_ShadowLightPosition[7], normal, luma(TESR_LightColor[7].rgb) * TESR_LightColor[7].w);
	Shadow += ShadowedLight(TESR_ShadowCubeMapBuffer8, world_pos, TESR_ShadowLightPosition[8], normal, luma(TESR_LightColor[8].rgb) * TESR_LightColor[8].w);
	Shadow += ShadowedLight(TESR_ShadowCubeMapBuffer9, world_pos, TESR_ShadowLightPosition[9], normal, luma(TESR_LightColor[9].rgb) * TESR_LightColor[9].w);
	Shadow += ShadowedLight(TESR_ShadowCubeMapBuffer10, world_pos, TESR_ShadowLightPosition[10], normal, luma(TESR_LightColor[10].rgb) * TESR_LightColor[10].w);
	[branch] if (!batch && TESR_ShadowLightPosition[11].w) {
		float4 light11 = GetPointLightDistance(world_pos, TESR_ShadowLightPosition[11]);
		Shadow += saturate(LampAmount(light11.xyz, light11.w, normal)) * EdgeWindow(light11.w);
	}

	[branch] if (!batch && TESR_SpotLightPosition.w)
		Shadow += GetSpotLightAmount(world_pos, TESR_SpotLightPosition, TESR_SpotLightDirection, normal) * luma(TESR_SpotLightColor.rgb) * TESR_SpotLightColor.w;
	
	for (int i = 0; i< 12 && !batch; i++){
		[branch] if (TESR_LightPosition[i].w) {
			float4 light = GetPointLightDistance(world_pos, TESR_LightPosition[i]);
			// UNOFFICIAL 2026-10-16 (MaskSkipFar, TESR_PointShadowNear.w): beyond its radius a lamp gives exactly 0 (as ShadowedLight).
			[branch] if (light.w < 1.0f || TESR_PointShadowNear.w < 0.5f)
				Shadow += saturate(LampAmount(light.xyz, light.w, normal) * luma(TESR_LightColor[i + 12].rgb) * TESR_LightColor[i + 12].w) * EdgeWindow(light.w);
		}
	}

	// UNOFFICIAL: the extra lamps (above), so what only they light is not darkened as unlit. A pixel lit in full needs none.
	[loop] for (int e = 0; e < 16 && !batch; e++) {
		[branch] if (!TESR_ExtraLights[e].w || Shadow >= 1.0f) break;
		float radius = floor(TESR_ExtraLights[e].w * 0.125f);
		float weight = TESR_ExtraLights[e].w - radius * 8.0f;
		float4 light = GetPointLightDistance(world_pos, float4(TESR_ExtraLights[e].xyz, radius));
		[branch] if (light.w < 1.0f || TESR_PointShadowNear.w < 0.5f)	// (MaskSkipFar)
			Shadow += saturate(LampAmount(light.xyz, light.w, normal) * weight) * EdgeWindow(light.w);
	}

	Shadow = saturate(Shadow);
	
	return float4(Shadow, Shadow, Shadow, 1.0f);
}

// UNOFFICIAL 2026-10-16 lamp contact shadows (after Bend Studio's screen-space shadows, which NVR has for the sun only,
// SunShadows.fx): the lamps' cube maps (512 px, filtered) lose the small dark lines where a prop meets the floor or a table the
// wall. A pass of its own (technique 2, ShaderManager), drawn over the lamp mask with a multiply: Shadow() above is at the
// ps_3_0 limit of 224 constant registers once the effect compiler has worked out its per-lamp values (preshader), so nothing
// could be added there.
// ContactProject undoes toWorld (Depth.hlsl): a camera-relative point -> its screen position and view depth (the view matrix's
// rotation: columns right, up, forward, as toWorld reads them).
float3 ContactProject(float3 r) {
	float3 v = mul(r, (float3x3)TESR_ViewTransform);
	return float3((v.x / v.z * TESR_ProjectionTransform[0][0]) * 0.5f + 0.5f, (v.y / v.z * TESR_ProjectionTransform[1][1]) * -0.5f + 0.5f, v.z);
}

// How open the way from this pixel toward a lamp is over the first TESR_LampContact.y units (at most half way to it; 1 open,
// 0 blocked right at the pixel): 6 steps through the depth buffer; a step behind a surface by less than the thickness is
// blocked. A blocker met further along shades less (1 - t), so the shadow fades out instead of ending on a line. With the
// darkening blurred the steps start at a per-pixel offset (interleaved gradient noise, Jimenez 2014); unblurred (the default) at
// the middle of each sixth, which would otherwise show as grain.
float ContactOpen(float3 r0, float3 worldPos, float3 n, float3 lampWorld, float2 uv, float pixelDepth) {
	float3 toLamp = lampWorld - worldPos;
	float dist = length(toLamp);
	float3 dir = toLamp / dist;		// (a lamp that lights this pixel is never at it)
	// Facing away from the lamp (or along): its own lighting shades that side; a ray from there runs straight into the surface
	// it starts on (seen in the Ultra-Luxe: the fountain's stem, lamps inside it, darkened all over).
	[branch] if (dot(dir, n) < 0.1f) return 1.0f;
	float len = min(TESR_LampContact.y, dist * 0.5f);
	float jitter = lerp(0.5f, frac(TESR_LampContactNoise.z * frac(dot(uv, TESR_LampContactNoise.xy))), TESR_LampContactNoise.w);	// (w 0: even steps)
	float3 start = r0 + n * (TESR_LampContact.w + pixelDepth * TESR_LampContactBias.z);
	float occ = 0.0f;
	[loop] for (int i = 0; i < 6; i++) {
		float t = (i + jitter) / 6.0f;
		float3 s = ContactProject(start + dir * (len * t));
		[branch] if (s.z < 1.0f || any(s.xy < 0.0f) || any(s.xy > 1.0f)) break;
		float delta = s.z - readDepthLod(screenToRect(s.xy));	// UNOFFICIAL 2026-10-27 RenderScale v2: ContactProject gives screen position
		occ = max(occ, (delta > TESR_LampContactBias.x + s.z * TESR_LampContactBias.y && delta < TESR_LampContact.z) ? 1.0f - t : 0.0f);
	}
	return 1.0f - occ;
}

// The mask's multiplier: the shadowed lamp that gives this pixel the most is traced toward (one: two cost twice as much for
// little more); the mask drops by the share of the shadowed lamps' light here that it loses (a hole in one lamp of a saturated
// sum would otherwise not show). Lamps weighed by distance and colour alone (cheaper than Shadow()'s amounts; the facing test is
// in ContactOpen). Faded in past the first-person weapon and out far away.
float4 LampContactPS(VSOUT IN) : COLOR0 {
	float2 uv = IN.UVCoord;
	float depth = readDepth(uv);
	float fade = saturate((depth - TESR_LampContactFade.x) * TESR_LampContactFade.y) * saturate((TESR_LampContactFade.z - depth) * TESR_LampContactFade.w);
	float keep = 1.0f;
	[branch] if (fade > 0.0f) {
		float3 camera_vector = toWorld(uv) * depth;
		float3 world_pos = TESR_CameraPosition.xyz + camera_vector;
		float total = 0.0f, b1 = 0.0f;
		float3 L1 = 0.0f;
		[unroll] for (int i = 0; i < 11; i++) {
			float4 lp = TESR_ShadowLightPosition[i];
			float3 lightDir = lp.xyz - world_pos;
			float s2 = saturate(dot(lightDir, lightDir) / max(lp.w * lp.w, 1.0f));
			float a = lp.w > 0.0f ? (1.0f - s2) * (1.0f - s2) * luma(TESR_LightColor[i].rgb) * TESR_LightColor[i].w : 0.0f;
			total += a;
			if (a > b1) { b1 = a; L1 = lp.xyz; }
		}
		[branch] if (b1 > TESR_LampContactBias.w * total && b1 > 0.0f) {
			float blocked = b1 * (1.0f - ContactOpen(camera_vector, world_pos, GetWorldNormal(uv), L1, uv, depth));
			keep = 1.0f - TESR_LampContact.x * fade * saturate(blocked / total);
		}
	}
	return float4(keep, keep, keep, 1.0f);
}

technique MergedPointShadows {
	pass {
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 Shadow(false);
	}
}

// UNOFFICIAL ExteriorLightPoints (ShaderManager::RenderPointShadowBatches): a further batch of 11 shadowed lamps, its light
// added to what the passes before wrote (saturated where the buffer is read). Only the shadowed lamps: the rest were in the
// first pass.
technique AddShadowedLamps {
	pass {
		AlphaBlendEnable = true;
		SrcBlend = ONE;
		DestBlend = ONE;
		BlendOp = ADD;
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 Shadow(true);
	}
}

// UNOFFICIAL 2026-10-16 lamp contact shadows (LampContactPS): the lamp mask multiplied by the contact term.
technique LampContact {
	pass {
		AlphaBlendEnable = true;
		SrcBlend = ZERO;
		DestBlend = SRCCOLOR;
		BlendOp = ADD;
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 LampContactPS();
	}
}

// UNOFFICIAL 2026-10-16 MaskHalfRes ([Shaders.ShadowsInteriors.Forward]; only with forward lamp shadows, where the mask holds
// the lamps' unshadowed reach, a smooth function of position and normal): the mask at half resolution, then a depth-aware
// upsample (as the AO's DedicatedCombine). Not exact at silhouettes and creases.
// Each half-resolution pixel reads the full-resolution pixel at its top-left (as the AO's PackedVS).
VSOUT HalfMaskVS(VSIN IN)
{
	VSOUT OUT = FrameVS(IN);
	OUT.UVCoord += 0.5 * TESR_ReciprocalResolution.xy;
	return OUT;
}

float4 LampMaskHalfPS(VSOUT IN) : COLOR0
{
	return float4(Shadow(IN, false).r, readDepth(IN.UVCoord), 0.0f, 1.0f);
}

float4 LampMaskUpsamplePS(VSOUT IN) : COLOR0
{
	float2 lowRR = 2.0f * TESR_ReciprocalResolution.xy;
	float2 position = IN.UVCoord / lowRR - 0.5f;
	float2 base = floor(position);
	float2 fraction = frac(position);
	float depth = readDepth(IN.UVCoord);
	float tolerance = 1.0f + 0.01f * depth;
	float sum = 0.0f, weights = 0.0f;
	[unroll] for (int y = 0; y < 2; ++y) {
		[unroll] for (int x = 0; x < 2; ++x) {
			float2 sample = tex2D(NVR_LampMaskHalf, (base + float2(x, y) + 0.5f) * lowRR).rg;
			float weight = (x ? fraction.x : 1.0f - fraction.x) * (y ? fraction.y : 1.0f - fraction.y) + 1e-4f;
			float gap = abs(sample.y - depth) / tolerance;
			weight /= 1.0f + gap * gap * 16.0f;
			sum += sample.x * weight;
			weights += weight;
		}
	}
	float mask = sum / max(weights, 1e-6f);
	return float4(mask, mask, mask, 1.0f);
}

technique LampMaskHalf {
	pass {
		VertexShader = compile vs_3_0 HalfMaskVS();
		PixelShader = compile ps_3_0 LampMaskHalfPS();
	}
}

technique LampMaskUpsample {
	pass {
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 LampMaskUpsamplePS();
	}
}
