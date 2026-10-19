#pragma once

#include <cstdint>

namespace LattePrimitiveRestartPolicy
{
template<typename Primitive>
constexpr bool IsNativeStrip(Primitive primitive) noexcept
{
    return primitive == Primitive::LINE_STRIP ||
        primitive == Primitive::LINE_STRIP_ADJACENT ||
        primitive == Primitive::TRIANGLE_STRIP ||
        primitive == Primitive::TRIANGLE_STRIP_ADJACENT;
}

// GX2 defaults set restart index without RESET_EN; native strips still cut on it
constexpr bool IsEnabled(bool uwpD3D12, bool nativeStrip, bool guestEnabled,
    unsigned indexBits, std::uint32_t restartIndex) noexcept
{
    const bool fixedMarker = (indexBits == 16 && restartIndex == 0xffffu) ||
        (indexBits == 32 && restartIndex == 0xffffffffu);
    return guestEnabled || (uwpD3D12 && nativeStrip && fixedMarker);
}
}
