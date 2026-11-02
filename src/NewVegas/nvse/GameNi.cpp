// Out-of-line parts of the engine headers (GameNi.h / Game.h), plus the compile-time layout checks.
#include "GameNi.h"	// (everything else arrives through the forced Framework.h)
#include "LayoutCheck.h"

// ---------------------------------------------------------------------------------------------------------------------
// Maths
// ---------------------------------------------------------------------------------------------------------------------

// Heading (around z, +90 so that 0 faces +y) and pitch from LookAt towards this point, in degrees; no roll.
void NiPoint3::GetLookAt(NiPoint3* target, NiPoint3* angles) {
	float dx = x - target->x;
	float dy = y - target->y;
	float dz = z - target->z;
	float distance = sqrt(pow(dx, 2) + pow(dy, 2) + pow(dz, 2));
	angles->x = D3DXToDegree(atan2(dy, dx)) + 90.0f;
	angles->y = D3DXToDegree(acos(dz / distance)) - 90.0f;
	angles->z = 0.0f;
}

// xyz to unit length (w untouched); a near-zero vector becomes +z.
void NiVector4::Normalize() {	// w is left alone
	float length = sqrt((x * x) + (y * y) + (z * z));
	if (!(length > 0.000001f)) {
		x = 0.0f; y = 0.0f; z = 1.0f;
		return;
	}
	x = x / length;
	y = y / length;
	z = z / length;
}

// Rotation from Euler angles (v->x around z, v->y around x, v->z around y), radians unless `degrees`.
void NiMatrix33::GenerateRotationMatrixZXY(NiPoint3* angles, bool degrees) {
	float a = angles->x, b = angles->y, c = angles->z;
	if (degrees) {
		a = D3DXToRadian(a);
		b = D3DXToRadian(b);
		c = D3DXToRadian(c);
	}
	const float ca = cos(a), sa = sin(a), cb = cos(b), sb = sin(b), cc = cos(c), sc = sin(c);
	data[0][0] = ca * cc - sa * sb * sc;
	data[0][1] = -cb * sa;
	data[0][2] = ca * sc + cc * sa * sb;
	data[1][0] = cc * sa + ca * sb * sc;
	data[1][1] = ca * cb;
	data[1][2] = sa * sc - ca * cc * sb;
	data[2][0] = -cb * sc;
	data[2][1] = sb;
	data[2][2] = cb * cc;
}

