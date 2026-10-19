#pragma once

#define PERFORMANCE_MONITOR_TRACK_CYCLES	(5) // one cycle lasts one second

// todo - replace PPCTimer with HighResolutionTimer.h
uint64 PPCTimer_getRawTsc();
uint64 PPCTimer_tscToMicroseconds(uint64 us);
#if defined(CEMU_UWP_VBLANK_BOUNDARY_EXPERIMENT) && !defined(CEMU_UWP_PERFORMANCE_EXPERIMENT)
#error CEMU_UWP_VBLANK_BOUNDARY_EXPERIMENT requires CEMU_UWP_PERFORMANCE_EXPERIMENT
#endif

#include <chrono>

#if defined(CEMU_UWP_D3D12_DIAGNOSTICS) || defined(CEMU_UWP_D3D12_WORKLOAD_PROFILE) || defined(CEMU_UWP_PERFORMANCE_EXPERIMENT)
#define LATTE_UWP_DETAILED_PROFILING
#endif

// render-thread CPU wall times, not GPU timestamps; nested scopes may overlap
enum class LatteUwpHitchOp
{
	PipelineCreate,
	PipelineMutexWait,
	PipelineDriverCreate,
	ShaderCompile,
	TextureReload,
	TextureCleanup,
	ShaderCleanup,
	BufferCacheCleanup,
	MemoryPressure,
	DescriptorRollover,
	QueueWait,
	Present,
	ShaderResidency,
	AsyncCompletionPoll,
	Count
};

#if defined(LATTE_UWP_DETAILED_PROFILING)
void LatteUwpRecordHitchOperation(LatteUwpHitchOp operation, uint64 elapsedUs);
#endif

#if defined(CEMU_UWP_PERFORMANCE_EXPERIMENT)
#if defined(CEMU_UWP_VBLANK_BOUNDARY_EXPERIMENT)
class LatteUwpVblankBoundaryCadence
{
public:
	static constexpr uint32 kPacketCadence = 32;
	bool ShouldCheckTimer() noexcept
	{
		if (--m_packetsUntilCheck != 0)
			return false;
		m_packetsUntilCheck = kPacketCadence;
		return true;
	}
private:
	uint32 m_packetsUntilCheck{kPacketCadence};
};
#endif
struct LatteUwpTimingDiagnostics
{
	struct GuestWait
	{
		uint64 durationUs{}, vblanks{};
		uint32 address{}, mask{}, compareOp{}, reference{}, initial{}, finalValue{};
	};
	struct Window
	{
		uint64 frames{}, vblanks{}, flips{}, services{}, overdueServices{}, skippedPeriods{};
		uint64 latenessUs{}, maxLatenessUs{}, periodUs{}, waitServices{};
		uint64 minFrameVblanks{~uint64{0}}, maxFrameVblanks{};
		uint32 minSwapInterval{~uint32{0}}, maxSwapInterval{}, swapInterval{};
		uint32 flipRequests{}, flipExecuted{};
		uint64 batches{}, batchUs{}, maxBatchUs{}, maxBatchVblanks{}, maxBatchGuestWaitUs{};
		uint64 guestWaits{}, stalledGuestWaits{}, guestWaitUs{};
#if defined(CEMU_UWP_VBLANK_BOUNDARY_EXPERIMENT)
		uint64 boundaryTimerChecks{}, boundaryVblanks{};
#endif
		GuestWait longestGuestWait{};
		bool mixedPhase{};
	} window;
	uint64 totalVblanks{}, totalGuestWaitUs{}, frameVblanks{};
	uint32 phase{}, mask{};
	bool measuring{};

