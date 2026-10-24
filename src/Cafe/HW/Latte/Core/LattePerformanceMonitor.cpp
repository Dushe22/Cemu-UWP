#include "Cafe/HW/Latte/Core/LattePerformanceMonitor.h"
#include "Cafe/HW/Latte/Core/LatteOverlay.h"
#include "WindowSystem.h"

#include "Cemu/Logging/CemuLogging.h"
#include <array>
#include <string>
#include <fmt/format.h>

performanceMonitor_t performanceMonitor{};

#if defined(LATTE_UWP_HITCH_LOG)
namespace
{
constexpr size_t kHitchOpCount = static_cast<size_t>(LatteUwpHitchOp::Count);
constexpr const char* kHitchOpNames[]{
	"pso", "psoMutex", "psoDriver", "shader", "textureReload", "textureCleanup", "shaderCleanup",
	"bufferCleanup", "memoryPressure", "descriptorRollover", "queueWait", "present", "shaderResidency", "asyncPoll"
};
static_assert(std::size(kHitchOpNames) == kHitchOpCount);
struct UwpHitchOperationTiming
{
	uint64 count{};
	uint64 totalUs{};
	uint64 maxUs{};
};
using UwpHitchOperationTimings = std::array<UwpHitchOperationTiming, kHitchOpCount>;
struct UwpFramePacing
{
	UwpHitchOperationTimings frameOperations{};
	UwpHitchOperationTimings windowOperations{};
	uint64 lastFrameClockUs{};
	uint64 frameSerial{};
	uint64 frameCount{};
	uint64 elapsedUs{};
	uint64 maxFrameUs{};
	std::array<uint64, 4> overThreshold{};
	uint32 detailedHitches{};
	uint32 suppressedHitches{};
};
// thread_local so prewarm workers do not race the render thread's bookkeeping
thread_local UwpFramePacing s_uwpFramePacing;

#if defined(CEMU_UWP_PERFORMANCE_EXPERIMENT)
thread_local LatteUwpTimingDiagnostics s_uwpTimingDiagnostics;

void LogUwpTimingDiagnostics()
{
	auto& diagnostics = s_uwpTimingDiagnostics;
	const auto& w = diagnostics.window;
	const auto& wait = w.longestGuestWait;
	cemuLog_log(LogType::Force,
		"UWP timing diagnostic: phase={} mask={} measuring={} mixedPhase={} frames={} "
		"vblanks/flips={} / {} vblanksPerFrame_min/max={}/{} swapInterval_latest/min/max={}/{}/{} "
		"flipRequest/execute={}/{} timerServices={} waitingServices={} overdueServices={} skippedPeriods={} "
		"period_us={} lateness_total/max_ms={:.3f}/{:.3f} "
		"batches={} batch_total/max_ms={:.3f}/{:.3f} maxBatch_vblanks/guestWait_ms={}/{:.3f} "
		"guestWaits/stalled={}/{} guestWait_total/max_ms={:.3f}/{:.3f} "
		"maxGuestWait_vblanks={} address=0x{:08x} mask=0x{:08x} op={} reference=0x{:08x} initial=0x{:08x} final=0x{:08x} "
		"(CPU durations inclusive; diagnostic only).",
		diagnostics.phase, diagnostics.mask, diagnostics.measuring, w.mixedPhase, w.frames,
		w.vblanks, w.flips, w.frames ? w.minFrameVblanks : 0, w.maxFrameVblanks,
		w.swapInterval, w.vblanks ? w.minSwapInterval : 0, w.maxSwapInterval,
		w.flipRequests, w.flipExecuted, w.services, w.waitServices, w.overdueServices, w.skippedPeriods,
		w.periodUs, (double)w.latenessUs / 1000.0, (double)w.maxLatenessUs / 1000.0,
		w.batches, (double)w.batchUs / 1000.0, (double)w.maxBatchUs / 1000.0,
		w.maxBatchVblanks, (double)w.maxBatchGuestWaitUs / 1000.0,
		w.guestWaits, w.stalledGuestWaits, (double)w.guestWaitUs / 1000.0, (double)wait.durationUs / 1000.0,
		wait.vblanks, wait.address, wait.mask, wait.compareOp, wait.reference, wait.initial, wait.finalValue);
#if defined(CEMU_UWP_VBLANK_BOUNDARY_EXPERIMENT)
	cemuLog_log(LogType::Force, "UWP vblank boundary experiment: checks={} deliveredVblanks={} (completed generic packets only; unchanged deadline-gated timer).",
		w.boundaryTimerChecks, w.boundaryVblanks);
#endif
	diagnostics.ResetWindow();
}
#endif

std::string FormatHitchOperations(const UwpHitchOperationTimings& timings)
{
	std::string result;
	for (size_t i = 0; i < timings.size(); ++i)
	{
		const auto& timing = timings[i];
		if (timing.count == 0)
			continue;
		fmt::format_to(std::back_inserter(result), "{}{}={}/{:.3f}/{:.3f}",
			result.empty() ? "" : " ", kHitchOpNames[i], timing.count,
			(double)timing.totalUs / 1000.0, (double)timing.maxUs / 1000.0);
	}
	return result.empty() ? "none" : result;
}

void LogUwpFramePacing()
{
	auto& pacing = s_uwpFramePacing;
	const uint64 now = (uint64)std::chrono::duration_cast<std::chrono::microseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count();
	++pacing.frameSerial;
	if (pacing.lastFrameClockUs == 0)
	{
		pacing.lastFrameClockUs = now;
		pacing.frameOperations = {};
		pacing.windowOperations = {};
#if defined(CEMU_UWP_PERFORMANCE_EXPERIMENT)
		s_uwpTimingDiagnostics.ResetWindow();
		s_uwpTimingDiagnostics.frameVblanks = 0;
#endif
		return;
	}
#if defined(CEMU_UWP_PERFORMANCE_EXPERIMENT)
	s_uwpTimingDiagnostics.FinishFrame();
#endif
	const uint64 frameUs = now - pacing.lastFrameClockUs;
	pacing.lastFrameClockUs = now;
	++pacing.frameCount;
	pacing.elapsedUs += frameUs;
	pacing.maxFrameUs = std::max(pacing.maxFrameUs, frameUs);
	constexpr std::array<uint64, 4> thresholdsUs{ 33333, 50000, 100000, 250000 };
	for (size_t i = 0; i < thresholdsUs.size(); ++i)
		pacing.overThreshold[i] += frameUs > thresholdsUs[i] ? 1 : 0;
	if (frameUs > thresholdsUs[2])
	{
		if (pacing.detailedHitches < 8)
		{
			++pacing.detailedHitches;
			cemuLog_log(LogType::Force,
				"UWP frame hitch: frame={} gap_ms={:.3f} cpu_ms commandInput/guestFence/readbackAsync/shaderCreate={:.3f}/{:.3f}/{:.3f}/{:.3f} operations_count/total_ms/max_ms [{}].",
				pacing.frameSerial, (double)frameUs / 1000.0,
				(double)PPCTimer_tscToMicroseconds(performanceMonitor.gpuTime_idleTime.getPreviousFrameValue()) / 1000.0,
				(double)PPCTimer_tscToMicroseconds(performanceMonitor.gpuTime_fenceTime.getPreviousFrameValue()) / 1000.0,
				(double)PPCTimer_tscToMicroseconds(performanceMonitor.gpuTime_waitForAsync.getPreviousFrameValue()) / 1000.0,
				(double)PPCTimer_tscToMicroseconds(performanceMonitor.gpuTime_shaderCreate.getPreviousFrameValue()) / 1000.0,
				FormatHitchOperations(pacing.frameOperations));
		}
		else
			++pacing.suppressedHitches;
	}
	if (pacing.elapsedUs >= 5000000)
	{
		cemuLog_log(LogType::Force,
			"UWP frame pacing: frames={} elapsed_ms={:.3f} mean_ms={:.3f} max_ms={:.3f} over33/50/100/250={}/{}/{}/{} suppressedHitches={}.",
			pacing.frameCount, (double)pacing.elapsedUs / 1000.0,
			(double)pacing.elapsedUs / (1000.0 * (double)pacing.frameCount),
			(double)pacing.maxFrameUs / 1000.0, pacing.overThreshold[0], pacing.overThreshold[1],
			pacing.overThreshold[2], pacing.overThreshold[3], pacing.suppressedHitches);
		cemuLog_log(LogType::Force,
			"UWP hitch work: count/total_ms/max_ms [{}] (inclusive CPU durations; nested work may overlap).",
			FormatHitchOperations(pacing.windowOperations));
#if defined(CEMU_UWP_PERFORMANCE_EXPERIMENT)
		LogUwpTimingDiagnostics();
#endif
		pacing.frameCount = 0;
		pacing.elapsedUs = 0;
		pacing.maxFrameUs = 0;
		pacing.overThreshold = {};
		pacing.detailedHitches = 0;
		pacing.suppressedHitches = 0;
		pacing.windowOperations = {};
	}
	pacing.frameOperations = {};
}
}

