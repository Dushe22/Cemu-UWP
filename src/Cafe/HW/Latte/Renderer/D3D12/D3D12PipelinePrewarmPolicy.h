#pragma once

#include <cstddef>
#include <cstdint>

namespace D3D12PipelinePrewarmPolicy
{
// storage and admission limits must match or the tail of the file is never prewarmed
inline constexpr std::size_t kMaxRecipeCacheEntries = 16384;
inline constexpr std::uint64_t kMaxRecipeCacheBytes = 256ull * 1024 * 1024;
inline constexpr std::uint64_t kPrewarmMemoryHeadroom = 2ull * 1024 * 1024 * 1024;
inline constexpr std::uint64_t kMaxPrewarmMemoryGrowth = 1024ull * 1024 * 1024;

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
}
