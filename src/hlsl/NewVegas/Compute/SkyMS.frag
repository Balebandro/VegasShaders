// SkyView technique 2 (SkyMS: the multiple scattering table Psi_ms, 32 x 32) as a Vulkan pass (needs NVR DXVK):
// Effects/SkyView.fx.hlsl SkyMSPS line for line. effects/SkyView.cpp ExtRender. Reads the SkyTrans table (t_trans).
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "Common.glsl"

#define SAMPLE_TRANS(coord) textureLod(sampler2D(t_trans, NVR_SAMPLER(pc.samplerIndex)), (coord), 0.0)
#define SAMPLE_MS(coord) vec4(0.0)   // not read by this technique

layout(set = 1, binding = 0) uniform texture2D t_trans;   // TESR_SkyTransLUT

layout(push_constant) uniform Push {
    vec4 physSky;      // 0: TESR_PhysSky (w: haze)
    vec4 physSkySun;   // 16: TESR_PhysSkySun (w: the ground albedo)
    uint samplerIndex; // 32: LINEAR, CLAMP (the heap index, filled by the host)
} pc;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o_color;

#include "SkyAtmosphere.glsl"
#include "SkyTables.glsl"

void main() {
    vec2 ij = floor(uv * MSN + 0.25);
    float muS = -1.0 + 2.0 * ij.x / (MSN - 1.0);
    float alt0 = ij.y / (MSN - 1.0) * (AtmTop - AtmGround);
    float r0 = AtmGround + alt0;
    float haze = pc.physSky.w, albedo = pc.physSkySun.w;
    vec3 sun = vec3(sqrt(clamp(1.0 - muS * muS, 0.0, 1.0)), 0.0, muS);
    vec3 L2 = vec3(0.0), F = vec3(0.0);
    for (int a = 0; a < MSDirs; a++) {
        float cosT = 1.0 - 2.0 * (float(a) + 0.5) / float(MSDirs);
        float sinT = sqrt(clamp(1.0 - cosT * cosT, 0.0, 1.0));
        for (int b = 0; b < MSDirs; b++) {
            float phi = 2.0 * AtmPi * (float(b) + 0.5) / float(MSDirs);
            vec3 dir = vec3(sinT * cos(phi), sinT * sin(phi), cosT);
            float disc = r0 * r0 * (cosT * cosT - 1.0) + AtmGround * AtmGround;
            float tMax = -r0 * cosT + sqrt(max(r0 * r0 * (cosT * cosT - 1.0) + AtmTop * AtmTop, 0.0));
            bool ground = (cosT < 0.0) && (disc >= 0.0);
            if (ground) tMax = max(-r0 * cosT - sqrt(max(disc, 0.0)), 0.0);
            float nu = dot(dir, sun);
            float phaseR = 3.0 / (16.0 * AtmPi) * (1.0 + nu * nu);
            float g2 = AtmMieG * AtmMieG;
            float phaseM = 3.0 / (8.0 * AtmPi) * ((1.0 - g2) * (1.0 + nu * nu)) / ((2.0 + g2) * pow(max(1.0 + g2 - 2.0 * AtmMieG * nu, 1e-4), 1.5));
            float dt = tMax / float(MSSteps);
            vec3 thr = vec3(1.0);
            for (int s = 0; s < MSSteps; s++) {
                vec3 p = vec3(0.0, 0.0, r0) + dir * ((float(s) + 0.5) * dt);
                float r = length(p);
                float alt = r - AtmGround;
                float dR = exp(-alt / AtmRayleighH), dM = exp(-alt / AtmMieH), dO = max(0.0, 1.0 - abs(alt - 25000.0) / 15000.0);
                vec3 scR = AtmRayleigh * dR;
                vec3 scM = vec3(AtmMieScatter * haze * dM);   // HLSL float3 = float: a broadcast
                vec3 ext = scR + AtmMieExtinct * haze * dM + AtmOzone * dO;
                vec3 x = ext * dt;
                vec3 f = mix(1.0 - 0.5 * x, (1.0 - exp(-x)) / max(x, vec3(1e-30)), step(vec3(1e-3), x));      // (1 - e^-x) / x without the cancellation
                vec3 tSun = exp(-AtmSunOD(r, dot(p, sun) / r));
                L2 += thr * tSun * (scR * phaseR + scM * phaseM) * (dt * f);
                F += thr * (scR + scM) * (dt * f);
                thr *= exp(-x);
            }
            if (ground) {                                                   // light the ground sends back (Lambert)
                vec3 pg = vec3(0.0, 0.0, r0) + dir * tMax;
                float rg = length(pg);
                float cosG = dot(pg, sun) / rg;
                L2 += thr * exp(-AtmSunOD(rg, cosG)) * (clamp(cosG, 0.0, 1.0) * albedo / AtmPi);
            }
        }
    }
    const float omega = 4.0 * AtmPi / float(MSDirs * MSDirs), iso = 1.0 / (4.0 * AtmPi);
    vec3 psi = (L2 * (omega * iso)) / (1.0 - F * (omega * iso));
    o_color = vec4(psi * TableScale, 1.0);
}
