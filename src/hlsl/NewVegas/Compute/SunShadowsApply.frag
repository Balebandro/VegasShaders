// SunShadows apply (techniques 0 and 3, last pass) as a Vulkan pass: Effects/SunShadows.fx.hlsl ApplySunMapsPS. Both channels raised
// to Intensity, then red takes the darker of the contact term and the sun maps' (cascades, RT Mode 1 fade, and in the c1 variant,
// CHARACTER_SHADOW_MAP=1 in variants.txt, the character map; its debug views replace red).
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "SunShadows.glsl"

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o_color;

void main() {
    vec2 contact = applyIntensity(sourceAt(uv).rg);
    bool debugView;
    float sun = sunMapsVisibility(uv, debugView);
    contact.r = debugView ? sun : min(contact.r, sun);
    o_color = vec4(contact, 0.0, 1.0);
}
