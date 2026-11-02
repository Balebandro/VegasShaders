// UNOFFICIAL 2026-10-27 [Shaders.Grass.Main] PointLights: the game's 1POINT grass VS (grass lit by one lamp or fire, as
// GRASS23x002.vso plus the lamp). NVR's GRASS23x002.vso with the lamp's attenuation coordinates, so a tuft by a lamp keeps
// NVR's sun shadow (and ShadowFix, the wind, the collision) instead of falling back to the game's shader.
#define GRASS_POINT_LIGHT
#include "GRASS23x002.vso.hlsl"
