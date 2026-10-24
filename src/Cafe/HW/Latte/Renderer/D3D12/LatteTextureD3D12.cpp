#include "Cafe/HW/Latte/Renderer/D3D12/LatteTextureD3D12.h"

#include "Cafe/HW/Latte/Renderer/D3D12/D3D12Renderer.h"
#include "Cafe/HW/Latte/Renderer/D3D12/LatteTextureViewD3D12.h"
#include "config/CemuConfig.h"

#include <algorithm>
#include <atomic>
#include <stdexcept>

namespace
{
std::atomic<uint64> s_nextD3D12TextureIdentity{ 1 };

D3D12_RESOURCE_DIMENSION GetResourceDimension(Latte::E_DIM dim)
{
	switch (dim)
	{
	case Latte::E_DIM::DIM_1D:
	case Latte::E_DIM::DIM_1D_ARRAY:
		return D3D12_RESOURCE_DIMENSION_TEXTURE1D;
	case Latte::E_DIM::DIM_3D:
		return D3D12_RESOURCE_DIMENSION_TEXTURE3D;
	case Latte::E_DIM::DIM_2D:
	case Latte::E_DIM::DIM_2D_ARRAY:
	case Latte::E_DIM::DIM_2D_MSAA:
	case Latte::E_DIM::DIM_2D_ARRAY_MSAA:
	case Latte::E_DIM::DIM_CUBEMAP:
	default:
		return D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	}
}

bool IsBlockCompressedFormat(DXGI_FORMAT format)
{
	return (format >= DXGI_FORMAT_BC1_TYPELESS && format <= DXGI_FORMAT_BC5_SNORM) ||
		(format >= DXGI_FORMAT_BC6H_TYPELESS && format <= DXGI_FORMAT_BC7_UNORM_SRGB);
}
}

