#include "Cafe/HW/Latte/LegacyShaderDecompiler/LatteDecompiler.h"
#include "Cafe/HW/Latte/LegacyShaderDecompiler/LatteDecompilerInternal.h"
#include "Cafe/HW/Latte/LegacyShaderDecompiler/LatteDecompilerEmitHLSL.hpp"
#include "Cafe/HW/Latte/Core/FetchShader.h"
#include "Cafe/HW/Latte/Core/LatteConst.h"
#include "Cafe/HW/Latte/Core/LatteShader.h"
#include "Cafe/HW/Latte/Core/LatteCachedFBO.h"
#include "Cafe/HW/Latte/Core/LatteShaderAssembly.h"
#include "Cafe/HW/Latte/ISA/RegDefines.h"
#include "config/ActiveSettings.h"
#include "util/helpers/StringBuf.h"

#include <algorithm>
#include <string>

#define HLSL_NL "\r\n"

void LatteDecompiler_emitClauseCode(LatteDecompilerShaderContext* shaderContext, LatteDecompilerCFInstruction* cfInstruction, bool isSubroutine);

namespace
{
constexpr uint32 HLSL_UNIFORM_REGISTER_BYTES = 16;
constexpr sint32 D3D12_VERTEX_BUFFER_SRV_BASE = 32;
constexpr sint32 D3D12_LOGIC_OP_DST_SRV_BINDING = 96;

const char* GetShaderTypeName(LatteConst::ShaderType shaderType)
{
	switch (shaderType)
	{
	case LatteConst::ShaderType::Vertex:
		return "vertex";
	case LatteConst::ShaderType::Geometry:
		return "geometry";
	case LatteConst::ShaderType::Pixel:
		return "pixel";
	default:
		return "unknown";
	}
}

const char* GetStageSuffix(LatteConst::ShaderType shaderType)
{
	switch (shaderType)
	{
	case LatteConst::ShaderType::Vertex:
		return "VS";
	case LatteConst::ShaderType::Geometry:
		return "GS";
	case LatteConst::ShaderType::Pixel:
		return "PS";
	default:
		return "UNK";
	}
}

const char* GetInterpolationModifier(const LatteShaderPSInputTable::psImport_t* psImport)
{
	if (!psImport)
		return "";
	if (psImport->isFlat)
		return "nointerpolation ";
	if (psImport->isNoPerspective)
		return "noperspective ";
	return "";
}

const LatteShaderPSInputTable::psImport_t* FindPSInputBySemanticId(LatteShaderPSInputTable* psInputTable, uint32 semanticId)
{
	if (!psInputTable)
		return nullptr;
	for (sint32 i = 0; i < psInputTable->count; ++i)
	{
		if (psInputTable->import[i].semanticId == semanticId)
			return &psInputTable->import[i];
	}
	return nullptr;
}

void EmitSetPositionMacro(LatteDecompilerShaderContext* shaderContext, StringBuf* src)
{
	if (shaderContext->contextRegistersNew->PA_CL_CLIP_CNTL.get_DX_CLIP_SPACE_DEF())
		src->add("#define SET_POSITION(v) output.position = (v)" HLSL_NL);
	else
		src->add("#define SET_POSITION(v) output.position = (v); output.position.z = (output.position.z + output.position.w) / 2.0f" HLSL_NL);
}

std::string GetRegisterVarName(const LatteDecompilerShaderContext* shaderContext, uint32 index)
{
	const bool useInt = shaderContext->typeTracker.defaultDataType == LATTE_DECOMPILER_DTYPE_SIGNED_INT;
	if (shaderContext->typeTracker.useArrayGPRs)
		return fmt::format("{}[{}]", useInt ? "Ri" : "Rf", index);
	return fmt::format("R{}{}", index, useInt ? "i" : "f");
}

std::string GetActiveMaskVarName(const LatteDecompilerShaderContext* shaderContext, sint32 index)
{
	if (shaderContext->isSubroutine)
		return fmt::format("activeMaskStackSub{:04x}[{}]", shaderContext->subroutineInfo->cfAddr, index);
	return fmt::format("activeMaskStack[{}]", index);
}

std::string GetActiveMaskCVarName(const LatteDecompilerShaderContext* shaderContext, sint32 index)
{
	if (shaderContext->isSubroutine)
		return fmt::format("activeMaskStackCSub{:04x}[{}]", shaderContext->subroutineInfo->cfAddr, index);
	return fmt::format("activeMaskStackC[{}]", index);
}

const char* GetChannelName(uint32 channel)
{
	switch (channel)
	{
	case 0:
		return "x";
	case 1:
		return "y";
	case 2:
		return "z";
	case 3:
		return "w";
	default:
		return "x";
	}
}

std::string GetXYZWByMask(uint32 mask)
{
	std::string result;
	if ((mask & (1u << 0)) != 0)
		result += "x";
	if ((mask & (1u << 1)) != 0)
		result += "y";
	if ((mask & (1u << 2)) != 0)
		result += "z";
	if ((mask & (1u << 3)) != 0)
		result += "w";
	return result;
}

std::string GetRegisterScalarExpression(const LatteDecompilerShaderContext* shaderContext, uint32 registerIndex, uint32 channel, sint32 requiredType)
{
	const std::string scalar = fmt::format("{}.{}", GetRegisterVarName(shaderContext, registerIndex), GetChannelName(channel));
	const sint32 sourceType = shaderContext->typeTracker.defaultDataType;
	if (sourceType == requiredType)
		return scalar;
	if (requiredType == LATTE_DECOMPILER_DTYPE_FLOAT)
		return fmt::format("asfloat({})", scalar);
	if (requiredType == LATTE_DECOMPILER_DTYPE_SIGNED_INT)
		return fmt::format("asint({})", scalar);
	return scalar;
}

// integer color buffers need integer pixel shader outputs
sint32 GetHLSLPixelOutputType(const LatteDecompilerShaderContext* shaderContext, uint32 target)
{
	const uint32 format = (uint32)LatteMRT::GetColorBufferFormat(target, *shaderContext->contextRegistersNew);
	if ((format & (uint32)Latte::E_GX2SURFFMT::FMT_BIT_INT) == 0)
		return LATTE_DECOMPILER_DTYPE_FLOAT;
	return (format & (uint32)Latte::E_GX2SURFFMT::FMT_BIT_SIGNED) != 0 ? LATTE_DECOMPILER_DTYPE_SIGNED_INT : LATTE_DECOMPILER_DTYPE_UNSIGNED_INT;
}

const char* GetHLSLPixelOutputTypeName(sint32 outputType)
{
	if (outputType == LATTE_DECOMPILER_DTYPE_SIGNED_INT)
		return "int4";
	if (outputType == LATTE_DECOMPILER_DTYPE_UNSIGNED_INT)
		return "uint4";
	return "float4";
}

void EmitHLSLExportReadCode(LatteDecompilerShaderContext* shaderContext, LatteDecompilerCFInstruction* cfInstruction, sint32 requiredType, uint32 burstIndex)
{
	StringBuf* src = shaderContext->shaderSource;
	uint32 numOutputs = 4;
	if (cfInstruction->type == GPU7_CF_INST_MEM_RING_WRITE)
	{
		numOutputs = 0;
		for (uint32 i = 0; i < 4; ++i)
		{
			if ((cfInstruction->memWriteCompMask & (1u << i)) != 0)
				++numOutputs;
		}
	}
	numOutputs = std::max<uint32>(numOutputs, 1);

	if (requiredType == LATTE_DECOMPILER_DTYPE_FLOAT)
		src->add(numOutputs == 1 ? "(float)(" : fmt::format("float{}(", numOutputs));
	else if (requiredType == LATTE_DECOMPILER_DTYPE_SIGNED_INT)
		src->add(numOutputs == 1 ? "(int)(" : fmt::format("int{}(", numOutputs));
	else
	{
		cemu_assert_unimplemented();
		src->add(numOutputs == 1 ? "(float)(" : fmt::format("float{}(", numOutputs));
	}

	uint32 actualOutputs = 0;
	for (uint32 i = 0; i < 4; ++i)
	{
		uint32 exportSel = 0;
		if (cfInstruction->type == GPU7_CF_INST_MEM_RING_WRITE)
		{
			exportSel = i;
			if ((cfInstruction->memWriteCompMask & (1u << i)) == 0)
				continue;
		}
		else
			exportSel = cfInstruction->exportComponentSel[i];

		if (actualOutputs++ != 0)
			src->add(", ");

		if (exportSel < 4)
			src->add(GetRegisterScalarExpression(shaderContext, cfInstruction->exportSourceGPR + burstIndex, exportSel, requiredType));
		else if (exportSel == 4 || exportSel == 7)
			src->add(requiredType == LATTE_DECOMPILER_DTYPE_FLOAT ? "0.0f" : "0");
		else if (exportSel == 5)
			src->add(requiredType == LATTE_DECOMPILER_DTYPE_FLOAT ? "1.0f" : "1");
		else
		{
			cemu_assert_debug(false);
			src->add(requiredType == LATTE_DECOMPILER_DTYPE_FLOAT ? "0.0f" : "0");
		}
	}
	src->add(")");
}

uint32 AlignUniformRegister(uint32 offset)
{
	return (offset + HLSL_UNIFORM_REGISTER_BYTES - 1) & ~(HLSL_UNIFORM_REGISTER_BYTES - 1);
}

uint32 AllocateUniformRegisters(uint32& offset, uint32 registerCount)
{
	offset = AlignUniformRegister(offset);
	const uint32 result = offset;
	offset += registerCount * HLSL_UNIFORM_REGISTER_BYTES;
	return result;
}

void EmitBindingSummary(LatteDecompilerShaderContext* shaderContext, StringBuf* src)
{
	const auto& mapping = shaderContext->output->resourceMappingD3D12;
	src->addFmt("// D3D12 register space: {}" HLSL_NL, (sint32)mapping.setIndex);
	src->addFmt("// Uniform variable CBV: b{}" HLSL_NL, (sint32)mapping.uniformVarsBufferBindingPoint);
	src->add("// Texture SRVs:" HLSL_NL);
	for (sint32 i = 0; i < LATTE_NUM_MAX_TEX_UNITS; ++i)
	{
	if (mapping.textureUnitToBindingPoint[i] >= 0)
			src->addFmt("//   texture unit {} -> t{}, s{}" HLSL_NL, i, (sint32)mapping.textureUnitToBindingPoint[i], (sint32)mapping.textureUnitToBindingPoint[i]);
	}
	src->add("// Uniform-bank SRVs:" HLSL_NL);
	for (sint32 i = 0; i < LATTE_NUM_MAX_UNIFORM_BUFFERS; ++i)
	{
		if (mapping.uniformBuffersBindingPoint[i] >= 0)
			src->addFmt("//   bank {} -> t{}" HLSL_NL, i, (sint32)mapping.uniformBuffersBindingPoint[i]);
	}
	if (mapping.tfStorageBindingPoint >= 0)
		src->addFmt("// Streamout UAV: u{}" HLSL_NL, (sint32)mapping.tfStorageBindingPoint);
	if (shaderContext->shaderType == LatteConst::ShaderType::Vertex && shaderContext->fetchShader)
	{
		src->add("// Vertex buffer SRVs:" HLSL_NL);
		for (auto& group : shaderContext->fetchShader->bufferGroups)
		{
			if (group.attributeBufferIndex < LATTE_MAX_VERTEX_BUFFERS)
				src->addFmt("//   vertex buffer {} -> t{}" HLSL_NL, group.attributeBufferIndex, D3D12_VERTEX_BUFFER_SRV_BASE + (sint32)group.attributeBufferIndex);
		}
	}
}

void EmitTextureQueryLodHelpers(StringBuf* src, bool hasIntegerTypes, bool hasDepthCompareTypes)
{
	src->add("float2 latteTextureQueryLodFromTexelCoord(float3 texelCoord, uint levels)" HLSL_NL "{" HLSL_NL);
	src->add("\tconst float lod = latteTextureLodFromGradients(ddx(texelCoord), ddy(texelCoord));" HLSL_NL);
	src->add("\treturn float2(clamp(lod, 0.0f, (float)(levels - 1u)), lod);" HLSL_NL "}" HLSL_NL);

	auto emitHardwareQuery = [&](const char* textureType, const char* coordType, const char* location)
	{
		src->addFmt("float2 latteTextureQueryLod({} tex, SamplerState samplerState, {} coord) {{ return float2(tex.CalculateLevelOfDetail(samplerState, {}), tex.CalculateLevelOfDetailUnclamped(samplerState, {})); }}" HLSL_NL,
			textureType, coordType, location, location);
	};
	auto emitDerivativeQuery = [&](const char* textureType, const char* samplerType, const char* coordType, const char* dimensions, const char* texelCoord)
	{
		src->addFmt("float2 latteTextureQueryLod({} tex, {} samplerState, {} coord) {{ uint {}; tex.GetDimensions(0u, {}); return latteTextureQueryLodFromTexelCoord({}, levels); }}" HLSL_NL,
			textureType, samplerType, coordType, dimensions, dimensions, texelCoord);
	};

	emitHardwareQuery("Texture1D<float4>", "float2", "coord.x");
	emitHardwareQuery("Texture1DArray<float4>", "float2", "coord.x");
	emitHardwareQuery("Texture2D<float4>", "float2", "coord");
	emitHardwareQuery("Texture2DArray<float4>", "float2", "coord");
	emitHardwareQuery("Texture3D<float4>", "float2", "float3(coord, 0.0f)");
	emitHardwareQuery("TextureCubeArray<float4>", "float3", "coord");

	// integer and comparison-sampled textures cannot use the LOD query intrinsics
	if (hasIntegerTypes)
	{
		emitDerivativeQuery("Texture1D<uint4>", "SamplerState", "float2", "width, levels", "float3(coord.x * width, 0.0f, 0.0f)");
		emitDerivativeQuery("Texture1DArray<uint4>", "SamplerState", "float2", "width, elements, levels", "float3(coord.x * width, 0.0f, 0.0f)");
		emitDerivativeQuery("Texture2D<uint4>", "SamplerState", "float2", "width, height, levels", "float3(coord * float2(width, height), 0.0f)");
		emitDerivativeQuery("Texture2DArray<uint4>", "SamplerState", "float2", "width, height, elements, levels", "float3(coord * float2(width, height), 0.0f)");
		emitDerivativeQuery("Texture3D<uint4>", "SamplerState", "float2", "width, height, depth, levels", "float3(coord * float2(width, height), 0.0f)");
		emitDerivativeQuery("TextureCubeArray<uint4>", "SamplerState", "float3", "width, height, elements, levels", "float3(latteCubeFaceCoord(coord) * float2(width, height), 0.0f)");
	}
	if (hasDepthCompareTypes)
	{
		emitDerivativeQuery("Texture1D<float>", "SamplerComparisonState", "float2", "width, levels", "float3(coord.x * width, 0.0f, 0.0f)");
		emitDerivativeQuery("Texture1DArray<float>", "SamplerComparisonState", "float2", "width, elements, levels", "float3(coord.x * width, 0.0f, 0.0f)");
		emitDerivativeQuery("Texture2D<float>", "SamplerComparisonState", "float2", "width, height, levels", "float3(coord * float2(width, height), 0.0f)");
		emitDerivativeQuery("Texture2DArray<float>", "SamplerComparisonState", "float2", "width, height, elements, levels", "float3(coord * float2(width, height), 0.0f)");
		emitDerivativeQuery("TextureCubeArray<float>", "SamplerComparisonState", "float3", "width, height, elements, levels", "float3(latteCubeFaceCoord(coord) * float2(width, height), 0.0f)");
	}
}

void EmitCompatHelpers(LatteDecompilerShaderContext* shaderContext, StringBuf* src)
{
	bool hasIntegerTexture = false;
	for (sint32 i = 0; i < LATTE_NUM_MAX_TEX_UNITS; i++)
	{
		if (shaderContext->output->textureUnitMask[i] && shaderContext->shader->textureIsIntegerFormat[i])
			hasIntegerTexture = true;
	}
	const bool hasTextureLodQuery = shaderContext->shader->shaderType == LatteConst::ShaderType::Pixel && shaderContext->analyzer.hasTextureLodQuery;

	src->add(R"(
typedef float2 vec2;
typedef float3 vec3;
typedef float4 vec4;
typedef int2 ivec2;
typedef int3 ivec3;
typedef int4 ivec4;
typedef uint2 uvec2;
typedef uint3 uvec3;
typedef uint4 uvec4;
typedef bool2 bvec2;
typedef bool3 bvec3;
typedef bool4 bvec4;

float2 make_vec2(float v) { return float2(v, v); }
float2 make_vec2(float x, float y) { return float2(x, y); }
float2 make_vec2(int2 v) { return float2(v); }
float2 make_vec2(uint2 v) { return float2(v); }
float3 make_vec3(float v) { return float3(v, v, v); }
float3 make_vec3(float x, float y, float z) { return float3(x, y, z); }
float3 make_vec3(float2 xy, float z) { return float3(xy, z); }
float3 make_vec3(int3 v) { return float3(v); }
float3 make_vec3(uint3 v) { return float3(v); }
float4 make_vec4(float v) { return float4(v, v, v, v); }
float4 make_vec4(float x, float y, float z, float w) { return float4(x, y, z, w); }
float4 make_vec4(float2 xy, float z, float w) { return float4(xy, z, w); }
float4 make_vec4(float2 xy, float2 zw) { return float4(xy, zw); }
float4 make_vec4(float3 xyz, float w) { return float4(xyz, w); }
float4 make_vec4(int4 v) { return float4(v); }
float4 make_vec4(uint4 v) { return float4(v); }
int2 make_ivec2(int v) { return int2(v, v); }
int2 make_ivec2(int x, int y) { return int2(x, y); }
int2 make_ivec2(float2 v) { return int2(v); }
int2 make_ivec2(int2 v) { return v; }
int2 make_ivec2(uint2 v) { return int2(v); }
int3 make_ivec3(int v) { return int3(v, v, v); }
int3 make_ivec3(int x, int y, int z) { return int3(x, y, z); }
int3 make_ivec3(int2 xy, int z) { return int3(xy, z); }
int3 make_ivec3(float3 v) { return int3(v); }
int3 make_ivec3(int3 v) { return v; }
int3 make_ivec3(uint3 v) { return int3(v); }
int4 make_ivec4(int v) { return int4(v, v, v, v); }
int4 make_ivec4(int x, int y, int z, int w) { return int4(x, y, z, w); }
int4 make_ivec4(int2 xy, int z, int w) { return int4(xy, z, w); }
int4 make_ivec4(int3 xyz, int w) { return int4(xyz, w); }
int4 make_ivec4(float4 v) { return int4(v); }
int4 make_ivec4(uint4 v) { return int4(v); }
int4 make_ivec4(int4 v) { return v; }
uint2 make_uvec2(uint v) { return uint2(v, v); }
uint2 make_uvec2(uint x, uint y) { return uint2(x, y); }
uint3 make_uvec3(uint v) { return uint3(v, v, v); }
uint3 make_uvec3(uint x, uint y, uint z) { return uint3(x, y, z); }
uint4 make_uvec4(uint v) { return uint4(v, v, v, v); }
uint4 make_uvec4(uint x, uint y, uint z, uint w) { return uint4(x, y, z, w); }

#define vec2(...) make_vec2(__VA_ARGS__)
#define vec3(...) make_vec3(__VA_ARGS__)
#define vec4(...) make_vec4(__VA_ARGS__)
#define ivec2(...) make_ivec2(__VA_ARGS__)
#define ivec3(...) make_ivec3(__VA_ARGS__)
#define ivec4(...) make_ivec4(__VA_ARGS__)
#define uvec2(...) make_uvec2(__VA_ARGS__)
#define uvec3(...) make_uvec3(__VA_ARGS__)
#define uvec4(...) make_uvec4(__VA_ARGS__)
#define uintBitsToFloat asfloat
#define intBitsToFloat asfloat
#define floatBitsToUint asuint
#define floatBitsToInt asint
#define dFdx ddx
#define dFdy ddy
#define fract frac
#define roundEven round
#define inversesqrt rsqrt
#define LATTE_LOAD_U8(buffer, byteOffset) (((buffer).Load(((byteOffset) >> 2u)) >> (((byteOffset) & 3u) * 8u)) & 0xffu)
#define LATTE_LOAD_U16_LE(buffer, byteOffset) (LATTE_LOAD_U8(buffer, byteOffset) | (LATTE_LOAD_U8(buffer, ((byteOffset) + 1u)) << 8u))
#define LATTE_LOAD_U16_BE(buffer, byteOffset) ((LATTE_LOAD_U8(buffer, byteOffset) << 8u) | LATTE_LOAD_U8(buffer, ((byteOffset) + 1u)))
#define LATTE_LOAD_U32_LE(buffer, byteOffset) (LATTE_LOAD_U8(buffer, byteOffset) | (LATTE_LOAD_U8(buffer, ((byteOffset) + 1u)) << 8u) | (LATTE_LOAD_U8(buffer, ((byteOffset) + 2u)) << 16u) | (LATTE_LOAD_U8(buffer, ((byteOffset) + 3u)) << 24u))
#define LATTE_LOAD_U32_BE(buffer, byteOffset) ((LATTE_LOAD_U8(buffer, byteOffset) << 24u) | (LATTE_LOAD_U8(buffer, ((byteOffset) + 1u)) << 16u) | (LATTE_LOAD_U8(buffer, ((byteOffset) + 2u)) << 8u) | LATTE_LOAD_U8(buffer, ((byteOffset) + 3u)))

float mix(float a, float b, bool t) { return t ? b : a; }
float2 mix(float2 a, float2 b, bool2 t) { return float2(t.x ? b.x : a.x, t.y ? b.y : a.y); }
float3 mix(float3 a, float3 b, bool3 t) { return float3(t.x ? b.x : a.x, t.y ? b.y : a.y, t.z ? b.z : a.z); }
float4 mix(float4 a, float4 b, bool4 t) { return float4(t.x ? b.x : a.x, t.y ? b.y : a.y, t.z ? b.z : a.z, t.w ? b.w : a.w); }
float mix(float a, float b, float t) { return lerp(a, b, t); }
float2 mix(float2 a, float2 b, float2 t) { return lerp(a, b, t); }
float3 mix(float3 a, float3 b, float3 t) { return lerp(a, b, t); }
float4 mix(float4 a, float4 b, float4 t) { return lerp(a, b, t); }

int clampFI32(int v)
{
	if (v == 0x7FFFFFFF)
		return asint(1.0f);
	if (v == -1)
		return asint(0.0f);
	return asint(clamp(asfloat(v), 0.0f, 1.0f));
}

float mul_nonIEEE(float a, float b)
{
	if (a == 0.0f || b == 0.0f)
		return 0.0f;
	return a * b;
}

uint4 lattePackUnorm8(float4 v)
{
	return (uint4)round(saturate(v) * 255.0f);
}

float4 latteUnpackUnorm8(uint4 v)
{
	return (float4)(v & 0xffu) / 255.0f;
}

)");
#if defined(CEMU_UWP)
	src->add(R"(
#undef LATTE_LOAD_U16_LE
#undef LATTE_LOAD_U16_BE
#undef LATTE_LOAD_U32_LE
#undef LATTE_LOAD_U32_BE

uint latteLoadU16LE(Buffer<uint> buffer, uint byteOffset)
{
	const uint wordIndex = byteOffset >> 2u;
	const uint shift = (byteOffset & 3u) * 8u;
	const uint low = buffer.Load(wordIndex);
	if (shift <= 16u)
		return (low >> shift) & 0xffffu;
	return ((low >> 24u) & 0xffu) | ((buffer.Load(wordIndex + 1u) & 0xffu) << 8u);
}

uint latteLoadU32LE(Buffer<uint> buffer, uint byteOffset)
{
	const uint wordIndex = byteOffset >> 2u;
	const uint shift = (byteOffset & 3u) * 8u;
	const uint low = buffer.Load(wordIndex);
	if (shift == 0u)
		return low;
	return (low >> shift) | (buffer.Load(wordIndex + 1u) << (32u - shift));
}

uint latteByteSwap16(uint value)
{
	return ((value & 0xffu) << 8u) | ((value >> 8u) & 0xffu);
}

uint latteByteSwap32(uint value)
{
	return ((value & 0x000000ffu) << 24u) |
		((value & 0x0000ff00u) << 8u) |
		((value & 0x00ff0000u) >> 8u) |
		((value & 0xff000000u) >> 24u);
}

#define LATTE_LOAD_U16_LE(buffer, byteOffset) latteLoadU16LE(buffer, byteOffset)
#define LATTE_LOAD_U16_BE(buffer, byteOffset) latteByteSwap16(latteLoadU16LE(buffer, byteOffset))
#define LATTE_LOAD_U32_LE(buffer, byteOffset) latteLoadU32LE(buffer, byteOffset)
#define LATTE_LOAD_U32_BE(buffer, byteOffset) latteByteSwap32(latteLoadU32LE(buffer, byteOffset))
)");

	src->add(R"(
float4 latteLogicOpUnorm8(float4 src, float4 dst, uint mode)
{
	uint4 srcBits = lattePackUnorm8(src);
	uint4 dstBits = lattePackUnorm8(dst);
	uint4 result = srcBits;
	switch (mode)
	{
	case 1u: result = 0u; break;
	case 2u: result = srcBits | dstBits; break;
	case 3u: result = 0xffu; break;
	case 4u: result = ~srcBits; break;
	case 5u: result = dstBits; break;
	case 6u: result = ~dstBits; break;
	case 7u: result = srcBits & dstBits; break;
	case 8u: result = ~(srcBits & dstBits); break;
	case 9u: result = ~(srcBits | dstBits); break;
	case 10u: result = srcBits ^ dstBits; break;
	case 11u: result = ~(srcBits ^ dstBits); break;
	case 12u: result = srcBits & ~dstBits; break;
	case 13u: result = ~srcBits & dstBits; break;
	case 14u: result = srcBits | ~dstBits; break;
	case 15u: result = ~srcBits | dstBits; break;
	default: break;
	}
	return latteUnpackUnorm8(result);
}
)");
#else
	src->add(R"(
float4 latteLogicOpUnorm8(float4 src, float4 dst, uint mode)
{
	if (mode == 1u)
		return float4(0.0f, 0.0f, 0.0f, 0.0f);
	if (mode == 3u)
		return float4(1.0f, 1.0f, 1.0f, 1.0f);
	if (mode == 2u)
	{
		uint4 srcBits = lattePackUnorm8(src);
		uint4 dstBits = lattePackUnorm8(dst);
		return latteUnpackUnorm8(srcBits | dstBits);
	}
	return src;
}
)");
#endif
	src->add(R"(

int textureSize(Texture1D<float4> tex, int lod)
{
	uint w, levels;
	tex.GetDimensions((uint)lod, w, levels);
	return (int)w;
}

int textureSize(Texture1D<uint4> tex, int lod)
{
	uint w, levels;
	tex.GetDimensions((uint)lod, w, levels);
	return (int)w;
}

int2 textureSize(Texture1DArray<float4> tex, int lod)
{
	uint w, elements, levels;
	tex.GetDimensions((uint)lod, w, elements, levels);
	return int2(w, elements);
}

int2 textureSize(Texture1DArray<uint4> tex, int lod)
{
	uint w, elements, levels;
	tex.GetDimensions((uint)lod, w, elements, levels);
	return int2(w, elements);
}

int2 textureSize(Texture2D<float4> tex, int lod)
{
	uint w, h, levels;
	tex.GetDimensions((uint)lod, w, h, levels);
	return int2(w, h);
}

int2 textureSize(Texture2D<uint4> tex, int lod)
{
	uint w, h, levels;
	tex.GetDimensions((uint)lod, w, h, levels);
	return int2(w, h);
}

int3 textureSize(Texture2DArray<float4> tex, int lod)
{
	uint w, h, elements, levels;
	tex.GetDimensions((uint)lod, w, h, elements, levels);
	return int3(w, h, elements);
}

int3 textureSize(Texture2DArray<uint4> tex, int lod)
{
	uint w, h, elements, levels;
	tex.GetDimensions((uint)lod, w, h, elements, levels);
	return int3(w, h, elements);
}

int3 textureSize(Texture3D<float4> tex, int lod)
{
	uint w, h, d, levels;
	tex.GetDimensions((uint)lod, w, h, d, levels);
	return int3(w, h, d);
}

int3 textureSize(Texture3D<uint4> tex, int lod)
{
	uint w, h, d, levels;
	tex.GetDimensions((uint)lod, w, h, d, levels);
	return int3(w, h, d);
}

int3 textureSize(TextureCubeArray<float4> tex, int lod)
{
	uint w, h, elements, levels;
	tex.GetDimensions((uint)lod, w, h, elements, levels);
	return int3(w, h, elements);
}

int3 textureSize(TextureCubeArray<uint4> tex, int lod)
{
	uint w, h, elements, levels;
	tex.GetDimensions((uint)lod, w, h, elements, levels);
	return int3(w, h, elements);
}

float4 latteTexelFetch(Texture1D<float4> tex, int coord, int lod) { return tex.Load(int2(coord, lod)); }
uint4 latteTexelFetch(Texture1D<uint4> tex, int coord, int lod) { return tex.Load(int2(coord, lod)); }
float4 latteTexelFetch(Texture2D<float4> tex, int2 coord, int lod) { return tex.Load(int3(coord, lod)); }
uint4 latteTexelFetch(Texture2D<uint4> tex, int2 coord, int lod) { return tex.Load(int3(coord, lod)); }
float4 latteTexelFetch(Texture2DArray<float4> tex, int3 coord, int lod) { return tex.Load(int4(coord.xy, coord.z, lod)); }
uint4 latteTexelFetch(Texture2DArray<uint4> tex, int3 coord, int lod) { return tex.Load(int4(coord.xy, coord.z, lod)); }
float4 latteTexelFetch(Texture3D<float4> tex, int3 coord, int lod) { return tex.Load(int4(coord, lod)); }
uint4 latteTexelFetch(Texture3D<uint4> tex, int3 coord, int lod) { return tex.Load(int4(coord, lod)); }

// GatherRed already has GLSL textureGather order. The shared FETCH4 emitter
// applies the guest component mapping; another permutation here corrupts it.
float4 latteTextureGather(Texture2D<float4> tex, SamplerState samplerState, float2 coord) { return tex.GatherRed(samplerState, coord); }
uint4 latteTextureGather(Texture2D<uint4> tex, SamplerState samplerState, float2 coord) { return tex.GatherRed(samplerState, coord); }
float4 latteTextureGather(Texture2DArray<float4> tex, SamplerState samplerState, float3 coord) { return tex.GatherRed(samplerState, coord); }
uint4 latteTextureGather(Texture2DArray<uint4> tex, SamplerState samplerState, float3 coord) { return tex.GatherRed(samplerState, coord); }
float4 latteTextureGather(TextureCubeArray<float4> tex, SamplerState samplerState, float4 coord) { return tex.GatherRed(samplerState, coord); }
uint4 latteTextureGather(TextureCubeArray<uint4> tex, SamplerState samplerState, float4 coord) { return tex.GatherRed(samplerState, coord); }

void redcCUBE(float4 src0, float4 src1, out float3 stm, out int faceId)
{
	float3 inputCoord = normalize(float3(src1.y, src1.x, src0.x));
	float rx = inputCoord.x;
	float ry = inputCoord.y;
	float rz = inputCoord.z;
	if (abs(rx) > abs(ry) && abs(rx) > abs(rz))
	{
		stm.z = rx * 2.0f;
		stm.xy = float2(ry, rz);
		faceId = rx >= 0.0f ? 0 : 1;
	}
	else if (abs(ry) > abs(rx) && abs(ry) > abs(rz))
	{
		stm.z = ry * 2.0f;
		stm.xy = float2(rx, rz);
		faceId = ry >= 0.0f ? 2 : 3;
	}
	else
	{
		stm.z = rz * 2.0f;
		stm.xy = float2(rx, ry);
		faceId = rz >= 0.0f ? 4 : 5;
	}
}

float3 redcCUBEReverse(float2 st, int faceId)
{
	st.yx = st.xy;
	float3 v;
	float majorAxis = 1.0f;
	if (faceId == 0)
	{
		v.yz = (st - float2(1.5f, 1.5f)) * (majorAxis * 2.0f);
		v.x = 1.0f;
	}
	else if (faceId == 1)
	{
		v.yz = (st - float2(1.5f, 1.5f)) * (majorAxis * 2.0f);
		v.x = -1.0f;
	}
	else if (faceId == 2)
	{
		v.xz = (st - float2(1.5f, 1.5f)) * (majorAxis * 2.0f);
		v.y = 1.0f;
	}
	else if (faceId == 3)
	{
		v.xz = (st - float2(1.5f, 1.5f)) * (majorAxis * 2.0f);
		v.y = -1.0f;
	}
	else if (faceId == 4)
	{
		v.xy = (st - float2(1.5f, 1.5f)) * (majorAxis * 2.0f);
		v.z = 1.0f;
	}
	else
	{
		v.xy = (st - float2(1.5f, 1.5f)) * (majorAxis * 2.0f);
		v.z = -1.0f;
	}
	return v;
}

float latteTextureLodFromGradients(float3 dx, float3 dy)
{
	return 0.5f * log2(max(max(dot(dx, dx), dot(dy, dy)), 1.0e-30f));
}

float2 latteCubeFaceCoord(float3 direction)
{
	const float3 magnitude = abs(direction);
	if (magnitude.x >= magnitude.y && magnitude.x >= magnitude.z)
		return float2(direction.x >= 0.0f ? -direction.z : direction.z, -direction.y) / (2.0f * magnitude.x) + 0.5f;
	if (magnitude.y >= magnitude.z)
		return float2(direction.x, direction.y >= 0.0f ? direction.z : -direction.z) / (2.0f * magnitude.y) + 0.5f;
	return float2(direction.z >= 0.0f ? direction.x : -direction.x, -direction.y) / (2.0f * magnitude.z) + 0.5f;
}

)" HLSL_NL);

#if defined(CEMU_UWP)
	src->add(R"(
int textureSize(Texture1D<float> tex, int lod)
{
	uint width, levels;
	tex.GetDimensions((uint)lod, width, levels);
	return (int)width;
}

int2 textureSize(Texture1DArray<float> tex, int lod)
{
	uint width, elements, levels;
	tex.GetDimensions((uint)lod, width, elements, levels);
	return int2(width, elements);
}

int2 textureSize(Texture2D<float> tex, int lod)
{
	uint width, height, levels;
	tex.GetDimensions((uint)lod, width, height, levels);
	return int2(width, height);
}

int3 textureSize(Texture2DArray<float> tex, int lod)
{
	uint width, height, elements, levels;
	tex.GetDimensions((uint)lod, width, height, elements, levels);
	return int3(width, height, elements);
}

int3 textureSize(TextureCubeArray<float> tex, int lod)
{
	uint width, height, elements, levels;
	tex.GetDimensions((uint)lod, width, height, elements, levels);
	return int3(width, height, elements);
}
)" HLSL_NL);

	if (hasIntegerTexture)
	{
		src->add(R"(
int latteIntegerTextureLevel(float lod, uint levels)
{
	return (int)clamp(floor(lod + 0.5f), 0.0f, (float)(levels - 1u));
}

uint4 latteIntegerTextureLoad(Texture1D<uint4> tex, SamplerState samplerState, float coord, float lod, int offset)
{
	uint width, levels;
	tex.GetDimensions(0u, width, levels);
	const int level = latteIntegerTextureLevel(lod, levels);
	tex.GetDimensions((uint)level, width, levels);
	return tex.Load(int2(clamp((int)floor(coord * width) + offset, 0, (int)width - 1), level));
}

uint4 latteIntegerTextureLoad(Texture1DArray<uint4> tex, SamplerState samplerState, float2 coord, float lod, int offset)
{
	uint width, elements, levels;
	tex.GetDimensions(0u, width, elements, levels);
	const int level = latteIntegerTextureLevel(lod, levels);
	tex.GetDimensions((uint)level, width, elements, levels);
	const int layer = clamp((int)floor(coord.y + 0.5f), 0, (int)elements - 1);
	return tex.Load(int3(clamp((int)floor(coord.x * width) + offset, 0, (int)width - 1), layer, level));
}

uint4 latteIntegerTextureLoad(Texture2D<uint4> tex, SamplerState samplerState, float2 coord, float lod, int2 offset)
{
	uint width, height, levels;
	tex.GetDimensions(0u, width, height, levels);
	const int level = latteIntegerTextureLevel(lod, levels);
	tex.GetDimensions((uint)level, width, height, levels);
	const int2 size = int2(width, height);
	return tex.Load(int3(clamp((int2)floor(coord * size) + offset, 0, size - 1), level));
}

uint4 latteIntegerTextureLoad(Texture2DArray<uint4> tex, SamplerState samplerState, float3 coord, float lod, int2 offset)
{
	uint width, height, elements, levels;
	tex.GetDimensions(0u, width, height, elements, levels);
	const int level = latteIntegerTextureLevel(lod, levels);
	tex.GetDimensions((uint)level, width, height, elements, levels);
	const int2 size = int2(width, height);
	const int layer = clamp((int)floor(coord.z + 0.5f), 0, (int)elements - 1);
	return tex.Load(int4(clamp((int2)floor(coord.xy * size) + offset, 0, size - 1), layer, level));
}

uint4 latteIntegerTextureLoad(Texture3D<uint4> tex, SamplerState samplerState, float3 coord, float lod, int3 offset)
{
	uint width, height, depth, levels;
	tex.GetDimensions(0u, width, height, depth, levels);
	const int level = latteIntegerTextureLevel(lod, levels);
	tex.GetDimensions((uint)level, width, height, depth, levels);
	const int3 size = int3(width, height, depth);
	return tex.Load(int4(clamp((int3)floor(coord * size) + offset, 0, size - 1), level));
}

// cube resources cannot be loaded; gather reads the most detailed mip in the order (0,1) (1,1) (1,0) (0,0)
uint4 latteIntegerTextureLoad(TextureCubeArray<uint4> tex, SamplerState samplerState, float4 coord, float lod)
{
	uint width, height, elements, levels;
	tex.GetDimensions(0u, width, height, elements, levels);
	const float2 footprint = frac(latteCubeFaceCoord(coord.xyz) * float2(width, height) - 0.5f);
	const uint corner = footprint.y >= 0.5f ? (footprint.x >= 0.5f ? 1u : 0u) : (footprint.x >= 0.5f ? 2u : 3u);
	return uint4(tex.GatherRed(samplerState, coord)[corner], tex.GatherGreen(samplerState, coord)[corner], tex.GatherBlue(samplerState, coord)[corner], tex.GatherAlpha(samplerState, coord)[corner]);
}

float latteIntegerTextureLod(Texture1D<uint4> tex, float dx, float dy)
{
	uint width, levels;
	tex.GetDimensions(0u, width, levels);
	return latteTextureLodFromGradients(float3(dx * width, 0.0f, 0.0f), float3(dy * width, 0.0f, 0.0f));
}

float latteIntegerTextureLod(Texture1DArray<uint4> tex, float2 dx, float2 dy)
{
	uint width, elements, levels;
	tex.GetDimensions(0u, width, elements, levels);
	return latteTextureLodFromGradients(float3(dx.x * width, 0.0f, 0.0f), float3(dy.x * width, 0.0f, 0.0f));
}

float latteIntegerTextureLod(Texture2D<uint4> tex, float2 dx, float2 dy)
{
	uint width, height, levels;
	tex.GetDimensions(0u, width, height, levels);
	return latteTextureLodFromGradients(float3(dx * float2(width, height), 0.0f), float3(dy * float2(width, height), 0.0f));
}

float latteIntegerTextureLod(Texture2DArray<uint4> tex, float3 dx, float3 dy)
{
	uint width, height, elements, levels;
	tex.GetDimensions(0u, width, height, elements, levels);
	return latteTextureLodFromGradients(float3(dx.xy * float2(width, height), 0.0f), float3(dy.xy * float2(width, height), 0.0f));
}

float latteIntegerTextureLod(Texture3D<uint4> tex, float3 dx, float3 dy)
{
	uint width, height, depth, levels;
	tex.GetDimensions(0u, width, height, depth, levels);
	return latteTextureLodFromGradients(dx * float3(width, height, depth), dy * float3(width, height, depth));
}

float latteIntegerTextureLod(TextureCubeArray<uint4> tex, float4 dx, float4 dy)
{
	return 0.0f;
}
)" HLSL_NL);
	}

	const bool isPixelShader = shaderContext->shader->shaderType == LatteConst::ShaderType::Pixel;
	auto emitSamplingHelpers = [&](const char* textureType, const char* resultType, const char* coordType, const char* offsetType)
	{
		if (isPixelShader)
		{
			src->addFmt("{} latteTextureSample({} tex, SamplerState samplerState, {} coord) {{ return tex.Sample(samplerState, coord); }}" HLSL_NL,
				resultType, textureType, coordType);
			src->addFmt("{} latteTextureSample({} tex, SamplerState samplerState, {} coord, float bias) {{ return tex.SampleBias(samplerState, coord, bias); }}" HLSL_NL,
				resultType, textureType, coordType);
			src->addFmt("{} latteTextureSampleOffset({} tex, SamplerState samplerState, {} coord, float bias) {{ return tex.SampleBias(samplerState, coord, bias); }}" HLSL_NL,
				resultType, textureType, coordType);
		}
		else
		{
			src->addFmt("{} latteTextureSample({} tex, SamplerState samplerState, {} coord) {{ return tex.SampleLevel(samplerState, coord, 0.0f); }}" HLSL_NL,
				resultType, textureType, coordType);
			src->addFmt("{} latteTextureSample({} tex, SamplerState samplerState, {} coord, float bias) {{ return tex.SampleLevel(samplerState, coord, bias); }}" HLSL_NL,
				resultType, textureType, coordType);
			src->addFmt("{} latteTextureSampleOffset({} tex, SamplerState samplerState, {} coord, float bias) {{ return tex.SampleLevel(samplerState, coord, bias); }}" HLSL_NL,
				resultType, textureType, coordType);
		}

		src->addFmt("{} latteTextureSampleLevel({} tex, SamplerState samplerState, {} coord, float lod) {{ return tex.SampleLevel(samplerState, coord, lod); }}" HLSL_NL,
			resultType, textureType, coordType);
		if (offsetType)
		{
			if (isPixelShader)
				src->addFmt("{} latteTextureSampleOffset({} tex, SamplerState samplerState, {} coord, {} offset) {{ return tex.Sample(samplerState, coord, offset); }}" HLSL_NL,
					resultType, textureType, coordType, offsetType);
			else
				src->addFmt("{} latteTextureSampleOffset({} tex, SamplerState samplerState, {} coord, {} offset) {{ return tex.SampleLevel(samplerState, coord, 0.0f, offset); }}" HLSL_NL,
					resultType, textureType, coordType, offsetType);
			src->addFmt("{} latteTextureSampleLevelOffset({} tex, SamplerState samplerState, {} coord, float lod, {} offset) {{ return tex.SampleLevel(samplerState, coord, lod, offset); }}" HLSL_NL,
				resultType, textureType, coordType, offsetType);
		}
	};

	emitSamplingHelpers("Texture1D<float4>", "float4", "float", "int");
	emitSamplingHelpers("Texture1DArray<float4>", "float4", "float2", "int");
	emitSamplingHelpers("Texture2D<float4>", "float4", "float2", "int2");
	emitSamplingHelpers("Texture2DArray<float4>", "float4", "float3", "int2");
	emitSamplingHelpers("Texture3D<float4>", "float4", "float3", "int3");
	emitSamplingHelpers("TextureCubeArray<float4>", "float4", "float4", nullptr);

	// integer formats are never filtered, so sampling them selects the nearest texel of the nearest mip
	auto emitIntegerSamplingHelpers = [&](const char* textureType, const char* coordType, const char* offsetType)
	{
		const char* noOffset = offsetType ? ", 0" : "";
		if (isPixelShader)
		{
			src->addFmt("uint4 latteTextureSample({} tex, SamplerState samplerState, {} coord) {{ return latteIntegerTextureLoad(tex, samplerState, coord, latteIntegerTextureLod(tex, ddx(coord), ddy(coord)){}); }}" HLSL_NL,
				textureType, coordType, noOffset);
			src->addFmt("uint4 latteTextureSample({} tex, SamplerState samplerState, {} coord, float bias) {{ return latteIntegerTextureLoad(tex, samplerState, coord, latteIntegerTextureLod(tex, ddx(coord), ddy(coord)) + bias{}); }}" HLSL_NL,
				textureType, coordType, noOffset);
			src->addFmt("uint4 latteTextureSampleOffset({} tex, SamplerState samplerState, {} coord, float bias) {{ return latteIntegerTextureLoad(tex, samplerState, coord, latteIntegerTextureLod(tex, ddx(coord), ddy(coord)) + bias{}); }}" HLSL_NL,
				textureType, coordType, noOffset);
		}
		else
		{
			src->addFmt("uint4 latteTextureSample({} tex, SamplerState samplerState, {} coord) {{ return latteIntegerTextureLoad(tex, samplerState, coord, 0.0f{}); }}" HLSL_NL,
				textureType, coordType, noOffset);
			src->addFmt("uint4 latteTextureSample({} tex, SamplerState samplerState, {} coord, float bias) {{ return latteIntegerTextureLoad(tex, samplerState, coord, bias{}); }}" HLSL_NL,
				textureType, coordType, noOffset);
			src->addFmt("uint4 latteTextureSampleOffset({} tex, SamplerState samplerState, {} coord, float bias) {{ return latteIntegerTextureLoad(tex, samplerState, coord, bias{}); }}" HLSL_NL,
				textureType, coordType, noOffset);
		}

		src->addFmt("uint4 latteTextureSampleLevel({} tex, SamplerState samplerState, {} coord, float lod) {{ return latteIntegerTextureLoad(tex, samplerState, coord, lod{}); }}" HLSL_NL,
			textureType, coordType, noOffset);
		if (offsetType)
		{
			if (isPixelShader)
				src->addFmt("uint4 latteTextureSampleOffset({} tex, SamplerState samplerState, {} coord, {} offset) {{ return latteIntegerTextureLoad(tex, samplerState, coord, latteIntegerTextureLod(tex, ddx(coord), ddy(coord)), offset); }}" HLSL_NL,
					textureType, coordType, offsetType);
			else
				src->addFmt("uint4 latteTextureSampleOffset({} tex, SamplerState samplerState, {} coord, {} offset) {{ return latteIntegerTextureLoad(tex, samplerState, coord, 0.0f, offset); }}" HLSL_NL,
					textureType, coordType, offsetType);
			src->addFmt("uint4 latteTextureSampleLevelOffset({} tex, SamplerState samplerState, {} coord, float lod, {} offset) {{ return latteIntegerTextureLoad(tex, samplerState, coord, lod, offset); }}" HLSL_NL,
				textureType, coordType, offsetType);
		}
	};

	if (hasIntegerTexture)
	{
		emitIntegerSamplingHelpers("Texture1D<uint4>", "float", "int");
		emitIntegerSamplingHelpers("Texture1DArray<uint4>", "float2", "int");
		emitIntegerSamplingHelpers("Texture2D<uint4>", "float2", "int2");
		emitIntegerSamplingHelpers("Texture2DArray<uint4>", "float3", "int2");
		emitIntegerSamplingHelpers("Texture3D<uint4>", "float3", "int3");
		emitIntegerSamplingHelpers("TextureCubeArray<uint4>", "float4", nullptr);
	}

	src->add("float4 latteTextureGrad(Texture1D<float4> tex, SamplerState samplerState, float coord, float dx, float dy) { return tex.SampleGrad(samplerState, coord, dx, dy); }" HLSL_NL);
	src->add("float4 latteTextureGrad(Texture2D<float4> tex, SamplerState samplerState, float2 coord, float2 dx, float2 dy) { return tex.SampleGrad(samplerState, coord, dx, dy); }" HLSL_NL);
	if (hasIntegerTexture)
	{
		src->add("uint4 latteTextureGrad(Texture1D<uint4> tex, SamplerState samplerState, float coord, float dx, float dy) { return latteIntegerTextureLoad(tex, samplerState, coord, latteIntegerTextureLod(tex, dx, dy), 0); }" HLSL_NL);
		src->add("uint4 latteTextureGrad(Texture2D<uint4> tex, SamplerState samplerState, float2 coord, float2 dx, float2 dy) { return latteIntegerTextureLoad(tex, samplerState, coord, latteIntegerTextureLod(tex, dx, dy), 0); }" HLSL_NL);
	}

	// SampleCmpLevel needs shader model 6.7 with advanced texture ops
	src->add(R"(
#if defined(LATTE_HAS_SAMPLE_CMP_LEVEL)
#define LATTE_SAMPLE_CMP_LEVEL(tex, samplerState, location, reference, lod) tex.SampleCmpLevel(samplerState, location, reference, lod)
#define LATTE_SAMPLE_CMP_LEVEL_OFFSET(tex, samplerState, location, reference, lod, offset) tex.SampleCmpLevel(samplerState, location, reference, lod, offset)
#else
#define LATTE_SAMPLE_CMP_LEVEL(tex, samplerState, location, reference, lod) tex.SampleCmpLevelZero(samplerState, location, reference)
#define LATTE_SAMPLE_CMP_LEVEL_OFFSET(tex, samplerState, location, reference, lod, offset) tex.SampleCmpLevelZero(samplerState, location, reference, offset)
#endif
)" HLSL_NL);

	auto emitComparisonHelpers = [&](const char* textureType, const char* coordParameters, const char* location, const char* reference, const char* offsetType)
	{
		const char* sampleMethod = isPixelShader ? "SampleCmp" : "SampleCmpLevelZero";
		src->addFmt("float latteTextureSample({} tex, SamplerComparisonState samplerState, {}) {{ return tex.{}(samplerState, {}, {}); }}" HLSL_NL,
			textureType, coordParameters, sampleMethod, location, reference);
		src->addFmt("float latteTextureSampleLevel({} tex, SamplerComparisonState samplerState, {}, float lod) {{ return LATTE_SAMPLE_CMP_LEVEL(tex, samplerState, {}, {}, lod); }}" HLSL_NL,
			textureType, coordParameters, location, reference);
		if (offsetType)
		{
			src->addFmt("float latteTextureSampleOffset({} tex, SamplerComparisonState samplerState, {}, {} offset) {{ return tex.{}(samplerState, {}, {}, offset); }}" HLSL_NL,
				textureType, coordParameters, offsetType, sampleMethod, location, reference);
			src->addFmt("float latteTextureSampleLevelOffset({} tex, SamplerComparisonState samplerState, {}, float lod, {} offset) {{ return LATTE_SAMPLE_CMP_LEVEL_OFFSET(tex, samplerState, {}, {}, lod, offset); }}" HLSL_NL,
				textureType, coordParameters, offsetType, location, reference);
		}
	};

	emitComparisonHelpers("Texture1D<float>", "float3 coord", "coord.x", "coord.z", "int");
	emitComparisonHelpers("Texture1DArray<float>", "float3 coord", "coord.xy", "coord.z", "int");
	emitComparisonHelpers("Texture2D<float>", "float3 coord", "coord.xy", "coord.z", "int2");
	emitComparisonHelpers("Texture2DArray<float>", "float4 coord", "coord.xyz", "coord.w", "int2");
	// cube array coordinates already use all four components, so the reference is a separate argument
	emitComparisonHelpers("TextureCubeArray<float>", "float4 coord, float reference", "coord", "reference", nullptr);
	src->addFmt("float latteTextureSampleOffset(TextureCubeArray<float> tex, SamplerComparisonState samplerState, float4 coord, float reference) {{ return tex.{}(samplerState, coord, reference); }}" HLSL_NL,
		isPixelShader ? "SampleCmp" : "SampleCmpLevelZero");
	src->add("float latteTextureSampleLevelOffset(TextureCubeArray<float> tex, SamplerComparisonState samplerState, float4 coord, float reference, float lod) { return LATTE_SAMPLE_CMP_LEVEL(tex, samplerState, coord, reference, lod); }" HLSL_NL);

	if (hasTextureLodQuery)
		EmitTextureQueryLodHelpers(src, hasIntegerTexture, true);

	src->add(R"(
#define LATTE_CONCAT_INNER(a, b) a##b
#define LATTE_CONCAT(a, b) LATTE_CONCAT_INNER(a, b)
#define LATTE_TEXTURE_SAMPLER(tex) LATTE_CONCAT(tex, Sampler)
#define texture(tex, ...) latteTextureSample(tex, LATTE_TEXTURE_SAMPLER(tex), __VA_ARGS__)
#define textureOffset(tex, ...) latteTextureSampleOffset(tex, LATTE_TEXTURE_SAMPLER(tex), __VA_ARGS__)
#define textureGrad(tex, coord, dx, dy) latteTextureGrad(tex, LATTE_TEXTURE_SAMPLER(tex), coord, dx, dy)
#define textureLod(tex, ...) latteTextureSampleLevel(tex, LATTE_TEXTURE_SAMPLER(tex), __VA_ARGS__)
#define textureLodOffset(tex, ...) latteTextureSampleLevelOffset(tex, LATTE_TEXTURE_SAMPLER(tex), __VA_ARGS__)
#define texelFetch(tex, coord, lod) latteTexelFetch(tex, coord, lod)
#define textureGather(tex, coord) latteTextureGather(tex, LATTE_TEXTURE_SAMPLER(tex), coord)
)" HLSL_NL);
	if (hasTextureLodQuery)
		src->add("#define textureQueryLod(tex, coord) latteTextureQueryLod(tex, LATTE_TEXTURE_SAMPLER(tex), coord)" HLSL_NL);
	else
		src->add("#define textureQueryLod(tex, coord) float2(0.0f, 0.0f)" HLSL_NL);
	src->add(HLSL_NL);
#else
	if (shaderContext->shader->shaderType == LatteConst::ShaderType::Pixel)
	{
		src->add(R"(
#define texture(tex, coord) tex.Sample(LATTE_TEXTURE_SAMPLER(tex), coord)
#define textureOffset(tex, coord, offset) tex.Sample(LATTE_TEXTURE_SAMPLER(tex), coord, offset)
#define textureGrad(tex, coord, dx, dy) tex.SampleGrad(LATTE_TEXTURE_SAMPLER(tex), coord, dx, dy)
)" HLSL_NL);
	}
	else
	{
		src->add(R"(
#define texture(tex, coord) tex.SampleLevel(LATTE_TEXTURE_SAMPLER(tex), coord, 0.0f)
#define textureOffset(tex, coord, offset) tex.SampleLevel(LATTE_TEXTURE_SAMPLER(tex), coord, 0.0f, offset)
#define textureGrad(tex, coord, dx, dy) tex.SampleGrad(LATTE_TEXTURE_SAMPLER(tex), coord, dx, dy)
)" HLSL_NL);
	}

	src->add(R"(
#define LATTE_CONCAT_INNER(a, b) a##b
#define LATTE_CONCAT(a, b) LATTE_CONCAT_INNER(a, b)
#define LATTE_TEXTURE_SAMPLER(tex) LATTE_CONCAT(tex, Sampler)
#define textureLod(tex, coord, lod) tex.SampleLevel(LATTE_TEXTURE_SAMPLER(tex), coord, lod)
#define textureLodOffset(tex, coord, lod, offset) tex.SampleLevel(LATTE_TEXTURE_SAMPLER(tex), coord, lod, offset)
#define texelFetch(tex, coord, lod) latteTexelFetch(tex, coord, lod)
#define textureGather(tex, coord) latteTextureGather(tex, LATTE_TEXTURE_SAMPLER(tex), coord)
)" HLSL_NL);
	if (hasTextureLodQuery)
	{
		EmitTextureQueryLodHelpers(src, hasIntegerTexture, false);
		src->add("#define textureQueryLod(tex, coord) latteTextureQueryLod(tex, LATTE_TEXTURE_SAMPLER(tex), coord)" HLSL_NL);
	}
	else
		src->add("#define textureQueryLod(tex, coord) float2(0.0f, 0.0f)" HLSL_NL);
	src->add(HLSL_NL);
#endif
}

void EmitUniformVariables(LatteDecompilerShaderContext* shaderContext, StringBuf* src)
{
	auto& offsets = shaderContext->output->uniformOffsetsD3D12;
	offsets = {};
	const auto& mapping = shaderContext->output->resourceMappingD3D12;
	const auto* shader = shaderContext->shader;
	const char* suffix = GetStageSuffix(shader->shaderType);

	if (mapping.uniformVarsBufferBindingPoint < 0)
		return;

	uint32 uniformOffset = 0;
	src->addFmt("cbuffer LatteUniformVars : register(b{}, space{})" HLSL_NL, (sint32)mapping.uniformVarsBufferBindingPoint, (sint32)mapping.setIndex);
	src->add("{" HLSL_NL);

	if (shader->uniformMode == LATTE_DECOMPILER_UNIFORM_MODE_REMAPPED)
	{
		const uint32 count = std::max<uint32>(1, (uint32)shader->list_remappedUniformEntries.size());
		offsets.offset_remapped = AllocateUniformRegisters(uniformOffset, count);
		src->addFmt("\tint4 uf_remapped{}[{}] : packoffset(c{});" HLSL_NL, suffix, count, offsets.offset_remapped / HLSL_UNIFORM_REGISTER_BYTES);
	}
	else if (shader->uniformMode == LATTE_DECOMPILER_UNIFORM_MODE_FULL_CFILE)
	{
		const uint32 count = std::max<uint32>(1, shaderContext->analyzer.uniformRegisterAccessTracker.DetermineSize(shaderContext->shaderBaseHash, 256));
		offsets.offset_uniformRegister = AllocateUniformRegisters(uniformOffset, count);
		offsets.count_uniformRegister = count;
		src->addFmt("\tint4 uf_uniformRegister{}[{}] : packoffset(c{});" HLSL_NL, suffix, count, offsets.offset_uniformRegister / HLSL_UNIFORM_REGISTER_BYTES);
	}

	const bool hasAnyViewportScaleDisabled =
		!shaderContext->contextRegistersNew->PA_CL_VTE_CNTL.get_VPORT_X_SCALE_ENA() ||
		!shaderContext->contextRegistersNew->PA_CL_VTE_CNTL.get_VPORT_Y_SCALE_ENA() ||
		!shaderContext->contextRegistersNew->PA_CL_VTE_CNTL.get_VPORT_Z_SCALE_ENA();
	if (shaderContext->shaderType == LatteConst::ShaderType::Vertex && hasAnyViewportScaleDisabled)
	{
		offsets.offset_windowSpaceToClipSpaceTransform = AllocateUniformRegisters(uniformOffset, 1);
		src->addFmt("\tfloat2 uf_windowSpaceToClipSpaceTransform : packoffset(c{});" HLSL_NL, offsets.offset_windowSpaceToClipSpaceTransform / HLSL_UNIFORM_REGISTER_BYTES);
	}

	const bool alphaTestEnable = shaderContext->contextRegistersNew->SX_ALPHA_TEST_CONTROL.get_ALPHA_TEST_ENABLE();
	if (shaderContext->shaderType == LatteConst::ShaderType::Pixel && alphaTestEnable)
	{
		offsets.offset_alphaTestRef = AllocateUniformRegisters(uniformOffset, 1);
		src->addFmt("\tfloat uf_alphaTestRef : packoffset(c{});" HLSL_NL, offsets.offset_alphaTestRef / HLSL_UNIFORM_REGISTER_BYTES);
	}

	if (shaderContext->analyzer.outputPointSize && shaderContext->analyzer.writesPointSize == false)
	{
		if ((shaderContext->shaderType == LatteConst::ShaderType::Vertex && !shaderContext->options->usesGeometryShader) ||
			shaderContext->shaderType == LatteConst::ShaderType::Geometry)
		{
			offsets.offset_pointSize = AllocateUniformRegisters(uniformOffset, 1);
			src->addFmt("\tfloat uf_pointSize : packoffset(c{});" HLSL_NL, offsets.offset_pointSize / HLSL_UNIFORM_REGISTER_BYTES);
		}
	}

	if (shaderContext->shaderType == LatteConst::ShaderType::Pixel)
	{
		offsets.offset_fragCoordScale = AllocateUniformRegisters(uniformOffset, 1);
		src->addFmt("\tfloat4 uf_fragCoordScale : packoffset(c{});" HLSL_NL, offsets.offset_fragCoordScale / HLSL_UNIFORM_REGISTER_BYTES);
	}

	for (sint32 t = 0; t < LATTE_NUM_MAX_TEX_UNITS; ++t)
	{
		if (!shaderContext->analyzer.texUnitUsesTexelCoordinates.test(t))
			continue;
		offsets.offset_texScale[t] = AllocateUniformRegisters(uniformOffset, 1);
		src->addFmt("\tfloat2 uf_tex{}Scale : packoffset(c{});" HLSL_NL, t, offsets.offset_texScale[t] / HLSL_UNIFORM_REGISTER_BYTES);
	}

	if (shaderContext->analyzer.useSSBOForStreamout &&
		((shader->shaderType == LatteConst::ShaderType::Vertex && !shaderContext->options->usesGeometryShader) ||
		 shader->shaderType == LatteConst::ShaderType::Geometry))
	{
		offsets.offset_verticesPerInstance = AllocateUniformRegisters(uniformOffset, 1);
		src->addFmt("\tint uf_verticesPerInstance : packoffset(c{});" HLSL_NL, offsets.offset_verticesPerInstance / HLSL_UNIFORM_REGISTER_BYTES);
		for (uint32 i = 0; i < LATTE_NUM_STREAMOUT_BUFFER; ++i)
		{
			if (!shaderContext->output->streamoutBufferWriteMask[i])
				continue;
			offsets.offset_streamoutBufferBase[i] = AllocateUniformRegisters(uniformOffset, 1);
			src->addFmt("\tint uf_streamoutBufferBase{} : packoffset(c{});" HLSL_NL, i, offsets.offset_streamoutBufferBase[i] / HLSL_UNIFORM_REGISTER_BYTES);
		}
	}

	offsets.offset_endOfBlock = uniformOffset;
	src->add("};" HLSL_NL);
}

const char* GetTextureObjectType(const LatteDecompilerShader* shader, sint32 textureUnit)
{
#if defined(CEMU_UWP)
	if (shader->textureUsesDepthCompare[textureUnit])
	{
		switch (shader->textureUnitDim[textureUnit])
		{
		case Latte::E_DIM::DIM_1D:
			return "Texture1D<float>";
		case Latte::E_DIM::DIM_1D_ARRAY:
			return "Texture1DArray<float>";
		case Latte::E_DIM::DIM_2D:
		case Latte::E_DIM::DIM_2D_MSAA:
		case Latte::E_DIM::DIM_2D_ARRAY_MSAA:
			return "Texture2D<float>";
		case Latte::E_DIM::DIM_2D_ARRAY:
			return "Texture2DArray<float>";
		case Latte::E_DIM::DIM_CUBEMAP:
			return "TextureCubeArray<float>";
		default:
			break;
		}
	}
#endif
	switch (shader->textureUnitDim[textureUnit])
	{
	case Latte::E_DIM::DIM_1D:
		return shader->textureIsIntegerFormat[textureUnit] ? "Texture1D<uint4>" : "Texture1D<float4>";
	case Latte::E_DIM::DIM_1D_ARRAY:
		return shader->textureIsIntegerFormat[textureUnit] ? "Texture1DArray<uint4>" : "Texture1DArray<float4>";
	case Latte::E_DIM::DIM_2D:
	case Latte::E_DIM::DIM_2D_MSAA:
	default:
		return shader->textureIsIntegerFormat[textureUnit] ? "Texture2D<uint4>" : "Texture2D<float4>";
	case Latte::E_DIM::DIM_2D_ARRAY:
		return shader->textureIsIntegerFormat[textureUnit] ? "Texture2DArray<uint4>" : "Texture2DArray<float4>";
	case Latte::E_DIM::DIM_3D:
		return shader->textureIsIntegerFormat[textureUnit] ? "Texture3D<uint4>" : "Texture3D<float4>";
	case Latte::E_DIM::DIM_CUBEMAP:
		return shader->textureIsIntegerFormat[textureUnit] ? "TextureCubeArray<uint4>" : "TextureCubeArray<float4>";
	}
}

uint32 GetFetchFormatComponentCount(uint8 format)
{
	switch (format)
	{
	case FMT_32_32_32_32:
	case FMT_32_32_32_32_FLOAT:
	case FMT_16_16_16_16:
	case FMT_16_16_16_16_FLOAT:
	case FMT_8_8_8_8:
	case FMT_2_10_10_10:
		return 4;
	case FMT_32_32_32:
	case FMT_32_32_32_FLOAT:
	case FMT_16_16_16:
	case FMT_16_16_16_FLOAT:
	case FMT_8_8_8:
		return 3;
	case FMT_32_32:
	case FMT_32_32_FLOAT:
	case FMT_16_16:
	case FMT_16_16_FLOAT:
	case FMT_8_8:
		return 2;
	case FMT_32:
	case FMT_32_FLOAT:
	case FMT_16:
	case FMT_16_FLOAT:
	case FMT_8:
		return 1;
	default:
		return 0;
	}
}

uint32 GetFetchFormatComponentBits(uint8 format)
{
	switch (format)
	{
	case FMT_32:
	case FMT_32_FLOAT:
	case FMT_32_32:
	case FMT_32_32_FLOAT:
	case FMT_32_32_32:
	case FMT_32_32_32_FLOAT:
	case FMT_32_32_32_32:
	case FMT_32_32_32_32_FLOAT:
		return 32;
	case FMT_16:
	case FMT_16_FLOAT:
	case FMT_16_16:
	case FMT_16_16_FLOAT:
	case FMT_16_16_16:
	case FMT_16_16_16_FLOAT:
	case FMT_16_16_16_16:
	case FMT_16_16_16_16_FLOAT:
		return 16;
	case FMT_8:
	case FMT_8_8:
	case FMT_8_8_8:
	case FMT_8_8_8_8:
		return 8;
	case FMT_2_10_10_10:
		return 0;
	default:
		return 0;
	}
}

bool IsFetchFormatFloat(uint8 format)
{
	switch (format)
	{
	case FMT_32_FLOAT:
	case FMT_32_32_FLOAT:
	case FMT_32_32_32_FLOAT:
	case FMT_32_32_32_32_FLOAT:
	case FMT_16_FLOAT:
	case FMT_16_16_FLOAT:
	case FMT_16_16_16_FLOAT:
	case FMT_16_16_16_16_FLOAT:
		return true;
	default:
		return false;
	}
}

std::string GetFetchByteOffsetExpression(const LatteParsedFetchShaderAttribute_t& attrib, const std::string& strideExpression)
{
	switch (attrib.fetchType)
	{
	case LatteConst::VertexFetchType2::VERTEX_DATA:
		return fmt::format("(gl_VertexID * {} + {}u)", strideExpression, attrib.offset);
	case LatteConst::VertexFetchType2::INSTANCE_DATA:
		if (attrib.aluDivisor > 1)
			return fmt::format("((gl_InstanceID / {}u) * {} + {}u)", (uint32)attrib.aluDivisor, strideExpression, attrib.offset);
		return fmt::format("(gl_InstanceID * {} + {}u)", strideExpression, attrib.offset);
	case LatteConst::VertexFetchType2::NO_INDEX_OFFSET_DATA:
	default:
		return fmt::format("{}u", attrib.offset);
	}
}

std::string GetFetchStrideExpression(const LatteParsedFetchShaderAttribute_t& attrib, uint32 staticStride, bool dynamicStride)
{
	if (dynamicStride && attrib.attributeBufferIndex < LATTE_MAX_VERTEX_BUFFERS)
		return fmt::format("uf_d3d12VertexStrides[{}].{}", attrib.attributeBufferIndex / 4, GetChannelName(attrib.attributeBufferIndex % 4));
	return fmt::format("{}u", staticStride);
}

const char* GetFetchLoad16Macro(LatteConst::VertexFetchEndianMode endianMode)
{
	if (endianMode == LatteConst::VertexFetchEndianMode::SWAP_U16 || endianMode == LatteConst::VertexFetchEndianMode::SWAP_U32)
		return "LATTE_LOAD_U16_BE";
	return "LATTE_LOAD_U16_LE";
}

const char* GetFetchLoad32Macro(LatteConst::VertexFetchEndianMode endianMode)
{
	return endianMode == LatteConst::VertexFetchEndianMode::SWAP_U32 ? "LATTE_LOAD_U32_BE" : "LATTE_LOAD_U32_LE";
}

void EmitHLSLFetchLoad32(StringBuf* src, const char* destination, LatteConst::VertexFetchEndianMode endianMode, const std::string& bufferName, uint32 byteOffset)
{
	src->addFmt("\t\t{} = {}({}, latteAttrByteOffset + {}u);" HLSL_NL, destination, GetFetchLoad32Macro(endianMode), bufferName, byteOffset);
	if (endianMode == LatteConst::VertexFetchEndianMode::SWAP_U16)
		src->addFmt("\t\t{} = (({} & 0x00ff00ffu) << 8u) | (({} >> 8u) & 0x00ff00ffu);" HLSL_NL, destination, destination, destination);
}

uint32 GetFetchComponent16ByteOffset(LatteConst::VertexFetchEndianMode endianMode, uint32 componentIndex)
{
	// an 8in32 swap reverses the whole dword, which also exchanges its two 16-bit halves
	if (endianMode == LatteConst::VertexFetchEndianMode::SWAP_U32)
		return (componentIndex / 2) * 4 + ((componentIndex & 1) != 0 ? 0 : 2);
	return componentIndex * 2;
}

uint32 GetFetchComponent8ByteOffset(LatteConst::VertexFetchEndianMode endianMode, uint32 componentIndex)
{
	if (endianMode == LatteConst::VertexFetchEndianMode::SWAP_U32)
		return (componentIndex & ~3u) | (3 - (componentIndex & 3));
	if (endianMode == LatteConst::VertexFetchEndianMode::SWAP_U16)
		return componentIndex ^ 1;
	return componentIndex;
}

void EmitHLSLFetchRawLoad(StringBuf* src, const LatteParsedFetchShaderAttribute_t& attrib, const std::string& bufferName)
{
	const uint32 componentCount = GetFetchFormatComponentCount(attrib.format);
	const uint32 componentBits = GetFetchFormatComponentBits(attrib.format);
	if (componentCount == 0)
	{
		src->addFmt("\t\t// D3D12 TODO: unsupported vertex fetch format 0x{:02x}" HLSL_NL, attrib.format);
		src->add("\t\tuint4 attrDecoder = uint4(0u, 0u, 0u, 0u);" HLSL_NL);
		return;
	}

	if (attrib.format == FMT_2_10_10_10)
	{
		src->add("\t\tuint lattePacked;" HLSL_NL);
		EmitHLSLFetchLoad32(src, "lattePacked", attrib.endianSwap, bufferName, 0);
		src->add("\t\tuint4 attrDecoder = uint4((lattePacked >> 0u) & 0x3ffu, (lattePacked >> 10u) & 0x3ffu, (lattePacked >> 20u) & 0x3ffu, (lattePacked >> 30u) & 0x3u);" HLSL_NL);
		if (attrib.nfa != 0)
		{
			if (attrib.isSigned)
			{
				src->add("\t\tif ((attrDecoder.x & 0x200u) != 0u) attrDecoder.x |= 0xfffffc00u;" HLSL_NL);
				src->add("\t\tif ((attrDecoder.y & 0x200u) != 0u) attrDecoder.y |= 0xfffffc00u;" HLSL_NL);
				src->add("\t\tif ((attrDecoder.z & 0x200u) != 0u) attrDecoder.z |= 0xfffffc00u;" HLSL_NL);
				src->add("\t\tif ((attrDecoder.w & 0x2u) != 0u) attrDecoder.w |= 0xfffffffcu;" HLSL_NL);
			}
			if (attrib.nfa == 2)
			{
				if (attrib.isSigned)
					src->add("\t\tattrDecoder = asuint(float4(int4(attrDecoder)));" HLSL_NL);
				else
					src->add("\t\tattrDecoder = asuint(float4(attrDecoder));" HLSL_NL);
			}
		}
		else
		{
			if (attrib.isSigned)
			{
				src->add("\t\tif ((attrDecoder.x & 0x200u) != 0u) attrDecoder.x |= 0xfffffc00u;" HLSL_NL);
				src->add("\t\tif ((attrDecoder.y & 0x200u) != 0u) attrDecoder.y |= 0xfffffc00u;" HLSL_NL);
				src->add("\t\tif ((attrDecoder.z & 0x200u) != 0u) attrDecoder.z |= 0xfffffc00u;" HLSL_NL);
				src->add("\t\tattrDecoder.x = asuint(max(float(int(attrDecoder.x)) / 511.0f, -1.0f));" HLSL_NL);
				src->add("\t\tattrDecoder.y = asuint(max(float(int(attrDecoder.y)) / 511.0f, -1.0f));" HLSL_NL);
				src->add("\t\tattrDecoder.z = asuint(max(float(int(attrDecoder.z)) / 511.0f, -1.0f));" HLSL_NL);
			}
			else
			{
				src->add("\t\tattrDecoder.x = asuint(float(attrDecoder.x) / 1023.0f);" HLSL_NL);
				src->add("\t\tattrDecoder.y = asuint(float(attrDecoder.y) / 1023.0f);" HLSL_NL);
				src->add("\t\tattrDecoder.z = asuint(float(attrDecoder.z) / 1023.0f);" HLSL_NL);
			}
			src->add("\t\tattrDecoder.w = asuint(float(attrDecoder.w));" HLSL_NL);
		}
		return;
	}

	src->add("\t\tuint4 attrDecoder = uint4(0u, 0u, 0u, 0u);" HLSL_NL);
	for (uint32 i = 0; i < componentCount; ++i)
	{
		const char* channel = GetChannelName(i);
		if (componentBits == 32)
			EmitHLSLFetchLoad32(src, fmt::format("attrDecoder.{}", channel).c_str(), attrib.endianSwap, bufferName, i * 4);
		else if (componentBits == 16)
			src->addFmt("\t\tattrDecoder.{} = {}({}, latteAttrByteOffset + {}u);" HLSL_NL, channel, GetFetchLoad16Macro(attrib.endianSwap), bufferName, GetFetchComponent16ByteOffset(attrib.endianSwap, i));
		else if (componentBits == 8)
			src->addFmt("\t\tattrDecoder.{} = LATTE_LOAD_U8({}, latteAttrByteOffset + {}u);" HLSL_NL, channel, bufferName, GetFetchComponent8ByteOffset(attrib.endianSwap, i));
	}

	if (IsFetchFormatFloat(attrib.format))
	{
		if (componentBits == 16)
		{
			for (uint32 i = 0; i < componentCount; ++i)
				src->addFmt("\t\tattrDecoder.{} = asuint(f16tof32(attrDecoder.{}));" HLSL_NL, GetChannelName(i), GetChannelName(i));
		}
		return;
	}

	if (attrib.nfa == 0)
	{
		const double denom = componentBits == 16 ? (attrib.isSigned ? 32767.0 : 65535.0) :
							 componentBits == 8  ? (attrib.isSigned ? 127.0 : 255.0) :
												   (attrib.isSigned ? 2147483647.0 : 4294967295.0);
		for (uint32 i = 0; i < componentCount; ++i)
		{
			const char* channel = GetChannelName(i);
			if (attrib.isSigned)
			{
				if (componentBits != 32)
				{
					const uint32 signBit = componentBits == 16 ? 0x8000 : 0x80;
					const uint32 signExtend = componentBits == 16 ? 0xffff0000 : 0xffffff00;
					src->addFmt("\t\tif ((attrDecoder.{} & 0x{:x}u) != 0u) attrDecoder.{} |= 0x{:x}u;" HLSL_NL, channel, signBit, channel, signExtend);
				}
				src->addFmt("\t\tattrDecoder.{} = asuint(max(float(int(attrDecoder.{})) / {:.1f}f, -1.0f));" HLSL_NL, channel, channel, denom);
			}
			else
				src->addFmt("\t\tattrDecoder.{} = asuint(float(attrDecoder.{}) / {:.1f}f);" HLSL_NL, channel, channel, denom);
		}
	}
	else if (attrib.nfa == 2)
	{
		for (uint32 i = 0; i < componentCount; ++i)
		{
			const char* channel = GetChannelName(i);
			if (attrib.isSigned)
			{
				const uint32 signBit = componentBits == 16 ? 0x8000 : componentBits == 8 ? 0x80 : 0x80000000;
				const uint32 signExtend = componentBits == 16 ? 0xffff0000 : componentBits == 8 ? 0xffffff00 : 0;
				if (componentBits != 32)
					src->addFmt("\t\tif ((attrDecoder.{} & 0x{:x}u) != 0u) attrDecoder.{} |= 0x{:x}u;" HLSL_NL, channel, signBit, channel, signExtend);
				src->addFmt("\t\tattrDecoder.{} = asuint(float(int(attrDecoder.{})));" HLSL_NL, channel, channel);
			}
			else
				src->addFmt("\t\tattrDecoder.{} = asuint(float(attrDecoder.{}));" HLSL_NL, channel, channel);
		}
	}
	else if (attrib.nfa == 1 && attrib.isSigned)
	{
		for (uint32 i = 0; i < componentCount; ++i)
		{
			const char* channel = GetChannelName(i);
			if (componentBits == 16)
				src->addFmt("\t\tif ((attrDecoder.{} & 0x8000u) != 0u) attrDecoder.{} |= 0xffff0000u;" HLSL_NL, channel, channel);
			else if (componentBits == 8)
				src->addFmt("\t\tif ((attrDecoder.{} & 0x80u) != 0u) attrDecoder.{} |= 0xffffff00u;" HLSL_NL, channel, channel);
		}
	}
}

std::string GetHLSLAttributeComponentExpression(const LatteParsedFetchShaderAttribute_t& attrib, uint8 ds, bool outputFloat)
{
	if (ds >= 6)
		ds = 4;
	if (outputFloat)
	{
		if (ds == 4)
			return "0.0f";
		if (ds == 5)
			return attrib.nfa == 1 ? "asfloat(1u)" : "1.0f";
		return fmt::format("asfloat(attrDecoder.{})", GetChannelName(ds));
	}
	if (ds == 4)
		return "0";
	if (ds == 5)
		return attrib.nfa == 1 ? "1" : "0x3f800000";
	return fmt::format("(int)attrDecoder.{}", GetChannelName(ds));
}

void EmitHLSLAttributeImport(LatteDecompilerShaderContext* shaderContext, LatteParsedFetchShaderAttribute_t& attrib, uint32 stride, bool dynamicStride)
{
	StringBuf* src = shaderContext->shaderSource;

	uint32 attributeShaderLoc = 0xffffffff;
	for (sint32 f = 0; f < 32; f++)
	{
		if (shaderContext->contextRegisters[mmSQ_VTX_SEMANTIC_0 + f] == attrib.semanticId)
		{
			attributeShaderLoc = f;
			break;
		}
	}
	if (attributeShaderLoc == 0xffffffff)
		return;
	const uint32 registerIndex = attributeShaderLoc + 1;
	if ((shaderContext->analyzer.gprUseMask[registerIndex / 8] & (1 << (registerIndex % 8))) == 0)
	{
		src->addFmt("\t// skipped unused attribute semantic {} for r{}" HLSL_NL, attrib.semanticId, registerIndex);
		return;
	}

	const std::string strideExpression = GetFetchStrideExpression(attrib, stride, dynamicStride);
	src->addFmt("\t// fetch semantic {} buffer {} format=0x{:02x} nfa={} signed={} endian={} offset={} stride={} strideExpr={} -> r{}" HLSL_NL,
				attrib.semanticId,
				attrib.attributeBufferIndex,
				attrib.format,
				attrib.nfa,
				attrib.isSigned,
				(uint32)attrib.endianSwap,
				attrib.offset,
				stride,
				strideExpression,
				registerIndex);
	src->add("\t{" HLSL_NL);
	if (attrib.attributeBufferIndex >= LATTE_MAX_VERTEX_BUFFERS)
	{
		src->add("\t\tuint4 attrDecoder = uint4(0u, 0u, 0u, 0u);" HLSL_NL);
	}
	else
	{
		const std::string bufferName = fmt::format("latteVertexBuffer{}", attrib.attributeBufferIndex);
#if defined(CEMU_UWP)
		src->addFmt("\t\tuint latteAttrByteOffset = {} + ((uf_d3d12VertexBaseOffsetLowBits >> {}u) & 3u);" HLSL_NL,
			GetFetchByteOffsetExpression(attrib, strideExpression), attrib.attributeBufferIndex * 2);
#else
		src->addFmt("\t\tuint latteAttrByteOffset = {};" HLSL_NL, GetFetchByteOffsetExpression(attrib, strideExpression));
#endif
		EmitHLSLFetchRawLoad(src, attrib, bufferName);
	}

	const bool outputFloat = shaderContext->typeTracker.defaultDataType == LATTE_DECOMPILER_DTYPE_FLOAT;
	src->addFmt("\t\t{} = {}(",
				GetRegisterVarName(shaderContext, registerIndex),
				outputFloat ? "float4" : "int4");
	for (sint32 i = 0; i < 4; ++i)
	{
		if (i != 0)
			src->add(", ");
		src->add(GetHLSLAttributeComponentExpression(attrib, attrib.ds[i], outputFloat));
	}
	src->add(");" HLSL_NL);
	src->add("\t}" HLSL_NL);
}

void EmitVertexAttributeImports(LatteDecompilerShaderContext* shaderContext, StringBuf* src)
{
	if (!shaderContext->fetchShader)
		return;
	for (auto& group : shaderContext->fetchShader->bufferGroups)
	{
		const uint32 stride = group.getCurrentBufferStride(shaderContext->contextRegisters);
		for (sint32 i = 0; i < group.attribCount; ++i)
			EmitHLSLAttributeImport(shaderContext, group.attrib[i], stride, true);
	}
	for (auto& group : shaderContext->fetchShader->bufferGroupsInvalid)
	{
		for (sint32 i = 0; i < group.attribCount; ++i)
			EmitHLSLAttributeImport(shaderContext, group.attrib[i], 0, false);
	}
}

void EmitResourceDeclarations(LatteDecompilerShaderContext* shaderContext, StringBuf* src)
{
	const auto& mapping = shaderContext->output->resourceMappingD3D12;
	const sint32 space = mapping.setIndex;
	src->addFmt("#define LATTE_D3D12_REGISTER_SPACE {}" HLSL_NL, space);
	EmitUniformVariables(shaderContext, src);
	src->add("cbuffer LatteD3D12DrawConstants : register(b1, space1)" HLSL_NL);
	src->add("{" HLSL_NL);
	src->add("\tuint uf_d3d12LogicOpMode;" HLSL_NL);
	src->add("\tuint uf_d3d12BaseVertex;" HLSL_NL);
	src->add("\tuint uf_d3d12BaseInstance;" HLSL_NL);
	src->add("\tuint uf_d3d12Reserved0;" HLSL_NL);
	src->add("\tfloat2 uf_d3d12InvViewportSize;" HLSL_NL);
	src->add("\tuint2 uf_d3d12DrawConstantsPadding;" HLSL_NL);
	src->add("\tuint4 uf_d3d12VertexStrides[4];" HLSL_NL);
#if defined(CEMU_UWP)
	src->add("\tuint uf_d3d12VertexBaseOffsetLowBits;" HLSL_NL);
#endif
	src->add("};" HLSL_NL);

	const char* texturePrefix = shaderContext->shaderType == LatteConst::ShaderType::Vertex ? "textureUnitVS" :
								shaderContext->shaderType == LatteConst::ShaderType::Pixel ? "textureUnitPS" :
																								 "textureUnitGS";
	for (sint32 i = 0; i < LATTE_NUM_MAX_TEX_UNITS; ++i)
	{
		if (!shaderContext->output->textureUnitMask[i])
			continue;
		if (mapping.textureUnitToBindingPoint[i] < 0)
			continue;
		src->addFmt("{} {}{} : register(t{}, space{});" HLSL_NL,
					GetTextureObjectType(shaderContext->shader, i),
					texturePrefix,
					i,
					(sint32)mapping.textureUnitToBindingPoint[i],
					space);
		src->addFmt("{} {}{}Sampler : register(s{}, space{});" HLSL_NL,
					LatteDecompiler_HLSLTextureUsesComparisonSampler(shaderContext->shader, i) ? "SamplerComparisonState" : "SamplerState",
					texturePrefix,
					i,
					(sint32)mapping.textureUnitToBindingPoint[i],
					space);
	}

	const char* uniformBlockPrefix = shaderContext->shaderType == LatteConst::ShaderType::Vertex ? "uf_blockVS" :
									 shaderContext->shaderType == LatteConst::ShaderType::Pixel ? "uf_blockPS" :
																									  "uf_blockGS";
	if (shaderContext->shader->uniformMode == LATTE_DECOMPILER_UNIFORM_MODE_FULL_CBANK)
	{
		for (sint32 i = 0; i < LATTE_NUM_MAX_UNIFORM_BUFFERS; ++i)
		{
			if (mapping.uniformBuffersBindingPoint[i] >= 0)
				src->addFmt("StructuredBuffer<float4> {}{} : register(t{}, space{});" HLSL_NL, uniformBlockPrefix, i, (sint32)mapping.uniformBuffersBindingPoint[i], space);
		}
	}

	if (mapping.tfStorageBindingPoint >= 0)
		src->addFmt("RWStructuredBuffer<int> sb_buffer : register(u{}, space{});" HLSL_NL, (sint32)mapping.tfStorageBindingPoint, space);

	if (shaderContext->shaderType == LatteConst::ShaderType::Pixel)
	{
#if defined(CEMU_UWP)
		for (uint32 i = 0; i < 8; ++i)
			src->addFmt("Texture2D<float4> latteD3D12LogicOpDst{} : register(t{}, space1);" HLSL_NL,
				i, D3D12_LOGIC_OP_DST_SRV_BINDING + (sint32)i);
#else
		src->addFmt("Texture2D<float4> latteD3D12LogicOpDst : register(t{}, space1);" HLSL_NL, D3D12_LOGIC_OP_DST_SRV_BINDING);
#endif
	}

	if (shaderContext->shaderType == LatteConst::ShaderType::Vertex && shaderContext->fetchShader)
	{
		for (auto& group : shaderContext->fetchShader->bufferGroups)
		{
			if (group.attributeBufferIndex >= LATTE_MAX_VERTEX_BUFFERS)
				continue;
			src->addFmt("Buffer<uint> latteVertexBuffer{} : register(t{}, space{});" HLSL_NL,
						group.attributeBufferIndex,
						D3D12_VERTEX_BUFFER_SRV_BASE + (sint32)group.attributeBufferIndex,
						space);
		}
	}
}

// GS-to-PS varyings take one signature register per parameter slot so the two stages link by register position
// SV_Position and SV_IsFrontFace take the other two of the 32 pixel input registers
constexpr uint32 kG2PMaxSlots = 30;

const LatteShaderPSInputTable::psImport_t* FindPSInputByG2PSlot(LatteShaderPSInputTable* psInputTable, uint32 slot)
{
	if (!psInputTable)
		return nullptr;
	for (sint32 i = 0; i < psInputTable->count; ++i)
	{
		const uint32 semanticId = psInputTable->import[i].semanticId;
		if (semanticId <= LATTE_ANALYZER_IMPORT_INDEX_PARAM_MAX && (semanticId & 0x7F) == slot)
			return &psInputTable->import[i];
	}
	return nullptr;
}

uint32 GetG2PSlotCountFromPSInputs()
{
	LatteShaderPSInputTable* psInputTable = LatteSHRC_GetPSInputTable();
	uint32 slotCount = 0;
	if (!psInputTable)
		return slotCount;
	for (sint32 i = 0; i < psInputTable->count; ++i)
	{
		const uint32 semanticId = psInputTable->import[i].semanticId;
		const uint32 slot = semanticId & 0x7F;
		if (semanticId <= LATTE_ANALYZER_IMPORT_INDEX_PARAM_MAX && slot < kG2PMaxSlots)
			slotCount = std::max<uint32>(slotCount, slot + 1);
	}
	return slotCount;
}

void EmitVaryingFields(LatteDecompilerShaderContext* shaderContext, StringBuf* src, bool fromVertexShader, const char* prefix)
{
	LatteShaderPSInputTable* psInputTable = LatteSHRC_GetPSInputTable();
	if (!psInputTable)
		return;
	if (fromVertexShader)
	{
		for (sint32 i = 0; i < psInputTable->count; ++i)
		{
			const uint32 semanticId = psInputTable->import[i].semanticId;
			if (semanticId > LATTE_ANALYZER_IMPORT_INDEX_PARAM_MAX)
				continue;
			src->addFmt("\t{}float4 {}{} : TEXCOORD{};" HLSL_NL,
						GetInterpolationModifier(&psInputTable->import[i]),
						prefix,
						semanticId,
						i);
		}
		return;
	}

	if (shaderContext->options->usesGeometryShader)
	{
		const uint32 slotCount = GetG2PSlotCountFromPSInputs();
		for (uint32 slot = 0; slot < slotCount; ++slot)
		{
			src->addFmt("\t{}float4 {}{} : TEXCOORD{};" HLSL_NL,
						GetInterpolationModifier(FindPSInputByG2PSlot(psInputTable, slot)),
						prefix,
						slot,
						slot);
		}
		return;
	}

	for (sint32 i = 0; i < psInputTable->count; ++i)
	{
		const uint32 semanticId = psInputTable->import[i].semanticId;
		if (semanticId > LATTE_ANALYZER_IMPORT_INDEX_PARAM_MAX)
			continue;
		src->addFmt("\t{}float4 {}{} : TEXCOORD{};" HLSL_NL,
					GetInterpolationModifier(&psInputTable->import[i]),
					prefix,
					semanticId,
					i);
	}
}

uint32 GetV2GParameterCount(const LatteDecompilerShaderContext* shaderContext)
{
	if (shaderContext->shader->shaderType == LatteConst::ShaderType::Vertex)
		return shaderContext->shader->ringParameterCount;
	if (shaderContext->shader->shaderType == LatteConst::ShaderType::Geometry)
		return shaderContext->shader->ringParameterCountFromPrevStage;
	return 0;
}

void EmitV2GFields(LatteDecompilerShaderContext* shaderContext, StringBuf* src)
{
	for (uint32 i = 0; i < GetV2GParameterCount(shaderContext); ++i)
		src->addFmt("\tint4 passV2GParameter{} : TEXCOORD{};" HLSL_NL, i, i);
}

void EmitV2GLocalStruct(LatteDecompilerShaderContext* shaderContext, StringBuf* src)
{
	const uint32 count = GetV2GParameterCount(shaderContext);
	if (count == 0)
		return;

	src->add("struct V2GLocal" HLSL_NL "{" HLSL_NL);
	for (uint32 i = 0; i < count; ++i)
		src->addFmt("\tint4 passV2GParameter{};" HLSL_NL, i);
	src->add("};" HLSL_NL);
}

void EmitVertexParameterLocalDeclarations(LatteDecompilerShaderContext* shaderContext, StringBuf* src)
{
	LatteShaderPSInputTable* psInputTable = LatteSHRC_GetPSInputTable();
	if (!psInputTable)
		return;

	bool emitted[256]{};
	const uint32 parameterMask = shaderContext->shader->outputParameterMask;
	for (uint32 i = 0; i < 32; ++i)
	{
		if ((parameterMask & (1u << i)) == 0)
			continue;
		const sint32 semanticId = LatteShaderPSInputTable::getVertexShaderOutParamSemanticId(shaderContext->contextRegisters, i);
		if (semanticId < 0 || semanticId == 0xFF || semanticId > LATTE_ANALYZER_IMPORT_INDEX_PARAM_MAX || semanticId >= (sint32)std::size(emitted))
			continue;
		if (!FindPSInputBySemanticId(psInputTable, (uint32)semanticId))
			continue;
		if (emitted[semanticId])
			continue;
		emitted[semanticId] = true;
		src->addFmt("\tfloat4 passParameterSem{} = float4(0.0f, 0.0f, 0.0f, 0.0f);" HLSL_NL, semanticId);
	}
}

uint32 GetG2PSlotCountFromCopyShader(const LatteDecompilerShaderContext* shaderContext)
{
	uint32 slotCount = 0;
	if (!shaderContext->parsedGSCopyShader)
		return slotCount;
	for (sint32 p = 0; p < shaderContext->parsedGSCopyShader->numParam; ++p)
	{
		const auto& mapping = shaderContext->parsedGSCopyShader->paramMapping[p];
		const uint32 slot = mapping.exportParam & 0x7F;
		if (mapping.exportType == 2 && slot < kG2PMaxSlots)
			slotCount = std::max<uint32>(slotCount, slot + 1);
	}
	return slotCount;
}

uint32 GetG2POutputSlotCount(const LatteDecompilerShaderContext* shaderContext)
{
	return std::max<uint32>(GetG2PSlotCountFromCopyShader(shaderContext), GetG2PSlotCountFromPSInputs());
}

void EmitG2PFields(LatteDecompilerShaderContext* shaderContext, StringBuf* src, const char* prefix)
{
	if (!shaderContext->parsedGSCopyShader)
		return;

	LatteShaderPSInputTable* psInputTable = LatteSHRC_GetPSInputTable();
	const uint32 slotCount = GetG2POutputSlotCount(shaderContext);
	for (uint32 slot = 0; slot < slotCount; ++slot)
	{
		src->addFmt("\t{}float4 {}{} : TEXCOORD{};" HLSL_NL,
					GetInterpolationModifier(FindPSInputByG2PSlot(psInputTable, slot)),
					prefix,
					slot,
					slot);
	}
}

void EmitG2PLocalDeclarations(LatteDecompilerShaderContext* shaderContext, StringBuf* src)
{
	if (!shaderContext->parsedGSCopyShader)
		return;

	bool emitted[128]{};
	for (sint32 p = 0; p < shaderContext->parsedGSCopyShader->numParam; ++p)
	{
		const auto& mapping = shaderContext->parsedGSCopyShader->paramMapping[p];
		if (mapping.exportType != 2)
			continue;
		const uint32 semantic = mapping.exportParam & 0x7F;
		if (semantic >= (sizeof(emitted) / sizeof(emitted[0])) || emitted[semantic])
			continue;
		emitted[semantic] = true;
		src->addFmt("\tfloat4 passG2PParameter{} = float4(0.0f, 0.0f, 0.0f, 0.0f);" HLSL_NL, (sint32)mapping.exportParam);
	}
}

void EmitG2POutputAssignments(LatteDecompilerShaderContext* shaderContext, StringBuf* src)
{
	if (!shaderContext->parsedGSCopyShader)
		return;

	bool emitted[128]{};
	for (sint32 p = 0; p < shaderContext->parsedGSCopyShader->numParam; ++p)
	{
		const auto& mapping = shaderContext->parsedGSCopyShader->paramMapping[p];
		if (mapping.exportType != 2)
			continue;
		const uint32 semantic = mapping.exportParam & 0x7F;
		if (semantic >= kG2PMaxSlots || emitted[semantic])
			continue;
		emitted[semantic] = true;
		src->addFmt(" output.passG2PParameter{} = passG2PParameter{};", semantic, (sint32)mapping.exportParam);
	}
}

const char* GetGeometryInputPrimitiveName(LatteDecompilerShaderContext* shaderContext)
{
	using Primitive = Latte::LATTE_VGT_PRIMITIVE_TYPE::E_PRIMITIVE_TYPE;
	const auto primitiveMode = shaderContext->contextRegistersNew->VGT_PRIMITIVE_TYPE.get_PRIMITIVE_MODE();
	switch (primitiveMode)
	{
	case Primitive::POINTS:
		return "point";
	case Primitive::LINE_STRIP:
	case Primitive::LINE_STRIP_ADJACENT:
		return "lineadj";
	case Primitive::TRIANGLES:
	default:
		return "triangle";
	}
}

uint32 GetGeometryInputVertexCount(LatteDecompilerShaderContext* shaderContext)
{
	using Primitive = Latte::LATTE_VGT_PRIMITIVE_TYPE::E_PRIMITIVE_TYPE;
	const auto primitiveMode = shaderContext->contextRegistersNew->VGT_PRIMITIVE_TYPE.get_PRIMITIVE_MODE();
	switch (primitiveMode)
	{
	case Primitive::POINTS:
		return 1;
	case Primitive::LINE_STRIP:
	case Primitive::LINE_STRIP_ADJACENT:
		return 4;
	case Primitive::TRIANGLES:
	default:
		return 3;
	}
}

uint32 GetGeometryMaxVertexCount(LatteDecompilerShaderContext* shaderContext)
{
	uint32 bytesPerVertex = shaderContext->contextRegisters[mmSQ_GS_VERT_ITEMSIZE] * 4;
	bytesPerVertex = std::max<uint32>(bytesPerVertex, 1);
	uint32 maxVerticesInGS = ((shaderContext->contextRegisters[mmSQ_GSVS_RING_ITEMSIZE] & 0x7FFF) * 4) / bytesPerVertex;
	if (!shaderContext->analyzer.hasLoops && shaderContext->analyzer.numEmitVertex != 0 && shaderContext->analyzer.numEmitVertex < maxVerticesInGS)
		maxVerticesInGS = shaderContext->analyzer.numEmitVertex;
	return std::max<uint32>(maxVerticesInGS, 1);
}

// maxvertexcount times the scalar size of one output vertex may not exceed this
constexpr uint32 kGeometryMaxOutputScalars = 1024;

uint32 GetGeometryOutputVertexScalars(const LatteDecompilerShaderContext* shaderContext)
{
	return 4 * (1 + GetG2POutputSlotCount(shaderContext));
}

// D3D12 has no point size, so rasterized point output is emitted as one quad per point
bool GeometryShaderExpandsPoints(LatteDecompilerShaderContext* shaderContext)
{
	return LatteDecompiler_HLSLGeometryShaderExpandsPoints(shaderContext->contextRegisters);
}

uint32 GetGeometryDeclaredMaxVertexCount(LatteDecompilerShaderContext* shaderContext)
{
	const uint32 vertexCount = GetGeometryMaxVertexCount(shaderContext) * (GeometryShaderExpandsPoints(shaderContext) ? 4 : 1);
	return std::clamp<uint32>(vertexCount, 1, kGeometryMaxOutputScalars / GetGeometryOutputVertexScalars(shaderContext));
}

const char* GetGeometryStreamTypeName(LatteDecompilerShaderContext* shaderContext)
{
	const uint32 gsOutPrimType = shaderContext->contextRegisters[mmVGT_GS_OUT_PRIM_TYPE];
	if (gsOutPrimType == 0)
		return GeometryShaderExpandsPoints(shaderContext) ? "TriangleStream" : "PointStream";
	if (gsOutPrimType == 1)
		return "LineStream";
	return "TriangleStream";
}

void EmitRegisterDeclarations(LatteDecompilerShaderContext* shaderContext, StringBuf* src)
{
	if (!shaderContext->typeTracker.useArrayGPRs)
	{
		for (sint32 i = 0; i < 128; ++i)
		{
			const bool reachedByRelativeRead = shaderContext->analyzer.usesRelativeGPRRead && i < (sint32)shaderContext->indexedGPRLimit;
			if (reachedByRelativeRead || (shaderContext->analyzer.gprUseMask[i / 8] & (1 << (i & 7))) != 0)
			{
				if (shaderContext->typeTracker.genIntReg)
					src->addFmt("ivec4 R{}i = ivec4(0);" HLSL_NL, i);
				else if (shaderContext->typeTracker.genFloatReg)
					src->addFmt("vec4 R{}f = vec4(0.0);" HLSL_NL, i);
			}
		}
	}
	else
	{
		if (shaderContext->typeTracker.genIntReg)
			src->add("ivec4 Ri[128];" HLSL_NL);
		else if (shaderContext->typeTracker.genFloatReg)
			src->add("vec4 Rf[128];" HLSL_NL);
		for (sint32 i = 0; i < 128; ++i)
		{
			if (shaderContext->typeTracker.genIntReg)
				src->addFmt("Ri[{}] = ivec4(0);" HLSL_NL, i);
			else if (shaderContext->typeTracker.genFloatReg)
				src->addFmt("Rf[{}] = vec4(0.0);" HLSL_NL, i);
		}
	}

	if (shaderContext->typeTracker.genIntReg)
	{
		src->add("int backupReg0i, backupReg1i, backupReg2i, backupReg3i, backupReg4i;" HLSL_NL);
		src->add("int PV0ix = 0, PV0iy = 0, PV0iz = 0, PV0iw = 0, PV1ix = 0, PV1iy = 0, PV1iz = 0, PV1iw = 0;" HLSL_NL);
		src->add("int PS0i = 0, PS1i = 0;" HLSL_NL);
		src->add("ivec4 tempi = ivec4(0);" HLSL_NL);
	}
	if (shaderContext->typeTracker.genFloatReg)
	{
		src->add("float backupReg0f, backupReg1f, backupReg2f, backupReg3f, backupReg4f;" HLSL_NL);
		src->add("float PV0fx = 0.0, PV0fy = 0.0, PV0fz = 0.0, PV0fw = 0.0, PV1fx = 0.0, PV1fy = 0.0, PV1fz = 0.0, PV1fw = 0.0;" HLSL_NL);
		src->add("float PS0f = 0.0, PS1f = 0.0;" HLSL_NL);
		src->add("vec4 tempf = vec4(0.0);" HLSL_NL);
	}
	if (shaderContext->analyzer.hasGradientLookup)
	{
		src->add("vec4 gradH;" HLSL_NL);
		src->add("vec4 gradV;" HLSL_NL);
	}
	src->add("float tempResultf;" HLSL_NL);
	src->add("int tempResulti;" HLSL_NL);
	src->add("ivec4 ARi = ivec4(0);" HLSL_NL);
	if (shaderContext->lowerIndexedGPRWrites)
	{
		src->add("ivec4 indexedWriteTempi = ivec4(0);" HLSL_NL);
		src->add("vec4 indexedWriteTempf = vec4(0.0);" HLSL_NL);
		src->add("int indexedWriteIndex = 0;" HLSL_NL);
	}
	src->add("bool predResult = true;" HLSL_NL);

	if (shaderContext->analyzer.modifiesPixelActiveState)
	{
		src->addFmt("bool activeMaskStack[{}];" HLSL_NL, shaderContext->analyzer.activeStackMaxDepth + 1);
		src->addFmt("bool activeMaskStackC[{}];" HLSL_NL, shaderContext->analyzer.activeStackMaxDepth + 2);
		for (sint32 i = 0; i < shaderContext->analyzer.activeStackMaxDepth; ++i)
			src->addFmt("activeMaskStack[{}] = false;" HLSL_NL, i);
		for (sint32 i = 0; i < shaderContext->analyzer.activeStackMaxDepth + 1; ++i)
			src->addFmt("activeMaskStackC[{}] = false;" HLSL_NL, i);
		src->add("activeMaskStack[0] = true;" HLSL_NL);
		src->add("activeMaskStackC[0] = true;" HLSL_NL);
		src->add("activeMaskStackC[1] = true;" HLSL_NL);
		for (auto& subroutineInfo : shaderContext->list_subroutines)
		{
			sint32 subroutineMaxStackDepth = 0;
			for (auto& cfInstruction : subroutineInfo.instructions)
				subroutineMaxStackDepth = std::max<sint32>(subroutineMaxStackDepth, cfInstruction.activeStackDepth);
			src->addFmt("bool activeMaskStackSub{:04x}[{}];" HLSL_NL, subroutineInfo.cfAddr, subroutineMaxStackDepth + 1);
			src->addFmt("bool activeMaskStackCSub{:04x}[{}];" HLSL_NL, subroutineInfo.cfAddr, subroutineMaxStackDepth + 2);
		}
	}

	if (shaderContext->analyzer.hasRedcCUBE)
	{
		src->add("float3 cubeMapSTM;" HLSL_NL);
		src->add("int cubeMapFaceId;" HLSL_NL);
	}
	for (sint32 i = 0; i < LATTE_NUM_MAX_TEX_UNITS; ++i)
	{
		if (shaderContext->output->textureUnitMask[i] && shaderContext->shader->textureUnitDim[i] == Latte::E_DIM::DIM_CUBEMAP)
			src->addFmt("float cubeMapArrayIndex{} = 0.0;" HLSL_NL, i);
	}
}

void EmitStreamoutBase(LatteDecompilerShaderContext* shaderContext, StringBuf* src)
{
	if (!shaderContext->analyzer.useSSBOForStreamout ||
		(shaderContext->shader->shaderType != LatteConst::ShaderType::Vertex && shaderContext->shader->shaderType != LatteConst::ShaderType::Geometry))
		return;

	for (sint32 i = 0; i < LATTE_NUM_STREAMOUT_BUFFER; ++i)
	{
		if (!shaderContext->output->streamoutBufferWriteMask[i])
			continue;
		if (shaderContext->shader->shaderType == LatteConst::ShaderType::Vertex)
			src->addFmt("int sbBase{} = uf_streamoutBufferBase{}/4 + (gl_VertexID + uf_verticesPerInstance * gl_InstanceID)*{};" HLSL_NL, i, i, shaderContext->output->streamoutBufferStride[i] / 4);
		else
		{
			uint32 bytesPerVertex = shaderContext->contextRegisters[mmSQ_GS_VERT_ITEMSIZE] * 4;
			bytesPerVertex = std::max<uint32>(bytesPerVertex, 1);
			const uint32 maxVerticesInGS = std::max<uint32>(((shaderContext->contextRegisters[mmSQ_GSVS_RING_ITEMSIZE] & 0x7FFF) * 4) / bytesPerVertex, 1);
			src->addFmt("int sbBase{} = uf_streamoutBufferBase{}/4 + (gl_PrimitiveIDIn * {})*{};" HLSL_NL, i, i, maxVerticesInGS, shaderContext->output->streamoutBufferStride[i] / 4);
		}
	}
}

void EmitHLSLClauseCode(LatteDecompilerShaderContext* shaderContext, LatteDecompilerCFInstruction* cfInstruction, bool isSubroutine);

void EmitHLSLExportCode(LatteDecompilerShaderContext* shaderContext, LatteDecompilerCFInstruction* cfInstruction)
{
	StringBuf* src = shaderContext->shaderSource;
	src->add("// hlsl export" HLSL_NL);
	if (shaderContext->shaderType == LatteConst::ShaderType::Vertex)
	{
		if (cfInstruction->exportBurstCount != 0)
			debugBreakpoint();
		if (cfInstruction->exportType == 1 && cfInstruction->exportArrayBase == GPU7_DECOMPILER_CF_EXPORT_BASE_POSITION)
		{
			const bool hasAnyViewportScaleDisabled =
				!shaderContext->contextRegistersNew->PA_CL_VTE_CNTL.get_VPORT_X_SCALE_ENA() ||
				!shaderContext->contextRegistersNew->PA_CL_VTE_CNTL.get_VPORT_Y_SCALE_ENA() ||
				!shaderContext->contextRegistersNew->PA_CL_VTE_CNTL.get_VPORT_Z_SCALE_ENA();

			if (hasAnyViewportScaleDisabled)
			{
				src->add("float4 finalPos = ");
				EmitHLSLExportReadCode(shaderContext, cfInstruction, LATTE_DECOMPILER_DTYPE_FLOAT, 0);
				src->add(";" HLSL_NL);
				src->add("finalPos.xy = finalPos.xy * uf_windowSpaceToClipSpaceTransform - float2(1.0f, 1.0f);" HLSL_NL);
				src->add("SET_POSITION(finalPos);" HLSL_NL);
			}
			else
			{
				src->add("SET_POSITION(");
				EmitHLSLExportReadCode(shaderContext, cfInstruction, LATTE_DECOMPILER_DTYPE_FLOAT, 0);
				src->add(");" HLSL_NL);
			}
		}
		else if (cfInstruction->exportType == 1 && cfInstruction->exportArrayBase == GPU7_DECOMPILER_CF_EXPORT_POINT_SIZE)
		{
			if (shaderContext->analyzer.outputPointSize)
			{
				cemu_assert_debug(shaderContext->analyzer.writesPointSize);
				src->add("gl_PointSize = (");
				EmitHLSLExportReadCode(shaderContext, cfInstruction, LATTE_DECOMPILER_DTYPE_FLOAT, 0);
				src->add(").x;" HLSL_NL);
			}
		}
		else if (cfInstruction->exportType == 2 && cfInstruction->exportArrayBase < 32)
		{
			const uint32 vsSemanticId = LatteShaderPSInputTable::getVertexShaderOutParamSemanticId(shaderContext->contextRegisters, cfInstruction->exportArrayBase);
			if (vsSemanticId != 0xFF && vsSemanticId <= LATTE_ANALYZER_IMPORT_INDEX_PARAM_MAX && FindPSInputBySemanticId(LatteSHRC_GetPSInputTable(), vsSemanticId))
			{
				src->addFmt("passParameterSem{} = ", vsSemanticId);
				EmitHLSLExportReadCode(shaderContext, cfInstruction, LATTE_DECOMPILER_DTYPE_FLOAT, 0);
				src->add(";" HLSL_NL);
			}
			else if (vsSemanticId != 0xFF)
				src->addFmt("// skipped export to unsupported semanticId {}" HLSL_NL, vsSemanticId);
			else
				src->add("// skipped export to semanticId 255" HLSL_NL);
		}
		else
			cemu_assert_unimplemented();
	}
	else if (shaderContext->shaderType == LatteConst::ShaderType::Pixel)
	{
		if (cfInstruction->exportType == 0 && cfInstruction->exportArrayBase < 8)
		{
			for (uint32 i = 0; i < (cfInstruction->exportBurstCount + 1); ++i)
			{
				const sint32 pixelColorOutputIndex = LatteDecompiler_getColorOutputIndexFromExportIndex(shaderContext, cfInstruction->exportArrayBase + i);
				if (pixelColorOutputIndex < 0)
				{
					src->add("// skipped unmapped pixel color export" HLSL_NL);
					continue;
				}

				const bool alphaTestEnable = shaderContext->contextRegistersNew->SX_ALPHA_TEST_CONTROL.get_ALPHA_TEST_ENABLE();
				const auto alphaTestFunc = shaderContext->contextRegistersNew->SX_ALPHA_TEST_CONTROL.get_ALPHA_FUNC();
				if (pixelColorOutputIndex == 0 && alphaTestEnable && alphaTestFunc == Latte::E_COMPAREFUNC::NEVER)
					src->add("discard;" HLSL_NL);
				else if (pixelColorOutputIndex == 0 && alphaTestEnable && alphaTestFunc != Latte::E_COMPAREFUNC::ALWAYS)
				{
					src->add("if (((");
					EmitHLSLExportReadCode(shaderContext, cfInstruction, LATTE_DECOMPILER_DTYPE_FLOAT, i);
					src->add(").a ");
					switch (alphaTestFunc)
					{
					case Latte::E_COMPAREFUNC::LESS:
						src->add("<");
						break;
					case Latte::E_COMPAREFUNC::EQUAL:
						src->add("==");
						break;
					case Latte::E_COMPAREFUNC::LEQUAL:
						src->add("<=");
						break;
					case Latte::E_COMPAREFUNC::GREATER:
						src->add(">");
						break;
					case Latte::E_COMPAREFUNC::NOTEQUAL:
						src->add("!=");
						break;
					case Latte::E_COMPAREFUNC::GEQUAL:
						src->add(">=");
						break;
					default:
						src->add("==");
						break;
					}
					src->add(" uf_alphaTestRef) == false) discard;" HLSL_NL);
				}

				const sint32 outputType = GetHLSLPixelOutputType(shaderContext, (uint32)pixelColorOutputIndex);
				src->addFmt("passPixelColor{} = ", pixelColorOutputIndex);
				if (outputType == LATTE_DECOMPILER_DTYPE_UNSIGNED_INT)
					src->add("asuint(");
				EmitHLSLExportReadCode(shaderContext, cfInstruction, outputType == LATTE_DECOMPILER_DTYPE_FLOAT ? LATTE_DECOMPILER_DTYPE_FLOAT : LATTE_DECOMPILER_DTYPE_SIGNED_INT, i);
				if (outputType == LATTE_DECOMPILER_DTYPE_UNSIGNED_INT)
					src->add(")");
				src->add(";" HLSL_NL);
			}
		}
		else if (cfInstruction->exportType == 0 && cfInstruction->exportArrayBase == GPU7_DECOMPILER_CF_EXPORT_POINT_SIZE)
		{
			if (cfInstruction->exportBurstCount > 0)
				cemu_assert_unimplemented();
			src->add("gl_FragDepth = (");
			EmitHLSLExportReadCode(shaderContext, cfInstruction, LATTE_DECOMPILER_DTYPE_FLOAT, 0);
			src->add(").x;" HLSL_NL);
		}
		else
			cemu_assert_unimplemented();
	}
}

void EmitHLSLCFRingWriteCode(LatteDecompilerShaderContext* shaderContext, LatteDecompilerCFInstruction* cfInstruction)
{
	StringBuf* src = shaderContext->shaderSource;
	uint32 bytesPerVertex = shaderContext->contextRegisters[mmSQ_GS_VERT_ITEMSIZE] * 4;
	bytesPerVertex = std::max<uint32>(bytesPerVertex, 1);
	uint32 parameterOffset = ((cfInstruction->exportArrayBase * 4) % bytesPerVertex);

	if (shaderContext->shaderType == LatteConst::ShaderType::Geometry && shaderContext->analyzer.hasStreamoutEnable)
	{
		if (!shaderContext->parsedGSCopyShader)
		{
			shaderContext->hasError = true;
			return;
		}

		for (uint32 burstIndex = 0; burstIndex < (cfInstruction->exportBurstCount + 1); ++burstIndex)
		{
			parameterOffset = ((cfInstruction->exportArrayBase * 4 + burstIndex * 0x10) % bytesPerVertex);
			LatteGSCopyShaderStreamWrite_t* streamWrite = nullptr;
			for (auto& it : shaderContext->parsedGSCopyShader->list_streamWrites)
			{
				if (it.offset == parameterOffset)
				{
					streamWrite = &it;
					break;
				}
			}
			if (!streamWrite)
			{
				cemu_assert_suspicious();
				shaderContext->hasError = true;
				return;
			}

			for (uint32 i = 0; i < 4; ++i)
			{
				if ((cfInstruction->memWriteCompMask & (1u << i)) == 0)
					continue;
				src->addFmt("sb_buffer[sbBase{} + {}] = {};" HLSL_NL,
							streamWrite->bufferIndex,
							streamWrite->exportArrayBase + i,
							GetRegisterScalarExpression(shaderContext, cfInstruction->exportSourceGPR + burstIndex, i, LATTE_DECOMPILER_DTYPE_SIGNED_INT));
			}
		}
		return;
	}

	const std::string writeMask = GetXYZWByMask(cfInstruction->memWriteCompMask);
	if (writeMask.empty())
		return;

	if (shaderContext->shaderType == LatteConst::ShaderType::Vertex)
	{
		if (cfInstruction->memWriteElemSize != 3)
			cemu_assert_unimplemented();
		if ((cfInstruction->exportArrayBase & 3) != 0)
			cemu_assert_unimplemented();
		for (uint32 burstIndex = 0; burstIndex < (cfInstruction->exportBurstCount + 1); ++burstIndex)
		{
			src->addFmt("v2g.passV2GParameter{}.{} = ", cfInstruction->exportArrayBase / 4 + burstIndex, writeMask);
			EmitHLSLExportReadCode(shaderContext, cfInstruction, LATTE_DECOMPILER_DTYPE_SIGNED_INT, burstIndex);
			src->add(";" HLSL_NL);
		}
	}
	else if (shaderContext->shaderType == LatteConst::ShaderType::Geometry)
	{
		cemu_assert_debug(cfInstruction->memWriteElemSize == 3);
		cemu_assert_debug((cfInstruction->exportArrayBase & 3) == 0);
		if (!shaderContext->parsedGSCopyShader)
		{
			shaderContext->hasError = true;
			return;
		}

		for (uint32 burstIndex = 0; burstIndex < (cfInstruction->exportBurstCount + 1); ++burstIndex)
		{
			uint32 parameterExportType = 0;
			uint32 parameterExportBase = 0;
			if (!LatteGSCopyShaderParser_getExportTypeByOffset(shaderContext->parsedGSCopyShader, parameterOffset + burstIndex * (cfInstruction->memWriteElemSize + 1) * 4, &parameterExportType, &parameterExportBase))
			{
				cemu_assert_debug(false);
				shaderContext->hasError = true;
				return;
			}

			if (parameterExportType == 1 && parameterExportBase == GPU7_DECOMPILER_CF_EXPORT_BASE_POSITION)
			{
				src->add("{" HLSL_NL);
				src->add("float4 pos = float4(0.0f, 0.0f, 0.0f, 1.0f);" HLSL_NL);
				src->addFmt("pos.{} = ", writeMask);
				EmitHLSLExportReadCode(shaderContext, cfInstruction, LATTE_DECOMPILER_DTYPE_FLOAT, burstIndex);
				src->add(";" HLSL_NL);
				src->add("SET_POSITION(pos);" HLSL_NL);
				src->add("}" HLSL_NL);
			}
			else if (parameterExportType == 2 && parameterExportBase < 16)
			{
				src->addFmt("passG2PParameter{}.{} = ", parameterExportBase, writeMask);
				EmitHLSLExportReadCode(shaderContext, cfInstruction, LATTE_DECOMPILER_DTYPE_FLOAT, burstIndex);
				src->add(";" HLSL_NL);
			}
			else
				cemu_assert_debug(false);
		}
	}
	else
		debugBreakpoint();
}

void EmitHLSLStreamWriteCode(LatteDecompilerShaderContext* shaderContext, LatteDecompilerCFInstruction* cfInstruction)
{
	StringBuf* src = shaderContext->shaderSource;
	if (!shaderContext->analyzer.hasStreamoutEnable)
	{
#ifdef CEMU_DEBUG_ASSERT
		src->add("// omitted streamout write" HLSL_NL);
#endif
		return;
	}

	uint32 streamoutBufferIndex = 0;
	if (cfInstruction->type == GPU7_CF_INST_MEM_STREAM0_WRITE)
		streamoutBufferIndex = 0;
	else if (cfInstruction->type == GPU7_CF_INST_MEM_STREAM1_WRITE)
		streamoutBufferIndex = 1;
	else
		cemu_assert_unimplemented();

	if (shaderContext->shaderType != LatteConst::ShaderType::Vertex)
	{
		cemu_assert_debug(false);
		return;
	}

	const uint32 arraySize = std::min<uint32>(cfInstruction->memWriteArraySize + 1, 4);
	for (uint32 i = 0; i < arraySize; ++i)
	{
		if ((cfInstruction->memWriteCompMask & (1u << i)) == 0)
			continue;
		src->addFmt("sb_buffer[sbBase{} + {}] = {};" HLSL_NL,
					streamoutBufferIndex,
					cfInstruction->exportArrayBase + i,
					GetRegisterScalarExpression(shaderContext, cfInstruction->exportSourceGPR, i, LATTE_DECOMPILER_DTYPE_SIGNED_INT));
	}
}

void EmitHLSLCFCall(LatteDecompilerShaderContext* shaderContext, LatteDecompilerCFInstruction* cfInstruction)
{
	StringBuf* src = shaderContext->shaderSource;
	LatteDecompilerSubroutineInfo* subroutineInfo = nullptr;
	for (auto& subroutineItr : shaderContext->list_subroutines)
	{
		if (subroutineItr.cfAddr == cfInstruction->addr)
		{
			subroutineInfo = &subroutineItr;
			break;
		}
	}
	if (!subroutineInfo)
	{
		cemu_assert_debug(false);
		return;
	}
	if (shaderContext->isSubroutine)
	{
		cemu_assert_debug(false);
		return;
	}

	src->addFmt("activeMaskStackSub{:04x}[0] = true;" HLSL_NL, subroutineInfo->cfAddr);
	src->addFmt("activeMaskStackCSub{:04x}[0] = true;" HLSL_NL, subroutineInfo->cfAddr);
	src->addFmt("activeMaskStackCSub{:04x}[1] = true;" HLSL_NL, subroutineInfo->cfAddr);

	shaderContext->isSubroutine = true;
	shaderContext->subroutineInfo = subroutineInfo;
	for (auto& subroutineInstruction : subroutineInfo->instructions)
		EmitHLSLClauseCode(shaderContext, &subroutineInstruction, true);
	shaderContext->isSubroutine = false;
	shaderContext->subroutineInfo = nullptr;
}

void EmitHLSLClauseCode(LatteDecompilerShaderContext* shaderContext, LatteDecompilerCFInstruction* cfInstruction, bool isSubroutine)
{
	StringBuf* src = shaderContext->shaderSource;

	if (cfInstruction->type == GPU7_CF_INST_ALU ||
		cfInstruction->type == GPU7_CF_INST_ALU_PUSH_BEFORE ||
		cfInstruction->type == GPU7_CF_INST_ALU_POP_AFTER ||
		cfInstruction->type == GPU7_CF_INST_ALU_POP2_AFTER ||
		cfInstruction->type == GPU7_CF_INST_ALU_BREAK ||
		cfInstruction->type == GPU7_CF_INST_ALU_ELSE_AFTER ||
		cfInstruction->type == GPU7_CF_INST_TEX)
	{
		LatteDecompiler_emitClauseCode(shaderContext, cfInstruction, isSubroutine);
	}
	else if (cfInstruction->type == GPU7_CF_INST_EXPORT || cfInstruction->type == GPU7_CF_INST_EXPORT_DONE)
	{
		EmitHLSLExportCode(shaderContext, cfInstruction);
	}
	else if (cfInstruction->type == GPU7_CF_INST_ELSE)
	{
		src->addFmt("{} = {} == false;" HLSL_NL, GetActiveMaskVarName(shaderContext, cfInstruction->activeStackDepth), GetActiveMaskVarName(shaderContext, cfInstruction->activeStackDepth));
		src->addFmt("{} = {} == true && {} == true;" HLSL_NL, GetActiveMaskCVarName(shaderContext, cfInstruction->activeStackDepth + 1), GetActiveMaskVarName(shaderContext, cfInstruction->activeStackDepth), GetActiveMaskCVarName(shaderContext, cfInstruction->activeStackDepth));
	}
	else if (cfInstruction->type == GPU7_CF_INST_POP)
	{
		src->addFmt("{} = {} == true && {} == true;" HLSL_NL, GetActiveMaskCVarName(shaderContext, cfInstruction->activeStackDepth + 1 - cfInstruction->popCount), GetActiveMaskVarName(shaderContext, cfInstruction->activeStackDepth - cfInstruction->popCount), GetActiveMaskCVarName(shaderContext, cfInstruction->activeStackDepth - cfInstruction->popCount));
	}
	else if (cfInstruction->type == GPU7_CF_INST_LOOP_START_DX10 ||
			 cfInstruction->type == GPU7_CF_INST_LOOP_START_NO_AL)
	{
		if (ActiveSettings::ShaderPreventInfiniteLoopsEnabled())
		{
			src->addFmt("int loopCounter{} = 0;" HLSL_NL, (sint32)cfInstruction->cfAddr);
			src->addFmt("while ({} == true && loopCounter{} < 500)" HLSL_NL, GetActiveMaskCVarName(shaderContext, cfInstruction->activeStackDepth + 1), (sint32)cfInstruction->cfAddr);
			src->add("{" HLSL_NL);
			src->addFmt("loopCounter{}++;" HLSL_NL, (sint32)cfInstruction->cfAddr);
		}
		else
		{
			src->addFmt("while ({} == true)" HLSL_NL, GetActiveMaskCVarName(shaderContext, cfInstruction->activeStackDepth + 1));
			src->add("{" HLSL_NL);
		}
	}
	else if (cfInstruction->type == GPU7_CF_INST_LOOP_END)
	{
		if (cfInstruction->popCount != 0)
			debugBreakpoint();
		src->add("}" HLSL_NL);
	}
	else if (cfInstruction->type == GPU7_CF_INST_LOOP_BREAK)
	{
		if (cfInstruction->popCount != 0)
			debugBreakpoint();
		if (shaderContext->analyzer.modifiesPixelActiveState)
			src->addFmt("if ({} == true) {{" HLSL_NL, GetActiveMaskCVarName(shaderContext, cfInstruction->activeStackDepth + 1));
		src->add("break;" HLSL_NL);
		if (shaderContext->analyzer.modifiesPixelActiveState)
			src->add("}" HLSL_NL);
	}
	else if (cfInstruction->type == GPU7_CF_INST_MEM_STREAM0_WRITE ||
			 cfInstruction->type == GPU7_CF_INST_MEM_STREAM1_WRITE)
	{
		EmitHLSLStreamWriteCode(shaderContext, cfInstruction);
	}
	else if (cfInstruction->type == GPU7_CF_INST_MEM_RING_WRITE)
	{
		EmitHLSLCFRingWriteCode(shaderContext, cfInstruction);
	}
	else if (cfInstruction->type == GPU7_CF_INST_EMIT_VERTEX)
	{
		if (shaderContext->analyzer.modifiesPixelActiveState)
			src->addFmt("if ({} == true) {{" HLSL_NL, GetActiveMaskCVarName(shaderContext, cfInstruction->activeStackDepth + 1));
		if (shaderContext->analyzer.outputPointSize && !shaderContext->analyzer.writesPointSize)
			src->add("gl_PointSize = uf_pointSize;" HLSL_NL);
		src->add("EmitVertex();" HLSL_NL);
		if (shaderContext->analyzer.useSSBOForStreamout)
		{
			for (sint32 i = 0; i < LATTE_NUM_STREAMOUT_BUFFER; ++i)
			{
				if (!shaderContext->output->streamoutBufferWriteMask[i])
					continue;
				cemu_assert_debug((shaderContext->output->streamoutBufferStride[i] & 3) == 0);
				src->addFmt("sbBase{} += {};" HLSL_NL, i, shaderContext->output->streamoutBufferStride[i] / 4);
			}
		}
		if (shaderContext->analyzer.modifiesPixelActiveState)
			src->add("}" HLSL_NL);
	}
	else if (cfInstruction->type == GPU7_CF_INST_CALL)
	{
		EmitHLSLCFCall(shaderContext, cfInstruction);
	}
	else if (cfInstruction->type == GPU7_CF_INST_RETURN)
	{
	}
	else
		cemu_assert_debug(false);
}

void EmitClauseCode(LatteDecompilerShaderContext* shaderContext)
{
	for (auto& cfInstruction : shaderContext->cfInstructions)
		EmitHLSLClauseCode(shaderContext, &cfInstruction, false);
}

void EmitVertexShader(LatteDecompilerShaderContext* shaderContext, StringBuf* src)
{
	const bool usesGeometryShader = shaderContext->options->usesGeometryShader;
	if (usesGeometryShader)
		EmitV2GLocalStruct(shaderContext, src);

	src->add("struct VSOut" HLSL_NL "{" HLSL_NL);
#if defined(CEMU_UWP)
	// precise keeps prepass and color-pass positions identical for EQUAL depth tests
	src->add("\tprecise float4 position : SV_Position;" HLSL_NL);
#else
	src->add("\tfloat4 position : SV_Position;" HLSL_NL);
#endif
	if (usesGeometryShader)
		EmitV2GFields(shaderContext, src);
	else
	{
		EmitVaryingFields(shaderContext, src, true, "passParameterSem");
		if (shaderContext->analyzer.outputPointSize)
			src->add("\tfloat pointSize : POINTSIZE;" HLSL_NL);
	}
	src->add("};" HLSL_NL);

	EmitSetPositionMacro(shaderContext, src);
	src->add("VSOut main(uint vertexId : SV_VertexID, uint instanceId : SV_InstanceID)" HLSL_NL "{" HLSL_NL);
	src->add("\tVSOut output = (VSOut)0;" HLSL_NL);
	if (usesGeometryShader && GetV2GParameterCount(shaderContext) != 0)
		src->add("\tV2GLocal v2g = (V2GLocal)0;" HLSL_NL);
	src->add("\tuint latteD3D12VertexId = vertexId;" HLSL_NL);
	src->add("\tif (uf_d3d12Reserved0 == 1u)" HLSL_NL);
	src->add("\t{" HLSL_NL);
	src->add("\t\tuint latteD3D12QuadVertexLut[6] = { 0u, 1u, 2u, 0u, 2u, 3u };" HLSL_NL);
	src->add("\t\tlatteD3D12VertexId = (vertexId / 6u) * 4u + latteD3D12QuadVertexLut[vertexId % 6u];" HLSL_NL);
	src->add("\t}" HLSL_NL);
	src->add("\tuint gl_VertexID = latteD3D12VertexId + uf_d3d12BaseVertex;" HLSL_NL);
	src->add("\tuint gl_InstanceID = instanceId + uf_d3d12BaseInstance;" HLSL_NL);
	if (shaderContext->analyzer.outputPointSize && shaderContext->analyzer.writesPointSize == false && !usesGeometryShader)
		src->add("\tfloat gl_PointSize = uf_pointSize;" HLSL_NL);
	else
		src->add("\tfloat gl_PointSize = 1.0f;" HLSL_NL);
	if (!usesGeometryShader)
		EmitVertexParameterLocalDeclarations(shaderContext, src);
	EmitRegisterDeclarations(shaderContext, src);
	EmitStreamoutBase(shaderContext, src);

	if ((shaderContext->analyzer.gprUseMask[0] & 1) != 0)
	{
		const std::string r0 = GetRegisterVarName(shaderContext, 0);
		if (shaderContext->typeTracker.defaultDataType == LATTE_DECOMPILER_DTYPE_SIGNED_INT)
			src->addFmt("{} = ivec4((int)gl_VertexID, 0, 0, (int)gl_InstanceID);" HLSL_NL, r0);
		else if (shaderContext->typeTracker.defaultDataType == LATTE_DECOMPILER_DTYPE_FLOAT)
			src->addFmt("{} = asfloat(ivec4((int)gl_VertexID, 0, 0, (int)gl_InstanceID));" HLSL_NL, r0);
	}

	EmitVertexAttributeImports(shaderContext, src);
	EmitClauseCode(shaderContext);

	if (usesGeometryShader)
	{
		for (uint32 i = 0; i < GetV2GParameterCount(shaderContext); ++i)
			src->addFmt("\toutput.passV2GParameter{} = v2g.passV2GParameter{};" HLSL_NL, i, i);
	}
	else
	{
		LatteShaderPSInputTable* psInputTable = LatteSHRC_GetPSInputTable();
		if (psInputTable)
		{
			const uint32 parameterMask = shaderContext->shader->outputParameterMask;
			for (uint32 i = 0; i < 32; ++i)
			{
				if ((parameterMask & (1u << i)) == 0)
					continue;
				const sint32 semanticId = LatteShaderPSInputTable::getVertexShaderOutParamSemanticId(shaderContext->contextRegisters, i);
				if (semanticId >= 0 && semanticId != 0xFF && semanticId <= LATTE_ANALYZER_IMPORT_INDEX_PARAM_MAX && FindPSInputBySemanticId(psInputTable, (uint32)semanticId))
					src->addFmt("\toutput.passParameterSem{} = passParameterSem{};" HLSL_NL, semanticId, semanticId);
			}
		}
	}
#if defined(CEMU_UWP)
	if (shaderContext->analyzer.outputPointSize && !shaderContext->options->usesGeometryShader)
#else
	if (shaderContext->analyzer.outputPointSize && shaderContext->analyzer.writesPointSize == false && !shaderContext->options->usesGeometryShader)
#endif
		src->add("\toutput.pointSize = gl_PointSize;" HLSL_NL);
	src->add("\treturn output;" HLSL_NL "}" HLSL_NL);
	src->add("#undef SET_POSITION" HLSL_NL);
}

void EmitPixelImports(LatteDecompilerShaderContext* shaderContext, StringBuf* src)
{
	LatteShaderPSInputTable* psInputTable = LatteSHRC_GetPSInputTable();
	if (!psInputTable)
		return;

	uint32 spiInterpControl = shaderContext->contextRegisters[mmSPI_INTERP_CONTROL_0];
	uint8 spriteEnable = (spiInterpControl >> 1) & 1;
	(void)spriteEnable;

	if (psInputTable->paramGen != 0)
	{
		const uint32 paramGenGPRIndex = psInputTable->paramGenGPR;
		if (shaderContext->typeTracker.defaultDataType == LATTE_DECOMPILER_DTYPE_FLOAT)
			src->addFmt("{} = gl_PointCoord.xyxy;" HLSL_NL, GetRegisterVarName(shaderContext, paramGenGPRIndex));
		else
			src->addFmt("{} = asint(gl_PointCoord.xyxy);" HLSL_NL, GetRegisterVarName(shaderContext, paramGenGPRIndex));
	}

	for (sint32 i = 0; i < psInputTable->count; ++i)
	{
		const sint32 gprIndex = i;
		if ((shaderContext->analyzer.gprUseMask[gprIndex / 8] & (1 << (gprIndex % 8))) == 0 && !shaderContext->analyzer.usesRelativeGPRRead)
			continue;
		const uint32 semanticId = psInputTable->import[i].semanticId;
		if (semanticId == LATTE_ANALYZER_IMPORT_INDEX_SPIPOSITION)
		{
			if (shaderContext->typeTracker.defaultDataType == LATTE_DECOMPILER_DTYPE_FLOAT)
				src->addFmt("{} = GET_FRAGCOORD();" HLSL_NL, GetRegisterVarName(shaderContext, gprIndex));
			else
				src->addFmt("{} = asint(GET_FRAGCOORD());" HLSL_NL, GetRegisterVarName(shaderContext, gprIndex));
			continue;
		}
		if (semanticId > LATTE_ANALYZER_IMPORT_INDEX_PARAM_MAX)
			continue;

		std::string varying = fmt::format("input.passParameterSem{}", semanticId);
		if (shaderContext->options->usesGeometryShader)
		{
			const uint32 slot = semanticId & 0x7F;
			varying = slot < kG2PMaxSlots ? fmt::format("input.passG2PParameter{}", slot) : "float4(0.0f, 0.0f, 0.0f, 0.0f)";
		}
		if (shaderContext->typeTracker.defaultDataType == LATTE_DECOMPILER_DTYPE_SIGNED_INT)
			src->addFmt("{} = asint({});" HLSL_NL, GetRegisterVarName(shaderContext, gprIndex), varying);
		else if (shaderContext->typeTracker.defaultDataType == LATTE_DECOMPILER_DTYPE_FLOAT)
			src->addFmt("{} = {};" HLSL_NL, GetRegisterVarName(shaderContext, gprIndex), varying);
	}

	uint32 psControl1 = shaderContext->contextRegisters[mmSPI_PS_IN_CONTROL_1];
	uint8 frontFaceEnabled = (psControl1 >> 8) & 1;
	uint8 frontFaceChan = (psControl1 >> 9) & 3;
	uint8 frontFaceRegIndex = (psControl1 >> 12) & 0x1F;
	if (frontFaceEnabled && (shaderContext->analyzer.gprUseMask[frontFaceRegIndex / 8] & (1 << (frontFaceRegIndex % 8))) != 0)
	{
		const char* chan = frontFaceChan == 0 ? "x" : frontFaceChan == 1 ? "y" : frontFaceChan == 2 ? "z" : "w";
		if (shaderContext->typeTracker.defaultDataType == LATTE_DECOMPILER_DTYPE_SIGNED_INT)
			src->addFmt("{}.{} = asint(gl_FrontFacing ? 1.0f : 0.0f);" HLSL_NL, GetRegisterVarName(shaderContext, frontFaceRegIndex), chan);
		else if (shaderContext->typeTracker.defaultDataType == LATTE_DECOMPILER_DTYPE_FLOAT)
			src->addFmt("{}.{} = gl_FrontFacing ? 1.0f : 0.0f;" HLSL_NL, GetRegisterVarName(shaderContext, frontFaceRegIndex), chan);
	}
}

void EmitPixelShader(LatteDecompilerShaderContext* shaderContext, StringBuf* src)
{
	src->add("struct PSIn" HLSL_NL "{" HLSL_NL);
	src->add("\tfloat4 position : SV_Position;" HLSL_NL);
	EmitVaryingFields(shaderContext, src, false, shaderContext->options->usesGeometryShader ? "passG2PParameter" : "passParameterSem");
	const bool usesD3D12PointExpansion = shaderContext->analyzer.isPointsPrimitive && !shaderContext->options->usesGeometryShader;
	if (usesD3D12PointExpansion)
#if defined(CEMU_UWP)
		src->add("\tfloat2 pointCoord : CEMUPOINTCOORD;" HLSL_NL);
#else
		src->add("\tfloat2 pointCoord : POINTCOORD;" HLSL_NL);
#endif
	src->add("};" HLSL_NL);

#if defined(CEMU_UWP)
	const uint32 colorMask = shaderContext->shader->pixelColorOutputMask;
	const bool hasOutputStruct = colorMask != 0 || shaderContext->shader->depthMask;
#else
	const uint32 colorMask = shaderContext->shader->pixelColorOutputMask ? shaderContext->shader->pixelColorOutputMask : 1u;
	constexpr bool hasOutputStruct = true;
#endif
	if (hasOutputStruct)
		src->add("struct PSOut" HLSL_NL "{" HLSL_NL);
	for (uint32 i = 0; i < 8; ++i)
	{
		if ((colorMask & (1u << i)) != 0)
			src->addFmt("\t{} passPixelColor{} : SV_Target{};" HLSL_NL, GetHLSLPixelOutputTypeName(GetHLSLPixelOutputType(shaderContext, i)), i, i);
	}
	if (shaderContext->shader->depthMask)
		src->add("\tfloat depth : SV_Depth;" HLSL_NL);
	if (hasOutputStruct)
		src->add("};" HLSL_NL);

	// SV_Position.w is already the clip-space w, unlike gl_FragCoord.w which holds its reciprocal
	src->add("#define GET_FRAGCOORD() float4(gl_FragCoord.xy * uf_fragCoordScale.xy, gl_FragCoord.z, gl_FragCoord.w)" HLSL_NL);
	if (hasOutputStruct)
	{
		src->add("PSOut main(PSIn input, bool isFrontFace : SV_IsFrontFace)" HLSL_NL "{" HLSL_NL);
		src->add("\tPSOut output = (PSOut)0;" HLSL_NL);
	}
	else
		src->add("void main(PSIn input, bool isFrontFace : SV_IsFrontFace)" HLSL_NL "{" HLSL_NL);
	src->add("\tfloat4 gl_FragCoord = input.position;" HLSL_NL);
	src->add("\tbool gl_FrontFacing = isFrontFace;" HLSL_NL);
	if (usesD3D12PointExpansion)
		src->add("\tfloat4 gl_PointCoord = float4(input.pointCoord, 0.0f, 0.0f);" HLSL_NL);
	else
		src->add("\tfloat4 gl_PointCoord = float4(0.0f, 0.0f, 0.0f, 0.0f);" HLSL_NL);
	for (uint32 i = 0; i < 8; ++i)
	{
		const char* typeName = GetHLSLPixelOutputTypeName(GetHLSLPixelOutputType(shaderContext, i));
		src->addFmt("\t{} passPixelColor{} = ({})0;" HLSL_NL, typeName, i, typeName);
	}
	if (shaderContext->shader->depthMask)
		src->add("\tfloat gl_FragDepth = gl_FragCoord.z;" HLSL_NL);
	EmitRegisterDeclarations(shaderContext, src);
	EmitPixelImports(shaderContext, src);
	EmitClauseCode(shaderContext);
	for (uint32 i = 0; i < 8; ++i)
	{
		if ((colorMask & (1u << i)) != 0)
		{
#if defined(CEMU_UWP)
			if (GetHLSLPixelOutputType(shaderContext, i) == LATTE_DECOMPILER_DTYPE_FLOAT)
			{
			src->add("\tif (uf_d3d12LogicOpMode != 0u)" HLSL_NL);
			src->add("\t{" HLSL_NL);
			src->add("\t\tfloat4 latteD3D12DstColor = float4(0.0f, 0.0f, 0.0f, 0.0f);" HLSL_NL);
			src->add("\t\tif (uf_d3d12LogicOpMode == 2u || uf_d3d12LogicOpMode >= 5u)" HLSL_NL);
			src->addFmt("\t\t\tlatteD3D12DstColor = latteD3D12LogicOpDst{}.Load(int3((int2)input.position.xy, 0));" HLSL_NL, i);
			src->addFmt("\t\tpassPixelColor{} = latteLogicOpUnorm8(passPixelColor{}, latteD3D12DstColor, uf_d3d12LogicOpMode);" HLSL_NL, i, i);
			src->add("\t}" HLSL_NL);
			}
#else
			if (i == 0)
			{
				src->add("\tif (uf_d3d12LogicOpMode != 0u)" HLSL_NL);
				src->add("\t{" HLSL_NL);
				src->add("\t\tfloat4 latteD3D12DstColor = float4(0.0f, 0.0f, 0.0f, 0.0f);" HLSL_NL);
				src->add("\t\tif (uf_d3d12LogicOpMode == 2u)" HLSL_NL);
				src->add("\t\t\tlatteD3D12DstColor = latteD3D12LogicOpDst.Load(int3((int2)input.position.xy, 0));" HLSL_NL);
				src->add("\t\tpassPixelColor0 = latteLogicOpUnorm8(passPixelColor0, latteD3D12DstColor, uf_d3d12LogicOpMode);" HLSL_NL);
				src->add("\t}" HLSL_NL);
			}
#endif
			src->addFmt("\toutput.passPixelColor{} = passPixelColor{};" HLSL_NL, i, i);
		}
	}
	if (shaderContext->shader->depthMask)
		src->add("\toutput.depth = gl_FragDepth;" HLSL_NL);
	if (hasOutputStruct)
		src->add("\treturn output;" HLSL_NL "}" HLSL_NL);
	else
		src->add("\treturn;" HLSL_NL "}" HLSL_NL);
	src->add("#undef GET_FRAGCOORD" HLSL_NL);
}

void EmitGeometryShader(LatteDecompilerShaderContext* shaderContext, StringBuf* src)
{
	src->add("struct GSIn" HLSL_NL "{" HLSL_NL);
	src->add("\tfloat4 position : SV_Position;" HLSL_NL);
	EmitV2GFields(shaderContext, src);
	src->add("};" HLSL_NL);

	src->add("struct GSOut" HLSL_NL "{" HLSL_NL);
#if defined(CEMU_UWP)
	src->add("\tprecise float4 position : SV_Position;" HLSL_NL);
#else
	src->add("\tfloat4 position : SV_Position;" HLSL_NL);
#endif
	EmitG2PFields(shaderContext, src, "passG2PParameter");
	src->add("};" HLSL_NL);

	const bool expandsPoints = GeometryShaderExpandsPoints(shaderContext);
	if (expandsPoints)
	{
		src->add("void latteEmitPointQuad(inout TriangleStream<GSOut> stream, GSOut center, float writtenPointSize)" HLSL_NL "{" HLSL_NL);
		if (shaderContext->analyzer.writesPointSize)
			src->add("\tfloat2 pointSize = max(writtenPointSize.xx, float2(0.125f, 0.125f));" HLSL_NL);
		else
			src->add("\tfloat2 pointSize = max(float2(asfloat(uf_d3d12DrawConstantsPadding.x), asfloat(uf_d3d12DrawConstantsPadding.y)), float2(0.125f, 0.125f));" HLSL_NL);
		src->add("\tfloat2 halfClip = pointSize * uf_d3d12InvViewportSize * center.position.w;" HLSL_NL);
		src->add("\tfloat2 offsets[4] = { float2(-halfClip.x, -halfClip.y), float2(-halfClip.x, halfClip.y), float2(halfClip.x, -halfClip.y), float2(halfClip.x, halfClip.y) };" HLSL_NL);
		src->add("\t[unroll] for (uint i = 0; i < 4; ++i)" HLSL_NL "\t{" HLSL_NL);
		src->add("\t\tGSOut corner = center;" HLSL_NL);
		src->add("\t\tcorner.position.xy += offsets[i];" HLSL_NL);
		src->add("\t\tstream.Append(corner);" HLSL_NL);
		src->add("\t}" HLSL_NL);
		src->add("\tstream.RestartStrip();" HLSL_NL);
		src->add("}" HLSL_NL);
	}

	EmitSetPositionMacro(shaderContext, src);
	src->add("#define EndPrimitive() stream.RestartStrip()" HLSL_NL);
	src->add("#define v2g input" HLSL_NL);
	src->add("#define EmitVertex() {");
	EmitG2POutputAssignments(shaderContext, src);
	if (expandsPoints)
		src->add(" latteEmitPointQuad(stream, output, gl_PointSize); output = (GSOut)0; }" HLSL_NL);
	else
		src->add(" stream.Append(output); output = (GSOut)0; }" HLSL_NL);

	src->addFmt("[maxvertexcount({})]" HLSL_NL, GetGeometryDeclaredMaxVertexCount(shaderContext));
	src->addFmt("void main({} GSIn input[{}], uint primitiveId : SV_PrimitiveID, inout {}<GSOut> stream)" HLSL_NL,
				GetGeometryInputPrimitiveName(shaderContext),
				GetGeometryInputVertexCount(shaderContext),
				GetGeometryStreamTypeName(shaderContext));
	src->add("{" HLSL_NL);
	src->add("\tGSOut output = (GSOut)0;" HLSL_NL);
	src->add("\tuint gl_PrimitiveIDIn = primitiveId;" HLSL_NL);
	if (shaderContext->analyzer.outputPointSize && shaderContext->analyzer.writesPointSize == false)
		src->add("\tfloat gl_PointSize = uf_pointSize;" HLSL_NL);
	else
		src->add("\tfloat gl_PointSize = 1.0f;" HLSL_NL);
	EmitG2PLocalDeclarations(shaderContext, src);
	EmitRegisterDeclarations(shaderContext, src);
	EmitStreamoutBase(shaderContext, src);
	EmitClauseCode(shaderContext);
	src->add("\tEndPrimitive();" HLSL_NL);
	src->add("}" HLSL_NL);
	src->add("#undef EmitVertex" HLSL_NL);
	src->add("#undef v2g" HLSL_NL);
	src->add("#undef EndPrimitive" HLSL_NL);
	src->add("#undef SET_POSITION" HLSL_NL);
}

void MarkStreamoutSourceRegistersUsed(LatteDecompilerShaderContext* shaderContext, const std::vector<LatteDecompilerCFInstruction>& cfInstructions)
{
	if (!shaderContext->analyzer.hasStreamoutEnable)
		return;
	for (const auto& cfInstruction : cfInstructions)
	{
		if (cfInstruction.type != GPU7_CF_INST_MEM_STREAM0_WRITE && cfInstruction.type != GPU7_CF_INST_MEM_STREAM1_WRITE)
			continue;
		const uint32 gprIndex = cfInstruction.exportSourceGPR;
		shaderContext->analyzer.gprUseMask[gprIndex / 8] |= (1 << (gprIndex % 8));
	}
}

// selects the indexed writes emit: one per relative write and candidate register
uint32 CountIndexedGPRWriteSelects(LatteDecompilerShaderContext* shaderContext)
{
	uint32 writes = 0;
	const auto countWrites = [&writes](const std::vector<LatteDecompilerCFInstruction>& cfInstructions)
	{
		for (const auto& cfInstruction : cfInstructions)
		{
			for (const auto& aluInstruction : cfInstruction.instructionsALU)
			{
				if (aluInstruction.destRel != 0 && aluInstruction.writeMask != 0)
					writes++;
			}
		}
	};
	countWrites(shaderContext->cfInstructions);
	for (const auto& subroutineInfo : shaderContext->list_subroutines)
		countWrites(subroutineInfo.instructions);
	uint32 registers = 0;
	for (sint32 i = 0; i < (sint32)shaderContext->indexedGPRLimit; i++)
	{
		if (shaderContext->analyzer.usesRelativeGPRRead || (shaderContext->analyzer.gprUseMask[i / 8] & (1 << (i % 8))) != 0)
			registers++;
	}
	return writes * registers;
}

// a program cannot address registers beyond the ones it allocates
uint32 GetIndexedGPRLimit(const LatteDecompilerShaderContext* shaderContext)
{
	const LatteContextRegister& registers = *shaderContext->contextRegistersNew;
	uint32 allocated;
	if (shaderContext->shaderType == LatteConst::ShaderType::Pixel)
		allocated = registers.SQ_PGM_RESOURCES_PS.get_NUM_GPRS();
	else if (shaderContext->shaderType == LatteConst::ShaderType::Geometry)
		allocated = registers.SQ_PGM_RESOURCES_GS.get_NUM_GPRS();
	else if (shaderContext->options->usesGeometryShader)
		allocated = registers.SQ_PGM_RESOURCES_ES.get_NUM_GPRS(); // a vertex shader feeding a geometry shader runs as the export shader
	else
		allocated = registers.SQ_PGM_RESOURCES_VS.get_NUM_GPRS();
	return allocated == 0 ? LATTE_NUM_GPR : std::min<uint32>(allocated, LATTE_NUM_GPR);
}
}