#if defined(CEMU_UWP_PERFORMANCE_EXPERIMENT)
void LatteUwpSetPerformanceExperimentPhase(uint32 phase, uint32 mask, bool measuring)
{
	s_uwpTimingDiagnostics.SetPhase(phase, mask, measuring);
}

void LatteUwpRecordVblank(uint32 interval, bool flipped, uint32 requests, uint32 executed)
{
	s_uwpTimingDiagnostics.RecordVblank(interval, flipped, requests, executed);
}

void LatteUwpRecordVblankService(uint64 latenessUs, uint64 missed, uint64 skipped, uint64 periodUs, bool waiting)
{
	s_uwpTimingDiagnostics.RecordService(latenessUs, missed, skipped, periodUs, waiting);
}

uint64 LatteUwpGetDiagnosticVblankCount()
{
	return s_uwpTimingDiagnostics.totalVblanks;
}
#if defined(CEMU_UWP_VBLANK_BOUNDARY_EXPERIMENT)
bool LatteUwpIsVblankBoundaryExperimentEnabled()
{
	return s_uwpTimingDiagnostics.IsBoundaryExperimentEnabled();
}
void LatteUwpRecordBoundaryTimerCheck(uint64 vblanks)
{
	++s_uwpTimingDiagnostics.window.boundaryTimerChecks;
	s_uwpTimingDiagnostics.window.boundaryVblanks += vblanks;
}
#endif

