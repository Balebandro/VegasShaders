// BloodFluid (BloodFlow v2, needs NVR DXVK): what BloodFluidSim.comp and BloodFluidDraw.frag share.
// src/effects/BloodFluid.{h,cpp} fills these blocks; keep the layouts identical (std140 / std430, vec4 only).
//
// Our own code. The method follows the screen-space fluid idea used by gwater2 (meetric, GPL-3.0; ideas only, no code
// copied) and NVIDIA's "Screen Space Fluid Rendering" (van der Laan, Green, Sainz 2009), plus the 2026-10-22 D3D9 spike
// (work\bench\deep-roadmap\evidence\fluidsim): particles on the GPU, a grid for pressure, cohesion and viscosity,
// collision against the depth buffer, with a ground height to fall back on where the camera cannot see.

// One particle (64 bytes). Positions are relative to an anchor near the camera (BloodFluid.cpp moves it, and every
// particle with it, when the camera goes far), so they keep their precision anywhere in a worldspace.
//  pos.xyz  position (game units)          pos.w  age in seconds (real time) when >= 0; -100 < w < 0: not born yet
//                                                  (counts up to 0, then it starts where it was spawned); <= -1e29: empty
//  vel.xyz  velocity (units/s)             vel.w  flags (below, as a whole number)
//  info.x   fallback ground height         info.y seed 0-1, info.z colour (0xRRGGBB of the game splat's decal colour as a
//           (z, anchor space, under        whole number, 0 none), info.w kind (0 thrown, 1 pool, 2 test throw)
//           the particle: SlopeOf)
//  nrm.xyz  the surface it last lay on     nrm.w  amount (1 = one particle's worth)
struct Particle {
    vec4 pos;
    vec4 vel;
    vec4 info;
    vec4 nrm;
};

const uint FlagContact = 1u;    // on a surface this step
const uint FlagTouched = 2u;    // has been on a surface at least once (thrown blood in the air has not)
const uint FlagGround  = 4u;    // lying on the fallback ground (pools under a body, drops that fell where nothing is seen)
const uint FlagFrozen  = 8u;    // on a surface the camera cannot see now: held where it is until it can be seen again
const uint FlagSettled = 16u;   // dried and still: no more motion, drawn as a stain
// UNOFFICIAL 2026-11-01 SteadyContact ([Shaders.BloodFlow.Fluid] SteadyContact; user: wet pools flicker until they settle; the report
// bursts: the film's strength followed the frame's sim step count, 1 or 2 at ~73 fps, a 2, 2, 1 cycle). A crowded wet particle is pushed
// off the surface by its neighbours' pressure and pulled back by gravity: FlagContact (this step) came and went every other step, and
// each time it went the particle counted as a drop in the air, which is drawn for a share of the particles only (Drops), so the film
// thinned or thickened with the step the frame ended on. Settled blood never lost it. Bits 5-6: steps left since its last contact
// (3 on contact, one less each step without); the tiles count a particle with some left as lying on its surface (StainOf).
const uint FlagRecentShift = 5u;
const uint FlagRecentMask  = 96u;
// Lying on something (now, or held, settled, on the fallback ground, or with SteadyContact within the last steps): a stain on that surface.
bool StainOf(uint flags, vec4 nrm, bool steady) {
    bool lying = (flags & (FlagContact | FlagFrozen | FlagSettled | FlagGround)) != 0u || (steady && (flags & FlagRecentMask) != 0u);
    return lying && dot(nrm.xyz, nrm.xyz) > 0.5;
}

const float DeadAge = -1e30;

// Flags, kinds and colours are kept as whole numbers in float fields (exact below 2^24; raw bit casts of small integers
// would be denormals, which a GPU may flush to zero).
uint Flags(float w) { return uint(w + 0.5); }

