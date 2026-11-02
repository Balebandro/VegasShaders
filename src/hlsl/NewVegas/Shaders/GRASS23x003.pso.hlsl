// UNOFFICIAL 2026-10-27 [Shaders.Grass.Main] PointLights: the game's grass PS with one lamp, without transparency multisampling
// (the game's MSAA off). NVR's GRASS23x000TMS.pso (sun shadow, ShadowFix) plus the game's lamp term and its hard alpha cut. The
// game's canopy shadow map of this variant is not read: NVR's sun shadow replaces it, as on GRASS23x003TMS.
#define GRASS_POINT_LIGHT
#define GRASS_ALPHA_TEST
#include "GRASS23x000TMS.pso.hlsl"
