// SkyView technique 1 (SkyTrans: the sun's optical depth table, 256 x 64) as a Vulkan pass (needs NVR DXVK):
// Effects/SkyView.fx.hlsl SkyTransPS line for line. effects/SkyView.cpp ExtRender.
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "SkyAtmosphere.glsl"

layout(push_constant) uniform Push {
    vec4 physSky;      // 0: TESR_PhysSky (w: haze)
    vec4 physSkySun;   // 16: TESR_PhysSkySun (unused here)
    uint samplerIndex; // 32: unused here
} pc;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o_color;

void main() {
    vec2 ij = floor(uv * vec2(TransW, TransH) + 0.25);       // the texel, whichever half-pixel convention the quad has
    float xmu = ij.x / (TransW - 1.0), xr = ij.y / (TransH - 1.0);
    float rho = xr * AtmHor;
    float r = sqrt(rho * rho + AtmGround * AtmGround);
    float dmin = AtmTop - r, dmax = rho + AtmHor;
    float d = dmin + xmu * (dmax - dmin);
    float mu = (d < 1.0e-3) ? 1.0 : clamp((AtmHor * AtmHor - rho * rho - d * d) / (2.0 * r * d), -1.0, 1.0);
    float dTop = -r * mu + sqrt(max(r * r * (mu * mu - 1.0) + AtmTop * AtmTop, 0.0));
    float st = max(-r * mu, 0.0);                                          // the tangent point (the ray's lowest point when it heads down first)
    vec3 sp = vec3(0.0);                                                   // Rayleigh, Mie, ozone density integrals (m)
    for (int seg = 0; seg < 2; seg++) {
        float len = (seg == 0) ? st : dTop - st;
        float dir = (seg == 0) ? -1.0 : 1.0;
        for (int i = 0; i < 40; i++) {                                     // cubic spacing: dense where the air is thick
            float v = (float(i) + 0.5) / 40.0;
            float s = st + dir * len * v * v * v;
            float ds = 3.0 * v * v * (1.0 / 40.0) * len;
            float rr = sqrt(max(r * r + 2.0 * r * mu * s + s * s, 0.0));
            float a = max(rr - AtmGround, 0.0);
            sp += vec3(exp(-a / AtmRayleighH), exp(-a / AtmMieH), max(0.0, 1.0 - abs(a - 25000.0) / 15000.0)) * ds;
        }
    }
    o_color = vec4(AtmRayleigh * sp.x + AtmMieExtinct * pc.physSky.w * sp.y + AtmOzone * sp.z, 1.0);
}
