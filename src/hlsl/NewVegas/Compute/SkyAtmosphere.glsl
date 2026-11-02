// The physical sky ([Shaders.Sky.Physical]) as Vulkan passes (needs NVR DXVK): Effects/Includes/Atmosphere.hlsl and the constants
// and closed-form functions of Effects/SkyView.fx.hlsl, line for line. Included by SkyView*.frag (effects/SkyView.cpp ExtRender).
// Pure maths: the sampled tables (AtmSunOD, AtmSkyX) are in SkyTables.glsl.
#ifndef SKY_ATMOSPHERE_INCLUDED
#define SKY_ATMOSPHERE_INCLUDED

const float AtmGround = 6360000.0;                                  // m
const float AtmTop = 6460000.0;
const vec3 AtmRayleigh = vec3(5.802e-6, 13.558e-6, 33.1e-6);        // scattering per m at sea level
const float AtmRayleighH = 8000.0;                                  // scale height, m
const float AtmMieScatter = 3.996e-6;
const float AtmMieExtinct = 4.44e-6;
const float AtmMieH = 1200.0;
const float AtmMieG = 0.8;
const vec3 AtmOzone = vec3(0.650e-6, 1.881e-6, 0.085e-6);           // absorption per m at the layer's peak
const float AtmOzoneColumn = 15000.0;                               // the layer's density integrated straight up, m
const float AtmPi = 3.14159265;

// SkyView.fx.hlsl's table constants
const float TableScale = 1000.0;
const float TransW = 256.0, TransH = 64.0;
const float MSN = 32.0;
const float AtmHor = 1132254.4;     // sqrt(top^2 - ground^2): the distance from the ground's tangent point to the top of the air
const int MSDirs = 12, MSSteps = 12;

// Schueler's Chapman approximation: the air along a ray toward a light, relative to straight up, times exp(-h).
float AtmChapman(float X, float h, float cosChi) {
    float c = sqrt(X + h);
    if (cosChi >= 0.0) return c / (c * cosChi + 1.0) * exp2(-h);
    float x0 = sqrt(1.0 - cosChi * cosChi) * (X + h);
    float c0 = sqrt(x0);
    return 2.0 * c0 * exp2(X - x0) - c / (1.0 - c * cosChi) * exp2(-h);   // below the horizon: through the earth's shadow
}

// The optical depth from radius r toward the sun, cosChi the cosine of the sun's angle from straight up there.
vec3 AtmSunDepth(float r, float cosChi, float haze) {
    float alt = r - AtmGround;
    float hR = AtmRayleighH * 0.693147, hM = AtmMieH * 0.693147;
    float chR = AtmChapman(AtmGround / hR, alt / hR, cosChi);
    float chM = AtmChapman(AtmGround / hM, alt / hM, cosChi);
    float airmass = chR / max(exp(-alt / AtmRayleighH), 1e-6);
    return AtmRayleigh * (AtmRayleighH * chR) + (AtmMieExtinct * haze * AtmMieH * chM) + AtmOzone * (AtmOzoneColumn * airmass);
}

// Sky radiance along dir (unit, z up) for a sun along sunDir (unit), seen from camAlt metres up. haze scales the Mie part.
vec3 AtmSky(vec3 dir, vec3 sunDir, float camAlt, float haze) {
    float r0 = AtmGround + camAlt;
    float mu = dir.z;
    float tMax = -r0 * mu + sqrt(max(r0 * r0 * (mu * mu - 1.0) + AtmTop * AtmTop, 0.0));
    float discGround = r0 * r0 * (mu * mu - 1.0) + AtmGround * AtmGround;
    if (mu < 0.0 && discGround >= 0.0) tMax = -r0 * mu - sqrt(discGround);

    float nu = dot(dir, sunDir);
    float phaseR = 3.0 / (16.0 * AtmPi) * (1.0 + nu * nu);
    float g2 = AtmMieG * AtmMieG;
    float phaseM = 3.0 / (8.0 * AtmPi) * ((1.0 - g2) * (1.0 + nu * nu)) / ((2.0 + g2) * pow(max(1.0 + g2 - 2.0 * AtmMieG * nu, 1e-4), 1.5));

    vec3 depth = vec3(0.0), sumR = vec3(0.0), sumM = vec3(0.0);
    for (int i = 0; i < 12; i++) {
        float s0 = float(i) / 12.0, s1 = float(i + 1) / 12.0;
        float t0 = tMax * s0 * s0, t1 = tMax * s1 * s1;   // denser near the eye, where the air is
        float dt = t1 - t0;
        vec3 p = vec3(0.0, 0.0, r0) + dir * (0.5 * (t0 + t1));
        float r = length(p);
        float alt = r - AtmGround;
        float dR = exp(-alt / AtmRayleighH), dM = exp(-alt / AtmMieH), dO = max(0.0, 1.0 - abs(alt - 25000.0) / 15000.0);
        vec3 ext = AtmRayleigh * dR + AtmMieExtinct * haze * dM + AtmOzone * dO;
        vec3 T = exp(-(depth + ext * (0.5 * dt) + AtmSunDepth(r, dot(p, sunDir) / r, haze)));
        sumR += T * (dR * dt);
        sumM += T * (dM * dt);
        depth += ext * dt;
    }
    return sumR * AtmRayleigh * phaseR + sumM * (AtmMieScatter * haze * phaseM);
}

#endif
