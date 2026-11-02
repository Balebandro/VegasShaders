// UNOFFICIAL 2026-10-30: the march's bindings and MarchRay, shared by VolumetricCloudsMarch.frag and VolumetricCloudsSkyProbe.frag
// (moved here unchanged from VolumetricCloudsMarch.frag).
layout(set = 1, binding = 0) uniform texture2D t_weather;    // TESR_CloudWeather (LINEAR, WRAP, LINEAR mips)
layout(set = 1, binding = 1) uniform texture3D t_shape;      // TESR_CloudShape (LINEAR, WRAP, LINEAR mips)
layout(set = 1, binding = 2) uniform texture3D t_detail;     // TESR_CloudDetail (LINEAR, WRAP, LINEAR mips)
layout(set = 1, binding = 3) uniform texture3D t_noise;      // TESR_NoiseVolume (LINEAR, WRAP, LINEAR mips)
layout(set = 1, binding = 4) uniform texture2D t_blueNoise;  // TESR_BlueNoiseSampler (POINT, WRAP)
layout(set = 1, binding = 5) uniform texture2D t_skyView;    // TESR_SkyViewLUT (LINEAR, CLAMP; a stand-in, never read, without PHYSICAL_SKY_ON)

#define CLOUDS_DENSITY
#include "VolumetricClouds.glsl"

float Phase(float cosT, float g) {
    return (1.0 - g * g) / pow(max(1.0 + g * g - 2.0 * g * cosT, 1e-4), 1.5);
}

