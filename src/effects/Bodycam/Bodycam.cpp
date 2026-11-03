// UNOFFICIAL, testbed (2026-10-13): police body-camera look (Bodycam.h).

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "Bodycam.h"
#include "BodycamAudio.h"
#include "../../core/DxvkCompute.h"	// UNOFFICIAL 2026-10-25 (NVR DXVK)

// UNOFFICIAL 2026-10-13: the microphone's settings ([Shaders.Bodycam.Main] Mic...), handed to the audio filter every frame while
// Bodycam is on (BodycamAudio.h). Kept here, not in Bodycam.h (included everywhere).
static BodycamAudio::Settings MicSettings;

void BodycamEffect::RegisterConstants() {
	TheShaderManager->RegisterConstant("TESR_BodycamLens", &Constants.Lens);
	TheShaderManager->RegisterConstant("TESR_BodycamImage", &Constants.Image);
	TheShaderManager->RegisterConstant("TESR_BodycamNoise", &Constants.Noise);
	TheShaderManager->RegisterConstant("TESR_BodycamText", &Constants.Text);
	TheShaderManager->RegisterConstant("TESR_BodycamFrame", &Constants.Frame);
	TheShaderManager->RegisterConstant("TESR_BodycamMotion", &Constants.Motion);
	TheShaderManager->RegisterConstant("TESR_BodycamGrade", &Constants.Grade);
	TheShaderManager->RegisterConstant("TESR_BodycamVideo", &Constants.Video);
	TheShaderManager->RegisterConstant("TESR_BodycamAdapt", &Constants.Adapt);
	TheShaderManager->RegisterConstant("TESR_BodycamBody", &Constants.Body);
	TheShaderManager->RegisterConstant("TESR_BodycamFlash", &Constants.Flash);
	TheShaderManager->RegisterConstant("TESR_BodycamGlitch", &Constants.Glitch);
}

void BodycamEffect::RegisterTextures() {
	// Written on the CPU (WriteText), so a managed texture rather than a render target.
	if (FAILED(TheRenderManager->device->CreateTexture(TextWidth, TextHeight, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &TextTexture, NULL))) {
		Logger::Log("[ERROR] Bodycam: could not create the text texture");
		TextTexture = nullptr;
	}
	TheTextureManager->RegisterTexture("TESR_BodycamTextBuffer", (IDirect3DBaseTexture9**)&TextTexture);
	WriteText("", "");
	// UNOFFICIAL 2026-10-13: the measured brightness (two, read one / write the other) and the held video frame (a small stand-in
	// until the first frame is kept: Render makes it the picture's size and format).
	TheTextureManager->InitTexture("NVR_BodycamMeasure0", &MeasureTexture[0], &MeasureSurface[0], 1, 1, D3DFMT_A16B16G16R16F);
	TheTextureManager->InitTexture("NVR_BodycamMeasure1", &MeasureTexture[1], &MeasureSurface[1], 1, 1, D3DFMT_A16B16G16R16F);
	MeasureRead = MeasureTexture[0];
	TheTextureManager->RegisterTexture("TESR_BodycamExposureBuffer", (IDirect3DBaseTexture9**)&MeasureRead);
	TheTextureManager->InitTexture("TESR_BodycamHoldBuffer", &HoldTexture, &HoldSurface, 4, 4, D3DFMT_A8R8G8B8);
}

// Each effect's name, range, and the section it is read from per look (BodycamEffect::LookKey order).
namespace {
	struct LookSetting { const char* Name; float Min, Max; };
	const LookSetting LookSettings[BodycamEffect::LookKeys] = {
		{ "Fisheye", 0.0f, 1.0f }, { "FitEdges", 0.0f, 1.0f }, { "Border", 0.0f, 1.0f }, { "Fringe", 0.0f, 10.0f },
		{ "EdgeBlur", 0.0f, 2.0f }, { "LensBlur", 0.0f, 2.0f }, { "Vignette", 0.0f, 1.0f }, { "Sharpen", 0.0f, 3.0f },
		{ "Exposure", 0.5f, 3.0f }, { "Contrast", 0.5f, 2.0f }, { "Saturation", 0.0f, 2.0f }, { "Lift", 0.0f, 1.5f },
		{ "Cool", 0.0f, 1.0f }, { "Glow", 0.0f, 2.0f }, { "Grain", 0.0f, 2.0f }, { "Blocks", 0.0f, 1.0f },
		{ "MotionBlur", 0.0f, 3.0f }, { "LensDirt", 0.0f, 2.0f }, { "Tint", 0.0f, 1.0f }, { "Text", 0.0f, 1.0f },
		{ "VideoFps", 0.0f, 60.0f }, { "AutoExposure", 0.0f, 1.0f }, { "ExposureSpeed", 0.1f, 10.0f }, { "Breakup", 0.0f, 2.0f },
		{ "RollingShutter", 0.0f, 3.0f },
		{ "Bounce", 0.0f, 2.0f }, { "GunJolt", 0.0f, 3.0f }, { "Overload", 0.0f, 2.0f }, { "LowLight", 0.0f, 1.0f },
		{ "LightSmear", 0.0f, 2.0f }, { "Hiccups", 0.0f, 1.0f }, { "Autofocus", 0.0f, 2.0f },
	};
}

