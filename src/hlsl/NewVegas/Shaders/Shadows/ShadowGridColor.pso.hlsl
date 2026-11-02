// UNOFFICIAL 2026-10-14: the light grid's map of the world from the sun (AmbientLight GridBounce; ShadowManager draws it with this
// shader in every caster pass). Two targets: the plain depth (as the ortho map) and the surface's own colour from its diffuse
// texture (the bounce light takes it: red rock bounces red). Without a texture (TESR_ShadowData.w and .y 0) a mid grey.

float4 TESR_ShadowData : register(c0);	// y: 1 alpha test (texture bound), w: 1 texture bound (RenderPass::BindDiffuse)

sampler2D DiffuseMap : register(s0) = sampler_state { ADDRESSU = WRAP; ADDRESSV = WRAP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };

struct VS_OUTPUT {
	float4 texcoord_0 : TEXCOORD0;
	float4 texcoord_1 : TEXCOORD1;
};

struct PS_OUTPUT {
	float4 color_0 : COLOR0;
	float4 color_1 : COLOR1;
};

PS_OUTPUT main(VS_OUTPUT IN) {
	PS_OUTPUT OUT;
	// Read at the top level (derivatives): a map texel covers metres, so the texture's small mips give its average colour.
	float4 diffuse = tex2D(DiffuseMap, IN.texcoord_1.xy);
	bool textured = TESR_ShadowData.y == 1.0f || TESR_ShadowData.w == 1.0f;
	if (TESR_ShadowData.y == 1.0f && diffuse.a < 0.5f) discard;
	OUT.color_0 = float4(IN.texcoord_0.z / IN.texcoord_0.w, 0.0f, 0.0f, 1.0f);
	OUT.color_1 = float4(textured ? diffuse.rgb : float3(0.5f, 0.5f, 0.5f), 1.0f);
	return OUT;
}
