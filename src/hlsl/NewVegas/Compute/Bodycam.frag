// Bodycam as a Vulkan pass (needs NVR DXVK): Effects/Bodycam.fx.hlsl technique 0 line for line. effects/Bodycam.cpp ExtRender.
// Five textures: t_rendered (LINEAR, CLAMP); t_text, t_hold, t_exposure, t_depth (POINT, CLAMP). The two sampler heap indices
// are at the end of the push block (linearIndex, pointIndex).
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "Common.glsl"

layout(set = 1, binding = 0) uniform texture2D t_rendered;   // TESR_RenderedBuffer
layout(set = 1, binding = 1) uniform texture2D t_text;       // TESR_BodycamTextBuffer
layout(set = 1, binding = 2) uniform texture2D t_hold;       // TESR_BodycamHoldBuffer
layout(set = 1, binding = 3) uniform texture2D t_exposure;   // TESR_BodycamExposureBuffer
layout(set = 1, binding = 4) uniform texture2D t_depth;      // TESR_DepthBuffer

layout(push_constant) uniform Push {
    vec4 recip;      // 0: TESR_ReciprocalResolution
    vec4 lensC;      // 16: TESR_BodycamLens
    vec4 imageC;     // 32: TESR_BodycamImage
    vec4 noiseC;     // 48: TESR_BodycamNoise
    vec4 textC;      // 64: TESR_BodycamText
    vec4 frameC;     // 80: TESR_BodycamFrame
    vec4 motionC;    // 96: TESR_BodycamMotion
    vec4 gradeC;     // 112: TESR_BodycamGrade
    vec4 videoC;     // 128: TESR_BodycamVideo
    vec4 adaptC;     // 144: TESR_BodycamAdapt
    vec4 bodyC;      // 160: TESR_BodycamBody
    vec4 flashC;     // 176: TESR_BodycamFlash
    vec4 glitchC;    // 192: TESR_BodycamGlitch
    uint linearIndex; // 208: LINEAR, CLAMP
    uint pointIndex;  // 212: POINT, CLAMP
} pc;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o_color;

const vec2 TextTextureSize = vec2(256.0, 12.0);	// Bodycam.h TextWidth, TextHeight

float Hash(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

// HLSL lerp, saturate and smoothstep carry over; smoothstep here is written with its edges in ascending order (GLSL leaves
// reversed edges undefined).
float ClipWhiteAmount(float m) { return clamp((m - 1.0) * 0.8, 0.0, 1.0); }

vec3 ClipToWhite(vec3 c) {
    c = max(c, 0.0);
    float m = max(c.r, max(c.g, c.b));
    return m <= 1.0 ? c : mix(c / m, vec3(1.0), ClipWhiteAmount(m));
}

float ValueNoise(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(Hash(i), Hash(i + vec2(1.0, 0.0)), f.x), mix(Hash(i + vec2(0.0, 1.0)), Hash(i + vec2(1.0, 1.0)), f.x), f.y);
}

// Smudges and specks on the glass (fixed to the screen), 0-1: a few soft blotches and some small round specks.
// (HLSL smoothstep(0.35, 0.1, d) is 1 - smoothstep(0.1, 0.35, d) with the edges ascending.)
float LensDirt(vec2 pos) {
    vec2 p = pos * vec2(pc.recip.z, 1.0);
    float blotch = smoothstep(0.55, 0.85, ValueNoise(p * 3.0 + 11.0) * 0.65 + ValueNoise(p * 7.0 + 3.0) * 0.35);
    vec2 cell = floor(p * 24.0);
    vec2 inCell = fract(p * 24.0) - 0.5;
    float speck = Hash(cell + 41.0) > 0.93 ? 1.0 - smoothstep(0.1, 0.35, length(inCell)) : 0.0;
    return clamp(blotch * 0.7 + speck * 0.6, 0.0, 1.0);
}

// Where the lens shows screen point pos from: centred, x in aspect units, pushed out by (1 + k r^2), zoomed back in.
// First the chest mount: the picture rolled and shifted (the bob, the gun's jolt), zoomed in so no edge shows.
vec2 Bend(vec2 pos, float scale) {
    float aspect = pc.recip.z;
    vec2 p = (pos * 2.0 - 1.0) * vec2(aspect, 1.0);
    float roll = pc.bodyC.z, rs = sin(roll), rc = cos(roll);
    p = vec2(p.x * rc - p.y * rs, p.x * rs + p.y * rc) / max(pc.bodyC.w, 1.0) + pc.bodyC.xy * 2.0 * vec2(aspect, 1.0);
    float r2 = dot(p, p);
    p *= (1.0 + pc.lensC.x * r2) * scale / pc.lensC.y;
    return p / vec2(aspect, 1.0) * 0.5 + 0.5;
}