void BodycamEffect::UpdateSettings() {
	const char* section = "Shaders.Bodycam.Main";
	auto readLook = [](const char* lookSection, LookStruct& look) {
		for (int k = 0; k < LookKeys; k++)
			look.v[k] = std::clamp(TheSettingManager->GetSettingF(lookSection, LookSettings[k].Name), LookSettings[k].Min, LookSettings[k].Max);
	};
	readLook(section, Day);
	readLook("Shaders.Bodycam.Night", Night);
	readLook("Shaders.Bodycam.Interiors", Interiors);
	Constants.Text.y = std::clamp(TheSettingManager->GetSettingF(section, "TextSize"), 1.0f, 12.0f);	// one size for all
	MicSettings.Mix = std::clamp(TheSettingManager->GetSettingF(section, "Mic"), 0.0f, 1.0f);
	MicSettings.Bits = std::clamp(TheSettingManager->GetSettingF(section, "MicBits"), 0.0f, 16.0f);
	MicSettings.SampleRate = std::clamp(TheSettingManager->GetSettingF(section, "MicSampleRate"), 0.0f, 48000.0f);
	MicSettings.Compression = std::clamp(TheSettingManager->GetSettingF(section, "MicCompression"), 0.0f, 1.0f);
	MicSettings.GainDb = std::clamp(TheSettingManager->GetSettingF(section, "MicGain"), -24.0f, 24.0f);
	MicSettings.Tone = std::clamp(TheSettingManager->GetSettingF(section, "MicTone"), 0.0f, 1.0f);
	MicSettings.Hiss = std::clamp(TheSettingManager->GetSettingF(section, "MicHiss"), 0.0f, 1.0f);
	const float wide = TheSettingManager->GetSettingF(section, "WideView");
	WideView = wide >= 30.0f ? (std::min)(wide, 150.0f) : 0.0f;
}

