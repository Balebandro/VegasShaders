// GT7-style auto-exposure and auto white balance: [Shaders.Exposure.Main] Mode 1 (shared by AvgLuma.fx and Exposure.fx,
// their technique 1 "GT7"; Mode 0 keeps the classic exposure, technique 0, unchanged).
//
// Follows the design Polyphony Digital presented in "Driving Toward Reality: Physically Based Tone Mapping and
// Perceptual Fidelity in Gran Turismo 7" (SIGGRAPH 2025, slides 188-222). The slides give the structure and ranges
// but not the tuned numbers, so every value marked ESTIMATE below is a choice made here, not Polyphony's.
//
// From the slides (followed as given):
//  - Two adaptation stages. Short-term (pupil and neural gain): 3-5 EV, settles in a few seconds.
//    Long-term (receptor sensitivity): wide range, settles over tens of minutes.
//  - Long-term adaptation limits both the short-term range and the whole adaptation range, so a sunny day
//    stays "mildly glaring" and the night stays dark. Light adaptation is faster than dark adaptation.
//  - Scene luminance from the frame buffer averaged with outlier rejection (like the sun), exponential smoothing.
//  - One parameter set for the whole game; no per-scene exposure overrides.
//  - Auto white balance uses the same mechanism on colour temperature and tint, adapting gradually toward D65;
//    the scene colour comes from several references blended as luminance-weighted Yxy, a faint virtual light keeps
//    the night reading stable, the result is blended with the ideal light, and the range follows real camera AWB.
//
// Differences: there are no light probes here; the references are the frame buffer, the weather's sun, sky and ambient
// colours, the rendered sky dome and the shadow and height maps (full field, below). The 1x1 state buffer has room for one
// white-balance value, so colour temperature is smoothed over time while the green-magenta tint is taken from the
// (already smooth) sun and sky references each frame.
//
// Any setup: the scene's brightness scale before the tone mapper differs between setups (Sunlight Dimmer, weather mods,
// lighting settings), and every user calibrates it once with the Tonemapping Exposure, which puts daylight at the right
// brightness on the display. So the light adaptation limit is defined after that Exposure (display-referred, as GT7's own
// limits are in real luminance) and converted back to scene units with the Exposure that is active (GT7LightLimit).
//
// Full field: the eye adapts to the light reaching it from its whole field of view, weighted by the cosine to the line
// of sight (illuminance at the eye); the screen is only the central part. The rest of the field is estimated around the
// camera from the references GT7 lists (light probe, sun, sky dome, sky visibility): the rendered sky dome (its
// spherical-harmonic projection), whether the sky is open in each direction (the top-down height map Skylighting uses),
// the ground lit or shaded as the camera is (sun shadow map at the camera), and the lit and shaded surface levels
// measured on screen. All in the scene's own units, so nothing needs calibrating. Falls back to the screen plus the sky
// on screen where those maps are missing (interiors, sun shadows off).
//
// Settings (Exposure.cpp, TESR_GT7AdaptData, per Main / Night / Interiors and blended like the other Exposure values):
// GT7AutoExposure and GT7WhiteBalance (0-1 strengths), GT7WeatherLight (0-1: how much the weather's sun, sky and ambient
// colours count in the white balance against the screen's own colour).
//
// State in TESR_AvgLumaBuffer (1x1, RGBA16F): r = short-term adaptation (log2 luminance), g = long-term adaptation,
// b = depth-of-field focus (as the classic technique writes it), a = white-balance colour temperature in mired (below
// 50 = not yet initialised; the C++ clears the texel when Mode changes).

#ifndef GT7_ADAPTATION_HLSL
#define GT7_ADAPTATION_HLSL

// ---- Auto-exposure (all values in EV = log2 of linear luminance, measured before the Tonemapping Exposure) ----
#define GT7AE_LIGHT_LIMIT_DISPLAY (-1.68) // ESTIMATE: light adaptation limit, in EV after the Tonemapping Exposure. Brighter scenes get no
                                         // further correction ("mildly glaring"). -1.68 = -3.0 + log2(2.5): sunny daylight set to look right
                                         // with Exposure 2.5 sits just above it (it meters about -1.0 after the Exposure).
