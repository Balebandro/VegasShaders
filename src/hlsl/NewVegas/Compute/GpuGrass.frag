// GPU grass: the strands' pixels (effects/GpuGrass.cpp). Opaque, no alpha test (early depth test works: one shaded sample per pixel
// and strand, the cost the game's alpha-tested cards cannot avoid), the colour the vertex shader worked out. Colour writes RGB only,
// as the game's grass draws (its alpha stays as it was).
//
// Build: glslangValidator -V --target-env vulkan1.1 -o GpuGrass.spv GpuGrass.frag (tools/nvr-dxvk/compile-compute.sh)
#version 450

layout(location = 0) in vec4 i_color;
layout(location = 0) out vec4 o_color;

void main() {
    o_color = vec4(i_color.rgb, 1.0);
}
