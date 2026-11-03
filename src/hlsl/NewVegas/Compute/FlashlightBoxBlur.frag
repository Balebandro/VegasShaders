// Flashlight's two box blur passes (BoxBlurMin and BoxBlurAvg(TESR_RenderedBuffer, 1.0)) as a Vulkan pass (needs NVR DXVK):
// Effects/Flashlight.fx.hlsl line for line; pc.mode 0 is BoxBlurMin, 1 BoxBlurAvg. effects/Flashlight.cpp ExtRender.
// The clip() keeps the cleared target's (0, 0, 0, 1) (ClearRenderTarget); at scaleFactor 1 it never kills a pixel on screen.
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "Flashlight.glsl"

void main() {
    const float scaleFactor = 1.0;
    vec2 io = vec2(-1.0, 1.0) * 0.5;
    if (any(greaterThan(uv, vec2(scaleFactor)))) {   // clip((IN.UVCoord <= scaleFactor) - 1)
        o_color = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    vec2 maxuv = scaleFactor - 1.5 * cb.reciprocalResolution.xy;
    vec4 color = LINEAR_LOD(t_rendered, uv);
    if (pc.mode == 0u) {
        color = min(color, LINEAR_LOD(t_rendered, min(uv + io.xx * cb.reciprocalResolution.xy, maxuv)));
        color = min(color, LINEAR_LOD(t_rendered, min(uv + io.xy * cb.reciprocalResolution.xy, maxuv)));
        color = min(color, LINEAR_LOD(t_rendered, min(uv + io.yx * cb.reciprocalResolution.xy, maxuv)));
        color = min(color, LINEAR_LOD(t_rendered, min(uv + io.yy * cb.reciprocalResolution.xy, maxuv)));
        o_color = color;
        return;
    }
    color += LINEAR_LOD(t_rendered, min(uv + io.xx * cb.reciprocalResolution.xy, maxuv));
    color += LINEAR_LOD(t_rendered, min(uv + io.xy * cb.reciprocalResolution.xy, maxuv));
    color += LINEAR_LOD(t_rendered, min(uv + io.yx * cb.reciprocalResolution.xy, maxuv));
    color += LINEAR_LOD(t_rendered, min(uv + io.yy * cb.reciprocalResolution.xy, maxuv));
    o_color = vec4(color.rgb / 5.0, 1.0);
}
