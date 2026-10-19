#include "Cafe/HW/Latte/Renderer/D3D12/D3D12DescriptorManager.h"

#include <algorithm>
#include <chrono>
#include <iterator>
#include <stdexcept>

namespace
{
void SetDescriptorHeapName(ID3D12DescriptorHeap* heap, const wchar_t* name)
{
#ifdef CEMU_DEBUG_ASSERT
	if (heap && name)
		heap->SetName(name);
#else
	(void)heap;
	(void)name;
#endif
}
}

void D3D12DescriptorManager::Initialize(ID3D12Device* device)
{
	if (!device)
		throw std::runtime_error("D3D12 descriptor manager requires a device");

	m_device = device;

	CreateHeap(device, m_persistentHeaps[HeapIndex(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV)], D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kPersistentCbvSrvUavCount, false, L"Cemu D3D12 persistent CBV/SRV/UAV heap");
	CreateHeap(device, m_persistentHeaps[HeapIndex(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER)], D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, kPersistentSamplerCount, false, L"Cemu D3D12 persistent sampler heap");
	CreateHeap(device, m_persistentHeaps[HeapIndex(D3D12_DESCRIPTOR_HEAP_TYPE_RTV)], D3D12_DESCRIPTOR_HEAP_TYPE_RTV, kPersistentRtvCount, false, L"Cemu D3D12 persistent RTV heap");
	CreateHeap(device, m_persistentHeaps[HeapIndex(D3D12_DESCRIPTOR_HEAP_TYPE_DSV)], D3D12_DESCRIPTOR_HEAP_TYPE_DSV, kPersistentDsvCount, false, L"Cemu D3D12 persistent DSV heap");

	CreateShaderVisibleHeap(device, m_shaderVisibleCbvSrvUavHeap, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kShaderVisibleCbvSrvUavCount, L"Cemu D3D12 shader-visible CBV/SRV/UAV heap");
	CreateShaderVisibleHeap(device, m_shaderVisibleSamplerHeap, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, kShaderVisibleSamplerCount, L"Cemu D3D12 shader-visible sampler heap");

	PrewarmShaderVisibleHeap(device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kShaderVisibleCbvSrvUavCount, L"Cemu D3D12 spare shader-visible CBV/SRV/UAV heap");
	for (uint32_t i = 0; i < 4; ++i)
		PrewarmShaderVisibleHeap(device, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, kShaderVisibleSamplerCount, L"Cemu D3D12 spare shader-visible sampler heap");
}

void D3D12DescriptorManager::Shutdown()
{
	for (auto& heap : m_persistentHeaps)
	{
		heap.retainedHeaps.clear();
		heap = {};
	}
	for (auto& freeList : m_persistentFreeLists)
		freeList.clear();
	m_shaderVisibleCbvSrvUavHeap = {};
	m_shaderVisibleSamplerHeap = {};
	for (auto& pool : m_recycledShaderVisibleHeaps)
		pool.clear();
	m_shaderVisibleHeapTimings = {};
	m_shaderVisibleHeapPoolDiscards = 0;
	m_device.Reset();
	m_shaderVisibleRollovers = 0;
	m_cbvSrvUavRollovers = 0;
	m_samplerRollovers = 0;
}

D3D12DescriptorManager::Allocation D3D12DescriptorManager::AllocatePersistent(D3D12_DESCRIPTOR_HEAP_TYPE type, uint32_t count)
{
	// persistent heaps are otherwise bump allocators and exhaust as textures churn
	if (count == 1)
	{
		auto& freeList = m_persistentFreeLists[HeapIndex(type)];
		if (!freeList.empty())
		{
			Allocation allocation = freeList.back();
			freeList.pop_back();
			return allocation;
		}
	}
	auto& state = m_persistentHeaps[HeapIndex(type)];
	if (state.heap && count != 0 && state.used + count > state.capacity && !state.shaderVisible && m_device)
	{
		state.retainedHeaps.emplace_back(std::move(state.heap));
		const uint32_t newCapacity = count > state.capacity ? count : state.capacity;
		CreateHeap(m_device.Get(), state, type, newCapacity, false, L"Cemu D3D12 persistent descriptor heap page");
	}
	return AllocateFromHeap(state, type, count);
}