	void SetPhase(uint32 newPhase, uint32 newMask, bool newMeasuring)
	{
		window.mixedPhase |= phase != newPhase || mask != newMask || measuring != newMeasuring;
		phase = newPhase;
		mask = newMask;
		measuring = newMeasuring;
	}
	void RecordVblank(uint32 interval, bool flipped, uint32 requests, uint32 executed)
	{
		++totalVblanks;
		++frameVblanks;
		++window.vblanks;
		window.flips += flipped ? 1 : 0;
		window.swapInterval = interval;
		if (interval < window.minSwapInterval) window.minSwapInterval = interval;
		if (interval > window.maxSwapInterval) window.maxSwapInterval = interval;
		window.flipRequests = requests;
		window.flipExecuted = executed;
	}
	void RecordService(uint64 lateUs, uint64 missed, uint64 skipped, uint64 period, bool waiting)
	{
		++window.services;
		window.overdueServices += missed != 0 ? 1 : 0;
		window.skippedPeriods += skipped;
		window.latenessUs += lateUs;
		if (lateUs > window.maxLatenessUs) window.maxLatenessUs = lateUs;
		window.periodUs = period;
		window.waitServices += waiting ? 1 : 0;
	}
	void FinishFrame()
	{
		++window.frames;
		if (frameVblanks < window.minFrameVblanks) window.minFrameVblanks = frameVblanks;
		if (frameVblanks > window.maxFrameVblanks) window.maxFrameVblanks = frameVblanks;
		frameVblanks = 0;
	}
	void RecordBatch(uint64 durationUs, uint64 vblanks, uint64 guestWaitUs)
	{
		++window.batches;
		window.batchUs += durationUs;
		if (durationUs > window.maxBatchUs)
		{
			window.maxBatchUs = durationUs;
			window.maxBatchVblanks = vblanks;
			window.maxBatchGuestWaitUs = guestWaitUs;
		}
	}
	void RecordGuestWait(const GuestWait& wait, bool stalled)
	{
		++window.guestWaits;
		window.stalledGuestWaits += stalled ? 1 : 0;
		window.guestWaitUs += wait.durationUs;
		totalGuestWaitUs += wait.durationUs;
		if (wait.durationUs > window.longestGuestWait.durationUs)
			window.longestGuestWait = wait;
	}
	void ResetWindow() { window = {}; }
#if defined(CEMU_UWP_VBLANK_BOUNDARY_EXPERIMENT)
	bool IsBoundaryExperimentEnabled() const noexcept { return (mask & 8u) != 0; }
#endif
};

void LatteUwpSetPerformanceExperimentPhase(uint32 phase, uint32 mask, bool measuring);
void LatteUwpRecordVblank(uint32 interval, bool flipped, uint32 requests, uint32 executed);
void LatteUwpRecordVblankService(uint64 latenessUs, uint64 missed, uint64 skipped, uint64 periodUs, bool waiting);
#if defined(CEMU_UWP_VBLANK_BOUNDARY_EXPERIMENT)
bool LatteUwpIsVblankBoundaryExperimentEnabled();
void LatteUwpRecordBoundaryTimerCheck(uint64 vblanks);
#endif
uint64 LatteUwpGetDiagnosticVblankCount();
void LatteUwpRecordGuestWait(const LatteUwpTimingDiagnostics::GuestWait& wait, bool stalled);

class LatteUwpCommandBatchScope
{
public:
	LatteUwpCommandBatchScope();
	~LatteUwpCommandBatchScope();
	LatteUwpCommandBatchScope(const LatteUwpCommandBatchScope&) = delete;
	LatteUwpCommandBatchScope& operator=(const LatteUwpCommandBatchScope&) = delete;
private:
	uint64 m_startedTicks{}, m_startedVblanks{}, m_startedGuestWaitUs{};
};
#endif

class LatteUwpHitchScope
{
public:
#if defined(LATTE_UWP_DETAILED_PROFILING)
	explicit LatteUwpHitchScope(LatteUwpHitchOp operation)
		: m_operation(operation), m_started(std::chrono::steady_clock::now()) {}
	~LatteUwpHitchScope()
	{
		LatteUwpRecordHitchOperation(m_operation,
			(uint64)std::chrono::duration_cast<std::chrono::microseconds>(
				std::chrono::steady_clock::now() - m_started).count());
	}
#else
	explicit LatteUwpHitchScope(LatteUwpHitchOp) noexcept {}
#endif
	LatteUwpHitchScope(const LatteUwpHitchScope&) = delete;
	LatteUwpHitchScope& operator=(const LatteUwpHitchScope&) = delete;

private:
#if defined(LATTE_UWP_DETAILED_PROFILING)
	LatteUwpHitchOp m_operation;
	std::chrono::steady_clock::time_point m_started;
#endif
};

class LattePerfStatTimer
{
public:
	void beginMeasuring()
	{
		timerStart = PPCTimer_getRawTsc();
	}

	void endMeasuring()
	{
		uint64 dif = PPCTimer_getRawTsc() - timerStart;
		currentSum += dif;
	}

	void frameFinished()
	{
		previousFrame = currentSum;
		currentSum = 0;
	}

