
// Must match [_Shaders.ShadowsExteriors.ShadowMaps] Mode, and SHADOW_FIXED_MODE in
// Shaders/Includes/Shadow.hlsl which decodes what this writes.
//   0 = VSM, 1 = EVSM2, 2 = EVSM4 (toml default)
#ifndef SHADOW_FIXED_MODE
    #define SHADOW_FIXED_MODE 3   // UNOFFICIAL 2026-10-27: PCF always (ShaderRecord defines 3 for every shader)
#endif

uniform float4 TESR_ShadowData : register(c0);
uniform float4 TESR_ShadowFormatData : register(c1);

uniform sampler2D DiffuseMap : register(s0) = sampler_state { ADDRESSU = WRAP; ADDRESSV = WRAP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = POINT; };

static const float format_bits = TESR_ShadowFormatData.y;

float2 GetEVSMExponents(in float positive_exponent, in float negative_exponent) {
    const float max_exponent = format_bits == 0.0 ? 5.54f : 42.0f;

    float2 light_space_exponents = float2(positive_exponent, negative_exponent);

    // Clamp to maximum range of fp32/fp16 to prevent overflow/underflow
    return min(light_space_exponents, max_exponent);
}

// Applies exponential warp to shadow map depth, input depth should be in [0, 1]
float2 WarpDepth(float eye_range, float2 loc_exponents) {
    // Rescale depth into [-1, 1]
    eye_range = 2.0f * eye_range - 1.0f;
    float pos_w = exp(loc_exponents.x * eye_range);
    float neg_e = -exp(-loc_exponents.y * eye_range);
    return float2(pos_w, neg_e);
}

struct VtxVaryings {
    float4 f_texcoord_0 : TEXCOORD0;
	float4 f_texcoord_1 : TEXCOORD1;
};

struct PixResult {
    float4 f_color_0 : COLOR0;
};

PixResult main(VtxVaryings pa) {
    PixResult pr;

	if (TESR_ShadowData.y == 1.0f)
	{ // Leaves (Speedtrees) or alpha is required
		float4 lambert = tex2D(DiffuseMap, pa.f_texcoord_1.xy);
        if (lambert.a < 0.5f)
            discard;
    }
	
	float eye_range = pa.f_texcoord_0.z / pa.f_texcoord_0.w;

	// Mode is a session-wide setting, so it is resolved at COMPILE time. As a runtime branch
	// ps_3_0 flattened it, and every shadow-map texel evaluated VSM, EVSM2 and EVSM4 before
	// discarding two of them -- 40 instruction slots, down to 24 once folded.
	// TESR_ShadowData.z stays a runtime test: it marks the ortho map, which is rendered in the
	// same frame as the cascades and wants plain depth.
	if (TESR_ShadowData.z)
	{
		// Ortho map: only depth.
		pr.f_color_0 = float4(eye_range, 0.0f, 0.0f, 1.0f);
	}
	else {
#if 0 == SHADOW_FIXED_MODE
		// VSM. Cheat to reduce shadow acne in variance maps.
		float dx_v = ddx(eye_range);
		float dy_v = ddy(eye_range);
		float loc_moment2 = eye_range * eye_range + 0.25 * (dx_v * dx_v + dy_v * dy_v);
		pr.f_color_0 = float4(eye_range, loc_moment2, 0.0f, 1.0f);
#elif 1 == SHADOW_FIXED_MODE
		// EVSM2
		float2 loc_exponents = GetEVSMExponents(40.0f, 5.0f);
		float2 loc_evsm2 = WarpDepth(eye_range, loc_exponents);
		pr.f_color_0 = float4(loc_evsm2, 0.0f, 1.0f);
#elif SHADOW_FIXED_MODE == 3
		// UNOFFICIAL PCF: the depth alone, compared at lookup (Shadow.hlsl ShadowPCF3x3).
		pr.f_color_0 = float4(eye_range, 0.0f, 0.0f, 1.0f);
#else
		// EVSM4
		float2 loc_exponents = GetEVSMExponents(40.0f, 5.0f);
		float2 loc_evsm2 = WarpDepth(eye_range, loc_exponents);
		pr.f_color_0 = float4(loc_evsm2, loc_evsm2 * loc_evsm2);
#endif
	}

	return pr;	
};