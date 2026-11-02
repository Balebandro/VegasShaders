// Refraction (heat haze / glass distortion) shaders for objects, drawn in the RenderNormals pass: each pixel writes the screen
// offset the refraction applies (rg, encoded 0..1), its strength (b) and a mask (a).
//
// Variants:
//   SKIN   bone-skinned meshes
//   FIRE   flames: the offset scrolls with time and fades where the surface turns from the camera
//   CLEAR  no normal map: a neutral (zero) offset
//   SM3    the game's Shader Model 3 constant layout
//
// Vertex shaders: SLS2068 plain, SLS2069 SKIN, SLS2070 FIRE, SLS2071 CLEAR, SLS2072 CLEAR + SKIN; SM3033-SM3037 the SM3 forms.
// Pixel shaders:  SLS2075 plain, SLS2076 FIRE, SLS2077 CLEAR; SM3032-SM3036 the SM3 forms.

#ifdef __INTELLISENSE__	// editor preview: the skinned vertex shader
    #define VS	// implied: vertex shader
    #define SKIN	// implied: skinned mesh
#endif

#include "includes/Helpers.hlsl"	// expand, compress, sqr, weight

#ifdef SKIN	// skinned mesh
    #include "includes/SkinHelpers.hlsl"	// BonesTransformPosition / Vector
#endif

#ifdef VS	// vertex shader

struct VertexIn {
    float4 posOS: POSITION;
    float3 normalOS: NORMAL;
#ifndef FIRE	// not: flame variant
    float4 texc: TEXCOORD0;
#endif
#ifdef SKIN	// skinned mesh
    float3 boneWeights: BLENDWEIGHT;	// three weights; the fourth is 1 - their sum
    float4 boneIndices: BLENDINDICES;	// bone indices as colour (x 765: three rows a bone)
#endif
};

struct VertexOut {
    float4 clipPos: POSITION;
#ifdef FIRE	// flame variant
    float4 scroll: TEXCOORD0;	// xy scrolling flame uv, z view depth, w its divisor (at least 1)
    float facing: TEXCOORD1;	// N . V
#elif !defined(CLEAR)	// not: no normal map: neutral
    float3 texc: TEXCOORD0;	// xy texture uv, z distance divisor (at least 1)
    float2 bend: TEXCOORD1;	// the view-space normal's xy, clamped to +-0.1
#endif
};

// The game's vertex constants (the skinned forms in their own registers, moved again in the SM3 layout).
#ifdef SKIN	// skinned mesh
    #ifdef SM3	// the game's SM3 register layout
        row_major float4x4 SkinModelViewProj: register(c20);
        row_major float4x4 SkinWorldView: register(c24);
        float4 Bones[54]: register(c52);
    #else
        row_major float4x4 SkinModelViewProj: register(c1);
        row_major float4x4 SkinWorldView: register(c5);
        float4 Bones[54]: register(c44);
    #endif
#else
    row_major float4x4 ModelViewProj: register(c0);
    row_major float4x4 WorldView: register(c4);
#endif

#ifdef FIRE	// flame variant
    #ifdef SM3	// the game's SM3 register layout
        float Time: register(c38);
        float4 VEyePosition: register(c40);
    #else
        float Time: register(c13);
        float4 VEyePosition: register(c16);
    #endif
#endif

// Closer surfaces bend more: the offset is divided by a divisor that grows with clip depth (750 units per step past 0.8).
float refractionDivisor(float clipZ) {
    return max(clipZ / 750 + 0.8, 1);
}

VertexOut main(VertexIn v) {
    VertexOut o;

#if defined(SKIN)
    float4 slots = v.boneIndices.zyxw * 765.01001;
    float4 w4 = v.boneWeights.xyzz;
    w4.w = 1 - weight(v.boneWeights.xyz);

    float4 p = v.posOS.xyzw;
    p.w = 1;
    p.xyz = BonesTransformPosition(Bones, slots, w4, p);
    o.clipPos.xyzw = mul(SkinModelViewProj, p.xyzw);

    #ifndef CLEAR	// not: no normal map: neutral
        o.texc.xy = v.texc.xy;
        o.texc.z = refractionDivisor(o.clipPos.z);

        float3 skinnedN = BonesTransformVector(Bones, slots, w4, v.normalOS.xyz);
        float2 nView = float2(dot(SkinWorldView[0].xyz, normalize(skinnedN)), dot(SkinWorldView[1].xyz, normalize(skinnedN)));
        o.bend = clamp(nView, -0.1, 0.1);
    #endif
#elif defined(FIRE)	// flame variant
    o.clipPos.xyzw = mul(ModelViewProj, v.posOS.xyzw);

    // the flame texture is laid out in view space, 300 units a repeat, scrolling with time
    float4 shrunk = v.posOS.xyzw / 300;
    float3 viewCoord = float3(dot(WorldView[0], shrunk), dot(WorldView[1], shrunk), dot(WorldView[2], shrunk));
    o.scroll.xy = viewCoord.xy - Time.x;
    o.scroll.z = viewCoord.z;
    o.scroll.w = max(viewCoord.z, 1);

    o.facing = dot(v.normalOS.xyz, normalize(VEyePosition.xyz - v.posOS.xyz));
#else
    o.clipPos.xyzw = mul(ModelViewProj, v.posOS.xyzw);

    #ifndef CLEAR	// not: no normal map: neutral
        o.texc.xy = v.texc.xy;
        o.texc.z = refractionDivisor(o.clipPos.z);

        float2 nView = float2(dot(WorldView[0].xyz, v.normalOS.xyz), dot(WorldView[1].xyz, v.normalOS.xyz));
        o.bend = clamp(nView, -0.1, 0.1);
    #endif
#endif

    return o;
}
#endif	// VS

#ifdef PS	// pixel shader

struct PixelIn {
#ifdef FIRE	// flame variant
    float4 scroll: TEXCOORD0;
    float facing: TEXCOORD1;
#else
    float3 texc: TEXCOORD0;
    float2 bend: TEXCOORD1;
#endif
};

struct PixelOut { float4 color: COLOR0; };

#ifndef CLEAR	// not: no normal map: neutral
    sampler2D NormalMap: register(s0);	// the distortion map: rg the offset direction
    #ifdef SM3	// the game's SM3 register layout
        float4 PSRefractionPower: register(c6);	// x strength, y mask
    #else
        float4 PSRefractionPower: register(c29);
    #endif
#endif

PixelOut main(PixelIn i) {
    PixelOut o;

#ifdef CLEAR	// no normal map: neutral
    o.color.rgba = float4(0.5, 0.5, 0, 0);	// no offset, no strength
#else
    o.color.b = PSRefractionPower.x;

    #ifdef FIRE	// flame variant
        float2 dir = tex2D(NormalMap, i.scroll.xy).xy;
        o.color.rg = compress(normalize(expand(dir.xy)) / i.scroll.w);
        o.color.a = sqr(i.facing) * 0.5;
    #else
        float2 dir = tex2D(NormalMap, i.texc.xy).xy;
        o.color.rg = compress((0.9 * clamp(expand(dir.xy), -0.1, 0.1) + i.bend.xy) / i.texc.z);
        o.color.a = PSRefractionPower.y;
    #endif
#endif

    return o;
}

#endif	// PS
