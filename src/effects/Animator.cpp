#include "Animator.h"

// Hours of game time since the start of the game; animations run on game time.
static inline float GameHoursNow(TimeGlobals* t) {
	return t->GameDaysPassed->data * 24;
}

Animator::Animator()
	: time(TimeGlobals::Get()), startValue(0), endValue(0), startTime(0), endTime(0), running(false), switched(false) {
}

Animator::~Animator() {
}

// Sets both ends of the animation to one value (it then holds still).
void Animator::Initialize(float value) {
	startValue = endValue = value;
}

// Begins moving from wherever the value is now to finalValue over duration game hours.
void Animator::Start(float duration, float finalValue) {
	const float now = GameHoursNow(time);

	startTime = now;
	endTime = now + duration;
	running = true;

	// GetValue() already sees the new times, so the old start/end values are what it blends
	startValue = GetValue();
	endValue = finalValue;
}

// The animated value at the current game time.
float Animator::GetValue() {
	if (!running) return startValue;

	const float now = GameHoursNow(time);
	if (now >= endTime) {	// UNOFFICIAL 2026-10-22: >= so a zero-length animation (WaterLens' Start(0, ...)) ends instead of dividing 0 by 0
		running = false;
		startValue = endValue;
		return endValue;
	}

	return std::lerp(startValue, endValue, invLerp(startTime, endTime, now));
}
