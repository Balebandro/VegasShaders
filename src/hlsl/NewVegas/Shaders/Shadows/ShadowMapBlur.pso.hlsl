uniform float4 TESR_ShadowBlur : register(c0);  // .x reciprocal resolution of shadow atlas, .y whether last cascade was updated
uniform float4 BlurDirection  : register(c1);

uniform sampler2D SourceBuffer : register(s0) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };

struct VSOUT
{
	float4 f_vert_pos : POSITION;
	float2 f_uvcoord : TEXCOORD0;
};

float4 Blur3(float2 tex_uv, float2 upper_left, float2 lower_right) {
    float wts[1] = { 0.5f };
    float offs[1] = { 0.6f };
    
    float2 ofs = BlurDirection * offs[0] * TESR_ShadowBlur.x;

    float2 offset_uv = clamp(tex_uv.xy + ofs, upper_left, lower_right);
    float4 pix = tex2Dlod(SourceBuffer, float4(offset_uv, 0.0f, 0.0f)) * wts[0];
    
    offset_uv = clamp(tex_uv.xy - ofs, upper_left, lower_right);
    pix += tex2Dlod(SourceBuffer, float4(offset_uv, 0.0f, 0.0f)) * wts[0];

    return pix;
}

float4 Blur5(float2 tex_uv, float2 upper_left, float2 lower_right) {
    float wts[2] = { 0.2941176470f, 0.3529411764f };
    float offs[2] = { 0.0f, 1.3333333333f };
    
    float4 pix = tex2Dlod(SourceBuffer, float4(tex_uv.xy, 0.0f, 0.0f)) * wts[0];
    
    float2 ofs, offset_uv;
    for (int k = 1; k < 2; k++) {
        ofs = BlurDirection * offs[k] * TESR_ShadowBlur.x;
        
        offset_uv = clamp(tex_uv.xy + ofs, upper_left, lower_right);
        pix += tex2Dlod(SourceBuffer, float4(offset_uv, 0.0f, 0.0f)) * wts[k];
        
        offset_uv = clamp(tex_uv.xy - ofs, upper_left, lower_right);
        pix += tex2Dlod(SourceBuffer, float4(offset_uv, 0.0f, 0.0f)) * wts[k];
    }
    
    return pix;
}

float4 Blur9(float2 tex_uv, float2 upper_left, float2 lower_right) {
    float wts[3] = { 0.2270270270f, 0.3162162162f, 0.0702702703f };
    float offs[3] = { 0.0f, 1.3846153846f, 3.2307692308f };
    
    float4 pix = tex2Dlod(SourceBuffer, float4(tex_uv.xy, 0.0f, 0.0f)) * wts[0];
    
    float2 ofs, offset_uv;
    for (int k = 1; k < 3; k++) {
        ofs = BlurDirection * offs[k] * TESR_ShadowBlur.x;
        
        offset_uv = clamp(tex_uv.xy + ofs, upper_left, lower_right);
        pix += tex2Dlod(SourceBuffer, float4(offset_uv, 0.0f, 0.0f)) * wts[k];
        
        offset_uv = clamp(tex_uv.xy - ofs, upper_left, lower_right);
        pix += tex2Dlod(SourceBuffer, float4(offset_uv, 0.0f, 0.0f)) * wts[k];
    }
    
    return pix;
}

// Gaussian blur using linear texture sampling.
// Weights calculated from the Pascal triangle, and recalculated as below.
// Sample in between texels.
// weight_l(t1, t2) = weight_d(t1) + weight_d(t2)
// offset_l(t1, t2) = (offset_d(t1) * weight_d(t1) + offset_d(t2) * weight_d(t2)) / weight_l(t1, t2)
// https://www.rastergrid.com/blog/2010/09/efficient-gaussian-blur-with-linear-sampling/
float4 main(VSOUT pa) : COLOR0 {
	// Blur the different cascades non-uniformly. The closer the cascade, the bigger the blur.
    float2 tex_uv = pa.f_uvcoord.xy;
    
    [branch] if (tex_uv.x < 0.5 && tex_uv.y < 0.5)
        return Blur9(tex_uv, float2(0.0f, 0.0f), float2(0.5f, 0.5f)); // Closest cascade.
    else if (tex_uv.y < 0.5)
        return Blur5(tex_uv, float2(0.5f, 0.0f), float2(1.0f, 0.5f)); // Second cascade.
    else if (tex_uv.x < 0.5)
        return Blur5(tex_uv, float2(0.0f, 0.5f), float2(0.5f, 1.0f)); // Third cascade.
    else if (TESR_ShadowBlur.y)
        return Blur5(tex_uv, float2(0.5f, 0.5f), float2(1.0f, 1.0f)); // Furthest cascade.
    else
        return tex2Dlod(SourceBuffer, float4(tex_uv.xy, 0.0f, 0.0f)); // Furthest cascade was not updated, leave it be.

}
