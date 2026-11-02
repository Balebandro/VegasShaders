#pragma once

// UNOFFICIAL 2026-10-20 grass field wind ([Shaders.Grass.Wind], OFF by default: a look change the user judges). Shaders:
// Includes/GrassWind.hlsl (the maths), GRASS23x000-003.vso (the game's grass cards), NewGrass.vso (NewGrass strands). Plan, limits
// and results: bench\deep-roadmap\evidence\grasswind-LOG.md.
//
// The grass moves like a field in the wind instead of every tuft swaying on its own: gusts roll across the field along the weather's
// wind like waves through wheat (always, stronger and faster as the weather's wind rises), each tuft flutters a little on its own so
// neighbours never move in lockstep, and the game's own sway is turned onto the weather's wind direction. Direction and wind speed
// are the ones cloth uses (core/ClothWind.h: sky->windDirection as an angle, sky->windSpeed 0-1), so the grass and the coats agree;
// the game itself sways all grass along world +Y whatever the weather (TallGrassShader::SetupTransformations 0x00BAAC10, from
// WallSoGB's map). Strength: multiples of the game's own sway (fGrassWindMagnitudeMin + windSpeed x (Max - Min) times the model's
// vertex alpha squared), so every grass model keeps its authored stiffness. Idea credited: the cloth wind here and Sousa's
// travelling-wave vegetation animation (GPU Gems 3 ch. 16, 2007); the sandstorm's grass gusts (Physically-Based-Reloaded, GPL-3.0)
// were the first moving fronts in this shader.
//
// Constants (see GrassWind.hlsl): the card shaders have no free register, so the four lanes (gust wave vector xy, gust lean,
// flutter) ride in the 4th column of the inverse view matrix rows c252-c255, which the shadow lookup never reads; SetForDraw writes
// those rows right after the shader's constant upload (same SetShadersHook). Off, nothing is written: that column is the matrix's
// own 0 0 0 1 (RenderManager builds it so), the shaders' branch is not taken and the grass sways exactly as the game's.
// The clock goes to c19.z, which the sandstorm's grass wind shares (its gusts at clock / 4, flutter at clock): while a storm blows
// the clock runs at a blend of this wind's rate and the storm's 8.4 rad/s, so the storm's grass keeps its pace.
// Phases are integrated from the game's clock in double precision and wrapped (8 pi: the gusts at clock / 4 and the flutter at
// 2 x clock both wrap whole), as ClothWind.h does: a pause or menu stops them, the time scale does not change the speed, and the
// motion never jitters after long play.
#include <cmath>

namespace GrassWind {

	struct SettingsStruct {
		bool	Enabled = false;
		float	GustStrength = 2.0f;	// how far the gusts lean the grass at their crest, times the game's own sway
		float	GustSize = 1200.0f;		// units from one gust front to the next (the waves' length)
		float	GustSpeed = 300.0f;		// units per second the gusts roll at (x 0.4 in dead calm .. x 1.6 in a gale: cloth's curve)
		float	Flutter = 0.3f;			// each tuft's own quiver, times the game's own sway (8 quivers per passing gust front)
		bool	UseGameWind = true;		// the weather's wind direction (as cloth); false: Direction
		float	Direction = 0.0f;		// degrees from +Y (north) toward +X (east), with UseGameWind off
		float	TimeOffset = 0.0f;		// test only: seconds the pattern is ahead (pictures at set moments)
		int		Log = 0;				// diagnostic: 1 = the game's grass wind (c4), the weather's wind and these lanes in the log every 600 grass binds
	};
	inline SettingsStruct S;

