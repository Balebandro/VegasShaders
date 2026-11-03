// Flashlight shaft: one scattering march per pixel along the view ray, written at half
// resolution into TESR_VolumetricBuffer. The Flashlight Combine pass adds that buffer to the
// frame, so the shaft gets the same tonemap and bloom as everything else.
//
// The march only walks the stretch of the ray that lies inside the cone. Marching the full
// ray spends nearly every sample in unlit air, and that average hides the beam.
//
// Known gap: no shadow map is read, so geometry between the lamp and a sample does not block
// the shaft. The Better Flashlight NVSE effect carries the same approach forward.

float4 TESR_SpotLightPosition;
float4 TESR_SpotLightColor;
float4 TESR_SpotLightDirection;
float4 TESR_SunColor;
float4 TESR_ReciprocalResolution;
float4 TESR_VolumetricControl;		// x strength, y anisotropy, z height falloff, w max distance
float4 TESR_VolumetricData;			// xyz march light position, w first person strength floor
float4x4 TESR_FlashLightViewProjTransform;

sampler2D TESR_DepthBuffer : register(s0) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
sampler2D TESR_SpotLightTexture : register(s1) < string ResourceName = "Effects\Flashlight.png"; > = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };

#include "Includes/Depth.hlsl"
#include "Includes/Helpers.hlsl"

#define VOL_STEPS 24

// Scales the scatter integral into the range of the surface light pool, so the Strength
// setting behaves like a multiplier around one.
static const float VOL_SCALE_K = 0.0008;
// Width of the band over which a sample fades as it nears the surface that ends the ray.
static const float VOL_FADE_RANGE = 256.0;


struct VSOUT { float4 clipPos : POSITION; float2 uvCoord : TEXCOORD0; };
struct VSIN  { float4 clipPos : POSITION0; float2 uvCoord : TEXCOORD0; };

VSOUT FrameVS(VSIN src) {
	VSOUT res = (VSOUT)0.0f;
	res.clipPos = src.clipPos;
	res.uvCoord = src.uvCoord;
	return res;
}

// Clip space to texture space: perspective divide, then map [-1,1] onto [0,1] with y flipped.
float4 projToUV(float4 q) {
	q.xyz /= q.w;
	q.x = q.x *  0.5f + 0.5f;
	q.y = q.y * -0.5f + 0.5f;
	return q;
}

// Henyey-Greenstein phase function; g > 0 favours forward scattering.
float phaseHG(float cosT, float gg) {
	float gSq = gg * gg;
	float num = 1.0 - gSq;
	float denom = max(1e-4, 1.0 + gSq - 2.0 * gg * cosT);
	return num / (4.0 * 3.14159265 * pow(denom, 1.5));
}

// Interleaved gradient noise. It jitters the first step so the fixed step count does not
// draw rings; the half resolution upsample smooths out the grain.
float ignNoise(float2 px) {
	float inner = frac(dot(px, float2(0.06711056, 0.00583715)));
	return frac(52.9829189 * inner);
}

// Returns the [tIn, tOut] span of the view ray that is inside the cone volume. The cone
// has its forward nappe, the range sphere caps it, and an endpoint is used when the ray
// begins or ends inside. rel is the ray origin measured from the apex. The span may be
// slightly generous: each sample still evaluates the exact cone and falloff, so the span
// only keeps steps from landing in empty air.
bool ConeSegment(float3 rel, float3 ray, float3 ax, float cosA, float range, float tLimit, out float tIn, out float tOut) {
	float cosSq = cosA * cosA;
	float dRA = dot(ray, ax);
	float rA  = dot(rel, ax);
	float dRR = dot(ray, rel);
	float rr  = dot(rel, rel);

	tIn = tLimit; tOut = 0.0;
	float tc, hc;
	float3 pc;

	float qa = dRA * dRA - cosSq;
	float qb = 2.0 * (dRA * rA - dRR * cosSq);
	float qc = rA * rA - rr * cosSq;
	float qd = qb * qb - 4.0 * qa * qc;
	if (abs(qa) > 1e-6 && qd > 0.0) {
		float root = sqrt(qd);
		tc = (-qb - root) / (2.0 * qa); hc = rA + tc * dRA;
		if (tc > 0.0 && tc < tLimit && hc > 0.0 && hc < range) { tIn = min(tIn, tc); tOut = max(tOut, tc); }
		tc = (-qb + root) / (2.0 * qa); hc = rA + tc * dRA;
		if (tc > 0.0 && tc < tLimit && hc > 0.0 && hc < range) { tIn = min(tIn, tc); tOut = max(tOut, tc); }
	}

	float dS = dRR * dRR - (rr - range * range);
	if (dS > 0.0) {
		float rootS = sqrt(dS);
		tc = -dRR - rootS; pc = rel + tc * ray; hc = dot(pc, ax);
		if (tc > 0.0 && tc < tLimit && hc > 0.0 && hc * hc > cosSq * dot(pc, pc)) { tIn = min(tIn, tc); tOut = max(tOut, tc); }
		tc = -dRR + rootS; pc = rel + tc * ray; hc = dot(pc, ax);
		if (tc > 0.0 && tc < tLimit && hc > 0.0 && hc * hc > cosSq * dot(pc, pc)) { tIn = min(tIn, tc); tOut = max(tOut, tc); }
	}

	if (rA > 0.0 && rA < range && rA * rA > cosSq * rr) tIn = 0.0;
	pc = rel + tLimit * ray; hc = dot(pc, ax);
	if (hc > 0.0 && dot(pc, pc) < range * range && hc * hc > cosSq * dot(pc, pc)) tOut = tLimit;

	return tOut > tIn;
}

