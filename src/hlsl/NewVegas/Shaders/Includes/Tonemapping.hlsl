// Tonemapping operators for the HDR blend shaders (ISHDRBLENDINSHADERCIN*.pso). tonemap() picks one by TESR_HDRData.x; the
// curve parameters come from TESR_LotteData, TESR_ToneMapping and TESR_HDRBloomData.w (white point / peak).

// Narkowicz's fit of the ACES curve (https://knarkowicz.wordpress.com/2016/01/06/aces-filmic-tone-mapping-curve/), per channel.
float3 ACESFilm(float3 c)
{
    // rational fit: c (n2 c + n1) / (c (d2 c + d1) + d0)
    float n2 = 2.51f, n1 = 0.03f;
    float d2 = 2.43f, d1 = 0.59f, d0 = 0.14f;
    return (c * (n2 * c + n1)) / (c * (d2 * c + d1) + d0);
}

// -----------------------------------------------------------------------------------------------
// The code in this file was originally written by Stephen Hill (@self_shadow), who deserves all
// credit for coming up with this fit and implementing it. Buy him a beer next time you see him. :)

// sRGB => XYZ => D65_2_D60 => AP1 => RRT_SAT
static const float3x3 ACESInputMat =
{
    { 0.59719, 0.35458, 0.04823 },
    { 0.07600, 0.90834, 0.01566 },
    { 0.02840, 0.13383, 0.83777 }
};

// ODT_SAT => XYZ => D60_2_D65 => sRGB
static const float3x3 ACESOutputMat =
{
    { 1.60475, -0.53108, -0.07367 },
    { -0.10208, 1.10813, -0.00605 },
    { -0.00327, -0.07276, 1.07602 }
};

float3 RRTAndODTFit(float3 v)
{
    float3 a = v * (v + 0.0245786f) - 0.000090537f;
    float3 b = v * (0.983729f * v + 0.4329510f) + 0.238081f;
    return a / b;
}

// ACES Filmic (Fitted)
float3 ACESFitted(float3 color)
{
    color = mul(ACESInputMat, color);

    // Apply RRT and ODT
    color = RRTAndODTFit(color);

    color = mul(ACESOutputMat, color);

    // Clamp to [0, 1]
    color = saturate(color);

    return color;
}
// -----------------------------------------------------------------------------------------------

// Reinhard on luminance, blended toward per-channel Reinhard as the channels saturate (Jodie's variant).
float3 tmReinhardJodie(float3 c)
{
    float y = luma(c);
    float3 squeezed = c / (1.0f + c);
    return lerp(c / (1.0f + y), squeezed, squeezed);
}

// Multiplies c so its luma becomes target (same hue).
float3 tmRescaleLuma(float3 c, float target)
{
    float y = luma(c);
    // UNOFFICIAL 2026-10-16: black has y = target = 0 (the ratio was 0/0 = NaN); the ratio tends to 1 there.
    return c * (abs(y) > 1e-8 ? target / y : 1.0);
}

// Extended Reinhard on luminance: y (1 + y / w^2) / (1 + y), w = the white point (setting x 3, at least 1).
float3 tmReinhardExtended(float3 c, float whitePt)
{
    float w = max(1.0, whitePt * 3.0);
    float y = luma(c);
    float num = y * (1.0f + (y / (w * w)));
    return tmRescaleLuma(c, num / (1.0f + y));
}

// Hable's filmic curve (Uncharted 2).
float3 tmUnchartedCurve(float3 c)
{
    float shoulderStr = 0.15;
    float linearStr = 0.50;
    float linearAngle = 0.10;
    float toeStr = 0.20;
    float toeNum = 0.01;
    float toeDen = 0.30;
    return ((c * (shoulderStr * c + linearAngle * linearStr) + toeStr * toeNum) / (c * (shoulderStr * c + linearStr) + toeStr * toeDen)) - toeNum / toeDen;
}
float3 tmUncharted2(float3 c, float exposure, float whitePt)
{
    float gain = max(0.1, exposure * 1.4);
    float3 mapped = tmUnchartedCurve(c * gain);

    float w = max(1.0, whitePt * 3.0);
    float3 invWhite = float3(1.0f, 1.0f, 1.0f) / tmUnchartedCurve(w);
    return mapped * invWhite;
}

