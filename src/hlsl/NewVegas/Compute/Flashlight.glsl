// Flashlight as Vulkan passes (needs NVR DXVK): the shared header of the Flashlight passes. It declares the stage variables, the bindings
// (set 1, the C++ FlashlightBindings order), the push block (32 bytes), the constants block the C++ fills (FlashlightUniforms, std140,
// 352 bytes, instance `cb`), and the helpers of Effects/Flashlight.fx.hlsl, line for line.
// Matrices: a GLSL mat4 element [c][r] is the D3D element [c][r]; the D3D mul(v, M) is M * v here, and mul(M, v) is v * M.
#include "Common.glsl"

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o_color;

// Set 1: the textures, in the C++ binding order.
layout(set = 1, binding = 0) uniform texture2D t_source;      // TESR_SourceBuffer
layout(set = 1, binding = 1) uniform texture2D t_rendered;    // TESR_RenderedBuffer (the chain's current image; the mask in shadow passes)
layout(set = 1, binding = 2) uniform texture2D t_depth;       // TESR_DepthBuffer
layout(set = 1, binding = 3) uniform texture2D t_normals;     // TESR_NormalsBuffer
layout(set = 1, binding = 4) uniform texture2D t_shadow;      // TESR_ShadowSpotlightBuffer0, or the traced mask
layout(set = 1, binding = 5) uniform texture2D t_cookie;      // TESR_SpotLightTexture
layout(set = 1, binding = 6) uniform texture2D t_volumetric;  // TESR_VolumetricBuffer

// Push block of every pass (32 bytes). linearSampler: LINEAR, CLAMP; cookieSampler: LINEAR with mips, CLAMP.
layout(push_constant) uniform Push {
    vec2 offsetMask;     // 0: depth-aware blur direction (1, 0) or (0, 1)
    uint mode;           // 8: pass mode (see each pass)
    uint linearSampler;  // 12
    uint cookieSampler;  // 16
    uint pad0, pad1, pad2;
} pc;

// Set 2: the constants block (instance cb).
layout(set = 2, binding = 0, std140) uniform FlashlightBlock {
    mat4 flashLightViewProj;     // 0: TESR_FlashLightViewProjTransform
    mat4 viewTransform;          // 64
    mat4 projectionTransform;    // 128
    vec4 cameraPosition;         // 192
    vec4 cameraData;             // 208: y far distance
    vec4 spotLightPosition;      // 224: w radius
    vec4 spotLightColor;         // 240: w dimmer
    vec4 spotLightDirection;     // 256: w outer cone angle, degrees
    vec4 sunColor;               // 272
    vec4 reciprocalResolution;   // 288
    vec4 flashLightTuning;       // 304: x near fade, y soft edges, z hotspot limit, w cookie strength
    vec4 flashLightComposite;    // 320: x source linear, y EdgeFix 2, z w 1 / beam buffer size
    vec4 volumetricControl;      // 336: x beam strength
} cb;

// A linear, clamped sample at level 0 of a texture2D (the shared sampler, push offset linearSampler).
#define LINEAR_LOD(tex, p) textureLod(sampler2D(tex, NVR_SAMPLER(pc.linearSampler)), (p), 0.0)

// Linear depth in game units (view distance): the stored value times the far distance.
float readDepth(vec2 p) {
    return LINEAR_LOD(t_depth, p).r * cb.cameraData.y;
}

// Depth.hlsl toWorld(): camera-relative world direction through this pixel (view depth 1 along forward).
vec3 toWorld(vec2 tex) {
    vec3 v = vec3(cb.viewTransform[0][2], cb.viewTransform[1][2], cb.viewTransform[2][2]);
    v += (1.0 / cb.projectionTransform[0][0] * (2.0 * tex.x - 1.0)) * vec3(cb.viewTransform[0][0], cb.viewTransform[1][0], cb.viewTransform[2][0]);
    v += (-1.0 / cb.projectionTransform[1][1] * (2.0 * tex.y - 1.0)) * vec3(cb.viewTransform[0][1], cb.viewTransform[1][1], cb.viewTransform[2][1]);
    return v;
}

vec3 surfaceWorld(vec2 p, float depth) { return cb.cameraPosition.xyz + toWorld(p) * depth; }

// A clip-space position as texture coordinates: perspective divide, 0 to 1 in x and y (y flipped), z and w kept.
vec4 ScreenCoordToTexCoord(vec4 clip) {
    vec3 ndc = clip.xyz / max(clip.w, 1e-6);
    return vec4(ndc.x * 0.5 + 0.5, 0.5 - 0.5 * ndc.y, ndc.z, clip.w);
}

// Distance falloff: 1 at the light, 0 at and beyond the radius.
float falloff(float dist) {
    float d = dist / max(cb.spotLightPosition.w, 1e-4);
    float s = clamp(d * d, 0.0, 1.0);
    return clamp((1.0 - s) * (1.0 - s) / (1.0 + 5.0 * s), 0.0, 1.0);
}

// The cone at a surface point (light-to-surface direction against the axis), squared.
float coneAt(vec3 pos) {
    vec3 fromLight = pos - cb.spotLightPosition.xyz;
    float len = max(length(fromLight), 0.001);
    float c = dot(cb.spotLightDirection.xyz, fromLight / len);
    float cosOuter = cos(radians(cb.spotLightDirection.w));
    float cosHalf = cos(radians(cb.spotLightDirection.w * 0.5));
    float t = clamp((c - cosOuter) / max(cosHalf - cosOuter, 1e-4), 0.0, 1.0);
    return t * t;
}

vec3 lightColour() {
    return cb.spotLightColor.rgb * cb.spotLightColor.w / max(0.05, luma(cb.sunColor.rgb));
}

// CookieBreakup of the .fx (the same frequencies and amplitudes).
float cookieBreakup(vec2 cuv, float strength) {
    vec2 p = (cuv - 0.5) * 2.0;
    float r = clamp(length(p), 0.0, 1.0);
    float warp = 0.4 * sin(7.0 * p.x) + 0.4 * sin(5.0 * p.y);
    float ring = 0.5 + 0.5 * sin(36.0 * r + warp);
    float a = 4.0 * sin(3.0 * p.x + 1.7 * p.y);
    float b = 4.0 * sin(2.6 * p.y + 0.9 * p.x);
    float mottle = 0.5 + 0.5 * sin(a) * sin(b);
    float amp = mix(0.10, 0.24, r);
    float pattern = 1.0 + amp * (2.0 * ring - 1.0) + 0.12 * (mottle - 0.5) - 0.08 * smoothstep(0.7, 1.0, r);
    return mix(1.0, max(pattern, 0.0), clamp(strength, 0.0, 1.0));
}

// The light-space clip position of a surface point (D3D mul(float4(pos,1), VP) is VP * v).
vec4 lightClip(vec3 pos) { return cb.flashLightViewProj * vec4(pos, 1.0); }
vec2 clipToUv(vec4 clip) { return ScreenCoordToTexCoord(clip).xy; }
