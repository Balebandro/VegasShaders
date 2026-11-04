// AvgLuma for Oblivion Reloaded

float4 TESR_MotionBlurData;
float4 TESR_DepthOfFieldData;
float4 TESR_GameTime;
float4 TESR_ExposureData; // x:min brightness, y;max brightness, z:dark adapt speed, w: light adapt speed
// UNOFFICIAL GT7 adaptation ([Shaders.Exposure.Main] Mode 1, technique GT7 below): what its full-field metering reads
float4 TESR_SunColor;     // weather sunlight colour
float4 TESR_SkyColor;     // weather upper sky colour
float4 TESR_SunAmbient;   // weather ambient colour
float4 TESR_HDRData;      // y: Tonemapping Exposure (the setup's scene-to-display scale)
float4 TESR_SunDirection; // z: sun height (sine of its elevation) outdoors by day
float4 TESR_SkyIrradiance[9];   // NVR's spherical-harmonic projection of the rendered sky (linear radiance, 1/pi folded in)
float4 TESR_SunsetColor;        // w: sky brightness multiplier the sky is rendered with
float4 TESR_ShadowFade;         // x: sun shadow fade (1 = no sun shadows, night), y: exterior sun shadows on
float4 TESR_ShadowSunLight;     // sunlight as the lighting uses it (all dimmers applied, gamma-encoded)
float4 TESR_ShadowAmbientLight; // ambient light as the lighting uses it
float4x4 TESR_ShadowCameraToLightTransformOrtho; // top-down height map (the one NVR's Skylighting uses for sky visibility)

sampler2D TESR_RenderedBuffer : register(s0) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_AvgLumaBuffer : register(s1) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_DepthBuffer : register(s2) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_ShadowAtlas : register(s3) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
sampler2D TESR_OrthoMapBuffer : register(s4) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };

static const float decreaseRate = -TESR_ExposureData.z; // max value for adaptation speed towards darker screens
// static const float decreaseRate = -TESR_ExposureData.z * 0.001; // max value for adaptation speed towards darker screens
static const float increaseRate = TESR_ExposureData.w; // max value for adaptation speed towards brighter screens
// static const float increaseRate = TESR_ExposureData.w * 0.001; // max value for adaptation speed towards brighter screens
static const float2 center = float2(0.5, 0.5); // this shader is to be applied on a 1x1 texture so we sample at the center

#include "Includes/Helpers.hlsl"
#include "Includes/Depth.hlsl"	// (includes Includes/RenderRect.hlsl)
#include "Includes/Shadows.hlsl"		// (GT7 technique: the sun cascades for its full-field metering)
#include "Includes/SunCascades.hlsl"
#include "Includes/GT7Adaptation.hlsl"

// UNOFFICIAL 2026-10-27 RenderScale v2: the image and depth are drawn only in their top-left TESR_RenderScale part in a rect frame
static const bool AvgLumaRectOn = TESR_RenderScale.x < 0.9999 || TESR_RenderScale.y < 0.9999;

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

static float2 taps[12] =
{
    float2(-0.326212, -0.405810),
    float2(-0.840144, -0.073580),
    float2(-0.695914,  0.457137),
    float2(-0.203345,  0.620716),
    float2( 0.962340, -0.194983),
    float2( 0.473434, -0.480026),
    float2( 0.519456,  0.767022),
    float2( 0.185461, -0.893124),
    float2( 0.507431,  0.064425),
    float2( 0.896420,  0.412458),
    float2(-0.321940, -0.932615),
    float2(-0.791559, -0.597710)
};

// returns a value from start to end within a min and max step from start
float stepTo(float startValue, float endValue, float minStep, float maxStep){
	// make sure min is negative and max is positive
	minStep = abs(minStep) * -1;
	maxStep = abs(maxStep);

	float diff = clamp(endValue - startValue, minStep, maxStep);
	return startValue + diff;
}