// AgX, after the minimal version at https://iolite-engine.com/blog_posts/minimal_agx_implementation
static const float3x3 agx_mat = {
    0.842479062253094,  0.0423282422610123, 0.0423756549057051,
    0.0784335999999992, 0.878468636469772,  0.0784336,
    0.0792237451477643, 0.0791661274605434, 0.879142973793104 };
static const float3x3 agx_mat_inv = {
     1.19687900512017,  -0.0528968517574562, -0.0529716355144438,
    -0.0980208811401368, 1.15190312990417,   -0.0980434501171241,
    -0.0990297440797205, -0.0989611768448433, 1.15107367264116 };

// Polynomial stand-in for the AgX sigmoid (mean squared error about 3.7e-6).
float3 agxDefaultContrastApprox(float3 t)
{
    float3 t2 = t * t;
    float3 t4 = t2 * t2;
    return 15.5 * t4 * t2 - 40.14 * t4 * t + 31.96 * t4 - 6.868 * t2 * t + 0.4298 * t2 + 0.1191 * t - 0.00232;
}

// Back out of the AgX space, then to linear with a 2.2 power (we output linear, not to an sRGB target).
float3 agxEotf(float3 c)
{
    // UNOFFICIAL 2026-10-18: row vector times matrix. The GLSL original fills its matrices column by column, so mul(M, v) was the
    // transpose and tinted everything pink. Fix by Conor (testbed).
    float3 outset = mul(c, agx_mat_inv);
    return pows(outset, 2.2);
}

// ASC CDL look with the neutral ("default") settings; golden would be slope (1, 0.9, 0.5), power 0.8, sat 0.8; punchy power 1.35,
// sat 1.4.
float3 agxLook(float3 c)
{
    float3 mono = luma(c);
    float3 lift = 0.0;
    float3 gain = 1.0;
    float gamma = 1.0;
    float chroma = 1.0;
    float3 graded = pows(c * gain + lift, gamma);
    return mono + chroma * (graded - mono);
}

float3 agx(float3 c)
{
    float evLo = -12.47393f;
    float evHi = 4.026069f;

    float3 inset = mul(c, agx_mat);	// UNOFFICIAL 2026-10-18: row vector, as agxEotf (fix by Conor)
    // log2 encoding over [evLo, evHi]. UNOFFICIAL 2026-10-16: negative input gave a NaN log; 1e-6 is under evLo anyway.
    float3 enc = clamp(log2(max(inset, 1e-6)), evLo, evHi);
    enc = (enc - evLo) / (evHi - evLo);
    return agxEotf(agxLook(agxDefaultContrastApprox(enc)));
}

// Lottes 2016 (https://gpuopen.com/wp-content/uploads/2016/03/GdcVdrLottes.pdf), via
// https://gist.github.com/KelSolaar/1213139203911a72fef531c32c3d4ec2 and vtastek's tuned version
// (https://gist.github.com/vtastek/935be12fb8d87adda751b5276fd88f0c), which is the default here.

#define CMAX 1.6e+6f	// largest value the curves take (keeps x^contrast finite)
#define EPS 1e-6f	// smallest peak (no 0/0)