// UNOFFICIAL 2026-10-27 (user: "the weird half circles"): a pool's fallback ground follows the slope of the ground under the body
// (BloodFlow.cpp GroundUnderBones; BloodFluid.cpp Offer). The slope rides in the flags word above the flag bits: dz/dx in bits
// 8-15 and dz/dy in bits 16-23, each a signed byte in 1/64 steps (no bits: level; the word stays below 2^24, exact in the float).
// info.x is then the plane's height under the particle where it is now (Integrate moves it along with the particle).
uint PackSlope(vec2 g) {
    ivec2 q = ivec2(round(clamp(g, -1.0, 1.0) * 64.0));
    return ((uint(q.x) & 255u) << 8) | ((uint(q.y) & 255u) << 16);
}
vec2 SlopeOf(uint flags) {
    ivec2 q = ivec2(int((flags >> 8) & 255u), int((flags >> 16) & 255u));
    q -= ivec2(greaterThan(q, ivec2(127))) * 256;
    return vec2(q) / 64.0;
}

// What the draw pass gets per particle on screen (48 bytes, written by the tiles pass).
//  a.xyz eye-relative position, a.w radius (> 0: a stain lying on the surface nrm; < 0: a drop in the air, a sphere)
//  b.xyz the surface's normal (stains), b.w age for the look (BloodFlow seconds: real time x TunedClockRate)
//  c.x colour, c.y amount (fading out at the end of its life), c.z kind, c.w flags (whole numbers, as in Particle)
struct DrawRec {
    vec4 a;
    vec4 b;
    vec4 c;
};

layout(set = 2, binding = 0, std140) uniform Frame {
    vec4  eyeRel;        // xyz: the camera in the particles' space (eye - anchor); w: farZ
    vec4  right;         // xyz: camera right (world); w: projection _11
    vec4  up;            // xyz: camera up; w: projection _22
    vec4  fwd;           // xyz: camera forward; w: nearZ
    vec4  screen;        // width, height, 1/width, 1/height
    vec4  sim;           // x: step (s), y: gravity (units/s^2), z: particle radius, w: steps this frame
    vec4  fluid;         // x: pressure, y: cohesion, z: viscosity, w: rest density (grid weight per cell)
    vec4  wall;          // x: film friction, y: cling (adhesion), z: cling range, w: collide window behind the surface
    vec4  settle;        // x: settle start (s), y: settle length (s), z: drag, w: splash
    vec4  gridf;         // x: cell size, y: 1 / cell size, z: top speed, w: lifetime (s)
    uvec4 counts;        // x: capacity, y: slots in use, z: tiles across, w: tiles down
    uvec4 counts2;       // x: particles per tile, y: grid table mask, z: debug view, w: emitters this frame
    vec4  look;          // x: stain radius, y: drop radius, z: threshold, w: bump
    vec4  look2;         // x: look clock rate (TunedClockRate), y: splat dry time, z: pool dry time, w: gloss
    vec4  viewRot0;      // rows of the view matrix's rotation: normals buffer (view space) -> world (Normals.hlsl GetWorldNormal)
    vec4  viewRot1;
    vec4  viewRot2;
    vec4  fresh;         // rgb: fresh blood colour (linear)
    vec4  dryColor;      // rgb: dried blood colour (linear)
    vec4  match;         // x: splatter grey (sRGB), y: brightness, z: 1 the colours are matched, w: 1 outdoors
    vec4  depthConsts;   // TESR_DepthConstants (z: inverted view-model depth)
    vec4  sunDir;
    vec4  sunColor;
    vec4  sunAmbient;
    vec4  horizon;
    vec4  lightPos[12];        // TESR_LightPosition
    vec4  shadowLightPos[12];  // TESR_ShadowLightPosition
    vec4  lightColor[24];      // TESR_LightColor
    // UNOFFICIAL 2026-10-27 (user: grey, almost reflective blood; "orbs" out of NPCs when hit):
    vec4  look3;         // TESR_BloodFlowLook (BloodFlow.fx.hlsl): x sheen x, y Fresnel cap, z sheen tint, w matched colour floor (linear)
    vec4  rework;        // x: 1 splat blood born on its surface, pools lying on the ground (0: thrown at it / born in the air, before);
                         // y: share of the drops in the air drawn; z: 1 the field's slope bends the normal mostly at its edges;
                         // w: the drops' glints x (OldThrow: 0, 1, 0, 1)
    // UNOFFICIAL 2026-10-27 evening (user: "the color of the fluid simulation ones seems pretty off, almost looks like ketchup";
    // "full bright as though it does not respond to ... lighting"):
    vec4  look4;         // x: 1 the fluid's own look (BloodFluidDraw.frag: a film over the image, as the game's decals), 0 as before
                         // (OldColour); y: how much thin blood follows the ground's texture (TextureRelief)
    uvec4 tiles2;        // UNOFFICIAL 2026-11-01 SteadyTiles (BloodFluidSim.comp TilesFill): x 1 = on, y where the fill counters start in
                         // TileCount (tiles across x down of the buffer), z 1 = SteadyContact (StainOf), w 0
    vec4  floorFix;      // UNOFFICIAL 2026-11-01 SteadyFloor (BloodFluidSim.comp): xy this frame's depth jitter as a uv offset (TAAEffect::
                         // DepthJitterUV: a depth pixel at uv lies on the ray through uv + xy), z 1 = SteadyFloor, w the rest speed (units/s)
} F;

