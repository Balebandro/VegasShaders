// The flashlight's in-air shaft as a Vulkan pass (needs NVR DXVK): the algorithm of Effects/FlashlightBeam.fx.hlsl, line for line. One
// pass into the beam's half-resolution target. Bindings: 0 = depth (read with the POINT sampler, push offset 0), 1 = the cookie (read with
// the LINEAR mip sampler, push offset 4), set 2 binding 0 = the constants block (336 bytes, std140, the C++ FlashlightBeamUniforms).
// Matrices: a GLSL mat4 element [c][r] is the D3D element [c][r]; the D3D mul(v, M) is M * v, and mul(M, v) is v * M.
// No early return: a miss has a zero-length segment; the output is then (0, 0, 0, 1), as is zero strength.
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "Common.glsl"

layout(set = 1, binding = 0) uniform texture2D t_depth;
layout(set = 1, binding = 1) uniform texture2D t_cookie;

layout(set = 2, binding = 0, std140) uniform FlashlightBeamBlock {
    vec4 volumetricControl;      // 0
    vec4 volumetricData;         // 16
    vec4 spotLightPosition;      // 32
    vec4 spotLightColor;         // 48
    vec4 spotLightDirection;     // 64
    vec4 sunColor;               // 80
    vec4 reciprocalResolution;   // 96
    vec4 cameraPosition;         // 112
    vec4 cameraData;             // 128: y far distance
    mat4 flashLightViewProj;     // 144
    mat4 viewTransform;          // 208
    mat4 projectionTransform;    // 272
} cb;

layout(push_constant) uniform Push {
    uint pointSamplerIndex;      // 0
    uint cookieSamplerIndex;     // 4
} pc;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o_color;

// Depth.hlsl toWorld(): camera-relative world direction through this pixel (view depth 1 along forward).
vec3 toWorldDir(vec2 tex) {
    vec3 v = vec3(cb.viewTransform[0][2], cb.viewTransform[1][2], cb.viewTransform[2][2]);
    v += (1.0 / cb.projectionTransform[0][0] * (2.0 * tex.x - 1.0)) * vec3(cb.viewTransform[0][0], cb.viewTransform[1][0], cb.viewTransform[2][0]);
    v += (-1.0 / cb.projectionTransform[1][1] * (2.0 * tex.y - 1.0)) * vec3(cb.viewTransform[0][1], cb.viewTransform[1][1], cb.viewTransform[2][1]);
    return v;
}

// BeamSegment of the .fx: the part of the ray inside the outer cone (cos^2 = k, nappe), the range sphere and [0, tMax].
void beamSegment(float p, float q, float s, float u, float k, float R, float tMax, out float t0, out float t1) {
    float lo = 0.0;
    float hi = tMax;
    float disc = u * u - (s - R * R);
    if (disc < 0.0) {
        hi = -1.0;
    } else {
        float sq = sqrt(disc);
        lo = max(lo, -u - sq);
        hi = min(hi, -u + sq);
    }
    float qa = q * q - k;
    float qb = p * q - k * u;
    float qc = p * p - k * s;
    if (qa < 0.0) {
        float dd = qb * qb - qa * qc;
        if (dd < 0.0) {
            hi = -1.0;
        } else {
            float sq2 = sqrt(dd);
            float r1 = (-qb - sq2) / qa;
            float r2 = (-qb + sq2) / qa;
            lo = max(lo, min(r1, r2));
            hi = min(hi, max(r1, r2));
        }
    }
    if (abs(q) > 1e-6) {
        float tn = -p / q;
        if (q > 0.0) lo = max(lo, tn); else hi = min(hi, tn);
    } else if (p < 0.0) {
        hi = -1.0;
    }
    t0 = lo;
    t1 = max(hi, lo);
}

void main() {
    const float kSteps = 24.0;

    vec3 viewRaw = toWorldDir(uv);
    float rayLen = max(length(viewRaw), 1e-6);
    vec3 rd = viewRaw / rayLen;
    float depth = textureLod(sampler2D(t_depth, NVR_SAMPLER(pc.pointSamplerIndex)), uv, 0.0).r * cb.cameraData.y;
    float sceneDist = depth * rayLen;

    vec3 C = cb.cameraPosition.xyz;
    vec3 L = cb.volumetricData.xyz;
    float R = max(cb.spotLightPosition.w, 1e-4);
    vec3 A = normalize(cb.spotLightDirection.xyz);
    float outerDeg = cb.spotLightDirection.w;
    float cosO = cos(radians(outerDeg));
    float cosI = cos(radians(outerDeg * 0.5));
    float tMax = min(cb.volumetricControl.w, sceneDist);

    vec3 v = C - L;
    float p = dot(v, A);
    float q = dot(rd, A);
    float s = dot(v, v);
    float u = dot(v, rd);
    float t0, t1;
    beamSegment(p, q, s, u, cosO * cosO, R, tMax, t0, t1);
    float stepLen = (t1 - t0) / kSteps;

    vec2 pixel = uv / cb.reciprocalResolution.xy;
    float ign = fract(52.9829189 * fract(dot(pixel, vec2(0.06711056, 0.00583715))));

    float g = cb.volumetricControl.y;
    float fp = clamp(cb.volumetricData.w, 0.0, 1.0);
    float heightFalloff = cb.volumetricControl.z;
    float groundZ = C.z - 128.0;

    float transmittance = 1.0;
    vec3 scatter = vec3(0.0);
    for (int i = 0; i < 24; i++) {
        float t = t0 + (float(i) + ign) * stepLen;
        vec3 P = C + rd * t;
        vec3 fromLight = P - L;
        float dl = max(length(fromLight), 0.001);
        vec3 dirL = fromLight / dl;
        float tc = clamp((dot(dirL, A) - cosO) / max(cosI - cosO, 1e-4), 0.0, 1.0);
        float cone = tc * tc;
        float dR = dl / R;
        float sq = clamp(dR * dR, 0.0, 1.0);
        float att = clamp((1.0 - sq) * (1.0 - sq) / (1.0 + 5.0 * sq), 0.0, 1.0);
        vec4 clip = cb.flashLightViewProj * vec4(P, 1.0);
        float cw = max(clip.w, 1e-6);
        vec2 cuv = vec2(clip.x / cw * 0.5 + 0.5, 0.5 - 0.5 * clip.y / cw);
        float cookie = mix(texture(sampler2D(t_cookie, NVR_SAMPLER(pc.cookieSamplerIndex)), cuv).r, 1.0, fp);
        float density = exp(-max(P.z - groundZ, 0.0) * heightFalloff * 0.0005);
        float cosS = dot(rd, -dirL);
        float phase = (1.0 - g * g) / (12.566370614 * pow(max(1.0 + g * g - 2.0 * g * cosS, 1e-4), 1.5));
        float surfaceFade = smoothstep(0.0, 256.0, sceneDist - t);
        float sigma = density * cone * att * cookie * surfaceFade;
        transmittance *= exp(-density * 0.5 * stepLen * 0.001);
        scatter += transmittance * sigma * phase * stepLen;
    }

    vec3 lightColour = cb.spotLightColor.rgb * cb.spotLightColor.w / max(0.05, luma(cb.sunColor.rgb));
    float cameraFade = max(clamp((length(L - C) - 40.0) / 60.0, 0.0, 1.0), fp);
    vec3 inscatter = scatter * (cb.volumetricControl.x * 0.0008 * cameraFade) * lightColour;

    float on = cb.volumetricControl.x > 0.0 ? 1.0 : 0.0;
    o_color = vec4(mix(vec3(0.0), inscatter, on), mix(1.0, transmittance, on));
}
