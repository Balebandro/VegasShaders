// Bloom.
// https://www.iryoku.com/next-generation-post-processing-in-call-of-duty-advanced-warfare/
// https://learnopengl.com/Guest-Articles/2022/Phys.-Based-Bloom
// https://www.froyok.fr/blog/2021-12-ue4-custom-bloom/
//
// Designed to work without thresholding, for HDR rendering.
// Should minimize potential bloom issues of filtering artifacts and fireflies.
//
// UNOFFICIAL additions ([Shaders.Bloom.*], each off by default; all off: exactly as before):
// - Spread: each wider level counts (1 + Spread) times as much as the one finer than it (additive mode, PassBlending 0),
//   so the glow reaches further (> 0) or stays tighter (< 0). The last upsample divides by the sum of the weights.
// - GlowBoost: bright lights (neon, lamps, fire; the same test as the bounce light's NeonSpill: brighter than GlowLightLevel
//   times a white surface in the scene's own light) count up to 1 + GlowBoost times as much in the first downsample, so they
//   glow more than the rest of the image.
// - LinearLight: the chain blurs light as it is (linear), as a lens spreads it, instead of the display-encoded image; a
//   bright light on a dark background then keeps much more of its glow. The last upsample encodes the result again,
//   so the tonemapper, the dirt lens and the rain read it as before.
// - AntiFlicker: the first downsample weighs each of its five boxes by 1 / (1 + its brightness) (Karis), so a lone tiny
//   highlight that flickers in and out no longer makes the glow flicker with it.

float4 TESR_BloomResolution;
float4 TESR_BloomData; // .x filterRadius x axis, .y filterRadius y axis, .z blendingCoefficient, .w inverse of number of passes for upscale
float4 TESR_BloomGlow; // UNOFFICIAL .x: weight of each wider level (1: as before), .y: what the last additive upsample scales by (.w of TESR_BloomData with .x at 1), .z: GlowBoost (0: none), .w: how bright (linear) a light must be (Bloom.cpp: GlowLightLevel times the scene's own light)
float4 TESR_BloomGlowExtra; // UNOFFICIAL .x: AntiFlicker (1: on), .y: LinearLight (1: on)

sampler2D TESR_BloomBuffer : register(s0) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_BloomBuffer2 : register(s1) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_BloomBuffer4 : register(s2) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_BloomBuffer8 : register(s3) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_BloomBuffer16 : register(s4) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_BloomBuffer32 : register(s5) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_BloomBuffer64 : register(s6) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_BloomBuffer128 : register(s7) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_RenderedBuffer : register(s8) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };

struct VSOUT
{
	float4 vertPos : POSITION;
	float2 UVCoord : TEXCOORD0;
};

struct VSIN
{
	float4 vertPos : POSITION0;
	float2 UVCoord : TEXCOORD0;
};

VSOUT FrameVS(VSIN IN)
{
	VSOUT OUT = (VSOUT)0.0f;
	OUT.vertPos = IN.vertPos;
	OUT.UVCoord = IN.UVCoord;
	return OUT;
}

#include "Includes/Sampling.hlsl"
#include "Includes/RenderRect.hlsl"

// UNOFFICIAL 2026-10-27 RenderScale v2: in a rect frame each level holds the picture only in its top-left part (the rest is an
// older frame), so the taps stay inside it; native frames run the original code untouched.
static const bool BloomRectOn = TESR_RenderScale.x < 0.9999 || TESR_RenderScale.y < 0.9999;
float4 RectTap(uniform sampler2D buffer, float2 uv, float2 texel) { return tex2Dlod(buffer, float4(clampToRect(uv, texel), 0, 0)); }	// lod: read inside a branch (the levels have one mip)

float4 DownsampleBox13Rect(uniform sampler2D buffer, float2 uv, float2 texelSize, float2 sourceTexel) {
    const float x = texelSize.x;
    const float y = texelSize.y;
    float4 a = RectTap(buffer, float2(uv.x - 2 * x, uv.y + 2 * y), sourceTexel);
    float4 b = RectTap(buffer, float2(uv.x, uv.y + 2 * y), sourceTexel);
    float4 c = RectTap(buffer, float2(uv.x + 2 * x, uv.y + 2 * y), sourceTexel);
    float4 d = RectTap(buffer, float2(uv.x - 2 * x, uv.y), sourceTexel);
    float4 e = RectTap(buffer, float2(uv.x, uv.y), sourceTexel);
    float4 f = RectTap(buffer, float2(uv.x + 2 * x, uv.y), sourceTexel);
    float4 g = RectTap(buffer, float2(uv.x - 2 * x, uv.y - 2 * y), sourceTexel);
    float4 h = RectTap(buffer, float2(uv.x, uv.y - 2 * y), sourceTexel);
    float4 i = RectTap(buffer, float2(uv.x + 2 * x, uv.y - 2 * y), sourceTexel);
    float4 j = RectTap(buffer, float2(uv.x - x, uv.y + y), sourceTexel);
    float4 k = RectTap(buffer, float2(uv.x + x, uv.y + y), sourceTexel);
    float4 l = RectTap(buffer, float2(uv.x - x, uv.y - y), sourceTexel);
    float4 m = RectTap(buffer, float2(uv.x + x, uv.y - y), sourceTexel);
    float4 boxes[5];
    boxes[0] = (a + b + d + e) * 0.25;
    boxes[1] = (b + c + e + f) * 0.25;
    boxes[2] = (d + e + g + h) * 0.25;
    boxes[3] = (e + f + h + i) * 0.25;
    boxes[4] = (j + k + l + m) * 0.25;
    return (boxes[0] + boxes[1] + boxes[2] + boxes[3]) * 0.125 + boxes[4] * 0.5;
}

