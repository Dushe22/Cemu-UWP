#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace D3D12PerformanceExperiment
{
#ifndef CEMU_UWP_D3D12_CPU_OPTIMIZATION_MASK
#define CEMU_UWP_D3D12_CPU_OPTIMIZATION_MASK 0
#endif
constexpr std::uint32_t kCpuFeatures = 7;
constexpr std::uint32_t kDescriptorFeatures = 16 | 32;
constexpr std::uint32_t kLogicCopyFeatures = 64 | 128;
constexpr std::uint32_t kAllowedFeatures = kCpuFeatures | kDescriptorFeatures | kLogicCopyFeatures;
constexpr std::uint32_t kDefaultMask = CEMU_UWP_D3D12_CPU_OPTIMIZATION_MASK & kAllowedFeatures;
enum Feature : std::uint32_t
{
    StageFeedbackCache = 1,
    SatisfiedViewTransition = 2,
    RetainedFeedbackTransitions = 4,
    ActiveFeedbackUnits = 16,
    ViewStateRanges = 32,
    LogicCopyScissor = 64,
    UniformOrBatch = 128,
};
constexpr std::uint32_t kAllFeatures = kCpuFeatures;
constexpr bool Enabled(std::uint32_t mask, Feature feature) noexcept { return (mask & feature) != 0; }
enum class Suite { Cpu, Boundary, Descriptor, LogicCopy };
constexpr bool kBoundarySuiteAvailable = false;

struct Config
{
    std::uint32_t mask{};
    bool automatic{};
    Suite suite{ Suite::Cpu };
};

inline bool ParseConfig(std::string_view text, Config& output) noexcept
{
    Config result{};
    while (!text.empty())
    {
        const auto newline = text.find('\n');
        auto line = text.substr(0, newline);
        if (newline == std::string_view::npos)
            text = {};
        else
            text.remove_prefix(newline + 1);
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) line.remove_prefix(1);
        while (!line.empty() && (line.back() == ' ' || line.back() == '\t' || line.back() == '\r')) line.remove_suffix(1);
        if (line.empty() || line.front() == '#') continue;
        if (line == "auto=0") result.automatic = false;
        else if (line == "auto=1") result.automatic = true;
        else if (line == "suite=cpu") result.suite = Suite::Cpu;
        else if (line == "suite=boundary" && kBoundarySuiteAvailable) result.suite = Suite::Boundary;
        else if (line == "suite=descriptor") result.suite = Suite::Descriptor;
        else if (line == "suite=logic-copy") result.suite = Suite::LogicCopy;
        else if (line.size() == 6 && line.substr(0, 5) == "mask=" && line[5] >= '0' && line[5] <= '7')
            result.mask = static_cast<std::uint32_t>(line[5] - '0');
        else if (line == "mask=16") result.mask = 16;
        else if (line == "mask=32") result.mask = 32;
        else if (line == "mask=48") result.mask = 48;
        else if (line == "mask=64") result.mask = 64;
        else if (line == "mask=128") result.mask = 128;
        else if (line == "mask=192") result.mask = 192;
        else
            return false;
    }
    if (result.suite == Suite::LogicCopy)
    {
        if ((result.mask & ~kLogicCopyFeatures) != 0) return false;
    }
    else if (result.suite == Suite::Descriptor)
    {
        if ((result.mask & ~kDescriptorFeatures) != 0) return false;
    }
    else if ((result.mask & ~kCpuFeatures) != 0)
        return false;
    output = result;
    return true;
}

template<std::size_t SlotCount, std::size_t ListCapacity>
bool HasStageFeedback(const std::array<std::uint32_t, SlotCount>& scratch,
    std::uint32_t invalidIndex, const std::uint8_t (&units)[ListCapacity],
    std::size_t count, std::size_t hostBase, std::size_t relativeCount, bool validStage) noexcept
{
    const auto anyFeedback = [&]()
    {
        return std::any_of(scratch.begin(), scratch.end(),
            [invalidIndex](std::uint32_t index) { return index != invalidIndex; });
    };
    if (!validStage || count > ListCapacity || hostBase > SlotCount || relativeCount > SlotCount - hostBase)
        return anyFeedback();
    // validate the whole list before using it, even if an earlier unit hits
    for (std::size_t i = 0; i < count; ++i)
        if (units[i] >= relativeCount) return anyFeedback();
    for (std::size_t i = 0; i < count; ++i)
        if (scratch[hostBase + units[i]] != invalidIndex) return true;
    return false;
}

template<class Vector>
Vector& SelectTransitionStorage(bool retain, Vector& local, Vector& retained)
{
    auto& selected = retain ? retained : local;
    selected.clear();
    return selected;
}

class Schedule
{
public:
    static constexpr std::uint32_t kStableFrames = 120;
    static constexpr std::uint32_t kSettleFrames = 30;
    static constexpr std::uint32_t kWindowFrames = 60;
    static constexpr std::uint32_t kMeasureFrames = 180;
    static constexpr std::array<std::uint32_t, 16> kMasks{ 0,1,0,2,0,4,0,7,0,7,0,4,0,2,0,1 };
    static constexpr std::array<std::uint32_t, 8> kBoundaryMasks{ 0,8,0,8,0,8,0,8 };
    static constexpr std::array<std::uint32_t, 12> kDescriptorMasks{ 0,16,0,32,0,48,0,48,0,32,0,16 };
    static constexpr std::array<std::uint32_t, 4> kLogicCopyMasks{ 0,128,0,128 };
    enum class State { Waiting, Settling, Measuring, Complete, Fixed };
    struct Event
    {
        bool beginSettle{}, beginMeasure{}, window{}, end{}, complete{};
        std::uint32_t completedPhase{}, completedMask{}, windowIndex{};
    };