void D3D12DescriptorManager::FreePersistent(const Allocation& allocation)
{
	// descriptors are read at record time, so a freed slot can be rewritten immediately
	if (!allocation.IsValid() || allocation.shaderVisible || allocation.count != 1)
		return;
	if (allocation.type != D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV &&
		allocation.type != D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER &&
		allocation.type != D3D12_DESCRIPTOR_HEAP_TYPE_RTV &&
		allocation.type != D3D12_DESCRIPTOR_HEAP_TYPE_DSV)
		return;
	m_persistentFreeLists[HeapIndex(allocation.type)].push_back(allocation);
}

D3D12DescriptorManager::Allocation D3D12DescriptorManager::AllocateShaderVisible(D3D12_DESCRIPTOR_HEAP_TYPE type, uint32_t count)
{
	if (type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV)
	{
		return AllocateFromHeap(m_shaderVisibleCbvSrvUavHeap, type, count);
	}
	if (type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER)
	{
		return AllocateFromHeap(m_shaderVisibleSamplerHeap, type, count);
	}
	throw std::runtime_error("Only CBV/SRV/UAV and sampler heaps can be shader-visible in D3D12");
}

D3D12DescriptorManager::Allocation D3D12DescriptorManager::CopyToShaderVisible(ID3D12Device* device, const Allocation& source)
{
	if (!source.IsValid())
		return {};
	if (source.shaderVisible)
		return source;
	if (source.type != D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV && source.type != D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER)
		throw std::runtime_error("Only CBV/SRV/UAV and sampler descriptors can be copied to shader-visible heaps");

	Allocation destination = AllocateShaderVisible(source.type, source.count);
	device->CopyDescriptorsSimple(source.count, destination.cpu, source.cpu, source.type);
	return destination;
}

void D3D12DescriptorManager::ResetShaderVisibleHeaps()
{
	m_shaderVisibleCbvSrvUavHeap.used = 0;
	m_shaderVisibleSamplerHeap.used = 0;
	m_shaderVisibleRollovers++;
	m_cbvSrvUavRollovers++;
	m_samplerRollovers++;
}

void D3D12DescriptorManager::RollShaderVisibleHeaps(ID3D12Device* device, std::vector<ShaderVisibleHeapPage>& retiredHeaps, bool rollCbvSrvUav, bool rollSampler)
{
	if (!device)
		throw std::runtime_error("D3D12 descriptor heap rollover requires a device");
	if (!rollCbvSrvUav && !rollSampler)
		return;

	auto prepareReplacement = [this, device](HeapState& replacement, D3D12_DESCRIPTOR_HEAP_TYPE type, uint32_t count, const wchar_t* debugName)
	{
		if (!AcquireRecycledShaderVisibleHeap(device, replacement, type, count))
			CreateShaderVisibleHeap(device, replacement, type, count, debugName);
	};
	auto activateReplacement = [&retiredHeaps](HeapState& heap, HeapState& replacement)
	{
		ShaderVisibleHeapPage retired;
		retired.heap = std::move(heap.heap);
		retired.requestedDesc = heap.requestedDesc;
		heap = std::move(replacement);
		if (retired.IsValid())
			retiredHeaps.emplace_back(std::move(retired));
	};

	// allocate all replacements before swapping so a failure keeps both old heaps
	HeapState cbvSrvUavReplacement;
	HeapState samplerReplacement;
	if (rollCbvSrvUav)
		prepareReplacement(cbvSrvUavReplacement, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kShaderVisibleCbvSrvUavCount, L"Cemu D3D12 shader-visible CBV/SRV/UAV heap");
	if (rollSampler)
		prepareReplacement(samplerReplacement, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, kShaderVisibleSamplerCount, L"Cemu D3D12 shader-visible sampler heap");
	retiredHeaps.reserve(retiredHeaps.size() + (rollCbvSrvUav ? 1 : 0) + (rollSampler ? 1 : 0));

	// roll only the exhausted heap so cached tables in the other stay valid
	if (rollCbvSrvUav)
	{
		activateReplacement(m_shaderVisibleCbvSrvUavHeap, cbvSrvUavReplacement);
		m_cbvSrvUavRollovers++;
	}
	if (rollSampler)
	{
		activateReplacement(m_shaderVisibleSamplerHeap, samplerReplacement);
		m_samplerRollovers++;
	}
	m_shaderVisibleRollovers++;
}

