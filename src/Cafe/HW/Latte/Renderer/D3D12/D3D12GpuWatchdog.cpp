#include "Cafe/HW/Latte/Renderer/D3D12/D3D12GpuWatchdog.h"

#include "Cafe/CafeSystem.h"
#include "Cafe/HW/Latte/Core/Latte.h"
#include "Cemu/Logging/CemuLogging.h"
#if defined(CEMU_UWP)
#include "uwp/UwpMemoryBudgetService.h"
#endif

#include <limits>
#include <string>

namespace
{
constexpr auto kPollInterval = std::chrono::milliseconds(250);
constexpr sint64 kGpuStallMilliseconds = 2000;
constexpr sint64 kFrameStallMilliseconds = 3000;
constexpr sint64 kStalledFlagMilliseconds = 1000;
constexpr auto kRepeatInterval = std::chrono::seconds(5);
constexpr uint32 kMaxStallReportsPerTitle = 24;

const char* ActivityName(uint32 activity)
{
	switch ((D3D12GpuWatchdog::Activity)activity)
	{
	case D3D12GpuWatchdog::Activity::PipelineCreate: return "creating a pipeline";
	case D3D12GpuWatchdog::Activity::FenceWait: return "waiting for a fence";
	case D3D12GpuWatchdog::Activity::Present: return "presenting";
	default: return "outside the renderer";
	}
}

sint64 ElapsedMilliseconds(sint64 fromTicks, sint64 toTicks)
{
	if (fromTicks == 0 || toTicks < fromTicks)
		return 0;
	return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::duration(toTicks - fromTicks)).count();
}

std::string DescribeCommandProcessor(sint64 nowTicks)
{
	const LatteCPWaitInfo info = LatteCP_GetWaitInfo();
	switch (info.wait)
	{
	case LatteCPWait::Commands:
		return fmt::format("waiting for commands for {} ms", ElapsedMilliseconds(info.sinceTicks, nowTicks));
	case LatteCPWait::GuestMemory:
		return fmt::format("waiting on guest memory 0x{:08x} (value 0x{:08x}) for {} ms",
			info.address, info.value, ElapsedMilliseconds(info.sinceTicks, nowTicks));
	default:
		return "processing commands";
	}
}

std::string DescribeMemory()
{
#if defined(CEMU_UWP)
	const auto memory = CemuUwp::MemoryBudgetService::SampleNow(true);
	return fmt::format("{}/{} MB ({}, {} allocation failures)", memory.usageBytes / (1024 * 1024), memory.limitBytes / (1024 * 1024),
		CemuUwp::MemoryBudgetService::PressureName(memory.pressure), memory.allocationFailures);
#else
	return "n/a";
#endif
}
}

D3D12GpuWatchdog::ActivityScope::ActivityScope(D3D12GpuWatchdog& watchdog, Activity activity, uint64 detail)
	: m_watchdog(watchdog),
	  m_previousActivity(watchdog.m_activity.load(std::memory_order_relaxed)),
	  m_previousDetail(watchdog.m_activityDetail.load(std::memory_order_relaxed)),
	  m_previousSince(watchdog.m_activitySince.load(std::memory_order_relaxed))
{
	watchdog.m_activityDetail.store(detail, std::memory_order_relaxed);
	watchdog.m_activitySince.store(NowTicks(), std::memory_order_relaxed);
	watchdog.m_activity.store((uint32)activity, std::memory_order_release);
}

D3D12GpuWatchdog::ActivityScope::~ActivityScope()
{
	m_watchdog.m_activityDetail.store(m_previousDetail, std::memory_order_relaxed);
	m_watchdog.m_activitySince.store(m_previousSince, std::memory_order_relaxed);
	m_watchdog.m_activity.store(m_previousActivity, std::memory_order_release);
}

D3D12GpuWatchdog::~D3D12GpuWatchdog()
{
	Stop();
}

sint64 D3D12GpuWatchdog::NowTicks()
{
	return std::chrono::steady_clock::now().time_since_epoch().count();
}

