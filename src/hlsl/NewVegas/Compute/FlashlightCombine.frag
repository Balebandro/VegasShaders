// Flashlight's composite (Combine(false), and Combine(true) of the EdgeFix techniques with pc.mode 1) as a Vulkan pass (needs
// NVR DXVK): Effects/Flashlight.fx.hlsl line for line, with its EdgeFix helpers. effects/Flashlight.cpp ExtRender.
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "Flashlight.glsl"

// ---- UNOFFICIAL: depth-aware edges ([Shaders.Flashlight.Main] EdgeFix); see the .fx.hlsl for the reasoning ----
const float EdgeDepthStart = 0.02;
const float EdgeDepthRamp = 0.03;
const float EdgeGate = 0.005;

float DepthBreak(float dc, float a, float b) {
    return abs(dc / max(a, 1.0) + dc / max(b, 1.0) - 2.0);
}

float DepthBreakAround(vec2 coord, float dc) {
    vec2 h = 0.5 * cb.reciprocalResolution.xy;
    float topLeft = readDepthLod(coord - h), bottomRight = readDepthLod(coord + h);
    return DepthBreak(dc, topLeft, bottomRight);
}

struct EdgeSplit {
    float weight;           // how much of the split result to use: 0 off edges, and where both sides look alike
    float coverage;         // share of the pixel taken by the near side
    vec2 uvNear, uvFar;     // the reference neighbours of the two sides
    vec3 nearColor, farColor;
    vec3 nearPart, farPart; // this pixel's colour, split between the two sides
};

vec3 SourceColorLod(vec2 coord, bool sourceIsLinear) {
    vec4 c = LINEAR_LOD(t_source, coord);
    return sourceIsLinear ? c.rgb : linearize(c.rgb);
}

// Combine's own modulation of the light by the surface colour (dimmer on already bright surfaces)
vec3 ModulatedLight(vec3 part, vec3 surface, vec3 light) {
    return part * max(vec3(0.0), luma(exp(-surface * 3.5)) * light);
}

EdgeSplit FindEdgeSplit(vec2 coord, float dc, vec3 C, bool sourceIsLinear) {
    EdgeSplit e = EdgeSplit(0.0, 0.0, vec2(0.0), vec2(0.0), vec3(0.0), vec3(0.0), vec3(0.0), vec3(0.0));
    vec2 px = cb.reciprocalResolution.xy;
    vec2 uvFar = coord;
    float dNear = 1e30, dFar = -1.0;
    vec2 offsets[8] = vec2[8](vec2(-px.x, 0.0), vec2(px.x, 0.0), vec2(0.0, -px.y), vec2(0.0, px.y),
        vec2(-px.x, -px.y), vec2(px.x, -px.y), vec2(-px.x, px.y), vec2(px.x, px.y));
    float depths[8];
    for (int i = 0; i < 8; i++) {
        depths[i] = readDepthLod(coord + offsets[i]);
        dNear = min(dNear, depths[i]);
        if (depths[i] > dFar) { dFar = depths[i]; uvFar = coord + offsets[i]; }
    }
    // Opposite pairs: left/right, up/down and the two diagonals
    float breakSize = max(max(DepthBreak(dc, depths[0], depths[1]), DepthBreak(dc, depths[2], depths[3])),
        max(DepthBreak(dc, depths[4], depths[7]), DepthBreak(dc, depths[5], depths[6])));
    float edge = saturate((breakSize - EdgeDepthStart) / EdgeDepthRamp);
    if (edge <= 0.0) return e;

    // The far side's reference is the farthest neighbour; the near side's the near direct neighbour least like it
    vec3 Cf = SourceColorLod(uvFar, sourceIsLinear);
    vec3 Cn = Cf;
    vec2 uvNear = uvFar;
    float nearSplit = mix(dNear, dFar, 0.5);
    float purest = -1.0;
    for (int k = 0; k < 4; k++) {
        if (depths[k] < nearSplit) {
            vec2 t = coord + offsets[k];
            vec3 c = SourceColorLod(t, sourceIsLinear);
            vec3 diff = c - Cf;
            float dist2 = dot(diff, diff);
            if (dist2 > purest) { purest = dist2; Cn = c; uvNear = t; }
        }
    }

    // Coverage of the near side: where this pixel's colour sits between the two references
    vec3 dC = Cn - Cf;
    float dd = dot(dC, dC);
    float a = saturate(dot(C - Cf, dC) / max(dd, 1e-10));

    // Split the pixel's own colour, so its texture detail is kept
    e.nearPart = max(a * C + a * (1.0 - a) * dC, vec3(0.0));
    e.farPart = max(C - e.nearPart, vec3(0.0));

    // Two sides of nearly the same colour say nothing about coverage: keep the old result there
    float contrast = dd / max(max(dot(Cn, Cn), dot(Cf, Cf)), 1e-8);
    e.weight = edge * saturate((contrast - 0.0025) / 0.0075);
    e.coverage = a;
    e.uvNear = uvNear;
    e.uvFar = uvFar;
    e.nearColor = Cn;
    e.farColor = Cf;
    return e;
}

