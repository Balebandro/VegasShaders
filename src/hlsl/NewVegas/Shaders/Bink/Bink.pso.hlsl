
#define TEX2D(value) tex2D(tex##value, in_s.f_t0)
#define EXTRA_TEXCOORD(idx)
#define CONSTANT_SCALE(x) x
#define RESCALE(x)

uniform float4 consta : register(c0);

uniform sampler tex0 : register(s0);
uniform sampler tex1 : register(s1);
uniform sampler tex2 : register(s2);

struct BinkVaryings
{
  float2 f_t0: TEXCOORD0; EXTRA_TEXCOORD(1) EXTRA_TEXCOORD(2)
};

float4 main( BinkVaryings in_s ) : COLOR
{
  const float4 crc_c = { CONSTANT_SCALE(1.595794678f), -CONSTANT_SCALE(0.813476563f), 0, 0.0 };
  const float4 crb_c = { 0, -CONSTANT_SCALE(0.391448975f), CONSTANT_SCALE(2.017822266f), 0.0 };
  const float4 adj_v = { -CONSTANT_SCALE(0.87065506f), CONSTANT_SCALE(0.529705048f), -CONSTANT_SCALE(1.081668854f), 0 };
  float4 pt;

  float yy = TEX2D(0).a;
  float cr_c = TEX2D(1).a;
  float cb_c = TEX2D(2).a;

  pt = yy * CONSTANT_SCALE(1.164123535f);

  pt += crc_c * cr_c;
  pt += crb_c * cb_c;
  pt += adj_v;

  RESCALE(pt);

  pt.w = 1.0;
  pt *= consta;
  return pt;
}