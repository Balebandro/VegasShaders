#pragma once

// Every IDirect3DDevice9Ex method that TESRDirect3DDevice9 simply hands on to the wrapped device is listed once here.
// Entry: M(return type, method, (parameters), (arguments)). The class declares them from this list (below), and
// Device.cpp defines them from the same list, so a declaration and its forwarding call cannot drift apart.
// MN is for the three entries Device.cpp defines with COM_DECLSPEC_NOTHROW; the header treats M and MN alike.
#define TESR_D3D9_UNPAREN(...) __VA_ARGS__
#define TESR_D3D9_DECLARE(Ret, Name, Params, Args) STDMETHOD_(Ret, Name)(THIS_ TESR_D3D9_UNPAREN Params);

#define TESR_D3D9_FORWARDS(M, MN) \
	/* IUnknown */ \
	M(HRESULT, QueryInterface, (REFIID riid, void** ppvObj), (riid, ppvObj)) \
	M(ULONG, AddRef, (), ()) \
	/* IDirect3DDevice9 */ \
	M(HRESULT, TestCooperativeLevel, (), ()) \
	M(UINT, GetAvailableTextureMem, (), ()) \
	M(HRESULT, EvictManagedResources, (), ()) \
	M(HRESULT, GetDeviceCaps, (D3DCAPS9* pCaps), (pCaps)) \
	M(HRESULT, GetDisplayMode, (UINT iSwapChain, D3DDISPLAYMODE* pMode), (iSwapChain, pMode)) \
	M(HRESULT, GetCreationParameters, (D3DDEVICE_CREATION_PARAMETERS* pParameters), (pParameters)) \
	M(HRESULT, SetCursorProperties, (UINT XHotSpot, UINT YHotSpot, IDirect3DSurface9* pCursorBitmap), (XHotSpot, YHotSpot, pCursorBitmap)) \
	M(void, SetCursorPosition, (int X, int Y, DWORD Flags), (X, Y, Flags)) \
	M(BOOL, ShowCursor, (BOOL bShow), (bShow)) \
	M(HRESULT, CreateAdditionalSwapChain, (D3DPRESENT_PARAMETERS* pPresentationParameters, IDirect3DSwapChain9** pSwapChain), (pPresentationParameters, pSwapChain)) \
	M(HRESULT, GetSwapChain, (UINT iSwapChain, IDirect3DSwapChain9** pSwapChain), (iSwapChain, pSwapChain)) \
	MN(UINT, GetNumberOfSwapChains, (), ()) \
	M(HRESULT, Reset, (D3DPRESENT_PARAMETERS* pPresentationParameters), (pPresentationParameters)) \
	M(HRESULT, Present, (CONST RECT* pSourceRect, CONST RECT* pDestRect, HWND hDestWindowOverride, CONST RGNDATA* pDirtyRegion), (pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion)) \
	M(HRESULT, GetBackBuffer, (UINT iSwapChain, UINT iBackBuffer, D3DBACKBUFFER_TYPE Type, IDirect3DSurface9** ppBackBuffer), (iSwapChain, iBackBuffer, Type, ppBackBuffer)) \
	M(HRESULT, GetRasterStatus, (UINT iSwapChain, D3DRASTER_STATUS* pRasterStatus), (iSwapChain, pRasterStatus)) \
	M(HRESULT, SetDialogBoxMode, (BOOL bEnableDialogs), (bEnableDialogs)) \
	M(void, SetGammaRamp, (UINT iSwapChain, DWORD Flags, CONST D3DGAMMARAMP* pRamp), (iSwapChain, Flags, pRamp)) \
	M(void, GetGammaRamp, (UINT iSwapChain, D3DGAMMARAMP* pRamp), (iSwapChain, pRamp)) \
	M(HRESULT, CreateTexture, (UINT Width, UINT Height, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DTexture9** ppTexture, HANDLE* pSharedHandle), (Width, Height, Levels, Usage, Format, Pool, ppTexture, pSharedHandle)) \
	M(HRESULT, CreateVolumeTexture, (UINT Width, UINT Height, UINT Depth, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DVolumeTexture9** ppVolumeTexture, HANDLE* pSharedHandle), (Width, Height, Depth, Levels, Usage, Format, Pool, ppVolumeTexture, pSharedHandle)) \
	M(HRESULT, CreateCubeTexture, (UINT EdgeLength, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DCubeTexture9** ppCubeTexture, HANDLE* pSharedHandle), (EdgeLength, Levels, Usage, Format, Pool, ppCubeTexture, pSharedHandle)) \
	M(HRESULT, CreateVertexBuffer, (UINT Length, DWORD Usage, DWORD FVF, D3DPOOL Pool, IDirect3DVertexBuffer9** ppVertexBuffer, HANDLE* pSharedHandle), (Length, Usage, FVF, Pool, ppVertexBuffer, pSharedHandle)) \
	M(HRESULT, CreateIndexBuffer, (UINT Length, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DIndexBuffer9** ppIndexBuffer, HANDLE* pSharedHandle), (Length, Usage, Format, Pool, ppIndexBuffer, pSharedHandle)) \
	M(HRESULT, CreateRenderTarget, (UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, DWORD MultisampleQuality, BOOL Lockable, IDirect3DSurface9** ppSurface, HANDLE* pSharedHandle), (Width, Height, Format, MultiSample, MultisampleQuality, Lockable, ppSurface, pSharedHandle)) \
	M(HRESULT, CreateDepthStencilSurface, (UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, DWORD MultisampleQuality, BOOL Discard, IDirect3DSurface9** ppSurface, HANDLE* pSharedHandle), (Width, Height, Format, MultiSample, MultisampleQuality, Discard, ppSurface, pSharedHandle)) \
	M(HRESULT, UpdateSurface, (IDirect3DSurface9* pSourceSurface, CONST RECT* pSourceRect, IDirect3DSurface9* pDestinationSurface, CONST POINT* pDestPoint), (pSourceSurface, pSourceRect, pDestinationSurface, pDestPoint)) \
	M(HRESULT, UpdateTexture, (IDirect3DBaseTexture9* pSourceTexture, IDirect3DBaseTexture9* pDestinationTexture), (pSourceTexture, pDestinationTexture)) \
	M(HRESULT, GetRenderTargetData, (IDirect3DSurface9* pRenderTarget, IDirect3DSurface9* pDestSurface), (pRenderTarget, pDestSurface)) \
	M(HRESULT, GetFrontBufferData, (UINT iSwapChain, IDirect3DSurface9* pDestSurface), (iSwapChain, pDestSurface)) \
	M(HRESULT, StretchRect, (IDirect3DSurface9* pSourceSurface, CONST RECT* pSourceRect, IDirect3DSurface9* pDestSurface, CONST RECT* pDestRect, D3DTEXTUREFILTERTYPE Filter), (pSourceSurface, pSourceRect, pDestSurface, pDestRect, Filter)) \
	M(HRESULT, ColorFill, (IDirect3DSurface9* pSurface, CONST RECT* pRect, D3DCOLOR color), (pSurface, pRect, color)) \
	M(HRESULT, CreateOffscreenPlainSurface, (UINT Width, UINT Height, D3DFORMAT Format, D3DPOOL Pool, IDirect3DSurface9** ppSurface, HANDLE* pSharedHandle), (Width, Height, Format, Pool, ppSurface, pSharedHandle)) \
	M(HRESULT, SetRenderTarget, (DWORD RenderTargetIndex, IDirect3DSurface9* pRenderTarget), (RenderTargetIndex, pRenderTarget)) \
	M(HRESULT, GetRenderTarget, (DWORD RenderTargetIndex, IDirect3DSurface9** ppRenderTarget), (RenderTargetIndex, ppRenderTarget)) \
	M(HRESULT, SetDepthStencilSurface, (IDirect3DSurface9* pNewZStencil), (pNewZStencil)) \
	M(HRESULT, GetDepthStencilSurface, (IDirect3DSurface9** ppZStencilSurface), (ppZStencilSurface)) \
	M(HRESULT, BeginScene, (), ()) \
	M(HRESULT, EndScene, (), ()) \
	M(HRESULT, Clear, (DWORD Count, CONST D3DRECT* pRects, DWORD Flags, D3DCOLOR Color, float Z, DWORD Stencil), (Count, pRects, Flags, Color, Z, Stencil)) \
	M(HRESULT, SetTransform, (D3DTRANSFORMSTATETYPE State, CONST D3DMATRIX* pMatrix), (State, pMatrix)) \
	M(HRESULT, GetTransform, (D3DTRANSFORMSTATETYPE State, D3DMATRIX* pMatrix), (State, pMatrix)) \
	M(HRESULT, MultiplyTransform, (D3DTRANSFORMSTATETYPE State, CONST D3DMATRIX* pMatrix), (State, pMatrix)) \
	M(HRESULT, SetViewport, (CONST D3DVIEWPORT9* pViewport), (pViewport)) \
	M(HRESULT, GetViewport, (D3DVIEWPORT9* pViewport), (pViewport)) \
	M(HRESULT, SetMaterial, (CONST D3DMATERIAL9* pMaterial), (pMaterial)) \
	M(HRESULT, GetMaterial, (D3DMATERIAL9* pMaterial), (pMaterial)) \
	M(HRESULT, SetLight, (DWORD Index, CONST D3DLIGHT9* pLight), (Index, pLight)) \
	M(HRESULT, GetLight, (DWORD Index, D3DLIGHT9* pLight), (Index, pLight)) \
	M(HRESULT, LightEnable, (DWORD Index, BOOL Enable), (Index, Enable)) \
	M(HRESULT, GetLightEnable, (DWORD Index, BOOL* pEnable), (Index, pEnable)) \
	M(HRESULT, SetClipPlane, (DWORD Index, CONST float* pPlane), (Index, pPlane)) \
	M(HRESULT, GetClipPlane, (DWORD Index, float* pPlane), (Index, pPlane)) \
	M(HRESULT, SetRenderState, (D3DRENDERSTATETYPE State, DWORD Value), (State, Value)) \
	M(HRESULT, GetRenderState, (D3DRENDERSTATETYPE State, DWORD* pValue), (State, pValue)) \
	M(HRESULT, CreateStateBlock, (D3DSTATEBLOCKTYPE Type, IDirect3DStateBlock9** ppSB), (Type, ppSB)) \
	M(HRESULT, BeginStateBlock, (), ()) \
	M(HRESULT, EndStateBlock, (IDirect3DStateBlock9** ppSB), (ppSB)) \
	M(HRESULT, SetClipStatus, (CONST D3DCLIPSTATUS9* pClipStatus), (pClipStatus)) \
	M(HRESULT, GetClipStatus, (D3DCLIPSTATUS9* pClipStatus), (pClipStatus)) \
	M(HRESULT, GetTexture, (DWORD Stage, IDirect3DBaseTexture9** ppTexture), (Stage, ppTexture)) \
	M(HRESULT, SetTexture, (DWORD Stage, IDirect3DBaseTexture9* pTexture), (Stage, pTexture)) \
	M(HRESULT, GetTextureStageState, (DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD* pValue), (Stage, Type, pValue)) \
	M(HRESULT, SetTextureStageState, (DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD Value), (Stage, Type, Value)) \
	M(HRESULT, GetSamplerState, (DWORD Sampler, D3DSAMPLERSTATETYPE Type, DWORD* pValue), (Sampler, Type, pValue)) \
	M(HRESULT, SetSamplerState, (DWORD Sampler, D3DSAMPLERSTATETYPE Type, DWORD Value), (Sampler, Type, Value)) \
	M(HRESULT, ValidateDevice, (DWORD* pNumPasses), (pNumPasses)) \
	M(HRESULT, SetPaletteEntries, (UINT PaletteNumber, CONST PALETTEENTRY* pEntries), (PaletteNumber, pEntries)) \
	M(HRESULT, GetPaletteEntries, (UINT PaletteNumber, PALETTEENTRY* pEntries), (PaletteNumber, pEntries)) \
	M(HRESULT, SetCurrentTexturePalette, (UINT PaletteNumber), (PaletteNumber)) \
	M(HRESULT, GetCurrentTexturePalette, (UINT* PaletteNumber), (PaletteNumber)) \
	M(HRESULT, SetScissorRect, (CONST RECT* pRect), (pRect)) \
	M(HRESULT, GetScissorRect, (RECT* pRect), (pRect)) \
	M(HRESULT, SetSoftwareVertexProcessing, (BOOL bSoftware), (bSoftware)) \
	MN(BOOL, GetSoftwareVertexProcessing, (), ()) \
	M(HRESULT, SetNPatchMode, (float nSegments), (nSegments)) \
	MN(float, GetNPatchMode, (), ()) \
	M(HRESULT, DrawPrimitive, (D3DPRIMITIVETYPE PrimitiveType, UINT StartVertex, UINT PrimitiveCount), (PrimitiveType, StartVertex, PrimitiveCount)) \
	M(HRESULT, DrawIndexedPrimitive, (D3DPRIMITIVETYPE PrimitiveType, INT BaseVertexIndex, UINT MinVertexIndex, UINT NumVertices, UINT startIndex, UINT primCount), (PrimitiveType, BaseVertexIndex, MinVertexIndex, NumVertices, startIndex, primCount)) \
	M(HRESULT, DrawPrimitiveUP, (D3DPRIMITIVETYPE PrimitiveType, UINT PrimitiveCount, CONST void* pVertexStreamZeroData, UINT VertexStreamZeroStride), (PrimitiveType, PrimitiveCount, pVertexStreamZeroData, VertexStreamZeroStride)) \
	M(HRESULT, DrawIndexedPrimitiveUP, (D3DPRIMITIVETYPE PrimitiveType, UINT MinVertexIndex, UINT NumVertices, UINT PrimitiveCount, CONST void* pIndexData, D3DFORMAT IndexDataFormat, CONST void* pVertexStreamZeroData, UINT VertexStreamZeroStride), (PrimitiveType, MinVertexIndex, NumVertices, PrimitiveCount, pIndexData, IndexDataFormat, pVertexStreamZeroData, VertexStreamZeroStride)) \
	M(HRESULT, ProcessVertices, (UINT SrcStartIndex, UINT DestIndex, UINT VertexCount, IDirect3DVertexBuffer9* pDestBuffer, IDirect3DVertexDeclaration9* pVertexDecl, DWORD Flags), (SrcStartIndex, DestIndex, VertexCount, pDestBuffer, pVertexDecl, Flags)) \
	M(HRESULT, CreateVertexDeclaration, (CONST D3DVERTEXELEMENT9* pVertexElements, IDirect3DVertexDeclaration9** ppDecl), (pVertexElements, ppDecl)) \
	M(HRESULT, SetVertexDeclaration, (IDirect3DVertexDeclaration9* pDecl), (pDecl)) \
	M(HRESULT, GetVertexDeclaration, (IDirect3DVertexDeclaration9** ppDecl), (ppDecl)) \
	M(HRESULT, SetFVF, (DWORD FVF), (FVF)) \
	M(HRESULT, GetFVF, (DWORD* pFVF), (pFVF)) \
	M(HRESULT, CreateVertexShader, (CONST DWORD* pFunction, IDirect3DVertexShader9** ppShader), (pFunction, ppShader)) \
	M(HRESULT, SetVertexShader, (IDirect3DVertexShader9* pShader), (pShader)) \
	M(HRESULT, GetVertexShader, (IDirect3DVertexShader9** ppShader), (ppShader)) \
	M(HRESULT, SetVertexShaderConstantF, (UINT StartRegister, CONST float* pConstantData, UINT Vector4fCount), (StartRegister, pConstantData, Vector4fCount)) \
	M(HRESULT, GetVertexShaderConstantF, (UINT StartRegister, float* pConstantData, UINT Vector4fCount), (StartRegister, pConstantData, Vector4fCount)) \
	M(HRESULT, SetVertexShaderConstantI, (UINT StartRegister, CONST int* pConstantData, UINT Vector4iCount), (StartRegister, pConstantData, Vector4iCount)) \
	M(HRESULT, GetVertexShaderConstantI, (UINT StartRegister, int* pConstantData, UINT Vector4iCount), (StartRegister, pConstantData, Vector4iCount)) \
	M(HRESULT, SetVertexShaderConstantB, (UINT StartRegister, CONST BOOL* pConstantData, UINT BoolCount), (StartRegister, pConstantData, BoolCount)) \
	M(HRESULT, GetVertexShaderConstantB, (UINT StartRegister, BOOL* pConstantData, UINT BoolCount), (StartRegister, pConstantData, BoolCount)) \
	M(HRESULT, SetStreamSource, (UINT StreamNumber, IDirect3DVertexBuffer9* pStreamData, UINT OffsetInBytes, UINT Stride), (StreamNumber, pStreamData, OffsetInBytes, Stride)) \
	M(HRESULT, GetStreamSource, (UINT StreamNumber, IDirect3DVertexBuffer9** ppStreamData, UINT* pOffsetInBytes, UINT* pStride), (StreamNumber, ppStreamData, pOffsetInBytes, pStride)) \
	M(HRESULT, SetStreamSourceFreq, (UINT StreamNumber, UINT Setting), (StreamNumber, Setting)) \
	M(HRESULT, GetStreamSourceFreq, (UINT StreamNumber, UINT* pSetting), (StreamNumber, pSetting)) \
	M(HRESULT, SetIndices, (IDirect3DIndexBuffer9* pIndexData), (pIndexData)) \
	M(HRESULT, GetIndices, (IDirect3DIndexBuffer9** ppIndexData), (ppIndexData)) \
	M(HRESULT, CreatePixelShader, (CONST DWORD* pFunction, IDirect3DPixelShader9** ppShader), (pFunction, ppShader)) \
	M(HRESULT, SetPixelShader, (IDirect3DPixelShader9* pShader), (pShader)) \
	M(HRESULT, GetPixelShader, (IDirect3DPixelShader9** ppShader), (ppShader)) \
	M(HRESULT, SetPixelShaderConstantF, (UINT StartRegister, CONST float* pConstantData, UINT Vector4fCount), (StartRegister, pConstantData, Vector4fCount)) \
	M(HRESULT, GetPixelShaderConstantF, (UINT StartRegister, float* pConstantData, UINT Vector4fCount), (StartRegister, pConstantData, Vector4fCount)) \
	M(HRESULT, SetPixelShaderConstantI, (UINT StartRegister, CONST int* pConstantData, UINT Vector4iCount), (StartRegister, pConstantData, Vector4iCount)) \
	M(HRESULT, GetPixelShaderConstantI, (UINT StartRegister, int* pConstantData, UINT Vector4iCount), (StartRegister, pConstantData, Vector4iCount)) \
	M(HRESULT, SetPixelShaderConstantB, (UINT StartRegister, CONST BOOL* pConstantData, UINT BoolCount), (StartRegister, pConstantData, BoolCount)) \
	M(HRESULT, GetPixelShaderConstantB, (UINT StartRegister, BOOL* pConstantData, UINT BoolCount), (StartRegister, pConstantData, BoolCount)) \
	M(HRESULT, DrawRectPatch, (UINT Handle, CONST float* pNumSegs, CONST D3DRECTPATCH_INFO* pRectPatchInfo), (Handle, pNumSegs, pRectPatchInfo)) \
	M(HRESULT, DrawTriPatch, (UINT Handle, CONST float* pNumSegs, CONST D3DTRIPATCH_INFO* pTriPatchInfo), (Handle, pNumSegs, pTriPatchInfo)) \
	M(HRESULT, DeletePatch, (UINT Handle), (Handle)) \
	M(HRESULT, CreateQuery, (D3DQUERYTYPE Type, IDirect3DQuery9** ppQuery), (Type, ppQuery)) \
	/* IDirect3DDevice9Ex */ \
	M(HRESULT, SetConvolutionMonoKernel, (UINT width, UINT height, float* rows, float* columns), (width, height, rows, columns)) \
	M(HRESULT, ComposeRects, (IDirect3DSurface9* pSrc, IDirect3DSurface9* pDst, IDirect3DVertexBuffer9* pSrcRectDescs, UINT NumRects, IDirect3DVertexBuffer9* pDstRectDescs, D3DCOMPOSERECTSOP Operation, int Xoffset, int Yoffset), (pSrc, pDst, pSrcRectDescs, NumRects, pDstRectDescs, Operation, Xoffset, Yoffset)) \
	M(HRESULT, PresentEx, (CONST RECT* pSourceRect, CONST RECT* pDestRect, HWND hDestWindowOverride, CONST RGNDATA* pDirtyRegion, DWORD dwFlags), (pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion, dwFlags)) \
	M(HRESULT, GetGPUThreadPriority, (INT* pPriority), (pPriority)) \
	M(HRESULT, SetGPUThreadPriority, (INT Priority), (Priority)) \
	M(HRESULT, WaitForVBlank, (UINT iSwapChain), (iSwapChain)) \
	M(HRESULT, CheckResourceResidency, (IDirect3DResource9** pResourceArray, UINT32 NumResources), (pResourceArray, NumResources)) \
	M(HRESULT, SetMaximumFrameLatency, (UINT MaxLatency), (MaxLatency)) \
	M(HRESULT, GetMaximumFrameLatency, (UINT* pMaxLatency), (pMaxLatency)) \
	M(HRESULT, CheckDeviceState, (HWND hDestinationWindow), (hDestinationWindow)) \
	M(HRESULT, CreateRenderTargetEx, (UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, DWORD MultisampleQuality, BOOL Lockable, IDirect3DSurface9** ppSurface, HANDLE* pSharedHandle, DWORD Usage), (Width, Height, Format, MultiSample, MultisampleQuality, Lockable, ppSurface, pSharedHandle, Usage)) \
	M(HRESULT, CreateOffscreenPlainSurfaceEx, (UINT Width, UINT Height, D3DFORMAT Format, D3DPOOL Pool, IDirect3DSurface9** ppSurface, HANDLE* pSharedHandle, DWORD Usage), (Width, Height, Format, Pool, ppSurface, pSharedHandle, Usage)) \
	M(HRESULT, CreateDepthStencilSurfaceEx, (UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, DWORD MultisampleQuality, BOOL Discard, IDirect3DSurface9** ppSurface, HANDLE* pSharedHandle, DWORD Usage), (Width, Height, Format, MultiSample, MultisampleQuality, Discard, ppSurface, pSharedHandle, Usage)) \
	M(HRESULT, ResetEx, (D3DPRESENT_PARAMETERS* pPresentationParameters, D3DDISPLAYMODEEX* pFullscreenDisplayMode), (pPresentationParameters, pFullscreenDisplayMode)) \
	M(HRESULT, GetDisplayModeEx, (UINT iSwapChain, D3DDISPLAYMODEEX* pMode, D3DDISPLAYROTATION* pRotation), (iSwapChain, pMode, pRotation))

class TESRDirect3DDevice9 : public IDirect3DDevice9Ex {
public:
	TESRDirect3DDevice9(IDirect3D9Ex* D3DInterface, IDirect3DDevice9Ex* D3DDevice);
	~TESRDirect3DDevice9();

	// Not forwarded as-is: Device.cpp gives these their own bodies.
	STDMETHOD_(ULONG, Release)(THIS);
	STDMETHOD(GetDirect3D)(THIS_ IDirect3D9** ppD3D9);

	TESR_D3D9_FORWARDS(TESR_D3D9_DECLARE, TESR_D3D9_DECLARE)

private:
	IDirect3D9Ex* D3DInterface;
	IDirect3DDevice9Ex* D3DDevice;
};
