#include "Cafe/HW/Latte/Renderer/D3D12/D3D12Formats.h"

#include "Cafe/HW/Latte/Core/LatteTextureLoader.h"

#include <cmath>

namespace
{
void SetColorFormat(D3D12FormatInfo& info, DXGI_FORMAT format, TextureDecoder* decoder, bool renderTarget = true)
{
	info.resourceFormat = GetD3D12TypelessFormat(format);
	info.srvFormat = format;
	info.rtvFormat = format;
	info.decoder = decoder;
	info.supportsRenderTarget = renderTarget;
	if (renderTarget)
		info.resourceFlags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
}

void SetDepthFormat(D3D12FormatInfo& info, DXGI_FORMAT resourceFormat, DXGI_FORMAT dsvFormat, DXGI_FORMAT depthSrvFormat, DXGI_FORMAT stencilSrvFormat, TextureDecoder* decoder)
{
	info.isDepth = true;
	info.hasStencil = stencilSrvFormat != DXGI_FORMAT_UNKNOWN;
	info.resourceFormat = resourceFormat;
	info.srvFormat = depthSrvFormat;
	info.dsvFormat = dsvFormat;
	info.depthSrvFormat = depthSrvFormat;
	info.stencilSrvFormat = stencilSrvFormat;
	info.decoder = decoder;
	info.supportsDepthStencil = true;
	info.resourceFlags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
}

void SetCompressedFormat(D3D12FormatInfo& info, DXGI_FORMAT format, TextureDecoder* decoder)
{
	SetColorFormat(info, format, decoder, false);
	info.isCompressed = true;
}

void SetAlternateColorFormat(D3D12FormatInfo& info, DXGI_FORMAT format, TextureDecoder* decoder)
{
	SetColorFormat(info, format, decoder);
	info.isAlternateFormat = true;
}

D3D12FormatCapabilities s_formatCapabilities{};

float SRGBToLinear(float value)
{
	if (value <= 0.04045f)
		return value / 12.92f;
	return std::pow((value + 0.055f) / 1.055f, 2.4f);
}

// float storage has no sRGB decode, so the sRGB-encoded RGBA32F output of TSRGBDecoder is linearised after decoding
template<typename TSRGBDecoder>
class TextureDecoder_SRGBToLinearRGBA32F : public TextureDecoder, public SingletonClass<TextureDecoder_SRGBToLinearRGBA32F<TSRGBDecoder>>
{
public:
	sint32 getBytesPerTexel(LatteTextureLoaderCtx* textureLoader) override
	{
		return TSRGBDecoder::getInstance()->getBytesPerTexel(textureLoader);
	}

	sint32 getTexelCountX(LatteTextureLoaderCtx* textureLoader) override
	{
		return TSRGBDecoder::getInstance()->getTexelCountX(textureLoader);
	}

	sint32 getTexelCountY(LatteTextureLoaderCtx* textureLoader) override
	{
		return TSRGBDecoder::getInstance()->getTexelCountY(textureLoader);
	}

	void decode(LatteTextureLoaderCtx* textureLoader, uint8* outputData) override
	{
		TSRGBDecoder::getInstance()->decode(textureLoader, outputData);
		const sint32 texelCount = getTexelCountX(textureLoader) * getTexelCountY(textureLoader);
		float* texel = reinterpret_cast<float*>(outputData);
		for (sint32 i = 0; i < texelCount; i++, texel += 4)
		{
			texel[0] = SRGBToLinear(texel[0]);
			texel[1] = SRGBToLinear(texel[1]);
			texel[2] = SRGBToLinear(texel[2]);
		}
	}

