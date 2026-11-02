// UNOFFICIAL 2026-10-30 the cloud sky probe (VolumetricClouds technique 12, Vulkan only; [Shaders.Reflections.Main] CloudSky): the clouds
// over the whole upper sky, one MarchRay per texel of a 64 x 64 hemi-octahedral map (texel -> world direction, z up), so a reflection
// that finds nothing shows the clouds where the camera never looked (the surroundings cube only holds the sky it saw). Same textures,
// constants and push block as the march; rgb the clouds' light (linear), a the sky's transmittance (the composite's blend).
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "Common.glsl"
#include "SkyAtmosphere.glsl"
#include "VolumetricCloudsMarchRay.glsl"

void main() {
    vec2 texel = floor(gl_FragCoord.xy);
    vec2 p = (texel + 0.5) / 32.0 - 1.0;   // [-1, 1]^2 over the 64 x 64 map (one 8-row band a frame: RenderSkyProbe's viewport)
    vec3 dir = vec3((p.x + p.y) * 0.5, (p.x - p.y) * 0.5, 0.0);
    dir.z = max(1.0 - abs(dir.x) - abs(dir.y), 0.0);
    dir = normalize(dir);
    float jitter = fract(textureLod(sampler2D(t_blueNoise, NVR_SAMPLER(pc.pointWrapIndex)), texel / 256.0, 0.0).r + cb.march.z * 0.61803398875);
    o_color = MarchRay(dir, jitter, 0.5, true, pc.towers != 0u);   // half the steps: a reflection, blurred by the surface
}
