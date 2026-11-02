namespace {
	// Byte copy of a value over another (the camera node's transforms are written this way, not through operator=).
	template <class V> inline void CopyBytes(V* into, const V& from) {
		memcpy(into, &from, sizeof(V));
	}

	// The camera node keeps the same placement in its world and its local transform.
	void PlaceCamera(NiAVObject* camera, const NiPoint3& at) {
		CopyBytes(&camera->m_worldTransform.pos, at);
		CopyBytes(&camera->m_localTransform.pos, at);
	}

	void OrientCamera(NiAVObject* camera, const NiMatrix33& axes) {
		CopyBytes(&camera->m_worldTransform.rot, axes);
		CopyBytes(&camera->m_localTransform.rot, axes);
	}

	// Rotation matrix from Euler angles given in degrees.
	NiMatrix33 FromDegrees(NiPoint3* angles) {
		NiMatrix33 result;
		result.GenerateRotationMatrixZXY(angles, 1);
		return result;
	}

	// A point given relative to a node (rotated by its axes, then moved to its origin).
	NiPoint3 RelativeTo(const NiMatrix33* axes, const NiPoint3* origin, const NiPoint3& local) {
		NiPoint3 world = *axes * local;
		world.x += origin->x;
		world.y += origin->y;
		world.z += origin->z;
		return world;
	}
}

void CameraManager::Initialize() {
	Logger::Log("Starting the camera manager...");
	CameraManager* cam = new CameraManager();
	TheCameraManager = cam;

	const NiPoint3 origin(0.0f, 0.0f, 0.0f);
	cam->DialogTarget = nullptr;
	cam->From = origin;
	cam->FromOffset = origin;
	cam->CameraCommands = new CameraManagerCommands();
	cam->FirstPersonView = false;
	cam->TogglePOV = false;
}

// With the camera mode on NVR tracks the view itself; otherwise the player's own state answers.
bool CameraManager::IsFirstPerson() {
	if (!TheSettingManager->SettingsMain.CameraMode.Enabled) return Player->IsFirstPerson();
	return FirstPersonView;
}

bool CameraManager::IsVanity() { return Player->IsVanity(); }

void CameraManager::SetFirstPerson(bool firstPerson) { FirstPersonView = firstPerson; }

void CameraManager::ResetCamera() { Player->ResetCamera(); }

// Per frame: the camera mode's near plane and field of view, then the field of view the shaders get.
void CameraManager::SetSceneGraph() {
	const SettingsMainStruct::CameraModeStruct& cameraMode = TheSettingManager->SettingsMain.CameraMode;
	const float wantedFov = cameraMode.FoV;

	if (cameraMode.Enabled) {
		SceneGraph* scene = WorldSceneGraph;
		scene->SetNearDistance(2.5f);
		// the field of view is left alone while aiming or with any menu open
		if (wantedFov != scene->cameraFOV && !Player->IsAiming() && InterfaceManager->IsActive(Menu::MenuType::kMenuType_None)) {
			scene->UpdateParticleShaderFoV(wantedFov);
			Player->SetFoV(wantedFov);
		}
	}
	BodycamEffect::ApplyWideView();	// UNOFFICIAL: Bodycam's WideView (Bodycam.cpp)
	const float shaderFov = Player->GetFoV(false);
	TheShaderManager->ShaderConst.ReciprocalResolution.w = shaderFov;
}

void CameraManager::ExecuteCommands(NiAVObject* cameraNode) {
	CameraCommands->Execute(cameraNode);
}

CameraManager::CameraManagerCommands::CameraManagerCommands() {
	memset(this, 0, sizeof(*this));
}

// Applies the scripted camera commands (translate / rotate / look at, relative to a reference or absolute) to the camera node.
void CameraManager::CameraManagerCommands::Execute(NiAVObject* cameraNode) {
	if (!(IsTranslating || IsRotating || IsLookingAt || IsTranslatingToPosition || IsRotatingToPosition || IsLookingAtPosition)) return;

	// the reference's frame: an actor's head for translating and looking at, its root node otherwise
	const NiMatrix33* refAxes = nullptr;
	const NiPoint3* refOrigin = nullptr;
	if (Ref != nullptr) {
		NiNode* root = Ref->GetNiNode();
		const bool useHead = Ref->IsActor() && (IsTranslating || IsLookingAt);
		NiAVObject* anchor = useHead ? root->GetObjectByName("Bip01 Head") : root;
		refAxes = &anchor->m_worldTransform.rot;
		refOrigin = &anchor->m_worldTransform.pos;
	}

	if (IsTranslating) PlaceCamera(cameraNode, RelativeTo(refAxes, refOrigin, Position));
	if (IsRotating) OrientCamera(cameraNode, *refAxes * FromDegrees(&Rotation));
	if (IsTranslatingToPosition) PlaceCamera(cameraNode, Position);
	if (IsRotatingToPosition) OrientCamera(cameraNode, FromDegrees(&Rotation));

	if (!IsLookingAt && !IsLookingAtPosition) return;

	// looking from where the camera is (unless a translation above already set Position) to the target
	if (!(IsTranslating || IsTranslatingToPosition)) {
		const NiPoint3& current = cameraNode->m_worldTransform.pos;
		Position.x = current.x;
		Position.y = current.y;
		Position.z = current.z;
	}
	if (IsLookingAt) LookingAtPosition = RelativeTo(refAxes, refOrigin, LookingAtOffset);
	NiPoint3 angles;
	Position.GetLookAt(&LookingAtPosition, &angles);
	OrientCamera(cameraNode, FromDegrees(&angles));
}