void D3D12DescriptorManager::BindShaderVisibleHeaps(ID3D12GraphicsCommandList* commandList) const
{
	if (!commandList)
		return;
	ID3D12DescriptorHeap* heaps[] =
	{
		m_shaderVisibleCbvSrvUavHeap.heap.Get(),
		m_shaderVisibleSamplerHeap.heap.Get(),
	};
	commandList->SetDescriptorHeaps(2, heaps);
}

D3D12DescriptorManager::Stats D3D12DescriptorManager::GetStats() const
{
	Stats stats{};
	stats.persistentCbvSrvUavUsed = m_persistentHeaps[HeapIndex(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV)].used;
	stats.persistentSamplerUsed = m_persistentHeaps[HeapIndex(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER)].used;
	stats.persistentRtvUsed = m_persistentHeaps[HeapIndex(D3D12_DESCRIPTOR_HEAP_TYPE_RTV)].used;
	stats.persistentDsvUsed = m_persistentHeaps[HeapIndex(D3D12_DESCRIPTOR_HEAP_TYPE_DSV)].used;
	stats.shaderVisibleCbvSrvUavUsed = m_shaderVisibleCbvSrvUavHeap.used;
	stats.shaderVisibleSamplerUsed = m_shaderVisibleSamplerHeap.used;
	stats.shaderVisibleCbvSrvUavCapacity = m_shaderVisibleCbvSrvUavHeap.capacity;
	stats.shaderVisibleSamplerCapacity = m_shaderVisibleSamplerHeap.capacity;
	stats.shaderVisibleRollovers = m_shaderVisibleRollovers;
	stats.shaderVisibleCbvSrvUavRollovers = m_cbvSrvUavRollovers;
	stats.shaderVisibleSamplerRollovers = m_samplerRollovers;
	const auto& cbvTiming = m_shaderVisibleHeapTimings[kCbvSrvUavPoolIndex];
	const auto& samplerTiming = m_shaderVisibleHeapTimings[kSamplerPoolIndex];
	stats.shaderVisibleCbvSrvUavHeapCreates = cbvTiming.createCount;
	stats.shaderVisibleCbvSrvUavHeapCreateFailures = cbvTiming.createFailureCount;
	stats.shaderVisibleCbvSrvUavHeapCreateTimeNs = cbvTiming.createTimeNs;
	stats.shaderVisibleCbvSrvUavHeapReuses = cbvTiming.reuseCount;
	stats.shaderVisibleCbvSrvUavHeapReuseTimeNs = cbvTiming.reuseTimeNs;
	stats.shaderVisibleSamplerHeapCreates = samplerTiming.createCount;
	stats.shaderVisibleSamplerHeapCreateFailures = samplerTiming.createFailureCount;
	stats.shaderVisibleSamplerHeapCreateTimeNs = samplerTiming.createTimeNs;
	stats.shaderVisibleSamplerHeapReuses = samplerTiming.reuseCount;
	stats.shaderVisibleSamplerHeapReuseTimeNs = samplerTiming.reuseTimeNs;
	stats.shaderVisibleHeapPoolDiscards = m_shaderVisibleHeapPoolDiscards;
	stats.shaderVisibleCbvSrvUavHeapPoolFree = (uint32_t)m_recycledShaderVisibleHeaps[kCbvSrvUavPoolIndex].size();
	stats.shaderVisibleSamplerHeapPoolFree = (uint32_t)m_recycledShaderVisibleHeaps[kSamplerPoolIndex].size();
	return stats;
}

