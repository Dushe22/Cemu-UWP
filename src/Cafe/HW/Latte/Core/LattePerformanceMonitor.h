#pragma once

#define PERFORMANCE_MONITOR_TRACK_CYCLES	(5) // one cycle lasts one second

// todo - replace PPCTimer with HighResolutionTimer.h
uint64 PPCTimer_getRawTsc();
uint64 PPCTimer_tscToMicroseconds(uint64 us);

#include <chrono>

// UWP release builds keep the sampled (1 in 64 draws) per-stage CPU profile: the forest is render-thread bound
#if defined(CEMU_UWP_D3D12_DIAGNOSTICS) || defined(CEMU_UWP_D3D12_WORKLOAD_PROFILE) || defined(CEMU_UWP)
#define LATTE_UWP_DETAILED_PROFILING
#endif
// release UWP builds keep the cheap per-frame hitch log (frames over 100 ms with what the render thread was doing)
#if defined(LATTE_UWP_DETAILED_PROFILING) || defined(CEMU_UWP)
#define LATTE_UWP_HITCH_LOG
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

#if defined(LATTE_UWP_HITCH_LOG)
void LatteUwpRecordHitchOperation(LatteUwpHitchOp operation, uint64 elapsedUs);
#endif


class LatteUwpHitchScope
{
public:
#if defined(LATTE_UWP_HITCH_LOG)
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
#if defined(LATTE_UWP_HITCH_LOG)
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