float4 UpsampleTent9Rect(uniform sampler2D buffer, float2 uv, float2 filterRadius, float2 sourceTexel) {
    float x = filterRadius.x;
    float y = filterRadius.y;
    float4 a = RectTap(buffer, float2(uv.x - x, uv.y + y), sourceTexel);
    float4 b = RectTap(buffer, float2(uv.x, uv.y + y), sourceTexel);
    float4 c = RectTap(buffer, float2(uv.x + x, uv.y + y), sourceTexel);
    float4 d = RectTap(buffer, float2(uv.x - x, uv.y), sourceTexel);
    float4 e = RectTap(buffer, float2(uv.x, uv.y), sourceTexel);
    float4 f = RectTap(buffer, float2(uv.x + x, uv.y), sourceTexel);
    float4 g = RectTap(buffer, float2(uv.x - x, uv.y - y), sourceTexel);
    float4 h = RectTap(buffer, float2(uv.x, uv.y - y), sourceTexel);
    float4 i = RectTap(buffer, float2(uv.x + x, uv.y - y), sourceTexel);
    return (e * 4.0 + (b + d + f + h) * 2.0 + (a + c + g + i)) * (1.0 / 16.0);
}

// the upsamples' blur: clamped in a rect frame (the smaller level's texel is twice this level's)
float4 UpsampleBlur(uniform sampler2D buffer, float2 uv, float2 filterRadius) {
    [branch] if (BloomRectOn) return UpsampleTent9Rect(buffer, uv, filterRadius, 2.0 * TESR_BloomResolution.zw);
    return UpsampleTent9(buffer, uv, filterRadius);
}

// the first downsample's reads of the image (twice level 0's size): a plain pass-through in native frames
float2 FirstTap(float2 uv) { return BloomRectOn ? clampToRect(uv, 0.5 * TESR_BloomResolution.zw) : uv; }

// Downsample with the 13 tap box.
float4 Downsample(VSOUT IN, uniform sampler2D buffer) : COLOR0 {
    float2 uv = IN.UVCoord;

    float2 texelSize = { TESR_BloomResolution.z, TESR_BloomResolution.w };

    [branch] if (BloomRectOn) return DownsampleBox13Rect(buffer, uv, texelSize, 0.5 * texelSize); // UNOFFICIAL RenderScale v2: the source level is twice the size

    float4 downsample = DownsampleBox13(buffer, uv, texelSize);

    return downsample;
}

// UNOFFICIAL: the part of a colour (linear) that counts as a light, 0 to 1: as the bounce light's SpillLight.
float GlowAmount(float3 c, float threshold)
{
    float peak = max(c.r, max(c.g, c.b));
    float saturation = 1.0f - min(c.r, min(c.g, c.b)) / max(peak, 1.0e-4f);
    float brightness = lerp(dot(c, float3(0.2126f, 0.7152f, 0.0722f)), peak, saturation);
    return saturate((brightness - threshold) / threshold);
}

// UNOFFICIAL: one read of the image, with GlowBoost and LinearLight applied. The display encoding is undone and redone with
// cheap fits (a cubic, and square roots) instead of pow: 13 reads a pixel made pow cost about 0.3 ms at 1440p, and the
// glow does not need the exact curve (the fits stay within about 1% up to 1, and are about 12% too bright at 3).
float3 GlowLinear(float3 c) { return c * (c * (c * 0.305306011f + 0.682171111f) + 0.012522878f); }
float3 GlowEncode(float3 c) {
    float3 s1 = sqrt(c), s2 = sqrt(s1), s3 = sqrt(s2);
    return max(0.662002687f * s1 + 0.684122060f * s2 - 0.323583601f * s3 - 0.0225411470f * c, 0.0f);
}
float4 GlowTap(float4 c, float threshold)
{
    float3 lin = GlowLinear(max(c.rgb, 0.0f));
    lin *= 1.0f + TESR_BloomGlow.z * GlowAmount(lin, threshold);
    return float4(TESR_BloomGlowExtra.y > 0.0f ? lin : GlowEncode(lin), c.a);
}

