// UNOFFICIAL 2026-10-29 RT hit textures ([Main.Main.RayTracing] HitTextures; core\RtCore.cpp TextureSlotOf): a diffuse texture at
// 64 x 64 into two targets at once: 0 = its alpha (R16F, the alpha atlas: alpha-tested casters), 1 = its colour (RGBA8, the albedo
// atlas: the colour at a ray's hit, RtHit.glsl). The texture's own mips downsample. Any pool and format.
// Compile: glslangValidator -V --target-env vulkan1.1 -o RtTextureToAtlas.spv RtTextureToAtlas.frag
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "Common.glsl"

layout(set = 1, binding = 0) uniform texture2D t_diffuse;

layout(push_constant) uniform Push {
    uint samplerIndex;
    uint pad[3];
} pc;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o_alpha;
layout(location = 1) out vec4 o_albedo;

void main() {
    vec4 c = texture(sampler2D(t_diffuse, NVR_SAMPLER(pc.samplerIndex)), uv);
    o_alpha = vec4(c.a);
    o_albedo = vec4(c.rgb, 1.0);   // as stored (sRGB-encoded diffuse); RtHit.glsl linearizes
}
