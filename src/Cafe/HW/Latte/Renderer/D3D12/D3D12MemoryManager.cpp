#include "Cafe/HW/Latte/Renderer/D3D12/D3D12MemoryManager.h"
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12RingRange.h"

#include "Cemu/Logging/CemuLogging.h"
#if defined(CEMU_UWP)
#include "uwp/UwpMemoryBudgetService.h"
#endif

#include <algorithm>
#include <d3d12sdklayers.h>
#include <stdexcept>
#include <utility>

namespace
{
uint64_t AlignUp(uint64_t value, uint64_t alignment)
{
	if (alignment == 0)
		return value;
	return (value + alignment - 1) & ~(alignment - 1);
}

void SetResourceName(ID3D12Object* object, const wchar_t* name)
{
#ifdef CEMU_DEBUG_ASSERT
	if (object && name)
		object->SetName(name);
#else
	(void)object;
	(void)name;
#endif
}

const char* D3D12MemoryMessageSeverityName(D3D12_MESSAGE_SEVERITY severity)
{
	switch (severity)
	{
	case D3D12_MESSAGE_SEVERITY_CORRUPTION:
		return "corruption";
	case D3D12_MESSAGE_SEVERITY_ERROR:
		return "error";
	case D3D12_MESSAGE_SEVERITY_WARNING:
		return "warning";
	case D3D12_MESSAGE_SEVERITY_INFO:
		return "info";
	case D3D12_MESSAGE_SEVERITY_MESSAGE:
		return "message";
	default:
		return "unknown";
	}
}

void LogD3D12InfoQueueMessages(ID3D12Device* device, const char* operation, uint32_t maxMessages)
{
	Microsoft::WRL::ComPtr<ID3D12InfoQueue> infoQueue;
	if (!device || FAILED(device->QueryInterface(IID_PPV_ARGS(&infoQueue))))
		return;

	const uint64_t messageCount = infoQueue->GetNumStoredMessagesAllowedByRetrievalFilter();
	const uint64_t firstMessage = messageCount > maxMessages ? messageCount - maxMessages : 0;
	cemuLog_log(LogType::Force, "D3D12 info queue after {}: stored messages={}", operation, (unsigned long long)messageCount);
	for (uint64_t i = firstMessage; i < messageCount; ++i)
	{
		SIZE_T messageLength = 0;
		if (FAILED(infoQueue->GetMessage(i, nullptr, &messageLength)) || messageLength == 0)
			continue;
		std::vector<char> messageStorage(messageLength);
		auto* message = reinterpret_cast<D3D12_MESSAGE*>(messageStorage.data());
		if (FAILED(infoQueue->GetMessage(i, message, &messageLength)))
			continue;
		cemuLog_log(LogType::Force, "D3D12 info [{}] id={} severity={}: {}",
					(unsigned long long)i,
					(uint32_t)message->ID,
					D3D12MemoryMessageSeverityName(message->Severity),
					message->pDescription ? message->pDescription : "");
	}
}

void LogD3D12DeviceRemovedData(ID3D12Device* device, HRESULT hr, const char* operation)
{
	if (!device)
		return;

	const HRESULT removedReason = device->GetDeviceRemovedReason();
	if (hr != DXGI_ERROR_DEVICE_REMOVED &&
		hr != DXGI_ERROR_DEVICE_RESET &&
		hr != DXGI_ERROR_DEVICE_HUNG &&
		removedReason == S_OK)
	{
		return;
	}

	cemuLog_log(LogType::Force, "D3D12 {} failed with 0x{:08x}; device removed reason 0x{:08x}",
				operation, (uint32_t)hr, (uint32_t)removedReason);
	LogD3D12InfoQueueMessages(device, operation, 32);

#ifdef __ID3D12DeviceRemovedExtendedData_INTERFACE_DEFINED__
	Microsoft::WRL::ComPtr<ID3D12DeviceRemovedExtendedData> dred;
	if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dred))))
		return;

	D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT breadcrumbs{};
	if (SUCCEEDED(dred->GetAutoBreadcrumbsOutput(&breadcrumbs)))
	{
		uint32_t nodeCount = 0;
		for (auto* node = breadcrumbs.pHeadAutoBreadcrumbNode; node; node = node->pNext)
		{
			nodeCount++;
			if (nodeCount > 16)
				continue;
			const uint32_t lastValue = node->pLastBreadcrumbValue ? *node->pLastBreadcrumbValue : 0;
			const char* commandListName = node->pCommandListDebugNameA ? node->pCommandListDebugNameA : "<unnamed>";
			const char* commandQueueName = node->pCommandQueueDebugNameA ? node->pCommandQueueDebugNameA : "<unnamed>";
			const uint32_t opIndex = lastValue > 0 ? std::min<uint32_t>(lastValue - 1, node->BreadcrumbCount - 1) : 0;
			const uint32_t op = node->pCommandHistory && node->BreadcrumbCount != 0 ? (uint32_t)node->pCommandHistory[opIndex] : 0;
			cemuLog_log(LogType::Force, "D3D12 DRED breadcrumb node {} queue={} list={} completed={}/{} lastOp={}",
						nodeCount,
						commandQueueName,
						commandListName,
						lastValue,
						node->BreadcrumbCount,
						op);
		}
		cemuLog_log(LogType::Force, "D3D12 DRED auto breadcrumb nodes: {}", nodeCount);
	}

	D3D12_DRED_PAGE_FAULT_OUTPUT pageFault{};
	if (SUCCEEDED(dred->GetPageFaultAllocationOutput(&pageFault)) && pageFault.PageFaultVA != 0)
	{
		cemuLog_log(LogType::Force, "D3D12 DRED page fault VA: 0x{:016x}",
					(unsigned long long)pageFault.PageFaultVA);
	}
