// SunShadows as Vulkan passes (needs NVR DXVK; effects/SunShadows.cpp ExtPingPong): what the five pass shaders share. The same
// arithmetic as Effects/SunShadows.fx.hlsl (wave 3 rewrite, 2026-11-02); see that file for the reasons behind each rule.
// Differences from the D3D9 passes, both forced by what the C++ binds:
//   - no TESR_BackDepthBuffer / TESR_BackDepthData: an occluder is taken to be 20 units thick (as the D3D9 passes do wherever
//     the BackDepth pass did not draw, its default);
//   - no TESR_RenderScale: DxvkCompute::PassesOn() keeps these passes out of RenderScale rect frames, so the drawn part is the whole
//     texture and the rect clamp is the plain half-texel clamp to it.
// Matrices are as stored by the D3D9 side: cb.M[i][j] is HLSL's M[i][j]; HLSL mul(v, M) is cb.M * v and mul(M, v) is v * cb.M.
#extension GL_GOOGLE_include_directive : enable
#include "Common.glsl"

// set 1: the .fx's samplers in declaration order (binding = its register)
layout(set = 1, binding = 0) uniform texture2D t_depth;     // TESR_DepthBuffer: view depth / far
layout(set = 1, binding = 1) uniform texture2D t_atlas;     // TESR_ShadowAtlas: 2 x 2 sun cascades, plain light-space depth (PCF)
layout(set = 1, binding = 2) uniform texture2D t_normals;   // TESR_NormalsBuffer: view-space normal, 0..1 encoded
layout(set = 1, binding = 3) uniform texture2D t_source;    // TESR_PointShadowBuffer slot: this pass's source (buffer or scratch)
layout(set = 1, binding = 4) uniform texture2D t_noise;     // TESR_NoiseSampler: bluenoise256
layout(set = 1, binding = 5) uniform texture2D t_charMap;   // TESR_CharShadowMap (the depth texture as a stand-in when off)

// set 2: SunShadows.cpp SunUniforms (std140, 832 bytes)
layout(set = 2, binding = 0, std140) uniform Constants {
    mat4 cascade[4];            //   0: TESR_ShadowCameraToLightTransformNear, Middle, Far, Lod
    mat4 projection;            // 256: TESR_ProjectionTransform
    mat4 invProjection;         // 320: TESR_InvProjectionTransform (not needed: positions come from the linear depth)
    mat4 view;                  // 384: TESR_ViewTransform
    mat4 invView;               // 448: TESR_InvViewTransform
    vec4 reciprocalResolution;  // 512: TESR_ReciprocalResolution
    vec4 smoothedSunDir;        // 528: TESR_SmoothedSunDir
    vec4 viewSpaceLightDir;     // 544: TESR_ViewSpaceLightDir
    vec4 formatData;            // 560: TESR_ShadowFormatData (x mode, z outdoors)
    vec4 screenSpaceData;       // 576: TESR_ShadowScreenSpaceData (x on, y BlurRadius, z RenderDistance, w Intensity)
    vec4 shadowFade;            // 592: TESR_ShadowFade (y maps drawn)
    vec4 shadowBlur;            // 608: TESR_ShadowBlur (x 1 / atlas size, z SmoothFilter)
    vec4 forwardData;           // 624: TESR_ShadowForwardData (x 1 = object shaders' lookups off, w HandoverBlend)
    vec4 center[4];             // 640: TESR_ShadowNearCenter .. LodCenter (xyz centre, w radius)
    vec4 cameraData;            // 704: TESR_CameraData (x near, y far)
    vec4 flags;                 // 720: x FORWARD_SHADOWS compiled in, y NearFade start, z NearFade end, w RT sun range (Mode 1)
    mat4 charToLight;           // 736: TESR_CharShadowCameraToLight
    vec4 charCenter;            // 800: TESR_CharShadowCenter
    vec4 charData;              // 816: TESR_CharShadowData
} cb;

