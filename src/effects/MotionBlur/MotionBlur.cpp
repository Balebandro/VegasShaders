#include "MotionBlur.h"
#include "../../core/DxvkCompute.h"	// UNOFFICIAL 2026-10-23 (NVR DXVK)

void MotionBlurEffect::UpdateConstants() {
	// UNOFFICIAL fixes-b #42 (LOOK CHANGE, MotionBlur is off by default): UpdateConstants runs twice a rendered frame. The turn history
	// (oldAngle, oldAmount, oldoldAmount) used to shift on both calls, so the second call saw no turn and the blur the frame
	// rendered with was about 1/3 of the intended 3-frame mean, with no smoothing. Now it shifts once per frame (FrameNumber);
	// the second call keeps the constants the first made. The blur of a steady turn is therefore about 3 times stronger than
	// before at the same settings (BlurScale defaults left as they are).
	static unsigned lastFrame = ~0u;
	if (TheFrameRateManager->FrameNumber == lastFrame) return;
	lastFrame = TheFrameRateManager->FrameNumber;

	const ValuesStruct& view = TheCameraManager->IsFirstPerson() ? Settings.FirstPerson : Settings.ThirdPerson;
	MotionBlurStruct& c = Constants;

	// how far the player turned since the previous frame, in degrees
	const float yaw = D3DXToDegree(Player->rot.z);
	const float pitch = D3DXToDegree(Player->rot.x);
	float turnX = c.oldAngleZ - yaw;
	const float turnY = c.oldAngleX - pitch;

	// take the shorter way round when the yaw wraps past 360
	const float wrapUp = turnX + 360.0f;
	const float wrapDown = (yaw - c.oldAngleZ + 360.0f) * -1.0f;
	if (abs(turnX) > abs(wrapUp))
		turnX = wrapUp;
	else if (abs(turnX) > abs(wrapDown))
		turnX = wrapDown;

	// ignore turns too small to matter (squared length, evaluated in double like pow)
	float outX = turnX, outY = turnY;
	if ((double)turnX * turnX + (double)turnY * turnY < view.BlurCutOff) {
		outX = 0.0f;
		outY = 0.0f;
	}

	// the shader blurs by the mean of this frame and the two before
	c.Data.x = (c.oldoldAmountX + c.oldAmountX + outX) / 3.0f;
	c.Data.y = (c.oldoldAmountY + c.oldAmountY + outY) / 3.0f;
	c.oldAngleZ = yaw;
	c.oldAngleX = pitch;
	c.oldoldAmountX = c.oldAmountX;
	c.oldoldAmountY = c.oldAmountY;
	c.oldAmountX = outX;
	c.oldAmountY = outY;
	c.BlurParams.x = view.GaussianWeight;
	c.BlurParams.y = view.BlurScale;
	c.BlurParams.z = view.BlurOffsetMax;
}

static void ReadBlurView(const char* section, MotionBlurEffect::ValuesStruct& out) {
	SettingManager* cfg = TheSettingManager;
	out.BlurCutOff = cfg->GetSettingF(section, "BlurCutOff");
	out.GaussianWeight = cfg->GetSettingF(section, "GaussianWeight");
	out.BlurScale = cfg->GetSettingF(section, "BlurScale");
	out.BlurOffsetMax = cfg->GetSettingF(section, "BlurOffsetMax");
}

void MotionBlurEffect::UpdateSettings() {
	ReadBlurView("Shaders.MotionBlur.FirstPersonView", Settings.FirstPerson);
	ReadBlurView("Shaders.MotionBlur.ThirdPersonView", Settings.ThirdPerson);
}

void MotionBlurEffect::RegisterConstants() {
	TheShaderManager->RegisterConstant("TESR_MotionBlurParams", &Constants.BlurParams);
	TheShaderManager->RegisterConstant("TESR_MotionBlurData", &Constants.Data);
}

bool MotionBlurEffect::ShouldRender()
{
	return Constants.Data.x != 0.0f || Constants.Data.y != 0.0f;
};

// UNOFFICIAL 2026-10-23 (NVR DXVK): MotionBlur.frag, drawn twice as the technique's two passes are (the second reads the first's
// result: the chain's current image moves on each Commit). 2026-10-25: each pass gets the chain's current surface (the one Render
// passed in is the image before the first pass, so the second pass was no longer recognised as the chain's), and only the second may
// write straight into the game target.
bool MotionBlurEffect::ExtRender(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, IDirect3DSurface9* RenderedSurface, UINT techniqueIndex, bool ClearRenderTarget, IDirect3DSurface9* SourceBuffer) {
	if (techniqueIndex != 0) return false;
	struct Push { D3DXVECTOR4 reciprocalResolution, blurParams, blurData; UInt32 samplerIndex; UInt32 pad[3]; } push = {};
	push.reciprocalResolution = TheShaderManager->ShaderConst.ReciprocalResolution;
	push.blurParams = Constants.BlurParams;
	push.blurData = Constants.Data;
	for (int pass = 0; pass < 2; pass++) {
		IDirect3DBaseTexture9* textures[] = { TheTextureManager->RenderedTexture };
		if (!ExtTexturePass(Device, RenderTarget, pass ? TheTextureManager->RenderedSurface : RenderedSurface, SourceBuffer, extPass, "MotionBlur", textures, _countof(textures), &push, sizeof(push), 48,
			false, 0, false, pass == 1))
			return pass == 1;	// the first pass drew and committed: finish with D3D9 would blur twice more; keep what is there
	}
	return true;
}
