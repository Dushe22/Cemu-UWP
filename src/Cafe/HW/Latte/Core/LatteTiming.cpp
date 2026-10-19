#include "Cafe/HW/Latte/Core/Latte.h"
#include "Cafe/OS/libs/gx2/GX2_Event.h"
#ifdef ENABLE_VULKAN
#include "Cafe/HW/Latte/Renderer/Vulkan/VsyncDriver.h"
#endif
#include "util/highresolutiontimer/HighResolutionTimer.h"
#include "config/CemuConfig.h"
#include "Cafe/CafeSystem.h"

#if defined(CEMU_UWP) && defined(CEMU_UWP_PERFORMANCE_EXPERIMENT)
#include "Cafe/HW/Latte/Core/LattePerformanceMonitor.h"
#endif

#if defined(CEMU_UWP) && defined(CEMU_UWP_ASYNC_COMPLETION_POLL)
#include "Cafe/HW/Latte/Renderer/Renderer.h"
#include "Cafe/HW/Latte/Core/LattePerformanceMonitor.h"
#include <algorithm>
#include <mutex>

namespace
{
HRTick s_lastAsyncCompletionPoll = 0;
HRTick s_asyncCompletionPollInterval = 1;
}
#endif

sint32 s_customVsyncFrequency = -1;

void LatteTiming_NotifyHostVSync();

// calculate time between vsync events in timer units
// standard rate on Wii U is 59.94, however to prevent tearing and microstutter on ~60Hz displays it is better if we slightly overshoot 60 Hz
// can be modified by graphic packs
HRTick LatteTime_CalculateTimeBetweenVSync()
{
	// 59.94 -> 60 * 0.999

	HRTick tick = HighResolutionTimer::getFrequency();
	if (s_customVsyncFrequency > 0)
	{
		tick /= (uint64)s_customVsyncFrequency;
	}
	else
	{
		tick *= 1000ull;
		tick /= 1002ull;
		tick /= 60ull;
	}
	return tick;
}

void LatteTiming_setCustomVsyncFrequency(sint32 frequency)
{
	s_customVsyncFrequency = frequency;
}

void LatteTiming_disableCustomVsyncFrequency()
{
	s_customVsyncFrequency = -1;
}

bool LatteTiming_getCustomVsyncFrequency(sint32& customFrequency)
{
	sint32 t = s_customVsyncFrequency;
	if (t <= 0)
		return false;
	customFrequency = t;
	return true;
}

bool s_usingHostDrivenVSync = false;

void LatteTiming_EnableHostDrivenVSync()
{
	if (s_usingHostDrivenVSync)
		return;
	#ifdef ENABLE_VULKAN
	VsyncDriver_startThread(LatteTiming_NotifyHostVSync);
	s_usingHostDrivenVSync = true;
	#endif
}

bool LatteTiming_IsUsingHostDrivenVSync()
{
	return s_usingHostDrivenVSync;
}

void LatteTiming_Init()
{
	LatteGPUState.timer_frequency = HighResolutionTimer::getFrequency();
	LatteGPUState.timer_bootUp = HighResolutionTimer::now().getTick();
	LatteGPUState.timer_nextVSync = LatteGPUState.timer_bootUp + LatteTime_CalculateTimeBetweenVSync();
#if defined(CEMU_UWP) && defined(CEMU_UWP_VBLANK_BOUNDARY_EXPERIMENT)
	cemuLog_log(LogType::Force, "UWP vblank boundary experiment compiled: phase mask bit 8 enables additional timer service checks every 32 completed generic packets, only outside active draw sequences; mask 0 is baseline. Vblank deadlines and flip logic are retained; guest event service timing is experimental.");
#endif
#if defined(CEMU_UWP) && defined(CEMU_UWP_PERFORMANCE_EXPERIMENT)
#if defined(CEMU_UWP_VBLANK_BOUNDARY_EXPERIMENT)
	cemuLog_log(LogType::Force, "UWP timing diagnostic enabled: vblank/guest-wait/command-batch aggregation; boundary service experiment is separately phase-gated.");
#else
	cemuLog_log(LogType::Force, "UWP timing diagnostic enabled: vblank/guest-wait/command-batch aggregation only; timing behavior unchanged.");
#endif
#endif
#if defined(CEMU_UWP) && defined(CEMU_UWP_ASYNC_COMPLETION_POLL)
	s_lastAsyncCompletionPoll = LatteGPUState.timer_bootUp;
	s_asyncCompletionPollInterval = std::max<HRTick>(HighResolutionTimer::getFrequency() / 1000, 1);
	cemuLog_log(LogType::Force, "D3D12 UWP: nonblocking async completion polling enabled while the command processor waits (1ms cadence; guest vsync unchanged).");
#endif
}

