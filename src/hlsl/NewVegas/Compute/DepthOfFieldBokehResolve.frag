// DepthOfField bokeh (needs NVR DXVK; fork-only, [Shaders.DepthOfField.Bokeh] Bokeh): the sprites DepthOfFieldBokehScatter.comp
// added up (a half-resolution fixed-point RGB buffer) into a half-resolution RGBA16F texture, one texel per pixel, so the bokeh
// Combine reads them with one bilinear tap instead of four raw buffer reads per full-resolution pixel. effects/DepthOfField.cpp.
#version 450

layout(set = 1, binding = 0, std430) readonly buffer Accum {
    uint accum[];                                            // (r, g, b) * 65536 per half-resolution pixel
};

layout(push_constant) uniform Push {
    uint width;                                              // the half-resolution size (= this target's)
    uint height;
    uint pad0, pad1;
} pc;

layout(location = 0) out vec4 o_color;

void main() {
    uvec2 p = min(uvec2(gl_FragCoord.xy), uvec2(pc.width - 1u, pc.height - 1u));
    uint base = (p.y * pc.width + p.x) * 3u;
    o_color = vec4(vec3(accum[base], accum[base + 1u], accum[base + 2u]) * (1.0 / 65536.0), 1.0);
}
