// Shared by NVR's Vulkan passes (needs NVR DXVK; core/DxvkCompute.h). GLSL ports of Effects/Includes/Helpers.hlsl.
// Every pass: set 0 binding 0 is DXVK's sampler heap; its index comes from the push block's samplerIndex (low 16 bits).
#extension GL_EXT_nonuniform_qualifier : enable

layout(set = 0, binding = 0) uniform sampler s_samplers[];

// (2026-10-26) The index is a push value, the same for every pixel of a draw: no nonuniformEXT, as DXVK's own D3D9 shaders
// index the heap (NonUniform made NVIDIA guard every texture read for an index that never differs).
#define NVR_SAMPLER(index) s_samplers[(index) & 0xFFFFu]

vec3 linearize(vec3 c) {
    vec3 lo = c / 12.92;
    vec3 hi = pow(abs((c + 0.055) / 1.055), vec3(2.4));
    return mix(hi, lo, lessThanEqual(c, vec3(0.04045)));
}
vec4 linearize(vec4 c) { return vec4(linearize(c.rgb), c.a); }

vec3 delinearize(vec3 c) {
    vec3 lo = c * 12.92;
    vec3 hi = (pow(abs(c), vec3(1.0 / 2.4)) * 1.055) - 0.055;
    return mix(hi, lo, lessThanEqual(c, vec3(0.0031308)));
}
vec4 delinearize(vec4 c) { return vec4(delinearize(c.rgb), c.a); }

float luma(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }
float luma(vec4 c) { return luma(c.rgb); }

// HLSL pows(a, b) = pow(abs(a), b) * sign(a)
vec3 pows(vec3 a, float b) { return pow(abs(a), vec3(b)) * sign(a); }
float pows(float a, float b) { return pow(abs(a), b) * sign(a); }
float saturate(float x) { return clamp(x, 0.0, 1.0); }
vec3 saturate(vec3 x) { return clamp(x, vec3(0.0), vec3(1.0)); }