void D3D12DescriptorManager::CreateHeap(ID3D12Device* device, HeapState& state, D3D12_DESCRIPTOR_HEAP_TYPE type, uint32_t count, bool shaderVisible, const wchar_t* debugName)
{
	D3D12_DESCRIPTOR_HEAP_DESC desc{};
	desc.Type = type;
	desc.NumDescriptors = count;
	desc.Flags = shaderVisible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
	if (FAILED(device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&state.heap))))
		throw std::runtime_error("Failed to create D3D12 descriptor heap");

	SetDescriptorHeapName(state.heap.Get(), debugName);
	state.descriptorSize = device->GetDescriptorHandleIncrementSize(type);
	state.capacity = count;
	state.used = 0;
	state.shaderVisible = shaderVisible;
	state.requestedDesc = desc;
}

void D3D12DescriptorManager::CreateShaderVisibleHeap(ID3D12Device* device, HeapState& state, D3D12_DESCRIPTOR_HEAP_TYPE type, uint32_t count, const wchar_t* debugName)
{
	const auto started = std::chrono::steady_clock::now();
	auto& timing = m_shaderVisibleHeapTimings[ShaderVisiblePoolIndex(type)];
	try
	{
		CreateHeap(device, state, type, count, true, debugName);
	}
	catch (...)
	{
		const uint64_t elapsedNs = (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
			std::chrono::steady_clock::now() - started).count();
		timing.createFailureCount++;
		timing.createTimeNs += elapsedNs;
		throw;
	}
	const uint64_t elapsedNs = (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
		std::chrono::steady_clock::now() - started).count();
	timing.createCount++;
	timing.createTimeNs += elapsedNs;
}

bool D3D12DescriptorManager::AcquireRecycledShaderVisibleHeap(ID3D12Device* device, HeapState& state, D3D12_DESCRIPTOR_HEAP_TYPE type, uint32_t count)
{
	const auto started = std::chrono::steady_clock::now();
	D3D12_DESCRIPTOR_HEAP_DESC expected{};
	expected.Type = type;
	expected.NumDescriptors = count;
	expected.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

	auto& pool = m_recycledShaderVisibleHeaps[ShaderVisiblePoolIndex(type)];
	const auto matches = [&expected](const ShaderVisibleHeapPage& page)
	{
		return page.IsValid() &&
			page.requestedDesc.Type == expected.Type &&
			page.requestedDesc.NumDescriptors == expected.NumDescriptors &&
			page.requestedDesc.Flags == expected.Flags &&
			page.requestedDesc.NodeMask == expected.NodeMask;
	};
	const auto reverseIt = std::find_if(pool.rbegin(), pool.rend(), matches);
	if (reverseIt == pool.rend())
		return false;

	const size_t index = pool.size() - 1 - (size_t)std::distance(pool.rbegin(), reverseIt);
	ShaderVisibleHeapPage page = std::move(pool[index]);
	pool.erase(pool.begin() + index);
	state.heap = std::move(page.heap);
	state.descriptorSize = device->GetDescriptorHandleIncrementSize(type);
	state.capacity = count;
	state.used = 0;
	state.shaderVisible = true;
	state.requestedDesc = page.requestedDesc;

	const uint64_t elapsedNs = (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
		std::chrono::steady_clock::now() - started).count();
	auto& timing = m_shaderVisibleHeapTimings[ShaderVisiblePoolIndex(type)];
	timing.reuseCount++;
	timing.reuseTimeNs += elapsedNs;
	return true;
}

void D3D12DescriptorManager::PrewarmShaderVisibleHeap(ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_TYPE type, uint32_t count, const wchar_t* debugName)
{
	try
	{
		HeapState state;
		CreateShaderVisibleHeap(device, state, type, count, debugName);
		ShaderVisibleHeapPage page;
		page.heap = std::move(state.heap);
		page.requestedDesc = state.requestedDesc;
		RecycleShaderVisibleHeap(std::move(page));
	}
	catch (const std::exception&)
	{
		// prewarming is optional; fall back to the normal rollover path
	}
}

