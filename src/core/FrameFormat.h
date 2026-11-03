#pragma once

// UNOFFICIAL, TEST ONLY (2026-10-17): the frame capture file (.nvrf) shared by the recorder inside NVR (FrameRecorder.h) and the
// standalone replayer (tests/frame_replay.cpp). Plain data, no NVR types: only <d3d9.h>.
//
// File = header, then records in call order until kRecEnd. Little endian.
//   header: "NVRF" (DWORD 0x4652564E), version, sizeof(FrameState) / 4, back buffer width, height, format
//   record: DWORD type, DWORD payload bytes, payload
// Resources are keyed by ids (1, 2, ...; 0 = none). A resource's description record comes before the first record that uses it.
// Contents (kRecData) are what the resource held at its first use in the frame; the replayer fills them before the frame starts.
// kRecState is the device state before the next action, as runs of 32-bit words of FrameState that changed since the last one.
// kRecUpdate is a write the game made to a resource during the frame (Lock/Unlock), replayed at its place.
#include <d3d9.h>

namespace FrameFormat {

	enum : unsigned { kMagic = 0x4652564E, kVersion = 1 };

	enum Record : unsigned {
		kRecTexture = 1,	// FrameTexDesc
		kRecCube = 2,		// FrameTexDesc (width = edge)
		kRecVolume = 3,		// FrameTexDesc
		kRecVertexBuffer = 4,	// FrameBufferDesc
		kRecIndexBuffer = 5,	// FrameBufferDesc
		kRecSurface = 6,	// FrameSurfaceDesc: a surface of its own (render target, depth stencil, plain, back buffer)
		kRecSubSurface = 7,	// FrameSubSurface: a surface of a texture / cube texture
		kRecShader = 8,		// DWORD id, DWORD pixel (0/1), bytecode
		kRecDecl = 9,		// DWORD id, D3DVERTEXELEMENT9[] (with the END element)
		kRecData = 10,		// FrameDataHeader + tightly packed rows (textures: per level/face; buffers: whole)
		kRecState = 11,		// DWORD runs, then per run: UInt16 word offset, UInt16 word count, words
		kRecAction = 12,	// FrameAction + optional inline data
		kRecMark = 13,		// DWORD kind (FrameMarkKind), then a zero-terminated name
		kRecName = 14,		// DWORD shader id, zero-terminated name
		kRecRefImage = 15,	// DWORD width, height, then BGRA8 rows: the back buffer at the recorded Present
		kRecInfo = 16,		// zero-terminated text: the recorder's summary
		kRecUpdate = 17,	// FrameUpdate + bytes
		kRecEnd = 18,
	};

	enum FrameMarkKind : unsigned { kMarkBegin = 0, kMarkEnd = 1, kMarkSplit = 2 };

	enum FrameSurfaceKind : unsigned { kSurfRenderTarget = 0, kSurfDepth = 1, kSurfPlain = 2, kSurfBackBuffer = 3 };

	enum FrameActionType : unsigned {
		kActDraw = 1,				// a0 type, a1 start vertex, a2 prim count
		kActDrawIndexed = 2,		// a0 type, a1 base vertex, a2 min index, a3 num vertices, a4 start index, a5 prim count
		kActDrawUP = 3,				// a0 type, a1 prim count, a2 stride, data: vertices
		kActDrawIndexedUP = 4,		// a0 type, a1 min index, a2 num vertices, a3 prim count, a4 index format, a5 stride, a6 index bytes, data: indices then vertices
		kActClear = 5,				// a0 rect count, a1 flags, a2 colour, a3 z (float bits), a4 stencil, data: rects
		kActStretchRect = 6,		// a0 src, a1 dst, a2 filter, a3 has src rect, a4 has dst rect, data: src rect, dst rect
		kActColorFill = 7,			// a0 surface, a1 colour, a2 has rect, data: rect
		kActUpdateSurface = 8,		// a0 src, a1 dst, a2 has src rect, a3 has dst point, data: rect, point
		kActUpdateTexture = 9,		// a0 src, a1 dst
		kActNvStretchRect = 10,		// a0 src (texture or surface id), a1 dst (texture or surface id): NvAPI_D3D9_StretchRectEx
		kActBeginScene = 11,
		kActEndScene = 12,
		kActPresent = 13,
	};

