// UNOFFICIAL 2026-10-20 (skyamb) Skylighting LightCap ([Shaders.AmbientOcclusion.Skylighting]; AmbientOcclusion.cpp SkyTakeComposite). The
// user at a neon canopy at dusk: with Skylighting on, the lamp-lit wall and the ring of glowing bulbs under it went nearly black. The
// factor (Includes/SkyShare.hlsl SkyFactor) knows only the sun and the sky: in shade all of a pixel's light counts as sky light, so under
// a wide cover (visibility ~ 0) it took all of it, the lamps' and the glow's too. Here the light taken is also held to what the blocked
// sky can have given: (1 - visibility) x cap, cap = LightCap x Strength x the sky's light on a white surface (linear luminance, x
// AmbientScale). A pixel lit by the sun and the sky alone loses what it lost before (its sky light is below that whenever its albedo is
// below LightCap); a lamp-lit or glowing one keeps the light the sky cannot explain. The same fraction of every channel (the hue stays).
// noLimit 1e6 (LightCap 0, debug views): the factor alone, as before. Scalars, used per pixel, so the effect compiler hoists nothing
// into a constant register of its own (VolumetricFog.fx has none to spare with the physical sky): AmbientOcclusion.fx and AmbientLight.fx
// pass TESR_AmbientOcclusionSky5.x and .w, VolumetricFog.fx NVR_CompositeAOTexel.z and .w.
float3 SkyTake(float3 linearColor, float factor, float visibility, float cap, float noLimit)
{
	float take = min(1.0f - factor, ((1.0f - visibility) * cap + noLimit) / max(luma(linearColor), 1.0e-6f));
	return linearColor * (1.0f - take);
}
