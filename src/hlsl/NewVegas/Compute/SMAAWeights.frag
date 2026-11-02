// SMAA's blending weight calculation as a Vulkan pass (needs NVR DXVK): Effects/SMAA.fx.hlsl technique BlendWeightCalculation
// line for line, with the parts of Includes/SMAA.hlsl it uses inlined (SMAA_HLSL_3, SMAA 1x: subsampleIndices 0; diagonal and
// corner detection on, SMAA_MAX_SEARCH_STEPS_DIAG 16, SMAA_CORNER_ROUNDING 25). effects/SMAA.cpp ExtRender.
// The D3D9 pass runs only where the edge pass left its stencil mark (StencilFunc EQUAL 1) into a target cleared to 0: here the
// mark is the edges' alpha at this pixel (SMAAEdges.frag), and an unmarked pixel writes 0 (pc.mode 1; 0 = no stencil buffer,
// every pixel runs, as D3D9 then does). round() is floor(x + 0.5), what fxc compiles HLSL round to for ps_3_0 (add 0.5, frc, sub).
// Samplers as the .fx: edges and area LINEAR, CLAMP; search POINT, CLAMP; none has mips.
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "Common.glsl"

layout(set = 1, binding = 0) uniform texture2D t_edges;      // TESR_SMAA_Edges
layout(set = 1, binding = 1) uniform texture2D t_area;       // TESR_SMAA_AreaTex (A8L8: .ra = DXVK's swizzled view)
layout(set = 1, binding = 2) uniform texture2D t_search;     // TESR_SMAA_SearchTex (L8)

layout(push_constant) uniform Push {
    vec4 resolution;            // 0: TESR_SMAAResolution (SMAA_RT_METRICS: 1/w, 1/h, w, h)
    vec4 data;                  // 16: TESR_SMAAData (x edge threshold, y relative depth threshold, z max search steps, w subpixel shift)
    vec4 depthData;             // 32: TESR_SMAADepthData
    vec4 pred;                  // 48: TESR_SMAAPred
    vec4 toneMappingClip;       // 64: TESR_ToneMappingClip
    uint samplerIndex;          // 80: LINEAR, CLAMP
    uint pointSamplerIndex;     // 84: POINT, CLAMP
    uint mode;                  // 88: 1 = only the pixels the edge pass marked (the D3D9 stencil test)
} pc;

layout(location = 0) in vec2 uv_in;
layout(location = 0) out vec4 o_color;

#define SMAA_RT_METRICS pc.resolution
#define SMAA_MAX_SEARCH_STEPS (pc.data.z > 0.0 ? pc.data.z : 32.0)
#define SMAA_MAX_SEARCH_STEPS_DIAG 16
#define SMAA_CORNER_ROUNDING 25
#define SMAA_AREATEX_MAX_DISTANCE 16
#define SMAA_AREATEX_MAX_DISTANCE_DIAG 20
#define SMAA_AREATEX_PIXEL_SIZE (1.0 / vec2(160.0, 560.0))
#define SMAA_AREATEX_SUBTEX_SIZE (1.0 / 7.0)
#define SMAA_SEARCHTEX_SIZE vec2(66.0, 33.0)
#define SMAA_SEARCHTEX_PACKED_SIZE vec2(64.0, 16.0)
#define SMAA_CORNER_ROUNDING_NORM (float(SMAA_CORNER_ROUNDING) / 100.0)

// SMAASampleLevelZero / SMAASampleLevelZeroOffset (coord + offset * SMAA_RT_METRICS.xy) / SMAASample, each through its .fx sampler
vec4 EdgesLevelZero(vec2 coord) { return textureLod(sampler2D(t_edges, NVR_SAMPLER(pc.samplerIndex)), coord, 0.0); }
vec4 EdgesLevelZeroOffset(vec2 coord, vec2 offset) { return EdgesLevelZero(coord + offset * SMAA_RT_METRICS.xy); }
vec4 AreaLevelZero(vec2 coord) { return textureLod(sampler2D(t_area, NVR_SAMPLER(pc.samplerIndex)), coord, 0.0); }
vec4 SearchLevelZero(vec2 coord) { return textureLod(sampler2D(t_search, NVR_SAMPLER(pc.pointSamplerIndex)), coord, 0.0); }

// HLSL round under ps_3_0
vec2 Round(vec2 x) { return floor(x + 0.5); }
vec4 Round(vec4 x) { return floor(x + 0.5); }

// SMAAMovc
void SMAAMovc(bvec2 cond, inout vec2 variable, vec2 value) {
    if (cond.x) variable.x = value.x;
    if (cond.y) variable.y = value.y;
}

