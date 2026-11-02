#pragma once

// UNOFFICIAL 2026-10-13 (user: a tester found our test tools left in a release): the test tools - the auto-test link and its A/B
// shots (SettingShots.h), the neon spill self-test (EffectSelfTest.h) and the lamp-pass test (LampPassTest.h) - are built only
// into development builds. A release tree carries the marker file ReleaseBuild.h next to this one, and they are left out.
#if __has_include("ReleaseBuild.h")
	#define NVR_TEST_TOOLS 0
#else
	#define NVR_TEST_TOOLS 1
#endif
