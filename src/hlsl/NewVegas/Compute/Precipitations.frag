// Precipitations (rain) as a Vulkan pass (needs NVR DXVK): Effects/Precipitations.fx.hlsl line for line. effects/Rain.cpp ExtRender.
// The matrices arrive as the D3D9 effect uploads them, so HLSL mul(v, M) is M * v here, and M[i][j] is the same element.
// fmod is x - y*trunc(x/y); smoothstep with edge0 > edge1 is written out with the HLSL formula (GLSL leaves it undefined).
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "Common.glsl"

layout(set = 1, binding = 0) uniform texture2D t_source;   // TESR_SourceBuffer (LINEAR, CLAMP)
layout(set = 1, binding = 1) uniform texture2D t_depth;    // TESR_DepthBuffer (LINEAR, CLAMP)
layout(set = 1, binding = 2) uniform texture2D t_ortho;    // TESR_OrthoMapBuffer (LINEAR, CLAMP)
layout(set = 1, binding = 3) uniform texture2D t_bloom;    // TESR_BloomBuffer (LINEAR, CLAMP)

layout(push_constant) uniform Push {
    mat4 shadowCameraToLight;  // 0: TESR_ShadowCameraToLightTransformOrtho
    mat4 viewTransform;        // 64: TESR_ViewTransform
    vec4 viewScale;            // 128: x TESR_ProjectionTransform[0][0], y TESR_ProjectionTransform[1][1], z TESR_CameraData.y (far), w TESR_GameTime.z
    vec4 rainData;             // 144: TESR_RainData (x amount, y vertical scale, z speed, w opacity)
    vec4 rainAspect;           // 160: TESR_RainAspect (x refraction, y colour, w SkipHiddenLayers)
    vec4 sunDirection;         // 176: TESR_SunDirection
    vec4 sunColor;             // 192: TESR_SunColor
    uint samplerIndex;         // 208: LINEAR, CLAMP
} pc;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o_color;

const int RainLayers = 15;
const float DEPTH = 100.0;	// distance step between each layer
const float PI = 3.1415926538;
const mat3 p = mat3(30.323122, 30.323122, 30.323122, 30.323122, 30.323122, 30.323122, 30.323122, 30.323122, 30.323122);

float fmodh(float x, float y) { return x - y * trunc(x / y); }
vec2 fmodh(vec2 x, float y) { return x - y * trunc(x / y); }