	void decodePixelToRGBA(uint8* blockData, uint8* outputPixel, uint8 blockOffsetX, uint8 blockOffsetY) override
	{
		TSRGBDecoder::getInstance()->decodePixelToRGBA(blockData, outputPixel, blockOffsetX, blockOffsetY);
	}
};

bool SetDecompressedBlockFormat(D3D12FormatInfo& info, Latte::E_GX2SURFFMT format)
{
	switch (format)
	{
	case Latte::E_GX2SURFFMT::BC1_UNORM:
		SetColorFormat(info, DXGI_FORMAT_R32G32B32A32_FLOAT, TextureDecoder_BC1_UNORM_uncompress::getInstance(), false);
		break;
	case Latte::E_GX2SURFFMT::BC1_SRGB:
		SetColorFormat(info, DXGI_FORMAT_R32G32B32A32_FLOAT, TextureDecoder_SRGBToLinearRGBA32F<TextureDecoder_BC1_SRGB_uncompress>::getInstance(), false);
		break;
	case Latte::E_GX2SURFFMT::BC2_UNORM:
		SetColorFormat(info, DXGI_FORMAT_R32G32B32A32_FLOAT, TextureDecoder_BC2_UNORM_uncompress::getInstance(), false);
		break;
	case Latte::E_GX2SURFFMT::BC2_SRGB:
		SetColorFormat(info, DXGI_FORMAT_R32G32B32A32_FLOAT, TextureDecoder_SRGBToLinearRGBA32F<TextureDecoder_BC2_SRGB_uncompress>::getInstance(), false);
		break;
	case Latte::E_GX2SURFFMT::BC3_UNORM:
		SetColorFormat(info, DXGI_FORMAT_R32G32B32A32_FLOAT, TextureDecoder_BC3_UNORM_uncompress::getInstance(), false);
		break;
	case Latte::E_GX2SURFFMT::BC3_SRGB:
		SetColorFormat(info, DXGI_FORMAT_R32G32B32A32_FLOAT, TextureDecoder_SRGBToLinearRGBA32F<TextureDecoder_BC3_SRGB_uncompress>::getInstance(), false);
		break;
	case Latte::E_GX2SURFFMT::BC4_UNORM:
		SetColorFormat(info, DXGI_FORMAT_R32G32_FLOAT, TextureDecoder_BC4_UNORM_uncompress::getInstance(), false);
		break;
	case Latte::E_GX2SURFFMT::BC4_SNORM:
		SetColorFormat(info, DXGI_FORMAT_R32G32_FLOAT, TextureDecoder_BC4_SNORM_uncompress::getInstance(), false);
		break;
	case Latte::E_GX2SURFFMT::BC5_UNORM:
		SetColorFormat(info, DXGI_FORMAT_R32G32_FLOAT, TextureDecoder_BC5_UNORM_uncompress::getInstance(), false);
		break;
	case Latte::E_GX2SURFFMT::BC5_SNORM:
		SetColorFormat(info, DXGI_FORMAT_R32G32_FLOAT, TextureDecoder_BC5_SNORM_uncompress::getInstance(), false);
		break;
	default:
		return false;
	}
	info.isAlternateFormat = true;
	return true;
}
}

void SetD3D12FormatCapabilities(const D3D12FormatCapabilities& capabilities)
{
	s_formatCapabilities = capabilities;
}