LatteTextureD3D12::LatteTextureD3D12(D3D12Renderer* renderer, Latte::E_DIM dim, MPTR physAddress, MPTR physMipAddress, Latte::E_GX2SURFFMT format, uint32 width, uint32 height, uint32 depth, uint32 pitch, uint32 mipLevels,
	uint32 swizzle, Latte::E_HWTILEMODE tileMode, bool isDepth)
	: LatteTexture(dim, physAddress, physMipAddress, format, width, height, depth, pitch, mipLevels, swizzle, tileMode, isDepth), m_renderer(renderer)
{
	Latte::E_GX2SURFFMT resourceFormat = format;
	if (overwriteInfo.hasFormatOverwrite)
		resourceFormat = (Latte::E_GX2SURFFMT)overwriteInfo.format;

	sint32 effectiveBaseWidth = width;
	sint32 effectiveBaseHeight = height;
	sint32 effectiveBaseDepth = depth;
	if (overwriteInfo.hasResolutionOverwrite)
	{
		effectiveBaseWidth = overwriteInfo.width;
		effectiveBaseHeight = overwriteInfo.height;
		effectiveBaseDepth = overwriteInfo.depth;
	}
	effectiveBaseWidth = std::max(1, effectiveBaseWidth);
	effectiveBaseHeight = std::max(1, effectiveBaseHeight);
	effectiveBaseDepth = std::max(1, effectiveBaseDepth);

	// the resource holds at most the mips its size allows; mipLevels keeps the guest count that texture cache lookups compare against
	const sint32 hostMipLevels = std::min<sint32>(std::max<sint32>(this->mipLevels, 1), std::max<sint32>(maxPossibleMipLevels, 1));

	m_formatInfo = GetD3D12FormatInfo(resourceFormat, isDepth, dim, effectiveBaseWidth, effectiveBaseHeight);
	m_arraySize = Is3DTexture() ? 1u : (uint32)effectiveBaseDepth;

	m_desc.Dimension = GetResourceDimension(dim);
	m_desc.Alignment = 0;
	m_desc.Width = (UINT64)effectiveBaseWidth;
	m_desc.Height = (dim == Latte::E_DIM::DIM_1D || dim == Latte::E_DIM::DIM_1D_ARRAY) ? 1u : (UINT)effectiveBaseHeight;
	m_desc.DepthOrArraySize = (UINT16)(Is3DTexture() ? effectiveBaseDepth : m_arraySize);
	m_desc.MipLevels = (UINT16)hostMipLevels;
	m_desc.Format = m_formatInfo.resourceFormat;
	m_desc.SampleDesc.Count = 1;
	m_desc.SampleDesc.Quality = 0;
	m_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	m_desc.Flags = m_formatInfo.resourceFlags;

	const uint32 max2DSize = 16384;
	const uint32 max3DSize = 2048;
	const uint32 maxArraySize = 2048;
	if (m_desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D)
	{
		m_desc.Width = std::min<UINT64>(m_desc.Width, 16384);
		m_desc.Height = 1;
		m_desc.DepthOrArraySize = (UINT16)std::clamp<uint32>(m_desc.DepthOrArraySize, 1, maxArraySize);
	}
	else if (m_desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D)
	{
		m_desc.Width = std::min<UINT64>(m_desc.Width, max2DSize);
		m_desc.Height = std::min<UINT>(std::max<UINT>(m_desc.Height, 1), max2DSize);
		m_desc.DepthOrArraySize = (UINT16)std::clamp<uint32>(m_desc.DepthOrArraySize, 1, maxArraySize);
	}
	else if (m_desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D)
	{
		m_desc.Width = std::min<UINT64>(m_desc.Width, max3DSize);
		m_desc.Height = std::min<UINT>(std::max<UINT>(m_desc.Height, 1), max3DSize);
		m_desc.DepthOrArraySize = (UINT16)std::clamp<uint32>(m_desc.DepthOrArraySize, 1, max3DSize);
	}
	m_desc.MipLevels = (UINT16)std::max<UINT16>(m_desc.MipLevels, 1);

	D3D12_CLEAR_VALUE optimizedClearValue{};
	D3D12_CLEAR_VALUE* optimizedClearValuePtr = nullptr;
	if ((m_desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) != 0 && m_formatInfo.dsvFormat != DXGI_FORMAT_UNKNOWN)
	{
		optimizedClearValue.Format = m_formatInfo.dsvFormat;
		optimizedClearValue.DepthStencil.Depth = 1.0f;
		optimizedClearValue.DepthStencil.Stencil = 0;
		optimizedClearValuePtr = &optimizedClearValue;
	}
	else if ((m_desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) != 0 && m_formatInfo.rtvFormat != DXGI_FORMAT_UNKNOWN)
	{
		optimizedClearValue.Format = m_formatInfo.rtvFormat;
		optimizedClearValue.Color[0] = 0.0f;
		optimizedClearValue.Color[1] = 0.0f;
		optimizedClearValue.Color[2] = 0.0f;
		optimizedClearValue.Color[3] = 1.0f;
		optimizedClearValuePtr = &optimizedClearValue;
	}

	bool textureCreated = false;
	// buffer copies place array slices above 0 of block-compressed textures wrongly on Xbox, CPU writes place them correctly
	if (m_renderer && m_renderer->GetMemoryManager() && m_renderer->IsUma() &&
		m_desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && m_desc.DepthOrArraySize > 1 &&
		m_desc.Flags == D3D12_RESOURCE_FLAG_NONE && IsBlockCompressedFormat(m_desc.Format) &&
		!GetConfig().d3d12_gpu_array_upload) // that path uploads through a single-slice staging texture instead
	{
		m_cpuWritable = m_renderer->GetMemoryManager()->CreateCpuWritableTexture(m_desc, m_resource, m_renderer->IsCacheCoherentUma());
		textureCreated = m_cpuWritable;
	}
	if (!textureCreated && m_renderer && m_renderer->GetMemoryManager())
	{
		textureCreated = m_renderer->CreateTextureWithRecovery(m_desc, D3D12_RESOURCE_STATE_COMMON, optimizedClearValuePtr, m_resource, "latte texture");
	}
	if (!textureCreated)
		throw std::runtime_error("Failed to create D3D12 Latte texture");

#ifdef CEMU_DEBUG_ASSERT
	if (m_resource.resource)
		m_resource.resource->SetName(L"Cemu D3D12 Latte texture");
#endif

	const uint32 planeCount = m_formatInfo.hasStencil ? 2u : 1u;
	const uint32 subresourceCount = (uint32)m_desc.MipLevels * m_arraySize * planeCount;
	m_subresourceStates.resize(std::max<uint32>(subresourceCount, 1), D3D12_RESOURCE_STATE_COMMON);
	m_contentIdentity = s_nextD3D12TextureIdentity.fetch_add(1, std::memory_order_relaxed);
	if (m_contentIdentity == 0)
		m_contentIdentity = s_nextD3D12TextureIdentity.fetch_add(1, std::memory_order_relaxed);
	m_contentGenerations.resize(m_subresourceStates.size(), 1);
}