vec2 SMAADecodeDiagBilinearAccess(vec2 e) {
    e.r = e.r * abs(5.0 * e.r - 5.0 * 0.75);
    return Round(e);
}
vec4 SMAADecodeDiagBilinearAccess(vec4 e) {
    e.rb = e.rb * abs(5.0 * e.rb - 5.0 * 0.75);
    return Round(e);
}

vec2 SMAASearchDiag1(vec2 texcoord, vec2 dir, out vec2 e) {
    vec4 coord = vec4(texcoord, -1.0, 1.0);
    vec3 t = vec3(SMAA_RT_METRICS.xy, 1.0);
    while (coord.z < float(SMAA_MAX_SEARCH_STEPS_DIAG - 1) &&
           coord.w > 0.9) {
        coord.xyz = t * vec3(dir, 1.0) + coord.xyz;
        e = EdgesLevelZero(coord.xy).rg;
        coord.w = dot(e, vec2(0.5, 0.5));
    }
    return coord.zw;
}

vec2 SMAASearchDiag2(vec2 texcoord, vec2 dir, out vec2 e) {
    vec4 coord = vec4(texcoord, -1.0, 1.0);
    coord.x += 0.25 * SMAA_RT_METRICS.x;
    vec3 t = vec3(SMAA_RT_METRICS.xy, 1.0);
    while (coord.z < float(SMAA_MAX_SEARCH_STEPS_DIAG - 1) &&
           coord.w > 0.9) {
        coord.xyz = t * vec3(dir, 1.0) + coord.xyz;
        e = EdgesLevelZero(coord.xy).rg;
        e = SMAADecodeDiagBilinearAccess(e);
        coord.w = dot(e, vec2(0.5, 0.5));
    }
    return coord.zw;
}

vec2 SMAAAreaDiag(vec2 dist, vec2 e, float offset) {
    vec2 texcoord = vec2(SMAA_AREATEX_MAX_DISTANCE_DIAG, SMAA_AREATEX_MAX_DISTANCE_DIAG) * e + dist;
    texcoord = SMAA_AREATEX_PIXEL_SIZE * texcoord + 0.5 * SMAA_AREATEX_PIXEL_SIZE;
    texcoord.x += 0.5;
    texcoord.y += SMAA_AREATEX_SUBTEX_SIZE * offset;
    return AreaLevelZero(texcoord).ra;	// SMAA_AREATEX_SELECT under SMAA_HLSL_3
}

vec2 SMAACalculateDiagWeights(vec2 texcoord, vec2 e, vec4 subsampleIndices) {
    vec2 weights = vec2(0.0, 0.0);
    vec4 d;
    vec2 end;
    if (e.r > 0.0) {
        d.xz = SMAASearchDiag1(texcoord, vec2(-1.0,  1.0), end);
        d.x += float(end.y > 0.9);
    } else
        d.xz = vec2(0.0, 0.0);
    d.yw = SMAASearchDiag1(texcoord, vec2(1.0, -1.0), end);

    if (d.x + d.y > 2.0) {
        vec4 coords = vec4(-d.x + 0.25, d.x, d.y, -d.y - 0.25) * SMAA_RT_METRICS.xyxy + texcoord.xyxy;
        vec4 c;
        c.xy = EdgesLevelZeroOffset(coords.xy, vec2(-1.0,  0.0)).rg;
        c.zw = EdgesLevelZeroOffset(coords.zw, vec2( 1.0,  0.0)).rg;
        c.yxwz = SMAADecodeDiagBilinearAccess(c.xyzw);
        vec2 cc = vec2(2.0, 2.0) * c.xz + c.yw;
        SMAAMovc(notEqual(step(vec2(0.9), d.zw), vec2(0.0)), cc, vec2(0.0, 0.0));
        weights += SMAAAreaDiag(d.xy, cc, subsampleIndices.z);
    }

    d.xz = SMAASearchDiag2(texcoord, vec2(-1.0, -1.0), end);
    if (EdgesLevelZeroOffset(texcoord, vec2(1.0, 0.0)).r > 0.0) {
        d.yw = SMAASearchDiag2(texcoord, vec2(1.0, 1.0), end);
        d.y += float(end.y > 0.9);
    } else
        d.yw = vec2(0.0, 0.0);

    if (d.x + d.y > 2.0) {
        vec4 coords = vec4(-d.x, -d.x, d.y, d.y) * SMAA_RT_METRICS.xyxy + texcoord.xyxy;
        vec4 c;
        c.x  = EdgesLevelZeroOffset(coords.xy, vec2(-1.0,  0.0)).g;
        c.y  = EdgesLevelZeroOffset(coords.xy, vec2( 0.0, -1.0)).r;
        c.zw = EdgesLevelZeroOffset(coords.zw, vec2( 1.0,  0.0)).gr;
        vec2 cc = vec2(2.0, 2.0) * c.xz + c.yw;
        SMAAMovc(notEqual(step(vec2(0.9), d.zw), vec2(0.0)), cc, vec2(0.0, 0.0));
        weights += SMAAAreaDiag(d.xy, cc, subsampleIndices.w).gr;
    }

    return weights;
}

