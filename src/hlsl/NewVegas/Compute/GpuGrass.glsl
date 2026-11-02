// GPU grass (needs NVR DXVK 1.7-grass: compute + geometry passes drawn into the scene; effects/GpuGrass.cpp). Shared by
// GpuGrassCull.comp (one thread per clump: places the strands of every grass tuft the game placed, culls and counts them) and
// GpuGrass.vert (builds each visible strand from its record). Docs: docs/next/FORK2-GRASS.md.
//
// Space: CAMERA-RELATIVE world units (the camera at 0, z up), so the numbers stay small. The game's grass instance positions are
// world positions (TallGrassShaderProperty::AddGrass); rows[] is the game's own grass ModelViewProj (c9-c12 of its grass vertex
// shader) with the camera's position folded into w on the CPU (in double), so clip = rows . (rel, 1) is exactly the game's clip.

struct GGDraw {         // one game grass draw the strands replace (std430, 128 bytes)
    vec4 shape;         // x plant height (units, the card's opaque top, before the tuft's size step), y card half width, z sway weight at the
                        //   root, w sway weight per unit of height (the card's vertex alpha^2 against height, NewGrass's fit)
    vec4 scaleMask;     // xyz the draw's ScaleMask (c2: which axes the size step scales), w flags (1 = only the tufts before the fade
                        //   start: GrassFirst part 1 of a mixed draw; 2 = the strands grow along the slope normal: the LFS card variant)
    vec4 sun;           // rgb the draw's sun colour (c1), w share of the strands (a sparse card gets fewer, down to half)
    vec4 ambient;       // rgb the draw's ambient colour (c6), w the card's opaque area (square units at size step 0)
    vec4 ramp[4];       // rgb the card's colour per height band (root -> tip, gamma, as the texture), w its silhouette edge there (share of
                        //   the half width)
};

struct GGBlade {        // one visible strand (std430, 48 bytes)
    vec4 a;             // xyz root (camera-relative), w width at the root (units)
    vec3 tip;           // tip - root
    uint packed;        // draw index (12 bits) | facing angle (6) | seed (6) | length share of the plant (8)
    vec3 ctrl;          // Bezier control point - root
    uint light;         // packHalf2x16(light scale, sun amount)
};

#define GG_FRAME_BLOCK \
    vec4 rows[4];   /*   0: the game's grass ModelViewProj rows, camera folded in: clip.i = dot(rows[i].xyz, rel) + rows[i].w */ \
    vec4 cam;       /*  64: xyz camera world position, w tufts this frame */ \
    vec4 sunDir;    /*  80: xyz DiffuseDir (c0), w the grass dimmer (c7.x) */ \
    vec4 wind;      /*  96: the game's WindData (c4): xy sway axis, z sway size, w phase */ \
    vec4 fade;      /* 112: x fade start, y fade range (c5.zw), z clumps per tuft (Density), w extra clump radius (units) */ \
    vec4 fogParam;  /* 128: c15 */ \
    vec4 fogColor;  /* 144: c14 */ \
    vec4 lod;       /* 160: x LodNear, y 1 / (LodFar - LodNear), z strands per clump near, w far */ \
    vec4 shape;     /* 176: x HeightScale, y Coverage, z LengthMin, w Spread */ \
    vec4 shape2;    /* 192: x TipReach, y Kink, z Droop, w Bend */ \
    vec4 shape3;    /* 208: x Flutter, y the narrowest strand (pixels), z units per pixel at distance 1, w DetailDistance (near / far list) */ \
    vec4 lanes;     /* 224: the field wind's lanes (GrassWind.h; all 0 = off: the game's sway) */ \
    vec4 misc;      /* 240: x the field wind's clock, y strands per list (capacity), z debug view, w 0 */ \
    vec4 look;      /* 256: x Variation, y TipLift, z Saturation, w 0 */ \
    vec4 planes[4]; /* 272: the frustum's side planes, camera-relative (xyz normal, w distance), normalized */

uint ggHash(uint x) {   // PCG-style integer hash (as the GPU particles')
    x = x * 747796405u + 2891336453u;
    uint w = ((x >> ((x >> 28u) + 4u)) ^ x) * 277803737u;
    return (w >> 22u) ^ w;
}
// A stable seed from a world position (the tuft's: strands keep their place when the engine rebuilds a group) and a salt.
uint ggSeed(vec2 p, uint salt) { return ggHash(floatBitsToUint(p.x) ^ ggHash(floatBitsToUint(p.y) ^ ggHash(salt))); }
float ggRand(inout uint s) { s = ggHash(s); return float(s & 0x00FFFFFFu) / 16777216.0; }

// The field wind (core/GrassWind.h, Includes/GrassWind.hlsl GrassWindMove, line for line): the game's sway turned onto the wind,
// the gust lean and the flutter, times the game's sway size and the point's weight; z drops as the strand leans (bends, not stretches).
vec3 ggWindMove(vec4 inst, vec4 lanes, float clock, float swing, vec4 windData, float weight, float height) {
    vec2 dir = lanes.xy * inversesqrt(dot(lanes.xy, lanes.xy));
    float gustPhase = clock * 0.25;
    float along = dot(inst.xy, lanes.xy);
    float front = 0.5 + 0.5 * sin(gustPhase - along);
    float patchWave = 0.75 + 0.25 * sin(gustPhase - 0.5 * (along + inst.y * lanes.x - inst.x * lanes.y));   // ('patch' is a GLSL keyword)
    float fa = 2.0 * clock + inst.w + (inst.x - inst.y) * 0.0078125;
    vec2 flutter = vec2(sin(fa), cos(fa));
    float alongWind = swing * length(windData.xy) + lanes.z * front * front * patchWave + lanes.w * flutter.x;
    vec2 xy = (dir * alongWind + vec2(-dir.y, dir.x) * (lanes.w * 0.5 * flutter.y)) * (windData.z * weight);
    float h = max(height, 1e-6);
    return vec3(xy, -min(dot(xy, xy) * 0.5 / h, h));
}
