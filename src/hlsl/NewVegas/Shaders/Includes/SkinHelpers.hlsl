// Skinning helpers: up to four bones per vertex, each bone stored as three float4 rows of a 3x4 matrix in the bone palette.

// Rows idx, idx+1, idx+2 of the palette applied to a point (w used).
float3 skinRowsPoint(float4 palette[54], float idx, float4 p) {
    float3 out3;
    out3.x = dot(palette[idx].xyzw, p.xyzw);
    out3.y = dot(palette[idx + 1].xyzw, p.xyzw);
    out3.z = dot(palette[idx + 2].xyzw, p.xyzw);
    return out3;
}

// The same for a direction (translation column left out).
float3 skinRowsDir(float4 palette[54], float idx, float3 d) {
    float3 out3;
    out3.x = dot(palette[idx].xyz, d.xyz);
    out3.y = dot(palette[idx + 1].xyz, d.xyz);
    out3.z = dot(palette[idx + 2].xyz, d.xyz);
    return out3;
}

// Skinned position: the four bone transforms weighted by blend.
float3 BonesTransformPosition(float4 palette[54], float4 slots, float4 w4, float4 p) {
    float3 acc = skinRowsPoint(palette, slots.x, p) * w4.x;
    acc += skinRowsPoint(palette, slots.y, p) * w4.y;
    acc += skinRowsPoint(palette, slots.z, p) * w4.z;
    acc += skinRowsPoint(palette, slots.w, p) * w4.w;
    return acc;
}

// Skinned direction (not normalised).
float3 BonesTransformVector(float4 palette[54], float4 slots, float4 w4, float3 d) {
    float3 acc = skinRowsDir(palette, slots.x, d) * w4.x;
    acc += skinRowsDir(palette, slots.y, d) * w4.y;
    acc += skinRowsDir(palette, slots.z, d) * w4.z;
    acc += skinRowsDir(palette, slots.w, d) * w4.w;
    return acc;
}

// Skinned tangent frame, each axis renormalised; rows are tangent, binormal, normal.
float3x3 BonesTransformTBN(float4 palette[54], float4 slots, float4 w4, float3 tangent, float3 binormal, float3 normal) {
    float3 t = normalize(BonesTransformVector(palette, slots, w4, tangent));
    float3 b = normalize(BonesTransformVector(palette, slots, w4, binormal));
    float3 n = normalize(BonesTransformVector(palette, slots, w4, normal));
    return float3x3(t, b, n);
}
