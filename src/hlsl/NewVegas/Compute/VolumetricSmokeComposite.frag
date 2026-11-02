// VolumetricSmoke technique VolumetricSmoke (0; step 2: light the smoke and lay it over the frame) as a Vulkan pass (needs NVR DXVK):
// Effects/VolumetricSmoke.fx.hlsl CompositePS, CutAt and HazeAt line for line. effects/VolumetricSmoke.cpp ExtRender (through the
// frame chain: ExtFullScreen for the whole screen, else the smoke's rectangle with ExtDrawTo).
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "Common.glsl"

layout(set = 1, binding = 0) uniform texture2D t_rendered;   // TESR_RenderedBuffer (POINT, CLAMP)
layout(set = 1, binding = 1) uniform texture2D t_depth;      // TESR_DepthBuffer (POINT, CLAMP)
layout(set = 1, binding = 2) uniform texture2D t_smoke;      // TESR_SmokeBuffer (LINEAR, CLAMP)
layout(set = 1, binding = 3) uniform texture2D t_viewModel;  // TESR_DepthBufferViewModel (POINT, CLAMP)
layout(set = 1, binding = 4) uniform texture2D t_low;        // TESR_SmokeBufferLow (LINEAR, CLAMP)
layout(set = 1, binding = 5) uniform texture2D t_haze;       // TESR_SmokeHaze (LINEAR, CLAMP)
layout(set = 1, binding = 6) uniform texture2D t_env;        // TESR_SmokeEnv (POINT, CLAMP)
layout(set = 1, binding = 7) uniform texture2D t_cut;        // TESR_SmokeCut (POINT, CLAMP)

#define VS_DEPTH
#define VS_SMOKE
#include "VolumetricSmoke.glsl"

layout(location = 0) out vec4 o_color;

// VolumetricSmoke.fx.hlsl CutAt: the bullets' cut of the segment smoke at a screen pixel, from the quarter-resolution buffer.
float CutAt(vec2 suv, float surface) {
    vec2 hres = ceil(1.0 / (4.0 * TESR_ReciprocalResolution.xy));
    vec2 p = suv * hres - 0.5;
    vec2 f = fract(p);
    vec2 at = (floor(p) + 0.5) / hres, stepSize = 1.0 / hres;
    vec4 c00 = POINT_LOD(t_cut, at);
    vec4 c10 = POINT_LOD(t_cut, at + vec2(stepSize.x, 0.0));
    vec4 c01 = POINT_LOD(t_cut, at + vec2(0.0, stepSize.y));
    vec4 c11 = POINT_LOD(t_cut, at + stepSize);
    vec4 w = vec4((1.0 - f.x) * (1.0 - f.y), f.x * (1.0 - f.y), (1.0 - f.x) * f.y, f.x * f.y);
    vec4 off = abs(vec4(c00.a, c10.a, c01.a, c11.a) - surface) / (2.0 + 0.04 * surface);
    w *= 1.0 / (1.0 + off * off * off) + 0.0001;
    return dot(vec4(c00.x, c10.x, c01.x, c11.x), w) / dot(w, vec4(1.0));
}

// VolumetricSmoke.fx.hlsl HazeAt: the haze at a screen pixel, from the quarter-resolution buffer.
vec4 HazeAt(vec2 suv, float surface) {
    vec2 hres = ceil(1.0 / (4.0 * TESR_ReciprocalResolution.xy));
    vec2 p = suv * hres - 0.5;
    vec2 f = fract(p);
    vec2 at = (floor(p) + 0.5) / hres, stepSize = 1.0 / hres;
    vec4 h00 = LINEAR_LOD(t_haze, at);
    vec4 h10 = LINEAR_LOD(t_haze, at + vec2(stepSize.x, 0.0));
    vec4 h01 = LINEAR_LOD(t_haze, at + vec2(0.0, stepSize.y));
    vec4 h11 = LINEAR_LOD(t_haze, at + stepSize);
    vec4 w = vec4((1.0 - f.x) * (1.0 - f.y), f.x * (1.0 - f.y), (1.0 - f.x) * f.y, f.x * f.y);
    vec4 off = abs(vec4(h00.a, h10.a, h01.a, h11.a) - surface) / (2.0 + 0.04 * surface);
    w *= 1.0 / (1.0 + off * off * off) + 0.0001;
    return (h00 * w.x + h10 * w.y + h01 * w.z + h11 * w.w) / dot(w, vec4(1.0));
}

