#pragma once

struct ShaderTemplate {
	const char* Name;
	D3DXMACRO Defines[64];	// UNOFFICIAL 2026-10-28: 64 (LINEAR_SCENE_ON: 42 appended + 6 template + the interior overrides passed 46); 2026-10-23: 48 (CHARACTER_SHADOW_MAP: the 6-define interior Fit records could reach 42 of 42); 2026-10-17: 36 (was 30): BuildDefines appends up to 20+ and the interior-override record of a 6-define template was at 27 of the old cap 28; 2026-10-21: 40 (FOLD_HIGHLIGHTS_ON made the 6-define interior Fit records 35 of 34); 2026-10-21: 44 (LAMP_FALLOFF_ON, with lampedge's POINT_SHADOW_MAG / POINT_SHADOW_EVEN: up to 41 of 38)
};
