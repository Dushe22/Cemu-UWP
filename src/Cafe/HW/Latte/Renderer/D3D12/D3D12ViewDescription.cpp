#include "Cafe/HW/Latte/Renderer/D3D12/D3D12ViewDescription.h"

std::string D3D12DescribeResource(ID3D12Resource* resource)
{
	if (!resource)
		return "resource=null";
	const D3D12_RESOURCE_DESC desc = resource->GetDesc();
	return fmt::format("resource[dim={} fmt={} {}x{}x{} mips={} samples={} layout={} flags=0x{:x}]",
		(uint32)desc.Dimension, (uint32)desc.Format, (uint64)desc.Width, desc.Height, desc.DepthOrArraySize,
		desc.MipLevels, desc.SampleDesc.Count, (uint32)desc.Layout, (uint32)desc.Flags);
}

std::string D3D12DescribeView(const D3D12_SHADER_RESOURCE_VIEW_DESC* desc)
{
	if (!desc)
		return "srv=default";
	std::string range;
	switch (desc->ViewDimension)
	{
	case D3D12_SRV_DIMENSION_BUFFER:
		range = fmt::format("first={} count={} stride={} flags=0x{:x}", desc->Buffer.FirstElement, desc->Buffer.NumElements,
			desc->Buffer.StructureByteStride, (uint32)desc->Buffer.Flags);
		break;
	case D3D12_SRV_DIMENSION_TEXTURE1D:
		range = fmt::format("mip={}+{}", desc->Texture1D.MostDetailedMip, desc->Texture1D.MipLevels);
		break;
	case D3D12_SRV_DIMENSION_TEXTURE1DARRAY:
		range = fmt::format("mip={}+{} slice={}+{}", desc->Texture1DArray.MostDetailedMip, desc->Texture1DArray.MipLevels,
			desc->Texture1DArray.FirstArraySlice, desc->Texture1DArray.ArraySize);
		break;
	case D3D12_SRV_DIMENSION_TEXTURE2D:
		range = fmt::format("mip={}+{} plane={}", desc->Texture2D.MostDetailedMip, desc->Texture2D.MipLevels, desc->Texture2D.PlaneSlice);
		break;
	case D3D12_SRV_DIMENSION_TEXTURE2DARRAY:
		range = fmt::format("mip={}+{} slice={}+{} plane={}", desc->Texture2DArray.MostDetailedMip, desc->Texture2DArray.MipLevels,
			desc->Texture2DArray.FirstArraySlice, desc->Texture2DArray.ArraySize, desc->Texture2DArray.PlaneSlice);
		break;
	case D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY:
		range = fmt::format("slice={}+{}", desc->Texture2DMSArray.FirstArraySlice, desc->Texture2DMSArray.ArraySize);
		break;
	case D3D12_SRV_DIMENSION_TEXTURE3D:
		range = fmt::format("mip={}+{}", desc->Texture3D.MostDetailedMip, desc->Texture3D.MipLevels);
		break;
	case D3D12_SRV_DIMENSION_TEXTURECUBE:
		range = fmt::format("mip={}+{}", desc->TextureCube.MostDetailedMip, desc->TextureCube.MipLevels);
		break;
	case D3D12_SRV_DIMENSION_TEXTURECUBEARRAY:
		range = fmt::format("mip={}+{} face={} cubes={}", desc->TextureCubeArray.MostDetailedMip, desc->TextureCubeArray.MipLevels,
			desc->TextureCubeArray.First2DArrayFace, desc->TextureCubeArray.NumCubes);
		break;
	default:
		break;
	}
	return fmt::format("srv[fmt={} dim={} mapping=0x{:x} {}]", (uint32)desc->Format, (uint32)desc->ViewDimension,
		desc->Shader4ComponentMapping, range);
}

