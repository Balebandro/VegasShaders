// TAA's resolve (Effects/TAA.fx.hlsl ResolvePS) as a Vulkan pass (needs NVR DXVK): effects/TAA.cpp ExtDraw. TAA.glsl holds the rest.
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "TAA.glsl"

void main() {
    o_color = vec4(ResolveCore(uv).color, 1.0);
}
