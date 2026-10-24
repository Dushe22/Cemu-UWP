#include "Cafe/HW/Latte/Renderer/D3D12/D3D12PipelineCompiler.h"

#include "Cafe/HW/Latte/Core/FetchShader.h"
#include "Cafe/HW/Latte/Core/Latte.h"
#include "Cafe/HW/Latte/Core/LatteShader.h"
#include "Cafe/HW/Latte/LegacyShaderDecompiler/LatteDecompilerEmitHLSL.hpp"
#include "Cafe/HW/Latte/Renderer/D3D12/CachedFBOD3D12.h"
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12PipelineStableCache.h"

#include <atomic>
#include <bit>

namespace
{
constexpr uint64 kD3D12PipelineKeyVersion = 9;

constexpr uint64 kVertexShaderStageTag = 0x5653000000000001ull;
constexpr uint64 kGeometryShaderStageTag = 0x4753000000000002ull;
constexpr uint64 kPixelShaderStageTag = 0x5053000000000003ull;

std::atomic_uint64_t s_identitiesSeen{ 0 };
std::atomic_uint64_t s_stableCacheHits{ 0 };
std::atomic_uint64_t s_stableCacheAdds{ 0 };
std::atomic_uint64_t s_invalidIdentities{ 0 };
std::atomic_bool s_dynamicDepthBias{ false };

uint64 MixHash(uint64 hash, uint64 value, uint32 rotate = 7)
{
	value += 0x9E3779B97F4A7C15ull;
	value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ull;
	value = (value ^ (value >> 27)) * 0x94D049BB133111EBull;
	value ^= value >> 31;
	hash ^= value + 0x9E3779B97F4A7C15ull + std::rotl(hash, rotate);
	return std::rotl(hash, rotate) * 0xD6E8FEB86659FD93ull;
}

uint64 MixShaderIdentity(uint64 hash, uint64 stageTag, const LatteDecompilerShader* shader)
{
	hash = MixHash(hash, stageTag, 11);
	hash = MixHash(hash, shader ? 1 : 0, 13);
	if (!shader)
		return hash;

	hash = MixHash(hash, shader->baseHash, 17);
	return MixHash(hash, shader->auxHash, 23);
}
}

D3D12PipelineCompiler::PipelineIdentity D3D12PipelineCompiler::RecordCurrentState(CachedFBOD3D12* activeFBO, bool allowNullFBO)
{
	PipelineIdentity identity{};
	if (LatteGPUState.activeShaderHasError || (!activeFBO && !allowNullFBO))
	{
		s_invalidIdentities++;
		return identity;
	}

	const auto* fetchShader = LatteSHRC_GetActiveFetchShader();
	const auto* vertexShader = LatteSHRC_GetActiveVertexShader();
	const auto* geometryShader = LatteSHRC_GetActiveGeometryShader();
	const auto* pixelShader = LatteSHRC_GetActivePixelShader();
	if (!fetchShader || !vertexShader)
	{
		s_invalidIdentities++;
		return identity;
	}

	identity.baseHash = CalculateShaderIdentityHash(vertexShader, geometryShader, pixelShader);
	// the UWP draw path only consumes the complete pipeline hash below
	identity.pipelineStateHash = CalculateGraphicsPipelineHash(fetchShader, vertexShader, geometryShader, pixelShader, activeFBO, LatteGPUState.contextNew);
	identity.valid = true;
	return identity;
}

void D3D12PipelineCompiler::RecordPipelineCreated(const PipelineIdentity& identity, uint64 nativePipelineHash)
{
	if (!identity.valid)
		return;

	s_identitiesSeen.fetch_add(1, std::memory_order_relaxed);
	const bool inserted = D3D12PipelineStableCache::GetInstance().AddPipelineToCache(identity.baseHash, nativePipelineHash);
	if (inserted)
		s_stableCacheAdds.fetch_add(1, std::memory_order_relaxed);
	else
		s_stableCacheHits.fetch_add(1, std::memory_order_relaxed);
}

uint64 D3D12PipelineCompiler::CalculateMinimalGraphicsPipelineHash(const LatteFetchShader* fetchShader, const LatteContextRegister& lcr)
{
	if (!fetchShader)
		return 0;

	uint64 stateHash = kD3D12PipelineKeyVersion;
	stateHash = MixHash(stateHash, LATTE_DECOMPILER_HLSL_EMITTER_VERSION);

	// UWP shaders read vertex strides from root constants; strides are not pipeline state

	stateHash = MixHash(stateHash, fetchShader->getVkPipelineHashFragment());
	stateHash = MixHash(stateHash, lcr.GetRawView()[mmVGT_PRIMITIVE_TYPE]);
	stateHash = MixHash(stateHash, lcr.GetRawView()[mmVGT_STRMOUT_EN]);

	if (lcr.PA_CL_CLIP_CNTL.get_DX_RASTERIZATION_KILL())
		stateHash = MixHash(stateHash, 0x333333);

	return stateHash;
}

