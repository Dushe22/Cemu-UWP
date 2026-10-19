#pragma once

#if BOOST_OS_WINDOWS
#include <d3d12.h>
#include <wrl/client.h>
#endif

#include <array>
#include <cstdint>
#include <vector>

class D3D12DescriptorManager
{
public:
	struct Allocation
	{
		D3D12_DESCRIPTOR_HEAP_TYPE type{};
		D3D12_CPU_DESCRIPTOR_HANDLE cpu{};
		D3D12_GPU_DESCRIPTOR_HANDLE gpu{};
		uint32_t index{};
		uint32_t count{};
		bool shaderVisible{};

		bool IsValid() const { return count != 0; }
	};

	struct Stats
	{
		uint32_t persistentCbvSrvUavUsed{};
		uint32_t persistentSamplerUsed{};
		uint32_t persistentRtvUsed{};
		uint32_t persistentDsvUsed{};
		uint32_t shaderVisibleCbvSrvUavUsed{};
		uint32_t shaderVisibleSamplerUsed{};
		uint32_t shaderVisibleCbvSrvUavCapacity{};
		uint32_t shaderVisibleSamplerCapacity{};
		uint32_t shaderVisibleRollovers{};
		uint32_t shaderVisibleCbvSrvUavRollovers{};
		uint32_t shaderVisibleSamplerRollovers{};
		uint64_t shaderVisibleCbvSrvUavHeapCreates{};
		uint64_t shaderVisibleCbvSrvUavHeapCreateFailures{};
		uint64_t shaderVisibleCbvSrvUavHeapCreateTimeNs{};
		uint64_t shaderVisibleCbvSrvUavHeapReuses{};
		uint64_t shaderVisibleCbvSrvUavHeapReuseTimeNs{};
		uint64_t shaderVisibleSamplerHeapCreates{};
		uint64_t shaderVisibleSamplerHeapCreateFailures{};
		uint64_t shaderVisibleSamplerHeapCreateTimeNs{};
		uint64_t shaderVisibleSamplerHeapReuses{};
		uint64_t shaderVisibleSamplerHeapReuseTimeNs{};
		uint64_t shaderVisibleHeapPoolDiscards{};
		uint32_t shaderVisibleCbvSrvUavHeapPoolFree{};
		uint32_t shaderVisibleSamplerHeapPoolFree{};
	};

	struct ShaderVisibleHeapPage
	{
		Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap;
		D3D12_DESCRIPTOR_HEAP_DESC requestedDesc{};

		bool IsValid() const { return heap.Get() != nullptr; }
	};

	void Initialize(ID3D12Device* device);
	void Shutdown();

	Allocation AllocatePersistent(D3D12_DESCRIPTOR_HEAP_TYPE type, uint32_t count = 1);
	void FreePersistent(const Allocation& allocation);
	Allocation AllocateShaderVisible(D3D12_DESCRIPTOR_HEAP_TYPE type, uint32_t count = 1);
	Allocation CopyToShaderVisible(ID3D12Device* device, const Allocation& source);
	void ResetShaderVisibleHeaps();
	void RollShaderVisibleHeaps(ID3D12Device* device, std::vector<ShaderVisibleHeapPage>& retiredHeaps, bool rollCbvSrvUav, bool rollSampler);
	void RecycleShaderVisibleHeap(ShaderVisibleHeapPage&& page);
	uint64_t ClearRecycledShaderVisibleHeaps();
	void BindShaderVisibleHeaps(ID3D12GraphicsCommandList* commandList) const;