float smoothstepHlsl(float a, float b, float x) {
    float t = clamp((x - a) / (b - a), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

float readDepth(vec2 coord) {
    return texture(sampler2D(t_depth, NVR_SAMPLER(pc.samplerIndex)), coord).x * pc.viewScale.z;
}

// convert a screen position to a world direction
vec3 toWorld(vec2 tex) {
    vec3 v = vec3(pc.viewTransform[0][2], pc.viewTransform[1][2], pc.viewTransform[2][2]);
    v += (1.0 / pc.viewScale.x * (2.0 * tex.x - 1.0)) * vec3(pc.viewTransform[0][0], pc.viewTransform[1][0], pc.viewTransform[2][0]);
    v += (-1.0 / pc.viewScale.y * (2.0 * tex.y - 1.0)) * vec3(pc.viewTransform[0][1], pc.viewTransform[1][1], pc.viewTransform[2][1]);
    return v;
}

// convert world position to a coordinate on a cylinder around the player
vec2 cylindrical(vec3 world) {
    float u = -atan(world.y, world.x) / PI;
    float v = world.z / length(world.xy);
    return vec2(u * 0.5 + 0.5, 0.004 * pc.rainData.y * v);
}

// checks wether a point is underneath something occluding
float getOrtho(vec4 orthoPos) {
    orthoPos.xyz /= orthoPos.w;
    float outofbounds = (orthoPos.x < -1.0 || orthoPos.x > 1.0 ||
        orthoPos.y < -1.0 || orthoPos.y > 1.0 ||
        orthoPos.z < 0.0 || orthoPos.z > 1.0) ? 1.0 : 0.0;

    orthoPos.x = orthoPos.x * 0.5 + 0.5;
    orthoPos.y = orthoPos.y * -0.5 + 0.5;
    float ortho = textureLod(sampler2D(t_ortho, NVR_SAMPLER(pc.samplerIndex)), orthoPos.xy, 0.0).r;
    return float(ortho > orthoPos.z - 0.0001) * (1.0 - outofbounds);
}

void main() {
    vec4 color = linearize(texture(sampler2D(t_source, NVR_SAMPLER(pc.samplerIndex)), uv));

    // calculating the ray along which the volumetric rain will be calculated
    vec3 rayStart = toWorld(uv);
    vec3 rayEnd = rayStart * DEPTH * float(RainLayers);
    vec4 orthoStart = pc.shadowCameraToLight * vec4(rayStart.xyz, 1.0);
    vec4 orthoEnd = pc.shadowCameraToLight * vec4(rayEnd.xyz, 1.0);
    vec4 orthoStep = (orthoEnd - orthoStart) / float(RainLayers);

    // each iteration adds a cylindrical layer of drops
    float depth = readDepth(uv);
    vec2 cyl = cylindrical(rayStart.xyz);
    float timetick = pc.viewScale.w;
    float speed = pc.rainData.z;

    float totalRain = 0.0;
    for (int i = 1; i < RainLayers; i++) {
        // a layer behind the pixel adds exactly 0 (the drop is multiplied by (depth > DEPTH * i) below), so its work is skipped
        if (depth > DEPTH * float(i) || pc.rainAspect.w < 0.5) {
            vec2 noiseSurfacePosition = cyl * (1.0 + float(i) * DEPTH); // scale cylinder coordinates with iterations
            noiseSurfacePosition += vec2(noiseSurfacePosition.y * (fmodh(float(i) * 107.238917, 1.0) - 0.5), speed * timetick); // animate y offset

            // creating the noise with magic
            vec3 m = vec3(floor(noiseSurfacePosition), float(i));
            vec3 mp = m / max(fract(p * m), vec3(1e-4));
            vec3 rainNoise = fract(mp); // r.x is a random generated grid of different darknesses

            // determin drop shape
            vec2 shape = abs(fmodh(noiseSurfacePosition, 1.0) + rainNoise.xy - 1.0);
            shape += 0.01 * abs(2.0 * fract(10.0 * noiseSurfacePosition.yx) - 1.0); // shape is a horizontal gradient on each square
            float dropGradient = 0.6 * max(shape.x - shape.y, shape.x + shape.y) + max(shape.x, shape.y) - 0.01; // circular gradient on each square
            float edge = 0.005 + 0.05 * min(1.0 * float(i), 4.0 * pc.rainData.x); // can't be 0 or smoothstep will fail

            // add new drop influence to the fragment, by extracting the brightest spots in the gradients grid using the edge parameter
            float drop = smoothstepHlsl(edge, -edge, dropGradient) * (rainNoise.x / (1.0 + 0.2 * float(i))); // fade opacity with each iteration
            drop *= getOrtho(orthoStart + orthoStep * float(i)) * float(depth > DEPTH * float(i)); // depth and ortho check for rain
            totalRain += drop;
        }
    }

    // a rain tint color that scales with the sun direction
    vec4 sunColorLinear = linearize(pc.sunColor);
    vec4 rainColor = mix(vec4(0.5), sunColorLinear * 2.0, pow(saturate(dot(normalize(rayStart.xyz), pc.sunDirection.xyz)), 2.0));

    // sample the bloom buffer and the source buffer with refracted UV to shade the rain with
    vec2 refractedUV = uv + vec2(totalRain * pc.rainAspect.x, -totalRain * pc.rainAspect.x);
    vec4 refractedColor = linearize(texture(sampler2D(t_source, NVR_SAMPLER(pc.samplerIndex)), refractedUV));
    refractedColor += texture(sampler2D(t_bloom, NVR_SAMPLER(pc.samplerIndex)), refractedUV);

    o_color = delinearize(mix(color, refractedColor + rainColor * pc.rainAspect.y * 0.02, totalRain * pc.rainData.w));
}