LatteTextureD3D12::~LatteTextureD3D12()
{
	cemu_assert_debug(views.empty());
	if (m_renderer)
		m_renderer->ReleaseResourceDeferred(m_resource);
}

void LatteTextureD3D12::AllocateOnHost()
{
	// resources are allocated in the constructor through D3D12MA
}

uint32 LatteTextureD3D12::GetSubresourceIndex(sint32 sliceIndex, sint32 mipIndex) const
{
	cemu_assert_debug(mipIndex >= 0 && mipIndex < mipLevels);
	// guest mips the resource cannot hold resolve to its smallest mip
	const uint32 hostMipLevels = std::max<uint32>(m_desc.MipLevels, 1);
	const uint32 mip = std::min<uint32>((uint32)std::max<sint32>(mipIndex, 0), hostMipLevels - 1);
	if (Is3DTexture())
		return mip;

	cemu_assert_debug(sliceIndex >= 0 && (uint32)sliceIndex < m_arraySize);
	return mip + (uint32)sliceIndex * hostMipLevels;
}

D3D12_RESOURCE_STATES LatteTextureD3D12::GetSubresourceState(uint32 subresource) const
{
	cemu_assert_debug(subresource < m_subresourceStates.size());
	return m_subresourceStates[subresource];
}

void LatteTextureD3D12::SetSubresourceState(uint32 subresource, D3D12_RESOURCE_STATES state)
{
	cemu_assert_debug(subresource < m_subresourceStates.size());
	m_subresourceStates[subresource] = state;
	if (m_uniformStateValid && state != m_uniformState)
		m_uniformStateValid = m_subresourceStates.size() == 1;
	if (m_subresourceStates.size() == 1)
		m_uniformState = state;
}

void LatteTextureD3D12::TryRestoreUniformState(D3D12_RESOURCE_STATES state)
{
	if (m_uniformStateValid && m_uniformState == state)
		return;
	for (const D3D12_RESOURCE_STATES subresourceState : m_subresourceStates)
	{
		if (subresourceState != state)
			return;
	}
	m_uniformStateValid = true;
	m_uniformState = state;
}

void LatteTextureD3D12::SetAllSubresourceStates(D3D12_RESOURCE_STATES state)
{
	std::fill(m_subresourceStates.begin(), m_subresourceStates.end(), state);
	m_resource.state = state;
	m_uniformStateValid = true;
	m_uniformState = state;
}

uint64 LatteTextureD3D12::GetContentGeneration(uint32 subresource) const
{
	if (subresource >= m_contentGenerations.size())
		return 0;
	return m_contentGenerations[subresource];
}

void LatteTextureD3D12::MarkSubresourceWritten(uint32 subresource)
{
	if (subresource >= m_contentGenerations.size())
		return;
	auto& generation = m_contentGenerations[subresource];
	if (++generation == 0)
		generation = 1;
}

void LatteTextureD3D12::MarkAllSubresourcesWritten()
{
	for (uint32 subresource = 0; subresource < m_contentGenerations.size(); ++subresource)
		MarkSubresourceWritten(subresource);
}

LatteTextureView* LatteTextureD3D12::CreateView(Latte::E_DIM dim, Latte::E_GX2SURFFMT format, sint32 firstMip, sint32 mipCount, sint32 firstSlice, sint32 sliceCount)
{
	// views allocate descriptors from the recorder's descriptor manager (Phase 4 S3)
	D3D12Renderer::RecordingExclusiveScope recordingScope(m_renderer);
	cemu_assert_debug(mipCount > 0);
	cemu_assert_debug(sliceCount > 0);
	cemu_assert_debug((firstMip + mipCount) <= this->mipLevels);
	cemu_assert_debug(Is3DTexture() || (firstSlice + sliceCount) <= this->depth);
	return new LatteTextureViewD3D12(m_renderer, this, dim, format, firstMip, mipCount, firstSlice, sliceCount);
}
