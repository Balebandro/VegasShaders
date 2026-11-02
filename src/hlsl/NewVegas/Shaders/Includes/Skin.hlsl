// Skin lighting for SkinTemplate.hlsl (UNOFFICIAL 2026-10-25, the skin shader rewritten on this build's own stack).
//
// Built from the includes every other lit surface in this build uses: the BRDF terms of PBR.hlsl (Fresnel, GGX through
// BRDF(), the geometric specular anti-aliasing, the split-sum environment response), the light and ambient strengths of
// PBRScale.hlsl, the sky of SkyAmbient.hlsl, linear lighting (LinearLight.hlsl) and the lamps' falloff (LampFalloff.hlsl).
// The sun's and the lamps' shadows come from Shadow.hlsl and PointShadowForward.hlsl in the template. What skin adds, and
// where it comes from:
//   - Diffusion under the skin: Penner's pre-integrated skin (GPU Pro 2, 2011) over d'Eon's measured diffusion profile
//     (GPU Gems 3 ch. 14), evaluated as the irradiance of a spherical Gaussian whose sharpness follows the surface's
//     curvature (Hill's closed form for an SG's cosine irradiance, from MJP's "SG Series" part 3). The sharpness constants
//     below were fitted for this build to the pre-integrated table, per colour, over radii 1-50 mm (bench scratch
//     fit_skin.py: largest error 0.07 red, 0.016 green, 0.008 blue; red's miss is the light that goes right round a 1 mm
//     sphere, which the transmission term carries instead). Fine relief is softened per colour with a blurred normal
//     (Penner's second technique). The curvature is measured from screen-space derivatives.
//   - Light through thin parts (ears, nostrils, fingers) against the light: Barre-Brisebois & Bouchard's approximation
//     (GDC 2011), coloured by Jimenez's transmittance of d'Eon's profile (Separable SSS, 2012), the thickness read off the
//     curvature. White at the thinnest rims, orange, then deep red.
//   - Highlights: two GGX lobes at skin's F0 of 0.028 (Jimenez 2015; Unreal's skin model: 0.75 x and 1.3 x the roughness,
//     mixed 0.85 / 0.15) through PBR.hlsl's BRDF, so they take the same masking term and specular anti-aliasing as the
//     objects next to the skin; and a sheen lobe for the fuzz on skin (Estevez & Kulla's "Charlie" distribution, Neubelt's
//     visibility). The light the highlight reflects does not also enter the skin (diffuse x (1 - F)).
//   - Shadow edges: the penumbra reddened per colour (light carried under the skin across the shadow line; bounded, and
//     only on penumbrae a few pixels wide, since 2026-10-27), and the skin's own far side lets the scattered light through
//     the shadow map's self-shadow near its terminator (SkinTemplate: a second, coarse tap toward the sun says whether a
//     shadow is the surface's own).
//   - Sky: the sky light from SkyAmbient.hlsl's harmonics, softened per colour like the direct light, and a dual-lobe sky
//     reflection with the same response as the highlights.
//
// Colour space: everything here works in the build's lighting space (the game's gamma, or linear with [Shaders.PBR.Linear]):
// lights come in through PBRLight / PBRAmbient, the albedo through LinColor, physical colours (the transmittance) through
// SkinLinearColor. Direct lighting works in whatever space the caller's vectors share (SkinTemplate: tangent space); the sky
// terms take WORLD-space vectors. Derivatives are taken in SkinSetup only, which must be called at the pixel shader's top level.
#ifndef SKIN_INCLUDED
#define SKIN_INCLUDED

#if defined(__INTELLISENSE__)
    #include "PBR.hlsl"
    #include "PBRScale.hlsl"
#else
    #include "includes/PBR.hlsl"
    #include "includes/PBRScale.hlsl"
#endif

