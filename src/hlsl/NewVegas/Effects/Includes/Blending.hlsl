float4 alphaBlend(float4 base_col, float4 blend_v)
{
	return base_col*base_col.a +(1-base_col.a)*blend_v;
}

float4 Desaturate(float4 input_v)
{
	float greyscale_v = luma(input_v);
	return float4(greyscale_v, greyscale_v, greyscale_v, input_v.a);
}

// photoshop overlay blend mode code from https://www.ryanjuckett.com/photoshop-blend-modes-in-hlsl/
float BlendMode_Overlay(float base_col, float blend_v)
{
	return (base_col <= 0.5f) ? 2.0f*base_col*blend_v : 1.0f - 2.0f*(1.0f-base_col)*(1.0f-blend_v);
}

float4 BlendMode_Overlay(float4 base_col, float4 blend_v)
{
	float4 result_c = float4(0,0,0,0);
	result_c.r = BlendMode_Overlay(base_col.r, blend_v.r);
	result_c.g = BlendMode_Overlay(base_col.g, blend_v.g);
	result_c.b = BlendMode_Overlay(base_col.b, blend_v.b);
	result_c.a = blend_v.a;

	return alphaBlend(result_c, base_col);
}

float4 BlendMode_SoftLight(float4 base_col, float4 blend_v)
{
	float4 low_values = 2.0f * base_col * blend_v * (1.0f + base_col * (1.0f - blend_v));
	float4 a_sqrt_v = sqrt(base_col);
	float4 high_values = (base_col + blend_v * (a_sqrt_v - base_col)) * 2.0f - a_sqrt_v;
	float4 result_c = (blend_v < 0.5f) ? low_values : high_values;
	result_c.a = blend_v.a;

	return alphaBlend(result_c, base_col);
}