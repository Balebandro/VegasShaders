#pragma once

class FrameRateManager { // Never disposed
public:

	static void		Initialize();

	void			UpdatePerformance();
	double			GetPerformance();
	void			PerformSync();

	LONGLONG		PerformanceFrequency;
	LONGLONG		PerformanceCounterStart;
	double			Time;
	double			LastTime;
	double			ElapsedTime;
	double			SmartControlMPF;
	double			LastPerformance;
	unsigned int	FrameNumber;	// UNOFFICIAL 2026-10-18: rendered frames, advanced once per Main::Render (UpdatePerformance); NOT per UpdateConstants, which runs twice a frame
};