// --- constants (effects/Skin.cpp). c192-c195 are free in every skin pixel shader: Shadow.hlsl pins c100-c133, the sky
// c134-c145, the sun cross-fade c146-c166, contact hardening c167, the lamp shadows c174-c190, linear lighting c191.
float4 TESR_SkinMaterial : register(c192); // x SpecularStrength, y Roughness, z Scattering (how far light spreads, x the measured profile), w Translucency
float4 TESR_SkinLook     : register(c193); // x SkyReflection (0 indoors), y wetness (rain x RainWetness), z ShadowScatter, w Sheen
// Per draw, set by the SkinShader hook (Hooks/Shaders.cpp) after the shader binds (so NVR_, not TESR_: ShaderRecord re-uploads
// every TESR_ constant when the shader changes): x 1 while the screen-space blur's targets are bound for this draw (the blur
// then does the diffusion: doing both doubled it), y 1 while the FaceGen maps are bound to a light-only pass.
float4 NVR_SkinDraw      : register(c194);
// x [Shaders.Skin.Debug] DebugView 1-7 (0 off, or one of the blur's views), y 1 while the Interiors settings are in use,
// z 1 = LegacyFaceShadows (the 2026-10-25 shadow-edge and curvature arithmetic, for an A/B; see SkinShadow). Declared by the
// template ahead of Shadow.hlsl when it needs it there (SKIN_DEBUG_DECLARED).
#ifndef SKIN_DEBUG_DECLARED
float4 TESR_SkinDebug    : register(c195);
#endif
#define SKIN_LEGACY (TESR_SkinDebug.z > 0.5f)

#define SKIN_F0 0.028f
#define SKIN_LOBE0 0.75f            // the sharp lobe's roughness, x Roughness
#define SKIN_LOBE1 1.3f             // the broad lobe's
#define SKIN_LOBE_MIX 0.15f         // the broad lobe's share
#define SKIN_SHEEN_ALPHA 0.1225f    // the fuzz lobe: roughness 0.35 squared
#define SKIN_SHEEN_F0 0.045f
#define SKIN_MM_PER_UNIT 14.2857f   // one game unit is 1/70 m
#define SKIN_SCATTER_BASE 2.0f      // Scattering 1 = twice the measured profile's width: the true width barely shows on a head at game distances
#define SKIN_TRANSMIT_DISTORTION 0.3f
#define SKIN_TRANSMIT_POWER 4.0f
#define SKIN_SOFT_NORMAL_BIAS 2.0f  // mip levels the normal map is read blurred by for the per-colour softening

// Per pixel, from SkinSetup.
static float skinRoughness = 0.5f;
static float skinSpecScale = 0.0f;     // the highlights' strength: the specular mask x SpecularStrength (x wetness)
static float skinCurvature = 0.0f;     // 1/mm, after Scattering: 0 flat (or the blur's), 1 a 1 mm radius
static float skinThinness = 0.0f;      // the same curvature without Scattering: 0 flat .. 1 a 1 mm radius, for the transmission
static float skinSkyWeight = 0.0f;     // the share of the sky the reflection takes (SkinSkyReflection), which the ambient leaves out

// A LINEAR colour (a physical constant) in the build's lighting space: encoded as the sky's colours are (sqrt), then decoded
// with the lights (LinColor returns it as is without linear lighting).
float3 SkinLinearColor(float3 c) { return LinColor(sqrt(max(c, 0.0f))); }

// Call once per pixel at the TOP LEVEL of the pixel shader (derivatives).
//   normalTS:     the bumped normal, tangent space (for the specular anti-aliasing)
//   worldNormal:  the interpolated vertex normal in world space (for the curvature)
//   worldPos:     the camera-relative world position
//   mask:         the normal map's alpha, the specular mask (0.5 reads as full skin)
//   blurTakes:    1 while the screen-space blur takes this draw (NVR_SkinDraw.x): the curvature diffusion then steps aside
void SkinSetup(float3 normalTS, float3 worldNormal, float3 worldPos, float mask, float blurTakes) {
    const float wet = saturate(TESR_SkinLook.y);
    const float roughness = lerp(TESR_SkinMaterial.y, TESR_SkinMaterial.y * 0.55f, wet);   // a water film is smoother than skin
    skinRoughness = clamp(SpecularAA(normalTS, roughness), 0.08f, 1.0f);
    skinSpecScale = mask * 2.0f * TESR_SkinMaterial.x * (1.0f + wet);

    // How fast the surface turns per millimetre: the curvature in 1/mm (Penner), capped at a 1 mm radius, where the fit ends.
    // 2026-10-27 (user, low side sun: red blotches in the shaded half of faces): CONVEX curvature only, signed: along each
    // screen axis, the change of the normal measured along the change of the position (1/R on a sphere of radius R, exactly as
    // before; negative in a crease). The unsigned measure (the length of the normal's change over the position's) read the
    // eye sockets, the corners of the mouth and nostrils and the folds of the neck as tiny convex spheres and wrapped red
    // light deep into them, and it took every centroid shift of the vertex normal along the triangle edges (MSAA) for
    // curvature; the projection on the position's change drops both. Penner's model is a convex sphere: a crease gets plain
    // shading. LegacyFaceShadows: the unsigned measure.
    const float3 dNx = ddx(worldNormal);
    const float3 dNy = ddy(worldNormal);
    const float3 dPx = ddx(worldPos);
    const float3 dPy = ddy(worldPos);
    float curvature = max(0.5f * (dot(dNx, dPx) / max(dot(dPx, dPx), 1e-8f) + dot(dNy, dPy) / max(dot(dPy, dPy), 1e-8f)), 0.0f) / SKIN_MM_PER_UNIT;
    [flatten] if (SKIN_LEGACY)
        curvature = length(abs(dNx) + abs(dNy)) / max(length(abs(dPx) + abs(dPy)) * SKIN_MM_PER_UNIT, 1e-4f);
    skinThinness = saturate(curvature * SKIN_SCATTER_BASE);
    skinCurvature = blurTakes > 0.0f ? 0.0f : saturate(curvature * SKIN_SCATTER_BASE * TESR_SkinMaterial.z);
}