vec4 CompositePS() {
    // The frame quad's uv and VPOS, at this pixel's centre (the pass may cover only a rectangle of the screen).
    vec2 vpos = floor(gl_FragCoord.xy);
    vec2 uv = gl_FragCoord.xy * TESR_ReciprocalResolution.xy;
    vec4 color = POINT_LOD(t_rendered, uv);
    // The smoke buffers are read at this pixel's exact centre, so the full-resolution one is not blurred by the smooth
    // sampling the half- and quarter-resolution ones need.
    vec2 suv = (vpos + 0.5) * TESR_ReciprocalResolution.xy;
    vec4 smokeBuffer = SmokeAt(suv);
    vec4 haze = LINEAR_LOD(t_haze, suv);                     // the gun-smoke haze (HazePS), added in below
    float tau = smokeBuffer.r + haze.r;
    if (tau < 0.0005) return color;
    // Your own gun (the first-person model) is always in front of the smoke, so smoke never shows inside it. Not in the debug view.
    if (SmokeInfo.w > 0.5 && SmokeInfo.y < 0.5 && POINT_LOD(t_viewModel, uv).x > 0.0) return color;
    float transmittance = exp(-tau);
    if (SmokeInfo.y > 0.5) {
        color.rgb = mix(color.rgb, vec3(1.0, 0.05, 0.05), saturate(1.0 - exp(-4.0 * tau)));
        return color;
    }
    // Behind this pixel's own surface (the gun, a wall's edge): hidden, softly (see HazeAt, CutAt).
    float viewDepth;
    vec3 world = reconstructWorldPosition(uv, viewDepth).xyz;
    vec3 camera = (TESR_InvViewTransform * vec4(0.0, 0.0, 0.0, 1.0)).xyz;
    float surface = length(world - camera);
    haze = HazeAt(suv, surface);                             // (sharper than the smooth read above, which only decided to start)
    // (The haze is already cut at the nearest surface of its blocks, so this is for the smoke only.)
    float smokeDist = smokeBuffer.a / max(smokeBuffer.r, 0.000001);
    // Bullets cut the segment smoke too: the smoke is taken as spread along the ray round its mean distance, and each bullet's
    // hole is worked out for this pixel's own ray (BulletHole, as in HazePS). Only where there is smoke and bullets.
    if (smokeBuffer.r >= 0.0005 && HazeInfo.y > 0.5) {
        // The cut, worked out at a quarter of the resolution by the haze pass (SegmentCut), read as the haze is (CutAt).
        float cut = CutAt(suv, surface);
        // While you move, your gun's own puff and trail round you stream past the tunnels, which stay where the bullets were fired:
        // so then the smoke within 60 units of you is left alone, the cut coming back fully by 150 units. Standing still: unchanged.
        cut *= 1.0 - saturate(BulletMove.x) * (1.0 - smoothstep(60.0, 150.0, smokeDist));
        // The edge softens by how see-through the smoke looks, not by how much of it goes.
        if (cut > 0.0) {                                     // (no hole: untouched, not even by rounding)
            float seen = mix(exp(-smokeBuffer.r), 1.0, cut);
            smokeBuffer *= saturate(-log(max(seen, 0.000001)) / max(smokeBuffer.r, 0.000001));
        }
    }
    if (SmokeLook.w > 0.5) smokeBuffer *= saturate((surface - smokeDist) / max(4.0, 0.08 * smokeDist) + 0.5);   // only with low-resolution smoke
    float tauSmoke = smokeBuffer.r, tauHaze = haze.r;
    tau = tauSmoke + tauHaze;
    if (tau < 0.0005) return color;
    transmittance = exp(-tau);
    const float age = 0.35;                                  // a mid-life tint (the buffer's alpha holds distance now)
    // Ambient light: the light of the surroundings (EnvPS, worked out once a frame).
    vec3 env = POINT_LOD(t_env, vec2(0.5, 0.5)).rgb;
    vec3 dir = normalize(world - camera);
    // Direct light, scattered mostly forward (Henyey-Greenstein, g 0.6, 1 straight toward the light).
    float cosView = dot(dir, LightDirection());
    float forward = 0.064 / pow(1.36 - 1.2 * cosView, 1.5);
    float outdoors = SmokeInfo.z;
    // Outdoors the sun is the main light (the sky's ambient a smaller part), so the lit and shaded sides differ clearly.
    vec3 ambient = mix(env, 0.72 * TESR_SunAmbient.rgb, 0.5 * outdoors) * mix(0.75, 0.8, outdoors);
    vec3 direct = outdoors > 0.5 ? 0.5 * TESR_SunColor.rgb * (0.4 + 1.1 * forward) : 0.45 * env;   // indoors: soft, from above
    // The smoke and the haze are lit each by itself, then mixed by how much of each there is.
    float deepSmoke = 1.0 - 0.5 * SmokeParams.z * (1.0 - exp(-tauSmoke));
    float deepHaze = 1.0 - 0.5 * SmokeParams.z * (1.0 - exp(-tauHaze));
    vec3 smokeLight = ambient * deepSmoke + direct * saturate(smokeBuffer.g / max(tauSmoke, 0.000001)) +
        SmokeFlashColor.rgb * (smokeBuffer.b / max(tauSmoke, 0.000001));
    vec3 hazeLight = ambient * deepHaze + direct * saturate(haze.g / max(tauHaze, 0.000001)) +
        SmokeFlashColor.rgb * (haze.b / max(tauHaze, 0.000001));
    // Relief: the smoke's thickness read like a height map a couple of pixels to each side, lit from the light's direction on screen.
    if (SmokeLook.y > 0.001 && smokeBuffer.r >= 0.0005) {
        vec2 px = TESR_ReciprocalResolution.xy * 2.0;
        float hL = 1.0 - exp(-SmokeAtLod(suv - vec2(px.x, 0.0)).r);
        float hR = 1.0 - exp(-SmokeAtLod(suv + vec2(px.x, 0.0)).r);
        float hU = 1.0 - exp(-SmokeAtLod(suv - vec2(0.0, px.y)).r);
        float hD = 1.0 - exp(-SmokeAtLod(suv + vec2(0.0, px.y)).r);
        vec3 bump = normalize(vec3(hL - hR, hD - hU, 0.3));                              // screen: x right, y up, z toward you
        vec3 lightView = LightDirection() * mat3(TESR_InvViewTransform);                  // x right, y up, z forward (HLSL mul(M3, v))
        vec3 lightScreen = normalize(vec3(lightView.x, lightView.y, 0.3 - lightView.z));
        float relief = (dot(bump, lightScreen) * 0.5 + 0.5) / max(lightScreen.z * 0.5 + 0.5, 0.2);
        smokeLight *= mix(1.0, clamp(relief, 0.8, 1.25), SmokeLook.y);                   // the smoke's edges only, not the haze behind it
    }
    vec3 smoke = (smokeLight * tauSmoke + hazeLight * tauHaze) / tau * SmokeParams.y;
    // Colour: fresh smoke is whiter and a little brighter, greying as it ages; thin smoke scatters blue a little more; light coming
    // through the smoke loses a little blue.
    float tint = SmokeLook.x;
    float thin = exp(-1.5 * tau);
    smoke *= mix(vec3(1.0), vec3(0.86, 0.93, 1.06), thin * tint) * mix(1.0, 1.12 - 0.24 * age, tint);
    vec3 through = exp(-tau * mix(vec3(1.0), vec3(0.93, 1.0, 1.08), tint));
    if (SmokeInfo.x < -0.5) { smoke = vec3(0.0, 1.0, 0.0); through = vec3(exp(-tau)); }   // test mode: the smoke in plain green
    color.rgb = color.rgb * through + smoke * (1.0 - through);
    return color;
}

void main() {
    o_color = CompositePS();
}