static const float HyperFocalDistance = max(0.0000001, TESR_DepthOfFieldData.x * 1000); // the distance at which DOF is greatest (infinite towards the distance and closest to the camera)

// calculates the current focal distance for depth of field shader and animates the value.
float getFocalDistance() {
	float oldFocus = tex2D(TESR_AvgLumaBuffer, center).b;

	// gets the autofocus depth based on a local average, scaled with player movement speed (to avoid flicker during fast movement)
	float2 speed = saturate(abs(float2(TESR_MotionBlurData.x, TESR_MotionBlurData.y))) + 0.1;

	// UNOFFICIAL 2026-10-27 RenderScale v2: screen positions -> the drawn part (x * 1 when off; the taps stay within 0.33 of the centre)
	float centerDepth = readDepth(screenToRect(center));
	float depth = centerDepth;
	float2 samplingRange = speed;
	for (int i=0; i<12; i++){
		depth += readDepth(screenToRect(center + 0.3 * taps[i] * samplingRange));
	}
	depth /= 12;

	depth = centerDepth > depth ? centerDepth:depth; //if pixel at the center is farther than the average, we use that value (fix for some iron sights)
	depth = saturate(depth/HyperFocalDistance); // remaps to [0-1] to save as color value in the texture

	float step = pow(max(oldFocus, depth), 0.1); // because of the nature of depth, we scale the speed with distance
	return stepTo(oldFocus, depth, 3 * step * TESR_GameTime.w, 1 * step * TESR_GameTime.w);
}

float4 AvgLuma(VSOUT IN) : COLOR0
{	
	float2 oldLuma = tex2D(TESR_AvgLumaBuffer, center).rg;

	// samples 100 different locations around the screen to calculate an average
	float4 color = float4(0, 0, 0, 0);
	float total = 0;
	for (float i = 0.05; i < 1; i+= 0.1){
		for (float j = 0.05; j < 1; j+= 0.1){
			color += linearize(tex2D(TESR_RenderedBuffer, screenToRect(float2(i, j))));	// UNOFFICIAL 2026-10-27 RenderScale v2
			total++;
		}
	}
	for (i= 0 ; i < 12; i++){
		// UNOFFICIAL 2026-10-27 RenderScale v2: these reach past the screen edge; in a rect frame kept on the drawn part's edge (as CLAMP does)
		float2 p = center + taps[i];
		color += linearize(tex2D(TESR_RenderedBuffer, AvgLumaRectOn ? screenToRect(saturate(p)) : p));
		total++;
	}
	color /= total;
	float newLuma = (luma(color) + (7 * oldLuma.r)) / 8; // average over 8 frames

	// gradually change average luma
	float animatedLuma = stepTo(oldLuma.g, newLuma, TESR_GameTime.w * decreaseRate, TESR_GameTime.w * increaseRate);

	// texture will store the actual current luma, the animated current luma, and the animated focal distance for DoF.
	return float4(newLuma, animatedLuma, getFocalDistance(), 1);
}
 
// ---------------------------------------------------------------------------------------------------------------
// Full-field metering (see Includes/GT7Adaptation.hlsl, "Full field"). The eye adapts to the light reaching it from its
// whole field of view, weighted by the cosine to the line of sight (the illuminance at the eye); the screen shows only
// the central part of that field. The rest is estimated around the camera as GT7 does with its light probe, sun and
// sky-visibility references: sky where the sky is open above the camera (NVR's top-down height map), the surfaces that
// block it elsewhere, and the ground below, lit or shaded as the camera itself is (sun shadow map at the camera).
// ---------------------------------------------------------------------------------------------------------------

// Mean radiance of the sky dome over the upper hemisphere, in scene units: exactly twice NVR's first SH coefficient
// (the integral of the rendered sky over the dome divided by 2 pi), times the sky brightness multiplier.
float GT7SkyDomeMean()
{
	float w = TESR_SunsetColor.w > 0.0 ? TESR_SunsetColor.w : 1.0;
	return max(luma(TESR_SkyIrradiance[0].rgb), 0.0) * 2.0 * w;
}

