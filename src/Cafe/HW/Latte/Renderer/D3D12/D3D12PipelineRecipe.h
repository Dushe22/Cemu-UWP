#pragma once

#if BOOST_OS_WINDOWS
#include <d3d12.h>
#endif

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace D3D12PipelineRecipe
{
inline constexpr std::uint32_t kRecordVersion = 1;
inline constexpr std::uint32_t kPipelineSchemaVersion = 1;
inline constexpr std::uint32_t kGameRootSignatureSchemaVersion = 1;
inline constexpr std::size_t kMaxRecordBytes = 16u * 1024u * 1024u;
inline constexpr std::size_t kMaxShaderStageBytes = 8u * 1024u * 1024u;

using Fingerprint = std::array<std::uint8_t, 32>;

enum class Status : std::uint32_t
{
	Success = 0,
	InvalidArgument,
	UnsupportedFeature,
	SizeLimitExceeded,
	OutOfMemory,
	InvalidMagic,
	VersionMismatch,
	SchemaMismatch,
	MalformedRecord,
	ChecksumMismatch,
	InvalidField,
};

const char* StatusName(Status status) noexcept;

struct SerializeOptions
{
#if defined(__ID3D12GraphicsCommandList8_INTERFACE_DEFINED__)
	const D3D12_DEPTH_STENCIL_DESC2* independentDepthStencil{};
#endif
};

struct Serialized
{
	std::vector<std::uint8_t> bytes;
	Fingerprint fingerprint{};
};

struct OwnedRecipe
{
	OwnedRecipe() = default;
	~OwnedRecipe() = default;
	OwnedRecipe(const OwnedRecipe&) = delete;
	OwnedRecipe& operator=(const OwnedRecipe&) = delete;
	OwnedRecipe(OwnedRecipe&& other) noexcept;
	OwnedRecipe& operator=(OwnedRecipe&& other) noexcept;

	// Decode leaves pRootSignature null; bind it right before native PSO creation
	void BindRootSignature(ID3D12RootSignature* rootSignature) noexcept;

	std::uint64_t pipelineKey{};
	D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
	std::vector<std::uint8_t> vertexShader;
	std::vector<std::uint8_t> geometryShader;
	std::vector<std::uint8_t> pixelShader;
	Fingerprint fingerprint{};
#if defined(__ID3D12GraphicsCommandList8_INTERFACE_DEFINED__)
	bool hasIndependentDepthStencil{};
	D3D12_DEPTH_STENCIL_DESC2 independentDepthStencil{};
#endif

private:
	friend Status Decode(std::span<const std::uint8_t> bytes, OwnedRecipe& output);
	void RebindShaderBytecode() noexcept;
};

Status Serialize(std::uint64_t pipelineKey,
	const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc,
	const SerializeOptions& options,
	Serialized& output);

Status Decode(std::span<const std::uint8_t> bytes, OwnedRecipe& output);
}