void BodycamEffect::UpdateConstants() {
	// UNOFFICIAL 2026-10-13 (user: VideoFps did nothing): NVR updates every effect's constants twice a frame (before the world
	// and again before the post effects), and the clock ran at twice real time: VideoFps 30 behaved as 60, so nothing was
	// held below 60 fps (and the exposure, the bob and the hiccups ran twice as fast). Everything here now advances once a frame.
	static double lastFrameTime = -1.0;
	const double frameTime = TheFrameRateManager->Time;
	if (frameTime == lastFrameTime) return;	// the second call this frame: the constants are already set
	const double elapsed = lastFrameTime < 0.0 ? 0.016 : frameTime - lastFrameTime;
	lastFrameTime = frameTime;
	Clock += std::clamp(elapsed, 0.0, 0.1);
	Constants.Noise.z = (float)fmod(floor(Clock * 30.0), 4096.0);	// the noise changes 30 times a second, like video

	// The grade: indoors its own; outdoors day and night blended by the time of day (1 day, 0 night, smooth through dawn
	// and dusk), as VolumetricLight does.
	LookStruct look = Interiors;
	if (TheShaderManager->GameState.isExterior) {
		const float day = std::clamp(TheShaderManager->GameState.isDayTime, 0.0f, 1.0f);
		for (int k = 0; k < LookKeys; k++) look.v[k] = Night.v[k] + (Day.v[k] - Night.v[k]) * day;
	}
	// The bend moves a point at squared radius r2 (x scaled by the aspect ratio) out by (1 + k r2); the corners (r2 = a^2 + 1)
	// stay inside the image when the picture is zoomed in by as much. FitEdges 0: no zoom (black corners).
	const float lensAspect = (float)TheRenderManager->width / max((float)TheRenderManager->height, 1.0f);
	Constants.Lens.x = look.v[Fisheye] * 0.25f;
	Constants.Lens.y = 1.0f + look.v[FitEdges] * Constants.Lens.x * (lensAspect * lensAspect + 1.0f);
	Constants.Lens.z = look.v[Fringe];
	Constants.Lens.w = look.v[Vignette];
	Constants.Image.x = look.v[Sharpen];
	Constants.Image.y = look.v[Contrast];
	Constants.Image.z = look.v[Saturation];
	Constants.Image.w = look.v[Glow];
	Constants.Noise.x = look.v[Grain];
	Constants.Noise.y = look.v[Blocks];
	Constants.Noise.w = look.v[Tint];
	Constants.Text.x = look.v[Text];
	Constants.Frame.x = look.v[Border];
	Constants.Frame.y = look.v[EdgeBlur];
	Constants.Frame.z = look.v[Exposure];
	Constants.Frame.w = look.v[LensDirt];
	Constants.Motion.z = look.v[MotionBlur];
	Constants.Motion.w = look.v[Lift];
	Constants.Grade.x = look.v[Cool];
	Constants.Grade.y = look.v[LensBlur];

	// Motion blur: how far the picture moved since last frame, from the player's turn (as MotionBlur does), about 90 degrees
	// across the screen; half of it smeared (a 180 degree shutter), eased so a single jumpy frame does not flash.
	float yaw = D3DXToDegree(Player->rot.z), pitch = D3DXToDegree(Player->rot.x);
	float dYaw = 0.0f, dPitch = 0.0f;
	if (HaveLast) {
		dYaw = fmodf(yaw - LastYaw + 540.0f, 360.0f) - 180.0f;	// the short way round
		dPitch = pitch - LastPitch;
	}
	LastYaw = yaw; LastPitch = pitch; HaveLast = true;
	const float aspect = (float)TheRenderManager->width / max((float)TheRenderManager->height, 1.0f);
	D3DXVECTOR2 moved(std::clamp(dYaw / 90.0f, -0.08f, 0.08f), std::clamp(dPitch / 90.0f * aspect, -0.08f, 0.08f));
	moved *= 0.5f * Constants.Motion.z;
	Constants.Motion.x += (moved.x - Constants.Motion.x) * 0.5f;
	Constants.Motion.y += (moved.y - Constants.Motion.y) * 0.5f;

	// UNOFFICIAL 2026-10-13: how fast the view moves (turning: screens a second; running: units a second), for the compression
	// breaking down and the rolling shutter. Rises at once, falls over about a third of a second (the encoder catches up).
	const float dt = (float)std::clamp(elapsed, 1e-4, 0.1);
	const D3DXVECTOR3 pos(Player->pos.x, Player->pos.y, Player->pos.z);
	D3DXVECTOR3 step = pos - LastPos;
	LastPos = pos;
	const float run = D3DXVec3Length(&step) / dt;
	const D3DXVECTOR2 turnPerSecond(dYaw / 90.0f / dt, dPitch / 90.0f * aspect / dt);	// picture widths a second
	const float level = std::clamp(D3DXVec2Length(&turnPerSecond) * 0.5f + (run < 2000.0f ? run / 600.0f : 0.0f), 0.0f, 1.5f);
	MotionLevel = level > MotionLevel ? level : MotionLevel + (level - MotionLevel) * std::clamp(dt / 0.3f, 0.0f, 1.0f);
	Constants.Video.x = look.v[Breakup] * MotionLevel;
	// Rolling shutter: the sensor reads its rows top to bottom over most of a video frame (about 30 ms), so while the view turns
	// each row shows the scene a little later: upright lines lean, and a fast tilt squashes or stretches the picture.
	const float readout = 0.03f * look.v[RollingShutter];
	Constants.Video.y = std::clamp(turnPerSecond.x * readout, -0.15f, 0.15f);
	Constants.Video.z = std::clamp(turnPerSecond.y * readout, -0.15f, 0.15f);
	// Auto exposure: how strongly it lags, and how fast it follows (the measure pass, Render).
	Constants.Adapt.x = look.v[AutoExposure];
	Constants.Adapt.y = dt;
	Constants.Adapt.z = look.v[ExposureSpeed];
	Constants.Adapt.w = MeasureValid ? 1.0f : 0.0f;
	VideoFrameRate = look.v[VideoFps];

	// UNOFFICIAL 2026-10-13: the chest mount (picture only: the aim is untouched). Steps from the speed over the ground: the
	// picture sways side to side once a stride and dips at each step, rolling a little with it; it also leans into turns.
	const float ground = run < 2000.0f ? sqrtf(step.x * step.x + step.y * step.y) / dt : 0.0f;
	const float moving = std::clamp(ground / 300.0f, 0.0f, 1.0f);
	StepAmount += ((ground > 15.0f ? moving : 0.0f) - StepAmount) * std::clamp(dt / 0.25f, 0.0f, 1.0f);
	StepPhase = fmodf(StepPhase + dt * (ground > 15.0f ? 0.45f + ground / 450.0f : 0.0f), 1.0f);	// strides a second
	const float bob = look.v[Bounce] * StepAmount, stride = StepPhase * 6.2831853f;
	const D3DXVECTOR3 steps(sinf(stride) * 0.004f * bob, (0.5f - 0.5f * cosf(stride * 2.0f)) * 0.006f * bob, sinf(stride) * 0.012f * bob);
	TurnRoll += (std::clamp(-turnPerSecond.x * 0.02f, -0.03f, 0.03f) * look.v[Bounce] - TurnRoll) * std::clamp(dt / 0.15f, 0.0f, 1.0f);

	// Gunfire: shots seen as the ammo count dropping (the same ammo; at most a few at once: not a drop of the whole stack).
	int shots = 0;
	InventoryChanges::EntryData* ammo = Player->process ? ((MiddleHighProcess*)Player->process)->ammoInfo : nullptr;
	if (ammo && ammo->type) {
		if (ammo->type == AmmoType && AmmoCount >= 0 && ammo->countDelta < AmmoCount && AmmoCount - ammo->countDelta <= 10)
			shots = AmmoCount - ammo->countDelta;
		AmmoType = ammo->type;
		AmmoCount = ammo->countDelta;
	}
	else { AmmoType = nullptr; AmmoCount = -1; }
	// Each shot jolts the mount (a spring: kicked, rings once, settles) and the muzzle flash overloads the sensor; the auto
	// exposure then pulls the picture down for a moment.
	if (shots > 0) {
		const float j = look.v[GunJolt];
		JoltSpeed += D3DXVECTOR3(((rand() % 1000) / 1000.0f - 0.5f) * 0.25f, -0.35f - (rand() % 1000) / 4000.0f, ((rand() % 1000) / 1000.0f - 0.5f) * 0.6f) * j;
		if (look.v[Overload] > 0.0f) { FlashNow = 1.0f; DipNow = 1.0f; }
	}
	const float w0 = 6.2831853f * 7.0f;	// 7 Hz
	for (float left = dt; left > 0.0f; left -= 1.0f / 120.0f) {
		const float h = (std::min)(left, 1.0f / 120.0f);
		JoltSpeed += (-Jolt * (w0 * w0) - JoltSpeed * (2.0f * 0.35f * w0)) * h;
		Jolt += JoltSpeed * h;
	}
	FlashNow *= expf(-dt / 0.06f);
	DipNow *= expf(-dt / 0.4f);
	Constants.Body.x = steps.x + Jolt.x;
	Constants.Body.y = steps.y + Jolt.y;
	Constants.Body.z = steps.z + TurnRoll + Jolt.z;
	Constants.Body.w = 1.0f + look.v[Bounce] * 0.035f + look.v[GunJolt] * 0.02f;	// zoomed in enough that no edge shows
	Constants.Flash.x = FlashNow * look.v[Overload];
	Constants.Flash.y = (std::max)(DipNow - FlashNow, 0.0f) * look.v[Overload];
	Constants.Flash.z = look.v[LowLight];
	Constants.Flash.w = look.v[LightSmear];

	// Encoder hiccups: now and then (more often while the view moves) a frozen picture (a dropped or repeated frame) or a short
	// burst of broken blocks.
	const float hiccups = look.v[Hiccups];
	FreezeLeft = (std::max)(FreezeLeft - dt, 0.0f);
	GlitchLeft = (std::max)(GlitchLeft - dt, 0.0f);
	if (hiccups > 0.0f) {
		HiccupWait -= dt * (1.0f + MotionLevel * 2.0f);
		if (HiccupWait <= 0.0f) {
			if (rand() % 2) FreezeLeft = 0.07f + (rand() % 1000) / 1000.0f * 0.15f;
			else { GlitchLeft = 0.1f + (rand() % 1000) / 1000.0f * 0.25f; Constants.Glitch.y = (float)(rand() % 997); }
			const float mean = 20.0f + (3.0f - 20.0f) * hiccups;	// seconds between hiccups on average
			HiccupWait = -logf((std::max)((rand() % 1000 + 1) / 1001.0f, 1e-3f)) * mean;
		}
	}
	else { FreezeLeft = GlitchLeft = 0.0f; }
	Constants.Glitch.x = GlitchLeft > 0.0f ? 0.5f + 0.5f * hiccups : 0.0f;
	Constants.Glitch.z = look.v[Autofocus];
	Constants.Glitch.w = hiccups;

	// The timestamp: the game's date and clock; the seconds run in real time (the game clock runs 30 times faster, so its
	// seconds would blur).
	TimeGlobals* time = TimeGlobals::Get();
	int year = 2281, month = 10, day = 1, hour = 0, minute = 0;
	if (time && time->GameYear && time->GameMonth && time->GameDay && time->GameHour) {
		year = (int)time->GameYear->data;
		month = (int)time->GameMonth->data + 1;	// the game counts months from 0
		day = (int)time->GameDay->data;
		const float h = fmodf(time->GameHour->data, 24.0f);
		hour = (int)h;
		minute = std::clamp((int)((h - hour) * 60.0f), 0, 59);
	}
	const int second = (int)fmod(Clock, 60.0);
	char line1[65], line2[65];
	std::snprintf(line1, sizeof(line1), "%04d-%02d-%02d T%02d:%02d:%02dZ", year, std::clamp(month, 1, 12), std::clamp(day, 1, 31), hour, minute, second);
	std::snprintf(line2, sizeof(line2), "NCR BODY CAM X117");
	if (strcmp(line1, Shown[0]) || strcmp(line2, Shown[1])) WriteText(line1, line2);
}

