// AmbientLight technique AmbientLightGrid pass GridInject as a Vulkan pass (needs NVR DXVK): Effects/AmbientLight.fx.hlsl GridInject, line
// for line (AmbientLight.glsl). Into the grid atlas (2 or 4 targets: the colours only when the grid carries them). All POINT.
// effects/AmbientLight.cpp ExtDraw.
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "AmbientLight.glsl"

layout(set = 1, binding = 0) uniform texture2D t_sunDepth;        // NVR_GridSunDepth
layout(set = 1, binding = 1) uniform texture2D t_sunColor;        // NVR_GridSunColor
layout(set = 1, binding = 2) uniform texture2D t_screenOld;       // NVR_GridScreenOld (the screen memory)
layout(set = 1, binding = 3) uniform texture2D t_screenColorOld;  // NVR_GridScreenColorOld
layout(set = 1, binding = 4) uniform texture2D t_wallMemory;      // NVR_GridWallMemory (NVR_GridWaveColor's stage)

layout(location = 0) out vec4 o_wave;
layout(location = 1) out vec4 o_accum;
layout(location = 2) out vec4 o_waveColor;
layout(location = 3) out vec4 o_accumColor;

vec3 SunMapPosition(vec2 uv, float depth) {
    vec4 p = cb.gridSunToGrid * vec4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, depth, 1.0);   // mul(v, NVR_GridSunToGrid)
    return p.xyz / p.w;
}

void main() {
    vec2 uvIn = frameUV();
    vec3 cell = AtlasCell(uvIn);
    bool fine = uvIn.x * AtlasDims.x >= AtlasSize;
    float size = fine ? cb.gridOrigin1.w : cb.gridOrigin.w;
    vec3 low = (fine ? cb.gridOrigin1.xyz - cb.gridOrigin.xyz : vec3(0.0)) + cell * size;
    vec4 sh = vec4(0.0);
    vec4 colour = vec4(0.0);
    vec3 normals = vec3(0.0);
    float count = 0.0;
    float faceShare = 0.0;
    if (cb.gridStep.z > 0.5) {
        vec4 centre = cb.gridToSun * vec4(low + 0.5 * size, 1.0);   // mul(v, NVR_GridToSun)
        vec2 centreUV = vec2(centre.x * 0.5 + 0.5, 0.5 - centre.y * 0.5);
        float span = 1.75 * size / cb.gridData.z;
        float stepTexels = span / 6.0;
        for (int j = 0; j < 6; j++) {
            for (int i = 0; i < 6; i++) {
                vec2 uv = centreUV + (vec2(float(i), float(j)) - 2.5) * stepTexels / SunMapSize;
                float depth = pointLod(t_sunDepth, uv).r;
                if (depth >= 0.9999) continue;
                vec3 p = SunMapPosition(uv, depth);
                vec3 inCell = p - low;
                if (any(lessThan(inCell, vec3(0.0))) || any(greaterThanEqual(inCell, vec3(size)))) continue;
                vec2 px = vec2(1.0 / SunMapSize);
                vec3 px1 = SunMapPosition(uv + vec2(px.x, 0.0), pointLod(t_sunDepth, uv + vec2(px.x, 0.0)).r);
                vec3 py1 = SunMapPosition(uv + vec2(0.0, px.y), pointLod(t_sunDepth, uv + vec2(0.0, px.y)).r);
                vec3 n = cross(px1 - p, py1 - p);
                float len = length(n);
                n = len > 1e-4 ? n / len : vec3(0.0, 0.0, 1.0);
                n *= dot(n, cb.sunDirection.xyz) < 0.0 ? -1.0 : 1.0;
                vec3 albedo = cb.gridData.w > 0.5 ? pow(pointLod(t_sunColor, uv).rgb, vec3(2.2)) : GridGroundColor;
                vec3 light = albedo * cb.gridLight.rgb;
                float l = luma(light);
                sh += CosineLobe(n) * l;
                colour += vec4(light, l);
                normals += n;
                count += 1.0;
            }
        }
        faceShare = (stepTexels * cb.gridData.z) * (stepTexels * cb.gridData.z) / (size * size);
        sh *= cb.gridLight.w * faceShare;
        colour *= faceShare;
    }
    if (cb.gridStep.w > 0.5) {
        sh += pointLod(t_screenOld, uvIn);
        colour += pointLod(t_screenColorOld, uvIn);
    }
    o_wave = sh;
    float normalLength = length(normals);
    float cover = count * faceShare;
    o_accum = vec4(cover * abs(normalLength > 1e-4 ? normals / normalLength : vec3(0.0, 0.0, 1.0)), cover);
    if (cb.gridWalls.y > 0.5) o_accum = max(o_accum, pointLod(t_wallMemory, uvIn));
    if (cb.gridLampInfo.x > 0.5 && any(greaterThan(o_accum.xyz, vec3(0.0)))) {
        vec3 centre = low + 0.5 * size;
        for (int k = 0; k < GridLampsMax; k++) {
            if (k >= int(cb.gridLampInfo.x)) break;
            vec4 lamp = cb.gridLamps[k];
            vec3 v = (lamp.xyz - cb.gridOrigin.xyz) - centre;
            float d = length(v);
            if (d >= lamp.w + 0.5 * size) continue;
            vec3 dir = d > 1e-3 ? v / d : vec3(0.0, 0.0, 1.0);
            float fall = saturate(1.0 - max(d - 0.5 * size, 0.0) / lamp.w);
            fall *= fall;
            float through = 1.0;
            if (cb.gridWalls.y > 0.5 && cb.gridLampInfo.z > 0.0 && d > size) {
                for (int s = 1; s <= 3; s++) {
                    vec3 c = floor((centre + v * (float(s) / 4.0)) / cb.gridOrigin.w);
                    if (any(lessThan(c, vec3(0.0))) || c.x >= GridX || c.y >= GridY || c.z >= GridZ) continue;
                    vec3 w = pointLod(t_wallMemory, CellUV(c)).xyz;
                    through *= 1.0 - saturate(dot(w, abs(dir)) * cb.gridLampInfo.z);
                }
            }
            float facing = min(dot(o_accum.xyz, abs(dir)), 1.0);
            vec3 light = cb.gridLampColors[k].rgb * (fall * through * facing * GridLampReflectance * cb.gridLampInfo.y);
            float l = luma(light);
            sh += CosineLobe(dir) * l;
            colour += vec4(light, l);
        }
        o_wave = sh;
    }
    o_waveColor = colour;
    o_accumColor = vec4(0.0);
}