// UNOFFICIAL: the first downsample, from the image: the 13 reads of DownsampleBox13, then (only with GlowBoost, LinearLight
// or AntiFlicker on) each read changed and/or the boxes weighed, all after the reads (no texture reads in the branches).
float4 DownsampleFirst(VSOUT IN, uniform sampler2D buffer) : COLOR0 {
    const float2 uv = IN.UVCoord;
    const float x = TESR_BloomResolution.z;
    const float y = TESR_BloomResolution.w;

    float4 a = tex2D(buffer, FirstTap(float2(uv.x - 2 * x, uv.y + 2 * y)));
    float4 b = tex2D(buffer, FirstTap(float2(uv.x, uv.y + 2 * y)));
    float4 c = tex2D(buffer, FirstTap(float2(uv.x + 2 * x, uv.y + 2 * y)));

    float4 d = tex2D(buffer, FirstTap(float2(uv.x - 2 * x, uv.y)));
    float4 e = tex2D(buffer, FirstTap(float2(uv.x, uv.y)));
    float4 f = tex2D(buffer, FirstTap(float2(uv.x + 2 * x, uv.y)));

    float4 g = tex2D(buffer, FirstTap(float2(uv.x - 2 * x, uv.y - 2 * y)));
    float4 h = tex2D(buffer, FirstTap(float2(uv.x, uv.y - 2 * y)));
    float4 i = tex2D(buffer, FirstTap(float2(uv.x + 2 * x, uv.y - 2 * y)));

    float4 j = tex2D(buffer, FirstTap(float2(uv.x - x, uv.y + y)));
    float4 k = tex2D(buffer, FirstTap(float2(uv.x + x, uv.y + y)));
    float4 l = tex2D(buffer, FirstTap(float2(uv.x - x, uv.y - y)));
    float4 m = tex2D(buffer, FirstTap(float2(uv.x + x, uv.y - y)));

    [branch] if (TESR_BloomGlow.z > 0.0f || TESR_BloomGlowExtra.y > 0.0f) {
        float t = max(TESR_BloomGlow.w, 0.002f); // GlowBoost's threshold (a floor: a pitch-black cell)
        a = GlowTap(a, t); b = GlowTap(b, t); c = GlowTap(c, t); d = GlowTap(d, t); e = GlowTap(e, t); f = GlowTap(f, t); g = GlowTap(g, t);
        h = GlowTap(h, t); i = GlowTap(i, t); j = GlowTap(j, t); k = GlowTap(k, t); l = GlowTap(l, t); m = GlowTap(m, t);
    }

    float4 boxes[5];
    boxes[0] = (a + b + d + e) * 0.25;
    boxes[1] = (b + c + e + f) * 0.25;
    boxes[2] = (d + e + g + h) * 0.25;
    boxes[3] = (e + f + h + i) * 0.25;
    boxes[4] = (j + k + l + m) * 0.25;

    [branch] if (TESR_BloomGlowExtra.x > 0.0f) {
        // Karis average: each box by 1 / (1 + its brightness), then the weights normalised.
        float weights[5] = { 0.125f, 0.125f, 0.125f, 0.125f, 0.5f };
        float4 sum = 0.0f;
        float total = 0.0f;
        [unroll] for (int n = 0; n < 5; n++) {
            float w = weights[n] / (1.0f + max(dot(boxes[n].rgb, float3(0.2126f, 0.7152f, 0.0722f)), 0.0f));
            sum += boxes[n] * w;
            total += w;
        }
        return sum / total;
    }
    float2 weights = float2(0.125, 0.5);
    return boxes[0] * weights[0] + boxes[1] * weights[0] + boxes[2] * weights[0] + boxes[3] * weights[0] + boxes[4] * weights[1];
}

// Intermediary upsamples.
float4 Upsample(VSOUT IN, uniform sampler2D buffer, uniform sampler2D addBuffer) : COLOR0 {
    float2 uv = IN.UVCoord;
    
    const float2 filterRadius = { TESR_BloomData.x, TESR_BloomData.y };
    
    float4 upsample = UpsampleBlur(buffer, uv, filterRadius);
    float4 nextMip = tex2D(addBuffer, uv);
    
    [branch] if (TESR_BloomData.z > 0.0) {
        return float4(lerp(nextMip.rgb, upsample.rgb, TESR_BloomData.z), 1);
    } else {
        return float4(upsample.rgb * TESR_BloomGlow.x + nextMip.rgb, 1); // UNOFFICIAL Spread: .x = 1 (as before) unless set
    }
}