void D3D12DescriptorManager::RecycleShaderVisibleHeap(ShaderVisibleHeapPage&& page)
{
	if (!page.IsValid() || !m_device)
		return;
	if (page.requestedDesc.Type != D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV &&
		page.requestedDesc.Type != D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER)
	{
		m_shaderVisibleHeapPoolDiscards++;
		return;
	}

	const bool isCbvSrvUav = page.requestedDesc.Type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	const uint32_t expectedCount = isCbvSrvUav ? kShaderVisibleCbvSrvUavCount : kShaderVisibleSamplerCount;
	if (page.requestedDesc.NumDescriptors != expectedCount ||
		page.requestedDesc.Flags != D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE ||
		page.requestedDesc.NodeMask != 0)
	{
		m_shaderVisibleHeapPoolDiscards++;
		return;
	}

	const size_t poolIndex = ShaderVisiblePoolIndex(page.requestedDesc.Type);
	auto& pool = m_recycledShaderVisibleHeaps[poolIndex];
	const size_t maxPages = isCbvSrvUav ? kMaxRecycledCbvSrvUavPages : kMaxRecycledSamplerPages;
	const uint64_t maxLogicalBytes = isCbvSrvUav ? kMaxRecycledCbvSrvUavLogicalBytes : kMaxRecycledSamplerLogicalBytes;
	const uint64_t descriptorSize = m_device->GetDescriptorHandleIncrementSize(page.requestedDesc.Type);
	const uint64_t pageLogicalBytes = descriptorSize * page.requestedDesc.NumDescriptors;
	uint64_t pooledLogicalBytes = 0;
	for (const auto& pooledPage : pool)
		pooledLogicalBytes += descriptorSize * pooledPage.requestedDesc.NumDescriptors;
	if (pool.size() >= maxPages || pageLogicalBytes > maxLogicalBytes - std::min(maxLogicalBytes, pooledLogicalBytes))
	{
		m_shaderVisibleHeapPoolDiscards++;
		return;
	}

	try
	{
		pool.emplace_back(std::move(page));
	}
	catch (...)
	{
		// recycling is optional; discard the page rather than fail frame cleanup
		m_shaderVisibleHeapPoolDiscards++;
	}
}

uint64_t D3D12DescriptorManager::ClearRecycledShaderVisibleHeaps()
{
	if (!m_device)
		return 0;

	uint64_t releasedLogicalBytes = 0;
	for (size_t poolIndex = 0; poolIndex < m_recycledShaderVisibleHeaps.size(); ++poolIndex)
	{
		const D3D12_DESCRIPTOR_HEAP_TYPE type = poolIndex == kCbvSrvUavPoolIndex ?
			D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV : D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
		const uint64_t descriptorSize = m_device->GetDescriptorHandleIncrementSize(type);
		for (const auto& page : m_recycledShaderVisibleHeaps[poolIndex])
			releasedLogicalBytes += descriptorSize * page.requestedDesc.NumDescriptors;
		m_recycledShaderVisibleHeaps[poolIndex].clear();
	}
	return releasedLogicalBytes;
}

size_t D3D12DescriptorManager::ShaderVisiblePoolIndex(D3D12_DESCRIPTOR_HEAP_TYPE type)
{
	if (type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV)
		return kCbvSrvUavPoolIndex;
	if (type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER)
		return kSamplerPoolIndex;
	throw std::runtime_error("Invalid D3D12 shader-visible descriptor heap type");
}

D3D12DescriptorManager::Allocation D3D12DescriptorManager::AllocateFromHeap(HeapState& state, D3D12_DESCRIPTOR_HEAP_TYPE type, uint32_t count)
{
	if (!state.heap || count == 0 || state.used + count > state.capacity)
		throw std::runtime_error("D3D12 descriptor heap exhausted");

	Allocation allocation{};
	allocation.type = type;
	allocation.index = state.used;
	allocation.count = count;
	allocation.shaderVisible = state.shaderVisible;
	allocation.cpu = state.heap->GetCPUDescriptorHandleForHeapStart();
	allocation.cpu.ptr += size_t(allocation.index) * state.descriptorSize;
	if (state.shaderVisible)
	{
		allocation.gpu = state.heap->GetGPUDescriptorHandleForHeapStart();
		allocation.gpu.ptr += uint64_t(allocation.index) * state.descriptorSize;
	}
	state.used += count;
	return allocation;
}

size_t D3D12DescriptorManager::HeapIndex(D3D12_DESCRIPTOR_HEAP_TYPE type)
{
	return static_cast<size_t>(type);
}
