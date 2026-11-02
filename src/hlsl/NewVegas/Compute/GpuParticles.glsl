// GPU particles (needs NVR DXVK 1.5-particles: compute + the geometry passes). Shared by GpuParticlesSim.comp,
// GpuParticles.vert and GpuParticles.frag. effects/GpuParticles.cpp fills the frame block and dispatches/draws.
//
// Space: positions are EYE-RELATIVE world units (the camera at 0, z up, as the game's world). Each frame the
// simulation subtracts how far the camera moved (camDelta), so the drops stay put in the world while the numbers
// stay small. viewProj projects "eye-relative + origin" (NVR's camera-centred space, as BloodFlow/VolumetricSmoke);
// orthoProj (the rain's top-down occlusion map, ShadowCameraToLight of MapOrtho) takes eye-relative positions,
// as Precipitations.frag uses it. Both matrices arrive as D3DX memory: HLSL mul(v, M) is M * v here.

struct Particle {
    vec4 a;   // xyz position (eye-relative), w state: < 0 falling, >= 0 seconds since it landed
    vec4 b;   // xyz velocity (units/s), w random seed in (0, 1); 0 = never initialised
};

#define GP_FRAME_BLOCK \
    mat4 viewProj;    /* 0   */ \
    mat4 orthoProj;   /* 64  */ \
    vec4 origin;      /* 128: xyz camera-centred origin, w far plane (units) */ \
    vec4 camDelta;    /* 144: xyz camera movement since last frame, w dt (s) */ \
    vec4 box;         /* 160: x half width (radius), y top above the eye, z bottom below the eye, w fall speed (units/s) */ \
    vec4 sim;         /* 176: x particle count, y frame seed, z landed life (s), w mode (0 rain, 1 snow) */ \
    vec4 look;        /* 192: x size (units), y streak length (s of motion), z opacity, w flags (1 splashes, 2 ortho map, 4 reset) */ \
    vec4 screen;      /* 208: x width, y height, z projection _11, w projection _22 */ \
    vec4 light;       /* 224: rgb light tint (frame space), a time (s) */ \
    vec4 camRight;    /* 240: xyz camera right (world) */ \
    vec4 camUp;       /* 256: xyz camera up (world) */ \
    vec4 camFwd;      /* 272: xyz camera forward (world), w near plane */

const float GP_SPLASHES = 1.0;
const float GP_ORTHO = 2.0;
const float GP_RESET = 4.0;

bool gpFlag(float flags, float bit) { return mod(floor(flags / bit), 2.0) > 0.5; }

uint gpHash(uint x) {   // PCG-style integer hash
    x = x * 747796405u + 2891336453u;
    uint w = ((x >> ((x >> 28u) + 4u)) ^ x) * 277803737u;
    return (w >> 22u) ^ w;
}
float gpRand(inout uint state) { state = gpHash(state); return float(state & 0x00FFFFFFu) / 16777216.0; }
