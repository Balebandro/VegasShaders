// Underwater, shared part of the Vulkan passes (NVR DXVK). The same maths as Effects/Underwater.fx.hlsl, line for line.
// Each pass includes Common.glsl, declares its own textures and push block, then includes this file.
// Matrices: HLSL mul(v, M) is M * v here, and M[i][j] is the same element in both (the engine's matrices are copied as they are).

layout(set = 2, binding = 0, std140) uniform Constants {
    vec4 reciprocalResolution;   // 0    TESR_ReciprocalResolution
    vec4 sunDirection;           // 16   TESR_SunDirection
    vec4 gameTime;               // 32   TESR_GameTime
    vec4 waterCoefficients;      // 48   TESR_WaterCoefficients
    vec4 sunColor;               // 64   TESR_SunColor
    vec4 waterFog;               // 80   TESR_WaterFog
    vec4 waterVolume;            // 96   TESR_WaterVolume
    vec4 waveParams;             // 112  TESR_WaveParams
    vec4 waterSettings;          // 128  TESR_WaterSettings
    vec4 waterShallowColor;      // 144  TESR_WaterShallowColor
    vec4 waterDeepColor;         // 160  TESR_WaterDeepColor
    vec4 horizonColor;           // 176  TESR_HorizonColor
    vec4 skyColor;               // 192  TESR_SkyColor
    vec4 cameraPosition;         // 208  TESR_CameraPosition
    vec4 cameraData;             // 224  TESR_CameraData (y: far Z)
    mat4 projectionTransform;    // 240  TESR_ProjectionTransform
    mat4 viewTransform;          // 304  TESR_ViewTransform
} cb;

vec4 sampleLod(texture2D t, uint samplerIndex, vec2 uv) {
    return textureLod(sampler2D(t, NVR_SAMPLER(samplerIndex)), uv, 0.0);
}

// Depth.hlsl: readDepth (linear depth times far Z) and toWorld (the view ray, forward component 1).
float readDepthAt(texture2D depth, uint samplerIndex, vec2 uv) {
    return sampleLod(depth, samplerIndex, uv).x * cb.cameraData.y;
}

vec3 toWorld(vec2 uv) {
    vec3 v = vec3(cb.viewTransform[0][2], cb.viewTransform[1][2], cb.viewTransform[2][2]);
    v += (1.0 / cb.projectionTransform[0][0] * (2.0 * uv.x - 1.0)) * vec3(cb.viewTransform[0][0], cb.viewTransform[1][0], cb.viewTransform[2][0]);
    v += (-1.0 / cb.projectionTransform[1][1] * (2.0 * uv.y - 1.0)) * vec3(cb.viewTransform[0][1], cb.viewTransform[1][1], cb.viewTransform[2][1]);
    return v;
}

// Effects/Underwater.fx.hlsl: abovePixel
bool abovePixel(vec3 eye, vec3 P) {
    float W = cb.waterSettings.x;
    if (eye.z > W + 4.0) return true;
    if (eye.z < W - 4.0) return false;
    return P.z > W - 0.6;
}

// The ray image: the upper-left quarter of the picture.
bool inQuarter(vec2 uv) {
    return all(greaterThanEqual(uv, vec2(0.0))) && all(lessThanEqual(uv, vec2(0.5)));
}

vec3 sunDirection() {
    return normalize(cb.sunDirection.xyz);
}

// 4x4 ordered dither, values 0..15 (Bayer), as the HLSL.
float bayer2(float a, float b) {
    return (b < 0.5) ? a * 2.0 : 3.0 - 2.0 * a;
}

float bayer4(vec2 p) {
    vec2 q = mod(floor(p), 4.0);
    vec2 lo = mod(q, 2.0);
    vec2 hi = floor(q * 0.5);
    return 4.0 * bayer2(lo.x, lo.y) + bayer2(hi.x, hi.y);
}

float tapWeight(int i) {
    if (i == 1) return 0.111075;
    if (i == 2) return 0.100087;
    if (i == 3) return 0.084136;
    if (i == 4) return 0.065981;
    if (i == 5) return 0.048273;
    return 0.032948;
}
