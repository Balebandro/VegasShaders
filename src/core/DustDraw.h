#pragma once

// 2026-11-04 dust on top surfaces ([Shaders.PBR.Dust], Includes/ObjectDust.hlsl): which object draws may take dust. User: "the dust
// seems to appear on our character". The shaders cannot tell a skinned mesh: SKIN is a vertex-shader define, and one pixel shader
// draws skinned and static meshes alike. So each object pass is told here, pixel constant c169.x (NVR_DustDraw; no shader declares
// c169 otherwise): 1 for a static mesh in the world scene (the RenderPassStandard hook), 0 for skinned meshes -- people, creatures,
// armour, the first-person arms (the RenderPassSkinned hook) -- and for first person, the reflections and anything else. Sent only
// when it changes, and again with the frame's first object (effects between frames may use the high registers). Off at startup
// (not compiled into the shaders): nothing is sent.
namespace DustDraw {
	inline bool Checked = false, Compiled = false;
	inline float Sent = -1.0f;

	inline void BeginFrame() {
		if (!Checked) {
			Checked = true;
			Compiled = TheSettingManager->GetSettingI("Shaders.PBR.Dust", "Enabled") != 0;	// as ShaderRecord compiled it (DUST_ON)
		}
		Sent = -1.0f;
	}

	// One object pass about to be drawn. allow: a static mesh drawn in the world scene.
	inline void ForDraw(IDirect3DDevice9* device, bool allow) {
		if (!Compiled) return;
		const float flag = allow ? 1.0f : 0.0f;
		if (flag == Sent) return;
		Sent = flag;
		const float value[4] = { flag, 0.0f, 0.0f, 0.0f };
		device->SetPixelShaderConstantF(169, value, 1);
	}
}
