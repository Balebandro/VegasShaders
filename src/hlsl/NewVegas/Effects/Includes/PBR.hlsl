// D
float GGX(float alpha, float ndot_h){
    float num_v = alpha * alpha;
    float denom_v = PI * pow(sqr(ndot_h) * (num_v - 1) + 1, 2);
    return num_v/max(denom_v, 0.0001);
}

// G1
float ShlickBeckmann(float ndot_x, float alpha){
    // float k = alpha / 2;
    float k = alpha * alpha * 0.797884560802865;
    return max(ndot_x, 0.0001)/max(ndot_x * (1 - k) + k, 0.0001);
}

// Smith
float GeometryShadowing(float alpha, float ndot_v, float n_dot_l){
    return ShlickBeckmann(ndot_v, alpha) * ShlickBeckmann(n_dot_l, alpha);
}

// F
float3 FresnelShlick(float3 reflectance_v, float3 half_vec, float3 eye_dir){
    return reflectance_v + (1 - reflectance_v) * pow(1 - shades(half_vec, eye_dir), 5.0);
}

float3 CookTorrance(float alpha, float3 fresnel_v, float ndot_v, float n_dot_l, float ndot_h){
    float3 num_v = GGX(alpha, ndot_h) * GeometryShadowing(alpha, ndot_v, n_dot_l) * fresnel_v;
    float denom_v = max(4 * ndot_v * n_dot_l, 0.0000001);
    return num_v/denom_v;
}

float3 BRDF(float roughness, float n_dot_l, float ndot_v, float ndot_h, float3 fresnel_v){
    return fresnel_v * CookTorrance(roughness, fresnel_v, ndot_v, n_dot_l, ndot_h);
}

float3 modifiedBRDF(float roughness, float n_dot_l, float ndot_v, float ndot_h, float3 fresnel_v){
    // BRDF without the fresnel term to get cleaner specular on water
    float3 BRDF = fresnel_v * CookTorrance(roughness, fresnel_v, ndot_v, n_dot_l, ndot_h);
    return BRDF * n_dot_l;
}

float3 PBR(float metallicness_v, float roughness, float3 base_rgb, float3 normal, float3 eye_dir, float3 light_dir, float3 light_color){
    float3 reflectance_v = lerp(float(0.04).rrr, base_rgb, metallicness_v);
    float3 half_vec = normalize(eye_dir + light_dir);
    float n_dot_l = max(shade(normal, light_dir), 0.0000001);
    float ndot_v = max(shade(normal, eye_dir), 0.0000001);
    float ndot_h = shades(normal, half_vec);

    float3 ks = FresnelShlick(reflectance_v, half_vec, eye_dir);
    float3 lambert_diffuse = (1 - metallicness_v) * (1 - ks) * base_rgb/PI;
    float3 spec_v = BRDF(roughness * roughness, n_dot_l, ndot_v, ndot_h, ks);
    return (lambert_diffuse + spec_v) * n_dot_l * light_color;
    // return (lambertDiffuse + spec) * NdotL * lerp(lightColor, albedo, metallicness);
}