#include "Cafe/HW/Latte/Renderer/D3D12/LatteTextureViewD3D12.h"

#include "Cafe/HW/Latte/Renderer/D3D12/D3D12Renderer.h"
#include "Cafe/HW/Latte/Renderer/D3D12/LatteTextureD3D12.h"

#include <algorithm>

namespace
{
struct ViewRange
{
	uint32 first;
	uint32 count;
};

ViewRange ClampViewRange(sint32 requestedFirst, sint32 requestedCount, uint32 availableCount)
{
	availableCount = std::max<uint32>(availableCount, 1);
	cemu_assert_debug(requestedFirst >= 0 && (uint32)requestedFirst < availableCount);
	cemu_assert_debug(requestedCount > 0);

	const uint32 first = std::min<uint32>((uint32)std::max<sint32>(requestedFirst, 0), availableCount - 1);
	const uint32 count = std::min<uint32>((uint32)std::max<sint32>(requestedCount, 1), availableCount - first);
	return {first, count};
}

ViewRange GetMipRange(const LatteTextureViewD3D12* view, const LatteTextureD3D12* texture)
{
	return ClampViewRange(view->firstMip, view->numMip, texture->GetDesc().MipLevels);
}

ViewRange GetArrayRange(const LatteTextureViewD3D12* view, const LatteTextureD3D12* texture)
{
	const auto& resourceDesc = texture->GetDesc();
	cemu_assert_debug(resourceDesc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE3D);
	return ClampViewRange(view->firstSlice, view->numSlice, resourceDesc.DepthOrArraySize);
}

ViewRange GetDepthRange(const LatteTextureViewD3D12* view, const LatteTextureD3D12* texture, uint32 mipSlice)
{
	const auto& resourceDesc = texture->GetDesc();
	cemu_assert_debug(resourceDesc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D);
	const uint32 mipDepth = std::max<uint32>((uint32)resourceDesc.DepthOrArraySize >> mipSlice, 1);
	return ClampViewRange(view->firstSlice, view->numSlice, mipDepth);
}

bool HasResourceDimension(const LatteTextureD3D12* texture, D3D12_RESOURCE_DIMENSION expected)
{
	const bool matches = texture->GetDesc().Dimension == expected;
	cemu_assert_debug(matches);
	return matches;
}

bool IsMultisampled(const LatteTextureD3D12* texture)
{
	return texture->GetDesc().SampleDesc.Count > 1;
}

bool UseArrayViewForSingleSlice(const LatteTextureD3D12* texture, const ViewRange& slices)
{
	return texture->GetDesc().DepthOrArraySize > 1 || slices.first != 0;
}

bool GetCubeRange(const ViewRange& faces, uint32& firstFace, uint32& cubeCount)
{
	const bool valid = faces.count >= 6 && (faces.count % 6) == 0;
	cemu_assert_debug(valid && "cubemap views must contain complete groups of six faces");
	if (!valid)
		return false;

	firstFace = faces.first;
	cubeCount = faces.count / 6;
	return true;
}

void SetTexture1DSrv(D3D12_SHADER_RESOURCE_VIEW_DESC& desc, const ViewRange& mips)
{
	desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1D;
	desc.Texture1D.MostDetailedMip = mips.first;
	desc.Texture1D.MipLevels = mips.count;
}

void SetTexture1DArraySrv(D3D12_SHADER_RESOURCE_VIEW_DESC& desc, const ViewRange& mips, const ViewRange& slices)
{
	desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1DARRAY;
	desc.Texture1DArray.MostDetailedMip = mips.first;
	desc.Texture1DArray.MipLevels = mips.count;
	desc.Texture1DArray.FirstArraySlice = slices.first;
	desc.Texture1DArray.ArraySize = slices.count;
}

void SetTexture2DSrv(D3D12_SHADER_RESOURCE_VIEW_DESC& desc, const ViewRange& mips)
{
	desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	desc.Texture2D.MostDetailedMip = mips.first;
	desc.Texture2D.MipLevels = mips.count;
	desc.Texture2D.PlaneSlice = 0;
}

void SetTexture2DArraySrv(D3D12_SHADER_RESOURCE_VIEW_DESC& desc, const ViewRange& mips, const ViewRange& slices)
{
	desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
	desc.Texture2DArray.MostDetailedMip = mips.first;
	desc.Texture2DArray.MipLevels = mips.count;
	desc.Texture2DArray.FirstArraySlice = slices.first;
	desc.Texture2DArray.ArraySize = slices.count;
	desc.Texture2DArray.PlaneSlice = 0;
}

void SetTexture2DMsSrv(D3D12_SHADER_RESOURCE_VIEW_DESC& desc, const ViewRange& slices, bool useArrayView)
{
	if (useArrayView)
	{
		desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY;
		desc.Texture2DMSArray.FirstArraySlice = slices.first;
		desc.Texture2DMSArray.ArraySize = slices.count;
	}
	else
	{
		desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
	}
}

uint32 AdjustTextureCompSelD3D12(Latte::E_GX2SURFFMT format, uint32 compSel)
{
	switch (format)
	{
	case Latte::E_GX2SURFFMT::R8_UNORM:
	case Latte::E_GX2SURFFMT::R8_SNORM:
		if (compSel >= 1 && compSel <= 3)
			compSel = 0;
		break;
	case Latte::E_GX2SURFFMT::A1_B5_G5_R5_UNORM:
	case Latte::E_GX2SURFFMT::A2_B10_G10_R10_UNORM:
		if (compSel <= 3)
			compSel = 3 - compSel;
		break;
	case Latte::E_GX2SURFFMT::BC4_UNORM:
	case Latte::E_GX2SURFFMT::BC4_SNORM:
		if (compSel >= 1 && compSel <= 3)
			compSel = 0;
		break;
	case Latte::E_GX2SURFFMT::BC5_UNORM:
	case Latte::E_GX2SURFFMT::BC5_SNORM:
		if (compSel == 3)
			compSel = 1;
		break;
	case Latte::E_GX2SURFFMT::X24_G8_UINT:
		if (compSel <= 3)
			compSel = 3;
		break;
	case Latte::E_GX2SURFFMT::R4_G4_UNORM:
		if (compSel == 0)
			compSel = 1;
		else if (compSel == 1)
			compSel = 0;
		break;
	default:
		break;
	}
	return compSel;
}

uint32 D3D12ComponentMappingFromCompSel(uint32 compSel)
{
	switch (compSel)
	{
	case 0:
		return D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_0;
	case 1:
		return D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_1;
	case 2:
		return D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_2;
	case 3:
		return D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_3;
	case 5:
		return D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1;
	case 4:
	case 6:
	case 7:
	default:
		return D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0;
	}
}

uint32 D3D12BuildShaderComponentMapping(Latte::E_GX2SURFFMT format, uint32 gpuSamplerSwizzle)
{
	uint32 compSelR = (gpuSamplerSwizzle >> 16) & 0x7;
	uint32 compSelG = (gpuSamplerSwizzle >> 19) & 0x7;
	uint32 compSelB = (gpuSamplerSwizzle >> 22) & 0x7;
	uint32 compSelA = (gpuSamplerSwizzle >> 25) & 0x7;
	compSelR = AdjustTextureCompSelD3D12(format, compSelR);
	compSelG = AdjustTextureCompSelD3D12(format, compSelG);
	compSelB = AdjustTextureCompSelD3D12(format, compSelB);
	compSelA = AdjustTextureCompSelD3D12(format, compSelA);
	return D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(
		D3D12ComponentMappingFromCompSel(compSelR),
		D3D12ComponentMappingFromCompSel(compSelG),
		D3D12ComponentMappingFromCompSel(compSelB),
		D3D12ComponentMappingFromCompSel(compSelA));
}
} // namespace