// The cosine irradiance of a normalised spherical Gaussian of sharpness lambda about the light, per colour (Hill's fit, MJP
// "SG Series" part 3): N.L blurred by the lobe. saturate(N.L) as lambda grows; the light wraps past the terminator and the
// peak drops as it shrinks.
float3 SkinSGIrradiance(float3 NdotL, float3 lambda) {
    const float c0 = 0.36f;
    const float c1 = 1.0f / (4.0f * c0);
    const float3 eml = exp(-lambda);
    const float3 em2l = eml * eml;
    const float3 rl = 1.0f / lambda;
    const float3 scale = 1.0f + 2.0f * em2l - rl;
    const float3 bias = (eml - em2l) * rl - em2l;
    const float3 x = sqrt(max(1.0f - scale, 1e-8f));
    const float3 x0 = c0 * NdotL;
    const float3 x1 = c1 * x;
    const float3 n = x0 + x1;
    const float3 y = abs(x0) <= x1 ? n * n / x : saturate(NdotL);
    return saturate(scale * y + bias);
}

// The diffused N.L of one light, per colour: red along the blurred normal, green and blue along normals blended toward it
// (Penner), each blurred by the lobe its colour's scattering gives at this curvature. Fitted sharpness: lambda = a c^-b with
// a, b = 2.34, 1.354 (red), 27.1, 2.017 (green), 63.4, 2.222 (blue). Flat skin, or skin the screen-space blur takes
// (skinCurvature 0): plain saturate(N.L), which the lobes become within 0.002 at the terminator. plain: each colour's
// saturate(N.L) along its own normal, without the lobes (what the curvature adds is the part above it).
float3 SkinDiffusion(float3 N, float3 Nsoft, float3 L, out float3 plain) {
    const float3 normalGreen = normalize(lerp(N, Nsoft, 0.6f));
    const float3 normalBlue = normalize(lerp(N, Nsoft, 0.3f));
    const float3 NdotL = float3(dot(Nsoft, L), dot(normalGreen, L), dot(normalBlue, L));
    plain = saturate(NdotL);
    [branch] if (skinCurvature > 0.002f) {
        const float c = skinCurvature;
        const float3 lambda = float3(2.34f * pow(c, -1.354f), 27.1f * pow(c, -2.017f), 63.4f * pow(c, -2.222f));
        return SkinSGIrradiance(NdotL, lambda);
    }
    return saturate(NdotL);
}

