float4 TESR_ReciprocalResolution;
float4 HazeLine; // projected muzzle and rear end (UV)
float4 HazeData; // heat, displacement in pixels, plume height and width in pixels
float4 HazeAnimation; // seconds, speed, debug mask, gun clip W
float4 HazeBlast; // muzzle blast: displacement in pixels now (fades after each shot), radius in pixels
sampler2D TESR_RenderedBuffer : register(s0) = sampler_state {
    ADDRESSU=CLAMP; ADDRESSV=CLAMP; MAGFILTER=LINEAR; MINFILTER=LINEAR; MIPFILTER=NONE;
};
sampler2D TESR_DepthBuffer : register(s1) = sampler_state {
    ADDRESSU=CLAMP; ADDRESSV=CLAMP; MAGFILTER=POINT; MINFILTER=POINT; MIPFILTER=NONE;
};
#include "Includes/Depth.hlsl"
struct VSIN { float4 position : POSITION0; float2 uv : TEXCOORD0; };
struct VSOUT { float4 position : POSITION0; float2 uv : TEXCOORD0; };
VSOUT FrameVS(VSIN input) { VSOUT o; o.position=input.position; o.uv=input.uv; return o; }
float4 HazePS(VSOUT input) : COLOR0 {
    float2 pixel = (input.uv-HazeLine.xy) / TESR_ReciprocalResolution.xy;
    float2 barrel = (HazeLine.zw-HazeLine.xy) / TESR_ReciprocalResolution.xy;
    float along = saturate(dot(pixel,barrel)/max(dot(barrel,barrel),1.0));
    float2 relative = pixel-barrel*along;
    float height=max(HazeData.z,1.0), width=max(HazeData.w,1.0);
    float rise=-relative.y/height;
    // A sleeve of shimmer all around the barrel (fading with distance from it, rounded at the ends), plus the plume
    // rising above it, since hot air goes up.
    float sleeve=exp(-dot(relative,relative)/(width*width));
    float plume=smoothstep(0.0,0.12,rise)*(1.0-smoothstep(0.65,1.0,rise))*exp(-relative.x*relative.x/(width*width));
    float mask=max(sleeve,plume)*HazeData.x;
    // Do not distort surfaces substantially closer than the drawn gun.
    // (skipped while the debug mask is shown, so the mask always shows the whole plume)
    if (HazeAnimation.z<0.5) mask *= step(HazeAnimation.w*0.8,readDepth(input.uv));
    float phase=HazeAnimation.x*HazeAnimation.y;
    float2 wave=float2(sin(relative.x/width*2.7+rise*14.0-phase*4.1),
                      sin(relative.x/width*4.3-rise*19.0+phase*2.9));
    float2 offset=wave*float2(1.0,0.35)*HazeData.y*mask*TESR_ReciprocalResolution.xy;
    // Muzzle blast: a ring of shimmer pushing outward from the muzzle, wider than the barrel haze, only for an instant
    // after each shot (HazeBlast.x fades to 0), whatever the barrel heat.
    float radius=max(HazeBlast.y,1.0);
    float dist=length(pixel);
    float blastMask=exp(-dist*dist/(radius*radius))*step(0.01,HazeBlast.x);
    if (HazeAnimation.z<0.5) blastMask *= step(HazeAnimation.w*0.8,readDepth(input.uv));
    float ripple=sin(dist/radius*9.0-phase*40.0)+0.5*sin(dist/radius*17.0+phase*23.0);
    offset+=(pixel/max(dist,1.0))*ripple*HazeBlast.x*blastMask*TESR_ReciprocalResolution.xy;
    float4 color=tex2D(TESR_RenderedBuffer,saturate(input.uv+offset));
    if (HazeAnimation.z>0.5) color.rgb=lerp(color.rgb,float3(1,0.2,0),saturate(mask)*0.65);
    if (HazeAnimation.z>0.5) color.rgb=lerp(color.rgb,float3(0,0.6,1),saturate(blastMask*HazeBlast.x/6.0)*0.6);
    return color;
}
technique HeatHaze {
    pass P0 {
        VertexShader=compile vs_3_0 FrameVS(); PixelShader=compile ps_3_0 HazePS();
        ZEnable=FALSE; ZWriteEnable=FALSE; AlphaBlendEnable=FALSE; AlphaTestEnable=FALSE;
        CullMode=NONE; ColorWriteEnable=15;
    }
}