// A 3 x 5 font: five rows of three pixels each ('#' lit), for the characters the timestamp uses.
static const char* Glyph(char c) {
	switch (c) {
	case '0': return "###" "#.#" "#.#" "#.#" "###";
	case '1': return ".#." "##." ".#." ".#." "###";
	case '2': return "###" "..#" "###" "#.." "###";
	case '3': return "###" "..#" "###" "..#" "###";
	case '4': return "#.#" "#.#" "###" "..#" "..#";
	case '5': return "###" "#.." "###" "..#" "###";
	case '6': return "###" "#.." "###" "#.#" "###";
	case '7': return "###" "..#" ".#." ".#." ".#.";
	case '8': return "###" "#.#" "###" "#.#" "###";
	case '9': return "###" "#.#" "###" "..#" "###";
	case '-': return "..." "..." "###" "..." "...";
	case ':': return "..." ".#." "..." ".#." "...";
	case 'A': return ".#." "#.#" "###" "#.#" "#.#";
	case 'B': return "##." "#.#" "##." "#.#" "##.";
	case 'C': return ".##" "#.." "#.." "#.." ".##";
	case 'D': return "##." "#.#" "#.#" "#.#" "##.";
	case 'M': return "#.#" "###" "###" "#.#" "#.#";
	case 'N': return "##." "#.#" "#.#" "#.#" "#.#";
	case 'O': return ".#." "#.#" "#.#" "#.#" ".#.";
	case 'R': return "##." "#.#" "##." "#.#" "#.#";
	case 'T': return "###" ".#." ".#." ".#." ".#.";
	case 'X': return "#.#" "#.#" ".#." "#.#" "#.#";
	case 'Y': return "#.#" "#.#" ".#." ".#." ".#.";
	case 'Z': return "###" "..#" ".#." "#.." "###";
	default: return nullptr;	// a space, or a character the font lacks
	}
}