// A shadow's visibility (0 dark .. 1 lit) with light carried under the skin across the shadow line: the penumbra is widened
// into the shadow per colour, red furthest, by ShadowScatter. 0 and 1 stay 0 and 1; off (0) returns the shadow as it is.
// edge: how much the shadow changes from this pixel to the next (fwidth of it, 0 for a light without a shadow map).
// 2026-10-27 (user, low side sun: "red spots" in the shade of faces, and a thin saturated red line along hard shadow edges):
// the colours used to be pow(shadow, 1 / (1 + reach)), whose red-to-blue ratio grows without bound toward the dark end
// (x2.2 at 5 % lit, x3.4 at 1 %): every faint speck of light inside a shadow (the soft-edge disc's grain, a thin caster's
// partial texels) came out saturated red, and so did the last pixel of a hard edge. Now shadow + reach x shadow x (1 - shadow):
// the same colours in the middle of a penumbra (within 0.01 at half lit), the shift vanishing at both ends, the red-to-blue
// ratio never above (1 + 0.6) / (1 + 0.12) at ShadowScatter 1. And only where the penumbra spans a few pixels: an edge that goes
// from lit to dark within about a pixel (aliased, sub-pixel, or speckle) keeps its plain grey, as the light carried a millimetre
// across it could not show at that size anyway. LegacyFaceShadows: the old curve, everywhere.
float3 SkinShadow(float shadow, float edge) {
    const float3 reach = TESR_SkinLook.z * float3(0.6f, 0.25f, 0.12f);
    const float s = saturate(shadow);
    const float3 legacy = pow(s.xxx, 1.0f / (1.0f + reach));
    const float3 bounded = saturate(s + reach * (s * (1.0f - s) * saturate(1.6f - 2.0f * edge)));
    return SKIN_LEGACY ? legacy : bounded;
}

// Jimenez's transmittance through d mm of skin (Separable SSS, after d'Eon's six Gaussians per colour): white at 0, orange,
// then red as the depth grows. LINEAR.
float3 SkinTransmittance(float d) {
    const float dd = -d * d;
    return float3(0.233f, 0.455f, 0.649f) * exp(dd / 0.0064f)
         + float3(0.1f, 0.336f, 0.344f) * exp(dd / 0.0484f)
         + float3(0.118f, 0.198f, 0.0f) * exp(dd / 0.187f)
         + float3(0.113f, 0.007f, 0.007f) * exp(dd / 0.567f)
         + float3(0.358f, 0.004f, 0.0f) * exp(dd / 1.99f)
         + float3(0.078f, 0.0f, 0.0f) * exp(dd / 7.41f);
}

// Both highlight lobes of one light (the light already decoded: PBRLight), times N.L and the light, in the build's
// convention (PBR.hlsl: a white Lambertian facing the light returns the light itself).
float3 SkinSpecular(float3 N, float3 V, float3 L, float3 light) {
    const float r0 = clamp(skinRoughness * SKIN_LOBE0, 0.04f, 1.0f);
    const float r1 = clamp(skinRoughness * SKIN_LOBE1, 0.04f, 1.0f);
    const float3 H = normalize(V + L);
    const float NdotL = max(shades(N, L), 0.00001f);
    const float NdotV = max(shades(N, V), 0.00001f);
    const float NdotH = shades(N, H);
    const float LdotH = shades(L, H);
    const float3 fresnel = Fresnel(SKIN_F0.xxx, (1.0f).xxx, LdotH);
    const float3 lobes = lerp(BRDF(r0, fresnel, NdotV, NdotL, NdotH), BRDF(r1, fresnel, NdotV, NdotL, NdotH), SKIN_LOBE_MIX);
    return lobes * (NdotL * PI * skinSpecScale) * light;
}

// The fuzz on skin: a sheen lobe (Charlie distribution, Neubelt's visibility, Schlick's Fresnel at the fuzz's F0), strongest
// where both the light and the view skim the skin: the rims of cheeks, ears and noses. Times N.L and the light as above.
float3 SkinSheen(float3 N, float3 V, float3 L, float3 light) {
    const float3 H = normalize(V + L);
    const float NdotL = clamp(dot(N, L), 1e-5f, 1.0f);
    const float NdotV = saturate(abs(dot(N, V)) + 1e-5f);
    const float NdotH = saturate(dot(N, H));
    const float VdotH = saturate(dot(V, H));
    const float invAlpha = 1.0f / SKIN_SHEEN_ALPHA;
    const float D = (2.0f + invAlpha) * pow(max(1.0f - NdotH * NdotH, 1e-6f), invAlpha * 0.5f) / (2.0f * PI);
    const float visibility = 1.0f / (4.0f * max(NdotL + NdotV - NdotL * NdotV, 1e-5f));
    const float fresnel = Fresnel(SKIN_SHEEN_F0.xxx, (1.0f).xxx, VdotH).x;
    return (D * visibility * fresnel * NdotL * PI * TESR_SkinLook.w) * light;
}

