#pragma once

#include "Cafe/HW/Latte/LegacyShaderDecompiler/LatteDecompiler.h"
#include "Cafe/HW/Latte/Core/LatteCachedFBO.h"

#if BOOST_OS_WINDOWS
#include <d3d12.h>
#include <dxgiformat.h>
#endif

#include <array>

class LatteTextureViewD3D12;

class CachedFBOD3D12 : public LatteCachedFBO
{
public:
	struct ColorAttachment
	{
		LatteTextureViewD3D12* view{};
		D3D12_CPU_DESCRIPTOR_HANDLE rtv{};
		DXGI_FORMAT format{ DXGI_FORMAT_UNKNOWN };
		bool valid{};
	};

	CachedFBOD3D12(uint64 key);
	~CachedFBOD3D12() override = default;

	const std::array<ColorAttachment, 8>& GetColorAttachments() const { return m_colorAttachments; }
	const std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 8>& GetRtvHandles() const { return m_rtvHandles; }
	D3D12_CPU_DESCRIPTOR_HANDLE GetDsvHandle() const { return m_dsvHandle; }
	DXGI_FORMAT GetDepthFormat() const { return m_depthFormat; }
	DXGI_FORMAT GetStencilFormat() const { return m_stencilFormat; }
	uint32 GetColorAttachmentCount() const { return m_colorAttachmentCount; }
	uint32 GetColorAttachmentMask() const { return m_colorAttachmentMask; }
	bool HasDepthStencilAttachment() const { return m_hasDepthStencilAttachment; }
	bool HasSparseColorAttachments() const { return m_hasSparseColorAttachments; }

private:
	std::array<ColorAttachment, 8> m_colorAttachments{};
	std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 8> m_rtvHandles{};
	D3D12_CPU_DESCRIPTOR_HANDLE m_dsvHandle{};
	DXGI_FORMAT m_depthFormat{ DXGI_FORMAT_UNKNOWN };
	DXGI_FORMAT m_stencilFormat{ DXGI_FORMAT_UNKNOWN };
	uint32 m_colorAttachmentCount{};
	uint32 m_colorAttachmentMask{};
	bool m_hasDepthStencilAttachment{};
	bool m_hasSparseColorAttachments{};
};
