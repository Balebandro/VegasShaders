// GPU grass: one instance per visible strand (effects/GpuGrass.cpp, GpuGrassCull.comp wrote them). Near list (GG_FAR 0): 4 levels
// along the strand, 5 triangles (15 vertices, a triangle list); far list (GG_FAR 1): one triangle. Lit exactly as the game's own
// grass pixel shader lights its cards ((ambient x light scale + sun x NdotL x light scale x dimmer) x colour, gamma space, no NVR
// light scales), the colour of the card's own texture at the strand's height on the plant (a 4-step ramp read once per card),
// a little lighter or darker per strand, and the game's grass fog (per vertex, as its VS). All in the vertex shader: the colour is
// exact at the levels (the ramp's knots) and linear between them, so the fragment shader only writes it.
//
// Build: glslangValidator -V --target-env vulkan1.1 -DGG_FAR=0 -o GpuGrassNearVS.spv GpuGrass.vert
//        glslangValidator -V --target-env vulkan1.1 -DGG_FAR=1 -o GpuGrassFarVS.spv GpuGrass.vert
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "GpuGrass.glsl"

#ifndef GG_FAR
#define GG_FAR 0
#endif

layout(set = 0, binding = 0, std140) uniform Frame { GG_FRAME_BLOCK } f;
layout(set = 0, binding = 3, std430) readonly buffer Draws { GGDraw draws[]; };
layout(set = 0, binding = 4, std430) readonly buffer Blades { GGBlade blades[]; };

layout(location = 0) out vec4 o_color;

#if GG_FAR
// root left, root right, tip
const uvec2 kVerts[3] = uvec2[3](uvec2(0u, 0u), uvec2(0u, 1u), uvec2(3u, 0u));
#else
// (level, side): level 3 is the tip
const uvec2 kVerts[15] = uvec2[15](
    uvec2(0u, 0u), uvec2(0u, 1u), uvec2(1u, 0u),
    uvec2(0u, 1u), uvec2(1u, 1u), uvec2(1u, 0u),
    uvec2(1u, 0u), uvec2(1u, 1u), uvec2(2u, 0u),
    uvec2(1u, 1u), uvec2(2u, 1u), uvec2(2u, 0u),
    uvec2(2u, 0u), uvec2(2u, 1u), uvec2(3u, 0u));
#endif

void main() {
    const uint cap = uint(f.misc.y);
    if (uint(gl_InstanceIndex) >= cap) {   // more strands were counted than fit (the cull pass stopped writing): nothing
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
        o_color = vec4(0.0);
        return;
    }
    const GGBlade b = blades[(GG_FAR != 0 ? cap : 0u) + uint(gl_InstanceIndex)];
    const uvec2 v = kVerts[gl_VertexIndex];
    const float t = float(v.x) * (1.0 / 3.0);

    const uint di = b.packed & 0xFFFu;
    const float ang = float((b.packed >> 12) & 63u) * (6.2831853 / 63.0);
    const float seed = float((b.packed >> 18) & 63u) * (1.0 / 63.0);
    const float lenShare = float(b.packed >> 24) * (1.0 / 255.0);
    const vec2 lightAmounts = unpackHalf2x16(b.light);   // x light scale, y sun amount

    // the Bezier from the root (p0 = root, p1 = root + ctrl, p2 = root + tip)
    vec3 pos = b.a.xyz + (2.0 * (1.0 - t) * t) * b.ctrl + (t * t) * b.tip;
    const vec3 side = vec3(-sin(ang), cos(ang), 0.0);
    const float w = b.a.w * (1.0 - t * t);
    if (v.x < 3u) pos += side * ((float(v.y) - 0.5) * w);
    // a sideways wobble from level to level (a dry twig's kinks)
    pos += side * (f.shape2.y * b.a.w * sin(t * 9.0 + seed * 6.2831853) * (0.5 + 0.5 * seed));

    const vec4 clip = vec4(dot(f.rows[0].xyz, pos) + f.rows[0].w, dot(f.rows[1].xyz, pos) + f.rows[1].w,
                           dot(f.rows[2].xyz, pos) + f.rows[2].w, dot(f.rows[3].xyz, pos) + f.rows[3].w);
    gl_Position = vec4(clip.x, -clip.y, clip.z, clip.w);   // D3D9 clip space has y up, Vulkan's viewport y down

    const GGDraw d = draws[di];
    // the card's colour at this height on the plant (a short strand's top shows the plant's middle colour)
    const float hp = clamp(t * lenShare, 0.0, 1.0) * 3.0;
    vec3 albedo = hp < 1.0 ? mix(d.ramp[0].rgb, d.ramp[1].rgb, hp)
                : hp < 2.0 ? mix(d.ramp[1].rgb, d.ramp[2].rgb, hp - 1.0)
                           : mix(d.ramp[2].rgb, d.ramp[3].rgb, hp - 2.0);
    const float tp = hp * (1.0 / 3.0);
    albedo *= 1.0 + (seed - 0.5) * 2.0 * f.look.x;                    // each strand a little lighter or darker
    albedo += (f.look.y * tp * tp * tp) * vec3(0.10, 0.08, 0.0);        // a warm lift toward the tip
    const float gray = dot(albedo, vec3(0.2126, 0.7152, 0.0722));
    albedo = max(mix(vec3(gray), albedo, f.look.z), vec3(0.0));

    vec3 color = (d.ambient.rgb * lightAmounts.x + d.sun.rgb * lightAmounts.y) * albedo;

    // the game's grass fog (GRASS23x00*.vso)
    const float fogDist = length(vec3(clip.xy, clip.w - clip.z));
    const float fogStrength = 1.0 - clamp((f.fogParam.x - fogDist) / f.fogParam.y, 0.0, 1.0);
    color = mix(color, f.fogColor.rgb, pow(fogStrength, f.fogParam.z));

    // DebugView 1: near strands red, far strands blue (lit by the light scale only); 2: a colour per game grass draw
    if (f.misc.z > 0.5 && f.misc.z < 1.5) color = (GG_FAR != 0 ? vec3(0.15, 0.35, 1.0) : vec3(1.0, 0.15, 0.1)) * (0.5 + 0.5 * lightAmounts.x);
    if (f.misc.z > 1.5) color = vec3(float(ggHash(di) & 255u), float(ggHash(di + 1u) & 255u), float(ggHash(di + 2u) & 255u)) * (1.0 / 255.0);

    o_color = vec4(color, 1.0);
}
