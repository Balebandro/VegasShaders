// UNOFFICIAL 2026-10-29 shading a ray's hit in the ray tracing scene (FORK-RT-CORE.md "Hit shading"): for any trace shader in the
// helper. rtHitAlbedo() is the hit's diffuse colour (linear): with [Main.Main.RayTracing] HitTextures the texture's colour at the hit's
// texcoords (the albedo atlas: RGBA8, 64 x 64 a texture, 32 slots a row; slot = flags >> 16), else the record's average colour.
// Before including: #define RT_ALBEDO_ATLAS <the albedo atlas: a layout(rgba8) readonly image2D> when the pass binds it
// (RtCore::AlbedoAtlasImage() among its images; leave it undefined: averages only). The records (binding 10) and geometry data
// (binding 11) are declared here unless RT_RECORDS_DECLARED is defined (RtAlpha.glsl defines it; either order works).
// Call with the committed hit: rayQueryGetIntersectionInstanceIdEXT(q, true), ...PrimitiveIndexEXT(q, true), ...BarycentricsEXT(q, true).

#ifndef RT_RECORDS_DECLARED
struct RtRecord { vec4 albedo; uint flags; uint geometryId; uint vertexOffset; uint indexOffset; };
layout(set = 0, binding = 10, std430) readonly buffer RtRecords { RtRecord records[]; };
layout(set = 0, binding = 11, std430) readonly buffer RtGeometry { uint words[]; };
#define RT_RECORDS_DECLARED
#endif

#define RT_HIT_TEXCOORDS_FLAGS 0x60u   // alpha-tested (0x20) or textured (0x40): texcoords at floatBitsToUint(albedo.w)

uvec3 rtHitTriangle(uint instance, uint primitive) {
	const uint o = records[instance].indexOffset + 3u * primitive;
	return uvec3(words[o], words[o + 1u], words[o + 2u]);
}

vec3 rtHitObjectPosition(uint instance, uint vertex) {
	const uint o = records[instance].vertexOffset + 3u * vertex;
	return vec3(uintBitsToFloat(words[o]), uintBitsToFloat(words[o + 1u]), uintBitsToFloat(words[o + 2u]));
}

// The triangle's normal in object space (unnormalized; turn it by the instance's object-to-world and face it to the ray)
vec3 rtHitObjectNormal(uint instance, uint primitive) {
	const uvec3 i = rtHitTriangle(instance, primitive);
	const vec3 a = rtHitObjectPosition(instance, i.x), b = rtHitObjectPosition(instance, i.y), c = rtHitObjectPosition(instance, i.z);
	return cross(b - a, c - a);
}

bool rtHitHasTexcoords(uint instance) { return (records[instance].flags & RT_HIT_TEXCOORDS_FLAGS) != 0u; }

vec2 rtHitTexcoord(uint instance, uint primitive, vec2 bary) {
	const uint uvOffset = floatBitsToUint(records[instance].albedo.w);
	const uvec3 i = rtHitTriangle(instance, primitive);
	vec2 t0 = vec2(uintBitsToFloat(words[uvOffset + 2u * i.x]), uintBitsToFloat(words[uvOffset + 2u * i.x + 1u]));
	vec2 t1 = vec2(uintBitsToFloat(words[uvOffset + 2u * i.y]), uintBitsToFloat(words[uvOffset + 2u * i.y + 1u]));
	vec2 t2 = vec2(uintBitsToFloat(words[uvOffset + 2u * i.z]), uintBitsToFloat(words[uvOffset + 2u * i.z + 1u]));
	return t0 * (1.0 - bary.x - bary.y) + t1 * bary.x + t2 * bary.y;
}

vec3 rtHitAlbedo(uint instance, uint primitive, vec2 bary) {
#ifdef RT_ALBEDO_ATLAS
	if (rtHitHasTexcoords(instance)) {
		const uint slot = records[instance].flags >> 16;
		const ivec2 origin = ivec2(int(slot % 32u), int(slot / 32u)) * 64;
		const ivec2 texel = origin + clamp(ivec2(fract(rtHitTexcoord(instance, primitive, bary)) * 64.0), ivec2(0), ivec2(63));
		const vec3 c = imageLoad(RT_ALBEDO_ATLAS, texel).rgb;   // as the texture stores it (sRGB-encoded)
		return mix(pow((c + 0.055) / 1.055, vec3(2.4)), c / 12.92, lessThanEqual(c, vec3(0.04045)));
	}
#endif
	return records[instance].albedo.rgb;
}

// UNOFFICIAL 2026-10-30 the hit's glow (emissive) colour, linear, before any brightness: 0 for meshes without a glow map (record flag
// 0x04). With [Main.Main.RayTracing] GlowTextures (+ HitTextures) and #define RT_GLOW_ATLAS <the glow atlas: layout(rgba8) readonly
// image2D> (RtCore::GlowAtlasImage() among the pass's images): the glow map's colour at the hit (same slot and texcoords as the albedo
// atlas): coloured neon tubes, lit window panes, only the lit parts. Else the whole mesh glows in rtHitAlbedo's colour (the old way).
#define RT_HIT_GLOW_FLAG 0x04u
vec3 rtHitGlow(uint instance, uint primitive, vec2 bary) {
	if ((records[instance].flags & RT_HIT_GLOW_FLAG) == 0u) return vec3(0.0);
#ifdef RT_GLOW_ATLAS
	if (rtHitHasTexcoords(instance)) {
		const uint slot = records[instance].flags >> 16;
		const ivec2 origin = ivec2(int(slot % 32u), int(slot / 32u)) * 64;
		const ivec2 texel = origin + clamp(ivec2(fract(rtHitTexcoord(instance, primitive, bary)) * 64.0), ivec2(0), ivec2(63));
		const vec3 c = imageLoad(RT_GLOW_ATLAS, texel).rgb;   // as the glow map stores it (sRGB-encoded)
		return mix(pow((c + 0.055) / 1.055, vec3(2.4)), c / 12.92, lessThanEqual(c, vec3(0.04045)));
	}
#endif
	return rtHitAlbedo(instance, primitive, bary);
}
