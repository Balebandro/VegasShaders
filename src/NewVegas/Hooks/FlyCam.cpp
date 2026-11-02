// The fly camera's speed: the add / subtract keys change the scroll multiplier, and the four movement patches below add
// (or take) Scroll to the camera's forward/back and right/left offsets in the game's own fly-cam update.

static float Scroll = 0.0f;	// units per step, read by the naked patches

typedef void(__thiscall* FlyCamUpdateFn)(PlayerCharacter*);
FlyCamUpdateFn UpdateFlyCam = (FlyCamUpdateFn)Hooks::UpdateFlyCam;

void __fastcall UpdateFlyCamHook(PlayerCharacter* player, UInt32 edx) {
	SettingsMainStruct::FlyCamStruct& flyCam = TheSettingManager->SettingsMain.FlyCam;
	const float previous = flyCam.ScrollMultiplier;

	// both keys are polled every call, add first
	const bool faster = Global->OnKeyDown(flyCam.KeyAdd);
	if (faster) flyCam.ScrollMultiplier += flyCam.StepValue;
	const bool slower = Global->OnKeyDown(flyCam.KeySubtract);
	if (slower) flyCam.ScrollMultiplier -= flyCam.StepValue;
	if (flyCam.ScrollMultiplier <= 0.0f) flyCam.ScrollMultiplier = 1.0f;	// never zero or below

	Scroll = flyCam.ScrollMultiplier * 10.0f;
	if (flyCam.ScrollMultiplier != previous) {
		char text[64];
		sprintf(text, "Fly cam scroll multiplier: %.1f", flyCam.ScrollMultiplier);
		InterfaceManager->ShowMessage(text);
	}
	UpdateFlyCam(player);
}

// The game keeps the forward/back offset at [ebp-80h] and the right/left one at [ebp-84h] in that function's frame.
#define NVR_FLYCAM_PATCH(name, offset, op, resume)		\
	__declspec(naked) void name() {						\
		__asm fld dword ptr [ebp - offset]				\
		__asm op dword ptr [Scroll]						\
		__asm fstp dword ptr [ebp - offset]				\
		__asm jmp resume								\
	}

NVR_FLYCAM_PATCH(UpdateForwardFlyCamHook, 80h, fadd, Jumpers::FlyCam::UpdateForwardFlyCamReturn)
NVR_FLYCAM_PATCH(UpdateBackwardFlyCamHook, 80h, fsub, Jumpers::FlyCam::UpdateBackwardFlyCamReturn)
NVR_FLYCAM_PATCH(UpdateRightFlyCamHook, 84h, fadd, Jumpers::FlyCam::UpdateRightFlyCamReturn)
NVR_FLYCAM_PATCH(UpdateLeftFlyCamHook, 84h, fsub, Jumpers::FlyCam::UpdateLeftFlyCamReturn)

#undef NVR_FLYCAM_PATCH
