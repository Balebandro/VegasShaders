// Sampling functions.

#if defined(__INTELLISENSE__)
    #include "Helpers.hlsl"
#else
    #include "includes/Helpers.hlsl"
#endif

// Downsample with blur by averaging 13 samples around the texel by weighting their values.
//
// Take 13 samples around current texel (basically forming 5 separate boxes):
// a - b - c
// - j - k -
// d - e - f
// - l - m -
// g - h - i
// === ('e' is the current texel) ===
//
// Apply weighted distribution:
// 0.5 + 0.125 + 0.125 + 0.125 + 0.125 = 1
// a,b,d,e * 0.125
// b,c,e,f * 0.125
// d,e,g,h * 0.125
// e,f,h,i * 0.125
// j,k,l,m * 0.5
float4 DownsampleBox13(uniform sampler2D buf, float2 uv, float2 texel_size) {
    const float x = texel_size.x;
    const float y = texel_size.y;

    float4 a = tex2D(buf, float2(uv.x - 2 * x, uv.y + 2 * y));
    float4 b = tex2D(buf, float2(uv.x, uv.y + 2 * y));
    float4 c = tex2D(buf, float2(uv.x + 2 * x, uv.y + 2 * y));

    float4 d = tex2D(buf, float2(uv.x - 2 * x, uv.y));
    float4 e = tex2D(buf, float2(uv.x, uv.y));
    float4 f = tex2D(buf, float2(uv.x + 2 * x, uv.y));

    float4 g_v = tex2D(buf, float2(uv.x - 2 * x, uv.y - 2 * y));
    float4 h_v = tex2D(buf, float2(uv.x, uv.y - 2 * y));
    float4 i = tex2D(buf, float2(uv.x + 2 * x, uv.y - 2 * y));

    float4 j = tex2D(buf, float2(uv.x - x, uv.y + y));
    float4 k = tex2D(buf, float2(uv.x + x, uv.y + y));
    float4 l = tex2D(buf, float2(uv.x - x, uv.y - y));
    float4 m = tex2D(buf, float2(uv.x + x, uv.y - y));
    
    float2 wts = float2(0.125, 0.5);
    
    float4 boxes_v[5];
    
    boxes_v[0] = (a + b + d + e) * 0.25;
    boxes_v[1] = (b + c + e + f) * 0.25;
    boxes_v[2] = (d + e + g_v + h_v) * 0.25;
    boxes_v[3] = (e + f + h_v + i) * 0.25;
    boxes_v[4] = (j + k + l + m) * 0.25;
	
    return boxes_v[0] * wts[0] + boxes_v[1] * wts[0] + boxes_v[2] * wts[0] + boxes_v[3] * wts[0] + boxes_v[4] * wts[1];
}

// Upsample with tent filter.
//
// Take 9 samples around current texel:
// a - b - c
// d - e - f
// g - h - i
// === ('e' is the current texel) ===
//
// Apply weighted distribution, by using a 3x3 tent filter:
//  1   | 1 2 1 |
// -- * | 2 4 2 |
// 16   | 1 2 1 |
float4 UpsampleTent9(uniform sampler2D buf, float2 uv, float2 filter_radius) {
	// The filter kernel is applied with a radius, specified in texture
    // coordinates, so that the radius will vary across mip resolutions.
    float x = filter_radius.x;
    float y = filter_radius.y;

    float4 a = tex2D(buf, float2(uv.x - x, uv.y + y));
    float4 b = tex2D(buf, float2(uv.x, uv.y + y));
    float4 c = tex2D(buf, float2(uv.x + x, uv.y + y));

    float4 d = tex2D(buf, float2(uv.x - x, uv.y));
    float4 e = tex2D(buf, float2(uv.x, uv.y));
    float4 f = tex2D(buf, float2(uv.x + x, uv.y));

    float4 g_v = tex2D(buf, float2(uv.x - x, uv.y - y));
    float4 h_v = tex2D(buf, float2(uv.x, uv.y - y));
    float4 i = tex2D(buf, float2(uv.x + x, uv.y - y));

    float4 upsample_v = e * 4.0;
    upsample_v += (b + d + f + h_v) * 2.0;
    upsample_v += (a + c + g_v + i);
    upsample_v *= 1.0 / 16.0;
    
    return upsample_v;
}