	inline float Lanes[4] = {};			// gust wave vector xy (radians per unit), gust lean, flutter; all 0 = off
	inline float ClockNow = 0.0f;		// the clock the shaders get (radians, 0..8 pi, TimeOffset applied)
	inline double Clock = 0.0;			// integrated (radians), wrapped at 8 pi
	inline double Rate = 0.0;			// its rate now (radians per second of play)
	inline double ClockSeconds = 0.0;	// seconds of play at the normal time scale (ClothWind::AdvanceClock's rule)
	inline float LastHour = -1.0f;
	inline UInt32 LastFrame = 0xFFFFFFFF;
	inline bool On = false;				// the lanes are in use this frame (Enabled, outdoors)
	inline bool LanesSent = false;		// SetForDraw left our lanes in c252-c255 (a clean matrix is written once after turning off)
	inline UInt32 Frames = 0;
	inline float WindDir[2] = { 0.0f, 1.0f }, WindSpeed = 0.0f, Speed = 0.0f;
	inline bool LoggedOnce = false;
	inline bool ClockWanted = false;	// UNOFFICIAL 2026-10-20 plant wind (core/PlantWind.h): the plants share this wind and clock, so it runs while they are on

	static const double kWrap = 8.0 * 3.14159265358979323846;

	inline void ReadSettings() {
		const char* s = "Shaders.Grass.Wind";
		S.Enabled = TheSettingManager->GetSettingI(s, "Enabled") != 0;
		S.GustStrength = std::clamp(TheSettingManager->GetSettingF(s, "GustStrength"), 0.0f, 10.0f);
		S.GustSize = std::clamp(TheSettingManager->GetSettingF(s, "GustSize"), 50.0f, 20000.0f);
		S.GustSpeed = std::clamp(TheSettingManager->GetSettingF(s, "GustSpeed"), 0.0f, 3000.0f);
		S.Flutter = std::clamp(TheSettingManager->GetSettingF(s, "Flutter"), 0.0f, 10.0f);
		S.UseGameWind = TheSettingManager->GetSettingI(s, "UseGameWind") != 0;
		S.Direction = TheSettingManager->GetSettingF(s, "Direction");
		S.TimeOffset = TheSettingManager->GetSettingF(s, "TimeOffset");
		S.Log = TheSettingManager->GetSettingI(s, "Log");
	}

	// Seconds of play from the game's hour (as ClothWind::AdvanceClock): stops in a pause or menu, not scaled by the time scale; a
	// wait or sleep moves on by at most a frame's worth.
	inline double AdvanceClock() {
		TimeGlobals* g = TimeGlobals::Get();
		if (!g || !g->GameHour) return 0.0;
		const float hour = g->GameHour->data;
		const double scale = g->TimeScale && g->TimeScale->data > 0.0f ? (double)g->TimeScale->data : 30.0;
		double dt = 0.0;
		if (LastHour >= 0.0f) {
			double dh = (double)hour - (double)LastHour;
			if (dh < -12.0) dh += 24.0;		// past midnight
			if (dh > 0.0) dt = (std::min)(dh * 3600.0 / scale, 0.1);
		}
		LastHour = hour;
		ClockSeconds += dt;
		return dt;
	}