void CameraManager::GetPlane(D3DXPLANE* out, float a, float b, float c, float d) {
	const D3DXPLANE unnormalized(a, b, c, d);
	D3DXPlaneNormalize(out, &unnormalized);
}

namespace {
	// Matrix entry at 1-based row r and column c (_rc in D3DMATRIX).
	inline float Cell(const D3DMATRIX* M, int r, int c) {
		return M->m[r - 1][c - 1];
	}

	// Builds one column of the combined value for a row: column 'lead' alone, or column 'lead' plus/minus 'trail'.
	inline float Combined(const D3DMATRIX* M, int row, int lead, int trail, bool subtract) {
		if (trail == 0) return Cell(M, row, lead);
		return subtract ? Cell(M, row, lead) - Cell(M, row, trail) : Cell(M, row, lead) + Cell(M, row, trail);
	}
}

// The six clip planes of a view-projection matrix (Gribb/Hartmann extraction, normalized).
void CameraManager::SetFrustum(frustum* planesOut, D3DMATRIX* viewProj) {

	// Near uses column 3 alone; the other five planes pair column 4 with another column.
	struct PlaneRecipe {
		int Index;
		int Lead;
		int Trail;
		bool Subtract;
	};
	static const PlaneRecipe recipes[] = {
		{ frustum::PLANENEAR,   3, 0, false },
		{ frustum::PLANEFAR,    4, 3, true  },
		{ frustum::PLANELEFT,   4, 1, false },
		{ frustum::PLANERIGHT,  4, 1, true  },
		{ frustum::PLANETOP,    4, 2, true  },
		{ frustum::PLANEBOTTOM, 4, 2, false },
	};

	for (const PlaneRecipe& recipe : recipes) {
		float row1 = Combined(viewProj, 1, recipe.Lead, recipe.Trail, recipe.Subtract);
		float row2 = Combined(viewProj, 2, recipe.Lead, recipe.Trail, recipe.Subtract);
		float row3 = Combined(viewProj, 3, recipe.Lead, recipe.Trail, recipe.Subtract);
		float row4 = Combined(viewProj, 4, recipe.Lead, recipe.Trail, recipe.Subtract);
		GetPlane(&planesOut->plane[recipe.Index], row1, row2, row3, row4);
	}
}


// The game's own frustum-plane object for a camera placed at the given (camera-relative) location with the matrix's axes.
void CameraManager::SetFrustumPlanes(NiFrustumPlanes* planesOut, D3DMATRIX* viewProj, D3DXVECTOR3 eyeOffset, NiFrustum lens) {
	eyeOffset += D3DXVECTOR3(TheRenderManager->CameraPosition);

	// forward = column 3, up = column 4 + column 2, right = column 4 + column 1
	NiPoint3 axis[3] = {
		NiPoint3(Cell(viewProj, 1, 3), Cell(viewProj, 2, 3), Cell(viewProj, 3, 3)),
		NiPoint3(Combined(viewProj, 1, 4, 2, false), Combined(viewProj, 2, 4, 2, false), Combined(viewProj, 3, 4, 2, false)),
		NiPoint3(Combined(viewProj, 1, 4, 1, false), Combined(viewProj, 2, 4, 1, false), Combined(viewProj, 3, 4, 1, false)),
	};
	for (NiPoint3& a : axis) a.Unitize();

	NiTransform placement;
	placement.pos = NiPoint3(eyeOffset.x, eyeOffset.y, eyeOffset.z);
	placement.rot = NiMatrix33(axis[0].x, axis[1].x, axis[2].x,
							   axis[0].y, axis[1].y, axis[2].y,
							   axis[0].z, axis[1].z, axis[2].z);

	planesOut->Set(lens, placement);
	planesOut->SetActivePlaneState(63);	// all six planes on
}


// Whether a node's bounding sphere is at least partly inside the frustum (planes are camera-relative). skipNear leaves the
// near plane out of the test.
bool CameraManager::InFrustum(frustum* planes, NiNode* node, bool skipNear) {
	const NiBound* sphere = node->GetWorldBound();
	if (sphere == nullptr) return false;

	const D3DXVECTOR4& eye = TheRenderManager->CameraPosition;
	const D3DXVECTOR3 center(sphere->Center.x - eye.x, sphere->Center.y - eye.y, sphere->Center.z - eye.z);

	for (int p = skipNear ? 1 : 0; p < 6; ++p)
		if (D3DXPlaneDotCoord(&planes->plane[p], &center) <= -sphere->Radius) return false;
	return true;
}
