#include "GrassCollision.h"

#include <algorithm>
#include <cmath>
#include <vector>

// UNOFFICIAL 2026-10-19 grass collision: see GrassCollision.h (after Community Shaders' Grass Collision, GPL-3.0).

namespace {
	const int N = 128;                 // cells a side, the grid and the texture
	const UInt32 ProcessListsAddress = 0x11E0E80;   // ProcessLists (as BloodFlow.cpp reads it)

	struct SettingsStruct {
		bool	Enabled = false;
		float	Strength = 28.0f;      // push at a blade's tip right at an actor's feet (world units)
		float	Radius = 1.0f;         // footprint size, times the actor's own
		float	Recovery = 2.5f;       // seconds for the grass to spring (about all the way) back
		float	Range = 1536.0f;       // how far round the camera actors push (world units, each way)
		int		Actors = 16;           // nearest actors that push
	} Settings;

	std::vector<float>	Field(N * N * 2, 0.0f);   // world-locked: cell (i, j) centred at ((OriginX + i + 0.5) Cell, (OriginY + j + 0.5) Cell)
	int		OriginX = INT32_MIN, OriginY = INT32_MIN;
	float	Cell = 24.0f;
	IDirect3DTexture9*	Texture = nullptr;
	bool	Checked = false, Supported = false, Filtered = false;
	float	Scale = 0.0f;              // published: 1 / the texture's width (0 off)
	double	LastTime = -1.0;
	bool	FieldEmpty = true;
	int		MinI = N, MinJ = N, MaxI = -1, MaxJ = -1;   // the cells holding a push (none: Max < Min); only texels that reach them are sampled

	struct Pusher { float x, y, radius; };

