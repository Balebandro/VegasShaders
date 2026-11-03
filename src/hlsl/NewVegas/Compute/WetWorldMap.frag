// WetWorld's first three passes as a Vulkan pass (needs NVR DXVK): WetMap, then BlurWetMap across and down, line for line
// Effects\WetWorld.fx.hlsl (effects/WetWorld.cpp ExtRender). passIndex: 0 WetMap, 1 BlurWetMap (1, 0), 2 BlurWetMap (0, 1).
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "Common.glsl"

layout(set = 1, binding = 0) uniform texture2D t_rendered;   // TESR_RenderedBuffer (LINEAR, CLAMP): the image the blur reads
layout(set = 1, binding = 1) uniform texture2D t_depth;      // TESR_DepthBuffer (LINEAR, CLAMP)
layout(set = 1, binding = 2) uniform texture2D t_depthVM;    // TESR_DepthBufferViewModel (LINEAR, CLAMP)
layout(set = 1, binding = 3) uniform texture2D t_ortho;      // TESR_OrthoMapBuffer (LINEAR, CLAMP)
layout(set = 1, binding = 4) uniform texture2D t_normals;    // TESR_NormalsBuffer (point, CLAMP)

layout(push_constant) uniform Push {
    uint sLin;          // LINEAR, CLAMP (heap index, filled in by the extension)
    uint sPoint;        // POINT, CLAMP
    uint sMipWrap;      // not used by these passes
    uint sMipClamp;     // not used by these passes
    uint passIndex;     // 0 WetMap, 1 blur across, 2 blur down
    uint pad[3];
} pc;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 o_color;

#include "WetWorldCommon.glsl"

// Wet-map pass
vec4 WetMap(vec2 uv) {
    float depth = readDepth(uv);
    if (depth > DrawD)
        return vec4(0.0, 0.0, 0.0, 1.0);
    vec4 surfacePosition = GetOrthoSurfacePosition(uv);
    float vmMask = GetViewModelMask(uv);
    float coverage = ComputeDepressionMask(surfacePosition, depth, vmMask);
    float dotUp = dot(GetWorldNormal(uv), vec3(0.0, 0.0, 1.0));
    vec4 roofPosition = surfacePosition * c.shadowToLight;
    float flatCoverage = GetRoofMask(roofPosition) * smoothstep(0.65, 0.92, dotUp)
        * (1.0 - vmMask);
    float flatWetCoverage = flatCoverage * smoothstep(0.90, 0.97, dotUp);
    return vec4(coverage, flatCoverage, flatWetCoverage, 1.0);
}

// Wet-map blur pass
vec4 BlurWetMap(vec2 uv, vec2 offsetMask) {
    float blurRadius = PuddleBlurRadius;
    float centerDepth = readDepth(uv);
    vec2 blurStep = c.reciprocalResolution.xy * offsetMask * max(blurRadius, 0.0) * 0.5;
    float depthTolerance = max(PuddleBlurDepthTolerance * max(centerDepth, 1.0), 0.0001);
    float weightSum = 0.375;
    vec3 coverage = texture(sampler2D(t_rendered, NVR_SAMPLER(pc.sLin)), uv).rgb * weightSum;
    for (int tap = -2; tap <= 2; tap++) {
        if (tap != 0) {
            vec2 sampleUV = saturate(uv + blurStep * float(tap));
            float sampleDepth = readDepth(sampleUV);
            float depthWeight = 1.0 - smoothstep(depthTolerance * 0.5,
                depthTolerance, abs(sampleDepth - centerDepth));
            float kernelWeight = abs(tap) == 1 ? 0.25 : 0.0625;
            float weight = kernelWeight * depthWeight;
            coverage += texture(sampler2D(t_rendered, NVR_SAMPLER(pc.sLin)), sampleUV).rgb * weight;
            weightSum += weight;
        }
    }
    coverage /= weightSum;
    return vec4(coverage, 1.0);
}

void main() {
    if (pc.passIndex == 0u) o_color = WetMap(vUV);
    else if (pc.passIndex == 1u) o_color = BlurWetMap(vUV, vec2(1.0, 0.0));
    else o_color = BlurWetMap(vUV, vec2(0.0, 1.0));
}
