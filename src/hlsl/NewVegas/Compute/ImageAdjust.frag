// ImageAdjust as a Vulkan pass (needs NVR DXVK): Effects/ImageAdjust.fx.hlsl line for line. effects/ImageAdjust.cpp ExtRender.
// Common.glsl's pows() is the sign-preserving power and luma() the Rec. 709 luma, as in the D3D9 file.
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "Common.glsl"

layout(set = 1, binding = 0) uniform texture2D t_rendered;   // TESR_RenderedBuffer (LINEAR, CLAMP)

layout(push_constant) uniform Push {
    vec4 data;               // 0: TESR_ImageAdjustData (x brightness, y contrast, z saturation, w strength)
    vec4 darkColor;          // 16: TESR_DarkAdjustColor (gamma encoded)
    vec4 lightColor;         // 32: TESR_LightAdjustColor (gamma encoded)
    uint samplerIndex;       // 48: LINEAR, CLAMP
} pc;                        // 64 bytes

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o_color;

const float GAMMA = 2.2;

void main() {
    vec3 c = textureLod(sampler2D(t_rendered, NVR_SAMPLER(pc.samplerIndex)), uv, 0.0).rgb;

    float grey = luma(c);
    c = mix(vec3(grey), c, pc.data.z);

    vec3 lin = pows(c, GAMMA);

    vec3 light = smoothstep(vec3(0.0), vec3(1.0), lin);
    vec3 dark = 1.0 - light;
    vec3 darkTint = pows(pc.darkColor.rgb, GAMMA);
    vec3 lightTint = pows(pc.lightColor.rgb, GAMMA);
    vec3 toned = lin * dark * darkTint + lin * light * lightTint;

    vec3 bright = max(toned * pc.data.x, vec3(0.0));
    vec3 blended = mix(lin, bright, pc.data.w);

    vec3 display = pows(blended, 1.0 / GAMMA);
    vec3 contrasted = (display - 0.5) * pc.data.y + 0.5;
    o_color = vec4(mix(display, contrasted, pc.data.w), 1.0);
}