	struct FrameTexDesc { DWORD id, width, height, depth, levels, usage, format, pool; };
	struct FrameBufferDesc { DWORD id, length, usage, format, pool; };	// format: FVF for vertex buffers, D3DFORMAT for index buffers
	struct FrameSurfaceDesc { DWORD id, kind, width, height, format, multisample, quality, usage, pool; };
	struct FrameSubSurface { DWORD id, parent, level, face; };
	struct FrameDataHeader { DWORD id, level, face, flags, width, height, depth, rowBytes, rows; };	// rows per slice; flags: kDataResolved...
	enum : DWORD { kDataReadBack = 1, kDataResolved = 2, kDataDepth = 4 };	// kDataDepth: a depth texture copied as R32F (rows are floats)
	struct FrameAction { DWORD type, a[7], dataBytes; };
	struct FrameUpdate { DWORD id, level, face, offset, left, top, right, bottom, rowBytes, rows, flags; };	// buffers: offset + bytes; textures: rect

	// The device state an action runs with. Words only (DWORD / float / int), so it is diffed and stored as words.
	enum : unsigned { kRenderStates = 210, kSamplers = 20, kSamplerStates = 14, kStages = 8, kStageStates = 33,
		kVSFloat = 256, kPSFloat = 224, kTransforms = 11 };
	struct FrameState {
		DWORD rs[kRenderStates];				// by D3DRENDERSTATETYPE
		DWORD ss[kSamplers][kSamplerStates];	// samplers 0-15, then D3DVERTEXTEXTURESAMPLER0-3; by D3DSAMPLERSTATETYPE
		DWORD tex[kSamplers];					// texture ids on those samplers
		DWORD tss[kStages][kStageStates];		// by D3DTEXTURESTAGESTATETYPE
		DWORD vs, ps, decl, fvf, ib;
		DWORD stream[16][4];					// id, offset, stride, frequency divider
		DWORD rt[4], ds;
		D3DVIEWPORT9 viewport;
		RECT scissor;
		float clip[6][4];
		float vsf[kVSFloat][4]; int vsi[16][4]; BOOL vsb[16];
		float psf[kPSFloat][4]; int psi[16][4]; BOOL psb[16];
		D3DMATRIX xf[kTransforms];				// WORLD, VIEW, PROJECTION, TEXTURE0-7 (read only for draws without a vertex shader)
	};
	static_assert(sizeof(FrameState) % 4 == 0, "FrameState must be words");
	enum : unsigned { kStateWords = sizeof(FrameState) / 4 };
	static_assert(kStateWords < 65536, "word offsets are 16-bit");

	inline const D3DTRANSFORMSTATETYPE TransformOf(unsigned i) {
		return i == 0 ? D3DTS_WORLD : i == 1 ? D3DTS_VIEW : i == 2 ? D3DTS_PROJECTION : (D3DTRANSFORMSTATETYPE)(D3DTS_TEXTURE0 + (i - 3));
	}
	inline DWORD SamplerOf(unsigned i) { return i < 16 ? i : D3DVERTEXTEXTURESAMPLER0 + (i - 16); }

	// The render states D3D9 defines (d3d9types.h).
	static const unsigned char RenderStates[] = { 7, 8, 9, 14, 15, 16, 19, 20, 22, 23, 24, 25, 26, 27, 28, 29, 34, 35, 36, 37, 38, 48, 52, 53, 54, 55,
		56, 57, 58, 59, 60, 128, 129, 130, 131, 132, 133, 134, 135, 136, 137, 139, 140, 141, 142, 143, 145, 146, 147, 148, 151, 152, 154, 155, 156,
		157, 158, 159, 160, 161, 162, 163, 165, 166, 167, 168, 170, 171, 172, 173, 174, 175, 176, 178, 179, 180, 181, 182, 183, 184, 185, 186, 187,
		188, 189, 190, 191, 192, 193, 194, 195, 198, 199, 200, 201, 202, 203, 204, 205, 206, 207, 208, 209 };
	static const unsigned char StageStates[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 22, 23, 24, 26, 27, 28, 32 };
	// Sampler states 1-13 (ADDRESSU .. DMAPOFFSET).