bool LatteDecompiler_HLSLGeometryShaderExpandsPoints(const uint32* contextRegisters)
{
	return contextRegisters[mmVGT_GS_OUT_PRIM_TYPE] == 0 && contextRegisters[mmVGT_STRMOUT_EN] == 0;
}

bool LatteDecompiler_HLSLTextureUsesComparisonSampler(const LatteDecompilerShader* shader, int32_t textureUnit)
{
#if defined(CEMU_UWP)
	return shader->textureUsesDepthCompare[textureUnit] && shader->textureUnitDim[textureUnit] != Latte::E_DIM::DIM_3D;
#else
	return false;
#endif
}

void LatteDecompiler_emitHLSLShader(LatteDecompilerShaderContext* shaderContext, LatteDecompilerShader* shader)
{
#if defined(CEMU_UWP)
	StringBuf* src = new StringBuf(256 * 1024);
#else
	StringBuf* src = new StringBuf(1024 * 1024 * 4);
#endif
	shaderContext->shaderSource = src;
#if defined(CEMU_UWP)
	shaderContext->passCubeCompareReference = true;
#endif
	// streamout reads its source register by name, so it has to be declared and covered by the write switch
	MarkStreamoutSourceRegistersUsed(shaderContext, shaderContext->cfInstructions);
	for (const auto& subroutineInfo : shaderContext->list_subroutines)
		MarkStreamoutSourceRegistersUsed(shaderContext, subroutineInfo.instructions);
	// the console driver hangs on an indexed register array or loses what is stored in it, so relative writes become
	// selects over register variables; bounding them by the allocated registers keeps the count small
	shaderContext->indexedGPRLimit = GetIndexedGPRLimit(shaderContext);
	constexpr uint32 kMaxIndexedGPRWriteSelects = 16384;
	if (shaderContext->analyzer.usesRelativeGPRWrite)
	{
		const uint32 indexedWriteSelects = CountIndexedGPRWriteSelects(shaderContext);
		if (indexedWriteSelects <= kMaxIndexedGPRWriteSelects)
		{
			shaderContext->typeTracker.useArrayGPRs = false;
			shaderContext->lowerIndexedGPRWrites = true;
		}
		else
		{
			cemuLog_log(LogType::Force, "HLSL shader {:016x} keeps the register array: {} indexed write selects over {} registers",
				shaderContext->shaderBaseHash, indexedWriteSelects, shaderContext->indexedGPRLimit);
		}
	}

	src->addFmt("// shader {:016x}" HLSL_NL, shaderContext->shaderBaseHash);
	src->addFmt("// HLSL emitter version {}" HLSL_NL, LATTE_DECOMPILER_HLSL_EMITTER_VERSION);
	src->addFmt("// Direct3D 12 {} shader emitter." HLSL_NL, GetShaderTypeName(shader->shaderType));
	src->add("// Native HLSL owns CF/export/streamout/GS plumbing; ALU/TEX bodies use shared Latte helper lowering." HLSL_NL);
	EmitBindingSummary(shaderContext, src);
	src->add(HLSL_NL);
	EmitCompatHelpers(shaderContext, src);
	EmitResourceDeclarations(shaderContext, src);
	src->add(HLSL_NL);

	switch (shader->shaderType)
	{
	case LatteConst::ShaderType::Vertex:
		EmitVertexShader(shaderContext, src);
		break;
	case LatteConst::ShaderType::Pixel:
		EmitPixelShader(shaderContext, src);
		break;
	case LatteConst::ShaderType::Geometry:
		EmitGeometryShader(shaderContext, src);
		break;
	default:
		shader->hasError = true;
		break;
	}

	cemuLog_logOnce(LogType::Force, "D3D12: HLSL game shader emission uses native CF/export/streamout plumbing with shared ALU/TEX lowering.");
	src->shrink_to_fit();
	shader->strBuf_shaderSource = src;
}

