#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>

namespace D3D12DiagnosticDrawContextPolicy
{
inline constexpr std::uint32_t kMaxDraws = 64;
inline constexpr std::uint32_t kDrawsPerPipeline = 2;
inline constexpr std::uint32_t kEventsPerDraw = 128;
inline constexpr std::uint32_t kMaxUniformSourceBytes = 16384;

inline constexpr bool HasPrefix(std::string_view text, std::string_view prefix) noexcept
{
    return text.size() >= prefix.size() && text.substr(0, prefix.size()) == prefix;
}
inline constexpr bool IsContextEvent(std::string_view event) noexcept
{
    return HasPrefix(event, "draw_") || HasPrefix(event, "uniform_") || HasPrefix(event, "game_pipeline_");
}
inline constexpr bool HasDrawField(std::string_view payload) noexcept
{
    return HasPrefix(payload, "draw=") || payload.find(" draw=") != std::string_view::npos;
}
// exact field match, never a substring such as lineageDraw
inline constexpr bool FieldEquals(std::string_view payload, std::string_view key, std::uint64_t expected) noexcept
{
    for (std::size_t pos = 0; pos < payload.size();)
    {
        const auto end = payload.find(' ', pos);
        const auto token = payload.substr(pos, end == std::string_view::npos ? payload.size() - pos : end - pos);
        if (token.size() > key.size() + 1 && token.substr(0, key.size()) == key && token[key.size()] == '=')
        {
            auto value = token.substr(key.size() + 1);
            unsigned base = 10;
            if (HasPrefix(value, "0x")) { base = 16; value.remove_prefix(2); }
            if (value.empty()) return false;
            std::uint64_t number = 0;
            for (char c : value)
            {
                const unsigned digit = c >= '0' && c <= '9' ? unsigned(c - '0') :
                    c >= 'a' && c <= 'f' ? unsigned(c - 'a' + 10) :
                    c >= 'A' && c <= 'F' ? unsigned(c - 'A' + 10) : 255;
                if (digit >= base || number > (std::numeric_limits<std::uint64_t>::max() - digit) / base) return false;
                number = number * base + digit;
            }
            return number == expected;
        }
        if (end == std::string_view::npos) break;
        pos = end + 1;
    }
    return false;
}
inline constexpr bool Retains(std::string_view event, std::string_view payload,
    std::uint64_t selectedDraw, std::uint64_t selectedPipeline) noexcept
{
    if (selectedDraw == 0 || selectedPipeline == 0 || !IsContextEvent(event)) return false;
    if (HasDrawField(payload)) return FieldEquals(payload, "draw", selectedDraw);
    return HasPrefix(event, "game_pipeline_") &&
        (FieldEquals(payload, "stateHash", selectedPipeline) || FieldEquals(payload, "pipeline", selectedPipeline));
}
class Budget
{
public:
    void Rearm() noexcept { *this = Budget{}; m_armed = true; }
    void Disarm() noexcept { m_armed = false; }
    bool IsArmed() const noexcept { return m_armed; }
    std::uint32_t DrawCount() const noexcept { return m_drawCount; }
    bool TrySelect(std::uint64_t draw, std::uint64_t pipeline, bool stencilEligible) noexcept
    {
        if (!m_armed || !stencilEligible || draw == 0 || pipeline == 0 || m_drawCount >= kMaxDraws) return false;
        std::size_t index = 0;
        for (; index < m_pipelineCount; ++index) if (m_pipelines[index] == pipeline) break;
        if (index == m_pipelineCount)
        {
            if (m_pipelineCount == m_pipelines.size()) return false;
            m_pipelines[m_pipelineCount++] = pipeline;
        }
        if (m_counts[index] >= kDrawsPerPipeline) return false;
        ++m_counts[index]; ++m_drawCount; return true;
    }
private:
    std::array<std::uint64_t, kMaxDraws> m_pipelines{};
    std::array<std::uint32_t, kMaxDraws> m_counts{};
    std::size_t m_pipelineCount{};
    std::uint32_t m_drawCount{};
    bool m_armed{};
};
}
