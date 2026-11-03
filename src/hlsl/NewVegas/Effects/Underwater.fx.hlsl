// Underwater post passes: quarter-resolution god rays, the water surface shade (caustics,
// depth fog, absorption and in-scatter) and the refraction wobble used behind the surface.
float4 TESR_ReciprocalResolution;
float4 TESR_SunDirection;
float4 TESR_GameTime;
float4 TESR_WaterCoefficients; // r g b scattering
float4 TESR_SunColor;
float4 TESR_FogColor;
// float4 TESR_FogData; // x: near fog distance, y:far fog distance, z: sun glare, w: fog power
float4 TESR_WaterFog; // x: near fog distance, y:far fog distance, z: sun glare, w: fog power
float4 TESR_WaterVolume; // x; causticsStrength, y:shore factor, z:turbidity, w: causticsStrengthS
float4 TESR_WaveParams;
float4 TESR_WaterSettings; // x: caustic strength, y:depthDarkness, w: refraction, z: caustic strength S ?
float4 TESR_WaterShallowColor; // Shallow color used by the game for water
float4 TESR_WaterDeepColor; // Deep color used by the game for water
float4 TESR_DebugVar;
float4 TESR_HorizonColor;
float4 TESR_SunAmount;
float4 TESR_SkyColor;

#include "Includes/Helpers.hlsl"

sampler2D TESR_RenderedBuffer : register(s0) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_SourceBuffer : register(s1) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_DepthBuffer : register(s2) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_WavesSampler : register(s3) < string ResourceName = "Water\water_NRM.dds"; > = sampler_state { ADDRESSU = WRAP; ADDRESSV = WRAP; ADDRESSW = WRAP; MAGFILTER = ANISOTROPIC; MINFILTER = ANISOTROPIC; MIPFILTER = ANISOTROPIC; } ;
sampler2D TESR_NormalsBuffer : register(s4) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = NONE; MINFILTER = NONE; MIPFILTER = NONE; };
sampler2D TESR_BlueNoiseSampler : register(s5) < string ResourceName = "Effects\bluenoise256.dds"; > = sampler_state { ADDRESSU = WRAP; ADDRESSV = WRAP; MAGFILTER = NONE; MINFILTER = NONE; MIPFILTER = NONE; };
sampler2D TESR_CausticsSampler : register(s6) < string ResourceName = "Water\caust_001.dds"; > = sampler_state { ADDRESSU = WRAP; ADDRESSV = WRAP; MAGFILTER = ANISOTROPIC; MINFILTER = ANISOTROPIC; MIPFILTER = ANISOTROPIC; };
sampler2D TESR_PointShadowBuffer : register(s7) = sampler_state { ADDRESSU = WRAP; ADDRESSV = WRAP; MAGFILTER = ANISOTROPIC; MINFILTER = ANISOTROPIC; MIPFILTER = ANISOTROPIC; };

static const float frame = TESR_GameTime.z * TESR_WaveParams.z * 1.5;
static const float4x4 ditherTable = {
	{ 0.0588, 0.5294, 0.1765, 0.6471 },
	{ 0.7647, 0.2941, 0.8824, 0.4118 },
	{ 0.2353, 0.7059, 0.1176, 0.5882 },
	{ 0.9412, 0.4706, 0.8235, 0.3259 }
};

static const float causticsGain = TESR_WaterVolume.x;
static const float shoreK = TESR_WaterVolume.y;
static const float murk = TESR_WaterVolume.z;
static const float rayGain = TESR_WaterVolume.w;
static const float3 extinction = TESR_WaterCoefficients.xyz;// * turbidity;
static const float scatterK = TESR_WaterCoefficients.w;// * turbidity;
static const float waveScale = TESR_WaveParams.y;
static const float darkK = TESR_WaterSettings.y;
static const float3 upAxis = float3(0, 0, 1);
static const float sunLum = luma(pows(TESR_SunColor.rgb,2.2)); // linearise

struct VSOUT {
	float4 clipPos : POSITION;
	float2 UVCoord : TEXCOORD0;
};

struct VSIN {
	float4 clipPos : POSITION0;
	float2 UVCoord : TEXCOORD0;
};

VSOUT FrameVS(VSIN src) {
	VSOUT res = (VSOUT)0.0f;
	res.clipPos = src.clipPos;
	res.UVCoord = src.UVCoord;
	return res;
}

#include "Includes/Depth.hlsl"
#include "Includes/Normals.hlsl"
#include "Includes/Blur.hlsl"
#include "Includes/Sky.hlsl"