#define GT7AE_LONG_RANGE        3.0    // ESTIMATE: long-term range below the light limit (GT7: 12 EV of real-world light; scaled to the much smaller day-night range of the game's lighting).
#define GT7AE_SHORT_DARK        2.0    // ESTIMATE: short-term reach below the long-term state (GT7: 3-5 EV in total).
#define GT7AE_SHORT_LIGHT       2.0    // ESTIMATE: short-term reach above the long-term state.
#define GT7AE_TAU_SHORT_LIGHT   0.4    // ESTIMATE: seconds, short-term, adapting to brighter ("a few seconds"; light faster than dark).
#define GT7AE_TAU_SHORT_DARK    1.5    // ESTIMATE: seconds, short-term, adapting to darker.
#define GT7AE_TAU_LONG_LIGHT   90.0    // ESTIMATE: seconds, long-term, adapting to brighter.
#define GT7AE_TAU_LONG_DARK   600.0    // ESTIMATE: seconds, long-term, adapting to darker ("tens of minutes").
#define GT7AE_REJECT_DARK       3.0    // ESTIMATE: outlier rejection, EV below the mean.
#define GT7AE_REJECT_LIGHT      1.5    // ESTIMATE: outlier rejection, EV above the mean (the sun, bright sky).

// ---- Sky-dome reference for the exposure (GT7 slides 210-211: the screen alone is not enough; GT7 blends the
// sky dome's luminance in with fixed weights, scaled by sky visibility). Here the sky dome is measured on the screen
// itself (pixels at the far plane), so it is in the same units, and its weight follows how much sky is in view. ----
#define GT7AE_SKY_WEIGHT        0.5    // ESTIMATE: weight of the sky reference at full visibility.
#define GT7AE_SKY_OFFSET        1.1    // MEASURED on an open-sky day: the sky dome meters about 1.1 EV above the scene there. A difference,
                                       // so the Exposure cancels out; where the sky is brighter next to the sunlit ground, it holds shade back more.
#define GT7AE_SKY_FULL          0.15   // ESTIMATE: share of the screen that is sky at which the sky counts fully (less sky, less weight).
#define GT7AE_SKY_MAX           3.5    // ESTIMATE: sky samples more than this many EV above the light limit are clamped: keeps the sun disc out.
#define GT7AE_SKY_DEPTH         0.9961 // depth (fraction of the far plane) from which a pixel is sky, as the fog uses.

// ---- Settings (Exposure.cpp) ----
float4 TESR_GT7AdaptData;   // x: GT7AutoExposure, y: GT7WhiteBalance, z: GT7WeatherLight (all 0-1), w: Mode (1)

float GT7AEStrength() { return saturate(TESR_GT7AdaptData.x); }
float GT7WBStrength() { return saturate(TESR_GT7AdaptData.y); }
// Weight of the weather's sun/sky/ambient in the white balance: 0 = the screen only (indoors), 1 = full weight (outdoors).
float GT7EnvWeight() { return saturate(TESR_GT7AdaptData.z); }

// Light adaptation limit in scene units (EV before the Tonemapping Exposure) for the Exposure that is active now
// (TESR_HDRData.y; Main / Night / Interiors as they are blended). Needs float4 TESR_HDRData declared by the effect.
float GT7LightLimit()
{
	return GT7AE_LIGHT_LIMIT_DISPLAY - log2(TESR_HDRData.y > 0.01 ? TESR_HDRData.y : 1.0);
}

