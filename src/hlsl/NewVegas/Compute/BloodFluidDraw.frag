// BloodFluid (BloodFlow v2, needs NVR DXVK): paints the simulated blood into the image, one full-screen pass blended over
// it (src/effects/BloodFluid.cpp, after BloodFlow's own paint; same place, same blend, same colour write mask).
//
// Every pixel reads the particles its 16 x 16 tile lists (BloodFluidSim.comp, tiles mode) and finds its surface point
// from the depth buffer, as BloodFlow's paint does:
//  - Blood lying on something is a STAIN: each particle adds a smooth bump (1 - d^2/R^2)^2 measured along the plane it
//    lies on, so neighbours run together into one film instead of beads (the spike's weak point: sprite fluids look
//    like frogspawn on walls). The sum is a thickness field; what shows is where it is thicker than a threshold, and its
//    slope bends the normal (a rounded edge, highlights along it).
//  - Blood in the air is a DROP: a small sphere in front of whatever is behind it.
// The look is BloodFlow's (BloodFlow.fx.hlsl BloodPS, line for line where it applies): the light at the pixel from the
// image and the sun, the blood's colour (the game splat's own colour with MatchGameColor) drying darker and browner,
// light passing through thin blood (a saturated red stain) and scattering in thick blood (its own deep red), small GGX
// highlights of the sun and the lamps, the sky at low angles, never brighter than the scene allows.
//
// Build: glslangValidator -V --target-env vulkan1.1 -o BloodFluidDraw.spv BloodFluidDraw.frag

#version 450
#extension GL_GOOGLE_include_directive : enable
#extension GL_EXT_samplerless_texture_functions : enable
#extension GL_EXT_nonuniform_qualifier : enable

#include "BloodFluid.glsl"

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 outColor;

layout(set = 1, binding = 0) uniform texture2D t_depth;        // TESR_DepthBuffer
layout(set = 1, binding = 1) uniform texture2D t_normals;      // TESR_NormalsBuffer (view space, x 0.5 + 0.5)
layout(set = 1, binding = 2) uniform texture2D t_scene;        // TESR_SourceBuffer: the image as it was before this pass
layout(set = 1, binding = 3) uniform texture2D t_pointShadow;  // TESR_PointShadowBuffer (outdoors: where the sun reaches)
layout(set = 1, binding = 4) uniform texture2D t_viewModel;    // TESR_DepthBufferViewModel
layout(set = 1, binding = 5, std430) readonly buffer Draws { DrawRec D[]; };
layout(set = 1, binding = 6, std430) readonly buffer TileCounts { uint TileCount[]; };
layout(set = 1, binding = 7, std430) readonly buffer TileLists { uint TileList[]; };
layout(set = 0, binding = 0) uniform sampler s_samplers[];   // DXVK's sampler heap
layout(push_constant) uniform Push { uint samplerIndex; uint pad0, pad1, pad2; } pc;   // POINT / CLAMP

// Every texture is read through the point sampler at the pixel centre (as NVR's working passes read them; texelFetch on
// these D3D9 textures gave 1.0 everywhere in game, 2026-10-26).
vec4 Texel(texture2D t, ivec2 px) { return textureLod(sampler2D(t, s_samplers[pc.samplerIndex & 0xFFFFu]), (vec2(px) + 0.5) * F.screen.zw, 0.0); }

float hash2(vec2 p) { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }
float valueNoise(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(hash2(i), hash2(i + vec2(1.0, 0.0)), f.x), mix(hash2(i + vec2(0.0, 1.0)), hash2(i + vec2(1.0, 1.0)), f.x), f.y);
}

float Glint(vec3 N, vec3 V, vec3 L, float a2) {
    vec3 H = normalize(L + V);
    float nh = saturate(dot(N, H));
    float lh = saturate(dot(L, H));
    float d = nh * nh * (a2 - 1.0) + 1.0;
    return a2 / (3.14159 * d * d) * (0.02 + 0.98 * pow(1.0 - lh, 5.0)) * 0.25 / max(lh * lh, 0.1) * saturate(dot(N, L));
}