uint64 D3D12PipelineCompiler::CalculateGraphicsPipelineHash(const LatteFetchShader* fetchShader,
															const LatteDecompilerShader* vertexShader,
															const LatteDecompilerShader* geometryShader,
															const LatteDecompilerShader* pixelShader,
															const CachedFBOD3D12* fbo,
															const LatteContextRegister& lcr,
															bool ignoreDepthBiasValues)
{
	uint64 stateHash = CalculateMinimalGraphicsPipelineHash(fetchShader, lcr);
	stateHash = (stateHash >> 8) + (stateHash * 0x370531ull) % 0x7F980D3BF9B4639Dull;

	const uint32* ctxRegister = lcr.GetRawView();
	stateHash = MixShaderIdentity(stateHash, kVertexShaderStageTag, vertexShader);
	stateHash = MixShaderIdentity(stateHash, kGeometryShaderStageTag, geometryShader);
	stateHash = MixShaderIdentity(stateHash, kPixelShaderStageTag, pixelShader);

	const uint32 polygonCtrl = lcr.PA_SU_SC_MODE_CNTL.getRawValue();
	stateHash = MixHash(stateHash, polygonCtrl);
	stateHash = MixHash(stateHash, ctxRegister[Latte::REGADDR::PA_CL_CLIP_CNTL]);
	if (lcr.PA_SU_SC_MODE_CNTL.get_OFFSET_FRONT_ENABLED() && s_dynamicDepthBias.load(std::memory_order_relaxed))
	{
		// bias values are set per draw
		stateHash = MixHash(stateHash, 0xD7B1A5ull, 11);
	}
	else if (lcr.PA_SU_SC_MODE_CNTL.get_OFFSET_FRONT_ENABLED() && ignoreDepthBiasValues)
	{
		stateHash = MixHash(stateHash, 0xB1A5F7EEull, 11);
	}
	else if (lcr.PA_SU_SC_MODE_CNTL.get_OFFSET_FRONT_ENABLED())
	{
		stateHash = MixHash(stateHash, QuantizeDepthBiasBits(ctxRegister[Latte::REGADDR::PA_SU_POLY_OFFSET_FRONT_SCALE]), 11);
		stateHash = MixHash(stateHash, ctxRegister[Latte::REGADDR::PA_SU_POLY_OFFSET_FRONT_OFFSET], 13);
		stateHash = MixHash(stateHash, QuantizeDepthBiasBits(ctxRegister[Latte::REGADDR::PA_SU_POLY_OFFSET_CLAMP]), 17);
	}

	const uint32 colorControlReg = ctxRegister[Latte::REGADDR::CB_COLOR_CONTROL];
	stateHash = MixHash(stateHash, colorControlReg);
	stateHash = MixHash(stateHash, ctxRegister[Latte::REGADDR::CB_TARGET_MASK]);

	const uint32 blendEnableMask = (colorControlReg >> 8) & 0xFF;
	for (uint32 i = 0; i < 8; ++i)
	{
		if ((blendEnableMask & (1u << i)) != 0)
			stateHash = MixHash(stateHash, ctxRegister[Latte::REGADDR::CB_BLEND0_CONTROL + i]);
	}

	stateHash = MixHash(stateHash, CalculateAttachmentHash(fbo));

	uint32 depthControl = ctxRegister[Latte::REGADDR::DB_DEPTH_CONTROL];
	const bool stencilTestEnable = (depthControl & 1) != 0;
	if (stencilTestEnable)
	{
		const uint32 frontStencilMasks =
			(uint32)lcr.DB_STENCILREFMASK.get_STENCILMASK_F() |
			((uint32)lcr.DB_STENCILREFMASK.get_STENCILWRITEMASK_F() << 8);
		stateHash = MixHash(stateHash, frontStencilMasks, 17);
		if ((depthControl & (1 << 7)) != 0)
		{
			const uint32 backStencilMasks =
				(uint32)lcr.DB_STENCILREFMASK_BF.get_STENCILMASK_B() |
				((uint32)lcr.DB_STENCILREFMASK_BF.get_STENCILWRITEMASK_B() << 8);
			stateHash = MixHash(stateHash, backStencilMasks, 13);
		}
	}
	else
		depthControl &= 0xFF;

	stateHash = MixHash(stateHash, depthControl, 17);
	if ((polygonCtrl & (1 << 11)) != 0)
		stateHash = MixHash(stateHash, 0x1111);

	return stateHash;
}

