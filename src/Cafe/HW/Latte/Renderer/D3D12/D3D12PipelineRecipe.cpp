#include "Cafe/HW/Latte/Renderer/D3D12/D3D12PipelineRecipe.h"

#include <openssl/sha.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <new>
#include <utility>

namespace
{
using D3D12PipelineRecipe::Fingerprint;
using D3D12PipelineRecipe::Status;

constexpr std::array<std::uint8_t, 8> kMagic{ 'C', '1', '2', 'R', 'E', 'C', 'P', '1' };
constexpr std::size_t kHeaderBytes = 64;
constexpr std::size_t kMaxFixedPayloadBytes = 512;

class Writer
{
public:
	explicit Writer(std::vector<std::uint8_t>& bytes) : m_bytes(bytes) {}

	void U8(std::uint8_t value)
	{
		m_bytes.emplace_back(value);
	}

	void U32(std::uint32_t value)
	{
		for (unsigned shift = 0; shift < 32; shift += 8)
			U8(static_cast<std::uint8_t>(value >> shift));
	}

	void I32(std::int32_t value)
	{
		U32(std::bit_cast<std::uint32_t>(value));
	}

	void U64(std::uint64_t value)
	{
		for (unsigned shift = 0; shift < 64; shift += 8)
			U8(static_cast<std::uint8_t>(value >> shift));
	}

	void F32(float value)
	{
		U32(std::bit_cast<std::uint32_t>(value));
	}

	void Bytes(const void* data, std::size_t size)
	{
		if (size == 0)
			return;
		const auto* begin = static_cast<const std::uint8_t*>(data);
		m_bytes.insert(m_bytes.end(), begin, begin + size);
	}

private:
	std::vector<std::uint8_t>& m_bytes;
};

class Reader
{
public:
	explicit Reader(std::span<const std::uint8_t> bytes) : m_bytes(bytes) {}

	bool U8(std::uint8_t& value)
	{
		if (m_position >= m_bytes.size())
			return false;
		value = m_bytes[m_position++];
		return true;
	}

	bool U32(std::uint32_t& value)
	{
		value = 0;
		for (unsigned shift = 0; shift < 32; shift += 8)
		{
			std::uint8_t byte{};
			if (!U8(byte))
				return false;
			value |= static_cast<std::uint32_t>(byte) << shift;
		}
		return true;
	}

	bool I32(std::int32_t& value)
	{
		std::uint32_t bits{};
		if (!U32(bits))
			return false;
		value = std::bit_cast<std::int32_t>(bits);
		return true;
	}

	bool U64(std::uint64_t& value)
	{
		value = 0;
		for (unsigned shift = 0; shift < 64; shift += 8)
		{
			std::uint8_t byte{};
			if (!U8(byte))
				return false;
			value |= static_cast<std::uint64_t>(byte) << shift;
		}
		return true;
	}

	bool F32(float& value)
	{
		std::uint32_t bits{};
		if (!U32(bits))
			return false;
		value = std::bit_cast<float>(bits);
		return true;
	}

	bool Bytes(void* output, std::size_t size)
	{
		if (size > Remaining())
			return false;
		if (size != 0)
			std::memcpy(output, m_bytes.data() + m_position, size);
		m_position += size;
		return true;
	}

	bool Vector(std::vector<std::uint8_t>& output, std::size_t size)
	{
		if (size > Remaining())
			return false;
		output.assign(m_bytes.begin() + m_position, m_bytes.begin() + m_position + size);
		m_position += size;
		return true;
	}