// Inside unless some active plane has the whole sphere behind it.
bool NiBound::WithinFrustum(NiFrustumPlanes* planes) {
	for (UInt32 plane = 0; plane < NiFrustumPlanes::MaxPlanes; ++plane) {
		if (planes->IsPlaneActive(plane) && WhichSide(planes->CullingPlanes[plane]) == NiPlane::NegativeSide)
			return false;
	}
	return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// Scene graph
// ---------------------------------------------------------------------------------------------------------------------

float NiAVObject::GetDistance(NiPoint3* point) {
	const NiPoint3& here = m_worldTransform.pos;
	NiPoint3 d;
	d.x = here.x - point->x;
	d.y = here.y - point->y;
	d.z = here.z - point->z;
	return sqrt((d.x * d.x) + (d.y * d.y) + (d.z * d.z));
}

// The scene graph's own FoV update (0xC52020), then the particle shaders' copy (0xB54000).
void SceneGraph::UpdateParticleShaderFoV(float fov) {
	ThisCall(0x00C52020, this, fov, 0, NULL, 0);
	CdeclCall<void>(0x00B54000, fov);
}

// ---------------------------------------------------------------------------------------------------------------------
// Shaders
// ---------------------------------------------------------------------------------------------------------------------

// The game's shader for `type`, made on first use. Only tall grass and geometry decals can be made here.
BSShader* BSShaderManager::GetShader(ShaderType type) {
	if (!GetRenderer() || type > BSSM_SHADER_VOLUMETRIC_FOG)
		return nullptr;

	BSShader*& slot = pspShaders[type];
	if (slot)
		return slot;

	if (type == BSSM_SHADER_TALL_GRASS)
		slot = TallGrassShader::CreateShader();
	else if (type == BSSM_SHADER_GEOM_DECAL)
		slot = GeometryDecalShader::CreateShader();
	else
		return nullptr;
	return slot;
}

// As the game builds it (0xBACF00): one stream of position, normal, colour and uv, plus the instance index in texcoord1.
TallGrassShader* __stdcall TallGrassShader::CreateShader() {	// the game's own: 0xBACF00
	typedef NiShaderDeclaration D;
	NiDX9ShaderDeclaration* declaration = NiDX9ShaderDeclaration::Create(NiDX9Renderer::GetSingleton(), 5, 1);
	declaration->SetEntry(0, 0, D::SHADERPARAM_NI_POSITION, D::SPTYPE_FLOAT3, 0);
	declaration->SetEntry(1, 3, D::SHADERPARAM_NI_NORMAL, D::SPTYPE_FLOAT3, 0);
	declaration->SetEntry(2, 5, D::SHADERPARAM_NI_COLOR, D::SPTYPE_FLOAT4, 0);
	declaration->SetEntry(3, 7, D::SHADERPARAM_NI_TEXCOORD0, D::SPTYPE_FLOAT2, 0);
	declaration->SetEntryAlt(0, 4, 6, D::SHADERPARAM_EXTRA_DATA_MASK, D::SPTYPE_FLOAT1, D::SPUSAGE_TEXCOORD, 1, D::SPTESS_DEFAULT);
	declaration->GetD3DDeclaration();

	TallGrassShader* shader = Create(declaration);
	shader->Initialize();
	shader->InitShaders();
	shader->InitShaderConstants();
	shader->InitPasses();
	shader->SetShaderDecl(declaration);
	return shader;
}

TallGrassShader* TallGrassShader::GetShader() {	// BSShaderManager makes it on first use
	BSShader* shader = BSShaderManager::GetShader(BSShaderManager::BSSM_SHADER_TALL_GRASS);
	return static_cast<TallGrassShader*>(shader);
}

// ---------------------------------------------------------------------------------------------------------------------
// World
// ---------------------------------------------------------------------------------------------------------------------

// Height of the water that matters for Ref: the plane the player stands in, else the nearest plane in front of the
// camera (more than ~78 degrees off the view axis does not count), else the cell's own water level. *waterForm gets the
// cell's water form.
float TES::GetWaterHeight(PlayerCharacter* Ref, SceneGraph* WorldSceneGraph, TESWaterForm** waterForm) {
	TESObjectCELL* cell = Ref->parentCell;
	if (cell) *waterForm = cell->GetWaterForm();
	const D3DXVECTOR3 refPosition = Ref->pos.toD3DXVEC3();
	float level = cell->GetWaterHeight();

	NiCamera* camera = WorldSceneGraph->camera;
	if (!camera) return level;
	const NiMatrix33& view = camera->m_worldTransform.rot;
	D3DXVECTOR3 forward;
	forward.x = view.data[0][0];
	forward.y = view.data[1][0];
	forward.z = view.data[2][0];

	const DList<WaterGroup> groups = waterManager->waterGroups;
	if (groups.count == 0) return level;

	float best = 1000000;
	DNode<WaterGroup>* group = groups.first;
	for (UInt32 g = 0; g < groups.count; g++, group = group->next) {
		const DList<TESObjectREFR> planes = group->data->waterPlanes;
		DNode<TESObjectREFR>* plane = planes.first;
		for (UInt32 p = 0; p < planes.count; p++, plane = plane->next) {
			NiNode* planeNode = plane->data->GetNode();
			NiBound* bound = planeNode->GetWorldBound();
			const float gap = planeNode->GetDistance(&Ref->pos) - bound->Radius;
			if (Ref->inWater && gap < 0) {		// inside this plane's bound: the best candidate
				best = gap;
				level = bound->Center.z;
				continue;
			}
			D3DXVECTOR3 toWater = bound->Center.toD3DXVEC3() - refPosition;
			D3DXVec3Normalize(&toWater, &toWater);
			const float facing = D3DXVec3Dot(&toWater, &forward);
			if (facing > 0.2 && gap < best) {
				best = gap;
				level = bound->Center.z;
			}
		}
	}
	return level;
}

// Turns on the eye position constant (flag 0x400) for every ShadowLightShader vertex shader pass (88..560).
void ShadowLightShader::EnableEyePositionForAllPasses() {	// for NVR's replacement shaders
	UInt32* vertexConstantFlags = (UInt32*)0x11FCC80;
	for (UInt32 pass = 88; pass <= 560; pass++)
		vertexConstantFlags[pass] |= 0x400;
}
