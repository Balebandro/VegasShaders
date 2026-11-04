// Shadow hooks: NVR's shadow maps drawn in place of the game's shadow call, the shadow flag of interior lights,
// and the name of the leaves node of a SpeedTree.

#include "../../core/GpuProfiler.h"
#include "../../core/SessionLog.h"	// 2026-11-04: CRASH lines' stage

// The game's own shadow render, which the hooked call site used to reach directly.
static constexpr UInt32 kGameRenderShadowMaps = 0x871A50;

void __fastcall RenderShadowMapHook(void* apThis) {
	// UNOFFICIAL TAA: the jitter covers the world scene only, on the understanding that shadow maps are rendered before it.
	// If that ever stops being true, the cascades get fitted to a jittered frustum and shadow edges shimmer -- say so.
	static bool jitterWarned = false;
	if (!jitterWarned) {
		TAAEffect* taa = TheShaderManager->Effects.TAA;
		if (taa && taa->IsJitterActive()) {
			Logger::Log("[WARNING] TAA : shadow maps rendered while the world camera was jittered; cascades will shimmer");
			jitterWarned = true;
		}
	}

	{
		SessionLog::StageScope stage("shadow maps");
		TheShadowManager->RenderShadowMaps();
	}

	// Then the call this hook replaced, timed so its share of the frame is visible.
	static GpuTimer gameShadowTimer("Game call 0x871A50");
	GpuProfileScope scope(gameShadowTimer, TheRenderManager->device);
	CdeclCall(kGameRenderShadowMaps);
}

// Decides whether a light that was just attached to a reference casts shadows (CastShadows) and records whether it is
// carried (CanCarry: 0 placed, 1 carried, 2 carried by the player). The player's own light in the visible hand casts none.
enum : UInt8 { kPlacedLight = 0, kCarriedLight = 1, kPlayersLight = 2 };	// NiPointLight::CanCarry values
static void SetLightShadowFlags(TESObjectREFR* Ref, TESObjectLIGH* Light, NiPointLight* LightPoint) {
	const bool carriable = (Light->lightFlags & TESObjectLIGH::LightFlags::kLightFlags_CanCarry) != 0;

	if (!carriable) {
		// A placed light reference: its own "casts shadows" flag decides.
		const bool isLightRef = Ref && Ref->baseForm && Ref->baseForm->formType == TESForm::FormType::kFormType_Light;
		if (isLightRef) {
			const bool noShadows = (Ref->flags & TESForm::FormFlags::kFormFlags_NotCastShadows) != 0;
			LightPoint->CastShadows = !noShadows;
			LightPoint->CanCarry = kPlacedLight;
		}
		return;
	}

	LightPoint->CastShadows = TheShaderManager->Effects.ShadowsExteriors->Settings.Interiors.TorchesCastShadows;
	LightPoint->CanCarry = kCarriedLight;
	if (Ref != Player)
		return;

	bool heldInView;
	if (Player->isThirdPerson)
		heldInView = (Player->firstPersonSkinInfo->LightForm == Light);
	else
		heldInView = !Player->ActorSkinInfo->LightForm && Player->firstPersonSkinInfo->LightForm == Light;
	if (heldInView)
		LightPoint->CastShadows = 0;
	LightPoint->CanCarry = kPlayersLight;
}

// Mid-function detour: reads the reference (arg 1), the light form (local) and the new NiPointLight (eax), calls
// SetLightShadowFlags with every register kept, then replays the instructions it overwrote and jumps back.
__declspec(naked) void AddCastShadowFlagHook() {
	__asm {
		mov		esi, dword ptr [ebp + 8]
		pushad
		mov		ecx, dword ptr [ebp - 164h]
		push	eax						// LightPoint
		push	ecx						// Light
		push	esi						// Ref
		call	SetLightShadowFlags
		pop		esi
		pop		ecx
		pop		eax
		popad
		pop		ecx
		pop		esi
		mov		ecx, dword ptr [ebp - 34h]
		jmp		Jumpers::Shadows::AddCastShadowFlagReturn
	}
}

// The second child of a tree's node holds its leaves; name it so it can be told apart later.
static void NameTreeLeaves(BSTreeNode* TreeNode) {
	auto* leavesChild = TreeNode->m_children.data[1];
	leavesChild->SetName("Leaves");
}

__declspec(naked) void LeavesNodeNameHook() {
	__asm {
		mov		dword ptr [ebp - 70h], eax
		push	eax
		call	NameTreeLeaves
		pop		eax
		jmp		Jumpers::Shadows::LeavesNodeNameReturn
	}
}