	std::size_t Position() const noexcept { return m_position; }
	std::size_t Remaining() const noexcept { return m_bytes.size() - m_position; }
	bool AtEnd() const noexcept { return m_position == m_bytes.size(); }

private:
	std::span<const std::uint8_t> m_bytes;
	std::size_t m_position{};
};

bool ComputeSha256(std::span<const std::uint8_t> bytes, Fingerprint& output) noexcept
{
	return SHA256(bytes.data(), bytes.size(), output.data()) != nullptr;
}

bool IsCanonicalBool(BOOL value) noexcept
{
	return value == FALSE || value == TRUE;
}

bool IsValidBlend(D3D12_BLEND value) noexcept
{
	switch (value)
	{
	case D3D12_BLEND_ZERO:
	case D3D12_BLEND_ONE:
	case D3D12_BLEND_SRC_COLOR:
	case D3D12_BLEND_INV_SRC_COLOR:
	case D3D12_BLEND_SRC_ALPHA:
	case D3D12_BLEND_INV_SRC_ALPHA:
	case D3D12_BLEND_DEST_ALPHA:
	case D3D12_BLEND_INV_DEST_ALPHA:
	case D3D12_BLEND_DEST_COLOR:
	case D3D12_BLEND_INV_DEST_COLOR:
	case D3D12_BLEND_SRC_ALPHA_SAT:
	case D3D12_BLEND_BLEND_FACTOR:
	case D3D12_BLEND_INV_BLEND_FACTOR:
	case D3D12_BLEND_SRC1_COLOR:
	case D3D12_BLEND_INV_SRC1_COLOR:
	case D3D12_BLEND_SRC1_ALPHA:
	case D3D12_BLEND_INV_SRC1_ALPHA:
	// ALPHA_FACTOR / INV_ALPHA_FACTOR, missing from older SDK headers
	case static_cast<D3D12_BLEND>(20):
	case static_cast<D3D12_BLEND>(21):
		return true;
	default:
		return false;
	}
}

bool IsValidPipelineFlags(D3D12_PIPELINE_STATE_FLAGS flags) noexcept
{
#if defined(__ID3D12GraphicsCommandList9_INTERFACE_DEFINED__)
	return (flags & ~D3D12_PIPELINE_STATE_FLAG_DYNAMIC_DEPTH_BIAS) == D3D12_PIPELINE_STATE_FLAG_NONE;
#else
	return flags == D3D12_PIPELINE_STATE_FLAG_NONE;
#endif
}

bool IsValidBlendOp(D3D12_BLEND_OP value) noexcept
{
	return value >= D3D12_BLEND_OP_ADD && value <= D3D12_BLEND_OP_MAX;
}

bool IsValidLogicOp(D3D12_LOGIC_OP value) noexcept
{
	return value >= D3D12_LOGIC_OP_CLEAR && value <= D3D12_LOGIC_OP_OR_INVERTED;
}

bool IsValidFillMode(D3D12_FILL_MODE value) noexcept
{
	return value == D3D12_FILL_MODE_WIREFRAME || value == D3D12_FILL_MODE_SOLID;
}

bool IsValidCullMode(D3D12_CULL_MODE value) noexcept
{
	return value >= D3D12_CULL_MODE_NONE && value <= D3D12_CULL_MODE_BACK;
}

bool IsValidComparison(D3D12_COMPARISON_FUNC value) noexcept
{
	return value >= D3D12_COMPARISON_FUNC_NEVER && value <= D3D12_COMPARISON_FUNC_ALWAYS;
}

bool IsValidStencilOp(D3D12_STENCIL_OP value) noexcept
{
	return value >= D3D12_STENCIL_OP_KEEP && value <= D3D12_STENCIL_OP_DECR;
}

bool IsValidStencilFace(const D3D12_DEPTH_STENCILOP_DESC& face) noexcept
{
	return IsValidStencilOp(face.StencilFailOp) &&
		IsValidStencilOp(face.StencilDepthFailOp) &&
		IsValidStencilOp(face.StencilPassOp) &&
		IsValidComparison(face.StencilFunc);
}

#if defined(__ID3D12GraphicsCommandList8_INTERFACE_DEFINED__)
bool IsValidStencilFace(const D3D12_DEPTH_STENCILOP_DESC1& face) noexcept
{
	return IsValidStencilOp(face.StencilFailOp) &&
		IsValidStencilOp(face.StencilDepthFailOp) &&
		IsValidStencilOp(face.StencilPassOp) &&
		IsValidComparison(face.StencilFunc);
}
#endif

bool IsValidRenderTargetFormat(DXGI_FORMAT format) noexcept
{
	switch (format)
	{
	case DXGI_FORMAT_UNKNOWN:
	case DXGI_FORMAT_R32G32B32A32_FLOAT:
	case DXGI_FORMAT_R32G32B32A32_UINT:
	case DXGI_FORMAT_R32G32B32A32_SINT:
	case DXGI_FORMAT_R16G16B16A16_FLOAT:
	case DXGI_FORMAT_R16G16B16A16_UNORM:
	case DXGI_FORMAT_R16G16B16A16_UINT:
	case DXGI_FORMAT_R16G16B16A16_SNORM:
	case DXGI_FORMAT_R16G16B16A16_SINT:
	case DXGI_FORMAT_R32G32_FLOAT:
	case DXGI_FORMAT_R32G32_UINT:
	case DXGI_FORMAT_R32G32_SINT:
	case DXGI_FORMAT_R10G10B10A2_UNORM:
	case DXGI_FORMAT_R10G10B10A2_UINT:
	case DXGI_FORMAT_R11G11B10_FLOAT:
	case DXGI_FORMAT_R8G8B8A8_UNORM:
	case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
	case DXGI_FORMAT_R8G8B8A8_UINT:
	case DXGI_FORMAT_R8G8B8A8_SNORM:
	case DXGI_FORMAT_R8G8B8A8_SINT:
	case DXGI_FORMAT_R16G16_FLOAT:
	case DXGI_FORMAT_R16G16_UNORM:
	case DXGI_FORMAT_R16G16_UINT:
	case DXGI_FORMAT_R16G16_SNORM:
	case DXGI_FORMAT_R16G16_SINT:
	case DXGI_FORMAT_R32_FLOAT:
	case DXGI_FORMAT_R32_UINT:
	case DXGI_FORMAT_R32_SINT:
	case DXGI_FORMAT_R8G8_UNORM:
	case DXGI_FORMAT_R8G8_UINT:
	case DXGI_FORMAT_R8G8_SNORM:
	case DXGI_FORMAT_R8G8_SINT:
	case DXGI_FORMAT_R16_FLOAT:
	case DXGI_FORMAT_R16_UNORM:
	case DXGI_FORMAT_R16_UINT:
	case DXGI_FORMAT_R16_SNORM:
	case DXGI_FORMAT_R16_SINT:
	case DXGI_FORMAT_R8_UNORM:
	case DXGI_FORMAT_R8_UINT:
	case DXGI_FORMAT_R8_SNORM:
	case DXGI_FORMAT_R8_SINT:
	case DXGI_FORMAT_A8_UNORM:
	case DXGI_FORMAT_B5G6R5_UNORM:
	case DXGI_FORMAT_B5G5R5A1_UNORM:
	case DXGI_FORMAT_B8G8R8A8_UNORM:
	case DXGI_FORMAT_B8G8R8X8_UNORM:
	case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
	case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
	case DXGI_FORMAT_B4G4R4A4_UNORM:
		return true;
	default:
		return false;
	}
}

bool IsValidDepthStencilFormat(DXGI_FORMAT format) noexcept
{
	switch (format)
	{
	case DXGI_FORMAT_UNKNOWN:
	case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
	case DXGI_FORMAT_D32_FLOAT:
	case DXGI_FORMAT_D24_UNORM_S8_UINT:
	case DXGI_FORMAT_D16_UNORM:
		return true;
	default:
		return false;
	}
}

bool IsValidShaderBytecode(const D3D12_SHADER_BYTECODE& bytecode, bool required) noexcept
{
	if (bytecode.BytecodeLength > D3D12PipelineRecipe::kMaxShaderStageBytes)
		return false;
	if ((bytecode.BytecodeLength == 0) != (bytecode.pShaderBytecode == nullptr))
		return false;
	return !required || bytecode.BytecodeLength != 0;
}

Status ValidateBaseDescriptor(const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc, bool requireRootSignature) noexcept
{
	if (requireRootSignature && !desc.pRootSignature)
		return Status::InvalidArgument;
	if (desc.HS.pShaderBytecode || desc.HS.BytecodeLength ||
		desc.DS.pShaderBytecode || desc.DS.BytecodeLength ||
		desc.StreamOutput.pSODeclaration || desc.StreamOutput.NumEntries ||
		desc.StreamOutput.pBufferStrides || desc.StreamOutput.NumStrides ||
		desc.StreamOutput.RasterizedStream ||
		desc.InputLayout.pInputElementDescs || desc.InputLayout.NumElements ||
		desc.CachedPSO.pCachedBlob || desc.CachedPSO.CachedBlobSizeInBytes)
	{
		return Status::UnsupportedFeature;
	}
	if (!IsValidShaderBytecode(desc.VS, true) ||
		!IsValidShaderBytecode(desc.GS, false) ||
		!IsValidShaderBytecode(desc.PS, false))
	{
		return Status::InvalidField;
	}
	if (!IsCanonicalBool(desc.BlendState.AlphaToCoverageEnable) ||
		!IsCanonicalBool(desc.BlendState.IndependentBlendEnable))
	{
		return Status::InvalidField;
	}

	bool anyLogicOp = false;
	for (const auto& target : desc.BlendState.RenderTarget)
	{
		if (!IsCanonicalBool(target.BlendEnable) || !IsCanonicalBool(target.LogicOpEnable) ||
			!IsValidBlend(target.SrcBlend) || !IsValidBlend(target.DestBlend) ||
			!IsValidBlendOp(target.BlendOp) || !IsValidBlend(target.SrcBlendAlpha) ||
			!IsValidBlend(target.DestBlendAlpha) || !IsValidBlendOp(target.BlendOpAlpha) ||
			!IsValidLogicOp(target.LogicOp) ||
			(target.RenderTargetWriteMask & ~static_cast<UINT8>(D3D12_COLOR_WRITE_ENABLE_ALL)) != 0 ||
			(target.BlendEnable && target.LogicOpEnable))
		{
			return Status::InvalidField;
		}
		anyLogicOp = anyLogicOp || target.LogicOpEnable;
	}
	if (anyLogicOp && desc.BlendState.IndependentBlendEnable)
		return Status::InvalidField;

	const auto& rasterizer = desc.RasterizerState;
	if (!IsValidFillMode(rasterizer.FillMode) || !IsValidCullMode(rasterizer.CullMode) ||
		!IsCanonicalBool(rasterizer.FrontCounterClockwise) ||
		!std::isfinite(rasterizer.DepthBiasClamp) || !std::isfinite(rasterizer.SlopeScaledDepthBias) ||
		!IsCanonicalBool(rasterizer.DepthClipEnable) || !IsCanonicalBool(rasterizer.MultisampleEnable) ||
		!IsCanonicalBool(rasterizer.AntialiasedLineEnable) ||
		(rasterizer.ForcedSampleCount != 0 && rasterizer.ForcedSampleCount != 1 &&
		 rasterizer.ForcedSampleCount != 4 && rasterizer.ForcedSampleCount != 8 && rasterizer.ForcedSampleCount != 16) ||
		(rasterizer.ConservativeRaster != D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF &&
		 rasterizer.ConservativeRaster != D3D12_CONSERVATIVE_RASTERIZATION_MODE_ON))
	{
		return Status::InvalidField;
	}

	const auto& depthStencil = desc.DepthStencilState;
	if (!IsCanonicalBool(depthStencil.DepthEnable) ||
		(depthStencil.DepthWriteMask != D3D12_DEPTH_WRITE_MASK_ZERO && depthStencil.DepthWriteMask != D3D12_DEPTH_WRITE_MASK_ALL) ||
		!IsValidComparison(depthStencil.DepthFunc) || !IsCanonicalBool(depthStencil.StencilEnable) ||
		!IsValidStencilFace(depthStencil.FrontFace) || !IsValidStencilFace(depthStencil.BackFace))
	{
		return Status::InvalidField;
	}

	if (desc.PrimitiveTopologyType == D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH)
		return Status::UnsupportedFeature;
	if (desc.IBStripCutValue < D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED ||
		desc.IBStripCutValue > D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFFFFFF ||
		desc.PrimitiveTopologyType < D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT ||
		desc.PrimitiveTopologyType > D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE ||
		desc.NumRenderTargets > D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT)
	{
		return Status::InvalidField;
	}
	for (std::size_t index = 0; index < std::size(desc.RTVFormats); ++index)
	{
		const DXGI_FORMAT format = desc.RTVFormats[index];
		if (!IsValidRenderTargetFormat(format) ||
			(index < desc.NumRenderTargets && format == DXGI_FORMAT_UNKNOWN) ||
			(index >= desc.NumRenderTargets && format != DXGI_FORMAT_UNKNOWN))
			return Status::InvalidField;
	}
	if (!IsValidDepthStencilFormat(desc.DSVFormat) ||
		(desc.DSVFormat == DXGI_FORMAT_UNKNOWN &&
		 (desc.DepthStencilState.DepthEnable || desc.DepthStencilState.StencilEnable)) ||
		desc.SampleDesc.Count == 0 || desc.SampleDesc.Count > 32 ||
		(desc.SampleDesc.Count & (desc.SampleDesc.Count - 1)) != 0 ||
		(desc.SampleDesc.Count == 1 && desc.SampleDesc.Quality != 0) ||
		!IsValidPipelineFlags(desc.Flags))
	{
		return Status::InvalidField;
	}
	return Status::Success;
}

#if defined(__ID3D12GraphicsCommandList8_INTERFACE_DEFINED__)
bool SameStencilOperations(const D3D12_DEPTH_STENCILOP_DESC& legacy, const D3D12_DEPTH_STENCILOP_DESC1& independent) noexcept
{
	return legacy.StencilFailOp == independent.StencilFailOp &&
		legacy.StencilDepthFailOp == independent.StencilDepthFailOp &&
		legacy.StencilPassOp == independent.StencilPassOp &&
		legacy.StencilFunc == independent.StencilFunc;
}

Status ValidateIndependentDepthStencil(const D3D12_DEPTH_STENCIL_DESC2& desc,
	const D3D12_DEPTH_STENCIL_DESC& legacy) noexcept
{
	if (!IsCanonicalBool(desc.DepthEnable) ||
		(desc.DepthWriteMask != D3D12_DEPTH_WRITE_MASK_ZERO && desc.DepthWriteMask != D3D12_DEPTH_WRITE_MASK_ALL) ||
		!IsValidComparison(desc.DepthFunc) || !IsCanonicalBool(desc.StencilEnable) ||
		!IsValidStencilFace(desc.FrontFace) || !IsValidStencilFace(desc.BackFace) ||
		!IsCanonicalBool(desc.DepthBoundsTestEnable))
	{
		return Status::InvalidField;
	}
	if (desc.DepthEnable != legacy.DepthEnable || desc.DepthWriteMask != legacy.DepthWriteMask ||
		desc.DepthFunc != legacy.DepthFunc || desc.StencilEnable != legacy.StencilEnable ||
		!SameStencilOperations(legacy.FrontFace, desc.FrontFace) ||
		!SameStencilOperations(legacy.BackFace, desc.BackFace))
	{
		return Status::InvalidField;
	}
	return Status::Success;
}
#endif

void WriteBool(Writer& writer, BOOL value)
{
	writer.U8(value ? 1 : 0);
}

template<typename Face>
void WriteStencilFace(Writer& writer, const Face& face)
{
	writer.U32(static_cast<std::uint32_t>(face.StencilFailOp));
	writer.U32(static_cast<std::uint32_t>(face.StencilDepthFailOp));
	writer.U32(static_cast<std::uint32_t>(face.StencilPassOp));
	writer.U32(static_cast<std::uint32_t>(face.StencilFunc));
}

#if defined(__ID3D12GraphicsCommandList8_INTERFACE_DEFINED__)
void WriteIndependentStencilFace(Writer& writer, const D3D12_DEPTH_STENCILOP_DESC1& face)
{
	WriteStencilFace(writer, face);
	writer.U8(face.StencilReadMask);
	writer.U8(face.StencilWriteMask);
}
#endif

void WriteBaseDescriptor(Writer& writer, const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc)
{
	WriteBool(writer, desc.BlendState.AlphaToCoverageEnable);
	WriteBool(writer, desc.BlendState.IndependentBlendEnable);
	for (const auto& target : desc.BlendState.RenderTarget)
	{
		WriteBool(writer, target.BlendEnable);
		WriteBool(writer, target.LogicOpEnable);
		writer.U32(static_cast<std::uint32_t>(target.SrcBlend));
		writer.U32(static_cast<std::uint32_t>(target.DestBlend));
		writer.U32(static_cast<std::uint32_t>(target.BlendOp));
		writer.U32(static_cast<std::uint32_t>(target.SrcBlendAlpha));
		writer.U32(static_cast<std::uint32_t>(target.DestBlendAlpha));
		writer.U32(static_cast<std::uint32_t>(target.BlendOpAlpha));
		writer.U32(static_cast<std::uint32_t>(target.LogicOp));
		writer.U8(target.RenderTargetWriteMask);
	}
	writer.U32(desc.SampleMask);

	const auto& rasterizer = desc.RasterizerState;
	writer.U32(static_cast<std::uint32_t>(rasterizer.FillMode));
	writer.U32(static_cast<std::uint32_t>(rasterizer.CullMode));
	WriteBool(writer, rasterizer.FrontCounterClockwise);
	writer.I32(rasterizer.DepthBias);
	writer.F32(rasterizer.DepthBiasClamp);
	writer.F32(rasterizer.SlopeScaledDepthBias);
	WriteBool(writer, rasterizer.DepthClipEnable);
	WriteBool(writer, rasterizer.MultisampleEnable);
	WriteBool(writer, rasterizer.AntialiasedLineEnable);
	writer.U32(rasterizer.ForcedSampleCount);
	writer.U32(static_cast<std::uint32_t>(rasterizer.ConservativeRaster));

	const auto& depthStencil = desc.DepthStencilState;
	WriteBool(writer, depthStencil.DepthEnable);
	writer.U32(static_cast<std::uint32_t>(depthStencil.DepthWriteMask));
	writer.U32(static_cast<std::uint32_t>(depthStencil.DepthFunc));
	WriteBool(writer, depthStencil.StencilEnable);
	writer.U8(depthStencil.StencilReadMask);
	writer.U8(depthStencil.StencilWriteMask);
	WriteStencilFace(writer, depthStencil.FrontFace);
	WriteStencilFace(writer, depthStencil.BackFace);

	writer.U32(static_cast<std::uint32_t>(desc.IBStripCutValue));
	writer.U32(static_cast<std::uint32_t>(desc.PrimitiveTopologyType));
	writer.U32(desc.NumRenderTargets);
	for (DXGI_FORMAT format : desc.RTVFormats)
		writer.U32(static_cast<std::uint32_t>(format));
	writer.U32(static_cast<std::uint32_t>(desc.DSVFormat));
	writer.U32(desc.SampleDesc.Count);
	writer.U32(desc.SampleDesc.Quality);
	writer.U32(desc.NodeMask);
	writer.U32(static_cast<std::uint32_t>(desc.Flags));
}

#if defined(__ID3D12GraphicsCommandList8_INTERFACE_DEFINED__)
void WriteIndependentDepthStencil(Writer& writer, const D3D12_DEPTH_STENCIL_DESC2& desc)
{
	WriteBool(writer, desc.DepthEnable);
	writer.U32(static_cast<std::uint32_t>(desc.DepthWriteMask));
	writer.U32(static_cast<std::uint32_t>(desc.DepthFunc));
	WriteBool(writer, desc.StencilEnable);
	WriteIndependentStencilFace(writer, desc.FrontFace);
	WriteIndependentStencilFace(writer, desc.BackFace);
	WriteBool(writer, desc.DepthBoundsTestEnable);
}
#endif

bool ReadBool(Reader& reader, BOOL& value)
{
	std::uint8_t raw{};
	if (!reader.U8(raw))
		return false;
	value = static_cast<BOOL>(raw);
	return true;
}

template<typename Enum>
bool ReadEnum(Reader& reader, Enum& value)
{
	std::uint32_t raw{};
	if (!reader.U32(raw))
		return false;
	value = static_cast<Enum>(raw);
	return true;
}

template<typename Face>
bool ReadStencilFace(Reader& reader, Face& face)
{
	return ReadEnum(reader, face.StencilFailOp) &&
		ReadEnum(reader, face.StencilDepthFailOp) &&
		ReadEnum(reader, face.StencilPassOp) &&
		ReadEnum(reader, face.StencilFunc);
}

#if defined(__ID3D12GraphicsCommandList8_INTERFACE_DEFINED__)
bool ReadIndependentStencilFace(Reader& reader, D3D12_DEPTH_STENCILOP_DESC1& face)
{
	return ReadStencilFace(reader, face) &&
		reader.U8(face.StencilReadMask) && reader.U8(face.StencilWriteMask);
}
#endif

bool ReadBaseDescriptor(Reader& reader, D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc)
{
	if (!ReadBool(reader, desc.BlendState.AlphaToCoverageEnable) ||
		!ReadBool(reader, desc.BlendState.IndependentBlendEnable))
	{
		return false;
	}
	for (auto& target : desc.BlendState.RenderTarget)
	{
		if (!ReadBool(reader, target.BlendEnable) || !ReadBool(reader, target.LogicOpEnable) ||
			!ReadEnum(reader, target.SrcBlend) || !ReadEnum(reader, target.DestBlend) ||
			!ReadEnum(reader, target.BlendOp) || !ReadEnum(reader, target.SrcBlendAlpha) ||
			!ReadEnum(reader, target.DestBlendAlpha) || !ReadEnum(reader, target.BlendOpAlpha) ||
			!ReadEnum(reader, target.LogicOp) || !reader.U8(target.RenderTargetWriteMask))
		{
			return false;
		}
	}
	if (!reader.U32(desc.SampleMask))
		return false;

	auto& rasterizer = desc.RasterizerState;
	if (!ReadEnum(reader, rasterizer.FillMode) || !ReadEnum(reader, rasterizer.CullMode) ||
		!ReadBool(reader, rasterizer.FrontCounterClockwise) || !reader.I32(rasterizer.DepthBias) ||
		!reader.F32(rasterizer.DepthBiasClamp) || !reader.F32(rasterizer.SlopeScaledDepthBias) ||
		!ReadBool(reader, rasterizer.DepthClipEnable) || !ReadBool(reader, rasterizer.MultisampleEnable) ||
		!ReadBool(reader, rasterizer.AntialiasedLineEnable) || !reader.U32(rasterizer.ForcedSampleCount) ||
		!ReadEnum(reader, rasterizer.ConservativeRaster))
	{
		return false;
	}

	auto& depthStencil = desc.DepthStencilState;
	if (!ReadBool(reader, depthStencil.DepthEnable) || !ReadEnum(reader, depthStencil.DepthWriteMask) ||
		!ReadEnum(reader, depthStencil.DepthFunc) || !ReadBool(reader, depthStencil.StencilEnable) ||
		!reader.U8(depthStencil.StencilReadMask) || !reader.U8(depthStencil.StencilWriteMask) ||
		!ReadStencilFace(reader, depthStencil.FrontFace) || !ReadStencilFace(reader, depthStencil.BackFace) ||
		!ReadEnum(reader, desc.IBStripCutValue) || !ReadEnum(reader, desc.PrimitiveTopologyType) ||
		!reader.U32(desc.NumRenderTargets))
	{
		return false;
	}
	for (auto& format : desc.RTVFormats)
	{
		if (!ReadEnum(reader, format))
			return false;
	}
	return ReadEnum(reader, desc.DSVFormat) &&
		reader.U32(desc.SampleDesc.Count) && reader.U32(desc.SampleDesc.Quality) &&
		reader.U32(desc.NodeMask) && ReadEnum(reader, desc.Flags);
}

#if defined(__ID3D12GraphicsCommandList8_INTERFACE_DEFINED__)
bool ReadIndependentDepthStencil(Reader& reader, D3D12_DEPTH_STENCIL_DESC2& desc)
{
	return ReadBool(reader, desc.DepthEnable) &&
		ReadEnum(reader, desc.DepthWriteMask) && ReadEnum(reader, desc.DepthFunc) &&
		ReadBool(reader, desc.StencilEnable) &&
		ReadIndependentStencilFace(reader, desc.FrontFace) &&
		ReadIndependentStencilFace(reader, desc.BackFace) &&
		ReadBool(reader, desc.DepthBoundsTestEnable);
}
#endif

void WriteShader(Writer& writer, const D3D12_SHADER_BYTECODE& shader)
{
	writer.U32(static_cast<std::uint32_t>(shader.BytecodeLength));
	writer.Bytes(shader.pShaderBytecode, shader.BytecodeLength);
}

Status ReadShader(Reader& reader, std::vector<std::uint8_t>& shader)
{
	std::uint32_t size{};
	if (!reader.U32(size))
		return Status::MalformedRecord;
	if (size > D3D12PipelineRecipe::kMaxShaderStageBytes)
		return Status::SizeLimitExceeded;
	return reader.Vector(shader, size) ? Status::Success : Status::MalformedRecord;
}
}

