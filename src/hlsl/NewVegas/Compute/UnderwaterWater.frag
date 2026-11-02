// Underwater pass 3, the water (NVR DXVK). Effects/Underwater.fx.hlsl Water, line for line.
// Samplers: LINEAR CLAMP at push offset 0 (rendered, source, depth), POINT CLAMP at 4 (normals), ANISOTROPIC WRAP at 8 (caustics, point shadow).
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "Common.glsl"

layout(set = 1, binding = 0) uniform texture2D t_rendered;   // TESR_RenderedBuffer (the god rays, blurred)
layout(set = 1, binding = 1) uniform texture2D t_source;     // TESR_SourceBuffer
layout(set = 1, binding = 2) uniform texture2D t_depth;      // TESR_DepthBuffer
layout(set = 1, binding = 3) uniform texture2D t_normals;    // TESR_NormalsBuffer
layout(set = 1, binding = 4) uniform texture2D t_caustics;   // TESR_CausticsSampler
layout(set = 1, binding = 5) uniform texture2D t_shadow;     // TESR_PointShadowBuffer

layout(push_constant) uniform Push {
    uint linearIndex;     // 0
    uint pointIndex;      // 4
    uint anisoIndex;      // 8
    uint pad;             // 12
} pc;

#include "Underwater.glsl"

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o_color;

void main() {
    vec4 source = sampleLod(t_source, pc.linearIndex, uv);
    vec3 eye = cb.cameraPosition.xyz;
    vec3 dirV = toWorld(uv);
    float viewZ = readDepthAt(t_depth, pc.linearIndex, uv);
    vec3 P = eye + dirV * viewZ;
    if (abovePixel(eye, P)) {
        o_color = source;
        return;
    }

    float dist = length(dirV) * viewZ;
    vec3 dir = dirV / max(length(dirV), 1.0e-6);
    float W = cb.waterSettings.x;

    vec3 scene = linearize(source.rgb);
    vec3 shallow = linearize(cb.waterShallowColor.rgb);
    vec3 deep = linearize(cb.waterDeepColor.rgb);
    vec3 horizon = linearize(cb.horizonColor.rgb);
    vec3 zenith = linearize(cb.skyColor.rgb);
    vec3 sunCol = linearize(cb.sunColor.rgb);
    vec3 sun = sunDirection();

    float sunAlign = clamp(dot(dir, sun), 0.0, 1.0);
    vec3 skyCol = mix(horizon, zenith, clamp(dir.z, 0.0, 1.0)) + sunCol * pow(sunAlign, 12.0) * 0.5;
    float toward = clamp(dir.z * 1.5 + 0.5, 0.0, 1.0);
    vec3 fogCol = mix(deep, shallow + 0.1 * skyCol, toward);
    vec3 fogLit = fogCol * sunCol * (0.4 + 0.6 * sunAlign);

    float fogF = clamp((dist - cb.waterFog.x) / max(cb.waterFog.y - cb.waterFog.x, 1.0), 0.0, 1.0);
    float turb = max(cb.waterVolume.z, 0.0);
    float murk = 1.0 - pow(max(1.0 - fogF, 0.0), max(turb, 0.01));
    float turbW = turb / (turb + 1.0);
    vec3 scatter = (1.0 - fogCol) * max(cb.waterCoefficients.w, 0.0) * turbW * 0.25;
    vec3 fogOut = fogLit + scatter * fogF;

    vec3 nV = (sampleLod(t_normals, pc.pointIndex, uv).xyz - 0.5) / 0.5;
    vec3 upV = (cb.viewTransform * vec4(0.0, 0.0, 1.0, 0.0)).xyz;
    vec3 sunV = (cb.viewTransform * vec4(sun, 0.0)).xyz;
    float faceUp = smoothstep(0.5, 0.9, dot(nV, upV));
    float faceSun = smoothstep(0.0, 0.5, dot(nV, sunV));
    float depthBelow = max(W - P.z, 0.0);
    float fadeDepth = 1.0 - smoothstep(0.0, 800.0, depthBelow);
    float fadeRange = 1.0 - smoothstep(4000.0, 6000.0, dist);
    vec4 shadow = sampleLod(t_shadow, pc.anisoIndex, uv);
    float sunUp = clamp(sun.z * 6.0, 0.0, 1.0);
    float drift = cb.gameTime.x * 0.001 * cb.waveParams.z * 0.02;
    float sunZ = max(sun.z, 0.05);
    vec3 q = P + sun * (depthBelow / sunZ);
    float c1 = sampleLod(t_caustics, pc.anisoIndex, q.xy / 600.0 + vec2(drift, drift * 0.7)).r;
    float c2 = sampleLod(t_caustics, pc.anisoIndex, q.xy / 720.0 - vec2(drift * 0.8, drift)).r;
    float caustic = pow(max(min(c1, c2), 0.0), 1.3) * 4.0;
    scene += caustic * cb.waterVolume.x * sunCol * faceUp * faceSun * fadeDepth * fadeRange * shadow.r * sunUp;

    float absorbStrength = 1.0 - smoothstep(3000.0, 6000.0, dist);
    scene *= mix(vec3(1.0), mix(deep, vec3(1.0), shadow.g), absorbStrength);

    vec3 tint = exp(-max(cb.waterCoefficients.xyz, 0.0) * dist * 0.0002);
    vec3 col = mix(scene * tint, fogOut, murk);
    col = mix(col, fogOut, smoothstep(4000.0, 5000.0, dist));

    float rays = sampleLod(t_rendered, pc.linearIndex, uv * 0.5).r;
    col += rays * sunCol * max(cb.waterVolume.w, 0.0);

    vec3 enc = clamp(delinearize(col), 0.0, 1.0);
    float dither = ((bayer4(gl_FragCoord.xy) + 1.0) / 17.0 - 0.5) / 255.0;
    o_color = vec4(enc + dither, 1.0);
}
