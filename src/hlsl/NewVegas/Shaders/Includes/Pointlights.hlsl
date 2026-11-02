// Point light falloff curves. Every input "toLight" is the unnormalised vector from the surface to the light.

#ifdef __INTELLISENSE__
    #include "Helpers.hlsl"	// editor only
#endif

// Game falloff: 1 - |d / r|^2, clamped.
float vanillaAtt(float3 toLight, float range) {
    const float3 scaled = toLight / range;
    return 1 - shades(scaled, scaled);
}

// The same curve from an object-space squared distance: 1 - saturate(d^2 / r^2) equals vanillaAtt exactly. Callers use it when
// the light vector went through a TBN that may not be orthonormal (lengths are not kept there), so d^2 is taken before that.
float vanillaAttSq(float distSq, float range) {
    return 1 - saturate(distSq / (range * range));
}

// Smooth windowed falloff, after https://lisyarus.github.io/blog/posts/point-light-attenuation.html
float lisyarusAtt(float3 toLight, float range, float falloff=5.0) {
    const float3 scaled = toLight / range;
    const float d2 = shades(scaled, scaled);
    return sqr(1 - d2) / (1 + falloff * d2);
}

// Inverse-square with a smooth window to zero at the range (Frostbite / UE4 style).
float frostbiteAtt(float3 toLight, float range) {
    const float lenSq = dot(toLight, toLight);
    const float rangeInvSq = 1.f / (range * range);
    const float ratio = lenSq * rangeInvSq;
    const float window = saturate(1.f - ratio * ratio);
    const float windowSq = window * window;
    return windowSq / (lenSq + 1);
}