// Draws the two lines into the text texture (white, alpha = lit), right-aligned: each character a 4 x 6 cell (3 x 5 glyph and a
// gap), line 2 under line 1. Text.z / Text.w: the size of the block the shader shows (texels).
void BodycamEffect::WriteText(const char* line1, const char* line2) {
	strncpy_s(Shown[0], line1, 64);
	strncpy_s(Shown[1], line2, 64);
	const int columns = (int)max(strlen(Shown[0]), strlen(Shown[1]));
	Constants.Text.z = (float)max(columns * 4 - 1, 1);
	Constants.Text.w = (float)TextHeight - 1.0f;
	if (!TextTexture) return;
	D3DLOCKED_RECT locked;
	if (FAILED(TextTexture->LockRect(0, &locked, NULL, 0))) return;
	for (int y = 0; y < TextHeight; y++) memset((char*)locked.pBits + y * locked.Pitch, 0, TextWidth * 4);
	for (int line = 0; line < 2; line++) {
		const char* text = Shown[line];
		const int length = (int)strlen(text);
		const int start = (columns - length) * 4;	// right-aligned
		for (int i = 0; i < length; i++) {
			const char* g = Glyph(text[i]);
			if (!g) continue;
			for (int gy = 0; gy < 5; gy++) for (int gx = 0; gx < 3; gx++) {
				if (g[gy * 3 + gx] != '#') continue;
				const int x = start + i * 4 + gx, y = line * 6 + gy;
				if (x < TextWidth && y < TextHeight) ((DWORD*)((char*)locked.pBits + y * locked.Pitch))[x] = 0xFFFFFFFF;
			}
		}
	}
	TextTexture->UnlockRect(0);
}

