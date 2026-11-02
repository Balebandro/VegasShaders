namespace {
	LONGLONG ReadCounter() {
		LARGE_INTEGER now;
		QueryPerformanceCounter(&now);
		return now.QuadPart;
	}
}

void FrameRateManager::Initialize() {
	Logger::Log("Starting the framerate manager...");
	FrameRateManager* clock = new FrameRateManager();
	TheFrameRateManager = clock;

	clock->Time = clock->LastTime = clock->ElapsedTime = 0.0;
	clock->LastPerformance = 0.0;
	clock->FrameNumber = 0;
	clock->SmartControlMPF = 1000.0 / TheSettingManager->SettingsMain.FrameRate.SmartControlFPS;	// target milliseconds per frame

	LARGE_INTEGER ticksPerSecond;
	QueryPerformanceFrequency(&ticksPerSecond);
	clock->PerformanceFrequency = ticksPerSecond.QuadPart;
	clock->PerformanceCounterStart = ReadCounter();
}

// Once per rendered frame: seconds since startup, and since the last frame.
void FrameRateManager::UpdatePerformance() {
	FrameNumber++;	// UNOFFICIAL 2026-10-18: once per rendered frame, before the frame's first UpdateConstants
	const LONGLONG ticks = ReadCounter() - PerformanceCounterStart;
	Time = (double)ticks / (double)PerformanceFrequency;
	const double previous = LastTime;
	LastTime = Time;
	ElapsedTime = Time - previous;
}

// Milliseconds since startup.
double FrameRateManager::GetPerformance() {
	const FrameRateManager* clock = TheFrameRateManager;
	const LONGLONG ticks = ReadCounter() - clock->PerformanceCounterStart;
	return (double)ticks * 1000.0 / (double)clock->PerformanceFrequency;
}

// Smart frame control: sleeps away what is left of the target frame time, then hands the game a frame time (with the
// flow-control margin) when it is within 10-120 ms.
void FrameRateManager::PerformSync() {
	const double frameStart = GetPerformance();
	const double spent = frameStart - LastPerformance;

	*Pointers::Generic::MPF = 0.0f;
	if (spent < SmartControlMPF) Sleep((DWORD)(SmartControlMPF - spent));
	const double frameEnd = GetPerformance();
	LastPerformance = frameEnd;

	const float reported = (float)(spent + TheSettingManager->SettingsMain.FrameRate.FlowControl + (frameEnd - frameStart));
	if (reported >= 10.0f && reported <= 120.0f) *Pointers::Generic::MPF = reported;
}
