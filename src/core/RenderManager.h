#pragma once

// The game's renderer object, extended (the game allocates it at NVR's size: see AttachHooks). Holds the per-frame camera
// matrices and vectors the effects read, and the depth-resolve path chosen at startup.
class RenderManager: public RenderManagerBase {
public:
	void				Initialize();
	void				ResolveDepthBuffer(IDirect3DTexture9* depthTarget);
	void				CreateD3DMatrix(D3DMATRIX* out, NiTransform* xf);	// an object's transform as a camera-relative world matrix
	void				GetScreenSpaceBoundSize(NiPoint2* sizeOut, NiBound* sphere, float tolerance = 1e-5f);
	void				UpdateSceneCameraData();
	void				SetupSceneCamera();
	void				CheckAndTakeScreenShot(IDirect3DSurface9* source, bool asHdr);
	float				GetObjectDistance(NiBound* sphere);
	bool				IsReversedDepth();
	const char*			D3D9RuntimeDescription(); // e.g. "DXVK (<path to d3d9.dll>)"; for the log
	const D3DCAPS9*		DeviceCaps(); // UNOFFICIAL: the device caps, fetched once (null if the device will not say); two effects used to ask every frame

	// this frame's camera (SetupSceneCamera)
	D3DXMATRIX			WorldViewProjMatrix, ViewProjMatrix, InvViewProjMatrix, InvProjMatrix, InvViewMatrix, ViewMatrix;
	D3DXVECTOR4			CameraData, FOVData, CameraForward, CameraPosition;
	IDirect3DSurface9*	BackBuffer;
	D3DXVECTOR4			DepthConstants;
	RECT				SaveGameScreenShotRECT;
	bool				IsSaveGameScreenShot;
	bool				RESZ, DXVK, ILS;	// depth resolve by the RESZ trick / d3d9.dll is DXVK / (unused)
};

// The node the detector window (debug scene view) lists the passes under.
class DWNode : public NiNode {
public:
	static void		Create();
	static DWNode*	Get();
	static void		AddNode(const char* label, NiAVObject* first, NiAVObject* second);
};
