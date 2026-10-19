#pragma once

#include <cstdint>
#include <limits>

namespace D3D12LogicOpCopyRegion
{
enum class Kind { Full, Partial, Empty };

// D3D12_RECT uses signed 32-bit LONG coordinates even on 64-bit Windows
struct Scissor
{
    std::int32_t left{}, top{}, right{}, bottom{};
};

struct Region
{
    Kind kind{ Kind::Full };
    std::uint32_t left{}, top{}, right{}, bottom{};
};

// bounded copies are only valid for single-sample color textures
inline Region Select(bool boundedCopyAllowed, Scissor scissor,
    std::uint64_t sourceMipWidth, std::uint64_t sourceMipHeight,
    std::uint64_t scratchWidth, std::uint64_t scratchHeight) noexcept
{
    constexpr auto maxCoordinate = std::numeric_limits<std::uint32_t>::max();
    if (!boundedCopyAllowed || sourceMipWidth == 0 || sourceMipHeight == 0 ||
        sourceMipWidth > maxCoordinate || sourceMipHeight > maxCoordinate ||
        sourceMipWidth != scratchWidth || sourceMipHeight != scratchHeight ||
        scissor.left > scissor.right || scissor.top > scissor.bottom)
        return {};

    const auto width = static_cast<std::uint32_t>(sourceMipWidth);
    const auto height = static_cast<std::uint32_t>(sourceMipHeight);
    const auto clamp = [](std::int32_t coordinate, std::uint32_t limit) noexcept
    {
        if (coordinate <= 0) return std::uint32_t{0};
        const auto positive = static_cast<std::uint32_t>(coordinate);
        return positive < limit ? positive : limit;
    };

    Region region;
    region.left = clamp(scissor.left, width);
    region.top = clamp(scissor.top, height);
    region.right = clamp(scissor.right, width);
    region.bottom = clamp(scissor.bottom, height);
    if (region.left == region.right || region.top == region.bottom)
        region.kind = Kind::Empty;
    else if (region.left == 0 && region.top == 0 && region.right == width && region.bottom == height)
        region.kind = Kind::Full;
    else
        region.kind = Kind::Partial;
    // same offset on both sides; shaders read absolute SV_Position.xy
    return region;
}
}