float SMAASearchLength(vec2 e, float offset) {
    vec2 scale = SMAA_SEARCHTEX_SIZE * vec2(0.5, -1.0);
    vec2 bias = SMAA_SEARCHTEX_SIZE * vec2(offset, 1.0);
    scale += vec2(-1.0,  1.0);
    bias  += vec2( 0.5, -0.5);
    scale *= 1.0 / SMAA_SEARCHTEX_PACKED_SIZE;
    bias *= 1.0 / SMAA_SEARCHTEX_PACKED_SIZE;
    return SearchLevelZero(scale * e + bias).r;	// SMAA_SEARCHTEX_SELECT
}

float SMAASearchXLeft(vec2 texcoord, float end) {
    vec2 e = vec2(0.0, 1.0);
    while (texcoord.x > end &&
           e.g > 0.8281 &&
           e.r == 0.0) {
        e = EdgesLevelZero(texcoord).rg;
        texcoord = -vec2(2.0, 0.0) * SMAA_RT_METRICS.xy + texcoord;
    }
    float offset = -(255.0 / 127.0) * SMAASearchLength(e, 0.0) + 3.25;
    return SMAA_RT_METRICS.x * offset + texcoord.x;
}

float SMAASearchXRight(vec2 texcoord, float end) {
    vec2 e = vec2(0.0, 1.0);
    while (texcoord.x < end &&
           e.g > 0.8281 &&
           e.r == 0.0) {
        e = EdgesLevelZero(texcoord).rg;
        texcoord = vec2(2.0, 0.0) * SMAA_RT_METRICS.xy + texcoord;
    }
    float offset = -(255.0 / 127.0) * SMAASearchLength(e, 0.5) + 3.25;
    return -SMAA_RT_METRICS.x * offset + texcoord.x;
}

float SMAASearchYUp(vec2 texcoord, float end) {
    vec2 e = vec2(1.0, 0.0);
    while (texcoord.y > end &&
           e.r > 0.8281 &&
           e.g == 0.0) {
        e = EdgesLevelZero(texcoord).rg;
        texcoord = -vec2(0.0, 2.0) * SMAA_RT_METRICS.xy + texcoord;
    }
    float offset = -(255.0 / 127.0) * SMAASearchLength(e.gr, 0.0) + 3.25;
    return SMAA_RT_METRICS.y * offset + texcoord.y;
}

float SMAASearchYDown(vec2 texcoord, float end) {
    vec2 e = vec2(1.0, 0.0);
    while (texcoord.y < end &&
           e.r > 0.8281 &&
           e.g == 0.0) {
        e = EdgesLevelZero(texcoord).rg;
        texcoord = vec2(0.0, 2.0) * SMAA_RT_METRICS.xy + texcoord;
    }
    float offset = -(255.0 / 127.0) * SMAASearchLength(e.gr, 0.5) + 3.25;
    return -SMAA_RT_METRICS.y * offset + texcoord.y;
}

vec2 SMAAArea(vec2 dist, float e1, float e2, float offset) {
    vec2 texcoord = vec2(SMAA_AREATEX_MAX_DISTANCE, SMAA_AREATEX_MAX_DISTANCE) * Round(4.0 * vec2(e1, e2)) + dist;
    texcoord = SMAA_AREATEX_PIXEL_SIZE * texcoord + 0.5 * SMAA_AREATEX_PIXEL_SIZE;
    texcoord.y = SMAA_AREATEX_SUBTEX_SIZE * offset + texcoord.y;
    return AreaLevelZero(texcoord).ra;	// SMAA_AREATEX_SELECT under SMAA_HLSL_3
}

void SMAADetectHorizontalCornerPattern(inout vec2 weights, vec4 texcoord, vec2 d) {
    vec2 leftRight = step(d.xy, d.yx);
    vec2 rounding = (1.0 - SMAA_CORNER_ROUNDING_NORM) * leftRight;
    rounding /= leftRight.x + leftRight.y;
    vec2 factor = vec2(1.0, 1.0);
    factor.x -= rounding.x * EdgesLevelZeroOffset(texcoord.xy, vec2(0.0,  1.0)).r;
    factor.x -= rounding.y * EdgesLevelZeroOffset(texcoord.zw, vec2(1.0,  1.0)).r;
    factor.y -= rounding.x * EdgesLevelZeroOffset(texcoord.xy, vec2(0.0, -2.0)).r;
    factor.y -= rounding.y * EdgesLevelZeroOffset(texcoord.zw, vec2(1.0, -2.0)).r;
    weights *= clamp(factor, 0.0, 1.0);
}