DXGI_FORMAT GetD3D12TypelessFormat(DXGI_FORMAT format)
{
	switch (format)
	{
	case DXGI_FORMAT_R32G32B32A32_FLOAT:
	case DXGI_FORMAT_R32G32B32A32_UINT:
	case DXGI_FORMAT_R32G32B32A32_SINT:
		return DXGI_FORMAT_R32G32B32A32_TYPELESS;
	case DXGI_FORMAT_R16G16B16A16_FLOAT:
	case DXGI_FORMAT_R16G16B16A16_UNORM:
	case DXGI_FORMAT_R16G16B16A16_UINT:
	case DXGI_FORMAT_R16G16B16A16_SNORM:
	case DXGI_FORMAT_R16G16B16A16_SINT:
		return DXGI_FORMAT_R16G16B16A16_TYPELESS;
	case DXGI_FORMAT_R32G32_FLOAT:
	case DXGI_FORMAT_R32G32_UINT:
	case DXGI_FORMAT_R32G32_SINT:
		return DXGI_FORMAT_R32G32_TYPELESS;
	case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
	case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
	case DXGI_FORMAT_X32_TYPELESS_G8X24_UINT:
		return DXGI_FORMAT_R32G8X24_TYPELESS;
	case DXGI_FORMAT_R10G10B10A2_UNORM:
	case DXGI_FORMAT_R10G10B10A2_UINT:
		return DXGI_FORMAT_R10G10B10A2_TYPELESS;
	case DXGI_FORMAT_R8G8B8A8_UNORM:
	case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
	case DXGI_FORMAT_R8G8B8A8_UINT:
	case DXGI_FORMAT_R8G8B8A8_SNORM:
	case DXGI_FORMAT_R8G8B8A8_SINT:
		return DXGI_FORMAT_R8G8B8A8_TYPELESS;
	case DXGI_FORMAT_R16G16_FLOAT:
	case DXGI_FORMAT_R16G16_UNORM:
	case DXGI_FORMAT_R16G16_UINT:
	case DXGI_FORMAT_R16G16_SNORM:
	case DXGI_FORMAT_R16G16_SINT:
		return DXGI_FORMAT_R16G16_TYPELESS;
	case DXGI_FORMAT_R32_FLOAT:
	case DXGI_FORMAT_R32_UINT:
	case DXGI_FORMAT_R32_SINT:
	case DXGI_FORMAT_D32_FLOAT:
		return DXGI_FORMAT_R32_TYPELESS;
	case DXGI_FORMAT_D24_UNORM_S8_UINT:
	case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
	case DXGI_FORMAT_X24_TYPELESS_G8_UINT:
		return DXGI_FORMAT_R24G8_TYPELESS;
	case DXGI_FORMAT_R8G8_UNORM:
	case DXGI_FORMAT_R8G8_UINT:
	case DXGI_FORMAT_R8G8_SNORM:
	case DXGI_FORMAT_R8G8_SINT:
		return DXGI_FORMAT_R8G8_TYPELESS;
	case DXGI_FORMAT_R16_FLOAT:
	case DXGI_FORMAT_R16_UNORM:
	case DXGI_FORMAT_R16_UINT:
	case DXGI_FORMAT_R16_SNORM:
	case DXGI_FORMAT_R16_SINT:
	case DXGI_FORMAT_D16_UNORM:
		return DXGI_FORMAT_R16_TYPELESS;
	case DXGI_FORMAT_R8_UNORM:
	case DXGI_FORMAT_R8_UINT:
	case DXGI_FORMAT_R8_SNORM:
	case DXGI_FORMAT_R8_SINT:
		return DXGI_FORMAT_R8_TYPELESS;
	case DXGI_FORMAT_BC1_UNORM:
	case DXGI_FORMAT_BC1_UNORM_SRGB:
		return DXGI_FORMAT_BC1_TYPELESS;
	case DXGI_FORMAT_BC2_UNORM:
	case DXGI_FORMAT_BC2_UNORM_SRGB:
		return DXGI_FORMAT_BC2_TYPELESS;
	case DXGI_FORMAT_BC3_UNORM:
	case DXGI_FORMAT_BC3_UNORM_SRGB:
		return DXGI_FORMAT_BC3_TYPELESS;
	case DXGI_FORMAT_BC4_UNORM:
	case DXGI_FORMAT_BC4_SNORM:
		return DXGI_FORMAT_BC4_TYPELESS;
	case DXGI_FORMAT_BC5_UNORM:
	case DXGI_FORMAT_BC5_SNORM:
		return DXGI_FORMAT_BC5_TYPELESS;
	default:
		return format;
	}
}

bool AreD3D12ViewFormatsCompatible(DXGI_FORMAT resourceFormat, DXGI_FORMAT viewFormat)
{
	return viewFormat != DXGI_FORMAT_UNKNOWN &&
		GetD3D12TypelessFormat(resourceFormat) == GetD3D12TypelessFormat(viewFormat);
}