layout(push_constant) uniform Push {
    uint linearSampler;   // 0: LINEAR, CLAMP (depth, atlas, normals, source, character map)
    uint noiseSampler;    // 4: LINEAR, WRAP, linear mips (blue noise)
    uint pass;            // 8: SunShadowsBlur: 0 across, 1 down, 2 down + Intensity
    uint pad;
} pc;

#define LINEAR_CLAMP NVR_SAMPLER(pc.linearSampler)
#define NOISE_WRAP NVR_SAMPLER(pc.noiseSampler)

const float SkyShare = 0.9999;
const float RayThickness = 20.0;

// HLSL smoothstep for any edges (GLSL's is undefined when edge0 >= edge1)
float hsmooth(float a, float b, float x) {
    float t = clamp((x - a) / (b - a), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

float farDistance() { return max(cb.cameraData.y, 1.0); }
float renderDistance() { return max(cb.screenSpaceData.z, 1.0); }
float contactIntensity() { return max(cb.screenSpaceData.w, 0.0); }

vec4 sourceAt(vec2 p) { return textureLod(sampler2D(t_source, LINEAR_CLAMP), p, 0.0); }
float viewDepthAt(vec2 p) { return textureLod(sampler2D(t_depth, LINEAR_CLAMP), p, 0.0).x * cb.cameraData.y; }

vec2 snapToTexel(vec2 p) {
    vec2 texel = cb.reciprocalResolution.xy;
    return (floor(p / texel) + 0.5) * texel;
}
vec2 clampToTexture(vec2 p) { return clamp(p, 0.5 * cb.reciprocalResolution.xy, 1.0 - 0.5 * cb.reciprocalResolution.xy); }
bool outsideTexture(vec2 p) { return any(lessThan(p, vec2(0.0))) || any(greaterThan(p, vec2(1.0))); }

vec3 viewPositionAt(vec2 p, float viewZ) {
    vec2 ndc = vec2(p.x * 2.0 - 1.0, 1.0 - p.y * 2.0);
    return vec3((ndc.x - cb.projection[2][0]) * viewZ / cb.projection[0][0],
                (ndc.y - cb.projection[2][1]) * viewZ / cb.projection[1][1], viewZ);
}

vec2 texCoordOf(vec3 viewPos) {
    float invZ = 1.0 / max(viewPos.z, 1.0e-3);
    vec2 ndc = vec2(viewPos.x * cb.projection[0][0] * invZ + cb.projection[2][0],
                    viewPos.y * cb.projection[1][1] * invZ + cb.projection[2][1]);
    return vec2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
}

vec3 viewNormalAt(vec2 p) {
    vec3 n = textureLod(sampler2D(t_normals, LINEAR_CLAMP), p, 0.0).xyz * 2.0 - 1.0;
    float len2 = dot(n, n);
    return len2 > 1.0e-4 ? n * inversesqrt(len2) : vec3(0.0, 0.0, -1.0);
}

vec3 worldPositionOf(vec3 viewPos) { return (cb.invView * vec4(viewPos, 1.0)).xyz; }
vec3 worldDirectionOf(vec3 viewDir) { return (vec4(viewDir, 0.0) * cb.view).xyz; }

// ---- contact shadows ----
float nearFadeWeight(float viewZ) {
    float start = cb.flags.y;
    float end = max(cb.flags.z, start + 1.0e-3);
    return cb.flags.z > 0.0 ? hsmooth(start, end, viewZ) : 1.0;
}

float sunFacing(vec3 viewNormal, vec3 viewSun) { return clamp(dot(viewNormal, viewSun) * 8.0, 0.0, 1.0); }

float rayBlocked(vec2 sampleUV, float rayZ) {
    if (outsideTexture(sampleUV) || rayZ <= cb.cameraData.x) return 0.0;
    float sceneZ = viewDepthAt(snapToTexel(sampleUV));
    float thickness = RayThickness;
    float bias = max(0.01, rayZ * 0.002);
    float gap = rayZ - sceneZ;
    return clamp((gap - bias) / bias, 0.0, 1.0) * clamp((thickness - gap) / (0.25 * thickness + 0.01), 0.0, 1.0);
}

float contactFromOcclusion(float occlusion, float facing, float viewZ) {
    float shade = pow(clamp(occlusion, 0.0, 1.0), 0.3) * facing * nearFadeWeight(viewZ);
    return mix(1.0 - shade, 1.0, hsmooth(0.8 * renderDistance(), renderDistance(), viewZ));
}

bool noContact(float viewZ, float facing) {
    return cb.screenSpaceData.x < 0.5 || viewZ >= renderDistance() || viewZ >= SkyShare * cb.cameraData.y || facing <= 0.0;
}

// ---- depth-aware blur ----
vec4 depthAwareBlur(vec2 p, vec2 direction) {
    vec4 centre = sourceAt(p);
    float centreZ = viewDepthAt(p);
    if (!(centreZ < renderDistance() && centreZ < SkyShare * cb.cameraData.y)) return centre;
    vec3 centrePos = viewPositionAt(p, centreZ);
    vec3 centreNormal = viewNormalAt(p);
    float tolerance = max(1.0, 0.012 * centreZ);
    vec2 stride = direction * max(cb.screenSpaceData.y, 0.0) * cb.reciprocalResolution.xy;
    vec4 sum = centre;
    float weights = 1.0;
    for (int i = 0; i < 12; i++) {
        float k = i < 6 ? float(i - 6) : float(i - 5);
        vec2 tapUV = clampToTexture(p + k * stride);
        float tapZ = viewDepthAt(tapUV);
        float planeGap = abs(dot(viewPositionAt(tapUV, tapZ) - centrePos, centreNormal));
        float weight = exp(-k * k / 18.0) * clamp(1.0 - planeGap / tolerance, 0.0, 1.0);
        sum += weight * sourceAt(tapUV);
        weights += weight;
    }
    return sum / weights;
}

vec2 applyIntensity(vec2 v) { return pow(max(v, vec2(1.0e-6)), vec2(contactIntensity())); }

// ---- sun cascade maps (Shaders/Includes/Shadow.hlsl GetSunShadow, Mode 3) ----
float atlasFilter(vec2 p, float depth, vec2 quadrant, float slopeBias) {
    float texel = max(cb.shadowBlur.x, 1.0 / 16384.0);
    vec2 lo = quadrant + 0.5 * texel;
    vec2 hi = quadrant + 0.5 - 0.5 * texel;
    float lit = 0.0;
    vec2 grid = p / texel - 0.5;
    if (cb.shadowBlur.z > 0.0) {
        vec2 base = floor(grid);
        vec2 t = grid - base;
        vec2 t2 = t * t, t3 = t2 * t;
        vec4 cx = vec4((1.0 - t.x) * (1.0 - t.x) * (1.0 - t.x), 3.0 * t3.x - 6.0 * t2.x + 4.0, -3.0 * t3.x + 3.0 * t2.x + 3.0 * t.x + 1.0, t3.x) / 6.0;
        vec4 cy = vec4((1.0 - t.y) * (1.0 - t.y) * (1.0 - t.y), 3.0 * t3.y - 6.0 * t2.y + 4.0, -3.0 * t3.y + 3.0 * t2.y + 3.0 * t.y + 1.0, t3.y) / 6.0;
        float cubicDepth = depth - 0.5 * slopeBias;
        for (int row = 0; row < 4; row++) {
            for (int col = 0; col < 4; col++) {
                vec2 tap = clamp((base + vec2(col - 1, row - 1) + 0.5) * texel, lo, hi);
                lit += (cubicDepth <= textureLod(sampler2D(t_atlas, LINEAR_CLAMP), tap, 0.0).x ? 1.0 : 0.0) * cx[col] * cy[row];
            }
        }
    }
    else {
        vec2 nearest = floor(grid + 0.5);
        vec2 f = grid - nearest;
        vec3 wx = vec3(0.5 * (0.5 - f.x) * (0.5 - f.x), 0.75 - f.x * f.x, 0.5 * (0.5 + f.x) * (0.5 + f.x));
        vec3 wy = vec3(0.5 * (0.5 - f.y) * (0.5 - f.y), 0.75 - f.y * f.y, 0.5 * (0.5 + f.y) * (0.5 + f.y));
        for (int row = 0; row < 3; row++) {
            for (int col = 0; col < 3; col++) {
                vec2 tap = clamp((nearest + vec2(col - 1, row - 1) + 0.5) * texel, lo, hi);
                lit += (depth <= textureLod(sampler2D(t_atlas, LINEAR_CLAMP), tap, 0.0).x ? 1.0 : 0.0) * wx[col] * wy[row];
            }
        }
    }
    return lit;
}

float cascadeLookup(int c, vec3 worldPos, float bias, float slopeBias) {
    vec2 quadrant = vec2((c == 1 || c == 3) ? 0.5 : 0.0, c >= 2 ? 0.5 : 0.0);
    vec4 light = cb.cascade[c] * vec4(worldPos, 1.0);
    light.xyz /= light.w;
    vec2 atlasUV = vec2(light.x * 0.5 + 0.5, light.y * -0.5 + 0.5) * 0.5 + quadrant;
    return atlasFilter(atlasUV, light.z - bias - 1.5 * slopeBias, quadrant, slopeBias);
}

float cascadeVisibility(vec3 worldPos, vec3 worldNormal) {
    float nDotL = dot(worldNormal, cb.smoothedSunDir.xyz);
    float offsetScale = clamp(1.0 - nDotL, 0.0, 1.0);
    vec4 radii = vec4(cb.center[0].w, cb.center[1].w, cb.center[2].w, cb.center[3].w);
    vec4 texelWorld = 4.0 * radii * max(cb.shadowBlur.x, 1.0 / 16384.0);
    vec4 normalOffset = offsetScale * 1.5 * texelWorld;
    float slopeBias = 2.0 * cb.shadowBlur.x * min(sqrt(clamp(1.0 - nDotL * nDotL, 0.0, 1.0)) / max(nDotL, 0.05), 5.0);
    float bias = 0.0002 * (1.0 + offsetScale);
    int cascade = -1;
    float fade = 0.0;
    for (int i = 3; i >= 0; i--) {   // the innermost cascade holding the point wins
        float radius = radii[i];
        float d = length(worldPos - cb.center[i].xyz);
        if (d < radius) {
            float start = radius - max(0.1 * radius, min(cb.forwardData.w, radius));
            cascade = i;
            fade = hsmooth(start, max(radius, start + 1.0e-3), d);
        }
    }
    if (cascade < 0) return 1.0;
    float offsetHere = cascade < 3 ? mix(normalOffset[cascade], normalOffset[cascade + 1], fade) : normalOffset[3];
    float slackScale = cascade == 0 ? mix(radii.x / max(radii.y, 0.001), 1.0, fade) : 1.0;
    vec3 lookupPos = worldPos + offsetHere * worldNormal;
    float visibility = cascadeLookup(cascade, lookupPos, bias, slopeBias);
    if (fade > 0.0) {
        float next = cascade < 3 ? cascadeLookup(cascade + 1, lookupPos, bias, slopeBias * slackScale) : 1.0;
        visibility = mix(visibility, next, fade);
    }
    return clamp(visibility, 0.0, 1.0);
}

#if defined(CHARACTER_SHADOW_MAP) && CHARACTER_SHADOW_MAP
// Shaders/Includes/Shadow.hlsl CharacterMapShadow; the LINEAR sampler at texel centres returns the texel itself (the .fx samples POINT)
float characterVisibility(vec3 worldPos, vec3 worldNormal, float nDotL, float cascadeVis, out bool debugView) {
    debugView = false;
    float texel = max(cb.charData.x, 1.0 / 16384.0);
    float halfExtent = max(cb.charCenter.w, 1.0);
    float offsetScale = clamp(1.0 - nDotL, 0.0, 1.0);
    vec3 position = worldPos + worldNormal * (offsetScale * 1.5 * 2.0 * halfExtent * texel);
    vec4 light = cb.charToLight * vec4(position, 1.0);
    float edge = max(abs(light.x), abs(light.y));
    float weight = 1.0 - hsmooth(1.0 - max(cb.charData.y, 0.01), 1.0, edge);
    if (weight <= 0.0) return cascadeVis;
    vec2 p = vec2(light.x * 0.5 + 0.5, light.y * -0.5 + 0.5);
    float slope = min(sqrt(clamp(1.0 - nDotL * nDotL, 0.0, 1.0)) / max(nDotL, 0.05), 5.0);
    float depth = min(light.z, 0.9999) - cb.charData.z - 1.5 * texel * slope;
    vec2 lo = vec2(0.5 * texel);
    vec2 hi = vec2(1.0 - 0.5 * texel);
    vec2 grid = p / texel - 0.5;
    vec2 nearest = floor(grid + 0.5);
    vec2 f = grid - nearest;
    vec3 wx = vec3(0.5 * (0.5 - f.x) * (0.5 - f.x), 0.75 - f.x * f.x, 0.5 * (0.5 + f.x) * (0.5 + f.x));
    vec3 wy = vec3(0.5 * (0.5 - f.y) * (0.5 - f.y), 0.75 - f.y * f.y, 0.5 * (0.5 + f.y) * (0.5 + f.y));
    float lit = 0.0;
    for (int row = 0; row < 3; row++) {
        for (int col = 0; col < 3; col++) {
            vec2 tap = clamp((nearest + vec2(col - 1, row - 1) + 0.5) * texel, lo, hi);
            lit += (depth <= textureLod(sampler2D(t_charMap, LINEAR_CLAMP), tap, 0.0).x ? 1.0 : 0.0) * wx[col] * wy[row];
        }
    }
    if (cb.charData.w > 1.5) {
        debugView = true;
        if (cb.charData.w > 3.5) return clamp(light.z, 0.0, 1.0);
        if (cb.charData.w > 2.5) return textureLod(sampler2D(t_charMap, LINEAR_CLAMP), clamp(p, lo, hi), 0.0).x;
        return lit;
    }
    return mix(cascadeVis, min(cascadeVis, lit), weight);
}
#endif

float sunMapsVisibility(vec2 p, out bool debugView) {
    debugView = false;
    float viewZ = viewDepthAt(p);
    bool objectShadersDoIt = cb.flags.x > 0.5 && cb.forwardData.x < 0.5;
    if (objectShadersDoIt || cb.shadowFade.y <= 0.5 || cb.formatData.z <= 0.5 || abs(cb.formatData.x - 3.0) >= 0.5 ||
        viewZ >= SkyShare * cb.cameraData.y)
        return 1.0;
    vec3 viewPos = viewPositionAt(p, viewZ);
    vec3 worldPos = worldPositionOf(viewPos);
    vec3 worldNormal = normalize(worldDirectionOf(viewNormalAt(p)));
    float visibility = cascadeVisibility(worldPos, worldNormal);
    float rtRange = cb.flags.w;
    if (rtRange > 0.0) visibility = mix(1.0, visibility, clamp((viewZ - 0.85 * rtRange) / (0.15 * rtRange), 0.0, 1.0));
#if defined(CHARACTER_SHADOW_MAP) && CHARACTER_SHADOW_MAP
    if (cb.charData.w > 0.0) visibility = characterVisibility(worldPos, worldNormal, dot(worldNormal, cb.smoothedSunDir.xyz), visibility, debugView);
#endif
    return visibility;
}