// ---- Full-field metering. GT7AE_SKY_WEIGHT/OFFSET/FULL above are then used only as the fallback
// (interiors, exterior sun shadows switched off). ----
#define GT7FF_ENABLED           1      // 0 = fall back to the screen-plus-sky metering everywhere.
#define GT7FF_DIRECTIONS       32      // directions sampled over the field outside the screen (cosine-distributed, equal weights).
#define GT7FF_RAY_NEAR        400.0    // game units (70 per metre): points along each ray tested against the height map
#define GT7FF_RAY_MID        1200.0    //   for open sky (about 6, 17 and 36 m). ESTIMATE: what counts as "overhead cover".
#define GT7FF_RAY_FAR        2500.0
#define GT7FF_OPEN_BIAS        48.0    // game units of tolerance in the height-map test (as Skylighting uses a small one).
#define GT7FF_CAM_REACH        48.0    // the camera's sunlight is tested this far around it (clear of the player's body)
#define GT7FF_CAM_DROP         24.0    //   and this far below it (chest height).
#define GT7FF_TRUST            10.0    // screen samples of lit or shaded surfaces needed to trust their measured level fully.
#define GT7FF_MAX_SUN_RATIO     5.0    // EV: sunlit/shaded ratio cap for the estimate used when one of them is not on screen.
#define GT7FF_ALBEDO            0.2    // ESTIMATE: average surface reflectance, used only when the screen shows nothing but sky.

// ---- Auto white balance ----
#define GT7WB_W_FRAME           0.25   // ESTIMATE: weight of the frame buffer colour.
#define GT7WB_W_SUN             0.45   // ESTIMATE: weight of the sunlight (times its height above the horizon).
#define GT7WB_W_SKY             0.20   // ESTIMATE: weight of the sky dome (weather upper sky colour).
#define GT7WB_W_AMBIENT         0.10   // ESTIMATE: weight of the ambient light (stands in for GT7's light probes).
#define GT7WB_VIRTUAL_Y         0.02   // ESTIMATE: luminance of the faint D65 virtual light that steadies the night reading.
#define GT7WB_IDEAL_BLEND       0.30   // ESTIMATE: how much the measured light is blended with the ideal (D65) light.
#define GT7WB_MIN_K          2500.0    // ESTIMATE: camera AWB range, low end (kelvin).
#define GT7WB_MAX_K         10000.0    // ESTIMATE: camera AWB range, high end (kelvin).
#define GT7WB_MAX_DUV           0.005  // ESTIMATE: largest green-magenta tint corrected (kept small so sunsets keep their colour).
#define GT7WB_TAU               3.0    // ESTIMATE: seconds; same kind of smoothing as the auto-exposure.
#define GT7WB_LIGHT_GAMMA       2.2    // the weather colours are decoded as the linear lighting does.

static const float2 GT7_D65 = float2(0.31271, 0.32902);

static const float3x3 GT7_RGB2XYZ = {
    0.4124564, 0.3575761, 0.1804375,
    0.2126729, 0.7151522, 0.0721750,
    0.0193339, 0.1191920, 0.9503041 };
static const float3x3 GT7_XYZ2RGB = {
     3.2404542, -1.5371385, -0.4985314,
    -0.9692660,  1.8760108,  0.0415560,
     0.0556434, -0.2040259,  1.0572252 };
static const float3x3 GT7_BRADFORD = {
     0.8951,  0.2664, -0.1614,
    -0.7502,  1.7135,  0.0367,
     0.0389, -0.0685,  1.0296 };
static const float3x3 GT7_BRADFORD_INV = {
     0.9869929, -0.1470543, 0.1599627,
     0.4323053,  0.5183603, 0.0492912,
    -0.0085287,  0.0400428, 0.9684867 };

float GT7Luma(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }

// CIE 1960 uv of the Planckian locus (Krystek 1985), 1000-15000 K.
float2 GT7PlanckUV(float T) {
    float u = (0.860117757 + 1.54118254e-4 * T + 1.28641212e-7 * T * T) / (1.0 + 8.42420235e-4 * T + 7.08145163e-7 * T * T);
    float v = (0.317398726 + 4.22806245e-5 * T + 4.20481691e-8 * T * T) / (1.0 - 2.89741816e-5 * T + 1.61456053e-7 * T * T);
    return float2(u, v);
}
float2 GT7XYtoUV(float2 xy) { float d = -2.0 * xy.x + 12.0 * xy.y + 3.0; return float2(4.0 * xy.x, 6.0 * xy.y) / d; }
float2 GT7UVtoXY(float2 uv) { float d = 2.0 * uv.x - 8.0 * uv.y + 4.0; return float2(3.0 * uv.x, 2.0 * uv.y) / d; }

