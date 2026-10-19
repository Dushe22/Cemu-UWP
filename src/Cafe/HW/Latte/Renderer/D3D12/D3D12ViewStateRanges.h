#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>

namespace D3D12ViewStateRanges
{
class Range
{
public:
    static bool TryCreate(std::uint32_t firstMip, std::uint32_t mipCount,
        std::uint32_t mipLevels, std::uint32_t firstSlice, std::uint32_t sliceCount,
        std::uint32_t arraySize, std::uint32_t planeCount,
        std::uint32_t trackedSubresources, Range& result) noexcept
    {
        if (mipLevels == 0 || arraySize == 0 || mipCount == 0 || sliceCount == 0 ||
            planeCount == 0 || planeCount > 2 || firstMip >= mipLevels ||
            firstSlice >= arraySize || mipCount > mipLevels - firstMip ||
            sliceCount > arraySize - firstSlice)
            return false;

        const std::uint64_t planeStride = std::uint64_t{mipLevels} * arraySize;
        if (planeStride > std::numeric_limits<std::uint32_t>::max() / planeCount)
            return false;

        result.m_firstMip = firstMip;
        result.m_mipCount = mipCount;
        result.m_mipLevels = mipLevels;
        result.m_firstSlice = firstSlice;
        result.m_sliceCount = sliceCount;
        result.m_planeCount = planeCount;
        result.m_planeStride = static_cast<std::uint32_t>(planeStride);
        result.m_trackedSubresources = trackedSubresources;
        result.m_count = std::size_t{mipCount} * sliceCount * planeCount;
        result.m_coversAll = firstMip == 0 && mipCount == mipLevels &&
            firstSlice == 0 && sliceCount == arraySize &&
            planeStride * planeCount == trackedSubresources;
        return true;
    }

    std::size_t Count() const noexcept { return m_count; }
    bool CoversAll() const noexcept { return m_coversAll; }

    template<typename Visitor>
    void ForEach(Visitor visitor) const
    {
        for (std::uint32_t plane = 0; plane < m_planeCount; ++plane)
        {
            for (std::uint32_t slice = 0; slice < m_sliceCount; ++slice)
            {
                for (std::uint32_t mip = 0; mip < m_mipCount; ++mip)
                {
                    const std::uint32_t subresource = m_firstMip + mip +
                        (m_firstSlice + slice) * m_mipLevels + plane * m_planeStride;
                    // tracked-state metadata may be shorter than the resource range
                    if (subresource < m_trackedSubresources)
                        visitor(subresource);
                }
            }
        }
    }

private:
    std::uint32_t m_firstMip{}, m_mipCount{}, m_mipLevels{};
    std::uint32_t m_firstSlice{}, m_sliceCount{}, m_planeCount{}, m_planeStride{};
    std::uint32_t m_trackedSubresources{};
    std::size_t m_count{};
    bool m_coversAll{};
};
}