// Irradiance-like sky term for a horizontal surface (E/pi from NVR's SH, the units the ambient lighting uses).
float GT7SkyOnGround()
{
	float3 e = TESR_SkyIrradiance[0].rgb + TESR_SkyIrradiance[2].rgb + TESR_SkyIrradiance[6].rgb * 2.0;
	return max(luma(e), 0.0);
}

// 1 if a camera-relative world point is above everything in NVR's top-down height map (open to the sky), else 0.
// Points outside the map, or no map at all, count as open.
float GT7OpenAt(float3 p)
{
	float4 o4 = mul(float4(p, 1.0), TESR_ShadowCameraToLightTransformOrtho);
	if (abs(o4.w) < 1e-6) return 1.0;
	float3 o = o4.xyz / o4.w;
	if (abs(o.x) > 1.0 || abs(o.y) > 1.0 || o.z < 0.0 || o.z > 1.0) return 1.0;
	float top = tex2Dlod(TESR_OrthoMapBuffer, float4(o.x * 0.5 + 0.5, o.y * -0.5 + 0.5, 0.0, 0.0)).r;
	float bias = GT7FF_OPEN_BIAS * abs(TESR_ShadowCameraToLightTransformOrtho[2][2]);
	return top > o.z - bias ? 1.0 : 0.0;
}

// Whether the sky is open from the camera in world direction d (three points along the ray must be open).
float GT7SkyOpen(float3 d)
{
	return GT7OpenAt(d * GT7FF_RAY_NEAR) * GT7OpenAt(d * GT7FF_RAY_MID) * GT7OpenAt(d * GT7FF_RAY_FAR);
}

// Sunlight reaching a camera-relative point: one lookup in the sun shadow cascade that covers it (as NVR's Sandstorm
// does), faded out with the sun shadows (dusk, night). Outside all cascades: lit.
float GT7SunVisible(float3 p)
{
	float fade = saturate(TESR_ShadowFade.x);
	[branch] if (fade >= 0.999) return 1.0;
	float4x4 lightTransform;
	float2 quadrant;
	float bleed;
	[branch] if (length(p - TESR_ShadowNearCenter.xyz) < TESR_ShadowNearCenter.w) {
		lightTransform = TESR_ShadowCameraToLightTransformNear; quadrant = float2(0.0, 0.0); bleed = 0.1;
	}
	else if (length(p - TESR_ShadowMiddleCenter.xyz) < TESR_ShadowMiddleCenter.w) {
		lightTransform = TESR_ShadowCameraToLightTransformMiddle; quadrant = float2(0.5, 0.0); bleed = 0.2;
	}
	else if (length(p - TESR_ShadowFarCenter.xyz) < TESR_ShadowFarCenter.w) {
		lightTransform = TESR_ShadowCameraToLightTransformFar; quadrant = float2(0.0, 0.5); bleed = 0.6;
	}
	else if (length(p - TESR_ShadowLodCenter.xyz) < TESR_ShadowLodCenter.w) {
		lightTransform = TESR_ShadowCameraToLightTransformLod; quadrant = float2(0.5, 0.5); bleed = 0.8;
	}
	else return 1.0;
	float4 lightSpace = SunCascadeTexCoord(mul(float4(p, 1.0), lightTransform));
	float2 uv = lightSpace.xy * 0.5 + quadrant;
	float lit;
	[branch] if (TESR_ShadowFormatData.x == 3.0) lit = ShadowPCF2x2(TESR_ShadowAtlas, uv, lightSpace.z - 0.0002, TESR_ShadowBlur.x);
	else {
		float4 moments = tex2Dlod(TESR_ShadowAtlas, float4(uv, 0.0, 0.0));
		float bias = TESR_ShadowFormatData.x == 0.0 ? 0.00001 : 0.01;
		[branch] if (TESR_ShadowFormatData.x == 0.0) lit = GetLightAmountValueVSM(moments.xy, lightSpace.z, bias, bleed);
		else if (TESR_ShadowFormatData.x == 1.0) lit = GetLightAmountValueEVSM2(moments.xy, lightSpace.z, bias, bleed, TESR_ShadowFormatData.y);
		else lit = GetLightAmountValueEVSM4(moments, lightSpace.z, bias, bleed, TESR_ShadowFormatData.y);
	}
	return lerp(saturate(lit), 1.0, fade);
}