void LatteUwpRecordGuestWait(const LatteUwpTimingDiagnostics::GuestWait& wait, bool stalled)
{
	s_uwpTimingDiagnostics.RecordGuestWait(wait, stalled);
}

LatteUwpCommandBatchScope::LatteUwpCommandBatchScope()
	: m_startedTicks(PPCTimer_getRawTsc()),
	  m_startedVblanks(s_uwpTimingDiagnostics.totalVblanks),
	  m_startedGuestWaitUs(s_uwpTimingDiagnostics.totalGuestWaitUs)
{
}

LatteUwpCommandBatchScope::~LatteUwpCommandBatchScope()
{
	s_uwpTimingDiagnostics.RecordBatch(PPCTimer_tscToMicroseconds(PPCTimer_getRawTsc() - m_startedTicks),
		s_uwpTimingDiagnostics.totalVblanks - m_startedVblanks,
		s_uwpTimingDiagnostics.totalGuestWaitUs - m_startedGuestWaitUs);
}
#endif

void LatteUwpRecordHitchOperation(LatteUwpHitchOp operation, uint64 elapsedUs)
{
	const size_t index = static_cast<size_t>(operation);
	if (index >= kHitchOpCount)
		return;
	for (auto* timings : { &s_uwpFramePacing.frameOperations, &s_uwpFramePacing.windowOperations })
	{
		auto& timing = (*timings)[index];
		++timing.count;
		timing.totalUs += elapsedUs;
		timing.maxUs = std::max(timing.maxUs, elapsedUs);
	}
}
#endif

