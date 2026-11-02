
uniform float4 TESR_ShadowData : register(c0);
uniform sampler2D DiffuseMap : register(s0);

struct VtxVaryings {
    float4 f_texcoord_0 : TEXCOORD0;
	float4 f_texcoord_1 : TEXCOORD1;
};

struct PixResult {
    float4 f_color_0 : COLOR0;
};

PixResult main(VtxVaryings pa) {
    PixResult pr;
	
	float4 tmp_r0;
	float tmp_r1;
	
	if (TESR_ShadowData.y == 1.0f) { // Alpha is required
		tmp_r0.rgba = tex2D(DiffuseMap, pa.f_texcoord_1.xy);
		if (tmp_r0.a < 0.2f) discard;
	}
	
	pr.f_color_0 = length(pa.f_texcoord_0) / TESR_ShadowData.z;
    return pr;
	
};