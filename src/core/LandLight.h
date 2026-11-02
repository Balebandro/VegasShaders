#pragma once
// UNOFFICIAL 2026-10-25 (bug: a tree's shadow under a cloud's shadow came out lighter than the clouded ground round it): the light
// vector the game hands the land's pixel shaders (TerrainTemplate.hlsl SunDir, c18), for the SunFar pass (SunFar.fx.hlsl) to split
// the land's light the way the land shader lit it. It is not the directional light's own direction (TESR_SunDirection, what the
// object shaders light with): measured at the Poseidon gas station, 17.4 h, it pointed the same way across the ground but stood
// 10.8 degrees high instead of 27.4, so on flat land the sun's NdotL was well under half of what SunFar took it for, and a cloud
// took away more light than the sun gave there. Nothing tells which it is beforehand, so it is read back: right after a land
// pixel shader has been bound and drawn with, at the next NVR shader bind (ShaderRecord::SetCT, before that shader's constants
// go up), c18 still holds what the land drew with. Kept only if it is a unit vector pointing across the ground the way the sun
// does (a later draw of a game shader could have put something else there).
namespace LandLight {
	inline bool		Pending = false;			// a land pixel shader was bound since the last read
	inline float	Dir[3] = { 0.0f, 0.0f, 0.0f };
	inline UInt32	Frame = 0;					// the frame of the last good read (SunFarEffect::UpdateConstants counts them in Now)
	inline UInt32	Now = 0;

	inline void Read() {
		Pending = false;
		float v[4] = {};
		if (FAILED(TheRenderManager->device->GetPixelShaderConstantF(18, v, 1))) return;
		const float length = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
		if (length < 0.98f || length > 1.02f || v[2] <= 0.0f) return;
		const D3DXVECTOR4& sun = TheShaderManager->ShaderConst.SunDir;
		const float a = sqrtf(v[0] * v[0] + v[1] * v[1]), b = sqrtf(sun.x * sun.x + sun.y * sun.y);
		if (a > 0.05f && b > 0.05f && (v[0] * sun.x + v[1] * sun.y) / (a * b) < 0.99f) return;
		Dir[0] = v[0] / length; Dir[1] = v[1] / length; Dir[2] = v[2] / length;
		Frame = Now;
	}
}