// Full-field adaptation level (log2 luminance). Returns false (and leaves the result alone) where the references it
// needs do not exist: interiors, sun shadows off, no sky data.
bool GT7FullField(float screenLevel, float screenWeight, float litSum, float litCount, float shadeSum, float shadeCount,
	float3 R, float3 U, float3 F, float tanA, float tanB, float lightLimit, out float field)
{
	field = screenLevel;
	float skyMean = GT7SkyDomeMean();
	if (TESR_ShadowFormatData.z < 0.5 || TESR_ShadowFade.y < 0.5 || skyMean <= 1e-5) return false;

	// The camera's own light: sun visibility at chest height around it (clear of the player's body).
	float camSun = 0.0;
	[loop] for (int s = 0; s < 4; s++) {
		float a = s * (PI * 0.5);
		camSun += 0.25 * GT7SunVisible(float3(cos(a) * GT7FF_CAM_REACH, sin(a) * GT7FF_CAM_REACH, -GT7FF_CAM_DROP));
	}

	// Lit and shaded surface levels from the screen; when one is missing, from the other through the ratio of the light
	// the lighting uses (sun on a level surface plus sky and ambient, against sky and ambient alone).
	float sunUp = max(TESR_SunDirection.z, 0.0) * (1.0 - saturate(TESR_ShadowFade.x));
	float3 sunRGB = pow(max(TESR_ShadowSunLight.rgb, 0.0), GT7WB_LIGHT_GAMMA);
	float3 ambRGB = pow(max(TESR_ShadowAmbientLight.rgb, 0.0), GT7WB_LIGHT_GAMMA);
	float sunLight = luma(sunRGB) * sunUp;
	float shadeLight = GT7SkyOnGround() + luma(ambRGB);
	float litOverShade = clamp(log2((shadeLight + sunLight) / max(shadeLight, 1e-5)), 0.0, GT7FF_MAX_SUN_RATIO);
	float skyLevel = min(log2(skyMean), lightLimit + GT7AE_SKY_MAX);
	float shadeFromSky = log2(GT7FF_ALBEDO * max(shadeLight, 1e-5));                // only when the screen shows sky alone
	float lit = litSum / max(litCount, 1.0), shade = shadeSum / max(shadeCount, 1.0);
	float litW = saturate(litCount / GT7FF_TRUST), shadeW = saturate(shadeCount / GT7FF_TRUST);
	float shadeAlt = litW > 0.0 ? lit - litOverShade : shadeFromSky;
	float shadeEst = lerp(shadeAlt, shade, shadeW);
	float litAlt = shadeW > 0.0 ? shade + litOverShade : shadeFromSky + litOverShade;
	float litEst = lerp(litAlt, lit, litW);
	float ground = lerp(shadeEst, litEst, camSun);   // the ground around the camera is lit as the camera is

	// The rest of the field: cosine-distributed directions around the line of sight, equal weight each; directions
	// that fall on the screen are already measured there.
	float sum = screenLevel * screenWeight, weight = screenWeight;
	const float n = GT7FF_DIRECTIONS;
	[loop] for (int i = 0; i < GT7FF_DIRECTIONS; i++) {
		float r = sqrt((i + 0.5) / n), phi = i * 2.39996323;
		float3 l = float3(r * cos(phi), r * sin(phi), sqrt(1.0 - r * r));
		[branch] if (abs(l.x) <= tanA * l.z && abs(l.y) <= tanB * l.z) continue;
		float3 d = R * l.x + U * l.y + F * l.z;
		float level = ground;
		[branch] if (d.z > 0.0) level = lerp(shadeEst, skyLevel, GT7SkyOpen(d));   // open sky, or what blocks it (shaded)
		sum += level * (PI / n);
		weight += PI / n;
	}
	field = sum / weight;
	return true;
}