	// Bytes of one row of blocks / pixels and the number of such rows for a level (0 = format not handled).
	inline bool IsBlock(D3DFORMAT f, unsigned& blockBytes) {
		switch ((unsigned)f) {
		case D3DFMT_DXT1: case MAKEFOURCC('A', 'T', 'I', '1'): blockBytes = 8; return true;
		case D3DFMT_DXT2: case D3DFMT_DXT3: case D3DFMT_DXT4: case D3DFMT_DXT5: case MAKEFOURCC('A', 'T', 'I', '2'): blockBytes = 16; return true;
		}
		return false;
	}
	inline unsigned PixelBytes(D3DFORMAT f) {
		switch ((unsigned)f) {
		case D3DFMT_A8R8G8B8: case D3DFMT_X8R8G8B8: case D3DFMT_A8B8G8R8: case D3DFMT_X8B8G8R8: case D3DFMT_A2R10G10B10: case D3DFMT_A2B10G10R10:
		case D3DFMT_G16R16: case D3DFMT_G16R16F: case D3DFMT_R32F: case D3DFMT_Q8W8V8U8: case D3DFMT_V16U16: case D3DFMT_X8L8V8U8: case D3DFMT_A2W10V10U10:
		case D3DFMT_D24S8: case D3DFMT_D24X8: case D3DFMT_D32: case D3DFMT_D32F_LOCKABLE: case MAKEFOURCC('I', 'N', 'T', 'Z'): case MAKEFOURCC('D', 'F', '2', '4'):
			return 4;
		case D3DFMT_R5G6B5: case D3DFMT_X1R5G5B5: case D3DFMT_A1R5G5B5: case D3DFMT_A4R4G4B4: case D3DFMT_A8L8: case D3DFMT_L16: case D3DFMT_R16F: case D3DFMT_V8U8:
		case D3DFMT_D16: case D3DFMT_D16_LOCKABLE: case D3DFMT_L6V5U5: case MAKEFOURCC('D', 'F', '1', '6'): case D3DFMT_X4R4G4B4: case D3DFMT_A8R3G3B2:
			return 2;
		case D3DFMT_L8: case D3DFMT_A8: case D3DFMT_P8: case D3DFMT_A4L4: case D3DFMT_R3G3B2:
			return 1;
		case D3DFMT_A16B16G16R16: case D3DFMT_A16B16G16R16F: case D3DFMT_G32R32F: case D3DFMT_Q16W16V16U16:
			return 8;
		case D3DFMT_A32B32G32R32F:
			return 16;
		case D3DFMT_R8G8B8:
			return 3;
		}
		return 0;
	}
	inline bool Layout(D3DFORMAT f, DWORD width, DWORD height, DWORD& rowBytes, DWORD& rows) {
		unsigned block = 0;
		if (IsBlock(f, block)) { rowBytes = ((width + 3) / 4) * block; rows = (height + 3) / 4; return true; }
		const unsigned p = PixelBytes(f);
		if (!p) { rowBytes = rows = 0; return false; }
		rowBytes = width * p; rows = height; return true;
	}
	inline bool IsDepthFormat(D3DFORMAT f) {
		switch ((unsigned)f) {
		case D3DFMT_D16: case D3DFMT_D16_LOCKABLE: case D3DFMT_D24S8: case D3DFMT_D24X8: case D3DFMT_D24X4S4: case D3DFMT_D24FS8: case D3DFMT_D32:
		case D3DFMT_D32F_LOCKABLE: case D3DFMT_D15S1: case MAKEFOURCC('I', 'N', 'T', 'Z'): case MAKEFOURCC('D', 'F', '2', '4'): case MAKEFOURCC('D', 'F', '1', '6'):
		case MAKEFOURCC('R', 'A', 'W', 'Z'):
			return true;
		}
		return false;
	}

	inline unsigned long long Hash(const void* data, size_t bytes) {	// FNV-1a 64
		unsigned long long h = 1469598103934665603ull;
		const unsigned char* p = (const unsigned char*)data;
		for (size_t i = 0; i < bytes; i++) { h ^= p[i]; h *= 1099511628211ull; }
		return h;
	}
}