// Tone-maps the brightest channel; the colour ratios get a crosstalk transform that desaturates toward white as it brightens.
float3 VTLottes(float3 c, float contrast, float b, float d, float shoulder, float crosstalk)
{
    float3 top = max(c.r, max(c.g, c.b));
    top = min(CMAX, max(EPS, top));
    float3 hue = min(CMAX, c / top);

    float weighted = dot(c, float3(0.5, 0.4, 0.33));
    float floorCh = max(0.0, min(c.r, min(c.g, c.b)));
    top += min(top, floorCh);
    top *= 0.5;
    top *= 1.0 + 1.666 * max(0, (top - weighted) / top);

    float3 zc = pows(top, contrast);
    top = zc / (pows(zc, shoulder) * b + d);

    float xtalk = max(1.0, crosstalk);
    float sat = contrast;
    float crossSat = contrast * (64.0 / xtalk);
    hue = pows(abs(hue + 0.11) * 0.90909, sat / crossSat);
    hue = lerp(hue, 1.0, pows(top, max(float3(1.0, 1.0, 1.0), float3(4.0, 3.0, 3.0) * xtalk) * 1.0 / top));
    hue = pows(min(1.0, hue), crossSat);
    return top * hue;
}

// Lottes curve x^a / ((x^a)^d * b + c): the b and c that put midIn on midOut and hdrMax on 1.
float ColToneB(float peakIn, float a, float d, float mIn, float mOut)
{
    return -((-pow(mIn, a) + (mOut * (pow(peakIn, a * d) * pow(mIn, a) - pow(peakIn, a) * pow(mIn, a * d) * mOut))
              / (pow(peakIn, a * d) * mOut - pow(mIn, a * d) * mOut))
             / (pow(mIn, a * d) * mOut));
}
float ColToneC(float peakIn, float a, float d, float mIn, float mOut)
{
    return (pow(peakIn, a * d) * pow(mIn, a) - pow(peakIn, a) * pow(mIn, a * d) * mOut)
         / (pow(peakIn, a * d) * mOut - pow(mIn, a * d) * mOut);
}
// q = (a, d, b, c)
float ColTone(float v, float4 q)
{
    float va = pow(v, q.x);
    return va / (pow(va, q.y) * q.z + q.w);
}

float3 Lottes(float3 c, float contrast, float midOut, float midIn, float hdrMax, float shoulder)
{
    float peakIn = max(1.0, hdrMax * 50.0);
    float a = max(0.01, contrast * 1.25);
    float d = saturate(shoulder * 0.993);	// must stay under 1
    float mIn = max(0.01, midIn * 0.18);
    float mOut = max(0.01, midOut * 0.1818);

    float b = ColToneB(peakIn, a, d, mIn, mOut);
    float k = ColToneC(peakIn, a, d, mIn, mOut);

    float3 top = max(c.r, max(c.g, c.b));
    top = min(CMAX, max(EPS, top));
    float3 hue = min(CMAX, c / top);

    float weighted = dot(c, float3(0.5, 0.4, 0.33));
    float floorCh = max(0.0, min(c.r, min(c.g, c.b)));
    top += min(top, floorCh);
    top *= 0.5;
    top *= 1.0 + 1.666 * max(0, (top - weighted) / top);

    top = ColTone(top.x, float4(a, d, b, k));	// (.x: what the implicit truncation passed; top is grey, x = y = z; ends warning X3206)

    float sat = a;
    float crossSat = a * 64.0;
    float3 one = 1.0;
    hue = pows(abs(hue + 0.11) * 0.90909, sat / crossSat);
    hue = lerp(hue, one, pow(abs(top), float3(4.0, 1.5, 1.5) * 1.0/top));	// (abs: free, top >= EPS here; ends warning X3571)
    hue = pows(min(1.0, hue), crossSat);
    return top * hue;
}

// Uchimura's GT curve for one channel: power toe, linear middle, exponential shoulder, joined with weights.
float uchimuraCurve(float v, float maxOut, float slope, float start, float len, float toePow, float pedestal)
{
    float lenOut = ((maxOut - start) * len) / slope;
    float linEndIn = start + lenOut;
    float linEndOut = start + slope * lenOut;
    float rate = (slope * maxOut) / (maxOut - linEndOut);

    float lin = start + slope * (v - start);
    float toe = start * pow(abs(v / start), toePow);	// (abs: free; was NaN below 0 and that part is faded out there; ends warning X3571)
    float sh = maxOut - (maxOut - linEndOut) * exp(-rate * (v - linEndIn) / maxOut);
    float toeW = 1 - smoothstep(0.0f, start, v);
    float shW = (v < start + lenOut) ? 0 : 1;
    float linW = 1 - toeW - shW;
    return (float) (toe * toeW + lin * linW + sh * shW);
}

