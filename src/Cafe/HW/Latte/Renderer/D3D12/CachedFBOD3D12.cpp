#include "Cafe/HW/Latte/Renderer/D3D12/CachedFBOD3D12.h"

#include "Cafe/HW/Latte/Renderer/D3D12/LatteTextureD3D12.h"
#include "Cafe/HW/Latte/Renderer/D3D12/LatteTextureViewD3D12.h"

CachedFBOD3D12::CachedFBOD3D12(uint64 key)
	: LatteCachedFBO(key)
{
	uint32 compactColorAttachmentCount = 0;
	for (uint32 i = 0; i < 8; i++)
	{
		auto* view = static_cast<LatteTextureViewD3D12*>(colorBuffer[i].texture);
		if (!view || !view->GetRTV().IsValid())
			continue;

		m_colorAttachments[i].view = view;
		m_colorAttachments[i].rtv = view->GetRTV().cpu;
		m_colorAttachments[i].format = view->GetRTVFormat();
		m_colorAttachments[i].valid = true;
		m_rtvHandles[i] = view->GetRTV().cpu;
		m_colorAttachmentMask |= (1u << i);
		m_colorAttachmentCount = i + 1;
		if (i != compactColorAttachmentCount)
			m_hasSparseColorAttachments = true;
		compactColorAttachmentCount++;
	}

	if (depthBuffer.texture)
	{
		auto* depthView = static_cast<LatteTextureViewD3D12*>(depthBuffer.texture);
		if (depthView && depthView->GetDSV().IsValid())
		{
			const auto* depthTexture = depthView->GetBaseImage();
			m_dsvHandle = depthView->GetDSV().cpu;
			m_depthFormat = depthTexture->GetFormatInfo().dsvFormat;
			m_stencilFormat = depthBuffer.hasStencil ? depthTexture->GetFormatInfo().dsvFormat : DXGI_FORMAT_UNKNOWN;
			m_hasDepthStencilAttachment = true;
		}
	}
}