// GT7-style metering and adaptation (see Includes/GT7Adaptation.hlsl). Stores: r short-term adaptation,
// g long-term adaptation (both log2 luminance), b depth-of-field focus, a white balance in mired.
// GT7WeatherLight (TESR_GT7AdaptData.z): weight of the weather's sun/sky/ambient light in the white balance (GT7EnvWeight).
float4 AvgLumaGT7(VSOUT IN) : COLOR0
{
	float lightLimit = GT7LightLimit();
	float4 state = tex2D(TESR_AvgLumaBuffer, center);
	bool initialised = state.a > 50.0;

	// Measurement: 10x10 grid over the screen, log-average with outliers (the sun, bright sky, black corners) rejected.
	float logSum = 0.0;
	for (float i = 0.05; i < 1; i += 0.1) {
		for (float j = 0.05; j < 1; j += 0.1) {
			logSum += log2(max(luma(linearize(tex2D(TESR_RenderedBuffer, screenToRect(float2(i, j))).rgb)), 1e-4));
		}
	}
	float mean = logSum / 100.0;

	// Camera basis and field of view (world directions; z is up).
	float3 R = normalize(float3(TESR_ViewTransform[0][0], TESR_ViewTransform[1][0], TESR_ViewTransform[2][0]));
	float3 U = normalize(float3(TESR_ViewTransform[0][1], TESR_ViewTransform[1][1], TESR_ViewTransform[2][1]));
	float3 F = normalize(float3(TESR_ViewTransform[0][2], TESR_ViewTransform[1][2], TESR_ViewTransform[2][2]));
	float tanA = 1.0 / TESR_ProjectionTransform[0][0], tanB = 1.0 / TESR_ProjectionTransform[1][1];
	float cell = (2.0 * tanA / 10.0) * (2.0 * tanB / 10.0);
	bool fieldPossible = TESR_ShadowFormatData.z > 0.5 && TESR_ShadowFade.y > 0.5;

	float measured = 0.0, plain = 0.0;
	float screenWeight = 0.0;
	float3 frameXYZ = 0.0;
	float skySum = 0.0, skyCount = 0.0;
	float litSum = 0.0, litCount = 0.0, shadeSum = 0.0, shadeCount = 0.0;
	[loop] for (int ki = 0; ki < 10; ki++) {
		[loop] for (int li = 0; li < 10; li++) {
			float k = ki * 0.1 + 0.05, l = li * 0.1 + 0.05;
			float2 uv = screenToRect(float2(k, l));
			float3 c = linearize(tex2Dlod(TESR_RenderedBuffer, float4(uv, 0.0, 0.0)).rgb);
			float y = max(luma(c), 1e-4);
			float clamped = clamp(log2(y), mean - GT7AE_REJECT_DARK, mean + GT7AE_REJECT_LIGHT);
			plain += clamped;
			frameXYZ += mul(GT7_RGB2XYZ, c * (exp2(clamped) / y));  // colour kept, outlier brightness clamped

			// Projected solid angle of this sample (cos^4 of its angle to the line of sight, for a flat screen).
			float2 ndc = float2(k * 2.0 - 1.0, 1.0 - l * 2.0);
			float3 ray = F + R * (ndc.x * tanA) + U * (ndc.y * tanB);
			float cosT = rsqrt(dot(ray, ray));
			float w = cosT * cosT * cosT * cosT * cell;
			measured += clamped * w;
			screenWeight += w;

			float depth01 = tex2Dlod(TESR_DepthBuffer, float4(uv, 0.0, 0.0)).x;
			float isSky = depth01 >= GT7AE_SKY_DEPTH ? 1.0 : 0.0;
			skySum += isSky * min(log2(y), lightLimit + GT7AE_SKY_MAX);
			skyCount += isSky;
			[branch] if (fieldPossible && isSky < 0.5) {
				float lit = GT7SunVisible(ray * (depth01 * farZ)) > 0.5 ? 1.0 : 0.0;
				litSum += lit * clamped;   litCount += lit;
				shadeSum += (1.0 - lit) * clamped;   shadeCount += 1.0 - lit;
			}
		}
	}
	measured /= max(screenWeight, 1e-6);
	plain /= 100.0;
	frameXYZ /= 100.0;

	float field;
	if (GT7FF_ENABLED && GT7FullField(measured, screenWeight, litSum, litCount, shadeSum, shadeCount, R, U, F, tanA, tanB, lightLimit, field)) {
		measured = field;
	}
	else {
		// Fallback (interiors, sun shadows off): the screen, with the sky on screen as a second reference.
		float skyWeight = GT7AE_SKY_WEIGHT * saturate(skyCount / (100.0 * GT7AE_SKY_FULL));
		float skyReference = skySum / max(skyCount, 1.0) - GT7AE_SKY_OFFSET;
		measured = lerp(plain, skyReference, skyWeight);
	}

	float dt = clamp(TESR_GameTime.w, 0.0, 0.25);
	float darkLimit = lightLimit - GT7AE_LONG_RANGE;

	// Long-term adaptation: within the dark and light limits, slow; adapting to brighter is faster.
	float longTerm = initialised ? state.g : clamp(measured, darkLimit, lightLimit);
	float longTarget = clamp(measured, darkLimit, lightLimit);
	float longTau = longTarget > longTerm ? GT7AE_TAU_LONG_LIGHT : GT7AE_TAU_LONG_DARK;
	longTerm += (longTarget - longTerm) * (1.0 - exp(-dt / longTau));

	// Short-term adaptation: limited to a band around the long-term state and to the whole range.
	float shortTerm = initialised ? state.r : longTerm;
	float shortTarget = clamp(measured, max(longTerm - GT7AE_SHORT_DARK, darkLimit), min(longTerm + GT7AE_SHORT_LIGHT, lightLimit));
	float shortTau = shortTarget > shortTerm ? GT7AE_TAU_SHORT_LIGHT : GT7AE_TAU_SHORT_DARK;
	shortTerm += (shortTarget - shortTerm) * (1.0 - exp(-dt / shortTau));

	// White balance: luminance-weighted references -> colour temperature, adapted gradually.
	float envWeight = GT7EnvWeight();
	float3 xyzSum = frameXYZ * GT7WB_W_FRAME
		+ GT7EnvironmentXYZ(TESR_SunColor.rgb, TESR_SunDirection.z, TESR_SkyColor.rgb, TESR_SunAmbient.rgb, envWeight);
	float targetMired = 1e6 / clamp(GT7XYtoCCT(GT7LightXY(xyzSum)), GT7WB_MIN_K, GT7WB_MAX_K);
	float mired = initialised ? state.a : targetMired;
	mired += (targetMired - mired) * (1.0 - exp(-dt / GT7WB_TAU));

	float seed = TESR_GameTime.z + TESR_GameTime.x * 0.001;
	return float4(GT7StochasticHalf(shortTerm, GT7Hash(seed)),
		GT7StochasticHalf(longTerm, GT7Hash(seed + 1.7)),
		getFocalDistance(),
		GT7StochasticHalf(mired, GT7Hash(seed + 3.1)));
}

technique
{
	pass
	{
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 AvgLuma();
	}
}

// UNOFFICIAL technique 1: GT7 adaptation ([Shaders.Exposure.Main] Mode 1; AvgLuma.cpp / EffectChain.cpp pick it).
technique GT7
{
	pass
	{
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 AvgLumaGT7();
	}
}