// lamp.xyz eye-relative here (BloodFluid.cpp takes the eye off), lamp.w its radius (0: none). WetWorld's fall-off.
float LampGlint(vec3 N, vec3 V, vec3 P, vec4 lamp, vec3 color, float a2) {
    if (lamp.w <= 0.0) return 0.0;
    vec3 toLamp = lamp.xyz - P;
    float s = saturate(dot(toLamp, toLamp) / (lamp.w * lamp.w));
    if (s >= 1.0) return 0.0;
    float atten = (1.0 - s) * (1.0 - s) / (1.0 + 5.0 * s);
    return Glint(N, V, normalize(toLamp), a2) * atten * saturate(luma(color));
}

float Highlights(vec3 N, vec3 V, vec3 P, float a2, float lit) {
    float glints = 0.0;
    if (F.match.w > 0.5) glints += Glint(N, V, normalize(F.sunDir.xyz), a2) * lit * saturate(luma(linearize(F.sunColor.rgb)));
    for (int i = 0; i < 12; i++) {
        glints += LampGlint(N, V, P, F.shadowLightPos[i], F.lightColor[i].rgb, a2);
        glints += LampGlint(N, V, P, F.lightPos[i], F.lightColor[i + 12].rgb, a2);
    }
    return glints;
}

vec3 UnpackColour(float t) {
    t = floor(t + 0.5);
    float r = floor(t / 65536.0);
    float gb = t - r * 65536.0;
    float g = floor(gb / 256.0);
    return vec3(r, g, gb - g * 256.0);
}

