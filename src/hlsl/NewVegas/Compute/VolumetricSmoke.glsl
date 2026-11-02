// VolumetricSmoke (GunFX's smoke) as Vulkan passes (needs NVR DXVK): what the passes of Effects/VolumetricSmoke.fx.hlsl share,
// line for line. The passes are the haze (VolumetricSmokeHaze.frag), the room light (VolumetricSmokeEnv.frag) and the composite
// (VolumetricSmokeComposite.frag); the segments (geometry) stay D3D9. effects/VolumetricSmoke.cpp (FillExtConstants, ExtRender).
// The matrices arrive as the D3D9 effect uploads them (SetMatrix keeps the logical matrix), so HLSL mul(v, M) is M * v here, and
// M[i][j] is the same element. One uniform block for every pass. Each pass includes Common.glsl, declares its textures, defines
// the parts it uses (VS_DEPTH, VS_SMOKE, VS_NOISE) and then includes this file. Screen coordinates: VPOS is floor(gl_FragCoord.xy),
// uv is gl_FragCoord.xy / the screen size (the same as the frame quad's uv at each pixel centre; a pass clipped to a rectangle
// has no other way to get it).

layout(set = 2, binding = 0, std140) uniform Constants {
    mat4 invView;               // TESR_InvViewTransform
    mat4 invProjection;         // TESR_InvProjectionTransform
    mat4 projection;            // TESR_ProjectionTransform
    vec4 reciprocalResolution;  // TESR_ReciprocalResolution
    vec4 sunDirection;          // TESR_SunDirection
    vec4 sunColor;              // TESR_SunColor
    vec4 sunAmbient;            // TESR_SunAmbient
    vec4 cameraData;            // TESR_CameraData
    vec4 cameraPosition;        // TESR_CameraPosition
    vec4 smokeInfo;             // SmokeInfo
    vec4 smokeParams;           // SmokeParams
    vec4 smokeFlash;            // SmokeFlash
    vec4 smokeFlashColor;       // SmokeFlashColor
    vec4 smokeLook;             // SmokeLook (the haze pass sets its own)
    vec4 hazeInfo;              // HazeInfo
    vec4 bulletMove;            // BulletMove
    vec4 envInfo;               // EnvInfo
    vec4 hazeShape[40];         // HazeShape
    vec4 hazeLook[40];          // HazeLook
    vec4 bulletFrom[16];        // BulletFrom
    vec4 bulletWay[16];         // BulletWay
} cb;

layout(push_constant) uniform Push {
    uint pointIndex;            // 0: POINT, CLAMP (the depth buffers, the frame, the room light, the cut)
    uint linearIndex;           // 4: LINEAR, CLAMP (the smoke buffers, the haze)
    uint wrapIndex;             // 8: LINEAR, WRAP, mips (the 3D noise)
    uint pad;
} pk;

#define POINT_LOD(t, coord) textureLod(sampler2D(t, NVR_SAMPLER(pk.pointIndex)), (coord), 0.0)
#define LINEAR_LOD(t, coord) textureLod(sampler2D(t, NVR_SAMPLER(pk.linearIndex)), (coord), 0.0)
#define NOISE_LOD(coord, lod) textureLod(sampler3D(t_noise, NVR_SAMPLER(pk.wrapIndex)), (coord), (lod))

#define TESR_ReciprocalResolution (cb.reciprocalResolution)
#define TESR_SunDirection (cb.sunDirection)
#define TESR_SunColor (cb.sunColor)
#define TESR_SunAmbient (cb.sunAmbient)
#define TESR_InvViewTransform (cb.invView)
#define TESR_InvProjectionTransform (cb.invProjection)
#define TESR_ProjectionTransform (cb.projection)
#define TESR_CameraData (cb.cameraData)
#define TESR_CameraPosition (cb.cameraPosition)
#define SmokeInfo (cb.smokeInfo)
#define SmokeParams (cb.smokeParams)
#define SmokeFlash (cb.smokeFlash)
#define SmokeFlashColor (cb.smokeFlashColor)
#define SmokeLook (cb.smokeLook)
#define HazeShape (cb.hazeShape)
#define HazeLook (cb.hazeLook)
#define HazeInfo (cb.hazeInfo)
#define BulletFrom (cb.bulletFrom)
#define BulletWay (cb.bulletWay)
#define BulletMove (cb.bulletMove)
#define EnvInfo (cb.envInfo)
#define nearZ (cb.cameraData.x)
#define farZ (cb.cameraData.y)

