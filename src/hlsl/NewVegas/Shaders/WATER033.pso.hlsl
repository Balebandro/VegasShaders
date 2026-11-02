// LOD autowater surface shader

uniform float4 EyePos : register(c1);
uniform float4 ShallowColor : register(c2);
uniform float4 DeepColor : register(c3);
uniform float4 ReflectionColor : register(c4);
uniform float4 FresnelRI : register(c5);
uniform float4 VarAmounts : register(c8);
uniform float4 FogParam : register(c9);
uniform float4 FogColor : register(c10);
uniform float4 SunDir : register(c12);
uniform float4 SunColor : register(c13);

uniform float4 TESR_WaveParams : register(c14);
uniform float4 TESR_WaterVolume : register(c15); // x: caustic strength, y:shoreFactor, w: turbidity, z: caustic strength S ?
uniform float4 TESR_WaterSettings : register(c16); // x: caustic strength, y:depthDarkness, w: turbidity, z: caustic strength S ?
uniform float4 TESR_GameTime : register(c17);
uniform float4 TESR_HorizonColor : register(c18);
uniform float4 TESR_SunDirection : register(c19);
uniform float4 TESR_WaterShorelineParams : register(c20);
uniform float4 TESR_DebugVar : register(c21);
uniform float4 TESR_WaterShallowColor : register(c22);
uniform float4 TESR_SunAmount : register(c23);
uniform float4 TESR_WaterLODColor : register(c24); // UNOFFICIAL LOD seam: the colour the closeup water fades to

uniform sampler2D ReflectionMap : register(s0);
uniform sampler2D RefractionMap : register(s1); //unused
uniform sampler2D NoiseMap : register(s2); //unused
uniform sampler2D DisplacementMap : register(s3); //unused
uniform sampler2D DepthMap : register(s4); //unused
uniform sampler2D TESR_samplerWater : register(s5) < string ResourceName = "Water\water_NRM.dds"; > = sampler_state { ADDRESSU = WRAP; ADDRESSV = WRAP; ADDRESSW = WRAP; MAGFILTER = ANISOTROPIC; MINFILTER = ANISOTROPIC; MIPFILTER = ANISOTROPIC; } ;

// UNOFFICIAL 2026-10-31 RT water reflections (Includes/Water.hlsl getRTWaterReflection)
#define RT_WATER
uniform float4 TESR_RTWaterData : register(c40); // x 1 = last frame's RT reflection holds the water, y 1 = no planar map this frame, z strength
uniform row_major float4x4 TESR_RTWaterReproject : register(c41); // camera-relative world now -> that frame's clip space
uniform sampler2D TESR_ReflectionsBuffer : register(s7) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };

#include "Includes/Helpers.hlsl"
#include "Includes/Water.hlsl"

PS_OUTPUT main(PS_INPUT pa) {
    PS_OUTPUT pr;

    float4 lin_sun_color = linearize(SunColor); //linearise
    float4 lin_shallow_color = linearize(ShallowColor); //linearise
    //float4 linShallowColor = linearize(TESR_WaterShallowColor); //linearise
    float4 lin_horizon_color = linearize(TESR_HorizonColor); //linearise

    float3 eye_vector = EyePos.xyz - pa.LTEXCOORD_0.xyz; // vector of camera position to point being shaded
    float3 eye_direction = normalize(eye_vector);         // normalized eye to world vector (for lighting)
    float flat_dist = length(eye_vector.xy);              // surface distance to eye
    float eye_range = length(eye_vector);                    // depth distance to eye

    // calculate fog coeffs
    float4 screen_pos = getScreenpos(pa);                // point coordinates in screen space for water surface
    float is_day_time = smoothstep(0, 0.5, TESR_SunAmount.x);
    float sun_luma = luma(lin_sun_color) * is_day_time;

    float3 surface_normal = getWaveTexture(pa, flat_dist, TESR_WaveParams).xyz;
    float refraction_coeff = ((saturate(flat_dist * 0.002) * (-4 + VarAmounts.w)) + 4);
    float4 reflection_pos = getReflectionSamplePosition(pa, surface_normal, refraction_coeff);
    float4 refl_c = getRTWaterReflection(pa, surface_normal, refraction_coeff, linearize(tex2Dproj(ReflectionMap, reflection_pos)), lin_horizon_color);

    float4 pix = linearize(TESR_WaterLODColor) * sun_luma; // UNOFFICIAL LOD seam: same constant as the closeup water's fade target
    // color = getDiffuse(surfaceNormal, TESR_SunDirection.xyz, eyeDirection, distance, linHorizonColor, color);
    pix = getFresnel(surface_normal, eye_direction, refl_c, TESR_WaveParams.w, pix);
    pix = getSpecular(surface_normal, TESR_SunDirection.xyz, eye_direction, lin_sun_color.rgb, pix);
    pix.a = 1;

    pix = delinearize(pix); //delinearise
    
    // Standard fog.
    float fog_strength = pow(1 - saturate((FogParam.x - eye_range) / FogParam.y), FresnelRI.y);
    
    pr.color_0.rgb = fog_strength * (FogColor.rgb - pix.rgb) + pix.rgb;
    pr.color_0.a = pix.a;

    return pr;	
};