#endif
}
}

class D3D12MemoryManager::SynchronizedRingAllocator
{
public:
	SynchronizedRingAllocator(D3D12MemoryManager& memoryManager, D3D12_HEAP_TYPE heapType, D3D12_RESOURCE_STATES initialState, uint32_t minimumBufferSize, const wchar_t* debugName)
		: m_memoryManager(memoryManager), m_heapType(heapType), m_initialState(initialState), m_minimumBufferSize(minimumBufferSize), m_debugName(debugName)
	{
	}

	~SynchronizedRingAllocator()
	{
		for (auto& buffer : m_buffers)
		{
			if (buffer.resource.resource)
				buffer.resource.resource->Unmap(0, nullptr);
		}
	}

	RingReservation Allocate(uint32_t size, uint32_t alignment, uint64_t fenceValue)
	{
		alignment = std::max<uint32_t>(alignment, 128);
		if (size == 0 || (alignment & (alignment - 1)) != 0)
			return {};
		const uint64_t alignedSize = AlignUp(size, 128);

		for (auto& buffer : m_buffers)
		{
			const bool hasPending = !buffer.syncPoints.empty();
			const uint64_t oldestOffset = hasPending ? buffer.syncPoints.front().offset : 0;
			const bool currentFenceOwnsOldestRange = hasPending &&
				buffer.syncPoints.front().fenceValue == fenceValue;
			uint64_t writeIndex = 0;
			if (!D3D12RingRange::TryReserve(buffer.size, buffer.writeIndex, alignedSize,
				alignment, hasPending, oldestOffset, currentFenceOwnsOldestRange, writeIndex))
				continue;

			AddSyncPoint(buffer, fenceValue, writeIndex);
			buffer.writeIndex = writeIndex + alignedSize;
			buffer.cleanupCounter = 0;
			return { buffer.resource.resource.Get(), buffer.basePtr + writeIndex, writeIndex, alignedSize, buffer.index };
		}

		if (!AllocateAdditionalBuffer((uint32_t)alignedSize))
			return {};
		return Allocate(size, alignment, fenceValue);
	}

