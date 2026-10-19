#pragma once

#include "Cafe/HW/Latte/Core/LatteTexture.h"
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12DescriptorManager.h"
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12Formats.h"
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12MemoryManager.h"

#include <unordered_map>

class D3D12Renderer;
class LatteTextureD3D12;

class LatteTextureViewD3D12 : public LatteTextureView
{
public:
	LatteTextureViewD3D12(D3D12Renderer* renderer, LatteTextureD3D12* texture, Latte::E_DIM dim, Latte::E_GX2SURFFMT format, sint32 firstMip, sint32 mipCount, sint32 firstSlice, sint32 sliceCount);
	~LatteTextureViewD3D12() override;

	const D3D12DescriptorManager::Allocation& GetSRV();
	const D3D12DescriptorManager::Allocation& GetSamplerSRV(uint32 gpuSamplerSwizzle);
	const D3D12DescriptorManager::Allocation& GetRTV() const { return m_rtv; }
	const D3D12DescriptorManager::Allocation& GetDSV() const { return m_dsv; }
	DXGI_FORMAT GetViewFormat() const { return m_viewFormatInfo.srvFormat; }
	DXGI_FORMAT GetRTVFormat() const { return m_viewFormatInfo.rtvFormat; }
	LatteTextureD3D12* GetBaseImage() const { return (LatteTextureD3D12*)baseTexture; }
	D3D12_SHADER_RESOURCE_VIEW_DESC BuildSrvDesc() const;
	D3D12_SHADER_RESOURCE_VIEW_DESC BuildSubresourceArraySrvDesc(bool stencilPlane) const;
	uint32 GetShaderComponentMapping(uint32 gpuSamplerSwizzle) const;

private:
	void CreateSrvDescriptor();
	void CreateRtvDescriptor();
	void CreateDsvDescriptor();
	bool CreateSamplingProxy();
	void PrepareForSampling();
	ID3D12Resource* GetSrvResource() const;
	bool UsesSamplingProxy() const { return m_samplingProxy.IsValid(); }

	friend class D3D12Renderer;

	D3D12Renderer* m_renderer{};
	D3D12FormatInfo m_viewFormatInfo{};
	D3D12DescriptorManager::Allocation m_srv;
	D3D12DescriptorManager::Allocation m_rtv;
	D3D12DescriptorManager::Allocation m_dsv;
	std::unordered_map<uint32, D3D12DescriptorManager::Allocation> m_samplerSrvs;
	D3D12MemoryManager::Resource m_samplingProxy;
	uint64 m_samplingProxyWriteEvent{~0ull};
	uint64 m_samplingProxyUpdateEvent{~0ull};
};
