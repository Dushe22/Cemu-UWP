#pragma once

// Detailed CPU-time splits in log.txt (emulated CPU cores and HLE calls, Latte thread, draw head, recorder stages).
// Off by default: the timers run on every HLE call, thread switch and draw. Read from the settings when a game
// starts and fixed for that run, so a split never mixes timed and untimed marks.
#if defined(CEMU_UWP)
#include <intrin.h>

inline bool g_uwpDetailedProfiling = false;

// rdtsc when profiling, 0 otherwise (differences of two ticks are then 0)
inline uint64 UwpProfTick()
{
	return g_uwpDetailedProfiling ? __rdtsc() : 0;
}
#endif
