#version 450
// Mimics a DXVK-converted D3D9 pixel shader: separate images, sampler heap, one cube gradient marker per lamp in a branch.
layout(set = 0, binding = 0) uniform sampler s_samplers[64];
layout(set = 1, binding = 8) uniform textureCube t8;
layout(set = 1, binding = 11) uniform textureCube t11;
layout(set = 1, binding = 0) uniform texture2D t0;
layout(set = 2, binding = 0, std140) uniform C { vec4 c[16]; };
layout(location = 1) in vec4 v0;
layout(location = 0) out vec4 o0;
void main() {
  vec4 base = texture(sampler2D(t0, s_samplers[0]), v0.xy);
  vec4 lit = vec4(1.0);
  if (c[0].x > 0.0) {
    vec4 r = textureGrad(samplerCube(t8, s_samplers[8]), vec4(v0.xyz, 1.0).xyz, c[1].xyz, vec3(0.001, 0.99, 255.0));
    lit.x = r.x;
  }
  vec4 r2 = textureGrad(samplerCube(t11, s_samplers[11]), v0.zyx, c[2].xyz, c[3].xyz);
  vec4 normalCube = textureLod(samplerCube(t11, s_samplers[11]), v0.xyz, 0.0);
  o0 = base * lit.x * r2.y + normalCube * 0.0;
}
