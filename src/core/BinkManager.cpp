namespace {
	// Creates one A8 texture for a Bink plane and records its byte-count (width * height).
	template <typename WidthT, typename HeightT>
	void MakePlaneTexture(IDirect3DDevice9* device, WidthT width, HeightT height, IDirect3DTexture9** target, UInt32* size) {
		device->CreateTexture(width, height, 1, 0, D3DFMT_A8, D3DPOOL_MANAGED, target, NULL);
		*size = width * height;
	}

	// Maps a locked texture into a Bink plane (buffer pointer and pitch).
	template <typename PlaneT>
	void MapPlane(IDirect3DTexture9* texture, PlaneT& plane, UInt32 lockFlags, D3DLOCKED_RECT& rect) {
		texture->LockRect(0, &rect, NULL, lockFlags);
		plane.Buffer = rect.pBits;
		plane.BufferPitch = rect.Pitch;
	}

	template <typename PlaneT>
	void UnmapPlane(IDirect3DTexture9* texture, PlaneT& plane) {
		texture->UnlockRect(0);
		plane.Buffer = NULL;
	}

	inline void ReleaseTexture(IDirect3DTexture9*& texture) {
		if (texture) {
			texture->Release();
			texture = NULL;
		}
	}

	// Full-screen quad corner: screen position plus texture coordinate.
	BinkManager::POS_TC_VERTEX Corner(float x, float y, float u, float v) {
		BinkManager::POS_TC_VERTEX corner;
		corner.sx = x;
		corner.sy = y;
		corner.sz = 0.0f;
		corner.rhw = 1.0f;
		corner.tu = u;
		corner.tv = v;
		return corner;
	}
}

void BinkManager::Initialize() {
	Logger::Log("Starting the bink manager...");
	BinkManager* player = new BinkManager();
	TheBinkManager = player;

	// pre-transformed position (xyzw) followed by one uv pair
	D3DVERTEXELEMENT9* layout = player->VertexDefinition;
	layout[0] = { 0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITIONT, 0 };
	layout[1] = { 0, (4 * sizeof(float)), D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 };
	layout[2] = D3DDECL_END();
	TheRenderManager->device->CreateVertexDeclaration(layout, &player->VertexShaderDeclaration);
	player->Pixel = static_cast<ShaderRecordPixel*>(ShaderRecord::LoadShader("Bink.pso", "Bink\\"));
	player->Bink = nullptr;
	player->Textures = {};
}

// One A8 texture per plane Bink asks NVR to allocate, for every frame buffer.
void BinkManager::AllocateFrameTextures() {
	IDirect3DDevice9* dev = TheRenderManager->device;
	BINKFRAMEBUFFERS& info = Textures.Buffers;

	for (int f = 0; f < info.TotalFrames; ++f) {
		BINKFRAMETEXTURES& slot = Textures.Textures[f];
		const BINKFRAMEPLANESET& planes = info.Frames[f];
		if (planes.YPlane.Allocate) MakePlaneTexture(dev, info.YABufferWidth, info.YABufferHeight, &slot.Y, &slot.Ysize);
		if (planes.cRPlane.Allocate) MakePlaneTexture(dev, info.cRcBBufferWidth, info.cRcBBufferHeight, &slot.cR, &slot.cRsize);
		if (planes.cBPlane.Allocate) MakePlaneTexture(dev, info.cRcBBufferWidth, info.cRcBBufferHeight, &slot.cB, &slot.cBsize);
		if (planes.APlane.Allocate) MakePlaneTexture(dev, info.YABufferWidth, info.YABufferHeight, &slot.A, &slot.Asize);
	}
}

void BinkManager::LockFrameTextures() {

	BINKFRAMETEXTURES* slot = Textures.Textures;
	BINKFRAMEPLANESET* plane = Textures.Buffers.Frames;
	D3DLOCKED_RECT rect;

	for (int f = 0; f < Textures.Buffers.TotalFrames; ++f, ++slot, ++plane) {
		// Only the frame Bink is decoding into is writable; the rest are read-only.
		const UInt32 lockFlags = (f == Textures.Buffers.FrameNum) ? 0 : D3DLOCK_READONLY;
		MapPlane(slot->Y, plane->YPlane, lockFlags, rect);
		MapPlane(slot->cR, plane->cRPlane, lockFlags, rect);
		MapPlane(slot->cB, plane->cBPlane, lockFlags, rect);
		if (slot->A) MapPlane(slot->A, plane->APlane, lockFlags, rect);
	}

}

void BinkManager::UnlockFrameTextures() {

	BINKFRAMETEXTURES* slot = Textures.Textures;
	BINKFRAMEPLANESET* plane = Textures.Buffers.Frames;

	for (int f = 0; f < Textures.Buffers.TotalFrames; ++f, ++slot, ++plane) {
		UnmapPlane(slot->Y, plane->YPlane);
		UnmapPlane(slot->cR, plane->cRPlane);
		UnmapPlane(slot->cB, plane->cBPlane);
		if (slot->A) UnmapPlane(slot->A, plane->APlane);
	}

}

