#include "UwpMemoryBudgetService.h"

#include "Cemu/Logging/CemuLogging.h"

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.System.Profile.h>

#include <windows.h>
#include <new.h>

#include <atomic>
#include <chrono>
#include <mutex>

namespace
{
using CemuUwp::MemoryBudgetService::Pressure;
using CemuUwp::MemoryBudgetService::Snapshot;
using winrt::Windows::System::AppMemoryUsageLevel;
using winrt::Windows::System::MemoryManager;

constexpr auto kSampleInterval = std::chrono::milliseconds(500);
constexpr auto kForcedSampleInterval = std::chrono::milliseconds(100);
constexpr double kSoftPressureRatio = 0.82;
constexpr double kHardPressureRatio = 0.90;
constexpr double kCriticalPressureRatio = 0.96;
constexpr double kTrimTargetRatio = 0.76;
// pipeline creation transiently commits over a gigabyte, hence the hysteresis
constexpr double kHeavyWorkStopRatio = 0.84;
constexpr double kHeavyWorkResumeRatio = 0.78;
// cap on heap compactions so an exhausted heap cannot spin inside operator new
constexpr uint32_t kMaxCompactionsPerWindow = 8;
constexpr auto kCompactionWindow = std::chrono::seconds(10);

std::mutex s_stateMutex;
Snapshot s_latestSnapshot{};
std::chrono::steady_clock::time_point s_lastSample{};
std::atomic_int64_t s_nextSampleTickNs{ 0 };
std::atomic_uint64_t s_sampleGeneration{ 0 };
std::atomic_bool s_initialized{ false };
std::atomic_bool s_enabled{ false };
std::atomic_bool s_forceSample{ true };
std::atomic_bool s_heavyWorkDeferred{ false };
std::atomic_uint64_t s_allocationFailures{ 0 };
std::atomic_uint64_t s_heapCompactions{ 0 };
uint64_t s_lastSampledAllocationFailures = 0;
winrt::event_token s_usageIncreasedToken{};
// peak usage between the 5 s memory log lines, and the last system usage level, to show spikes before a termination
uint64_t s_peakUsageBytes = 0;
std::chrono::steady_clock::time_point s_lastMemoryReport{};
int s_lastUsageLevel = -1;
winrt::event_token s_limitChangingToken{};
bool s_eventsRegistered = false;

_PNH s_previousNewHandler = nullptr;
std::atomic_bool s_newHandlerInstalled{ false };
std::atomic_int64_t s_compactionWindowStartNs{ 0 };
std::atomic_uint32_t s_compactionsInWindow{ 0 };
thread_local std::chrono::steady_clock::time_point t_allocationFailureWindowStart{};
thread_local uint32_t t_allocationFailuresInWindow = 0;

bool IsXboxDevice()
{
	try
	{
		return winrt::Windows::System::Profile::AnalyticsInfo::VersionInfo().DeviceFamily() == L"Windows.Xbox";
	}
	catch (...)
	{
		return false;
	}
}

Pressure ClassifyPressure(uint64_t usage, uint64_t limit, AppMemoryUsageLevel usageLevel)
{
	if (limit == 0)
		return usageLevel == AppMemoryUsageLevel::OverLimit ? Pressure::Critical : Pressure::Normal;

	const double ratio = static_cast<double>(usage) / static_cast<double>(limit);
	if (usageLevel == AppMemoryUsageLevel::OverLimit || ratio >= kCriticalPressureRatio)
		return Pressure::Critical;
	if (usageLevel == AppMemoryUsageLevel::High || ratio >= kHardPressureRatio)
		return Pressure::Hard;
	if (ratio >= kSoftPressureRatio)
		return Pressure::Soft;
	return Pressure::Normal;
}

int64_t NowTickNs()
{
	return std::chrono::duration_cast<std::chrono::nanoseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count();
}

// must not allocate, log or throw; non-zero makes operator new retry
int __cdecl AllocationFailureHandler(size_t requestedBytes) noexcept
{
	s_allocationFailures.fetch_add(1, std::memory_order_relaxed);
	s_forceSample.store(true, std::memory_order_release);
	s_heavyWorkDeferred.store(true, std::memory_order_release);
	if (s_previousNewHandler)
	{
		(void)s_previousNewHandler(requestedBytes);
	}

	const auto now = std::chrono::steady_clock::now();
	if (now - t_allocationFailureWindowStart > std::chrono::seconds(1))
	{
		t_allocationFailureWindowStart = now;
		t_allocationFailuresInWindow = 0;
	}
	if (++t_allocationFailuresInWindow > 1)
		return 0; // the retry after compaction failed as well

	const int64_t nowNs = NowTickNs();
	const int64_t windowStartNs = s_compactionWindowStartNs.load(std::memory_order_relaxed);
	const int64_t windowNs = std::chrono::duration_cast<std::chrono::nanoseconds>(kCompactionWindow).count();
	if (nowNs - windowStartNs > windowNs)
	{
		s_compactionWindowStartNs.store(nowNs, std::memory_order_relaxed);
		s_compactionsInWindow.store(0, std::memory_order_relaxed);
	}
	if (s_compactionsInWindow.fetch_add(1, std::memory_order_relaxed) >= kMaxCompactionsPerWindow)
		return 0;

	if (!CemuUwp::MemoryBudgetService::CompactProcessHeap())
		return 0;
	return 1;
}

void InstallAllocationFailureHandler()
{
	if (s_newHandlerInstalled.exchange(true, std::memory_order_acq_rel))
		return;
	s_previousNewHandler = _set_new_handler(AllocationFailureHandler);
	if (s_previousNewHandler == AllocationFailureHandler)
		s_previousNewHandler = nullptr;
}

void UninstallAllocationFailureHandler()
{
	if (!s_newHandlerInstalled.exchange(false, std::memory_order_acq_rel))
		return;
	(void)_set_new_handler(s_previousNewHandler);
	s_previousNewHandler = nullptr;
}

bool Sample(bool forced, std::chrono::milliseconds minimumInterval)
{
	if (!s_initialized.load(std::memory_order_acquire) || !s_enabled.load(std::memory_order_acquire))
		return false;

	const auto now = std::chrono::steady_clock::now();
	const int64_t nowTickNs = std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
	const bool eventForced = s_forceSample.exchange(false, std::memory_order_acq_rel);
	if (!forced && !eventForced)
	{
		const int64_t nextSampleTickNs = s_nextSampleTickNs.load(std::memory_order_acquire);
		if (nextSampleTickNs != 0 && nowTickNs < nextSampleTickNs)
			return false;
	}
	{
		std::scoped_lock lock(s_stateMutex);
		if (s_lastSample.time_since_epoch().count() != 0)
		{
			const auto elapsed = now - s_lastSample;
			if (!forced && !eventForced && elapsed < kSampleInterval)
				return false;
			if ((forced || eventForced) && elapsed < minimumInterval)
			{
				if (eventForced)
					s_forceSample.store(true, std::memory_order_release);
				return false;
			}
		}
		s_lastSample = now;
		s_nextSampleTickNs.store(
			nowTickNs + std::chrono::duration_cast<std::chrono::nanoseconds>(kSampleInterval).count(),
			std::memory_order_release);
	}

	try
	{
		Snapshot result{};
		result.enabled = true;
		result.usageBytes = MemoryManager::AppMemoryUsage();
		result.limitBytes = MemoryManager::AppMemoryUsageLimit();
		result.expectedLimitBytes = MemoryManager::ExpectedAppMemoryUsageLimit();
		const AppMemoryUsageLevel usageLevel = MemoryManager::AppMemoryUsageLevel();
		result.pressure = ClassifyPressure(result.usageBytes, result.limitBytes, usageLevel);
		{
			std::scoped_lock lock(s_stateMutex);
			s_peakUsageBytes = std::max(s_peakUsageBytes, result.usageBytes);
			const bool levelChanged = s_lastUsageLevel != (int)usageLevel;
			if (levelChanged || now - s_lastMemoryReport >= std::chrono::seconds(5))
			{
				cemuLog_log(LogType::Force, "Xbox memory: usage {} MB, peak {} MB since last line, limit {} MB, system level {}{}.",
					(unsigned long long)(result.usageBytes >> 20), (unsigned long long)(s_peakUsageBytes >> 20),
					(unsigned long long)(result.limitBytes >> 20), (int)usageLevel, levelChanged ? " (changed)" : "");
				s_lastUsageLevel = (int)usageLevel;
				s_lastMemoryReport = now;
				s_peakUsageBytes = result.usageBytes;
			}
		}
		result.allocationFailures = s_allocationFailures.load(std::memory_order_relaxed);
		result.heapCompactions = s_heapCompactions.load(std::memory_order_relaxed);

		bool allocationFailedSinceLastSample = false;
		{
			std::scoped_lock lock(s_stateMutex);
			allocationFailedSinceLastSample = result.allocationFailures != s_lastSampledAllocationFailures;
			s_lastSampledAllocationFailures = result.allocationFailures;
		}
		if (allocationFailedSinceLastSample)
		{
			result.pressure = Pressure::Critical;
		}

		if (result.pressure != Pressure::Normal && result.limitBytes != 0)
		{
			const uint64_t targetUsage = static_cast<uint64_t>(static_cast<double>(result.limitBytes) * kTrimTargetRatio);
			if (result.usageBytes > targetUsage)
				result.recommendedReleaseBytes = result.usageBytes - targetUsage;
		}

		const double ratio = result.limitBytes != 0
			? static_cast<double>(result.usageBytes) / static_cast<double>(result.limitBytes)
			: 0.0;
		bool deferred = s_heavyWorkDeferred.load(std::memory_order_acquire);
		if (!deferred)
		{
			if (allocationFailedSinceLastSample || result.pressure == Pressure::Critical || ratio >= kHeavyWorkStopRatio)
			{
				deferred = true;
				cemuLog_log(LogType::Force,
					"Xbox memory guard: deferring new shader/pipeline work at {}/{} MB ({}%{}).",
					(unsigned long long)(result.usageBytes / (1024ull * 1024ull)),
					(unsigned long long)(result.limitBytes / (1024ull * 1024ull)),
					(int)(ratio * 100.0),
					allocationFailedSinceLastSample ? ", allocation failure" : "");
			}
		}
		else if (!allocationFailedSinceLastSample && result.pressure < Pressure::Hard && ratio < kHeavyWorkResumeRatio)
		{
			deferred = false;
			cemuLog_log(LogType::Force,
				"Xbox memory guard: resuming shader/pipeline work at {}/{} MB ({}%).",
				(unsigned long long)(result.usageBytes / (1024ull * 1024ull)),
				(unsigned long long)(result.limitBytes / (1024ull * 1024ull)),
				(int)(ratio * 100.0));
		}
		s_heavyWorkDeferred.store(deferred, std::memory_order_release);
		result.heavyWorkDeferred = deferred;

		std::scoped_lock lock(s_stateMutex);
		s_latestSnapshot = result;
		uint64_t generation = s_sampleGeneration.load(std::memory_order_relaxed) + 1;
		if (generation == 0)
			generation = 1;
		s_sampleGeneration.store(generation, std::memory_order_release);
		return true;
	}
	catch (const winrt::hresult_error& ex)
	{
		cemuLog_logOnce(LogType::Force, "Xbox memory budget query failed (0x{:08x}); retaining the last valid sample.", (uint32_t)ex.code());
	}
	return false;
}
}

