#pragma once

#include "Cafe/HW/Latte/Core/LatteTexture.h"
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12Formats.h"
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12MemoryManager.h"

#include <vector>

class D3D12Renderer;

class LatteTextureD3D12 : public LatteTexture
{
public:
	LatteTextureD3D12(D3D12Renderer* renderer, Latte::E_DIM dim, MPTR physAddress, MPTR physMipAddress, Latte::E_GX2SURFFMT format, uint32 width, uint32 height, uint32 depth, uint32 pitch, uint32 mipLevels,
		uint32 swizzle, Latte::E_HWTILEMODE tileMode, bool isDepth);
	~LatteTextureD3D12() override;

	void AllocateOnHost() override;

	ID3D12Resource* GetResource() const { return m_resource.resource.Get(); }
	const D3D12_RESOURCE_DESC& GetDesc() const { return m_desc; }
	const D3D12FormatInfo& GetFormatInfo() const { return m_formatInfo; }
	DXGI_FORMAT GetResourceFormat() const { return m_formatInfo.resourceFormat; }

	uint32 GetSubresourceIndex(sint32 sliceIndex, sint32 mipIndex) const;
	D3D12_RESOURCE_STATES GetSubresourceState(uint32 subresource) const;
	void SetSubresourceState(uint32 subresource, D3D12_RESOURCE_STATES state);
	void SetAllSubresourceStates(D3D12_RESOURCE_STATES state);
	bool AllSubresourcesInState(D3D12_RESOURCE_STATES state) const { return m_uniformStateValid && m_uniformState == state; }
	uint32 GetSubresourceCount() const { return (uint32)m_subresourceStates.size(); }
	uint32 GetArraySize() const { return m_arraySize; }
	uint64 GetContentIdentity() const { return m_contentIdentity; }
	uint64 GetAllocationSize() const { return m_resource.size; }
	// uploaded with WriteToSubresource instead of a buffer copy
	bool IsCpuWritable() const { return m_cpuWritable; }
	uint64 GetContentGeneration(uint32 subresource) const;
	void MarkSubresourceWritten(uint32 subresource);
	void MarkAllSubresourcesWritten();

	uint64 m_d3d12FlushIndex{};
	uint64 m_d3d12FlushIndex_read{};
	uint64 m_d3d12FlushIndex_write{};
	uint32 m_collisionCheckIndex{};
	uint32 m_cpuWriteSyncedReload{};

protected:
	LatteTextureView* CreateView(Latte::E_DIM dim, Latte::E_GX2SURFFMT format, sint32 firstMip, sint32 mipCount, sint32 firstSlice, sint32 sliceCount) override;

private:
	D3D12Renderer* m_renderer{};
	D3D12MemoryManager::Resource m_resource;
	D3D12_RESOURCE_DESC m_desc{};
	D3D12FormatInfo m_formatInfo{};
	std::vector<D3D12_RESOURCE_STATES> m_subresourceStates;
	bool m_uniformStateValid{ true };
	D3D12_RESOURCE_STATES m_uniformState{ D3D12_RESOURCE_STATE_COMMON };
	uint32 m_arraySize{ 1 };
	bool m_cpuWritable{};
	uint64 m_contentIdentity{};
	std::vector<uint64> m_contentGenerations;
};