D3D12_PRIMITIVE_TOPOLOGY_TYPE D3D12PipelineCompiler::GetPrimitiveTopologyType(Latte::LATTE_VGT_PRIMITIVE_TYPE::E_PRIMITIVE_TYPE primitiveMode)
{
	using Primitive = Latte::LATTE_VGT_PRIMITIVE_TYPE::E_PRIMITIVE_TYPE;
	switch (primitiveMode)
	{
	case Primitive::POINTS:
		return D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
	case Primitive::LINES:
	case Primitive::LINES_ADJACENT:
	case Primitive::LINE_STRIP:
	case Primitive::LINE_LOOP:
	case Primitive::LINE_STRIP_ADJACENT:
		return D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
	case Primitive::TRIANGLES:
	case Primitive::TRIANGLE_FAN:
	case Primitive::TRIANGLE_STRIP:
	case Primitive::TRIANGLES_ADJACENT:
	case Primitive::TRIANGLE_STRIP_ADJACENT:
	case Primitive::QUADS:
	case Primitive::QUAD_STRIP:
	case Primitive::RECTS:
	default:
		return D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	}
}

D3D_PRIMITIVE_TOPOLOGY D3D12PipelineCompiler::GetPrimitiveTopology(Latte::LATTE_VGT_PRIMITIVE_TYPE::E_PRIMITIVE_TYPE primitiveMode)
{
	using Primitive = Latte::LATTE_VGT_PRIMITIVE_TYPE::E_PRIMITIVE_TYPE;
	switch (primitiveMode)
	{
	case Primitive::POINTS:
		return D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
	case Primitive::LINES:
		return D3D_PRIMITIVE_TOPOLOGY_LINELIST;
	case Primitive::LINES_ADJACENT:
		return D3D_PRIMITIVE_TOPOLOGY_LINELIST_ADJ;
	case Primitive::LINE_STRIP:
	case Primitive::LINE_LOOP:
		return D3D_PRIMITIVE_TOPOLOGY_LINESTRIP;
	case Primitive::LINE_STRIP_ADJACENT:
		return D3D_PRIMITIVE_TOPOLOGY_LINESTRIP_ADJ;
	case Primitive::TRIANGLE_STRIP:
		return D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
	case Primitive::RECTS:
		// the UWP rect geometry shader consumes one triangle per rectangle
		return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
	case Primitive::TRIANGLES_ADJACENT:
		return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST_ADJ;
	case Primitive::TRIANGLE_STRIP_ADJACENT:
		return D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP_ADJ;
	case Primitive::TRIANGLE_FAN:
		// UWP expands fans to a triangle list
		return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
	case Primitive::TRIANGLES:
	case Primitive::QUADS:
	case Primitive::QUAD_STRIP:
	default:
		return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
	}
}

D3D12PipelineCompiler::Stats D3D12PipelineCompiler::GetStats()
{
	return {
		s_identitiesSeen.load(std::memory_order::relaxed),
		s_stableCacheHits.load(std::memory_order::relaxed),
		s_stableCacheAdds.load(std::memory_order::relaxed),
		s_invalidIdentities.load(std::memory_order::relaxed),
	};
}

void D3D12PipelineCompiler::SetDynamicDepthBias(bool enabled)
{
	s_dynamicDepthBias.store(enabled, std::memory_order_relaxed);
}

uint64 D3D12PipelineCompiler::CalculateShaderIdentityHash(const LatteDecompilerShader* vertexShader,
												 const LatteDecompilerShader* geometryShader,
												 const LatteDecompilerShader* pixelShader)
{
	uint64 hash = MixHash(kD3D12PipelineKeyVersion, LATTE_DECOMPILER_HLSL_EMITTER_VERSION);
	hash = MixShaderIdentity(hash, kVertexShaderStageTag, vertexShader);
	hash = MixShaderIdentity(hash, kGeometryShaderStageTag, geometryShader);
	return MixShaderIdentity(hash, kPixelShaderStageTag, pixelShader);
}

uint64 D3D12PipelineCompiler::CalculateAttachmentHash(const CachedFBOD3D12* fbo)
{
	if (!fbo)
		return 0;

	uint64 hash = 0xD312F00Cull;
	hash = MixHash(hash, fbo->GetColorAttachmentMask());
	hash = MixHash(hash, fbo->GetColorAttachmentCount());
	hash = MixHash(hash, fbo->HasSparseColorAttachments() ? 1 : 0);

	const auto& colorAttachments = fbo->GetColorAttachments();
	for (uint32 i = 0; i < colorAttachments.size(); ++i)
	{
		const auto& attachment = colorAttachments[i];
		hash = MixHash(hash, i);
		hash = MixHash(hash, attachment.valid ? (uint32)attachment.format : (uint32)DXGI_FORMAT_UNKNOWN);
	}

	hash = MixHash(hash, fbo->HasDepthStencilAttachment() ? 1 : 0);
	hash = MixHash(hash, (uint32)fbo->GetDepthFormat());
	hash = MixHash(hash, (uint32)fbo->GetStencilFormat());
	return hash;
}
