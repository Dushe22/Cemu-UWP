#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace D3D12TextureUnitMask
{
template<std::size_t SlotCount, std::size_t ListCapacity>
bool AddStage(std::array<bool, SlotCount>& mask,
    const std::uint8_t (&units)[ListCapacity], std::size_t count,
    std::size_t hostBase, std::size_t relativeUnitCount) noexcept
{
    if (count > ListCapacity || hostBase > SlotCount ||
        relativeUnitCount > SlotCount - hostBase)
    {
        mask.fill(true);
        return false;
    }

    for (std::size_t i = 0; i < count; ++i)
    {
        if (units[i] >= relativeUnitCount)
        {
            mask.fill(true);
            return false;
        }
        mask[hostBase + units[i]] = true;
    }
    return true;
}
}
