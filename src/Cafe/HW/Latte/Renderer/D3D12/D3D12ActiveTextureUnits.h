#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>

namespace D3D12ActiveTextureUnits
{
template<std::size_t SlotCount>
class OrderedSet
{
public:
    void IncludeAll() noexcept
    {
        m_words.fill(~std::uint64_t{0});
        if constexpr (SlotCount % kWordBits != 0)
            m_words.back() = (std::uint64_t{1} << (SlotCount % kWordBits)) - 1;
    }

    template<std::size_t ListCapacity>
    bool AddStage(const std::uint8_t (&units)[ListCapacity], std::size_t count,
        std::size_t hostBase, std::size_t relativeUnitCount) noexcept
    {
        if (count > ListCapacity || hostBase > SlotCount ||
            relativeUnitCount > SlotCount - hostBase)
        {
            IncludeAll();
            return false;
        }
        for (std::size_t i = 0; i < count; ++i)
        {
            if (units[i] >= relativeUnitCount)
            {
                IncludeAll();
                return false;
            }
            const auto hostUnit = hostBase + units[i];
            m_words[hostUnit / kWordBits] |= std::uint64_t{1} << (hostUnit % kWordBits);
        }
        return true;
    }

    template<class Visitor>
    void ForEach(Visitor&& visitor) const
    {
        for (std::size_t wordIndex = 0; wordIndex < m_words.size(); ++wordIndex)
        {
            auto remaining = m_words[wordIndex];
            while (remaining != 0)
            {
                const auto bit = static_cast<std::size_t>(std::countr_zero(remaining));
                visitor(wordIndex * kWordBits + bit);
                remaining &= remaining - 1;
            }
        }
    }

private:
    static constexpr std::size_t kWordBits = 64;
    std::array<std::uint64_t, (SlotCount + kWordBits - 1) / kWordBits> m_words{};
};
}