	// Once a rendered frame (GrassShaders::UpdateConstants runs twice a frame: the first call moves the clock).
	inline void Update(const Sky* sky, bool exterior, float stormIntensity) {
		const UInt32 frame = TheFrameRateManager ? TheFrameRateManager->FrameNumber : 0;
		if (frame == LastFrame) return;
		LastFrame = frame;
		On = S.Enabled && exterior;
		if (!On && !(ClockWanted && exterior)) { for (float& l : Lanes) l = 0.0f; LastHour = -1.0f; return; }

		// The weather's wind, as cloth reads it: direction as an angle around up, speed 0-1.
		float a = S.Direction * 0.0174532925f, w = 0.5f;
		if (S.UseGameWind && sky) { a = sky->windDirection; w = std::clamp(sky->windSpeed, 0.0f, 1.0f); }
		WindDir[0] = sinf(a); WindDir[1] = cosf(a); WindSpeed = w;
		const double k = 2.0 * 3.14159265358979323846 / S.GustSize;
		Lanes[0] = (float)(WindDir[0] * k); Lanes[1] = (float)(WindDir[1] * k);
		Lanes[2] = S.GustStrength; Lanes[3] = S.Flutter;
		if (!On) for (float& l : Lanes) l = 0.0f;	// (the grass wind off, the plants on: the grass keeps the game's sway)

		// The clock: the gusts (clock / 4) roll at Speed along the wind, so the clock runs at 4 x Speed x k; in a sandstorm its rate
		// blends to the storm's own 8.4 rad/s (its grass path reads the same c19.z).
		Speed = S.GustSpeed * (0.4f + 1.2f * w);
		const double ours = 4.0 * Speed * k, storm = 8.4, i = std::clamp((double)stormIntensity, 0.0, 1.0);
		Rate = ours + (storm - ours) * i;
		const double dt = AdvanceClock();
		Clock = fmod(Clock + dt * Rate, kWrap);
		double now = fmod(Clock + (double)S.TimeOffset * Rate, kWrap);
		if (now < 0.0) now += kWrap;
		ClockNow = (float)now;
	}

	// GrassShaders::SetForDraw, for NVR's grass card vertex shaders, right after SetCT put the inverse view matrix in c252-c255: the
	// same rows with the lanes in their 4th column. Off, the rows are left alone (once cleaned after turning off).
	inline void BindCards(IDirect3DDevice9* device) {
		if (!On && !LanesSent) return;
		float rows[16];
		memcpy(rows, &TheRenderManager->InvViewMatrix, sizeof(rows));
		if (On) { rows[3] = Lanes[0]; rows[7] = Lanes[1]; rows[11] = Lanes[2]; rows[15] = Lanes[3]; }
		device->SetVertexShaderConstantF(252, rows, 4);
		LanesSent = On;
	}

	// Diagnostic (Log 1), every 600 grass binds from GrassShaders::SetForDraw: what the game gives its grass shader in c4 (its sway axis,
	// size and phase) against the weather's wind, the matrix column the lanes ride in, the lanes, and where the grass geometry being
	// drawn stands (its instance positions are world positions only if that is the world origin).
	inline void LogState(IDirect3DDevice9* device) {
		if (!S.Log || ++Frames < 600) return;
		Frames = 0;
		const NiAVObject* geometry = *(NiAVObject**)(*(void**)0x011F91E0);	// the game's current geometry (as SetShadersHook reads it)
		float c4[4] = {}, c252[16] = {};
		const bool got = SUCCEEDED(device->GetVertexShaderConstantF(4, c4, 1)) && SUCCEEDED(device->GetVertexShaderConstantF(252, c252, 4));
		const D3DXMATRIX& m = TheRenderManager->InvViewMatrix;
		Logger::Log("UNOFFICIAL Grass wind: %s; weather wind direction %.3f (%.3f %.3f) speed %.3f; game grass c4 %s%.3f %.3f size %.2f phase %.3f; "
			"lanes %.5f %.5f %.2f %.2f, clock %.3f (rate %.3f rad/s, gusts %.0f units/s); c252-c255 .w %.5f %.5f %.2f %.2f (matrix's own %.1f %.1f %.1f %.1f); "
			"grass geometry at %.0f %.0f %.0f",
			On ? "on" : "off", atan2f(WindDir[0], WindDir[1]), WindDir[0], WindDir[1], WindSpeed, got ? "" : "(not readable) ", c4[0], c4[1], c4[2], c4[3],
			Lanes[0], Lanes[1], Lanes[2], Lanes[3], ClockNow, Rate, Speed, c252[3], c252[7], c252[11], c252[15], m._14, m._24, m._34, m._44,
			geometry ? geometry->m_worldTransform.pos.x : 0.0f, geometry ? geometry->m_worldTransform.pos.y : 0.0f, geometry ? geometry->m_worldTransform.pos.z : 0.0f);
	}
}