    void Configure(Config config) noexcept
    {
        *this = Schedule{};
        m_suite = config.suite == Suite::LogicCopy ? Suite::LogicCopy :
            (config.suite == Suite::Descriptor ? Suite::Descriptor :
            (kBoundarySuiteAvailable && config.suite == Suite::Boundary ? Suite::Boundary : Suite::Cpu));
        m_state = config.automatic ? State::Waiting : State::Fixed;
        m_mask = config.automatic || IsBoundarySuite() ? 0 :
            (config.mask & (IsLogicCopySuite() ? kLogicCopyFeatures : (IsDescriptorSuite() ? kDescriptorFeatures : kCpuFeatures)));
    }
    static bool EligibleFrame(std::uint64_t draws, bool createdPso) noexcept
    {
        return draws > 1000 && !createdPso;
    }
    std::uint64_t MinimumDrawsExclusive() const noexcept { return IsLogicCopySuite() ? 100u : 1000u; }
    bool EligibleForSuite(std::uint64_t draws, bool createdPso, std::uint64_t logicCopies = 0) const noexcept
    {
        return IsLogicCopySuite() ? (draws > MinimumDrawsExclusive() && !createdPso && logicCopies != 0) :
            EligibleFrame(draws, createdPso);
    }
    Event OnFrame(std::uint64_t draws, bool createdPso, std::uint64_t logicCopies = 0) noexcept
    {
        Event event{};
        if (m_state == State::Fixed || m_state == State::Complete) return event;
        if (m_state == State::Waiting)
        {
            m_stableFrames = EligibleForSuite(draws, createdPso, logicCopies) ? m_stableFrames + 1 : 0;
            if (m_stableFrames == kStableFrames)
            {
                m_state = State::Settling;
                m_mask = PhaseMask(0);
                event.beginSettle = true;
            }
            return event;
        }
        ++m_frames;
        if (m_state == State::Settling)
        {
            if (m_frames == kSettleFrames)
            {
                m_frames = 0;
                m_state = State::Measuring;
                event.beginMeasure = true;
            }
            return event;
        }
        if (m_frames % kWindowFrames == 0)
        {
            event.window = true;
            event.completedPhase = Phase();
            event.completedMask = m_mask;
            event.windowIndex = m_frames / kWindowFrames;
        }
        if (m_frames == kMeasureFrames)
        {
            event.end = true;
            m_frames = 0;
            if (++m_phaseIndex == PhaseCount())
            {
                m_state = State::Complete;
                m_mask = 0;
                event.complete = true;
            }
            else
            {
                m_state = State::Settling;
                m_mask = PhaseMask(m_phaseIndex);
                event.beginSettle = true;
            }
        }
        return event;
    }
    std::uint32_t Mask() const noexcept { return m_mask; }
    std::uint32_t CpuMask() const noexcept
    {
        return IsLogicCopySuite() ? (kDefaultMask & ~kLogicCopyFeatures) :
            (IsDescriptorSuite() ? (kDefaultMask & kCpuFeatures) :
            (IsBoundarySuite() ? kDefaultMask : (m_mask & kCpuFeatures)));
    }
    std::uint32_t FeatureMask() const noexcept
    {
        return CpuMask() | (IsLogicCopySuite() ? (m_mask & kLogicCopyFeatures) :
            (IsDescriptorSuite() ? (m_mask & kDescriptorFeatures) : 0u));
    }
    bool IsBoundarySuite() const noexcept { return m_suite == Suite::Boundary; }
    bool IsDescriptorSuite() const noexcept { return m_suite == Suite::Descriptor; }
    bool IsLogicCopySuite() const noexcept { return m_suite == Suite::LogicCopy; }
    const char* SuiteName() const noexcept { return IsLogicCopySuite() ? "logic-copy" : (IsDescriptorSuite() ? "descriptor" : (IsBoundarySuite() ? "boundary" : "cpu")); }
    std::uint32_t PhaseCount() const noexcept
    {
        return static_cast<std::uint32_t>(IsLogicCopySuite() ? kLogicCopyMasks.size() :
            (IsDescriptorSuite() ? kDescriptorMasks.size() : (IsBoundarySuite() ? kBoundaryMasks.size() : kMasks.size())));
    }
    std::uint32_t Phase() const noexcept { return m_phaseIndex < PhaseCount() ? m_phaseIndex + 1 : 0; }
    State GetState() const noexcept { return m_state; }
    bool Measuring() const noexcept { return m_state == State::Measuring; }
    std::uint32_t StableFrames() const noexcept { return m_stableFrames; }

private:
    std::uint32_t PhaseMask(std::size_t index) const noexcept
    {
        return IsLogicCopySuite() ? kLogicCopyMasks[index] :
            (IsDescriptorSuite() ? kDescriptorMasks[index] : (IsBoundarySuite() ? kBoundaryMasks[index] : kMasks[index]));
    }
    Suite m_suite{ Suite::Cpu };
    State m_state{ State::Fixed };
    std::uint32_t m_mask{}, m_phaseIndex{}, m_frames{}, m_stableFrames{};
};
}
