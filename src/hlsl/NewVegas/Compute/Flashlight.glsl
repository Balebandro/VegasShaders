// Shared by the Flashlight Vulkan passes (needs NVR DXVK): the constants, textures and helpers of Effects/Flashlight.fx.hlsl and
// its includes (Depth.hlsl, Helpers.hlsl, Normals.hlsl, Blur.hlsl), line for line. effects/Flashlight.cpp ExtRender.
#extension GL_GOOGLE_include_directive : enable
#include "Common.glsl"

// Every pass declares the same bindings; a texture a pass does not read may be a placeholder. All but the cookie have one level and
// are read at level 0 (tex2D on them is level 0 too). The depth buffer's MINFILTER ANISOTROPIC runs at the effect samplers'
// MAXANISOTROPY 1 on a texture without mips: LINEAR, as ShadowsExteriors' port reads the same declaration.
layout(set = 1, binding = 0) uniform texture2D t_source;      // TESR_SourceBuffer: the image before the effect (LINEAR, CLAMP)
layout(set = 1, binding = 1) uniform texture2D t_rendered;    // TESR_RenderedBuffer: the chain's current image (LINEAR, CLAMP)
layout(set = 1, binding = 2) uniform texture2D t_depth;       // TESR_DepthBuffer: linear depth / farZ (see above)
layout(set = 1, binding = 3) uniform texture2D t_normals;     // TESR_NormalsBuffer (LINEAR, CLAMP)
layout(set = 1, binding = 4) uniform texture2D t_shadow;      // TESR_ShadowSpotlightBuffer0 (LINEAR, CLAMP)
layout(set = 1, binding = 5) uniform texture2D t_cookie;      // TESR_SpotLightTexture: Effects\Flashlight.png (LINEAR mips, CLAMP)
layout(set = 1, binding = 6) uniform texture2D t_volumetric;  // TESR_VolumetricBuffer: FlashlightBeam's buffer (LINEAR, no mips, CLAMP)

// The constants as the D3D9 constant table holds them this frame, uploaded once per frame (the matrices make it bigger than push
// data). Matrices: the D3DX matrix as stored (SetMatrix keeps the logical matrix), so cb.m[i][j] is the HLSL's m[i][j],
// mul(v, M) is cb.m * v and mul(M, v) is v * cb.m.
layout(set = 2, binding = 0, std140) uniform Constants {
    mat4 flashLightViewProj;       // TESR_FlashLightViewProjTransform
    mat4 viewTransform;            // TESR_ViewTransform
    mat4 projectionTransform;      // TESR_ProjectionTransform
    vec4 cameraPosition;           // TESR_CameraPosition
    vec4 cameraData;               // TESR_CameraData: x near, y far
    vec4 spotLightPosition;        // TESR_SpotLightPosition: xyz position, w radius
    vec4 spotLightColor;           // TESR_SpotLightColor: rgb, w dimmer
    vec4 spotLightDirection;       // TESR_SpotLightDirection: xyz axis, w outside angle
    vec4 sunColor;                 // TESR_SunColor
    vec4 reciprocalResolution;     // TESR_ReciprocalResolution
    vec4 flashLightTuning;         // TESR_FlashLightTuning: x near fade, y soft edges, z hotspot limit, w cookie strength
    vec4 flashLightComposite;      // TESR_FlashLightComposite: x source is linear, y EdgeFix beam, zw 1 / beam buffer size
    vec4 volumetricControl;        // TESR_VolumetricControl: x beam strength, 0 when FlashlightBeam is off
} cb;

// Per pass (32 bytes).
layout(push_constant) uniform Push {
    vec2 offsetMask;               // 0: DepthBlur's OffsetMaskH (1, 0) or OffsetMaskV (0, 1)
    uint mode;                     // 8: Flashlight 1 = no shadow pass (NoShadow's 1); BoxBlur 0 min, 1 avg; Combine 1 = edgeFix
    uint linearSampler;            // 12: LINEAR, CLAMP
    uint cookieSampler;            // 16: LINEAR with mips, CLAMP
} pc;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o_color;

#define LINEAR_LOD(t, coord) textureLod(sampler2D(t, NVR_SAMPLER(pc.linearSampler)), (coord), 0.0)

#define FL_NEARFADE         cb.flashLightTuning.x
#define FL_SOFTEDGE         cb.flashLightTuning.y
#define FL_HOTSPOTLIMIT     cb.flashLightTuning.z
#define FL_COOKIESTRENGTH   cb.flashLightTuning.w
#define FL_LINEARSOURCE     cb.flashLightComposite.x
#define FL_EDGEBEAM         cb.flashLightComposite.y
#define FL_BEAMTEXEL        cb.flashLightComposite.zw

// Helpers.hlsl
float shades(vec3 n, vec3 l) { return saturate(dot(n, l)); }
float invlerps(float a, float b, float t) { return saturate((t - a) / (b - a)); }
float sqr(float v) { return v * v; }
vec3 sqr(vec3 v) { return v * v; }

// Depth.hlsl readDepth and readDepthLod (the same read on a texture without mips)
float readDepth(vec2 coord) {
    return LINEAR_LOD(t_depth, coord).x * cb.cameraData.y;
}
float readDepthLod(vec2 coord) {
    return LINEAR_LOD(t_depth, coord).x * cb.cameraData.y;
}

// Depth.hlsl toWorld
vec3 toWorld(vec2 tex) {
    vec3 v = vec3(cb.viewTransform[0][2], cb.viewTransform[1][2], cb.viewTransform[2][2]);
    v += (1.0 / cb.projectionTransform[0][0] * (2.0 * tex.x - 1.0)) * vec3(cb.viewTransform[0][0], cb.viewTransform[1][0], cb.viewTransform[2][0]);
    v += (-1.0 / cb.projectionTransform[1][1] * (2.0 * tex.y - 1.0)) * vec3(cb.viewTransform[0][1], cb.viewTransform[1][1], cb.viewTransform[2][1]);
    return v;
}

// Normals.hlsl GetWorldNormal: mul(TESR_ViewTransform, normal)
vec3 GetWorldNormal(vec2 coord) {
    vec4 normal = vec4(LINEAR_LOD(t_normals, coord).xyz * 2.0 - 1.0, 1.0);
    return (normal * cb.viewTransform).xyz;
}

// Flashlight.fx.hlsl ScreenCoordToTexCoord
vec4 ScreenCoordToTexCoord(vec4 coord) {
    coord.xyz /= coord.w;
    coord.x = coord.x * 0.5 + 0.5;
    coord.y = coord.y * -0.5 + 0.5;
    return coord;
}
