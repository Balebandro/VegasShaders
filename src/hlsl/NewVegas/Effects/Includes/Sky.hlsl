
float3 GetSunColor(float sun_height, float athmosphere_v, float day_time, float3 sun_color, float3 sunset_color ){
    sun_color = linearize(sun_color);
    sunset_color = linearize(sunset_color);
    
    float3 rgb_acc = (1 + sun_height) * sun_color; // increase suncolor strength with sun height
    float sun_set = saturate(pows(1 - sun_height, 8.0)) * day_time;
    rgb_acc += sunset_color.rgb * sun_set * athmosphere_v; // add extra red to the sun at sunsets
    return rgb_acc;
}

float3 GetSkyColor(float verticality_v, float athmosphere_v, float sun_height, float sun_influence, float sun_strength, float3 sky_color, float3 sky_low_color, float3 horizon_color, float3 sun_color){
    sky_color = linearize(sky_color);
    sky_low_color = linearize(sky_low_color);
    horizon_color = linearize(horizon_color);

    float is_day_time = smoothstep(0, 0.5, TESR_SunAmount.x);

    float3 rgb_acc = lerp(sky_low_color.rgb, sky_color.rgb, verticality_v); // fade from low sky to high sky with height
    rgb_acc = lerp(rgb_acc, horizon_color.rgb, saturate(athmosphere_v * (0.5 + sun_influence * 0.5))); // fade from base rgb_acc to horizon rgb_acc in athmosphere (stronger on the sun side)
    rgb_acc += lerp(0.0, sun_color * sun_influence * (1.0 - sun_height) * (((1.0 - verticality_v) + athmosphere_v)/2) * sun_strength, is_day_time);

    return rgb_acc;
}

// UNOFFICIAL 2026-10-26 [Shaders.Sky.Main] sky look: Shaders/Includes/Sky.hlsl SkyLook, line for line (the effects compile
// against this folder). Change one and change the other, and Sky.cpp SkyLook.
float3 SkyLook(float3 rgb_acc, float up_vec, float3 tint_rgb, float3 horizon_tint, float4 look_v) {
    if (look_v.x != 1.0f) rgb_acc = max(lerp(luma(rgb_acc).xxx, rgb_acc, look_v.x), 0.0f);
    float overhead_v = smoothstep(0.0f, look_v.y, saturate(up_vec));
    return rgb_acc * tint_rgb * lerp(horizon_tint, 1.0f, overhead_v);
}
