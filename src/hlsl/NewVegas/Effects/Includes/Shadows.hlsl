static const float BIAS = 0.018;

float GetPointLightAmountValue(samplerCUBE shadow_cube_map_buffer, float3 light_dir, float distance) {
	if (TESR_ShadowFade.z == 0) return 1;

	float light_depth = texCUBE(shadow_cube_map_buffer, light_dir).r;
	float shadow = light_depth + BIAS * distance > distance; // increase BIAS with distance

	// ignore shadow sample if lightDepth is not within range of cube map or if pointLights are disabled (ShadowFade.z set to 0)
	return lerp(1, shadow, light_depth > 0.0f && light_depth < 1.0f);
}

float GetPointLightAtten(float3 light_dir, float distance, float4 normal) {

	// radius based attenuation based on https://lisyarus.github.io/blog/graphics/2022/07/30/point-light-attenuation.html
	float s = saturate(distance * distance); 
	float falloff_v = saturate(((1 - s) * (1 - s)) / (1 + 5.0 * s));

	light_dir = normalize(light_dir); // normalize
	float lambert = lerp(1, dot(light_dir, normal.xyz), smoothstep(0, 0.2, distance)); // add some light bleeding at very short distance

	return saturate(lambert * falloff_v);
}

// returns a point light contribution using a shadow map
float GetPointLightAmount(samplerCUBE shadow_cube_map_buffer, float4 world_pos, float4 light_pos, float4 normal) {
	if (!light_pos.w) return 0; // w is light radius.

	float3 light_dir = light_pos.xyz - world_pos.xyz;
	float3 light_uv = light_dir * float3(-1, -1, 1);

	float distance = length(light_dir) / light_pos.w; // normalize distance over light range

	float light_amount = GetPointLightAmountValue(shadow_cube_map_buffer, light_uv, distance) * GetPointLightAtten(light_dir, distance, normal);
	return saturate(light_amount);
}

// get the normalized distance between the light and a point as a ratio of the light radius
float4 GetPointLightDistance(float4 world_pos, float4 light_pos){
	float3 light_dir = light_pos.xyz - world_pos.xyz;
	float distance = length(light_dir) / light_pos.w; // normalize distance over light range
	return float4(light_dir, distance);
}

// returns a point light contribution with no shadow map sampling
float GetPointLightContribution(float4 worldPos, float4 light_pos, float4 normal) {
    float4 light_v = GetPointLightDistance(worldPos, light_pos);
    return GetPointLightAtten(light_v.xyz, light_v.w, normal);
}

float Linstep(float a, float b, float v) {
    return saturate((v - a) / (b - a));
}

// Reduces VSM light bleedning
float ReduceLightBleeding(float p_max, float amount_v) {
  // Remove the [0, amount] tail and linearly rescale (amount, 1].
    return Linstep(amount_v, 1.0f, p_max);
}

float ChebyshevUpperBound(float2 moments_v, float mean_v, float min_variance,
                          float light_bleeding_reduction) {
    // Compute variance
    float variance_v = moments_v.y - (moments_v.x * moments_v.x);
    variance_v = max(variance_v, min_variance);

    // Compute probabilistic upper bound
    float d = mean_v - moments_v.x;
    float p_max = variance_v / (variance_v + (d * d));

    p_max = ReduceLightBleeding(p_max, light_bleeding_reduction);

    // One-tailed Chebyshev
    return (mean_v <= moments_v.x ? 1.0f : p_max);
}

// UNOFFICIAL 2026-10-14 PCF ([Shaders.ShadowsExteriors.ShadowMaps] Mode 3; Shaders/Includes/Shadow.hlsl has the 3 x 3 the world
// uses): the atlas holds plain light-space depth; the 2 x 2 texels round the point compared with its depth and blended
// bilinearly. For the volumetric effects, which take many lookups each and blur them anyway.
float ShadowPCF2x2(sampler2D atlas_v, float2 uv, float eye_range, float texel_v) {
    float2 p = uv / texel_v - 0.5f;
    float2 base_col = floor(p);
    float2 f = p - base_col;
    float2 tmp_uv0 = (base_col + 0.5f) * texel_v;
    float4 d = float4(tex2Dlod(atlas_v, float4(tmp_uv0, 0.0f, 0.0f)).x, tex2Dlod(atlas_v, float4(tmp_uv0 + float2(texel_v, 0.0f), 0.0f, 0.0f)).x,
                      tex2Dlod(atlas_v, float4(tmp_uv0 + float2(0.0f, texel_v), 0.0f, 0.0f)).x, tex2Dlod(atlas_v, float4(tmp_uv0 + texel_v, 0.0f, 0.0f)).x);
    float4 lit = eye_range <= d ? 1.0f : 0.0f;
    return lerp(lerp(lit.x, lit.y, f.x), lerp(lit.z, lit.w, f.x), f.y);
}

float GetLightAmountValueVSM(float2 moments_v, float eye_range, float bias, float bleed_reduction) {
    return ChebyshevUpperBound(moments_v, eye_range, bias, bleed_reduction);
}

float2 GetEVSMExponents(in float positive_exponent, in float negative_exponent, in float format_bits) {
    const float max_exponent = format_bits == 0.0 ? 5.54f : 42.0f;

    float2 light_space_exponents = float2(positive_exponent, negative_exponent);

    // Clamp to maximum range of fp32/fp16 to prevent overflow/underflow
    return min(light_space_exponents, max_exponent);
}

// Applies exponential warp to shadow map depth, input depth should be in [0, 1]
float2 WarpDepth(float eye_range, float2 exponents_v) {
    // Rescale depth into [-1, 1]
    eye_range = 2.0f * eye_range - 1.0f;
    float pos = exp(exponents_v.x * eye_range);
    float neg_e = -exp(-exponents_v.y * eye_range);
    return float2(pos, neg_e);
}

float GetLightAmountValueEVSM2(float2 moments_v, float eye_range, float bias, float bleed_reduction, float format_bits) {
    float2 exponents_v = GetEVSMExponents(40.0f, 5.0f, format_bits);
    float2 warped_depth = WarpDepth(eye_range, exponents_v);

    // Derivative of warping at depth
    float2 depth_scale = bias * exponents_v * warped_depth;
    float2 min_variance = depth_scale * depth_scale;

    return ChebyshevUpperBound(moments_v, warped_depth.x, min_variance.x, bleed_reduction);
}

float GetLightAmountValueEVSM4(float4 moments_v, float eye_range, float bias, float bleed_reduction, float format_bits) {
    float2 exponents_v = GetEVSMExponents(40.0f, 5.0f, format_bits);
    float2 warped_depth = WarpDepth(eye_range, exponents_v);

    // Derivative of warping at depth
    float2 depth_scale = bias * exponents_v * warped_depth;
    float2 min_variance = depth_scale * depth_scale;

    float pos_contrib = ChebyshevUpperBound(moments_v.xz, warped_depth.x, min_variance.x, bleed_reduction);
    float neg_contrib = ChebyshevUpperBound(moments_v.yw, warped_depth.y, min_variance.y, bleed_reduction);
    
    return min(pos_contrib, neg_contrib);
}
