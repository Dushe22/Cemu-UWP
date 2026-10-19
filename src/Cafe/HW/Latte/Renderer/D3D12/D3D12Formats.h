#pragma once

#include "Cafe/HW/Latte/Core/LatteConst.h"

#if BOOST_OS_WINDOWS
#include <d3d12.h>
#include <dxgiformat.h>
#endif

class TextureDecoder;

struct D3D12FormatInfo
{
	DXGI_FORMAT resourceFormat{ DXGI_FORMAT_R8G8B8A8_UNORM };
	DXGI_FORMAT srvFormat{ DXGI_FORMAT_R8G8B8A8_UNORM };
	DXGI_FORMAT rtvFormat{ DXGI_FORMAT_R8G8B8A8_UNORM };
	DXGI_FORMAT dsvFormat{ DXGI_FORMAT_UNKNOWN };
	DXGI_FORMAT depthSrvFormat{ DXGI_FORMAT_UNKNOWN };
	DXGI_FORMAT stencilSrvFormat{ DXGI_FORMAT_UNKNOWN };
	D3D12_RESOURCE_FLAGS resourceFlags{ D3D12_RESOURCE_FLAG_NONE };
	TextureDecoder* decoder{};
	sint32 texelCountX{};
	sint32 texelCountY{};
	bool isCompressed{};
	bool isDepth{};
	bool hasStencil{};
	bool supportsRenderTarget{};
	bool supportsDepthStencil{};
	// host texel layout differs from the guest format
	bool isAlternateFormat{};
};

struct D3D12FormatCapabilities
{
	bool packedB5G6R5{};
	bool packedB5G5R5A1{};
	bool unalignedBlockTextures{ true };
};

void SetD3D12FormatCapabilities(const D3D12FormatCapabilities& capabilities);
D3D12FormatInfo GetD3D12FormatInfo(Latte::E_GX2SURFFMT format, bool isDepth, Latte::E_DIM dim, sint32 width, sint32 height);
DXGI_FORMAT GetD3D12TypelessFormat(DXGI_FORMAT format);
bool AreD3D12ViewFormatsCompatible(DXGI_FORMAT resourceFormat, DXGI_FORMAT viewFormat);
