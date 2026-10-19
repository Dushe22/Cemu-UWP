#include "Cafe/HW/Latte/Renderer/D3D12/D3D12GeometryProbe.h"


#include "Cemu/Logging/CemuLogging.h"
#include "config/ActiveSettings.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <limits>
#include <string_view>
#include <utility>

namespace
{
constexpr uint32_t kProbeSampleBudget = 16;
constexpr uint64_t kPipelineStatisticsSize = sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS);
constexpr uint64_t kOcclusionResultSize = sizeof(uint64_t);
constexpr uint64_t kOcclusionResultOffset = (kPipelineStatisticsSize + 7u) & ~uint64_t{ 7u };
constexpr uint64_t kResultStride = kOcclusionResultOffset + kOcclusionResultSize;
constexpr uint64_t kReadbackSize = kProbeSampleBudget * kResultStride;

static_assert((kPipelineStatisticsSize & 7u) == 0);
static_assert(kOcclusionResultOffset == kPipelineStatisticsSize);
static_assert(kResultStride == sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS) + sizeof(uint64_t));

std::string ReadMarkerToken()
{
	try
	{
		std::ifstream marker(
			ActiveSettings::GetUserDataPath("diagnostics/d3d12_geometry_probe.txt"),
			std::ios::binary);
		if (!marker)
			return {};

		std::array<char, 256> bytes{};
		marker.read(bytes.data(), bytes.size());
		std::string token(bytes.data(), static_cast<size_t>(marker.gcount()));
		const auto isWhitespace = [](char ch)
		{
			return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
		};
		while (!token.empty() && isWhitespace(token.back()))
			token.pop_back();
		size_t first = 0;
		while (first < token.size() && isWhitespace(token[first]))
			++first;
		if (first != 0)
			token.erase(0, first);
		return token;
	}
	catch (...)
	{
		return {};
	}
}

bool IsDisarmToken(std::string_view token)
{
	if (token.empty() || token == "0")
		return true;
	if (token.size() != 3)
		return false;
	return (token[0] == 'o' || token[0] == 'O') &&
		(token[1] == 'f' || token[1] == 'F') &&
		(token[2] == 'f' || token[2] == 'F');
}

uint64_t ResultBaseOffset(uint32_t index)
{
	return static_cast<uint64_t>(index) * kResultStride;
}
}

void D3D12GeometryProbe::Poll(ID3D12Device* device, D3D12MemoryManager* memoryManager,
	ID3D12Fence* fence)
{
	if (m_deviceRemoved || !fence)
		return;

	const uint64_t completedFence = fence->GetCompletedValue();
	if (completedFence == std::numeric_limits<uint64_t>::max())
	{
		// UINT64_MAX signals device removal; query storage must not be touched afterwards
		m_deviceRemoved = true;
		m_armed = false;
		return;
	}

	ProcessCompletedSamples(completedFence);

	const auto now = std::chrono::steady_clock::now();
	if (now < m_nextMarkerPoll)
		return;
	m_nextMarkerPoll = now + std::chrono::seconds(1);

	std::string token = ReadMarkerToken();
	if (IsDisarmToken(token))
	{
		m_armed = false;
		return;
	}
	if (m_hasAttemptedNonce && token == m_lastAttemptedNonce)
		return;

	if (!PriorQueriesComplete(completedFence) || !device || !memoryManager)
		return;

	m_lastAttemptedNonce = std::move(token);
	m_hasAttemptedNonce = true;
	ResetCapture();
	if (!EnsureResources(device, memoryManager))
	{
		m_armed = false;
		return;
	}
	m_armed = true;
}

bool D3D12GeometryProbe::WantsSample(uint64_t pipeline, bool depthWrite) const
{
	if (!m_armed || !m_resourcesReady || m_sampleCount >= kSampleBudget)
		return false;
	const uint32_t category = depthWrite ? 0u : 1u;
	return m_sampledPipelineCounts[category] < kSamplesPerCategory &&
		!PipelineAlreadySampled(pipeline, depthWrite);
}

int D3D12GeometryProbe::Begin(ID3D12GraphicsCommandList* commandList, uint64_t pipeline,
	bool depthWrite, bool allowOcclusion, std::string metadata)
{
	if (!commandList || !WantsSample(pipeline, depthWrite))
		return -1;

	const uint32_t index = m_sampleCount++;
	const uint32_t category = depthWrite ? 0u : 1u;
	m_sampledPipelines[category][m_sampledPipelineCounts[category]++] = pipeline;

	constexpr size_t kMetadataLimit = 16 * 1024;
	constexpr std::string_view kTruncated = " [metadata-truncated]";
	if (metadata.size() > kMetadataLimit)
	{
		metadata.resize(kMetadataLimit - kTruncated.size());
		metadata += kTruncated;
	}
	std::replace(metadata.begin(), metadata.end(), '\r', ' ');
	std::replace(metadata.begin(), metadata.end(), '\n', ' ');

	auto& sample = m_samples[index];
	sample.pipeline = pipeline;
	sample.depthWrite = depthWrite;
	sample.occlusionBegun = allowOcclusion;
	sample.metadata = std::move(metadata);
	sample.begun = true;

	commandList->BeginQuery(m_pipelineStatisticsHeap.Get(),
		D3D12_QUERY_TYPE_PIPELINE_STATISTICS, index);
	if (sample.occlusionBegun)
	{
		commandList->BeginQuery(m_occlusionHeap.Get(),
			D3D12_QUERY_TYPE_BINARY_OCCLUSION, index);
	}

	if (m_sampleCount >= kSampleBudget)
		m_armed = false;
	return static_cast<int>(index);
}