LatteTextureViewD3D12::LatteTextureViewD3D12(D3D12Renderer* renderer, LatteTextureD3D12* texture, Latte::E_DIM dim, Latte::E_GX2SURFFMT format, sint32 firstMip, sint32 mipCount, sint32 firstSlice, sint32 sliceCount)
	: LatteTextureView(texture, firstMip, mipCount, firstSlice, sliceCount, dim, format), m_renderer(renderer)
{
	// the host format choice depends on the resource size, not on the viewed mip
	if (texture->overwriteInfo.hasFormatOverwrite)
		m_viewFormatInfo = texture->GetFormatInfo();
	else
		m_viewFormatInfo = GetD3D12FormatInfo(format, texture->isDepth, dim, (sint32)texture->GetDesc().Width, (sint32)texture->GetDesc().Height);

	CreateSamplingProxy();
	CreateSrvDescriptor();
	CreateRtvDescriptor();
	CreateDsvDescriptor();
}

LatteTextureViewD3D12::~LatteTextureViewD3D12()
{
	auto* descriptorManager = m_renderer ? m_renderer->GetDescriptorManager() : nullptr;
	if (m_renderer)
		m_renderer->NotifyTextureViewDescriptorsReleased(this);
	if (!descriptorManager)
		return;
	descriptorManager->FreePersistent(m_srv);
	descriptorManager->FreePersistent(m_rtv);
	descriptorManager->FreePersistent(m_dsv);
	for (const auto& entry : m_samplerSrvs)
		descriptorManager->FreePersistent(entry.second);
	if (m_samplingProxy.IsValid())
		m_renderer->ReleaseResourceDeferred(m_samplingProxy);
}

