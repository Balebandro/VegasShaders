#pragma once

// Plays Bink movies (NVR's intro and main menu videos) through D3D9: Bink decodes into textures NVR owns, one A8 texture
// per plane, and a pixel shader turns Y/Cr/Cb into colour. Made once at startup, never freed.
class BinkManager {
public:
	// the textures behind one Bink frame buffer, with each plane's byte count
	struct BINKFRAMETEXTURES {
		UInt32				Ysize, cRsize, cBsize, Asize;
		IDirect3DTexture9	*Y, *cR, *cB, *A;
	};

	struct BINKTEXTURESET {
		BINKFRAMEBUFFERS	Buffers;							// what Bink reports and decodes into
		BINKFRAMETEXTURES	Textures[BINKMAXFRAMEBUFFERS];		// NVR's texture per frame buffer
		BINKFRAMETEXTURES	DrawingTextures;
	};

	// a pre-transformed vertex with one texture coordinate
	struct POS_TC_VERTEX { float sx, sy, sz, rhw, tu, tv; };

	static void		Initialize();

private:
	void			AllocateFrameTextures();
	void			LockFrameTextures();
	void			UnlockFrameTextures();
	void			ReleaseFrameTextures();
	void			DrawCurrentFrame();

public:
	void			Render(const char* movieFile);	// once per frame while the movie plays
	void			Close();

	D3DVERTEXELEMENT9				VertexDefinition[3];
	IDirect3DVertexDeclaration9*	VertexShaderDeclaration;
	ShaderRecordPixel*				Pixel;			// Bink.pso
	HBINK							Bink;			// the open movie, or null
	BINKTEXTURESET					Textures;
};