vec3 SplitLight(EdgeSplit e) {
    vec3 Ln = LINEAR_LOD(t_rendered, e.uvNear).rgb;
    vec3 Lf = LINEAR_LOD(t_rendered, e.uvFar).rgb;
    return ModulatedLight(e.nearPart, e.nearColor, Ln) + ModulatedLight(e.farPart, e.farColor, Lf);
}

// Depth-aware upsample of the half resolution beam buffer for the pixel at coord (EdgeFix 2)
vec3 BeamUpsampled(vec2 coord, float dc, bool isFlat) {
    vec2 px = cb.reciprocalResolution.xy;
    vec2 halfPx = FL_BEAMTEXEL;
    vec2 pos = floor(coord / px) * (px / halfPx);
    if (isFlat) return LINEAR_LOD(t_volumetric, (pos + 0.5) * halfPx).rgb;

    vec2 base = floor(pos);
    vec2 f = pos - base;
    vec2 corners[4] = vec2[4](vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(0.0, 1.0), vec2(1.0, 1.0));
    vec4 rel;
    for (int i = 0; i < 4; i++)
        rel[i] = abs(readDepthLod(0.5 * px + (base + corners[i]) * halfPx) - dc) / max(dc, 1.0);
    if (max(max(rel.x, rel.y), max(rel.z, rel.w)) < 0.04)
        return LINEAR_LOD(t_volumetric, (pos + 0.5) * halfPx).rgb;

    vec3 sum = vec3(0.0);
    float wsum = 0.0;
    vec3 closest = vec3(0.0);
    float closestRel = 1e30;
    for (int k = 0; k < 4; k++) {
        vec3 v = LINEAR_LOD(t_volumetric, (base + corners[k] + 0.5) * halfPx).rgb;
        vec2 wb2 = mix(1.0 - f, f, corners[k]);
        float wb = wb2.x * wb2.y;
        float w = wb / (1.0 + sqr(rel[k] / 0.08));
        sum += v * w;
        wsum += w;
        if (wb > 0.0 && rel[k] < closestRel) { closestRel = rel[k]; closest = v; }
    }
    return wsum > 1e-4 ? sum / wsum : closest;
}

// The shaft in front of the pixel at coord, with the same yield and soft ceiling Combine applies
vec3 BeamAt(vec2 coord, float depth, bool isFlat) {
    vec3 vol = BeamUpsampled(coord, depth, isFlat);
    vec3 surfPos = cb.cameraPosition.xyz + toWorld(coord) * depth;
    vec3 surfToL = cb.spotLightPosition.xyz - surfPos;
    float surfDist = length(surfToL);
    float surfCone = pow(invlerps(
        cos(radians(cb.spotLightDirection.w)),
        cos(radians(cb.spotLightDirection.w * 0.5)),
        shades(cb.spotLightDirection.xyz, -surfToL / max(surfDist, 0.001))), 2.0);
    float sSurf = saturate(sqr(surfDist / cb.spotLightPosition.w));
    float surfAtten = saturate(sqr(1.0 - sSurf) / (1.0 + 5.0 * sSurf));
    vol *= 1.0 - 0.85 * saturate(surfCone * surfAtten * 4.0);
    return vol / (1.0 + luma(vol) * 2.5);
}