void LattePerformanceMonitor_frameEnd()
{
	// per-frame stats
	performanceMonitor.gpuTime_shaderCreate.frameFinished();
	performanceMonitor.gpuTime_frameTime.frameFinished();
	performanceMonitor.gpuTime_idleTime.frameFinished();
	performanceMonitor.gpuTime_fenceTime.frameFinished();

	performanceMonitor.gpuTime_dcStageTextures.frameFinished();
	performanceMonitor.gpuTime_dcStageVertexMgr.frameFinished();
	performanceMonitor.gpuTime_dcStageShaderAndUniformMgr.frameFinished();
	performanceMonitor.gpuTime_dcStageIndexMgr.frameFinished();
	performanceMonitor.gpuTime_dcStageMRT.frameFinished();
	performanceMonitor.gpuTime_dcStageDrawcallAPI.frameFinished();
	performanceMonitor.gpuTime_waitForAsync.frameFinished();

#if defined(LATTE_UWP_HITCH_LOG)
	LogUwpFramePacing();
#endif
	// CPU wall times from the command processor, not GPU timestamps
	static uint64 logIdleTicks = 0;
	static uint64 logGuestFenceTicks = 0;
	static uint64 logAsyncTicks = 0;
	logIdleTicks += performanceMonitor.gpuTime_idleTime.getPreviousFrameValue();
	logGuestFenceTicks += performanceMonitor.gpuTime_fenceTime.getPreviousFrameValue();
	logAsyncTicks += performanceMonitor.gpuTime_waitForAsync.getPreviousFrameValue();

	uint32 elapsedTime = GetTickCount() - performanceMonitor.cycle[performanceMonitor.cycleIndex].lastUpdate;
	if (elapsedTime >= 1000)
	{
		bool isFirstUpdate = performanceMonitor.cycle[performanceMonitor.cycleIndex].lastUpdate == 0;

		static uint64 logElapsedTime = 0;
		static uint64 logFrameCount = 0;
		static uint64 logDrawCalls = 0;
		if (isFirstUpdate)
		{
			logElapsedTime = 0;
			logFrameCount = 0;
			logDrawCalls = 0;
			logIdleTicks = 0;
			logGuestFenceTicks = 0;
			logAsyncTicks = 0;
		}
		else
		{
			logElapsedTime += elapsedTime;
			logFrameCount += performanceMonitor.cycle[performanceMonitor.cycleIndex].frameCounter;
			logDrawCalls += performanceMonitor.cycle[performanceMonitor.cycleIndex].drawCallCounter;
			if (logElapsedTime >= 5000)
			{
				const double gameFps = static_cast<double>(logFrameCount) * 1000.0 /
					static_cast<double>(logElapsedTime);
				const double averageFrameTime = logFrameCount != 0 ?
					static_cast<double>(logElapsedTime) / static_cast<double>(logFrameCount) : 0.0;
				cemuLog_log(LogType::Force,
					"UWP game-frame performance: frames={} window_ms={} fps={:.2f} avg_frame_ms={:.3f} draws_per_frame={} "
					"cpu_wait_ms commandInput/guestFence/readbackAsync={:.2f}/{:.2f}/{:.2f}",
					logFrameCount, logElapsedTime, gameFps, averageFrameTime, logFrameCount != 0 ? logDrawCalls / logFrameCount : 0,
					static_cast<double>(PPCTimer_tscToMicroseconds(logIdleTicks)) / 1000.0,
					static_cast<double>(PPCTimer_tscToMicroseconds(logGuestFenceTicks)) / 1000.0,
					static_cast<double>(PPCTimer_tscToMicroseconds(logAsyncTicks)) / 1000.0);
				logElapsedTime = 0;
				logFrameCount = 0;
				logDrawCalls = 0;
				logIdleTicks = 0;
				logGuestFenceTicks = 0;
				logAsyncTicks = 0;
			}
		}

		// sum up raw stats
		uint32 totalElapsedTime = GetTickCount() - performanceMonitor.cycle[(performanceMonitor.cycleIndex + 1) % PERFORMANCE_MONITOR_TRACK_CYCLES].lastUpdate;
		uint32 totalElapsedTimeFPS = GetTickCount() - performanceMonitor.cycle[(performanceMonitor.cycleIndex + PERFORMANCE_MONITOR_TRACK_CYCLES - 1) % PERFORMANCE_MONITOR_TRACK_CYCLES].lastUpdate;
		uint32 elapsedFrames = 0;
		uint32 elapsedFrames2S = 0; // elapsed frames for last two entries (seconds)
		uint64 skippedCycles = 0;
		uint64 vertexDataUploaded = 0;
		uint64 vertexDataCached = 0;
		uint64 uniformBankUploadedData = 0;
		uint64 uniformBankUploadedCount = 0;
		uint64 indexDataUploaded = 0;
		uint64 indexDataCached = 0;
		uint32 frameCounter = 0;
		uint32 drawCallCounter = 0;
		uint32 fastDrawCallCounter = 0;
		uint32 shaderBindCounter = 0;
		uint32 recompilerLeaveCount = 0;
		uint32 threadLeaveCount = 0;
		for (sint32 i = 0; i < PERFORMANCE_MONITOR_TRACK_CYCLES; i++)
		{
			elapsedFrames += performanceMonitor.cycle[i].frameCounter;
			skippedCycles += performanceMonitor.cycle[i].skippedCycles;
			vertexDataUploaded += performanceMonitor.cycle[i].vertexDataUploaded;
			vertexDataCached += performanceMonitor.cycle[i].vertexDataCached;
			uniformBankUploadedData += performanceMonitor.cycle[i].uniformBankUploadedData;
			uniformBankUploadedCount += performanceMonitor.cycle[i].uniformBankUploadedCount;
			indexDataUploaded += performanceMonitor.cycle[i].indexDataUploaded;
			indexDataCached += performanceMonitor.cycle[i].indexDataCached;
			frameCounter += performanceMonitor.cycle[i].frameCounter;
			drawCallCounter += performanceMonitor.cycle[i].drawCallCounter;
			fastDrawCallCounter += performanceMonitor.cycle[i].fastDrawCallCounter;
			shaderBindCounter += performanceMonitor.cycle[i].shaderBindCount;
			recompilerLeaveCount += performanceMonitor.cycle[i].recompilerLeaveCount;
			threadLeaveCount += performanceMonitor.cycle[i].threadLeaveCount;
		}
		elapsedFrames = std::max<uint32>(elapsedFrames, 1);
		elapsedFrames2S = performanceMonitor.cycle[(performanceMonitor.cycleIndex + PERFORMANCE_MONITOR_TRACK_CYCLES - 0) % PERFORMANCE_MONITOR_TRACK_CYCLES].frameCounter;
		elapsedFrames2S += performanceMonitor.cycle[(performanceMonitor.cycleIndex + PERFORMANCE_MONITOR_TRACK_CYCLES - 1) % PERFORMANCE_MONITOR_TRACK_CYCLES].frameCounter;
		elapsedFrames2S = std::max<uint32>(elapsedFrames2S, 1);
		// calculate stats
		uint64 passedCycles = PPCInterpreter_getMainCoreCycleCounter() - performanceMonitor.cycle[(performanceMonitor.cycleIndex + 1) % PERFORMANCE_MONITOR_TRACK_CYCLES].lastCycleCount;
		passedCycles -= skippedCycles;
		uint64 vertexDataUploadPerFrame = (vertexDataUploaded / (uint64)elapsedFrames);
		vertexDataUploadPerFrame /= 1024ULL;
		uint64 vertexDataCachedPerFrame = (vertexDataCached / (uint64)elapsedFrames);
		vertexDataCachedPerFrame /= 1024ULL;
		uint64 uniformBankDataUploadedPerFrame = (uniformBankUploadedData / (uint64)elapsedFrames);
		uniformBankDataUploadedPerFrame /= 1024ULL;
		uint32 uniformBankCountUploadedPerFrame = (uint32)(uniformBankUploadedCount / (uint64)elapsedFrames);
		uint64 indexDataUploadPerFrame = (indexDataUploaded / (uint64)elapsedFrames);

		double fps = (double)elapsedFrames2S * 1000.0 / (double)totalElapsedTimeFPS;
		uint32 shaderBindsPerFrame = shaderBindCounter / elapsedFrames;
		passedCycles = passedCycles * 1000ULL / totalElapsedTime;
		uint32 rlps = (uint32)((uint64)recompilerLeaveCount * 1000ULL / (uint64)totalElapsedTime);
		uint32 tlps = (uint32)((uint64)threadLeaveCount * 1000ULL / (uint64)totalElapsedTime);
		// set stats
		performanceMonitor.stats.indexDataUploadPerFrame = indexDataUploadPerFrame;
		// next counter cycle
		sint32 nextCycleIndex = (performanceMonitor.cycleIndex + 1) % PERFORMANCE_MONITOR_TRACK_CYCLES;
		performanceMonitor.cycle[nextCycleIndex].drawCallCounter = 0;
		performanceMonitor.cycle[nextCycleIndex].fastDrawCallCounter = 0;
		performanceMonitor.cycle[nextCycleIndex].frameCounter = 0;
		performanceMonitor.cycle[nextCycleIndex].shaderBindCount = 0;
		performanceMonitor.cycle[nextCycleIndex].lastCycleCount = PPCInterpreter_getMainCoreCycleCounter();
		performanceMonitor.cycle[nextCycleIndex].skippedCycles = 0;
		performanceMonitor.cycle[nextCycleIndex].vertexDataUploaded = 0;
		performanceMonitor.cycle[nextCycleIndex].vertexDataCached = 0;
		performanceMonitor.cycle[nextCycleIndex].uniformBankUploadedData = 0;
		performanceMonitor.cycle[nextCycleIndex].uniformBankUploadedCount = 0;
		performanceMonitor.cycle[nextCycleIndex].indexDataUploaded = 0;
		performanceMonitor.cycle[nextCycleIndex].indexDataCached = 0;
		performanceMonitor.cycle[nextCycleIndex].recompilerLeaveCount = 0;
		performanceMonitor.cycle[nextCycleIndex].threadLeaveCount = 0;
		performanceMonitor.cycleIndex = nextCycleIndex;

		// next update in 1 second
		performanceMonitor.cycle[performanceMonitor.cycleIndex].lastUpdate = GetTickCount();

		if (isFirstUpdate)
		{
			LatteOverlay_updateStats(0.0, 0, 0);
			WindowSystem::UpdateWindowTitles(false, false, 0.0);
		}
		else
		{
			LatteOverlay_updateStats(fps, drawCallCounter / elapsedFrames, fastDrawCallCounter / elapsedFrames);
			WindowSystem::UpdateWindowTitles(false, false, fps);
		}
	}
}

void LattePerformanceMonitor_frameBegin()
{
	performanceMonitor.vk.numDrawBarriersPerFrame.reset();
	performanceMonitor.vk.numBeginRenderpassPerFrame.reset();
}