// Blue-noise lookup in screen space; the 256 texel tile repeats across the frame.
float3 blueAt(float2 p)
{
	return tex2D(TESR_BlueNoiseSampler, (p/256 + 0.5) / TESR_ReciprocalResolution.xy).xyz;
}

// Sky radiance rebuilt for refraction, since the vanilla buffer already carries water tint.
// Blends horizon and zenith colours by view elevation, then adds the sun glow.
float3 skyFromView(float3 viewDir, float3 sunCol){
	float3 sky = lerp(linearize(TESR_HorizonColor).rgb, linearize(TESR_SkyColor).rgb, pow(shade(viewDir, float3(0, 0, 1)), 0.5));
	sky += sunCol * pows(shades(viewDir, TESR_SunDirection.xyz), 12);
	return sky;
}

float4 waveSample(float2 uv) {
	float2 origin = uv;

	float wScale = TESR_WaveParams.y;
	float chop = TESR_WaveParams.x;
	float drift = TESR_GameTime.x * 0.002 * TESR_WaveParams.z;
	float smallK = 0.5;
	float bigK = 2;
	float4 wA = expand(tex2D(TESR_WavesSampler, origin * smallK * wScale + normalize(float2(1, 4)) * drift));
	float4 wB = expand(tex2D(TESR_WavesSampler, origin * bigK * wScale + normalize(float2(-3, -2)) * drift));

	// merge the two octaves
	wA = float4(wB.xy + wA.xy,  1, 1);
	wA.xy *= chop;

	wA = normalize(wA);
	return wA;
}

// Caustic intensity at a world position, found by projecting the point up to the surface
// along the sun direction and sampling two drifting layers there.
float causticAt(float3 p){
	float dz = TESR_WaterSettings.z - p.z;
	p += (dz/TESR_SunDirection.z) * TESR_SunDirection.xyz;

	float drift = TESR_GameTime.x * 0.01 * TESR_WaveParams.z;
	float sc = 0.002;

	float l1 = tex2D(TESR_CausticsSampler, p.xy * sc  + drift * normalize(float2(-1.2, - 2.5))).r;
	float l2 = tex2D(TESR_CausticsSampler, p.xy * sc * 1.2  + drift * normalize(float2(0.5, 2))).r;

	return pows(min(l1, l2), 1.3) * 4.0; //strengthen the definition and strength of the caustics
}


float4 Godrays( VSOUT IN) : COLOR0 {
	// volumetric god rays rendered at quarter resolution
	float2 uvG = IN.UVCoord * 2;
	clip( (uvG <= TESR_RenderScale.xy) - 1);	// UNOFFICIAL 2026-10-27 RenderScale v2: a quarter of the drawn part

	float3 viewDir = toWorld(uvG);
	float sceneDist = length(readDepth(uvG) * viewDir);

	float maxRay = 1000;
	float stepLen = maxRay / 50;
	float3 stepVec = viewDir * stepLen;

	float4 acc = float4(0, 0, 0, 1);

	float3 samplePos = TESR_CameraPosition.xyz;
	[unroll]
	for (float k=1; k < 50; k++){
		samplePos += stepVec * lerp(0.5, 1, blueAt(uvG + TESR_ReciprocalResolution.xx * k));
		float dist = stepLen * k;
		float depthFadeS = smoothstep(800, 0, TESR_WaterSettings.x - samplePos.z);
		float distFadeS = 1 - dist/maxRay;
		float4 rayCol = lerp(float4(0, 0, 0, 1), float4(1, 1, 1, 1), distFadeS) * sunLum * 0.06;
		acc += causticAt(samplePos) * rayCol * depthFadeS * distFadeS * (dist < sceneDist);
	}

	return float4(smoothstep(0.1, 0.4, acc).rgb, 1); // clamp the values to increase the strength and improve blending
}


float aboveWater(float eyeZ, float pointZ, float waterZ, float d){
	if (eyeZ < waterZ - 4.0) return false;
	return eyeZ > (waterZ + 4.0) || pointZ > (waterZ - 0.6);
}