// UNOFFICIAL 2026-10-13: before the picture, the brightness measure (technique 1: the picture's brightness now, and the camera's
// exposure following it slowly); then the picture, unless the video frame shown is still the held one (VideoFps): then the
// shader shows that frame again. After drawing a new video frame, it is kept.
void BodycamEffect::Render(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, IDirect3DSurface9* RenderedSurface, UINT techniqueIndex,
	bool ClearRenderTarget, IDirect3DSurface9* SourceBuffer) {
	if (!Enabled || !Effect) { EffectRecord::Render(Device, RenderTarget, RenderedSurface, techniqueIndex, ClearRenderTarget, SourceBuffer); return; }
	const bool stale = LastRender < 0.0 || Clock - LastRender > 0.25;	// a menu, a load: start again
	LastRender = Clock;
	if (stale) { HoldValid = false; MeasureValid = false; }

	if ((Constants.Adapt.x > 0.0f || Constants.Glitch.z > 0.0f) && MeasureSurface[0] && MeasureSurface[1]) {
		const int write = 1 - MeasureIndex;
		MeasureRead = MeasureTexture[MeasureIndex];			// last frame's, read by the measure pass
		Constants.Adapt.w = MeasureValid ? 1.0f : 0.0f;
		Device->SetRenderTarget(0, MeasureSurface[write]);
		EffectRecord::Render(Device, MeasureSurface[write], MeasureSurface[write], 1, false, MeasureSurface[write]);
		Device->SetRenderTarget(0, RenderTarget);
		MeasureIndex = write;
		MeasureRead = MeasureTexture[write];				// this frame's, read by the picture
		MeasureValid = true;
		Constants.Adapt.w = 1.0f;
	}
	else MeasureValid = false;

	const double tick = VideoFrameRate >= 1.0f ? floor(Clock * VideoFrameRate) : -1.0;
	// (A hiccup's frozen picture: the last kept frame shown again; a frame is kept every frame while hiccups are on.)
	const bool hold = HoldValid && ((VideoFrameRate >= 1.0f && tick == HoldTick) || FreezeLeft > 0.0f);
	Constants.Video.w = hold ? 1.0f : 0.0f;
	// UNOFFICIAL 2026-10-13, diagnostic (user: VideoFps does not seem to work): every 5 s while it is on (8 times), how many
	// frames were drawn and held, the game's frame rate, and where the kept frame came from.
	static int drawnCount = 0, heldCount = 0, reports = 0;
	static double reportStart = -1.0;
	static const char* source = "none";
	if (VideoFrameRate >= 1.0f && reports < 8) {
		if (reportStart < 0.0) reportStart = Clock;
		(hold ? heldCount : drawnCount)++;
		if (Clock - reportStart >= 5.0) {
			Logger::Log("UNOFFICIAL Bodycam video: %d drawn, %d held in %.1f s (game %.0f fps, video %.0f fps); kept frame from %s, chain %s",
				drawnCount, heldCount, Clock - reportStart, (drawnCount + heldCount) / (Clock - reportStart), VideoFrameRate, source,
				TheShaderManager->Chain.IsActive() ? "on" : "off");
			drawnCount = heldCount = 0; reportStart = Clock; reports++;
		}
	}
	EffectRecord::Render(Device, RenderTarget, RenderedSurface, 0, ClearRenderTarget, SourceBuffer);
	if (hold) return;
	if (VideoFrameRate < 1.0f && Constants.Glitch.w <= 0.0f) { HoldValid = false; return; }

	// Keep the new video frame: where the picture went (the chain's current image, the game target if Bodycam drew straight
	// into it, or the target on the old path).
	FrameChain& chain = TheShaderManager->Chain;
	IDirect3DSurface9* result = !chain.IsActive() ? RenderTarget : chain.IsDirectFinal(this) ? chain.FinalSurface() : TheTextureManager->RenderedSurface;
	source = !chain.IsActive() ? "the target (no chain)" : chain.IsDirectFinal(this) ? "the game target (direct final)" :
		result == RenderedSurface ? "the chain's current image (same as the input!)" : "the chain's current image";
	D3DSURFACE_DESC desc;
	if (!result || FAILED(result->GetDesc(&desc))) { HoldValid = false; return; }
	if (!HoldSurface || desc.Width != HoldDesc.Width || desc.Height != HoldDesc.Height || desc.Format != HoldDesc.Format) {
		if (HoldSurface) { HoldSurface->Release(); HoldSurface = nullptr; }
		if (HoldTexture) { HoldTexture->Release(); HoldTexture = nullptr; }
		if (FAILED(Device->CreateTexture(desc.Width, desc.Height, 1, D3DUSAGE_RENDERTARGET, desc.Format, D3DPOOL_DEFAULT, &HoldTexture, NULL))) {
			HoldTexture = nullptr;
			Logger::Log("[ERROR] Bodycam: could not create the held video frame (%ux%u, format %u)", desc.Width, desc.Height, (unsigned)desc.Format);
			HoldDesc = {};
			HoldValid = false;
			return;
		}
		HoldTexture->GetSurfaceLevel(0, &HoldSurface);
		HoldDesc = desc;
		Logger::Log("UNOFFICIAL Bodycam: video frames held at %ux%u, format %u", desc.Width, desc.Height, (unsigned)desc.Format);
	}
	HoldValid = SUCCEEDED(Device->StretchRect(result, NULL, HoldSurface, NULL, D3DTEXF_NONE));
	HoldTick = tick;
}