void SMAADetectVerticalCornerPattern(inout vec2 weights, vec4 texcoord, vec2 d) {
    vec2 leftRight = step(d.xy, d.yx);
    vec2 rounding = (1.0 - SMAA_CORNER_ROUNDING_NORM) * leftRight;
    rounding /= leftRight.x + leftRight.y;
    vec2 factor = vec2(1.0, 1.0);
    factor.x -= rounding.x * EdgesLevelZeroOffset(texcoord.xy, vec2( 1.0, 0.0)).g;
    factor.x -= rounding.y * EdgesLevelZeroOffset(texcoord.zw, vec2( 1.0, 1.0)).g;
    factor.y -= rounding.x * EdgesLevelZeroOffset(texcoord.xy, vec2(-2.0, 0.0)).g;
    factor.y -= rounding.y * EdgesLevelZeroOffset(texcoord.zw, vec2(-2.0, 1.0)).g;
    weights *= clamp(factor, 0.0, 1.0);
}

// SMAABlendingWeightCalculationPS
vec4 SMAABlendingWeightCalculationPS(vec2 texcoord, vec2 pixcoord, vec4 offset[3], vec4 subsampleIndices) {
    vec4 weights = vec4(0.0, 0.0, 0.0, 0.0);
    vec2 e = texture(sampler2D(t_edges, NVR_SAMPLER(pc.samplerIndex)), texcoord).rg;	// SMAASample

    if (e.g > 0.0) { // Edge at north
        weights.rg = SMAACalculateDiagWeights(texcoord, e, subsampleIndices);

        if (weights.r == -weights.g) { // weights.r + weights.g == 0.0
        vec2 d;
        vec3 coords;
        coords.x = SMAASearchXLeft(offset[0].xy, offset[2].x);
        coords.y = offset[1].y;
        d.x = coords.x;
        float e1 = EdgesLevelZero(coords.xy).r;
        coords.z = SMAASearchXRight(offset[0].zw, offset[2].y);
        d.y = coords.z;
        d = abs(Round(SMAA_RT_METRICS.zz * d + -pixcoord.xx));
        vec2 sqrt_d = sqrt(d);
        float e2 = EdgesLevelZeroOffset(coords.zy, vec2(1.0, 0.0)).r;
        weights.rg = SMAAArea(sqrt_d, e1, e2, subsampleIndices.y);
        coords.y = texcoord.y;
        SMAADetectHorizontalCornerPattern(weights.rg, coords.xyzy, d);
        } else
            e.r = 0.0; // Skip vertical processing.
    }

    if (e.r > 0.0) { // Edge at west
        vec2 d;
        vec3 coords;
        coords.y = SMAASearchYUp(offset[1].xy, offset[2].z);
        coords.x = offset[0].x;
        d.x = coords.y;
        float e1 = EdgesLevelZero(coords.xy).g;
        coords.z = SMAASearchYDown(offset[1].zw, offset[2].w);
        d.y = coords.z;
        d = abs(Round(SMAA_RT_METRICS.ww * d + -pixcoord.yy));
        vec2 sqrt_d = sqrt(d);
        float e2 = EdgesLevelZeroOffset(coords.xz, vec2(0.0, 1.0)).g;
        weights.ba = SMAAArea(sqrt_d, e1, e2, subsampleIndices.x);
        coords.x = texcoord.x;
        SMAADetectVerticalCornerPattern(weights.ba, coords.xyxz, d);
    }

    return weights;
}

void main() {
    // the D3D9 stencil test: the edge pass did not discard this pixel
    if (pc.mode == 1u && texelFetch(sampler2D(t_edges, NVR_SAMPLER(pc.samplerIndex)), ivec2(gl_FragCoord.xy), 0).a < 0.5) {
        o_color = vec4(0.0);
        return;
    }

    // DX9_SMAABlendingWeightCalculationVS: texcoord += SMAA_SUBPIXEL_SHIFT; SMAABlendingWeightCalculationVS
    vec2 texcoord = uv_in + pc.data.w * pc.resolution.xy;
    vec2 pixcoord = texcoord * SMAA_RT_METRICS.zw;
    vec4 offset[3];
    offset[0] = SMAA_RT_METRICS.xyxy * vec4(-0.25, -0.125, 1.25, -0.125) + texcoord.xyxy;
    offset[1] = SMAA_RT_METRICS.xyxy * vec4(-0.125, -0.25, -0.125, 1.25) + texcoord.xyxy;
    offset[2] = SMAA_RT_METRICS.xxyy * (vec4(-2.0, 2.0, -2.0, 2.0) * float(SMAA_MAX_SEARCH_STEPS)) + vec4(offset[0].xz, offset[1].yw);

    o_color = SMAABlendingWeightCalculationPS(texcoord, pixcoord, offset, vec4(0.0));
}
