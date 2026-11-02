
// water height for auto water

uniform float fNoiseScale : register(c3);
uniform float4 TESR_GameTime : register(c6);
uniform float4 TESR_WaveParams : register(c7);

uniform sampler2D NoiseMap : register(s0);
uniform sampler3D TESR_Water : register(s1) < string ResourceName = "Water\water_NRM.dds"; > = sampler_state { ADDRESSU = WRAP; ADDRESSV = WRAP; ADDRESSW = WRAP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; } ;

struct VtxAttrs {
    float2 f_texcoord_0 : TEXCOORD0;
};

struct VtxVaryings {
    float4 f_color_0 : COLOR0;
};

VtxVaryings main(VtxAttrs pa) {
    VtxVaryings pr;

    float frame_t = TESR_GameTime.z * TESR_WaveParams.z;
    float4 height_map = tex3D(TESR_Water, float3(pa.f_texcoord_0.xy, frac(frame_t)));

	pr.f_color_0 = height_map;
    return pr;
	
};