	void Cleanup(uint64_t completedFence)
	{
		for (auto& buffer : m_buffers)
		{
			while (!buffer.syncPoints.empty() && buffer.syncPoints.front().fenceValue <= completedFence)
				buffer.syncPoints.pop();
			if (buffer.syncPoints.empty())
				buffer.cleanupCounter = std::min(buffer.cleanupCounter + 1, idleCleanupThreshold);
		}

		bool removedBuffer = false;
		for (size_t index = m_buffers.size(); index-- > 1;)
		{
			auto& buffer = m_buffers[index];
			if (!buffer.syncPoints.empty() || buffer.cleanupCounter < idleCleanupThreshold)
				continue;
			if (buffer.resource.resource)
				buffer.resource.resource->Unmap(0, nullptr);
			m_currentBytes -= buffer.size;
			m_reclaimedBuffers++;
			m_buffers.erase(m_buffers.begin() + index);
			removedBuffer = true;
		}
		if (removedBuffer)
		{
			for (size_t index = 0; index < m_buffers.size(); ++index)
				m_buffers[index].index = (uint32_t)index;
		}
	}

	RingStats GetStats() const
	{
		RingStats stats{};
		stats.buffers = (uint32_t)m_buffers.size();
		for (const auto& buffer : m_buffers)
		{
			stats.totalBytes += buffer.size;
			stats.usedBytes += buffer.writeIndex;
		}
		stats.peakBuffers = m_peakBuffers;
		stats.peakBytes = m_peakBytes;
		stats.reclaimedBuffers = m_reclaimedBuffers;
		return stats;
	}

private:
	static constexpr uint32_t idleCleanupThreshold = 240;

	struct SyncPoint
	{
		uint64_t fenceValue{};
		uint64_t offset{};
	};

	struct Buffer
	{
		Resource resource;
		uint8_t* basePtr{};
		uint64_t size{};
		uint64_t writeIndex{};
		std::queue<SyncPoint> syncPoints;
		uint64_t lastSyncFenceValue{ UINT64_MAX };
		uint32_t index{};
		uint32_t cleanupCounter{};
	};

	bool AllocateAdditionalBuffer(uint32_t sizeRequired)
	{
		uint64_t bufferSize = m_minimumBufferSize;
		while (bufferSize < sizeRequired)
			bufferSize += m_minimumBufferSize;

		Buffer buffer{};
		buffer.index = (uint32_t)m_buffers.size();
		buffer.size = bufferSize;
		if (!m_memoryManager.CreateBuffer(bufferSize, m_heapType, D3D12_RESOURCE_FLAG_NONE, m_initialState, buffer.resource))
		{
			cemuLog_log(LogType::Force, "D3D12: failed to allocate {} ring buffer of {} bytes.",
						m_heapType == D3D12_HEAP_TYPE_UPLOAD ? "upload" : "readback",
						(unsigned long long)bufferSize);
			cemuLog_waitForFlush();
			return false;
		}

		D3D12_RANGE readRange{ 0, 0 };
		if (FAILED(buffer.resource.resource->Map(0, &readRange, reinterpret_cast<void**>(&buffer.basePtr))))
		{
			cemuLog_log(LogType::Force, "D3D12: failed to map {} ring buffer of {} bytes.",
						m_heapType == D3D12_HEAP_TYPE_UPLOAD ? "upload" : "readback",
						(unsigned long long)bufferSize);
			cemuLog_waitForFlush();
			return false;
		}

		SetResourceName(buffer.resource.resource.Get(), m_debugName);
		m_buffers.emplace_back(std::move(buffer));
		m_currentBytes += bufferSize;
		m_peakBuffers = std::max<uint32_t>(m_peakBuffers, (uint32_t)m_buffers.size());
		m_peakBytes = std::max(m_peakBytes, m_currentBytes);
		return true;
	}

	void AddSyncPoint(Buffer& buffer, uint64_t fenceValue, uint64_t offset)
	{
		if (buffer.lastSyncFenceValue == fenceValue)
			return;
		buffer.lastSyncFenceValue = fenceValue;
		buffer.syncPoints.push({ fenceValue, offset });
	}

