// TAA's debug views (Effects/TAA.fx.hlsl DebugPS) as a Vulkan pass (needs NVR DXVK): effects/TAA.cpp ExtDraw. TAA.glsl holds the rest.
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "TAA.glsl"

void main() {
    TAAResult r = ResolveCore(uv);

    vec3 motion = vec3(clamp(abs(r.motionPixels) / 16.0, 0.0, 1.0), 0.0);
    vec3 error = clamp(abs(r.historyRaw - r.current) * 4.0, 0.0, 1.0);
    vec3 historyUse = vec3(clamp(r.weight + r.clipAmount * 8.0, 0.0, 1.0), r.weight, r.weight);
    vec3 weaponMask = mix(r.current * 0.35, vec3(1.0, 0.0, 1.0), r.isViewModel * 0.75);
    vec3 depth = vec3(clamp(log2(1.0 + r.depth) / log2(1.0 + farZ), 0.0, 1.0));

    vec3 view = r.color;
    view = (debugView > 0.5 && debugView < 1.5) ? motion : view;
    view = (debugView > 1.5 && debugView < 2.5) ? error : view;
    view = (debugView > 2.5 && debugView < 3.5) ? historyUse : view;
    view = (debugView > 3.5 && debugView < 4.5) ? weaponMask : view;
    view = (debugView > 4.5) ? depth : view;
    o_color = vec4(view, 1.0);
}