void main() {
    ivec2 px = ivec2(gl_FragCoord.xy);
    if (px.x >= int(F.screen.x) || px.y >= int(F.screen.y)) discard;   // (2026-10-29: outside a RenderScale frame's drawn part)
    uint tile = uint(px.y >> 4) * F.counts.z + uint(px.x >> 4);
    uint count = min(TileCount[F.tiles2.x != 0u ? F.tiles2.y + tile : tile], F.counts2.x);   // (SteadyTiles: the fill counters)
    if (count == 0u) discard;
    if (IsViewModel(Texel(t_viewModel, px).x)) discard;

    float farZ = F.eyeRel.w;
    float depth = min(Texel(t_depth, px).x * farZ, farZ * 0.98);
    bool sky = depth >= farZ * 0.979;
    vec3 rel = RayAt(vec2(px) + 0.5) * depth;    // the surface here, eye-relative
    float sceneDist = length(rel);
    vec3 rd = rel / sceneDist;
    vec3 nv = Texel(t_normals, px).xyz * 2.0 - 1.0;
    vec3 Ns = vec3(dot(F.viewRot0.xyz, nv), dot(F.viewRot1.xyz, nv), dot(F.viewRot2.xyz, nv));
    Ns = dot(Ns, Ns) > 1e-6 ? normalize(Ns) : -rd;
    float aa = max(depth * 2.0 / (F.screen.y * F.up.w), 0.05);   // a pixel's size on the surface (units)
    uint debug = F.counts2.z;
    if (debug == 3u) {   // TEST: the depth this pass reads (red bands every 64 units; green where the raw value is above 0)
        outColor = vec4(fract(depth / 64.0), Texel(t_depth, px).x > 0.0 ? 1.0 : 0.0, 0.0, 0.8);
        return;
    }

    // Gather: stains (thickness field on the surface) and drops (spheres in the air).
    float T = 0.0, ageSum = 0.0, poolSum = 0.0, tintW = 0.0, waterSum = 0.0;   // (2026-10-29 WaterGun: waterSum = kind 3)
    vec3 tintSum = vec3(0.0), grad = vec3(0.0);
    float A = 0.0, nearT = 1e9, dropAge = 0.0, dropTint = 0.0, dropWater = 0.0;
    vec3 dropN = -rd;
    vec4 dot_ = vec4(0.0);
    uint base = tile * F.counts2.x;
    for (uint k = 0u; k < count; k++) {
        DrawRec r = D[TileList[base + k]];
        float R = abs(r.a.w);
        float amount = r.c.y;
        if (debug == 2u) {
            vec3 s = ProjectRel(r.a.xyz);
            vec2 dp = s.xy - (vec2(px) + 0.5);
            if (s.z > 1.0 && dot(dp, dp) < 4.0) {
                uint fl = Flags(r.c.w);
                vec3 c = (fl & FlagSettled) != 0u ? vec3(0.35, 0.18, 0.08) : (fl & FlagFrozen) != 0u ? vec3(0.1, 0.3, 1.0) :
                    (fl & FlagGround) != 0u ? vec3(1.0, 0.85, 0.0) : (fl & FlagContact) != 0u ? vec3(1.0, 0.05, 0.05) :
                    (fl & FlagTouched) != 0u ? vec3(1.0, 0.4, 0.7) : vec3(1.0);
                dot_ = vec4(c, 1.0);
            }
            continue;
        }
        if (r.a.w > 0.0) {
            if (sky) continue;
            vec3 dd = rel - r.a.xyz;
            vec3 nn = r.b.xyz;
            float dn = dot(dd, nn);
            vec3 ip = dd - nn * dn;
            float q = dot(ip, ip) / (R * R);
            if (q >= 1.0) continue;
            float keep = saturate(1.0 - abs(dn) / (0.5 * R + 1.0 + aa)) * saturate((dot(Ns, nn) + 0.2) / 0.5) * amount;
            float w1 = 1.0 - q;
            float w = w1 * w1 * keep;
            if (w <= 0.0) continue;
            T += w;
            ageSum += w * r.b.w;
            poolSum += r.c.z > 0.5 && r.c.z < 1.5 ? w : 0.0;
            waterSum += r.c.z > 2.5 ? w : 0.0;
            grad += ip * (-4.0 * w1 / (R * R) * keep);
            if (r.c.x > 0.5) { tintSum += w * UnpackColour(r.c.x); tintW += w; }
        }
        else {
            vec3 c = r.a.xyz;
            float t = dot(c, rd);
            float d2 = dot(c, c) - t * t;
            if (d2 >= R * R) continue;
            float h = sqrt(R * R - d2);
            float tf = t - h;
            if (tf <= 0.0 || tf > sceneDist) continue;
            A += 2.0 * h * amount;
            if (tf < nearT) { nearT = tf; dropN = normalize(rd * tf - c); dropAge = r.b.w; dropTint = r.c.x; dropWater = r.c.z > 2.5 ? 1.0 : 0.0; }
        }
    }
    if (debug == 2u) {
        if (dot_.a > 0.0) { outColor = vec4(delinearize(dot_.rgb), 0.95); return; }
        discard;
    }

    float th = F.look.z;
    float slope = length(grad);
    // UNOFFICIAL 2026-10-27 night (user: "almost looks like ketchup", "full bright as though it does not respond to shaders or lighting
    // conditions", then a screenshot: "the color doesn't match the vanilla blood on the right, we need it to match"; look4.x, OldColour
    // 0): the fluid's own look. BloodPS's (kept below for OldColour) lights the blood's colour by its own estimate of the light: the
    // image's brightest channel over 0.3, but never under the open sky's light (the sun and ambient constants). That floor knows
    // nothing of NVR's shadows, ambient occlusion and exposure, so the fluid, mostly a film the old look took for thin, came out lit
    // as in the open sun: in the user's shot sRGB (140 1 1), twice the ground's red (70 64 55) and no green at all. The game's own
    // blood in the same shot is a dark film over the ground: (38 14 14) on (40 33 27), (30 8 10) on (35 30 27): in linear about 0.6-0.9
    // of the ground's red kept and 0.2-0.4 of its green and blue. The fluid now does the same: a filter over the pixel under it, in
    // the blood's own hue (the game splat's colour, drying browner), keeping 0.85 of the image in that hue and 0.32 of the rest at a
    // film's edge, down to 0.45 and 0.12 where thick (thick at 1.2 over the threshold), a little darker as it dries. So it is lit
    // exactly as the ground under it is (sun, shade, lamps, AO, exposure) and matches the decals by construction.
    // look4.y (TextureRelief): the ground's own detail. NVR's normals come from depth, without the textures' normal maps, so it is
    // read from the picture under the blood: its brightness, as a height (darker = lower, a crack or dip). A film's edge creeps into
    // the dips first and breaks up on raised bits, and a thin film's highlights follow the bumps (a thick pool is flat).
    bool deep = F.look4.x > 0.5;
    float relief = 0.0;           // > 0: the ground here is darker than round it (a crack, a dip); log brightness
    vec2 reliefGrad = vec2(0.0);  // the ground's slope in that height, per 2 pixels: screen x, screen y (down)
    if (deep && F.look4.y > 0.0 && T > 0.5 * th) {
        float lc = log(max(luma(linearize(max(Texel(t_scene, px).rgb, vec3(0.0)))), 1e-5));
        float lr = log(max(luma(linearize(max(Texel(t_scene, px + ivec2(2, 0)).rgb, vec3(0.0)))), 1e-5));
        float ll = log(max(luma(linearize(max(Texel(t_scene, px - ivec2(2, 0)).rgb, vec3(0.0)))), 1e-5));
        float ld = log(max(luma(linearize(max(Texel(t_scene, px + ivec2(0, 2)).rgb, vec3(0.0)))), 1e-5));
        float lu = log(max(luma(linearize(max(Texel(t_scene, px - ivec2(0, 2)).rgb, vec3(0.0)))), 1e-5));
        // (Steps far bigger than a texture's, an object's edge or a shadow's, are left out.)
        reliefGrad = clamp(vec2(lr - ll, ld - lu) * 0.5, vec2(-0.6), vec2(0.6));
        relief = clamp((lr + ll + ld + lu) * 0.25 - lc, -0.5, 0.5);
    }
    float cover = saturate((T + relief * F.look4.y * th * 1.2 - th) / max(slope * aa * 1.5, 0.02) + 0.5);
    float thick = saturate((T - th) / (deep ? 1.2 : 2.5));
    float coverA = saturate(A / (0.6 + aa));
    float amountAll = max(cover, coverA);
    if (debug == 1u) {
        // Debug view 1: stains red (brighter = thicker), the field's faint edge orange, drops white.
        if (coverA > 0.5) { outColor = vec4(1.0, 1.0, 1.0, 0.9); return; }
        if (cover > 0.5) { outColor = vec4(1.0, 0.15 + 0.6 * (1.0 - thick), 0.1, 0.85); return; }
        if (T > 0.05) { outColor = vec4(1.0, 0.5, 0.0, 0.5); return; }
        discard;
    }
    if (amountAll < 0.004) discard;

    vec3 sceneLin = linearize(max(Texel(t_scene, px).rgb, vec3(0.0)));
    // The light here (BloodPS): from the image's brightest channel over a typical albedo, or the sky and sun, the brighter.
    float lit = 0.0;
    if (F.match.w > 0.5) {
        float sunAmbient = max(luma(linearize(F.sunAmbient.rgb)), 0.001);
        vec4 shadows = Texel(t_pointShadow, px);
        lit = saturate(pow(abs((shadows.r + shadows.g) / sunAmbient), 5.0));
    }
    float irradiance = luma(linearize(saturate(F.sunAmbient.rgb))) + lit * luma(linearize(saturate(F.sunColor.rgb))) * saturate(dot(Ns, normalize(F.sunDir.xyz)));
    float sceneLight = max(sceneLin.r, max(sceneLin.g, sceneLin.b)) / 0.3;
    float light = min(max(sceneLight, irradiance), 2.5);
    if (deep) light = min(light, 3.0 * sceneLight);   // (2026-10-27 evening: no brighter than 3x what the image shows here)

    float age = T > 1e-4 ? ageSum / T : dropAge;
    float pool = T > 1e-4 ? poolSum / T : 0.0;
    float dryTime = mix(F.look2.y, F.look2.z, pool);
    float dry = cover > 0.0 ? smoothstep(0.0, 1.0, age / (dryTime * mix(0.5, 1.6, thick))) : 0.0;
    // UNOFFICIAL 2026-10-29 WaterGun (BloodFluid.cpp): water (kind 3) does not dry; it is clear and bends what is under it.
    float water = T > 1e-4 ? saturate(waterSum / T * 2.0 - 0.5) : 0.0;
    dry *= 1.0 - water;
    vec3 freshColour = F.fresh.rgb;
    vec3 dryColour = F.dryColor.rgb;
    vec3 glintTint = vec3(0.02, 0.004, 0.004);
    if (F.match.z > 0.5 && tintW > 0.5 * T && tintW > 1e-4) {
        freshColour = linearize(saturate(tintSum / tintW / 255.0 * F.match.x)) * F.match.y;
        freshColour *= max(1.0, F.look3.w / max(max(freshColour.r, max(freshColour.g, freshColour.b)), 1e-4));   // (BloodPS 2026-10-27: MatchFloor)
        dryColour = freshColour * vec3(0.72, 0.8, 0.8) + freshColour.r * vec3(0.0, 0.1, 0.05);
        glintTint = 0.004 + 0.016 * freshColour / max(max(freshColour.r, max(freshColour.g, freshColour.b)), 1e-4);   // (BloodPS 2026-10-25: the bead's tint in the blood's own hue)
    }
    vec3 albedo = mix(freshColour, dryColour, dry);
    vec3 at = rel + F.eyeRel.xyz;   // fixed to the surface (anchor space)
    float mottle = mix(1.0, 0.8 + 0.4 * valueNoise((at.xy + at.z) * 0.35), dry);
    albedo *= mottle;
    float rim = saturate(1.0 - (T - th) / 0.6) * cover;
    vec3 blood;
    if (deep) {
        // (2026-10-27 night: a filter over the image, in the blood's hue, as the game's decal looks; see above.)
        vec3 bloodHue = albedo / max(max(albedo.r, max(albedo.g, albedo.b)), 1e-4);
        float keepHue = mix(0.85, 0.45, thick) * mix(1.0, 0.7, dry);
        float keepRest = mix(0.32, 0.12, thick) * mix(1.0, 0.8, dry);
        blood = sceneLin * mix(vec3(keepRest), vec3(keepHue), bloodHue) * mottle * (1.0 - 0.35 * dry * rim);
        // (Toward the sun, thin fresh blood lets it through: a red glow, from what is under it.)
        blood += sceneLin * bloodHue * (pow(saturate(dot(rd, normalize(F.sunDir.xyz))), 4.0) * lit * (1.0 - dry) * (1.0 - thick) * 0.5);
    }
    else {
        // (BloodPS 2026-10-25: light passes the blood in its own colour, measured from its strongest channel, not from red: matched
        // green or yellow blood no longer glows or goes red; red blood unchanged.)
        vec3 through = exp(-(1.2 + log(max(max(albedo.r, max(albedo.g, albedo.b)), 0.001) / max(albedo, vec3(0.001)))) * (0.25 + 2.0 * thick));
        blood = (sceneLin * through + light * albedo * mix(1.0, 0.6, saturate(thick * 1.3 - 0.3)) * (1.0 - through)) * (1.0 - 0.35 * dry * rim);
        blood += light * albedo * (pow(saturate(dot(rd, normalize(F.sunDir.xyz))), 4.0) * lit * (1.0 - dry) * (1.0 - thick) * 0.8);
    }
    if (water > 0.0) {
        // (Water: what is under it, shifted by the surface's slope (refraction) and a little darker and bluer where deep; a wet
        // film darkens the ground as wet dirt does.)
        vec2 bendPx = vec2(dot(grad, F.right.xyz), -dot(grad, F.up.xyz)) * (6.0 + 18.0 * thick);
        ivec2 rp = clamp(px + ivec2(bendPx), ivec2(0), ivec2(F.screen.xy) - 1);
        vec3 under = linearize(max(Texel(t_scene, rp).rgb, vec3(0.0)));
        vec3 clearWater = under * mix(vec3(0.72, 0.74, 0.76), vec3(0.62, 0.78, 0.86), thick);
        blood = mix(blood, clearWater, water);
    }
    vec3 colour = mix(sceneLin, blood, cover);

    // Wet blood shines (BloodPS): small highlights, at most about the light here; the edge's slope bends the normal.
    // (BloodPS 2026-10-27, look3 = TESR_BloodFlowLook: the sheen weaker, capped, part in the blood's own hue.)
    vec3 hue = saturate((glintTint - 0.004) / 0.016);
    float gloss = F.look2.w * (1.0 - dry) * cover * mix(0.5, 1.0, thick);
    gloss = mix(gloss, cover * mix(0.6, 1.0, thick), water);   // (WaterGun: water always shines)
    vec3 V = -rd;
    if (gloss > 0.001) {
        // (2026-10-27, rework.z: a pool's top is flat; the field's slope bends the normal mostly where the blood is thin, at its
        // edges (a rounded rim), at most half way. Bent everywhere by every particle's own bump, a pool caught the sky and the
        // sun all over in specks: the "almost reflective" look.)
        vec3 bend = grad * F.look.w;
        if (F.rework.z > 0.5) {
            bend /= max(T / max(th, 0.05), 1.0);
            float bl = length(bend);
            if (bl > 0.5) bend *= 0.5 / bl;
        }
        vec3 Nb = normalize(Ns - bend);
        if (deep && F.look4.y > 0.0) {
            // (2026-10-27 night, TextureRelief: a thin film's surface follows the ground's bumps, brighter taken as higher.)
            vec3 bump = (reliefGrad.x * F.right.xyz - reliefGrad.y * F.up.xyz) * (0.6 * F.look4.y * (1.0 - thick));
            bump -= Ns * dot(bump, Ns);
            float bumpLength = length(bump);
            if (bumpLength > 0.4) bump *= 0.4 / bumpLength;
            Nb = normalize(Nb - bump);
        }
        float rough = mix(mix(0.12, 0.45, dry), 0.05, water);
        float a2 = rough * rough * rough * rough;
        float glints = Highlights(Nb, V, rel, a2, lit);
        float fresnel = 0.02 + 0.98 * pow(1.0 - saturate(dot(Nb, V)), 5.0);
        vec3 surroundings = F.match.w > 0.5 ? linearize(saturate(F.horizon.rgb)) : vec3(light * 0.1);
        if (deep) surroundings *= saturate(sceneLight / max(irradiance, 0.05));   // (2026-10-27 evening: less where the spot is shut in)
        vec3 sheenTint = mix(mix(vec3(1.0), hue, F.look3.z), vec3(1.0), water);
        float sheen = mix(min(fresnel, F.look3.y) * F.look3.x, fresnel, water);
        colour += (min(glints, 1.0) * 0.4 * min(light, 1.2) + sheen * surroundings * sheenTint * (1.0 - rough) * 0.5) * gloss;
    }
    // A drop in the air: a dark red bead with a glint, in front of whatever is behind it.
    if (coverA > 0.0) {
        vec3 dropColour = freshColour, dropTintRgb = glintTint;
        if (F.rework.x > 0.5 && F.match.z > 0.5 && dropTint > 0.5 && !(tintW > 0.5 * T && tintW > 1e-4)) {
            // (2026-10-27: a drop takes its own splat's colour, as the stains do; it fell back to FreshColor, a far brighter red.)
            dropColour = linearize(saturate(UnpackColour(dropTint) / 255.0 * F.match.x)) * F.match.y;
            dropColour *= max(1.0, F.look3.w / max(max(dropColour.r, max(dropColour.g, dropColour.b)), 1e-4));
            dropTintRgb = 0.004 + 0.016 * dropColour / max(max(dropColour.r, max(dropColour.g, dropColour.b)), 1e-4);
        }
        vec3 drop = light * (dropColour * 0.7 + dropTintRgb * F.look2.w);
        if (deep) {
            // (2026-10-27 night: a drop as dark as thick blood over what is behind it, in its hue: lit as the scene there is.)
            vec3 dropHue = dropColour / max(max(dropColour.r, max(dropColour.g, dropColour.b)), 1e-4);
            drop = max(sceneLin.r, max(sceneLin.g, sceneLin.b)) * mix(vec3(0.12), vec3(0.45), dropHue);
        }
        float a2 = 0.12 * 0.12 * 0.12 * 0.12;
        // (2026-10-27, rework.w: the drops' white glints made each one a shiny orb; weaker, and part in the blood's hue.)
        vec3 dropGlint = mix(vec3(1.0), saturate((dropTintRgb - 0.004) / 0.016), F.look3.z) * F.rework.w;
        drop += min(Highlights(dropN, V, rd * nearT, a2, lit), 1.0) * 0.4 * min(light, 1.2) * F.look2.w * dropGlint;
        if (dropWater > 0.5) {
            // (WaterGun: a clear drop: what is behind it, bent through it, and a white glint.)
            ivec2 rp = clamp(px + ivec2(vec2(dot(dropN, F.right.xyz), -dot(dropN, F.up.xyz)) * 10.0), ivec2(0), ivec2(F.screen.xy) - 1);
            drop = linearize(max(Texel(t_scene, rp).rgb, vec3(0.0))) * vec3(0.85, 0.92, 0.96)
                + min(Highlights(dropN, V, rd * nearT, a2, lit), 1.0) * 0.6 * min(light, 1.2);
        }
        colour = mix(colour, drop, coverA);
    }
    float ceiling = max(max(sceneLin.r, max(sceneLin.g, sceneLin.b)), 1.0);
    colour = min(colour, vec3(ceiling));

    // Blended over the image by alpha (BloodPS): the colour is what the pixel is at full strength.
    float alpha = amountAll;
    vec3 full = (colour - sceneLin * (1.0 - amountAll)) / max(amountAll, 0.004);
    full = max(full, vec3(0.0));
    full *= ceiling / max(max(full.r, max(full.g, full.b)), ceiling);
    outColor = vec4(delinearize(full), alpha);
}
