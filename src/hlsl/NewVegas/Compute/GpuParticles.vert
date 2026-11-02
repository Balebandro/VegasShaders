// GPU particles: one instance per particle, 6 vertices (two triangles) each (needs NVR DXVK 1.5-particles,
// effects/GpuParticles.cpp). Falling rain is a thin streak along its motion over look.y seconds (at least
// ~1 pixel wide; thinner drops fade instead), a rain splash a small crown standing on the surface, snow a
// round flake (falling or settled). The vertex shader reads the particle buffer directly.
//
// Build: glslangValidator -V --target-env vulkan1.1 -S vert -o GpuParticlesVS.spv GpuParticles.vert
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "GpuParticles.glsl"

layout(set = 0, binding = 1, std430) readonly buffer Particles { Particle particles[]; };
layout(set = 0, binding = 2, std140) uniform Frame { GP_FRAME_BLOCK } f;

layout(location = 0) out vec2 o_local;      // x across (-1..1), y along (0 tail / bottom .. 1 head / top)
layout(location = 1) out float o_alpha;     // coverage * fades
layout(location = 2) out float o_depth;     // view depth of this vertex (units)
layout(location = 3) flat out float o_kind; // 0 rain streak, 1 splash, 2 snow flake
layout(location = 4) flat out float o_age;  // landed: 0..1 of its life

const vec2 corners[6] = vec2[6](vec2(-1.0, 0.0), vec2(1.0, 0.0), vec2(1.0, 1.0), vec2(-1.0, 0.0), vec2(1.0, 1.0), vec2(-1.0, 1.0));

vec4 project(vec3 p) { return f.viewProj * vec4(p + f.origin.xyz, 1.0); }

void hide() { gl_Position = vec4(2.0, 2.0, 2.0, 1.0); o_alpha = 0.0; o_local = vec2(0.0); o_depth = 0.0; o_kind = 0.0; o_age = 0.0; }

void main() {
    Particle p = particles[gl_InstanceIndex];
    vec2 c = corners[gl_VertexIndex % 6];
    float size = f.look.x;
    float near = f.camFwd.w + 1.0;
    bool snow = f.sim.w > 0.5;
    bool landed = p.a.w >= 0.0;
    o_age = landed ? clamp(p.a.w / max(f.sim.z, 1e-3), 0.0, 1.0) : 0.0;

    if (p.b.w <= 0.0) { hide(); return; }

    if (!snow && !landed) {
        // streak: head at the drop, tail where it was look.y seconds ago; clipped to the near plane in clip space
        vec4 ch = project(p.a.xyz);
        vec4 ct = project(p.a.xyz - p.b.xyz * f.look.y);
        if (ch.w < near && ct.w < near) { hide(); return; }
        if (ch.w < near) ch = mix(ct, ch, (ct.w - near) / (ct.w - ch.w));
        if (ct.w < near) ct = mix(ch, ct, (ch.w - near) / (ch.w - ct.w));
        vec2 halfScreen = 0.5 * f.screen.xy;
        vec2 dir = (ch.xy / ch.w - ct.xy / ct.w) * halfScreen;
        dir = dot(dir, dir) > 1e-6 ? normalize(dir) : vec2(0.0, 1.0);
        vec2 perp = vec2(-dir.y, dir.x);
        vec4 cp = mix(ct, ch, c.y);
        // half width in pixels at this end; at least 0.6 px, the rest goes into the alpha (thin far drops fade, no flicker)
        float truePx = 0.5 * size * f.screen.z * halfScreen.x / cp.w;
        float px = max(truePx, 0.6);
        o_alpha = truePx / px;
        cp.xy += perp * (c.x * px / halfScreen) * cp.w;
        gl_Position = vec4(cp.x, -cp.y, cp.zw);   // D3D9 clip space has y up, Vulkan's viewport y down
        o_depth = cp.w;
        o_kind = 0.0;
        o_local = c;
        return;
    }

    // camera-facing quads: the splash stands on its point (y up from 0), the flake is centred on it
    vec3 right = normalize(vec3(f.camRight.xy, 0.0) + vec3(1e-4, 0.0, 0.0));
    vec3 world;
    if (snow) {
        float s = size * 0.9;
        world = p.a.xyz + (f.camRight.xyz * c.x + f.camUp.xyz * (c.y * 2.0 - 1.0)) * s;
        o_kind = 2.0;
    }
    else {
        // 2026-10-26 run 2: a crown standing on the ground was a sliver seen from above; the splash now faces the camera,
        // centred a little above its point, and reads as an expanding ring with thrown droplets from any angle
        float s = size * mix(3.0, 10.0, sqrt(o_age));
        world = p.a.xyz + vec3(0.0, 0.0, s * 0.3) + (f.camRight.xyz * c.x + f.camUp.xyz * (c.y * 2.0 - 1.0)) * s;
        o_kind = 1.0;
    }
    vec4 cp = project(world);
    if (cp.w < near) { hide(); return; }
    // sub-pixel quads widen to 1.5 px and fade by what they lost
    float truePx = size * f.screen.z * 0.5 * f.screen.x / cp.w;
    o_alpha = clamp(truePx / 1.5, 0.0, 1.0);
    if (truePx < 1.5) {
        vec4 cc = project(p.a.xyz);
        cp.xy = cc.xy + (cp.xy / cp.w - cc.xy / cc.w) * (1.5 / max(truePx, 1e-3)) * cc.w;
        cp.zw = cc.zw;
    }
    gl_Position = vec4(cp.x, -cp.y, cp.zw);
    o_depth = cp.w;
    o_local = c;
}
