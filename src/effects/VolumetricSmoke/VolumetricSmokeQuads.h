#pragma once

#include <cstdint>
#include <cstring>

// Screen quads for the GunFX volumetric smoke (VolumetricSmoke.fx, step 1): one quad per tube segment that can be on
// screen, covering everything its smoke can reach. Kept free of NVR's globals so the GPU test (work\smoke_device.cpp)
// runs exactly this code.
namespace VolumetricSmokeQuads {
    // Clip position, then the segment's record (the same for all corners; see VolumetricSmoke.fx).
    struct Vertex { float x, y, z, w; float a[4], b[4], c[4], d[4]; };

    // records: `stride` floats per segment (end A x, y, z, radius; end B x, y, z, radius; density A, density B, age,
    // flags; with stride 16 also the noise coordinate at A and B and the age at A and B), positions in NVR's
    // camera-centred world space. viewProj: the scene camera's view * projection for that space.
    // sx, sy: the projection's scale (1 / InvProj._11, 1 / InvProj._22). Smoke closer than nearFade is invisible (the
    // shader fades it), so a segment is cut off there. Writes 6 vertices (two triangles) per kept segment into out and
    // returns how many segments were kept; bounds gets the union of their areas in UV (min u, min v, max u, max v).
    // Optional: a segment whose quad covers more than bigArea of the screen (big and close: huge and soft on screen) goes
    // into outBig instead (its count in keptBig), to be drawn at a lower resolution; between 0.75 and 1.5 times bigArea it
    // goes into both, its density shared between them, so a segment growing or shrinking on screen fades from one to the
    // other instead of popping. area adds up every kept quad's share of the screen (how many screens' worth of pixels the
    // step shades).
    // Optional, thinShare > 0: thinning right in front of the eyes. A segment whose quad covers more than thinShare of the
    // screen is huge and blurred there, and only the sum of many such segments shows. Each one is kept or dropped by a
    // stable pseudo-random draw on its noise coordinate (fixed at the point's birth, so a segment never flips between the
    // two): one in three is kept, its density raised so the expected total stays the same, and the others fade out --
    // fully at twice thinShare, where they are not drawn at all. Records without a noise coordinate (stride 12) draw on
    // their index instead.
    inline int Build(const float* records, int stride, int total, const D3DXMATRIX& viewProj, float sx, float sy, float nearFade,
        Vertex* out, int maxSegments, float bounds[4], Vertex* outBig = nullptr, int* keptBig = nullptr, float bigArea = 2.0f,
        float* area = nullptr, float thinShare = 0.0f) {
        if (keptBig) *keptBig = 0;
        if (area) *area = 0.0f;
        auto lo = [](float p, float q) { return p < q ? p : q; };
        auto hi = [](float p, float q) { return p > q ? p : q; };
        bounds[0] = bounds[1] = 2.0f;
        bounds[2] = bounds[3] = -1.0f;
        int kept = 0;
        for (int i = 0; i < total; i++) {
            const float* r = records + i * stride;
            D3DXVECTOR4 clip[2];
            float radius[2];
            for (int e = 0; e < 2; e++) {
                const D3DXVECTOR4 p(r[e * 4], r[e * 4 + 1], r[e * 4 + 2], 1.0f);
                D3DXVec4Transform(&clip[e], &p, &viewProj);
                radius[e] = r[e * 4 + 3];
            }
            // The Gaussian's reach (2.5 radii, as the shader cuts it), 20% wider for a sphere's outline near the screen
            // edges, where its projection stretches.
            const float reachMax = 3.0f * hi(radius[0], radius[1]);
            if (clip[0].w < nearFade - reachMax && clip[1].w < nearFade - reachMax) continue;   // all too close or behind
            // Cut the segment where it comes closer than nearFade (view depth is linear along it).
            for (int e = 0; e < 2; e++) {
                const int o = 1 - e;
                if (clip[e].w >= nearFade) continue;
                if (clip[o].w <= nearFade) { clip[e] = clip[o]; radius[e] = radius[o]; continue; }
                const float k = (nearFade - clip[e].w) / (clip[o].w - clip[e].w);
                clip[e] = clip[e] + (clip[o] - clip[e]) * k;
                radius[e] = radius[e] + (radius[o] - radius[e]) * k;
            }
            float u0 = 2.0f, v0 = 2.0f, u1 = -1.0f, v1 = -1.0f;
            for (int e = 0; e < 2; e++) {
                const float depth = hi(clip[e].w, 1.0f);
                const float reach = 3.0f * radius[e];
                const float u = 0.5f + 0.5f * clip[e].x / depth, v = 0.5f - 0.5f * clip[e].y / depth;
                const float du = 0.5f * sx * reach / depth, dv = 0.5f * sy * reach / depth;
                u0 = lo(u0, u - du); u1 = hi(u1, u + du);
                v0 = lo(v0, v - dv); v1 = hi(v1, v + dv);
            }
            if (u1 < 0.0f || u0 > 1.0f || v1 < 0.0f || v0 > 1.0f) continue;   // off screen
            u0 = hi(u0, 0.0f); v0 = hi(v0, 0.0f); u1 = lo(u1, 1.0f); v1 = lo(v1, 1.0f);
            const float share = (u1 - u0) * (v1 - v0);
            // Thinning right in front of the eyes (see thinShare above).
            float thin = 1.0f;
            if (thinShare > 0.0f && share > thinShare) {
                float t = (share - thinShare) / thinShare;
                t = t > 1.0f ? 1.0f : t;
                t = t * t * (3.0f - 2.0f * t);
                const float key = stride >= 16 ? r[12] : (float)i;
                uint32_t h;
                std::memcpy(&h, &key, sizeof(h));
                h ^= h >> 16; h *= 0x7feb352du; h ^= h >> 15; h *= 0x846ca68bu; h ^= h >> 16;
                thin = h % 3u == 0u ? 1.0f + 2.0f * t : 1.0f - t;
                if (thin <= 0.0f) continue;                       // dropped: not drawn at all
            }
            bounds[0] = lo(bounds[0], u0); bounds[1] = lo(bounds[1], v0);
            bounds[2] = hi(bounds[2], u1); bounds[3] = hi(bounds[3], v1);
            if (area) *area += share;
            // How much of it goes to the lower resolution (0 none .. 1 all).
            float toBig = 0.0f;
            if (outBig && keptBig) {
                const float lowEdge = 0.75f * bigArea, highEdge = 1.5f * bigArea;
                toBig = share <= lowEdge ? 0.0f : share >= highEdge ? 1.0f : (share - lowEdge) / (highEdge - lowEdge);
                toBig = toBig * toBig * (3.0f - 2.0f * toBig);
            }
            // Two triangles over the area, in clip space (x right, y up).
            const float x0 = 2.0f * u0 - 1.0f, x1 = 2.0f * u1 - 1.0f, y0 = 1.0f - 2.0f * v0, y1 = 1.0f - 2.0f * v1;
            const float corners[6][2] = { { x0, y0 }, { x1, y0 }, { x0, y1 }, { x1, y0 }, { x1, y1 }, { x0, y1 } };
            auto write = [&](Vertex* dest, float share) {
                for (int k = 0; k < 6; k++) {
                    Vertex& v = dest[k];
                    v.x = corners[k][0]; v.y = corners[k][1]; v.z = 0.5f; v.w = 1.0f;
                    for (int j = 0; j < 4; j++) { v.a[j] = r[j]; v.b[j] = r[4 + j]; v.c[j] = r[8 + j]; }
                    v.c[0] *= share; v.c[1] *= share;                   // its density, shared between the two
                    if (stride >= 16) for (int j = 0; j < 4; j++) v.d[j] = r[12 + j];
                    else { v.d[0] = v.d[1] = 0.0f; v.d[2] = v.d[3] = r[10]; }   // older records: no noise coordinate, one age
                }
            };
            if (toBig < 1.0f && kept < maxSegments) { write(out + kept * 6, (1.0f - toBig) * thin); kept++; }
            if (toBig > 0.0f && *keptBig < maxSegments) { write(outBig + *keptBig * 6, toBig * thin); (*keptBig)++; }
        }
        return kept;
    }
}