void D3D12GpuWatchdog::Start(ID3D12Device* device, ID3D12Fence* fence)
{
	Stop();
	if (!device || !fence)
		return;
	m_device = device;
	m_fence = fence;
	m_stop = false;
	m_thread = std::thread([this]() { Run(); });
}

void D3D12GpuWatchdog::Stop()
{
	if (m_thread.joinable())
	{
		{
			std::lock_guard lock(m_threadMutex);
			m_stop = true;
		}
		m_threadCondition.notify_all();
		m_thread.join();
	}
	m_fence.Reset();
	m_device.Reset();
}

void D3D12GpuWatchdog::RecordGamePipeline(uint64 pipelineHash, uint64 vertexHash, uint64 geometryHash, uint64 pixelHash)
{
	auto& pipelines = m_current.pipelines;
	if (!pipelines.empty() && pipelines.back().pipelineHash == pipelineHash && pipelines.back().pixelHash == pixelHash)
		return;
	if (pipelines.size() >= kMaxPipelinesPerList)
	{
		// keep the latest binds; the ones closest to where the GPU stopped matter most
		pipelines.erase(pipelines.begin());
		m_current.droppedPipelines++;
	}
	pipelines.push_back({ pipelineHash, vertexHash, geometryHash, pixelHash });
}

void D3D12GpuWatchdog::RecordOperation(Operation operation)
{
	m_current.operations[(size_t)operation]++;
}

void D3D12GpuWatchdog::Submit(uint64 fenceValue, uint32 drawCount)
{
	m_current.fenceValue = fenceValue;
	m_current.submitted = std::chrono::steady_clock::now();
	m_current.drawCount = drawCount;
	{
		std::lock_guard lock(m_listMutex);
		const uint64 completed = m_lastCompletedFence.load(std::memory_order_relaxed);
		while (!m_submitted.empty() && (m_submitted.front().fenceValue <= completed || m_submitted.size() >= kMaxTrackedLists))
			m_submitted.pop_front();
		m_submitted.emplace_back(std::move(m_current));
	}
	m_current = {};
	m_lastSubmittedFence.store(fenceValue, std::memory_order_release);
}

void D3D12GpuWatchdog::NotifyGameFrame()
{
	m_lastGameFrame.store(NowTicks(), std::memory_order_relaxed);
}

void D3D12GpuWatchdog::LogUnfinishedWorkLocked(uint64 completedFence)
{
	const auto now = std::chrono::steady_clock::now();
	uint32 logged = 0;
	for (const auto& list : m_submitted)
	{
		if (list.fenceValue <= completedFence)
			continue;
		if (++logged > 4)
		{
			cemuLog_log(LogType::Force, "D3D12 watchdog:   (more unfinished command lists not shown)");
			break;
		}
		std::string pipelines;
		for (const auto& pipeline : list.pipelines)
		{
			if (!pipelines.empty())
				pipelines += ' ';
			pipelines += fmt::format("{:016x}[vs {:016x} gs {:016x} ps {:016x}]",
				pipeline.pipelineHash, pipeline.vertexHash, pipeline.geometryHash, pipeline.pixelHash);
		}
		const auto& ops = list.operations;
		cemuLog_log(LogType::Force,
			"D3D12 watchdog:   unfinished fence {} submitted {} ms ago: draws={} output={} snapshot={} overlay={} copies={} readbacks={} game pipelines (oldest first, {} earlier dropped): {}",
			list.fenceValue,
			std::chrono::duration_cast<std::chrono::milliseconds>(now - list.submitted).count(),
			list.drawCount,
			ops[(size_t)Operation::OutputDraw], ops[(size_t)Operation::SnapshotDraw], ops[(size_t)Operation::OverlayDraw],
			ops[(size_t)Operation::SurfaceCopy], ops[(size_t)Operation::Readback],
			list.droppedPipelines,
			pipelines.empty() ? std::string("none") : pipelines);
	}
	if (logged == 0)
		cemuLog_log(LogType::Force, "D3D12 watchdog:   no unfinished command lists");
}

