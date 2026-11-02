// UNOFFICIAL 2026-10-31 BackDepth (core/BackDepth.h, [Main.Main.Quality] BackDepth, off; ROOT-ASSUMPTIONS-PLAN B3): the nearest back face of
// the objects near the camera, per pixel, so an effect knows how thick the surface in front is instead of guessing. The effect declares
// sampler2D TESR_BackDepthBuffer (POINT, CLAMP) before including this. 0 = no back face known there (land, past the distance, off).
float4 TESR_BackDepthData;	// x 1 = drawn this frame, y the projection's _33, z its _43 (depth -> view distance)

// The view distance of the nearest back face at uv (texture coordinates, as the depth buffer's), 0 when none is known.
float BackViewZ(float2 uv)
{
	float viewZ = 0.0f;
	[branch] if (TESR_BackDepthData.x > 0.5f) {
		float z = tex2Dlod(TESR_BackDepthBuffer, float4(uv, 0.0f, 0.0f)).r;
		if (z > 0.0f) viewZ = TESR_BackDepthData.z / (z - TESR_BackDepthData.y);
	}
	return viewZ;
}
