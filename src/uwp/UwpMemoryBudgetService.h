#pragma once

#include <cstdint>

namespace CemuUwp::MemoryBudgetService
{
enum class Pressure : uint8_t
{
	Normal,
	Soft,
	Hard,
	Critical,
};

struct Snapshot
{
	bool enabled{};
	bool updated{};
	bool heavyWorkDeferred{};
	uint64_t usageBytes{};
	uint64_t limitBytes{};
	uint64_t expectedLimitBytes{};
	uint64_t recommendedReleaseBytes{};
	uint64_t allocationFailures{};
	uint64_t heapCompactions{};
	Pressure pressure{ Pressure::Normal };
};

void Initialize();
void Shutdown();
void Tick();
// rate-limited unless immediate
Snapshot SampleNow(bool immediate = false);
bool TryPoll(uint64_t& generation, Snapshot& snapshot);
Snapshot GetLatestSnapshot();
bool IsHeavyWorkDeferred();
uint64_t AllocationFailureCount();
// safe from any thread, the heap lock serialises callers
bool CompactProcessHeap();
const char* PressureName(Pressure pressure);
}