	D3D12MemoryManager& m_memoryManager;
	D3D12_HEAP_TYPE m_heapType;
	D3D12_RESOURCE_STATES m_initialState;
	uint32_t m_minimumBufferSize;
	const wchar_t* m_debugName;
	std::vector<Buffer> m_buffers;
	uint32_t m_peakBuffers{};
	uint64_t m_currentBytes{};
	uint64_t m_peakBytes{};
	uint64_t m_reclaimedBuffers{};
};

D3D12MemoryManager::D3D12MemoryManager() = default;

D3D12MemoryManager::~D3D12MemoryManager()
{
	Shutdown();
}

void D3D12MemoryManager::Initialize(ID3D12Device* device, IDXGIAdapter* adapter)
{
	if (!device || !adapter)
		throw std::runtime_error("D3D12 memory manager requires a device and adapter");
	m_device = device;

	D3D12MA::ALLOCATOR_DESC allocatorDesc{};
	allocatorDesc.Flags = D3D12MA_RECOMMENDED_ALLOCATOR_FLAGS;
	allocatorDesc.pDevice = device;
	allocatorDesc.pAdapter = adapter;
	D3D12MA::Allocator* allocator = nullptr;
	if (FAILED(D3D12MA::CreateAllocator(&allocatorDesc, &allocator)))
		throw std::runtime_error("Failed to create D3D12 memory allocator");
	m_allocator.Attach(allocator);

	constexpr uint32_t uploadRingSize = 32u * 1024u * 1024u;
	constexpr uint32_t readbackRingSize = 32u * 1024u * 1024u;
	m_uploadRing = std::make_unique<SynchronizedRingAllocator>(*this, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, uploadRingSize, L"Cemu D3D12 upload ring buffer");
	m_readbackRing = std::make_unique<SynchronizedRingAllocator>(*this, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, readbackRingSize, L"Cemu D3D12 readback ring buffer");
	cemuLog_log(LogType::Force, "D3D12: upload/readback rings protect unfinished GPU ranges at wrap and alignment boundaries.");
}

void D3D12MemoryManager::Shutdown()
{
	m_uploadRing.reset();
	m_readbackRing.reset();
	m_textureUploadBuffer.clear();
	m_textureUploadBuffer.shrink_to_fit();
	m_allocator.Reset();
	m_device.Reset();
}

bool D3D12MemoryManager::CreateBuffer(uint64_t size, D3D12_HEAP_TYPE heapType, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES initialState, Resource& outResource)
{
	if (!m_allocator)
		return false;

	D3D12_RESOURCE_DESC desc{};
	desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	desc.Alignment = 0;
	desc.Width = std::max<uint64_t>(size, 1);
	desc.Height = 1;
	desc.DepthOrArraySize = 1;
	desc.MipLevels = 1;
	desc.Format = DXGI_FORMAT_UNKNOWN;
	desc.SampleDesc.Count = 1;
	desc.SampleDesc.Quality = 0;
	desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	desc.Flags = flags;

	D3D12MA::ALLOCATION_DESC allocationDesc{};
	allocationDesc.HeapType = heapType;

	D3D12MA::Allocation* allocation = nullptr;
	ID3D12Resource* resource = nullptr;
	HRESULT hr = m_allocator->CreateResource(&allocationDesc, &desc, initialState, nullptr, &allocation, IID_PPV_ARGS(&resource));
#if defined(CEMU_UWP)
	if (hr == E_OUTOFMEMORY)
	{
		if (resource)
			resource->Release();
		if (allocation)
			allocation->Release();
		resource = nullptr;
		allocation = nullptr;
		CemuUwp::MemoryBudgetService::CompactProcessHeap();
		hr = m_allocator->CreateResource(&allocationDesc, &desc, initialState, nullptr, &allocation, IID_PPV_ARGS(&resource));
		if (SUCCEEDED(hr))
			m_bufferAllocationRetries++;
	}
#endif
	if (FAILED(hr))
	{
		if (resource)
			resource->Release();
		if (allocation)
			allocation->Release();
		const char* heapName = "unknown";
		switch (heapType)
		{
		case D3D12_HEAP_TYPE_DEFAULT: heapName = "default"; break;
		case D3D12_HEAP_TYPE_UPLOAD: heapName = "upload"; break;
		case D3D12_HEAP_TYPE_READBACK: heapName = "readback"; break;
		default: break;
		}
		cemuLog_log(LogType::Force, "D3D12 buffer allocation failed: heap={} size={} flags=0x{:08x} state=0x{:08x} HRESULT=0x{:08x}",
					heapName,
					(unsigned long long)size,
					(uint32_t)flags,
					(uint32_t)initialState,
					(uint32_t)hr);
		LogD3D12DeviceRemovedData(m_device.Get(), hr, "CreateBuffer");
		cemuLog_waitForFlush();
		return false;
	}

	outResource.resource.Attach(resource);
	outResource.allocation.Attach(allocation);
	outResource.size = size;
	outResource.state = initialState;

	switch (heapType)
	{
	case D3D12_HEAP_TYPE_DEFAULT: m_createdDefaultResources++; break;
	case D3D12_HEAP_TYPE_UPLOAD: m_createdUploadResources++; break;
	case D3D12_HEAP_TYPE_READBACK: m_createdReadbackResources++; break;
	default: break;
	}
	return true;
}