D3D12FormatInfo GetD3D12FormatInfo(Latte::E_GX2SURFFMT format, bool isDepth, Latte::E_DIM dim, sint32 width, sint32 height)
{
	D3D12FormatInfo info{};
	info.texelCountX = width;
	info.texelCountY = height;

	if (isDepth)
	{
		switch (format)
		{
		case Latte::E_GX2SURFFMT::D24_S8_UNORM:
			SetDepthFormat(info, DXGI_FORMAT_R24G8_TYPELESS, DXGI_FORMAT_D24_UNORM_S8_UINT, DXGI_FORMAT_R24_UNORM_X8_TYPELESS, DXGI_FORMAT_X24_TYPELESS_G8_UINT, TextureDecoder_D24_S8::getInstance());
			break;
		case Latte::E_GX2SURFFMT::D24_S8_FLOAT:
			SetDepthFormat(info, DXGI_FORMAT_R32G8X24_TYPELESS, DXGI_FORMAT_D32_FLOAT_S8X24_UINT, DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS, DXGI_FORMAT_X32_TYPELESS_G8X24_UINT, TextureDecoder_NullData64::getInstance());
			info.isAlternateFormat = true;
			break;
		case Latte::E_GX2SURFFMT::D32_FLOAT:
			SetDepthFormat(info, DXGI_FORMAT_R32_TYPELESS, DXGI_FORMAT_D32_FLOAT, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_UNKNOWN, TextureDecoder_R32_FLOAT::getInstance());
			break;
		case Latte::E_GX2SURFFMT::D16_UNORM:
			SetDepthFormat(info, DXGI_FORMAT_R16_TYPELESS, DXGI_FORMAT_D16_UNORM, DXGI_FORMAT_R16_UNORM, DXGI_FORMAT_UNKNOWN, TextureDecoder_R16_UNORM::getInstance());
			break;
		case Latte::E_GX2SURFFMT::D32_S8_FLOAT:
			SetDepthFormat(info, DXGI_FORMAT_R32G8X24_TYPELESS, DXGI_FORMAT_D32_FLOAT_S8X24_UINT, DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS, DXGI_FORMAT_X32_TYPELESS_G8X24_UINT, TextureDecoder_D32_S8_UINT_X24::getInstance());
			break;
		default:
			cemuLog_log(LogType::Force, "Unsupported D3D12 depth texture format {:04x}", (uint32)format);
			SetDepthFormat(info, DXGI_FORMAT_R16_TYPELESS, DXGI_FORMAT_D16_UNORM, DXGI_FORMAT_R16_UNORM, DXGI_FORMAT_UNKNOWN, nullptr);
			info.isAlternateFormat = true;
			break;
		}
		return info;
	}

	if (format == (Latte::E_GX2SURFFMT::R16_G16_B16_A16_FLOAT | Latte::E_GX2SURFFMT::FMT_BIT_SRGB))
		format = Latte::E_GX2SURFFMT::R16_G16_B16_A16_FLOAT;

	// block-compressed resources need a top-level size aligned to the block size unless the device relaxes it
	if (Latte::IsCompressedFormat(format) && !s_formatCapabilities.unalignedBlockTextures &&
		((width % 4) != 0 || (height % 4) != 0) && SetDecompressedBlockFormat(info, format))
		return info;

	switch (format)
	{
	case Latte::E_GX2SURFFMT::R32_G32_B32_A32_FLOAT:
		SetColorFormat(info, DXGI_FORMAT_R32G32B32A32_FLOAT, TextureDecoder_R32_G32_B32_A32_FLOAT::getInstance());
		break;
	case Latte::E_GX2SURFFMT::R32_G32_B32_A32_UINT:
		SetColorFormat(info, DXGI_FORMAT_R32G32B32A32_UINT, TextureDecoder_R32_G32_B32_A32_UINT::getInstance());
		break;
	case Latte::E_GX2SURFFMT::R32_G32_B32_A32_SINT:
		SetColorFormat(info, DXGI_FORMAT_R32G32B32A32_SINT, TextureDecoder_R32_G32_B32_A32_UINT::getInstance());
		break;
	case Latte::E_GX2SURFFMT::R16_G16_B16_A16_FLOAT:
		SetColorFormat(info, DXGI_FORMAT_R16G16B16A16_FLOAT, TextureDecoder_R16_G16_B16_A16_FLOAT::getInstance());
		break;
	case Latte::E_GX2SURFFMT::R16_G16_B16_A16_UINT:
		SetColorFormat(info, DXGI_FORMAT_R16G16B16A16_UINT, TextureDecoder_R16_G16_B16_A16_UINT::getInstance());
		break;
	case Latte::E_GX2SURFFMT::R16_G16_B16_A16_SINT:
		SetColorFormat(info, DXGI_FORMAT_R16G16B16A16_SINT, TextureDecoder_R16_G16_B16_A16_UINT::getInstance());
		break;
	case Latte::E_GX2SURFFMT::R16_G16_B16_A16_UNORM:
		SetColorFormat(info, DXGI_FORMAT_R16G16B16A16_UNORM, TextureDecoder_R16_G16_B16_A16::getInstance());
		break;
	case Latte::E_GX2SURFFMT::R16_G16_B16_A16_SNORM:
		SetColorFormat(info, DXGI_FORMAT_R16G16B16A16_SNORM, TextureDecoder_R16_G16_B16_A16::getInstance());
		break;
	case Latte::E_GX2SURFFMT::R8_G8_B8_A8_UNORM:
		SetColorFormat(info, DXGI_FORMAT_R8G8B8A8_UNORM, TextureDecoder_R8_G8_B8_A8::getInstance());
		break;
	case Latte::E_GX2SURFFMT::R8_G8_B8_A8_SNORM:
		SetColorFormat(info, DXGI_FORMAT_R8G8B8A8_SNORM, TextureDecoder_R8_G8_B8_A8::getInstance());
		break;
	case Latte::E_GX2SURFFMT::R8_G8_B8_A8_SRGB:
		SetColorFormat(info, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, TextureDecoder_R8_G8_B8_A8::getInstance());
		break;
	case Latte::E_GX2SURFFMT::R8_G8_B8_A8_UINT:
		SetColorFormat(info, DXGI_FORMAT_R8G8B8A8_UINT, TextureDecoder_R8_G8_B8_A8_UINT::getInstance());
		break;
	case Latte::E_GX2SURFFMT::R8_G8_B8_A8_SINT:
		SetColorFormat(info, DXGI_FORMAT_R8G8B8A8_SINT, TextureDecoder_R8_G8_B8_A8::getInstance());
		break;
	case Latte::E_GX2SURFFMT::R32_G32_FLOAT:
		SetColorFormat(info, DXGI_FORMAT_R32G32_FLOAT, TextureDecoder_R32_G32_FLOAT::getInstance());
		break;
	case Latte::E_GX2SURFFMT::R32_G32_UINT:
		SetColorFormat(info, DXGI_FORMAT_R32G32_UINT, TextureDecoder_R32_G32_UINT::getInstance());
		break;
	case Latte::E_GX2SURFFMT::R32_G32_SINT:
		SetColorFormat(info, DXGI_FORMAT_R32G32_SINT, TextureDecoder_R32_G32_UINT::getInstance());
		break;
	case Latte::E_GX2SURFFMT::R16_G16_UNORM:
		SetColorFormat(info, DXGI_FORMAT_R16G16_UNORM, TextureDecoder_R16_G16::getInstance());
		break;
	case Latte::E_GX2SURFFMT::R16_G16_SNORM:
		SetColorFormat(info, DXGI_FORMAT_R16G16_SNORM, TextureDecoder_R16_G16::getInstance());
		break;
	case Latte::E_GX2SURFFMT::R16_G16_UINT:
		SetColorFormat(info, DXGI_FORMAT_R16G16_UINT, TextureDecoder_R16_G16::getInstance());
		break;
	case Latte::E_GX2SURFFMT::R16_G16_SINT:
		SetColorFormat(info, DXGI_FORMAT_R16G16_SINT, TextureDecoder_R16_G16::getInstance());
		break;
	case Latte::E_GX2SURFFMT::R16_G16_FLOAT:
		SetColorFormat(info, DXGI_FORMAT_R16G16_FLOAT, TextureDecoder_R16_G16_FLOAT::getInstance());
		break;
	case Latte::E_GX2SURFFMT::R8_G8_UNORM:
		SetColorFormat(info, DXGI_FORMAT_R8G8_UNORM, TextureDecoder_R8_G8::getInstance());
		break;
	case Latte::E_GX2SURFFMT::R8_G8_SNORM:
		SetColorFormat(info, DXGI_FORMAT_R8G8_SNORM, TextureDecoder_R8_G8::getInstance());
		break;
	case Latte::E_GX2SURFFMT::R8_G8_UINT:
		SetColorFormat(info, DXGI_FORMAT_R8G8_UINT, TextureDecoder_R8_G8::getInstance());
		break;
	case Latte::E_GX2SURFFMT::R8_G8_SINT:
		SetColorFormat(info, DXGI_FORMAT_R8G8_SINT, TextureDecoder_R8_G8::getInstance());
		break;
	case Latte::E_GX2SURFFMT::R4_G4_UNORM:
		SetAlternateColorFormat(info, DXGI_FORMAT_R8G8B8A8_UNORM, TextureDecoder_R4G4_UNORM_To_RGBA8::getInstance());
		break;
	case Latte::E_GX2SURFFMT::R32_FLOAT:
		SetColorFormat(info, DXGI_FORMAT_R32_FLOAT, TextureDecoder_R32_FLOAT::getInstance());
		break;
	case Latte::E_GX2SURFFMT::R32_UINT:
		SetColorFormat(info, DXGI_FORMAT_R32_UINT, TextureDecoder_R32_UINT::getInstance());
		break;
	case Latte::E_GX2SURFFMT::R32_SINT:
		SetColorFormat(info, DXGI_FORMAT_R32_SINT, TextureDecoder_R32_UINT::getInstance());
		break;
	case Latte::E_GX2SURFFMT::R16_FLOAT:
		SetColorFormat(info, DXGI_FORMAT_R16_FLOAT, TextureDecoder_R16_FLOAT::getInstance());
		break;
	case Latte::E_GX2SURFFMT::R16_UNORM:
		SetColorFormat(info, DXGI_FORMAT_R16_UNORM, TextureDecoder_R16_UNORM::getInstance());
		break;
	case Latte::E_GX2SURFFMT::R16_SNORM:
		SetColorFormat(info, DXGI_FORMAT_R16_SNORM, TextureDecoder_R16_SNORM::getInstance());
		break;
	case Latte::E_GX2SURFFMT::R16_UINT:
		SetColorFormat(info, DXGI_FORMAT_R16_UINT, TextureDecoder_R16_UINT::getInstance());
		break;
	case Latte::E_GX2SURFFMT::R16_SINT:
		SetColorFormat(info, DXGI_FORMAT_R16_SINT, TextureDecoder_R16_UINT::getInstance());
		break;
	case Latte::E_GX2SURFFMT::R8_UNORM:
		SetColorFormat(info, DXGI_FORMAT_R8_UNORM, TextureDecoder_R8::getInstance());
		break;
	case Latte::E_GX2SURFFMT::R8_SNORM:
		SetColorFormat(info, DXGI_FORMAT_R8_SNORM, TextureDecoder_R8::getInstance());
		break;
	case Latte::E_GX2SURFFMT::R8_UINT:
		SetColorFormat(info, DXGI_FORMAT_R8_UINT, TextureDecoder_R8_UINT::getInstance());
		break;
	case Latte::E_GX2SURFFMT::R8_SINT:
		SetColorFormat(info, DXGI_FORMAT_R8_SINT, TextureDecoder_R8_UINT::getInstance());
		break;
	case Latte::E_GX2SURFFMT::R5_G6_B5_UNORM:
		// B5G6R5 keeps red in the high bits, GX2 keeps it in the low bits
		if (s_formatCapabilities.packedB5G6R5)
			SetAlternateColorFormat(info, DXGI_FORMAT_B5G6R5_UNORM, TextureDecoder_R5_G6_B5_swappedRB::getInstance());
		else
			SetAlternateColorFormat(info, DXGI_FORMAT_R8G8B8A8_UNORM, TextureDecoder_R5G6B5_UNORM_To_RGBA8::getInstance());
		break;
	case Latte::E_GX2SURFFMT::R5_G5_B5_A1_UNORM:
		if (s_formatCapabilities.packedB5G5R5A1)
			SetAlternateColorFormat(info, DXGI_FORMAT_B5G5R5A1_UNORM, TextureDecoder_R5_G5_B5_A1_UNORM_swappedRB::getInstance());
		else
			SetAlternateColorFormat(info, DXGI_FORMAT_R8G8B8A8_UNORM, TextureDecoder_R5_G5_B5_A1_UNORM_swappedRB_To_RGBA8::getInstance());
		break;
	case Latte::E_GX2SURFFMT::A1_B5_G5_R5_UNORM:
		if (s_formatCapabilities.packedB5G5R5A1)
			SetAlternateColorFormat(info, DXGI_FORMAT_B5G5R5A1_UNORM, TextureDecoder_A1_B5_G5_R5_UNORM_vulkan::getInstance());
		else
			SetAlternateColorFormat(info, DXGI_FORMAT_R8G8B8A8_UNORM, TextureDecoder_A1_B5_G5_R5_UNORM_vulkan_To_RGBA8::getInstance());
		break;
	case Latte::E_GX2SURFFMT::R11_G11_B10_FLOAT:
		SetColorFormat(info, DXGI_FORMAT_R11G11B10_FLOAT, TextureDecoder_R11_G11_B10_FLOAT::getInstance());
		break;
	case Latte::E_GX2SURFFMT::R4_G4_B4_A4_UNORM:
		SetAlternateColorFormat(info, DXGI_FORMAT_R8G8B8A8_UNORM, TextureDecoder_R4G4B4A4_UNORM_To_RGBA8::getInstance());
		break;
	case Latte::E_GX2SURFFMT::R10_G10_B10_A2_UNORM:
		SetColorFormat(info, DXGI_FORMAT_R10G10B10A2_UNORM, TextureDecoder_R10_G10_B10_A2_UNORM::getInstance());
		break;
	case Latte::E_GX2SURFFMT::R10_G10_B10_A2_SNORM:
		SetAlternateColorFormat(info, DXGI_FORMAT_R16G16B16A16_SNORM, TextureDecoder_R10_G10_B10_A2_SNORM_To_RGBA16::getInstance());
		break;
	case Latte::E_GX2SURFFMT::R10_G10_B10_A2_SRGB:
		SetColorFormat(info, DXGI_FORMAT_R10G10B10A2_UNORM, TextureDecoder_R10_G10_B10_A2_UNORM::getInstance());
		break;
	case Latte::E_GX2SURFFMT::A2_B10_G10_R10_UNORM:
		SetAlternateColorFormat(info, DXGI_FORMAT_R16G16B16A16_UNORM, TextureDecoder_A2_B10_G10_R10_UNORM_To_RGBA16::getInstance());
		break;
	case Latte::E_GX2SURFFMT::BC1_SRGB:
		SetCompressedFormat(info, DXGI_FORMAT_BC1_UNORM_SRGB, TextureDecoder_BC1::getInstance());
		break;
	case Latte::E_GX2SURFFMT::BC1_UNORM:
		SetCompressedFormat(info, DXGI_FORMAT_BC1_UNORM, TextureDecoder_BC1::getInstance());
		break;
	case Latte::E_GX2SURFFMT::BC2_UNORM:
		SetCompressedFormat(info, DXGI_FORMAT_BC2_UNORM, TextureDecoder_BC2::getInstance());
		break;
	case Latte::E_GX2SURFFMT::BC2_SRGB:
		SetCompressedFormat(info, DXGI_FORMAT_BC2_UNORM_SRGB, TextureDecoder_BC2::getInstance());
		break;
	case Latte::E_GX2SURFFMT::BC3_UNORM:
		SetCompressedFormat(info, DXGI_FORMAT_BC3_UNORM, TextureDecoder_BC3::getInstance());
		break;
	case Latte::E_GX2SURFFMT::BC3_SRGB:
		SetCompressedFormat(info, DXGI_FORMAT_BC3_UNORM_SRGB, TextureDecoder_BC3::getInstance());
		break;
	case Latte::E_GX2SURFFMT::BC4_UNORM:
		SetCompressedFormat(info, DXGI_FORMAT_BC4_UNORM, TextureDecoder_BC4::getInstance());
		break;
	case Latte::E_GX2SURFFMT::BC4_SNORM:
		SetCompressedFormat(info, DXGI_FORMAT_BC4_SNORM, TextureDecoder_BC4::getInstance());
		break;
	case Latte::E_GX2SURFFMT::BC5_UNORM:
		SetCompressedFormat(info, DXGI_FORMAT_BC5_UNORM, TextureDecoder_BC5::getInstance());
		break;
	case Latte::E_GX2SURFFMT::BC5_SNORM:
		SetCompressedFormat(info, DXGI_FORMAT_BC5_SNORM, TextureDecoder_BC5::getInstance());
		break;
	case Latte::E_GX2SURFFMT::R24_X8_UNORM:
		SetAlternateColorFormat(info, DXGI_FORMAT_R32_FLOAT, TextureDecoder_R24_X8::getInstance());
		break;
	case Latte::E_GX2SURFFMT::X24_G8_UINT:
		SetAlternateColorFormat(info, DXGI_FORMAT_R8G8B8A8_UINT, TextureDecoder_X24_G8_UINT::getInstance());
		break;
	case Latte::E_GX2SURFFMT::R32_X8_FLOAT:
		SetAlternateColorFormat(info, DXGI_FORMAT_R32_FLOAT, TextureDecoder_NullData64::getInstance());
		break;
	default:
		cemuLog_log(LogType::Force, "Unsupported D3D12 color texture format {:04x}", (uint32)format);
		SetAlternateColorFormat(info, DXGI_FORMAT_R8G8B8A8_UNORM, nullptr);
		break;
	}

	if (Latte::IsCompressedFormat(format))
		info.isCompressed = true;
	return info;
}
