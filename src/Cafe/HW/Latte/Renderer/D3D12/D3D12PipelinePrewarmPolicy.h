#pragma once

#include <cstddef>
#include <cstdint>

namespace D3D12PipelinePrewarmPolicy
{
// storage and admission limits must match or the tail of the file is never prewarmed
inline constexpr std::size_t kMaxRecipeCacheEntries = 16384;
inline constexpr std::uint64_t kMaxRecipeCacheBytes = 256ull * 1024 * 1024;
// stop about 0.4 GB below the point where heavy work is deferred (84% of 5 GB): BOTW's prewarm stopped at
// 3.1 GB with 2 GB of the budget unused, leaving those pipelines to be built during play
inline constexpr std::uint64_t kPrewarmMemoryHeadroom = 1280ull * 1024 * 1024;
inline constexpr std::uint64_t kMaxPrewarmMemoryGrowth = 1536ull * 1024 * 1024;

enum class Decision { Allow, RecordLimit, MemoryPressure, MemoryHeadroom, MemoryGrowth };

inline Decision CheckAdmission(bool memoryEnabled, bool heavyWorkDeferred, bool memoryPressure,
    std::uint64_t usage, std::uint64_t limit, std::uint64_t baseline, std::size_t pending) noexcept
{
    if (heavyWorkDeferred || (memoryEnabled && memoryPressure))
        return Decision::MemoryPressure;
    if (pending >= kMaxRecipeCacheEntries)
        return Decision::RecordLimit;
    if (!memoryEnabled)
        return Decision::Allow;
    if (limit <= kPrewarmMemoryHeadroom || usage >= limit - kPrewarmMemoryHeadroom)
        return Decision::MemoryHeadroom;
    // charge all process growth since D3D12 does not expose PSO allocation sizes
    if (usage > baseline && usage - baseline >= kMaxPrewarmMemoryGrowth)
        return Decision::MemoryGrowth;
    return Decision::Allow;
}

inline const char* DecisionName(Decision decision) noexcept
{
    switch (decision)
    {
    case Decision::Allow: return "complete";
    case Decision::RecordLimit: return "record_limit";
    case Decision::MemoryPressure: return "memory_pressure";
    case Decision::MemoryHeadroom: return "memory_headroom";
    case Decision::MemoryGrowth: return "memory_growth";
    }
    return "unknown";
}

inline bool CanStoreRecipe(std::size_t entryCount, std::uint64_t totalBytes,
    std::uint64_t previousBytes, std::uint64_t newBytes, bool replacingExisting) noexcept
{
    if (newBytes == 0 || previousBytes > totalBytes || totalBytes > kMaxRecipeCacheBytes ||
        (!replacingExisting && (previousBytes != 0 || entryCount >= kMaxRecipeCacheEntries)))
        return false;
    return newBytes <= kMaxRecipeCacheBytes - (totalBytes - previousBytes);
}

// recipes a draw used stay until unused this many days. Launches used to count instead: BOTW reached session 37 after a
// few days of short test launches and deleted 639 recipes, the rain pass among them
inline constexpr std::uint32_t kPruneAfterDays = 30;
// never used by a draw (an async build whose depth bias drifted away): gone after this many sessions
inline constexpr std::uint32_t kPruneNeverUsedAfterSessions = 2;

// pipelines from draws that cannot be skipped are never pruned: rebuilding one stalls the render thread
inline bool ShouldPruneRecipe(bool neverUsed, bool mustBeReady, std::uint32_t sessionsSinceUse, std::uint32_t daysSinceUse) noexcept
{
    if (mustBeReady)
        return false;
    if (neverUsed)
        return sessionsSinceUse >= kPruneNeverUsedAfterSessions;
    return daysSinceUse >= kPruneAfterDays;
}

// importance tiers from usage history. The emulator cannot tell what a pipeline draws, so "used in at least half of the
// game's sessions" stands in for core gameplay (Link, abilities, common enemies, UI). A fixed count did not work: BOTW at
// session 41 put 993 of 1853 recipes in core with 10 sessions, while MK8 at session 7 had none
enum class Tier { Core = 1, Frequent = 2, Occasional = 3 };
inline constexpr std::uint32_t kFrequentTierSessions = 3;

inline Tier ClassifyRecipe(bool mustBeReady, std::uint32_t sessionsUsed, std::uint32_t gameSessions) noexcept
{
    const std::uint32_t coreSessions = gameSessions / 2 > kFrequentTierSessions ? gameSessions / 2 : kFrequentTierSessions;
    if (mustBeReady || sessionsUsed >= coreSessions)
        return Tier::Core;
    return sessionsUsed >= kFrequentTierSessions ? Tier::Frequent : Tier::Occasional;
}
}
