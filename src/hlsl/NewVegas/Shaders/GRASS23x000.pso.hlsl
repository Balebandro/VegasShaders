// UNOFFICIAL 2026-10-27 (user: "make grass get nvr's shadows and conor's fixes with msaa off too"): the game's grass PS without
// transparency multisampling (used whenever the game's MSAA is off, as FSR's RenderScale needs). Until now the game's own ran then:
// no NVR sun shadow, no ShadowFix / SkyLightScale. NVR's GRASS23x000TMS.pso with the game's hard alpha cut (AlphaTestRef).
#define GRASS_ALPHA_TEST
#include "GRASS23x000TMS.pso.hlsl"
