// UNOFFICIAL 2026-10-29 alpha-tested casters in the ray tracing scene ([Main.Main.RayTracing] AlphaTested; FORK-RT-CORE.md): for any
// trace shader in the helper. Alpha-tested instances are non-opaque, have the cull mask bit 0x10 and the record flag 0x20; their
// texcoords (float2) are in the geometry data at the word offset floatBitsToUint(albedo.w), their diffuse alpha in the alpha atlas
// (R16F, 64 x 64 texels a texture, 32 slots a row; slot = flags >> 16), the reference in (flags >> 8) & 0xFF.
//
// Before including: #define RT_ALPHA_ATLAS <the atlas: a layout(r16f) readonly image2D>. The records (binding 10) and the geometry
// data (binding 11) are declared here as records[] / words[], unless RT_RECORDS_DECLARED is defined (then the includer's records[]
// must have .albedo, .flags and .indexOffset, its words[] the uint data).
// Use: trace WITHOUT gl_RayFlagsOpaqueEXT, and for each candidate:
//   while (rayQueryProceedEXT(q))
//     if (rayQueryGetIntersectionTypeEXT(q, false) == gl_RayQueryCandidateIntersectionTriangleEXT &&
//         rtAlphaPass(rayQueryGetIntersectionInstanceIdEXT(q, false), rayQueryGetIntersectionPrimitiveIndexEXT(q, false),
//                     rayQueryGetIntersectionBarycentricsEXT(q, false)))
//       rayQueryConfirmIntersectionEXT(q);
// (Opaque geometry never shows up as a candidate: it commits by itself.)

#ifndef RT_RECORDS_DECLARED
struct RtRecord { vec4 albedo; uint flags; uint geometryId; uint vertexOffset; uint indexOffset; };
layout(set = 0, binding = 10, std430) readonly buffer RtRecords { RtRecord records[]; };
layout(set = 0, binding = 11, std430) readonly buffer RtGeometry { uint words[]; };
#define RT_RECORDS_DECLARED
#endif

#define RT_RECORD_ALPHA_TESTED 0x20u
#define RT_ALPHA_SLOT_SIZE 64
#define RT_ALPHA_SLOTS_A_ROW 32u

vec2 rtTexcoord(uint uvOffset, uint vertex) {
	return vec2(uintBitsToFloat(words[uvOffset + 2u * vertex]), uintBitsToFloat(words[uvOffset + 2u * vertex + 1u]));
}

// True: this candidate hit is solid (its alpha passes the material's test). Not alpha-tested: always true.
bool rtAlphaPass(uint instance, uint primitive, vec2 bary) {
	const uint flags = records[instance].flags;
	if ((flags & RT_RECORD_ALPHA_TESTED) == 0u)
		return true;
	const uint uvOffset = floatBitsToUint(records[instance].albedo.w);
	const uint indexOffset = records[instance].indexOffset;
	const uvec3 i = uvec3(words[indexOffset + 3u * primitive], words[indexOffset + 3u * primitive + 1u], words[indexOffset + 3u * primitive + 2u]);
	const vec2 uv = rtTexcoord(uvOffset, i.x) * (1.0 - bary.x - bary.y) + rtTexcoord(uvOffset, i.y) * bary.x + rtTexcoord(uvOffset, i.z) * bary.y;
	const uint slot = flags >> 16;
	const ivec2 origin = ivec2(int(slot % RT_ALPHA_SLOTS_A_ROW), int(slot / RT_ALPHA_SLOTS_A_ROW)) * RT_ALPHA_SLOT_SIZE;
	const ivec2 texel = origin + clamp(ivec2(fract(uv) * float(RT_ALPHA_SLOT_SIZE)), ivec2(0), ivec2(RT_ALPHA_SLOT_SIZE - 1));
	const float alpha = imageLoad(RT_ALPHA_ATLAS, texel).r;
	return alpha > float((flags >> 8) & 0xFFu) / 255.0;	// D3DCMP_GREATER, the game's alpha test
}