void D3D12GeometryProbe::End(ID3D12GraphicsCommandList* commandList, int index,
	uint64_t readyFence)
{
	if (!commandList || index < 0 || static_cast<uint32_t>(index) >= m_sampleCount)
		return;
	auto& sample = m_samples[static_cast<uint32_t>(index)];
	if (!sample.begun || sample.ended)
		return;

	if (sample.occlusionBegun)
	{
		commandList->EndQuery(m_occlusionHeap.Get(),
			D3D12_QUERY_TYPE_BINARY_OCCLUSION, static_cast<uint32_t>(index));
	}
	commandList->EndQuery(m_pipelineStatisticsHeap.Get(),
		D3D12_QUERY_TYPE_PIPELINE_STATISTICS, static_cast<uint32_t>(index));

	const uint64_t baseOffset = ResultBaseOffset(static_cast<uint32_t>(index));
	commandList->ResolveQueryData(m_pipelineStatisticsHeap.Get(),
		D3D12_QUERY_TYPE_PIPELINE_STATISTICS, static_cast<uint32_t>(index), 1,
		m_readback.resource.Get(), baseOffset);
	if (sample.occlusionBegun)
	{
		commandList->ResolveQueryData(m_occlusionHeap.Get(),
			D3D12_QUERY_TYPE_BINARY_OCCLUSION, static_cast<uint32_t>(index), 1,
			m_readback.resource.Get(), baseOffset + kOcclusionResultOffset);
	}

	sample.readyFence = readyFence;
	sample.ended = true;
}

bool D3D12GeometryProbe::EnsureResources(ID3D12Device* device,
	D3D12MemoryManager* memoryManager)
{
	static_assert(kSampleBudget == kProbeSampleBudget);
	if (m_resourcesReady)
		return true;

	m_pipelineStatisticsHeap.Reset();
	m_occlusionHeap.Reset();
	m_readback.Reset();

	try
	{
		D3D12_QUERY_HEAP_DESC pipelineHeapDesc{};
		pipelineHeapDesc.Type = D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS;
		pipelineHeapDesc.Count = kSampleBudget;
		const HRESULT pipelineResult = device->CreateQueryHeap(
			&pipelineHeapDesc, IID_PPV_ARGS(&m_pipelineStatisticsHeap));
		if (FAILED(pipelineResult))
		{
			LogResourceFailureOnce("pipeline-statistics query heap", pipelineResult);
			m_pipelineStatisticsHeap.Reset();
			return false;
		}

		D3D12_QUERY_HEAP_DESC occlusionHeapDesc{};
		occlusionHeapDesc.Type = D3D12_QUERY_HEAP_TYPE_OCCLUSION;
		occlusionHeapDesc.Count = kSampleBudget;
		const HRESULT occlusionResult = device->CreateQueryHeap(
			&occlusionHeapDesc, IID_PPV_ARGS(&m_occlusionHeap));
		if (FAILED(occlusionResult))
		{
			LogResourceFailureOnce("binary-occlusion query heap", occlusionResult);
			m_pipelineStatisticsHeap.Reset();
			m_occlusionHeap.Reset();
			return false;
		}

		if (!memoryManager->CreateBuffer(kReadbackSize, D3D12_HEAP_TYPE_READBACK,
			D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, m_readback))
		{
			LogResourceFailureOnce("query readback buffer", E_OUTOFMEMORY);
			m_pipelineStatisticsHeap.Reset();
			m_occlusionHeap.Reset();
			m_readback.Reset();
			return false;
		}
	}
	catch (...)
	{
		LogResourceFailureOnce("query resources", E_FAIL);
		m_pipelineStatisticsHeap.Reset();
		m_occlusionHeap.Reset();
		m_readback.Reset();
		return false;
	}

	m_resourcesReady = true;
	return true;
}

bool D3D12GeometryProbe::PriorQueriesComplete(uint64_t completedFence) const
{
	for (uint32_t index = 0; index < m_sampleCount; ++index)
	{
		const auto& sample = m_samples[index];
		if (sample.begun && (!sample.ended || sample.readyFence == 0 ||
			completedFence < sample.readyFence))
		{
			return false;
		}
	}
	return true;
}

bool D3D12GeometryProbe::PipelineAlreadySampled(uint64_t pipeline, bool depthWrite) const
{
	const uint32_t category = depthWrite ? 0u : 1u;
	for (uint32_t index = 0; index < m_sampledPipelineCounts[category]; ++index)
	{
		if (m_sampledPipelines[category][index] == pipeline)
			return true;
	}
	return false;
}