vec3 Read(vec2 pos) {
    return textureLod(sampler2D(t_rendered, NVR_SAMPLER(pc.linearIndex)), pos, 0.0).rgb;
}

// While the view turns: seven reads along the way the picture moved this frame (the camera's shutter smearing it).
vec3 Smeared(vec2 pos, bool moving) {
    if (!moving) return Read(pos);
    vec3 c = vec3(0.0);
    for (int i = 0; i < 7; i++) c += Read(pos + pc.motionC.xy * (float(i) / 6.0 - 0.5));
    return c / 7.0;
}

vec4 Picture() {
    vec2 px = pc.recip.xy;
    // Between video frames (VideoFps) the last one shows again, as recorded video does.
    if (pc.videoC.w > 0.5) return textureLod(sampler2D(t_hold, NVR_SAMPLER(pc.pointIndex)), uv, 0.0);
    float scale1080 = 1.0 / (px.y * 1080.0);		// pixels at this resolution per pixel at 1080p

    // The compressed video: the picture sampled at its 8 x 8 block's centre, mixed in where it is dark and flat.
    vec2 blockUV = (floor(uv / (px * 8.0)) + 0.5) * px * 8.0;
    // The rolling shutter: lower rows were read later in the turn, so they are moved along with it.
    vec2 shutter = pc.videoC.yz * (uv.y - 0.5);
    vec2 at = Bend(uv, 1.0) + shutter;
    float edge = length((uv * 2.0 - 1.0) * vec2(pc.recip.z, 1.0));

    // Colour fringes: red a little further out, blue a little further in, growing towards the edges.
    float fringe = pc.lensC.z * scale1080 * px.y * edge * edge * 0.5;
    vec2 outward = normalize(at - 0.5 + 1e-5);
    bool moving = length(pc.motionC.xy / px) > 0.75;
    vec3 colour;
    colour.r = Smeared(at + outward * fringe, moving).r;
    colour.g = Smeared(at, moving).g;
    colour.b = Smeared(at - outward * fringe, moving).b;

    // The cheap lens blurs towards its edges: eight reads on two rings round the point, wider the further out.
    float blurRadius = (pc.frameC.y * edge * edge * 2.0 + pc.gradeC.y * 2.5) * px.y * scale1080;
    // The autofocus hunting: soft while the lens catches up with how far the middle of the picture is.
    if (pc.glitchC.z > 0.0 && pc.adaptC.w > 0.5) {
        vec2 focus = textureLod(sampler2D(t_exposure, NVR_SAMPLER(pc.pointIndex)), vec2(0.5), 0.0).ba;
        blurRadius += pc.glitchC.z * clamp(abs(focus.x - focus.y) * 0.7, 0.0, 1.0) * 6.0 * px.y * scale1080;
    }
    if (blurRadius > px.y * 0.5) {
        vec2 r = blurRadius * vec2(px.x / px.y, 1.0);
        vec3 ring = vec3(0.0);
        // The rings turned by a different angle at each pixel (interleaved gradient noise), so a wide blur is a soft grainy disc.
        float turn = fract(52.9829189 * fract(dot(uv / px, vec2(0.06711056, 0.00583715)))) * 1.570796;
        for (int k = 0; k < 4; k++) {
            float a = float(k) * 1.570796 + turn;
            ring += Read(at + r * vec2(cos(a), sin(a)) * 0.5);							// inner ring
            ring += Read(at + r * vec2(cos(a + 0.785398), sin(a + 0.785398)));			// outer ring, turned 45 degrees
        }
        colour = colour * 0.2 + ring * 0.1;
    }

    // Oversharpening: the difference from the four neighbours, added back (the halos small cameras leave along edges).
    vec2 stepUV = px * 1.25;
    vec3 around = (Read(at + vec2(stepUV.x, 0.0)) + Read(at - vec2(stepUV.x, 0.0)) + Read(at + vec2(0.0, stepUV.y)) + Read(at - vec2(0.0, stepUV.y))) * 0.25;
    colour += (colour - around) * pc.imageC.x;

    // Highlight glow: bright areas bleed over what is near them. Two rings of eight reads, the near ring stronger.
    vec3 glow = vec3(0.0);
    for (int i = 0; i < 8; i++) {
        float a = float(i) * 0.785398;
        vec2 way = vec2(cos(a), sin(a)) * vec2(px.x / px.y, 1.0);
        vec2 way2 = vec2(cos(a + 0.3927), sin(a + 0.3927)) * vec2(px.x / px.y, 1.0);
        glow += max(Read(at + way * 0.012) - 0.65, 0.0) * 0.65;
        glow += max(Read(at + way2 * 0.035) - 0.65, 0.0) * 0.35;
    }
    colour += glow * 0.125 * pc.imageC.w * 2.0;
    // Light smear: a vertical streak up and down the column through very bright light, most of all in low light.
    if (pc.flashC.w > 0.0) {
        float jitter = fract(52.9829189 * fract(dot(uv / px, vec2(0.06711056, 0.00583715))));
        vec3 streak = vec3(0.0);
        for (int i = 1; i <= 6; i++) {
            float o = (float(i) - 0.5 + jitter) * 0.045;
            float weight = 1.0 - float(i) / 7.0;
            streak += (max(Read(at + vec2(0.0, o)) - 0.85, 0.0) + max(Read(at - vec2(0.0, o)) - 0.85, 0.0)) * weight;
        }
        colour += streak * 0.35 * pc.flashC.w * (1.0 + pc.flashC.z);
    }
    // The muzzle flash overloads the sensor: the frame washes out round bright parts for a moment.
    colour += glow * 0.5 * pc.flashC.x;

    // Compression blocks in the dark and flat parts.
    vec3 blockColour = Read(Bend(blockUV, 1.0) + shutter);
    float flatness = 1.0 - clamp(length(colour - around) * 8.0, 0.0, 1.0);
    float dark = 1.0 - clamp(luma(blockColour) * 2.5, 0.0, 1.0);
    colour = mix(colour, blockColour, pc.noiseC.y * flatness * dark);
    // While the view turns or runs the encoder runs out of bits: bigger blocks (16 x 16, then 32 x 32) and colour banding.
    if (pc.videoC.x > 0.01) {
        float breakup = pc.videoC.x;
        vec2 bigUV = (floor(uv / (px * 16.0)) + 0.5) * px * 16.0;
        vec3 big = Read(Bend(bigUV, 1.0) + shutter);
        vec2 hugeUV = (floor(uv / (px * 32.0)) + 0.5) * px * 32.0;
        vec3 huge = Read(Bend(hugeUV, 1.0) + shutter);
        colour = mix(colour, blockColour, clamp(breakup * 0.35 * (0.4 + 0.6 * flatness), 0.0, 1.0));
        colour = mix(colour, big, clamp((breakup - 0.3) * 0.45 * (0.3 + 0.7 * flatness), 0.0, 1.0));
        colour = mix(colour, huge, clamp((breakup - 1.2) * 0.4 * (0.2 + 0.8 * flatness), 0.0, 1.0));
        float levels = max(48.0 - breakup * 12.0, 8.0);
        colour = mix(colour, floor(colour * levels + 0.5) / levels, clamp(breakup * 0.3, 0.0, 1.0));
    }

    // Lens dirt: smudges on the glass that light up where bright light falls on them.
    float dirt = LensDirt(uv);
    float bright = luma(glow) * 0.125 + 0.2 * luma(colour) * luma(colour);
    colour += dirt * bright * pc.frameC.w;

    // The camera's colour: overexposed, contrast round mid-grey, washed out, a faint green cast; highlights clipped softly.
    // The auto exposure lags: when the scene got brighter than the camera has caught up with, the picture is overexposed.
    float gain = 1.0;
    if (pc.adaptC.x > 0.0 && pc.adaptC.w > 0.5) {
        vec2 measured = textureLod(sampler2D(t_exposure, NVR_SAMPLER(pc.pointIndex)), vec2(0.5), 0.0).rg;
        gain = clamp(pow(max(measured.x, 1e-4) / max(measured.y, 1e-4), pc.adaptC.x / 2.2), 0.2, 5.0);
    }
    // A shot's flash blows the frame out, then the exposure pulls it down before settling.
    gain *= (1.0 + pc.flashC.x * 1.5) * (1.0 - pc.flashC.y * 0.4);
    colour *= pc.frameC.z * gain;
    colour = pow(ClipToWhite(colour), vec3(1.0 / (1.0 + pc.motionC.w)));
    colour = ClipToWhite((colour - 0.5) * pc.imageC.y + 0.5);
    // Cool: a fixed white balance towards the neutral whites of real footage, its gains scaled so grey keeps its brightness.
    colour *= mix(vec3(1.0), vec3(0.93, 0.98, 1.12) / 0.9795, pc.gradeC.x);
    float lum = luma(colour);
    colour = mix(vec3(lum), colour, pc.imageC.z);
    colour *= mix(vec3(1.0), vec3(0.94, 1.0, 0.95), pc.noiseC.w);
    // Low-light mode: in the dark the camera gives up colour for brightness.
    if (pc.flashC.z > 0.0) {
        float l = luma(colour);
        colour = mix(colour, vec3(0.96, 1.0, 0.97) * pow(max(l, 0.0), 0.85), pc.flashC.z * 0.9);
    }
    colour = ClipToWhite(colour);
    colour = (1.0 - exp(-max(colour, 0.0) * 1.15)) / (1.0 - exp(-1.15));	// compresses the top, keeps 0 and 1
    colour = clamp(colour, 0.0, 1.0);

    // Sensor noise, per video frame: grey grain plus a little colour noise, stronger in the dark.
    float frameNo = pc.noiseC.z;
    vec2 cell = floor(uv / px);
    float n = Hash(cell + frameNo * 17.31) - 0.5;
    vec3 chroma = vec3(Hash(cell * 1.3 + frameNo * 3.7), Hash(cell * 0.7 + frameNo * 5.1), Hash(cell * 1.9 + frameNo * 7.3)) - 0.5;
    float amount = pc.noiseC.x * 0.06 * mix(1.0, 0.35, clamp(luma(colour) * 1.5, 0.0, 1.0)) * (1.0 + pc.flashC.z * 1.5);
    colour += (n + chroma * 0.4) * amount;

    // Vignette; outside the bent picture (FitEdges below 1): black.
    colour *= 1.0 - pc.lensC.w * smoothstep(0.6, 1.9, edge);
    // The camera's frame: a soft black oval (in screen proportions) closing in from the corners.
    float oval = length(uv * 2.0 - 1.0);
    float ovalStart = mix(1.6, 0.75, pc.frameC.x);
    colour *= 1.0 - smoothstep(ovalStart, ovalStart + 0.35, oval);
    colour *= (all(greaterThanEqual(at, vec2(0.0))) && all(lessThanEqual(at, vec2(1.0)))) ? 1.0 : 0.0;

    // An encoder hiccup: for a moment some 16 x 16 blocks show stale picture (the last kept frame, shifted sideways) or smear,
    // some with their colour channels slipped.
    if (pc.glitchC.x > 0.0) {
        vec2 blockId = floor(uv / (px * 16.0));
        float h = Hash(blockId + pc.glitchC.y);
        float row = Hash(vec2(blockId.y, pc.glitchC.y * 0.37));
        if (h < pc.glitchC.x * 0.35 || row < pc.glitchC.x * 0.08) {
            vec2 shift = vec2((Hash(blockId * 1.7 + pc.glitchC.y) - 0.5) * 0.08, 0.0);
            vec3 stale = textureLod(sampler2D(t_hold, NVR_SAMPLER(pc.pointIndex)), uv + shift, 0.0).rgb;
            vec3 smear = Read(Bend((blockId + 0.5) * px * 16.0, 1.0));
            colour = Hash(blockId + 7.0) > 0.5 ? stale : smear;
            if (Hash(blockId + 3.0) > 0.6) colour.rb = colour.br;
        }
    }

    // The timestamp, top right, not bent: white with a dark edge.
    float lit = 0.0, shadow = 0.0;
    float texel = max(floor(pc.textC.y * scale1080 + 0.5), 1.0);	// whole pixels per texel: crisp
    vec2 corner = vec2(1.0 / px.x - pc.textC.z * texel - 40.0 * scale1080, 34.0 * scale1080);
    vec2 pixel = uv / px;
    vec2 t = (pixel - corner) / texel;
    if (pc.textC.x > 0.0 && all(greaterThan(t, vec2(-1.0))) && all(lessThan(t, pc.textC.zw + 1.0))) {
        lit = textureLod(sampler2D(t_text, NVR_SAMPLER(pc.pointIndex)), (floor(t) + 0.5) / TextTextureSize, 0.0).a;
        vec2 back = (pixel - corner - max(floor(texel * 0.35), 1.0)) / texel;
        shadow = textureLod(sampler2D(t_text, NVR_SAMPLER(pc.pointIndex)), (floor(back) + 0.5) / TextTextureSize, 0.0).a;
    }
    colour = mix(colour, colour * 0.25, shadow * pc.textC.x);
    colour = mix(colour, vec3(0.93, 0.93, 0.9), lit * pc.textC.x);
    return vec4(clamp(colour, 0.0, 1.0), 1.0);
}

void main() {
    o_color = Picture();
}
