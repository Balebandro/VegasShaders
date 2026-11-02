// AmbientOcclusion, with Skylighting: screen-space darkening of creases, under ledges and at the base of props, computed from
// the linear depth (CombineDepth) and the view-space normals (Normals), applied to the frame before the rest of the chain.
// Skylighting (outdoors, off by default) takes away the sky's share of the light where something overhead blocks the sky.
//
// Techniques (the C++ asks for them by name; technique 0 is the legacy single pass):
//   (unnamed)            legacy: one pass, the random-sample estimate and the combine, read from TESR_RenderedBuffer
//   DedicatedAO          Estimate (random samples), BlurX, BlurY (ping-pong AO 0 / AO 1), Combine (into the frame)
//   DedicatedAOFast      as DedicatedAO, the far pixels return early (the same picture, less work)
//   DedicatedGTAO        as DedicatedAO, the horizon-based (GTAO) estimate
//   DedicatedGTAOFast    as DedicatedGTAO, the far pixels return early
//   SkyVisibilityV2      Skylighting v2 sky pass: R = sky factor, G = depth, B = sky visibility, A = 1
//
// AO target: G16R16F, R = AO (1 open), G = linear depth in game units capped at 8000.

#include "includes/RenderRect.hlsl"
#include "includes/Helpers.hlsl"

static const float PI_AO = 3.14159265f;
static const float FadeStart = 2000.0f;	// the AO fades to 1 between these distances (game units)
static const float FadeEnd = 8000.0f;

uniform float4x4 TESR_ProjectionTransform;
uniform float4x4 TESR_InvProjectionTransform;
uniform float4x4 TESR_InvViewTransform;
uniform float4x4 TESR_ShadowCameraToLightTransformOrtho;
uniform float4 TESR_AmbientOcclusionAOData;		// x samples (unused), y StrengthMultiplier, z ClampStrength, w Range
uniform float4 TESR_AmbientOcclusionData;		// x AngleBias, y LumThreshold, z BlurDropThreshold, w BlurRadiusMultiplier
uniform float4 TESR_AmbientOcclusionGTAO;		// x 1 = GTAO on, w MaxRadius in pixels
uniform float4 TESR_AmbientOcclusionSky;		// Skylighting v1: x on, y Strength, z Distance, w depth slack
uniform float4 TESR_AmbientOcclusionSky2;		// Skylighting v2: x on, y Strength, z A (sky light), w S (sun light)
uniform float4 TESR_AmbientOcclusionSky3;		// x DepthNormals, y 1 / OpenThreshold, z ShareApply, w debug view
uniform float4 TESR_AmbientOcclusionSky4;		// x ray reach (Distance), y SoftWidth, z sun mask bound, w sky factor bound
uniform float4 TESR_AmbientOcclusionSky5;		// x LightCap take limit, y OpenSides, z unused, w no-limit value
uniform float4 TESR_SmoothedSunDir;				// xyz: the sun direction (world, unit)
uniform float4 TESR_ShadowFade;					// x: the cascade fade of the sun
uniform float4 TESR_ReciprocalResolution;		// xy: 1 / the full frame's size
uniform float4 TESR_FogData;					// x start, y end
uniform float4 TESR_FogColor;					// rgb
uniform float4 TESR_CameraData;					// x near, y far
uniform float4 NVR_AOLayout;					// zw: 1 / the AO target's size
uniform float4 NVR_SkySmooth;					// x: 1 when NVR_SkyPrep holds a smoothed normal this frame