void BinkManager::ReleaseFrameTextures() {

	BINKFRAMETEXTURES* sets[] = { &Textures.Textures[0], &Textures.Textures[1], &Textures.DrawingTextures };

	for (BINKFRAMETEXTURES* set : sets) {
		ReleaseTexture(set->Y);
		ReleaseTexture(set->cR);
		ReleaseTexture(set->cB);
		ReleaseTexture(set->A);
	}

}

// The current frame's Y, Cr and Cb planes to the screen through Bink.pso (one full-screen strip, no depth, no blending).
void BinkManager::DrawCurrentFrame() {
	IDirect3DDevice9* dev = TheRenderManager->device;
	NiDX9RenderState* states = TheRenderManager->renderState;
	const BINKFRAMETEXTURES& slot = Textures.Textures[Textures.Buffers.FrameNum];

	// Pixel shader constant: all ones.
	float shaderOne[4] = { 1.0f, 1.0f, 1.0f, 1.0f };

	IDirect3DTexture9* const planes[3] = { slot.Y, slot.cR, slot.cB };
	for (DWORD s = 0; s < 3; s++) dev->SetTexture(s, planes[s]);
	for (int s = 0; s < 3; s++) {
		TheRenderManager->SetSamplerState(s, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
		TheRenderManager->SetSamplerState(s, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
		TheRenderManager->SetSamplerState(s, D3DSAMP_ADDRESSW, D3DTADDRESS_CLAMP);
		TheRenderManager->SetSamplerState(s, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
		TheRenderManager->SetSamplerState(s, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
	}
	static const struct { D3DRENDERSTATETYPE state; DWORD value; } kMovieStates[] = {
		{ D3DRS_ZENABLE, D3DZB_FALSE }, { D3DRS_ZWRITEENABLE, D3DZB_FALSE }, { D3DRS_CULLMODE, D3DCULL_NONE }, { D3DRS_ALPHABLENDENABLE, 0 },
	};
	for (const auto& entry : kMovieStates) states->SetRenderState(entry.state, entry.value, RenderStateArgs);
	dev->SetPixelShader(Pixel->ShaderHandle);
	dev->SetVertexDeclaration(VertexShaderDeclaration);
	dev->SetPixelShaderConstantF(0, shaderOne, 1);

	// Triangle strip over the whole screen: top-left, top-right, bottom-left, bottom-right.
	const float screenW = TheRenderManager->width;
	const float screenH = TheRenderManager->height;
	POS_TC_VERTEX quad[4] = {
		Corner(0.0f,    0.0f,    0.0f, 0.0f),
		Corner(screenW, 0.0f,    1.0f, 0.0f),
		Corner(0.0f,    screenH, 0.0f, 1.0f),
		Corner(screenW, screenH, 1.0f, 1.0f),
	};
	dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(quad[0]));
}

// Plays a movie over whatever is on screen, one call per game frame: opens it on the first call, decodes a new frame when
// Bink says it is time, and draws until the last frame (the caller closes it).
void BinkManager::Render(const char* movieFile) {
	if (Bink == nullptr) {
		char fullPath[MAX_PATH] = {};
		GetCurrentDirectoryA(MAX_PATH, fullPath);
		strcat(fullPath, movieFile);
		Bink = BinkOpen(fullPath, BINKNOFRAMEBUFFERS);
		if (Bink == nullptr) return;
		BinkSetVolume(Bink, 0, 0);
		BinkGetFrameBuffersInfo(Bink, &Textures.Buffers);
		AllocateFrameTextures();
	}

	const bool decode = BinkWait(Bink) == 0;	// 0: the next frame is due
	if (decode) {
		LockFrameTextures();
		BinkRegisterFrameBuffers(Bink, &Textures.Buffers);
		BinkDoFrame(Bink);
		UnlockFrameTextures();
	}
	if (Bink->Frames != Bink->FrameNum) {
		// the draw uses texture stages 0-2: the game's textures there are put back after it
		IDirect3DDevice9* dev = TheRenderManager->device;
		IDirect3DBaseTexture9* kept[3] = {};
		for (DWORD s = 0; s < 3; s++) dev->GetTexture(s, &kept[s]);
		DrawCurrentFrame();
		for (DWORD s = 0; s < 3; s++) dev->SetTexture(s, kept[s]);
		for (IDirect3DBaseTexture9* held : kept) if (held) held->Release(); // UNOFFICIAL: GetTexture adds a reference
	}
	if (decode) BinkNextFrame(Bink);
}

void BinkManager::Close() {
	if (Bink == nullptr) return;
	BinkClose(Bink);
	ReleaseFrameTextures();
	Bink = nullptr;
}
