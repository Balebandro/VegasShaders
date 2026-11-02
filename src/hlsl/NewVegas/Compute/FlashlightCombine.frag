// Flashlight composite (Vulkan, needs NVR DXVK): CompositePS and CompositeEdgePS of Effects/Flashlight.fx.hlsl, line for line. Mode 0: the
// plain composite; mode 1: the depth-aware one (EdgeFix). Bindings: 0 = the image before the effect (source), 1 = the blurred light (the
// chain's current image), 2 = depth, 6 = the beam shaft (half resolution). Samplers: linear (push 12) for all of them.
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "Flashlight.glsl"




vec3 sourceAt(vec2 p) {
    return textureLod(sampler2D(t_source, NVR_SAMPLER(pc.linearSampler)), p, 0.0).rgb;
}
vec3 linearSource(vec2 p) {
    vec3 s = sourceAt(p);
    return cb.flashLightComposite.x > 0.5 ? s : linearize(s);
}
vec3 lightAt(vec2 p) {
    return textureLod(sampler2D(t_rendered, NVR_SAMPLER(pc.linearSampler)), p, 0.0).rgb;
}
float storedDepthAt(vec2 p) {
    return textureLod(sampler2D(t_depth, NVR_SAMPLER(pc.linearSampler)), p, 0.0).r;
}

// The added light of one pixel: source times the light, dimmed on bright surfaces, and the hotspot limit.
vec3 addedLight(vec3 lin, vec3 light) {
    float dim = luma(exp(-3.5 * lin));
    vec3 added = max(vec3(0.0), dim * light) * lin;
    if (cb.flashLightTuning.z > 0.0) {
        float limit = 1.0 / cb.flashLightTuning.z;
        added *= limit / max(limit, luma(added));
    }
    return added;
}

// The beam shaft: dimmed inside the cone, soft-capped. EdgeFix 2 upsamples it depth-aware from its half-resolution buffer.
vec3 beamAt(vec2 p, float depth) {
    vec3 shaft = textureLod(sampler2D(t_volumetric, NVR_SAMPLER(pc.linearSampler)), p, 0.0).rgb;
    if (cb.flashLightComposite.y > 0.5) {
        vec2 bt = cb.flashLightComposite.zw;
        vec2 hh = p / bt - 0.5;
        vec2 base = floor(hh);
        vec2 f = hh - base;
        vec2 o4[4] = vec2[4](vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(0.0, 1.0), vec2(1.0, 1.0));
        vec3 sum = vec3(0.0);
        float wsum = 0.0;
        for (int j = 0; j < 4; j++) {
            vec2 o = o4[j];
            vec2 suv = (base + o + 0.5) * bt;
            float gap = abs(storedDepthAt(suv) * cb.cameraData.y - depth) / (1.0 + 0.01 * depth);
            float bw = mix(1.0 - f.x, f.x, o.x) * mix(1.0 - f.y, f.y, o.y);
            float w = bw / (1.0 + 100.0 * gap * gap);
            sum += w * textureLod(sampler2D(t_volumetric, NVR_SAMPLER(pc.linearSampler)), suv, 0.0).rgb;
            wsum += w;
        }
        shaft = sum / max(wsum, 1e-12);
    }
    shaft *= (1.0 - 0.85 * coneAt(surfaceWorld(p, depth)));
    return shaft / (1.0 + 2.5 * luma(shaft));
}

vec3 encodeOut(vec3 c) {
    return cb.flashLightComposite.x > 0.5 ? c : delinearize(c);
}

void main() {
    vec2 rt = cb.reciprocalResolution.xy;
    vec3 lin = linearSource(uv);
    vec3 light = lightAt(uv);
    float depthC = cb.cameraData.y * storedDepthAt(uv);	// game units, as readDepth gives them

    if (pc.mode == 1u) {
        float c = storedDepthAt(uv);
        float dL = storedDepthAt(uv + vec2(-rt.x, 0.0)), dR = storedDepthAt(uv + vec2(rt.x, 0.0));
        float dU = storedDepthAt(uv + vec2(0.0, -rt.y)), dD = storedDepthAt(uv + vec2(0.0, rt.y));
        float dUL = storedDepthAt(uv + vec2(-rt.x, -rt.y)), dUR = storedDepthAt(uv + vec2(rt.x, -rt.y));
        float dDL = storedDepthAt(uv + vec2(-rt.x, rt.y)), dDR = storedDepthAt(uv + vec2(rt.x, rt.y));
        float c0 = max(c, 1e-6);
        float b1 = abs(c0 / max(dL, 1e-4) + c0 / max(dR, 1e-4) - 2.0);
        float b2 = abs(c0 / max(dU, 1e-4) + c0 / max(dD, 1e-4) - 2.0);
        float b3 = abs(c0 / max(dUL, 1e-4) + c0 / max(dDR, 1e-4) - 2.0);
        float b4 = abs(c0 / max(dUR, 1e-4) + c0 / max(dDL, 1e-4) - 2.0);
        float edge = smoothstep(0.02, 0.05, max(max(b1, b2), max(b3, b4)));
        if (edge > 0.0) {
            vec2 farOff = vec2(-rt.x, 0.0);
            float farDepth = dL;
            if (dR > farDepth) { farDepth = dR; farOff = vec2(rt.x, 0.0); }
            if (dU > farDepth) { farDepth = dU; farOff = vec2(0.0, -rt.y); }
            if (dD > farDepth) { farDepth = dD; farOff = vec2(0.0, rt.y); }
            if (dUL > farDepth) { farDepth = dUL; farOff = vec2(-rt.x, -rt.y); }
            if (dUR > farDepth) { farDepth = dUR; farOff = vec2(rt.x, -rt.y); }
            if (dDL > farDepth) { farDepth = dDL; farOff = vec2(-rt.x, rt.y); }
            if (dDR > farDepth) { farDepth = dDR; farOff = vec2(rt.x, rt.y); }

            float mid = 0.5 * (c + farDepth);
            vec2 offs[4] = vec2[4](vec2(-rt.x, 0.0), vec2(rt.x, 0.0), vec2(0.0, -rt.y), vec2(0.0, rt.y));
            float dirDepth[4] = float[4](dL, dR, dU, dD);
            float bestDiff = -1.0;
            vec2 nearOff = vec2(0.0);
            for (int k = 0; k < 4; k++) {
                float diff = length(linearSource(uv + offs[k]) - lin);
                if (dirDepth[k] < mid && diff > bestDiff) { bestDiff = diff; nearOff = offs[k]; }
            }
            vec3 nearCol = linearSource(uv + nearOff);
            vec3 farCol = linearSource(uv + farOff);
            vec3 axis = nearCol - farCol;
            float contrast = length(axis);
            float weightOn = smoothstep(0.0025, 0.01, contrast);
            float t = clamp(dot(lin - farCol, axis) / max(dot(axis, axis), 1e-10), 0.0, 1.0);
            vec3 split = mix(lightAt(uv + farOff), lightAt(uv + nearOff), t);
            light = mix(light, split, edge * weightOn);
        }
    }
    vec3 added = addedLight(lin, light);
    if (cb.volumetricControl.x > 0.0) added += beamAt(uv, depthC) * cb.volumetricControl.x;
    o_color = vec4(encodeOut(lin + added), 1.0);
}
