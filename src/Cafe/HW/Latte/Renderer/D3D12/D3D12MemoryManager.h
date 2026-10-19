#pragma once

#if BOOST_OS_WINDOWS
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#define D3D12MA_D3D12_HEADERS_ALREADY_INCLUDED
#include <D3D12MemAlloc.h>
#endif

#include <cstdint>
#include <memory>
#include <queue>
#include <utility>
#include <vector>

class D3D12MemoryManager
{
public:
	struct Resource
	{
		Microsoft::WRL::ComPtr<ID3D12Resource> resource;
		Microsoft::WRL::ComPtr<D3D12MA::Allocation> allocation;
		uint64_t size{};
		D3D12_RESOURCE_STATES state{};

		Resource() = default;
		Resource(const Resource&) = delete;
		Resource& operator=(const Resource&) = delete;

		Resource(Resource&& other) noexcept
		{
			resource.Attach(other.resource.Detach());
			allocation.Attach(other.allocation.Detach());
			size = std::exchange(other.size, 0);
			state = std::exchange(other.state, D3D12_RESOURCE_STATE_COMMON);
		}

		Resource& operator=(Resource&& other) noexcept
		{
			if (this == &other)
				return *this;
			Reset();
			resource.Attach(other.resource.Detach());
			allocation.Attach(other.allocation.Detach());
			size = std::exchange(other.size, 0);
			state = std::exchange(other.state, D3D12_RESOURCE_STATE_COMMON);
			return *this;
		}

		~Resource()
		{
			Reset();
		}

		void Reset() noexcept
		{
			// D3D12MA requires the resource to be released before its allocation
			resource.Reset();
			allocation.Reset();
			size = 0;
			state = D3D12_RESOURCE_STATE_COMMON;
		}

		bool IsValid() const { return resource != nullptr; }
	};

	struct RingReservation
	{
		ID3D12Resource* resource{};
		uint8_t* cpuPtr{};
		uint64_t offset{};
		uint64_t size{};
		uint32_t bufferIndex{};

		bool IsValid() const { return resource != nullptr && cpuPtr != nullptr && size != 0; }
	};

	struct RingStats
	{
		uint32_t buffers{};
		uint64_t totalBytes{};
		uint64_t usedBytes{};
		uint32_t peakBuffers{};
		uint64_t peakBytes{};
		uint64_t reclaimedBuffers{};
	};

	struct Stats
	{
		RingStats upload;
		RingStats readback;
		uint64_t createdDefaultResources{};
		uint64_t createdUploadResources{};
		uint64_t createdReadbackResources{};
		uint64_t bufferAllocationRetries{};
		uint64_t textureUploadScratchBytes{};
		uint64_t localUsageBytes{};
		uint64_t localBudgetBytes{};
		uint64_t allocatorBlockBytes{};
		uint64_t allocatorAllocationBytes{};
	};

	D3D12MemoryManager();
	D3D12MemoryManager(const D3D12MemoryManager&) = delete;
	D3D12MemoryManager& operator=(const D3D12MemoryManager&) = delete;
	~D3D12MemoryManager();

	void Initialize(ID3D12Device* device, IDXGIAdapter* adapter);
	void Shutdown();

	bool CreateBuffer(uint64_t size, D3D12_HEAP_TYPE heapType, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES initialState, Resource& outResource);
	bool CreateTexture(const D3D12_RESOURCE_DESC& desc, D3D12_RESOURCE_STATES initialState, const D3D12_CLEAR_VALUE* clearValue, Resource& outResource);
	// committed texture in a CPU-writable heap, filled with WriteToSubresource
	bool CreateCpuWritableTexture(const D3D12_RESOURCE_DESC& desc, Resource& outResource);
	RingReservation AllocateUpload(uint32_t size, uint32_t alignment, uint64_t fenceValue);
	RingReservation AllocateReadback(uint32_t size, uint32_t alignment, uint64_t fenceValue);
	void Cleanup(uint64_t completedFence);

	void* AcquireTextureUploadBuffer(uint32_t size);
	void ReleaseTextureUploadBuffer(uint8_t* mem);
	uint64_t TrimTextureUploadBuffer();

	Stats GetStats() const;

private:
	class SynchronizedRingAllocator;

	Microsoft::WRL::ComPtr<ID3D12Device> m_device;
	Microsoft::WRL::ComPtr<D3D12MA::Allocator> m_allocator;
	std::unique_ptr<SynchronizedRingAllocator> m_uploadRing;
	std::unique_ptr<SynchronizedRingAllocator> m_readbackRing;
	std::vector<uint8_t> m_textureUploadBuffer;
	uint64_t m_createdDefaultResources{};
	uint64_t m_createdUploadResources{};
	uint64_t m_createdReadbackResources{};
	uint64_t m_bufferAllocationRetries{};
};