const D3D12DescriptorManager::Allocation& LatteTextureViewD3D12::GetSRV()
{
	PrepareForSampling();
	return m_srv;
}

bool LatteTextureViewD3D12::CreateSamplingProxy()
{
	auto* texture = GetBaseImage();
	if (!m_renderer || !texture || !texture->GetResource() || texture->GetDesc().Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE3D)
		return false;

	const ViewRange mips = GetMipRange(this, texture);
	const ViewRange depth = GetDepthRange(this, texture, mips.first);
	const uint32 fullDepth = std::max<uint32>((uint32)texture->GetDesc().DepthOrArraySize >> mips.first, 1);
	const bool needsProxy = dim == Latte::E_DIM::DIM_2D ||
						 dim == Latte::E_DIM::DIM_2D_ARRAY ||
						 (dim == Latte::E_DIM::DIM_3D && (depth.first != 0 || depth.count != fullDepth));
	if (!needsProxy)
		return false;
	if (dim != Latte::E_DIM::DIM_2D && dim != Latte::E_DIM::DIM_2D_ARRAY && dim != Latte::E_DIM::DIM_3D)
		return false;

	D3D12_RESOURCE_DESC proxyDesc = texture->GetDesc();
	proxyDesc.Alignment = 0;
	proxyDesc.Width = std::max<UINT64>(proxyDesc.Width >> mips.first, 1);
	proxyDesc.Height = std::max<UINT>(proxyDesc.Height >> mips.first, 1);
	proxyDesc.MipLevels = (UINT16)mips.count;
	proxyDesc.SampleDesc = {1, 0};
	proxyDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	proxyDesc.Flags = D3D12_RESOURCE_FLAG_NONE;
	if (dim == Latte::E_DIM::DIM_3D)
	{
		proxyDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
		proxyDesc.DepthOrArraySize = (UINT16)depth.count;
	}
	else
	{
		proxyDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		proxyDesc.DepthOrArraySize = (UINT16)(dim == Latte::E_DIM::DIM_2D_ARRAY ? depth.count : 1);
	}

	return m_renderer->CreateTextureWithRecovery(proxyDesc, D3D12_RESOURCE_STATE_COMMON, nullptr, m_samplingProxy, "ranged 3D sampling proxy");
}

void LatteTextureViewD3D12::PrepareForSampling()
{
	if (!UsesSamplingProxy() || !m_renderer)
		return;
	auto* texture = GetBaseImage();
	if (!texture)
		return;
	if (m_samplingProxyWriteEvent == texture->lastWriteEventCounter &&
		m_samplingProxyUpdateEvent == texture->lastUpdateEventCounter)
		return;
	if (m_renderer->RefreshRanged3DSamplingProxy(this))
	{
		m_samplingProxyWriteEvent = texture->lastWriteEventCounter;
		m_samplingProxyUpdateEvent = texture->lastUpdateEventCounter;
	}
}

