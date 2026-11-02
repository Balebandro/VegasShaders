#pragma once

// The camera helpers: first/third person state, the scripted camera commands, and frustum planes built from matrices
// (for culling). Made once at startup, never freed.
class CameraManager {
public:
	static void Initialize();

	// What the camera script commands asked for; applied to the camera node every frame (Execute).
	class CameraManagerCommands {
	public:
		CameraManagerCommands();	// all zero: no command active

		bool			IsTranslating, IsRotating, IsLookingAt;							// relative to Ref
		bool			IsTranslatingToPosition, IsRotatingToPosition, IsLookingAtPosition;	// absolute
		NiPoint3		Position, Rotation, LookingAtOffset, LookingAtPosition;
		TESObjectREFR*	Ref;

		void			Execute(NiAVObject* cameraNode);
	};

	bool					IsFirstPerson();
	bool					IsVanity();
	void					SetFirstPerson(bool firstPerson);
	void					SetSceneGraph();
	void					ResetCamera();
	void					ExecuteCommands(NiAVObject* cameraNode);
	void					GetPlane(D3DXPLANE* out, float a, float b, float c, float d);	// normalized plane
	void					SetFrustum(frustum* planesOut, D3DMATRIX* viewProj);	// the six planes of a view-projection matrix
	void					SetFrustumPlanes(NiFrustumPlanes* planesOut, D3DMATRIX* viewProj, D3DXVECTOR3 eyeOffset, NiFrustum lens);
	bool					InFrustum(frustum* planes, NiNode* node, bool skipNear = false);

	Actor*					DialogTarget;
	NiPoint3				From, FromOffset;
	CameraManagerCommands*	CameraCommands;
	bool					TogglePOV;

private:
	bool					FirstPersonView;
};