	uint64 getPreviousFrameValue()
	{
		return previousFrame;
	}

#if defined(CEMU_UWP_PERFORMANCE_EXPERIMENT)
	uint64 getCurrentSumForDiagnostics() const { return currentSum; }
#endif

private:
	uint64 currentSum{};
	uint64 previousFrame{};
	uint64 timerStart{};
};

class LattePerfStatCounter
{
public:
	void increment()
	{
		m_value++;
	}

	void decrement()
	{
		cemu_assert_debug(m_value > 0);
		m_value--;
	}

	void decrement(uint32 count)
	{
		cemu_assert_debug(count <= m_value);
		m_value -= count;
	}

	uint32 get()
	{
		return m_value;
	}

	void reset()
	{
		m_value = 0;
	}

private:
	std::atomic_uint32_t m_value{};
};

typedef struct
{
	struct
	{
		// CPU
		uint64 lastCycleCount;
		uint64 skippedCycles;
		uint32 recompilerLeaveCount; // increased everytime the recompiler switches back to interpreter
		uint32 threadLeaveCount; // increased everytime a thread gives up it's timeslice
		// GPU
		uint32 lastUpdate;
		uint32 frameCounter;
		uint32 drawCallCounter;
		uint32 fastDrawCallCounter;
		uint32 shaderBindCount;
		uint64 vertexDataUploaded; // amount of vertex data uploaded to GPU (bytes)
		uint64 vertexDataCached; // amount of vertex data reused from GPU cache (bytes)
		uint64 uniformBankUploadedData; // amount of uniform buffer data (excluding remapped uniforms) uploaded to GPU
		uint64 uniformBankUploadedCount; // number of separate uploads for uniformBankDataUploaded
		uint64 indexDataUploaded;
		uint64 indexDataCached;
	}cycle[PERFORMANCE_MONITOR_TRACK_CYCLES];
	sint32 cycleIndex;
	// new stats
	LattePerfStatTimer gpuTime_frameTime;
	LattePerfStatTimer gpuTime_shaderCreate;
	LattePerfStatTimer gpuTime_idleTime; // time spent waiting for new commands from CPU
	LattePerfStatTimer gpuTime_fenceTime; // time spent waiting for fence condition

	LattePerfStatTimer gpuTime_dcStageTextures; // drawcall texture/mrt setup
	LattePerfStatTimer gpuTime_dcStageVertexMgr; // drawcall vertex setup and upload
	LattePerfStatTimer gpuTime_dcStageShaderAndUniformMgr; // drawcall shader setup and uniform management/upload
	LattePerfStatTimer gpuTime_dcStageIndexMgr; // drawcall index data setup and upload
	LattePerfStatTimer gpuTime_dcStageMRT; // drawcall render target API

	LattePerfStatTimer gpuTime_dcStageDrawcallAPI; // drawcall api call
	LattePerfStatTimer gpuTime_waitForAsync; // waiting for operations to complete (e.g. GX2DrawDone or force texture readback) Also includes texture readback and occlusion query polling logic

	// generic
	uint32 numCompiledVS; // number of compiled vertex shader programs
	uint32 numCompiledGS; // number of compiled geometry shader programs
	uint32 numCompiledPS; // number of compiled pixel shader programs

	// Vulkan
	struct  
	{
		LattePerfStatCounter numDescriptorSets;
		LattePerfStatCounter numDescriptorDynUniformBuffers;
		LattePerfStatCounter numDescriptorStorageBuffers;
		LattePerfStatCounter numDescriptorSamplerTextures;
		LattePerfStatCounter numGraphicPipelines;
		LattePerfStatCounter numImages;
		LattePerfStatCounter numImageViews;
		LattePerfStatCounter numSamplers;
		LattePerfStatCounter numRenderPass;
		LattePerfStatCounter numFramebuffer;

		// per frame
		LattePerfStatCounter numDrawBarriersPerFrame;
		LattePerfStatCounter numBeginRenderpassPerFrame;
	}vk;

	// calculated stats (per frame)
	struct
	{
		uint32 indexDataUploadPerFrame;
	}stats;
}performanceMonitor_t;

extern performanceMonitor_t performanceMonitor;

void LattePerformanceMonitor_frameEnd();
void LattePerformanceMonitor_frameBegin();

#define beginPerfMonProfiling(__obj) if( THasProfiling ) __obj.beginMeasuring()
#define endPerfMonProfiling(__obj) if( THasProfiling ) __obj.endMeasuring()