std::string D3D12DescribeView(const D3D12_RENDER_TARGET_VIEW_DESC* desc)
{
	if (!desc)
		return "rtv=default";
	std::string range;
	switch (desc->ViewDimension)
	{
	case D3D12_RTV_DIMENSION_TEXTURE1D:
		range = fmt::format("mip={}", desc->Texture1D.MipSlice);
		break;
	case D3D12_RTV_DIMENSION_TEXTURE1DARRAY:
		range = fmt::format("mip={} slice={}+{}", desc->Texture1DArray.MipSlice, desc->Texture1DArray.FirstArraySlice, desc->Texture1DArray.ArraySize);
		break;
	case D3D12_RTV_DIMENSION_TEXTURE2D:
		range = fmt::format("mip={} plane={}", desc->Texture2D.MipSlice, desc->Texture2D.PlaneSlice);
		break;
	case D3D12_RTV_DIMENSION_TEXTURE2DARRAY:
		range = fmt::format("mip={} slice={}+{} plane={}", desc->Texture2DArray.MipSlice, desc->Texture2DArray.FirstArraySlice,
			desc->Texture2DArray.ArraySize, desc->Texture2DArray.PlaneSlice);
		break;
	case D3D12_RTV_DIMENSION_TEXTURE2DMSARRAY:
		range = fmt::format("slice={}+{}", desc->Texture2DMSArray.FirstArraySlice, desc->Texture2DMSArray.ArraySize);
		break;
	case D3D12_RTV_DIMENSION_TEXTURE3D:
		range = fmt::format("mip={} w={}+{}", desc->Texture3D.MipSlice, desc->Texture3D.FirstWSlice, desc->Texture3D.WSize);
		break;
	default:
		break;
	}
	return fmt::format("rtv[fmt={} dim={} {}]", (uint32)desc->Format, (uint32)desc->ViewDimension, range);
}

std::string D3D12DescribeView(const D3D12_DEPTH_STENCIL_VIEW_DESC* desc)
{
	if (!desc)
		return "dsv=default";
	std::string range;
	switch (desc->ViewDimension)
	{
	case D3D12_DSV_DIMENSION_TEXTURE1D:
		range = fmt::format("mip={}", desc->Texture1D.MipSlice);
		break;
	case D3D12_DSV_DIMENSION_TEXTURE1DARRAY:
		range = fmt::format("mip={} slice={}+{}", desc->Texture1DArray.MipSlice, desc->Texture1DArray.FirstArraySlice, desc->Texture1DArray.ArraySize);
		break;
	case D3D12_DSV_DIMENSION_TEXTURE2D:
		range = fmt::format("mip={}", desc->Texture2D.MipSlice);
		break;
	case D3D12_DSV_DIMENSION_TEXTURE2DARRAY:
		range = fmt::format("mip={} slice={}+{}", desc->Texture2DArray.MipSlice, desc->Texture2DArray.FirstArraySlice, desc->Texture2DArray.ArraySize);
		break;
	case D3D12_DSV_DIMENSION_TEXTURE2DMSARRAY:
		range = fmt::format("slice={}+{}", desc->Texture2DMSArray.FirstArraySlice, desc->Texture2DMSArray.ArraySize);
		break;
	default:
		break;
	}
	return fmt::format("dsv[fmt={} dim={} flags=0x{:x} {}]", (uint32)desc->Format, (uint32)desc->ViewDimension, (uint32)desc->Flags, range);
}

std::string D3D12DescribeView(const D3D12_UNORDERED_ACCESS_VIEW_DESC* desc)
{
	if (!desc)
		return "uav=default";
	if (desc->ViewDimension == D3D12_UAV_DIMENSION_BUFFER)
	{
		return fmt::format("uav[fmt={} buffer first={} count={} stride={} counter={} flags=0x{:x}]", (uint32)desc->Format,
			desc->Buffer.FirstElement, desc->Buffer.NumElements, desc->Buffer.StructureByteStride,
			desc->Buffer.CounterOffsetInBytes, (uint32)desc->Buffer.Flags);
	}
	return fmt::format("uav[fmt={} dim={}]", (uint32)desc->Format, (uint32)desc->ViewDimension);
}

std::string D3D12DescribeSampler(const D3D12_SAMPLER_DESC* desc)
{
	if (!desc)
		return "sampler=null";
	return fmt::format("sampler[filter=0x{:x} address={}/{}/{} bias={} aniso={} compare={} lod={}..{}]",
		(uint32)desc->Filter, (uint32)desc->AddressU, (uint32)desc->AddressV, (uint32)desc->AddressW, desc->MipLODBias,
		desc->MaxAnisotropy, (uint32)desc->ComparisonFunc, desc->MinLOD, desc->MaxLOD);
}
