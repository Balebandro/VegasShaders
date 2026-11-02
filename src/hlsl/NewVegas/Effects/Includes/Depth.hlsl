// A collection of functions that allow to query depth of a given pixel and also to reconstruct/project a point from screen to view space
// requires the shader to get access to the TESR_DepthBuffer sampler before the include.
// UNOFFICIAL 2026-10-27 RenderScale v2 (Includes/RenderRect.hlsl, docs/next/RENDERSCALE-V2.md): uv arguments are texture ("rect")
// coordinates; the NDC maths below takes the screen position they show (rectToScreen), and projectPosition returns a texture
// coordinate again (screenToRect). With RenderScale off both are the identity, so nothing changes.
#include "Includes/RenderRect.hlsl"

uniform float4x4 TESR_ProjectionTransform;
uniform float4x4 TESR_InvProjectionTransform;
uniform float4x4 TESR_ViewTransform;
uniform float4x4 TESR_InvViewTransform;
uniform float4 TESR_DepthConstants;
uniform float4 TESR_CameraData;
uniform float4 TESR_CameraPosition;

static const float invertedDepth = TESR_DepthConstants.b;
static const float nearZ = TESR_CameraData.r;
static const float farZ = TESR_CameraData.g;
static const float Q = farZ / (farZ - nearZ);

// CombineDepth stores linear depth (viewZ / farZ) in the first channel. The post-projection depth
// it used to store as a second channel is an exact function of it (CombineDepth's ToPS), so it is
// recomputed here and the buffer can be a single R32F channel -- half the bytes per depth read.
float projectedDepthFromLinear(float linear01_v)
{
	float view_z = max(linear01_v * farZ, 1.0e-6);
	return nearZ * (farZ - view_z) / (view_z * (farZ - nearZ));
}

float readDepth(float2 texel_uv)
{
	return tex2D(TESR_DepthBuffer, texel_uv).x * farZ;
}

// Explicit LOD variants are safe behind ps_3_0 dynamic branches. Full-screen effects use
// level zero, so this is equivalent to tex2D for buffers without mipmaps.
float readDepthLod(float2 texel_uv)
{
	return tex2Dlod(TESR_DepthBuffer, float4(texel_uv, 0.0f, 0.0f)).x * farZ;
}

float3 reconstructPosition(float2 uv_in)
{
    float2 tmp_s01 = rectToScreen(uv_in);
    float x = tmp_s01.x * 2 - 1;
    float y = (1 - tmp_s01.y) * 2 - 1;
    float z = projectedDepthFromLinear(tex2D(TESR_DepthBuffer, uv_in).x);
    float4 clip_space = float4(x, y, z, 1.0f);

    float4 view_space = mul(clip_space, TESR_InvProjectionTransform);
	
    view_space /= view_space.w;
	
    return view_space.xyz;
}

float3 projectPosition(float3 posn){
	float4 projection_v = mul(float4 (posn, 1.0), TESR_ProjectionTransform);
	projection_v.xyz /= projection_v.w;

	projection_v.x = projection_v.x * 0.5 + 0.5;
	projection_v.y = 0.5 - 0.5 * projection_v.y;
	projection_v.xy = screenToRect(projection_v.xy);

	return projection_v.xyz;
}

float3 toWorld(float2 uv)
{
	float2 tex = rectToScreen(uv);
	float3 ray_dir = float3(TESR_ViewTransform[0][2], TESR_ViewTransform[1][2], TESR_ViewTransform[2][2]);
	ray_dir += (1 / TESR_ProjectionTransform[0][0] * (2 * tex.x - 1)).xxx * float3(TESR_ViewTransform[0][0], TESR_ViewTransform[1][0], TESR_ViewTransform[2][0]);
	ray_dir += (-1 / TESR_ProjectionTransform[1][1] * (2 * tex.y - 1)).xxx * float3(TESR_ViewTransform[0][1], TESR_ViewTransform[1][1], TESR_ViewTransform[2][1]);
	return ray_dir;
}

float getHomogenousDepth(float2 uv)
{
	float eye_range = readDepth(uv);
	float3 camera_vector_v = toWorld(uv) * eye_range;
	return length(camera_vector_v);
}

float4 reconstructWorldPosition(float2 uv, out float view_depth){
    float2 tmp_s01 = rectToScreen(uv);
    float x = tmp_s01.x * 2 - 1;
    float y = (1 - tmp_s01.y) * 2 - 1;
    float z = projectedDepthFromLinear(tex2D(TESR_DepthBuffer, uv).x);
    float4 clip_space = float4(x, y, z, 1.0f);

    float4 view_space = mul(clip_space, TESR_InvProjectionTransform);
	
    view_space /= view_space.w;
	
    view_depth = view_space.z;
	
    float4 world_space = mul(view_space, TESR_InvViewTransform);

    return float4(world_space.xyz, 1.0f);
}

float4 reconstructWorldPositionLod(float2 uv, out float view_depth){
    float2 tmp_s01 = rectToScreen(uv);
    float x = tmp_s01.x * 2 - 1;
    float y = (1 - tmp_s01.y) * 2 - 1;
    float z = projectedDepthFromLinear(tex2Dlod(TESR_DepthBuffer, float4(uv, 0.0f, 0.0f)).x);
    float4 view_space = mul(float4(x, y, z, 1.0f), TESR_InvProjectionTransform);

    view_space /= view_space.w;
    view_depth = view_space.z;

    float4 world_space = mul(view_space, TESR_InvViewTransform);
    return float4(world_space.xyz, 1.0f);
}