vec4 MarchRay(vec3 dir, float jitter, float stepScale, bool fixedExit, bool towers) {
    float steps = max(Steps * stepScale, 4.0);
    // (2026-10-25 Towering, as the .fx) the storm techniques march with at least the 64 steps the D3D9 loop allows. UNOFFICIAL 2026-10-27
    // (user: towering clouds noisy at Steps 256, "changing cloud steps seems to do nothing"): it was ALWAYS 64, whatever Steps said; here
    // (no loop limit) Steps counts (the CPU's count, already a quarter more for each extra layer depth).
    if (towers) steps = max(max(Steps, 64.0) * stepScale, 4.0);
    vec3 camera = cb.cameraPosition.xyz * MetresPerUnit;
    float baseAbove = max(Base - camera.z, 50.0);
    float tStart = ShellDistance(dir.z, baseAbove);
    float tEnd = ShellDistance(dir.z, baseAbove + Thickness);
    if (dir.z < -0.15 || tStart > MaxDistance) return vec4(0.0, 0.0, 0.0, 1.0);
    tEnd = min(tEnd, tStart + 4.0 * Thickness + 8000.0);
    float dt = (tEnd - tStart) / steps;

    vec3 light = normalize(cb.sunDirection.xyz);
    float day = saturate(cb.sunAmount.x);
    float cosT = dot(dir, light);
    vec3 sun = linearize(cb.sunColor.rgb) * SunStrength * mix(0.15, 1.0, day) * (1.0 - 0.6 * RainDark);
    vec3 skyTop = linearize(cb.skyColor.rgb);
    vec3 skyLow = linearize(cb.horizonColor.rgb);
    vec3 ambientTop = skyTop * AmbientStrength * (1.0 - 0.4 * RainDark);
    vec3 ambientBottom = mix(skyLow, skyTop, 0.5) * AmbientStrength * 0.5 * (1.0 - 0.6 * RainDark);
    // #if PHYSICAL_SKY_ON
    if (pc.physicalSky != 0u && cb.physSky.x > 0.0) {
        float share = cb.physSky.x, tint = cb.physSky.z, tableScale = 0.001 * cb.physSky.y;
        vec3 through = exp(-AtmSunDepth(AtmGround + Base + 0.5 * Thickness, cb.physSkySun.z, cb.physSky.w));
        vec3 physSun = luma(sun) * through / max(luma(through), 1e-4);
        sun = mix(sun, mix(physSun, sun, tint), share);
        vec3 zenith = LINEAR_LOD(t_skyView, vec2(0.5, 1.0)).rgb * tableScale;
        vec3 low = (LINEAR_LOD(t_skyView, vec2(0.0, 0.35)).rgb + LINEAR_LOD(t_skyView, vec2(0.5, 0.35)).rgb +
            LINEAR_LOD(t_skyView, vec2(1.0, 0.35)).rgb) * (tableScale / 3.0);
        zenith *= luma(skyTop) / max(luma(zenith), 1e-6);
        low *= luma(skyLow) / max(luma(low), 1e-6);
        vec3 physTop = mix(zenith, skyTop, tint);
        vec3 physLow = mix(low, skyLow, tint);
        ambientTop = mix(ambientTop, physTop * AmbientStrength * (1.0 - 0.4 * RainDark), share);
        ambientBottom = mix(ambientBottom, mix(physLow, physTop, 0.5) * AmbientStrength * 0.5 * (1.0 - 0.6 * RainDark), share);
    }
    float lightStep = (towers ? BaseThickness : Thickness) / 12.0;   // (Towering: the ordinary clouds keep their light march)
    float lightRise = max(light.z, 0.1);

    vec3 wind = vec3(cb.wind.xy, 0.0);
    vec3 scattered = vec3(0.0);
    float transmittance = 1.0;
    float depthSum = 0.0, depthWeight = 0.0;
    // The D3D9 technique March never leaves this loop early: the compiler writes its 'break' as 'break_ne -c, -c' (the same value
    // on both sides, never taken; checked with fxc), so every ray runs all 64 steps. That is kept here (no exit unless fixedExit;
    // or towers, since 2026-10-27).
    // UNOFFICIAL 2026-10-26: Steps above 64 (Vulkan only: no unrolled loop here) march that many; up to 64 this is the D3D9 loop.
    // (2026-10-27 Towering, as the .fx) the storm techniques always take the exit: a ray stops once the cloud is opaque.
    float loopEnd = max(64.0, steps);
    for (float i = 0.0; i < loopEnd && (!(fixedExit || towers) || !(i >= steps || transmittance < 0.01)); i += 1.0) {
        float t = tStart + (i + jitter) * dt;
        float altitude = camera.z + t * dir.z + t * t / (2.0 * EarthRadius);
        float h = (altitude - Base) / Thickness;
        vec3 p = vec3(camera.xy + dir.xy * t, altitude) + wind;
        float lod = saturate(t / 40000.0) * 2.0 * (1.0 - 0.5 * saturate(Sharpness));
        float density = CloudDensity(p, h, lod, false, towers);
        if (density > 0.0) {
            float opticalDepth = 0.0;
            float reach = 0.0;
            if (towers) {
                // (the .fx's real loop for the towers: the same six samples)
                float stepLength = lightStep * 0.5;
                for (float j = 0.0; j < 6.0; j += 1.0) {
                    reach += stepLength;
                    vec3 q = p + light * reach;
                    float hq = h + reach * lightRise / Thickness;
                    opticalDepth += CloudDensity(q, hq, lod + 1.0, j > 2.5, true) * stepLength;
                    stepLength *= 2.0;
                }
            }
            else {
                for (int j = 0; j < 6; j++) {
                    float stepLength = lightStep * exp2(float(j)) * 0.5;
                    reach += stepLength;
                    vec3 q = p + light * reach;
                    float hq = h + reach * lightRise / Thickness;
                    opticalDepth += CloudDensity(q, hq, lod + 1.0, j > 2, false) * stepLength;
                }
            }
            // (Towering) light scatters on through a storm: from half way up the ordinary layer the sun's way in counts for less.
            if (towers) opticalDepth *= 1.0 - 0.8 * Towering * saturate((h * InvTowerShare - 0.5) / 1.5);
            vec3 sunLight = vec3(0.0);
            float a = 1.0, b = 1.0, c = 1.0;
            for (int k = 0; k < 3; k++) {
                float g = Forward * c;
                sunLight += b * exp(-Extinction * opticalDepth * a) * mix(Phase(cosT, g), Phase(cosT, -0.3 * c), 0.5);
                a *= 0.25; b *= 0.5; c *= 0.5;
            }
            float powder = 1.0 - exp(-2.0 * Extinction * density * 40.0);
            float hb = towers ? h * InvTowerShare : h;   // Towering: the height in the ordinary layer (above 1 up a tower)
            vec3 ambient = mix(ambientBottom, ambientTop, saturate(hb));
            if (towers) ambient = mix(ambient, vec3(luma(ambient) * 1.15), 0.7 * Towering * saturate((hb - 0.5) / 1.5));
            float lowLight = mix(1.0, mix(0.75, 1.0, smoothstep(0.0, 0.2, hb)), Cumulus);
            vec3 radiance = (sun * sunLight * mix(0.5, 1.0, powder) + ambient) * lowLight;
            float sigma = Extinction * density;
            float stepTransmittance = exp(-sigma * dt);
            scattered += transmittance * radiance * (1.0 - stepTransmittance);
            depthSum += t * transmittance * (1.0 - stepTransmittance);
            depthWeight += transmittance * (1.0 - stepTransmittance);
            transmittance *= stepTransmittance;
        }
    }
    // (2026-10-25 WeatherTint, as the .fx) the weather's own cloud colour at this hour, brightness unchanged.
    scattered *= mix(vec3(1.0), cb.tint.rgb, cb.tint.w);
    float dist = depthWeight > 0.0 ? depthSum / depthWeight : tStart;
    // (Towering) the high tops of towers far off show through less of the low haze.
    if (towers) {
        float hitAltitude = camera.z + dist * dir.z + dist * dist / (2.0 * EarthRadius);
        dist *= 1.0 - 0.75 * Towering * saturate(3.0 * (hitAltitude - Base - 0.5 * BaseThickness) / max(Thickness - BaseThickness, 1.0));
    }
    float haze = exp(-(dist * dist) / (HazeDistance * HazeDistance)) * saturate((MaxDistance - dist) / (0.4 * MaxDistance));
    scattered *= haze;
    transmittance = mix(1.0, transmittance, haze);
    return vec4(scattered, transmittance);
}