namespace
{
std::string StripGLSLComments(const std::string& src)
{
	std::string out;
	out.reserve(src.size());
	for (size_t i = 0; i < src.size(); i++)
	{
		if (src[i] == '/' && i + 1 < src.size() && src[i + 1] == '/')
		{
			while (i < src.size() && src[i] != '\n')
				i++;
			if (i < src.size())
				out.push_back('\n');
			continue;
		}
		if (src[i] == '/' && i + 1 < src.size() && src[i + 1] == '*')
		{
			i += 2;
			while (i + 1 < src.size() && !(src[i] == '*' && src[i + 1] == '/'))
			{
				if (src[i] == '\n')
					out.push_back('\n');
				i++;
			}
			i++;
			continue;
		}
		out.push_back(src[i]);
	}
	return out;
}

std::string TrimString(const std::string& s)
{
	const size_t begin = s.find_first_not_of(" \t\r\n");
	if (begin == std::string::npos)
		return {};
	const size_t end = s.find_last_not_of(" \t\r\n");
	return s.substr(begin, end - begin + 1);
}

std::string NormalizeParameterTypes(const std::string& params)
{
	static const std::regex s_qualifiers(R"(\b(in|out|inout|const|highp|mediump|lowp)\s+)");
	static const std::regex s_vecType(R"((b|i|u)?vec([234]))");
	std::string types;
	size_t start = 0;
	while (start <= params.size())
	{
		size_t comma = params.find(',', start);
		if (comma == std::string::npos)
			comma = params.size();
		std::string p = std::regex_replace(TrimString(params.substr(start, comma - start)), s_qualifiers, "");
		const size_t lastSpace = p.find_last_of(" \t");
		std::string type = lastSpace == std::string::npos ? p : TrimString(p.substr(0, lastSpace));
		std::smatch m;
		if (std::regex_match(type, m, s_vecType))
		{
			const std::string prefix = m[1].str();
			type = (prefix == "b" ? "bool" : prefix == "i" ? "int" : prefix == "u" ? "uint" : "float") + m[2].str();
		}
		types += type + ",";
		start = comma + 1;
	}
	return types;
}

bool HLSLDefinesFunction(const std::string& hlsl, const std::string& name, const std::string& normalizedParams)
{
	const std::regex definition("(^|\\n)[ \\t]*(?:inline[ \\t]+)?[A-Za-z_][A-Za-z0-9_]*[ \\t]+" + name + "[ \\t]*\\(([^;{)]*)\\)[ \\t\\r\\n]*\\{");
	for (auto it = std::sregex_iterator(hlsl.begin(), hlsl.end(), definition); it != std::sregex_iterator(); ++it)
	{
		if (NormalizeParameterTypes((*it)[2].str()) == normalizedParams)
			return true;
	}
	return false;
}

struct GLSLInterfaceVariable
{
	uint32 location;
	std::string type;
	std::string name;
};
}