float4 Volumetric(VSOUT IN) : COLOR0 {
	float strengthK = TESR_VolumetricControl.x;
	if (strengthK <= 0.0) return float4(0.0, 0.0, 0.0, 1.0);

	float phaseG   = TESR_VolumetricControl.y;
	float fallRate = TESR_VolumetricControl.z;
	float limitD   = TESR_VolumetricControl.w;

	float3 viewRay = toWorld(IN.uvCoord);
	float3 rayDir  = normalize(viewRay);
	float3 eyePos  = TESR_CameraPosition.xyz;

	float3 lampPos  = TESR_VolumetricData.xyz;
	float  fpFloor  = saturate(TESR_VolumetricData.w);
	float3 aimDir   = TESR_SpotLightDirection.xyz;
	float  lampRng  = TESR_SpotLightPosition.w;
	float  cosOuter = cos(radians(TESR_SpotLightDirection.w));
	float  cosInner = cos(radians(TESR_SpotLightDirection.w * 0.5));
	float  floorZ   = eyePos.z - 128.0;		// about the player's feet; dust sinks to here

	float surfDist = length(viewRay) * readDepth(IN.uvCoord);
	float marchEnd = min(surfDist, limitD);

	float segIn, segOut;
	if (!ConeSegment(eyePos - lampPos, rayDir, aimDir, max(cosOuter, 0.1), lampRng, marchEnd, segIn, segOut))
		return float4(0.0, 0.0, 0.0, 1.0);

	float dt = (segOut - segIn) / VOL_STEPS;
	float t  = segIn + ignNoise(IN.uvCoord / TESR_ReciprocalResolution.xy) * dt;

	float3 acc = 0;
	float  tr  = 1.0;
	[loop]
	for (int k = 0; k < VOL_STEPS; k++) {
		float3 sp     = eyePos + rayDir * t;
		float3 toLamp = lampPos - sp;
		float  dLamp  = length(toLamp);
		float3 lDir   = toLamp / max(dLamp, 0.001);

		float cone = pow(invlerps(cosOuter, cosInner, shades(aimDir, -lDir)), 2.0);
		float sq   = saturate((dLamp / lampRng) * (dLamp / lampRng));
		float fall = saturate(((1 - sq) * (1 - sq)) / (1 + 5.0 * sq));

		float4 lsc = projToUV(mul(float4(sp, 1), TESR_FlashLightViewProjTransform));
		float2 cookieCoord = lsc.xy;
		// The cookie is projected from the real lamp position, but in first person the march
		// starts from a shifted origin. Flatten the cookie there to hide that parallax error.
		float cookie = lerp(tex2D(TESR_SpotLightTexture, cookieCoord).r, 1.0, fpFloor);

		float dens = exp(-max(0.0, sp.z - floorZ) * fallRate * 0.0005);
		float ph   = phaseHG(dot(rayDir, -lDir), phaseG);

		float surfFade = smoothstep(0.0, VOL_FADE_RANGE, surfDist - t);
		float sig = dens * cone * fall * cookie * surfFade;
		acc += tr * sig * ph * dt;
		tr *= exp(-dens * 0.5 * dt * 0.001);
		t += dt;
	}

	// Colour, dimmer and night boost follow the surface pool's energy model. That keeps the
	// shaft at a fixed ratio to the pool while auto exposure moves between day and night.
	float sunInv = 1.0 / max(0.05, luma(TESR_SunColor));
	float3 lampCol = TESR_SpotLightColor.rgb * TESR_SpotLightColor.w * sunInv;

	// Scale the in-scatter down as the lamp nears the eye, so it cannot stack onto the surface
	// pool when the source sits on the camera. In first person the offset origin provides the
	// parallax, and fpFloor keeps this fade at the configured first person strength.
	float camFade = saturate((length(lampPos - eyePos) - 40.0) / 60.0);
	camFade = max(camFade, fpFloor);

	float3 shaft = acc * (strengthK * VOL_SCALE_K * camFade) * lampCol;
	return float4(shaft, tr);
}

technique {
	pass {
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader  = compile ps_3_0 Volumetric();
	}
}
