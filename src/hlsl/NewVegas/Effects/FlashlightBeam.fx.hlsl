// The flashlight's in-air shaft (wave 3, written from specs/FlashlightBeam.md, 2026-11-02). One technique, one pass: the view ray is
// marched through the light's cone into the half-resolution TESR_VolumetricBuffer, which the Flashlight composite adds.
// Output: rgb in-scatter (linear HDR), a transmittance. Miss or zero strength: exactly (0, 0, 0, 1).
// No early return: a miss has a zero-length segment, so every texture read is in uniform control flow.

float4 TESR_VolumetricControl;		// x strength (0 = off, also the composite gate), y phase g, z height falloff, w max march distance
float4 TESR_VolumetricData;			// xyz the march's light position (first person: moved), w first-person blend 0 to 1
float4 TESR_SpotLightPosition;		// xyz true light position, w range
float4 TESR_SpotLightColor;			// rgb colour, w dimmer
float4 TESR_SpotLightDirection;		// xyz axis, w outer cone angle in degrees
float4 TESR_SunColor;				// only its luminance is used
float4 TESR_ReciprocalResolution;	// 1 / full frame width and height (the noise pattern's pixel position)
float4x4 TESR_FlashLightViewProjTransform;	// light view x projection, D3D row convention

sampler2D TESR_DepthBuffer : register(s0) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
sampler2D TESR_SpotLightTexture : register(s1) < string ResourceName = "Effects\Flashlight.png"; > = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };

struct BeamVertexIn
{
	float4 pos : POSITION0;
	float2 uv : TEXCOORD0;
};

struct BeamVertexOut
{
	float4 pos : POSITION;
	float2 uv : TEXCOORD0;
};

#include "Includes/Helpers.hlsl"
#include "Includes/Depth.hlsl"

BeamVertexOut ScreenVS(BeamVertexIn IN)
{
	BeamVertexOut o = (BeamVertexOut)0.0f;
	o.pos = IN.pos;
	o.uv = IN.uv;
	return o;
}

// The segment [t0, t1] of the ray (origin v from the light, unit direction rd) that lies inside the outer cone (cos^2 = k, nappe p + q t >= 0),
// inside the range sphere and within [0, tMax]. Brackets may be loose; the samples still test the exact cone. t1 = t0 when the ray misses.
void BeamSegment(float p, float q, float s, float u, float k, float R, float tMax, out float t0, out float t1)
{
	float lo = 0.0f;
	float hi = tMax;
	// Range sphere: |v + t rd| <= R.
	float disc = u * u - (s - R * R);
	if (disc < 0.0f) {
		hi = -1.0f;
	} else {
		float sq = sqrt(disc);
		lo = max(lo, -u - sq);
		hi = min(hi, -u + sq);
	}
	// Cone: f(t) = qa t^2 + 2 qb t + qc >= 0 (between the roots when qa < 0; otherwise left loose).
	float qa = q * q - k;
	float qb = p * q - k * u;
	float qc = p * p - k * s;
	if (qa < 0.0f) {
		float dd = qb * qb - qa * qc;
		if (dd < 0.0f) {
			hi = -1.0f;
		} else {
			float sq2 = sqrt(dd);
			float r1 = (-qb - sq2) / qa;
			float r2 = (-qb + sq2) / qa;
			lo = max(lo, min(r1, r2));
			hi = min(hi, max(r1, r2));
		}
	}
	// Nappe: p + q t >= 0.
	if (abs(q) > 1e-6f) {
		float tn = -p / q;
		if (q > 0.0f) lo = max(lo, tn); else hi = min(hi, tn);
	} else if (p < 0.0f) {
		hi = -1.0f;
	}
	t0 = lo;
	t1 = max(hi, lo);
}