// The light the smoke is lit by: the sun outdoors, from above indoors.
vec3 LightDirection() { return SmokeInfo.z > 0.5 ? normalize(TESR_SunDirection.xyz) : vec3(0.0, 0.0, 1.0); }

#ifdef VS_DEPTH
// Includes/Depth.hlsl, the parts used here (TESR_DepthBuffer as t_depth, POINT). The depth buffer has no mip levels, so the
// HLSL tex2D and tex2Dlod versions read the same texels: one function serves both.
float projectedDepthFromLinear(float linear01) {
    float viewZ = max(linear01 * farZ, 1.0e-6);
    return nearZ * (farZ - viewZ) / (viewZ * (farZ - nearZ));
}

vec4 reconstructWorldPosition(vec2 uv, out float viewDepth) {
    float x = uv.x * 2.0 - 1.0;
    float y = (1.0 - uv.y) * 2.0 - 1.0;
    float z = projectedDepthFromLinear(POINT_LOD(t_depth, uv).x);
    vec4 viewSpace = TESR_InvProjectionTransform * vec4(x, y, z, 1.0);
    viewSpace /= viewSpace.w;
    viewDepth = viewSpace.z;
    vec4 worldSpace = TESR_InvViewTransform * viewSpace;
    return vec4(worldSpace.xyz, 1.0);
}
#endif

#ifdef VS_SMOKE
// VolumetricSmoke.fx.hlsl SmokeAt, SmokeAtLod: all the smoke step 1 added up at a point (normal and big segments).
vec4 SmokeAt(vec2 uv) { return LINEAR_LOD(t_smoke, uv) + LINEAR_LOD(t_low, uv); }
vec4 SmokeAtLod(vec2 uv) { return LINEAR_LOD(t_smoke, uv) + LINEAR_LOD(t_low, uv); }
#endif

#ifdef VS_NOISE
// The 3D noise's 4-octave fbm, at a coarser mip level (lod).
float noiseFbmLod(vec3 x, float lod) { return NOISE_LOD(x / 4.0, lod).g; }
#endif

// ---- Bullet holes (VolumetricSmoke.fx.hlsl BulletHole, BulletField, BlobStretch) -----------------------------------------
const float BulletBehind = 30.0;

