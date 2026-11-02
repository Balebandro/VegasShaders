// Normals: a view-space surface normal per pixel, built from CombineDepth's linear depth, stored in TESR_NormalsBuffer.
// Encoding (read by most NVR effects): RGB = the unit normal n as n * 0.5 + 0.5 (decode: stored * 2 - 1), view space
// +x right, +y up, +z away from the camera, so a surface facing the camera has n.z < 0. The normal is not flipped toward
// the camera. Alpha = 1.
//
// Technique 0 "Normals": the normals pass. Technique "NormalsSmooth" (two passes, optional, SmoothNormals > 0): pass 0
// smooths TESR_NormalsBuffer along x into the spare target, pass 1 smooths the spare along y back into the normals buffer.

#include "Includes/RenderRect.hlsl"

float4x4 TESR_ProjectionTransform;
float4x4 TESR_InvProjectionTransform;
float4 TESR_CameraData;			// x: near plane, y: far plane
float4 TESR_ReciprocalResolution;	// xy: 1 / texture size
float4 TESR_NormalsSmoothData;	// x: smoothing reach in texels (0 = off)

sampler2D TESR_DepthBuffer : register(s0) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
sampler2D TESR_NormalsBuffer : register(s1) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
sampler2D TESR_NormalsSmoothTemp : register(s2) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };

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
    VSOUT OUT = (VSOUT) 0.0f;
    OUT.vertPos = IN.vertPos;
    OUT.UVCoord = IN.UVCoord;
    return (OUT);
}

// The post-projection (reversed-Z) depth of a linear depth, as CombineDepth stores it in G.
float ProjectedFromViewZ(float viewZ)
{
    float nearZ = TESR_CameraData.x;
    float farZ = TESR_CameraData.y;
    return nearZ * (farZ - viewZ) / (viewZ * (farZ - nearZ));
}

// The linear depth (viewZ, game units) at a rect uv.
float ViewZAt(float2 uv)
{
    return max(tex2D(TESR_DepthBuffer, uv).x * TESR_CameraData.y, 1.0e-6);
}

// The view-space position of a rect uv at a linear depth, through the inverse projection.
float3 ViewPositionAt(float2 uv, float viewZ)
{
    float2 s01 = rectToScreen(uv);
    float4 clip = float4(s01.x * 2.0f - 1.0f, (1.0f - s01.y) * 2.0f - 1.0f, ProjectedFromViewZ(viewZ), 1.0f);
    float4 view = mul(clip, TESR_InvProjectionTransform);
    return view.xyz / view.w;
}

// The view-space position of a rect uv at a linear depth, from the projection's x and y terms only (the smoothing
// pass: the projection is what is available in both the D3D9 and the Vulkan passes).
float3 ViewPositionFromProjection(float2 uv, float viewZ)
{
    float2 s01 = rectToScreen(uv);
    float ndcX = s01.x * 2.0f - 1.0f;
    float ndcY = (1.0f - s01.y) * 2.0f - 1.0f;
    float x = viewZ * (ndcX - TESR_ProjectionTransform[2][0]) / TESR_ProjectionTransform[0][0];
    float y = viewZ * (ndcY - TESR_ProjectionTransform[2][1]) / TESR_ProjectionTransform[1][1];
    return float3(x, y, viewZ);
}

float4 NormalsPS(VSOUT IN) : COLOR
{
    float2 t = TESR_ReciprocalResolution.xy;
    float2 uv = clampToRectIfOn(IN.UVCoord, t);

    // One and two texels to each side, clamped to the drawn part: the nearest valid neighbour at an edge.
    float2 uvL1 = clampToRect(uv - float2(t.x, 0.0f), t), uvL2 = clampToRect(uv - float2(2.0f * t.x, 0.0f), t);
    float2 uvR1 = clampToRect(uv + float2(t.x, 0.0f), t), uvR2 = clampToRect(uv + float2(2.0f * t.x, 0.0f), t);
    float2 uvU1 = clampToRect(uv - float2(0.0f, t.y), t), uvU2 = clampToRect(uv - float2(0.0f, 2.0f * t.y), t);
    float2 uvD1 = clampToRect(uv + float2(0.0f, t.y), t), uvD2 = clampToRect(uv + float2(0.0f, 2.0f * t.y), t);

    float zC = ViewZAt(uv);
    float zL1 = ViewZAt(uvL1), zL2 = ViewZAt(uvL2);
    float zR1 = ViewZAt(uvR1), zR2 = ViewZAt(uvR2);
    float zU1 = ViewZAt(uvU1), zU2 = ViewZAt(uvU2);
    float zD1 = ViewZAt(uvD1), zD2 = ViewZAt(uvD2);

    float3 pC = ViewPositionAt(uv, zC);
    float3 pL1 = ViewPositionAt(uvL1, zL1), pR1 = ViewPositionAt(uvR1, zR1);
    float3 pU1 = ViewPositionAt(uvU1, zU1), pD1 = ViewPositionAt(uvD1, zD1);

    // Per axis, the side whose two-texel extrapolation predicts the centre's depth better is used (creases and
    // silhouettes are not averaged across). Horizontal: left only when strictly better (ties go right).
    // Vertical: down only when strictly better (ties go up). Both derivatives point along +x and +y.
    float errL = abs((2.0f * zL1 - zL2) - zC);
    float errR = abs((2.0f * zR1 - zR2) - zC);
    float errU = abs((2.0f * zU1 - zU2) - zC);
    float errD = abs((2.0f * zD1 - zD2) - zC);
    float3 dx = errL < errR ? pC - pL1 : pR1 - pC;
    float3 dy = errD < errU ? pC - pD1 : pU1 - pC;

    float3 n = cross(dy, dx);
    float len = length(n);
    n = len > 1.0e-12f ? n / len : float3(0.0f, 0.0f, -1.0f);
    return float4(n * 0.5f + 0.5f, 1.0f);
}