// replacement pixel shaders are edited copies of the GLSL generated for the same shader, so the declarations,
// bindings and IO layout of the generated HLSL are kept and only the edited code is transplanted
bool LatteDecompiler_ConvertGLSLPixelShaderToHLSL(const std::string& generatedHLSL, const std::string& glslSource, std::string& hlslOut)
{
	static const std::string kInterfaceEndMarker = "// end of shader inputs/outputs";
	const size_t markerPos = glslSource.find(kInterfaceEndMarker);
	const size_t mainPos = generatedHLSL.find("\nPSOut main(");
	if (markerPos == std::string::npos || mainPos == std::string::npos)
		return false;
	const size_t signatureEnd = generatedHLSL.find('\n', mainPos + 1);
	if (signatureEnd == std::string::npos)
		return false;
	const std::string prefix = generatedHLSL.substr(0, mainPos + 1);
	const std::string signature = TrimString(generatedHLSL.substr(mainPos + 1, signatureEnd - mainPos - 1));

	// generated PS interface: the TEXCOORD index is the input location, SV_TargetN the output location
	std::map<uint32, std::string> inputFields;
	std::map<uint32, std::pair<std::string, std::string>> outputFields;
	std::string depthField;
	const std::regex fieldRegex(R"(\n[ \t]*(?:(?:nointerpolation|noperspective|centroid|sample|linear)[ \t]+)*([A-Za-z0-9_]+)[ \t]+([A-Za-z0-9_]+)[ \t]*:[ \t]*([A-Za-z_]+?)([0-9]*)[ \t]*;)");
	for (auto it = std::sregex_iterator(prefix.begin(), prefix.end(), fieldRegex); it != std::sregex_iterator(); ++it)
	{
		const std::string semantic = (*it)[3].str();
		const uint32 index = (*it)[4].str().empty() ? 0 : (uint32)std::stoul((*it)[4].str());
		if (semantic == "TEXCOORD")
			inputFields[index] = (*it)[2].str();
		else if (semantic == "SV_Target")
			outputFields[index] = { (*it)[1].str(), (*it)[2].str() };
		else if (semantic == "SV_Depth")
			depthField = (*it)[2].str();
	}

	const std::string header = StripGLSLComments(glslSource.substr(0, markerPos));
	std::vector<GLSLInterfaceVariable> inputs;
	std::vector<GLSLInterfaceVariable> outputs;
	const std::regex ioRegex(R"((?:layout[ \t]*\([ \t]*location[ \t]*=[ \t]*([0-9]+)[ \t]*\)|ATTR_LAYOUT[ \t]*\([ \t]*[0-9]+[ \t]*,[ \t]*([0-9]+)[ \t]*\))[ \t]*(?:(?:flat|noperspective|centroid|smooth)[ \t]+)*(in|out)[ \t]+([A-Za-z0-9_]+)[ \t]+([A-Za-z0-9_]+)[ \t]*;)");
	for (auto it = std::sregex_iterator(header.begin(), header.end(), ioRegex); it != std::sregex_iterator(); ++it)
	{
		const std::string locationText = (*it)[1].matched ? (*it)[1].str() : (*it)[2].str();
		GLSLInterfaceVariable variable{ (uint32)std::stoul(locationText), (*it)[4].str(), (*it)[5].str() };
		((*it)[3].str() == "in" ? inputs : outputs).push_back(variable);
	}
	if (outputs.empty())
		return false;

	// split the edited code into top-level items and drop helpers the generated HLSL already defines
	const std::string body = StripGLSLComments(glslSource.substr(markerPos + kInterfaceEndMarker.size()));
	static const std::regex s_functionHeader(R"([A-Za-z_][A-Za-z0-9_]*[ \t\r\n]+([A-Za-z_][A-Za-z0-9_]*)[ \t\r\n]*\(([^)]*)\))");
	static const std::regex s_prototype(R"([A-Za-z_][A-Za-z0-9_]*[ \t\r\n]+[A-Za-z_][A-Za-z0-9_]*[ \t\r\n]*\([^=]*\))");
	std::string convertedBody;
	bool hasMain = false;
	size_t pos = 0;
	while (pos < body.size())
	{
		const size_t itemStart = body.find_first_not_of(" \t\r\n", pos);
		if (itemStart == std::string::npos)
			break;
		if (body[itemStart] == '#')
		{
			const size_t lineEnd = body.find('\n', itemStart);
			convertedBody += body.substr(itemStart, lineEnd == std::string::npos ? std::string::npos : lineEnd - itemStart) + "\n";
			pos = lineEnd == std::string::npos ? body.size() : lineEnd + 1;
			continue;
		}
		const size_t stop = body.find_first_of(";{", itemStart);
		if (stop == std::string::npos)
			return false;
		const std::string head = TrimString(body.substr(itemStart, stop - itemStart));
		if (body[stop] == ';')
		{
			// globals are implicit uniforms in HLSL unless static
			if (head.rfind("uniform", 0) != 0)
			{
				const bool keepAsIs = head.rfind("static", 0) == 0 || std::regex_match(head, s_prototype);
				convertedBody += (keepAsIs ? head : "static " + head) + ";\n";
			}
			pos = stop + 1;
			continue;
		}
		size_t depth = 1;
		size_t end = stop + 1;
		while (end < body.size() && depth > 0)
		{
			if (body[end] == '{')
				depth++;
			else if (body[end] == '}')
				depth--;
			end++;
		}
		if (depth != 0)
			return false;
		const std::string block = body.substr(stop, end - stop);
		if (head.rfind("struct", 0) == 0)
		{
			const size_t semicolon = body.find(';', end);
			convertedBody += head + " " + block + ";\n";
			pos = semicolon == std::string::npos ? end : semicolon + 1;
			continue;
		}
		std::smatch m;
		if (!std::regex_match(head, m, s_functionHeader))
			return false;
		const std::string name = m[1].str();
		if (name == "main")
		{
			hasMain = true;
			convertedBody += "void latteCustomShaderMain()\n" + block + "\n";
		}
		else if (!HLSLDefinesFunction(prefix, name, NormalizeParameterTypes(m[2].str())))
			convertedBody += head + "\n" + block + "\n";
		pos = end;
	}
	if (!hasMain)
		return false;

	std::string out;
	out.reserve(prefix.size() + convertedBody.size() + 8192);
	out += prefix;
	out += "float2 mix(float2 a, float2 b, float t) { return lerp(a, b, t); }\n";
	out += "float3 mix(float3 a, float3 b, float t) { return lerp(a, b, t); }\n";
	out += "float4 mix(float4 a, float4 b, float t) { return lerp(a, b, t); }\n";
	for (const char* type : { "float", "float2", "float3", "float4" })
		out += fmt::format("{0} mod({0} a, {0} b) {{ return a - b * floor(a / b); }}\n", type);
	for (const char* type : { "float2", "float3", "float4" })
		out += fmt::format("{0} mod({0} a, float b) {{ return a - b * floor(a / b); }}\n", type);
	const std::pair<const char*, const char*> comparisons[] = { { "lessThan", "<" }, { "lessThanEqual", "<=" }, { "greaterThan", ">" }, { "greaterThanEqual", ">=" }, { "equal", "==" }, { "notEqual", "!=" } };
	for (const auto& comparison : comparisons)
		for (const char* base : { "float", "int", "uint" })
			for (int n = 2; n <= 4; n++)
				out += fmt::format("bool{2} {0}({1}{2} a, {1}{2} b) {{ return a {3} b; }}\n", comparison.first, base, n, comparison.second);
	out += "#define LATTE_ATAN_SELECT(_1, _2, NAME, ...) NAME\n";
	out += "#define atan(...) LATTE_ATAN_SELECT(__VA_ARGS__, atan2, atan)(__VA_ARGS__)\n";
	out += "#define texture2D texture\n";
	out += "static float4 gl_FragCoord;\nstatic bool gl_FrontFacing;\nstatic float4 gl_PointCoord;\nstatic float gl_FragDepth;\n";
	for (const auto& v : inputs)
		out += fmt::format("static {} {};\n", v.type, v.name);
	for (const auto& v : outputs)
		out += fmt::format("static {} {};\n", v.type, v.name);
	out += convertedBody;
	out += signature + "\n{\n";
	out += "\tPSOut output = (PSOut)0;\n";
	out += "\tgl_FragCoord = input.position;\n";
	out += signature.find("isFrontFace") != std::string::npos ? "\tgl_FrontFacing = isFrontFace;\n" : "\tgl_FrontFacing = true;\n";
	out += "\tgl_FragDepth = input.position.z;\n";
	for (const auto& v : inputs)
	{
		auto field = inputFields.find(v.location);
		if (field != inputFields.end())
			out += fmt::format("\t{} = input.{};\n", v.name, field->second);
		else
			out += fmt::format("\t{} = ({})0;\n", v.name, v.type);
	}
	out += "\tlatteCustomShaderMain();\n";
	for (const auto& v : outputs)
	{
		auto field = outputFields.find(v.location);
		if (field == outputFields.end())
			continue;
		const std::string& fieldType = field->second.first;
		const bool packIsInteger = v.type.rfind("ivec", 0) == 0 || v.type.rfind("uvec", 0) == 0 || v.type == "int" || v.type == "uint";
		if (packIsInteger && fieldType.rfind("uint", 0) == 0)
			out += fmt::format("\toutput.{} = asuint({});\n", field->second.second, v.name);
		else if (packIsInteger && fieldType.rfind("int", 0) == 0)
			out += fmt::format("\toutput.{} = asint({});\n", field->second.second, v.name);
		else
			out += fmt::format("\toutput.{} = ({}){};\n", field->second.second, fieldType, v.name);
	}
	if (!depthField.empty())
		out += fmt::format("\toutput.{} = gl_FragDepth;\n", depthField);
	out += "\treturn output;\n}\n";
	hlslOut = std::move(out);
	return true;
}