// UNOFFICIAL 2026-10-25 (NVR DXVK): the two passes as Bodycam.frag and BodycamMeasure.frag (EffectRecord::ExtFullScreen; the frame
// chain, the direct final write and the copies are its). The textures and constants are those the D3D9 techniques read. The
// picture reads the hold frame, the text and the measure as they are at this moment, as the D3D9 pass does.
bool BodycamEffect::ExtRender(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, IDirect3DSurface9* RenderedSurface, UINT techniqueIndex,
	bool ClearRenderTarget, IDirect3DSurface9* SourceBuffer) {
	IDirect3DTexture9* depth = TheShaderManager->Effects.CombineDepth ? TheShaderManager->Effects.CombineDepth->Textures.CombinedDepthTexture : nullptr;
	if (techniqueIndex == 1) {
		struct Push { D3DXVECTOR4 adapt; UInt32 linearIndex; UInt32 pointIndex; } push = {};
		push.adapt = Constants.Adapt;
		IDirect3DBaseTexture9* textures[] = { TheTextureManager->RenderedTexture, MeasureRead, depth };
		return ExtTwoSamplers(Device, RenderTarget, RenderedSurface, SourceBuffer, extMeasure, "BodycamMeasure", textures, _countof(textures), &push, sizeof(push), 16);
	}
	if (techniqueIndex != 0) return false;
	struct Push {
		D3DXVECTOR4 reciprocal, lens, image, noise, text, frame, motion, grade, video, adapt, body, flash, glitch;
		UInt32 linearIndex, pointIndex;	// the sampler heap indices: the extension fills them in
	} push = {};
	push.reciprocal = TheShaderManager->ShaderConst.ReciprocalResolution;
	push.lens = Constants.Lens;
	push.image = Constants.Image;
	push.noise = Constants.Noise;
	push.text = Constants.Text;
	push.frame = Constants.Frame;
	push.motion = Constants.Motion;
	push.grade = Constants.Grade;
	push.video = Constants.Video;
	push.adapt = Constants.Adapt;
	push.body = Constants.Body;
	push.flash = Constants.Flash;
	push.glitch = Constants.Glitch;
	IDirect3DBaseTexture9* textures[] = { TheTextureManager->RenderedTexture, TextTexture, HoldTexture, MeasureRead, depth };
	return ExtTwoSamplers(Device, RenderTarget, RenderedSurface, SourceBuffer, extPass, "Bodycam", textures, _countof(textures), &push, sizeof(push), 208);
}