// Last upsample that normalizes the result if needed.
float4 UpsampleLast(VSOUT IN, uniform sampler2D buffer, uniform sampler2D addBuffer) : COLOR0 {
    float2 uv = IN.UVCoord;
    
    const float2 filterRadius = { TESR_BloomData.x, TESR_BloomData.y };
    
    float4 upsample = UpsampleBlur(buffer, uv, filterRadius);
    float4 nextMip = tex2D(addBuffer, uv);
    
    float3 result;
    [branch] if (TESR_BloomData.z > 0.0) {
        result = lerp(nextMip.rgb, upsample.rgb, TESR_BloomData.z);
    } else {
        result = (upsample.rgb * TESR_BloomGlow.x + nextMip.rgb) * TESR_BloomGlow.y; // UNOFFICIAL Spread (as before at .x = 1)
    }
    // UNOFFICIAL LinearLight: the chain held linear light; encoded again for its readers.
    [branch] if (TESR_BloomGlowExtra.y > 0.0f) result = delinearize(max(result, 0.0f));
    return float4(result, 1);
}

technique // 0
{
	pass
	{
		VertexShader = compile vs_3_0 FrameVS();
        PixelShader = compile ps_3_0 DownsampleFirst(TESR_RenderedBuffer); // output to BloomBuffer
    }	
}

technique // 1
{
	pass
	{
		VertexShader = compile vs_3_0 FrameVS();
        PixelShader = compile ps_3_0 Downsample(TESR_BloomBuffer); // output to BloomBuffer2
    }	
}

technique // 2
{
	pass
	{
		VertexShader = compile vs_3_0 FrameVS();
        PixelShader = compile ps_3_0 Downsample(TESR_BloomBuffer2); //output to BloomBuffer4
    }	
}

technique // 3
{
	pass
	{
		VertexShader = compile vs_3_0 FrameVS();
        PixelShader = compile ps_3_0 Downsample(TESR_BloomBuffer4); // output to BloomBuffer8
    }	
}

technique // 4
{
	pass
	{
		VertexShader = compile vs_3_0 FrameVS();
        PixelShader = compile ps_3_0 Downsample(TESR_BloomBuffer8); // output to BloomBuffer16
    }	
}

technique // 5
{
    pass {
        VertexShader = compile vs_3_0 FrameVS();
        PixelShader = compile ps_3_0 Downsample(TESR_BloomBuffer16); // output to BloomBuffer32
    }
}

technique // 6
{
    pass {
        VertexShader = compile vs_3_0 FrameVS();
        PixelShader = compile ps_3_0 Downsample(TESR_BloomBuffer32); // output to BloomBuffer64
    }
}

technique // 7
{
    pass {
        VertexShader = compile vs_3_0 FrameVS();
        PixelShader = compile ps_3_0 Downsample(TESR_BloomBuffer64); // output to BloomBuffer128
    }
}

technique // 8
{
    pass {
        VertexShader = compile vs_3_0 FrameVS();
        PixelShader = compile ps_3_0 Upsample(TESR_BloomBuffer128, TESR_BloomBuffer64); // output to BloomBuffer64
    }
}

technique // 9
{
    pass {
        VertexShader = compile vs_3_0 FrameVS();
        PixelShader = compile ps_3_0 Upsample(TESR_BloomBuffer64, TESR_BloomBuffer32); // output to BloomBuffer32
    }
}

technique // 10
{
    pass {
        VertexShader = compile vs_3_0 FrameVS();
        PixelShader = compile ps_3_0 Upsample(TESR_BloomBuffer32, TESR_BloomBuffer16); // output to BloomBuffer16
    }
}

technique // 11
{
    pass {
        VertexShader = compile vs_3_0 FrameVS();
        PixelShader = compile ps_3_0 Upsample(TESR_BloomBuffer16, TESR_BloomBuffer8); // output to BloomBuffer8
    }
}

technique // 12
{
	pass
	{
		VertexShader = compile vs_3_0 FrameVS();
        PixelShader = compile ps_3_0 Upsample(TESR_BloomBuffer8, TESR_BloomBuffer4); // output to BloomBuffer4
    }	
}

technique // 13
{
	pass
	{
		VertexShader = compile vs_3_0 FrameVS();
        PixelShader = compile ps_3_0 Upsample(TESR_BloomBuffer4, TESR_BloomBuffer2); // output to BloomBuffer2
    }	
}

technique // 14
{
	pass
	{
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 UpsampleLast(TESR_BloomBuffer2, TESR_BloomBuffer); // output to BloomBuffer
	}	
}
