// Sky colour model shared by the sky and object shaders. Colours arrive in sRGB (game values) and are worked in linear.

// Sun disc colour: brighter as the sun climbs, plus the sunset colour near the horizon (only by day, scaled by the haze amount).
float3 GetSunColor(float elevation, float haze, float dayFactor, float3 sunTint, float3 sunsetTint ){
    float3 sunLin = linearize(sunTint);
    float3 sunsetLin = linearize(sunsetTint);

    float3 result = (1 + elevation) * sunLin;
    float lowSun = saturate(pows(1 - elevation, 8.0)) * dayFactor;
    result += sunsetLin.rgb * lowSun * haze;
    return result;
}

// Sky dome colour for one view direction: low-to-high gradient by verticality, pulled toward the horizon colour where the haze is
// (more on the sun's side), and by day a sun glow that fades out as the sun rises.
float3 GetSkyColor(float verticality, float haze, float elevation, float sunSide, float glowStrength, float3 skyTop, float3 skyLow, float3 skyHorizon, float3 sunTint){
    float3 topLin = linearize(skyTop);
    float3 lowLin = linearize(skyLow);
    float3 horizonLin = linearize(skyHorizon);

    float byDay = smoothstep(0, 0.5, TESR_SunAmount.x);

    float3 result = lerp(lowLin.rgb, topLin.rgb, verticality);
    result = lerp(result, horizonLin.rgb, saturate(haze * (0.5 + sunSide * 0.5)));
    result += lerp(0.0, sunTint * sunSide * (1.0 - elevation) * (haze) * glowStrength, byDay);
    return result;
}

// UNOFFICIAL 2026-10-26: the true world direction of a sky pixel, for SkyLook's horizon fade. The sky shaders' own eye direction is
// the dome mesh's vertex position, and the dome sits so that even the visible horizon has a large up component (the horizon tint
// never showed). Rebuilt from the pixel and the camera: the shader declares TESR_ReciprocalResolution, TESR_InvProjectionTransform
// and TESR_InvViewTransform (row_major). Only the main view's camera: in the water reflection the fade is approximate.
#ifdef SKY_VIEW_DIRECTION
// UNOFFICIAL 2026-10-27 RenderScale v2 (user: a line in the sky that grows and shrinks with RenderScale): with RenderScale below 1 the
// world (the sky with it) is drawn into the top-left TESR_RenderScale.xy of the screen, so the pixel's share of the screen is its
// share of that part (x zw); off, zw = 1.
float3 SkyViewDirection(float2 vpos) {
    float2 s01 = (vpos + 0.5f) * TESR_ReciprocalResolution.xy * TESR_RenderScale.zw;
    float2 ndc = float2(s01.x * 2.0f - 1.0f, 1.0f - s01.y * 2.0f);
    float4 view = mul(float4(ndc, 0.5f, 1.0f), TESR_InvProjectionTransform);
    return normalize(mul(float4(view.xyz / view.w, 0.0f), TESR_InvViewTransform).xyz);
}
#endif

// UNOFFICIAL 2026-10-26 [Shaders.Sky.Main] sky look, for SKY.pso and the sky the flat clouds blend with (SKYTEX.pso). Sky.cpp SkyLook
// mirrors it for the sky light on objects: change one and change the other. Saturation first (look.x, 1 = no change), then the tint,
// which is the main tint times the horizon tint at the horizon, fading to the main tint alone at look.y (TintGradientHeight, the view
// direction's up component). With TintKeepsBrightness on, Sky.cpp hands both tints over already divided by their own brightness.
float3 SkyLook(float3 color, float up, float3 tint, float3 horizonTint, float4 look) {
    if (look.x != 1.0f) color = max(lerp(luma(color).xxx, color, look.x), 0.0f);
    float overhead = smoothstep(0.0f, look.y, saturate(up));
    return color * tint * lerp(horizonTint, 1.0f, overhead);
}