void D3D12GeometryProbe::ProcessCompletedSamples(uint64_t completedFence)
{
	if (!m_resourcesReady || !m_readback.resource || m_readbackFailed)
		return;

	uint64_t firstReadOffset = kReadbackSize;
	uint64_t lastReadOffset = 0;
	bool hasCompletedSample = false;
	for (uint32_t index = 0; index < m_sampleCount; ++index)
	{
		const auto& sample = m_samples[index];
		if (!sample.ended || sample.logged || sample.readyFence == 0 ||
			completedFence < sample.readyFence)
		{
			continue;
		}
		hasCompletedSample = true;
		const uint64_t baseOffset = ResultBaseOffset(index);
		firstReadOffset = std::min(firstReadOffset, baseOffset);
		lastReadOffset = std::max(lastReadOffset, baseOffset + kPipelineStatisticsSize +
			(sample.occlusionBegun ? kOcclusionResultSize : 0));
	}
	if (!hasCompletedSample)
		return;

	void* mapped = nullptr;
	const D3D12_RANGE readRange
	{
		static_cast<SIZE_T>(firstReadOffset),
		static_cast<SIZE_T>(lastReadOffset),
	};
	const HRESULT mapResult = m_readback.resource->Map(0, &readRange, &mapped);
	if (FAILED(mapResult) || !mapped)
	{
		m_armed = false;
		m_readbackFailed = true;
		if (!m_mapFailureLogged)
		{
			m_mapFailureLogged = true;
			cemuLog_log(LogType::Force,
				"D3D12 geometry probe query readback map failed with HRESULT 0x{:08x}; capture disarmed.",
				static_cast<uint32_t>(mapResult));
		}
		return;
	}

	const auto* bytes = static_cast<const uint8_t*>(mapped);
	for (uint32_t index = 0; index < m_sampleCount; ++index)
	{
		auto& sample = m_samples[index];
		if (!sample.ended || sample.logged || sample.readyFence == 0 ||
			completedFence < sample.readyFence)
		{
			continue;
		}

		const uint64_t baseOffset = ResultBaseOffset(index);
		D3D12_QUERY_DATA_PIPELINE_STATISTICS statistics{};
		std::memcpy(&statistics, bytes + baseOffset, sizeof(statistics));
		uint64_t binaryOcclusion = 0;
		if (sample.occlusionBegun)
		{
			std::memcpy(&binaryOcclusion, bytes + baseOffset + kOcclusionResultOffset,
				sizeof(binaryOcclusion));
		}

		cemuLog_log(LogType::Force,
			"D3D12 geometry probe: sample={} category={} pipeline=0x{:016x} "
			"ia_vertices={} ia_primitives={} vs_invocations={} gs_invocations={} gs_primitives={} "
			"clipper_invocations={} clipper_primitives={} ps_invocations={} hs_invocations={} "
			"ds_invocations={} cs_invocations={} binary_occlusion_sampled={} binary_occlusion={} metadata={}",
			index, sample.depthWrite ? "depth_write" : "readonly_color",
			static_cast<unsigned long long>(sample.pipeline),
			static_cast<unsigned long long>(statistics.IAVertices),
			static_cast<unsigned long long>(statistics.IAPrimitives),
			static_cast<unsigned long long>(statistics.VSInvocations),
			static_cast<unsigned long long>(statistics.GSInvocations),
			static_cast<unsigned long long>(statistics.GSPrimitives),
			static_cast<unsigned long long>(statistics.CInvocations),
			static_cast<unsigned long long>(statistics.CPrimitives),
			static_cast<unsigned long long>(statistics.PSInvocations),
			static_cast<unsigned long long>(statistics.HSInvocations),
			static_cast<unsigned long long>(statistics.DSInvocations),
			static_cast<unsigned long long>(statistics.CSInvocations),
			sample.occlusionBegun ? 1 : 0,
			static_cast<unsigned long long>(binaryOcclusion), sample.metadata);
		sample.logged = true;
		sample.metadata.clear();
	}

	const D3D12_RANGE writtenRange{ 0, 0 };
	m_readback.resource->Unmap(0, &writtenRange);
}

void D3D12GeometryProbe::ResetCapture()
{
	for (auto& sample : m_samples)
		sample = {};
	for (auto& pipelines : m_sampledPipelines)
		pipelines.fill(0);
	m_sampledPipelineCounts.fill(0);
	m_sampleCount = 0;
	m_mapFailureLogged = false;
	m_readbackFailed = false;
}

void D3D12GeometryProbe::LogResourceFailureOnce(const char* resource, HRESULT result)
{
	if (m_resourceFailureLogged)
		return;
	m_resourceFailureLogged = true;
	cemuLog_log(LogType::Force,
		"D3D12 geometry probe failed to create {} (HRESULT 0x{:08x}); capture disarmed.",
		resource, static_cast<uint32_t>(result));
}