float BulletHole(int j, vec3 cam, vec3 dir, float sceneDist, out float t0, out float t1, out float tPuff) {
    vec4 from = BulletFrom[j], way = BulletWay[j];
    float since = from.w;
    float open = HazeInfo.z > 0.0 ? HazeInfo.z : 0.05, life = HazeInfo.w > 0.0 ? HazeInfo.w : 1.2;
    float closeFrom = max(open, 0.1 * life);
    float closing = saturate((since - closeFrom) / max(life - closeFrom, 0.05));
    float opened = saturate(since / open);
    float rt = (6.0 + 3.0 * min(way.w, 3.0)) * opened * (2.0 - opened) * (1.0 - closing * sqrt(closing));
    vec3 w = cam - from.xyz;
    float wb = dot(w, way.xyz), db = dot(dir, way.xyz);
    vec3 wp = w - way.xyz * wb, dp = dir - way.xyz * db;
    float A = dot(dp, dp), B = 2.0 * dot(wp, dp), C = dot(wp, wp);
    float ta = 6.0, tb = sceneDist;
    float u0 = (-BulletBehind - wb) / (abs(db) > 0.00001 ? db : 0.00001), u1 = (1500.0 - wb) / (abs(db) > 0.00001 ? db : 0.00001);
    ta = max(ta, min(u0, u1)); tb = min(tb, max(u0, u1));
    float tn = clamp(-B / (2.0 * max(A, 0.000001)), ta, tb);
    vec3 off = wp + dp * tn;
    float dmin = length(off);
    float hole = 0.0;
    tPuff = db > 0.00001 ? (35.0 - wb) / db : 0.0;
    t0 = 1.0e9; t1 = -1.0e9;
    if (rt >= 0.3 && way.w > 0.0 && tb > ta && dmin < 1.7 * rt) {
        vec3 e1 = normalize(cross(way.xyz, abs(way.z) < 0.9 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0)));
        vec3 e2 = cross(way.xyz, e1);
        vec2 z = vec2(dot(off, e1), dot(off, e2)) / max(dmin, 0.001);
        vec2 z2 = vec2(z.x * z.x - z.y * z.y, 2.0 * z.x * z.y);
        vec2 z3 = vec2(z2.x * z.x - z2.y * z.y, z2.x * z.y + z2.y * z.x);
        vec2 z5 = vec2(z3.x * z2.x - z3.y * z2.y, z3.x * z2.y + z3.y * z2.x);
        float seed = fract((SmokeParams.w - since) * 0.6180339) * 97.0;
        vec3 phase = seed * vec3(1.0, 1.7, 2.3) + (wb + db * tn) * vec3(0.0012, 0.0018, 0.0026) + since * vec3(0.9, -1.3, 1.7);
        vec3 ps = sin(phase), pcs = cos(phase);
        float odd = smoothstep(0.0, 0.5, dmin / max(rt, 0.001));
        float ragged = 0.42 * (z2.y * pcs.x + z2.x * ps.x) + odd * (0.33 * (z3.y * pcs.y + z3.x * ps.y) + 0.25 * (z5.y * pcs.z + z5.x * ps.z));
        float rn = rt * (1.0 + 0.35 * ragged);
        float x = dmin / rn;
        float k = exp(-2.0 * x * x) * saturate(way.w);
        float ta2 = ta, tb2 = tb;
        float R2 = 1.5625 * rn * rn;
        float disc = B * B - 4.0 * A * (C - R2);
        float sq = sqrt(max(disc, 0.0));
        float h0 = A > 0.000001 ? max(ta2, (-B - sq) / (2.0 * A)) : ta2;
        float h1 = A > 0.000001 ? min(tb2, (-B + sq) / (2.0 * A)) : (C < R2 ? tb2 : ta2);
        k *= smoothstep(0.3, 3.0, rt);
        if ((A <= 0.000001 || disc > 0.0) && h1 > h0 && k > 0.0) { t0 = h0; t1 = h1; hole = k; }
    }
    return hole;
}

float BulletField(vec3 cam, vec3 dir, float sceneDist, out float t0, out float t1, out float tPuff) {
    float total = 0.0, weight = 0.0, fromSum = 0.0, toSum = 0.0, puff = 0.0;
    for (int j = 0; j < 16; j++) {
        if (float(j) >= HazeInfo.y) break;
        float h0, h1, hp;
        float c = BulletHole(j, cam, dir, sceneDist, h0, h1, hp);
        if (c > 0.0) { float w = c * c * c; total += c; weight += w; fromSum += w * h0; toSum += w * h1; puff += w * hp; }
    }
    t0 = weight > 0.0 ? fromSum / weight : 1.0e9;
    t1 = weight > 0.0 ? toSum / weight : -1.0e9;
    tPuff = weight > 0.0 ? puff / weight : 0.0;
    return total > 0.0 ? smoothstep(0.07, 0.375, total) : 0.0;
}

float BlobStretch(float s, float kk, float iR2) {
    return s * (kk * kk - s * s * iR2 * (0.6666667 * kk - 0.2 * s * s * iR2));
}