// One light. Returns the diffuse light (diffusion and transmission, times the albedo) and, in specular, the highlights
// (both lobes and the sheen), kept apart for the screen-space blur. Every vector in one space (SkinTemplate: tangent).
//   albedo:        decoded (LinColor)
//   N, Nsoft:      the bumped normal and its blurred version
//   geometricNdotL: the vertex normal's cosine to the light (the surface's own far side, for the self-shadow and transmission)
//   light:         the light's colour, decoded (PBRLight), with any gamma-space factor (the lamp falloff, the projected actor
//                  shadow) multiplied in beforehand
//   shadow:        the light's shadow here, 0..1 (NVR's sun or lamp shadow)
//   shadowEdge:    fwidth(shadow), taken at the pixel shader's top level (0 for a light whose shadow is applied outside)
//   selfLit:       the sun's visibility a hand's breadth further toward it (1 for lamps): says whether a shadow at the
//                  terminator is the surface's own (lit out there) or cast by something else (dark there too)
float3 SkinLight(float3 albedo, float3 N, float3 Nsoft, float geometricNdotL, float3 V, float3 L, float3 light, float shadow, float shadowEdge, float selfLit, out float3 specular) {
    L = normalize(L);

    // Diffusion, split into the plain N.L and what the scattering carries past the terminator.
    // 2026-10-27: "plain" is each colour's own N.L (red along the blurred normal), so the wrap is what the curvature adds
    // alone. It was measured against the sharp bumped normal, which made the blurred red normal's lead over every pore and
    // dirt bump of the normal map count as wrap, and on the far side (below) that escaped the shadow: red mottling in the
    // shaded half of a face. In the light (no escape) the sum is the same either way. LegacyFaceShadows: the old split.
    float3 plain;
    const float3 scattered = SkinDiffusion(N, Nsoft, L, plain);
    const float3 direct = min(scattered, SKIN_LEGACY ? saturate(dot(N, L)).xxx : plain);
    const float3 wrapped = scattered - direct;

    // The shadow map marks the surface's own far side as shadowed, which would cut the wrap off at the geometric
    // terminator. On that far side only (geometric N.L under 0.05) the WRAPPED part escapes the shadow, when the look
    // further toward the light (selfLit) found it lit: a shadow that is the surface's own. Skin facing the light inside
    // another surface's shadow (a neck under the jaw) stays shadowed, and so does the terminator of skin that lies whole
    // in something else's shadow (hands under a roof): selfLit is 0 there. Off with ShadowScatter 0.
    // 2026-10-27: and only near the terminator, as far as light carried under the skin can reach: full to geometric N.L
    // -0.15, gone by -0.45 (1-3 cm round a head). It used to escape over the whole far side, so any wrap there (a misread
    // curvature, see SkinSetup) showed as red patches deep in the shade. LegacyFaceShadows: the whole far side.
    float selfShadowed = saturate((0.05f - geometricNdotL) / 0.15f);
    selfShadowed = selfShadowed * selfShadowed * (3.0f - 2.0f * selfShadowed) * saturate(TESR_SkinLook.z) * saturate(selfLit);
    selfShadowed *= SKIN_LEGACY ? 1.0f : saturate((geometricNdotL + 0.45f) * 3.333f);
    // 2026-10-27 (user: "still get weird red lines w/ the new skin shader under heavy shadow"): where the surface turns edge-on to the
    // light (geometric N.L under 0.25) a shadow map breaks into thin stripes (a lookup grazing the surface, the map's texels stretched
    // across it), and each stripe's edges took the red of a penumbra: red lines over a face turned from the sun. The light is fading
    // there anyway (N.L): those edges stay grey (the plain shadow); the red stays on the edges over skin that faces the light.
    const float3 shadowColor = lerp(saturate(shadow).xxx, SkinShadow(shadow, shadowEdge), SKIN_LEGACY ? 1.0f : saturate(geometricNdotL * 4.0f));
    float3 diffuse = albedo * (direct * shadowColor + wrapped * lerp(shadowColor, 1.0f, selfShadowed));
    // The light the highlight reflects does not also enter the skin (as PBR.hlsl's diffuse: 1 - F at L.H).
    diffuse *= 1.0f - Fresnel(SKIN_F0.xxx, (1.0f).xxx, saturate(dot(L, normalize(V + L)))).x;

    // Light through thin features (high curvature) where it reaches their far side (geometric N.L below 0.3), seen looking
    // into it (Barre-Brisebois: the light bent by the surface toward the eye). The colour is the transmittance of the
    // feature's thickness: 0.25 mm at the thinnest rim, up to 1 mm where it fades out (thinness 0.25).
    const float thin = smoothstep(0.3f, 0.9f, skinThinness);
    const float farSide = saturate(0.3f - geometricNdotL);
    const float3 transmitDir = normalize(L + Nsoft * SKIN_TRANSMIT_DISTORTION);
    const float back = pow(saturate(dot(V, -transmitDir)), SKIN_TRANSMIT_POWER);
    const float3 tint = SkinLinearColor(SkinTransmittance(0.25f / max(skinThinness, 0.25f)) * 1.5f);
    const float3 transmitted = albedo * tint * (back * thin * farSide * TESR_SkinMaterial.w * shadow);

    specular = SkinSpecular(N, V, L, light);
    [branch] if (TESR_SkinLook.w > 0.0f)
        specular += SkinSheen(N, V, L, light);
    specular *= shadow;
    return (diffuse + transmitted) * light;
}

