// DepthOfField's push block and helpers for its Vulkan passes (needs NVR DXVK; effects/DepthOfField.cpp DofPush, 224 bytes).
// Include after Common.glsl. Line for line with Effects/DepthOfField.fx.hlsl in native frames (the Vulkan passes are not used in
// render-rectangle frames, so the rectangle helpers are the identity here). The matrices are the D3DXMATRIX memory as stored, so
// HLSL T[i][j] is pc.viewTransform[i][j] (GLSL indexes the column first, the same element).
// Pass files: DepthOfField.frag (distant blur), DepthOfFieldFocus, DepthOfFieldMosaic, DepthOfFieldBokeh, DepthOfFieldCombine,
// DepthOfFieldCombineBokeh (.frag).

layout(push_constant) uniform Push {
    mat4  viewTransform;        // 0
    mat4  projectionTransform;  // 64
    vec4  reciprocalResolution; // 128: TESR_ReciprocalResolution (xy)
    vec4  blur;                 // 144: x distant blur on, y start, z end (game units), w base blur radius (texels)
    vec4  data;                 // 160: x focus scale (BlurFallOff x enabled), y radius (texels), z diameter range, w near cut-off
    vec4  cameraPosition;       // 176
    vec4  cameraData;           // 192: y far Z
    uint  samplerIndex;         // 208: LINEAR, CLAMP
    float useBokeh;             // 212: the bokeh blur passes: 1 bokeh select, 0 plain (other passes ignore it)
    vec2  jitter;               // 216: JitterFix depth offset in UV (0 when off)
} pc;

#define DOF_SAMPLE(tex, uv) textureLod(sampler2D(tex, NVR_SAMPLER(pc.samplerIndex)), (uv), 0.0)

const float DOF_TINY = 0.0001;
const float DOF_SKY_DEPTH = 0.9999;

float dofLuma(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }

// HLSL Depth.hlsl toWorld, with the view and projection matrices of the push block.
vec3 dofToWorld(vec2 uv) {
    vec3 v = vec3(pc.viewTransform[0][2], pc.viewTransform[1][2], pc.viewTransform[2][2]);
    v += (1.0 / pc.projectionTransform[0][0] * (2.0 * uv.x - 1.0)) * vec3(pc.viewTransform[0][0], pc.viewTransform[1][0], pc.viewTransform[2][0]);
    v += (-1.0 / pc.projectionTransform[1][1] * (2.0 * uv.y - 1.0)) * vec3(pc.viewTransform[0][1], pc.viewTransform[1][1], pc.viewTransform[2][1]);
    return v;
}

// Near cut-off attenuation of the near blur (see the .fx.hlsl): 0.5 at the camera, 0 at half the cut-off, 1 from the cut-off on.
float dofNearCutAtten(float d) {
    float c = pc.data.w;
    if (c <= DOF_TINY) return 1.0;
    float h = 0.5 * c;
    return 0.5 * (1.0 - smoothstep(0.0, h, d)) + smoothstep(h, c, d);
}

// Distance blur amount (0..1): the depth ramp, kept off high points.
float dofDistantAmount(float dist, vec2 uv) {
    if (pc.blur.x == 0.0) return 0.0;
    float ramp = clamp((dist - pc.blur.y) / max(pc.blur.z - pc.blur.y, 1.0), 0.0, 1.0);
    float height = pc.cameraPosition.z + dofToWorld(uv).z * dist;
    return ramp * clamp((100000.0 - height) / 95000.0, 0.0, 1.0);
}

// Quadrants (native frame: local = uv). q: quadrant per axis (0 or 1); l: position inside it (0..1).
vec2 dofQuadOf(vec2 uv) { return min(floor(uv * 2.0), vec2(1.0)); }
vec2 dofQuadLocal(vec2 uv) { vec2 l = uv * 2.0; return l - min(floor(l), vec2(1.0)); }

// The packed texture coordinate of local position l in quadrant q, kept half a texel inside the quadrant.
vec2 dofPacked(vec2 q, vec2 l) {
    vec2 tex = pc.reciprocalResolution.xy;
    return clamp((q + l) * 0.5, q * 0.5 + 0.5 * tex, (q + 1.0) * 0.5 - 0.5 * tex);
}
