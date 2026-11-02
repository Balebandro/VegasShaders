#pragma once

// UNOFFICIAL 2026-10-13: ShaderManager::UpdateConstants runs twice a frame (before the world, NewVegas/Hooks/Render.cpp, and again
// before the post effects), so an effect that added TheFrameRateManager->ElapsedTime there advanced its clock twice a frame.
// This gives the frame's elapsed seconds (clamped to 0.1, as the effects did) on the first call of a frame, and 0 on the second.
// lastFrameTime: the caller's own (start it at -1).
inline double ElapsedOncePerFrame(double& lastFrameTime) {
	const double now = TheFrameRateManager->Time;
	if (now == lastFrameTime) return 0.0;
	const double elapsed = lastFrameTime < 0.0 ? 0.0 : now - lastFrameTime;
	lastFrameTime = now;
	return elapsed < 0.0 ? 0.0 : (elapsed > 0.1 ? 0.1 : elapsed);
}

// Effects whose speeds were tuned by eye while their clock ran twice a frame keep that look by running at this rate.
static const double TunedClockRate = 2.0;