namespace CemuUwp::MemoryBudgetService
{
const char* PressureName(Pressure pressure)
{
	switch (pressure)
	{
	case Pressure::Soft: return "soft";
	case Pressure::Hard: return "hard";
	case Pressure::Critical: return "critical";
	case Pressure::Normal:
	default: return "normal";
	}
}

bool CompactProcessHeap()
{
	HANDLE heap = GetProcessHeap();
	if (!heap)
		return false;
	// HeapCompact decommits freed CRT heap pages back to the app commit budget
	(void)HeapCompact(heap, 0);
	s_heapCompactions.fetch_add(1, std::memory_order_relaxed);
	return true;
}

void Initialize()
{
	bool expected = false;
	if (!s_initialized.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
		return;

	const bool enabled = IsXboxDevice();
	s_enabled.store(enabled, std::memory_order_release);
	if (!enabled)
		return;

	InstallAllocationFailureHandler();

	try
	{
		s_usageIncreasedToken = MemoryManager::AppMemoryUsageIncreased(
			[](const winrt::Windows::Foundation::IInspectable&, const winrt::Windows::Foundation::IInspectable&)
			{
				s_forceSample.store(true, std::memory_order_release);
			});
		s_limitChangingToken = MemoryManager::AppMemoryUsageLimitChanging(
			[](const winrt::Windows::Foundation::IInspectable&, const winrt::Windows::System::AppMemoryUsageLimitChangingEventArgs&)
			{
				s_forceSample.store(true, std::memory_order_release);
			});
		s_eventsRegistered = true;
	}
	catch (const winrt::hresult_error& ex)
	{
		cemuLog_log(LogType::Force, "UWP memory budget event registration failed (0x{:08x}); polling remains enabled.", (uint32_t)ex.code());
	}

	s_forceSample.store(true, std::memory_order_release);
	Tick();
	const Snapshot initial = GetLatestSnapshot();
	if (initial.enabled)
	{
		cemuLog_log(LogType::Force,
			"Xbox memory budget initialized: usage={}MB limit={}MB expected={}MB pressure={} (heavy work deferred at {}%, resumed below {}%).",
			(unsigned long long)(initial.usageBytes / (1024ull * 1024ull)),
			(unsigned long long)(initial.limitBytes / (1024ull * 1024ull)),
			(unsigned long long)(initial.expectedLimitBytes / (1024ull * 1024ull)),
			PressureName(initial.pressure),
			(int)(kHeavyWorkStopRatio * 100.0),
			(int)(kHeavyWorkResumeRatio * 100.0));
	}
}

void Shutdown()
{
	if (!s_initialized.exchange(false, std::memory_order_acq_rel))
		return;

	if (s_eventsRegistered)
	{
		try
		{
			MemoryManager::AppMemoryUsageIncreased(s_usageIncreasedToken);
			MemoryManager::AppMemoryUsageLimitChanging(s_limitChangingToken);
		}
		catch (...)
		{
		}
	}

	UninstallAllocationFailureHandler();
	s_eventsRegistered = false;
	s_usageIncreasedToken = {};
	s_limitChangingToken = {};
	s_enabled.store(false, std::memory_order_release);
	s_forceSample.store(true, std::memory_order_release);
	s_heavyWorkDeferred.store(false, std::memory_order_release);
	std::scoped_lock lock(s_stateMutex);
	s_latestSnapshot = {};
	s_lastSample = {};
	s_lastSampledAllocationFailures = s_allocationFailures.load(std::memory_order_relaxed);
	s_nextSampleTickNs.store(0, std::memory_order_release);
	s_sampleGeneration.store(0, std::memory_order_release);
}

void Tick()
{
	(void)Sample(false, kSampleInterval);
}

Snapshot SampleNow(bool immediate)
{
	(void)Sample(true, immediate ? std::chrono::milliseconds(0) : kForcedSampleInterval);
	return GetLatestSnapshot();
}

bool TryPoll(uint64_t& lastGeneration, Snapshot& result)
{
	uint64_t generation = s_sampleGeneration.load(std::memory_order_acquire);
	if (generation == lastGeneration)
		return false;

	std::scoped_lock lock(s_stateMutex);
	generation = s_sampleGeneration.load(std::memory_order_relaxed);
	if (generation == lastGeneration)
		return false;
	result = s_latestSnapshot;
	result.updated = true;
	lastGeneration = generation;
	return true;
}

Snapshot GetLatestSnapshot()
{
	std::scoped_lock lock(s_stateMutex);
	Snapshot result = s_latestSnapshot;
	result.updated = false;
	result.heavyWorkDeferred = s_heavyWorkDeferred.load(std::memory_order_acquire);
	return result;
}

bool IsHeavyWorkDeferred()
{
	return s_heavyWorkDeferred.load(std::memory_order_acquire);
}

uint64_t AllocationFailureCount()
{
	return s_allocationFailures.load(std::memory_order_relaxed);
}
}