float4 Water( VSOUT IN ) : COLOR0 {
	float d0 = readDepth(IN.UVCoord);
	float3 rayV = toWorld(IN.UVCoord);
	float3 vdir = normalize(rayV);
	float3 wpos = TESR_CameraPosition.xyz + rayV * d0;
	float fogD = length(rayV * d0); // a depth measure that's at the same distance from the camera at all angles
	float wDepth = TESR_WaterSettings.x - wpos.z;
	float3 floorN = GetWorldNormal(IN.UVCoord);

	float4 col = tex2D(TESR_SourceBuffer, IN.UVCoord);
	if (aboveWater(TESR_CameraPosition.z, wpos.z, TESR_WaterSettings.x, d0)) return col;

	col = linearize(col); //linearise

	// vertical exponential depth fog used to darken bottom
	float dens = 0.08;
	float falloffK = 0.00225; // tie to darkness setting?
	float fogAmt = saturate((dens/falloffK) * exp(-TESR_CameraPosition.z*falloffK) * (1.0 - exp( -fogD*vdir.z*falloffK ))/vdir.z);

	float3 deepCol = linearize(TESR_WaterDeepColor).rgb;
	float3 shallowCol = linearize(TESR_WaterShallowColor).rgb;
	float3 sunC = linearize(TESR_SunColor).rgb;

	// horizontal scattering
	float3 skyC = skyFromView(vdir, sunC);

	// interpolate fog color between shallow and deep water based on viewing angle. Shallow waters get tinted with the sky
	float vFade = saturate(compress(dot(vdir, upAxis) * 1.5)); // fade from deep to shallow based on viewing angle
	float sunFade = compress(dot(vdir.xyz, TESR_SunDirection.xyz)); // fade based on proximity to the sun angle

	float3 fogC = lerp(deepCol, shallowCol + skyC * 0.2, vFade); // mix in some sky color into shallow fog
	fogC *= sunLum * (0.4 + (1 - 0.4) * sunFade) * extinction; //scale fog brightness with sunluma/sun direction and apply user setting modifiers
	float3 scat = (1 - fogC) * scatterK; // lerp colors at different speed
	float linFog = saturate(invlerp(TESR_WaterFog.x, TESR_WaterFog.y, fogD));

	// fading parameters for caustics and god rays
	float floorAng = (smoothstep(0.5,1, dot(floorN, upAxis)) + smoothstep(0.8,1, dot(floorN, TESR_SunDirection.xyz))) / 2;
	float dFade = smoothstep(800, 0, wDepth);
	float distFade = invlerp(6000, 0, fogD);

	float2 shad = tex2D(TESR_PointShadowBuffer, IN.UVCoord).rg; // only sample point light shadows
	float caus = causticAt(wpos) * shad.r;

	col.rgb += col * pows(caus, 2.0) * sunC * dFade * floorAng * distFade * sunLum * causticsGain * 100;

	col.rgb = lerp(col.rgb, fogC, saturate(pow(invlerp(0, 5000, fogD), 10))); // lod hiding

	// blend fog layers colors
	col.rgb *= lerp(1, deepCol, saturate(fogAmt * pows(darkK, 0.5)) * ((1 - shad.g * distFade))); // surface light absorption

	float fogThick = saturate( 2 - pows(murk, 0.5) * 2);
	float murkPow = (murk * 3) / (1 +  3 * murk);
	col.rgb = lerp(col.rgb, fogC, saturate(scat * pows(linFog, fogThick) * murkPow)); // scattering absorption

	// blend in godrays
	col.rgb += tex2D(TESR_RenderedBuffer, IN.UVCoord / 2).r * sunC * rayGain * col.rgb;

	// reduce banding
	float2 uvQ = IN.UVCoord.xy / TESR_ReciprocalResolution.xy;
	col = delinearize(col);
	col.rgb += ditherTable[ (uvQ.x)%4 ][ (uvQ.y)%4 ] / 255;

	return float4(col.rgb, 1);
}

float4 WaterDistortion( VSOUT IN ) : COLOR0 {

	float d0 = readDepth(IN.UVCoord);
	float3 rayV = toWorld(IN.UVCoord);
	float3 wpos = TESR_CameraPosition.xyz + rayV * d0;
	float4 col = tex2D(TESR_SourceBuffer, IN.UVCoord);
	if (aboveWater(TESR_CameraPosition.z, wpos.z, TESR_WaterSettings.x, d0)) return col;

	// UNOFFICIAL 2026-10-27 RenderScale v2: the same wobble across the picture in rect frames (phase from the screen position)
	float2 s01 = rectToScreen(IN.UVCoord);
	IN.UVCoord.x += sin(frame * 3 + s01.x * 60) * 0.001f * TESR_RenderScale.x;
	IN.UVCoord.y += cos(frame * 3 + s01.y * 60) * 0.001f * TESR_RenderScale.y;
	col = tex2D(TESR_RenderedBuffer, IN.UVCoord);
	col.a = 1;
	return col;
}

technique
{
	pass
	{
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 Godrays();
	}
	pass
	{
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 Blur(TESR_RenderedBuffer, OffsetMaskV, 2, 0.5);
	}
	pass
	{
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 Blur(TESR_RenderedBuffer, OffsetMaskH, 2, 0.5);
	}

	pass
	{
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 Water();
	}

	pass
	{
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 WaterDistortion();
	}
}
