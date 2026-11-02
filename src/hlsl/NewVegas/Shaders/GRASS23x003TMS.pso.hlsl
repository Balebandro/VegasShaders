// UNOFFICIAL 2026-10-27 [Shaders.Grass.Main] PointLights: the game's grass PS with one lamp (1POINT grass, transparency
// multisampling on). NVR's GRASS23x000TMS.pso (sun shadow, ShadowFix) plus the game's lamp term. The game's canopy shadow map of this variant is not read: NVR's sun shadow replaces it, as on GRASS23x000TMS.
#define GRASS_POINT_LIGHT
#include "GRASS23x000TMS.pso.hlsl"
