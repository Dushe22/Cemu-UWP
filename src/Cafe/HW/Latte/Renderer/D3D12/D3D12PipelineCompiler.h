#pragma once

#include "Cafe/HW/Latte/ISA/LatteReg.h"

#include <bit>

#if BOOST_OS_WINDOWS
#include <d3d12.h>
#endif

class CachedFBOD3D12;
struct LatteContextRegister;
struct LatteFetchShader;
struct LatteDecompilerShader;

class D3D12PipelineCompiler
{
public:
	// Without dynamic depth bias (Xbox) the polygon offset is baked into each PSO, and titles such as
	// BOTW recompute the slope every frame (-0.214672, -0.214671, ...): 82% of a BOTW session's PSOs
	// differed only there. Rounding the float to 6 mantissa bits (at most 0.8% off, invisible as a
	// shadow bias) cut that session from 6579 to 1519 pipelines. Hash and PSO use the same value.
	static uint32 QuantizeDepthBiasBits(uint32 floatBits)
	{
		constexpr uint32 kDroppedBits = 23 - 6;
		if ((floatBits & 0x7F800000u) == 0x7F800000u)
			return floatBits; // keep inf/nan as they are
		floatBits += 1u << (kDroppedBits - 1);
		return floatBits & ~((1u << kDroppedBits) - 1u);
	}
	static float QuantizeDepthBias(float value)
	{
		return std::bit_cast<float>(QuantizeDepthBiasBits(std::bit_cast<uint32>(value)));
	}

	struct PipelineIdentity
	{
		uint64 baseHash{};
		uint64 minimalStateHash{};
		uint64 pipelineStateHash{};
		bool valid{};
	};

	struct Stats
	{
		uint64 identitiesSeen{};
		uint64 stableCacheHits{};
		uint64 stableCacheAdds{};
		uint64 invalidIdentities{};
	};

	static PipelineIdentity RecordCurrentState(CachedFBOD3D12* activeFBO, bool allowNullFBO = false);
	static void RecordPipelineCreated(const PipelineIdentity& identity, uint64 nativePipelineHash);
	static uint64 CalculateMinimalGraphicsPipelineHash(const LatteFetchShader* fetchShader, const LatteContextRegister& lcr);
	static uint64 CalculateGraphicsPipelineHash(const LatteFetchShader* fetchShader,
												const LatteDecompilerShader* vertexShader,
												const LatteDecompilerShader* geometryShader,
												const LatteDecompilerShader* pixelShader,
												const CachedFBOD3D12* fbo,
												const LatteContextRegister& lcr,
												bool ignoreDepthBiasValues = false); // true: key shared by all depth bias variants
	static D3D12_PRIMITIVE_TOPOLOGY_TYPE GetPrimitiveTopologyType(Latte::LATTE_VGT_PRIMITIVE_TYPE::E_PRIMITIVE_TYPE primitiveMode);
	static D3D_PRIMITIVE_TOPOLOGY GetPrimitiveTopology(Latte::LATTE_VGT_PRIMITIVE_TYPE::E_PRIMITIVE_TYPE primitiveMode);
	static Stats GetStats();
	static void SetDynamicDepthBias(bool enabled);

private:
	static uint64 CalculateShaderIdentityHash(const LatteDecompilerShader* vertexShader,
										  const LatteDecompilerShader* geometryShader,
										  const LatteDecompilerShader* pixelShader);
	static uint64 CalculateAttachmentHash(const CachedFBOD3D12* fbo);
};
