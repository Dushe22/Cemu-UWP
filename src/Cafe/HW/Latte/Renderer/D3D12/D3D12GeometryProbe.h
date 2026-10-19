#pragma once


#include "Cafe/HW/Latte/Renderer/D3D12/D3D12MemoryManager.h"

#include <d3d12.h>
#include <wrl/client.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <string>

class D3D12GeometryProbe final
{
public:
	D3D12GeometryProbe() = default;
	D3D12GeometryProbe(const D3D12GeometryProbe&) = delete;
	D3D12GeometryProbe& operator=(const D3D12GeometryProbe&) = delete;

	void Poll(ID3D12Device* device, D3D12MemoryManager* memoryManager, ID3D12Fence* fence);
	bool WantsSample(uint64_t pipeline, bool depthWrite) const;
	int Begin(ID3D12GraphicsCommandList* commandList, uint64_t pipeline, bool depthWrite,
		bool allowOcclusion, std::string metadata);
	void End(ID3D12GraphicsCommandList* commandList, int index, uint64_t readyFence);

private:
	static constexpr uint32_t kSamplesPerCategory = 8;
	static constexpr uint32_t kSampleBudget = 2 * kSamplesPerCategory;

	struct Sample
	{
		uint64_t pipeline{};
		uint64_t readyFence{};
		std::string metadata;
		bool depthWrite{};
		bool occlusionBegun{};
		bool begun{};
		bool ended{};
		bool logged{};
	};

	bool EnsureResources(ID3D12Device* device, D3D12MemoryManager* memoryManager);
	bool PriorQueriesComplete(uint64_t completedFence) const;
	bool PipelineAlreadySampled(uint64_t pipeline, bool depthWrite) const;
	void ProcessCompletedSamples(uint64_t completedFence);
	void ResetCapture();
	void LogResourceFailureOnce(const char* resource, HRESULT result);

	Microsoft::WRL::ComPtr<ID3D12QueryHeap> m_pipelineStatisticsHeap;
	Microsoft::WRL::ComPtr<ID3D12QueryHeap> m_occlusionHeap;
	D3D12MemoryManager::Resource m_readback;
	std::array<Sample, kSampleBudget> m_samples{};
	std::array<std::array<uint64_t, kSamplesPerCategory>, 2> m_sampledPipelines{};
	std::array<uint32_t, 2> m_sampledPipelineCounts{};
	uint32_t m_sampleCount{};
	std::string m_lastAttemptedNonce;
	std::chrono::steady_clock::time_point m_nextMarkerPoll{};
	bool m_hasAttemptedNonce{};
	bool m_resourcesReady{};
	bool m_armed{};
	bool m_deviceRemoved{};
	bool m_resourceFailureLogged{};
	bool m_mapFailureLogged{};
	bool m_readbackFailed{};
};

