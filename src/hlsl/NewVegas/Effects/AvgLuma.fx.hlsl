// AvgLuma: the picture's brightness and the depth-of-field focus, measured and smoothed over time. One 1x1 texel:
//   R = newLuma: this frame's average linear luma, smoothed (newLuma = (average + 7 x previous R) / 8)
//   G = animatedLuma: newLuma after the adaptation speed limit (Exposure reads this)
//   B = the focal distance as a fraction of the hyperfocal distance (0 at the camera, 1 at or beyond it)
//   A = 1
// The texel is read back as the previous value (R, G, B), so the pass reads and writes TESR_AvgLumaBuffer.
// The compute versions (Compute/AvgLuma.comp, AvgLumaHistogram.comp) keep the same state and the same four values.

#include "includes/Helpers.hlsl"
#include "includes/RenderRect.hlsl"

uniform float4 TESR_MotionBlurData;		// xy: camera rotation speed (the focus search spread)
uniform float4 TESR_DepthOfFieldData;	// x: hyperfocal distance in thousands of game units
uniform float4 TESR_GameTime;			// w: frame time in seconds
uniform float4 TESR_ExposureData;		// z: dark adaptation speed, w: light adaptation speed (brightness units per second)
uniform float4 TESR_CameraData;			// y: far plane
uniform float4 TESR_ReciprocalResolution;

uniform sampler2D TESR_RenderedBuffer : register(s0) = sampler_state {
    ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
uniform sampler2D TESR_AvgLumaBuffer : register(s1) = sampler_state {
    ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
uniform sampler2D TESR_DepthBuffer : register(s2) = sampler_state {
    ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };

struct VSOUT { float4 vertPos : POSITION; float2 UVCoord : TEXCOORD0; };

struct VSIN { float4 vertPos : POSITION0; float2 UVCoord : TEXCOORD0; };

VSOUT FrameVS(VSIN px)
{
    VSOUT res = (VSOUT) 0.0f;
    res.vertPos = px.vertPos;
    res.UVCoord = px.UVCoord;
    return (res);
}

// The twelve fixed taps: a golden-angle spiral out to the frame's edge, as offsets from the centre (screen units).
float2 TapOffset(int i)
{
    float angle_v = (float)i * 2.39996323f;
    float radius_v = 0.5f * sqrt(((float)i + 0.5f) / 12.0f);
    return float2(cos(angle_v), sin(angle_v)) * radius_v;
}

// Linear depth (game units) at a screen position (0..1 of the whole frame), kept in the drawn part.
float LinearDepthAt(float2 screen_uv)
{
    float2 uv = clampToRectIfOn(screen_uv * TESR_RenderScale.xy, TESR_ReciprocalResolution.xy);
    return tex2Dlod(TESR_DepthBuffer, float4(uv, 0.0f, 0.0f)).x * TESR_CameraData.y;
}

float4 AvgLumaPS(VSOUT px) : COLOR
{
    // Average luma: 100 samples on a 10 x 10 grid plus the 12 taps, each linearised before the mean. Read in the drawn part.
    float3 colour_sum = 0.0f;
    [loop]
    for (int gy_v = 0; gy_v < 10; gy_v++)
    {
        [loop]
        for (int gx_v = 0; gx_v < 10; gx_v++)
        {
            float2 screen_uv = float2(0.05f + 0.1f * gx_v, 0.05f + 0.1f * gy_v);
            float2 uv = screen_uv * TESR_RenderScale.xy;
            colour_sum += linearize(tex2Dlod(TESR_RenderedBuffer, float4(uv, 0.0f, 0.0f)).rgb);
        }
    }
    [loop]
    for (int t = 0; t < 12; t++)
    {
        float2 screen_uv = float2(0.5f, 0.5f) + TapOffset(t);
        float2 uv = clampToRectIfOn(screen_uv * TESR_RenderScale.xy, TESR_ReciprocalResolution.xy);
        colour_sum += linearize(tex2Dlod(TESR_RenderedBuffer, float4(uv, 0.0f, 0.0f)).rgb);
    }
    float average_v = dot(colour_sum / 112.0f, float3(0.2126f, 0.7152f, 0.0722f));	// BT.709 luminance of the mean linear colour

    // The previous texel (the 1x1 target: any uv).
    float4 previous_v = tex2Dlod(TESR_AvgLumaBuffer, float4(0.5f, 0.5f, 0.0f, 0.0f));
    float new_luma = (average_v + 7.0f * previous_v.r) / 8.0f;

    // Adaptation: at most dt x DarkAdapt down and dt x LightAdapt up per frame.
    float dt_v = TESR_GameTime.w;
    float delta_v = new_luma - previous_v.g;
    float animated_luma = delta_v < 0.0f ?
        previous_v.g + max(delta_v, -dt_v * TESR_ExposureData.z) :
        previous_v.g + min(delta_v, dt_v * TESR_ExposureData.w);

    // Focus: the centre and twelve taps spread by the camera's rotation speed. The mean (centre counted once more, over 12)
    // is used, except that a centre farther than that mean wins (iron sights).
    float2 speed_v = saturate(abs(TESR_MotionBlurData.xy)) + 0.1f;
    float centre_depth = LinearDepthAt(float2(0.5f, 0.5f));
    float depth_sum = centre_depth;
    [loop]
    for (int d = 0; d < 12; d++)
    {
        depth_sum += LinearDepthAt(float2(0.5f, 0.5f) + 0.3f * TapOffset(d) * speed_v);
    }
    float mean_depth = depth_sum / 12.0f;
    float eye_range = centre_depth > mean_depth ? centre_depth : mean_depth;
    float hyper_focal = max(1.0e-7f, TESR_DepthOfFieldData.x * 1000.0f);
    eye_range = saturate(eye_range / hyper_focal);

    // Move toward it: at most 3 x step x dt nearer, 1 x step x dt farther (focus pulls in fast and relaxes slowly).
    float step_v = pow(max(previous_v.b, eye_range), 0.1f);
    float focus_delta = eye_range - previous_v.b;
    float focus_v = focus_delta < 0.0f ?
        previous_v.b + max(focus_delta, -3.0f * step_v * dt_v) :
        previous_v.b + min(focus_delta, step_v * dt_v);

    return float4(new_luma, animated_luma, focus_v, 1.0f);
}

technique
{
    pass
    {
        VertexShader = compile vs_3_0 FrameVS( );
        PixelShader = compile ps_3_0 AvgLumaPS( );
    }
}
