#version 450
// Replacement for the test's hand-assembled ps_3_0 "oC0 = tex2D(s0, v0) * c5", following the contract in
// d3d9_nvr_ext_api.h (NVR_EXT_SHADER_REPLACEMENT_DESC): the game's texture 0 (set 1 binding 0), its float
// constants (set 2 binding 6, compacted: FloatRegisterMask selects c5 as c[0]), the D3D9 sampler heap indices
// at push offset 32, NVR's own resources in set 4 with their sampler indices at push offset 64.
#extension GL_EXT_nonuniform_qualifier : enable

layout(set = 0, binding = 0) uniform sampler s_samplers[];
layout(set = 1, binding = 0) uniform texture2D t0;
layout(set = 2, binding = 6, std140) uniform Consts { vec4 c[1]; };
layout(set = 4, binding = 0, std140) uniform Extra { vec4 scale; };
layout(set = 4, binding = 1) uniform texture2D extraTex;

layout(push_constant) uniform Push {
  layout(offset = 32) uint d3d9Samplers[8];   // 16 x uint16: sampler heap index of D3D9 sampler i at bits (i & 1) * 16
  layout(offset = 64) uint extraSamplers[4];  // 8 x uint16: the extra sampler bindings, in declaration order
};

layout(location = 1) in vec4 texcoord0;       // TEXCOORD0
layout(location = 0) out vec4 color;

void main() {
  uint s0 = d3d9Samplers[0] & 0xFFFFu;
  uint e0 = extraSamplers[0] & 0xFFFFu;
  vec4 tex = texture(sampler2D(t0, s_samplers[nonuniformEXT(s0)]), texcoord0.xy);
  vec4 ex  = texture(sampler2D(extraTex, s_samplers[nonuniformEXT(e0)]), texcoord0.xy);
  color = tex * c[0] * scale + ex;
}
