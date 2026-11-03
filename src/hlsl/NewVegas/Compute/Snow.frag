// Snow as a Vulkan pass (needs NVR DXVK): Effects/Snow.fx.hlsl line for line. effects/Snow.cpp ExtRender.
// The matrices reach the effect through ID3DXEffect::SetMatrix, so HLSL M[i][j] is the D3DXMATRIX element (i+1, j+1). The push
// block holds the shadow matrix's memory as stored (rows as GLSL columns), so mul(v, M) is M * v; toWorld's view columns are
// filled by Snow.cpp from the matrix's columns (2026-10-25: they were its rows, a transposed view ray).
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "Common.glsl"

layout(set = 1, binding = 0) uniform texture2D t_rendered;   // TESR_RenderedBuffer (LINEAR, CLAMP)
layout(set = 1, binding = 1) uniform texture2D t_depth;      // TESR_DepthBuffer (R32F: linear depth / farZ)
layout(set = 1, binding = 2) uniform texture2D t_ortho;      // TESR_OrthoMapBuffer (R32F)

layout(push_constant) uniform Push {
    mat4 shadowToLight;    // 0: TESR_ShadowCameraToLightTransformOrtho
    vec4 viewColumn0;      // 64: TESR_ViewTransform, the HLSL columns 0-2 (rows 0-3 of each)
    vec4 viewColumn1;      // 80
    vec4 viewColumn2;      // 96
    vec4 gameTime;         // 112: TESR_GameTime
    vec4 snowData;         // 128: TESR_SnowData (x amount, z speed)
    vec4 sunColor;         // 144: TESR_SunColor
    vec4 sunAmbient;       // 160: TESR_SunAmbient
    vec4 projectionFar;    // 176: x TESR_ProjectionTransform[0][0], y [1][1], z TESR_CameraData.y (farZ)
    uint samplerIndex;     // 192: LINEAR, CLAMP
} pc;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o_color;

const float PI = 3.14159265;
const float hscale = 0.1;
const mat3 p = mat3(30.323122, 30.323122, 30.323122, 30.323122, 30.323122, 30.323122, 30.323122, 30.323122, 30.323122);
const float DEPTH = 50.0;     // distance between each snow layer
const float angle = 1.0;
const int SnowLayers = 8;

// HLSL fmod: x - y * trunc(x / y)
float fmodHlsl(float x, float y) { return x - y * trunc(x / y); }
vec2 fmodHlsl(vec2 x, float y) { return x - y * trunc(x / y); }

vec3 toWorld(vec2 tex) {
    vec3 v = pc.viewColumn2.xyz;
    v += (1.0 / pc.projectionFar.x * (2.0 * tex.x - 1.0)) * pc.viewColumn0.xyz;
    v += (-1.0 / pc.projectionFar.y * (2.0 * tex.y - 1.0)) * pc.viewColumn1.xyz;
    return v;
}

float readDepth(vec2 coord) {
    return texture(sampler2D(t_depth, NVR_SAMPLER(pc.samplerIndex)), coord).x * pc.projectionFar.z;
}

vec2 cylindrical(vec3 world) {
    float u = -atan(world.y, world.x) / PI;
    float v = world.z / length(world.xy);
    return vec2(0.5 * u + 0.5, hscale * v);
}

// checks wether a point is underneath something occluding
float getOrtho(vec4 orthoPos) {
    orthoPos.xyz /= orthoPos.w;
    float outofbounds = float(orthoPos.x < -1.0 || orthoPos.x > 1.0 ||
        orthoPos.y < -1.0 || orthoPos.y > 1.0 ||
        orthoPos.z < 0.0 || orthoPos.z > 1.0);

    orthoPos.x = orthoPos.x * 0.5 + 0.5;
    orthoPos.y = orthoPos.y * -0.5 + 0.5;
    float ortho = texture(sampler2D(t_ortho, NVR_SAMPLER(pc.samplerIndex)), orthoPos.xy).r;
    return float((ortho > orthoPos.z - 0.0001) || outofbounds != 0.0); // always true if out of bounds
}

void main() {
    vec4 color = texture(sampler2D(t_rendered, NVR_SAMPLER(pc.samplerIndex)), uv);
    color.rgb = pows(color.rgb, 2.2); // linearise

    // calculating the ray along which the volumetric snow will be calculated
    vec3 rayStart = toWorld(uv);
    vec3 rayEnd = rayStart * DEPTH * float(SnowLayers);
    vec4 orthoStart = pc.shadowToLight * vec4(rayStart, 1.0);
    vec4 orthoEnd = pc.shadowToLight * vec4(rayEnd, 1.0);
    vec4 rayStep = (orthoEnd - orthoStart) / float(SnowLayers);

    // each iteration adds a cylindrical layer of drops
    float depth = readDepth(uv);
    vec2 cyl = cylindrical(rayStart); // converts world coordinates to cylinder coordinates around the player
    const float timetick = pc.gameTime.z;
    const float speed = pc.snowData.z;

    float totalSnow = 0.0;

    for (int i = 1; i < SnowLayers; i++) {
        float fi = float(i);
        vec2 noiseSurfacePosition = cyl * (1.0 + fi * DEPTH); // scale cylinder coordinates with iterations
        float wind = sin(timetick * 0.5 + fi) * angle; // horizontal offset animated over time
        noiseSurfacePosition += vec2(wind * noiseSurfacePosition.y * (fmodHlsl(fi * 107.238917, 1.0) - 0.5), speed * timetick);

        // creating the noise with magic
        vec3 m = vec3(floor(noiseSurfacePosition), fi);
        vec3 mp = m / fract(p * m);
        vec3 snowNoise = fract(mp); // r.x is a random generated grid of different darknesses

        // determin flake shape
        vec2 shape = abs(fmodHlsl(noiseSurfacePosition, 1.0) + snowNoise.xy - 1.0);
        shape += 0.01 * abs(2.0 * fract(10.0 * noiseSurfacePosition.yx) - 1.0);
        float dropGradient = 0.6 * max(shape.x - shape.y, shape.x + shape.y) + max(shape.x, shape.y) - 0.01;
        float edge = 0.005 + 0.05 * min(1.0 * fi, 4.0 * pc.snowData.x); // can't be 0

        // smoothstep(edge, -edge, x) with edge0 > edge1 (undefined in GLSL): the D3D9 result, saturate((x - edge0) / (edge1 - edge0)) then t*t*(3-2t)
        float t = clamp((dropGradient - edge) / (-edge - edge), 0.0, 1.0);
        float flake = t * t * (3.0 - 2.0 * t) * snowNoise.x;
        flake *= getOrtho(orthoStart + rayStep * fi) * float(depth > DEPTH * fi); // depth and ortho check for snow layer
        totalSnow += flake;
    }
    totalSnow = clamp(totalSnow, 0.0, 1.0);

    vec4 snowColor = max(vec4(pows(pc.sunAmbient.rgb, 2.2), pc.sunAmbient.a), vec4(pows(pc.sunColor.rgb, 2.2), pc.sunColor.a));

    color = mix(color, snowColor, totalSnow);
    color.rgb = pows(color.rgb, 1.0 / 2.2); // delinearise

    o_color = color;
}