	Stats GetStats() const;
	uint32_t GetShaderVisibleRollovers() const { return m_shaderVisibleRollovers; }
	uint32_t GetShaderVisibleCbvSrvUavRollovers() const { return m_cbvSrvUavRollovers; }
	uint32_t GetShaderVisibleSamplerRollovers() const { return m_samplerRollovers; }
	uint32_t GetShaderVisibleCbvSrvUavUsed() const { return m_shaderVisibleCbvSrvUavHeap.used; }
	uint32_t GetShaderVisibleCbvSrvUavCapacity() const { return m_shaderVisibleCbvSrvUavHeap.capacity; }
	uint32_t GetShaderVisibleSamplerUsed() const { return m_shaderVisibleSamplerHeap.used; }
	uint32_t GetShaderVisibleSamplerCapacity() const { return m_shaderVisibleSamplerHeap.capacity; }

private:
	struct HeapState
	{
		Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap;
		std::vector<Microsoft::WRL::ComPtr<ID3D12DescriptorHeap>> retainedHeaps;
		uint32_t descriptorSize{};
		uint32_t capacity{};
		uint32_t used{};
		bool shaderVisible{};
		D3D12_DESCRIPTOR_HEAP_DESC requestedDesc{};
	};

	static constexpr uint32_t kPersistentCbvSrvUavCount = 65536;
	static constexpr uint32_t kPersistentSamplerCount = 4096;
	static constexpr uint32_t kPersistentRtvCount = 8192;
	static constexpr uint32_t kPersistentDsvCount = 4096;
	static constexpr uint32_t kShaderVisibleCbvSrvUavCount = 262144;
	static constexpr uint32_t kShaderVisibleSamplerCount = 2048;

	void CreateHeap(ID3D12Device* device, HeapState& state, D3D12_DESCRIPTOR_HEAP_TYPE type, uint32_t count, bool shaderVisible, const wchar_t* debugName);
	struct ShaderVisibleHeapTiming
	{
		uint64_t createCount{};
		uint64_t createFailureCount{};
		uint64_t createTimeNs{};
		uint64_t reuseCount{};
		uint64_t reuseTimeNs{};
	};

	static constexpr size_t kCbvSrvUavPoolIndex = 0;
	static constexpr size_t kSamplerPoolIndex = 1;
	static constexpr size_t kMaxRecycledCbvSrvUavPages = 2;
	static constexpr size_t kMaxRecycledSamplerPages = 16;
	static constexpr uint64_t kMaxRecycledCbvSrvUavLogicalBytes = 32ull * 1024ull * 1024ull;
	static constexpr uint64_t kMaxRecycledSamplerLogicalBytes = 2ull * 1024ull * 1024ull;

	void CreateShaderVisibleHeap(ID3D12Device* device, HeapState& state, D3D12_DESCRIPTOR_HEAP_TYPE type, uint32_t count, const wchar_t* debugName);
	bool AcquireRecycledShaderVisibleHeap(ID3D12Device* device, HeapState& state, D3D12_DESCRIPTOR_HEAP_TYPE type, uint32_t count);
	void PrewarmShaderVisibleHeap(ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_TYPE type, uint32_t count, const wchar_t* debugName);
	static size_t ShaderVisiblePoolIndex(D3D12_DESCRIPTOR_HEAP_TYPE type);
	Allocation AllocateFromHeap(HeapState& state, D3D12_DESCRIPTOR_HEAP_TYPE type, uint32_t count);
	static size_t HeapIndex(D3D12_DESCRIPTOR_HEAP_TYPE type);

	std::array<HeapState, D3D12_DESCRIPTOR_HEAP_TYPE_NUM_TYPES> m_persistentHeaps{};
	std::array<std::vector<Allocation>, D3D12_DESCRIPTOR_HEAP_TYPE_NUM_TYPES> m_persistentFreeLists;
	HeapState m_shaderVisibleCbvSrvUavHeap{};
	HeapState m_shaderVisibleSamplerHeap{};
	Microsoft::WRL::ComPtr<ID3D12Device> m_device;
	std::array<std::vector<ShaderVisibleHeapPage>, 2> m_recycledShaderVisibleHeaps;
	std::array<ShaderVisibleHeapTiming, 2> m_shaderVisibleHeapTimings{};
	uint64_t m_shaderVisibleHeapPoolDiscards{};
	uint32_t m_shaderVisibleRollovers{};
	uint32_t m_cbvSrvUavRollovers{};
	uint32_t m_samplerRollovers{};
};
