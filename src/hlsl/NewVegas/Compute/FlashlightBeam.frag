// FlashlightBeam as a Vulkan pass (needs NVR DXVK): Effects/FlashlightBeam.fx.hlsl line for line. effects/FlashlightBeam.cpp ExtRender.
// Matrices: the block holds the D3DX matrices as stored, so HLSL M[i][j] is cb.m[i][j] here, and mul(v, M) is M * v.
// Two changes that keep the picture: the HLSL early return for a ray that misses the cone is a zero-length march
// (stepLen 0 gives transmittance 1 and no scatter, the same (0, 0, 0, 1)), so every texture read stays in uniform
// control flow and its implicit derivatives are defined; and the result is chosen by the hit test at the end.
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "Common.glsl"

#define VOL_STEPS 24

// Converts the scatter integral to something in the same range as the surface pool
const float VOL_CALIBRATION = 0.0008;
// Distance over which a sample fades out as it approaches the surface the ray lands on
const float VOL_SURFACE_FADE = 256.0;

layout(set = 1, binding = 0) uniform texture2D t_depth;      // TESR_DepthBuffer (R32F: linear depth / farZ)
layout(set = 1, binding = 1) uniform texture2D t_cookie;     // TESR_SpotLightTexture (Effects\Flashlight.png, LINEAR mips, CLAMP)

// The constants the HLSL reads, filled by the host each frame (the matrices make the block bigger than push data).
layout(set = 2, binding = 0, std140) uniform Constants {
    vec4 volumetricControl;        // TESR_VolumetricControl: x strength, y anisotropy, z height falloff, w max distance
    vec4 volumetricData;           // TESR_VolumetricData: xyz march light position, w first person strength floor
    vec4 spotLightPosition;        // TESR_SpotLightPosition: xyz position, w radius
    vec4 spotLightColor;           // TESR_SpotLightColor
    vec4 spotLightDirection;       // TESR_SpotLightDirection: xyz axis, w outside angle
    vec4 sunColor;                 // TESR_SunColor
    vec4 reciprocalResolution;     // TESR_ReciprocalResolution
    vec4 cameraPosition;           // TESR_CameraPosition
    vec4 cameraData;               // TESR_CameraData: x near, y far
    mat4 flashLightViewProj;       // TESR_FlashLightViewProjTransform
    mat4 viewTransform;            // TESR_ViewTransform
    mat4 projectionTransform;      // TESR_ProjectionTransform
} cb;

layout(push_constant) uniform Push {
    uint pointSamplerIndex;        // 0: POINT, CLAMP (the depth buffer)
    uint cookieSamplerIndex;       // 4: LINEAR with mips, CLAMP (the cookie)
} pc;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o_color;

// Includes/Depth.hlsl readDepth (tex2D of a buffer without mips: level 0)
float readDepth(vec2 coord) {
    return textureLod(sampler2D(t_depth, NVR_SAMPLER(pc.pointSamplerIndex)), coord, 0.0).x * cb.cameraData.y;
}

// Includes/Depth.hlsl toWorld
vec3 toWorld(vec2 tex) {
    vec3 v = vec3(cb.viewTransform[0][2], cb.viewTransform[1][2], cb.viewTransform[2][2]);
    v += (1.0 / cb.projectionTransform[0][0] * (2.0 * tex.x - 1.0)) * vec3(cb.viewTransform[0][0], cb.viewTransform[1][0], cb.viewTransform[2][0]);
    v += (-1.0 / cb.projectionTransform[1][1] * (2.0 * tex.y - 1.0)) * vec3(cb.viewTransform[0][1], cb.viewTransform[1][1], cb.viewTransform[2][1]);
    return v;
}

// Includes/Helpers.hlsl invlerps and shades
float invlerps(float a, float b, float t) { return saturate((t - a) / (b - a)); }
float shades(vec3 n, vec3 l) { return saturate(dot(n, l)); }

// Henyey-Greenstein phase function, forward scattering for g > 0
float HenyeyGreenstein(float cosTheta, float g) {
    float g2 = g * g;
    return (1.0 - g2) / (4.0 * 3.14159265 * pow(max(1e-4, 1.0 + g2 - 2.0 * g * cosTheta), 1.5));
}

// Interleaved gradient noise, used to offset the first step
float ign(vec2 px) {
    return fract(52.9829189 * fract(dot(px, vec2(0.06711056, 0.00583715))));
}