float4 BeamPS(BeamVertexOut IN) : COLOR0
{
	float2 uv = IN.uv;
	const float kSteps = 24.0f;

	// The view ray (not normalised by distance: its forward part is 1), and the scene distance along it.
	float3 viewRaw = toWorld(uv);
	float rayLen = max(length(viewRaw), 1e-6f);
	float3 rd = viewRaw / rayLen;
	float depth = readDepthLod(uv);	// view distance over the far distance, times far: game units
	float sceneDist = depth * rayLen;

	float3 C = TESR_CameraPosition.xyz;
	float3 L = TESR_VolumetricData.xyz;
	float R = max(TESR_SpotLightPosition.w, 1e-4f);
	float3 A = normalize(TESR_SpotLightDirection.xyz);
	float outerDeg = TESR_SpotLightDirection.w;
	float cosO = cos(radians(outerDeg));
	float cosI = cos(radians(outerDeg * 0.5f));
	float tMax = min(TESR_VolumetricControl.w, sceneDist);

	float3 v = C - L;
	float p = dot(v, A);
	float q = dot(rd, A);
	float s = dot(v, v);
	float u = dot(v, rd);
	float t0, t1;
	BeamSegment(p, q, s, u, cosO * cosO, R, tMax, t0, t1);
	float stepLen = (t1 - t0) / kSteps;

	// The first sample's offset: a fixed interleaved-gradient pattern of the full-frame pixel (static; not temporal).
	float2 pixel = uv / TESR_ReciprocalResolution.xy;
	float ign = frac(52.9829189f * frac(dot(pixel, float2(0.06711056f, 0.00583715f))));

	float g = TESR_VolumetricControl.y;
	float fp = saturate(TESR_VolumetricData.w);
	float heightFalloff = TESR_VolumetricControl.z;
	float groundZ = C.z - 128.0f;

	float transmittance = 1.0f;
	float3 scatter = 0.0f;
	[unroll]
	for (int i = 0; i < 24; i++) {
		float t = t0 + (float(i) + ign) * stepLen;
		float3 P = C + rd * t;
		float3 fromLight = P - L;
		float dl = max(length(fromLight), 0.001f);
		float3 dirL = fromLight / dl;	// light to sample
		float tc = saturate((dot(dirL, A) - cosO) / max(cosI - cosO, 1e-4f));
		float cone = tc * tc;
		float dR = dl / R;
		float sq = saturate(dR * dR);
		float att = saturate((1.0f - sq) * (1.0f - sq) / (1.0f + 5.0f * sq));
		float4 clip = mul(float4(P, 1.0f), TESR_FlashLightViewProjTransform);
		float2 cuv = float2(clip.x / max(clip.w, 1e-6f) * 0.5f + 0.5f, 0.5f - 0.5f * clip.y / max(clip.w, 1e-6f));
		float cookie = lerp(tex2D(TESR_SpotLightTexture, cuv).r, 1.0f, fp);
		float density = exp(-max(P.z - groundZ, 0.0f) * heightFalloff * 0.0005f);
		float cosS = dot(rd, -dirL);	// the view ray against the direction from the sample to the light
		float phase = (1.0f - g * g) / (12.566370614f * pow(max(1.0f + g * g - 2.0f * g * cosS, 1e-4f), 1.5f));
		float surfaceFade = smoothstep(0.0f, 256.0f, sceneDist - t);
		float sigma = density * cone * att * cookie * surfaceFade;
		transmittance *= exp(-density * 0.5f * stepLen * 0.001f);
		scatter += transmittance * sigma * phase * stepLen;
	}

	float3 lightColour = TESR_SpotLightColor.rgb * TESR_SpotLightColor.w / max(0.05f, dot(TESR_SunColor.rgb, float3(0.2126f, 0.7152f, 0.0722f)));
	float cameraFade = max(saturate((length(L - C) - 40.0f) / 60.0f), fp);
	float3 inscatter = scatter * (TESR_VolumetricControl.x * 0.0008f * cameraFade) * lightColour;

	// Strength 0 (off): exactly (0, 0, 0, 1).
	float on = TESR_VolumetricControl.x > 0.0f ? 1.0f : 0.0f;
	return float4(lerp(float3(0.0f, 0.0f, 0.0f), inscatter, on), lerp(1.0f, transmittance, on));
}

technique BeamMarch
{
	pass P0
	{
		VertexShader = compile vs_3_0 ScreenVS();
		PixelShader = compile ps_3_0 BeamPS();
		ZEnable = FALSE; ZWriteEnable = FALSE; AlphaBlendEnable = FALSE; AlphaTestEnable = FALSE;
		CullMode = NONE; ColorWriteEnable = 15;
	}
}
