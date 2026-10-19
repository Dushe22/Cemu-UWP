#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace D3D12SamplerTableState
{
// flags are mixed in so toggling an override stays distinct from its value
template<typename Texture, typename Mixer>
std::uint64_t MixTextureOverrides(std::uint64_t key, const Texture* texture, Mixer mix)
{
    if (!texture)
        return key;
    key = mix(key, texture->overwriteInfo.hasLodBias ? 1 : 0);
    key = mix(key, static_cast<std::uint16_t>(texture->overwriteInfo.lodBias));
    key = mix(key, texture->overwriteInfo.hasRelativeLodBias ? 1 : 0);
    key = mix(key, static_cast<std::uint16_t>(texture->overwriteInfo.relativeLodBias));
    key = mix(key, static_cast<std::uint8_t>(texture->overwriteInfo.anisotropicLevel));
    return key;
}

template<typename Description, std::size_t Capacity>
class FinalSlots
{
public:
    FinalSlots(std::size_t count, const Description& defaultDescription)
        : m_count(std::min(count, Capacity))
    {
        std::fill_n(m_descriptions.begin(), m_count, defaultDescription);
    }

    void Set(std::size_t index, const Description& description)
    {
        if (index < m_count)
            m_descriptions[index] = description;
    }

    template<typename Visitor>
    void ForEach(Visitor visitor) const
    {
        for (std::size_t i = 0; i < m_count; ++i)
            visitor(i, m_descriptions[i]);
    }

private:
    std::array<Description, Capacity> m_descriptions;
    std::size_t m_count;
};
}