	// The actors in the high and middle-high process levels with their 3D loaded (ProcessLists, as BloodFlow.cpp reads them).
	int ReadActors(Pusher* out, int max, float cx, float cy, float range) {
		int n = 0;
		__try {
			UInt8* lists = (UInt8*)ProcessListsAddress;
			Actor** data = *(Actor***)(lists + 0x08);
			if (!data) return 0;
			for (int level = 0; level < 2; level++) {
				UInt32 begin = *(UInt32*)(lists + 0x14 + level * 4);
				UInt32 end = *(UInt32*)(lists + 0x24 + level * 4);
				if (end < begin || end - begin > 4096) continue;
				for (UInt32 i = begin; i < end && n < max; i++) {
					Actor* actor = data[i];
					if (!actor || (TESObjectREFR*)actor == (TESObjectREFR*)Player) continue;
					if ((actor->flags & 0x800) != 0) continue;   // disabled
					NiNode* node = actor->GetNiNode();
					if (!node) continue;
					const float dx = actor->pos.x - cx, dy = actor->pos.y - cy;
					if (dx * dx + dy * dy > range * range) continue;
					out[n++] = Pusher{ actor->pos.x, actor->pos.y, node->GetWorldBoundRadius() };
				}
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER) { return n; }
		return n;
	}

	void Clear() {
		std::fill(Field.begin(), Field.end(), 0.0f);
		FieldEmpty = true;
		MinI = MinJ = N;
		MaxI = MaxJ = -1;
	}

	// The grid follows the camera in whole cells: what stays in range keeps its push.
	void Scroll(int ox, int oy) {
		if (OriginX == INT32_MIN || abs(ox - OriginX) >= N || abs(oy - OriginY) >= N) { Clear(); OriginX = ox; OriginY = oy; return; }
		if (ox == OriginX && oy == OriginY) return;
		std::vector<float> moved(N * N * 2, 0.0f);
		const int sx = ox - OriginX, sy = oy - OriginY;
		for (int j = 0; j < N; j++) {
			const int oj = j + sy;
			if (oj < 0 || oj >= N) continue;
			for (int i = 0; i < N; i++) {
				const int oi = i + sx;
				if (oi < 0 || oi >= N) continue;
				moved[(j * N + i) * 2] = Field[(oj * N + oi) * 2];
				moved[(j * N + i) * 2 + 1] = Field[(oj * N + oi) * 2 + 1];
			}
		}
		Field.swap(moved);
		OriginX = ox;
		OriginY = oy;
	}

	// One footprint: pushed straight out from the centre, full strength under the actor, smoothly to nothing at 2.5 radii.
	void Stamp(float x, float y, float radius) {
		const float inner = radius * 0.5f, outer = radius * 2.5f;
		const int i0 = (std::max)(0, (int)floorf((x - outer) / Cell) - OriginX), i1 = (std::min)(N - 1, (int)floorf((x + outer) / Cell) - OriginX);
		const int j0 = (std::max)(0, (int)floorf((y - outer) / Cell) - OriginY), j1 = (std::min)(N - 1, (int)floorf((y + outer) / Cell) - OriginY);
		for (int j = j0; j <= j1; j++) {
			const float cy = (OriginY + j + 0.5f) * Cell - y;
			for (int i = i0; i <= i1; i++) {
				const float cx = (OriginX + i + 0.5f) * Cell - x;
				const float d = sqrtf(cx * cx + cy * cy);
				if (d >= outer || d < 0.01f) continue;
				float t = std::clamp((d - inner) / (outer - inner), 0.0f, 1.0f);
				const float s = Settings.Strength * (1.0f - t * t * (3.0f - 2.0f * t));
				const float px = cx / d * s, py = cy / d * s;
				float* f = &Field[(j * N + i) * 2];
				if (px * px + py * py > f[0] * f[0] + f[1] * f[1]) {
					f[0] = px; f[1] = py; FieldEmpty = false;
					MinI = (std::min)(MinI, i); MaxI = (std::max)(MaxI, i); MinJ = (std::min)(MinJ, j); MaxJ = (std::max)(MaxJ, j);
				}
			}
		}
	}

	// The grid at a world point, bilinear between cell centres (0 outside).
	void Sample(float x, float y, float& px, float& py) {
		const float fx = x / Cell - OriginX - 0.5f, fy = y / Cell - OriginY - 0.5f;
		const int i = (int)floorf(fx), j = (int)floorf(fy);
		const float tx = fx - i, ty = fy - j;
		px = py = 0.0f;
		for (int b = 0; b < 2; b++)
			for (int a = 0; a < 2; a++) {
				const int ii = i + a, jj = j + b;
				if (ii < 0 || jj < 0 || ii >= N || jj >= N) continue;
				const float w = (a ? tx : 1.0f - tx) * (b ? ty : 1.0f - ty);
				px += Field[(jj * N + ii) * 2] * w;
				py += Field[(jj * N + ii) * 2 + 1] * w;
			}
	}

	bool CheckSupport(IDirect3DDevice9* Device) {
		IDirect3D9* d3d = nullptr;
		D3DDEVICE_CREATION_PARAMETERS creation = {};
		D3DDISPLAYMODE mode = {};
		bool vertexTexture = false;
		if (SUCCEEDED(Device->GetDirect3D(&d3d)) && d3d && SUCCEEDED(Device->GetCreationParameters(&creation)) &&
			SUCCEEDED(Device->GetDisplayMode(0, &mode))) {
			vertexTexture = SUCCEEDED(d3d->CheckDeviceFormat(creation.AdapterOrdinal, creation.DeviceType, mode.Format,
				D3DUSAGE_QUERY_VERTEXTEXTURE, D3DRTYPE_TEXTURE, D3DFMT_A32B32G32R32F));
			Filtered = vertexTexture && SUCCEEDED(d3d->CheckDeviceFormat(creation.AdapterOrdinal, creation.DeviceType, mode.Format,
				D3DUSAGE_QUERY_VERTEXTEXTURE | D3DUSAGE_QUERY_FILTER, D3DRTYPE_TEXTURE, D3DFMT_A32B32G32R32F));
		}
		if (d3d) d3d->Release();
		Logger::Log("UNOFFICIAL grass collision: vertex textures %s%s", vertexTexture ? "supported" : "NOT supported (grass collision stays off)",
			vertexTexture ? (Filtered ? ", filtered" : ", unfiltered (point sampled)") : "");
		return vertexTexture;
	}
}

void GrassCollision::ReadSettings() {
	const char* section = "Shaders.Grass.Collision";
	const float range = Settings.Range;
	Settings.Enabled = TheSettingManager->GetSettingI(section, "Enabled") != 0;
	Settings.Strength = std::clamp(TheSettingManager->GetSettingF(section, "Strength"), 0.0f, 200.0f);
	Settings.Radius = std::clamp(TheSettingManager->GetSettingF(section, "Radius"), 0.1f, 5.0f);
	Settings.Recovery = std::clamp(TheSettingManager->GetSettingF(section, "Recovery"), 0.0f, 60.0f);
	Settings.Range = std::clamp(TheSettingManager->GetSettingF(section, "Range"), 256.0f, 8192.0f);
	Settings.Actors = std::clamp(TheSettingManager->GetSettingI(section, "Actors"), 0, 64);
	Cell = 2.0f * Settings.Range / N;
	if (Settings.Range != range) { OriginX = INT32_MIN; Clear(); }
}

void GrassCollision::Update() {
	Scale = 0.0f;
	if (!Settings.Enabled || !Player || !TheShaderManager->GameState.isExterior) {
		if (!FieldEmpty) Clear();
		return;
	}
	IDirect3DDevice9* Device = TheRenderManager->device;
	if (!Checked) { Checked = true; Supported = CheckSupport(Device); }
	if (!Supported) return;
	if (!Texture && FAILED(Device->CreateTexture(N, N, 1, 0, D3DFMT_A32B32G32R32F, D3DPOOL_MANAGED, &Texture, NULL))) {
		Logger::Log("[ERROR] Grass collision: could not create its %dx%d vertex texture; off", N, N);
		Texture = nullptr;
		Supported = false;
		return;
	}

	const D3DXVECTOR4& camera = TheRenderManager->CameraPosition;
	Scroll((int)floorf(camera.x / Cell) - N / 2, (int)floorf(camera.y / Cell) - N / 2);

	// Springing back: the push fades toward nothing, about all of it within Recovery seconds.
	const double now = TheFrameRateManager->Time;
	const float dt = LastTime < 0.0 ? 0.0f : (float)std::clamp(now - LastTime, 0.0, 0.25);
	LastTime = now;
	// (The cells still holding a push are found again as it fades: after a scroll they have moved.)
	MinI = MinJ = N;
	MaxI = MaxJ = -1;
	if (!FieldEmpty) {
		const float keep = Settings.Recovery > 0.0f ? expf(-3.0f * dt / Settings.Recovery) : 0.0f;
		for (int j = 0; j < N; j++)
			for (int i = 0; i < N; i++) {
				float* f = &Field[(j * N + i) * 2];
				f[0] *= keep; if (fabsf(f[0]) < 0.05f) f[0] = 0.0f;
				f[1] *= keep; if (fabsf(f[1]) < 0.05f) f[1] = 0.0f;
				if (f[0] != 0.0f || f[1] != 0.0f) {
					MinI = (std::min)(MinI, i); MaxI = (std::max)(MaxI, i); MinJ = (std::min)(MinJ, j); MaxJ = (std::max)(MaxJ, j);
				}
			}
		FieldEmpty = MaxI < 0;
	}

	// The player and the nearest actors (their 3D's bound: about half a person's height for a person).
	Pusher pushers[65];
	int count = 0;
	if (Player->GetNiNode()) pushers[count++] = Pusher{ Player->pos.x, Player->pos.y, Player->GetNiNode()->GetWorldBoundRadius() };
	Pusher found[256];
	const int seen = ReadActors(found, 256, camera.x, camera.y, Settings.Range * 0.9f);
	std::sort(found, found + seen, [&](const Pusher& a, const Pusher& b) {
		return (a.x - camera.x) * (a.x - camera.x) + (a.y - camera.y) * (a.y - camera.y) < (b.x - camera.x) * (b.x - camera.x) + (b.y - camera.y) * (b.y - camera.y);
	});
	for (int i = 0; i < seen && count < Settings.Actors + 1; i++) pushers[count++] = found[i];
	for (int i = 0; i < count; i++) Stamp(pushers[i].x, pushers[i].y, std::clamp(pushers[i].radius * 0.35f, 10.0f, 200.0f) * Settings.Radius);

	// Into the camera-centred texture (its edge texels stay 0, so the clamp outside reads no push). A texel is only sampled when one
	// of its two cells a side (as Sample takes them) holds a push: the rest are the 0 Sample would give, and most of the grid is.
	D3DLOCKED_RECT rect;
	if (FAILED(Texture->LockRect(0, &rect, NULL, 0))) return;
	const float width = N * Cell;
	auto Reaches = [](float coordinate, int origin, int low, int high) {
		const int c = (int)floorf(coordinate / Cell - origin - 0.5f);
		return c + 1 >= low && c <= high;
	};
	bool columns[N];
	for (int i = 0; i < N; i++) columns[i] = !FieldEmpty && Reaches(camera.x + ((i + 0.5f) / N - 0.5f) * width, OriginX, MinI, MaxI);
	for (int j = 0; j < N; j++) {
		float* row = (float*)((BYTE*)rect.pBits + (size_t)j * rect.Pitch);
		const float y = camera.y + ((j + 0.5f) / N - 0.5f) * width;
		const bool rowReaches = !FieldEmpty && Reaches(y, OriginY, MinJ, MaxJ);
		for (int i = 0; i < N; i++) {
			float px = 0.0f, py = 0.0f;
			if (rowReaches && columns[i] && i > 0 && j > 0 && i < N - 1 && j < N - 1) Sample(camera.x + ((i + 0.5f) / N - 0.5f) * width, y, px, py);
			row[i * 4] = px; row[i * 4 + 1] = py; row[i * 4 + 2] = 0.0f; row[i * 4 + 3] = 0.0f;
		}
	}
	Texture->UnlockRect(0);
	Scale = 1.0f / width;
}

void GrassCollision::Bind(IDirect3DDevice9* Device) {
	if (Scale <= 0.0f || !Texture) return;
	Device->SetTexture(D3DVERTEXTEXTURESAMPLER0, Texture);
	const DWORD filter = Filtered ? D3DTEXF_LINEAR : D3DTEXF_POINT;
	Device->SetSamplerState(D3DVERTEXTEXTURESAMPLER0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
	Device->SetSamplerState(D3DVERTEXTEXTURESAMPLER0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
	Device->SetSamplerState(D3DVERTEXTEXTURESAMPLER0, D3DSAMP_MAGFILTER, filter);
	Device->SetSamplerState(D3DVERTEXTEXTURESAMPLER0, D3DSAMP_MINFILTER, filter);
	Device->SetSamplerState(D3DVERTEXTEXTURESAMPLER0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
}

float GrassCollision::FieldScale() { return Scale; }