bool D3D12MemoryManager::CreateTexture(const D3D12_RESOURCE_DESC& desc, D3D12_RESOURCE_STATES initialState, const D3D12_CLEAR_VALUE* clearValue, Resource& outResource)
{
	if (!m_allocator)
		return false;

	D3D12MA::ALLOCATION_DESC allocationDesc{};
	allocationDesc.HeapType = D3D12_HEAP_TYPE_DEFAULT;

	D3D12MA::Allocation* allocation = nullptr;
	ID3D12Resource* resource = nullptr;
	const HRESULT hr = m_allocator->CreateResource(&allocationDesc, &desc, initialState, clearValue, &allocation, IID_PPV_ARGS(&resource));
	if (FAILED(hr))
	{
		if (resource)
			resource->Release();
		if (allocation)
			allocation->Release();
		const D3D12_RESOURCE_ALLOCATION_INFO allocationInfo = m_device->GetResourceAllocationInfo(0, 1, &desc);
		cemuLog_log(LogType::Force,
					"D3D12 texture allocation failed: HRESULT 0x{:08x} dim={} format={} size={}x{} depthOrArray={} mipLevels={} sample={} layout={} flags=0x{:08x} state=0x{:08x} allocSize={} alignment={} clear={}",
					(uint32_t)hr,
					(uint32_t)desc.Dimension,
					(uint32_t)desc.Format,
					(unsigned long long)desc.Width,
					desc.Height,
					desc.DepthOrArraySize,
					desc.MipLevels,
					desc.SampleDesc.Count,
					(uint32_t)desc.Layout,
					(uint32_t)desc.Flags,
					(uint32_t)initialState,
					(unsigned long long)allocationInfo.SizeInBytes,
					(unsigned long long)allocationInfo.Alignment,
					clearValue ? 1 : 0);
		LogD3D12DeviceRemovedData(m_device.Get(), hr, "CreateTexture");
		cemuLog_waitForFlush();
		return false;
	}

	outResource.Reset();
	outResource.resource.Attach(resource);
	outResource.allocation.Attach(allocation);
	outResource.size = allocation ? allocation->GetSize() : 0;
	outResource.state = initialState;
	m_createdDefaultResources++;
	return true;
}

