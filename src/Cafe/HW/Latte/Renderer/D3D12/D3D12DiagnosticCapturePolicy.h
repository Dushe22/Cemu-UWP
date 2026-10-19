#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

// admission policy only; the caller owns synchronization
namespace D3D12DiagnosticCapturePolicy
{
inline constexpr std::size_t kMaxAddresses = 16;
inline constexpr std::uint32_t kSamplesPerAddress = 8;
inline constexpr std::uint32_t kMaxSamples = 64;

inline constexpr bool IsCompletedCaptureEvent(std::string_view event) noexcept
{
    return event == "fbo_attachment_unbind" ||
        event == "output_source_before" ||
        event == "surface_copy_dst_after" ||
        event == "texture_copy_dst_after" ||
        event == "texture_copy_raw_alias_dst_after";
}

namespace Detail
{
inline constexpr bool HasPrefix(std::string_view value, std::string_view prefix) noexcept
{
    return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

inline constexpr bool HasToken(std::string_view value, std::string_view token) noexcept
{
    for (std::size_t start = 0; start < value.size();)
    {
        const std::size_t separator = value.find('_', start);
        const std::size_t end = separator == std::string_view::npos ? value.size() : separator;
        if (value.substr(start, end - start) == token)
            return true;
        start = end + 1;
    }
    return false;
}

inline constexpr bool HasEventPrefix(std::string_view event, std::string_view prefix) noexcept
{
    return event == prefix ||
        (event.size() > prefix.size() && event[prefix.size()] == '_' && HasPrefix(event, prefix));
}
}

// token matching avoids false positives such as "devicebuffer" or "notfailed"
inline constexpr bool ShouldRetainWhileDisarmed(std::string_view event) noexcept
{
    return Detail::HasEventPrefix(event, "renderer_initialize") ||
        Detail::HasEventPrefix(event, "renderer_shutdown") ||
        Detail::HasEventPrefix(event, "renderer_title_shutdown") ||
        Detail::HasPrefix(event, "gpu_capture_") ||
        event == "gpu_stats_capture_begin" || event == "gpu_stats_capture_end" ||
        Detail::HasToken(event, "failed") || Detail::HasToken(event, "failure") ||
        Detail::HasToken(event, "device");
}

enum class Decision
{
    Allow,
    Disarmed,
    IneligibleEvent,
    AddressNotConfigured,
    GlobalQuotaExhausted,
    AddressQuotaExhausted,
};

inline constexpr const char* DecisionName(Decision decision) noexcept
{
    switch (decision)
    {
    case Decision::Allow: return "capture_primary_address";
    case Decision::Disarmed: return "gpu_capture_disarmed";
    case Decision::IneligibleEvent: return "gpu_capture_event_metadata_only";
    case Decision::AddressNotConfigured: return "gpu_capture_primary_address_not_configured";
    case Decision::GlobalQuotaExhausted: return "gpu_capture_global_quota_exhausted";
    case Decision::AddressQuotaExhausted: return "gpu_capture_address_quota_exhausted";
    }
    return "gpu_capture_unknown_decision";
}

class CaptureBudget
{
public:
    bool Rearm(const std::uint32_t* addresses, std::size_t count) noexcept
    {
        *this = CaptureBudget{};
        if (!addresses || count == 0 || count > kMaxAddresses)
            return false;
        for (std::size_t i = 0; i < count; ++i)
        {
            if (addresses[i] == 0)
                return false;
        }
        for (std::size_t i = 0; i < count; ++i)
        {
            if (FindAddress(addresses[i]) == kMaxAddresses)
                m_addresses[m_addressCount++] = addresses[i];
        }
        m_armed = true;
        return true;
    }

    void Disarm() noexcept { m_armed = false; }
    bool IsArmed() const noexcept { return m_armed; }
    std::size_t AddressCount() const noexcept { return m_addressCount; }
    std::uint32_t TotalSamples() const noexcept { return m_totalSamples; }
    std::uint32_t AddressAt(std::size_t index) const noexcept
    {
        return index < m_addressCount ? m_addresses[index] : 0;
    }
    std::uint32_t AddressSamples(std::uint32_t address) const noexcept
    {
        const std::size_t index = FindAddress(address);
        return index == kMaxAddresses ? 0 : m_samples[index];
    }
    bool GlobalQuotaExhausted() const noexcept { return m_totalSamples >= kMaxSamples; }
    bool AddressQuotaExhausted(std::uint32_t address) const noexcept
    {
        const std::size_t index = FindAddress(address);
        return index != kMaxAddresses && m_samples[index] >= kSamplesPerAddress;
    }
    bool IsExhausted() const noexcept
    {
        if (GlobalQuotaExhausted())
            return true;
        if (m_addressCount == 0)
            return false;
        for (std::size_t i = 0; i < m_addressCount; ++i)
        {
            if (m_samples[i] < kSamplesPerAddress)
                return false;
        }
        return true;
    }

    // primaryAddress is the snapshotted texture, never parsed from payload text
    Decision Check(std::string_view event, std::uint32_t primaryAddress) const noexcept
    {
        if (!m_armed)
            return Decision::Disarmed;
        if (!IsCompletedCaptureEvent(event))
            return Decision::IneligibleEvent;
        const std::size_t index = FindAddress(primaryAddress);
        if (index == kMaxAddresses)
            return Decision::AddressNotConfigured;
        if (GlobalQuotaExhausted())
            return Decision::GlobalQuotaExhausted;
        if (m_samples[index] >= kSamplesPerAddress)
            return Decision::AddressQuotaExhausted;
        return Decision::Allow;
    }

    // charged before the readback so a failing readback cannot retry unbounded
    Decision TryAdmit(std::string_view event, std::uint32_t primaryAddress) noexcept
    {
        const Decision decision = Check(event, primaryAddress);
        if (decision == Decision::Allow)
        {
            ++m_samples[FindAddress(primaryAddress)];
            ++m_totalSamples;
        }
        // exhausting one address does not disarm; the caller stops on IsExhausted()
        return decision;
    }

private:
    std::size_t FindAddress(std::uint32_t address) const noexcept
    {
        for (std::size_t i = 0; i < m_addressCount; ++i)
        {
            if (m_addresses[i] == address)
                return i;
        }
        return kMaxAddresses;
    }

    std::array<std::uint32_t, kMaxAddresses> m_addresses{};
    std::array<std::uint32_t, kMaxAddresses> m_samples{};
    std::size_t m_addressCount{};
    std::uint32_t m_totalSamples{};
    bool m_armed{};
};
}