void D3D12GpuWatchdog::Report(const char* what, uint64 completedFence, HRESULT removedReason)
{
	const sint64 now = NowTicks();
	const sint64 lastFrame = m_lastGameFrame.load(std::memory_order_relaxed);
	const uint32 activity = m_activity.load(std::memory_order_acquire);
	cemuLog_log(LogType::Force,
		"D3D12 watchdog: {}: last game frame {} ms ago, GPU completed fence {} of {}, render thread {} for {} ms (0x{:016x}), command processor {}, memory {}, device removed reason 0x{:08x}",
		what,
		lastFrame != 0 ? ElapsedMilliseconds(lastFrame, now) : -1,
		completedFence,
		m_lastSubmittedFence.load(std::memory_order_acquire),
		ActivityName(activity),
		activity != 0 ? ElapsedMilliseconds(m_activitySince.load(std::memory_order_relaxed), now) : 0,
		m_activityDetail.load(std::memory_order_relaxed),
		DescribeCommandProcessor(now),
		DescribeMemory(),
		(uint32)removedReason);
	std::lock_guard lock(m_listMutex);
	LogUnfinishedWorkLocked(completedFence);
}

void D3D12GpuWatchdog::Run()
{
	using clock = std::chrono::steady_clock;
	uint64 lastCompleted = m_fence->GetCompletedValue();
	m_lastCompletedFence.store(lastCompleted, std::memory_order_relaxed);
	auto lastProgress = clock::now();
	auto nextReport = clock::time_point{};
	bool stallReported = false;
	bool removalReported = false;
	uint32 reportsThisTitle = 0;

	std::unique_lock lock(m_threadMutex);
	while (!m_threadCondition.wait_for(lock, kPollInterval, [this]() { return m_stop; }))
	{
		const auto now = clock::now();
		if (!CafeSystem::IsTitleRunning())
		{
			// frame tracking restarts with the next title
			m_lastGameFrame.store(0, std::memory_order_relaxed);
			reportsThisTitle = 0;
		}

		const uint64 completed = m_fence->GetCompletedValue();
		const HRESULT removedReason = m_device->GetDeviceRemovedReason();
		if (FAILED(removedReason) || completed == std::numeric_limits<uint64>::max())
		{
			if (!removalReported)
			{
				// the fence reads as complete once the device is gone, so report against the last real value
				Report("device removed", lastCompleted, removedReason);
				removalReported = true;
			}
			continue;
		}
		if (completed != lastCompleted)
		{
			lastCompleted = completed;
			lastProgress = now;
			m_lastCompletedFence.store(completed, std::memory_order_relaxed);
		}

		const uint64 submitted = m_lastSubmittedFence.load(std::memory_order_acquire);
		// an idle GPU is not stalled; time new work from when the queue last had nothing pending
		if (submitted <= completed)
			lastProgress = now;
		const sint64 gpuStallMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastProgress).count();
		const sint64 lastFrame = m_lastGameFrame.load(std::memory_order_relaxed);
		const sint64 frameStallMs = lastFrame != 0 ? ElapsedMilliseconds(lastFrame, NowTicks()) : 0;
		m_stalled.store(gpuStallMs >= kStalledFlagMilliseconds || frameStallMs >= kStalledFlagMilliseconds, std::memory_order_relaxed);
		const bool gpuStalled = gpuStallMs >= kGpuStallMilliseconds;
		const bool framesStalled = frameStallMs >= kFrameStallMilliseconds;
		if (gpuStalled || framesStalled)
		{
			// a frame stall alone (loading, pausing) is reported once; a stuck GPU keeps reporting
			if ((!stallReported || (gpuStalled && now >= nextReport)) && reportsThisTitle < kMaxStallReportsPerTitle)
			{
				Report(gpuStalled ? "GPU stopped completing work" : "game frames stopped", completed, removedReason);
				reportsThisTitle++;
				nextReport = now + kRepeatInterval;
			}
			stallReported = true;
		}
		else if (stallReported)
		{
			cemuLog_log(LogType::Force, "D3D12 watchdog: work resumed, GPU completed fence {} of {}", completed, submitted);
			stallReported = false;
		}
	}
}
