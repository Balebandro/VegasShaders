// GPU particles: shading (needs NVR DXVK 1.5-particles, effects/GpuParticles.cpp). Drawn after tonemapping, alpha
// blended over the frame (the frame space is the post-tonemap one, as the screen-space rain's). Lit simply: a drop
// is the scene behind it, brightened, tinted by the sky light; a soft fade where it meets the scene's depth (no hard
// line where a splash or a streak meets the ground) and behind anything nearer.
//
// Build: glslangValidator -V --target-env vulkan1.1 -o GpuParticlesFS.spv GpuParticles.frag
#version 450
#extension GL_GOOGLE_include_directive : enable
#extension GL_EXT_samplerless_texture_functions : enable

layout(set = 1, binding = 0) uniform texture2D t_depth;   // TESR_DepthBuffer (linear depth / far)
layout(set = 1, binding = 1) uniform texture2D t_scene;   // the frame as it is now (TESR_RenderedBuffer)

layout(push_constant) uniform Push {
    vec4 screen;   // x width, y height, z far (units), w opacity
    vec4 tint;     // rgb light tint, a soft fade distance (units)
    vec4 range;    // x fade-out distance (units), y kind of the frame (0 rain, 1 snow)
} pc;

layout(location = 0) in vec2 i_local;
layout(location = 1) in float i_alpha;
layout(location = 2) in float i_depth;
layout(location = 3) flat in float i_kind;
layout(location = 4) flat in float i_age;
layout(location = 0) out vec4 o_color;

void main() {
    ivec2 px = ivec2(gl_FragCoord.xy);
    float scene = texelFetch(t_depth, px, 0).x * pc.screen.z;
    float soft = i_kind > 0.5 ? max(pc.tint.a * 0.25, 1.0) : pc.tint.a;
    float fade = clamp((scene - i_depth) / soft, 0.0, 1.0);          // behind the scene: 0, soft where they meet
    fade *= clamp((i_depth - 12.0) / 40.0, 0.0, 1.0);                // not right on the lens
    fade *= clamp(1.0 - i_depth / pc.range.x, 0.0, 1.0);             // out at the box's reach

    float shape;
    if (i_kind < 0.5) {
        // streak: round across, brightest towards the head, fading to the tail
        shape = (1.0 - i_local.x * i_local.x) * smoothstep(0.0, 0.6, i_local.y);
        shape *= 0.55;
    }
    else if (i_kind < 1.5) {
        // splash crown: thin droplet arcs thrown up and out, spreading and fading over its short life
        // (camera-facing quad, centred: a ring that expands with age, brighter along a few droplet rays)
        vec2 q = vec2(i_local.x, i_local.y * 2.0 - 1.0);
        float r = length(q);
        float ang = atan(q.x, q.y);
        float rays = pow(abs(cos(ang * 3.5 + 1.3)), 6.0);
        float rr = mix(0.35, 0.85, i_age);
        float ring = 1.0 - smoothstep(0.0, 0.18, abs(r - rr));
        float core = (1.0 - smoothstep(0.0, 0.35, r)) * (1.0 - smoothstep(0.0, 0.4, i_age));   // the impact itself
        shape = max(ring * mix(0.5, 1.0, rays), core) * (1.0 - i_age);
    }
    else {
        // snow flake: soft round dot; a settled one fades out over its time on the ground
        vec2 q = vec2(i_local.x, i_local.y * 2.0 - 1.0);
        shape = 1.0 - smoothstep(0.35, 1.0, length(q));
        shape *= 1.0 - smoothstep(0.7, 1.0, i_age);
    }

    float alpha = clamp(shape * fade * i_alpha * pc.screen.w, 0.0, 1.0);
    if (alpha < 1.0 / 512.0) discard;

    vec3 behind = texelFetch(t_scene, px, 0).rgb;
    float brighten = i_kind > 0.5 ? 0.8 : 0.45;                     // snow takes the light, water mostly what is behind it
    vec3 color = mix(behind, pc.tint.rgb, brighten);                  // tint: the sky's light (the fog colour, brightened)
    o_color = vec4(clamp(color, 0.0, 1.0), alpha);
}
