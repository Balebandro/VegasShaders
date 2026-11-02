#pragma once

class GrassShaders : public ShaderCollection
{
public:
	GrassShaders() : ShaderCollection("Grass") {};

	struct GrassStruct {
		D3DXVECTOR4		Scale;
	};
	GrassStruct	Constants;
	bool		GameShaderFix = true;	// UNOFFICIAL 2026-10-17 [Shaders.Grass.Main] GameShaderFix (see SetForDraw)
	bool		DrawFirst = false;		// UNOFFICIAL 2026-10-17 [Shaders.Grass.Main] DrawFirst: grass before the land (Render.cpp namespace GrassFirst)
	int			DrawFirstRestore = 5;	// GrassFirst diagnostic: what is put back after the early part (bit mask, Render.cpp Snapshot)
	bool		DrawFirstCull = true;	// GrassFirst: whole draws all near / all fading get one part only (Render.cpp Classify)
	static inline int	DrawPart = 0;			// GrassFirst: 0 every tuft, 1 only the near (unfaded) tufts, 2 only the fading ones (c19.w)
	static inline float	LastPairing[4] = {};	// GrassFirst: the last c19 written (put back with w = 0 after the late part)
	static inline bool	LastBindNVR = false;	// GrassFirst: the last grass bind used NVR's vertex shader (which honours DrawPart)
	static inline unsigned	BindSerial = 0;			// UNOFFICIAL 2026-10-28: grass binds so far (GrassFirst trusts LastBindNVR only for a bind of its own run)
	static inline const char*	LastBindName = nullptr;	// the vertex shader of that bind (GrassFirst's log)

	void	UpdateConstants();
	void	RegisterConstants();
	void	UpdateSettings();
	void	SetForDraw(NiD3DVertexShaderEx* VertexShader, NiD3DPixelShaderEx* PixelShader);
};