bool BodycamEffect::ExtTwoSamplers(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, IDirect3DSurface9* RenderedSurface, IDirect3DSurface9* SourceBuffer,
	ExtPass& pass, const char* spvName, IDirect3DBaseTexture9* const* textures, UINT count, const void* push, UINT pushSize, UINT samplerPushOffset) {
	if (!pass.Tried) {
		pass.Tried = true;
		NVR_EXT_BINDING_DESC bindings[7] = {};
		for (UINT i = 0; i < count; i++) {
			bindings[i].Set = 1; bindings[i].Binding = i; bindings[i].Type = NVR_EXT_BIND_SAMPLED_IMAGE; bindings[i].Slot = i; bindings[i].ViewType = NVR_EXT_VIEW_2D;
		}
		bindings[count].Type = NVR_EXT_BIND_SAMPLER; bindings[count].Slot = 0; bindings[count].PushOffset = samplerPushOffset;
		bindings[count + 1].Type = NVR_EXT_BIND_SAMPLER; bindings[count + 1].Slot = 1; bindings[count + 1].PushOffset = samplerPushOffset + 4;
		pass.Pipeline = DxvkCompute::CreateGraphicsPipeline(spvName, bindings, count + 2, pushSize, true);
	}
	ID3D9NvrExtSampler* linear = DxvkCompute::LinearClamp();
	ID3D9NvrExtSampler* point = DxvkCompute::PointClamp();
	if (!pass.Pipeline || !linear || !point) return false;
	NVR_EXT_RESOURCE_BINDING resources[7] = {};
	for (UINT i = 0; i < count; i++) {
		if (!textures[i]) return false;
		resources[i].Slot = i; resources[i].Type = NVR_EXT_BIND_SAMPLED_IMAGE; resources[i].pResource = textures[i];
	}
	resources[count].Slot = 0; resources[count].Type = NVR_EXT_BIND_SAMPLER; resources[count].pResource = linear;
	resources[count + 1].Slot = 1; resources[count + 1].Type = NVR_EXT_BIND_SAMPLER; resources[count + 1].pResource = point;
	return ExtFullScreen(Device, RenderTarget, RenderedSurface, SourceBuffer, pass.Pipeline, resources, count + 2, push, pushSize);
}

// UNOFFICIAL 2026-10-13 (user: the wide view as an option): [Shaders.Bodycam.Main] WideView, the world's field of view while
// Bodycam is on (a body camera sees much wider than the game). Eased in and out; left to the game while aiming (its zoom) and in
// menus; the game's own value back when Bodycam or WideView goes off. Only the live value is changed, never the game's INI
// settings, so nothing can be saved with it. Not while [Main.CameraMode] sets its own.
void BodycamEffect::ApplyWideView() {
	static bool widened = false;
	static float before = 0.0f, current = 0.0f;
	BodycamEffect* cam = TheShaderManager->Effects.Bodycam;
	// (Also the microphone, every frame: on while Bodycam is, off at once when it is switched off; installed on first use.)
	BodycamAudio::Settings mic = MicSettings;
	if (!cam || !cam->Enabled) mic.Mix = 0.0f;
	BodycamAudio::Wanted = mic;
	if (mic.Mix > 0.0f) BodycamAudio::Install();
	if (!Player || !WorldSceneGraph || TheSettingManager->SettingsMain.CameraMode.Enabled) return;
	const float wide = cam && cam->Enabled ? cam->WideView : 0.0f;
	const bool free = !Player->IsAiming() && InterfaceManager->IsActive(Menu::MenuType::kMenuType_None) && !Player->IsVanity();
	const float dt = (float)std::clamp((double)TheFrameRateManager->ElapsedTime, 0.0, 0.1);
	auto set = [](float fov) {
		Player->worldFoV = fov;
		if (fabsf(WorldSceneGraph->cameraFOV - fov) > 0.01f) WorldSceneGraph->UpdateParticleShaderFoV(fov);
	};
	if (wide > 0.0f) {
		// UNOFFICIAL 2026-11-02 (hidden-state review): the FOV to go back to is taken only while the view is free. Taken while aiming
		// (the game's zoomed FOV) it stuck after Bodycam was switched off. [Shaders.Bodycam.Main] FreeViewRestore false = as before.
		if (!widened && (free || !TheSettingManager->GetSettingI("Shaders.Bodycam.Main", "FreeViewRestore"))) { before = current = Player->worldFoV; widened = true; }
		if (!widened) return;	// switched on while aiming / in a menu: start once the view is free
		if (!free) { current = Player->worldFoV; return; }
		current = fabsf(wide - current) < 0.05f ? wide : current + (wide - current) * (std::min)(1.0f, dt * 6.0f);
		set(current);
	}
	else if (widened && free) {
		widened = false;
		set(before);
	}
}