// Unit normal of the locus at T, pointing toward higher v (the green side): Duv is measured along it.
float2 GT7LocusNormal(float T) {
    float2 t = GT7PlanckUV(T * 1.01) - GT7PlanckUV(T * 0.99);
    float2 n = normalize(float2(-t.y, t.x));
    return n.y < 0.0 ? -n : n;
}

// Chromaticity -> colour temperature (McCamy) and Duv.
float GT7XYtoCCT(float2 xy) { float n = (xy.x - 0.3320) / (0.1858 - xy.y); return ((449.0 * n + 3525.0) * n + 6823.3) * n + 5520.33; }
float GT7XYtoDuv(float2 xy, float T) { return dot(GT7XYtoUV(xy) - GT7PlanckUV(T), GT7LocusNormal(T)); }
float2 GT7WhiteXY(float T, float duv) { return GT7UVtoXY(GT7PlanckUV(T) + duv * GT7LocusNormal(T)); }

float3 GT7DecodeLight(float3 c) { return pow(max(c, 0.0), GT7WB_LIGHT_GAMMA); }

// Luminance-weighted sum (as XYZ) of the weather references: sunlight, sky dome, ambient. envWeight: 1 outdoors, 0 indoors.
float3 GT7EnvironmentXYZ(float3 sunColor, float sunHeight, float3 skyColor, float3 ambientColor, float envWeight) {
    float3 sun = GT7DecodeLight(sunColor) * saturate(sunHeight);
    float3 xyz = mul(GT7_RGB2XYZ, sun) * GT7WB_W_SUN
               + mul(GT7_RGB2XYZ, GT7DecodeLight(skyColor)) * GT7WB_W_SKY
               + mul(GT7_RGB2XYZ, GT7DecodeLight(ambientColor)) * GT7WB_W_AMBIENT;
    return xyz * envWeight;
}

// Weighted XYZ sum -> chromaticity of the light, steadied by the virtual light and blended with the ideal light.
float2 GT7LightXY(float3 xyzSum) {
    float3 v = float3(GT7_D65.x / GT7_D65.y, 1.0, (1.0 - GT7_D65.x - GT7_D65.y) / GT7_D65.y) * GT7WB_VIRTUAL_Y;
    float3 t = xyzSum + v;
    float2 xy = t.xy / max(t.x + t.y + t.z, 1e-6);
    return lerp(xy, GT7_D65, GT7WB_IDEAL_BLEND);
}

// Linear sRGB -> linear sRGB matrix adapting light of chromaticity srcXY to D65 (Bradford), applied with strength 0-1.
float3 GT7WhiteBalance(float3 rgb, float2 srcXY, float strength) {
    srcXY = lerp(GT7_D65, srcXY, saturate(strength));
    float3 srcLMS = mul(GT7_BRADFORD, float3(srcXY.x / srcXY.y, 1.0, (1.0 - srcXY.x - srcXY.y) / srcXY.y));
    float3 dstLMS = mul(GT7_BRADFORD, float3(GT7_D65.x / GT7_D65.y, 1.0, (1.0 - GT7_D65.x - GT7_D65.y) / GT7_D65.y));
    float3 lms = mul(GT7_BRADFORD, mul(GT7_RGB2XYZ, rgb)) * (dstLMS / srcLMS);
    return mul(GT7_XYZ2RGB, mul(GT7_BRADFORD_INV, lms));
}

// Rounds to a value a 16-bit float holds exactly, up or down at random in proportion, so slow changes that are
// smaller than one step of the 16-bit buffer still add up over time instead of being lost.
float GT7StochasticHalf(float v, float rnd) {
    float ulp = exp2(floor(log2(max(abs(v), 6.104e-5))) - 10.0);
    return floor(v / ulp + rnd) * ulp;
}

float GT7Hash(float seed) { return frac(sin(seed * 12.9898 + 78.233) * 43758.5453); }

#endif
