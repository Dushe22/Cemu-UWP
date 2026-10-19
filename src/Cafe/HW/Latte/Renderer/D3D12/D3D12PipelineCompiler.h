#pragma once

#include "Cafe/HW/Latte/ISA/LatteReg.h"

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
												const LatteContextRegister& lcr);
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
