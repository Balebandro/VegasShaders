// UNOFFICIAL 2026-10-14 physical sky ([Shaders.Sky.Physical]; SkyView.fx.hlsl fills the sky's table with it, SKY.pso.hlsl
// reads the table). The light the air scatters toward the eye,
// worked out from the air itself instead of painted: sunlight crossing the atmosphere (a shell 100 km thick round the
// earth) is scattered by the air molecules (Rayleigh: much more blue than red, so a blue sky and a red low sun) and by
// dust and haze (Mie: grey, mostly straight on, the bright glow round the sun), and partly absorbed by ozone (which
// keeps the twilight sky blue). Earth values from "A Scalable and Production Ready Sky and Atmosphere Rendering
// Technique" (Hillaire, 2020); single scattering only.
//
// Per direction: 12 steps along the view ray to the top of the atmosphere (or the ground); at each, the sunlight that
// reaches that point comes from a closed-form estimate of the air in the way (the Chapman function, Schueler's
// approximation, GPU Pro 3), including the earth's shadow once the sun is below that point's horizon.
//
// Returned radiance is for a sun of irradiance 1, linear. Sky.cpp (PhysicalSkyRadiance) mirrors this exactly for the
// brightness match and the sky light on objects: change one and change the other.
#ifndef ATMOSPHERE_INCLUDED
#define ATMOSPHERE_INCLUDED

static const float AtmGround = 6360000.0f;                                  // m
static const float AtmTop = 6460000.0f;
static const float3 AtmRayleigh = float3(5.802e-6f, 13.558e-6f, 33.1e-6f);  // scattering per m at sea level
static const float AtmRayleighH = 8000.0f;                                  // scale height, m
static const float AtmMieScatter = 3.996e-6f;
static const float AtmMieExtinct = 4.44e-6f;
static const float AtmMieH = 1200.0f;
static const float AtmMieG = 0.8f;
static const float3 AtmOzone = float3(0.650e-6f, 1.881e-6f, 0.085e-6f);    // absorption per m at the layer's peak
static const float AtmOzoneColumn = 15000.0f;                               // the layer's density integrated straight up, m
static const float AtmPi = 3.14159265f;

// Schueler's Chapman approximation: the air along a ray toward a light, relative to straight up, times exp(-h).
// X: the ground's radius and h: the point's altitude, both in units of the 50% height (scale height * ln 2).
float AtmChapman(float X, float h, float cosChi) {
    float c = sqrt(X + h);
    if (cosChi >= 0.0f) return c / (c * cosChi + 1.0f) * exp2(-h);
    float x0 = sqrt(1.0f - cosChi * cosChi) * (X + h);
    float c0 = sqrt(x0);
    return 2.0f * c0 * exp2(X - x0) - c / (1.0f - c * cosChi) * exp2(-h);   // below the horizon: through the earth's shadow
}

// The optical depth from radius r toward the sun, cosChi the cosine of the sun's angle from straight up there.
float3 AtmSunDepth(float r, float cosChi, float haze) {
    float alt = r - AtmGround;
    float hR = AtmRayleighH * 0.693147f, hM = AtmMieH * 0.693147f;
    float chR = AtmChapman(AtmGround / hR, alt / hR, cosChi);
    float chM = AtmChapman(AtmGround / hM, alt / hM, cosChi);
    float airmass = chR / max(exp(-alt / AtmRayleighH), 1e-6f);
    return AtmRayleigh * (AtmRayleighH * chR) + (AtmMieExtinct * haze * AtmMieH * chM) + AtmOzone * (AtmOzoneColumn * airmass);
}

// Sky radiance along dir (unit, z up) for a sun along sunDir (unit), seen from camAlt metres up. haze scales the Mie part.
float3 AtmSky(float3 dir, float3 sunDir, float camAlt, float haze) {
    float r0 = AtmGround + camAlt;
    float mu = dir.z;
    float tMax = -r0 * mu + sqrt(max(r0 * r0 * (mu * mu - 1.0f) + AtmTop * AtmTop, 0.0f));
    float discGround = r0 * r0 * (mu * mu - 1.0f) + AtmGround * AtmGround;
    if (mu < 0.0f && discGround >= 0.0f) tMax = -r0 * mu - sqrt(discGround);

    float nu = dot(dir, sunDir);
    float phaseR = 3.0f / (16.0f * AtmPi) * (1.0f + nu * nu);
    float g2 = AtmMieG * AtmMieG;
    float phaseM = 3.0f / (8.0f * AtmPi) * ((1.0f - g2) * (1.0f + nu * nu)) / ((2.0f + g2) * pow(max(1.0f + g2 - 2.0f * AtmMieG * nu, 1e-4f), 1.5f));

    float3 depth = 0.0f, sumR = 0.0f, sumM = 0.0f;
    [unroll] for (int i = 0; i < 12; i++) {
        float s0 = i / 12.0f, s1 = (i + 1) / 12.0f;
        float t0 = tMax * s0 * s0, t1 = tMax * s1 * s1;   // denser near the eye, where the air is
        float dt = t1 - t0;
        float3 p = float3(0.0f, 0.0f, r0) + dir * (0.5f * (t0 + t1));
        float r = length(p);
        float alt = r - AtmGround;
        float dR = exp(-alt / AtmRayleighH), dM = exp(-alt / AtmMieH), dO = max(0.0f, 1.0f - abs(alt - 25000.0f) / 15000.0f);
        float3 ext = AtmRayleigh * dR + AtmMieExtinct * haze * dM + AtmOzone * dO;
        float3 T = exp(-(depth + ext * (0.5f * dt) + AtmSunDepth(r, dot(p, sunDir) / r, haze)));
        sumR += T * (dR * dt);
        sumM += T * (dM * dt);
        depth += ext * dt;
    }
    return sumR * AtmRayleigh * phaseR + sumM * (AtmMieScatter * haze * phaseM);
}

#endif