void LatteTiming_signalVsync()
{
	static uint32 s_vsyncIntervalCounter = 0;
#if defined(CEMU_UWP) && defined(CEMU_UWP_PERFORMANCE_EXPERIMENT)
	bool diagnosticFlipped = false;
#endif

	if (!LatteGPUState.gx2InitCalled)
		return;
	s_vsyncIntervalCounter++;
	uint32 swapInterval = 1;
	if (LatteGPUState.sharedArea)
		swapInterval = LatteGPUState.sharedArea->swapInterval;

	// flip
	if (s_vsyncIntervalCounter >= swapInterval)
	{
		// a buffer is only flipped once the GPU has processed a swap request since the last flip
		bool flipPending = false;
		if (LatteGPUState.flipRequestCount > 0)
		{
			LatteGPUState.flipRequestCount.fetch_sub(1);
			flipPending = true;
		}
		if (LatteGPUState.sharedArea)
		{
			// hack/workaround - only execute flip if GX2SwapScanBuffers() isn't lagging behind
			uint64 currentTitleId = CafeSystem::GetForegroundTitleId();
			if (currentTitleId == 0x00050000101c9500 || currentTitleId == 0x00050000101c9400 || currentTitleId == 0x0005000e101c9300)
			{
				uint32 currentFlipRequestCount = _swapEndianU32(LatteGPUState.sharedArea->flipRequestCountBE);
				uint32 currentFlipExecuteCount = _swapEndianU32(LatteGPUState.sharedArea->flipExecuteCountBE);

				if ((currentFlipRequestCount >= currentFlipExecuteCount) || (currentFlipExecuteCount - currentFlipRequestCount < 4))
				{
					LatteGPUState.sharedArea->flipExecuteCountBE = _swapEndianU32(_swapEndianU32(LatteGPUState.sharedArea->flipExecuteCountBE) + 1);
				}

				LatteGPUState.flipCounter++;

			}
			else
			{
				// old code for all other games
				if (flipPending)
				{
					LatteGPUState.sharedArea->flipExecuteCountBE = _swapEndianU32(_swapEndianU32(LatteGPUState.sharedArea->flipExecuteCountBE) + 1);
				}

			}
		}
		// the flip event marks a newly displayed buffer, games pace their next swap from it
		if (flipPending)
		{
			GX2::__GX2NotifyEvent(GX2::GX2CallbackEventType::FLIP);
#if defined(CEMU_UWP) && defined(CEMU_UWP_PERFORMANCE_EXPERIMENT)
			diagnosticFlipped = true;
#endif
			s_vsyncIntervalCounter = 0;
		}
	}
	// vsync
#if defined(CEMU_UWP) && defined(CEMU_UWP_PERFORMANCE_EXPERIMENT)
	LatteUwpRecordVblank(swapInterval, diagnosticFlipped,
		LatteGPUState.sharedArea ? _swapEndianU32(LatteGPUState.sharedArea->flipRequestCountBE) : 0,
		LatteGPUState.sharedArea ? _swapEndianU32(LatteGPUState.sharedArea->flipExecuteCountBE) : 0);
#endif
	GX2::__GX2NotifyEvent(GX2::GX2CallbackEventType::VSYNC);
}

HRTick s_lastHostVsync = 0;

// notify when host vsync event is triggered (on renderer canvas)
void LatteTiming_NotifyHostVSync()
{
	if (!LatteTiming_IsUsingHostDrivenVSync())
		return;
	auto nowTimePoint = HighResolutionTimer::now().getTick();
	auto dif = nowTimePoint - s_lastHostVsync;
	auto vsyncPeriod = LatteTime_CalculateTimeBetweenVSync();


	if (dif < vsyncPeriod)
	{
		// skip
		return;
	}
	uint64 elapsedPeriods = dif / vsyncPeriod;
	if (elapsedPeriods >= 10)
	{
		s_lastHostVsync = nowTimePoint;
	}
	else
		s_lastHostVsync += vsyncPeriod;

	LatteTiming_signalVsync();
}

// handle timed vsync event
void LatteTiming_HandleTimedVsync(bool isCommandProcessorWaiting)
{
	// simulate VSync
	uint64 currentTimer = HighResolutionTimer::now().getTick();
	if( currentTimer >= LatteGPUState.timer_nextVSync )
	{
		if(!LatteTiming_IsUsingHostDrivenVSync())
			LatteTiming_signalVsync();
		// even if vsync is delegated to the host device, we still use this virtual vsync timer to check finished states
		LatteQuery_UpdateFinishedQueries();
		LatteTextureReadback_UpdateFinishedTransfers(false);
#if defined(CEMU_UWP) && defined(CEMU_UWP_ASYNC_COMPLETION_POLL)
		s_lastAsyncCompletionPoll = currentTimer;
#endif
		// update vsync timer
		uint64 vsyncTime = LatteTime_CalculateTimeBetweenVSync();
		uint64 missedVsyncCount = (currentTimer - LatteGPUState.timer_nextVSync) / vsyncTime;
#if defined(CEMU_UWP) && defined(CEMU_UWP_PERFORMANCE_EXPERIMENT)
		LatteUwpRecordVblankService(HighResolutionTimer::ticksToMicroseconds(currentTimer - LatteGPUState.timer_nextVSync),
			missedVsyncCount, missedVsyncCount >= 2 ? missedVsyncCount : 0,
			HighResolutionTimer::ticksToMicroseconds(vsyncTime), isCommandProcessorWaiting);
#endif
		if (missedVsyncCount >= 2)
		{
			LatteGPUState.timer_nextVSync += vsyncTime*(missedVsyncCount+1ULL);
		}
		else	
			LatteGPUState.timer_nextVSync += vsyncTime;
	}
#if defined(CEMU_UWP) && defined(CEMU_UWP_ASYNC_COMPLETION_POLL)
	else if (isCommandProcessorWaiting &&
		(currentTimer - s_lastAsyncCompletionPoll) >= s_asyncCompletionPollInterval)
	{
		// query/readback completion is not vblank-bound, so poll before the next vsync
		s_lastAsyncCompletionPoll = currentTimer;
		const bool queriesPending = LatteQuery_HasPendingCompletions();
		const bool readbacksPending = LatteTextureReadback_HasPendingCompletions();
		if (queriesPending || readbacksPending)
		{
			std::unique_lock<std::recursive_mutex> rendererLock(g_rendererMutex);
			LatteUwpHitchScope pollTiming(LatteUwpHitchOp::AsyncCompletionPoll);
			if (queriesPending)
				LatteQuery_UpdateFinishedQueries();
			if (readbacksPending)
				LatteTextureReadback_UpdateFinishedTransfers(false);
		}
	}
#else
	(void)isCommandProcessorWaiting;
#endif
}