// [t0, t1] bracket of the view ray inside the cone volume (see FlashlightBeam.fx.hlsl)
bool ConeSegment(vec3 co, vec3 rd, vec3 axis, float cosA, float range, float tMax, out float t0, out float t1) {
    float cos2 = cosA * cosA;
    float da   = dot(rd, axis);
    float ca   = dot(co, axis);
    float dco  = dot(rd, co);
    float coco = dot(co, co);

    t0 = tMax; t1 = 0.0;
    float t, h;
    vec3 p;

    float A = da * da - cos2;
    float B = 2.0 * (da * ca - dco * cos2);
    float C = ca * ca - coco * cos2;
    float disc = B * B - 4.0 * A * C;
    if (abs(A) > 1e-6 && disc > 0.0) {
        float sq = sqrt(disc);
        t = (-B - sq) / (2.0 * A); h = ca + t * da;
        if (t > 0.0 && t < tMax && h > 0.0 && h < range) { t0 = min(t0, t); t1 = max(t1, t); }
        t = (-B + sq) / (2.0 * A); h = ca + t * da;
        if (t > 0.0 && t < tMax && h > 0.0 && h < range) { t0 = min(t0, t); t1 = max(t1, t); }
    }

    float ds = dco * dco - (coco - range * range);
    if (ds > 0.0) {
        float sq = sqrt(ds);
        t = -dco - sq; p = co + t * rd; h = dot(p, axis);
        if (t > 0.0 && t < tMax && h > 0.0 && h * h > cos2 * dot(p, p)) { t0 = min(t0, t); t1 = max(t1, t); }
        t = -dco + sq; p = co + t * rd; h = dot(p, axis);
        if (t > 0.0 && t < tMax && h > 0.0 && h * h > cos2 * dot(p, p)) { t0 = min(t0, t); t1 = max(t1, t); }
    }

    if (ca > 0.0 && ca < range && ca * ca > cos2 * coco) t0 = 0.0;
    p = co + tMax * rd; h = dot(p, axis);
    if (h > 0.0 && dot(p, p) < range * range && h * h > cos2 * dot(p, p)) t1 = tMax;

    return t1 > t0;
}

void main() {
    float strength = cb.volumetricControl.x;
    if (strength <= 0.0) { o_color = vec4(0.0, 0.0, 0.0, 1.0); return; }

    float g          = cb.volumetricControl.y;
    float heightFall = cb.volumetricControl.z;
    float maxDist    = cb.volumetricControl.w;

    vec3 rayVec = toWorld(uv);
    vec3 dir    = normalize(rayVec);
    vec3 camPos = cb.cameraPosition.xyz;

    vec3 lightPos = cb.volumetricData.xyz;
    float fpBlend = saturate(cb.volumetricData.w);
    vec3 lightDir = cb.spotLightDirection.xyz;
    float radius  = cb.spotLightPosition.w;
    float angleCosMax = cos(radians(cb.spotLightDirection.w));
    float angleCosMin = cos(radians(cb.spotLightDirection.w * 0.5));
    float groundZ   = camPos.z - 128.0;		// roughly the player's feet; dust settles low

    float sceneDist = length(rayVec) * readDepth(uv);
    float tMax = min(sceneDist, maxDist);

    float tEnter, tExit;
    bool hit = ConeSegment(camPos - lightPos, dir, lightDir, max(angleCosMax, 0.1), radius, tMax, tEnter, tExit);

    // A miss marches zero length (see the header): the loop below then adds nothing and the result is the HLSL's (0, 0, 0, 1).
    float stepLen = hit ? (tExit - tEnter) / float(VOL_STEPS) : 0.0;
    float t = tEnter + ign(uv / cb.reciprocalResolution.xy) * stepLen;

    vec3 scatter = vec3(0.0);
    float transmittance = 1.0;
    for (int i = 0; i < VOL_STEPS; i++) {
        vec3 p = camPos + dir * t;
        vec3 toL = lightPos - p;
        float distToL = length(toL);
        vec3 Ldir = toL / max(distToL, 0.001);

        float cone = pow(invlerps(angleCosMax, angleCosMin, shades(lightDir, -Ldir)), 2.0);
        float s = saturate((distToL / radius) * (distToL / radius));
        float atten = saturate(((1.0 - s) * (1.0 - s)) / (1.0 + 5.0 * s));

        // mul(float4(p, 1), TESR_FlashLightViewProjTransform), with the matrix as stored
        vec4 lsc = cb.flashLightViewProj * vec4(p, 1.0);
        // ScreenCoordToTexCoord
        lsc.xyz /= lsc.w;
        lsc.x = lsc.x * 0.5 + 0.5;
        lsc.y = lsc.y * -0.5 + 0.5;
        vec2 cookieUV = lsc.xy;
        // The cookie projects from the true light position, but in first person the march
        // runs from a virtual offset one, so blend it flat to hide the parallax mismatch
        float cookie = mix(texture(sampler2D(t_cookie, NVR_SAMPLER(pc.cookieSamplerIndex)), cookieUV).r, 1.0, fpBlend);

        float density = exp(-max(0.0, p.z - groundZ) * heightFall * 0.0005);
        float phase   = HenyeyGreenstein(dot(dir, -Ldir), g);

        float surfaceFade = smoothstep(0.0, VOL_SURFACE_FADE, sceneDist - t);
        float sigma = density * cone * atten * cookie * surfaceFade;
        scatter += transmittance * sigma * phase * stepLen;
        transmittance *= exp(-density * 0.5 * stepLen * 0.001);
        t += stepLen;
    }

    // Same energy model as the surface pool - colour, dimmer and the night boost
    float sunLuma = 1.0 / max(0.05, luma(cb.sunColor));
    vec3 lightColor = cb.spotLightColor.rgb * cb.spotLightColor.w * sunLuma;

    // Fade with the light to camera distance so the in-scatter cannot pile onto the surface
    // pool when the source sits on the eye. In first person w floors this fade.
    float camFade = saturate((length(lightPos - camPos) - 40.0) / 60.0);
    camFade = max(camFade, fpBlend);

    vec3 inScatter = scatter * (strength * VOL_CALIBRATION * camFade) * lightColor;
    o_color = hit ? vec4(inScatter, transmittance) : vec4(0.0, 0.0, 0.0, 1.0);
}