// --- sky -------------------------------------------------------------------------------------------------------------

// The sky along a world direction, GAMMA-ENCODED as SkyAmbient.hlsl gives it, blurred by the roughness: the sharp sky
// (SkyAmbientSharp, the harmonics read as radiance) for a smooth lobe, the cosine-convolved sky for a rough one. Below the
// horizon the harmonics hold no sky: the convolved value stands in, as SkyAmbientSharpened does for metal.
float3 SkinSkyRadiance(float3 r, float roughness) {
#if SKYLIGHTING_MODE == 1
    // No harmonics in this mode: one read of the sky, kept above the horizon, dimmed to the ground below it.
    const float3 sky = sqrt(max(GetSkyRadiance(normalize(float3(r.xy, max(r.z, 0.02f)))), 0.0f));
    return sky * lerp(1.0f, 0.4f, saturate(-r.z));
#else
    const float3 plain = SkyAmbientRadiance(r, TESR_PBRExtraData.z);
    float3 sharp = SkyAmbientSharp(r);
    sharp = lerp(sharp, max(sharp, plain), saturate(-r.z));
    return lerp(sharp, plain, saturate(roughness));
#endif
}

// The sky reflected off both lobes (the split-sum response at skin's F0, PBR.hlsl EnvBRDFApprox), faded where the
// reflection points into the surface (horizon occlusion). Sets skinSkyWeight: call it BEFORE SkinAmbient. WORLD-space
// vectors; geometricNormal is the vertex normal, N the bumped one.
float3 SkinSkyReflection(float3 V, float3 geometricNormal, float3 N) {
    const float3 r = reflect(-V, N);
    const float NdotV = saturate(dot(N, V));
    float horizon = saturate(1.0f + 1.2f * dot(r, geometricNormal));
    horizon *= horizon;

    const float r0 = clamp(skinRoughness * SKIN_LOBE0, 0.04f, 1.0f);
    const float r1 = clamp(skinRoughness * SKIN_LOBE1, 0.04f, 1.0f);
    const float response = lerp(EnvBRDFApprox(SKIN_F0.xxx, r0, NdotV), EnvBRDFApprox(SKIN_F0.xxx, r1, NdotV), SKIN_LOBE_MIX).x;
    const float weight = response * horizon * skinSpecScale * TESR_SkinLook.x;
    skinSkyWeight = saturate(weight);

    const float3 radiance = lerp(SkinSkyRadiance(r, r0), SkinSkyRadiance(r, r1), SKIN_LOBE_MIX);
    return LinColor(radiance) * (LinScale(TESR_PBRExtraData.y) * weight);
}

// The weather ambient plus the sky light (PBRScale.hlsl SkyAmbient), the sky read along the bumped and the blurred normal
// and blended per colour as the direct light is; less the share the reflection took. WORLD-space normals.
float3 SkinAmbient(float3 ambient, float3 albedo, float3 N, float3 Nsoft) {
    const float3 flat = PBRAmbient(ambient);
    const float3 sharp = SkyAmbient(N, 1.0f);
    const float3 soft = SkyAmbient(Nsoft, 1.0f);
    const float3 sky = float3(soft.r, lerp(sharp.g, soft.g, 0.6f), lerp(sharp.b, soft.b, 0.3f));
    return (flat + sky) * albedo * (1.0f - skinSkyWeight);
}

#endif