float3 tmUchimura(float3 c, float contrast, float brightness, float midIn, float hdrMax, float shoulder)
{
    float maxOut = 1.0 * brightness;
    float slope = 1.0 * contrast;
    float start = 0.22 * midIn;
    float len = 0.4 * hdrMax;
    float toePow = 1.0 + (0.333 * shoulder);
    float pedestal = 0.0;
    return float3(uchimuraCurve(c.r, maxOut, slope, start, len, toePow, pedestal),
                  uchimuraCurve(c.g, maxOut, slope, start, len, toePow, pedestal),
                  uchimuraCurve(c.b, maxOut, slope, start, len, toePow, pedestal));
}

// 1 - e^(-x p): the DICE shoulder's approach to its maximum.
float tmRangePow(float x, float p)
{
    return 1.0 - pow(exp(-x), p);
}

float tmShoulder(float v, float top, float knee, float p)
{
    float rolled = knee + (top - knee) * tmRangePow((v - knee) / (top - knee), p);
    return v <= knee ? v : rolled;
}

// DICE: a shoulder on luminance only, from shoulder% of the output maximum upward.
float3 tmDice(float3 c, float contrast, float brightness, float hdrMax, float shoulder)
{
    float knee01 = shoulder * 0.01;
    float p = contrast;
    float nits = brightness;

    float y = luma(c);
    float top = hdrMax / nits;
    float knee = knee01 * top;
    float yOut = tmShoulder(y, top, knee, p);
    return c * (y != 0.0 ? yOut / y : 1.0);	// UNOFFICIAL 2026-10-22: black was 0/0 = NaN
}


float3 tonemap(float3 hdr)
{
    float mode = TESR_HDRData.x;
    // 0: leave it (for HDR output through SpecialK / DXVK: display mapping cannot happen here, this is not the last shader)
    if (mode == 0)
        return hdr;
    else if (mode == 1) {
        hdr = max(0.0, hdr);
        return max(0.0, VTLottes(min(CMAX, hdr), TESR_LotteData.x, TESR_LotteData.z, TESR_LotteData.y, TESR_LotteData.w, TESR_ToneMapping.x));
    }
    else if (mode == 2) {
        hdr = max(0.0, hdr);
        return max(0.0, Lottes(min(CMAX, hdr), TESR_LotteData.x, TESR_LotteData.y, TESR_LotteData.z, TESR_HDRBloomData.w, TESR_LotteData.w));
    }
    else if (mode == 3)
        return tmReinhardExtended(hdr, TESR_HDRBloomData.w);
    else if (mode == 4)
        return tmReinhardJodie(hdr);
    else if (mode == 5)
        return ACESFilm(hdr);
    else if (mode == 6)
        return ACESFitted(hdr);
    else if (mode == 7) {
        hdr = max(0.0, hdr);	// UNOFFICIAL 2026-10-16: like 1, 2 and 8 (the curve's denominator reaches 0 near -0.12)
        return tmUncharted2(hdr, TESR_LotteData.y, TESR_HDRBloomData.w);
    }
    else if (mode == 8) {
        hdr = max(0.0, hdr);
        return max(0.0, tmUchimura(min(CMAX, hdr), TESR_LotteData.x, TESR_LotteData.y, TESR_LotteData.z, TESR_HDRBloomData.w, TESR_LotteData.w));
    }
    else if (mode == 9)
        return agx(hdr);
    else if (mode == 10)
        return tmDice(min(CMAX, hdr), TESR_LotteData.x, TESR_LotteData.y, TESR_HDRBloomData.w, TESR_LotteData.w);
    else
        return hdr;
}
