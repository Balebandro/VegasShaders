// Flashlight depth-aware blur (Vulkan, needs NVR DXVK): the D3D9 ShadowBlur of Effects/Flashlight.fx.hlsl, line for line. Binding 1 = the
// shadow mask (the chain's current image), binding 2 = depth. Run horizontal (offsetMask 1,0) then vertical (0,1). Beyond the reach: black.
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "Flashlight.glsl"





float maskAt(vec2 p) {
    return textureLod(sampler2D(t_rendered, NVR_SAMPLER(pc.linearSampler)), p, 0.0).r;
}

void main() {
    float depth0 = readDepth(uv);
    float radius = max(cb.spotLightPosition.w, 1e-4);
    if (length(cb.spotLightPosition.xyz - surfaceWorld(uv, depth0)) / radius >= 1.0) {
        o_color = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }
    float storedC = textureLod(sampler2D(t_depth, NVR_SAMPLER(pc.linearSampler)), uv, 0.0).r;
    float threshold = 350.0 * storedC;
    float sum = maskAt(uv);
    float wsum = 1.0;
    for (int i = 1; i <= 6; i++) {
        float w = exp(-float(i * i) / 18.0);
        for (int s = -1; s <= 1; s += 2) {
            vec2 tuv = uv + pc.offsetMask * (float(s * i)) * cb.reciprocalResolution.xy;
            if (abs(readDepth(tuv) - depth0) < threshold) {
                sum += w * maskAt(tuv);
                wsum += w;
            }
        }
    }
    float v = sum / wsum;
    o_color = vec4(v, v, v, 1.0);
}