// The smoothing of one pass. The source is the normals buffer (pass 0) or the spare (pass 1); the axis is the pass's.
float4 SmoothNormals(sampler2D source, float2 axis, float2 uv)
{
    float2 t = TESR_ReciprocalResolution.xy;
    float4 centreSample = tex2D(source, uv);
    float zC = ViewZAt(uv);
    if (zC >= 0.99f * TESR_CameraData.y) return centreSample;	// sky: left unchanged

    float3 nC = normalize(centreSample.xyz * 2.0f - 1.0f);
    float3 pC = ViewPositionFromProjection(uv, zC);
    float reach = TESR_NormalsSmoothData.x;

    // Six taps at k x reach / 3 texels, k = -3, -2, -1, 1, 2, 3, along the pass's axis.
    float3 sum = nC;
    [unroll]
    for (int i = 0; i < 6; i++)
    {
        int k = i < 3 ? i - 3 : i - 2;	// -3 -2 -1 1 2 3
        float distTexels = abs((float)k) * reach / 3.0f;
        float2 uvRaw = uv + axis * (float)k * reach / 3.0f * t;
        float onScreen = outsideRect(uvRaw) ? 0.0f : 1.0f;
        float2 uvT = clampToRect(uvRaw, t);

        float zT = ViewZAt(uvT);
        float4 tapSample = tex2D(source, uvT);
        float3 nT = normalize(tapSample.xyz * 2.0f - 1.0f);
        float3 pT = ViewPositionFromProjection(uvT, zT);

        // Plane factor: the tap's distance from the centre's tangent plane against a tolerance that grows with the
        // distance (a bend of about 30 degrees stays inside it; a rock's thickness does not). Normal factor: a wall
        // meeting a floor, whose normals are far apart, stays a crease.
        float tolerance = max(zC * 0.0005f * distTexels, 0.5f);
        float planeDist = abs(dot(pT - pC, nC));
        float planeFactor = saturate(1.0f - planeDist / tolerance);
        float normalFactor = smoothstep(0.5f, 0.8f, dot(nT, nC));
        sum += nT * (planeFactor * normalFactor * onScreen);
    }
    float len = length(sum);
    float3 n = len > 1.0e-12f ? sum / len : nC;
    return float4(n * 0.5f + 0.5f, centreSample.a);
}

float4 SmoothXPS(VSOUT IN) : COLOR
{
    float2 uv = clampToRectIfOn(IN.UVCoord, TESR_ReciprocalResolution.xy);
    return SmoothNormals(TESR_NormalsBuffer, float2(1.0f, 0.0f), uv);
}

float4 SmoothYPS(VSOUT IN) : COLOR
{
    float2 uv = clampToRectIfOn(IN.UVCoord, TESR_ReciprocalResolution.xy);
    return SmoothNormals(TESR_NormalsSmoothTemp, float2(0.0f, 1.0f), uv);
}

technique Normals
{
    pass
    {
        VertexShader = compile vs_3_0 FrameVS();
        PixelShader = compile ps_3_0 NormalsPS();
    }
}

technique NormalsSmooth
{
    pass SmoothX
    {
        VertexShader = compile vs_3_0 FrameVS();
        PixelShader = compile ps_3_0 SmoothXPS();
    }
    pass SmoothY
    {
        VertexShader = compile vs_3_0 FrameVS();
        PixelShader = compile ps_3_0 SmoothYPS();
    }
}