void main() {
    bool edgeFix = pc.mode != 0u;

    // With RenderPreTonemapping the source is already linear: only decode / re-encode on the post tonemapping LDR surface
    bool sourceIsLinear = FL_LINEARSOURCE > 0.5;
    vec4 source = LINEAR_LOD(t_source, uv);
    vec4 color = sourceIsLinear ? source : linearize(source);
    vec4 light = LINEAR_LOD(t_rendered, uv);

    vec3 addLight = color.rgb * max(vec3(0.0), luma(exp(-color.rgb * 3.5)) * light.rgb); // modulate light with base color brightness

    // UNOFFICIAL: depth-aware edges, see FindEdgeSplit
    EdgeSplit edge = EdgeSplit(0.0, 0.0, vec2(0.0), vec2(0.0), vec3(0.0), vec3(0.0), vec3(0.0), vec3(0.0));
    float depthHere = 0.0;
    bool isFlat = true;
    bool edgeBeam = edgeFix && FL_EDGEBEAM > 0.5 && cb.volumetricControl.x > 0.0;
    if (edgeFix) {
        // Nothing to fix where neither the pool nor (EdgeFix 2) the shaft reaches
        vec3 shaftHere = edgeBeam ? LINEAR_LOD(t_volumetric, uv).rgb : vec3(0.0);
        if (luma(light.rgb) > 0.0 || luma(shaftHere) > 0.0) {
            depthHere = readDepthLod(uv);
            isFlat = DepthBreakAround(uv, depthHere) < EdgeGate;
            if (!isFlat) {
                edge = FindEdgeSplit(uv, depthHere, color.rgb, sourceIsLinear);
                if (edge.weight > 0.0) addLight = mix(addLight, SplitLight(edge), edge.weight);
            }
        }
    }

    // In-air light shaft from the FlashlightBeam effect, gated on the same value the march is gated on
    if (cb.volumetricControl.x > 0.0) {
        // The sampler is bilinear, so this upsamples the half res buffer for free
        vec3 vol = LINEAR_LOD(t_volumetric, uv).rgb;

        // The shaft yields wherever this pixel's surface is already inside the beam
        vec3 surfPos = cb.cameraPosition.xyz + toWorld(uv) * readDepth(uv);
        vec3 surfToL = cb.spotLightPosition.xyz - surfPos;
        float surfDist = length(surfToL);
        float surfCone = pow(invlerps(
            cos(radians(cb.spotLightDirection.w)),
            cos(radians(cb.spotLightDirection.w * 0.5)),
            shades(cb.spotLightDirection.xyz, -surfToL / max(surfDist, 0.001))), 2.0);
        float sSurf = saturate(sqr(surfDist / cb.spotLightPosition.w));
        float surfAtten = saturate(sqr(1.0 - sSurf) / (1.0 + 5.0 * sSurf));
        vol *= 1.0 - 0.85 * saturate(surfCone * surfAtten * 4.0);

        // Soft ceiling: dim shafts pass nearly unchanged
        vol /= 1.0 + luma(vol) * 2.5;
        // UNOFFICIAL (EdgeFix 2): depth-aware upsample, and at an edge each side's own shaft by its share of the pixel
        if (edgeBeam) {
            vol = BeamAt(uv, depthHere, isFlat);
            if (edge.weight > 0.0) {
                vec3 nearVol = BeamAt(edge.uvNear, readDepthLod(edge.uvNear), true);
                vec3 farVol = BeamAt(edge.uvFar, readDepthLod(edge.uvFar), true);
                vol = mix(vol, edge.coverage * nearVol + (1.0 - edge.coverage) * farVol, edge.weight);
            }
        }
        addLight += vol;
    }

    // Reinhard style roll off on the added light only
    if (FL_HOTSPOTLIMIT > 0.0) {
        float limit = 1.0 / FL_HOTSPOTLIMIT;
        addLight *= limit / max(limit, luma(addLight));
    }

    color.rgb += addLight;

    o_color = sourceIsLinear ? color : delinearize(color);
}
