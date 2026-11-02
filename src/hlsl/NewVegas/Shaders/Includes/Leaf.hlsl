// Pixel-shader pieces shared by the SpeedTree leaf shaders (STLEAF2000/2001 and their TMS variants).
// The vertex shader hands over the game's summed lighting (TEXCOORD1) and, separately, the sun's share of it (TEXCOORD3), so the
// pixel shader can take away exactly the shadowed part of the sun: fully lit gives back the game's own value to the bit.
#ifndef LEAF_INCLUDED
#define LEAF_INCLUDED

// UNOFFICIAL linear lighting (Includes/LinearLight.hlsl, [Shaders.PBR.Linear]), compiled in only when on at startup
// (LINEAR_LIGHTING_ON, ShaderRecord::LoadShader); otherwise LinColor / LinEncode / LinScale return their input.
#if defined(LINEAR_LIGHTING_ON) && LINEAR_LIGHTING_ON && !defined(LINEAR_LIGHTING)
    #define LINEAR_LIGHTING
#endif
#ifdef __INTELLISENSE__
    #include "LinearLight.hlsl"
#else
    #include "includes/LinearLight.hlsl"
#endif

sampler2D DiffuseMap: register(s0);	// leaf texture

// Interpolators written by the leaf vertex shaders (the member names are used by the STLEAF pixel shaders).
struct PS_INPUT {	// vertex to pixel
    float2 uv:             TEXCOORD0;
    float4 lighting:       TEXCOORD1_centroid;	// game lighting, all lights summed
    float4 fog:            TEXCOORD2_centroid;	// xyz fog colour, w fog amount
    float3 sun:            TEXCOORD3_centroid;	// the sun's part of .lighting
    float4 shadowWorldPos: TEXCOORD4;	// w flags whether the vertex shader filled it in
};

struct PS_OUTPUT { float4 color: COLOR0; };

// Call from main's top level only (the shadow lookup takes gradients, which dynamic branches would forbid).
float3 LeafLighting(PS_INPUT leaf) {
#if FORWARD_SHADOWS	// NVR sun shadows in the object pass
    float3 faceN = GetShadowGeometricNormal(leaf.shadowWorldPos.xyz);
    float sunVis = SHADOW_VS_PRESENT(leaf.shadowWorldPos.w) ? GetSunShadow(leaf.shadowWorldPos.xyz, faceN) : 1.0f;
    #ifdef LINEAR_LIGHTING
        // UNOFFICIAL linear lighting: decode the sun and everything else separately and add them in linear (the leaf shaders encode).
        return LinColor(leaf.lighting.rgb - leaf.sun) + LinColor(leaf.sun) * sunVis;
    #else
        return leaf.lighting.rgb - leaf.sun * (1.0f - sunVis);
    #endif
#else
    #ifdef LINEAR_LIGHTING
        return LinColor(leaf.lighting.rgb - leaf.sun) + LinColor(leaf.sun);
    #else
        return leaf.lighting.rgb;
    #endif
#endif
}

#endif