uniform sampler2D TESR_RenderedBuffer : register(s0) = sampler_state {
    ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
uniform sampler2D TESR_DepthBuffer : register(s1) = sampler_state {
    ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
uniform sampler2D TESR_SourceBuffer : register(s2) = sampler_state {
    ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
uniform sampler2D TESR_BlueNoiseSampler : register(s3) < string ResourceName = "Effects\bluenoise256.dds"; > = sampler_state {
    ADDRESSU = WRAP; ADDRESSV = WRAP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
uniform sampler2D TESR_NormalsBuffer : register(s4) = sampler_state {
    ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
uniform sampler2D NVR_AOBuffer : register(s5) = sampler_state {
    ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
uniform sampler2D NVR_OrthoMap : register(s6) = sampler_state {
    ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
uniform sampler2D NVR_SkyVis : register(s7) = sampler_state {
    ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
uniform sampler2D NVR_SunMask : register(s8) = sampler_state {
    ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
uniform sampler2D NVR_SkyPrep : register(s9) = sampler_state {
    ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };

struct VSOUT { float4 vertPos : POSITION; float2 UVCoord : TEXCOORD0; };

struct VSIN { float4 vertPos : POSITION0; float2 UVCoord : TEXCOORD0; };

VSOUT FrameVS(VSIN px)
{
    VSOUT res = (VSOUT) 0.0f;
    res.vertPos = px.vertPos;
    res.UVCoord = px.UVCoord;
    return (res);
}

// ---- Geometry ----

// The linear depth (game units) at a rect uv.
float ViewZAt(float2 uv)
{
    return max(tex2Dlod(TESR_DepthBuffer, float4(uv, 0.0f, 0.0f)).x * TESR_CameraData.y, 1.0e-6f);
}

// The view-space position of a rect uv at a linear depth.
float3 ViewPosAt(float2 uv, float view_z)
{
    float2 tmp_s01 = rectToScreen(uv);
    float near_z = TESR_CameraData.x;
    float far_z = TESR_CameraData.y;
    float projected_v = near_z * (far_z - view_z) / (view_z * (far_z - near_z));
    float4 view_v = mul(float4(tmp_s01.x * 2.0f - 1.0f, (1.0f - tmp_s01.y) * 2.0f - 1.0f, projected_v, 1.0f), TESR_InvProjectionTransform);
    return view_v.xyz / view_v.w;
}

// The view-space normal at a rect uv (the normals buffer, decoded), unit length.
float3 NormalAt(float2 uv)
{
    float3 n = tex2Dlod(TESR_NormalsBuffer, float4(uv, 0.0f, 0.0f)).xyz * 2.0f - 1.0f;
    float len_v = length(n);
    return len_v > 1.0e-5f ? n / len_v : float3(0.0f, 0.0f, -1.0f);
}

// The rect uv of a view-space point.
float2 ProjectToRect(float3 p)
{
    float4 clip_v = mul(float4(p, 1.0f), TESR_ProjectionTransform);
    float2 ndc_v = clip_v.xy / clip_v.w;
    return screenToRect(float2(ndc_v.x * 0.5f + 0.5f, 0.5f - 0.5f * ndc_v.y));
}

// The full-frame pixel of a rect uv.
float2 PixelOf(float2 uv)
{
    return rectToScreen(uv) / TESR_ReciprocalResolution.xy;
}

// The normal from the depth alone (Skylighting's DepthNormals): on each axis the neighbour with the smaller depth step, oriented toward the camera.
float3 DepthNormalAt(float2 uv, float view_z)
{
    float2 t = TESR_ReciprocalResolution.xy;
    float2 lo_v = 0.5f * t, hi_v = TESR_RenderScale.xy - 0.5f * t;
    float2 uv_l = clamp(uv - float2(t.x, 0.0f), lo_v, hi_v), uvR = clamp(uv + float2(t.x, 0.0f), lo_v, hi_v);
    float2 uv_u = clamp(uv - float2(0.0f, t.y), lo_v, hi_v), uvD = clamp(uv + float2(0.0f, t.y), lo_v, hi_v);
    float z_l = ViewZAt(uv_l), z_r = ViewZAt(uvR), zU = ViewZAt(uv_u), zD = ViewZAt(uvD);
    float3 P = ViewPosAt(uv, view_z);
    float3 p_l = ViewPosAt(uv_l, z_l), pR = ViewPosAt(uvR, z_r), pU = ViewPosAt(uv_u, zU), pD = ViewPosAt(uvD, zD);
    float3 dx_v = abs(z_r - view_z) < abs(view_z - z_l) ? pR - P : P - p_l;
    float3 dy_v = abs(zU - view_z) < abs(view_z - zD) ? pU - P : P - pD;
    float3 n = cross(dy_v, dx_v);
    float len_v = length(n);
    n = len_v > 1.0e-12f ? n / len_v : float3(0.0f, 0.0f, -1.0f);
    return n.z > 0.0f ? -n : n;
}

// Blue-noise values: one 3-vector per sample index, in -1..1 (the offset per index decorrelates the samples).
float3 NoiseVector(float2 pixel_v, float index_v)
{
    float2 p = pixel_v + float2(index_v * 37.0f, index_v * 71.0f) + 0.5f;
    return tex2Dlod(TESR_BlueNoiseSampler, float4(p / 256.0f, 0.0f, 0.0f)).rgb * 2.0f - 1.0f;
}

// One blue-noise value in 0..1 for the pixel.
float NoiseScalar(float2 pixel_v)
{
    return tex2Dlod(TESR_BlueNoiseSampler, float4((pixel_v + 0.5f) / 256.0f, 0.0f, 0.0f)).r;
}

// ---- Ambient occlusion estimates ----

// One sample of the random-sample estimate: 1 weighted by the falloff when the stored surface is nearer than the sample
// point and within Range, else 0. The first quarter of the samples falls off linearly, the rest with its square.
float SampleOcclusion(float3 s, float ao_range, int index_v)
{
    float2 uv_s = ProjectToRect(s);
    if (outsideRect(uv_s)) return 0.0f;
    float d = s.z - ViewZAt(uv_s);
    if (d <= 0.0f || d >= ao_range) return 0.0f;
    float t = d / ao_range;
    return index_v < 2 ? 1.0f - t : (1.0f - t) * (1.0f - t);
}

// The random-sample estimate: five samples per axis from the blue noise, the horizontal one in the (x, z) plane and the vertical
// one in the (y, z) plane (the same random values), each turned by the pixel's quarter-turn angle and flipped to the normal's side.
// The two one-dimensional estimates multiply.
float RandomVisibility(float2 uv, float3 P, float3 N)
{
    float ao_range = max(abs(TESR_AmbientOcclusionAOData.w), 1.0e-3f);
    float strength_v = TESR_AmbientOcclusionAOData.y;
    float angle_bias = saturate(TESR_AmbientOcclusionData.x);
    float2 pixel_v = PixelOf(uv);
    float rotation_v = NoiseScalar(pixel_v) * (PI_AO * 0.5f);
    float c = cos(rotation_v), s = sin(rotation_v);

    float occ_h = 0.0f, occ_v = 0.0f;
    [loop]
    for (int i = 0; i < 5; i++)
    {
        float3 r = NoiseVector(pixel_v, (float)i);
        float3 d_h = float3(c * r.x - s * r.z, 0.0f, s * r.x + c * r.z);
        float3 d_v = float3(0.0f, c * r.y - s * r.z, s * r.y + c * r.z);
        d_h = dot(d_h, N) < 0.0f ? -d_h : d_h;
        d_v = dot(d_v, N) < 0.0f ? -d_v : d_v;
        float scl = lerp(angle_bias, 1.0f, pow((float)(i + 1) / 5.0f, 2.0f));
        occ_h += SampleOcclusion(P + normalize(d_h + 1.0e-6f) * scl * ao_range, ao_range, i);
        occ_v += SampleOcclusion(P + normalize(d_v + 1.0e-6f) * scl * ao_range, ao_range, i);
    }
    float ao_h = 1.0f - strength_v * occ_h / 5.0f;
    float ao_v = 1.0f - strength_v * occ_v / 5.0f;
    return saturate(ao_h) * saturate(ao_v);
}

// The horizon-based (GTAO) estimate: two slices across the half turn (the slice angle from the blue noise), four horizon steps
// each way out to a radius in pixels (Range, capped at MaxRadius). The cosine-weighted visibility between the horizons is the AO.
// Samples past 0.385 of the radius count less, none past it.
float GTAOVisibility(float2 uv, float3 P, float3 N, float view_z)
{
    float max_radius = clamp(TESR_AmbientOcclusionGTAO.w, 4.0f, 256.0f);
    float ao_range = max(abs(TESR_AmbientOcclusionAOData.w), 1.0e-3f);
    float strength_v = TESR_AmbientOcclusionAOData.y;
    float2 t = TESR_ReciprocalResolution.xy;
    float pixels_per_unit = 0.5f / t.y * TESR_ProjectionTransform[1][1] / view_z;
    float radius_px = clamp(ao_range * pixels_per_unit, 1.0f, max_radius);

    float2 pixel_v = PixelOf(uv);
    float noise_v = NoiseScalar(pixel_v);
    float3 V = normalize(-P);
    float sum_vis = 0.0f;
    [loop]
    for (int slice_v = 0; slice_v < 2; slice_v++)
    {
        float phi_v = ((float)slice_v + noise_v) * PI_AO * 0.5f;
        float2 dir_s = float2(cos(phi_v), sin(phi_v));			// the slice's direction on the screen (pixels)
        float3 slice_dir = float3(dir_s.x, -dir_s.y, 0.0f);	// the same direction in view space
        float3 slice_n = cross(slice_dir, V);
        float slice_len = length(slice_n);
        slice_n = slice_len > 1.0e-6f ? slice_n / slice_len : float3(0.0f, 0.0f, 1.0f);

        // The normal projected into the slice, and the angle gamma from the view direction to it (positive toward sliceDir).
        float3 n_p = N - slice_n * dot(N, slice_n);
        float n_len = length(n_p);
        float3 n_pn = n_len > 1.0e-5f ? n_p / n_len : V;
        float cos_gamma = clamp(dot(n_pn, V), -1.0f, 1.0f);
        float gamma_v = dot(n_pn, slice_dir) >= 0.0f ? acos(cos_gamma) : -acos(cos_gamma);

        float cos_h1 = -1.0f, cos_h2 = -1.0f;	// the largest cosines to the view direction on each side of the slice
        [loop]
        for (int k = 1; k <= 4; k++)
        {
            float t_k = (float)k / 4.0f;
            float weight_v = t_k <= 0.385f ? 1.0f : saturate((1.0f - t_k) / (1.0f - 0.385f));
            float2 offset = dir_s * t_k * radius_px * t;
            [unroll]
            for (int side_v = 0; side_v < 2; side_v++)
            {
                float2 uv_s = side_v == 0 ? uv + offset : uv - offset;
                if (outsideRect(uv_s)) continue;
                float z_s = ViewZAt(uv_s);
                float3 v = ViewPosAt(uv_s, z_s) - P;
                float len_v = length(v);
                if (len_v < 1.0e-5f) continue;
                float cos_s = lerp(-1.0f, dot(v, V) / len_v, weight_v);
                if (side_v == 0) cos_h2 = max(cos_h2, cos_s);
                else cos_h1 = max(cos_h1, cos_s);
            }
        }

        // Horizon angles from the view direction, limited to the normal's hemisphere (gamma +- 90 degrees).
        float tmp_h1 = -acos(clamp(cos_h1, -1.0f, 1.0f));
        float tmp_h2 = acos(clamp(cos_h2, -1.0f, 1.0f));
        tmp_h1 = gamma_v + max(tmp_h1 - gamma_v, -PI_AO * 0.5f);
        tmp_h2 = gamma_v + min(tmp_h2 - gamma_v, PI_AO * 0.5f);
        float a_slice = 0.25f * ((-cos(2.0f * tmp_h1 - gamma_v) + cos(gamma_v) + 2.0f * tmp_h1 * sin(gamma_v)) +
                                (-cos(2.0f * tmp_h2 - gamma_v) + cos(gamma_v) + 2.0f * tmp_h2 * sin(gamma_v)));
        sum_vis += a_slice * n_len;
    }
    float visibility_v = saturate(sum_vis / 2.0f);
    return saturate(1.0f - strength_v * (1.0f - visibility_v));
}

// ---- Skylighting: the top-down map ----

// The ortho map's openness at a world point: 1 where the map's top lies below the point, 0 where the point is under the map's
// top; soft over SoftWidth game units. Outside the map: open.
float Openness(float3 W)
{
    float4 m = mul(float4(W, 1.0f), TESR_ShadowCameraToLightTransformOrtho);
    float2 uv_m = m.xy * 0.5f + 0.5f;
    if (any(uv_m < 0.0f) || any(uv_m > 1.0f)) return 1.0f;
    float top_v = tex2Dlod(NVR_OrthoMap, float4(uv_m, 0.0f, 0.0f)).r;
    float3 z_row = float3(TESR_ShadowCameraToLightTransformOrtho[0][2], TESR_ShadowCameraToLightTransformOrtho[1][2], TESR_ShadowCameraToLightTransformOrtho[2][2]);
    float z_scale = max(length(z_row), 1.0e-6f);
    float soft_v = max(TESR_AmbientOcclusionSky4.y, 1.0e-3f) * z_scale;
    return saturate((m.z - top_v) / soft_v + 0.5f);
}

// A ray from a world point is open when both its probes (at the given fraction of the reach, and at the full reach) are open.
float RayOpen(float3 W, float3 dir, float reach_v, float near_fraction)
{
    return min(Openness(W + dir * reach_v * near_fraction), Openness(W + dir * reach_v));
}

// Skylighting v1: the visibility of the sky from the ortho map (one straight up, six rays on two rings, each probed at a third
// and at the full distance), counted by the cosine to the normal. Lerped with the strength.
float SkyV1Visibility(float3 P, float3 N, float2 uv)
{
    float3 W = mul(float4(P, 1.0f), TESR_InvViewTransform).xyz;
    float3 NW = normalize(mul(float4(N, 0.0f), TESR_InvViewTransform).xyz);
    float reach_v = TESR_AmbientOcclusionSky.z;
    float rotation_v = NoiseScalar(PixelOf(uv)) * 2.0f * PI_AO;

    float total_f = max(NW.z, 0.0f);
    float open_v = total_f * RayOpen(W, float3(0.0f, 0.0f, 1.0f), reach_v, 1.0f / 3.0f);
    [loop]
    for (int k = 0; k < 6; k++)
    {
        float elevation_v = k < 3 ? 60.0f : 30.0f;
        float azimuth_v = rotation_v + (float)(k % 3) * 2.0943951f;
        float ce_v = cos(elevation_v * PI_AO / 180.0f), se_v = sin(elevation_v * PI_AO / 180.0f);
        float3 dir = float3(ce_v * cos(azimuth_v), ce_v * sin(azimuth_v), se_v);
        float w = max(dot(dir, NW), 0.0f);
        total_f += w;
        open_v += w * RayOpen(W, dir, reach_v, 1.0f / 3.0f);
    }
    float visibility_v = total_f > 1.0e-6f ? open_v / total_f : 1.0f;
    return lerp(1.0f, visibility_v, TESR_AmbientOcclusionSky.y);
}

// ---- Estimate ----

// The estimate (AO in R, depth in G): the chosen estimate, Skylighting v1 multiplied in, fog lightening, and the fade to 1
// between FadeStart and FadeEnd. Pixels beyond FadeEnd return 1 (with the fast path, without any work).
float2 EstimateCore(float2 uv, bool gtao_v, bool fast_path)
{
    float view_z = ViewZAt(uv);
    if (fast_path && view_z >= FadeEnd) return float2(1.0f, FadeEnd);

    float3 N = NormalAt(uv);
    float3 P = ViewPosAt(uv, view_z);
    float ao_v = gtao_v ? GTAOVisibility(uv, P, N, view_z) : RandomVisibility(uv, P, N);
    if (TESR_AmbientOcclusionSky.x > 0.5f) ao_v *= SkyV1Visibility(P, N, uv);

    // Fog: the AO is pulled toward the fog colour's luminance, but never darkened by it.
    float fog_span = max(TESR_FogData.y - TESR_FogData.x, 1.0e-3f);
    float fog_amount = saturate((view_z - TESR_FogData.x) / fog_span);
    float fog_luma = dot(TESR_FogColor.rgb, float3(0.2126f, 0.7152f, 0.0722f));
    ao_v = max(ao_v, lerp(ao_v, fog_luma, fog_amount));

    ao_v = lerp(ao_v, 1.0f, saturate((view_z - FadeStart) / (FadeEnd - FadeStart)));
    return float2(saturate(ao_v), min(view_z, FadeEnd));
}

float4 EstimatePS(VSOUT px) : COLOR
{
    return float4(EstimateCore(px.UVCoord, false, false), 0.0f, 1.0f);
}

float4 EstimateFastPS(VSOUT px) : COLOR
{
    return float4(EstimateCore(px.UVCoord, false, true), 0.0f, 1.0f);
}

float4 EstimateGTAOPS(VSOUT px) : COLOR
{
    return float4(EstimateCore(px.UVCoord, true, false), 0.0f, 1.0f);
}

float4 EstimateGTAOFastPS(VSOUT px) : COLOR
{
    return float4(EstimateCore(px.UVCoord, true, true), 0.0f, 1.0f);
}

// ---- Blur ----

// A twelve-tap separable blur along the axis (six taps each side, Gaussian weights), the step in pixels being
// BlurRadiusMultiplier x Range / the pixel's distance (so the screen radius stays about constant), each tap kept only when its
// depth is within BlurDropThreshold x (1.5 + n_z). Pixels beyond FadeEnd return their input.
float4 BlurAlong(float2 uv, float2 axis_v)
{
    float4 centre_v = tex2Dlod(NVR_AOBuffer, float4(uv, 0.0f, 0.0f));
    if (centre_v.g >= FadeEnd) return centre_v;

    float3 N = NormalAt(uv);
    float tolerance_v = TESR_AmbientOcclusionData.z * (1.5f + N.z);
    float step_px = min(TESR_AmbientOcclusionData.w * TESR_AmbientOcclusionAOData.w / max(centre_v.g, 1.0e-3f), 8.0f);

    float sum_ao = centre_v.r;
    float sum_w = 1.0f;
    [loop]
    for (int k = 1; k <= 6; k++)
    {
        float w = exp(-(float)(k * k) / 18.0f);
        float2 offset = axis_v * (float)k * step_px * TESR_ReciprocalResolution.xy;
        float4 plus_v = tex2Dlod(NVR_AOBuffer, float4(uv + offset, 0.0f, 0.0f));
        float4 minus_v = tex2Dlod(NVR_AOBuffer, float4(uv - offset, 0.0f, 0.0f));
        if (abs(plus_v.g - centre_v.g) <= tolerance_v) { sum_ao += plus_v.r * w; sum_w += w; }
        if (abs(minus_v.g - centre_v.g) <= tolerance_v) { sum_ao += minus_v.r * w; sum_w += w; }
    }
    return float4(sum_ao / max(sum_w, 1.0e-6f), centre_v.g, 0.0f, 0.0f);
}

float4 BlurXPS(VSOUT px) : COLOR
{
    return BlurAlong(px.UVCoord, float2(1.0f, 0.0f));
}

float4 BlurYPS(VSOUT px) : COLOR
{
    return BlurAlong(px.UVCoord, float2(0.0f, 1.0f));
}

// ---- Combine ----

float3 LinearLumaWeights() { return float3(0.2126f, 0.7152f, 0.0722f); }

// The AO (and the sky factor and visibility, when bound) of a frame pixel, from the AO grid's 2 x 2 neighbours: each weighted by its
// bilinear weight over (1 + the depth difference / BlurDropThreshold), so the AO does not cross a silhouette.
void UpsampleGrid(float2 uv, float view_z, out float ao_v, out float sky_rgb, out float visibility_v)
{
    float2 aoSize = 1.0f / NVR_AOLayout.zw;
    float2 pos = uv * aoSize - 0.5f;
    float2 baseTexel = floor(pos);
    float2 f = pos - baseTexel;
    bool sky_bound = TESR_AmbientOcclusionSky4.w > 0.5f;
    float sum_w = 0.0f;
    ao_v = 0.0f; sky_rgb = 0.0f; visibility_v = 0.0f;
    [unroll]
    for (int j = 0; j < 4; j++)
    {
        float dx_v = j == 1 || j == 3 ? 1.0f : 0.0f;
        float dy_v = j >= 2 ? 1.0f : 0.0f;
        float bilinear = (dx_v > 0.5f ? f.x : 1.0f - f.x) * (dy_v > 0.5f ? f.y : 1.0f - f.y);
        float2 tuv = (baseTexel + float2(dx_v, dy_v) + 0.5f) / aoSize;
        float4 a = tex2Dlod(NVR_AOBuffer, float4(tuv, 0.0f, 0.0f));
        float w = bilinear / (1.0f + abs(a.g - view_z) / max(TESR_AmbientOcclusionData.z, 0.001f));
        ao_v += a.r * w;
        sum_w += w;
        if (sky_bound)
        {
            float4 sk = tex2Dlod(NVR_SkyVis, float4(tuv, 0.0f, 0.0f));
            sky_rgb += sk.r * w;
            visibility_v += sk.b * w;
        }
    }
    float inv = 1.0f / max(sum_w, 1.0e-6f);
    ao_v *= inv; sky_rgb *= inv; visibility_v *= inv;
}

// The darkened scene: the AO clamped to ClampStrength, reduced on bright surfaces (LumThreshold), and the sky's share taken
// (Skylighting v2, LightCap-limited), applied to the linear colour.
float4 CompositeScene(float4 scene_sample, float ao_v, float sky_rgb, float visibility_v, bool sky_bound)
{
    ao_v = lerp(TESR_AmbientOcclusionAOData.z, 1.0f, ao_v);
    float3 linear_colour = linearize(scene_sample.rgb);
    float lum_v = dot(linear_colour, LinearLumaWeights());
    ao_v = lerp(ao_v, 1.0f, saturate((lum_v - TESR_AmbientOcclusionData.y) * 3.0f));

    float take_v = 0.0f;
    if (sky_bound)
    {
        float cap_v = TESR_AmbientOcclusionSky5.x;
        float no_limit = TESR_AmbientOcclusionSky5.w;
        take_v = saturate(min(1.0f - sky_rgb, ((1.0f - visibility_v) * cap_v + no_limit) / max(lum_v, 1.0e-4f)));
    }
    return float4(delinearize(linear_colour * ao_v * (1.0f - take_v)), 1.0f);
}

float4 CombinePS(VSOUT px) : COLOR
{
    float4 scene_sample = tex2D(TESR_SourceBuffer, px.UVCoord);
    float view_z = ViewZAt(px.UVCoord);
    if (view_z >= FadeEnd) return scene_sample;
    float ao_v, sky_rgb, visibility_v;
    UpsampleGrid(px.UVCoord, view_z, ao_v, sky_rgb, visibility_v);
    return CompositeScene(scene_sample, ao_v, sky_rgb, visibility_v, TESR_AmbientOcclusionSky4.w > 0.5f);
}

// The legacy path: one pass from the frame, the random-sample AO at full size, the same combine without Skylighting.
float4 LegacyPS(VSOUT px) : COLOR
{
    float4 scene_sample = tex2D(TESR_RenderedBuffer, px.UVCoord);
    float view_z = ViewZAt(px.UVCoord);
    if (view_z >= FadeEnd) return scene_sample;
    float ao_v = EstimateCore(px.UVCoord, false, false).x;
    return CompositeScene(scene_sample, ao_v, 0.0f, 1.0f, false);
}

// ---- Skylighting v2: the sky pass ----

float4 SkyPS(VSOUT px) : COLOR
{
    float2 uv = px.UVCoord;
    float view_z = ViewZAt(uv);
    if (view_z >= FadeEnd) return float4(1.0f, FadeEnd, 1.0f, 1.0f);

    float3 N = TESR_AmbientOcclusionSky3.x > 0.5f ? DepthNormalAt(uv, view_z) : NormalAt(uv);
    if (NVR_SkySmooth.x > 0.5f)
    {
        float3 smoothed_v = tex2Dlod(NVR_SkyPrep, float4(uv, 0.0f, 0.0f)).xyz * 2.0f - 1.0f;
        float len_v = length(smoothed_v);
        if (len_v > 1.0e-5f) N = smoothed_v / len_v;
    }

    float3 P = ViewPosAt(uv, view_z);
    float3 W = mul(float4(P, 1.0f), TESR_InvViewTransform).xyz;
    float3 NW = normalize(mul(float4(N, 0.0f), TESR_InvViewTransform).xyz);
    float3 wp = W + NW * 24.0f;		// off the surface's own texels of the map
    float reach_v = TESR_AmbientOcclusionSky4.x;
    float3 up_vec = float3(0.0f, 0.0f, 1.0f);

    // Covered: something blocks a probe 32 units above the point.
    float covered_v = 1.0f - Openness(wp + up_vec * 32.0f);
    float open_sides = TESR_AmbientOcclusionSky5.y;

    float total_f = 0.0f, open_v = 0.0f;
    float w_up = 0.034f * max(NW.z, 0.0f);
    total_f += w_up;
    open_v += w_up * RayOpen(wp, up_vec, reach_v, 0.4f);

    [loop]
    for (int k = 0; k < 8; k++)
    {
        float azimuth_v = (22.5f + 45.0f * (float)k) * PI_AO / 180.0f;
        float2 h_v = float2(cos(azimuth_v), sin(azimuth_v));

        float3 tmp_d60 = float3(h_v * 0.5f, 0.8660254f);
        float tmp_w60 = 0.0324f * max(dot(tmp_d60, NW), 0.0f);
        total_f += tmp_w60;
        open_v += tmp_w60 * RayOpen(wp, tmp_d60, reach_v, 0.4f);

        float3 tmp_d30 = float3(h_v * 0.8660254f, 0.5f);
        float tmp_w30 = 0.0884f * max(dot(tmp_d30, NW), 0.0f);
        float open30_v = RayOpen(wp, tmp_d30, reach_v, 0.4f);
        float open_horizontal = Openness(wp + float3(h_v, 0.0f) * reach_v);
        open30_v = lerp(open30_v, open_horizontal, open_sides * covered_v);
        total_f += tmp_w30;
        open_v += tmp_w30 * open30_v;
    }

    float visibility_v = total_f > 1.0e-6f ? open_v / total_f : 1.0f;

    // The map's outer 20 percent fades to open, so the border never shows as a ring.
    float4 m = mul(float4(wp, 1.0f), TESR_ShadowCameraToLightTransformOrtho);
    float edge_v = saturate((max(abs(m.x), abs(m.y)) - 0.8f) / 0.2f);
    visibility_v = lerp(visibility_v, 1.0f, edge_v);
    visibility_v = saturate(visibility_v * TESR_AmbientOcclusionSky3.y);	// / OpenThreshold

    // The sky's share of the light (ShareApply): the sky light against the sun's, the sun shadowed where the mask says so.
    float share_v = 1.0f;
    if (TESR_AmbientOcclusionSky3.z > 0.5f)
    {
        float sun_mask = 1.0f;
        if (TESR_AmbientOcclusionSky4.z > 0.5f)
            sun_mask = lerp(TESR_ShadowFade.x, 1.0f, tex2Dlod(NVR_SunMask, float4(uv, 0.0f, 0.0f)).r);
        float sky_light = TESR_AmbientOcclusionSky2.z;
        float sun_light = TESR_AmbientOcclusionSky2.w;
        float sun_e = sun_light * saturate(dot(NW, TESR_SmoothedSunDir.xyz)) * sun_mask;
        share_v = saturate(sky_light / max(sky_light + sun_e, 1.0e-6f));
    }

    float factor_v = saturate(1.0f - TESR_AmbientOcclusionSky2.y * (1.0f - visibility_v) * share_v);
    int debug_view = (int)TESR_AmbientOcclusionSky3.w;
    if (debug_view == 1) factor_v = visibility_v;
    else if (debug_view == 2) factor_v = share_v;
    else if (debug_view == 4) factor_v = 1.0f - covered_v;
    return float4(factor_v, min(view_z, FadeEnd), visibility_v, 1.0f);
}

// ---- Techniques ----

technique
{
    pass Legacy
    {
        VertexShader = compile vs_3_0 FrameVS( );
        PixelShader = compile ps_3_0 LegacyPS( );
    }
}

technique DedicatedAO
{
    pass Estimate
    {
        VertexShader = compile vs_3_0 FrameVS( );
        PixelShader = compile ps_3_0 EstimatePS( );
    }
    pass BlurX
    {
        VertexShader = compile vs_3_0 FrameVS( );
        PixelShader = compile ps_3_0 BlurXPS( );
    }
    pass BlurY
    {
        VertexShader = compile vs_3_0 FrameVS( );
        PixelShader = compile ps_3_0 BlurYPS( );
    }
    pass Combine
    {
        VertexShader = compile vs_3_0 FrameVS( );
        PixelShader = compile ps_3_0 CombinePS( );
    }
}

technique DedicatedAOFast
{
    pass Estimate
    {
        VertexShader = compile vs_3_0 FrameVS( );
        PixelShader = compile ps_3_0 EstimateFastPS( );
    }
    pass BlurX
    {
        VertexShader = compile vs_3_0 FrameVS( );
        PixelShader = compile ps_3_0 BlurXPS( );
    }
    pass BlurY
    {
        VertexShader = compile vs_3_0 FrameVS( );
        PixelShader = compile ps_3_0 BlurYPS( );
    }
    pass Combine
    {
        VertexShader = compile vs_3_0 FrameVS( );
        PixelShader = compile ps_3_0 CombinePS( );
    }
}

technique DedicatedGTAO
{
    pass Estimate
    {
        VertexShader = compile vs_3_0 FrameVS( );
        PixelShader = compile ps_3_0 EstimateGTAOPS( );
    }
    pass BlurX
    {
        VertexShader = compile vs_3_0 FrameVS( );
        PixelShader = compile ps_3_0 BlurXPS( );
    }
    pass BlurY
    {
        VertexShader = compile vs_3_0 FrameVS( );
        PixelShader = compile ps_3_0 BlurYPS( );
    }
    pass Combine
    {
        VertexShader = compile vs_3_0 FrameVS( );
        PixelShader = compile ps_3_0 CombinePS( );
    }
}

technique DedicatedGTAOFast
{
    pass Estimate
    {
        VertexShader = compile vs_3_0 FrameVS( );
        PixelShader = compile ps_3_0 EstimateGTAOFastPS( );
    }
    pass BlurX
    {
        VertexShader = compile vs_3_0 FrameVS( );
        PixelShader = compile ps_3_0 BlurXPS( );
    }
    pass BlurY
    {
        VertexShader = compile vs_3_0 FrameVS( );
        PixelShader = compile ps_3_0 BlurYPS( );
    }
    pass Combine
    {
        VertexShader = compile vs_3_0 FrameVS( );
        PixelShader = compile ps_3_0 CombinePS( );
    }
}

technique SkyVisibilityV2
{
    pass Sky
    {
        VertexShader = compile vs_3_0 FrameVS( );
        PixelShader = compile ps_3_0 SkyPS( );
    }
}