bool D3D12MemoryManager::CreateCpuWritableTexture(const D3D12_RESOURCE_DESC& desc, Resource& outResource, bool cacheCoherentUma)
{
	if (!m_device)
		return false;

	D3D12_HEAP_PROPERTIES heapProperties{};
	heapProperties.Type = D3D12_HEAP_TYPE_CUSTOM;
	// WriteToSubresource swizzles on the CPU with scattered stores; into write-combined memory that ran at ~30 MB/s
	// (3.6 s per reload of BOTW's 110 MB terrain array). Cache-coherent UMA allows write-back pages.
	heapProperties.CPUPageProperty = cacheCoherentUma ? D3D12_CPU_PAGE_PROPERTY_WRITE_BACK : D3D12_CPU_PAGE_PROPERTY_WRITE_COMBINE;
	heapProperties.MemoryPoolPreference = D3D12_MEMORY_POOL_L0;
	ID3D12Resource* resource = nullptr;
	const HRESULT hr = m_device->CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&resource));
	if (FAILED(hr))
	{
		cemuLog_log(LogType::Force,
					"D3D12 CPU-writable texture allocation failed: HRESULT 0x{:08x} format={} size={}x{} depthOrArray={} mipLevels={}",
					(uint32_t)hr,
					(uint32_t)desc.Format,
					(unsigned long long)desc.Width,
					desc.Height,
					desc.DepthOrArraySize,
					desc.MipLevels);
		return false;
	}

	outResource.Reset();
	outResource.resource.Attach(resource);
	outResource.size = m_device->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes;
	outResource.state = D3D12_RESOURCE_STATE_COMMON;
	m_createdDefaultResources++;
	return true;
}

D3D12MemoryManager::RingReservation D3D12MemoryManager::AllocateUpload(uint32_t size, uint32_t alignment, uint64_t fenceValue)
{
	if (!m_uploadRing)
		return {};
	return m_uploadRing->Allocate(size, alignment, fenceValue);
}

D3D12MemoryManager::RingReservation D3D12MemoryManager::AllocateReadback(uint32_t size, uint32_t alignment, uint64_t fenceValue)
{
	if (!m_readbackRing)
		return {};
	return m_readbackRing->Allocate(size, alignment, fenceValue);
}

void D3D12MemoryManager::Cleanup(uint64_t completedFence)
{
	if (m_uploadRing)
		m_uploadRing->Cleanup(completedFence);
	if (m_readbackRing)
		m_readbackRing->Cleanup(completedFence);
}

void* D3D12MemoryManager::AcquireTextureUploadBuffer(uint32_t size)
{
	if (m_textureUploadBuffer.size() < size)
		m_textureUploadBuffer.resize(size);
	m_textureUploadBufferInUse = true;
	return m_textureUploadBuffer.data();
}

void D3D12MemoryManager::ReleaseTextureUploadBuffer(uint8_t* mem)
{
	if (!m_textureUploadBuffer.empty() && m_textureUploadBuffer.data() != mem)
		throw std::runtime_error("Released D3D12 texture upload buffer does not match active buffer");
	m_textureUploadBufferInUse = false;
}

uint64_t D3D12MemoryManager::TrimTextureUploadBuffer()
{
	if (m_textureUploadBufferInUse)
		return 0;
	const uint64_t releasedBytes = m_textureUploadBuffer.capacity();
	decltype(m_textureUploadBuffer) empty;
	m_textureUploadBuffer.swap(empty);
	return releasedBytes;
}

D3D12MemoryManager::Stats D3D12MemoryManager::GetStats() const
{
	Stats stats{};
	if (m_uploadRing)
		stats.upload = m_uploadRing->GetStats();
	if (m_readbackRing)
		stats.readback = m_readbackRing->GetStats();
	stats.createdDefaultResources = m_createdDefaultResources;
	stats.createdUploadResources = m_createdUploadResources;
	stats.createdReadbackResources = m_createdReadbackResources;
	stats.bufferAllocationRetries = m_bufferAllocationRetries;
	stats.textureUploadScratchBytes = m_textureUploadBuffer.capacity();
	if (m_allocator)
	{
		D3D12MA::Budget localBudget{};
		m_allocator->GetBudget(&localBudget, nullptr);
		stats.localUsageBytes = localBudget.UsageBytes;
		stats.localBudgetBytes = localBudget.BudgetBytes;
		stats.allocatorBlockBytes = localBudget.Stats.BlockBytes;
		stats.allocatorAllocationBytes = localBudget.Stats.AllocationBytes;
	}
	return stats;
}
