// UNOFFICIAL 2026-10-27 [Shaders.Grass.Main] PointLights: the game's grass PS with one lamp, without transparency multisampling
// (the game's MSAA off). NVR's GRASS23x000TMS.pso (sun shadow, ShadowFix) plus the game's lamp term and its hard alpha cut.
#define GRASS_POINT_LIGHT
#define GRASS_ALPHA_TEST
#include "GRASS23x000TMS.pso.hlsl"