ID3D12Resource* LatteTextureViewD3D12::GetSrvResource() const
{
	return UsesSamplingProxy() ? m_samplingProxy.resource.Get() : GetBaseImage()->GetResource();
}

void LatteTextureViewD3D12::CreateSrvDescriptor()
{
	auto* texture = GetBaseImage();
	if (!m_renderer || !m_renderer->GetDescriptorManager() || !texture->GetResource())
		return;

	D3D12_SHADER_RESOURCE_VIEW_DESC desc = BuildSrvDesc();
	if (desc.Format == DXGI_FORMAT_UNKNOWN)
		return;

	m_srv = m_renderer->GetDescriptorManager()->AllocatePersistent(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	m_renderer->CreateShaderResourceViewChecked(GetSrvResource(), &desc, m_srv.cpu, "texture view", false);
}

uint32 LatteTextureViewD3D12::GetShaderComponentMapping(uint32 gpuSamplerSwizzle) const
{
	return D3D12BuildShaderComponentMapping(format, gpuSamplerSwizzle & 0x0FFF0000);
}

const D3D12DescriptorManager::Allocation& LatteTextureViewD3D12::GetSamplerSRV(uint32 gpuSamplerSwizzle)
{
	PrepareForSampling();
	gpuSamplerSwizzle &= 0x0FFF0000;
	const uint32 componentMapping = GetShaderComponentMapping(gpuSamplerSwizzle);
	if (gpuSamplerSwizzle == 0x06880000 && componentMapping == D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING)
		return m_srv;

	auto it = m_samplerSrvs.find(gpuSamplerSwizzle);
	if (it != m_samplerSrvs.end())
		return it->second;

	auto* texture = GetBaseImage();
	static const D3D12DescriptorManager::Allocation empty{};
	if (!m_renderer || !m_renderer->GetDescriptorManager() || !texture->GetResource())
		return empty;

	D3D12_SHADER_RESOURCE_VIEW_DESC desc = BuildSrvDesc();
	if (desc.Format == DXGI_FORMAT_UNKNOWN)
		return empty;
	desc.Shader4ComponentMapping = componentMapping;

	auto srv = m_renderer->GetDescriptorManager()->AllocatePersistent(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	m_renderer->CreateShaderResourceViewChecked(GetSrvResource(), &desc, srv.cpu, "texture view swizzle", false);
	auto [insertedIt, inserted] = m_samplerSrvs.emplace(gpuSamplerSwizzle, srv);
	(void)inserted;
	return insertedIt->second;
}

D3D12_SHADER_RESOURCE_VIEW_DESC LatteTextureViewD3D12::BuildSrvDesc() const
{
	auto* texture = GetBaseImage();
	D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
	desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	desc.Format = texture->isDepth ? m_viewFormatInfo.depthSrvFormat : m_viewFormatInfo.srvFormat;
	if (desc.Format == DXGI_FORMAT_UNKNOWN)
		return desc;
	ID3D12Resource* srvResource = GetSrvResource();
	const DXGI_FORMAT resourceFormat = srvResource ? srvResource->GetDesc().Format : DXGI_FORMAT_UNKNOWN;
	if (!AreD3D12ViewFormatsCompatible(resourceFormat, desc.Format))
	{
#if defined(CEMU_UWP_D3D12_CORRECTNESS_TELEMETRY)
		m_renderer->RecordIncompatibleTextureView(resourceFormat, desc.Format);
#endif
		desc.Format = DXGI_FORMAT_UNKNOWN;
		return desc;
	}

	if (UsesSamplingProxy())
	{
		const auto proxyDesc = m_samplingProxy.resource->GetDesc();
		switch (dim)
		{
		case Latte::E_DIM::DIM_2D:
			SetTexture2DSrv(desc, {0, proxyDesc.MipLevels});
			break;
		case Latte::E_DIM::DIM_2D_ARRAY:
			SetTexture2DArraySrv(desc, {0, proxyDesc.MipLevels}, {0, proxyDesc.DepthOrArraySize});
			break;
		case Latte::E_DIM::DIM_3D:
			desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
			desc.Texture3D.MostDetailedMip = 0;
			desc.Texture3D.MipLevels = proxyDesc.MipLevels;
			break;
		default:
			desc.Format = DXGI_FORMAT_UNKNOWN;
			break;
		}
		return desc;
	}

	const ViewRange mips = GetMipRange(this, texture);

	switch (dim)
	{
	case Latte::E_DIM::DIM_1D:
	{
		if (!HasResourceDimension(texture, D3D12_RESOURCE_DIMENSION_TEXTURE1D))
			break;
		const ViewRange slices = GetArrayRange(this, texture);
		cemu_assert_debug(numSlice == 1);
		// shaders declare Texture1D; only a non-zero first slice needs an array view
		if (slices.first != 0)
			SetTexture1DArraySrv(desc, mips, {slices.first, 1});
		else
			SetTexture1DSrv(desc, mips);
		break;
	}
	case Latte::E_DIM::DIM_1D_ARRAY:
		if (!HasResourceDimension(texture, D3D12_RESOURCE_DIMENSION_TEXTURE1D))
			break;
		SetTexture1DArraySrv(desc, mips, GetArrayRange(this, texture));
		break;
	case Latte::E_DIM::DIM_2D:
	case Latte::E_DIM::DIM_2D_MSAA:
	{
		if (!HasResourceDimension(texture, D3D12_RESOURCE_DIMENSION_TEXTURE2D))
			break;
		const ViewRange slices = GetArrayRange(this, texture);
		cemu_assert_debug(numSlice == 1);
		const ViewRange selectedSlice{slices.first, 1};
		if (IsMultisampled(texture))
		{
			cemu_assert_debug(mips.first == 0 && mips.count == 1);
			SetTexture2DMsSrv(desc, selectedSlice, UseArrayViewForSingleSlice(texture, selectedSlice));
		}
		else if (selectedSlice.first != 0)
		{
			// shaders declare Texture2D; only a non-zero first slice needs an array view
			SetTexture2DArraySrv(desc, mips, selectedSlice);
		}
		else
		{
			SetTexture2DSrv(desc, mips);
		}
		break;
	}
	case Latte::E_DIM::DIM_2D_ARRAY:
	case Latte::E_DIM::DIM_2D_ARRAY_MSAA:
	{
		if (texture->GetDesc().Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D)
		{
			cemu_assert_debug(false && "D3D12 cannot represent a 2D-array SRV over a 3D texture");
			break;
		}
		if (!HasResourceDimension(texture, D3D12_RESOURCE_DIMENSION_TEXTURE2D))
			break;
		const ViewRange slices = GetArrayRange(this, texture);
		if (IsMultisampled(texture))
		{
			cemu_assert_debug(mips.first == 0 && mips.count == 1);
			SetTexture2DMsSrv(desc, slices, true);
		}
		else
		{
			SetTexture2DArraySrv(desc, mips, slices);
		}
		break;
	}
	case Latte::E_DIM::DIM_3D:
		if (!HasResourceDimension(texture, D3D12_RESOURCE_DIMENSION_TEXTURE3D))
			break;
		cemu_assert_debug(firstSlice == 0 && "D3D12 3D SRVs cannot select a W-slice range");
		desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
		desc.Texture3D.MostDetailedMip = mips.first;
		desc.Texture3D.MipLevels = mips.count;
		break;
	case Latte::E_DIM::DIM_CUBEMAP:
	{
		if (!HasResourceDimension(texture, D3D12_RESOURCE_DIMENSION_TEXTURE2D))
			break;
		cemu_assert_debug(!IsMultisampled(texture) && "D3D12 does not support multisampled cubemap SRVs");
		if (IsMultisampled(texture))
			break;
		uint32 firstFace;
		uint32 cubeCount;
		if (!GetCubeRange(GetArrayRange(this, texture), firstFace, cubeCount))
			break;
		// decompiled HLSL declares cubemaps as TextureCubeArray even for one cube
		desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBEARRAY;
		desc.TextureCubeArray.MostDetailedMip = mips.first;
		desc.TextureCubeArray.MipLevels = mips.count;
		desc.TextureCubeArray.First2DArrayFace = firstFace;
		desc.TextureCubeArray.NumCubes = cubeCount;
		break;
	}
	default:
		cemu_assert_unimplemented();
		break;
	}

	if (desc.ViewDimension == D3D12_SRV_DIMENSION_UNKNOWN)
		desc.Format = DXGI_FORMAT_UNKNOWN;
	return desc;
}

D3D12_SHADER_RESOURCE_VIEW_DESC LatteTextureViewD3D12::BuildSubresourceArraySrvDesc(bool stencilPlane) const
{
	auto* texture = GetBaseImage();
	D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
	desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	desc.Format = m_viewFormatInfo.srvFormat;
	if (texture->isDepth)
		desc.Format = stencilPlane ? m_viewFormatInfo.stencilSrvFormat : m_viewFormatInfo.depthSrvFormat;
	else if (stencilPlane)
		desc.Format = DXGI_FORMAT_UNKNOWN;
	ID3D12Resource* srvResource = GetSrvResource();
	if (!srvResource)
	{
		desc.Format = DXGI_FORMAT_UNKNOWN;
		return desc;
	}
	const D3D12_RESOURCE_DESC resourceDesc = srvResource->GetDesc();
	if (!AreD3D12ViewFormatsCompatible(resourceDesc.Format, desc.Format) ||
		resourceDesc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || resourceDesc.SampleDesc.Count != 1)
	{
		desc.Format = DXGI_FORMAT_UNKNOWN;
		return desc;
	}

	// a sampling proxy starts at the view's first mip and slice
	const ViewRange mips = UsesSamplingProxy() ? ViewRange{0, 1} : GetMipRange(this, texture);
	const ViewRange slices = UsesSamplingProxy() ? ViewRange{0, 1} : GetArrayRange(this, texture);
	SetTexture2DArraySrv(desc, {mips.first, 1}, {slices.first, 1});
	desc.Texture2DArray.PlaneSlice = stencilPlane ? 1 : 0;
	return desc;
}

void LatteTextureViewD3D12::CreateRtvDescriptor()
{
	auto* texture = GetBaseImage();
	if (texture->isDepth || !m_viewFormatInfo.supportsRenderTarget || m_viewFormatInfo.rtvFormat == DXGI_FORMAT_UNKNOWN)
		return;
	if (!m_renderer || !m_renderer->GetDescriptorManager() || !texture->GetResource())
		return;

	D3D12_RENDER_TARGET_VIEW_DESC desc{};
	desc.Format = m_viewFormatInfo.rtvFormat;
	const auto& resourceDesc = texture->GetDesc();
	if (!AreD3D12ViewFormatsCompatible(resourceDesc.Format, desc.Format))
	{
#if defined(CEMU_UWP_D3D12_CORRECTNESS_TELEMETRY)
		m_renderer->RecordIncompatibleTextureView(resourceDesc.Format, desc.Format);
#endif
		return;
	}
	const ViewRange mips = GetMipRange(this, texture);

	if (resourceDesc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D)
	{
		if (dim != Latte::E_DIM::DIM_3D && dim != Latte::E_DIM::DIM_2D_ARRAY)
		{
			cemu_assert_debug(false && "unsupported D3D12 view dimension for a 3D render target");
			return;
		}
		const ViewRange depth = GetDepthRange(this, texture, mips.first);
		desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE3D;
		desc.Texture3D.MipSlice = mips.first;
		desc.Texture3D.FirstWSlice = depth.first;
		desc.Texture3D.WSize = depth.count;
		m_rtv = m_renderer->GetDescriptorManager()->AllocatePersistent(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
		m_renderer->CreateRenderTargetViewChecked(texture->GetResource(), &desc, m_rtv.cpu, "texture view 3D");
		return;
	}

	const ViewRange slices = GetArrayRange(this, texture);
	const bool multisampled = IsMultisampled(texture);

	switch (dim)
	{
	case Latte::E_DIM::DIM_1D:
		if (!HasResourceDimension(texture, D3D12_RESOURCE_DIMENSION_TEXTURE1D))
			return;
		cemu_assert_debug(numSlice == 1);
		if (UseArrayViewForSingleSlice(texture, slices))
		{
			desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE1DARRAY;
			desc.Texture1DArray.MipSlice = mips.first;
			desc.Texture1DArray.FirstArraySlice = slices.first;
			desc.Texture1DArray.ArraySize = 1;
		}
		else
		{
			desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE1D;
			desc.Texture1D.MipSlice = mips.first;
		}
		break;
	case Latte::E_DIM::DIM_1D_ARRAY:
		if (!HasResourceDimension(texture, D3D12_RESOURCE_DIMENSION_TEXTURE1D))
			return;
		desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE1DARRAY;
		desc.Texture1DArray.MipSlice = mips.first;
		desc.Texture1DArray.FirstArraySlice = slices.first;
		desc.Texture1DArray.ArraySize = slices.count;
		break;
	case Latte::E_DIM::DIM_2D:
	case Latte::E_DIM::DIM_2D_MSAA:
		if (!HasResourceDimension(texture, D3D12_RESOURCE_DIMENSION_TEXTURE2D))
			return;
		cemu_assert_debug(numSlice == 1);
		if (multisampled)
		{
			cemu_assert_debug(mips.first == 0 && mips.count == 1);
			if (UseArrayViewForSingleSlice(texture, slices))
			{
				desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMSARRAY;
				desc.Texture2DMSArray.FirstArraySlice = slices.first;
				desc.Texture2DMSArray.ArraySize = 1;
			}
			else
			{
				desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMS;
			}
		}
		else if (UseArrayViewForSingleSlice(texture, slices))
		{
			desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
			desc.Texture2DArray.MipSlice = mips.first;
			desc.Texture2DArray.FirstArraySlice = slices.first;
			desc.Texture2DArray.ArraySize = 1;
			desc.Texture2DArray.PlaneSlice = 0;
		}
		else
		{
			desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
			desc.Texture2D.MipSlice = mips.first;
			desc.Texture2D.PlaneSlice = 0;
		}
		break;
	case Latte::E_DIM::DIM_2D_ARRAY:
	case Latte::E_DIM::DIM_2D_ARRAY_MSAA:
	case Latte::E_DIM::DIM_CUBEMAP:
		if (!HasResourceDimension(texture, D3D12_RESOURCE_DIMENSION_TEXTURE2D))
			return;
		if (multisampled)
		{
			cemu_assert_debug(mips.first == 0 && mips.count == 1);
			desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMSARRAY;
			desc.Texture2DMSArray.FirstArraySlice = slices.first;
			desc.Texture2DMSArray.ArraySize = slices.count;
		}
		else
		{
			desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
			desc.Texture2DArray.MipSlice = mips.first;
			desc.Texture2DArray.FirstArraySlice = slices.first;
			desc.Texture2DArray.ArraySize = slices.count;
			desc.Texture2DArray.PlaneSlice = 0;
		}
		break;
	default:
		return;
	}

	m_rtv = m_renderer->GetDescriptorManager()->AllocatePersistent(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
	m_renderer->CreateRenderTargetViewChecked(texture->GetResource(), &desc, m_rtv.cpu, "texture view");
}

void LatteTextureViewD3D12::CreateDsvDescriptor()
{
	auto* texture = GetBaseImage();
	if (!texture->isDepth || !m_viewFormatInfo.supportsDepthStencil || m_viewFormatInfo.dsvFormat == DXGI_FORMAT_UNKNOWN)
		return;
	if (!m_renderer || !m_renderer->GetDescriptorManager() || !texture->GetResource())
		return;

	D3D12_DEPTH_STENCIL_VIEW_DESC desc{};
	desc.Format = m_viewFormatInfo.dsvFormat;
	const auto& resourceDesc = texture->GetDesc();
	if (!AreD3D12ViewFormatsCompatible(resourceDesc.Format, desc.Format))
	{
#if defined(CEMU_UWP_D3D12_CORRECTNESS_TELEMETRY)
		m_renderer->RecordIncompatibleTextureView(resourceDesc.Format, desc.Format);
#endif
		return;
	}
	if (resourceDesc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D)
	{
		cemu_assert_debug(false && "D3D12 does not support depth-stencil views of 3D textures");
		return;
	}
	const ViewRange mips = GetMipRange(this, texture);
	const ViewRange slices = GetArrayRange(this, texture);
	const bool multisampled = IsMultisampled(texture);

	switch (dim)
	{
	case Latte::E_DIM::DIM_1D:
		if (!HasResourceDimension(texture, D3D12_RESOURCE_DIMENSION_TEXTURE1D))
			return;
		cemu_assert_debug(numSlice == 1);
		if (UseArrayViewForSingleSlice(texture, slices))
		{
			desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE1DARRAY;
			desc.Texture1DArray.MipSlice = mips.first;
			desc.Texture1DArray.FirstArraySlice = slices.first;
			desc.Texture1DArray.ArraySize = 1;
		}
		else
		{
			desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE1D;
			desc.Texture1D.MipSlice = mips.first;
		}
		break;
	case Latte::E_DIM::DIM_1D_ARRAY:
		if (!HasResourceDimension(texture, D3D12_RESOURCE_DIMENSION_TEXTURE1D))
			return;
		desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE1DARRAY;
		desc.Texture1DArray.MipSlice = mips.first;
		desc.Texture1DArray.FirstArraySlice = slices.first;
		desc.Texture1DArray.ArraySize = slices.count;
		break;
	case Latte::E_DIM::DIM_2D:
	case Latte::E_DIM::DIM_2D_MSAA:
		if (!HasResourceDimension(texture, D3D12_RESOURCE_DIMENSION_TEXTURE2D))
			return;
		cemu_assert_debug(numSlice == 1);
		if (multisampled)
		{
			cemu_assert_debug(mips.first == 0 && mips.count == 1);
			if (UseArrayViewForSingleSlice(texture, slices))
			{
				desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DMSARRAY;
				desc.Texture2DMSArray.FirstArraySlice = slices.first;
				desc.Texture2DMSArray.ArraySize = 1;
			}
			else
			{
				desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DMS;
			}
		}
		else if (UseArrayViewForSingleSlice(texture, slices))
		{
			desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
			desc.Texture2DArray.MipSlice = mips.first;
			desc.Texture2DArray.FirstArraySlice = slices.first;
			desc.Texture2DArray.ArraySize = 1;
		}
		else
		{
			desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
			desc.Texture2D.MipSlice = mips.first;
		}
		break;
	case Latte::E_DIM::DIM_2D_ARRAY:
	case Latte::E_DIM::DIM_2D_ARRAY_MSAA:
	case Latte::E_DIM::DIM_CUBEMAP:
		if (!HasResourceDimension(texture, D3D12_RESOURCE_DIMENSION_TEXTURE2D))
			return;
		if (multisampled)
		{
			cemu_assert_debug(mips.first == 0 && mips.count == 1);
			desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DMSARRAY;
			desc.Texture2DMSArray.FirstArraySlice = slices.first;
			desc.Texture2DMSArray.ArraySize = slices.count;
		}
		else
		{
			desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
			desc.Texture2DArray.MipSlice = mips.first;
			desc.Texture2DArray.FirstArraySlice = slices.first;
			desc.Texture2DArray.ArraySize = slices.count;
		}
		break;
	default:
		return;
	}

	m_dsv = m_renderer->GetDescriptorManager()->AllocatePersistent(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
	m_renderer->CreateDepthStencilViewChecked(texture->GetResource(), &desc, m_dsv.cpu, "texture view");
}
