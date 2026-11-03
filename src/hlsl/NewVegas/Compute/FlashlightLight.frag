// Flashlight's light pass (Flashlight) as a Vulkan pass (needs NVR DXVK): Effects/Flashlight.fx.hlsl line for line.
// effects/Flashlight.cpp ExtRender. Without shadows the technique's first pass (NoShadow) fills the image with 1 and this pass
// reads it back as isShadow; that pass is not drawn here, pc.mode 1 stands for its 1 (exact in any target format).
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "Flashlight.glsl"

// Procedural rings and mottling layered over the projected cookie (Flashlight.fx.hlsl BeamBreakup)
float BeamBreakup(vec2 cuv) {
    if (FL_COOKIESTRENGTH <= 0.001) return 1.0;

    vec2 p = cuv - 0.5;
    float r = saturate(length(p) * 2.0);

    float warp = sin(cuv.x * 17.0 + cuv.y * 5.5) * 0.45 + sin(cuv.x * -6.5 + cuv.y * 19.0) * 0.35;
    float rings = sin(r * 36.0 + warp) * 0.5 + 0.5;
    float mottled = sin(cuv.x * 41.0 + sin(cuv.y * 13.0) * 2.2) * sin(cuv.y * 29.0 + sin(cuv.x * 11.0) * 1.7);
    mottled = mottled * 0.5 + 0.5;

    float edge = smoothstep(0.25, 0.95, r);
    float breakup = 1.0 + (rings - 0.5) * (0.10 + 0.14 * edge) + (mottled - 0.5) * 0.12;
    breakup -= edge * (0.04 + 0.04 * mottled);
    return mix(1.0, saturate(breakup), saturate(FL_COOKIESTRENGTH));
}

void main() {
    float depth = readDepth(uv);
    vec3 eyeVector = toWorld(uv);
    vec3 eyeDirection = normalize(eyeVector);
    vec3 worldPos = cb.cameraPosition.xyz + eyeVector * depth;
    vec3 normal = GetWorldNormal(uv);

    vec3 lightpos = cb.spotLightPosition.xyz;
    vec3 lightDir = cb.spotLightDirection.xyz;
    vec3 lightToWorld = lightpos - worldPos;
    vec3 lightVector = normalize(lightToWorld);

    // Soft edges: squared wrap lighting at 3/4 energy instead of the clamped N.L (see the .fx.hlsl)
    float ndl = dot(lightVector, normal);
    float wrapped = saturate(ndl * 0.5 + 0.5);
    float diffuse = (FL_SOFTEDGE > 0.5) ? (0.75 * wrapped * wrapped) : max(ndl, 0.0);
    float specular = pows(shades(normalize(eyeDirection + lightVector * -1.0), normal), 5.0);

    // radius based attenuation
    float radius = cb.spotLightPosition.w;
    float Distance = length(lightToWorld) / radius;
    float s = saturate(Distance * Distance);
    float atten = saturate(((1.0 - s) * (1.0 - s)) / (1.0 + 5.0 * s));

    vec4 lightSpaceCoord = ScreenCoordToTexCoord(cb.flashLightViewProj * vec4(worldPos, 1.0));
    float isShadow = (pc.mode != 0u) ? 1.0 : LINEAR_LOD(t_rendered, uv).x;

    // tex2D with mips: implicit derivatives, in uniform control flow
    vec2 cookieUV = lightSpaceCoord.xy;
    float lightTexture = texture(sampler2D(t_cookie, NVR_SAMPLER(pc.cookieSampler)), cookieUV).r * BeamBreakup(cookieUV);

    float sunLuma = 1.0 / max(0.05, luma(cb.sunColor));
    vec3 lightColor = cb.spotLightColor.rgb * cb.spotLightColor.w * sunLuma;

    float angleCosMax = cos(radians(cb.spotLightDirection.w));
    float angleCosMin = cos(radians(cb.spotLightDirection.w * 0.5));
    float cone = pow(invlerps(angleCosMax, angleCosMin, shades(lightDir, lightVector * -1.0)), 2.0);

    // Ramp the pool down point blank so a wall a foot away doesn't clip to white
    float nearFade = (FL_NEARFADE > 0.0) ? smoothstep(0.0, FL_NEARFADE, length(lightToWorld)) : 1.0;

    vec3 light = (diffuse + specular) * lightColor * cone * atten * lightTexture * isShadow * nearFade;
    o_color = vec4(light, 1.0);
}