namespace D3D12PipelineRecipe
{
const char* StatusName(Status status) noexcept
{
	switch (status)
	{
	case Status::Success: return "success";
	case Status::InvalidArgument: return "invalid argument";
	case Status::UnsupportedFeature: return "unsupported feature";
	case Status::SizeLimitExceeded: return "size limit exceeded";
	case Status::OutOfMemory: return "out of memory";
	case Status::InvalidMagic: return "invalid magic";
	case Status::VersionMismatch: return "version mismatch";
	case Status::SchemaMismatch: return "schema mismatch";
	case Status::MalformedRecord: return "malformed record";
	case Status::ChecksumMismatch: return "checksum mismatch";
	case Status::InvalidField: return "invalid field";
	default: return "unknown";
	}
}

OwnedRecipe::OwnedRecipe(OwnedRecipe&& other) noexcept
{
	*this = std::move(other);
}

OwnedRecipe& OwnedRecipe::operator=(OwnedRecipe&& other) noexcept
{
	if (this == &other)
		return *this;
	pipelineKey = other.pipelineKey;
	desc = other.desc;
	vertexShader = std::move(other.vertexShader);
	geometryShader = std::move(other.geometryShader);
	pixelShader = std::move(other.pixelShader);
	fingerprint = other.fingerprint;
	contentFingerprint = other.contentFingerprint;
#if defined(__ID3D12GraphicsCommandList8_INTERFACE_DEFINED__)
	hasIndependentDepthStencil = other.hasIndependentDepthStencil;
	independentDepthStencil = other.independentDepthStencil;
#endif
	RebindShaderBytecode();
	other.pipelineKey = 0;
	other.desc = {};
	other.fingerprint = {};
	other.contentFingerprint = {};
#if defined(__ID3D12GraphicsCommandList8_INTERFACE_DEFINED__)
	other.hasIndependentDepthStencil = false;
	other.independentDepthStencil = {};
#endif
	return *this;
}

void OwnedRecipe::BindRootSignature(ID3D12RootSignature* rootSignature) noexcept
{
	desc.pRootSignature = rootSignature;
}

void OwnedRecipe::RebindShaderBytecode() noexcept
{
	desc.VS = vertexShader.empty() ? D3D12_SHADER_BYTECODE{} :
		D3D12_SHADER_BYTECODE{ vertexShader.data(), vertexShader.size() };
	desc.GS = geometryShader.empty() ? D3D12_SHADER_BYTECODE{} :
		D3D12_SHADER_BYTECODE{ geometryShader.data(), geometryShader.size() };
	desc.PS = pixelShader.empty() ? D3D12_SHADER_BYTECODE{} :
		D3D12_SHADER_BYTECODE{ pixelShader.data(), pixelShader.size() };
}

Status Serialize(std::uint64_t pipelineKey,
	const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc,
	const SerializeOptions& options,
	Serialized& output)
{
	if (desc.VS.BytecodeLength > kMaxShaderStageBytes ||
		desc.GS.BytecodeLength > kMaxShaderStageBytes ||
		desc.PS.BytecodeLength > kMaxShaderStageBytes)
	{
		return Status::SizeLimitExceeded;
	}
	const Status baseStatus = ValidateBaseDescriptor(desc, true);
	if (baseStatus != Status::Success)
		return baseStatus;
#if defined(__ID3D12GraphicsCommandList8_INTERFACE_DEFINED__)
	if (options.independentDepthStencil)
	{
		const Status independentStatus = ValidateIndependentDepthStencil(
			*options.independentDepthStencil, desc.DepthStencilState);
		if (independentStatus != Status::Success)
			return independentStatus;
	}
#endif

	const std::uint64_t shaderBytes = static_cast<std::uint64_t>(desc.VS.BytecodeLength) +
		static_cast<std::uint64_t>(desc.GS.BytecodeLength) + static_cast<std::uint64_t>(desc.PS.BytecodeLength);
	if (shaderBytes > kMaxRecordBytes - kHeaderBytes - kMaxFixedPayloadBytes)
		return Status::SizeLimitExceeded;

	try
	{
		std::vector<std::uint8_t> payload;
		payload.reserve(kMaxFixedPayloadBytes + static_cast<std::size_t>(shaderBytes));
		Writer payloadWriter(payload);
		payloadWriter.U64(pipelineKey);
#if defined(__ID3D12GraphicsCommandList8_INTERFACE_DEFINED__)
		const bool hasIndependentDepthStencil = options.independentDepthStencil != nullptr;
#else
		constexpr bool hasIndependentDepthStencil = false;
#endif
		payloadWriter.U8(hasIndependentDepthStencil ? 1 : 0);
		WriteBaseDescriptor(payloadWriter, desc);
#if defined(__ID3D12GraphicsCommandList8_INTERFACE_DEFINED__)
		if (hasIndependentDepthStencil)
			WriteIndependentDepthStencil(payloadWriter, *options.independentDepthStencil);
#endif
		WriteShader(payloadWriter, desc.VS);
		WriteShader(payloadWriter, desc.GS);
		WriteShader(payloadWriter, desc.PS);
		if (payload.size() > kMaxRecordBytes - kHeaderBytes)
			return Status::SizeLimitExceeded;

		Fingerprint payloadChecksum{};
		if (!ComputeSha256(payload, payloadChecksum))
			return Status::InvalidArgument;
		Fingerprint contentFingerprint{};
		if (!ComputeSha256(std::span<const std::uint8_t>(payload).subspan(sizeof(std::uint64_t)), contentFingerprint))
			return Status::InvalidArgument;

		Serialized built;
		built.bytes.reserve(kHeaderBytes + payload.size());
		Writer recordWriter(built.bytes);
		recordWriter.Bytes(kMagic.data(), kMagic.size());
		recordWriter.U32(kRecordVersion);
		recordWriter.U32(static_cast<std::uint32_t>(kHeaderBytes));
		recordWriter.U32(kPipelineSchemaVersion);
		recordWriter.U32(kGameRootSignatureSchemaVersion);
		recordWriter.U64(payload.size());
		recordWriter.Bytes(payloadChecksum.data(), payloadChecksum.size());
		recordWriter.Bytes(payload.data(), payload.size());
		if (built.bytes.size() > kMaxRecordBytes || !ComputeSha256(built.bytes, built.fingerprint))
			return Status::SizeLimitExceeded;
		built.contentFingerprint = contentFingerprint;
		output = std::move(built);
		return Status::Success;
	}
	catch (const std::bad_alloc&)
	{
		return Status::OutOfMemory;
	}
}

Status Decode(std::span<const std::uint8_t> bytes, OwnedRecipe& output)
{
	if (bytes.size() > kMaxRecordBytes)
		return Status::SizeLimitExceeded;
	if (bytes.size() < kHeaderBytes)
		return Status::MalformedRecord;

	try
	{
		Reader headerReader(bytes.first(kHeaderBytes));
		std::array<std::uint8_t, 8> magic{};
		std::uint32_t recordVersion{};
		std::uint32_t headerBytes{};
		std::uint32_t pipelineSchema{};
		std::uint32_t rootSignatureSchema{};
		std::uint64_t payloadBytes{};
		Fingerprint expectedChecksum{};
		if (!headerReader.Bytes(magic.data(), magic.size()) || !headerReader.U32(recordVersion) ||
			!headerReader.U32(headerBytes) || !headerReader.U32(pipelineSchema) ||
			!headerReader.U32(rootSignatureSchema) || !headerReader.U64(payloadBytes) ||
			!headerReader.Bytes(expectedChecksum.data(), expectedChecksum.size()) || !headerReader.AtEnd())
		{
			return Status::MalformedRecord;
		}
		if (magic != kMagic)
			return Status::InvalidMagic;
		if (recordVersion != kRecordVersion)
			return Status::VersionMismatch;
		if (headerBytes != kHeaderBytes || pipelineSchema != kPipelineSchemaVersion ||
			rootSignatureSchema != kGameRootSignatureSchemaVersion)
		{
			return Status::SchemaMismatch;
		}
		if (payloadBytes > kMaxRecordBytes - kHeaderBytes ||
			payloadBytes != bytes.size() - kHeaderBytes)
		{
			return Status::MalformedRecord;
		}

		const std::span<const std::uint8_t> payload = bytes.subspan(kHeaderBytes, static_cast<std::size_t>(payloadBytes));
		Fingerprint actualChecksum{};
		if (!ComputeSha256(payload, actualChecksum))
			return Status::InvalidArgument;
		if (actualChecksum != expectedChecksum)
			return Status::ChecksumMismatch;

		Reader reader(payload);
		OwnedRecipe decoded;
		std::uint8_t hasIndependentDepthStencil{};
		if (!reader.U64(decoded.pipelineKey) || !reader.U8(hasIndependentDepthStencil))
			return Status::MalformedRecord;
		if (hasIndependentDepthStencil > 1)
			return Status::InvalidField;
		if (!ReadBaseDescriptor(reader, decoded.desc))
			return Status::MalformedRecord;
#if defined(__ID3D12GraphicsCommandList8_INTERFACE_DEFINED__)
		decoded.hasIndependentDepthStencil = hasIndependentDepthStencil != 0;
		if (decoded.hasIndependentDepthStencil && !ReadIndependentDepthStencil(reader, decoded.independentDepthStencil))
			return Status::MalformedRecord;
#else
		if (hasIndependentDepthStencil)
			return Status::UnsupportedFeature;
#endif
		const Status vertexStatus = ReadShader(reader, decoded.vertexShader);
		if (vertexStatus != Status::Success)
			return vertexStatus;
		const Status geometryStatus = ReadShader(reader, decoded.geometryShader);
		if (geometryStatus != Status::Success)
			return geometryStatus;
		const Status pixelStatus = ReadShader(reader, decoded.pixelShader);
		if (pixelStatus != Status::Success)
			return pixelStatus;
		if (!reader.AtEnd())
			return Status::MalformedRecord;
		decoded.RebindShaderBytecode();
		const Status baseStatus = ValidateBaseDescriptor(decoded.desc, false);
		if (baseStatus != Status::Success)
			return baseStatus;
#if defined(__ID3D12GraphicsCommandList8_INTERFACE_DEFINED__)
		if (decoded.hasIndependentDepthStencil)
		{
			const Status independentStatus = ValidateIndependentDepthStencil(
				decoded.independentDepthStencil, decoded.desc.DepthStencilState);
			if (independentStatus != Status::Success)
				return independentStatus;
		}
#endif
		if (!ComputeSha256(bytes, decoded.fingerprint) ||
			!ComputeSha256(payload.subspan(sizeof(std::uint64_t)), decoded.contentFingerprint))
			return Status::InvalidArgument;
		output = std::move(decoded);
		return Status::Success;
	}
	catch (const std::bad_alloc&)
	{
		return Status::OutOfMemory;
	}
}
}