// Integer hashes (the same everywhere: the simulation must not depend on the order threads run in).
uint hashU(uint x) {
    x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16;
    return x;
}
float hash01(uint x) { return float(hashU(x) & 0xFFFFFFu) / 16777216.0; }

// Eye-relative position -> pixel (x, y) and view depth (z); z <= 0 behind the camera.
vec3 ProjectRel(vec3 rel) {
    float z = dot(rel, F.fwd.xyz);
    if (z <= 0.5) return vec3(-1.0, -1.0, z);
    float nx = dot(rel, F.right.xyz) * F.right.w / z;
    float ny = dot(rel, F.up.xyz) * F.up.w / z;
    return vec3((nx * 0.5 + 0.5) * F.screen.x, (0.5 - ny * 0.5) * F.screen.y, z);
}

// The eye ray through pixel centre px (eye-relative, scaled so that its view depth is 1: Depth.hlsl toWorld).
vec3 RayAt(vec2 px) {
    vec2 uv = px * F.look4.zw;   // (2026-10-29: 1 / the drawn part's size; screen.zw is 1 / the textures' size, for UVs: they differ in RenderScale frames)
    return F.fwd.xyz + F.right.xyz * ((2.0 * uv.x - 1.0) / F.right.w) - F.up.xyz * ((2.0 * uv.y - 1.0) / F.up.w);
}

// Helpers.hlsl's (as Common.glsl, which is not included: it declares the sampler heap, and these passes use none).
vec3 linearize(vec3 c) {
    vec3 lo = c / 12.92;
    vec3 hi = pow(abs((c + 0.055) / 1.055), vec3(2.4));
    return mix(hi, lo, lessThanEqual(c, vec3(0.04045)));
}
vec3 delinearize(vec3 c) {
    vec3 lo = c * 12.92;
    vec3 hi = (pow(abs(c), vec3(1.0 / 2.4)) * 1.055) - 0.055;
    return mix(hi, lo, lessThanEqual(c, vec3(0.0031308)));
}
float luma(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }
float saturate(float x) { return clamp(x, 0.0, 1.0); }
vec3 saturate(vec3 x) { return clamp(x, vec3(0.0), vec3(1.0)); }

// BloodFlow.fx.hlsl's view-model test (first-person arms and weapon): true where the view model is drawn.
bool IsViewModel(float vm) {
    return (F.depthConsts.z == 0.0 && vm < 0.9) || (F.depthConsts.z > 0.0 && vm > 0.01);
}
