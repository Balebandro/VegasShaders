// Reflections technique 5 (Capture, one cube face per draw) as a Vulkan pass: Effects/Reflections.fx.hlsl CapturePS line for line.
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "Reflections.glsl"

void main() {
    vec2 vpos = floor(gl_FragCoord.xy);
    vec2 st = (vpos + 0.5) / CubeSize * 2.0 - 1.0;
    vec3 dir = FaceDirection(CubeFace, st);
    vec4 result = CubeReset > 0.0 ? vec4(0.0) : textureLod(samplerCube(t_capture, POINT_S), dir, 0.0) * (1.0 - CubeFade);

    vec3 view = WorldToViewDir(dir);
    if (view.z > 0.05) {
        vec2 uv = ToScreen(view);
        if (all(greaterThan(uv, vec2(0.005))) && all(lessThan(uv, vec2(0.995)))) {
            // A cube texel covers a patch of many screen pixels: average a 4 x 4 grid of reads across it.
            vec2 du = ToScreen(WorldToViewDir(FaceDirection(CubeFace, st + vec2(2.0 / CubeSize, 0.0)))) - uv;
            vec2 dv = ToScreen(WorldToViewDir(FaceDirection(CubeFace, st + vec2(0.0, 2.0 / CubeSize)))) - uv;
            vec3 sum = vec3(0.0);
            float found = 0.0;
            for (int y = 0; y < 4; y++) {
                for (int x = 0; x < 4; x++) {
                    vec2 at = uv + du * ((float(x) - 1.5) / 4.0) + dv * ((float(y) - 1.5) / 4.0);
                    bool usable = all(greaterThan(at, vec2(0.0))) && all(lessThan(at, vec2(1.0))) && !IsWeapon(at) && readDepthLod(at) > 40.0;
                    vec3 c = linearize(textureLod(sampler2D(t_rendered, LINEAR_S), at, 0.0).rgb);
                    float brightest = max(c.r, max(c.g, c.b));
                    sum += usable ? c * min(1.0, 32.0 / max(brightest, 1.0e-4)) : vec3(0.0);
                    found += usable ? 1.0 : 0.0;
                }
            }
            if (found > 0.0) {
                vec2 edge = min(uv, 1.0 - uv);
                float blend = CaptureBlend * saturate(min(edge.x, edge.y) * 20.0) * found / 16.0;
                result = mix(result, vec4(sum / found, 1.0), blend);
            }
        }
    }
    o_color = result;
}
