// DepthOfField, technique 0, pass 6 (Combine) plus the bokeh sprites (needs NVR DXVK; fork-only, [Shaders.DepthOfField.Bokeh]
// Bokeh): DepthOfFieldCombine.frag line for line, then the sprites DepthOfFieldBokehScatter.comp scattered (resolved into a
// half-resolution RGBA16F texture by DepthOfFieldBokehResolve.frag, read with one bilinear tap) are added in linear light with a soft limit, 1 - (1 - c) * exp(-b):
// a plain add while the sum is dark, never past white. effects/DepthOfField.cpp ExtRender.
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "Common.glsl"

layout(set = 1, binding = 0) uniform texture2D t_rendered;   // TESR_RenderedBuffer
layout(set = 1, binding = 1) uniform texture2D t_source;     // TESR_SourceBuffer

layout(set = 1, binding = 2) uniform texture2D t_bokeh;      // the resolved sprites (half resolution, LINEAR, CLAMP)

#include "DepthOfField.glsl"

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 o_color;

void main() {
    vec2 uv = inUV;

    // DepthOfFieldCombine.frag
    vec2 halfPixel = pc.reciprocalResolution.xy * 1.5;
    vec2 nearImageUV = clamp(uv * 0.5, vec2(0.0, 0.0), vec2(0.5, 0.5 - halfPixel.y));
    vec2 nearBlurUV = clamp(uv * 0.5 + vec2(0.0, 0.5), vec2(0.0, 0.5 + halfPixel.y), vec2(0.5 + halfPixel.x, 1.0));
    vec2 farImageUV = clamp(uv * 0.5 + vec2(0.5, 0.0), vec2(0.5 + halfPixel.x, 0.0), vec2(1.0, 0.5 - halfPixel.y));
    vec2 farBlurUV = clamp(uv * 0.5 + vec2(0.5, 0.5), vec2(0.5 + halfPixel.x, 0.5 + halfPixel.y), vec2(1.0, 1.0));

    vec4 nearBlurImage = textureLod(sampler2D(t_rendered, NVR_SAMPLER(pc.samplerIndex)), nearImageUV, 0.0);
    vec4 farBlurImage = textureLod(sampler2D(t_rendered, NVR_SAMPLER(pc.samplerIndex)), farImageUV, 0.0);
    float nearBlurAmount = textureLod(sampler2D(t_rendered, NVR_SAMPLER(pc.samplerIndex)), nearBlurUV, 0.0).r;
    float farBlurAmount = textureLod(sampler2D(t_rendered, NVR_SAMPLER(pc.samplerIndex)), farBlurUV, 0.0).g;

    vec4 baseColor = textureLod(sampler2D(t_source, NVR_SAMPLER(pc.samplerIndex)), uv, 0.0);
    vec4 color = mix(baseColor, farBlurImage, farBlurAmount);
    color = mix(color, nearBlurImage, nearBlurAmount);

    vec3 bokeh = textureLod(sampler2D(t_bokeh, NVR_SAMPLER(pc.samplerIndex)), uv, 0.0).rgb;
    vec3 lin = saturate(linearize(color.rgb));
    lin = 1.0 - (1.0 - lin) * exp(-bokeh);

    o_color = vec4(delinearize(lin), 1.0);
}
