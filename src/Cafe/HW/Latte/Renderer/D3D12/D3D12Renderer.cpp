#include "Cafe/HW/Latte/Renderer/D3D12/D3D12Renderer.h"
#include "Cafe/HW/Latte/LegacyShaderDecompiler/LatteDecompilerEmitHLSL.hpp"

#include "Cafe/HW/Latte/Renderer/D3D12/D3D12DescriptorManager.h"
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12DiagnosticCapturePolicy.h"
#include "Cafe/HW/Latte/Renderer/D3D12/CachedFBOD3D12.h"
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12Formats.h"
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12GeometryProbe.h"
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12MemoryManager.h"
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12PipelineCompiler.h"
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12PipelineRecipe.h"
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12PipelinePrewarmPolicy.h"
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12PipelineStableCache.h"
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12Query.h"
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12SamplerTableState.h"
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12TextureUnitMask.h"
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12ActiveTextureUnits.h"
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12ViewStateRanges.h"
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12LogicOpCopyRegion.h"
#include "Cafe/HW/Latte/Renderer/D3D12/LatteTextureD3D12.h"
#include "Cafe/HW/Latte/Renderer/D3D12/LatteTextureViewD3D12.h"
#include "Cafe/HW/Latte/Renderer/D3D12/RendererShaderD3D12.h"
#include "Cafe/HW/Latte/Renderer/D3D12/TextureReadbackD3D12.h"
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12ViewDescription.h"
#if defined(CEMU_UWP_D3D12_DIAGNOSTICS)
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12DiagnosticDrawContextPolicy.h"
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12TypedSurfaceStats.h"
#endif
#include "Cafe/HW/Latte/Core/FetchShader.h"
#include "Cafe/HW/Latte/Core/Latte.h"
#include "Cafe/HW/Latte/Core/LatteIndices.h"
#include "Cafe/HW/Latte/Core/LattePrimitiveRestartPolicy.h"
#include "Cafe/HW/Latte/Core/LatteOverlay.h"
#include "Cafe/HW/Latte/Core/LattePerformanceMonitor.h"
#include "Cafe/HW/Latte/Core/LatteShader.h"
#include "Cafe/HW/Latte/Core/LatteTexture.h"
#include "Cafe/HW/Latte/ISA/RegDefines.h"
#include "Cafe/HW/Latte/Renderer/RendererCore.h"
#include "Cafe/HW/Latte/Renderer/RendererOuputShader.h"
#include "Cafe/HW/MMU/MMU.h"
#include "Cafe/CafeSystem.h"
#if defined(CEMU_UWP)
#include "uwp/UwpMemoryBudgetService.h"
#include "uwp/UwpMediaStreamer.h"
#endif
#include "config/CemuConfig.h"
#include "config/ActiveSettings.h"
#include "imgui.h"
#include "imgui/imgui_impl_dx12.h"
#include "imgui/imgui_extension.h"
#include "WindowSystem.h"
#include "util/helpers/StringBuf.h"
#include "util/helpers/helpers.h"
#include <png.h>

#if BOOST_OS_WINDOWS
#include <d3d12sdklayers.h>
#include <dxgi1_4.h>
#include <fileapifromapp.h>
#include <winrt/Windows.System.Profile.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/base.h>
#endif

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <bit>
#include <optional>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <cwchar>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

bool g_d3d12_available = false;
extern bool hasValidFramebufferAttached;

// queue the call for the recording thread when the Latte side makes it; arguments are captured by value
#define D3D12_QUEUE_RECORDING(...) \
	do { if (ShouldQueueRecording()) { QueueRecording([=, this]() { __VA_ARGS__; }); return; } } while (false)
// the rest of the function uses recording state directly: wait for queued work and keep the recorder out
#define D3D12_EXCLUSIVE_RECORDING() RecordingExclusiveScope recordingExclusiveScope(this)

// Phase 4 (two-thread recording): everything after the pipeline lookup in draw_execute reads the emulated GPU state from
// these copies instead of LatteGPUState, so it can later run on its own thread. SyncRecordState refreshes them per draw.
namespace
{
std::unique_ptr<LatteContextRegister> s_recordRegs;
bool s_recordRegsComplete = false;
LatteFetchShader* s_recordFetchShader = nullptr;
LatteShaderPSInputTable s_recordPsInputTable{};

const LatteContextRegister& RecordRegs() { return *s_recordRegs; }
uint32* RecordRegWords() { return reinterpret_cast<uint32*>(s_recordRegs.get()); }

// bumped whenever the record copy of a register chunk read by descriptor table keys changes (texture resource
// words, sampler words, sampler border colors); lets a table key be reused while nothing it reads changed
// index 0..2 = vertex/pixel/geometry (LatteConst::ShaderType order): texture resource words and sampler words with
// border colors, each bumped separately so a change for one stage keeps the other stages' keys
std::array<uint64, 3> s_recordTextureRegGeneration{ 1, 1, 1 };
std::array<uint64, 3> s_recordSamplerRegGeneration{ 1, 1, 1 };
constexpr uint32 kRecordChunkBytes = 64 * sizeof(uint32);
void BumpAllRecordSamplingGenerations()
{
	for (auto& generation : s_recordTextureRegGeneration)
		++generation;
	for (auto& generation : s_recordSamplerRegGeneration)
		++generation;
}
void NoteRecordChunkChanged(uint32 chunk)
{
	const size_t first = (size_t)chunk * kRecordChunkBytes;
	const size_t end = first + kRecordChunkBytes;
	auto overlaps = [first, end](size_t rangeFirst, size_t rangeSize) { return end > rangeFirst && first < rangeFirst + rangeSize; };
	constexpr size_t vs = (size_t)LatteConst::ShaderType::Vertex, ps = (size_t)LatteConst::ShaderType::Pixel, gs = (size_t)LatteConst::ShaderType::Geometry;
	if (overlaps(offsetof(LatteContextRegister, SQ_TEX_START_PS), sizeof(LatteContextRegister::SQ_TEX_START_PS)))
		++s_recordTextureRegGeneration[ps];
	if (overlaps(offsetof(LatteContextRegister, SQ_TEX_START_VS), sizeof(LatteContextRegister::SQ_TEX_START_VS)))
		++s_recordTextureRegGeneration[vs];
	if (overlaps(offsetof(LatteContextRegister, SQ_TEX_START_GS), sizeof(LatteContextRegister::SQ_TEX_START_GS)))
		++s_recordTextureRegGeneration[gs];
	// samplers: 18 per stage in PS, VS, GS order (LatteDecompiler_getTextureSamplerBaseIndex)
	constexpr size_t samplerSetSize = sizeof(_LatteRegisterSetSampler) * 18;
	const size_t samplerBase = offsetof(LatteContextRegister, SQ_TEX_SAMPLER);
	if (overlaps(samplerBase, samplerSetSize) || overlaps(offsetof(LatteContextRegister, TD_PS_SAMPLER_BORDER_COLOR), sizeof(LatteContextRegister::TD_PS_SAMPLER_BORDER_COLOR)))
		++s_recordSamplerRegGeneration[ps];
	if (overlaps(samplerBase + samplerSetSize, samplerSetSize) || overlaps(offsetof(LatteContextRegister, TD_VS_SAMPLER_BORDER_COLOR), sizeof(LatteContextRegister::TD_VS_SAMPLER_BORDER_COLOR)))
		++s_recordSamplerRegGeneration[vs];
	if (overlaps(samplerBase + 2 * samplerSetSize, samplerSetSize) || overlaps(offsetof(LatteContextRegister, TD_GS_SAMPLER_BORDER_COLOR), sizeof(LatteContextRegister::TD_GS_SAMPLER_BORDER_COLOR)))
		++s_recordSamplerRegGeneration[gs];
}

void SyncRecordState(LatteFetchShader* fetchShader)
{
	if (!s_recordRegs)
		s_recordRegs = std::make_unique<LatteContextRegister>();
	uint32* dst = RecordRegWords();
	const uint32* src = reinterpret_cast<const uint32*>(&LatteGPUState.contextNew);
	if (!s_recordRegsComplete)
	{
		memcpy(dst, src, sizeof(LatteContextRegister));
		std::fill(std::begin(LatteGPUState.registerChunkDirty), std::end(LatteGPUState.registerChunkDirty), 0);
		s_recordRegsComplete = true;
		BumpAllRecordSamplingGenerations();
	}
	else
	{
		for (uint32 word = 0; word < std::size(LatteGPUState.registerChunkDirty); ++word)
		{
			uint64 bits = std::exchange(LatteGPUState.registerChunkDirty[word], 0);
			while (bits)
			{
				const uint32 chunk = word * 64 + (uint32)std::countr_zero(bits);
				bits &= bits - 1;
				memcpy(dst + chunk * 64, src + chunk * 64, 64 * sizeof(uint32));
				NoteRecordChunkChanged(chunk);
			}
		}
		// the few words after the register file (special state values) are not written through the command processor
		memcpy(dst + LATTE_MAX_REGISTER, src + LATTE_MAX_REGISTER, sizeof(LatteContextRegister) - LATTE_MAX_REGISTER * sizeof(uint32));
		// self-check: right after a sync the copy must equal the live file; a difference is a register write that
		// skipped LatteGPUState_markRegistersDirty
		static uint32 s_syncCount = 0;
		static uint32 s_mismatchLogs = 0;
		if ((++s_syncCount & 4095) == 0 && memcmp(dst, src, LATTE_MAX_REGISTER * sizeof(uint32)) != 0)
		{
			const uint32 first = (uint32)(std::mismatch(dst, dst + LATTE_MAX_REGISTER, src).first - dst);
			if (s_mismatchLogs++ < 16)
				cemuLog_log(LogType::Force, "D3D12 record registers out of sync at 0x{:04x} (copy 0x{:08x}, live 0x{:08x}); a register write is not marked dirty",
					first, dst[first], src[first]);
			memcpy(dst, src, LATTE_MAX_REGISTER * sizeof(uint32));
			BumpAllRecordSamplingGenerations();
		}
	}
	s_recordFetchShader = fetchShader;
	if (const LatteShaderPSInputTable* psInputTable = LatteSHRC_GetPSInputTable())
		s_recordPsInputTable = *psInputTable;
}

// threaded recording: the Latte side collects what changed since the last draw, the recording thread applies it
// before the draw's tail; the first delta carries the whole register file
constexpr uint32 kRecordSpecialWords = (uint32)((sizeof(LatteContextRegister) - LATTE_MAX_REGISTER * sizeof(uint32)) / sizeof(uint32));
struct RecordStateDelta
{
	bool full = false;
	std::vector<uint32> chunks;
	std::vector<uint32> words; // 64 per chunk, or the whole file when full
	std::array<uint32, kRecordSpecialWords> special{};
	LatteFetchShader* fetchShader = nullptr;
	LatteShaderPSInputTable psInputTable{};
};
bool s_latteSentFullRegisters = false;

// threaded recording: per-draw buffers are filled on the Latte thread and emptied on the recorder; reusing them avoids
// a heap allocation on one thread and the free on the other for every draw
template<typename T>
class RecordVectorPool
{
public:
	std::vector<T> Take()
	{
		std::scoped_lock lock(m_mutex);
		if (m_free.empty())
			return {};
		std::vector<T> v = std::move(m_free.back());
		m_free.pop_back();
		return v;
	}
	void Give(std::vector<T>&& v)
	{
		if (v.capacity() == 0 || v.capacity() > 64 * 1024)
			return;
		v.clear();
		std::scoped_lock lock(m_mutex);
		if (m_free.size() < 1024)
			m_free.push_back(std::move(v));
	}
private:
	std::mutex m_mutex;
	std::vector<std::vector<T>> m_free;
};
RecordVectorPool<uint32> s_recordWordPool;
RecordVectorPool<uint8> s_recordBytePool;

void CollectRecordState(RecordStateDelta& delta, LatteFetchShader* fetchShader)
{
	const uint32* src = reinterpret_cast<const uint32*>(&LatteGPUState.contextNew);
	if (!s_latteSentFullRegisters)
	{
		delta.full = true;
		delta.words.assign(src, src + LATTE_MAX_REGISTER);
		std::fill(std::begin(LatteGPUState.registerChunkDirty), std::end(LatteGPUState.registerChunkDirty), 0);
		s_latteSentFullRegisters = true;
	}
	else
	{
		uint32 dirtyChunks = 0;
		for (uint64 bits : LatteGPUState.registerChunkDirty)
			dirtyChunks += (uint32)std::popcount(bits);
		if (dirtyChunks != 0)
		{
			delta.chunks = s_recordWordPool.Take();
			delta.words = s_recordWordPool.Take();
			delta.chunks.reserve(dirtyChunks);
			delta.words.reserve((size_t)dirtyChunks * 64);
		}
		for (uint32 word = 0; word < std::size(LatteGPUState.registerChunkDirty); ++word)
		{
			uint64 bits = std::exchange(LatteGPUState.registerChunkDirty[word], 0);
			while (bits)
			{
				const uint32 chunk = word * 64 + (uint32)std::countr_zero(bits);
				bits &= bits - 1;
				delta.chunks.push_back(chunk);
				delta.words.insert(delta.words.end(), src + chunk * 64, src + chunk * 64 + 64);
			}
		}
	}
	std::copy_n(src + LATTE_MAX_REGISTER, kRecordSpecialWords, delta.special.begin());
	delta.fetchShader = fetchShader;
	if (const LatteShaderPSInputTable* psInputTable = LatteSHRC_GetPSInputTable())
		delta.psInputTable = *psInputTable;
}

void ApplyRecordState(const RecordStateDelta& delta)
{
	if (!s_recordRegs)
		s_recordRegs = std::make_unique<LatteContextRegister>();
	uint32* dst = RecordRegWords();
	if (delta.full)
	{
		std::copy(delta.words.begin(), delta.words.end(), dst);
		s_recordRegsComplete = true;
		BumpAllRecordSamplingGenerations();
	}
	else
	{
		for (size_t i = 0; i < delta.chunks.size(); ++i)
		{
			std::copy_n(delta.words.data() + i * 64, 64, dst + delta.chunks[i] * 64);
			NoteRecordChunkChanged(delta.chunks[i]);
		}
	}
	std::copy(delta.special.begin(), delta.special.end(), dst + LATTE_MAX_REGISTER);
	s_recordFetchShader = delta.fetchShader;
	s_recordPsInputTable = delta.psInputTable;
}
}


namespace
{
// Increment when synthetic geometry-shader HLSL changes.
constexpr uint64 kD3D12SyntheticGsGeneratorVersion = 3;
constexpr uint32 kGameTextureSrvDescriptorCount = 112;
constexpr uint32 kGameVertexBufferSrvBase = 32;
constexpr uint32 kGameLogicOpDstSrvBinding = 96;
constexpr uint32 kGameLogicOpDstSrvCount = 8;
constexpr uint32 kGameUavDescriptorCount = LATTE_NUM_STREAMOUT_BUFFER;
constexpr uint32 kGameSrvDescriptorOffset = 0;
constexpr uint32 kGameSamplerDescriptorCount = LATTE_NUM_MAX_TEX_UNITS;
constexpr uint64 kTraceGpuStatsSampledDefaultLimit = 128;
constexpr uint64 kTraceGpuStatsFocusedDefaultLimit = 4096;
constexpr uint64 kTraceGpuStatsUwpFocusedDefaultLimit = 64;
constexpr uint32 kTraceGpuStatsMaxAddressFilters = 32;
constexpr uint32 kTraceGpuStatsMaxPipelineFilters = 32;
#if defined(CEMU_UWP_D3D12_DIAGNOSTICS)
constexpr uint64 kUwpD3D12TraceMaxBytes = 128ull * 1024ull * 1024ull;
constexpr uint32 kGpuStatsCaptureContextEventBudget = 8192;
std::atomic_bool s_uwpD3D12TraceDisabled{ false };
std::mutex s_uwpD3D12InvalidTraceEventMutex;
std::unordered_set<std::string> s_uwpD3D12InvalidTraceEvents;
#endif

constexpr uint64 kD3D12GamePsoBlobMagic = 0x4F53503132444D43ull; // "CMD12PSO"
constexpr uint32 kD3D12GamePsoBlobVersion = 1;
constexpr uint64 kD3D12GamePsoSchemaVersion = 3;
constexpr uint64 kD3D12GamePsoBlobMaxBytes = 64ull * 1024ull * 1024ull;

struct D3D12GamePsoBlobHeader
{
	uint64 magic{};
	uint32 version{};
	uint32 headerSize{};
	uint64 titleId{};
	uint64 pipelineKey{};
	uint64 fingerprint{};
	uint32 adapterLuidLow{};
	sint32 adapterLuidHigh{};
	uint64 blobSize{};
};

static_assert(std::is_trivially_copyable_v<D3D12GamePsoBlobHeader>);

bool IsD3D12DeviceFailure(HRESULT hr)
{
	return hr == DXGI_ERROR_DEVICE_REMOVED ||
		   hr == DXGI_ERROR_DEVICE_RESET ||
		   hr == DXGI_ERROR_DEVICE_HUNG ||
		   hr == DXGI_ERROR_DRIVER_INTERNAL_ERROR;
}

bool IsD3D12TransientMemoryFailure(HRESULT hr)
{
	return hr == E_OUTOFMEMORY;
}

bool IsStalePipelineLibraryError(HRESULT hr)
{
	return hr == E_INVALIDARG ||
		   hr == D3D12_ERROR_DRIVER_VERSION_MISMATCH ||
		   hr == D3D12_ERROR_ADAPTER_NOT_FOUND;
}

bool IsXboxConsole()
{
#if defined(CEMU_UWP)
	static const bool isXbox = []()
	{
		try
		{
			return winrt::Windows::System::Profile::AnalyticsInfo::VersionInfo().DeviceFamily() == L"Windows.Xbox";
		}
		catch (...)
		{
			return false;
		}
	}();
	return isXbox;
#else
	return false;
#endif
}

#if defined(__ID3D12GraphicsCommandList8_INTERFACE_DEFINED__)
template<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE Type, typename Value>
struct alignas(void*) D3D12PipelineStreamSubobject
{
	D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type{ Type };
	Value value{};
};

struct D3D12IndependentStencilPipelineStream
{
	D3D12PipelineStreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, ID3D12RootSignature*> rootSignature;
	D3D12PipelineStreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS, D3D12_SHADER_BYTECODE> vs;
	D3D12PipelineStreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS, D3D12_SHADER_BYTECODE> ps;
	D3D12PipelineStreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS, D3D12_SHADER_BYTECODE> ds;
	D3D12PipelineStreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS, D3D12_SHADER_BYTECODE> hs;
	D3D12PipelineStreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS, D3D12_SHADER_BYTECODE> gs;
	D3D12PipelineStreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT, D3D12_STREAM_OUTPUT_DESC> streamOutput;
	D3D12PipelineStreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND, D3D12_BLEND_DESC> blend;
	D3D12PipelineStreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK, UINT> sampleMask;
	D3D12PipelineStreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER, D3D12_RASTERIZER_DESC> rasterizer;
	D3D12PipelineStreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL2, D3D12_DEPTH_STENCIL_DESC2> depthStencil;
	D3D12PipelineStreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT, D3D12_INPUT_LAYOUT_DESC> inputLayout;
	D3D12PipelineStreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_IB_STRIP_CUT_VALUE, D3D12_INDEX_BUFFER_STRIP_CUT_VALUE> stripCut;
	D3D12PipelineStreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY, D3D12_PRIMITIVE_TOPOLOGY_TYPE> primitiveTopology;
	D3D12PipelineStreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS, D3D12_RT_FORMAT_ARRAY> renderTargetFormats;
	D3D12PipelineStreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT, DXGI_FORMAT> depthStencilFormat;
	D3D12PipelineStreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC, DXGI_SAMPLE_DESC> sampleDesc;
	D3D12PipelineStreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_NODE_MASK, UINT> nodeMask;
	D3D12PipelineStreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO, D3D12_CACHED_PIPELINE_STATE> cachedPso;
	D3D12PipelineStreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_FLAGS, D3D12_PIPELINE_STATE_FLAGS> flags;
};

static_assert(std::is_standard_layout_v<D3D12IndependentStencilPipelineStream>);
static_assert(alignof(D3D12IndependentStencilPipelineStream) >= alignof(void*));

HRESULT CreateGraphicsPipelineWithIndependentStencil(
	ID3D12Device2* device,
	const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc,
	const D3D12_DEPTH_STENCIL_DESC2& depthStencil,
	Microsoft::WRL::ComPtr<ID3D12PipelineState>& pipeline)
{
	if (!device)
		return E_NOINTERFACE;

	D3D12IndependentStencilPipelineStream stream{};
	stream.rootSignature.value = desc.pRootSignature;
	stream.vs.value = desc.VS;
	stream.ps.value = desc.PS;
	stream.ds.value = desc.DS;
	stream.hs.value = desc.HS;
	stream.gs.value = desc.GS;
	stream.streamOutput.value = desc.StreamOutput;
	stream.blend.value = desc.BlendState;
	stream.sampleMask.value = desc.SampleMask;
	stream.rasterizer.value = desc.RasterizerState;
	stream.depthStencil.value = depthStencil;
	stream.inputLayout.value = desc.InputLayout;
	stream.stripCut.value = desc.IBStripCutValue;
	stream.primitiveTopology.value = desc.PrimitiveTopologyType;
	for (uint32 i = 0; i < std::size(desc.RTVFormats); ++i)
		stream.renderTargetFormats.value.RTFormats[i] = desc.RTVFormats[i];
	stream.renderTargetFormats.value.NumRenderTargets = desc.NumRenderTargets;
	stream.depthStencilFormat.value = desc.DSVFormat;
	stream.sampleDesc.value = desc.SampleDesc;
	stream.nodeMask.value = desc.NodeMask;
	stream.cachedPso.value = desc.CachedPSO;
	stream.flags.value = desc.Flags;

	D3D12_PIPELINE_STATE_STREAM_DESC streamDesc{ sizeof(stream), &stream };
	return device->CreatePipelineState(&streamDesc, IID_PPV_ARGS(&pipeline));
}
#endif

enum class D3D12TraceGpuStatsMode
{
	Off,
	Sampled,
	Focused,
	Full,
};

enum class UwpScreenLayout : sint32
{
	TVOnly = 0,
	GamePadOnly,
	SideBySide,
	Stacked,
	PictureInPicture,
	GamePadPictureInPicture,
};

struct UwpOutputRect
{
	sint32 x = 0;
	sint32 y = 0;
	sint32 w = 0;
	sint32 h = 0;
	bool visible = false;
};

sint32 ClampUwpScreenLayout(sint32 layout)
{
	switch (static_cast<UwpScreenLayout>(layout))
	{
	case UwpScreenLayout::TVOnly:
	case UwpScreenLayout::GamePadOnly:
	case UwpScreenLayout::SideBySide:
	case UwpScreenLayout::Stacked:
	case UwpScreenLayout::PictureInPicture:
	case UwpScreenLayout::GamePadPictureInPicture:
		return layout;
	default:
		return static_cast<sint32>(UwpScreenLayout::TVOnly);
	}
}

uint64 GetUwpTitleId()
{
	return CafeSystem::IsTitleRunning() ? CafeSystem::GetForegroundTitleId() : 0;
}

sint32 ResolveUwpScreenLayout()
{
	auto& display = GetConfig().uwp_display;
	const uint64 titleId = GetUwpTitleId();
	if (titleId != 0)
	{
		const auto it = display.game_layouts.find(titleId);
		if (it != display.game_layouts.end())
			return ClampUwpScreenLayout(it->second.layout);
	}
	return ClampUwpScreenLayout(display.default_layout.GetValue());
}

UwpOutputRect FitUwpOutputRect(sint32 x, sint32 y, sint32 w, sint32 h)
{
	constexpr double aspect = 16.0 / 9.0;
	if (w <= 0 || h <= 0)
		return {};
	sint32 outW = w;
	sint32 outH = std::max<sint32>(1, (sint32)std::lround((double)outW / aspect));
	if (outH > h)
	{
		outH = h;
		outW = std::max<sint32>(1, (sint32)std::lround((double)outH * aspect));
	}
	return { x + (w - outW) / 2, y + (h - outH) / 2, outW, outH, true };
}

UwpOutputRect GetUwpInsetRect(const Vector2i& size)
{
	const sint32 margin = std::max<sint32>(18, std::min(size.x, size.y) / 32);
	const sint32 insetW = std::max<sint32>(180, size.x / 3);
	const sint32 insetH = std::max<sint32>(100, (sint32)std::lround((double)insetW * 9.0 / 16.0));
	const sint32 fittedW = std::min(insetW, std::max<sint32>(1, size.x - margin * 2));
	const sint32 fittedH = std::min(insetH, std::max<sint32>(1, size.y - margin * 2));
	return FitUwpOutputRect(size.x - fittedW - margin, size.y - fittedH - margin, fittedW, fittedH);
}

UwpOutputRect ResolveUwpOutputRect(bool padView, const Vector2i& size)
{
	const sint32 gap = std::max<sint32>(8, std::min(size.x, size.y) / 90);
	const auto full = FitUwpOutputRect(0, 0, std::max<sint32>(1, size.x), std::max<sint32>(1, size.y));
	UwpOutputRect tv{};
	UwpOutputRect pad{};
	switch (static_cast<UwpScreenLayout>(ResolveUwpScreenLayout()))
	{
	case UwpScreenLayout::TVOnly:
		tv = full;
		break;
	case UwpScreenLayout::GamePadOnly:
		pad = full;
		break;
	case UwpScreenLayout::SideBySide:
	{
		const sint32 cellW = std::max<sint32>(1, (size.x - gap) / 2);
		tv = FitUwpOutputRect(0, 0, cellW, size.y);
		pad = FitUwpOutputRect(cellW + gap, 0, size.x - cellW - gap, size.y);
		break;
	}
	case UwpScreenLayout::Stacked:
	{
		const sint32 cellH = std::max<sint32>(1, (size.y - gap) / 2);
		tv = FitUwpOutputRect(0, 0, size.x, cellH);
		pad = FitUwpOutputRect(0, cellH + gap, size.x, size.y - cellH - gap);
		break;
	}
	case UwpScreenLayout::PictureInPicture:
		tv = full;
		pad = GetUwpInsetRect(size);
		break;
	case UwpScreenLayout::GamePadPictureInPicture:
		pad = full;
		tv = GetUwpInsetRect(size);
		break;
	}

	if (GetConfig().uwp_display.swap_screens.GetValue())
		std::swap(tv, pad);
	return padView ? pad : tv;
}

uint64 GetUwpOutputArea(const UwpOutputRect& rect)
{
	return rect.visible ? (uint64)std::max<sint32>(rect.w, 0) * (uint64)std::max<sint32>(rect.h, 0) : 0;
}

bool UwpOutputRectsOverlap(const UwpOutputRect& a, const UwpOutputRect& b)
{
	return a.visible && b.visible &&
		   a.x < b.x + b.w && b.x < a.x + a.w &&
		   a.y < b.y + b.h && b.y < a.y + a.h;
}

bool UwpLayoutShowsGamePad()
{
	return ResolveUwpOutputRect(true, { 1280, 720 }).visible;
}

struct UwpDecodedImage
{
	std::vector<uint8> rgba;
	sint32 width = 0;
	sint32 height = 0;
};

std::string GetUwpUnderlayPath()
{
	if (!CafeSystem::IsTitleRunning())
		return {};
	const uint64 titleId = CafeSystem::GetForegroundTitleId();
	const sint32 layout = ResolveUwpScreenLayout();
	static uint64 s_underlayCachedTitleId = 0;
	static sint32 s_underlayCachedLayout = -100;
	static std::string s_underlayCachedPath;
	if (titleId == s_underlayCachedTitleId && layout == s_underlayCachedLayout)
		return s_underlayCachedPath;
	std::string layoutFile;
	switch (static_cast<UwpScreenLayout>(layout))
	{
	case UwpScreenLayout::SideBySide:
		layoutFile = "side_by_side.png";
		break;
	case UwpScreenLayout::Stacked:
		layoutFile = "stacked.png";
		break;
	default:
		s_underlayCachedTitleId = titleId;
		s_underlayCachedLayout = layout;
		s_underlayCachedPath.clear();
		return {};
	}

	const std::string title = fmt::format("{:016x}", titleId);
	const std::string alternate = layoutFile == "side_by_side.png" ? "underlay_side_by_side.png" : "underlay_stacked.png";
	const std::array candidates{
		ActiveSettings::GetUserDataPath("titleImages/{}/{}", title, layoutFile),
		ActiveSettings::GetUserDataPath("titleImages/{}/{}", title, alternate),
		ActiveSettings::GetUserDataPath("titles/{}/{}", title, layoutFile),
		ActiveSettings::GetUserDataPath("titles/{}/{}", title, alternate),
		ActiveSettings::GetDataPath("titleImages/{}/{}", title, layoutFile),
		ActiveSettings::GetDataPath("titleImages/{}/{}", title, alternate),
		ActiveSettings::GetDataPath("titles/{}/{}", title, layoutFile),
		ActiveSettings::GetDataPath("titles/{}/{}", title, alternate),
	};
	s_underlayCachedTitleId = titleId;
	s_underlayCachedLayout = layout;
	s_underlayCachedPath.clear();
	for (const auto& candidate : candidates)
	{
		if (fs::exists(candidate))
		{
			s_underlayCachedPath = _pathToUtf8(candidate);
			break;
		}
	}
	return s_underlayCachedPath;
}

bool DecodeUwpPngFile(const fs::path& path, UwpDecodedImage& out)
{
	std::ifstream file(path, std::ios::binary);
	if (!file)
		return false;
	std::vector<uint8> bytes((std::istreambuf_iterator<char>(file)), {});
	if (bytes.size() < 8 || png_sig_cmp(bytes.data(), 0, 8) != 0)
		return false;

	png_image image{};
	image.version = PNG_IMAGE_VERSION;
	if (!png_image_begin_read_from_memory(&image, bytes.data(), bytes.size()))
		return false;
	image.format = PNG_FORMAT_RGBA;
	std::vector<uint8> rgba(PNG_IMAGE_SIZE(image));
	if (!png_image_finish_read(&image, nullptr, rgba.data(), 0, nullptr))
	{
		png_image_free(&image);
		return false;
	}

	out.width = (sint32)image.width;
	out.height = (sint32)image.height;
	out.rgba = std::move(rgba);
	png_image_free(&image);
	return out.width > 0 && out.height > 0 && !out.rgba.empty();
}

struct D3D12TraceGpuStatsDecision
{
	bool capture{};
	const char* reason{};
};

D3D12_CPU_DESCRIPTOR_HANDLE OffsetCpuHandle(D3D12_CPU_DESCRIPTOR_HANDLE handle, uint32 descriptorSize, uint32 index)
{
	handle.ptr += size_t(descriptorSize) * index;
	return handle;
}

uint64 MixGameDescriptorTableKey(uint64 key, uint64 value)
{
	key ^= value + 0x9e3779b97f4a7c15ull + (key << 6) + (key >> 2);
	return std::rotl(key, 23);
}

uint64 MixGameStateBytes(uint64 key, const void* data, size_t size)
{
	const auto* bytes = static_cast<const uint8*>(data);
	while (size >= sizeof(uint64))
	{
		uint64 value{};
		memcpy(&value, bytes, sizeof(value));
		key = MixGameDescriptorTableKey(key, value);
		bytes += sizeof(value);
		size -= sizeof(value);
	}
	if (size != 0)
	{
		uint64 value{};
		memcpy(&value, bytes, size);
		key = MixGameDescriptorTableKey(key, value ^ ((uint64)size << 56));
	}
	return key;
}

bool D3D12RuntimeLogCapped(uint32& counter, const char* eventName, uint32 limit = 64)
{
	static const bool verboseRuntimeLogging = []()
	{
		return false;
	}();
	if (!verboseRuntimeLogging)
		return false;

	if (counter < limit)
	{
		counter++;
		return true;
	}
	if (counter == limit)
	{
		counter++;
		cemuLog_log(LogType::Force, "D3D12: further {} logs suppressed.", eventName);
	}
	return false;
}

const char* D3D12InterpolationModifier(const LatteShaderPSInputTable::psImport_t* psImport)
{
	if (!psImport)
		return "";
	if (psImport->isFlat)
		return "nointerpolation ";
	if (psImport->isNoPerspective)
		return "noperspective ";
	return "";
}

const char* D3D12FetchTypeName(LatteConst::VertexFetchType2 fetchType)
{
	switch (fetchType)
	{
	case LatteConst::VertexFetchType2::VERTEX_DATA:
		return "vertex";
	case LatteConst::VertexFetchType2::INSTANCE_DATA:
		return "instance";
	case LatteConst::VertexFetchType2::NO_INDEX_OFFSET_DATA:
		return "constant";
	default:
		return "unknown";
	}
}

const char* D3D12EndianName(LatteConst::VertexFetchEndianMode endianMode)
{
	switch (endianMode)
	{
	case LatteConst::VertexFetchEndianMode::SWAP_NONE:
		return "none";
	case LatteConst::VertexFetchEndianMode::SWAP_U16:
		return "u16";
	case LatteConst::VertexFetchEndianMode::SWAP_U32:
		return "u32";
	default:
		return "unknown";
	}
}

uint32 ReadD3D12ShadowU32(const std::vector<uint8>& shadow, uint32 byteOffset, LatteConst::VertexFetchEndianMode endianMode)
{
	if ((size_t)byteOffset + 4 > shadow.size())
		return 0;
	const uint8* p = shadow.data() + byteOffset;
	if (endianMode == LatteConst::VertexFetchEndianMode::SWAP_U32)
		return ((uint32)p[0] << 24) | ((uint32)p[1] << 16) | ((uint32)p[2] << 8) | (uint32)p[3];
	return (uint32)p[0] | ((uint32)p[1] << 8) | ((uint32)p[2] << 16) | ((uint32)p[3] << 24);
}

bool GetD3D12PrimitiveRestartEnabled(const LatteContextRegister& lcr, Latte::LATTE_VGT_PRIMITIVE_TYPE::E_PRIMITIVE_TYPE primitiveMode,
	Renderer::INDEX_TYPE hostIndexType)
{
	const bool guestEnabled = lcr.VGT_MULTI_PRIM_IB_RESET_EN.get_RESET_EN();
	const unsigned indexBits = hostIndexType == Renderer::INDEX_TYPE::U16 ? 16 :
		hostIndexType == Renderer::INDEX_TYPE::U32 ? 32 : 0;
	return LattePrimitiveRestartPolicy::IsEnabled(true, LattePrimitiveRestartPolicy::IsNativeStrip(primitiveMode),
		guestEnabled, indexBits, lcr.VGT_MULTI_PRIM_IB_RESET_INDX.get_RESTART_INDEX());
}

bool D3D12PrimitiveUsesFaceCulling(Latte::LATTE_VGT_PRIMITIVE_TYPE::E_PRIMITIVE_TYPE primitiveMode)
{
	using Primitive = Latte::LATTE_VGT_PRIMITIVE_TYPE::E_PRIMITIVE_TYPE;
	switch (primitiveMode)
	{
	case Primitive::TRIANGLES:
	case Primitive::TRIANGLE_FAN:
	case Primitive::TRIANGLE_STRIP:
	case Primitive::TRIANGLES_ADJACENT:
	case Primitive::TRIANGLE_STRIP_ADJACENT:
	case Primitive::QUADS:
	case Primitive::QUAD_STRIP:
	case Primitive::RECTS:
		return true;
	default:
		return false;
	}
}

struct D3D12OrderedPrimitiveSpan
{
	uint32 start{};
	uint32 count{};
	bool flipWinding{};
};

std::vector<D3D12OrderedPrimitiveSpan> BuildD3D12OrderedPrimitiveSpans(
	D3D_PRIMITIVE_TOPOLOGY topology,
	uint32 indexCount,
	Renderer::INDEX_TYPE indexType,
	const uint8* indexData,
	bool primitiveRestartEnabled)
{
	uint32 primitiveSize = 0;
	uint32 primitiveStep = 0;
	bool stripTopology = false;
	bool alternatingTriangleWinding = false;
	switch (topology)
	{
	case D3D_PRIMITIVE_TOPOLOGY_POINTLIST:
		primitiveSize = primitiveStep = 1;
		break;
	case D3D_PRIMITIVE_TOPOLOGY_LINELIST:
		primitiveSize = primitiveStep = 2;
		break;
	case D3D_PRIMITIVE_TOPOLOGY_LINESTRIP:
		primitiveSize = 2;
		primitiveStep = 1;
		stripTopology = true;
		break;
	case D3D_PRIMITIVE_TOPOLOGY_LINELIST_ADJ:
		primitiveSize = primitiveStep = 4;
		break;
	case D3D_PRIMITIVE_TOPOLOGY_LINESTRIP_ADJ:
		primitiveSize = 4;
		primitiveStep = 1;
		stripTopology = true;
		break;
	case D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST:
		primitiveSize = primitiveStep = 3;
		break;
	case D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP:
		primitiveSize = 3;
		primitiveStep = 1;
		stripTopology = true;
		alternatingTriangleWinding = true;
		break;
	case D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST_ADJ:
		primitiveSize = primitiveStep = 6;
		break;
	case D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP_ADJ:
		primitiveSize = 6;
		primitiveStep = 2;
		stripTopology = true;
		alternatingTriangleWinding = true;
		break;
	default:
		return {};
	}

	std::vector<D3D12OrderedPrimitiveSpan> spans;
	if (primitiveSize == 0 || primitiveStep == 0 || indexCount < primitiveSize)
		return spans;
	spans.reserve((indexCount - primitiveSize) / primitiveStep + 1);
	const auto appendSegment = [&](uint32 segmentStart, uint32 segmentCount)
	{
		uint32 primitiveIndex = 0;
		for (uint32 offset = 0; offset + primitiveSize <= segmentCount; offset += primitiveStep, ++primitiveIndex)
		{
			spans.push_back({
				segmentStart + offset,
				primitiveSize,
				alternatingTriangleWinding && (primitiveIndex & 1u) != 0,
			});
		}
	};

	if (!stripTopology || !primitiveRestartEnabled || indexType == Renderer::INDEX_TYPE::NONE || !indexData)
	{
		appendSegment(0, indexCount);
		return spans;
	}

	uint32 segmentStart = 0;
	for (uint32 i = 0; i < indexCount; ++i)
	{
		bool isCut = false;
		if (indexType == Renderer::INDEX_TYPE::U16)
			isCut = reinterpret_cast<const uint16*>(indexData)[i] == std::numeric_limits<uint16>::max();
		else if (indexType == Renderer::INDEX_TYPE::U32)
			isCut = reinterpret_cast<const uint32*>(indexData)[i] == std::numeric_limits<uint32>::max();
		if (!isCut)
			continue;
		appendSegment(segmentStart, i - segmentStart);
		segmentStart = i + 1;
	}
	appendSegment(segmentStart, indexCount - segmentStart);
	return spans;
}

bool D3D12FrontCounterClockwise(Latte::LATTE_PA_SU_SC_MODE_CNTL::E_FRONTFACE latteFrontFace)
{
	// output/surface-copy flips are handled in their own passes, not by PSO culling
	return latteFrontFace == Latte::LATTE_PA_SU_SC_MODE_CNTL::E_FRONTFACE::CCW;
}

struct D3D12EffectiveCullState
{
	bool cullFront{};
	bool cullBack{};
};

D3D12EffectiveCullState GetD3D12EffectiveCullState(Latte::LATTE_VGT_PRIMITIVE_TYPE::E_PRIMITIVE_TYPE primitiveMode,
												   const Latte::LATTE_PA_SU_SC_MODE_CNTL& polygonControlReg)
{
	D3D12EffectiveCullState state
	{
		polygonControlReg.get_CULL_FRONT(),
		polygonControlReg.get_CULL_BACK(),
	};
	if (primitiveMode == Latte::LATTE_VGT_PRIMITIVE_TYPE::E_PRIMITIVE_TYPE::RECTS)
	{
		// expanded rects have clockwise effective winding, matching the Vulkan backend
		if (polygonControlReg.get_FRONT_FACE() == Latte::LATTE_PA_SU_SC_MODE_CNTL::E_FRONTFACE::CW)
			state.cullFront = state.cullBack;
		else
			state.cullBack = state.cullFront;
	}
	return state;
}

bool D3D12PrimitiveUsesPointExpansion(Latte::LATTE_VGT_PRIMITIVE_TYPE::E_PRIMITIVE_TYPE primitiveMode)
{
	return primitiveMode == Latte::LATTE_VGT_PRIMITIVE_TYPE::E_PRIMITIVE_TYPE::POINTS;
}

D3D12EffectiveCullState GetD3D12StencilFaceCullState(const D3D12EffectiveCullState& cullState, bool usePointExpansionGS)
{
	// points are never culled and always front-facing, so only the front stencil state applies
	if (usePointExpansionGS)
		return { false, true };
	return cullState;
}

bool D3D12ShouldDumpHLSL()
{
	static const bool enabled = []()
	{
#if defined(CEMU_UWP_D3D12_DIAGNOSTICS)
		std::error_code ec;
		return fs::exists(ActiveSettings::GetUserDataPath("diagnostics/d3d12_dump_hlsl"), ec);
#else
		return false;
#endif
	}();
	return enabled;
}

bool D3D12ShouldEnableDebug()
{
	static const bool enabled = []()
	{
		return false;
	}();
	return enabled;
}

bool D3D12ShouldChecksumOutput()
{
	static const bool enabled = []()
	{
		return false;
	}();
	return enabled;
}

#if defined(CEMU_UWP_D3D12_DIAGNOSTICS)
const fs::path& D3D12UwpGpuCapturePath()
{
	// contents: whitespace-separated hex guest texture addresses, optional 0x prefix
	static const fs::path path = ActiveSettings::GetUserDataPath("diagnostics/d3d12_gpu_capture.txt");
	return path;
}

const fs::path& D3D12UwpGpuCaptureArmPath()
{
	static const fs::path path = ActiveSettings::GetUserDataPath("diagnostics/d3d12_gpu_capture_arm.txt");
	return path;
}

std::string D3D12ReadUwpGpuCaptureArmNonce()
{
	std::ifstream marker(D3D12UwpGpuCaptureArmPath(), std::ios::binary);
	if (!marker)
		return {};

	// bound the read so a malformed arm file cannot cause an unbounded allocation
	std::array<char, 256> bytes{};
	marker.read(bytes.data(), bytes.size());
	std::string nonce(bytes.data(), (size_t)marker.gcount());
	auto isWhitespace = [](char ch)
	{
		return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
	};
	while (!nonce.empty() && isWhitespace(nonce.back()))
		nonce.pop_back();
	size_t first = 0;
	while (first < nonce.size() && isWhitespace(nonce[first]))
		first++;
	if (first != 0)
		nonce.erase(0, first);
	return nonce;
}

bool D3D12UwpGpuCaptureNonceIsDisarmed(std::string_view nonce)
{
	return nonce.empty() || nonce == "0" || nonce == "off" || nonce == "disarm" || nonce == "disarmed";
}

uint64 D3D12HashUwpGpuCaptureNonce(std::string_view nonce)
{
	uint64 hash = 1469598103934665603ull;
	for (const char ch : nonce)
	{
		hash ^= (uint8)ch;
		hash *= 1099511628211ull;
	}
	return hash;
}

bool D3D12UwpGpuCaptureEnabled()
{
	static const bool enabled = []()
	{
		std::error_code ec;
		return fs::exists(D3D12UwpGpuCapturePath(), ec);
	}();
	return enabled;
}
#endif

D3D12TraceGpuStatsMode D3D12GetTraceGpuStatsMode()
{
	static const D3D12TraceGpuStatsMode mode = []()
	{
#if defined(CEMU_UWP_D3D12_DIAGNOSTICS)
		if (D3D12UwpGpuCaptureEnabled())
			return D3D12TraceGpuStatsMode::Focused;
#endif
		return D3D12TraceGpuStatsMode::Off;
	}();
	return mode;
}

const char* D3D12TraceGpuStatsModeName(D3D12TraceGpuStatsMode mode)
{
	switch (mode)
	{
	case D3D12TraceGpuStatsMode::Off:
		return "off";
	case D3D12TraceGpuStatsMode::Sampled:
		return "sampled";
	case D3D12TraceGpuStatsMode::Focused:
		return "focused";
	case D3D12TraceGpuStatsMode::Full:
		return "full";
	}
	return "unknown";
}

#if !defined(CEMU_UWP_D3D12_DIAGNOSTICS)
constexpr bool D3D12ShouldTrace()
{
	return false;
}
#else
bool D3D12ShouldTrace()
{
	static const bool enabled = []()
	{
		return true;
	}();
#if defined(CEMU_UWP_D3D12_DIAGNOSTICS)
	return enabled && !s_uwpD3D12TraceDisabled.load(std::memory_order_relaxed);
#else
	return enabled;
#endif
}
#endif

#if defined(CEMU_UWP_D3D12_DIAGNOSTICS)
bool D3D12TraceIsPriorityEvent(std::string_view eventName)
{
	return eventName.rfind("renderer_", 0) == 0 ||
		   eventName.rfind("gpu_capture_", 0) == 0 ||
		   eventName.rfind("gpu_stats_", 0) == 0 ||
		   eventName.find("failed") != std::string_view::npos ||
		   eventName.find("device") != std::string_view::npos ||
		   eventName.find("pipeline_create") != std::string_view::npos ||
		   eventName.find("pipeline_library") != std::string_view::npos ||
		   eventName.find("texture_copy") != std::string_view::npos ||
		   eventName.find("surface_copy") != std::string_view::npos ||
		   eventName.find("descriptor_rollover") != std::string_view::npos ||
		   eventName.find("uwp_output") != std::string_view::npos;
}

bool D3D12TraceIsGpuCaptureContextEvent(std::string_view eventName)
{
	return eventName.rfind("draw_", 0) == 0 ||
		   eventName.rfind("uniform_", 0) == 0 ||
		   eventName.rfind("game_pipeline_", 0) == 0 ||
		   eventName == "fbo_attachment_bind" ||
		   eventName == "present_end";
}

bool D3D12TraceIsUwpArmedGpuCaptureEvent(std::string_view eventName)
{
	return D3D12DiagnosticCapturePolicy::IsCompletedCaptureEvent(eventName);
}

bool D3D12TraceIsGpuStatsResult(std::string_view eventName, std::string_view payload)
{
	if (payload.find("stats=ok") != std::string_view::npos ||
		payload.find("stats=invalid_size") != std::string_view::npos ||
		payload.find("stats=readback_unavailable") != std::string_view::npos ||
		payload.find("stats=readback_null") != std::string_view::npos ||
		payload.find("stats=empty_readback") != std::string_view::npos)
	{
		return true;
	}

	return eventName == "gpu_stats_capture_end" &&
		(payload.find("result=readback_unavailable") != std::string_view::npos ||
		 payload.find("result=readback_null") != std::string_view::npos ||
		 payload.find("result=empty_readback") != std::string_view::npos);
}

bool D3D12TraceEventIsSuppressed(const char* eventName)
{
	std::scoped_lock lock(s_uwpD3D12InvalidTraceEventMutex);
	return s_uwpD3D12InvalidTraceEvents.contains(eventName);
}

void D3D12SuppressInvalidTraceEvent(const char* eventName, const char* reason)
{
	bool inserted = false;
	{
		std::scoped_lock lock(s_uwpD3D12InvalidTraceEventMutex);
		inserted = s_uwpD3D12InvalidTraceEvents.emplace(eventName).second;
	}
	if (inserted)
	{
		cemuLog_log(LogType::Force,
			"D3D12 diagnostics suppressed malformed trace event '{}': {}",
			eventName, reason ? reason : "formatting failed");
	}
}
#endif

bool D3D12ShouldKeepBufferShadow()
{
	return D3D12ShouldChecksumOutput() || D3D12ShouldTrace();
}

// at 1080p one feedback copy of a render target is 8-16 MB: with 12 / 64 MB the pool evicted and recreated
// ~60 textures per second and the scratch budget forced extra command-list submits. Memory trims release it.
constexpr size_t kD3D12ScratchTexturePoolMaxEntries = 32;
constexpr uint64 kD3D12ScratchTexturePoolMaxBytes = 256ull * 1024ull * 1024ull;

bool D3D12ScratchTextureDescMatches(const D3D12_RESOURCE_DESC& a, const D3D12_RESOURCE_DESC& b)
{
	return a.Dimension == b.Dimension &&
		   a.Alignment == b.Alignment &&
		   a.Width == b.Width &&
		   a.Height == b.Height &&
		   a.DepthOrArraySize == b.DepthOrArraySize &&
		   a.MipLevels == b.MipLevels &&
		   a.Format == b.Format &&
		   a.SampleDesc.Count == b.SampleDesc.Count &&
		   a.SampleDesc.Quality == b.SampleDesc.Quality &&
		   a.Layout == b.Layout &&
		   a.Flags == b.Flags;
}

bool D3D12ShouldTraceGpu()
{
	return D3D12GetTraceGpuStatsMode() != D3D12TraceGpuStatsMode::Off;
}

bool D3D12TraceGpuSamplesOnly()
{
	static const bool enabled = []()
	{
		return false;
	}();
	return enabled;
}

bool D3D12TraceFocusedEventsOnly()
{
	static const bool enabled = []()
	{
		return false;
	}();
	return enabled;
}

bool D3D12TraceGpuStatsIsHighFrequency(std::string_view eventName)
{
	return eventName == "fbo_attachment_bind" ||
		   eventName == "draw_fbo_attachment_before" ||
		   eventName == "draw_fbo_attachment_after" ||
		   eventName == "draw_texture_binding";
}

bool D3D12TraceGpuStatsIsUsefulSample(std::string_view eventName)
{
	return eventName == "output_source_before" ||
		   eventName == "expand_depth_after" ||
		   eventName == "texture_load" ||
		   eventName == "texture_clear_color" ||
		   eventName == "texture_clear_depth" ||
		   eventName == "texture_copy_src_before" ||
		   eventName == "texture_copy_dst_after" ||
		   eventName == "texture_copy_raw_alias_src_before" ||
		   eventName == "texture_copy_raw_alias_dst_after" ||
		   eventName == "surface_copy_src_before" ||
		   eventName == "surface_copy_dst_after";
}

bool D3D12TraceGpuStatsIgnoresSampleLimit(std::string_view eventName)
{
	return eventName == "output_source_before";
}

bool D3D12ParseUnsignedToken(std::string_view text, uint64& value)
{
	if (text.empty())
		return false;

	uint32 base = 10;
	size_t offset = 0;
	if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X'))
	{
		base = 16;
		offset = 2;
	}

	uint64 parsed = 0;
	bool any = false;
	for (size_t i = offset; i < text.size(); i++)
	{
		const char ch = text[i];
		uint32 digit = 0;
		if (ch >= '0' && ch <= '9')
			digit = (uint32)(ch - '0');
		else if (base == 16 && ch >= 'a' && ch <= 'f')
			digit = (uint32)(ch - 'a' + 10);
		else if (base == 16 && ch >= 'A' && ch <= 'F')
			digit = (uint32)(ch - 'A' + 10);
		else
			break;
		if (digit >= base)
			return false;
		parsed = parsed * base + digit;
		any = true;
	}
	if (!any)
		return false;
	value = parsed;
	return true;
}

#if defined(CEMU_UWP_D3D12_DIAGNOSTICS)
bool D3D12ParseHexToken(std::string_view text, uint64& value)
{
	if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X'))
		text.remove_prefix(2);
	if (text.empty())
		return false;

	uint64 parsed = 0;
	for (const char ch : text)
	{
		uint32 digit = 0;
		if (ch >= '0' && ch <= '9')
			digit = (uint32)(ch - '0');
		else if (ch >= 'a' && ch <= 'f')
			digit = (uint32)(ch - 'a' + 10);
		else if (ch >= 'A' && ch <= 'F')
			digit = (uint32)(ch - 'A' + 10);
		else
			return false;
		if (parsed > (std::numeric_limits<uint64>::max() - digit) / 16)
			return false;
		parsed = parsed * 16 + digit;
	}
	value = parsed;
	return true;
}

template <size_t N>
void D3D12TraceReadUwpGpuCaptureAddresses(std::array<uint64, N>& values, uint32& count)
{
	std::ifstream marker(D3D12UwpGpuCapturePath(), std::ios::binary);
	std::string token;
	while (count < values.size() && marker >> token)
	{
		uint64 parsed = 0;
		if (D3D12ParseHexToken(token, parsed))
			values[count++] = parsed;
	}
}
#endif

bool D3D12TracePayloadValue(std::string_view payload, std::string_view key, uint64& value)
{
	const size_t pos = payload.find(key);
	if (pos == std::string_view::npos)
		return false;
	size_t begin = pos + key.size();
	size_t end = begin;
	while (end < payload.size())
	{
		const char ch = payload[end];
		if (!((ch >= '0' && ch <= '9') ||
			  (ch >= 'a' && ch <= 'f') ||
			  (ch >= 'A' && ch <= 'F') ||
			  ch == 'x' || ch == 'X'))
			break;
		end++;
	}
	return D3D12ParseUnsignedToken(payload.substr(begin, end - begin), value);
}

uint64 D3D12TraceReadEnvU64(const wchar_t* name)
{
	return std::numeric_limits<uint64>::max();
}

template <size_t N>
void D3D12TraceReadEnvU64List(const wchar_t* name, std::array<uint64, N>& values, uint32& count)
{
	return;
}

struct D3D12TraceGpuStatsFilter
{
	bool hasDrawMin{};
	bool hasDrawMax{};
	bool hasPipeline{};
	bool hasAddress{};
	uint64 drawMin{};
	uint64 drawMax{};
	uint64 address{};
	std::array<uint64, kTraceGpuStatsMaxPipelineFilters> pipelines{};
	uint32 pipelineCount{};
	std::array<uint64, kTraceGpuStatsMaxAddressFilters> addresses{};
	uint32 addressCount{};
};

const D3D12TraceGpuStatsFilter& D3D12TraceGpuFilter()
{
	static const D3D12TraceGpuStatsFilter filter = []()
	{
		D3D12TraceGpuStatsFilter result{};
		const uint64 drawMin = D3D12TraceReadEnvU64(L"CEMU_D3D12_TRACE_GPU_DRAW_MIN");
		const uint64 drawMax = D3D12TraceReadEnvU64(L"CEMU_D3D12_TRACE_GPU_DRAW_MAX");
		const uint64 pipeline = D3D12TraceReadEnvU64(L"CEMU_D3D12_TRACE_GPU_PIPELINE");
		const uint64 address = D3D12TraceReadEnvU64(L"CEMU_D3D12_TRACE_GPU_ADDR");
		D3D12TraceReadEnvU64List(L"CEMU_D3D12_TRACE_GPU_PIPELINES", result.pipelines, result.pipelineCount);
		D3D12TraceReadEnvU64List(L"CEMU_D3D12_TRACE_GPU_ADDRS", result.addresses, result.addressCount);
#if defined(CEMU_UWP_D3D12_DIAGNOSTICS)
		if (D3D12UwpGpuCaptureEnabled())
			D3D12TraceReadUwpGpuCaptureAddresses(result.addresses, result.addressCount);
#endif
		if (drawMin != std::numeric_limits<uint64>::max())
		{
			result.hasDrawMin = true;
			result.drawMin = drawMin;
		}
		if (drawMax != std::numeric_limits<uint64>::max())
		{
			result.hasDrawMax = true;
			result.drawMax = drawMax;
		}
		if (pipeline != std::numeric_limits<uint64>::max())
		{
			result.hasPipeline = true;
			if (result.pipelineCount < result.pipelines.size())
				result.pipelines[result.pipelineCount++] = pipeline;
		}
		if (address != std::numeric_limits<uint64>::max())
		{
			result.hasAddress = true;
			result.address = address;
			if (result.addressCount < result.addresses.size())
				result.addresses[result.addressCount++] = address;
		}
		if (result.addressCount > 0)
			result.hasAddress = true;
		if (result.pipelineCount > 0)
			result.hasPipeline = true;
		return result;
	}();
	return filter;
}

bool D3D12TraceGpuPipelineMatches(uint64 pipeline)
{
	const auto& filter = D3D12TraceGpuFilter();
	for (uint32 i = 0; i < filter.pipelineCount; ++i)
	{
		if (filter.pipelines[i] == pipeline)
			return true;
	}
	return false;
}

bool D3D12TraceGpuAddressMatches(uint64 address)
{
	const auto& filter = D3D12TraceGpuFilter();
	for (uint32 i = 0; i < filter.addressCount; ++i)
	{
		if (filter.addresses[i] == address)
			return true;
	}
	return false;
}

bool D3D12TracePayloadPipelineValue(std::string_view payload, uint64& value)
{
	return D3D12TracePayloadValue(payload, "pipeline=", value) ||
		   D3D12TracePayloadValue(payload, "pipelineKey=", value);
}

bool D3D12TracePayloadAddressMatch(std::string_view payload, bool& sawAddress)
{
	sawAddress = false;
	uint64 value = 0;
	const std::string_view keys[] = { "addr=", "src=", "dst=" };
	for (std::string_view key : keys)
	{
		size_t searchPos = 0;
		while (searchPos < payload.size())
		{
			const size_t pos = payload.find(key, searchPos);
			if (pos == std::string_view::npos)
				break;
			sawAddress = true;
			if (D3D12TracePayloadValue(payload.substr(pos), key, value) && D3D12TraceGpuAddressMatches(value))
				return true;
			searchPos = pos + key.size();
		}
	}
	return false;
}

struct D3D12DrawIsolationFilter
{
	bool enabled{};
	bool hasDraw{};
	bool hasDrawMin{};
	bool hasDrawMax{};
	bool hasPipeline{};
	bool hasVertexShader{};
	bool hasGeometryShader{};
	bool hasPixelShader{};
	bool hasRenderTarget{};
	uint64 draw{};
	uint64 drawMin{};
	uint64 drawMax{};
	uint64 pipeline{};
	uint64 vertexShader{};
	uint64 geometryShader{};
	uint64 pixelShader{};
	uint64 renderTarget{};
};

const D3D12DrawIsolationFilter& D3D12GetDrawIsolationFilter()
{
	static const D3D12DrawIsolationFilter filter = []()
	{
		D3D12DrawIsolationFilter result{};
		auto readValue = [&result](const wchar_t* name, bool& hasValue, uint64& value)
		{
			const uint64 parsed = D3D12TraceReadEnvU64(name);
			if (parsed != std::numeric_limits<uint64>::max())
			{
				hasValue = true;
				value = parsed;
				result.enabled = true;
			}
		};
		readValue(L"CEMU_D3D12_SKIP_DRAW", result.hasDraw, result.draw);
		readValue(L"CEMU_D3D12_SKIP_DRAW_MIN", result.hasDrawMin, result.drawMin);
		readValue(L"CEMU_D3D12_SKIP_DRAW_MAX", result.hasDrawMax, result.drawMax);
		readValue(L"CEMU_D3D12_SKIP_PIPELINE", result.hasPipeline, result.pipeline);
		readValue(L"CEMU_D3D12_SKIP_VS", result.hasVertexShader, result.vertexShader);
		readValue(L"CEMU_D3D12_SKIP_GS", result.hasGeometryShader, result.geometryShader);
		readValue(L"CEMU_D3D12_SKIP_PS", result.hasPixelShader, result.pixelShader);
		readValue(L"CEMU_D3D12_SKIP_RT", result.hasRenderTarget, result.renderTarget);
		return result;
	}();
	return filter;
}

bool D3D12DrawIsolationMatches(uint64 drawId, uint64 pipelineHash, uint64 vertexShaderHash, uint64 geometryShaderHash, uint64 pixelShaderHash, uint64 renderTargetAddress)
{
	const auto& filter = D3D12GetDrawIsolationFilter();
	if (!filter.enabled)
		return false;
	if (filter.hasDraw && drawId != filter.draw)
		return false;
	if (filter.hasDrawMin && drawId < filter.drawMin)
		return false;
	if (filter.hasDrawMax && drawId > filter.drawMax)
		return false;
	if (filter.hasPipeline && pipelineHash != filter.pipeline)
		return false;
	if (filter.hasVertexShader && vertexShaderHash != filter.vertexShader)
		return false;
	if (filter.hasGeometryShader && geometryShaderHash != filter.geometryShader)
		return false;
	if (filter.hasPixelShader && pixelShaderHash != filter.pixelShader)
		return false;
	if (filter.hasRenderTarget && renderTargetAddress != filter.renderTarget)
		return false;
	return true;
}

bool D3D12TraceGpuStatsMatchesFilter(std::string_view payload)
{
	const auto& filter = D3D12TraceGpuFilter();
	if (!filter.hasDrawMin && !filter.hasDrawMax && !filter.hasPipeline && !filter.hasAddress)
		return false;

	bool matchedAny = false;
	if (filter.hasDrawMin || filter.hasDrawMax)
	{
		uint64 draw = 0;
		if (D3D12TracePayloadValue(payload, "draw=", draw))
		{
			if (filter.hasDrawMin && draw < filter.drawMin)
				return false;
			if (filter.hasDrawMax && draw > filter.drawMax)
				return false;
			matchedAny = true;
		}
	}

	if (filter.hasPipeline)
	{
		uint64 pipeline = 0;
		if (D3D12TracePayloadPipelineValue(payload, pipeline))
		{
			if (!D3D12TraceGpuPipelineMatches(pipeline))
				return false;
			matchedAny = true;
		}
	}
	if (filter.hasAddress)
	{
		bool sawAddress = false;
		const bool addressMatched = D3D12TracePayloadAddressMatch(payload, sawAddress);
		if (sawAddress)
		{
			if (!addressMatched)
				return false;
			matchedAny = true;
		}
	}
	return matchedAny;
}

uint64 D3D12TraceGpuLimit(D3D12TraceGpuStatsMode mode)
{
	static const uint64 limit = []()
	{
		const auto mode = D3D12GetTraceGpuStatsMode();
		if (mode == D3D12TraceGpuStatsMode::Full)
			return std::numeric_limits<uint64>::max();
		if (mode == D3D12TraceGpuStatsMode::Focused)
			return kTraceGpuStatsUwpFocusedDefaultLimit;
		return kTraceGpuStatsSampledDefaultLimit;
	}();
	(void)mode;
	return limit;
}

class D3D12TraceSink
{
public:
	static D3D12TraceSink& Get()
	{
		static D3D12TraceSink sink;
		return sink;
	}

	bool Enabled() const
	{
		return m_enabled;
	}

#if defined(CEMU_UWP_D3D12_DIAGNOSTICS)
	// render thread owns selection/quota changes; other writers only read the atomic ID
	bool BeginPriorityStencilDraw(uint64 draw, uint64 pipeline, bool eligible)
	{
		EndPriorityStencilDraw();
		if (!m_enabled || !m_gpuStatsArmActive.load(std::memory_order_relaxed) ||
			!m_priorityStencilBudget.TrySelect(draw, pipeline, eligible))
			return false;
		m_priorityStencilPipeline.store(pipeline, std::memory_order_relaxed);
		m_priorityStencilEventsRemaining.store(D3D12DiagnosticDrawContextPolicy::kEventsPerDraw, std::memory_order_relaxed);
		m_priorityStencilDraw.store(draw, std::memory_order_release);
		return true;
	}

	void EndPriorityStencilDraw()
	{
		m_priorityStencilDraw.store(0, std::memory_order_release);
		m_priorityStencilEventsRemaining.store(0, std::memory_order_relaxed);
	}

	bool IsPriorityStencilDraw(uint64 draw) const
	{
		return draw != 0 && m_gpuStatsArmActive.load(std::memory_order_relaxed) &&
			m_priorityStencilDraw.load(std::memory_order_acquire) == draw;
	}
#endif

	bool GpuStatsEnabled() const
	{
		if (!m_enabled || m_gpuStatsMode == D3D12TraceGpuStatsMode::Off || m_gpuStatsCount >= m_gpuStatsLimit)
			return false;
#if defined(CEMU_UWP_D3D12_DIAGNOSTICS)
		if (m_gpuStatsMode == D3D12TraceGpuStatsMode::Focused && !m_gpuStatsArmActive)
			return false;
#endif
		return true;
	}

	bool CanReuseGpuStats(std::string_view eventName, std::string_view payload, uint32 primaryAddress) const
	{
		(void)primaryAddress;
		if (!GpuStatsEnabled())
			return false;
		if (m_gpuStatsMode == D3D12TraceGpuStatsMode::Focused)
		{
#if defined(CEMU_UWP_D3D12_DIAGNOSTICS)
			return m_gpuCaptureBudget.Check(eventName, primaryAddress) == D3D12DiagnosticCapturePolicy::Decision::Allow;
#else
			if (!D3D12TraceGpuStatsMatchesFilter(payload))
				return false;
#endif
		}
		return true;
	}

	bool PollGpuCaptureArm()
	{
#if defined(CEMU_UWP_D3D12_DIAGNOSTICS)
		if (!m_enabled || m_gpuStatsMode != D3D12TraceGpuStatsMode::Focused)
			return false;

		const auto now = std::chrono::steady_clock::now();
		if (now < m_gpuStatsArmNextPoll)
			return false;
		m_gpuStatsArmNextPoll = now + std::chrono::seconds(1);

		const std::string nonce = D3D12ReadUwpGpuCaptureArmNonce();
		if (D3D12UwpGpuCaptureNonceIsDisarmed(nonce))
		{
			m_gpuStatsArmActive = false;
			m_gpuCaptureBudget.Disarm();
			m_priorityStencilBudget.Disarm();
			EndPriorityStencilDraw();
			m_gpuStatsCaptureContextEventsRemaining.store(0, std::memory_order_relaxed);
			return false;
		}
		if (nonce == m_gpuStatsArmNonce)
			return false;

		const uint64 oldCount = m_gpuStatsCount;
		m_gpuStatsArmNonce = nonce;
		m_gpuStatsCount = 0;
		m_gpuStatsHighFrequencyCount = 0;
		const auto& filter = D3D12TraceGpuFilter();
		std::array<uint32, D3D12DiagnosticCapturePolicy::kMaxAddresses> addresses{};
		bool validAddresses = filter.addressCount > 0 && filter.addressCount <= addresses.size();
		for (uint32 i = 0; validAddresses && i < filter.addressCount; ++i)
		{
			validAddresses = filter.addresses[i] != 0 && filter.addresses[i] <= std::numeric_limits<uint32>::max();
			addresses[i] = (uint32)filter.addresses[i];
		}
		m_gpuCaptureBudget.Disarm();
		m_gpuStatsArmActive = validAddresses && m_gpuCaptureBudget.Rearm(addresses.data(), filter.addressCount);
		EndPriorityStencilDraw();
		m_priorityStencilBudget.Rearm();
		if (!m_gpuStatsArmActive.load(std::memory_order_relaxed))
			m_priorityStencilBudget.Disarm();
		m_gpuStatsCaptureContextEventsRemaining.store(kGpuStatsCaptureContextEventBudget, std::memory_order_relaxed);
		m_gpuStatsArmGeneration++;
		WriteAndFlush("gpu_capture_rearm",
			fmt::format("generation={} nonceHash=0x{:016x} previousCount={} limit={} mode={} eventFilter=completed_fbo,output,completed_copy cacheInvalidation=requested contextEventBudget={} armed={} perAddressLimit=8 addressCount={} stencilContextDrawLimit=64 stencilContextPerPipeline=2 stencilContextEventsPerDraw=128",
				(unsigned long long)m_gpuStatsArmGeneration,
				(unsigned long long)D3D12HashUwpGpuCaptureNonce(nonce),
				(unsigned long long)oldCount,
				(unsigned long long)m_gpuStatsLimit,
				D3D12TraceGpuStatsModeName(m_gpuStatsMode),
				kGpuStatsCaptureContextEventBudget, m_gpuStatsArmActive.load(std::memory_order_relaxed), filter.addressCount));
		return true;
#else
		return false;
#endif
	}

	uint64 GpuCaptureGeneration() const
	{
#if defined(CEMU_UWP_D3D12_DIAGNOSTICS)
		return m_gpuStatsArmGeneration;
#else
		return 0;
#endif
	}

	D3D12TraceGpuStatsDecision TryBeginGpuStats(std::string_view eventName, std::string_view payload, uint32 primaryAddress)
	{
		(void)primaryAddress;
		if (!m_enabled || m_gpuStatsMode == D3D12TraceGpuStatsMode::Off)
			return { false, "trace_gpu_off" };

#if defined(CEMU_UWP_D3D12_DIAGNOSTICS)
		if (m_gpuStatsMode == D3D12TraceGpuStatsMode::Focused && !m_gpuStatsArmActive)
			return { false, "gpu_capture_disarmed" };
		if (m_gpuStatsMode == D3D12TraceGpuStatsMode::Focused)
		{
			const auto decision = m_gpuCaptureBudget.TryAdmit(eventName, primaryAddress);
			const bool admitted = decision == D3D12DiagnosticCapturePolicy::Decision::Allow;
			if (admitted)
				++m_gpuStatsCount;
			if (m_gpuCaptureBudget.IsExhausted())
			{
				m_gpuStatsArmActive = false;
				m_gpuStatsCaptureContextEventsRemaining.store(0, std::memory_order_relaxed);
				m_gpuCaptureBudget.Disarm();
				m_priorityStencilBudget.Disarm();
				EndPriorityStencilDraw();
				WriteAndFlush("gpu_capture_quota_reached", fmt::format("generation={} admittedSamples={} lastAddr=0x{:08x}",
					m_gpuStatsArmGeneration, m_gpuStatsCount, primaryAddress));
			}
			return { admitted, D3D12DiagnosticCapturePolicy::DecisionName(decision) };
		}
#endif

		const bool ignoreSampleLimit = m_gpuStatsMode == D3D12TraceGpuStatsMode::Sampled && D3D12TraceGpuStatsIgnoresSampleLimit(eventName);
		if (!ignoreSampleLimit && m_gpuStatsCount >= m_gpuStatsLimit)
		{
#if defined(CEMU_UWP_D3D12_DIAGNOSTICS)
			m_gpuStatsCaptureContextEventsRemaining.store(0, std::memory_order_relaxed);
#endif
			return { false, "trace_gpu_limit" };
		}

		const bool focusedMatch = m_gpuStatsMode == D3D12TraceGpuStatsMode::Focused && D3D12TraceGpuStatsMatchesFilter(payload);
		if (m_gpuStatsMode == D3D12TraceGpuStatsMode::Focused && !focusedMatch)
			return { false, "focused_filter_metadata_only" };

#if defined(CEMU_UWP_D3D12_DIAGNOSTICS)
		if (m_gpuStatsMode == D3D12TraceGpuStatsMode::Focused)
		{
			if (!D3D12TraceIsUwpArmedGpuCaptureEvent(eventName))
				return { false, "gpu_capture_event_metadata_only" };
		}
#endif

		if (m_gpuStatsMode == D3D12TraceGpuStatsMode::Sampled)
		{
			if (!ignoreSampleLimit && D3D12TraceGpuStatsIsHighFrequency(eventName))
			{
				if (m_gpuStatsHighFrequencyCount >= 8)
					return { false, "sampled_high_frequency_metadata_only" };
				m_gpuStatsHighFrequencyCount++;
			}
			else if (!ignoreSampleLimit && !D3D12TraceGpuStatsIsUsefulSample(eventName))
			{
				return { false, "sampled_event_metadata_only" };
			}
		}

		m_gpuStatsCount++;
		return { true, focusedMatch ? "capture_focused_filter" : "capture" };
	}

	void Write(std::string_view eventName, std::string_view payload)
	{
		WriteImpl(eventName, payload, false);
	}

	void WriteAndFlush(std::string_view eventName, std::string_view payload)
	{
		WriteImpl(eventName, payload, true);
	}

private:
	void WriteImpl(std::string_view eventName, std::string_view payload, bool forceFlush)
	{
		if (!m_enabled)
			return;
#if defined(CEMU_UWP_D3D12_DIAGNOSTICS)
		// while disarmed, skip routine events so loading loops don't eat the file budget
		if (m_gpuStatsMode == D3D12TraceGpuStatsMode::Focused && !m_gpuStatsArmActive.load(std::memory_order_relaxed) &&
			!D3D12DiagnosticCapturePolicy::ShouldRetainWhileDisarmed(eventName) &&
			!D3D12TraceIsGpuStatsResult(eventName, payload))
			return;
#endif
		const uint64 eventNumber = ++m_eventsSeen;
#if defined(CEMU_UWP_D3D12_DIAGNOSTICS)
		// full draw/descriptor metadata reaches GB per minute; sample routine events
		bool gpuCaptureContext = false;
		bool priorityStencilContext = false;
		const uint64 selectedPriorityDraw = m_priorityStencilDraw.load(std::memory_order_acquire);
		const uint64 selectedPriorityPipeline = m_priorityStencilPipeline.load(std::memory_order_relaxed);
		const uint64 selectedPriorityDrawAgain = m_priorityStencilDraw.load(std::memory_order_acquire);
		if (selectedPriorityDraw != 0 && selectedPriorityDraw == selectedPriorityDrawAgain &&
			m_gpuStatsArmActive.load(std::memory_order_relaxed) &&
			D3D12DiagnosticDrawContextPolicy::Retains(eventName, payload, selectedPriorityDraw, selectedPriorityPipeline))
		{
			uint32 remaining = m_priorityStencilEventsRemaining.load(std::memory_order_relaxed);
			while (remaining != 0 && !m_priorityStencilEventsRemaining.compare_exchange_weak(remaining, remaining - 1,
				std::memory_order_relaxed, std::memory_order_relaxed)) {}
			priorityStencilContext = remaining != 0;
		}
		if (D3D12TraceIsGpuCaptureContextEvent(eventName))
		{
			uint32 remaining = m_gpuStatsCaptureContextEventsRemaining.load(std::memory_order_relaxed);
			while (remaining != 0 &&
				   !m_gpuStatsCaptureContextEventsRemaining.compare_exchange_weak(remaining, remaining - 1,
					   std::memory_order_relaxed, std::memory_order_relaxed))
			{
			}
			gpuCaptureContext = remaining != 0;
		}
		if (eventNumber > 8192 && !D3D12TraceIsPriorityEvent(eventName) &&
			!gpuCaptureContext && !priorityStencilContext && !D3D12TraceIsGpuStatsResult(eventName, payload) && (eventNumber & 0x3FF) != 0)
			return;
#endif
		EnsureOpen();
		if (!m_file)
			return;
		if (D3D12TraceFocusedEventsOnly() && !D3D12TraceGpuStatsMatchesFilter(payload)
#if defined(CEMU_UWP_D3D12_DIAGNOSTICS)
			&& !priorityStencilContext
#endif
			)
			return;
		const auto now = std::chrono::steady_clock::now();
		const uint64 elapsedUs = (uint64)std::chrono::duration_cast<std::chrono::microseconds>(now - m_startTime).count();
		std::lock_guard lock(m_mutex);
#if defined(CEMU_UWP_D3D12_DIAGNOSTICS)
		const uint64 recordBytes = (uint64)eventName.size() + (uint64)payload.size() + 64;
		if (m_bytesWritten + recordBytes > kUwpD3D12TraceMaxBytes)
		{
			m_file << ++m_sequence << '\t' << elapsedUs << "\ttrace_limit\tmax_bytes=" << kUwpD3D12TraceMaxBytes << '\n';
			m_file.flush();
			s_uwpD3D12TraceDisabled.store(true, std::memory_order_relaxed);
			return;
		}
		m_bytesWritten += recordBytes;
#endif
		m_file << ++m_sequence << '\t' << elapsedUs << '\t' << eventName << '\t' << payload << '\n';
#if defined(CEMU_UWP_D3D12_DIAGNOSTICS)
		// flush the final result even if admission already disarmed the generation
		if (m_gpuStatsMode == D3D12TraceGpuStatsMode::Focused &&
			payload.find(" stats=ok cached=0 ") != std::string_view::npos)
			forceFlush = true;
#endif
		if (forceFlush || (m_sequence & 0xFF) == 0)
			m_file.flush();
	}

	D3D12TraceSink() :
		m_enabled(D3D12ShouldTrace()),
		m_gpuStatsMode(D3D12GetTraceGpuStatsMode()),
		m_gpuStatsLimit(D3D12TraceGpuLimit(m_gpuStatsMode)),
		m_startTime(std::chrono::steady_clock::now())
	{
#if defined(CEMU_UWP_D3D12_DIAGNOSTICS)
		// a nonce present at startup is the disarmed baseline; only a later change arms
		m_gpuStatsArmNonce = D3D12ReadUwpGpuCaptureArmNonce();
		m_gpuStatsArmNextPoll = m_startTime + std::chrono::seconds(1);
#endif
	}

	void EnsureOpen()
	{
		if (m_openAttempted)
			return;
		m_openAttempted = true;
		try
		{
			const auto now = std::chrono::system_clock::now();
			const std::time_t nowTime = std::chrono::system_clock::to_time_t(now);
			std::tm localTime{};
			localtime_s(&localTime, &nowTime);
			const fs::path traceDir = ActiveSettings::GetUserDataPath("diagnostics");
			std::error_code ec;
			fs::create_directories(traceDir, ec);
			const fs::path tracePath = traceDir / fmt::format("d3d12_trace_{:04}{:02}{:02}_{:02}{:02}{:02}.tsv",
															 localTime.tm_year + 1900,
															 localTime.tm_mon + 1,
															 localTime.tm_mday,
															 localTime.tm_hour,
															 localTime.tm_min,
															 localTime.tm_sec);
			m_file.open(tracePath, std::ios::binary);
			if (m_file)
			{
				const auto& gpuFilter = D3D12TraceGpuFilter();
				std::string gpuAddressFilter = "none";
				if (gpuFilter.addressCount > 0)
				{
					gpuAddressFilter.clear();
					for (uint32 i = 0; i < gpuFilter.addressCount; ++i)
					{
						if (!gpuAddressFilter.empty())
							gpuAddressFilter.push_back(';');
						gpuAddressFilter += fmt::format("0x{:08x}", gpuFilter.addresses[i]);
					}
				}
				std::string gpuPipelineFilter = "none";
				if (gpuFilter.pipelineCount > 0)
				{
					gpuPipelineFilter.clear();
					for (uint32 i = 0; i < gpuFilter.pipelineCount; ++i)
					{
						if (!gpuPipelineFilter.empty())
							gpuPipelineFilter.push_back(';');
						gpuPipelineFilter += fmt::format("0x{:016x}", gpuFilter.pipelines[i]);
					}
				}
				m_file << "seq\telapsed_us\tevent\tdata\n";
				m_file << "0\t0\ttrace_begin\tgpu_stats=" << (m_gpuStatsMode != D3D12TraceGpuStatsMode::Off ? 1 : 0)
					   << " gpu_stats_mode=" << D3D12TraceGpuStatsModeName(m_gpuStatsMode)
					   << " gpu_limit=" << m_gpuStatsLimit
					   << " gpu_draw_min=" << (gpuFilter.hasDrawMin ? fmt::format("{}", gpuFilter.drawMin) : "none")
					   << " gpu_draw_max=" << (gpuFilter.hasDrawMax ? fmt::format("{}", gpuFilter.drawMax) : "none")
					   << " gpu_pipeline=" << gpuPipelineFilter
					   << " gpu_addr=" << gpuAddressFilter
					   << " gpu_event_filter=" << (D3D12TraceFocusedEventsOnly() ? "focused_only" : "all")
					   << " path=" << tracePath.generic_string() << '\n';
				m_file.flush();
				cemuLog_log(LogType::Force, "D3D12 trace writing to {}", tracePath.generic_string());
			}
		}
		catch (...)
		{
		}
	}

	bool m_enabled{};
	D3D12TraceGpuStatsMode m_gpuStatsMode{};
	uint64 m_gpuStatsLimit{};
	uint64 m_gpuStatsCount{};
	uint64 m_gpuStatsHighFrequencyCount{};
	std::atomic<uint64> m_eventsSeen{};
	bool m_openAttempted{};
	uint64 m_sequence{};
#if defined(CEMU_UWP_D3D12_DIAGNOSTICS)
	uint64 m_bytesWritten{};
#endif
#if defined(CEMU_UWP_D3D12_DIAGNOSTICS)
	std::string m_gpuStatsArmNonce;
	std::chrono::steady_clock::time_point m_gpuStatsArmNextPoll{};
	uint64 m_gpuStatsArmGeneration{};
	std::atomic<uint32> m_gpuStatsCaptureContextEventsRemaining{};
	std::atomic_bool m_gpuStatsArmActive{ false };
	D3D12DiagnosticCapturePolicy::CaptureBudget m_gpuCaptureBudget;
	D3D12DiagnosticDrawContextPolicy::Budget m_priorityStencilBudget;
	std::atomic<uint64> m_priorityStencilDraw{};
	std::atomic<uint64> m_priorityStencilPipeline{};
	std::atomic<uint32> m_priorityStencilEventsRemaining{};
#endif
	std::chrono::steady_clock::time_point m_startTime;
	std::ofstream m_file;
	std::mutex m_mutex;
};

#if !defined(CEMU_UWP_D3D12_DIAGNOSTICS)
#define D3D12TraceEvent(...) ((void)0)
#define D3D12TraceEventFlush(...) ((void)0)
#else
template <typename... Args>
void D3D12TraceEvent(const char* eventName, fmt::format_string<Args...> format, Args&&... args)
{
	if (!D3D12ShouldTrace())
		return;
#if defined(CEMU_UWP_D3D12_DIAGNOSTICS)
	if (D3D12TraceEventIsSuppressed(eventName))
		return;
	try
	{
#endif
		D3D12TraceSink::Get().Write(eventName, fmt::format(format, std::forward<Args>(args)...));
#if defined(CEMU_UWP_D3D12_DIAGNOSTICS)
	}
	catch (const fmt::format_error& ex)
	{
		D3D12SuppressInvalidTraceEvent(eventName, ex.what());
	}
#endif
}

template <typename... Args>
void D3D12TraceEventFlush(const char* eventName, fmt::format_string<Args...> format, Args&&... args)
{
	if (!D3D12ShouldTrace())
		return;
#if defined(CEMU_UWP_D3D12_DIAGNOSTICS)
	if (D3D12TraceEventIsSuppressed(eventName))
		return;
	try
	{
#endif
		D3D12TraceSink::Get().WriteAndFlush(eventName, fmt::format(format, std::forward<Args>(args)...));
#if defined(CEMU_UWP_D3D12_DIAGNOSTICS)
	}
	catch (const fmt::format_error& ex)
	{
		D3D12SuppressInvalidTraceEvent(eventName, ex.what());
	}
#endif
}
#endif

uint32 D3D12ChecksumBytesPerPixel(DXGI_FORMAT format)
{
	switch (format)
	{
	case DXGI_FORMAT_R8_UNORM:
	case DXGI_FORMAT_R8_UINT:
	case DXGI_FORMAT_R8_SNORM:
	case DXGI_FORMAT_R8_SINT:
		return 1;
	case DXGI_FORMAT_R8G8_UNORM:
	case DXGI_FORMAT_R8G8_UINT:
	case DXGI_FORMAT_R8G8_SNORM:
	case DXGI_FORMAT_R8G8_SINT:
	case DXGI_FORMAT_R16_UNORM:
	case DXGI_FORMAT_R16_UINT:
	case DXGI_FORMAT_R16_SNORM:
	case DXGI_FORMAT_R16_SINT:
	case DXGI_FORMAT_R16_FLOAT:
		return 2;
	case DXGI_FORMAT_R8G8B8A8_UNORM:
	case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
	case DXGI_FORMAT_R8G8B8A8_UINT:
	case DXGI_FORMAT_R8G8B8A8_SNORM:
	case DXGI_FORMAT_R8G8B8A8_SINT:
	case DXGI_FORMAT_B8G8R8A8_UNORM:
	case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
	case DXGI_FORMAT_R10G10B10A2_UNORM:
	case DXGI_FORMAT_R10G10B10A2_UINT:
	case DXGI_FORMAT_R16G16_UNORM:
	case DXGI_FORMAT_R16G16_UINT:
	case DXGI_FORMAT_R16G16_SNORM:
	case DXGI_FORMAT_R16G16_SINT:
	case DXGI_FORMAT_R16G16_FLOAT:
	case DXGI_FORMAT_R32_UINT:
	case DXGI_FORMAT_R32_SINT:
	case DXGI_FORMAT_R32_FLOAT:
		return 4;
	case DXGI_FORMAT_R16G16B16A16_UNORM:
	case DXGI_FORMAT_R16G16B16A16_UINT:
	case DXGI_FORMAT_R16G16B16A16_SNORM:
	case DXGI_FORMAT_R16G16B16A16_SINT:
	case DXGI_FORMAT_R16G16B16A16_FLOAT:
	case DXGI_FORMAT_R32G32_UINT:
	case DXGI_FORMAT_R32G32_SINT:
	case DXGI_FORMAT_R32G32_FLOAT:
		return 8;
	case DXGI_FORMAT_R32G32B32A32_UINT:
	case DXGI_FORMAT_R32G32B32A32_SINT:
	case DXGI_FORMAT_R32G32B32A32_FLOAT:
		return 16;
	default:
		return 4;
	}
}

std::vector<std::pair<uint32, uint32>> D3D12TraceGpuSamplePoints(uint32 width, uint32 height)
{
	std::vector<std::pair<uint32, uint32>> points;
	if (width == 0 || height == 0)
		return points;


	const std::array<std::pair<uint32, uint32>, 17> normalized =
	{ {
		{ 1, 16 }, { 1, 8 }, { 3, 16 }, { 1, 4 }, { 5, 16 }, { 3, 8 },
		{ 7, 16 }, { 1, 2 }, { 9, 16 }, { 5, 8 }, { 11, 16 }, { 3, 4 },
		{ 13, 16 }, { 7, 8 }, { 15, 16 }, { 1, 3 }, { 2, 3 },
	} };
	points.reserve(normalized.size() + 5);
	for (const auto& [nx, ny] : normalized)
	{
		const uint32 x = std::min<uint32>((uint64)(width - 1) * nx / 16, width - 1);
		const uint32 y = std::min<uint32>((uint64)(height - 1) * ny / 16, height - 1);
		if (std::find(points.begin(), points.end(), std::pair<uint32, uint32>{ x, y }) == points.end())
			points.emplace_back(x, y);
	}
	const std::array<std::pair<uint32, uint32>, 5> anchors =
	{ {
		{ 0, 0 },
		{ width - 1, 0 },
		{ width / 2, height / 2 },
		{ 0, height - 1 },
		{ width - 1, height - 1 },
	} };
	for (const auto& point : anchors)
	{
		if (std::find(points.begin(), points.end(), point) == points.end())
			points.emplace_back(point);
	}
	return points;
}

struct D3D12TraceGpuGridConfig
{
	bool enabled{};
	uint32 x{};
	uint32 y{};
	uint32 width{};
	uint32 height{};
	uint32 columns{};
	uint32 rows{};
};

const D3D12TraceGpuGridConfig& D3D12TraceGpuGrid()
{
	static const D3D12TraceGpuGridConfig config = []()
	{
		D3D12TraceGpuGridConfig result{};
		return result;
	}();
	return config;
}

std::string D3D12BuildTraceGpuGridPayload(const uint8* data, size_t dataBytes, uint32 width, uint32 height, uint32 bytesPerPixel, bool compressed)
{
	const auto& grid = D3D12TraceGpuGrid();
	if (!grid.enabled)
		return {};
	if (!data || dataBytes == 0 || width == 0 || height == 0 || bytesPerPixel == 0 || compressed)
		return " grid=unavailable";

	const uint32 x0 = std::min<uint32>(grid.x, width - 1);
	const uint32 y0 = std::min<uint32>(grid.y, height - 1);
	const uint32 regionWidth = std::min<uint32>(grid.width == 0 ? width - x0 : grid.width, width - x0);
	const uint32 regionHeight = std::min<uint32>(grid.height == 0 ? height - y0 : grid.height, height - y0);
	if (regionWidth == 0 || regionHeight == 0)
		return " grid=empty";

	std::string rows;
	rows.reserve((size_t)grid.rows * ((size_t)grid.columns + 1));
	uint32 rgbCells = 0;
	uint32 alphaOnlyCells = 0;
	for (uint32 row = 0; row < grid.rows; ++row)
	{
		if (row != 0)
			rows.push_back('|');
		const uint32 y = y0 + std::min<uint32>(regionHeight - 1, (uint32)(((uint64)row * 2 + 1) * regionHeight / ((uint64)grid.rows * 2)));
		for (uint32 column = 0; column < grid.columns; ++column)
		{
			const uint32 x = x0 + std::min<uint32>(regionWidth - 1, (uint32)(((uint64)column * 2 + 1) * regionWidth / ((uint64)grid.columns * 2)));
			const size_t offset = ((size_t)y * width + x) * bytesPerPixel;
			if (offset >= dataBytes)
			{
				rows.push_back('?');
				continue;
			}
			const uint8* pixel = data + offset;
			bool hasRgb = false;
			bool hasAlphaOnly = false;
			if (bytesPerPixel >= 4)
			{
				hasRgb = (pixel[0] | pixel[1] | pixel[2]) != 0;
				hasAlphaOnly = !hasRgb && pixel[3] != 0;
			}
			else
			{
				for (uint32 b = 0; b < bytesPerPixel && offset + b < dataBytes; ++b)
					hasRgb |= pixel[b] != 0;
			}
			if (hasRgb)
			{
				rows.push_back('#');
				rgbCells++;
			}
			else if (hasAlphaOnly)
			{
				rows.push_back('+');
				alphaOnlyCells++;
			}
			else
			{
				rows.push_back('.');
			}
		}
	}

	return fmt::format(" gridOrigin={},{} gridSize={}x{} gridDim={}x{} gridRgbCells={} gridAlphaOnlyCells={} gridRows={}",
					   x0,
					   y0,
					   regionWidth,
					   regionHeight,
					   grid.columns,
					   grid.rows,
					   rgbCells,
					   alphaOnlyCells,
					   rows);
}

std::string D3D12BuildTraceGpuSamplesPayload(const uint8* data, size_t dataBytes, uint32 width, uint32 height, uint32 bytesPerPixel, DXGI_FORMAT viewFormat, bool compressed)
{
	if (!data || dataBytes == 0 || width == 0 || height == 0 || bytesPerPixel == 0 || compressed)
		return "samples=unavailable";

	const auto points = D3D12TraceGpuSamplePoints(width, height);
	if (points.empty())
		return "samples=none";

	std::string result = "samples=";
	bool first = true;
	for (size_t i = 0; i < points.size(); ++i)
	{
		const auto [x, y] = points[i];
		const size_t offset = ((size_t)y * width + x) * bytesPerPixel;
		if (offset >= dataBytes)
			continue;

		if (!first)
			result += ';';
		first = false;

		const uint8* pixel = data + offset;
		const size_t available = std::min<size_t>(bytesPerPixel, dataBytes - offset);
		result += fmt::format("p{}@{},{}=", i, x, y);
		for (size_t b = 0; b < available && b < 16; ++b)
			result += fmt::format("{:02x}", pixel[b]);
		if (bytesPerPixel == 4 && available >= 4)
		{
			uint32 raw = 0;
			float asFloat = 0.0f;
			std::memcpy(&raw, pixel, sizeof(raw));
			std::memcpy(&asFloat, pixel, sizeof(asFloat));
			result += fmt::format("/u32=0x{:08x}", raw);
			if (viewFormat == DXGI_FORMAT_R32_FLOAT)
				result += fmt::format("/f={:.9g}", asFloat);
			else
				result += fmt::format("/rgba={},{},{},{}", (uint32)pixel[0], (uint32)pixel[1], (uint32)pixel[2], (uint32)pixel[3]);
		}
	}
	if (first)
		return "samples=none_in_range";
	return result;
}

void D3D12DumpHLSL(RendererShader::ShaderType type, uint64 baseHash, uint64 auxHash, bool isGameShader, const std::string& source)
{
	const bool dumpEnabled = D3D12ShouldDumpHLSL();
	if (!isGameShader || source.empty() || !dumpEnabled)
		return;

	try
	{
		const fs::path dumpDir = ActiveSettings::GetUserDataPath("shaderCache/d3d12_hlsl_dump");
		std::error_code ec;
		fs::create_directories(dumpDir, ec);
		if (ec)
			return;

		const char* stage = type == RendererShader::ShaderType::kVertex ? "vs" :
							type == RendererShader::ShaderType::kGeometry ? "gs" : "ps";
		const fs::path path = dumpDir / fmt::format("{}_{:016x}_{:016x}.hlsl", stage, baseHash, auxHash);
		std::ofstream file(path, std::ios::binary);
		if (file)
			file.write(source.data(), (std::streamsize)source.size());
	}
	catch (...)
	{
	}
}

const char* D3D12MessageSeverityName(D3D12_MESSAGE_SEVERITY severity)
{
	switch (severity)
	{
	case D3D12_MESSAGE_SEVERITY_CORRUPTION:
		return "corruption";
	case D3D12_MESSAGE_SEVERITY_ERROR:
		return "error";
	case D3D12_MESSAGE_SEVERITY_WARNING:
		return "warning";
	case D3D12_MESSAGE_SEVERITY_INFO:
		return "info";
	case D3D12_MESSAGE_SEVERITY_MESSAGE:
		return "message";
	default:
		return "unknown";
	}
}

enum class D3D12LogicOpEmulationMode : uint32
{
	Disabled = 0,
	Clear = 1,
	Or = 2,
	Set = 3,
	CopyInverted = 4,
	Noop = 5,
	Invert = 6,
	And = 7,
	Nand = 8,
	Nor = 9,
	Xor = 10,
	Equiv = 11,
	AndReverse = 12,
	AndInverted = 13,
	OrReverse = 14,
	OrInverted = 15,
};

bool TryGetD3D12LogicOp(Latte::LATTE_CB_COLOR_CONTROL::E_LOGICOP logicOp, D3D12_LOGIC_OP& d3dLogicOp)
{
	// the ROP byte is the full two-input truth table, same sixteen ops as D3D12
	switch (static_cast<uint32>(logicOp))
	{
	case 0x00: d3dLogicOp = D3D12_LOGIC_OP_CLEAR; return true;
	case 0xFF: d3dLogicOp = D3D12_LOGIC_OP_SET; return true;
	case 0xCC: d3dLogicOp = D3D12_LOGIC_OP_COPY; return true;
	case 0x33: d3dLogicOp = D3D12_LOGIC_OP_COPY_INVERTED; return true;
	case 0xAA: d3dLogicOp = D3D12_LOGIC_OP_NOOP; return true;
	case 0x55: d3dLogicOp = D3D12_LOGIC_OP_INVERT; return true;
	case 0x88: d3dLogicOp = D3D12_LOGIC_OP_AND; return true;
	case 0x77: d3dLogicOp = D3D12_LOGIC_OP_NAND; return true;
	case 0xEE: d3dLogicOp = D3D12_LOGIC_OP_OR; return true;
	case 0x11: d3dLogicOp = D3D12_LOGIC_OP_NOR; return true;
	case 0x66: d3dLogicOp = D3D12_LOGIC_OP_XOR; return true;
	case 0x99: d3dLogicOp = D3D12_LOGIC_OP_EQUIV; return true;
	case 0x44: d3dLogicOp = D3D12_LOGIC_OP_AND_REVERSE; return true;
	case 0x22: d3dLogicOp = D3D12_LOGIC_OP_AND_INVERTED; return true;
	case 0xDD: d3dLogicOp = D3D12_LOGIC_OP_OR_REVERSE; return true;
	case 0xBB: d3dLogicOp = D3D12_LOGIC_OP_OR_INVERTED; return true;
	default: return false;
	}
}

uint32 GetD3D12LogicOpEmulationMode(D3D12_LOGIC_OP logicOp)
{
	switch (logicOp)
	{
	case D3D12_LOGIC_OP_CLEAR: return static_cast<uint32>(D3D12LogicOpEmulationMode::Clear);
	case D3D12_LOGIC_OP_OR: return static_cast<uint32>(D3D12LogicOpEmulationMode::Or);
	case D3D12_LOGIC_OP_SET: return static_cast<uint32>(D3D12LogicOpEmulationMode::Set);
	case D3D12_LOGIC_OP_COPY_INVERTED: return static_cast<uint32>(D3D12LogicOpEmulationMode::CopyInverted);
	case D3D12_LOGIC_OP_NOOP: return static_cast<uint32>(D3D12LogicOpEmulationMode::Noop);
	case D3D12_LOGIC_OP_INVERT: return static_cast<uint32>(D3D12LogicOpEmulationMode::Invert);
	case D3D12_LOGIC_OP_AND: return static_cast<uint32>(D3D12LogicOpEmulationMode::And);
	case D3D12_LOGIC_OP_NAND: return static_cast<uint32>(D3D12LogicOpEmulationMode::Nand);
	case D3D12_LOGIC_OP_NOR: return static_cast<uint32>(D3D12LogicOpEmulationMode::Nor);
	case D3D12_LOGIC_OP_XOR: return static_cast<uint32>(D3D12LogicOpEmulationMode::Xor);
	case D3D12_LOGIC_OP_EQUIV: return static_cast<uint32>(D3D12LogicOpEmulationMode::Equiv);
	case D3D12_LOGIC_OP_AND_REVERSE: return static_cast<uint32>(D3D12LogicOpEmulationMode::AndReverse);
	case D3D12_LOGIC_OP_AND_INVERTED: return static_cast<uint32>(D3D12LogicOpEmulationMode::AndInverted);
	case D3D12_LOGIC_OP_OR_REVERSE: return static_cast<uint32>(D3D12LogicOpEmulationMode::OrReverse);
	case D3D12_LOGIC_OP_OR_INVERTED: return static_cast<uint32>(D3D12LogicOpEmulationMode::OrInverted);
	case D3D12_LOGIC_OP_COPY:
	default:
		return static_cast<uint32>(D3D12LogicOpEmulationMode::Disabled);
	}
}

uint32 GetD3D12EffectiveRenderTargetMask(const LatteContextRegister& lcr)
{
	if (lcr.CB_COLOR_CONTROL.get_SPECIAL_OP() == Latte::LATTE_CB_COLOR_CONTROL::E_SPECIALOP::DISABLE)
		return 0;
	return lcr.CB_TARGET_MASK.get_MASK();
}

bool D3D12LogicOpEmulationNeedsDestination(uint32 mode)
{
	return mode != static_cast<uint32>(D3D12LogicOpEmulationMode::Disabled) &&
		mode != static_cast<uint32>(D3D12LogicOpEmulationMode::Clear) &&
		mode != static_cast<uint32>(D3D12LogicOpEmulationMode::Set) &&
		mode != static_cast<uint32>(D3D12LogicOpEmulationMode::CopyInverted);
}

bool SupportsUnorm8LogicOpEmulation(DXGI_FORMAT format)
{
	switch (format)
	{
	case DXGI_FORMAT_R8_UNORM:
	case DXGI_FORMAT_R8G8_UNORM:
	case DXGI_FORMAT_R8G8B8A8_UNORM:
	case DXGI_FORMAT_B8G8R8A8_UNORM:
	case DXGI_FORMAT_B8G8R8X8_UNORM:
	case DXGI_FORMAT_B5G6R5_UNORM:
	case DXGI_FORMAT_B5G5R5A1_UNORM:
		return true;
	default:
		return false;
	}
}

// logic ops do not apply to float and sRGB targets, the fragment color is written unchanged
bool D3D12LogicOpIgnoresFormat(DXGI_FORMAT format)
{
	switch (format)
	{
	case DXGI_FORMAT_R32G32B32A32_FLOAT:
	case DXGI_FORMAT_R16G16B16A16_FLOAT:
	case DXGI_FORMAT_R32G32_FLOAT:
	case DXGI_FORMAT_R11G11B10_FLOAT:
	case DXGI_FORMAT_R16G16_FLOAT:
	case DXGI_FORMAT_R32_FLOAT:
	case DXGI_FORMAT_R16_FLOAT:
	case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
	case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
	case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
		return true;
	default:
		return false;
	}
}

bool SupportsLogicOpEmulationWithoutDestination(DXGI_FORMAT format, uint32 mode)
{
	switch (format)
	{
	case DXGI_FORMAT_R10G10B10A2_UNORM:
	case DXGI_FORMAT_R16G16B16A16_UNORM:
	case DXGI_FORMAT_R16G16_UNORM:
	case DXGI_FORMAT_R16_UNORM:
	case DXGI_FORMAT_B5G6R5_UNORM:
	case DXGI_FORMAT_B5G5R5A1_UNORM:
	case DXGI_FORMAT_B4G4R4A4_UNORM:
	case DXGI_FORMAT_A8_UNORM:
		// CLEAR/SET are exact at any UNORM width, COPY_INVERTED is 1 - src at 8-bit precision
		return !D3D12LogicOpEmulationNeedsDestination(mode);
	case DXGI_FORMAT_R16G16B16A16_SNORM:
	case DXGI_FORMAT_R16G16_SNORM:
	case DXGI_FORMAT_R16_SNORM:
	case DXGI_FORMAT_R8G8B8A8_SNORM:
	case DXGI_FORMAT_R8G8_SNORM:
	case DXGI_FORMAT_R8_SNORM:
		return mode == static_cast<uint32>(D3D12LogicOpEmulationMode::Clear);
	default:
		return false;
	}
}

#if defined(CEMU_UWP_D3D12_WORKLOAD_PROFILE) || defined(CEMU_UWP_PERFORMANCE_EXPERIMENT)
// Logical copy payload only: not allocation size, compression, or measured bus traffic.
uint64 D3D12WorkloadLogicCopyBytes(const D3D12_RESOURCE_DESC& desc, DXGI_FORMAT viewFormat)
{
	const uint64 channels = viewFormat == DXGI_FORMAT_R8_UNORM ? 1u :
		(viewFormat == DXGI_FORMAT_R8G8_UNORM ? 2u : 4u);
	return SupportsUnorm8LogicOpEmulation(viewFormat) ? desc.Width * desc.Height * channels : 0;
}
#endif

struct D3D12SelectedStencilState
{
	UINT8 readMask{};
	UINT8 writeMask{};
	UINT ref{};
	UINT8 frontReadMask{};
	UINT8 frontWriteMask{};
	UINT frontRef{};
	UINT8 backReadMask{};
	UINT8 backWriteMask{};
	UINT backRef{};
	bool usedBackFace{};
	bool needsIndependentFrontBackState{};
};

D3D12SelectedStencilState GetD3D12SelectedStencilState(const LatteContextRegister& lcr, const D3D12EffectiveCullState& cullState)
{
	const bool backStencilEnable = lcr.DB_DEPTH_CONTROL.get_BACK_STENCIL_ENABLE();
	const UINT8 frontReadMask = (UINT8)lcr.DB_STENCILREFMASK.get_STENCILMASK_F();
	const UINT8 frontWriteMask = (UINT8)lcr.DB_STENCILREFMASK.get_STENCILWRITEMASK_F();
	const UINT frontRef = lcr.DB_STENCILREFMASK.get_STENCILREF_F();
	const UINT8 backReadMask = backStencilEnable ? (UINT8)lcr.DB_STENCILREFMASK_BF.get_STENCILMASK_B() : frontReadMask;
	const UINT8 backWriteMask = backStencilEnable ? (UINT8)lcr.DB_STENCILREFMASK_BF.get_STENCILWRITEMASK_B() : frontWriteMask;
	const UINT backRef = backStencilEnable ? lcr.DB_STENCILREFMASK_BF.get_STENCILREF_B() : frontRef;

	D3D12SelectedStencilState selected{
		frontReadMask, frontWriteMask, frontRef,
		frontReadMask, frontWriteMask, frontRef,
		backReadMask, backWriteMask, backRef,
		false, false
	};
	if (backStencilEnable && cullState.cullFront && !cullState.cullBack)
	{
		selected.readMask = backReadMask;
		selected.writeMask = backWriteMask;
		selected.ref = backRef;
		selected.usedBackFace = true;
		return selected;
	}

	if (backStencilEnable && !cullState.cullFront && !cullState.cullBack)
	{
		selected.needsIndependentFrontBackState =
			frontReadMask != backReadMask ||
			frontWriteMask != backWriteMask ||
			frontRef != backRef;
	}
	return selected;
}

uint32 AlignUpU32(uint32 value, uint32 alignment)
{
	return (value + alignment - 1) & ~(alignment - 1);
}

uint32 D3D12CopyFormatClass(DXGI_FORMAT format)
{
	switch (format)
	{
	case DXGI_FORMAT_R8G8B8A8_TYPELESS:
	case DXGI_FORMAT_R8G8B8A8_UNORM:
	case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
	case DXGI_FORMAT_R8G8B8A8_SNORM:
	case DXGI_FORMAT_R8G8B8A8_UINT:
	case DXGI_FORMAT_R8G8B8A8_SINT:
		return 0x08080808;
	case DXGI_FORMAT_R32_TYPELESS:
	case DXGI_FORMAT_R32_FLOAT:
	case DXGI_FORMAT_R32_UINT:
	case DXGI_FORMAT_R32_SINT:
	case DXGI_FORMAT_D32_FLOAT:
		return 0x00000032;
	case DXGI_FORMAT_R24G8_TYPELESS:
	case DXGI_FORMAT_D24_UNORM_S8_UINT:
	case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
	case DXGI_FORMAT_X24_TYPELESS_G8_UINT:
		return 0x00002408;
	case DXGI_FORMAT_R32G8X24_TYPELESS:
	case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
	case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
	case DXGI_FORMAT_X32_TYPELESS_G8X24_UINT:
		return 0x00320824;
	case DXGI_FORMAT_R16_TYPELESS:
	case DXGI_FORMAT_R16_FLOAT:
	case DXGI_FORMAT_R16_UNORM:
	case DXGI_FORMAT_R16_UINT:
	case DXGI_FORMAT_R16_SNORM:
	case DXGI_FORMAT_R16_SINT:
	case DXGI_FORMAT_D16_UNORM:
		return 0x00000016;
	case DXGI_FORMAT_R8G8_TYPELESS:
	case DXGI_FORMAT_R8G8_UNORM:
	case DXGI_FORMAT_R8G8_UINT:
	case DXGI_FORMAT_R8G8_SNORM:
	case DXGI_FORMAT_R8G8_SINT:
		return 0x00000808;
	case DXGI_FORMAT_R8_TYPELESS:
	case DXGI_FORMAT_R8_UNORM:
	case DXGI_FORMAT_R8_UINT:
	case DXGI_FORMAT_R8_SNORM:
	case DXGI_FORMAT_R8_SINT:
		return 0x00000008;
	case DXGI_FORMAT_R16G16_TYPELESS:
	case DXGI_FORMAT_R16G16_FLOAT:
	case DXGI_FORMAT_R16G16_UNORM:
	case DXGI_FORMAT_R16G16_UINT:
	case DXGI_FORMAT_R16G16_SNORM:
	case DXGI_FORMAT_R16G16_SINT:
		return 0x00001616;
	case DXGI_FORMAT_R32G32_TYPELESS:
	case DXGI_FORMAT_R32G32_FLOAT:
	case DXGI_FORMAT_R32G32_UINT:
	case DXGI_FORMAT_R32G32_SINT:
		return 0x00003232;
	case DXGI_FORMAT_R16G16B16A16_TYPELESS:
	case DXGI_FORMAT_R16G16B16A16_FLOAT:
	case DXGI_FORMAT_R16G16B16A16_UNORM:
	case DXGI_FORMAT_R16G16B16A16_UINT:
	case DXGI_FORMAT_R16G16B16A16_SNORM:
	case DXGI_FORMAT_R16G16B16A16_SINT:
		return 0x16161616;
	case DXGI_FORMAT_R32G32B32A32_TYPELESS:
	case DXGI_FORMAT_R32G32B32A32_FLOAT:
	case DXGI_FORMAT_R32G32B32A32_UINT:
	case DXGI_FORMAT_R32G32B32A32_SINT:
		return 0x32323232;
	case DXGI_FORMAT_R10G10B10A2_TYPELESS:
	case DXGI_FORMAT_R10G10B10A2_UNORM:
	case DXGI_FORMAT_R10G10B10A2_UINT:
		return 0x10101002;
	case DXGI_FORMAT_BC1_TYPELESS:
	case DXGI_FORMAT_BC1_UNORM:
	case DXGI_FORMAT_BC1_UNORM_SRGB:
		return 0x00000BC1;
	case DXGI_FORMAT_BC2_TYPELESS:
	case DXGI_FORMAT_BC2_UNORM:
	case DXGI_FORMAT_BC2_UNORM_SRGB:
		return 0x00000BC2;
	case DXGI_FORMAT_BC3_TYPELESS:
	case DXGI_FORMAT_BC3_UNORM:
	case DXGI_FORMAT_BC3_UNORM_SRGB:
		return 0x00000BC3;
	case DXGI_FORMAT_BC4_TYPELESS:
	case DXGI_FORMAT_BC4_UNORM:
	case DXGI_FORMAT_BC4_SNORM:
		return 0x00000BC4;
	case DXGI_FORMAT_BC5_TYPELESS:
	case DXGI_FORMAT_BC5_UNORM:
	case DXGI_FORMAT_BC5_SNORM:
		return 0x00000BC5;
	default:
		return 0;
	}
}

bool AreD3D12CopyFormatsCompatible(DXGI_FORMAT srcFormat, DXGI_FORMAT dstFormat)
{
	if (srcFormat == dstFormat)
		return true;
	const uint32 srcClass = D3D12CopyFormatClass(srcFormat);
	const uint32 dstClass = D3D12CopyFormatClass(dstFormat);
	return srcClass != 0 && srcClass == dstClass;
}

bool IsFullSubresourceTextureCopy(const LatteTextureD3D12* texture, sint32 mipIndex,
								  sint32 x, sint32 y, sint32 z,
								  sint32 copyWidth, sint32 copyHeight, sint32 copyDepth)
{
	if (!texture || mipIndex < 0)
		return false;
	const auto& desc = texture->GetDesc();
	const uint32 mipWidth = std::max<uint32>(1, (uint32)(desc.Width >> mipIndex));
	const uint32 mipHeight = std::max<uint32>(1, (uint32)(desc.Height >> mipIndex));
	const uint32 mipDepth = desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? std::max<uint32>(1, (uint32)(desc.DepthOrArraySize >> mipIndex)) : 1u;
	return x == 0 &&
		   y == 0 &&
		   z == 0 &&
		   copyWidth == (sint32)mipWidth &&
		   copyHeight == (sint32)mipHeight &&
		   copyDepth == (sint32)mipDepth;
}

bool IsValidBCTextureCopyRegion(const LatteTextureD3D12* srcTexture, sint32 srcMip,
								const LatteTextureD3D12* dstTexture, sint32 dstMip,
								sint32 srcX, sint32 srcY, sint32 dstX, sint32 dstY, sint32 copyWidth, sint32 copyHeight)
{
	if ((!srcTexture || !srcTexture->GetFormatInfo().isCompressed) && (!dstTexture || !dstTexture->GetFormatInfo().isCompressed))
		return true;
	constexpr sint32 kBcBlockSize = 4;
	if (srcX < 0 || srcY < 0 || dstX < 0 || dstY < 0 || copyWidth <= 0 || copyHeight <= 0 ||
		(srcX % kBcBlockSize) != 0 || (srcY % kBcBlockSize) != 0 ||
		(dstX % kBcBlockSize) != 0 || (dstY % kBcBlockSize) != 0)
	{
		return false;
	}

	const auto mipExtent = [](const LatteTextureD3D12* texture, sint32 mip, bool width)
	{
		if (!texture || mip < 0)
			return 0;
		const auto& desc = texture->GetDesc();
		const uint64 base = width ? desc.Width : desc.Height;
		return (sint32)std::max<uint64>(base >> mip, 1);
	};
	const sint32 srcWidth = mipExtent(srcTexture, srcMip, true);
	const sint32 srcHeight = mipExtent(srcTexture, srcMip, false);
	const sint32 dstWidth = mipExtent(dstTexture, dstMip, true);
	const sint32 dstHeight = mipExtent(dstTexture, dstMip, false);
	if (srcX + copyWidth > srcWidth || srcY + copyHeight > srcHeight ||
		dstX + copyWidth > dstWidth || dstY + copyHeight > dstHeight)
	{
		return false;
	}

	const bool validWidth = (copyWidth % kBcBlockSize) == 0 ||
		(srcX + copyWidth == srcWidth && dstX + copyWidth == dstWidth);
	const bool validHeight = (copyHeight % kBcBlockSize) == 0 ||
		(srcY + copyHeight == srcHeight && dstY + copyHeight == dstHeight);
	return validWidth && validHeight;
}

struct D3D12CopyElementLayout
{
	uint32 blockSize{};
	uint32 bytes{};
};

// raw copies move whole texels, or whole 4x4 blocks of block-compressed formats
D3D12CopyElementLayout GetD3D12CopyElementLayout(DXGI_FORMAT format)
{
	switch (GetD3D12TypelessFormat(format))
	{
	case DXGI_FORMAT_R32G32B32A32_TYPELESS:
		return { 1, 16 };
	case DXGI_FORMAT_R16G16B16A16_TYPELESS:
	case DXGI_FORMAT_R32G32_TYPELESS:
		return { 1, 8 };
	case DXGI_FORMAT_R10G10B10A2_TYPELESS:
	case DXGI_FORMAT_R11G11B10_FLOAT:
	case DXGI_FORMAT_R8G8B8A8_TYPELESS:
	case DXGI_FORMAT_R16G16_TYPELESS:
	case DXGI_FORMAT_R32_TYPELESS:
		return { 1, 4 };
	case DXGI_FORMAT_R8G8_TYPELESS:
	case DXGI_FORMAT_R16_TYPELESS:
	case DXGI_FORMAT_B5G6R5_UNORM:
	case DXGI_FORMAT_B5G5R5A1_UNORM:
		return { 1, 2 };
	case DXGI_FORMAT_R8_TYPELESS:
		return { 1, 1 };
	case DXGI_FORMAT_BC1_TYPELESS:
	case DXGI_FORMAT_BC4_TYPELESS:
		return { 4, 8 };
	case DXGI_FORMAT_BC2_TYPELESS:
	case DXGI_FORMAT_BC3_TYPELESS:
	case DXGI_FORMAT_BC5_TYPELESS:
		return { 4, 16 };
	default:
		return {};
	}
}

D3D12_RESOURCE_STATES GetD3D12StateAfterUpload(D3D12_RESOURCE_STATES previousState)
{
	if (previousState == D3D12_RESOURCE_STATE_COMMON)
		return D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
	return previousState;
}

D3D12_COMPARISON_FUNC GetD3D12CompareFunc(Latte::E_COMPAREFUNC func)
{
	static constexpr D3D12_COMPARISON_FUNC table[] =
	{
		D3D12_COMPARISON_FUNC_NEVER,
		D3D12_COMPARISON_FUNC_LESS,
		D3D12_COMPARISON_FUNC_EQUAL,
		D3D12_COMPARISON_FUNC_LESS_EQUAL,
		D3D12_COMPARISON_FUNC_GREATER,
		D3D12_COMPARISON_FUNC_NOT_EQUAL,
		D3D12_COMPARISON_FUNC_GREATER_EQUAL,
		D3D12_COMPARISON_FUNC_ALWAYS,
	};
	const auto index = (size_t)func;
	return index < std::size(table) ? table[index] : D3D12_COMPARISON_FUNC_ALWAYS;
}

bool IsD3D12IntegerFormat(DXGI_FORMAT format)
{
	switch (format)
	{
	case DXGI_FORMAT_R8_UINT:
	case DXGI_FORMAT_R8_SINT:
	case DXGI_FORMAT_R8G8_UINT:
	case DXGI_FORMAT_R8G8_SINT:
	case DXGI_FORMAT_R8G8B8A8_UINT:
	case DXGI_FORMAT_R8G8B8A8_SINT:
	case DXGI_FORMAT_R16_UINT:
	case DXGI_FORMAT_R16_SINT:
	case DXGI_FORMAT_R16G16_UINT:
	case DXGI_FORMAT_R16G16_SINT:
	case DXGI_FORMAT_R16G16B16A16_UINT:
	case DXGI_FORMAT_R16G16B16A16_SINT:
	case DXGI_FORMAT_R32_UINT:
	case DXGI_FORMAT_R32_SINT:
	case DXGI_FORMAT_R32G32_UINT:
	case DXGI_FORMAT_R32G32_SINT:
	case DXGI_FORMAT_R32G32B32_UINT:
	case DXGI_FORMAT_R32G32B32_SINT:
	case DXGI_FORMAT_R32G32B32A32_UINT:
	case DXGI_FORMAT_R32G32B32A32_SINT:
	case DXGI_FORMAT_R10G10B10A2_UINT:
		return true;
	default:
		return false;
	}
}

D3D12_TEXTURE_ADDRESS_MODE GetD3D12TextureAddressMode(Latte::LATTE_SQ_TEX_SAMPLER_WORD0_0::E_CLAMP clamp)
{
	static constexpr D3D12_TEXTURE_ADDRESS_MODE table[] =
	{
		D3D12_TEXTURE_ADDRESS_MODE_WRAP,        // WRAP
		D3D12_TEXTURE_ADDRESS_MODE_MIRROR,      // MIRROR
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP,       // CLAMP_LAST_TEXEL
		D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE, // MIRROR_ONCE_LAST_TEXEL
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP,       // CLAMP_HALF_BORDER
		D3D12_TEXTURE_ADDRESS_MODE_BORDER,      // MIRROR_ONCE_HALF_BORDER
		D3D12_TEXTURE_ADDRESS_MODE_BORDER,      // CLAMP_BORDER
		D3D12_TEXTURE_ADDRESS_MODE_BORDER,      // MIRROR_ONCE_BORDER
	};
	const auto index = (size_t)clamp;
	return index < std::size(table) ? table[index] : D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
}

D3D12_FILTER_TYPE GetD3D12FilterType(Latte::LATTE_SQ_TEX_SAMPLER_WORD0_0::E_XY_FILTER filter)
{
	return (filter == Latte::LATTE_SQ_TEX_SAMPLER_WORD0_0::E_XY_FILTER::POINT ||
			filter == Latte::LATTE_SQ_TEX_SAMPLER_WORD0_0::E_XY_FILTER::ANISO_POINT)
			   ? D3D12_FILTER_TYPE_POINT
			   : D3D12_FILTER_TYPE_LINEAR;
}

D3D12_FILTER_TYPE GetD3D12MipFilterType(Latte::LATTE_SQ_TEX_SAMPLER_WORD0_0::E_Z_FILTER filter)
{
	// invalid mip filter values fall back to linear
	return (filter == Latte::LATTE_SQ_TEX_SAMPLER_WORD0_0::E_Z_FILTER::NONE ||
			filter == Latte::LATTE_SQ_TEX_SAMPLER_WORD0_0::E_Z_FILTER::POINT)
			   ? D3D12_FILTER_TYPE_POINT
			   : D3D12_FILTER_TYPE_LINEAR;
}

const _LatteRegisterSetSamplerBorderColor* GetD3D12SamplerBorderColorRegisters(LatteConst::ShaderType shaderType, uint32 stageSamplerIndex)
{
	if (shaderType == LatteConst::ShaderType::Vertex)
		return RecordRegs().TD_VS_SAMPLER_BORDER_COLOR + stageSamplerIndex;
	if (shaderType == LatteConst::ShaderType::Pixel)
		return RecordRegs().TD_PS_SAMPLER_BORDER_COLOR + stageSamplerIndex;
	return RecordRegs().TD_GS_SAMPLER_BORDER_COLOR + stageSamplerIndex;
}

void SetD3D12SamplerBorderColor(D3D12_SAMPLER_DESC& desc, LatteConst::ShaderType shaderType, uint32 stageSamplerIndex, Latte::LATTE_SQ_TEX_SAMPLER_WORD0_0::E_BORDER_COLOR_TYPE borderType)
{
	if (borderType == Latte::LATTE_SQ_TEX_SAMPLER_WORD0_0::E_BORDER_COLOR_TYPE::TRANSPARENT_BLACK)
	{
		desc.BorderColor[0] = 0.0f;
		desc.BorderColor[1] = 0.0f;
		desc.BorderColor[2] = 0.0f;
		desc.BorderColor[3] = 0.0f;
		return;
	}
	if (borderType == Latte::LATTE_SQ_TEX_SAMPLER_WORD0_0::E_BORDER_COLOR_TYPE::OPAQUE_BLACK)
	{
		desc.BorderColor[0] = 0.0f;
		desc.BorderColor[1] = 0.0f;
		desc.BorderColor[2] = 0.0f;
		desc.BorderColor[3] = 1.0f;
		return;
	}
	if (borderType == Latte::LATTE_SQ_TEX_SAMPLER_WORD0_0::E_BORDER_COLOR_TYPE::OPAQUE_WHITE)
	{
		desc.BorderColor[0] = 1.0f;
		desc.BorderColor[1] = 1.0f;
		desc.BorderColor[2] = 1.0f;
		desc.BorderColor[3] = 1.0f;
		return;
	}

	auto* borderColorReg = GetD3D12SamplerBorderColorRegisters(shaderType, stageSamplerIndex);
	desc.BorderColor[0] = borderColorReg->red.get_channelValue();
	desc.BorderColor[1] = borderColorReg->green.get_channelValue();
	desc.BorderColor[2] = borderColorReg->blue.get_channelValue();
	desc.BorderColor[3] = borderColorReg->alpha.get_channelValue();
}

D3D12_SAMPLER_DESC BuildD3D12DefaultGameSamplerDesc()
{
	D3D12_SAMPLER_DESC desc{};
	desc.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
	desc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	desc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	desc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	desc.MipLODBias = 0.0f;
	desc.MaxAnisotropy = 1;
	desc.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
	desc.BorderColor[0] = 0.0f;
	desc.BorderColor[1] = 0.0f;
	desc.BorderColor[2] = 0.0f;
	desc.BorderColor[3] = 0.0f;
	desc.MinLOD = 0.0f;
	desc.MaxLOD = D3D12_FLOAT32_MAX;
	return desc;
}


D3D12_SAMPLER_DESC BuildD3D12GameSamplerDesc(LatteDecompilerShader* shader, uint32 relativeUnit, LatteTextureViewD3D12* textureView,
											bool anisoFilterWithPointMipSupported)
{
	D3D12_SAMPLER_DESC desc = BuildD3D12DefaultGameSamplerDesc();
	if (!shader || relativeUnit >= LATTE_NUM_MAX_TEX_UNITS)
		return desc;

	const uint32 stageSamplerIndex = shader->textureUnitSamplerAssignment[relativeUnit];
	if (stageSamplerIndex == LATTE_DECOMPILER_SAMPLER_NONE)
		return desc;

	const uint32 samplerIndex = stageSamplerIndex + LatteDecompiler_getTextureSamplerBaseIndex(shader->shaderType);
	const _LatteRegisterSetSampler* samplerWords = RecordRegs().SQ_TEX_SAMPLER + samplerIndex;

	uint32 iMinLOD = samplerWords->WORD1.get_MIN_LOD();
	uint32 iMaxLOD = samplerWords->WORD1.get_MAX_LOD();
	sint32 iLodBias = samplerWords->WORD1.get_LOD_BIAS();

	LatteTexture* baseTexture = textureView ? textureView->baseTexture : nullptr;
	if (baseTexture)
	{
		if (baseTexture->overwriteInfo.hasRelativeLodBias)
			iLodBias += baseTexture->overwriteInfo.relativeLodBias;
		if (baseTexture->overwriteInfo.hasLodBias)
			iLodBias = baseTexture->overwriteInfo.lodBias;
	}

	auto mipFilter = samplerWords->WORD0.get_MIP_FILTER();
	D3D12_FILTER_TYPE mipFilterType = GetD3D12MipFilterType(mipFilter);
	if (mipFilter == Latte::LATTE_SQ_TEX_SAMPLER_WORD0_0::E_Z_FILTER::NONE)
	{
		desc.MinLOD = 0.0f;
		desc.MaxLOD = 0.25f;
	}
	else
	{
		desc.MinLOD = (float)iMinLOD / 64.0f;
		desc.MaxLOD = (float)iMaxLOD / 64.0f;
	}
	desc.MaxLOD = std::max(desc.MaxLOD, desc.MinLOD);

	auto minFilter = samplerWords->WORD0.get_XY_MIN_FILTER();
	auto magFilter = samplerWords->WORD0.get_XY_MAG_FILTER();
	auto effectiveMagFilter = magFilter;
	if (minFilter == Latte::LATTE_SQ_TEX_SAMPLER_WORD0_0::E_XY_FILTER::ANISO_POINT)
		effectiveMagFilter = Latte::LATTE_SQ_TEX_SAMPLER_WORD0_0::E_XY_FILTER::POINT;
	uint32 maxAniso = samplerWords->WORD0.get_MAX_ANISO_RATIO();
	if (baseTexture && baseTexture->overwriteInfo.anisotropicLevel >= 0)
		maxAniso = baseTexture->overwriteInfo.anisotropicLevel;
	const D3D12_FILTER_REDUCTION_TYPE reductionType = LatteDecompiler_HLSLTextureUsesComparisonSampler(shader, (int32_t)relativeUnit) ? D3D12_FILTER_REDUCTION_TYPE_COMPARISON : D3D12_FILTER_REDUCTION_TYPE_STANDARD;
	const D3D12_FILTER_TYPE minFilterType = GetD3D12FilterType(minFilter);
	const D3D12_FILTER_TYPE magFilterType = GetD3D12FilterType(effectiveMagFilter);

	// D3D12 anisotropic filters imply linear min/mag; without the point-mip variant, point mips fall back to linear mips
	const bool anisotropicMinMag = maxAniso > 0 && minFilterType == D3D12_FILTER_TYPE_LINEAR && magFilterType == D3D12_FILTER_TYPE_LINEAR;
#if !defined(__ID3D12GraphicsCommandList9_INTERFACE_DEFINED__)
	(void)anisoFilterWithPointMipSupported;
#endif
	if (anisotropicMinMag && mipFilterType == D3D12_FILTER_TYPE_LINEAR)
	{
		desc.Filter = D3D12_ENCODE_ANISOTROPIC_FILTER(reductionType);
		desc.MaxAnisotropy = std::min<UINT>(16, 1u << std::min<uint32>(maxAniso, 4));
	}
#if defined(__ID3D12GraphicsCommandList9_INTERFACE_DEFINED__)
	else if (anisotropicMinMag && anisoFilterWithPointMipSupported)
	{
		desc.Filter = D3D12_ENCODE_MIN_MAG_ANISOTROPIC_MIP_POINT_FILTER(reductionType);
		desc.MaxAnisotropy = std::min<UINT>(16, 1u << std::min<uint32>(maxAniso, 4));
	}
#endif
	else if (anisotropicMinMag)
	{
		if (mipFilter == Latte::LATTE_SQ_TEX_SAMPLER_WORD0_0::E_Z_FILTER::NONE)
			desc.MinLOD = desc.MaxLOD = 0.0f;
		desc.Filter = D3D12_ENCODE_ANISOTROPIC_FILTER(reductionType);
		desc.MaxAnisotropy = std::min<UINT>(16, 1u << std::min<uint32>(maxAniso, 4));
	}
	else
	{
		desc.Filter = D3D12_ENCODE_BASIC_FILTER(minFilterType, magFilterType, mipFilterType, reductionType);
		desc.MaxAnisotropy = 1;
	}

	desc.AddressU = GetD3D12TextureAddressMode(samplerWords->WORD0.get_CLAMP_X());
	desc.AddressV = GetD3D12TextureAddressMode(samplerWords->WORD0.get_CLAMP_Y());
	desc.AddressW = GetD3D12TextureAddressMode(samplerWords->WORD0.get_CLAMP_Z());
	// the GPU takes biases down to -32 (Mario Kart 8 uses that); D3D12 allows only [-16, 15.99] and the Xbox runtime
	// removes the device for anything outside it. Vulkan and OpenGL drivers clamp the same way.
	desc.MipLODBias = std::clamp((float)iLodBias / 64.0f, D3D12_MIP_LOD_BIAS_MIN, D3D12_MIP_LOD_BIAS_MAX);
	desc.ComparisonFunc = GetD3D12CompareFunc((Latte::E_COMPAREFUNC)(uint32)samplerWords->WORD0.get_DEPTH_COMPARE_FUNCTION());
	SetD3D12SamplerBorderColor(desc, shader->shaderType, stageSamplerIndex, samplerWords->WORD0.get_BORDER_COLOR_TYPE());
	return desc;
}

D3D12_STENCIL_OP GetD3D12StencilOp(Latte::LATTE_DB_DEPTH_CONTROL::E_STENCILACTION action)
{
	static constexpr D3D12_STENCIL_OP table[] =
	{
		D3D12_STENCIL_OP_KEEP,
		D3D12_STENCIL_OP_ZERO,
		D3D12_STENCIL_OP_REPLACE,
		D3D12_STENCIL_OP_INCR_SAT,
		D3D12_STENCIL_OP_DECR_SAT,
		D3D12_STENCIL_OP_INVERT,
		D3D12_STENCIL_OP_INCR,
		D3D12_STENCIL_OP_DECR,
	};
	const auto index = (size_t)action;
	return index < std::size(table) ? table[index] : D3D12_STENCIL_OP_KEEP;
}

bool D3D12StencilOpsCommute(D3D12_STENCIL_OP a, UINT8 writeMaskA, UINT refA, D3D12_STENCIL_OP b, UINT8 writeMaskB, UINT refB)
{
	if (a == D3D12_STENCIL_OP_KEEP || b == D3D12_STENCIL_OP_KEEP || writeMaskA == 0 || writeMaskB == 0)
		return true;
	const auto isAssign = [](D3D12_STENCIL_OP op) { return op == D3D12_STENCIL_OP_ZERO || op == D3D12_STENCIL_OP_REPLACE; };
	const auto isBitwise = [&](D3D12_STENCIL_OP op) { return isAssign(op) || op == D3D12_STENCIL_OP_INVERT; };
	const UINT8 overlap = writeMaskA & writeMaskB;
	if (isBitwise(a) && isBitwise(b))
	{
		if (overlap == 0 || (a == D3D12_STENCIL_OP_INVERT && b == D3D12_STENCIL_OP_INVERT))
			return true;
		if (isAssign(a) && isAssign(b))
		{
			const UINT valueA = a == D3D12_STENCIL_OP_ZERO ? 0u : refA;
			const UINT valueB = b == D3D12_STENCIL_OP_ZERO ? 0u : refB;
			return ((valueA ^ valueB) & overlap) == 0;
		}
		return false;
	}
	// increments carry across bits, so counters only commute over the whole stencil byte
	if (writeMaskA != 0xFF || writeMaskB != 0xFF)
		return false;
	const auto isWrap = [](D3D12_STENCIL_OP op) { return op == D3D12_STENCIL_OP_INCR || op == D3D12_STENCIL_OP_DECR; };
	if (isWrap(a) && isWrap(b))
		return true;
	return a == b && (a == D3D12_STENCIL_OP_INCR_SAT || a == D3D12_STENCIL_OP_DECR_SAT);
}

// replaying all front faces before all back faces is only exact when neither face reads or reorders the other's stencil writes
bool D3D12StencilFacesOrderIndependent(const LatteContextRegister& lcr, const D3D12SelectedStencilState& state, bool depthTestEnabled)
{
	struct StencilFace
	{
		D3D12_COMPARISON_FUNC func{};
		std::array<D3D12_STENCIL_OP, 3> ops{};
		uint32 opCount{};
		UINT8 readMask{};
		UINT8 writeMask{};
		UINT ref{};
	};
	const auto makeFace = [depthTestEnabled](Latte::E_COMPAREFUNC func,
		Latte::LATTE_DB_DEPTH_CONTROL::E_STENCILACTION failOp,
		Latte::LATTE_DB_DEPTH_CONTROL::E_STENCILACTION depthFailOp,
		Latte::LATTE_DB_DEPTH_CONTROL::E_STENCILACTION passOp,
		UINT8 readMask, UINT8 writeMask, UINT ref)
	{
		StencilFace face;
		face.func = GetD3D12CompareFunc(func);
		face.readMask = readMask;
		face.writeMask = writeMask;
		face.ref = ref;
		if (face.func != D3D12_COMPARISON_FUNC_ALWAYS)
			face.ops[face.opCount++] = GetD3D12StencilOp(failOp);
		if (face.func != D3D12_COMPARISON_FUNC_NEVER)
		{
			if (depthTestEnabled)
				face.ops[face.opCount++] = GetD3D12StencilOp(depthFailOp);
			face.ops[face.opCount++] = GetD3D12StencilOp(passOp);
		}
		bool writesStencil = false;
		for (uint32 i = 0; i < face.opCount; ++i)
			writesStencil |= face.ops[i] != D3D12_STENCIL_OP_KEEP;
		if (!writesStencil)
			face.writeMask = 0;
		return face;
	};
	const auto& depth = lcr.DB_DEPTH_CONTROL;
	const StencilFace front = makeFace(depth.get_STENCIL_FUNC_F(), depth.get_STENCIL_FAIL_F(), depth.get_STENCIL_ZFAIL_F(),
		depth.get_STENCIL_ZPASS_F(), state.frontReadMask, state.frontWriteMask, state.frontRef);
	const StencilFace back = makeFace(depth.get_STENCIL_FUNC_B(), depth.get_STENCIL_FAIL_B(), depth.get_STENCIL_ZFAIL_B(),
		depth.get_STENCIL_ZPASS_B(), state.backReadMask, state.backWriteMask, state.backRef);
	const auto testIgnores = [](const StencilFace& face, const StencilFace& other)
	{
		return face.func == D3D12_COMPARISON_FUNC_ALWAYS || face.func == D3D12_COMPARISON_FUNC_NEVER ||
			(face.readMask & other.writeMask) == 0;
	};
	if (!testIgnores(front, back) || !testIgnores(back, front))
		return false;
	for (uint32 i = 0; i < front.opCount; ++i)
	{
		for (uint32 j = 0; j < back.opCount; ++j)
		{
			if (!D3D12StencilOpsCommute(front.ops[i], front.writeMask, front.ref, back.ops[j], back.writeMask, back.ref))
				return false;
		}
	}
	return true;
}

D3D12_BLEND_OP GetD3D12BlendOp(Latte::LATTE_CB_BLENDN_CONTROL::E_COMBINEFUNC combineFunc)
{
	switch (combineFunc)
	{
	case Latte::LATTE_CB_BLENDN_CONTROL::E_COMBINEFUNC::DST_PLUS_SRC:
		return D3D12_BLEND_OP_ADD;
	case Latte::LATTE_CB_BLENDN_CONTROL::E_COMBINEFUNC::SRC_MINUS_DST:
		return D3D12_BLEND_OP_SUBTRACT;
	case Latte::LATTE_CB_BLENDN_CONTROL::E_COMBINEFUNC::MIN_DST_SRC:
		return D3D12_BLEND_OP_MIN;
	case Latte::LATTE_CB_BLENDN_CONTROL::E_COMBINEFUNC::MAX_DST_SRC:
		return D3D12_BLEND_OP_MAX;
	case Latte::LATTE_CB_BLENDN_CONTROL::E_COMBINEFUNC::DST_MINUS_SRC:
		return D3D12_BLEND_OP_REV_SUBTRACT;
	default:
		return D3D12_BLEND_OP_ADD;
	}
}

D3D12_BLEND GetD3D12BlendFactor(Latte::LATTE_CB_BLENDN_CONTROL::E_BLENDFACTOR factor)
{
	static constexpr D3D12_BLEND table[] =
	{
		/* 0x00 */ D3D12_BLEND_ZERO,
		/* 0x01 */ D3D12_BLEND_ONE,
		/* 0x02 */ D3D12_BLEND_SRC_COLOR,
		/* 0x03 */ D3D12_BLEND_INV_SRC_COLOR,
		/* 0x04 */ D3D12_BLEND_SRC_ALPHA,
		/* 0x05 */ D3D12_BLEND_INV_SRC_ALPHA,
		/* 0x06 */ D3D12_BLEND_DEST_ALPHA,
		/* 0x07 */ D3D12_BLEND_INV_DEST_ALPHA,
		/* 0x08 */ D3D12_BLEND_DEST_COLOR,
		/* 0x09 */ D3D12_BLEND_INV_DEST_COLOR,
		/* 0x0A */ D3D12_BLEND_SRC_ALPHA_SAT,
		/* 0x0B */ D3D12_BLEND_SRC_ALPHA,
		/* 0x0C */ D3D12_BLEND_INV_SRC_ALPHA,
		/* 0x0D */ D3D12_BLEND_BLEND_FACTOR,
		/* 0x0E */ D3D12_BLEND_INV_BLEND_FACTOR,
		/* 0x0F */ D3D12_BLEND_SRC1_COLOR,
		/* 0x10 */ D3D12_BLEND_INV_SRC1_COLOR,
		/* 0x11 */ D3D12_BLEND_SRC1_ALPHA,
		/* 0x12 */ D3D12_BLEND_INV_SRC1_ALPHA,
		/* 0x13 */ D3D12_BLEND_BLEND_FACTOR,
		/* 0x14 */ D3D12_BLEND_INV_BLEND_FACTOR,
	};
	const auto index = (size_t)factor;
	return index < std::size(table) ? table[index] : D3D12_BLEND_ONE;
}

D3D12_BLEND GetD3D12AlphaBlendFactor(Latte::LATTE_CB_BLENDN_CONTROL::E_BLENDFACTOR factor)
{
	switch (factor)
	{
	case Latte::LATTE_CB_BLENDN_CONTROL::E_BLENDFACTOR::BLEND_SRC_COLOR:
		return D3D12_BLEND_SRC_ALPHA;
	case Latte::LATTE_CB_BLENDN_CONTROL::E_BLENDFACTOR::BLEND_ONE_MINUS_SRC_COLOR:
		return D3D12_BLEND_INV_SRC_ALPHA;
	case Latte::LATTE_CB_BLENDN_CONTROL::E_BLENDFACTOR::BLEND_DST_COLOR:
		return D3D12_BLEND_DEST_ALPHA;
	case Latte::LATTE_CB_BLENDN_CONTROL::E_BLENDFACTOR::BLEND_ONE_MINUS_DST_COLOR:
		return D3D12_BLEND_INV_DEST_ALPHA;
	case Latte::LATTE_CB_BLENDN_CONTROL::E_BLENDFACTOR::BLEND_SRC_ALPHA_SATURATE:
		return D3D12_BLEND_ONE;
	case Latte::LATTE_CB_BLENDN_CONTROL::E_BLENDFACTOR::BLEND_SRC1_COLOR:
		return D3D12_BLEND_SRC1_ALPHA;
	case Latte::LATTE_CB_BLENDN_CONTROL::E_BLENDFACTOR::BLEND_INV_SRC1_COLOR:
		return D3D12_BLEND_INV_SRC1_ALPHA;
	default:
		return GetD3D12BlendFactor(factor);
	}
}

D3D12_BLEND GetD3D12ColorBlendFactor(Latte::LATTE_CB_BLENDN_CONTROL::E_BLENDFACTOR factor, bool alphaBlendFactorSupported)
{
#if defined(__ID3D12GraphicsCommandList9_INTERFACE_DEFINED__)
	if (alphaBlendFactorSupported)
	{
		if (factor == Latte::LATTE_CB_BLENDN_CONTROL::E_BLENDFACTOR::BLEND_CONST_ALPHA)
			return D3D12_BLEND_ALPHA_FACTOR;
		if (factor == Latte::LATTE_CB_BLENDN_CONTROL::E_BLENDFACTOR::BLEND_ONE_MINUS_CONST_ALPHA)
			return D3D12_BLEND_INV_ALPHA_FACTOR;
	}
#else
	(void)alphaBlendFactorSupported;
#endif
	return GetD3D12BlendFactor(factor);
}

bool D3D12BlendConstantNeedsAlphaSplat(const LatteContextRegister& lcr, const CachedFBOD3D12* fbo)
{
	if (!fbo)
		return false;
	using BlendFactor = Latte::LATTE_CB_BLENDN_CONTROL::E_BLENDFACTOR;
	const uint32 blendEnableMask = lcr.CB_COLOR_CONTROL.get_BLEND_MASK();
	const auto& colorAttachments = fbo->GetColorAttachments();
	bool usesConstantAlpha = false;
	bool usesConstantColor = false;
	for (uint32 i = 0; i < fbo->GetColorAttachmentCount() && i < colorAttachments.size(); ++i)
	{
		if ((blendEnableMask & (1u << i)) == 0 || !colorAttachments[i].valid)
			continue;
		const auto& blendControlReg = lcr.CB_BLENDN_CONTROL[i];
		for (const BlendFactor factor : { blendControlReg.get_COLOR_SRCBLEND(), blendControlReg.get_COLOR_DSTBLEND() })
		{
			if (factor == BlendFactor::BLEND_CONST_ALPHA || factor == BlendFactor::BLEND_ONE_MINUS_CONST_ALPHA)
				usesConstantAlpha = true;
			else if (factor == BlendFactor::BLEND_CONST_COLOR || factor == BlendFactor::BLEND_ONE_MINUS_CONST_COLOR)
				usesConstantColor = true;
		}
	}
	if (usesConstantAlpha && usesConstantColor)
	{
		cemuLog_logOnce(LogType::Force, "D3D12: a draw blends with both the constant color and the constant alpha; the constant alpha uses the constant color.");
		return false;
	}
	return usesConstantAlpha;
}

constexpr D3D_FEATURE_LEVEL kMinD3D12FeatureLevel =
	D3D_FEATURE_LEVEL_11_0;

const char* D3D12FeatureLevelName(D3D_FEATURE_LEVEL featureLevel)
{
	switch (featureLevel)
	{
	case D3D_FEATURE_LEVEL_12_2:
		return "12_2";
	case D3D_FEATURE_LEVEL_12_1:
		return "12_1";
	case D3D_FEATURE_LEVEL_12_0:
		return "12_0";
	case D3D_FEATURE_LEVEL_11_1:
		return "11_1";
	case D3D_FEATURE_LEVEL_11_0:
		return "11_0";
	default:
		return "unknown";
	}
}

bool CheckAdapterSupport(IDXGIAdapter1* adapter, D3D_SHADER_MODEL* highestShaderModel = nullptr, D3D_FEATURE_LEVEL* createdFeatureLevel = nullptr)
{
	Microsoft::WRL::ComPtr<ID3D12Device> device;
	if (FAILED(D3D12CreateDevice(adapter, kMinD3D12FeatureLevel, IID_PPV_ARGS(&device))))
		return false;

	D3D12_FEATURE_DATA_SHADER_MODEL shaderModel{ D3D_SHADER_MODEL_6_4 };
	if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &shaderModel, sizeof(shaderModel))))
		return false;

	if (highestShaderModel)
		*highestShaderModel = shaderModel.HighestShaderModel;
	if (createdFeatureLevel)
		*createdFeatureLevel = kMinD3D12FeatureLevel;

	return shaderModel.HighestShaderModel >= D3D_SHADER_MODEL_6_4;
}

std::string WideToUtf8(const wchar_t* str)
{
	if (!str)
		return {};
	std::wstring_view wide{ str };
	if (wide.empty())
		return {};
	const int len = WideCharToMultiByte(CP_UTF8, 0, wide.data(), (int)wide.size(), nullptr, 0, nullptr, nullptr);
	std::string result(len, '\0');
	WideCharToMultiByte(CP_UTF8, 0, wide.data(), (int)wide.size(), result.data(), len, nullptr, nullptr);
	return result;
}

void SetD3D12ObjectName(ID3D12Object* object, const wchar_t* name)
{
#if defined(CEMU_DEBUG_ASSERT) || defined(CEMU_UWP_D3D12_DIAGNOSTICS)
	if (object && name)
		object->SetName(name);
#else
	(void)object;
	(void)name;
#endif
}

DXGI_FORMAT GetD3D12IndexFormat(Renderer::INDEX_TYPE indexType)
{
	switch (indexType)
	{
	case Renderer::INDEX_TYPE::U16:
		return DXGI_FORMAT_R16_UINT;
	case Renderer::INDEX_TYPE::U32:
		return DXGI_FORMAT_R32_UINT;
	case Renderer::INDEX_TYPE::NONE:
	default:
		return DXGI_FORMAT_UNKNOWN;
	}
}

bool TextureViewSubresourcesOverlap(const LatteTextureViewD3D12* a, const LatteTextureViewD3D12* b)
{
	if (!a || !b || !a->baseTexture || !b->baseTexture)
		return false;

	const bool sameTextureObject = a->baseTexture == b->baseTexture;
	const bool sameD3D12Resource = a->GetBaseImage() && b->GetBaseImage() && a->GetBaseImage()->GetResource() == b->GetBaseImage()->GetResource();
	const bool sameLatteAddress = a->baseTexture->physAddress != 0 && a->baseTexture->physAddress == b->baseTexture->physAddress;
	const bool overlappingLatteRange =
		a->baseTexture->texDataPtrLow < b->baseTexture->texDataPtrHigh &&
		b->baseTexture->texDataPtrLow < a->baseTexture->texDataPtrHigh;
	if (!sameTextureObject && !sameD3D12Resource && !sameLatteAddress && !overlappingLatteRange)
		return false;

	const sint32 aMipEnd = a->firstMip + a->numMip;
	const sint32 bMipEnd = b->firstMip + b->numMip;
	if (a->firstMip >= bMipEnd || b->firstMip >= aMipEnd)
		return false;

	const sint32 aSliceEnd = a->firstSlice + a->numSlice;
	const sint32 bSliceEnd = b->firstSlice + b->numSlice;
	return a->firstSlice < bSliceEnd && b->firstSlice < aSliceEnd;
}

const char* kSurfaceCopyVertexShader = R"(
struct VSOut
{
	float4 position : SV_Position;
};

VSOut main(uint vertexId : SV_VertexID)
{
	static const float2 positions[3] =
	{
		float2(-1.0f, -1.0f),
		float2(-1.0f,  3.0f),
		float2( 3.0f, -1.0f)
	};

	VSOut output;
	output.position = float4(positions[vertexId % 3], 0.0f, 1.0f);
	return output;
}
)";

const char* kSurfaceCopyDepthToColorPixelShader = R"(
Texture2DArray<float> sourceTexture : register(t0);
SamplerState sourceSampler : register(s0);
cbuffer CopyConstants : register(b0)
{
	int2 srcTexelOffset;
	int2 dstTexelOffset;
};

float4 main(float4 position : SV_Position) : SV_Target0
{
	int2 sourceTexel = int2(position.xy) - dstTexelOffset + srcTexelOffset;
	return float4(sourceTexture.Load(int4(sourceTexel, 0, 0)), 0.0f, 0.0f, 1.0f);
}
)";

const char* kSurfaceCopyColorToColorPixelShader = R"(
Texture2DArray<float4> sourceTexture : register(t0);
SamplerState sourceSampler : register(s0);
cbuffer CopyConstants : register(b0)
{
	int2 srcTexelOffset;
	int2 dstTexelOffset;
};

float4 main(float4 position : SV_Position) : SV_Target0
{
	int2 sourceTexel = int2(position.xy) - dstTexelOffset + srcTexelOffset;
	return sourceTexture.Load(int4(sourceTexel, 0, 0));
}
)";

const char* kSurfaceCopyColorToDepthPixelShader = R"(
Texture2DArray<float4> sourceTexture : register(t0);
SamplerState sourceSampler : register(s0);
cbuffer CopyConstants : register(b0)
{
	int2 srcTexelOffset;
	int2 dstTexelOffset;
};

struct PSOut
{
	float depth : SV_Depth;
};

PSOut main(float4 position : SV_Position)
{
	PSOut output;
	int2 sourceTexel = int2(position.xy) - dstTexelOffset + srcTexelOffset;
	output.depth = sourceTexture.Load(int4(sourceTexel, 0, 0)).r;
	return output;
}
)";

const char* kSurfaceCopyDepthToDepthPixelShader = R"(
Texture2DArray<float> sourceTexture : register(t0);
SamplerState sourceSampler : register(s0);
cbuffer CopyConstants : register(b0)
{
	int2 srcTexelOffset;
	int2 dstTexelOffset;
};

struct PSOut
{
	float depth : SV_Depth;
};

PSOut main(float4 position : SV_Position)
{
	PSOut output;
	int2 sourceTexel = int2(position.xy) - dstTexelOffset + srcTexelOffset;
	output.depth = sourceTexture.Load(int4(sourceTexel, 0, 0));
	return output;
}
)";

// stencil-plane SRV formats return the stencil value in the green channel
const char* kSurfaceCopyStencilToStencilPixelShader = R"(
Texture2DArray<uint2> sourceTexture : register(t0);
SamplerState sourceSampler : register(s0);
cbuffer CopyConstants : register(b0)
{
	int2 srcTexelOffset;
	int2 dstTexelOffset;
};

struct PSOut
{
	uint stencil : SV_StencilRef;
};

PSOut main(float4 position : SV_Position)
{
	PSOut output;
	int2 sourceTexel = int2(position.xy) - dstTexelOffset + srcTexelOffset;
	output.stencil = sourceTexture.Load(int4(sourceTexel, 0, 0)).g;
	return output;
}
)";
}

bool InitializeGlobalD3D12()
{
	if (g_d3d12_available)
		return true;

	g_d3d12_available = !D3D12Renderer::GetDevices().empty();
	if (!g_d3d12_available)
		cemuLog_log(LogType::Force, "Direct3D 12 renderer unavailable. Requires Feature Level {} and Shader Model 6.4.",
					D3D12FeatureLevelName(kMinD3D12FeatureLevel));
	return g_d3d12_available;
}

D3D12Renderer::DeviceInfo::DeviceInfo(const std::string& name_, const LUID& luid_)
	: name(name_)
{
	static_assert(sizeof(luid) == sizeof(luid_));
	std::memcpy(luid.data(), &luid_, sizeof(luid_));
}

std::vector<D3D12Renderer::DeviceInfo> D3D12Renderer::GetDevices()
{
	std::vector<DeviceInfo> result;

	UINT flags = 0;
#ifdef CEMU_DEBUG_ASSERT
	flags |= DXGI_CREATE_FACTORY_DEBUG;
#endif
	Microsoft::WRL::ComPtr<IDXGIFactory6> factory6;
	if (FAILED(CreateDXGIFactory2(flags, IID_PPV_ARGS(&factory6))))
		return result;

	Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
	for (UINT index = 0; factory6->EnumAdapters1(index, &adapter) != DXGI_ERROR_NOT_FOUND; ++index)
	{
		DXGI_ADAPTER_DESC1 desc{};
		adapter->GetDesc1(&desc);
		if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0)
			continue;
		if (!CheckAdapterSupport(adapter.Get()))
			continue;
		result.emplace_back(WideToUtf8(desc.Description), desc.AdapterLuid);
	}
	return result;
}

D3D12Renderer::D3D12Renderer()
	: Renderer(RendererAPI::Direct3D12)
{
	cemuLog_log(LogType::Force, "------- Init Direct3D 12 graphics backend -------");
	ResetScratchCopyLookup();
#if defined(CEMU_UWP_PERFORMANCE_EXPERIMENT)
	InitializePerformanceExperiment();
#endif
	m_submitThreshold = GetDefaultSubmitThreshold();
	RendererShaderD3D12::Init();
	CreateDevice();
	CreateCommandObjects();
	CreateRtvHeap();
	CreateDescriptorManager();
	CreateMemoryManager();
	cemuLog_log(LogType::Force,
		"D3D12 UWP: decoded indices use CPU cache staging with per-command-fence upload-ring residency.");
#if defined(CEMU_UWP_D3D12_DIAGNOSTICS) || defined(CEMU_UWP_D3D12_WORKLOAD_PROFILE) || defined(CEMU_UWP)
	CreateTimingResources();
#endif
	CreateSparseMrtNullRtv();
	CreateOutputRootSignature();
	CreateOutputSamplers();
	CreateGameRootSignature();
	CreateSurfaceCopyRootSignature();
	CreateImguiDescriptorHeap();
	m_gpuWatchdog.Start(m_device.Get(), m_fence.Get());
}

D3D12Renderer::~D3D12Renderer()
{
	try
	{
		Shutdown();
	}
	catch (const std::exception& ex)
	{
		cemuLog_log(LogType::Force, "D3D12 UWP: renderer destruction continued after shutdown failed: {}", ex.what());
	}
	catch (...)
	{
		cemuLog_log(LogType::Force, "D3D12 UWP: renderer destruction continued after an unknown shutdown failure.");
	}
	// Release deferred D3D12MA allocations before their allocator.
	m_deferredReleaseQueue.clear();
}

D3D12Renderer* D3D12Renderer::GetInstance()
{
	return static_cast<D3D12Renderer*>(g_renderer.get());
}

void D3D12Renderer::CreateDevice()
{
	UINT flags = 0;
#ifdef __ID3D12DeviceRemovedExtendedDataSettings_INTERFACE_DEFINED__
	if (D3D12ShouldEnableDebug() || D3D12ShouldTrace())
	{
		Microsoft::WRL::ComPtr<ID3D12DeviceRemovedExtendedDataSettings> dredSettings;
		if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dredSettings))))
		{
			dredSettings->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
			dredSettings->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
		}
	}
#endif

#ifdef CEMU_DEBUG_ASSERT
	const bool enableDebugLayer = true;
#else
	const bool enableDebugLayer = D3D12ShouldEnableDebug();
#endif
	if (enableDebugLayer)
	{
		Microsoft::WRL::ComPtr<ID3D12Debug> debug;
		if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
		{
			debug->EnableDebugLayer();
			m_debugLayerEnabled = true;
		}
		flags |= DXGI_CREATE_FACTORY_DEBUG;
	}
	if (FAILED(CreateDXGIFactory2(flags, IID_PPV_ARGS(&m_factory))))
		throw std::runtime_error("Failed to create DXGI factory");

	const auto configuredLuid = GetConfig().d3d12_graphic_device_luid;
	const bool hasConfiguredLuid = std::any_of(configuredLuid.begin(), configuredLuid.end(), [](uint8 v) { return v != 0; });
	Microsoft::WRL::ComPtr<IDXGIAdapter1> fallbackAdapter;
	Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
	for (UINT index = 0; m_factory->EnumAdapters1(index, &adapter) != DXGI_ERROR_NOT_FOUND; ++index)
	{
		DXGI_ADAPTER_DESC1 desc{};
		adapter->GetDesc1(&desc);
		if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0)
			continue;
		if (!CheckAdapterSupport(adapter.Get()))
			continue;
		if (!fallbackAdapter)
			fallbackAdapter = adapter;
		if (hasConfiguredLuid && std::memcmp(configuredLuid.data(), &desc.AdapterLuid, sizeof(desc.AdapterLuid)) == 0)
		{
			m_adapter = adapter;
			break;
		}
	}
	if (!m_adapter)
		m_adapter = fallbackAdapter;
	if (!m_adapter)
		throw std::runtime_error(fmt::format("No Direct3D 12 adapter supports Feature Level {} and Shader Model 6.4",
											 D3D12FeatureLevelName(kMinD3D12FeatureLevel)));

	if (FAILED(D3D12CreateDevice(m_adapter.Get(), kMinD3D12FeatureLevel, IID_PPV_ARGS(&m_device))))
		throw std::runtime_error("Failed to create Direct3D 12 device");
	m_featureLevel = kMinD3D12FeatureLevel;

	D3D12_FEATURE_DATA_SHADER_MODEL shaderModel{ D3D_SHADER_MODEL_6_4 };
	if (FAILED(m_device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &shaderModel, sizeof(shaderModel))) ||
		shaderModel.HighestShaderModel < D3D_SHADER_MODEL_6_4)
	{
		throw std::runtime_error("Direct3D 12 device does not support Shader Model 6.4");
	}
	m_shaderModel = shaderModel.HighestShaderModel;

	D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
	const HRESULT optionsResult = m_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options));
	if (SUCCEEDED(optionsResult))
	{
		m_resourceBindingTier = options.ResourceBindingTier;
		m_tiledResourcesTier = options.TiledResourcesTier;
		m_outputMergerLogicOpSupported = options.OutputMergerLogicOp != FALSE;
		m_psSpecifiedStencilRefSupported = options.PSSpecifiedStencilRefSupported != FALSE;
	}
#if defined(CEMU_UWP_D3D12_WORKLOAD_PROFILE)
	// distinguish a failed query from a supported query whose feature bit is false
	cemuLog_log(LogType::Force,
		"D3D12 UWP OR capabilities: optionsHr=0x{:08x} nativeLogicOp={} rovs={} typedUavAdditional={} bindingTier={}.",
		(uint32)optionsResult, options.OutputMergerLogicOp != FALSE, options.ROVsSupported != FALSE,
		options.TypedUAVLoadAdditionalFormats != FALSE, (uint32)options.ResourceBindingTier);
	D3D12_FEATURE_DATA_FORMAT_SUPPORT orAccumulatorSupport{};
	orAccumulatorSupport.Format = DXGI_FORMAT_R32_UINT;
	const HRESULT orAccumulatorResult = m_device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT,
		&orAccumulatorSupport, sizeof(orAccumulatorSupport));
	cemuLog_log(LogType::Force,
		"D3D12 UWP OR capabilities: format=R32_UINT hr=0x{:08x} support1=0x{:08x} support2=0x{:08x} typedUav={} atomicBitwise={} typedLoad={} typedStore={}.",
		(uint32)orAccumulatorResult, (uint32)orAccumulatorSupport.Support1, (uint32)orAccumulatorSupport.Support2,
		(orAccumulatorSupport.Support1 & D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW) != 0,
		(orAccumulatorSupport.Support2 & D3D12_FORMAT_SUPPORT2_UAV_ATOMIC_BITWISE_OPS) != 0,
		(orAccumulatorSupport.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD) != 0,
		(orAccumulatorSupport.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE) != 0);
#endif
#if defined(__ID3D12GraphicsCommandList8_INTERFACE_DEFINED__)
	m_independentFrontBackStencilSupported = false;
	m_independentStencilDevice.Reset();
	D3D12_FEATURE_DATA_D3D12_OPTIONS14 options14{};
	const bool options14Supported =
		SUCCEEDED(m_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS14, &options14, sizeof(options14))) &&
		options14.IndependentFrontAndBackStencilRefMaskSupported != FALSE;
	if (options14Supported && SUCCEEDED(m_device.As(&m_independentStencilDevice)))
		m_independentFrontBackStencilSupported = true;
#endif
#if defined(__ID3D12GraphicsCommandList9_INTERFACE_DEFINED__)
	D3D12_FEATURE_DATA_D3D12_OPTIONS13 options13{};
	if (SUCCEEDED(m_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS13, &options13, sizeof(options13))))
	{
		m_alphaBlendFactorSupported = options13.AlphaBlendFactorSupported != FALSE;
		m_invertedViewportHeightFlipsYSupported = options13.InvertedViewportHeightFlipsYSupported != FALSE;
	}
	D3D12_FEATURE_DATA_D3D12_OPTIONS16 options16{};
	if (SUCCEEDED(m_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS16, &options16, sizeof(options16))))
		m_dynamicDepthBiasSupported = options16.DynamicDepthBiasSupported != FALSE;
	D3D12_FEATURE_DATA_D3D12_OPTIONS19 options19{};
	if (SUCCEEDED(m_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS19, &options19, sizeof(options19))))
		m_anisoFilterWithPointMipSupported = options19.AnisoFilterWithPointMipSupported != FALSE;
#endif

	D3D12_FEATURE_DATA_ROOT_SIGNATURE rootSignature{ D3D_ROOT_SIGNATURE_VERSION_1_1 };
	if (FAILED(m_device->CheckFeatureSupport(D3D12_FEATURE_ROOT_SIGNATURE, &rootSignature, sizeof(rootSignature))))
		rootSignature.HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_0;
	m_rootSignatureVersion = rootSignature.HighestVersion;

	D3D12_FEATURE_DATA_ARCHITECTURE architecture{};
	if (SUCCEEDED(m_device->CheckFeatureSupport(D3D12_FEATURE_ARCHITECTURE, &architecture, sizeof(architecture))))
	{
		m_isUma = architecture.UMA != FALSE;
		m_isCacheCoherentUma = architecture.CacheCoherentUMA != FALSE;
	}

	auto supportsPackedColorFormat = [this](DXGI_FORMAT format)
	{
		const UINT requiredSupport = (UINT)(D3D12_FORMAT_SUPPORT1_TEXTURE1D | D3D12_FORMAT_SUPPORT1_TEXTURE2D |
			D3D12_FORMAT_SUPPORT1_TEXTURE3D | D3D12_FORMAT_SUPPORT1_TEXTURECUBE | D3D12_FORMAT_SUPPORT1_MIP |
			D3D12_FORMAT_SUPPORT1_SHADER_LOAD | D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE |
			D3D12_FORMAT_SUPPORT1_RENDER_TARGET | D3D12_FORMAT_SUPPORT1_BLENDABLE);
		D3D12_FEATURE_DATA_FORMAT_SUPPORT support{};
		support.Format = format;
		return SUCCEEDED(m_device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support))) &&
			((UINT)support.Support1 & requiredSupport) == requiredSupport;
	};
	D3D12FormatCapabilities formatCapabilities{};
	formatCapabilities.packedB5G6R5 = supportsPackedColorFormat(DXGI_FORMAT_B5G6R5_UNORM);
	formatCapabilities.packedB5G5R5A1 = supportsPackedColorFormat(DXGI_FORMAT_B5G5R5A1_UNORM);
	D3D12_FEATURE_DATA_D3D12_OPTIONS8 options8{};
	if (SUCCEEDED(m_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS8, &options8, sizeof(options8))))
	{
		formatCapabilities.unalignedBlockTextures = options8.UnalignedBlockTexturesSupported != FALSE;
	}
	else
	{
		// runtimes that predate the query still accept unaligned block-compressed sizes when they support them
		D3D12_RESOURCE_DESC unalignedBlockDesc{};
		unalignedBlockDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		unalignedBlockDesc.Width = 6;
		unalignedBlockDesc.Height = 6;
		unalignedBlockDesc.DepthOrArraySize = 1;
		unalignedBlockDesc.MipLevels = 1;
		unalignedBlockDesc.Format = DXGI_FORMAT_BC1_UNORM;
		unalignedBlockDesc.SampleDesc.Count = 1;
		unalignedBlockDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
		const D3D12_RESOURCE_ALLOCATION_INFO allocationInfo = m_device->GetResourceAllocationInfo(0, 1, &unalignedBlockDesc);
		formatCapabilities.unalignedBlockTextures = allocationInfo.SizeInBytes != UINT64_MAX;
	}
	SetD3D12FormatCapabilities(formatCapabilities);
	cemuLog_log(LogType::Force, "D3D12 B5G6R5 textures: {}, B5G5R5A1 textures: {}, unaligned block-compressed textures: {}",
		formatCapabilities.packedB5G6R5 ? "yes" : "no",
		formatCapabilities.packedB5G5R5A1 ? "yes" : "no",
		formatCapabilities.unalignedBlockTextures ? "yes" : "no");

	Microsoft::WRL::ComPtr<ID3D12InfoQueue> infoQueue;
	if (SUCCEEDED(m_device.As(&infoQueue)))
	{
		m_infoQueueEnabled = m_debugLayerEnabled || D3D12ShouldTrace();
#ifdef CEMU_DEBUG_ASSERT
#endif
	}

#ifdef __ID3D12DeviceRemovedExtendedData_INTERFACE_DEFINED__
	Microsoft::WRL::ComPtr<ID3D12DeviceRemovedExtendedData> dred;
	m_dredAvailable = SUCCEEDED(m_device.As(&dred));
#endif

	DXGI_ADAPTER_DESC1 desc{};
	m_adapter->GetDesc1(&desc);
	m_adapterLuid = desc.AdapterLuid;
	m_selectedDeviceName = WideToUtf8(desc.Description);
	cemuLog_log(LogType::Force, "Using D3D12 GPU: {}", m_selectedDeviceName);
	cemuLog_log(LogType::Force, "D3D12 feature level: {}, shader model: 0x{:x}, resource binding tier: {}, tiled resources tier: {}",
				D3D12FeatureLevelName(m_featureLevel), (uint32)m_shaderModel, (uint32)m_resourceBindingTier, (uint32)m_tiledResourcesTier);
	cemuLog_log(LogType::Force, "D3D12 root signature version: 1.{}, UMA: {}, cache coherent UMA: {}, debug layer: {}, info queue: {}, DRED: {}",
				m_rootSignatureVersion >= D3D_ROOT_SIGNATURE_VERSION_1_1 ? 1 : 0,
				m_isUma ? "yes" : "no",
				m_isCacheCoherentUma ? "yes" : "no",
				m_debugLayerEnabled ? "yes" : "no",
				m_infoQueueEnabled ? "yes" : "no",
				m_dredAvailable ? "yes" : "no");
#if defined(__ID3D12GraphicsCommandList8_INTERFACE_DEFINED__)
	cemuLog_log(LogType::Force, "D3D12 independent front/back stencil masks and references: {}",
		m_independentFrontBackStencilSupported ? "yes" : "no");
#endif
	cemuLog_log(LogType::Force, "D3D12 alpha blend factor: {}, dynamic depth bias: {}, anisotropic filter with point mip: {}, inverted viewport height: {}",
		m_alphaBlendFactorSupported ? "yes" : "no",
		m_dynamicDepthBiasSupported ? "yes" : "no",
		m_anisoFilterWithPointMipSupported ? "yes" : "no",
		m_invertedViewportHeightFlipsYSupported ? "yes" : "no");

	switch (desc.VendorId)
	{
	case 0x10DE: m_vendor = GfxVendor::Nvidia; break;
	case 0x8086: m_vendor = GfxVendor::Intel; break;
	case 0x1002: m_vendor = GfxVendor::AMD; break;
	default: m_vendor = GfxVendor::Generic; break;
	}

#if BOOST_OS_WINDOWS
	m_dxgi_wrapper = std::make_unique<DXGIWrapper>((uint8*)&desc.AdapterLuid);
#endif
}

void D3D12Renderer::CreateCommandObjects()
{
	D3D12_COMMAND_QUEUE_DESC queueDesc{};
	queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	queueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
	if (FAILED(m_device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&m_directQueue))))
		throw std::runtime_error("Failed to create D3D12 command queue");
	SetD3D12ObjectName(m_directQueue.Get(), L"Cemu D3D12 direct queue");

	for (uint32 i = 0; i < kCommandBufferPoolSize; ++i)
	{
		auto& slot = m_commandSlots[i];
		if (FAILED(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&slot.allocator))))
			throw std::runtime_error("Failed to create D3D12 command allocator");
		if (FAILED(m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, slot.allocator.Get(), nullptr, IID_PPV_ARGS(&slot.commandList))))
			throw std::runtime_error("Failed to create D3D12 command list");
#if defined(__ID3D12GraphicsCommandList8_INTERFACE_DEFINED__)
		if (m_independentFrontBackStencilSupported && FAILED(slot.commandList.As(&slot.commandList8)))
			m_independentFrontBackStencilSupported = false;
#endif
#if defined(__ID3D12GraphicsCommandList9_INTERFACE_DEFINED__)
		if (m_dynamicDepthBiasSupported && FAILED(slot.commandList.As(&slot.commandList9)))
			m_dynamicDepthBiasSupported = false;
#endif
		if (FAILED(slot.commandList->Close()))
			throw std::runtime_error("Failed to close initial D3D12 command list");
		SetD3D12ObjectName(slot.allocator.Get(), L"Cemu D3D12 command allocator");
		SetD3D12ObjectName(slot.commandList.Get(), L"Cemu D3D12 command list");
	}
#if defined(__ID3D12GraphicsCommandList8_INTERFACE_DEFINED__)
	if (!m_independentFrontBackStencilSupported)
	{
		m_independentStencilDevice.Reset();
		for (auto& slot : m_commandSlots)
			slot.commandList8.Reset();
	}
#endif
#if defined(__ID3D12GraphicsCommandList9_INTERFACE_DEFINED__)
	if (!m_dynamicDepthBiasSupported)
	{
		for (auto& slot : m_commandSlots)
			slot.commandList9.Reset();
	}
#else
	m_dynamicDepthBiasSupported = false;
#endif
	D3D12PipelineCompiler::SetDynamicDepthBias(m_dynamicDepthBiasSupported);
	m_commandListOpen = false;

	if (FAILED(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence))))
		throw std::runtime_error("Failed to create D3D12 fence");
	SetD3D12ObjectName(m_fence.Get(), L"Cemu D3D12 command fence");
	m_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
	if (!m_fenceEvent)
		throw std::runtime_error("Failed to create D3D12 fence event");
}

void D3D12Renderer::CreateRtvHeap()
{
	D3D12_DESCRIPTOR_HEAP_DESC desc{};
	desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
	desc.NumDescriptors = 16;
	if (FAILED(m_device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&m_rtvHeap))))
		throw std::runtime_error("Failed to create D3D12 RTV descriptor heap");
	SetD3D12ObjectName(m_rtvHeap.Get(), L"Cemu D3D12 RTV heap");
	m_rtvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
}

void D3D12Renderer::CreateDescriptorManager()
{
	m_descriptorManager = std::make_unique<D3D12DescriptorManager>();
	m_descriptorManager->Initialize(m_device.Get());
}

void D3D12Renderer::CreateMemoryManager()
{
	m_memoryManager = std::make_unique<D3D12MemoryManager>();
	m_memoryManager->Initialize(m_device.Get(), m_adapter.Get());
}

#if defined(CEMU_UWP_D3D12_DIAGNOSTICS) || defined(CEMU_UWP_D3D12_WORKLOAD_PROFILE) || defined(CEMU_UWP)
void D3D12Renderer::CreateTimingResources()
{
	if (!m_device || !m_directQueue || !m_memoryManager)
	{
#if defined(CEMU_UWP_D3D12_WORKLOAD_PROFILE)
		cemuLog_log(LogType::Force, "D3D12 UWP workload GPU timing unavailable: device/queue/memory manager is not ready.");
#endif
		return;
	}
	D3D12_QUERY_HEAP_DESC queryDesc{};
	queryDesc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
	queryDesc.Count = 2 * kCommandBufferPoolSize;
	if (FAILED(m_device->CreateQueryHeap(&queryDesc, IID_PPV_ARGS(&m_timestampQueryHeap))))
	{
#if defined(CEMU_UWP_D3D12_WORKLOAD_PROFILE)
		cemuLog_log(LogType::Force, "D3D12 UWP workload GPU timing unavailable: timestamp query heap creation failed.");
#endif
		return;
	}
	if (FAILED(m_directQueue->GetTimestampFrequency(&m_timestampFrequency)) || m_timestampFrequency == 0)
	{
		m_timestampQueryHeap.Reset();
		m_timestampFrequency = 0;
#if defined(CEMU_UWP_D3D12_WORKLOAD_PROFILE)
		cemuLog_log(LogType::Force, "D3D12 UWP workload GPU timing unavailable: queue timestamp frequency could not be obtained.");
#endif
		return;
	}
	const uint64 readbackBytes = sizeof(uint64) * queryDesc.Count;
	if (!m_memoryManager->CreateBuffer(readbackBytes, D3D12_HEAP_TYPE_READBACK,
		D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, m_timestampReadback))
	{
		m_timestampQueryHeap.Reset();
		m_timestampFrequency = 0;
#if defined(CEMU_UWP_D3D12_WORKLOAD_PROFILE)
		cemuLog_log(LogType::Force, "D3D12 UWP workload GPU timing unavailable: timestamp readback allocation failed.");
#endif
		return;
	}
	void* mapped = nullptr;
	D3D12_RANGE readRange{ 0, (SIZE_T)readbackBytes };
	if (FAILED(m_timestampReadback.resource->Map(0, &readRange, &mapped)))
	{
		m_timestampReadback = {};
		m_timestampQueryHeap.Reset();
		m_timestampFrequency = 0;
#if defined(CEMU_UWP_D3D12_WORKLOAD_PROFILE)
		cemuLog_log(LogType::Force, "D3D12 UWP workload GPU timing unavailable: timestamp readback mapping failed.");
#endif
		return;
	}
	m_timestampReadbackMapped = static_cast<const uint64*>(mapped);
	SetD3D12ObjectName(m_timestampQueryHeap.Get(), L"Cemu D3D12 command timing queries");
	SetD3D12ObjectName(m_timestampReadback.resource.Get(), L"Cemu D3D12 command timing readback");
#if defined(CEMU_UWP_D3D12_WORKLOAD_PROFILE)
	cemuLog_log(LogType::Force, "D3D12 UWP workload GPU timing available: frequency={}Hz slots={} readbackBytes={}; completed-fence readback only, no added CPU wait.",
		m_timestampFrequency, kCommandBufferPoolSize, readbackBytes);
#endif
}

void D3D12Renderer::ReleaseTimingResources()
{
	if (m_timestampReadback.resource && m_timestampReadbackMapped)
	{
		D3D12_RANGE writtenRange{ 0, 0 };
		m_timestampReadback.resource->Unmap(0, &writtenRange);
	}
	m_timestampReadbackMapped = nullptr;
	m_timestampReadback = {};
	m_timestampQueryHeap.Reset();
	m_timestampFrequency = 0;
}
#endif

#if defined(CEMU_UWP_D3D12_WORKLOAD_PROFILE)
void D3D12Renderer::LogWorkloadGpuTiming()
{
	const bool available = m_timestampQueryHeap && m_timestampReadback.resource &&
		m_timestampReadbackMapped && m_timestampFrequency != 0;
	const uint64 samples = m_gpuCommandListSamples - m_workloadGpuLastSamples;
	const uint64 ticks = m_gpuCommandListTicks - m_workloadGpuLastTicks;
	const uint64 invalidPairs = m_workloadGpuInvalidPairs - m_workloadGpuLastInvalidPairs;
	const double totalMs = available ? (double)ticks * 1000.0 / (double)m_timestampFrequency : 0.0;
	const double maxMs = available ? (double)m_workloadGpuWindowMaxTicks * 1000.0 / (double)m_timestampFrequency : 0.0;
	cemuLog_log(LogType::Force,
		"D3D12 UWP workload GPU timing: available={} frequency={}Hz completedLists={} total_ms={:.3f} max_ms={:.3f} pendingLists={} invalidPairs={} "
		"(completion-bucketed direct-queue timestamp spans include work/dependencies; not frame-local GPU utilization).",
		available, m_timestampFrequency, samples, totalMs, maxMs,
		m_numSubmittedCommandLists - m_numCompletedCommandLists, invalidPairs);
	m_workloadGpuLastSamples = m_gpuCommandListSamples;
	m_workloadGpuLastTicks = m_gpuCommandListTicks;
	m_workloadGpuLastInvalidPairs = m_workloadGpuInvalidPairs;
	m_workloadGpuWindowMaxTicks = 0;
}
#endif

#if defined(CEMU_UWP_D3D12_DIAGNOSTICS)
void D3D12Renderer::LogPerformanceSummary(std::string_view phase) const
{
	const D3D12MemoryManager::Stats memoryStats = m_memoryManager ?
		m_memoryManager->GetStats() : D3D12MemoryManager::Stats{};
	const double gpuMilliseconds = m_timestampFrequency != 0 ?
		(double)m_gpuCommandListTicks * 1000.0 / (double)m_timestampFrequency : 0.0;
	const double gpuMaxMilliseconds = m_timestampFrequency != 0 ?
		(double)m_gpuCommandListMaxTicks * 1000.0 / (double)m_timestampFrequency : 0.0;
	const double barriersPerCall = m_transitionBarrierCalls != 0 ?
		(double)m_transitionBarrierCount / (double)m_transitionBarrierCalls : 0.0;
	cemuLog_log(LogType::Force,
		"D3D12 UWP performance [{}]: PSO create count={} total={:.2f}ms max={:.2f}ms, PSO blobs hit/miss/store/reject={}/{}/{}/{}, "
		"GPU command lists={} total={:.2f}ms max={:.2f}ms, fence waits={} total={:.2f}ms max={:.2f}ms, "
		"barriers={} calls={} ({:.2f}/call), scratch command/global/miss/high-water={}/{}/{}/{}, overlap hit/miss={}/{}, "
		"copy hit/miss/skipped={} / {} / {}MB, conversion hit/miss={}/{}, "
		"upload ring buffers/MB/peak/peakMB/reclaimed={}/{}/{}/{}/{}, readback={}/{}/{}/{}/{}.",
		phase,
		(unsigned long long)m_gamePsoCreateCount,
		(double)m_gamePsoCreateTimeUs / 1000.0,
		(double)m_gamePsoCreateMaxUs / 1000.0,
		(unsigned long long)m_singlePsoCacheHits,
		(unsigned long long)m_singlePsoCacheMisses,
		(unsigned long long)m_singlePsoCacheStores,
		(unsigned long long)m_singlePsoCacheRejects,
		(unsigned long long)m_gpuCommandListSamples,
		gpuMilliseconds,
		gpuMaxMilliseconds,
		(unsigned long long)m_fenceWaitCount,
		(double)m_fenceWaitTimeUs / 1000.0,
		(double)m_fenceWaitMaxUs / 1000.0,
		(unsigned long long)m_transitionBarrierCount,
		(unsigned long long)m_transitionBarrierCalls,
		barriersPerCall,
		(unsigned long long)m_commandScratchTextureHits,
		(unsigned long long)m_scratchTexturePoolHits,
		(unsigned long long)m_scratchTexturePoolMisses,
		(unsigned long long)m_commandScratchTextureHighWater,
		(unsigned long long)m_feedbackOverlapCacheHits,
		(unsigned long long)m_feedbackOverlapCacheMisses,
		(unsigned long long)m_textureCopyCacheHits,
		(unsigned long long)m_textureCopyCacheMisses,
		(unsigned long long)(m_textureCopyBytesSkipped / (1024ull * 1024ull)),
		(unsigned long long)m_surfaceConversionCacheHits,
		(unsigned long long)m_surfaceConversionCacheMisses,
		memoryStats.upload.buffers,
		(unsigned long long)(memoryStats.upload.totalBytes / (1024ull * 1024ull)),
		memoryStats.upload.peakBuffers,
		(unsigned long long)(memoryStats.upload.peakBytes / (1024ull * 1024ull)),
		(unsigned long long)memoryStats.upload.reclaimedBuffers,
		memoryStats.readback.buffers,
		(unsigned long long)(memoryStats.readback.totalBytes / (1024ull * 1024ull)),
		memoryStats.readback.peakBuffers,
		(unsigned long long)(memoryStats.readback.peakBytes / (1024ull * 1024ull)),
		(unsigned long long)memoryStats.readback.reclaimedBuffers);
}
#endif

#if defined(CEMU_UWP_D3D12_CORRECTNESS_TELEMETRY)
void D3D12Renderer::LogCorrectnessTelemetry(std::string_view phase) const
{
	cemuLog_log(LogType::Force,
		"D3D12 UWP correctness [{}]: presents={} draws attempt/submitted/seq/noPso/index/logic={}/{}/{}/{}/{}/{}, "
		"sequence shader/fbo/attachment/raster={}/{}/{}/{}, special clear/copy={}/{}, stencil/ordered={}/{}, "
		"fetch invalid/unbound/formatMask/overflow={}/{}/0x{:016x}/{}, primitiveMask=0x{:016x}, "
		"colorlessPS={}, depthCompare draws/units/dimMask/unsupported={}/{}/0x{:016x}/{}, "
		"views incompatible={} blendUnsupported={}, surface request/native/raw/shader/badView/badOutput/noPso={}/{}/{}/{}/{}/{}/{}.",
		phase,
		(unsigned long long)m_correctnessPresentCount,
		(unsigned long long)m_correctnessDrawAttempts,
		(unsigned long long)m_gameDrawCallsSubmitted,
		(unsigned long long)m_correctnessDrawSkippedSequence,
		(unsigned long long)m_correctnessDrawSkippedNoPso,
		(unsigned long long)m_correctnessDrawSkippedInvalidIndex,
		(unsigned long long)m_correctnessDrawSkippedLogicOp,
		(unsigned long long)m_correctnessSequenceShaderError,
		(unsigned long long)m_correctnessSequenceInvalidFbo,
		(unsigned long long)m_correctnessSequenceMissingAttachment,
		(unsigned long long)m_correctnessSequenceRasterDisabled,
		(unsigned long long)m_correctnessSpecialStateDepthClear,
		(unsigned long long)m_correctnessSpecialStateDepthCopy,
		(unsigned long long)m_correctnessStencilFallbackDraws,
		(unsigned long long)m_correctnessOrderedLogicOpDraws,
		(unsigned long long)m_correctnessInvalidFetchDraws,
		(unsigned long long)m_correctnessUnboundFetchDraws,
		(unsigned long long)m_correctnessFetchFormatMask,
		(unsigned long long)m_correctnessFetchFormatOverflow,
		(unsigned long long)m_correctnessPrimitiveMask,
		(unsigned long long)m_correctnessColorlessPixelDraws,
		(unsigned long long)m_correctnessDepthCompareDraws,
		(unsigned long long)m_correctnessDepthCompareUnitUses,
		(unsigned long long)m_correctnessDepthCompareDimMask,
		(unsigned long long)m_correctnessUnsupportedDepthCompareDraws,
		(unsigned long long)m_correctnessIncompatibleTextureViews,
		(unsigned long long)m_correctnessUnsupportedBlendStates,
		(unsigned long long)m_correctnessSurfaceCopyRequests,
		(unsigned long long)m_correctnessSurfaceCopyNative,
		(unsigned long long)m_correctnessSurfaceCopyRawAlias,
		(unsigned long long)m_correctnessSurfaceCopyShader,
		(unsigned long long)m_correctnessSurfaceCopyInvalidView,
		(unsigned long long)m_correctnessSurfaceCopyInvalidOutput,
		(unsigned long long)m_correctnessSurfaceCopyNoPso);
}

void D3D12Renderer::RecordIncompatibleTextureView(DXGI_FORMAT resourceFormat, DXGI_FORMAT viewFormat)
{
	++m_correctnessIncompatibleTextureViews;
	if (m_correctnessIncompatibleTextureViews <= 16)
	{
		cemuLog_log(LogType::Force,
			"D3D12 UWP: incompatible texture view resourceFormat={} viewFormat={} resourceFamily={} viewFamily={}.",
			(uint32)resourceFormat,
			(uint32)viewFormat,
			(uint32)GetD3D12TypelessFormat(resourceFormat),
			(uint32)GetD3D12TypelessFormat(viewFormat));
	}
}

#endif

void D3D12Renderer::CreateSparseMrtNullRtv()
{
	if (!m_descriptorManager)
		throw std::runtime_error("D3D12 sparse MRT null RTV requires a descriptor manager");

	m_sparseMrtNullRtv = m_descriptorManager->AllocatePersistent(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
	if (!m_sparseMrtNullRtv.IsValid() || m_sparseMrtNullRtv.cpu.ptr == 0)
		throw std::runtime_error("Failed to allocate D3D12 sparse MRT null RTV");

	// a real 1x1 RTV would clamp rasterization of the other targets to one pixel
	D3D12_RENDER_TARGET_VIEW_DESC rtvDesc{};
	rtvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
	m_device->CreateRenderTargetView(nullptr, &rtvDesc, m_sparseMrtNullRtv.cpu);
	cemuLog_log(LogType::Force, "D3D12: sparse MRT slots use null RTV descriptors (no 1x1 rasterization limit).");
	cemuLog_log(LogType::Force, "D3D12: feedback scratch pools retain requested texture descriptions for reuse across normalized resource alignment.");
}

void D3D12Renderer::CreateOutputRootSignature()
{
	D3D12_DESCRIPTOR_RANGE srvRange{};
	srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
	srvRange.NumDescriptors = 1;
	srvRange.BaseShaderRegister = 0;
	srvRange.RegisterSpace = 0;
	srvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

	D3D12_DESCRIPTOR_RANGE samplerRange{};
	samplerRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
	samplerRange.NumDescriptors = 1;
	samplerRange.BaseShaderRegister = 0;
	samplerRange.RegisterSpace = 0;
	samplerRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

	std::array<D3D12_ROOT_PARAMETER, 4> rootParameters{};
	rootParameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	rootParameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	rootParameters[0].DescriptorTable.NumDescriptorRanges = 1;
	rootParameters[0].DescriptorTable.pDescriptorRanges = &srvRange;

	rootParameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	rootParameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	rootParameters[1].DescriptorTable.NumDescriptorRanges = 1;
	rootParameters[1].DescriptorTable.pDescriptorRanges = &samplerRange;

	rootParameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
	rootParameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	rootParameters[2].Descriptor.ShaderRegister = 0;
	rootParameters[2].Descriptor.RegisterSpace = 0;

	// the output colour transfer reads the same uniforms through b0 in space 1
	rootParameters[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
	rootParameters[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	rootParameters[3].Descriptor.ShaderRegister = 0;
	rootParameters[3].Descriptor.RegisterSpace = 1;

	D3D12_ROOT_SIGNATURE_DESC desc{};
	desc.NumParameters = (UINT)rootParameters.size();
	desc.pParameters = rootParameters.data();
	desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

	Microsoft::WRL::ComPtr<ID3DBlob> signature;
	Microsoft::WRL::ComPtr<ID3DBlob> error;
	const HRESULT serializeHr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error);
	if (FAILED(serializeHr))
	{
		const char* message = error ? static_cast<const char*>(error->GetBufferPointer()) : "unknown";
		throw std::runtime_error(fmt::format("Failed to serialize D3D12 output root signature: {}", message));
	}
	if (FAILED(m_device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&m_outputRootSignature))))
		throw std::runtime_error("Failed to create D3D12 output root signature");
	SetD3D12ObjectName(m_outputRootSignature.Get(), L"Cemu D3D12 output root signature");
}

void D3D12Renderer::CreateOutputSamplers()
{
	auto createSampler = [this](D3D12_FILTER filter)
	{
		auto allocation = m_descriptorManager->AllocatePersistent(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
		D3D12_SAMPLER_DESC desc{};
		desc.Filter = filter;
		desc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		desc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		desc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		desc.MipLODBias = 0.0f;
		desc.MaxAnisotropy = 1;
		desc.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
		desc.MinLOD = 0.0f;
		desc.MaxLOD = D3D12_FLOAT32_MAX;
		m_device->CreateSampler(&desc, allocation.cpu);
		return allocation;
	};

	m_outputLinearSampler = createSampler(D3D12_FILTER_MIN_MAG_MIP_LINEAR);
	m_outputPointSampler = createSampler(D3D12_FILTER_MIN_MAG_MIP_POINT);
}

void D3D12Renderer::CreateGameRootSignature()
{
	std::array<D3D12_DESCRIPTOR_RANGE, 3> srvRanges{};
	std::array<D3D12_DESCRIPTOR_RANGE, 3> uavRanges{};
	std::array<D3D12_DESCRIPTOR_RANGE, 3> samplerRanges{};
	std::array<D3D12_ROOT_PARAMETER, 13> rootParameters{};
	for (uint32 stage = 0; stage < 3; ++stage)
	{
		srvRanges[stage].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
		srvRanges[stage].NumDescriptors = kGameTextureSrvDescriptorCount;
		srvRanges[stage].BaseShaderRegister = 0;
		srvRanges[stage].RegisterSpace = stage;
		srvRanges[stage].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

		uavRanges[stage].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
		uavRanges[stage].NumDescriptors = kGameUavDescriptorCount;
		uavRanges[stage].BaseShaderRegister = 0;
		uavRanges[stage].RegisterSpace = stage;
		uavRanges[stage].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

		samplerRanges[stage].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
		samplerRanges[stage].NumDescriptors = kGameSamplerDescriptorCount;
		samplerRanges[stage].BaseShaderRegister = 0;
		samplerRanges[stage].RegisterSpace = stage;
		samplerRanges[stage].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

		rootParameters[stage].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		rootParameters[stage].DescriptorTable.NumDescriptorRanges = 1;
		rootParameters[stage].DescriptorTable.pDescriptorRanges = &srvRanges[stage];
		rootParameters[stage].ShaderVisibility = stage == 0 ? D3D12_SHADER_VISIBILITY_VERTEX :
												 stage == 1 ? D3D12_SHADER_VISIBILITY_PIXEL :
															  D3D12_SHADER_VISIBILITY_GEOMETRY;

		rootParameters[3 + stage].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
		rootParameters[3 + stage].Descriptor.ShaderRegister = 0;
		rootParameters[3 + stage].Descriptor.RegisterSpace = stage;
		rootParameters[3 + stage].ShaderVisibility = rootParameters[stage].ShaderVisibility;

		rootParameters[7 + stage].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		rootParameters[7 + stage].DescriptorTable.NumDescriptorRanges = 1;
		rootParameters[7 + stage].DescriptorTable.pDescriptorRanges = &samplerRanges[stage];
		rootParameters[7 + stage].ShaderVisibility = rootParameters[stage].ShaderVisibility;

		rootParameters[10 + stage].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		rootParameters[10 + stage].DescriptorTable.NumDescriptorRanges = 1;
		rootParameters[10 + stage].DescriptorTable.pDescriptorRanges = &uavRanges[stage];
		rootParameters[10 + stage].ShaderVisibility = rootParameters[stage].ShaderVisibility;
	}

	rootParameters[6].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
	rootParameters[6].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
	rootParameters[6].Constants.ShaderRegister = 1;
	rootParameters[6].Constants.RegisterSpace = 1;
	rootParameters[6].Constants.Num32BitValues = kGameDrawConstantCount;

	D3D12_ROOT_SIGNATURE_DESC desc{};
	desc.NumParameters = (UINT)rootParameters.size();
	desc.pParameters = rootParameters.data();
	desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

	Microsoft::WRL::ComPtr<ID3DBlob> signature;
	Microsoft::WRL::ComPtr<ID3DBlob> error;
	const HRESULT serializeHr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error);
	if (FAILED(serializeHr))
	{
		const char* message = error ? static_cast<const char*>(error->GetBufferPointer()) : "unknown";
		throw std::runtime_error(fmt::format("Failed to serialize D3D12 game root signature: {}", message));
	}
	if (FAILED(m_device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&m_gameRootSignature))))
		throw std::runtime_error("Failed to create D3D12 game root signature");
	SetD3D12ObjectName(m_gameRootSignature.Get(), L"Cemu D3D12 game root signature");

	m_nullCbvDescriptor = m_descriptorManager->AllocatePersistent(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	m_device->CreateConstantBufferView(nullptr, m_nullCbvDescriptor.cpu);

	m_nullSrvDescriptor = m_descriptorManager->AllocatePersistent(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	D3D12_SHADER_RESOURCE_VIEW_DESC nullSrvDesc{};
	nullSrvDesc.Format = DXGI_FORMAT_R32_UINT;
	nullSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
	nullSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	nullSrvDesc.Buffer.FirstElement = 0;
	nullSrvDesc.Buffer.NumElements = 1;
	nullSrvDesc.Buffer.StructureByteStride = 0;
	nullSrvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
	m_device->CreateShaderResourceView(nullptr, &nullSrvDesc, m_nullSrvDescriptor.cpu);

	m_nullSrvDescriptorRange = m_descriptorManager->AllocatePersistent(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kGameTextureSrvDescriptorCount);
	{
		const uint32 nullRangeDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
		for (uint32 i = 0; i < kGameTextureSrvDescriptorCount; ++i)
			m_device->CreateShaderResourceView(nullptr, &nullSrvDesc, OffsetCpuHandle(m_nullSrvDescriptorRange.cpu, nullRangeDescriptorSize, i));
	}

	m_nullUavDescriptor = m_descriptorManager->AllocatePersistent(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	D3D12_UNORDERED_ACCESS_VIEW_DESC nullUavDesc{};
	nullUavDesc.Format = DXGI_FORMAT_UNKNOWN;
	nullUavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
	nullUavDesc.Buffer.FirstElement = 0;
	nullUavDesc.Buffer.NumElements = 1;
	nullUavDesc.Buffer.StructureByteStride = 4;
	nullUavDesc.Buffer.CounterOffsetInBytes = 0;
	nullUavDesc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;
	m_device->CreateUnorderedAccessView(nullptr, nullptr, &nullUavDesc, m_nullUavDescriptor.cpu);
}

void D3D12Renderer::CreateSurfaceCopyRootSignature()
{
	D3D12_DESCRIPTOR_RANGE srvRange{};
	srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
	srvRange.NumDescriptors = 1;
	srvRange.BaseShaderRegister = 0;
	srvRange.RegisterSpace = 0;
	srvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

	D3D12_DESCRIPTOR_RANGE samplerRange{};
	samplerRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
	samplerRange.NumDescriptors = 1;
	samplerRange.BaseShaderRegister = 0;
	samplerRange.RegisterSpace = 0;
	samplerRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

	std::array<D3D12_ROOT_PARAMETER, 3> rootParameters{};
	rootParameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	rootParameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	rootParameters[0].DescriptorTable.NumDescriptorRanges = 1;
	rootParameters[0].DescriptorTable.pDescriptorRanges = &srvRange;

	rootParameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	rootParameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	rootParameters[1].DescriptorTable.NumDescriptorRanges = 1;
	rootParameters[1].DescriptorTable.pDescriptorRanges = &samplerRange;

	rootParameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
	rootParameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	rootParameters[2].Constants.ShaderRegister = 0;
	rootParameters[2].Constants.RegisterSpace = 0;
	rootParameters[2].Constants.Num32BitValues = 4;

	D3D12_ROOT_SIGNATURE_DESC desc{};
	desc.NumParameters = (UINT)rootParameters.size();
	desc.pParameters = rootParameters.data();
	desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

	Microsoft::WRL::ComPtr<ID3DBlob> signature;
	Microsoft::WRL::ComPtr<ID3DBlob> error;
	const HRESULT serializeHr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error);
	if (FAILED(serializeHr))
	{
		const char* message = error ? static_cast<const char*>(error->GetBufferPointer()) : "unknown";
		throw std::runtime_error(fmt::format("Failed to serialize D3D12 surface-copy root signature: {}", message));
	}
	if (FAILED(m_device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&m_surfaceCopyRootSignature))))
		throw std::runtime_error("Failed to create D3D12 surface-copy root signature");
	SetD3D12ObjectName(m_surfaceCopyRootSignature.Get(), L"Cemu D3D12 surface-copy root signature");
}

void D3D12Renderer::CreateImguiDescriptorHeap()
{
	D3D12_DESCRIPTOR_HEAP_DESC desc{};
	desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	desc.NumDescriptors = m_imguiDescriptorCapacity;
	desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	if (FAILED(m_device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&m_imguiSrvHeap))))
		throw std::runtime_error("Failed to create D3D12 ImGui descriptor heap");
	SetD3D12ObjectName(m_imguiSrvHeap.Get(), L"Cemu D3D12 ImGui SRV heap");
	m_imguiDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	m_imguiNextDescriptorIndex = 1;
	m_imguiFreeDescriptorIndices.clear();
}

uint32 D3D12Renderer::AllocateImguiDescriptor()
{
	ProcessFinishedCommandLists();
	if (!m_imguiFreeDescriptorIndices.empty())
	{
		const uint32 index = m_imguiFreeDescriptorIndices.back();
		m_imguiFreeDescriptorIndices.pop_back();
		return index;
	}
	if (m_imguiNextDescriptorIndex >= m_imguiDescriptorCapacity)
		throw std::runtime_error("D3D12 ImGui descriptor heap exhausted");
	return m_imguiNextDescriptorIndex++;
}

D3D12_CPU_DESCRIPTOR_HANDLE D3D12Renderer::GetImguiCpuHandle(uint32 index) const
{
	D3D12_CPU_DESCRIPTOR_HANDLE handle = m_imguiSrvHeap->GetCPUDescriptorHandleForHeapStart();
	handle.ptr += (size_t)index * m_imguiDescriptorSize;
	return handle;
}

D3D12_GPU_DESCRIPTOR_HANDLE D3D12Renderer::GetImguiGpuHandle(uint32 index) const
{
	D3D12_GPU_DESCRIPTOR_HANDLE handle = m_imguiSrvHeap->GetGPUDescriptorHandleForHeapStart();
	handle.ptr += (uint64)index * m_imguiDescriptorSize;
	return handle;
}

void D3D12Renderer::Initialize()
{
	if (m_initialized)
		return;
	D3D12TraceEvent("renderer_initialize", "device={} feature=0x{:x} shaderModel=0x{:x} bindingTier={} tiledTier={} traceGpu={}",
					(const void*)m_device.Get(),
					(uint32)m_featureLevel,
					(uint32)m_shaderModel,
					(uint32)m_resourceBindingTier,
					(uint32)m_tiledResourcesTier,
					D3D12ShouldTraceGpu() ? 1 : 0);
	Renderer::Initialize();
#if defined(CEMU_UWP_D3D12_DIAGNOSTICS)
	m_geometryProbe = std::make_unique<D3D12GeometryProbe>();
#endif
	auto initImguiContext = [this](ImGuiContext* context)
	{
		ImGui::SetCurrentContext(context);
		constexpr uint32 imguiFramesInFlight = kBackBufferCount;
		if (!ImGui_ImplDX12_Init(m_device.Get(), imguiFramesInFlight, DXGI_FORMAT_R8G8B8A8_UNORM, m_imguiSrvHeap.Get(), GetImguiCpuHandle(0), GetImguiGpuHandle(0)))
			throw std::runtime_error("Failed to initialize D3D12 ImGui backend");
	};
	initImguiContext(imguiTVContext);
	initImguiContext(imguiPadContext);
	m_imguiInitialized = true;
	m_initialized = true;
	// the recorder starts with the first game draw: with it running from here the frontend menu hung on Xbox (its
	// first overlay command list never finished on the GPU), and the menu has nothing to gain from it
	m_threadedRecordingWanted = GetConfig().d3d12_threaded_recording;
	if (m_threadedRecordingWanted)
		cemuLog_log(LogType::Force, "D3D12: threaded draw recording is on; it starts with the first game draw.");
}

void D3D12Renderer::PrepareForTitleShutdown()
{
	D3D12_EXCLUSIVE_RECORDING();
	if (!m_initialized || m_shutdownComplete)
		return;
	// back to direct recording for the frontend; the next title starts the recorder again
	StopRecordingThread();

	D3D12TraceEventFlush("renderer_title_shutdown_begin",
		"draws={} submittedLists={} completedLists={} deferredBatches={}",
		(unsigned long long)m_gameDrawCallsSubmitted,
		(unsigned long long)m_numSubmittedCommandLists,
		(unsigned long long)m_numCompletedCommandLists,
		m_deferredReleaseQueue.size());
	// background prewarm would only compete with the shutdown for the CPU
	D3D12PipelineStableCache::GetInstance().StopPrewarm();
	PersistGamePipelineLibrary();
	if (m_device)
		WaitForIdle();
#if defined(CEMU_UWP_D3D12_DIAGNOSTICS)
	LogPerformanceSummary("title shutdown");
#endif
#if defined(CEMU_UWP_D3D12_CORRECTNESS_TELEMETRY)
	LogCorrectnessTelemetry("title shutdown");
#endif

	// must be released before Latte's texture cache destroys the guest objects
	if (m_uwpUnderlayTexture.id)
		DeleteTexture(m_uwpUnderlayTexture.id);
	m_uwpUnderlayTexture = {};
	ResetUwpOutputSnapshots();
	for (auto& readback : m_uwpStreamReadbacks)
		readback = {};

	m_boundTextures.fill(nullptr);
	BumpAllTableBindingGenerations();
	m_activeFBO = nullptr;
	m_feedbackScratchCopies.clear();
	m_retainedFeedbackCopies.clear();
	ResetScratchCopyLookup();
	m_feedbackOverlapCache = {};
	m_textureCopyCache.clear();
	ReleaseScratchTexturePool();
	ResetCommandStateCache();
	D3D12TraceEventFlush("renderer_title_shutdown_prepared",
		"submittedLists={} completedLists={} deferredBatches={}",
		(unsigned long long)m_numSubmittedCommandLists,
		(unsigned long long)m_numCompletedCommandLists,
		m_deferredReleaseQueue.size());
}

void D3D12Renderer::FinishTitleShutdown()
{
	if (!m_initialized || m_shutdownComplete)
		return;
	D3D12_EXCLUSIVE_RECORDING();
	FlushPendingDestroys();
	m_latteActiveFBO = nullptr;
	m_latteStreamoutRingOffsets = {};
	m_latteUniformSources = {};

	D3D12PipelineStableCache::GetInstance().StopPrewarm();
	ClearPendingGamePipelineRecipes();
	// drop caches referencing title-owned objects; device, swapchain, PSOs persist
	m_boundTextures.fill(nullptr);
	BumpAllTableBindingGenerations();
	m_activeFBO = nullptr;
	m_feedbackOverlapCache = {};
	m_vertexBindings = {};
	m_uniformBindings = {};
	m_streamoutBindings = {};
	m_streamoutVerticesPerInstance = 0;
	m_resourceTableKeyMemo = {};
	m_samplerTableKeyMemo = {};
	m_gameUniformUploadCache = {};
	m_gameStreamoutUavTable = {};
	m_gameStreamoutUavTableKey = 0;
	m_gameStreamoutUavTableGeneration = kInvalidDescriptorIndex;
	InvalidateGameShaderVisibleDescriptorCaches();
	InvalidateGameRootParameterCache();
	m_textureLineage.clear();
	m_traceTextureStatsCache.clear();
	m_textureCopyCache.clear();
	m_logicOpEmulationMode = 0;
	m_disableNativeLogicOpForDraw = false;
	m_drawSequenceActive = false;
	m_drawSequenceSkip = false;
	m_drawNeedsFullBufferSync = false;
	m_loggedDrawFailureSignatures.clear();

	ReleaseResourceDeferred(m_bufferCache);
	ReleaseResourceDeferred(m_bufferCacheCopyTemp);
	ReleaseResourceDeferred(m_textureAliasCopyBuffer);
	ReleaseResourceDeferred(m_streamoutBuffer);
	ReleaseOutputPrePassTargets();
	m_bufferCacheShadow.clear();
	ProcessFinishedCommandLists();
	D3D12TraceEventFlush("renderer_title_shutdown_complete",
		"submittedLists={} completedLists={} deferredBatches={}",
		(unsigned long long)m_numSubmittedCommandLists,
		(unsigned long long)m_numCompletedCommandLists,
		m_deferredReleaseQueue.size());
}

void D3D12Renderer::Shutdown()
{
	if (m_shutdownComplete)
		return;
	StopRecordingThread();
	FlushPendingDestroys();
	m_gpuWatchdog.Stop();
	D3D12TraceEventFlush("renderer_shutdown", "draws={} submittedLists={} completedLists={} deferredBatches={} feedbackCopies={} logicOpCopies={} psoFailures={}",
						 (unsigned long long)m_gameDrawCallsSubmitted,
						 (unsigned long long)m_numSubmittedCommandLists,
						 (unsigned long long)m_numCompletedCommandLists,
						 m_deferredReleaseQueue.size(),
						 (unsigned long long)m_feedbackLoopCopies,
						 (unsigned long long)m_logicOpEmulationCopies,
						 (unsigned long long)m_gamePsoCreateFailures);
	// prewarm and async pipeline workers use the device and root signature
	D3D12PipelineStableCache::GetInstance().StopPrewarm();
	StopAsyncPipelineThreads();
	StopCpuSliceUploadThreads(); // drains queued writes first
	PersistGamePipelineLibrary();
	RendererShaderD3D12::Shutdown();
	if (m_device)
	{
		D3D12TraceEventFlush("renderer_shutdown_wait_idle_begin", "submittedLists={} completedLists={} nextFence={} completedFence={}",
							 (unsigned long long)m_numSubmittedCommandLists,
							 (unsigned long long)m_numCompletedCommandLists,
							 (unsigned long long)m_nextFenceValue,
							 (unsigned long long)m_lastCompletedFenceValue);
		try
		{
			WaitForIdle();
		}
		catch (const std::exception& ex)
		{
			cemuLog_log(LogType::Force, "D3D12: shutdown is continuing after GPU idle wait failed: {}", ex.what());
		}
		D3D12TraceEventFlush("renderer_shutdown_wait_idle_end", "submittedLists={} completedLists={} nextFence={} completedFence={}",
							 (unsigned long long)m_numSubmittedCommandLists,
							 (unsigned long long)m_numCompletedCommandLists,
							 (unsigned long long)m_nextFenceValue,
							 (unsigned long long)m_lastCompletedFenceValue);
	}
#if defined(CEMU_UWP_D3D12_DIAGNOSTICS)
	LogPerformanceSummary("renderer shutdown");
#endif
#if defined(CEMU_UWP_D3D12_DIAGNOSTICS) || defined(CEMU_UWP_D3D12_WORKLOAD_PROFILE) || defined(CEMU_UWP)
	ReleaseTimingResources();
#endif
	// Probe query heaps/readbacks must outlive the command lists using them.
	m_geometryProbe.reset();
#if defined(CEMU_UWP_D3D12_CORRECTNESS_TELEMETRY)
	LogCorrectnessTelemetry("renderer shutdown");
#endif
	if (m_initialized)
	{
		if (m_uwpUnderlayTexture.id)
			DeleteTexture(m_uwpUnderlayTexture.id);
		m_uwpUnderlayTexture = {};
		ResetUwpOutputSnapshots();
		DeleteFontTextures();
		if (m_imguiInitialized)
		{
			ImGui::SetCurrentContext(imguiTVContext);
			ImGui_ImplDX12_Shutdown();
			ImGui::SetCurrentContext(imguiPadContext);
			ImGui_ImplDX12_Shutdown();
			m_imguiInitialized = false;
		}
		Renderer::Shutdown();
		m_initialized = false;
	}
	m_boundTextures.fill(nullptr);
	BumpAllTableBindingGenerations();
	m_activeFBO = nullptr;
	m_feedbackScratchCopies.clear();
	m_retainedFeedbackCopies.clear();
	ResetScratchCopyLookup();
	m_feedbackOverlapCache = {};
	m_textureCopyCache.clear();
	ReleaseScratchTexturePool();
	m_imguiTextures.clear();
	m_mainSwapchain = {};
	m_padSwapchain = {};
	m_outputPipelineCache.clear();
	m_gamePipelineCache.clear();
	m_depthBiasSiblingPipelines.clear();
	m_arrayUploadStaging.clear();
	ClearPendingGamePipelineRecipes();
	m_gamePipelineRetryFrame.clear();
	m_gamePipelineLibrary.Reset();
	m_gamePipelineLibraryBlob.clear();
	m_gamePipelineLibraryTitleId = 0;
	m_gamePipelineLibraryDirty = false;
	m_pointExpansionGeometryShaders.clear();
	m_rectEmulationGeometryShaders.clear();
	m_surfaceCopyPipelineCache.clear();
	m_surfaceCopyShaders.clear();
	m_outputLinearSampler = {};
	m_outputPointSampler = {};
	for (auto& target : m_outputPrePassTargets)
		target = {};
	InvalidateGameShaderVisibleDescriptorCaches();
	m_nullCbvDescriptor = {};
	m_nullSrvDescriptor = {};
	m_nullSrvDescriptorRange = {};
	m_nullUavDescriptor = {};
	m_sparseMrtNullRtv = {};
	m_bufferCache = {};
	m_streamoutBuffer = {};
	m_occlusionQueryPages.clear();
	m_retiredOcclusionQuerySlots.clear();
	m_activeOcclusionQueries.clear();
	m_deferredReleaseQueue.clear();
	m_retiredImguiDescriptors.clear();
	m_imguiFreeDescriptorIndices.clear();
	m_imguiSrvHeap.Reset();
	m_outputRootSignature.Reset();
	m_gameRootSignature.Reset();
	m_surfaceCopyRootSignature.Reset();
	m_descriptorManager.reset();
	m_rtvHeap.Reset();
	if (m_fenceEvent)
	{
		CloseHandle(m_fenceEvent);
		m_fenceEvent = nullptr;
	}
	// LatteThread_Exit frees textures after Shutdown returns; D3D12MA must outlive them
	m_shutdownComplete = true;
	D3D12TraceEventFlush("renderer_shutdown_complete", "draws={} submittedLists={} completedLists={}",
						 (unsigned long long)m_gameDrawCallsSubmitted,
						 (unsigned long long)m_numSubmittedCommandLists,
						 (unsigned long long)m_numCompletedCommandLists);
}

void D3D12Renderer::InitializeSurface(const Vector2i& size, bool mainWindow)
{
	D3D12_EXCLUSIVE_RECORDING();
	auto& windowInfo = mainWindow ? WindowSystem::GetWindowInfo().canvas_main : WindowSystem::GetWindowInfo().canvas_pad;
	if (windowInfo.backend != WindowSystem::WindowHandleInfo::Backend::WindowsCoreWindow || windowInfo.surface == nullptr)
		throw std::runtime_error("D3D12 UWP requires a Windows CoreWindow surface");
	CreateOrResizeSwapchain(GetSwapchain(mainWindow), size, windowInfo.backend, windowInfo.surface);
}

bool D3D12Renderer::GetScreenOutputRect(bool padView, sint32& x, sint32& y, sint32& width, sint32& height) const
{
	const auto& chain = GetSwapchain(true);
	if (!chain.active)
		return false;
	const UwpOutputRect rect = ResolveUwpOutputRect(padView, chain.size);
	if (!rect.visible || rect.w <= 0 || rect.h <= 0)
		return false;
	x = rect.x;
	y = rect.y;
	width = rect.w;
	height = rect.h;
	return true;
}

void D3D12Renderer::ResizeSurface(const Vector2i& size, bool mainWindow)
{
	D3D12_EXCLUSIVE_RECORDING();
	auto& chain = GetSwapchain(mainWindow);
	if (!chain.active || size.x <= 0 || size.y <= 0 || chain.size == size)
		return;
	CreateOrResizeSwapchain(chain, size, chain.backend, chain.surface);
}

void D3D12Renderer::CreateOrResizeSwapchain(SwapchainState& chain, const Vector2i& size, WindowSystem::WindowHandleInfo::Backend backend, void* surface)
{
	WaitForIdle();
	chain.backend = backend;
	chain.surface = surface;
	chain.size = size;
	if (!chain.swapchain)
	{
		DXGI_SWAP_CHAIN_DESC1 desc{};
		desc.Width = std::max(1, size.x);
		desc.Height = std::max(1, size.y);
		desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		desc.SampleDesc.Count = 1;
		desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
		desc.BufferCount = kBackBufferCount;
		desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
		desc.Scaling = DXGI_SCALING_STRETCH;
		desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

		Microsoft::WRL::ComPtr<IDXGISwapChain1> swapchain1;
#if defined(CEMU_UWP)
		if (backend != WindowSystem::WindowHandleInfo::Backend::WindowsCoreWindow)
			throw std::runtime_error("D3D12 UWP swapchain requires a CoreWindow surface");
		auto coreWindow = winrt::Windows::UI::Core::CoreWindow{ nullptr };
		winrt::copy_from_abi(coreWindow, surface);
		auto coreWindowUnknown = reinterpret_cast<IUnknown*>(winrt::get_abi(coreWindow));
		const HRESULT createSwapchainResult = m_factory->CreateSwapChainForCoreWindow(m_directQueue.Get(), coreWindowUnknown, &desc, nullptr, &swapchain1);
#else
		if (backend != WindowSystem::WindowHandleInfo::Backend::Windows)
			throw std::runtime_error("D3D12 swapchain requires a Windows HWND surface");
		const HRESULT createSwapchainResult = m_factory->CreateSwapChainForHwnd(m_directQueue.Get(), static_cast<HWND>(surface), &desc, nullptr, nullptr, &swapchain1);
#endif
		if (FAILED(createSwapchainResult))
			throw std::runtime_error("Failed to create D3D12 swapchain");
		if (FAILED(swapchain1.As(&chain.swapchain)))
			throw std::runtime_error("Failed to query IDXGISwapChain3");
	}
	else
	{
		for (auto& backBuffer : chain.backBuffers)
			backBuffer.Reset();
		const HRESULT hr = chain.swapchain->ResizeBuffers(kBackBufferCount, std::max(1, size.x), std::max(1, size.y), DXGI_FORMAT_R8G8B8A8_UNORM, 0);
		CheckDeviceRemoved(hr, "ResizeBuffers");
		if (FAILED(hr))
			throw std::runtime_error("Failed to resize D3D12 swapchain");
	}
	chain.active = true;
	chain.frameIndex = chain.swapchain->GetCurrentBackBufferIndex();
	CreateBackBufferViews(chain);
}

void D3D12Renderer::CreateBackBufferViews(SwapchainState& chain)
{
	if (chain.rtvBaseIndex == kInvalidDescriptorIndex)
	{
		chain.rtvBaseIndex = m_nextRtvIndex;
		m_nextRtvIndex += kBackBufferCount;
	}
	D3D12_CPU_DESCRIPTOR_HANDLE start = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
	for (uint32 i = 0; i < kBackBufferCount; ++i)
	{
		if (FAILED(chain.swapchain->GetBuffer(i, IID_PPV_ARGS(&chain.backBuffers[i]))))
			throw std::runtime_error("Failed to get D3D12 swapchain backbuffer");
		chain.rtvHandles[i].ptr = start.ptr + (chain.rtvBaseIndex + i) * m_rtvDescriptorSize;
		m_device->CreateRenderTargetView(chain.backBuffers[i].Get(), nullptr, chain.rtvHandles[i]);
		chain.states[i] = D3D12_RESOURCE_STATE_PRESENT;
	}
}

void D3D12Renderer::InvalidateGameCbvSrvUavDescriptorCaches()
{
	m_gameNullResourceTable = {};
	m_gameNullResourceTableGeneration = kInvalidDescriptorIndex;
	m_gameNullUavTable = {};
	m_gameNullUavTableGeneration = kInvalidDescriptorIndex;
	m_gameResourceTableCache.clear();
	m_gameResourceTableCacheGeneration = kInvalidDescriptorIndex;
	m_resourceTableKeyMemo = {};
	m_gameStreamoutUavTable = {};
	m_gameStreamoutUavTableKey = 0;
	m_gameStreamoutUavTableGeneration = kInvalidDescriptorIndex;
}

void D3D12Renderer::InvalidateGameSamplerDescriptorCaches()
{
	m_gameSamplerTableCache.clear();
	m_gameSamplerTableCacheGeneration = kInvalidDescriptorIndex;
	m_samplerTableKeyMemo = {};
	m_shaderVisibleOutputLinearSampler = {};
	m_shaderVisibleOutputPointSampler = {};
	m_shaderVisibleOutputSamplerGeneration = kInvalidDescriptorIndex;
}

void D3D12Renderer::InvalidateGameShaderVisibleDescriptorCaches()
{
	InvalidateGameCbvSrvUavDescriptorCaches();
	InvalidateGameSamplerDescriptorCaches();
}

uint32 D3D12Renderer::GetGameResourceTableDescriptorCount(LatteDecompilerShader* shader) const
{
	if (!shader)
		return 1;

	uint32 descriptorCount = 1;
	auto requireDescriptor = [&descriptorCount](uint32 descriptorIndex)
	{
		descriptorCount = std::max(descriptorCount, descriptorIndex + 1);
	};

	for (uint32 textureListIndex = 0; textureListIndex < shader->textureUnitListCount; ++textureListIndex)
	{
		const uint32 relativeUnit = shader->textureUnitList[textureListIndex];
		const sint8 bindingPoint = shader->resourceMapping.textureUnitToBindingPoint[relativeUnit];
		if (bindingPoint >= 0 && (uint32)bindingPoint < kGameTextureSrvDescriptorCount)
			requireDescriptor((uint32)bindingPoint);
	}

	for (uint32 bufferIndex = 0; bufferIndex < LATTE_NUM_MAX_UNIFORM_BUFFERS; ++bufferIndex)
	{
		const sint8 bindingPoint = shader->resourceMapping.uniformBuffersBindingPoint[bufferIndex];
		if (bindingPoint >= 0 && (uint32)bindingPoint < kGameTextureSrvDescriptorCount)
			requireDescriptor((uint32)bindingPoint);
	}

	if (shader->shaderType == LatteConst::ShaderType::Vertex)
	{
		auto* fetchShader = s_recordFetchShader;
		if (fetchShader)
		{
			for (auto& group : fetchShader->bufferGroups)
			{
				const uint32 bindingPoint = kGameVertexBufferSrvBase + group.attributeBufferIndex;
				if (bindingPoint < kGameTextureSrvDescriptorCount)
					requireDescriptor(bindingPoint);
			}
		}
	}

	if (shader->shaderType == LatteConst::ShaderType::Pixel && m_logicOpEmulationMode != 0)
		requireDescriptor(kGameLogicOpDstSrvBinding + kGameLogicOpDstSrvCount - 1);

	return std::min<uint32>(descriptorCount, kGameTextureSrvDescriptorCount);
}

uint32 D3D12Renderer::GetGameUavTableDescriptorCount(LatteDecompilerShader* shader) const
{
	return shader && shader->resourceMapping.tfStorageBindingPoint >= 0 &&
		(uint32)shader->resourceMapping.tfStorageBindingPoint < kGameUavDescriptorCount ? kGameUavDescriptorCount : 0;
}

uint32 D3D12Renderer::GetGameSamplerTableDescriptorCount(LatteDecompilerShader* shader) const
{
	if (!shader)
		return 1;

	uint32 descriptorCount = 1;
	for (uint32 textureListIndex = 0; textureListIndex < shader->textureUnitListCount; ++textureListIndex)
	{
		const uint32 relativeUnit = shader->textureUnitList[textureListIndex];
		const sint8 bindingPoint = shader->resourceMapping.textureUnitToBindingPoint[relativeUnit];
		if (bindingPoint >= 0 && (uint32)bindingPoint < kGameSamplerDescriptorCount)
			descriptorCount = std::max<uint32>(descriptorCount, (uint32)bindingPoint + 1);
	}
	return descriptorCount;
}

uint64 D3D12Renderer::BuildGameResourceTableCacheKey(LatteDecompilerShader* shader, uint32 descriptorGeneration, bool& cacheable)
{
	cacheable = false;
	if (!shader || !m_descriptorManager)
		return 0;

	bool hasCurrentDrawFeedbackScratch = std::any_of(m_feedbackScratchByTextureUnit.begin(),
		m_feedbackScratchByTextureUnit.end(), [](uint32 index) { return index != kInvalidScratchCopyIndex; });
	bool checkStageFeedback = PerformanceExperimentEnabled(D3D12PerformanceExperiment::StageFeedbackCache);
#if defined(CEMU_UWP_PERFORMANCE_EXPERIMENT)
	checkStageFeedback = true;
#endif
	if (hasCurrentDrawFeedbackScratch && checkStageFeedback)
	{
		uint32 stageHostBase = 0;
		bool validStage = true;
		switch (shader->shaderType)
		{
		case LatteConst::ShaderType::Vertex: stageHostBase = LATTE_CEMU_VS_TEX_UNIT_BASE; break;
		case LatteConst::ShaderType::Pixel: stageHostBase = LATTE_CEMU_PS_TEX_UNIT_BASE; break;
		case LatteConst::ShaderType::Geometry: stageHostBase = LATTE_CEMU_GS_TEX_UNIT_BASE; break;
		default: validStage = false; break;
		}
		const bool stageFeedback = D3D12PerformanceExperiment::HasStageFeedback(m_feedbackScratchByTextureUnit,
			kInvalidScratchCopyIndex, shader->textureUnitList, shader->textureUnitListCount,
			stageHostBase, LATTE_NUM_MAX_TEX_UNITS, validStage);
#if defined(CEMU_UWP_PERFORMANCE_EXPERIMENT)
		if (!stageFeedback && !(shader->shaderType == LatteConst::ShaderType::Pixel && m_logicOpEmulationMode != 0))
			++m_performanceExperimentWindow.stageCacheEligible;
#endif
		if (PerformanceExperimentEnabled(D3D12PerformanceExperiment::StageFeedbackCache))
			hasCurrentDrawFeedbackScratch = stageFeedback;
	}
	if (hasCurrentDrawFeedbackScratch)
		return 0;
	if (shader->shaderType == LatteConst::ShaderType::Pixel && m_logicOpEmulationMode != 0)
		return 0;

	uint64 key = 0xd312d15c0de00001ull;
	key = MixGameDescriptorTableKey(key, descriptorGeneration);
	key = MixGameDescriptorTableKey(key, (uint64)shader->shaderType);
	key = MixGameDescriptorTableKey(key, shader->baseHash);
	key = MixGameDescriptorTableKey(key, shader->auxHash);
	key = MixGameDescriptorTableKey(key, (uint64)reinterpret_cast<size_t>(shader));
	key = MixGameDescriptorTableKey(key, GetGameResourceTableDescriptorCount(shader));
	key = MixGameDescriptorTableKey(key, (uint64)reinterpret_cast<size_t>(m_bufferCache.resource.Get()));

	const size_t stageIndex = (size_t)shader->shaderType;
	if (stageIndex < m_uniformBindings.size())
	{
		for (uint32 bufferIndex = 0; bufferIndex < LATTE_NUM_MAX_UNIFORM_BUFFERS; ++bufferIndex)
		{
			const sint8 bindingPoint = shader->resourceMapping.uniformBuffersBindingPoint[bufferIndex];
			if (bindingPoint < 0 || (uint32)bindingPoint >= kGameTextureSrvDescriptorCount)
				continue;
			const auto& binding = m_uniformBindings[stageIndex][bufferIndex];
			key = MixGameDescriptorTableKey(key, 0x10000000ull + bufferIndex);
			key = MixGameDescriptorTableKey(key, (uint64)(uint8)bindingPoint);
			key = MixGameDescriptorTableKey(key, binding.valid ? 1 : 0);
			key = MixGameDescriptorTableKey(key, binding.offset);
			key = MixGameDescriptorTableKey(key, binding.size);
		}
	}

	if (shader->shaderType == LatteConst::ShaderType::Vertex)
	{
		auto* fetchShader = s_recordFetchShader;
		key = MixGameDescriptorTableKey(key, (uint64)reinterpret_cast<size_t>(fetchShader));
		if (fetchShader)
		{
			for (auto& group : fetchShader->bufferGroups)
			{
				const uint32 bufferIndex = group.attributeBufferIndex;
				const uint32 bindingPoint = kGameVertexBufferSrvBase + bufferIndex;
				if (bufferIndex >= m_vertexBindings.size() || bindingPoint >= kGameTextureSrvDescriptorCount)
					continue;
				const auto& binding = m_vertexBindings[bufferIndex];
				key = MixGameDescriptorTableKey(key, 0x20000000ull + bufferIndex);
				key = MixGameDescriptorTableKey(key, binding.valid ? 1 : 0);
				key = MixGameDescriptorTableKey(key, binding.offset);
				key = MixGameDescriptorTableKey(key, binding.size);
			}
		}
	}

	uint32 hostTextureUnitBase = 0;
	const _LatteRegisterSetTextureUnit* texRegs = nullptr;
	switch (shader->shaderType)
	{
	case LatteConst::ShaderType::Vertex:
		hostTextureUnitBase = LATTE_CEMU_VS_TEX_UNIT_BASE;
		texRegs = RecordRegs().SQ_TEX_START_VS;
		break;
	case LatteConst::ShaderType::Pixel:
		hostTextureUnitBase = LATTE_CEMU_PS_TEX_UNIT_BASE;
		texRegs = RecordRegs().SQ_TEX_START_PS;
		break;
	case LatteConst::ShaderType::Geometry:
		hostTextureUnitBase = LATTE_CEMU_GS_TEX_UNIT_BASE;
		texRegs = RecordRegs().SQ_TEX_START_GS;
		break;
	default:
		return 0;
	}

	for (uint32 textureListIndex = 0; textureListIndex < shader->textureUnitListCount; ++textureListIndex)
	{
		const uint32 relativeUnit = shader->textureUnitList[textureListIndex];
		const sint8 bindingPoint = shader->resourceMapping.textureUnitToBindingPoint[relativeUnit];
		if (bindingPoint < 0 || (uint32)bindingPoint >= kGameTextureSrvDescriptorCount)
			continue;

		uint32 gpuSamplerSwizzle = 0x06880000;
		if (texRegs)
		{
			const auto& word4 = texRegs[relativeUnit].word4;
			gpuSamplerSwizzle = ((uint32)word4.get_DST_SEL_X() << 16) |
								((uint32)word4.get_DST_SEL_Y() << 19) |
								((uint32)word4.get_DST_SEL_Z() << 22) |
								((uint32)word4.get_DST_SEL_W() << 25);
		}

		const uint32 hostTextureUnit = hostTextureUnitBase + relativeUnit;
		auto* texture = hostTextureUnit < m_boundTextures.size() ? m_boundTextures[hostTextureUnit] : nullptr;
		key = MixGameDescriptorTableKey(key, 0x30000000ull + relativeUnit);
		key = MixGameDescriptorTableKey(key, (uint64)(uint8)bindingPoint);
		key = MixGameDescriptorTableKey(key, hostTextureUnit);
		key = MixGameDescriptorTableKey(key, gpuSamplerSwizzle);
		key = MixGameDescriptorTableKey(key, (uint64)reinterpret_cast<size_t>(texture));
		if (!texture)
		{
			key = MixGameDescriptorTableKey(key, 0);
			continue;
		}

		const auto& samplerSrv = texture->GetSamplerSRV(gpuSamplerSwizzle);
		key = MixGameDescriptorTableKey(key, texture->baseTexture ? texture->baseTexture->physAddress : 0);
		key = MixGameDescriptorTableKey(key, (uint64)texture->format);
		key = MixGameDescriptorTableKey(key, (uint64)texture->dim);
		key = MixGameDescriptorTableKey(key, texture->firstMip);
		key = MixGameDescriptorTableKey(key, texture->numMip);
		key = MixGameDescriptorTableKey(key, texture->firstSlice);
		key = MixGameDescriptorTableKey(key, texture->numSlice);
		key = MixGameDescriptorTableKey(key, samplerSrv.IsValid() ? samplerSrv.index + 1ull : 0);
	}

	cacheable = true;
	return key;
}

uint64 D3D12Renderer::GetMemoizedTableCacheKey(std::array<TableKeyMemo, (size_t)LatteConst::ShaderType::TotalCount>& memoArray,
											   bool isSamplerTable, LatteDecompilerShader* shader, uint32 descriptorGeneration, bool& cacheable)
{
	if (!shader)
		return isSamplerTable ? BuildGameSamplerTableCacheKey(shader, descriptorGeneration, cacheable)
							  : BuildGameResourceTableCacheKey(shader, descriptorGeneration, cacheable);
	if (auto* memo = GetCurrentTableKeyMemo(memoArray, shader, descriptorGeneration))
	{
		cacheable = memo->cacheable;
		return memo->key;
	}
	auto& memo = memoArray[(size_t)shader->shaderType];
	memo = {};
	// across draws: the key only changes with the shader, the bindings and the sampling registers it reads. Resource
	// keys of draws with feedback copies or pixel logic op are never cached (they are built uncacheable)
	const size_t stage = (size_t)shader->shaderType;
	auto& persistent = (isSamplerTable ? m_samplerStageKeyCache : m_resourceStageKeyCache)[stage];
	const bool persistentAllowed = isSamplerTable ||
		(!(shader->shaderType == LatteConst::ShaderType::Pixel && m_logicOpEmulationMode != 0) &&
		 std::none_of(m_feedbackScratchByTextureUnit.begin(), m_feedbackScratchByTextureUnit.end(),
			[](uint32 index) { return index != kInvalidScratchCopyIndex; }));
	ID3D12Resource* bufferResource = m_bufferCache.resource.Get();
	// sampler keys read the stage's bound textures and sampler registers; resource keys the bound textures, texture
	// resource registers and the stage's uniform buffers (vertex shaders also the vertex buffers)
	const size_t genStage = std::min<size_t>(stage, 2);
	const uint64 bindingGeneration = isSamplerTable ? m_textureBindingGeneration[genStage] :
		m_textureBindingGeneration[genStage] * 0x9E3779B97F4A7C15ull + m_uniformBindingGeneration[stage] * 0xC2B2AE3D27D4EB4Full +
		(shader->shaderType == LatteConst::ShaderType::Vertex ? m_vertexBindingGeneration : 0);
	const uint64 registerGeneration = isSamplerTable ? s_recordSamplerRegGeneration[genStage] : s_recordTextureRegGeneration[genStage];
	if (persistentAllowed && persistent.valid && persistent.shader == shader && persistent.fetchShader == s_recordFetchShader &&
		persistent.descriptorGeneration == descriptorGeneration && persistent.bindingGeneration == bindingGeneration &&
		persistent.registerGeneration == registerGeneration && persistent.bufferResource == bufferResource)
	{
		cacheable = persistent.cacheable;
		memo.key = persistent.key;
		m_tableKeyReuses[isSamplerTable ? 1 : 0]++;
	}
	else
	{
		memo.key = isSamplerTable ? BuildGameSamplerTableCacheKey(shader, descriptorGeneration, cacheable)
								  : BuildGameResourceTableCacheKey(shader, descriptorGeneration, cacheable);
		m_tableKeyBuilds[isSamplerTable ? 1 : 0]++;
		if (persistentAllowed)
			persistent = { true, shader, s_recordFetchShader, descriptorGeneration, bindingGeneration,
				registerGeneration, bufferResource, memo.key, cacheable };
		else
			persistent.valid = false;
	}
	memo.cacheable = cacheable;
	memo.shader = shader;
	memo.generation = descriptorGeneration;
	memo.epoch = m_tableKeyMemoEpoch;
	return memo.key;
}

D3D12Renderer::TableKeyMemo* D3D12Renderer::GetCurrentTableKeyMemo(std::array<TableKeyMemo, (size_t)LatteConst::ShaderType::TotalCount>& memoArray,
																	LatteDecompilerShader* shader, uint32 descriptorGeneration)
{
	if (!shader || m_tableKeyMemoEpoch == 0)
		return nullptr;
	auto& memo = memoArray[(size_t)shader->shaderType];
	if (memo.shader != shader || memo.generation != descriptorGeneration || memo.epoch != m_tableKeyMemoEpoch)
		return nullptr;
	return &memo;
}

bool D3D12Renderer::HasCachedGameResourceTable(LatteDecompilerShader* shader, uint32 descriptorGeneration)
{
	if (!shader || !m_descriptorManager)
		return false;
	if (m_gameResourceTableCacheGeneration != descriptorGeneration)
		return false;
	bool cacheable = false;
	const uint64 key = GetMemoizedTableCacheKey(m_resourceTableKeyMemo, false, shader, descriptorGeneration, cacheable);
	if (!cacheable)
		return false;
	auto* memo = GetCurrentTableKeyMemo(m_resourceTableKeyMemo, shader, descriptorGeneration);
	if (memo && memo->lookupDone)
		return memo->lookupHit;
	const auto it = m_gameResourceTableCache.find(key);
	const bool found = it != m_gameResourceTableCache.end();
	if (memo)
	{
		memo->lookupDone = true;
		memo->lookupHit = found;
		memo->cachedAllocation = found ? it->second.allocation : D3D12DescriptorManager::Allocation{};
	}
	return found;
}

uint64 D3D12Renderer::BuildGameSamplerTableCacheKey(LatteDecompilerShader* shader, uint32 descriptorGeneration, bool& cacheable)
{
	cacheable = false;
	if (!m_descriptorManager)
		return 0;

	uint64 key = 0xd3125a7cafe00001ull;
	key = MixGameDescriptorTableKey(key, descriptorGeneration);
	if (!shader)
	{
		cacheable = true;
		return MixGameDescriptorTableKey(key, 0);
	}

	key = MixGameDescriptorTableKey(key, (uint64)shader->shaderType);
	key = MixGameDescriptorTableKey(key, shader->baseHash);
	key = MixGameDescriptorTableKey(key, shader->auxHash);
	key = MixGameDescriptorTableKey(key, (uint64)reinterpret_cast<size_t>(shader));
	key = MixGameDescriptorTableKey(key, GetGameSamplerTableDescriptorCount(shader));
	uint32 hostTextureUnitBase = 0;
	switch (shader->shaderType)
	{
	case LatteConst::ShaderType::Vertex:
		hostTextureUnitBase = LATTE_CEMU_VS_TEX_UNIT_BASE;
		break;
	case LatteConst::ShaderType::Pixel:
		hostTextureUnitBase = LATTE_CEMU_PS_TEX_UNIT_BASE;
		break;
	case LatteConst::ShaderType::Geometry:
		hostTextureUnitBase = LATTE_CEMU_GS_TEX_UNIT_BASE;
		break;
	default:
		return 0;
	}

	for (uint32 textureListIndex = 0; textureListIndex < shader->textureUnitListCount; ++textureListIndex)
	{
		const uint32 relativeUnit = shader->textureUnitList[textureListIndex];
		const sint8 bindingPoint = shader->resourceMapping.textureUnitToBindingPoint[relativeUnit];
		if (bindingPoint < 0 || (uint32)bindingPoint >= kGameSamplerDescriptorCount)
			continue;

		const uint32 hostTextureUnit = hostTextureUnitBase + relativeUnit;
		LatteTextureViewD3D12* textureView = hostTextureUnit < m_boundTextures.size() ? m_boundTextures[hostTextureUnit] : nullptr;
		LatteTexture* baseTexture = textureView ? textureView->baseTexture : nullptr;
		const uint32 stageSamplerIndex = shader->textureUnitSamplerAssignment[relativeUnit];

		key = MixGameDescriptorTableKey(key, 0x51000000ull + relativeUnit);
		key = MixGameDescriptorTableKey(key, (uint64)(uint8)bindingPoint);
		key = MixGameDescriptorTableKey(key, stageSamplerIndex);
		key = MixGameDescriptorTableKey(key, shader->textureUsesDepthCompare[relativeUnit] ? 1 : 0);
		// key on sampler overrides, not view identity, so identical tables are shared
		key = D3D12SamplerTableState::MixTextureOverrides(key, baseTexture, MixGameDescriptorTableKey);

		if (stageSamplerIndex == LATTE_DECOMPILER_SAMPLER_NONE)
			continue;

		const uint32 samplerIndex = stageSamplerIndex + LatteDecompiler_getTextureSamplerBaseIndex(shader->shaderType);
		const _LatteRegisterSetSampler* samplerWords = RecordRegs().SQ_TEX_SAMPLER + samplerIndex;
		key = MixGameDescriptorTableKey(key, samplerWords->WORD0.getRawValue());
		key = MixGameDescriptorTableKey(key, samplerWords->WORD1.getRawValue());
		key = MixGameDescriptorTableKey(key, samplerWords->WORD2.getRawValue());

		if (samplerWords->WORD0.get_BORDER_COLOR_TYPE() == Latte::LATTE_SQ_TEX_SAMPLER_WORD0_0::E_BORDER_COLOR_TYPE::REGISTER)
		{
			auto* borderColorReg = GetD3D12SamplerBorderColorRegisters(shader->shaderType, stageSamplerIndex);
			key = MixGameDescriptorTableKey(key, std::bit_cast<uint32>(borderColorReg->red.get_channelValue()));
			key = MixGameDescriptorTableKey(key, std::bit_cast<uint32>(borderColorReg->green.get_channelValue()));
			key = MixGameDescriptorTableKey(key, std::bit_cast<uint32>(borderColorReg->blue.get_channelValue()));
			key = MixGameDescriptorTableKey(key, std::bit_cast<uint32>(borderColorReg->alpha.get_channelValue()));
		}
	}

	cacheable = true;
	return key;
}

bool D3D12Renderer::HasCachedGameSamplerTable(LatteDecompilerShader* shader, uint32 descriptorGeneration)
{
	if (!m_descriptorManager)
		return false;
	if (m_gameSamplerTableCacheGeneration != descriptorGeneration)
		return false;
	bool cacheable = false;
	const uint64 key = GetMemoizedTableCacheKey(m_samplerTableKeyMemo, true, shader, descriptorGeneration, cacheable);
	if (!cacheable)
		return false;
	auto* memo = GetCurrentTableKeyMemo(m_samplerTableKeyMemo, shader, descriptorGeneration);
	if (memo && memo->lookupDone)
		return memo->lookupHit;
	const auto it = m_gameSamplerTableCache.find(key);
	const bool found = it != m_gameSamplerTableCache.end();
	if (memo)
	{
		memo->lookupDone = true;
		memo->lookupHit = found;
		memo->cachedAllocation = found ? it->second.allocation : D3D12DescriptorManager::Allocation{};
	}
	return found;
}

uint64 D3D12Renderer::BuildGameUniformVarsSourceKey(LatteDecompilerShader* shader) const
{
	if (!shader)
		return 0;

	uint64 key = 0xd312cb0f5eed0001ull;
	key = MixGameDescriptorTableKey(key, (uint64)reinterpret_cast<size_t>(shader));
	key = MixGameDescriptorTableKey(key, shader->baseHash);
	key = MixGameDescriptorTableKey(key, shader->auxHash);
	key = MixGameDescriptorTableKey(key, shader->uniform.uniformRangeSize);

	for (const auto& entry : shader->uniform.list_ufTexRescale)
	{
		const float* scale = LatteTexture_getEffectiveTextureScale(shader->shaderType, entry.texUnit);
		key = MixGameStateBytes(key, scale, sizeof(float) * 2);
	}

	if (shader->uniform.loc_alphaTestRef >= 0)
	{
		const float alphaRef = LatteGPUState.contextNew.SX_ALPHA_REF.get_ALPHA_TEST_REF();
		key = MixGameStateBytes(key, &alphaRef, sizeof(alphaRef));
	}
	if (shader->uniform.loc_pointSize >= 0)
	{
		float pointWidth = (float)LatteGPUState.contextNew.PA_SU_POINT_SIZE.get_WIDTH() / 8.0f;
		if (pointWidth == 0.0f)
			pointWidth = 1.0f / 8.0f;
		key = MixGameStateBytes(key, &pointWidth, sizeof(pointWidth));
	}

	uint32 shaderAluConst = 0;
	uint32 shaderUniformRegisterOffset = mmSQ_PS_UNIFORM_BLOCK_START;
	switch (shader->shaderType)
	{
	case LatteConst::ShaderType::Vertex:
		shaderAluConst = 0x400;
		shaderUniformRegisterOffset = mmSQ_VTX_UNIFORM_BLOCK_START;
		break;
	case LatteConst::ShaderType::Geometry:
		shaderUniformRegisterOffset = mmSQ_GS_UNIFORM_BLOCK_START;
		break;
	default:
		break;
	}

	if (shader->uniform.loc_remapped >= 0)
	{
		const uint8* aluConstants = reinterpret_cast<const uint8*>(LatteGPUState.contextRegister + mmSQ_ALU_CONSTANT0_0 + shaderAluConst);
		for (const auto& entry : shader->list_remappedUniformEntries_register)
			key = MixGameStateBytes(key, aluConstants + entry.indexOffset, 16);

		for (const auto& bufferGroup : shader->list_remappedUniformEntries_bufferGroups)
		{
			const MPTR physicalAddress = LatteGPUState.contextRegister[shaderUniformRegisterOffset + bufferGroup.kcacheBankIdOffset / 4];
			key = MixGameDescriptorTableKey(key, physicalAddress);
			if (physicalAddress)
			{
				const uint8* uniformBase = memory_base + physicalAddress;
				for (const auto& entry : bufferGroup.entries)
					key = MixGameStateBytes(key, uniformBase + entry.indexOffset, 16);
			}
			else
			{
				key = MixGameDescriptorTableKey(key, bufferGroup.entries.size());
			}
		}
	}

	if (shader->uniform.loc_uniformRegister >= 0 && shader->uniform.count_uniformRegister != 0)
	{
		const uint32* uniformRegisters = LatteGPUState.contextRegister + mmSQ_ALU_CONSTANT0_0 + shaderAluConst;
		key = MixGameStateBytes(key, uniformRegisters, (size_t)shader->uniform.count_uniformRegister * 16);
	}
	if (shader->uniform.loc_windowSpaceToClipSpaceTransform >= 0)
	{
		sint32 viewportSize[2]{ 1, 1 };
		LatteRenderTarget_GetCurrentVirtualViewportSize(&viewportSize[0], &viewportSize[1]);
		key = MixGameStateBytes(key, viewportSize, sizeof(viewportSize));
	}
	if (shader->uniform.loc_fragCoordScale >= 0)
	{
		float fragCoordScale[4]{};
		LatteMRT::GetCurrentFragCoordScale(fragCoordScale);
		key = MixGameStateBytes(key, fragCoordScale, sizeof(fragCoordScale));
	}
	if (shader->uniform.loc_verticesPerInstance >= 0)
		key = MixGameDescriptorTableKey(key, (uint32)m_streamoutVerticesPerInstance);
	// the Latte-side mirror: with threaded recording m_streamoutBindings still holds an earlier draw's offsets here
	for (uint32 bufferIndex = 0; bufferIndex < m_latteStreamoutRingOffsets.size(); ++bufferIndex)
	{
		if (shader->uniform.loc_streamoutBufferBase[bufferIndex] >= 0)
			key = MixGameDescriptorTableKey(key, (uint32)m_latteStreamoutRingOffsets[bufferIndex]);
	}
	return key;
}

uint64 D3D12Renderer::BuildGameUavTableCacheKey(LatteDecompilerShader* shader, uint32 descriptorGeneration) const
{
	if (!shader || shader->resourceMapping.tfStorageBindingPoint < 0 || !m_streamoutBuffer.resource)
		return 0;

	uint64 key = 0xd3125a0a7eed0001ull;
	key = MixGameDescriptorTableKey(key, descriptorGeneration);
	key = MixGameDescriptorTableKey(key, (uint64)reinterpret_cast<size_t>(m_streamoutBuffer.resource.Get()));
	key = MixGameDescriptorTableKey(key, (uint32)shader->resourceMapping.tfStorageBindingPoint);
	key = MixGameDescriptorTableKey(key, m_streamoutBuffer.size);
	return key;
}

#if defined(CEMU_UWP_D3D12_DIAGNOSTICS)
static void D3D12TracePriorityUniformSources(LatteDecompilerShader* shader, uint64 drawId, uint64 pipelineHash)
{
	if (!shader || !D3D12TraceSink::Get().IsPriorityStencilDraw(drawId))
		return;
	constexpr uint32 maxBytes = D3D12DiagnosticDrawContextPolicy::kMaxUniformSourceBytes;
	uint32 aluOffset = 0;
	uint32 uniformRegisterBase = 0;
	switch (shader->shaderType)
	{
	case LatteConst::ShaderType::Vertex: aluOffset = 0x400; uniformRegisterBase = mmSQ_VTX_UNIFORM_BLOCK_START; break;
	case LatteConst::ShaderType::Pixel: uniformRegisterBase = mmSQ_PS_UNIFORM_BLOCK_START; break;
	case LatteConst::ShaderType::Geometry: uniformRegisterBase = mmSQ_GS_UNIFORM_BLOCK_START; break;
	default: return;
	}
	if (shader->uniform.uniformRangeSize > maxBytes || shader->uniform.count_uniformRegister > 256)
	{
		D3D12TraceEvent("uniform_priority_source_skipped", "draw={} pipeline=0x{:016x} shader=0x{:016x} reason=source_limit rangeSize={} registerCount={}",
			drawId, pipelineHash, shader->baseHash, shader->uniform.uniformRangeSize, shader->uniform.count_uniformRegister);
		return;
	}
	uint32 remappedBytes = 0;
	auto includeRemapped = [&](const auto& entry)
	{
		if ((entry.mappedIndexOffset & 15) != 0 || (uint64)entry.mappedIndexOffset + 16 > maxBytes)
			return false;
		remappedBytes = std::max(remappedBytes, (uint32)entry.mappedIndexOffset + 16);
		return true;
	};
	for (const auto& entry : shader->list_remappedUniformEntries_register)
		if ((entry.indexOffset & 15) != 0 || (uint64)entry.indexOffset + 16 > 256 * 16 || !includeRemapped(entry)) return;
	for (const auto& group : shader->list_remappedUniformEntries_bufferGroups)
	{
		if (group.bufferId >= LATTE_NUM_MAX_UNIFORM_BUFFERS || group.kcacheBankIdOffset != group.bufferId * (7 * 4))
			return;
		const MPTR base = LatteGPUState.contextRegister[uniformRegisterBase + group.kcacheBankIdOffset / 4];
		for (const auto& entry : group.entries)
		{
			if ((entry.indexOffset & 15) != 0 || !includeRemapped(entry)) return;
			// A zero CB address is intentionally zero-filled by the shared loader.
			const uint64 address = (uint64)base + entry.indexOffset;
			if (base && (address > std::numeric_limits<MPTR>::max() - 15 ||
				!memory_isAddressRangeAccessible((MPTR)address, 16)))
				return;
		}
	}
	std::vector<uint32> remapped((remappedBytes + 3) / 4, 0);
	if (!remapped.empty())
		LatteBufferCache_LoadRemappedUniforms(shader, reinterpret_cast<float*>(remapped.data()), true,
			(1 << LATTE_NUM_MAX_UNIFORM_BUFFERS) - 1);
	const uint32* registers = LatteGPUState.contextRegister + mmSQ_ALU_CONSTANT0_0 + aluOffset;
	const uint32 registerWords = shader->uniform.loc_uniformRegister >= 0 ? shader->uniform.count_uniformRegister * 4 : 0;
	auto formatWords = [](const uint32* words, uint32 count)
	{
		std::string result;
		result.reserve((size_t)count * 11);
		for (uint32 i = 0; i < count; ++i)
			fmt::format_to(std::back_inserter(result), "{}0x{:08x}", i ? "," : "", words[i]);
		return result.empty() ? std::string("none") : result;
	};
	D3D12TraceEvent("uniform_priority_source", "draw={} pipeline=0x{:016x} stage={} shader=0x{:016x} aux=0x{:016x} snapshotKind=cpu_alu_and_remapped_sources cacheBehavior=unchanged gpuBytesVerified=0 rangeSize={} locRemapped={} locUniformRegister={} remappedWordCount={} uniformRegisterWordCount={} remappedWords={} uniformRegisterWords={}",
		drawId, pipelineHash, (uint32)shader->shaderType, shader->baseHash, shader->auxHash,
		shader->uniform.uniformRangeSize, shader->uniform.loc_remapped, shader->uniform.loc_uniformRegister,
		remapped.size(), registerWords, formatWords(remapped.data(), (uint32)remapped.size()), formatWords(registers, registerWords));
}
#endif

// Latte side of the uniform upload (reads registers and guest memory): builds the stage's bytes when their sources
// changed since they were last sent; otherwise the recorder keeps the bytes it has
void D3D12Renderer::PrepareGameUniformVars(LatteDecompilerShader* shader, uint64 drawId, uint64 pipelineHash, UniformUpdate& update)
{
	if (!shader || !m_memoryManager || (size_t)shader->shaderType >= m_latteUniformSources.size())
		return;
	auto& source = m_latteUniformSources[(size_t)shader->shaderType];
	update.shader = shader;
	if (shader->resourceMapping.uniformVarsBufferBindingPoint != 0 || shader->uniform.uniformRangeSize == 0)
	{
		// no uniform buffer: the recorder's bytes for this stage are dropped
		if (!source.sent || source.shader != shader || source.sourceKey != 0)
		{
			update.send = true;
			source = { shader, 0, true };
		}
		return;
	}

	const uint64 sourceKey = BuildGameUniformVarsSourceKey(shader);
	if (source.sent && source.shader == shader && source.sourceKey == sourceKey)
		return;

	auto& uniformData = update.data;
	if (uniformData.capacity() == 0)
		uniformData = s_recordBytePool.Take();
	uniformData.assign(std::max<uint32>(shader->uniform.uniformRangeSize, 16), 0);
	auto getUniformDataPtr = [&uniformData](size_t index) -> float*
	{
		return reinterpret_cast<float*>(uniformData.data() + index);
	};

	sint32 shaderAluConst = 0;
	switch (shader->shaderType)
	{
	case LatteConst::ShaderType::Vertex:
		shaderAluConst = 0x400;
		break;
	case LatteConst::ShaderType::Pixel:
	case LatteConst::ShaderType::Geometry:
		shaderAluConst = 0;
		break;
	default:
		break;
	}

	for (auto& entry : shader->uniform.list_ufTexRescale)
	{
		if (entry.uniformLocation < 0 || entry.uniformLocation + (sint32)sizeof(float) * 2 > (sint32)uniformData.size())
			continue;
		float* xyScale = LatteTexture_getEffectiveTextureScale(shader->shaderType, entry.texUnit);
		memcpy(getUniformDataPtr(entry.uniformLocation), xyScale, sizeof(float) * 2);
	}
	if (shader->uniform.loc_alphaTestRef >= 0 && shader->uniform.loc_alphaTestRef + (sint32)sizeof(float) <= (sint32)uniformData.size())
		*getUniformDataPtr(shader->uniform.loc_alphaTestRef) = LatteGPUState.contextNew.SX_ALPHA_REF.get_ALPHA_TEST_REF();
	if (shader->uniform.loc_pointSize >= 0 && shader->uniform.loc_pointSize + (sint32)sizeof(float) <= (sint32)uniformData.size())
	{
		const auto& pointSizeReg = LatteGPUState.contextNew.PA_SU_POINT_SIZE;
		float pointWidth = (float)pointSizeReg.get_WIDTH() / 8.0f;
		if (pointWidth == 0.0f)
			pointWidth = 1.0f / 8.0f;
		*getUniformDataPtr(shader->uniform.loc_pointSize) = pointWidth;
	}
	if (shader->uniform.loc_remapped >= 0 && shader->uniform.loc_remapped < (sint32)uniformData.size())
	{
		LatteBufferCache_LoadRemappedUniforms(shader, getUniformDataPtr(shader->uniform.loc_remapped), true, (1 << LATTE_NUM_MAX_UNIFORM_BUFFERS) - 1);
		if (D3D12ShouldChecksumOutput() && shader->shaderType == LatteConst::ShaderType::Pixel)
		{
			static uint32 s_psRemappedUniformLogCount = 0;
			if (D3D12RuntimeLogCapped(s_psRemappedUniformLogCount, "PS remapped uniform", 48))
			{
				const uint32* u = reinterpret_cast<const uint32*>(uniformData.data() + shader->uniform.loc_remapped);
				const float* f = reinterpret_cast<const float*>(u);
				cemuLog_log(LogType::Force,
							 "D3D12: PS remapped[0] shader={:016x} raw=0x{:08x},0x{:08x},0x{:08x},0x{:08x} float={:.6g},{:.6g},{:.6g},{:.6g}.",
							 shader->baseHash,
							 u[0], u[1], u[2], u[3],
							 f[0], f[1], f[2], f[3]);
			}
		}
	}
	if (shader->uniform.loc_uniformRegister >= 0 && shader->uniform.loc_uniformRegister + shader->uniform.count_uniformRegister * 16 <= (sint32)uniformData.size())
	{
		uint32* uniformRegData = LatteGPUState.contextRegister + mmSQ_ALU_CONSTANT0_0 + shaderAluConst;
		memcpy(getUniformDataPtr(shader->uniform.loc_uniformRegister), uniformRegData, shader->uniform.count_uniformRegister * 16);
	}
	if (shader->uniform.loc_windowSpaceToClipSpaceTransform >= 0 && shader->uniform.loc_windowSpaceToClipSpaceTransform + (sint32)sizeof(float) * 2 <= (sint32)uniformData.size())
	{
		sint32 viewportWidth = 1;
		sint32 viewportHeight = 1;
		LatteRenderTarget_GetCurrentVirtualViewportSize(&viewportWidth, &viewportHeight);
		float* v = getUniformDataPtr(shader->uniform.loc_windowSpaceToClipSpaceTransform);
		v[0] = 2.0f / (float)std::max<sint32>(viewportWidth, 1);
		v[1] = 2.0f / (float)std::max<sint32>(viewportHeight, 1);
	}
	float fragCoordScale[4]{};
	bool hasFragCoordScale = false;
	if (shader->uniform.loc_fragCoordScale >= 0 && shader->uniform.loc_fragCoordScale + (sint32)sizeof(float) * 4 <= (sint32)uniformData.size())
	{
		float* scale = getUniformDataPtr(shader->uniform.loc_fragCoordScale);
		LatteMRT::GetCurrentFragCoordScale(scale);
		memcpy(fragCoordScale, scale, sizeof(fragCoordScale));
		hasFragCoordScale = true;
	}
	else if (shader->uniform.loc_fragCoordScale >= 0)
	{
		D3D12TraceEvent("uniform_fragcoord_scale_skipped", "draw={} pipeline=0x{:016x} stage={} shader=0x{:016x} aux=0x{:016x} rangeSize={} locFragCoordScale={} requiredBytes=16",
						(unsigned long long)drawId,
						(unsigned long long)pipelineHash,
						(uint32)shader->shaderType,
						shader->baseHash,
						shader->auxHash,
						shader->uniform.uniformRangeSize,
						shader->uniform.loc_fragCoordScale);
	}
	if (shader->uniform.loc_verticesPerInstance >= 0 &&
		shader->uniform.loc_verticesPerInstance + (sint32)sizeof(sint32) <= (sint32)uniformData.size())
	{
		memcpy(uniformData.data() + shader->uniform.loc_verticesPerInstance,
			&m_streamoutVerticesPerInstance, sizeof(m_streamoutVerticesPerInstance));
	}
	// Latte thread: m_streamoutBindings is recorder state, so with threaded recording it held the previous draw's ring
	// offset and particles were simulated into the wrong place (BOTW rain drawn as screen-wide streaks)
	for (uint32 bufferIndex = 0; bufferIndex < m_latteStreamoutRingOffsets.size(); ++bufferIndex)
	{
		const sint32 location = shader->uniform.loc_streamoutBufferBase[bufferIndex];
		if (location < 0 || location + (sint32)sizeof(sint32) > (sint32)uniformData.size())
			continue;
		const sint32 ringBufferOffset = m_latteStreamoutRingOffsets[bufferIndex];
		memcpy(uniformData.data() + location, &ringBufferOffset, sizeof(ringBufferOffset));
	}

	update.send = true;
	source = { shader, sourceKey, true };
}

// recording side: installs the bytes sent with the draw (an empty send means the stage has no uniform buffer)
void D3D12Renderer::ApplyGameUniformUpdate(UniformUpdate& update)
{
	if (!update.shader || !update.send || (size_t)update.shader->shaderType >= m_gameUniformUploadCache.size())
		return;
	auto& slot = m_gameUniformUploadCache[(size_t)update.shader->shaderType];
	slot.shader = update.shader;
	std::swap(slot.data, update.data);
	s_recordBytePool.Give(std::move(update.data)); // the previous bytes go back to PrepareGameUniformVars
	slot.gpuAddress = 0;
}

// recording side: copies the prepared bytes into the upload ring once per command list (ring memory is retired by fence)
D3D12_GPU_VIRTUAL_ADDRESS D3D12Renderer::CommitGameUniformVars(LatteDecompilerShader* shader, uint64 drawId, uint64 pipelineHash)
{
	if (!shader || !m_memoryManager || (size_t)shader->shaderType >= m_gameUniformUploadCache.size())
		return 0;
	auto& slot = m_gameUniformUploadCache[(size_t)shader->shaderType];
	if (slot.shader != shader || slot.data.empty())
		return 0;
	if (slot.gpuAddress != 0)
		return slot.gpuAddress;

	const uint32 cbvSize = AlignUpU32((uint32)slot.data.size(), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
	auto upload = m_memoryManager->AllocateUpload(cbvSize, D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT, m_nextFenceValue);
	if (!upload.IsValid())
	{
		D3D12TraceEvent("uniform_vars_upload_failed", "draw={} pipeline=0x{:016x} stage={} shader=0x{:016x} aux=0x{:016x} rangeSize={} cbvSize={} nextFence={}",
						(unsigned long long)drawId,
						(unsigned long long)pipelineHash,
						(uint32)shader->shaderType,
						shader->baseHash,
						shader->auxHash,
						shader->uniform.uniformRangeSize,
						cbvSize,
						(unsigned long long)m_nextFenceValue);
		return 0;
	}

	memcpy(upload.cpuPtr, slot.data.data(), slot.data.size());
	if (cbvSize > slot.data.size())
		memset(upload.cpuPtr + slot.data.size(), 0, cbvSize - slot.data.size());
	slot.gpuAddress = upload.resource->GetGPUVirtualAddress() + upload.offset;
	const D3D12_GPU_VIRTUAL_ADDRESS uniformCbvAddress = slot.gpuAddress;
	uint32 firstUniformWords[4]{};
	memcpy(firstUniformWords, slot.data.data(), std::min<size_t>(slot.data.size(), sizeof(firstUniformWords)));
	D3D12TraceEvent("uniform_vars_upload", "draw={} pipeline=0x{:016x} stage={} shader=0x{:016x} aux=0x{:016x} rangeSize={} cbvSize={} uploadResource={} uploadOffset={} gpuVA=0x{:016x} locFragCoordScale={} firstWords=0x{:08x},0x{:08x},0x{:08x},0x{:08x}",
					(unsigned long long)drawId,
					(unsigned long long)pipelineHash,
					(uint32)shader->shaderType,
					shader->baseHash,
					shader->auxHash,
					shader->uniform.uniformRangeSize,
					cbvSize,
					(const void*)upload.resource,
					(unsigned long long)upload.offset,
					(unsigned long long)uniformCbvAddress,
					shader->uniform.loc_fragCoordScale,
					firstUniformWords[0],
					firstUniformWords[1],
					firstUniformWords[2],
					firstUniformWords[3]);
	if (D3D12ShouldTrace() && slot.data.size() <= 128)
	{
		std::string words;
		std::string floats;
		const uint32 wordCount = (uint32)(slot.data.size() / sizeof(uint32));
		words.reserve(wordCount * 11);
		floats.reserve(wordCount * 14);
		for (uint32 i = 0; i < wordCount; ++i)
		{
			uint32 word = 0;
			memcpy(&word, slot.data.data() + (size_t)i * sizeof(uint32), sizeof(uint32));
			if (i != 0)
			{
				words.push_back(',');
				floats.push_back(',');
			}
			fmt::format_to(std::back_inserter(words), "0x{:08x}", word);
			fmt::format_to(std::back_inserter(floats), "{:.9g}", std::bit_cast<float>(word));
		}
		D3D12TraceEvent("uniform_vars_upload_full", "draw={} pipeline=0x{:016x} stage={} shader=0x{:016x} aux=0x{:016x} rangeSize={} cbvSize={} wordCount={} locRemapped={} locUniformRegister={} uniformRegisterCount={} locWindowToClip={} locFragCoordScale={} locAlphaRef={} locPointSize={} words={} floats={}",
						(unsigned long long)drawId,
						(unsigned long long)pipelineHash,
						(uint32)shader->shaderType,
						shader->baseHash,
						shader->auxHash,
						shader->uniform.uniformRangeSize,
						cbvSize,
						wordCount,
						shader->uniform.loc_remapped,
						shader->uniform.loc_uniformRegister,
						shader->uniform.count_uniformRegister,
						shader->uniform.loc_windowSpaceToClipSpaceTransform,
						shader->uniform.loc_fragCoordScale,
						shader->uniform.loc_alphaTestRef,
						shader->uniform.loc_pointSize,
						words,
						floats);
	}
	return uniformCbvAddress;
}

D3D12DescriptorManager::Allocation D3D12Renderer::BuildGameResourceTable(LatteDecompilerShader* shader, D3D12_GPU_VIRTUAL_ADDRESS& uniformCbvAddress, uint64 drawId, uint64 pipelineHash)
{
	uniformCbvAddress = 0;
	const uint32 descriptorGeneration = m_descriptorManager->GetShaderVisibleCbvSrvUavRollovers();
	if (!shader)
	{
		if (m_gameNullResourceTable.IsValid() &&
			m_gameNullResourceTableGeneration == descriptorGeneration)
		{
			return m_gameNullResourceTable;
		}
	}
	else
	{
		// the bytes were prepared before the split (PrepareGameUniformVars reads guest state)
		uniformCbvAddress = CommitGameUniformVars(shader, drawId, pipelineHash);
		if (m_gameResourceTableCacheGeneration != descriptorGeneration)
		{
			m_gameResourceTableCache.clear();
			m_gameResourceTableCacheGeneration = descriptorGeneration;
		}

		bool cacheable = false;
		const uint64 cacheKey = GetMemoizedTableCacheKey(m_resourceTableKeyMemo, false, shader, descriptorGeneration, cacheable);
		if (cacheable)
		{
			const auto* memo = GetCurrentTableKeyMemo(m_resourceTableKeyMemo, shader, descriptorGeneration);
			if (memo && memo->lookupDone)
			{
				if (memo->lookupHit)
				{
					m_gameResourceTableCacheHits++;
					return memo->cachedAllocation;
				}
			}
			else if (auto it = m_gameResourceTableCache.find(cacheKey); it != m_gameResourceTableCache.end())
			{
				m_gameResourceTableCacheHits++;
				return it->second.allocation;
			}
			m_gameResourceTableCacheMisses++;
		}
	}
	m_tableBuildTextureRefs.clear();

	const uint32 descriptorCount = GetGameResourceTableDescriptorCount(shader);
	auto allocation = m_descriptorManager->AllocateShaderVisible(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, descriptorCount);
	const uint32 descriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	const uint32 srvDescriptorCount = std::min<uint32>(descriptorCount, kGameTextureSrvDescriptorCount);
	if (srvDescriptorCount != 0)
		CopyDescriptorsSimpleChecked(srvDescriptorCount, OffsetCpuHandle(allocation.cpu, descriptorSize, kGameSrvDescriptorOffset), m_nullSrvDescriptorRange.cpu, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, "null texture table");

	if (!shader)
	{
		m_gameNullResourceTable = allocation;
		m_gameNullResourceTableGeneration = descriptorGeneration;
		return allocation;
	}
	bool cacheable = false;
	const uint64 cacheKey = GetMemoizedTableCacheKey(m_resourceTableKeyMemo, false, shader, descriptorGeneration, cacheable);

	const size_t stageIndex = (size_t)shader->shaderType;
	if (shader->shaderType == LatteConst::ShaderType::Pixel && m_logicOpEmulationMode != 0 &&
		kGameSrvDescriptorOffset + kGameLogicOpDstSrvBinding + kGameLogicOpDstSrvCount <= allocation.count)
	{
		for (uint32 renderTargetIndex = 0; renderTargetIndex < kGameLogicOpDstSrvCount; ++renderTargetIndex)
		{
			const FeedbackScratchCopy* logicScratch = GetLogicOpScratchCopy(renderTargetIndex);
			if (!logicScratch)
				continue;

			const uint32 bindingPoint = kGameLogicOpDstSrvBinding + renderTargetIndex;
			const auto tableSlot = OffsetCpuHandle(allocation.cpu, descriptorSize, kGameSrvDescriptorOffset + bindingPoint);
			CreateShaderResourceViewChecked(logicScratch->texture.resource.Get(), &logicScratch->srvDesc, tableSlot, "logic-op destination copy", true);
			D3D12TraceEvent("logic_op_scratch_srv_bind",
							"draw={} pipeline=0x{:016x} stage={} shader=0x{:016x} rt={} bindingPoint={} tableIndex={} tableGpu=0x{:016x} srvFormat={} scratchResource={}",
							(unsigned long long)drawId,
							(unsigned long long)pipelineHash,
							(uint32)shader->shaderType,
							shader->baseHash,
							renderTargetIndex,
							bindingPoint,
							allocation.index + kGameSrvDescriptorOffset + bindingPoint,
							(unsigned long long)allocation.gpu.ptr,
							(uint32)logicScratch->srvDesc.Format,
							(const void*)logicScratch->texture.resource.Get());
		}
	}

	if (stageIndex < m_uniformBindings.size() && m_bufferCache.resource)
	{
		for (uint32 bufferIndex = 0; bufferIndex < LATTE_NUM_MAX_UNIFORM_BUFFERS; ++bufferIndex)
		{
			const sint8 bindingPoint = shader->resourceMapping.uniformBuffersBindingPoint[bufferIndex];
			if (bindingPoint < 0 || (uint32)bindingPoint >= kGameTextureSrvDescriptorCount || (uint32)bindingPoint >= allocation.count)
				continue;
			const auto& binding = m_uniformBindings[stageIndex][bufferIndex];
			if (!binding.valid || binding.size == 0)
				continue;

			Transition(m_bufferCache.resource.Get(), m_bufferCache.state,
					   D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER |
						   D3D12_RESOURCE_STATE_INDEX_BUFFER |
						   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
						   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

			D3D12_SHADER_RESOURCE_VIEW_DESC uniformSrv{};
			uniformSrv.Format = DXGI_FORMAT_UNKNOWN;
			uniformSrv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
			uniformSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			uniformSrv.Buffer.FirstElement = binding.offset / 16;
			uniformSrv.Buffer.NumElements = std::max<UINT>(1, AlignUpU32(binding.size + (binding.offset & 15), 16) / 16);
			uniformSrv.Buffer.StructureByteStride = 16;
			uniformSrv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
			CreateShaderResourceViewChecked(m_bufferCache.resource.Get(), &uniformSrv, OffsetCpuHandle(allocation.cpu, descriptorSize, kGameSrvDescriptorOffset + (uint32)bindingPoint), "uniform buffer", true);
			D3D12TraceEvent("uniform_buffer_srv", "draw={} pipeline=0x{:016x} stage={} shader=0x{:016x} bufferIndex={} bindingPoint={} tableIndex={} tableGpu=0x{:016x} offset={} size={} firstElement={} numElements={} mainResource={}",
							(unsigned long long)drawId,
							(unsigned long long)pipelineHash,
							(uint32)shader->shaderType,
							shader->baseHash,
							bufferIndex,
							(sint32)bindingPoint,
							allocation.index + kGameSrvDescriptorOffset + (uint32)bindingPoint,
							(unsigned long long)allocation.gpu.ptr,
							binding.offset,
							binding.size,
							uniformSrv.Buffer.FirstElement,
							uniformSrv.Buffer.NumElements,
							(const void*)m_bufferCache.resource.Get());
		}
	}

	if (shader->shaderType == LatteConst::ShaderType::Vertex && m_bufferCache.resource)
	{
		auto* fetchShader = s_recordFetchShader;
		if (fetchShader)
		{
			for (auto& group : fetchShader->bufferGroups)
			{
				const uint32 bufferIndex = group.attributeBufferIndex;
				const uint32 bindingPoint = kGameVertexBufferSrvBase + bufferIndex;
				if (bufferIndex >= m_vertexBindings.size() || bindingPoint >= kGameTextureSrvDescriptorCount || bindingPoint >= allocation.count)
					continue;

				const auto& binding = m_vertexBindings[bufferIndex];
				if (!binding.valid || binding.size == 0)
					continue;

				Transition(m_bufferCache.resource.Get(), m_bufferCache.state,
						   D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER |
							   D3D12_RESOURCE_STATE_INDEX_BUFFER |
							   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
							   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

				D3D12_SHADER_RESOURCE_VIEW_DESC vertexSrv{};
				vertexSrv.Format = DXGI_FORMAT_R32_UINT;
				vertexSrv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
				vertexSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
				vertexSrv.Buffer.FirstElement = binding.offset / 4;
				vertexSrv.Buffer.NumElements = std::max<UINT>(1, AlignUpU32(binding.size + (binding.offset & 3), 4) / 4);
				vertexSrv.Buffer.StructureByteStride = 0;
				vertexSrv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
				CreateShaderResourceViewChecked(m_bufferCache.resource.Get(), &vertexSrv, OffsetCpuHandle(allocation.cpu, descriptorSize, kGameSrvDescriptorOffset + bindingPoint), "vertex buffer", true);
				D3D12TraceEvent("vertex_buffer_srv", "draw={} pipeline=0x{:016x} stage={} shader=0x{:016x} attributeBufferIndex={} bindingPoint={} tableIndex={} tableGpu=0x{:016x} offset={} size={} firstElement={} numElements={} mainResource={}",
								(unsigned long long)drawId,
								(unsigned long long)pipelineHash,
								(uint32)shader->shaderType,
								shader->baseHash,
								bufferIndex,
								bindingPoint,
								allocation.index + kGameSrvDescriptorOffset + bindingPoint,
								(unsigned long long)allocation.gpu.ptr,
								binding.offset,
								binding.size,
								vertexSrv.Buffer.FirstElement,
								vertexSrv.Buffer.NumElements,
								(const void*)m_bufferCache.resource.Get());
			}
		}
	}

	uint32 hostTextureUnitBase = 0;
	const _LatteRegisterSetTextureUnit* texRegs = nullptr;
	switch (shader->shaderType)
	{
	case LatteConst::ShaderType::Vertex:
		hostTextureUnitBase = LATTE_CEMU_VS_TEX_UNIT_BASE;
		texRegs = RecordRegs().SQ_TEX_START_VS;
		break;
	case LatteConst::ShaderType::Pixel:
		hostTextureUnitBase = LATTE_CEMU_PS_TEX_UNIT_BASE;
		texRegs = RecordRegs().SQ_TEX_START_PS;
		break;
	case LatteConst::ShaderType::Geometry:
		hostTextureUnitBase = LATTE_CEMU_GS_TEX_UNIT_BASE;
		texRegs = RecordRegs().SQ_TEX_START_GS;
		break;
	default:
		return allocation;
	}

	for (uint32 textureListIndex = 0; textureListIndex < shader->textureUnitListCount; ++textureListIndex)
	{
		const uint32 relativeUnit = shader->textureUnitList[textureListIndex];
		const sint8 bindingPoint = shader->resourceMapping.textureUnitToBindingPoint[relativeUnit];
		if (bindingPoint < 0 || (uint32)bindingPoint >= kGameTextureSrvDescriptorCount || (uint32)bindingPoint >= allocation.count)
			continue;

		const uint32 hostTextureUnit = hostTextureUnitBase + relativeUnit;
		if (hostTextureUnit >= m_boundTextures.size())
			continue;

		D3D12_CPU_DESCRIPTOR_HANDLE source{};
		auto* texture = m_boundTextures[hostTextureUnit];
		// a view whose dimension differs from the shader declaration stays unbound
		const Latte::E_DIM shaderDim = shader->textureUnitDim[relativeUnit];
		if (texture &&
			((shaderDim == Latte::E_DIM::DIM_1D && texture->dim != Latte::E_DIM::DIM_1D) ||
			 (shaderDim == Latte::E_DIM::DIM_2D && texture->dim != Latte::E_DIM::DIM_2D && texture->dim != Latte::E_DIM::DIM_2D_MSAA)))
		{
			continue;
		}
		const FeedbackScratchCopy* feedbackScratch = GetFeedbackScratchCopy(hostTextureUnit);
		uint32 gpuSamplerSwizzle = 0x06880000;
		if (texRegs)
		{
			const auto& word4 = texRegs[relativeUnit].word4;
			gpuSamplerSwizzle = ((uint32)word4.get_DST_SEL_X() << 16) |
								((uint32)word4.get_DST_SEL_Y() << 19) |
								((uint32)word4.get_DST_SEL_Z() << 22) |
								((uint32)word4.get_DST_SEL_W() << 25);
		}

		if (!feedbackScratch && texture)
		{
			const auto& samplerSrv = texture->GetSamplerSRV(gpuSamplerSwizzle);
			if (samplerSrv.IsValid())
				source = samplerSrv.cpu;
			if (D3D12ShouldChecksumOutput() && shader->shaderType == LatteConst::ShaderType::Pixel && relativeUnit == 0)
			{
				static uint32 s_psTexture0StatsLogCount = 0;
				LogTextureStats(fmt::format("PS t0 shader={:016x} swizzle=0x{:08x}", shader->baseHash, gpuSamplerSwizzle),
								texture,
								false,
								s_psTexture0StatsLogCount,
								24);
			}
		}
		if (!feedbackScratch && !source.ptr)
			continue;

		const auto tableSlot = OffsetCpuHandle(allocation.cpu, descriptorSize, kGameSrvDescriptorOffset + (uint32)bindingPoint);
		if (feedbackScratch)
		{
			D3D12_SHADER_RESOURCE_VIEW_DESC scratchSrvDesc = feedbackScratch->srvDesc;
			if (feedbackScratch->sampledView)
				scratchSrvDesc.Shader4ComponentMapping = feedbackScratch->sampledView->GetShaderComponentMapping(gpuSamplerSwizzle);
			CreateShaderResourceViewChecked(feedbackScratch->texture.resource.Get(), &scratchSrvDesc, tableSlot, "feedback copy", true);
		}
		else
		{
			CopyDescriptorsSimpleChecked(1, tableSlot, source, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, "texture");
			if (texture && texture->baseTexture)
				m_tableBuildTextureRefs.push_back(texture->baseTexture);
		}
		D3D12TraceEvent("texture_srv_copy", "draw={} pipeline=0x{:016x} stage={} shader=0x{:016x} relativeUnit={} hostUnit={} bindingPoint={} tableIndex={} tableGpu=0x{:016x} sourceCpu=0x{:016x} feedbackScratch={} feedbackDraw={} feedbackScratchResource={} textureView={} texture={} addr=0x{:08x}",
						(unsigned long long)drawId,
						(unsigned long long)pipelineHash,
						(uint32)shader->shaderType,
						shader->baseHash,
						relativeUnit,
						hostTextureUnit,
						(sint32)bindingPoint,
						allocation.index + kGameSrvDescriptorOffset + (uint32)bindingPoint,
						(unsigned long long)allocation.gpu.ptr,
						(unsigned long long)source.ptr,
						feedbackScratch ? 1 : 0,
						feedbackScratch ? (unsigned long long)feedbackScratch->drawId : 0ull,
						feedbackScratch ? (const void*)feedbackScratch->texture.resource.Get() : nullptr,
						(const void*)texture,
						texture ? (const void*)texture->GetBaseImage() : nullptr,
						texture && texture->baseTexture ? texture->baseTexture->physAddress : 0);
	}

	if (cacheable)
	{
		m_gameResourceTableCache[cacheKey] = { allocation, m_tableBuildTextureRefs };
		m_gameResourceTableCacheGeneration = m_descriptorManager->GetShaderVisibleCbvSrvUavRollovers();
		if (auto* memo = GetCurrentTableKeyMemo(m_resourceTableKeyMemo, shader, descriptorGeneration))
		{
			memo->lookupDone = true;
			memo->lookupHit = true;
			memo->cachedAllocation = allocation;
		}
	}
	return allocation;
}

D3D12DescriptorManager::Allocation D3D12Renderer::BuildGameUavTable(LatteDecompilerShader* shader)
{
	const uint32 descriptorGeneration = m_descriptorManager->GetShaderVisibleCbvSrvUavRollovers();
	const sint32 streamoutBindingPoint = shader ? shader->resourceMapping.tfStorageBindingPoint : -1;
	const bool needsStreamoutUavTable = streamoutBindingPoint >= 0 &&
		(uint32)streamoutBindingPoint < kGameUavDescriptorCount && m_streamoutBuffer.resource;
	if (!needsStreamoutUavTable)
	{
		if (m_gameNullUavTable.IsValid() &&
			m_gameNullUavTableGeneration == descriptorGeneration)
		{
			return m_gameNullUavTable;
		}

		auto allocation = m_descriptorManager->AllocateShaderVisible(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kGameUavDescriptorCount);
		const uint32 descriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
		for (uint32 i = 0; i < kGameUavDescriptorCount; ++i)
			CopyDescriptorsSimpleChecked(1, OffsetCpuHandle(allocation.cpu, descriptorSize, i), m_nullUavDescriptor.cpu, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, "null UAV");
		m_gameNullUavTable = allocation;
		m_gameNullUavTableGeneration = descriptorGeneration;
		return allocation;
	}

	Transition(m_streamoutBuffer.resource.Get(), m_streamoutBuffer.state, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	const uint64 cacheKey = BuildGameUavTableCacheKey(shader, descriptorGeneration);
	if (m_gameStreamoutUavTable.IsValid() &&
		m_gameStreamoutUavTableGeneration == descriptorGeneration &&
		m_gameStreamoutUavTableKey == cacheKey)
	{
		return m_gameStreamoutUavTable;
	}

	auto allocation = m_descriptorManager->AllocateShaderVisible(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kGameUavDescriptorCount);
	const uint32 descriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	for (uint32 i = 0; i < kGameUavDescriptorCount; ++i)
		CopyDescriptorsSimpleChecked(1, OffsetCpuHandle(allocation.cpu, descriptorSize, i), m_nullUavDescriptor.cpu, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, "null UAV");

	D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
	uavDesc.Format = DXGI_FORMAT_UNKNOWN;
	uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
	uavDesc.Buffer.FirstElement = 0;
	uavDesc.Buffer.NumElements = std::max<UINT>(1, (UINT)(m_streamoutBuffer.size / 4));
	uavDesc.Buffer.StructureByteStride = 4;
	uavDesc.Buffer.CounterOffsetInBytes = 0;
	uavDesc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;
	CreateUnorderedAccessViewChecked(m_streamoutBuffer.resource.Get(), &uavDesc,
		OffsetCpuHandle(allocation.cpu, descriptorSize, (uint32)streamoutBindingPoint), "streamout");
	D3D12TraceEvent("streamout_uav", "stage={} shader=0x{:016x} bindingPoint={} tableIndex={} tableGpu=0x{:016x} firstElement={} numElements={} streamoutResource={}",
					shader ? (uint32)shader->shaderType : 0,
					shader ? shader->baseHash : 0,
					streamoutBindingPoint,
					allocation.index + (uint32)streamoutBindingPoint,
					(unsigned long long)allocation.gpu.ptr,
					uavDesc.Buffer.FirstElement,
					uavDesc.Buffer.NumElements,
					(const void*)m_streamoutBuffer.resource.Get());
	m_gameStreamoutUavTable = allocation;
	m_gameStreamoutUavTableKey = cacheKey;
	m_gameStreamoutUavTableGeneration = descriptorGeneration;
	return allocation;
}

D3D12DescriptorManager::Allocation D3D12Renderer::BuildGameSamplerTable(LatteDecompilerShader* shader)
{
	const uint32 descriptorGeneration = m_descriptorManager->GetShaderVisibleSamplerRollovers();
	if (m_gameSamplerTableCacheGeneration != descriptorGeneration)
	{
		m_gameSamplerTableCache.clear();
		m_gameSamplerTableCacheGeneration = descriptorGeneration;
	}

	bool cacheable = false;
	const uint64 cacheKey = GetMemoizedTableCacheKey(m_samplerTableKeyMemo, true, shader, descriptorGeneration, cacheable);
	if (cacheable)
	{
		const auto* memo = GetCurrentTableKeyMemo(m_samplerTableKeyMemo, shader, descriptorGeneration);
		if (memo && memo->lookupDone)
		{
			if (memo->lookupHit)
			{
				m_gameSamplerTableCacheHits++;
				return memo->cachedAllocation;
			}
		}
		else if (auto it = m_gameSamplerTableCache.find(cacheKey); it != m_gameSamplerTableCache.end())
		{
			m_gameSamplerTableCacheHits++;
			return it->second.allocation;
		}
		m_gameSamplerTableCacheMisses++;
	}
	m_tableBuildTextureRefs.clear();

	const uint32 descriptorCount = GetGameSamplerTableDescriptorCount(shader);
	auto allocation = m_descriptorManager->AllocateShaderVisible(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, descriptorCount);
	const uint32 descriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
	const D3D12_SAMPLER_DESC defaultSamplerDesc = BuildD3D12DefaultGameSamplerDesc();
	D3D12SamplerTableState::FinalSlots<D3D12_SAMPLER_DESC, kGameSamplerDescriptorCount>
		finalSamplers(descriptorCount, defaultSamplerDesc);

	if (shader)
	{
		uint32 hostTextureUnitBase = 0;
		switch (shader->shaderType)
		{
		case LatteConst::ShaderType::Vertex:
			hostTextureUnitBase = LATTE_CEMU_VS_TEX_UNIT_BASE;
			break;
		case LatteConst::ShaderType::Pixel:
			hostTextureUnitBase = LATTE_CEMU_PS_TEX_UNIT_BASE;
			break;
		case LatteConst::ShaderType::Geometry:
			hostTextureUnitBase = LATTE_CEMU_GS_TEX_UNIT_BASE;
			break;
		default:
			break;
		}

		for (uint32 textureListIndex = 0; textureListIndex < shader->textureUnitListCount; ++textureListIndex)
		{
			const uint32 relativeUnit = shader->textureUnitList[textureListIndex];
			const sint8 bindingPoint = shader->resourceMapping.textureUnitToBindingPoint[relativeUnit];
			if (bindingPoint < 0 || (uint32)bindingPoint >= descriptorCount)
				continue;

			const uint32 hostTextureUnit = hostTextureUnitBase + relativeUnit;
			LatteTextureViewD3D12* textureView = hostTextureUnit < m_boundTextures.size() ? m_boundTextures[hostTextureUnit] : nullptr;
			D3D12_SAMPLER_DESC samplerDesc = BuildD3D12GameSamplerDesc(shader, relativeUnit, textureView, m_anisoFilterWithPointMipSupported);
			finalSamplers.Set((uint32)bindingPoint, samplerDesc);
			// Sampler tables retain state derived from each referenced texture view.
			if (textureView && textureView->baseTexture)
				m_tableBuildTextureRefs.push_back(textureView->baseTexture);
		}
	}

	finalSamplers.ForEach([&](size_t index, const D3D12_SAMPLER_DESC& samplerDesc)
	{
		CreateSamplerChecked(&samplerDesc, OffsetCpuHandle(allocation.cpu, descriptorSize, (uint32)index), "game sampler");
	});

	if (cacheable)
	{
		m_gameSamplerTableCache[cacheKey] = { allocation, m_tableBuildTextureRefs };
		if (auto* memo = GetCurrentTableKeyMemo(m_samplerTableKeyMemo, shader, descriptorGeneration))
		{
			memo->lookupDone = true;
			memo->lookupHit = true;
			memo->cachedAllocation = allocation;
		}
	}
	return allocation;
}

D3D12DescriptorManager::Allocation D3D12Renderer::GetShaderVisibleOutputSampler(bool linear)
{
	if (!m_descriptorManager)
		return {};

	const auto stats = m_descriptorManager->GetStats();
	if (m_shaderVisibleOutputSamplerGeneration != stats.shaderVisibleSamplerRollovers)
	{
		m_shaderVisibleOutputLinearSampler = {};
		m_shaderVisibleOutputPointSampler = {};
		m_shaderVisibleOutputSamplerGeneration = stats.shaderVisibleSamplerRollovers;
	}

	auto& cachedSampler = linear ? m_shaderVisibleOutputLinearSampler : m_shaderVisibleOutputPointSampler;
	if (cachedSampler.IsValid())
		return cachedSampler;

	const auto& sourceSampler = linear ? m_outputLinearSampler : m_outputPointSampler;
	if (!sourceSampler.IsValid())
		return {};
	cachedSampler = CopyToShaderVisibleChecked(sourceSampler, "output sampler");
	return cachedSampler;
}

void D3D12Renderer::BindGameDescriptorTables(LatteDecompilerShader* vertexShader, LatteDecompilerShader* geometryShader, LatteDecompilerShader* pixelShader, uint32 baseVertex, uint32 baseInstance, uint32 vertexIdRemapMode, uint64 pipelineHash)
{
	if (!m_commandListOpen || !m_gameRootSignature)
		return;

	auto* commandList = GetCurrentCommandSlot().commandList.Get();
	SetD3D12GraphicsRootSignature(m_gameRootSignature.Get());

	D3D12_GPU_VIRTUAL_ADDRESS vsUniformCbv = 0;
	D3D12_GPU_VIRTUAL_ADDRESS psUniformCbv = 0;
	D3D12_GPU_VIRTUAL_ADDRESS gsUniformCbv = 0;
	const uint64 drawId = m_gameDrawCallsSubmitted + 1;
	auto vsTable = BuildGameResourceTable(vertexShader, vsUniformCbv, drawId, pipelineHash);
	auto psTable = BuildGameResourceTable(pixelShader, psUniformCbv, drawId, pipelineHash);
	auto gsTable = BuildGameResourceTable(geometryShader, gsUniformCbv, drawId, pipelineHash);
	auto vsUavTable = BuildGameUavTable(vertexShader);
	auto psUavTable = BuildGameUavTable(pixelShader);
	auto gsUavTable = BuildGameUavTable(geometryShader);
	auto vsSamplerTable = BuildGameSamplerTable(vertexShader);
	auto psSamplerTable = BuildGameSamplerTable(pixelShader);
	auto gsSamplerTable = BuildGameSamplerTable(geometryShader);
	// building tables may roll the heap page; bind only after all tables are resolved
	BindShaderVisibleHeaps();

	const float invViewportWidth = 1.0f / std::max<float>(m_currentViewport.Width, 1.0f);
	const float invViewportHeight = 1.0f / std::max<float>(std::abs(m_currentViewport.Height), 1.0f);
	const auto& pointSizeReg = RecordRegs().PA_SU_POINT_SIZE;
	const float pointWidth = (float)pointSizeReg.get_WIDTH() / 8.0f;
	const float pointHeight = (float)pointSizeReg.get_HEIGHT() / 8.0f;
	uint32 drawConstants[kGameDrawConstantCount] =
	{
		m_logicOpEmulationMode,
		baseVertex,
		baseInstance,
		vertexIdRemapMode,
		std::bit_cast<uint32>(invViewportWidth),
		std::bit_cast<uint32>(invViewportHeight),
		std::bit_cast<uint32>(pointWidth),
		std::bit_cast<uint32>(pointHeight),
	};
	if (auto* fetchShader = s_recordFetchShader)
	{
		for (auto& group : fetchShader->bufferGroups)
		{
			if (group.attributeBufferIndex < LATTE_MAX_VERTEX_BUFFERS)
				drawConstants[8 + group.attributeBufferIndex] = group.getCurrentBufferStride(RecordRegWords());
		}
	}
	for (uint32 bufferIndex = 0; bufferIndex < LATTE_MAX_VERTEX_BUFFERS; ++bufferIndex)
	{
		if (bufferIndex < m_vertexBindings.size() && m_vertexBindings[bufferIndex].valid)
			drawConstants[24] |= (m_vertexBindings[bufferIndex].offset & 3u) << (bufferIndex * 2);
	}
	if (D3D12ShouldTrace())
	{
		std::string strideSummary;
		for (uint32 i = 0; i < LATTE_MAX_VERTEX_BUFFERS; ++i)
		{
			if (!strideSummary.empty())
				strideSummary.push_back(',');
			fmt::format_to(std::back_inserter(strideSummary), "{}:{}", i, drawConstants[8 + i]);
		}
		D3D12TraceEvent("draw_vertex_stride_constants", "draw={} pipeline=0x{:016x} strides={}",
						(unsigned long long)drawId,
						(unsigned long long)pipelineHash,
						strideSummary);
	}
	D3D12TraceEvent("draw_descriptor_tables", "draw={} pipeline=0x{:016x} vs=0x{:016x} ps=0x{:016x} gs=0x{:016x} rootResourceTables=0,1,2 rootCbvs=3,4,5 rootDrawConstants=6 rootSamplerTables=7,8,9 rootUavTables=10,11,12 vsTableGpu=0x{:016x} psTableGpu=0x{:016x} gsTableGpu=0x{:016x} vsTableIndex={} psTableIndex={} gsTableIndex={} vsCbv=0x{:016x} psCbv=0x{:016x} gsCbv=0x{:016x} drawConstLogicMode={} drawConstBaseVertex={} drawConstBaseInstance={} drawConstReserved={} invViewportBits=0x{:08x},0x{:08x} invViewportFloat={:.9g},{:.9g} vsSamplerGpu=0x{:016x} psSamplerGpu=0x{:016x} gsSamplerGpu=0x{:016x} vsUavGpu=0x{:016x} psUavGpu=0x{:016x} gsUavGpu=0x{:016x}",
					(unsigned long long)drawId,
					(unsigned long long)pipelineHash,
					vertexShader ? vertexShader->baseHash : 0,
					pixelShader ? pixelShader->baseHash : 0,
					geometryShader ? geometryShader->baseHash : 0,
					(unsigned long long)vsTable.gpu.ptr,
					(unsigned long long)psTable.gpu.ptr,
					(unsigned long long)gsTable.gpu.ptr,
					vsTable.index,
					psTable.index,
					gsTable.index,
					(unsigned long long)vsUniformCbv,
					(unsigned long long)psUniformCbv,
					(unsigned long long)gsUniformCbv,
					drawConstants[0],
					drawConstants[1],
					drawConstants[2],
					drawConstants[3],
					drawConstants[4],
					drawConstants[5],
					invViewportWidth,
					invViewportHeight,
					(unsigned long long)vsSamplerTable.gpu.ptr,
					(unsigned long long)psSamplerTable.gpu.ptr,
					(unsigned long long)gsSamplerTable.gpu.ptr,
					(unsigned long long)vsUavTable.gpu.ptr,
					(unsigned long long)psUavTable.gpu.ptr,
					(unsigned long long)gsUavTable.gpu.ptr);
	auto traceMissingUniformCbv = [drawId, pipelineHash](const char* stageName, LatteDecompilerShader* shader, D3D12_GPU_VIRTUAL_ADDRESS cbvAddress)
	{
		if (!shader || cbvAddress != 0 || shader->uniform.uniformRangeSize == 0 || shader->resourceMapping.uniformVarsBufferBindingPoint != 0)
			return;
		D3D12TraceEvent("draw_uniform_cbv_missing", "draw={} pipeline=0x{:016x} stage={} shader=0x{:016x} aux=0x{:016x} rangeSize={} bindingPoint={} locRemapped={} locUniformRegister={} uniformRegisterCount={} locWindowToClip={} locFragCoordScale={} locAlphaRef={} locPointSize={}",
						(unsigned long long)drawId,
						(unsigned long long)pipelineHash,
						stageName,
						shader->baseHash,
						shader->auxHash,
						shader->uniform.uniformRangeSize,
						(sint32)shader->resourceMapping.uniformVarsBufferBindingPoint,
						shader->uniform.loc_remapped,
						shader->uniform.loc_uniformRegister,
						shader->uniform.count_uniformRegister,
						shader->uniform.loc_windowSpaceToClipSpaceTransform,
						shader->uniform.loc_fragCoordScale,
						shader->uniform.loc_alphaTestRef,
						shader->uniform.loc_pointSize);
	};
	traceMissingUniformCbv("VS", vertexShader, vsUniformCbv);
	traceMissingUniformCbv("PS", pixelShader, psUniformCbv);
	traceMissingUniformCbv("GS", geometryShader, gsUniformCbv);
	auto setRootDescriptorTable = [this, commandList](uint32 rootIndex, D3D12_GPU_DESCRIPTOR_HANDLE table)
	{
		if (rootIndex < m_gameRootDescriptorTableValid.size() &&
			m_gameRootDescriptorTableValid[rootIndex] &&
			m_gameRootDescriptorTables[rootIndex].ptr == table.ptr)
		{
			m_commandStateBindSkips++;
			return;
		}
		commandList->SetGraphicsRootDescriptorTable(rootIndex, table);
		if (rootIndex < m_gameRootDescriptorTableValid.size())
		{
			m_gameRootDescriptorTables[rootIndex] = table;
			m_gameRootDescriptorTableValid[rootIndex] = true;
		}
		m_commandStateBinds++;
	};
	auto setRootCbv = [this, commandList](uint32 rootIndex, D3D12_GPU_VIRTUAL_ADDRESS address)
	{
		if (address == 0)
			return;
		if (rootIndex < m_gameRootCbvValid.size() &&
			m_gameRootCbvValid[rootIndex] &&
			m_gameRootCbvs[rootIndex] == address)
		{
			m_commandStateBindSkips++;
			return;
		}
		commandList->SetGraphicsRootConstantBufferView(rootIndex, address);
		if (rootIndex < m_gameRootCbvValid.size())
		{
			m_gameRootCbvs[rootIndex] = address;
			m_gameRootCbvValid[rootIndex] = true;
		}
		m_commandStateBinds++;
	};
	auto setRootDrawConstants = [this, commandList](const uint32 (&constants)[kGameDrawConstantCount])
	{
		if (m_gameRootDrawConstantsValid &&
			std::memcmp(m_gameRootDrawConstants.data(), constants, sizeof(constants)) == 0)
		{
			m_commandStateBindSkips++;
			return;
		}
		commandList->SetGraphicsRoot32BitConstants(6, kGameDrawConstantCount, constants, 0);
		std::copy(std::begin(constants), std::end(constants), m_gameRootDrawConstants.begin());
		m_gameRootDrawConstantsValid = true;
		m_commandStateBinds++;
	};
	setRootDescriptorTable(0, vsTable.gpu);
	setRootDescriptorTable(1, psTable.gpu);
	setRootDescriptorTable(2, gsTable.gpu);
	setRootCbv(3, vsUniformCbv);
	setRootCbv(4, psUniformCbv);
	setRootCbv(5, gsUniformCbv);
	setRootDrawConstants(drawConstants);
	setRootDescriptorTable(7, vsSamplerTable.gpu);
	setRootDescriptorTable(8, psSamplerTable.gpu);
	setRootDescriptorTable(9, gsSamplerTable.gpu);
	setRootDescriptorTable(10, vsUavTable.gpu);
	setRootDescriptorTable(11, psUavTable.gpu);
	setRootDescriptorTable(12, gsUavTable.gpu);
}

ID3D12PipelineState* D3D12Renderer::GetOutputPipeline(RendererOutputShader* shader, bool padView)
{
	if (!shader || !m_outputRootSignature)
	{
		D3D12TraceEvent("output_pipeline_unavailable", "reason=missing_shader_or_root shader={} rootSig={} pad={}",
						(const void*)shader,
						m_outputRootSignature ? 1 : 0,
						padView ? 1 : 0);
		return nullptr;
	}

	auto* vertexShader = static_cast<RendererShaderD3D12*>(shader->GetVertexShader());
	auto* pixelShader = static_cast<RendererShaderD3D12*>(shader->GetFragmentShader());
	if (!vertexShader || !pixelShader || !vertexShader->WaitForCompiled() || !pixelShader->WaitForCompiled())
	{
		D3D12TraceEvent("output_pipeline_unavailable", "reason=shader_compile_incomplete shader={} vs={} ps={} pad={}",
						(const void*)shader,
						(const void*)vertexShader,
						(const void*)pixelShader,
						padView ? 1 : 0);
		return nullptr;
	}
	if (!vertexShader->HasDXIL() || !pixelShader->HasDXIL())
	{
		D3D12TraceEvent("output_pipeline_unavailable", "reason=missing_dxil shader={} vs={} ps={} vsDxil={} psDxil={} pad={}",
						(const void*)shader,
						(const void*)vertexShader,
						(const void*)pixelShader,
						vertexShader->HasDXIL() ? 1 : 0,
						pixelShader->HasDXIL() ? 1 : 0,
						padView ? 1 : 0);
		return nullptr;
	}

	const uint64 key = ((uint64)(uintptr_t)vertexShader) ^ std::rotl((uint64)(uintptr_t)pixelShader, 17) ^ ((uint64)padView << 3);
	auto it = m_outputPipelineCache.find(key);
	if (it != m_outputPipelineCache.end())
	{
		D3D12TraceEvent("output_pipeline_cache_hit", "key=0x{:016x} shader={} vs={} ps={} pad={} pipeline={}",
						key,
						(const void*)shader,
						(const void*)vertexShader,
						(const void*)pixelShader,
						padView ? 1 : 0,
						(const void*)it->second.Get());
		return it->second.Get();
	}

	D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
	desc.pRootSignature = m_outputRootSignature.Get();
	desc.VS = { vertexShader->GetDXIL().data(), vertexShader->GetDXIL().size() };
	desc.PS = { pixelShader->GetDXIL().data(), pixelShader->GetDXIL().size() };
	desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
	desc.SampleMask = UINT_MAX;
	desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
	desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
	desc.RasterizerState.FrontCounterClockwise = FALSE;
	desc.RasterizerState.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
	desc.RasterizerState.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
	desc.RasterizerState.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
	desc.RasterizerState.DepthClipEnable = TRUE;
	desc.RasterizerState.MultisampleEnable = FALSE;
	desc.RasterizerState.AntialiasedLineEnable = FALSE;
	desc.RasterizerState.ForcedSampleCount = 0;
	desc.RasterizerState.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
	desc.DepthStencilState.DepthEnable = FALSE;
	desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
	desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
	desc.DepthStencilState.StencilEnable = FALSE;
	desc.InputLayout = {};
	desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	desc.NumRenderTargets = 1;
	desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
	desc.SampleDesc.Count = 1;

	D3D12TraceEvent("output_pipeline_create_begin", "key=0x{:016x} shader={} vs={} ps={} pad={} vsBytes={} psBytes={} rtv0={} rootSig={}",
					key,
					(const void*)shader,
					(const void*)vertexShader,
					(const void*)pixelShader,
					padView ? 1 : 0,
					(unsigned long long)desc.VS.BytecodeLength,
					(unsigned long long)desc.PS.BytecodeLength,
					(uint32)desc.RTVFormats[0],
					(const void*)m_outputRootSignature.Get());
	Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline;
	const HRESULT hr = m_device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pipeline));
	if (FAILED(hr))
	{
		cemuLog_log(LogType::Force, "D3D12: failed to create output blit PSO (HRESULT 0x{:08x})", (uint32)hr);
		D3D12TraceEvent("output_pipeline_create_failed", "key=0x{:016x} hr=0x{:08x} shader={} vs={} ps={} pad={} vsBytes={} psBytes={} rtv0={}",
						key,
						(uint32)hr,
						(const void*)shader,
						(const void*)vertexShader,
						(const void*)pixelShader,
						padView ? 1 : 0,
						(unsigned long long)desc.VS.BytecodeLength,
						(unsigned long long)desc.PS.BytecodeLength,
						(uint32)desc.RTVFormats[0]);
		return nullptr;
	}
	SetD3D12ObjectName(pipeline.Get(), L"Cemu D3D12 output blit PSO");
	auto [insertedIt, inserted] = m_outputPipelineCache.emplace(key, std::move(pipeline));
	// keep the PSO names apart in captures: the FSR upscale pass draws into its own target, not the swapchain
	if (shader == RendererOutputShader::s_fsr_upscale_shader)
		SetD3D12ObjectName(insertedIt->second.Get(), L"Cemu D3D12 output upscale PSO");
	(void)inserted;
	D3D12TraceEvent("output_pipeline_create_success", "key=0x{:016x} shader={} vs={} ps={} pad={} pipeline={} cacheSize={}",
					key,
					(const void*)shader,
					(const void*)vertexShader,
					(const void*)pixelShader,
					padView ? 1 : 0,
					(const void*)insertedIt->second.Get(),
					m_outputPipelineCache.size());
	return insertedIt->second.Get();
}

D3D12DescriptorManager::Allocation D3D12Renderer::DrawOutputPrePass(RendererOutputShader* prePass, const D3D12DescriptorManager::Allocation& sourceSrv,
	sint32 width, sint32 height, bool padView, D3D12_GPU_VIRTUAL_ADDRESS uniformAddress)
{
	if (!prePass || !sourceSrv.IsValid() || width <= 0 || height <= 0)
		return {};
	OutputPrePassTarget& target = m_outputPrePassTargets[padView ? 1 : 0];
	if (!target.texture.IsValid() || target.width != width || target.height != height)
	{
		if (target.texture.IsValid())
			ReleaseResourceDeferred(target.texture);
		if (target.rtv.IsValid())
			m_descriptorManager->FreePersistent(target.rtv);
		if (target.srv.IsValid())
			m_descriptorManager->FreePersistent(target.srv);
		target = {};

		// the backbuffer format, so the output pipelines fit both passes
		D3D12_RESOURCE_DESC desc{};
		desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		desc.Width = (UINT64)width;
		desc.Height = (UINT)height;
		desc.DepthOrArraySize = 1;
		desc.MipLevels = 1;
		desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		desc.SampleDesc.Count = 1;
		desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
		desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
		if (!CreateTextureWithRecovery(desc, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, target.texture, "output upscale target"))
		{
			cemuLog_log(LogType::Force, "D3D12: could not create the {}x{} output upscale target; using the plain copy instead.", width, height);
			target = {};
			return {};
		}
		target.rtv = m_descriptorManager->AllocatePersistent(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
		target.srv = m_descriptorManager->AllocatePersistent(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
		if (!target.rtv.IsValid() || !target.srv.IsValid())
		{
			ReleaseResourceDeferred(target.texture);
			if (target.rtv.IsValid())
				m_descriptorManager->FreePersistent(target.rtv);
			if (target.srv.IsValid())
				m_descriptorManager->FreePersistent(target.srv);
			target = {};
			return {};
		}
		CreateRenderTargetViewChecked(target.texture.resource.Get(), nullptr, target.rtv.cpu, "output pre-pass");
		CreateShaderResourceViewChecked(target.texture.resource.Get(), nullptr, target.srv.cpu, "output pre-pass", false);
		SetD3D12ObjectName(target.texture.resource.Get(), padView ? L"Cemu D3D12 pad output upscale target" : L"Cemu D3D12 TV output upscale target");
		target.width = width;
		target.height = height;
	}

	ID3D12PipelineState* pipeline = GetOutputPipeline(prePass, padView);
	if (!pipeline)
		return {};

	const auto descriptorStats = m_descriptorManager->GetStats();
	const bool hasCachedPointSampler = m_shaderVisibleOutputPointSampler.IsValid() &&
		m_shaderVisibleOutputSamplerGeneration == descriptorStats.shaderVisibleSamplerRollovers;
	EnsureShaderVisibleDescriptorSpace(1, hasCachedPointSampler ? 0 : 1);
	const auto srv = CopyToShaderVisibleChecked(sourceSrv, "output pre-pass");
	// the upscale gathers its own taps, so the filter does not matter
	const auto sampler = GetShaderVisibleOutputSampler(false);
	BindShaderVisibleHeaps();

	Transition(target.texture.resource.Get(), target.texture.state, D3D12_RESOURCE_STATE_RENDER_TARGET);
	auto* commandList = GetCurrentCommandSlot().commandList.Get();
	SetD3D12PipelineState(pipeline);
	SetD3D12GraphicsRootSignature(m_outputRootSignature.Get());
	commandList->SetGraphicsRootDescriptorTable(0, srv.gpu);
	commandList->SetGraphicsRootDescriptorTable(1, sampler.gpu);
	commandList->SetGraphicsRootConstantBufferView(2, uniformAddress);
	commandList->SetGraphicsRootConstantBufferView(3, uniformAddress);
	SetD3D12RenderTargets(1, &target.rtv.cpu, nullptr);
	const D3D12_VIEWPORT viewport{ 0.0f, 0.0f, (float)width, (float)height, 0.0f, 1.0f };
	const D3D12_RECT scissor{ 0, 0, width, height };
	SetD3D12Viewport(viewport);
	SetD3D12Scissor(scissor);
	SetD3D12PrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	commandList->DrawInstanced(6, 1, 0, 0);
	Transition(target.texture.resource.Get(), target.texture.state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	D3D12TraceEvent("output_prepass", "window={} size={}x{} target={} pipeline={}",
		padView ? "pad" : "tv", width, height, (const void*)target.texture.resource.Get(), (const void*)pipeline);
	return target.srv;
}

void D3D12Renderer::ReleaseOutputPrePassTargets()
{
	for (auto& target : m_outputPrePassTargets)
	{
		if (target.texture.IsValid())
			ReleaseResourceDeferred(target.texture);
		if (m_descriptorManager)
		{
			if (target.rtv.IsValid())
				m_descriptorManager->FreePersistent(target.rtv);
			if (target.srv.IsValid())
				m_descriptorManager->FreePersistent(target.srv);
		}
		target = {};
	}
}

bool D3D12Renderer::ShouldUsePointExpansionGeometryShader(LatteDecompilerShader* geometryShader,
														  Latte::LATTE_VGT_PRIMITIVE_TYPE::E_PRIMITIVE_TYPE primitiveMode) const
{
	return geometryShader == nullptr && D3D12PrimitiveUsesPointExpansion(primitiveMode);
}

bool D3D12Renderer::UsesDynamicDepthBias(bool usePointExpansionGS) const
{
	return m_dynamicDepthBiasSupported && !usePointExpansionGS &&
		RecordRegs().PA_SU_SC_MODE_CNTL.get_OFFSET_FRONT_ENABLED();
}

RendererShaderD3D12* D3D12Renderer::GetPointExpansionGeometryShader(LatteDecompilerShader* vertexShader, LatteDecompilerShader* pixelShader)
{
	if (!vertexShader)
		return nullptr;

	struct SemanticMapping
	{
		uint32 fullSemantic{};
		uint32 semanticIndex{};
		const char* interpolationModifier{};
	};
	std::vector<SemanticMapping> semantics;
	LatteShaderPSInputTable* psInputTable = (&s_recordPsInputTable);
	if (psInputTable)
	{
		for (sint32 i = 0; i < psInputTable->count; ++i)
		{
			const uint32 semanticId = psInputTable->import[i].semanticId;
			if (semanticId > LATTE_ANALYZER_IMPORT_INDEX_PARAM_MAX)
				continue;
			semantics.push_back({ semanticId, (uint32)i, D3D12InterpolationModifier(&psInputTable->import[i]) });
		}
	}
	else
	{
		bool emittedLocal[128]{};
		const uint32 parameterMask = vertexShader->outputParameterMask;
		for (uint32 i = 0; i < 32; ++i)
		{
			if ((parameterMask & (1u << i)) == 0)
				continue;
			const sint32 semanticId = LatteShaderPSInputTable::getVertexShaderOutParamSemanticId(RecordRegWords(), i);
			if (semanticId < 0 || semanticId == 0xFF)
				continue;
			const uint32 localSemantic = (uint32)semanticId & 0x7F;
			if (localSemantic >= std::size(emittedLocal) || emittedLocal[localSemantic])
				continue;
			emittedLocal[localSemantic] = true;
			semantics.push_back({ (uint32)semanticId, localSemantic, "" });
		}
	}

	uint64 key = 0xD3129017u + kD3D12SyntheticGsGeneratorVersion;
	key = std::rotl(key, 7) ^ vertexShader->outputParameterMask;
	key = std::rotl(key, 7) ^ (pixelShader ? pixelShader->baseHash : 0);
	const bool shaderWritesPointSize = vertexShader->uniform.loc_pointSize < 0;
	key = std::rotl(key, 7) ^ (shaderWritesPointSize ? 0x50535752u : 0x50534658u);
	for (const auto& semantic : semantics)
	{
		key = std::rotl(key, 5) ^ semantic.fullSemantic;
		key = std::rotl(key, 5) ^ semantic.semanticIndex;
		key = std::rotl(key, 5) ^ (semantic.interpolationModifier[0] == 'n' ? (semantic.interpolationModifier[2] == 'i' ? 0xF147u : 0xF0A5u) : 0u);
	}

	auto it = m_pointExpansionGeometryShaders.find(key);
	if (it != m_pointExpansionGeometryShaders.end())
		return it->second.get();

	std::string hlsl;
	hlsl.reserve(8192);
	hlsl += fmt::format("// D3D12 synthetic point expansion geometry shader {:016x}\n", key);
	hlsl += "cbuffer LatteD3D12DrawConstants : register(b1, space1)\n";
	hlsl += "{\n";
	hlsl += "\tuint uf_d3d12LogicOpMode;\n";
	hlsl += "\tuint uf_d3d12BaseVertex;\n";
	hlsl += "\tuint uf_d3d12BaseInstance;\n";
	hlsl += "\tuint uf_d3d12Reserved0;\n";
	hlsl += "\tfloat2 uf_d3d12InvViewportSize;\n";
	hlsl += "\tuint2 uf_d3d12DrawConstantsPadding;\n";
	hlsl += "\tuint4 uf_d3d12VertexStrides[4];\n";
	hlsl += "};\n";
	hlsl += "struct GSIn\n{\n";
	hlsl += "\tfloat4 position : SV_Position;\n";
	for (const auto& semantic : semantics)
		hlsl += fmt::format("\t{}float4 passParameterSem{} : TEXCOORD{};\n", semantic.interpolationModifier, semantic.fullSemantic, semantic.semanticIndex);
	// Match VSOut field order on Xbox and preserve shader-written point sizes.
	hlsl += "\tfloat pointSize : POINTSIZE;\n";
	hlsl += "};\n";
	hlsl += "struct GSOut\n{\n";
	hlsl += "\tfloat4 position : SV_Position;\n";
	for (const auto& semantic : semantics)
		hlsl += fmt::format("\t{}float4 passParameterSem{} : TEXCOORD{};\n", semantic.interpolationModifier, semantic.fullSemantic, semantic.semanticIndex);
	// pointCoord must come after the varyings to match the PS signature order
	hlsl += "\tfloat2 pointCoord : CEMUPOINTCOORD;\n";
	hlsl += "};\n";
	hlsl += "[maxvertexcount(4)]\n";
	hlsl += "void main(point GSIn input[1], inout TriangleStream<GSOut> stream)\n";
	hlsl += "{\n";
	hlsl += "\tfloat4 center = input[0].position;\n";
	if (shaderWritesPointSize)
		hlsl += "\tfloat2 pointSize = max(input[0].pointSize.xx, float2(0.125f, 0.125f));\n";
	else
		hlsl += "\tfloat2 pointSize = max(float2(asfloat(uf_d3d12DrawConstantsPadding.x), asfloat(uf_d3d12DrawConstantsPadding.y)), float2(0.125f, 0.125f));\n";
	hlsl += "\tfloat2 halfClip = pointSize * uf_d3d12InvViewportSize * center.w;\n";
	hlsl += "\tfloat2 offsets[4] = { float2(-halfClip.x, -halfClip.y), float2(-halfClip.x, halfClip.y), float2(halfClip.x, -halfClip.y), float2(halfClip.x, halfClip.y) };\n";
	hlsl += "\tfloat2 coords[4] = { float2(0.0f, 1.0f), float2(0.0f, 0.0f), float2(1.0f, 1.0f), float2(1.0f, 0.0f) };\n";
	hlsl += "\t[unroll] for (uint i = 0; i < 4; ++i)\n";
	hlsl += "\t{\n";
	hlsl += "\t\tGSOut output = (GSOut)0;\n";
	hlsl += "\t\toutput.position = center;\n";
	hlsl += "\t\toutput.position.xy += offsets[i];\n";
	hlsl += "\t\toutput.pointCoord = coords[i];\n";
	for (const auto& semantic : semantics)
		hlsl += fmt::format("\t\toutput.passParameterSem{} = input[0].passParameterSem{};\n", semantic.fullSemantic, semantic.fullSemantic);
	hlsl += "\t\tstream.Append(output);\n";
	hlsl += "\t}\n";
	hlsl += "\tstream.RestartStrip();\n";
	hlsl += "}\n";

	D3D12DumpHLSL(RendererShader::ShaderType::kGeometry, key, 0xD3129017ull, true, hlsl);
	auto shader = std::make_unique<RendererShaderD3D12>(RendererShader::ShaderType::kGeometry, key, 0xD3129017u, false, false, hlsl);
	auto* shaderPtr = shader.get();
	m_pointExpansionGeometryShaders.emplace(key, std::move(shader));
	cemuLog_logOnce(LogType::Force, "D3D12: using synthetic geometry shaders for native point-size expansion.");
	return shaderPtr;
}

RendererShaderD3D12* D3D12Renderer::GetRectEmulationGeometryShader(LatteDecompilerShader* vertexShader, LatteDecompilerShader* pixelShader)
{
	if (!vertexShader)
		return nullptr;

	struct SemanticMapping
	{
		uint32 fullSemantic{};
		uint32 semanticIndex{};
		const char* interpolationModifier{};
	};
	std::vector<SemanticMapping> semantics;
	LatteShaderPSInputTable* psInputTable = (&s_recordPsInputTable);
	if (psInputTable)
	{
		for (sint32 i = 0; i < psInputTable->count; ++i)
		{
			const uint32 semanticId = psInputTable->import[i].semanticId;
			if (semanticId > LATTE_ANALYZER_IMPORT_INDEX_PARAM_MAX)
				continue;
			semantics.push_back({ semanticId, (uint32)i, D3D12InterpolationModifier(&psInputTable->import[i]) });
		}
	}
	else
	{
		bool emittedLocal[128]{};
		const uint32 parameterMask = vertexShader->outputParameterMask;
		for (uint32 i = 0; i < 32; ++i)
		{
			if ((parameterMask & (1u << i)) == 0)
				continue;
			const sint32 semanticId = LatteShaderPSInputTable::getVertexShaderOutParamSemanticId(RecordRegWords(), i);
			if (semanticId < 0 || semanticId == 0xFF)
				continue;
			const uint32 localSemantic = (uint32)semanticId & 0x7F;
			if (localSemantic >= std::size(emittedLocal) || emittedLocal[localSemantic])
				continue;
			emittedLocal[localSemantic] = true;
			semantics.push_back({ (uint32)semanticId, localSemantic, "" });
		}
	}

	uint64 key = 0xD3120EC7ull + kD3D12SyntheticGsGeneratorVersion;
	key = std::rotl(key, 7) ^ vertexShader->outputParameterMask;
	key = std::rotl(key, 7) ^ (pixelShader ? pixelShader->baseHash : 0);
	for (const auto& semantic : semantics)
	{
		key = std::rotl(key, 5) ^ semantic.fullSemantic;
		key = std::rotl(key, 5) ^ semantic.semanticIndex;
		key = std::rotl(key, 5) ^ (semantic.interpolationModifier[0] == 'n' ? (semantic.interpolationModifier[2] == 'i' ? 0xF147u : 0xF0A5u) : 0u);
	}

	auto it = m_rectEmulationGeometryShaders.find(key);
	if (it != m_rectEmulationGeometryShaders.end())
		return it->second.get();

	std::string hlsl;
	hlsl.reserve(8192);
	hlsl += fmt::format("// D3D12 synthetic rectangle emulation geometry shader {:016x}\n", key);
	hlsl += "struct GSIn\n{\n";
	hlsl += "\tfloat4 position : SV_Position;\n";
	for (const auto& semantic : semantics)
		hlsl += fmt::format("\t{}float4 passParameterSem{} : TEXCOORD{};\n", semantic.interpolationModifier, semantic.fullSemantic, semantic.semanticIndex);
	hlsl += "};\n";
	hlsl += "struct GSOut\n{\n";
	hlsl += "\tfloat4 position : SV_Position;\n";
	for (const auto& semantic : semantics)
		hlsl += fmt::format("\t{}float4 passParameterSem{} : TEXCOORD{};\n", semantic.interpolationModifier, semantic.fullSemantic, semantic.semanticIndex);
	hlsl += "};\n";
	hlsl += "float4 gen4thVertexA(float4 a, float4 b, float4 c)\n{\n\treturn b - (c - a);\n}\n";
	hlsl += "float4 gen4thVertexB(float4 a, float4 b, float4 c)\n{\n\treturn c - (b - a);\n}\n";
	hlsl += "float4 gen4thVertexC(float4 a, float4 b, float4 c)\n{\n\treturn c + (b - a);\n}\n";
	hlsl += "void emitSingleVertex(uint index, GSIn input[3], inout TriangleStream<GSOut> stream)\n{\n";
	hlsl += "\tGSOut output = (GSOut)0;\n";
	hlsl += "\toutput.position = input[index].position;\n";
	for (const auto& semantic : semantics)
		hlsl += fmt::format("\toutput.passParameterSem{} = input[index].passParameterSem{};\n", semantic.fullSemantic, semantic.fullSemantic);
	hlsl += "\tstream.Append(output);\n";
	hlsl += "}\n";
	hlsl += "void emitGeneratedVertex(uint variant, GSIn input[3], inout TriangleStream<GSOut> stream)\n{\n";
	hlsl += "\tGSOut output = (GSOut)0;\n";
	hlsl += "\tif (variant == 0) output.position = gen4thVertexA(input[0].position, input[1].position, input[2].position);\n";
	hlsl += "\telse if (variant == 1) output.position = gen4thVertexB(input[0].position, input[1].position, input[2].position);\n";
	hlsl += "\telse output.position = gen4thVertexC(input[0].position, input[1].position, input[2].position);\n";
	for (const auto& semantic : semantics)
	{
		hlsl += fmt::format("\tif (variant == 0) output.passParameterSem{} = gen4thVertexA(input[0].passParameterSem{}, input[1].passParameterSem{}, input[2].passParameterSem{});\n",
							semantic.fullSemantic, semantic.fullSemantic, semantic.fullSemantic, semantic.fullSemantic);
		hlsl += fmt::format("\telse if (variant == 1) output.passParameterSem{} = gen4thVertexB(input[0].passParameterSem{}, input[1].passParameterSem{}, input[2].passParameterSem{});\n",
							semantic.fullSemantic, semantic.fullSemantic, semantic.fullSemantic, semantic.fullSemantic);
		hlsl += fmt::format("\telse output.passParameterSem{} = gen4thVertexC(input[0].passParameterSem{}, input[1].passParameterSem{}, input[2].passParameterSem{});\n",
							semantic.fullSemantic, semantic.fullSemantic, semantic.fullSemantic, semantic.fullSemantic);
	}
	hlsl += "\tstream.Append(output);\n";
	hlsl += "}\n";
	hlsl += "void emitRectVertex(uint vertex, uint variant, GSIn input[3], inout TriangleStream<GSOut> stream)\n{\n";
	hlsl += "\tif (vertex == 3) emitGeneratedVertex(variant, input, stream);\n";
	hlsl += "\telse emitSingleVertex(vertex, input, stream);\n";
	hlsl += "}\n";
	hlsl += "void emitRect(uint p0, uint p1, uint p2, uint p3, uint variant, GSIn input[3], inout TriangleStream<GSOut> stream)\n{\n";
	hlsl += "\temitRectVertex(p0, variant, input, stream);\n";
	hlsl += "\temitRectVertex(p1, variant, input, stream);\n";
	hlsl += "\temitRectVertex(p2, variant, input, stream);\n";
	hlsl += "\temitRectVertex(p3, variant, input, stream);\n";
	hlsl += "\tstream.RestartStrip();\n";
	hlsl += "}\n";
	hlsl += "[maxvertexcount(4)]\n";
	hlsl += "void main(triangle GSIn input[3], inout TriangleStream<GSOut> stream)\n{\n";
	hlsl += "\tfloat dist0_1 = length(input[1].position.xy - input[0].position.xy);\n";
	hlsl += "\tfloat dist0_2 = length(input[2].position.xy - input[0].position.xy);\n";
	hlsl += "\tfloat dist1_2 = length(input[2].position.xy - input[1].position.xy);\n";
	hlsl += "\tif (dist0_1 > dist0_2 && dist0_1 > dist1_2)\n";
	hlsl += "\t\temitRect(2, 1, 0, 3, 0, input, stream);\n";
	hlsl += "\telse if (dist0_2 > dist0_1 && dist0_2 > dist1_2)\n";
	hlsl += "\t\temitRect(1, 2, 0, 3, 1, input, stream);\n";
	hlsl += "\telse\n";
	hlsl += "\t\temitRect(0, 1, 2, 3, 2, input, stream);\n";
	hlsl += "}\n";

	auto shader = std::make_unique<RendererShaderD3D12>(RendererShader::ShaderType::kGeometry, key, 0xD3120EC7ull, false, false, hlsl);
	auto* shaderPtr = shader.get();
	m_rectEmulationGeometryShaders.emplace(key, std::move(shader));
	cemuLog_logOnce(LogType::Force, "D3D12: using synthetic geometry shaders for native rectangle primitive emulation.");
	return shaderPtr;
}

#if defined(CEMU_UWP)
void D3D12Renderer::PrepareGamePipelineLibrary(uint64 titleId)
{
	if (m_gamePipelineLibraryTitleId != 0 && m_gamePipelineLibraryTitleId != titleId)
		ClearPendingGamePipelineRecipes();
	EnsureGamePipelineLibrary(titleId);
}

void D3D12Renderer::BeginGamePipelineRecipeLoading(uint64 titleId)
{
	ClearPendingGamePipelineRecipes();
	PrepareGamePipelineLibrary(titleId);
	m_gamePipelineRecipePrewarmAttempts = 0;
	m_gamePipelineRecipePrewarmAccepted = 0;
	m_gamePipelineRecipePrewarmRejected = 0;
	m_gamePipelineRecipePrewarmFailure = S_OK;
	m_gamePipelineRecipeBackground = false;
	m_gamePipelineRecipePrewarmStartTime = std::chrono::steady_clock::now();
	const auto memory = CemuUwp::MemoryBudgetService::SampleNow(true);
	m_gamePipelineRecipePrewarmStartUsage = memory.usageBytes;
	m_gamePipelineRecipePrewarmPeakUsage = memory.usageBytes;
	m_gamePipelineRecipePrewarmStopReason = "complete";
	m_gamePipelineRecipeLoadingTitleId = titleId;
	const bool loading = m_device && m_gameRootSignature && titleId != 0;
	m_gamePipelineRecipePrewarmStopped = !loading;
	m_gamePipelineRecipeLoading = loading;
	D3D12TraceEvent("game_pipeline_recipe_loading_begin", "title=0x{:016x} enabled={} cap={}",
		titleId,
		loading ? 1 : 0,
		D3D12PipelinePrewarmPolicy::kMaxRecipeCacheEntries);
	cemuLog_log(LogType::Force,
		"D3D12 UWP pipeline prewarm admission: cap={} startUsageMB={} limitMB={} reservedHeadroomMB={} maxGrowthMB={}.",
		D3D12PipelinePrewarmPolicy::kMaxRecipeCacheEntries, memory.usageBytes / (1024 * 1024),
		memory.limitBytes / (1024 * 1024), D3D12PipelinePrewarmPolicy::kPrewarmMemoryHeadroom / (1024 * 1024),
		D3D12PipelinePrewarmPolicy::kMaxPrewarmMemoryGrowth / (1024 * 1024));
}

void D3D12Renderer::StopGamePipelineRecipePrewarm(const char* reason)
{
	// the first reason wins; later stops are consequences of it
	if (!m_gamePipelineRecipePrewarmStopped.exchange(true, std::memory_order_acq_rel))
		m_gamePipelineRecipePrewarmStopReason.store(reason, std::memory_order_release);
}

void D3D12Renderer::RecordGamePipelineRecipePrewarmUsage(uint64 usageBytes)
{
	uint64 peak = m_gamePipelineRecipePrewarmPeakUsage.load(std::memory_order_relaxed);
	while (usageBytes > peak && !m_gamePipelineRecipePrewarmPeakUsage.compare_exchange_weak(peak, usageBytes, std::memory_order_relaxed))
	{
	}
}

D3D12Renderer::RecipePrewarmResult D3D12Renderer::PrewarmGamePipelineRecipe(uint64 expectedPipelineKey, std::span<const uint8> bytes)
{
	// Runs on prewarm workers next to the render thread, so only atomics and the hand-off list are shared.
	// The pending map and the live pipeline cache belong to the render thread; AdoptPrewarmedGamePipelines
	// drops results whose key went live meanwhile.
	const uint64 generation = m_gamePipelineRecipeGeneration.load(std::memory_order_acquire);
	if (IsGamePipelineRecipePrewarmStopped() || !m_device || !m_gameRootSignature)
		return RecipePrewarmResult::Stopped;
	if (expectedPipelineKey == 0 || bytes.empty())
		return RecipePrewarmResult::Rejected;

	m_gamePipelineRecipePrewarmAttempts.fetch_add(1, std::memory_order_relaxed);
	D3D12PipelineRecipe::OwnedRecipe recipe;
	const D3D12PipelineRecipe::Status decodeStatus = D3D12PipelineRecipe::Decode(bytes, recipe);
	if (decodeStatus != D3D12PipelineRecipe::Status::Success || recipe.pipelineKey != expectedPipelineKey)
	{
		m_gamePipelineRecipePrewarmRejected.fetch_add(1, std::memory_order_relaxed);
		if (decodeStatus == D3D12PipelineRecipe::Status::OutOfMemory)
			StopGamePipelineRecipePrewarm("decode_out_of_memory");
		static std::atomic_uint32_t s_recipeDecodeLogCount{ 0 };
		if (s_recipeDecodeLogCount.fetch_add(1, std::memory_order_relaxed) < 16)
		{
			cemuLog_log(LogType::Force,
				"D3D12 UWP: rejecting pipeline recipe key {:016x} (status={}, embedded key {:016x}).",
				expectedPipelineKey,
				D3D12PipelineRecipe::StatusName(decodeStatus),
				recipe.pipelineKey);
		}
		return decodeStatus == D3D12PipelineRecipe::Status::OutOfMemory ? RecipePrewarmResult::Stopped : RecipePrewarmResult::Rejected;
	}
#if defined(__ID3D12GraphicsCommandList9_INTERFACE_DEFINED__)
	if ((recipe.desc.Flags & D3D12_PIPELINE_STATE_FLAG_DYNAMIC_DEPTH_BIAS) != 0 && !m_dynamicDepthBiasSupported)
	{
		m_gamePipelineRecipePrewarmRejected.fetch_add(1, std::memory_order_relaxed);
		return RecipePrewarmResult::Rejected;
	}
#endif

	recipe.BindRootSignature(m_gameRootSignature.Get());
	Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline;
	HRESULT hr = E_FAIL;
	// every pipeline created this session counts toward the record limit, adopted or not
	const size_t createdPipelines = (size_t)m_gamePipelineRecipePrewarmAccepted.load(std::memory_order_relaxed);
	{
		RendererShaderD3D12::NativeCompilationSlot nativeCompilationSlot;
		const auto memory = CemuUwp::MemoryBudgetService::SampleNow(true);
		RecordGamePipelineRecipePrewarmUsage(memory.usageBytes);
		const uint64 effectiveLimit = memory.expectedLimitBytes != 0 ?
			std::min(memory.limitBytes, memory.expectedLimitBytes) : memory.limitBytes;
		const uint64 growthBaseline = m_gamePipelineRecipeBackground.load(std::memory_order_acquire) ?
			memory.usageBytes : m_gamePipelineRecipePrewarmStartUsage;
		const auto decision = D3D12PipelinePrewarmPolicy::CheckAdmission(memory.enabled || IsXboxConsole(),
			memory.heavyWorkDeferred, memory.pressure != CemuUwp::MemoryBudgetService::Pressure::Normal,
			memory.usageBytes, effectiveLimit, growthBaseline, createdPipelines);
		if (decision != D3D12PipelinePrewarmPolicy::Decision::Allow)
		{
			m_gamePipelineRecipePrewarmRejected.fetch_add(1, std::memory_order_relaxed);
			StopGamePipelineRecipePrewarm(D3D12PipelinePrewarmPolicy::DecisionName(decision));
			cemuLog_log(LogType::Force,
				"D3D12 UWP pipeline prewarm stopped: reason={} created={} usageMB={} limitMB={}.",
				D3D12PipelinePrewarmPolicy::DecisionName(decision), createdPipelines,
				memory.usageBytes / (1024 * 1024), memory.limitBytes / (1024 * 1024));
			return RecipePrewarmResult::Stopped;
		}
#if defined(__ID3D12GraphicsCommandList8_INTERFACE_DEFINED__)
		if (recipe.hasIndependentDepthStencil)
		{
			if (!m_independentFrontBackStencilSupported || !m_independentStencilDevice)
				hr = E_NOINTERFACE;
			else
				hr = CreateGraphicsPipelineWithIndependentStencil(
					m_independentStencilDevice.Get(), recipe.desc, recipe.independentDepthStencil, pipeline);
		}
		else
#endif
		{
			hr = m_device->CreateGraphicsPipelineState(&recipe.desc, IID_PPV_ARGS(&pipeline));
		}
		const auto afterMemory = CemuUwp::MemoryBudgetService::SampleNow(true);
		RecordGamePipelineRecipePrewarmUsage(afterMemory.usageBytes);
		const uint64 afterLimit = afterMemory.expectedLimitBytes != 0 ?
			std::min(afterMemory.limitBytes, afterMemory.expectedLimitBytes) : afterMemory.limitBytes;
		const auto afterDecision = D3D12PipelinePrewarmPolicy::CheckAdmission(afterMemory.enabled || IsXboxConsole(),
			afterMemory.heavyWorkDeferred, afterMemory.pressure != CemuUwp::MemoryBudgetService::Pressure::Normal,
			afterMemory.usageBytes, afterLimit, growthBaseline, createdPipelines);
		if (SUCCEEDED(hr) && pipeline && afterDecision != D3D12PipelinePrewarmPolicy::Decision::Allow)
		{
			// release the object that crossed the budget; EndLoading rechecks afterwards
			pipeline.Reset();
			m_gamePipelineRecipePrewarmRejected.fetch_add(1, std::memory_order_relaxed);
			StopGamePipelineRecipePrewarm(D3D12PipelinePrewarmPolicy::DecisionName(afterDecision));
			cemuLog_log(LogType::Force,
				"D3D12 UWP pipeline prewarm stopped after a creation: reason={} created={} usageMB={} limitMB={}.",
				D3D12PipelinePrewarmPolicy::DecisionName(afterDecision), createdPipelines,
				afterMemory.usageBytes / (1024 * 1024), afterMemory.limitBytes / (1024 * 1024));
			return RecipePrewarmResult::Stopped;
		}
	}
	if (FAILED(hr) || !pipeline)
	{
		m_gamePipelineRecipePrewarmRejected.fetch_add(1, std::memory_order_relaxed);
		StopGamePipelineRecipePrewarm("native_creation_failure");
		// device loss is handled on the render thread when it adopts the results
		HRESULT expected = S_OK;
		m_gamePipelineRecipePrewarmFailure.compare_exchange_strong(expected, FAILED(hr) ? hr : E_FAIL);
		m_prewarmedGamePipelinesAvailable.store(true, std::memory_order_release);
		static std::atomic_uint32_t s_recipeCreateFailureLogCount{ 0 };
		if (s_recipeCreateFailureLogCount.fetch_add(1, std::memory_order_relaxed) < 16)
		{
			cemuLog_log(LogType::Force,
				"D3D12 UWP: stopping pipeline recipe prewarm after key {:016x} failed (HRESULT 0x{:08x}, created {}).",
				expectedPipelineKey,
				(uint32)hr,
				createdPipelines);
		}
		return RecipePrewarmResult::Stopped;
	}

	SetD3D12ObjectName(pipeline.Get(), L"Cemu D3D12 prewarmed game PSO");
	try
	{
		PrewarmedGamePipeline prewarmed;
		prewarmed.generation = generation;
		prewarmed.pipelineKey = expectedPipelineKey;
		prewarmed.recipe.pipeline = std::move(pipeline);
		prewarmed.recipe.fingerprint = recipe.fingerprint;
		prewarmed.recipe.contentFingerprint = recipe.contentFingerprint;
		std::scoped_lock lock(m_prewarmedGamePipelinesMutex);
		m_prewarmedGamePipelines.emplace_back(std::move(prewarmed));
		m_prewarmedGamePipelinesAvailable.store(true, std::memory_order_release);
	}
	catch (const std::bad_alloc&)
	{
		m_gamePipelineRecipePrewarmRejected.fetch_add(1, std::memory_order_relaxed);
		StopGamePipelineRecipePrewarm("bookkeeping_out_of_memory");
		return RecipePrewarmResult::Stopped;
	}

	const uint64 accepted = m_gamePipelineRecipePrewarmAccepted.fetch_add(1, std::memory_order_relaxed) + 1;
	if ((accepted % 512) == 0)
		cemuLog_log(LogType::Force, "D3D12 UWP pipeline prewarm progress: created={} sampledPeakUsageMB={} elapsed_ms={}.",
			accepted, m_gamePipelineRecipePrewarmPeakUsage.load(std::memory_order_relaxed) / (1024 * 1024),
			std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - m_gamePipelineRecipePrewarmStartTime).count());
	return RecipePrewarmResult::Created;
}

ID3D12PipelineState* D3D12Renderer::FindGamePipelineByContent(const std::array<uint8, 32>& contentFingerprint, uint64& canonicalKey)
{
	const auto it = m_gamePipelineKeyByContent.find(contentFingerprint);
	if (it == m_gamePipelineKeyByContent.end())
		return nullptr;
	canonicalKey = it->second;
	if (const auto live = m_gamePipelineCache.find(canonicalKey); live != m_gamePipelineCache.end() && live->second.pipeline)
		return live->second.pipeline.Get();
	if (const auto pending = m_pendingGamePipelineRecipes.find(canonicalKey); pending != m_pendingGamePipelineRecipes.end() && pending->second.pipeline)
		return pending->second.pipeline.Get();
	// evicted or released since; the next build of this content registers itself again
	m_gamePipelineKeyByContent.erase(it);
	return nullptr;
}

size_t D3D12Renderer::AdoptPrewarmedGamePipelines()
{
	if (!m_prewarmedGamePipelinesAvailable.load(std::memory_order_acquire))
		return 0;
	std::vector<PrewarmedGamePipeline> prewarmed;
	{
		std::scoped_lock lock(m_prewarmedGamePipelinesMutex);
		prewarmed.swap(m_prewarmedGamePipelines);
		m_prewarmedGamePipelinesAvailable.store(false, std::memory_order_release);
	}
	const HRESULT failure = m_gamePipelineRecipePrewarmFailure.exchange(S_OK);
	if (FAILED(failure))
	{
		if (IsD3D12TransientMemoryFailure(failure))
			m_pendingGamePipelineRecipes.clear();
		CheckDeviceRemoved(failure, "CreateGraphicsPipelineState(recipe prewarm)");
	}

	const uint64 generation = m_gamePipelineRecipeGeneration.load(std::memory_order_acquire);
	size_t adopted = 0;
	for (auto& entry : prewarmed)
	{
		// a draw may have created the pipeline itself while the worker was busy with it
		if (entry.generation != generation || m_gamePipelineCache.contains(entry.pipelineKey) ||
			m_pendingGamePipelineRecipes.contains(entry.pipelineKey))
			continue;
		try
		{
			entry.recipe.adoptedFrame = LatteGPUState.frameCounter;
			if (entry.recipe.contentFingerprint != std::array<uint8, 32>{})
				m_gamePipelineKeyByContent.try_emplace(entry.recipe.contentFingerprint, entry.pipelineKey);
			m_pendingGamePipelineRecipes.emplace(entry.pipelineKey, std::move(entry.recipe));
			++adopted;
		}
		catch (const std::bad_alloc&)
		{
			m_pendingGamePipelineRecipes.clear();
			StopGamePipelineRecipePrewarm("bookkeeping_out_of_memory");
			break;
		}
	}
	return adopted;
}

void D3D12Renderer::EndGamePipelineRecipeLoading(bool continuesInBackground)
{
	AdoptPrewarmedGamePipelines();
	const auto memory = CemuUwp::MemoryBudgetService::SampleNow(true);
	RecordGamePipelineRecipePrewarmUsage(memory.usageBytes);
	const uint64 effectiveLimit = memory.expectedLimitBytes != 0 ?
		std::min(memory.limitBytes, memory.expectedLimitBytes) : memory.limitBytes;
	// Ignore the record count here: completing a full inventory is success.
	const auto decision = D3D12PipelinePrewarmPolicy::CheckAdmission(memory.enabled || IsXboxConsole(),
		memory.heavyWorkDeferred, memory.pressure != CemuUwp::MemoryBudgetService::Pressure::Normal,
		memory.usageBytes, effectiveLimit, m_gamePipelineRecipePrewarmStartUsage, 0);
	if (decision != D3D12PipelinePrewarmPolicy::Decision::Allow)
	{
		// none of the pending objects reached the GPU, so the whole set can be dropped
		m_pendingGamePipelineRecipes.clear();
		StopGamePipelineRecipePrewarm(D3D12PipelinePrewarmPolicy::DecisionName(decision));
	}
	const bool stopped = m_gamePipelineRecipePrewarmStopped.load(std::memory_order_acquire);
	D3D12TraceEvent("game_pipeline_recipe_loading_end",
		"title=0x{:016x} attempts={} accepted={} rejected={} pending={} stopped={} background={}",
		m_gamePipelineRecipeLoadingTitleId,
		m_gamePipelineRecipePrewarmAttempts.load(),
		m_gamePipelineRecipePrewarmAccepted.load(),
		m_gamePipelineRecipePrewarmRejected.load(),
		m_pendingGamePipelineRecipes.size(),
		stopped ? 1 : 0,
		continuesInBackground ? 1 : 0);
	cemuLog_log(LogType::Force,
		"D3D12 UWP pipeline recipe prewarm: title={:016x} attempts={} nativeCreated={} retained={} rejected={} stopped={} reason={} elapsed_ms={} usageMB={} sampledPeakUsageMB={} limitMB={}{}.",
		m_gamePipelineRecipeLoadingTitleId,
		m_gamePipelineRecipePrewarmAttempts.load(),
		m_gamePipelineRecipePrewarmAccepted.load(),
		m_pendingGamePipelineRecipes.size(),
		m_gamePipelineRecipePrewarmRejected.load(),
		stopped ? 1 : 0,
		m_gamePipelineRecipePrewarmStopReason.load(),
		std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - m_gamePipelineRecipePrewarmStartTime).count(),
		memory.usageBytes / (1024 * 1024), m_gamePipelineRecipePrewarmPeakUsage.load() / (1024 * 1024), memory.limitBytes / (1024 * 1024),
		continuesInBackground && !stopped ? "; the rest continues in the background" : "");
	if (!continuesInBackground || stopped)
		m_gamePipelineRecipeLoading = false;
	else
		m_gamePipelineRecipeBackground = true;
}

void D3D12Renderer::FinishBackgroundGamePipelineRecipePrewarm()
{
	cemuLog_log(LogType::Force,
		"D3D12 UWP background pipeline prewarm finished: title={:016x} attempts={} nativeCreated={} rejected={} stopped={} reason={} elapsed_ms={} sampledPeakUsageMB={}.",
		m_gamePipelineRecipeLoadingTitleId,
		m_gamePipelineRecipePrewarmAttempts.load(),
		m_gamePipelineRecipePrewarmAccepted.load(),
		m_gamePipelineRecipePrewarmRejected.load(),
		m_gamePipelineRecipePrewarmStopped.load() ? 1 : 0,
		m_gamePipelineRecipePrewarmStopReason.load(),
		std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - m_gamePipelineRecipePrewarmStartTime).count(),
		m_gamePipelineRecipePrewarmPeakUsage.load() / (1024 * 1024));
	m_gamePipelineRecipeLoading = false;
	m_gamePipelineRecipeBackground = false;
}

void D3D12Renderer::ClearPendingGamePipelineRecipes()
{
	// results still in flight from a worker belong to the old generation and are dropped on adoption
	m_gamePipelineRecipeGeneration.fetch_add(1, std::memory_order_acq_rel);
	{
		std::scoped_lock lock(m_prewarmedGamePipelinesMutex);
		m_prewarmedGamePipelines.clear();
		m_prewarmedGamePipelinesAvailable.store(false, std::memory_order_release);
	}
	m_pendingGamePipelineRecipes.clear();
	m_gamePipelineRecipeLoadingTitleId = 0;
	m_gamePipelineRecipeLoading = false;
	m_gamePipelineRecipeBackground = false;
	m_gamePipelineRecipePrewarmStopped = false;
	{
		std::scoped_lock lock(m_asyncPipelineMutex);
		m_asyncPipelineJobs.clear();
		m_asyncPipelineFailedKeys.clear();
	}
	m_asyncPipelinesInFlight.clear();
	m_asyncPipelineFailed.clear();
}

bool D3D12Renderer::QueueAsyncGamePipeline(uint64 pipelineKey, std::vector<uint8>&& recipe)
{
	try
	{
		std::scoped_lock lock(m_asyncPipelineMutex);
		if (m_asyncPipelineThreads.empty())
		{
			// PSO creation is mostly single threaded in the driver; two workers keep a burst moving
			// without taking cores from the shader compilers or the emulated CPU
			const uint32 threadCount = std::clamp<uint32>(RendererShaderD3D12::GetNativeCompilationConcurrency() / 2, 1u, 2u);
			m_asyncPipelineStop = false;
			for (uint32 i = 0; i < threadCount; ++i)
				m_asyncPipelineThreads.emplace_back(&D3D12Renderer::AsyncPipelineThreadFunc, this);
			cemuLog_log(LogType::Force, "D3D12: {} async pipeline creation threads.", threadCount);
		}
		AsyncPipelineJob job;
		job.generation = m_gamePipelineRecipeGeneration.load(std::memory_order_acquire);
		job.pipelineKey = pipelineKey;
		job.recipe = std::move(recipe);
		job.rootSignature = m_gameRootSignature;
		m_asyncPipelinesInFlight[pipelineKey] = job.generation;
		m_asyncPipelineJobs.emplace_back(std::move(job));
	}
	catch (const std::exception&)
	{
		m_asyncPipelinesInFlight.erase(pipelineKey);
		return false;
	}
	m_asyncPipelineCondition.notify_one();
	m_asyncPipelinesQueued++;
	return true;
}

void D3D12Renderer::DrainAsyncGamePipelineFailures()
{
	std::vector<uint64> failed;
	{
		std::scoped_lock lock(m_asyncPipelineMutex);
		if (m_asyncPipelineFailedKeys.empty())
			return;
		failed.swap(m_asyncPipelineFailedKeys);
	}
	for (const uint64 key : failed)
	{
		m_asyncPipelinesInFlight.erase(key);
		m_asyncPipelineFailed.insert(key);
	}
}

bool D3D12Renderer::IsAsyncGamePipelinePending(uint64 pipelineKey)
{
	auto it = m_asyncPipelinesInFlight.find(pipelineKey);
	if (it == m_asyncPipelinesInFlight.end())
		return false;
	DrainAsyncGamePipelineFailures();
	AdoptPrewarmedGamePipelines();
	it = m_asyncPipelinesInFlight.find(pipelineKey);
	if (it == m_asyncPipelinesInFlight.end())
		return false;
	// a dropped generation (title change, memory pressure) discards the result, so it has to be queued again
	if (it->second != m_gamePipelineRecipeGeneration.load(std::memory_order_acquire))
	{
		m_asyncPipelinesInFlight.erase(it);
		return false;
	}
	return !m_pendingGamePipelineRecipes.contains(pipelineKey);
}

void D3D12Renderer::ReleaseStaleAsyncGamePipelines()
{
	constexpr uint32 kSweepIntervalFrames = 300;
	constexpr uint32 kUnusedFrames = 1800; // ~30 s at 60 Hz
	static uint32 s_lastSweepFrame = 0;
	const uint32 frame = LatteGPUState.frameCounter;
	if (frame - s_lastSweepFrame < kSweepIntervalFrames)
		return;
	s_lastSweepFrame = frame;
	size_t released = 0;
	for (auto it = m_pendingGamePipelineRecipes.begin(); it != m_pendingGamePipelineRecipes.end();)
	{
		if (it->second.fromAsync && frame - it->second.adoptedFrame >= kUnusedFrames)
		{
			// a later draw with this key queues it again instead of waiting on a result that is gone
			m_asyncPipelinesInFlight.erase(it->first);
			it = m_pendingGamePipelineRecipes.erase(it); // never bound, so no GPU reference
			released++;
		}
		else
			++it;
	}
	m_asyncPipelinesReleasedUnused += released;
	if (released != 0)
	{
		cemuLog_log(LogType::Force, "D3D12 UWP: released {} async PSOs no draw used again within {} frames (total {}, pending {}).",
			released, kUnusedFrames, m_asyncPipelinesReleasedUnused, m_pendingGamePipelineRecipes.size());
	}
}

size_t D3D12Renderer::ReleaseUnusedPrewarmedPipelines(uint32 minAgeFrames, size_t maxPipelines)
{
	const uint32 frame = LatteGPUState.frameCounter;
	size_t released = 0;
	auto& pipelineStableCache = D3D12PipelineStableCache::GetInstance();
	for (auto it = m_pendingGamePipelineRecipes.begin(); it != m_pendingGamePipelineRecipes.end() && released < maxPipelines;)
	{
		// kept: its draw cannot be skipped, so without it the render thread stalls 0.2-1.2 s (rain, Sheikah Slate)
		if (frame - it->second.adoptedFrame >= minAgeFrames && !pipelineStableCache.IsMustBeReady(it->first))
		{
			m_asyncPipelinesInFlight.erase(it->first);
			it = m_pendingGamePipelineRecipes.erase(it); // never bound, so no GPU reference
			++released;
		}
		else
			++it;
	}
	m_asyncPipelinesReleasedUnused += released;
	return released;
}

void D3D12Renderer::StopAsyncPipelineThreads()
{
	std::vector<std::thread> threads;
	{
		std::scoped_lock lock(m_asyncPipelineMutex);
		m_asyncPipelineStop = true;
		m_asyncPipelineJobs.clear();
		threads.swap(m_asyncPipelineThreads);
	}
	m_asyncPipelineCondition.notify_all();
	for (auto& thread : threads)
		thread.join();
}

void D3D12Renderer::AsyncPipelineThreadFunc()
{
	SetThreadName("d3d12AsyncPSO");
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
	while (true)
	{
		AsyncPipelineJob job;
		{
			std::unique_lock lock(m_asyncPipelineMutex);
			m_asyncPipelineCondition.wait(lock, [this]() { return m_asyncPipelineStop || !m_asyncPipelineJobs.empty(); });
			if (m_asyncPipelineStop)
				return;
			// above the memory guard a long queue gets one worker at a time: after a teleport 500-1200 queued PSOs kept
			// both workers busy and the game ran at ~24 FPS in light scenes until the queue drained (0.2.8.5 turned async
			// off instead). Short bursts (enemy deaths, drops, a new ability) keep both, or they pop in for seconds.
			constexpr size_t kGuardedQueueForOneWorker = 32;
			if (CemuUwp::MemoryBudgetService::IsHeavyWorkDeferred() && m_asyncPipelineJobs.size() > kGuardedQueueForOneWorker &&
				m_asyncPipelineJobsOutstanding.load(std::memory_order_acquire) != 0)
			{
				m_asyncPipelineCondition.wait_for(lock, std::chrono::milliseconds(20));
				continue;
			}
			job = std::move(m_asyncPipelineJobs.front());
			m_asyncPipelineJobs.pop_front();
			// counted before the lock drops, so the queue and this count never both read empty mid-job
			m_asyncPipelineJobsOutstanding.fetch_add(1, std::memory_order_acq_rel);
		}
		stdx::scope_exit jobDone([this]() { m_asyncPipelineJobsOutstanding.fetch_sub(1, std::memory_order_acq_rel); });
		// stale jobs are dropped; the render thread notices the generation change and queues again
		if (job.generation != m_gamePipelineRecipeGeneration.load(std::memory_order_acquire))
			continue;

		HRESULT hr = E_FAIL;
		Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline;
		D3D12PipelineRecipe::OwnedRecipe recipe;
		try
		{
			if (D3D12PipelineRecipe::Decode(job.recipe, recipe) == D3D12PipelineRecipe::Status::Success &&
				recipe.pipelineKey == job.pipelineKey)
			{
				recipe.BindRootSignature(job.rootSignature.Get());
				// the render thread is blocked on a pipeline of its own: do not queue ahead of it in the driver
				for (uint32 waitedMs = 0; m_renderThreadPipelineCreates.load(std::memory_order_acquire) != 0 && waitedMs < 2000; ++waitedMs)
					std::this_thread::sleep_for(std::chrono::milliseconds(1));
				const auto createStarted = std::chrono::steady_clock::now();
				hr = m_device->CreateGraphicsPipelineState(&recipe.desc, IID_PPV_ARGS(&pipeline));
				const uint64 createMs = (uint64)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - createStarted).count();
				if (createMs >= 500 && m_slowAsyncPipelineLogs.fetch_add(1, std::memory_order_relaxed) < 100)
				{
					size_t queuedJobs;
					{
						std::scoped_lock lock(m_asyncPipelineMutex);
						queuedJobs = m_asyncPipelineJobs.size();
					}
					cemuLog_log(LogType::Force, "D3D12: background PSO took {} ms (queued jobs {}).", createMs, queuedJobs);
				}
			}
		}
		catch (const std::exception&)
		{
			hr = E_OUTOFMEMORY;
		}

		bool delivered = false;
		if (SUCCEEDED(hr) && pipeline)
		{
			SetD3D12ObjectName(pipeline.Get(), L"Cemu D3D12 async game PSO");
			try
			{
				PrewarmedGamePipeline result;
				result.generation = job.generation;
				result.pipelineKey = job.pipelineKey;
				result.recipe.pipeline = std::move(pipeline);
				result.recipe.fingerprint = recipe.fingerprint;
				result.recipe.contentFingerprint = recipe.contentFingerprint;
				result.recipe.fromAsync = true;
				std::scoped_lock lock(m_prewarmedGamePipelinesMutex);
				m_prewarmedGamePipelines.emplace_back(std::move(result));
				m_prewarmedGamePipelinesAvailable.store(true, std::memory_order_release);
				delivered = true;
				m_asyncPipelinesCreated.fetch_add(1, std::memory_order_relaxed);
			}
			catch (const std::exception&)
			{
			}
		}
		if (!delivered)
		{
			// the synchronous path retries it and handles device loss or memory recovery
			std::scoped_lock lock(m_asyncPipelineMutex);
			m_asyncPipelineFailedKeys.push_back(job.pipelineKey);
		}
	}
}

bool D3D12Renderer::EnsureGamePipelineLibrary(uint64 titleId)
{
	if (!m_device || titleId == 0)
		return false;
	if (m_gamePipelineLibraryTitleId == titleId)
		return m_gamePipelineLibrary != nullptr;

	PersistGamePipelineLibrary();
	m_gamePipelineLibrary.Reset();
	m_gamePipelineLibraryBlob.clear();
	m_gamePipelineLibraryTitleId = titleId;
	m_gamePipelineLibraryDirty = false;
	if (IsXboxConsole())
	{
		// Native pipeline-library operations remove the graphics device on Xbox UWP.
		EnsureSinglePsoCacheSupport();
		cemuLog_logOnce(LogType::Force,
			"D3D12 UWP: native pipeline-cache APIs are unavailable on Xbox; using direct PSO creation plus Cemu's stable shader cache.");
		D3D12TraceEvent("game_pipeline_library_xbox_veto", "title=0x{:016x}", titleId);
		return false;
	}

	Microsoft::WRL::ComPtr<ID3D12Device1> device1;
	if (FAILED(m_device.As(&device1)))
		return false;

	D3D12_FEATURE_DATA_SHADER_CACHE shaderCacheSupport{};
	const HRESULT supportHr = m_device->CheckFeatureSupport(
		D3D12_FEATURE_SHADER_CACHE, &shaderCacheSupport, sizeof(shaderCacheSupport));
	if (IsD3D12DeviceFailure(supportHr))
	{
		CheckDeviceRemoved(supportHr, "CheckFeatureSupport(D3D12_FEATURE_SHADER_CACHE)");
		throw std::runtime_error("D3D12 device failed while querying native pipeline-library support");
	}
	if (FAILED(supportHr) ||
		(shaderCacheSupport.SupportFlags & D3D12_SHADER_CACHE_SUPPORT_LIBRARY) == 0)
	{
		cemuLog_logOnce(LogType::Force,
			"D3D12 UWP: native pipeline libraries are not supported by this device "
			"(HRESULT 0x{:08x}, support flags 0x{:08x}); using the regular PSO and stable shader caches.",
			(uint32)supportHr, (uint32)shaderCacheSupport.SupportFlags);
		D3D12TraceEvent("game_pipeline_library_unsupported", "title=0x{:016x} hr=0x{:08x} supportFlags=0x{:08x}",
			titleId, (uint32)supportHr, (uint32)shaderCacheSupport.SupportFlags);
		return false;
	}

	const fs::path path = ActiveSettings::GetCachePath("shaderCache/transferable/{:016x}_d3d12_native_pso.bin", titleId);
	std::ifstream input(path, std::ios::binary | std::ios::ate);
	if (input)
	{
		const std::streamoff size = input.tellg();
		constexpr std::streamoff kMaxPipelineLibraryBytes = 512ll * 1024ll * 1024ll;
		if (size > 0 && size <= kMaxPipelineLibraryBytes)
		{
			m_gamePipelineLibraryBlob.resize((size_t)size);
			input.seekg(0, std::ios::beg);
			if (!input.read(reinterpret_cast<char*>(m_gamePipelineLibraryBlob.data()), size))
				m_gamePipelineLibraryBlob.clear();
		}
	}
	HRESULT hr = device1->CreatePipelineLibrary(
		m_gamePipelineLibraryBlob.empty() ? nullptr : m_gamePipelineLibraryBlob.data(),
		m_gamePipelineLibraryBlob.size(), IID_PPV_ARGS(&m_gamePipelineLibrary));
	if (IsD3D12DeviceFailure(hr))
	{
		CheckDeviceRemoved(hr, "CreatePipelineLibrary(game)");
		m_gamePipelineLibrary.Reset();
		m_gamePipelineLibraryBlob.clear();
		throw std::runtime_error("D3D12 device failed while opening the native pipeline library");
	}
	if (FAILED(hr) && !m_gamePipelineLibraryBlob.empty() && IsStalePipelineLibraryError(hr))
	{
		// driver/runtime updates invalidate serialized libraries; start a fresh one
		m_gamePipelineLibrary.Reset();
		m_gamePipelineLibraryBlob.clear();
		hr = device1->CreatePipelineLibrary(nullptr, 0, IID_PPV_ARGS(&m_gamePipelineLibrary));
		if (IsD3D12DeviceFailure(hr))
		{
			CheckDeviceRemoved(hr, "CreatePipelineLibrary(game replacement)");
			m_gamePipelineLibrary.Reset();
			throw std::runtime_error("D3D12 device failed while replacing a stale native pipeline library");
		}
		m_gamePipelineLibraryDirty = SUCCEEDED(hr);
	}
	if (FAILED(hr))
	{
		m_gamePipelineLibrary.Reset();
		m_gamePipelineLibraryBlob.clear();
		cemuLog_logOnce(LogType::Force, "D3D12 UWP: native pipeline-library caching is unavailable (HRESULT 0x{:08x}).", (uint32)hr);
		return false;
	}

	D3D12TraceEvent("game_pipeline_library_open", "title=0x{:016x} serializedBytes={} staleReplaced={}",
		titleId, m_gamePipelineLibraryBlob.size(), m_gamePipelineLibraryDirty ? 1 : 0);
	return true;
}

void D3D12Renderer::PersistGamePipelineLibrary()
{
	if (!m_gamePipelineLibrary || !m_gamePipelineLibraryDirty || m_gamePipelineLibraryTitleId == 0)
		return;

	const SIZE_T size = m_gamePipelineLibrary->GetSerializedSize();
	if (size == 0 || size > 512ull * 1024ull * 1024ull)
		return;
	std::vector<uint8> serialized(size);
	const HRESULT hr = m_gamePipelineLibrary->Serialize(serialized.data(), serialized.size());
	if (FAILED(hr))
	{
		CheckDeviceRemoved(hr, "SerializePipelineLibrary(game)");
		cemuLog_log(LogType::Force, "D3D12 UWP: failed to serialize the native pipeline library (HRESULT 0x{:08x}).", (uint32)hr);
		return;
	}

	const fs::path path = ActiveSettings::GetCachePath("shaderCache/transferable/{:016x}_d3d12_native_pso.bin", m_gamePipelineLibraryTitleId);
	const fs::path temporaryPath = path.wstring() + L".tmp";
	std::error_code ec;
	fs::create_directories(path.parent_path(), ec);
	std::ofstream output(temporaryPath, std::ios::binary | std::ios::trunc);
	if (!output || !output.write(reinterpret_cast<const char*>(serialized.data()), serialized.size()))
	{
		cemuLog_log(LogType::Force, "D3D12 UWP: failed to write native pipeline library {}.", _pathToUtf8(path));
		return;
	}
	output.close();
	ec.clear();
	const bool destinationExists = fs::exists(path, ec);
	if (ec)
	{
		cemuLog_log(LogType::Force, "D3D12 UWP: failed to install native pipeline library {}: {}.", _pathToUtf8(path), ec.message());
		return;
	}

	const BOOL installed = destinationExists ?
		ReplaceFileFromAppW(path.c_str(), temporaryPath.c_str(), nullptr, 0, nullptr, nullptr) :
		MoveFileFromAppW(temporaryPath.c_str(), path.c_str());
	if (!installed)
	{
		const std::error_code installError((int)GetLastError(), std::system_category());
		cemuLog_log(LogType::Force, "D3D12 UWP: failed to atomically install native pipeline library {}: {}.",
			_pathToUtf8(path), installError.message());
		return;
	}
	m_gamePipelineLibraryDirty = false;
	D3D12TraceEvent("game_pipeline_library_saved", "title=0x{:016x} serializedBytes={} entries={}",
		m_gamePipelineLibraryTitleId, serialized.size(), m_gamePipelineLibraryStores);
}

bool D3D12Renderer::EnsureSinglePsoCacheSupport()
{
	if (m_singlePsoCacheSupportQueried)
		return m_singlePsoCacheSupported;
	m_singlePsoCacheSupportQueried = true;
	m_singlePsoCacheSupported = false;
	m_singlePsoCacheStoreEnabled = false;
	if (!m_device)
		return false;
	if (IsXboxConsole())
	{
		// GetCachedBlob removes the graphics device on Xbox despite advertised support.
		D3D12TraceEvent("game_pso_blob_xbox_veto", "reason=GetCachedBlob_device_removed");
		return false;
	}

	D3D12_FEATURE_DATA_SHADER_CACHE support{};
	const HRESULT hr = m_device->CheckFeatureSupport(D3D12_FEATURE_SHADER_CACHE, &support, sizeof(support));
	if (IsD3D12DeviceFailure(hr))
	{
		CheckDeviceRemoved(hr, "CheckFeatureSupport(D3D12_FEATURE_SHADER_CACHE single PSO)");
		throw std::runtime_error("D3D12 device failed while querying per-PSO cache support");
	}
	m_singlePsoCacheSupported = SUCCEEDED(hr) &&
		(support.SupportFlags & D3D12_SHADER_CACHE_SUPPORT_SINGLE_PSO) != 0;
	m_singlePsoCacheStoreEnabled = m_singlePsoCacheSupported;
	D3D12TraceEvent("game_pso_blob_support", "hr=0x{:08x} supportFlags=0x{:08x} enabled={}",
		(uint32)hr, (uint32)support.SupportFlags, m_singlePsoCacheSupported ? 1 : 0);
	return m_singlePsoCacheSupported;
}

uint64 D3D12Renderer::BuildGamePipelineFingerprint(uint64 pipelineKey, const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc) const
{
	uint64 fingerprint = MixGameDescriptorTableKey(0xd312ca5e9f501001ull, kD3D12GamePsoSchemaVersion);
	fingerprint = MixGameDescriptorTableKey(fingerprint, pipelineKey);
	const auto mixBytecode = [&fingerprint](const D3D12_SHADER_BYTECODE& bytecode)
	{
		fingerprint = MixGameDescriptorTableKey(fingerprint, bytecode.BytecodeLength);
		if (bytecode.pShaderBytecode && bytecode.BytecodeLength != 0)
			fingerprint = MixGameStateBytes(fingerprint, bytecode.pShaderBytecode, bytecode.BytecodeLength);
	};
	mixBytecode(desc.VS);
	mixBytecode(desc.GS);
	mixBytecode(desc.HS);
	mixBytecode(desc.DS);
	mixBytecode(desc.PS);
	return fingerprint;
}

bool D3D12Renderer::LoadGamePipelineBlob(uint64 pipelineKey, uint64 fingerprint, std::vector<uint8>& blob)
{
	blob.clear();
	if (!m_singlePsoCacheSupported || m_gamePipelineLibraryTitleId == 0)
		return false;

	const fs::path path = ActiveSettings::GetCachePath(
		"shaderCache/driver/d3d12/{:016x}/{:016x}_{:016x}.bin",
		m_gamePipelineLibraryTitleId, pipelineKey, fingerprint);
	std::ifstream input(path, std::ios::binary | std::ios::ate);
	if (!input)
	{
		m_singlePsoCacheMisses++;
		return false;
	}

	const std::streamoff fileSize = input.tellg();
	input.seekg(0, std::ios::beg);
	D3D12GamePsoBlobHeader header{};
	const bool headerRead = fileSize >= (std::streamoff)sizeof(header) &&
		!!input.read(reinterpret_cast<char*>(&header), sizeof(header));
	const bool valid = headerRead &&
		header.magic == kD3D12GamePsoBlobMagic &&
		header.version == kD3D12GamePsoBlobVersion &&
		header.headerSize == sizeof(header) &&
		header.titleId == m_gamePipelineLibraryTitleId &&
		header.pipelineKey == pipelineKey &&
		header.fingerprint == fingerprint &&
		header.adapterLuidLow == m_adapterLuid.LowPart &&
		header.adapterLuidHigh == m_adapterLuid.HighPart &&
		header.blobSize != 0 && header.blobSize <= kD3D12GamePsoBlobMaxBytes &&
		(uint64)fileSize == sizeof(header) + header.blobSize;
	if (!valid)
	{
		input.close();
		std::error_code ec;
		fs::remove(path, ec);
		m_singlePsoCacheRejects++;
		m_singlePsoCacheMisses++;
		return false;
	}

	blob.resize((size_t)header.blobSize);
	if (!input.read(reinterpret_cast<char*>(blob.data()), (std::streamsize)blob.size()))
	{
		blob.clear();
		input.close();
		std::error_code ec;
		fs::remove(path, ec);
		m_singlePsoCacheRejects++;
		m_singlePsoCacheMisses++;
		return false;
	}
	return true;
}

void D3D12Renderer::RemoveGamePipelineBlob(uint64 pipelineKey, uint64 fingerprint)
{
	if (m_gamePipelineLibraryTitleId == 0)
		return;
	const fs::path path = ActiveSettings::GetCachePath(
		"shaderCache/driver/d3d12/{:016x}/{:016x}_{:016x}.bin",
		m_gamePipelineLibraryTitleId, pipelineKey, fingerprint);
	std::error_code ec;
	fs::remove(path, ec);
}

void D3D12Renderer::StoreGamePipelineBlob(uint64 pipelineKey, uint64 fingerprint, ID3D12PipelineState* pipeline)
{
	if (!pipeline || !m_singlePsoCacheSupported || !m_singlePsoCacheStoreEnabled || m_gamePipelineLibraryTitleId == 0)
		return;

	Microsoft::WRL::ComPtr<ID3DBlob> cachedBlob;
	const HRESULT blobHr = pipeline->GetCachedBlob(&cachedBlob);
	if (IsD3D12DeviceFailure(blobHr))
	{
		CheckDeviceRemoved(blobHr, "GetCachedBlob(game)");
		throw std::runtime_error("D3D12 device failed while retrieving a cached game PSO");
	}
	if (FAILED(blobHr) || !cachedBlob || cachedBlob->GetBufferSize() == 0 ||
		cachedBlob->GetBufferSize() > kD3D12GamePsoBlobMaxBytes)
	{
		m_singlePsoCacheStoreEnabled = false;
		cemuLog_logOnce(LogType::Force,
			"D3D12 UWP: per-PSO driver blob persistence is unavailable for this session (HRESULT 0x{:08x}).",
			(uint32)blobHr);
		return;
	}

	D3D12GamePsoBlobHeader header{};
	header.magic = kD3D12GamePsoBlobMagic;
	header.version = kD3D12GamePsoBlobVersion;
	header.headerSize = sizeof(header);
	header.titleId = m_gamePipelineLibraryTitleId;
	header.pipelineKey = pipelineKey;
	header.fingerprint = fingerprint;
	header.adapterLuidLow = m_adapterLuid.LowPart;
	header.adapterLuidHigh = m_adapterLuid.HighPart;
	header.blobSize = cachedBlob->GetBufferSize();

	const fs::path path = ActiveSettings::GetCachePath(
		"shaderCache/driver/d3d12/{:016x}/{:016x}_{:016x}.bin",
		m_gamePipelineLibraryTitleId, pipelineKey, fingerprint);
	const fs::path temporaryPath = path.wstring() + L".tmp";
	std::error_code ec;
	fs::create_directories(path.parent_path(), ec);
	std::ofstream output(temporaryPath, std::ios::binary | std::ios::trunc);
	if (!output ||
		!output.write(reinterpret_cast<const char*>(&header), sizeof(header)) ||
		!output.write(reinterpret_cast<const char*>(cachedBlob->GetBufferPointer()), (std::streamsize)cachedBlob->GetBufferSize()))
	{
		output.close();
		fs::remove(temporaryPath, ec);
		return;
	}
	output.close();
	ec.clear();
	const bool destinationExists = fs::exists(path, ec);
	if (ec)
	{
		fs::remove(temporaryPath, ec);
		return;
	}
	const BOOL installed = destinationExists ?
		ReplaceFileFromAppW(path.c_str(), temporaryPath.c_str(), nullptr, 0, nullptr, nullptr) :
		MoveFileFromAppW(temporaryPath.c_str(), path.c_str());
	if (!installed)
	{
		fs::remove(temporaryPath, ec);
		return;
	}
	m_singlePsoCacheStores++;
}
#endif

ID3D12PipelineState* D3D12Renderer::GetGamePipeline(const D3D12PipelineCompiler::PipelineIdentity& identity,
													LatteDecompilerShader* vertexShader,
													LatteDecompilerShader* geometryShader,
													LatteDecompilerShader* pixelShader,
													Latte::LATTE_VGT_PRIMITIVE_TYPE::E_PRIMITIVE_TYPE primitiveMode,
											bool noRasterizationOutput,
											Renderer::INDEX_TYPE hostIndexType,
											StencilFallbackPass stencilFallbackPass)
{
	const auto logUnavailable = [this, &identity, vertexShader, geometryShader, pixelShader](uint32 reason, const char* message)
	{
		D3D12TraceEvent("game_pipeline_unavailable", "reason={} message={} identity={} stateHash=0x{:016x} rootSig={} fbo=0x{:016x} fboSize={}x{} colorCount={} colorMask=0x{:02x} hasDepth={} vs=0x{:016x} gs=0x{:016x} ps=0x{:016x}",
						reason,
						message,
						identity.valid ? 1 : 0,
						identity.valid ? identity.pipelineStateHash : 0,
						m_gameRootSignature ? 1 : 0,
						m_activeFBO ? m_activeFBO->key : 0,
						m_activeFBO ? m_activeFBO->m_size.x : 0,
						m_activeFBO ? m_activeFBO->m_size.y : 0,
						m_activeFBO ? m_activeFBO->GetColorAttachmentCount() : 0,
						m_activeFBO ? m_activeFBO->GetColorAttachmentMask() : 0,
						m_activeFBO && m_activeFBO->HasDepthStencilAttachment() ? 1 : 0,
						vertexShader ? vertexShader->baseHash : 0,
						geometryShader ? geometryShader->baseHash : 0,
						pixelShader ? pixelShader->baseHash : 0);
		uint64 signature = reason;
		signature = std::rotl(signature, 7) ^ (identity.valid ? identity.pipelineStateHash : 0);
		signature = std::rotl(signature, 7) ^ (m_gameRootSignature ? 0x10001ull : 0);
		signature = std::rotl(signature, 7) ^ (m_activeFBO ? 0x20002ull : 0);
		signature = std::rotl(signature, 7) ^ (vertexShader ? vertexShader->baseHash : 0);
		signature = std::rotl(signature, 7) ^ (geometryShader ? geometryShader->baseHash : 0);
		signature = std::rotl(signature, 7) ^ (pixelShader ? pixelShader->baseHash : 0);
		if (reason >= 2)
			m_gamePipelineShaderCodeMissing = true;
		if (!ShouldLogDrawFailure(signature))
			return;
		cemuLog_log(LogType::Force, "D3D12: game PSO unavailable: {} (identity={}, rootSig={}, fbo={}, vs={:016x}, gs={:016x}, ps={:016x}).",
					message,
					identity.valid ? "valid" : "invalid",
					m_gameRootSignature ? "yes" : "no",
					m_activeFBO ? "yes" : "no",
					vertexShader ? vertexShader->baseHash : 0,
					geometryShader ? geometryShader->baseHash : 0,
					pixelShader ? pixelShader->baseHash : 0);
	};

	if (!identity.valid || !m_gameRootSignature || (!m_activeFBO && !noRasterizationOutput) || !vertexShader)
	{
		logUnavailable(1, "state prerequisites are incomplete");
		return nullptr;
	}

	const bool usePointExpansionGS = !noRasterizationOutput && ShouldUsePointExpansionGeometryShader(geometryShader, primitiveMode);
	const bool rasterizesPointQuads = usePointExpansionGS || (geometryShader && LatteDecompiler_HLSLGeometryShaderExpandsPoints(RecordRegWords()));
	const bool useRectEmulationGS = !noRasterizationOutput && geometryShader == nullptr && primitiveMode == Latte::LATTE_VGT_PRIMITIVE_TYPE::E_PRIMITIVE_TYPE::RECTS;
	using LattePrimitive = Latte::LATTE_VGT_PRIMITIVE_TYPE::E_PRIMITIVE_TYPE;
	const bool usesFixedRestartTopology = primitiveMode == LattePrimitive::TRIANGLE_STRIP ||
								 primitiveMode == LattePrimitive::TRIANGLE_STRIP_ADJACENT ||
								 primitiveMode == LattePrimitive::LINE_STRIP ||
								 primitiveMode == LattePrimitive::LINE_LOOP ||
								 primitiveMode == LattePrimitive::LINE_STRIP_ADJACENT ||
								 primitiveMode == LattePrimitive::TRIANGLE_FAN;
	const bool primitiveRestartEnabled = GetD3D12PrimitiveRestartEnabled(RecordRegs(), primitiveMode, hostIndexType);
	const uint32 primitiveRestartIndex = RecordRegs().VGT_MULTI_PRIM_IB_RESET_INDX.get_RESTART_INDEX();
	const bool hostStreamContainsRestartCuts = hostIndexType == Renderer::INDEX_TYPE::U32 ||
		(hostIndexType == Renderer::INDEX_TYPE::U16 && primitiveRestartIndex <= std::numeric_limits<uint16>::max());
	D3D12_INDEX_BUFFER_STRIP_CUT_VALUE stripCutValue = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;
	if (primitiveRestartEnabled && usesFixedRestartTopology && hostStreamContainsRestartCuts && hostIndexType == Renderer::INDEX_TYPE::U16)
		stripCutValue = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFF;
	else if (primitiveRestartEnabled && usesFixedRestartTopology && hostStreamContainsRestartCuts && hostIndexType == Renderer::INDEX_TYPE::U32)
		stripCutValue = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFFFFFF;
	const auto derivePipelineKey = [&](uint64 key)
	{
		if (stripCutValue != D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED)
			key = std::rotl(key, 13) ^ (0xD312C071ull + (uint64)stripCutValue);
		if (noRasterizationOutput)
			key = std::rotl(key, 11) ^ 0xD3125EEDull;
		if (stencilFallbackPass != StencilFallbackPass::None)
			key = std::rotl(key, 13) ^ (0xD31257E0C1000000ull + static_cast<uint64>(stencilFallbackPass));
		if (usePointExpansionGS)
		{
			key = std::rotl(key, 9) ^ 0xD3129017ull;
			key = std::rotl(key, 7) ^ vertexShader->outputParameterMask;
			key = std::rotl(key, 7) ^ (pixelShader ? pixelShader->baseHash : 0);
		}
		if (useRectEmulationGS)
		{
			key = std::rotl(key, 9) ^ 0xD3120EC7ull;
			key = std::rotl(key, 7) ^ vertexShader->outputParameterMask;
			key = std::rotl(key, 7) ^ (pixelShader ? pixelShader->baseHash : 0);
		}

		return key;
	};
	const uint64 pipelineKey = derivePipelineKey(identity.pipelineStateHash);
	// a ready pipeline differing only in baked depth bias stands in while the exact one is built
	const bool hasBakedDepthBias = RecordRegs().PA_SU_SC_MODE_CNTL.get_OFFSET_FRONT_ENABLED();
	const auto depthBiasSiblingKey = [&]()
	{
		return derivePipelineKey(D3D12PipelineCompiler::CalculateGraphicsPipelineHash(s_recordFetchShader,
			vertexShader, geometryShader, pixelShader, m_activeFBO, RecordRegs(), true));
	};
	const float drawBiasSlope = RecordRegs().PA_SU_POLY_OFFSET_FRONT_SCALE.get_SCALE();
	const float drawBiasOffset = RecordRegs().PA_SU_POLY_OFFSET_FRONT_OFFSET.get_OFFSET();
	const float drawBiasClamp = RecordRegs().PA_SU_POLY_OFFSET_CLAMP.get_CLAMP();
	// BOTW drifts its shadow bias every frame; within 1/16 (offset: or one depth unit) the difference is invisible
	const auto biasClose = [](float a, float b, float absoluteSlack)
	{
		return std::fabs(a - b) <= std::max(std::max(std::fabs(a), std::fabs(b)) * (1.0f / 16.0f), absoluteSlack);
	};
	const auto findDepthBiasSibling = [&](bool requireClose = false) -> ID3D12PipelineState*
	{
		if (!hasBakedDepthBias)
			return nullptr;
		const auto siblingIt = m_depthBiasSiblingPipelines.find(depthBiasSiblingKey());
		if (siblingIt == m_depthBiasSiblingPipelines.end())
			return nullptr;
		const auto& sibling = siblingIt->second;
		if (requireClose && !(biasClose(sibling.slope, drawBiasSlope, 0.0f) && biasClose(sibling.offset, drawBiasOffset, 1.0f) && sibling.clamp == drawBiasClamp))
			return nullptr;
		const auto pipelineIt = m_gamePipelineCache.find(sibling.pipelineKey);
		if (pipelineIt == m_gamePipelineCache.end() || !pipelineIt->second.pipeline)
			return nullptr; // evicted or failed
		pipelineIt->second.lastUsedFrame = LatteGPUState.frameCounter;
		if (!requireClose)
			++m_depthBiasSiblingDraws;
		return pipelineIt->second.pipeline.Get();
	};

	auto it = m_gamePipelineCache.find(pipelineKey);
	if (it != m_gamePipelineCache.end())
	{
		D3D12TraceEvent("game_pipeline_cache_hit", "pipelineKey=0x{:016x} stateHash=0x{:016x} primitive={} noRaster={} pointGS={} rectGS={} fbo=0x{:016x} vs=0x{:016x} gs=0x{:016x} ps=0x{:016x} pipeline={}",
						pipelineKey,
						identity.pipelineStateHash,
						(uint32)primitiveMode,
						noRasterizationOutput ? 1 : 0,
						usePointExpansionGS ? 1 : 0,
						useRectEmulationGS ? 1 : 0,
						m_activeFBO ? m_activeFBO->key : 0,
						vertexShader ? vertexShader->baseHash : 0,
						geometryShader ? geometryShader->baseHash : 0,
						pixelShader ? pixelShader->baseHash : 0,
						(const void*)it->second.pipeline.Get());
		it->second.lastUsedFrame = LatteGPUState.frameCounter;
		return it->second.pipeline.Get();
	}
	if (auto retryIt = m_gamePipelineRetryFrame.find(pipelineKey); retryIt != m_gamePipelineRetryFrame.end())
	{
		if (static_cast<sint32>(LatteGPUState.frameCounter - retryIt->second) < 0)
			return nullptr;
		m_gamePipelineRetryFrame.erase(retryIt);
	}

	auto* d3dVertexShader = static_cast<RendererShaderD3D12*>(vertexShader->shader);
	auto* d3dGeometryShader = geometryShader ? static_cast<RendererShaderD3D12*>(geometryShader->shader) : nullptr;
	auto* d3dPointExpansionShader = usePointExpansionGS ? GetPointExpansionGeometryShader(vertexShader, pixelShader) : nullptr;
	auto* d3dRectEmulationShader = useRectEmulationGS ? GetRectEmulationGeometryShader(vertexShader, pixelShader) : nullptr;
	auto* d3dPixelShader = pixelShader ? static_cast<RendererShaderD3D12*>(pixelShader->shader) : nullptr;
	if (!d3dVertexShader)
	{
		logUnavailable(2, "vertex renderer shader is missing");
		return nullptr;
	}
	d3dVertexShader->RequestCompilationPriority();
	if (d3dGeometryShader)
		d3dGeometryShader->RequestCompilationPriority();
	if (d3dPointExpansionShader)
		d3dPointExpansionShader->RequestCompilationPriority();
	if (d3dRectEmulationShader)
		d3dRectEmulationShader->RequestCompilationPriority();
	if (d3dPixelShader && !noRasterizationOutput)
		d3dPixelShader->RequestCompilationPriority();

	// a ready pipeline with a near-identical bias serves this key for good: no build, no recipe
	if (auto* closeSibling = !m_pendingGamePipelineRecipes.contains(pipelineKey) ? findDepthBiasSibling(true) : nullptr)
	{
		m_gamePipelineCache.emplace(pipelineKey, GamePipelineEntry{ Microsoft::WRL::ComPtr<ID3D12PipelineState>(closeSibling), LatteGPUState.frameCounter });
		++m_depthBiasAliases;
		return closeSibling;
	}

	if (m_allowAsyncShaderSkip)
	{
		const auto pending = [](RendererShaderD3D12* shader) { return shader && !shader->IsCompiled(); };
		if (pending(d3dVertexShader) || pending(d3dGeometryShader) || pending(d3dPointExpansionShader) ||
			pending(d3dRectEmulationShader) || (!noRasterizationOutput && pending(d3dPixelShader)))
		{
			m_gamePipelineAsyncPending = true;
			return nullptr;
		}
		if (IsAsyncGamePipelinePending(pipelineKey))
		{
			if (auto* sibling = findDepthBiasSibling())
				return sibling;
			m_gamePipelineAsyncPending = true;
			return nullptr;
		}
	}

	if (!d3dVertexShader->WaitForCompiled() || !d3dVertexShader->HasDXIL())
	{
		logUnavailable(3, "vertex DXIL is missing");
		return nullptr;
	}
	if (d3dGeometryShader && (!d3dGeometryShader->WaitForCompiled() || !d3dGeometryShader->HasDXIL()))
	{
		logUnavailable(4, "geometry DXIL is missing");
		return nullptr;
	}
	if (usePointExpansionGS && (!d3dPointExpansionShader || !d3dPointExpansionShader->WaitForCompiled() || !d3dPointExpansionShader->HasDXIL()))
	{
		logUnavailable(6, "point expansion geometry DXIL is missing");
		return nullptr;
	}
	if (useRectEmulationGS && (!d3dRectEmulationShader || !d3dRectEmulationShader->WaitForCompiled() || !d3dRectEmulationShader->HasDXIL()))
	{
		logUnavailable(7, "rectangle emulation geometry DXIL is missing");
		return nullptr;
	}
	if (d3dPixelShader && !noRasterizationOutput && (!d3dPixelShader->WaitForCompiled() || !d3dPixelShader->HasDXIL()))
	{
		logUnavailable(5, "pixel DXIL is missing");
		return nullptr;
	}

	D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
	desc.pRootSignature = m_gameRootSignature.Get();

	const auto& latteRegister = RecordRegs();
	const auto& colorControlReg = latteRegister.CB_COLOR_CONTROL;
	const uint32 blendEnableMask = colorControlReg.get_BLEND_MASK();
	const uint32 rawRenderTargetMask = latteRegister.CB_TARGET_MASK.get_MASK();
	const uint32 renderTargetMask = GetD3D12EffectiveRenderTargetMask(latteRegister);
	const bool logicOpRequested = colorControlReg.get_ROP() != Latte::LATTE_CB_COLOR_CONTROL::E_LOGICOP::COPY;
	bool logicOpEnable = !noRasterizationOutput && m_outputMergerLogicOpSupported && logicOpRequested &&
		m_logicOpEmulationMode == 0 && !m_disableNativeLogicOpForDraw;
	D3D12_LOGIC_OP d3dLogicOp = D3D12_LOGIC_OP_COPY;
	const bool hasExactD3D12LogicOp = TryGetD3D12LogicOp(colorControlReg.get_ROP(), d3dLogicOp);
	logicOpEnable = logicOpEnable && hasExactD3D12LogicOp;
	if (logicOpEnable)
	{
		const auto& colorAttachments = m_activeFBO->GetColorAttachments();
		const uint32 pixelOutputMask = pixelShader ? pixelShader->pixelColorOutputMask : 0u;
		for (uint32 i = 0; i < m_activeFBO->GetColorAttachmentCount() && i < colorAttachments.size(); ++i)
		{
			if (!colorAttachments[i].valid || ((renderTargetMask >> (i * 4)) & 0xF) == 0)
				continue;
			if ((pixelOutputMask & (1u << i)) == 0)
				continue;
			if (!FormatSupportsOutputMergerLogicOp(colorAttachments[i].format))
			{
				logicOpEnable = false;
				break;
			}
		}
	}
	desc.BlendState.AlphaToCoverageEnable = FALSE;
	// D3D12 rejects logic ops with IndependentBlendEnable set
	desc.BlendState.IndependentBlendEnable = (logicOpEnable || noRasterizationOutput) ? FALSE : TRUE;
	for (auto& rtBlend : desc.BlendState.RenderTarget)
	{
		rtBlend.BlendEnable = FALSE;
		rtBlend.LogicOpEnable = logicOpEnable;
		rtBlend.SrcBlend = D3D12_BLEND_ONE;
		rtBlend.DestBlend = D3D12_BLEND_ZERO;
		rtBlend.BlendOp = D3D12_BLEND_OP_ADD;
		rtBlend.SrcBlendAlpha = D3D12_BLEND_ONE;
		rtBlend.DestBlendAlpha = D3D12_BLEND_ZERO;
		rtBlend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
		rtBlend.LogicOp = D3D12_LOGIC_OP_COPY;
		rtBlend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
	}
	if (hasExactD3D12LogicOp)
	{
		for (auto& rtBlend : desc.BlendState.RenderTarget)
			rtBlend.LogicOp = d3dLogicOp;
	}
	static uint32 s_unsupportedRtBlendLogCount = 0;
	for (uint32 i = 0; i < std::size(desc.BlendState.RenderTarget); ++i)
	{
		auto& rtBlend = desc.BlendState.RenderTarget[i];
		const auto& blendControlReg = latteRegister.CB_BLENDN_CONTROL[i];
		const bool rtSlotActive =
			!noRasterizationOutput &&
			m_activeFBO &&
			i < m_activeFBO->GetColorAttachmentCount() &&
			i < m_activeFBO->GetColorAttachments().size() &&
			m_activeFBO->GetColorAttachments()[i].valid;
		const DXGI_FORMAT rtFormat = rtSlotActive ? m_activeFBO->GetColorAttachments()[i].format : DXGI_FORMAT_UNKNOWN;
		const bool integerRenderTarget = rtSlotActive && IsD3D12IntegerFormat(rtFormat);
		const bool blendRequested = rtSlotActive && !integerRenderTarget && !logicOpEnable &&
			m_logicOpEmulationMode == 0 && !m_disableNativeLogicOpForDraw && ((blendEnableMask & (1u << i)) != 0);
		const bool blendSupported = !blendRequested || FormatSupportsBlending(rtFormat);
		rtBlend.BlendEnable = blendRequested && blendSupported;
		rtBlend.SrcBlend = GetD3D12ColorBlendFactor(blendControlReg.get_COLOR_SRCBLEND(), m_alphaBlendFactorSupported);
		rtBlend.DestBlend = GetD3D12ColorBlendFactor(blendControlReg.get_COLOR_DSTBLEND(), m_alphaBlendFactorSupported);
		rtBlend.BlendOp = GetD3D12BlendOp(blendControlReg.get_COLOR_COMB_FCN());
		if (blendControlReg.get_SEPARATE_ALPHA_BLEND())
		{
			rtBlend.SrcBlendAlpha = GetD3D12AlphaBlendFactor(blendControlReg.get_ALPHA_SRCBLEND());
			rtBlend.DestBlendAlpha = GetD3D12AlphaBlendFactor(blendControlReg.get_ALPHA_DSTBLEND());
			rtBlend.BlendOpAlpha = GetD3D12BlendOp(blendControlReg.get_ALPHA_COMB_FCN());
		}
		else
		{
			rtBlend.SrcBlendAlpha = GetD3D12AlphaBlendFactor(blendControlReg.get_COLOR_SRCBLEND());
			rtBlend.DestBlendAlpha = GetD3D12AlphaBlendFactor(blendControlReg.get_COLOR_DSTBLEND());
			rtBlend.BlendOpAlpha = rtBlend.BlendOp;
		}
		const bool shaderWritesTarget = pixelShader && (pixelShader->pixelColorOutputMask & (1u << i)) != 0;
		rtBlend.RenderTargetWriteMask = noRasterizationOutput || !shaderWritesTarget ?
			0 : (UINT8)((renderTargetMask >> (i * 4)) & 0xF);
		if (blendRequested && !blendSupported)
		{
#if defined(CEMU_UWP_D3D12_CORRECTNESS_TELEMETRY)
			++m_correctnessUnsupportedBlendStates;
#endif
			if (s_unsupportedRtBlendLogCount < 16)
			{
				cemuLog_log(LogType::Force,
							"D3D12: disabling unsupported native RT blend for PSO 0x{:016x} rt={} format={} src={} dst={} op={} srcA={} dstA={} opA={}.",
							identity.pipelineStateHash,
							i,
							(uint32)rtFormat,
							(uint32)rtBlend.SrcBlend,
							(uint32)rtBlend.DestBlend,
							(uint32)rtBlend.BlendOp,
							(uint32)rtBlend.SrcBlendAlpha,
							(uint32)rtBlend.DestBlendAlpha,
							(uint32)rtBlend.BlendOpAlpha);
			}
			else if (s_unsupportedRtBlendLogCount == 16)
				cemuLog_log(LogType::Force, "D3D12: suppressing repeated unsupported native RT blend logs.");
			s_unsupportedRtBlendLogCount++;
			D3D12TraceEvent("game_pipeline_unsupported_rt_blend",
							"pipelineKey=0x{:016x} stateHash=0x{:016x} rt={} format={} src={} dst={} op={} srcA={} dstA={} opA={}",
							pipelineKey,
							identity.pipelineStateHash,
							i,
							(uint32)rtFormat,
							(uint32)rtBlend.SrcBlend,
							(uint32)rtBlend.DestBlend,
							(uint32)rtBlend.BlendOp,
							(uint32)rtBlend.SrcBlendAlpha,
							(uint32)rtBlend.DestBlendAlpha,
							(uint32)rtBlend.BlendOpAlpha);
		}
	}
	desc.SampleMask = UINT_MAX;

	const auto& polygonControlReg = latteRegister.PA_SU_SC_MODE_CNTL;
	desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
	const auto frontFace = polygonControlReg.get_FRONT_FACE();
	const auto cullState = GetD3D12EffectiveCullState(primitiveMode, polygonControlReg);
	auto selectedStencilState = GetD3D12SelectedStencilState(latteRegister, GetD3D12StencilFaceCullState(cullState, rasterizesPointQuads));
	const bool stencilFrontPass =
		stencilFallbackPass == StencilFallbackPass::Front ||
		stencilFallbackPass == StencilFallbackPass::FrontFlipped;
	const bool stencilBackPass =
		stencilFallbackPass == StencilFallbackPass::Back ||
		stencilFallbackPass == StencilFallbackPass::BackFlipped;
	const bool flipFallbackWinding =
		stencilFallbackPass == StencilFallbackPass::WindingFlipped ||
		stencilFallbackPass == StencilFallbackPass::FrontFlipped ||
		stencilFallbackPass == StencilFallbackPass::BackFlipped;
	if (stencilFrontPass)
	{
		selectedStencilState.readMask = selectedStencilState.frontReadMask;
		selectedStencilState.writeMask = selectedStencilState.frontWriteMask;
		selectedStencilState.ref = selectedStencilState.frontRef;
		selectedStencilState.usedBackFace = false;
	}
	else if (stencilBackPass)
	{
		selectedStencilState.readMask = selectedStencilState.backReadMask;
		selectedStencilState.writeMask = selectedStencilState.backWriteMask;
		selectedStencilState.ref = selectedStencilState.backRef;
		selectedStencilState.usedBackFace = true;
	}
	if (stencilFrontPass)
		desc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
	else if (stencilBackPass)
		desc.RasterizerState.CullMode = D3D12_CULL_MODE_FRONT;
	else if (rasterizesPointQuads)
		desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
	else if (cullState.cullFront && cullState.cullBack)
	{
		cemuLog_logOnce(LogType::Force, "D3D12: front-and-back culling skips non-streamout draws; streamout draws use the no-raster pipeline variant.");
		desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
	}
	else if (cullState.cullFront)
		desc.RasterizerState.CullMode = D3D12_CULL_MODE_FRONT;
	else if (cullState.cullBack)
		desc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
	else
		desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
	bool frontCounterClockwise = D3D12FrontCounterClockwise(frontFace);
	// Fan-to-strip conversion reverses triangle orientation; compensate in the PSO.
	if (flipFallbackWinding)
		frontCounterClockwise = !frontCounterClockwise;
	// the point expansion shader emits clockwise quads
	if (rasterizesPointQuads)
		frontCounterClockwise = false;
	desc.RasterizerState.FrontCounterClockwise = frontCounterClockwise;
	const bool dynamicDepthBias = UsesDynamicDepthBias(rasterizesPointQuads);
#if defined(__ID3D12GraphicsCommandList9_INTERFACE_DEFINED__)
	if (dynamicDepthBias)
		desc.Flags |= D3D12_PIPELINE_STATE_FLAG_DYNAMIC_DEPTH_BIAS;
#endif
	// polygon offset does not apply to points
	if (polygonControlReg.get_OFFSET_FRONT_ENABLED() && !rasterizesPointQuads && !dynamicDepthBias)
	{
		// quantised exactly like the pipeline key (see D3D12PipelineCompiler::QuantizeDepthBias)
		const float frontScale = D3D12PipelineCompiler::QuantizeDepthBias(latteRegister.PA_SU_POLY_OFFSET_FRONT_SCALE.get_SCALE()) / 16.0f;
		const float frontOffset = latteRegister.PA_SU_POLY_OFFSET_FRONT_OFFSET.get_OFFSET();
		const float offsetClamp = D3D12PipelineCompiler::QuantizeDepthBias(latteRegister.PA_SU_POLY_OFFSET_CLAMP.get_CLAMP());
		desc.RasterizerState.DepthBias = (INT)std::clamp<float>(std::round(frontOffset), (float)std::numeric_limits<INT>::min(), (float)std::numeric_limits<INT>::max());
		desc.RasterizerState.DepthBiasClamp = offsetClamp;
		desc.RasterizerState.SlopeScaledDepthBias = frontScale;
	}
	else
	{
		desc.RasterizerState.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
		desc.RasterizerState.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
		desc.RasterizerState.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
	}
	desc.RasterizerState.DepthClipEnable =
		!latteRegister.PA_CL_CLIP_CNTL.get_ZCLIP_NEAR_DISABLE() &&
		!latteRegister.PA_CL_CLIP_CNTL.get_ZCLIP_FAR_DISABLE();
	desc.RasterizerState.MultisampleEnable = FALSE;
	desc.RasterizerState.AntialiasedLineEnable = FALSE;
	desc.RasterizerState.ForcedSampleCount = 0;
	desc.RasterizerState.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;

	const bool hasDepth = !noRasterizationOutput && m_activeFBO->HasDepthStencilAttachment();
	const auto& depthControlReg = latteRegister.DB_DEPTH_CONTROL;
	desc.DepthStencilState.DepthEnable = hasDepth && depthControlReg.get_Z_ENABLE();
	desc.DepthStencilState.DepthWriteMask = hasDepth && depthControlReg.get_Z_WRITE_ENABLE() ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
	desc.DepthStencilState.DepthFunc = GetD3D12CompareFunc(depthControlReg.get_Z_FUNC());
	const bool hasStencil = hasDepth && m_activeFBO->GetStencilFormat() != DXGI_FORMAT_UNKNOWN;
	desc.DepthStencilState.StencilEnable = hasStencil && depthControlReg.get_STENCIL_ENABLE();
	desc.DepthStencilState.StencilReadMask = selectedStencilState.readMask;
	desc.DepthStencilState.StencilWriteMask = selectedStencilState.writeMask;
	desc.DepthStencilState.FrontFace.StencilFunc = GetD3D12CompareFunc(depthControlReg.get_STENCIL_FUNC_F());
	desc.DepthStencilState.FrontFace.StencilFailOp = GetD3D12StencilOp(depthControlReg.get_STENCIL_FAIL_F());
	desc.DepthStencilState.FrontFace.StencilDepthFailOp = GetD3D12StencilOp(depthControlReg.get_STENCIL_ZFAIL_F());
	desc.DepthStencilState.FrontFace.StencilPassOp = GetD3D12StencilOp(depthControlReg.get_STENCIL_ZPASS_F());
	if (depthControlReg.get_BACK_STENCIL_ENABLE())
	{
		desc.DepthStencilState.BackFace.StencilFunc = GetD3D12CompareFunc(depthControlReg.get_STENCIL_FUNC_B());
		desc.DepthStencilState.BackFace.StencilFailOp = GetD3D12StencilOp(depthControlReg.get_STENCIL_FAIL_B());
		desc.DepthStencilState.BackFace.StencilDepthFailOp = GetD3D12StencilOp(depthControlReg.get_STENCIL_ZFAIL_B());
		desc.DepthStencilState.BackFace.StencilPassOp = GetD3D12StencilOp(depthControlReg.get_STENCIL_ZPASS_B());
	}
	else
		desc.DepthStencilState.BackFace = desc.DepthStencilState.FrontFace;

#if defined(__ID3D12GraphicsCommandList8_INTERFACE_DEFINED__)
	const bool needsIndependentStencilState =
		desc.DepthStencilState.StencilEnable &&
		!cullState.cullFront && !cullState.cullBack &&
		!stencilFrontPass && !stencilBackPass &&
		selectedStencilState.needsIndependentFrontBackState;
	const bool useIndependentStencilMasks =
		needsIndependentStencilState &&
		m_independentFrontBackStencilSupported &&
		(selectedStencilState.frontReadMask != selectedStencilState.backReadMask ||
		 selectedStencilState.frontWriteMask != selectedStencilState.backWriteMask);
	D3D12_DEPTH_STENCIL_DESC2 independentDepthStencil{};
	if (useIndependentStencilMasks)
	{
		independentDepthStencil.DepthEnable = desc.DepthStencilState.DepthEnable;
		independentDepthStencil.DepthWriteMask = desc.DepthStencilState.DepthWriteMask;
		independentDepthStencil.DepthFunc = desc.DepthStencilState.DepthFunc;
		independentDepthStencil.StencilEnable = desc.DepthStencilState.StencilEnable;
		independentDepthStencil.FrontFace = {
			desc.DepthStencilState.FrontFace.StencilFailOp,
			desc.DepthStencilState.FrontFace.StencilDepthFailOp,
			desc.DepthStencilState.FrontFace.StencilPassOp,
			desc.DepthStencilState.FrontFace.StencilFunc,
			selectedStencilState.frontReadMask,
			selectedStencilState.frontWriteMask,
		};
		independentDepthStencil.BackFace = {
			desc.DepthStencilState.BackFace.StencilFailOp,
			desc.DepthStencilState.BackFace.StencilDepthFailOp,
			desc.DepthStencilState.BackFace.StencilPassOp,
			desc.DepthStencilState.BackFace.StencilFunc,
			selectedStencilState.backReadMask,
			selectedStencilState.backWriteMask,
		};
		independentDepthStencil.DepthBoundsTestEnable = FALSE;
	}
#endif

	desc.InputLayout = {};
	desc.PrimitiveTopologyType = D3D12PipelineCompiler::GetPrimitiveTopologyType(primitiveMode);
	desc.IBStripCutValue = stripCutValue;
	desc.NumRenderTargets = noRasterizationOutput ? 0 : std::min<UINT>(m_activeFBO->GetColorAttachmentCount(), 8);
	if (!noRasterizationOutput)
	{
		const auto& colorAttachments = m_activeFBO->GetColorAttachments();
		for (uint32 i = 0; i < desc.NumRenderTargets && i < colorAttachments.size(); ++i)
		{
			desc.RTVFormats[i] = colorAttachments[i].valid ? colorAttachments[i].format : DXGI_FORMAT_R8G8B8A8_UNORM;
			if (!colorAttachments[i].valid)
				desc.BlendState.RenderTarget[i].RenderTargetWriteMask = 0;
		}
	}
	desc.DSVFormat = hasDepth ? m_activeFBO->GetDepthFormat() : DXGI_FORMAT_UNKNOWN;
	desc.SampleDesc.Count = 1;

	desc.VS ={ d3dVertexShader->GetDXIL().data(), d3dVertexShader->GetDXIL().size() };
	if (d3dGeometryShader)
		desc.GS = { d3dGeometryShader->GetDXIL().data(), d3dGeometryShader->GetDXIL().size() };
	else if (d3dPointExpansionShader)
		desc.GS = { d3dPointExpansionShader->GetDXIL().data(), d3dPointExpansionShader->GetDXIL().size() };
	else if (d3dRectEmulationShader)
		desc.GS = { d3dRectEmulationShader->GetDXIL().data(), d3dRectEmulationShader->GetDXIL().size() };
	if (d3dPixelShader && !noRasterizationOutput)
		desc.PS = { d3dPixelShader->GetDXIL().data(), d3dPixelShader->GetDXIL().size() };

	D3D12TraceEvent("game_pipeline_create_begin", "pipelineKey=0x{:016x} stateHash=0x{:016x} primitive={} topologyType={} noRaster={} pointGS={} rectGS={} fbo=0x{:016x} fboSize={}x{} colorCount={} colorMask=0x{:02x} drawBuffersMask=0x{:08x} rt0={} rt1={} rt2={} rt3={} rt4={} rt5={} rt6={} rt7={} dsv={} depthEnable={} depthWrite={} depthFunc={} latteZEnable={} latteZWrite={} latteZFunc={} stencilEnable={} stencilFuncF={} stencilFuncB={} stencilRef={} stencilReadMask=0x{:02x} stencilWriteMask=0x{:02x} stencilBackSelected={} stencilTwoSidedStateMismatch={} cullMode={} frontCCW={} latteFrontFace={} latteCullFront={} latteCullBack={} depthClip={} zClipNearDisable={} zClipFarDisable={} rasterKill={} depthBias={} slopeBias={} logicOp={} independentBlend={} blendMask=0x{:02x} targetMask=0x{:08x} rawTargetMask=0x{:08x} colorSpecialOp={} alphaTest={} alphaFunc={} vs=0x{:016x} gs=0x{:016x} ps=0x{:016x} vsBytes={} gsBytes={} psBytes={}",
					pipelineKey,
					identity.pipelineStateHash,
					(uint32)primitiveMode,
					(uint32)desc.PrimitiveTopologyType,
					noRasterizationOutput ? 1 : 0,
					usePointExpansionGS ? 1 : 0,
					useRectEmulationGS ? 1 : 0,
					m_activeFBO ? m_activeFBO->key : 0,
					m_activeFBO ? m_activeFBO->m_size.x : 0,
					m_activeFBO ? m_activeFBO->m_size.y : 0,
					m_activeFBO ? m_activeFBO->GetColorAttachmentCount() : 0,
					m_activeFBO ? m_activeFBO->GetColorAttachmentMask() : 0,
					m_activeFBO ? m_activeFBO->drawBuffersMask : 0,
					(uint32)desc.RTVFormats[0],
					(uint32)desc.RTVFormats[1],
					(uint32)desc.RTVFormats[2],
					(uint32)desc.RTVFormats[3],
					(uint32)desc.RTVFormats[4],
					(uint32)desc.RTVFormats[5],
					(uint32)desc.RTVFormats[6],
					(uint32)desc.RTVFormats[7],
					(uint32)desc.DSVFormat,
					desc.DepthStencilState.DepthEnable ? 1 : 0,
					desc.DepthStencilState.DepthWriteMask == D3D12_DEPTH_WRITE_MASK_ALL ? 1 : 0,
					(uint32)desc.DepthStencilState.DepthFunc,
					depthControlReg.get_Z_ENABLE() ? 1 : 0,
					depthControlReg.get_Z_WRITE_ENABLE() ? 1 : 0,
					(uint32)depthControlReg.get_Z_FUNC(),
					desc.DepthStencilState.StencilEnable ? 1 : 0,
					(uint32)depthControlReg.get_STENCIL_FUNC_F(),
					(uint32)depthControlReg.get_STENCIL_FUNC_B(),
					selectedStencilState.ref,
					(uint32)selectedStencilState.readMask,
					(uint32)selectedStencilState.writeMask,
					selectedStencilState.usedBackFace ? 1 : 0,
					selectedStencilState.needsIndependentFrontBackState ? 1 : 0,
					(uint32)desc.RasterizerState.CullMode,
					desc.RasterizerState.FrontCounterClockwise ? 1 : 0,
					(uint32)frontFace,
					polygonControlReg.get_CULL_FRONT() ? 1 : 0,
					polygonControlReg.get_CULL_BACK() ? 1 : 0,
					desc.RasterizerState.DepthClipEnable ? 1 : 0,
					latteRegister.PA_CL_CLIP_CNTL.get_ZCLIP_NEAR_DISABLE() ? 1 : 0,
					latteRegister.PA_CL_CLIP_CNTL.get_ZCLIP_FAR_DISABLE() ? 1 : 0,
					latteRegister.PA_CL_CLIP_CNTL.get_DX_RASTERIZATION_KILL() ? 1 : 0,
					desc.RasterizerState.DepthBias,
					desc.RasterizerState.SlopeScaledDepthBias,
					logicOpEnable ? 1 : 0,
					desc.BlendState.IndependentBlendEnable ? 1 : 0,
					blendEnableMask,
					renderTargetMask,
					rawRenderTargetMask,
					(uint32)colorControlReg.get_SPECIAL_OP(),
					latteRegister.SX_ALPHA_TEST_CONTROL.get_ALPHA_TEST_ENABLE() ? 1 : 0,
					(uint32)latteRegister.SX_ALPHA_TEST_CONTROL.get_ALPHA_FUNC(),
					vertexShader ? vertexShader->baseHash : 0,
					geometryShader ? geometryShader->baseHash : 0,
					pixelShader ? pixelShader->baseHash : 0,
					(unsigned long long)desc.VS.BytecodeLength,
					(unsigned long long)desc.GS.BytecodeLength,
					(unsigned long long)desc.PS.BytecodeLength);
	for (uint32 i = 0; i < std::size(desc.BlendState.RenderTarget); ++i)
	{
		const auto& rtBlend = desc.BlendState.RenderTarget[i];
		const auto& blendControlReg = latteRegister.CB_BLENDN_CONTROL[i];
		D3D12TraceEvent("game_pipeline_rt_state", "pipelineKey=0x{:016x} stateHash=0x{:016x} rtIndex={} active={} format={} writeMask=0x{:02x} blendEnable={} logicOpEnable={} logicOp={} srcColor={} dstColor={} colorOp={} srcAlpha={} dstAlpha={} alphaOp={} fbo=0x{:016x} colorMask=0x{:02x} targetMask=0x{:08x} latteSrcColor={} latteDstColor={} latteColorOp={} latteSrcAlpha={} latteDstAlpha={} latteAlphaOp={} latteSeparateAlpha={}",
						pipelineKey,
						identity.pipelineStateHash,
						i,
						i < desc.NumRenderTargets ? 1 : 0,
						(uint32)desc.RTVFormats[i],
						(uint32)rtBlend.RenderTargetWriteMask,
						rtBlend.BlendEnable ? 1 : 0,
						rtBlend.LogicOpEnable ? 1 : 0,
						(uint32)rtBlend.LogicOp,
						(uint32)rtBlend.SrcBlend,
						(uint32)rtBlend.DestBlend,
						(uint32)rtBlend.BlendOp,
						(uint32)rtBlend.SrcBlendAlpha,
						(uint32)rtBlend.DestBlendAlpha,
						(uint32)rtBlend.BlendOpAlpha,
						m_activeFBO ? m_activeFBO->key : 0,
						m_activeFBO ? m_activeFBO->GetColorAttachmentMask() : 0,
						renderTargetMask,
						(uint32)blendControlReg.get_COLOR_SRCBLEND(),
						(uint32)blendControlReg.get_COLOR_DSTBLEND(),
						(uint32)blendControlReg.get_COLOR_COMB_FCN(),
						(uint32)blendControlReg.get_ALPHA_SRCBLEND(),
						(uint32)blendControlReg.get_ALPHA_DSTBLEND(),
						(uint32)blendControlReg.get_ALPHA_COMB_FCN(),
						blendControlReg.get_SEPARATE_ALPHA_BLEND() ? 1 : 0);
	}
	Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline;
	HRESULT hr = E_FAIL;
	bool createNativePipeline = true;
	const uint64 currentTitleId = GetUwpTitleId();
	// title id is briefly 0 during startup handoff, so only a non-zero change counts
	if (currentTitleId != 0 && m_gamePipelineRecipeLoadingTitleId != 0 &&
		m_gamePipelineRecipeLoadingTitleId != currentTitleId)
		ClearPendingGamePipelineRecipes();
	D3D12PipelineRecipe::SerializeOptions recipeOptions{};
#if defined(__ID3D12GraphicsCommandList8_INTERFACE_DEFINED__)
	if (useIndependentStencilMasks)
		recipeOptions.independentDepthStencil = &independentDepthStencil;
#endif
	D3D12PipelineRecipe::Serialized serializedPipelineRecipe;
	const D3D12PipelineRecipe::Status recipeStatus = D3D12PipelineRecipe::Serialize(
		pipelineKey, desc, recipeOptions, serializedPipelineRecipe);
	const bool hasSerializedPipelineRecipe = recipeStatus == D3D12PipelineRecipe::Status::Success;
	bool pipelinePromotedFromRecipe = false;
	bool pipelinePromotedFromAsync = false;
	bool storedRecipeIsStale = false;
	DrainAsyncGamePipelineFailures();
	AdoptPrewarmedGamePipelines();
	if (auto pendingIt = m_pendingGamePipelineRecipes.find(pipelineKey);
		pendingIt != m_pendingGamePipelineRecipes.end())
	{
		if (hasSerializedPipelineRecipe && pendingIt->second.pipeline &&
			pendingIt->second.fingerprint == serializedPipelineRecipe.fingerprint)
		{
			pipeline = std::move(pendingIt->second.pipeline);
			m_pendingGamePipelineRecipes.erase(pendingIt);
			hr = S_OK;
			createNativePipeline = false;
			pipelinePromotedFromRecipe = true;
			pipelinePromotedFromAsync = m_asyncPipelinesInFlight.erase(pipelineKey) != 0;
			m_gamePipelineRecipePromotions++;
			if (m_gamePipelineRecipePromotions == 1 || (m_gamePipelineRecipePromotions % 64) == 0)
			{
				cemuLog_log(LogType::Force,
					"D3D12 UWP pipeline recipes: exact live promotions={} pending={} latest={:016x}.",
					m_gamePipelineRecipePromotions,
					m_pendingGamePipelineRecipes.size(),
					pipelineKey);
			}
			D3D12TraceEvent("game_pipeline_recipe_promoted",
				"pipelineKey=0x{:016x} pending={} cacheSize={}",
				pipelineKey,
				m_pendingGamePipelineRecipes.size(),
				m_gamePipelineCache.size());
		}
		else
		{
			m_pendingGamePipelineRecipes.erase(pendingIt);
			m_gamePipelineRecipeMismatches++;
			storedRecipeIsStale = true;
			static uint32 s_recipeMismatchLogCount = 0;
			if (D3D12RuntimeLogCapped(s_recipeMismatchLogCount, "pipeline recipe fingerprint mismatch", 16))
			{
				cemuLog_log(LogType::Force,
					"D3D12 UWP: rejected stale pipeline recipe {:016x} on exact live fingerprint mismatch (status={}, total mismatches={}).",
					pipelineKey,
					D3D12PipelineRecipe::StatusName(recipeStatus),
					m_gamePipelineRecipeMismatches);
			}
			D3D12TraceEvent("game_pipeline_recipe_mismatch",
				"pipelineKey=0x{:016x} serializeStatus={} pending={}",
				pipelineKey,
				D3D12PipelineRecipe::StatusName(recipeStatus),
				m_pendingGamePipelineRecipes.size());
		}
	}
	else if (!hasSerializedPipelineRecipe)
	{
		static uint32 s_recipeSerializeLogCount = 0;
		if (D3D12RuntimeLogCapped(s_recipeSerializeLogCount, "pipeline recipe serialization rejection", 16))
		{
			cemuLog_log(LogType::Force,
				"D3D12 UWP: pipeline {:016x} cannot be persisted as a recipe (status={}); native creation continues.",
				pipelineKey,
				D3D12PipelineRecipe::StatusName(recipeStatus));
		}
	}

	const bool xboxUwpDevice = IsXboxConsole();
	EnsureGamePipelineLibrary(currentTitleId);
	bool pipelineLibraryEligible = true;
#if defined(__ID3D12GraphicsCommandList8_INTERFACE_DEFINED__)
	pipelineLibraryEligible = !useIndependentStencilMasks;
#endif
	wchar_t pipelineLibraryName[40]{};
	if (createNativePipeline && !xboxUwpDevice && pipelineLibraryEligible && m_gamePipelineLibrary)
	{
		swprintf_s(pipelineLibraryName, L"cemu-%016llx", (unsigned long long)pipelineKey);
		hr = m_gamePipelineLibrary->LoadGraphicsPipeline(pipelineLibraryName, &desc, IID_PPV_ARGS(&pipeline));
		if (SUCCEEDED(hr))
		{
			createNativePipeline = false;
			m_gamePipelineLibraryHits++;
			D3D12TraceEvent("game_pipeline_library_hit", "pipelineKey=0x{:016x} title=0x{:016x}",
				pipelineKey, m_gamePipelineLibraryTitleId);
		}
		else
		{
			m_gamePipelineLibraryMisses++;
			if (IsD3D12DeviceFailure(hr))
			{
				CheckDeviceRemoved(hr, "LoadGraphicsPipeline(game)");
				throw std::runtime_error("D3D12 device failed while loading a cached game pipeline");
			}
		}
	}

	uint64 pipelineFingerprint = 0;
	std::vector<uint8> cachedPipelineBlob;
	bool cachedPipelineBlobLoaded = false;
	bool pipelineCreatedFromBlob = false;
	if (createNativePipeline && xboxUwpDevice && EnsureSinglePsoCacheSupport() && m_gamePipelineLibraryTitleId != 0)
	{
		pipelineFingerprint = BuildGamePipelineFingerprint(pipelineKey, desc);
		cachedPipelineBlobLoaded = LoadGamePipelineBlob(pipelineKey, pipelineFingerprint, cachedPipelineBlob);
		if (cachedPipelineBlobLoaded)
			desc.CachedPSO = { cachedPipelineBlob.data(), cachedPipelineBlob.size() };
	}
	// async_compile: build the PSO on a worker and skip this draw instead of stalling the render thread.
	// Pipelines with a driver blob are cheap to create here. Memory pressure does not turn this off: a synchronous
	// build costs the same memory, and after a teleport above the 84% guard every new PSO blocked the render thread
	// (1-2 FPS, ~500 async jobs starved behind the render-thread builds) until usage fell below 78%.
	uint64 contentCanonicalKey = 0;
	bool pipelineFromContentMatch = false;
	if (createNativePipeline && hasSerializedPipelineRecipe)
	{
		if (ID3D12PipelineState* sameContent = FindGamePipelineByContent(serializedPipelineRecipe.contentFingerprint, contentCanonicalKey))
		{
			pipeline = sameContent;
			hr = S_OK;
			createNativePipeline = false;
			pipelineFromContentMatch = true;
			m_gamePipelineContentReuses++;
		}
	}
	bool asyncPipelineEligible = createNativePipeline && m_allowAsyncShaderSkip && hasSerializedPipelineRecipe &&
		!cachedPipelineBlobLoaded && !m_asyncPipelineFailed.contains(pipelineKey);
#if defined(__ID3D12GraphicsCommandList8_INTERFACE_DEFINED__)
	asyncPipelineEligible = asyncPipelineEligible && !useIndependentStencilMasks;
#endif
	bool pipelineRecipeRecorded = false;
	if (asyncPipelineEligible)
	{
		// the recipe is stored when a draw adopts the result (below), so variants no draw ever used (drifting shadow
		// bias, short-lived effects) do not fill the 16384-entry cache that the launch prewarm rebuilds every session
		if (QueueAsyncGamePipeline(pipelineKey, std::move(serializedPipelineRecipe.bytes)))
		{
			D3D12TraceEvent("game_pipeline_async_queued", "pipelineKey=0x{:016x} queued={}", pipelineKey, m_asyncPipelinesQueued);
			if (auto* sibling = findDepthBiasSibling())
				return sibling;
			m_gamePipelineAsyncPending = true;
			return nullptr;
		}
	}
	if (createNativePipeline)
	{
		const auto createPipeline = [&]() -> HRESULT
		{
			LatteUwpHitchScope hitchScope(LatteUwpHitchOp::PipelineCreate);
#if defined(CEMU_UWP_PERFORMANCE_EXPERIMENT)
			// Render-thread runtime creation only; prewarm workers never write this.
			++m_performanceExperimentRuntimePsoCreates;
#endif
			const auto slowStarted = std::chrono::steady_clock::now();
			m_renderThreadPipelineCreates.fetch_add(1, std::memory_order_acq_rel);
			stdx::scope_exit renderThreadCreateDone([&]() { m_renderThreadPipelineCreates.fetch_sub(1, std::memory_order_acq_rel); });
		#if defined(CEMU_UWP_D3D12_DIAGNOSTICS)
			const auto started = std::chrono::steady_clock::now();
		#endif
			// no compilation slot: a draw must not queue behind shader compiles or background prewarm,
			// and the render thread creates one pipeline at a time anyway
			HRESULT createHr;
			{
				LatteUwpHitchScope driverScope(LatteUwpHitchOp::PipelineDriverCreate);
				D3D12GpuWatchdog::ActivityScope watchdogActivity(m_gpuWatchdog, D3D12GpuWatchdog::Activity::PipelineCreate, identity.pipelineStateHash);
#if defined(__ID3D12GraphicsCommandList8_INTERFACE_DEFINED__)
				createHr = useIndependentStencilMasks ?
				CreateGraphicsPipelineWithIndependentStencil(m_independentStencilDevice.Get(), desc, independentDepthStencil, pipeline) :
				m_device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pipeline));
#else
				createHr = m_device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pipeline));
#endif
			}
			const uint64 slowElapsedMs = (uint64)std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now() - slowStarted).count();
			if (slowElapsedMs >= 100 && m_slowSyncPipelineLogs < 200)
			{
				++m_slowSyncPipelineLogs;
				cemuLog_log(LogType::Force, "D3D12: render thread built a PSO in {} ms (skip blocked=0x{:02x} [1 async off, 2 no FBO, 4 1600x1600, 8 no depth <=6 indices, 16 occlusion query, 32 stream-out], async eligible={}, recipe={}, heavy work deferred={}, async in flight={}, vs={:016x} ps={:016x}, key={:016x}, recipe cache: {}).",
					slowElapsedMs, m_asyncSkipBlockReasons, asyncPipelineEligible ? 1 : 0, hasSerializedPipelineRecipe ? 1 : 0,
					CemuUwp::MemoryBudgetService::IsHeavyWorkDeferred() ? 1 : 0, m_asyncPipelinesInFlight.size(),
					vertexShader ? vertexShader->baseHash : 0, pixelShader ? pixelShader->baseHash : 0,
					// tells why the launch prewarm did not cover it (the rain PSO was rebuilt every session)
					pipelineKey, D3D12PipelineStableCache::GetInstance().DescribePipeline(pipelineKey));
			}
		#if defined(CEMU_UWP_D3D12_DIAGNOSTICS)
			const uint64 elapsedUs = (uint64)std::chrono::duration_cast<std::chrono::microseconds>(
				std::chrono::steady_clock::now() - started).count();
			m_gamePsoCreateCount++;
			m_gamePsoCreateTimeUs += elapsedUs;
			m_gamePsoCreateMaxUs = std::max(m_gamePsoCreateMaxUs, elapsedUs);
		#endif
			return createHr;
		};
		const auto discardCachedPipelineBlob = [&]()
		{
			RemoveGamePipelineBlob(pipelineKey, pipelineFingerprint);
			m_singlePsoCacheRejects++;
			m_singlePsoCacheMisses++;
			pipeline.Reset();
			cachedPipelineBlob.clear();
			desc.CachedPSO = {};
			cachedPipelineBlobLoaded = false;
		};
		hr = createPipeline();
		if (cachedPipelineBlobLoaded && FAILED(hr) &&
			!IsD3D12DeviceFailure(hr) && !IsD3D12TransientMemoryFailure(hr))
		{
			discardCachedPipelineBlob();
			hr = createPipeline();
		}
		if (IsD3D12TransientMemoryFailure(hr))
		{
			RecoverFromPipelineAllocationFailure();
			pipeline.Reset();
			hr = createPipeline();
			if (cachedPipelineBlobLoaded && FAILED(hr) &&
				!IsD3D12DeviceFailure(hr) && !IsD3D12TransientMemoryFailure(hr))
			{
				discardCachedPipelineBlob();
				hr = createPipeline();
			}
		}
		if (cachedPipelineBlobLoaded && SUCCEEDED(hr))
		{
			pipelineCreatedFromBlob = true;
			m_singlePsoCacheHits++;
		}
	}
	if (FAILED(hr))
	{
		m_gamePsoCreateFailures++;
		cemuLog_log(LogType::Force,
					"D3D12: failed to create game PSO 0x{:016x} (HRESULT 0x{:08x}) vs={:016x} gs={:016x} ps={:016x} topology={} rts={} dsv={} depth={} stencil={} logicOp={} independentBlend={} noRaster={} pointGS={} rectGS={} shaderBytes={}/{}/{} blendMask=0x{:02x} targetMask=0x{:08x}.",
					identity.pipelineStateHash,
					(uint32)hr,
					vertexShader ? vertexShader->baseHash : 0,
					geometryShader ? geometryShader->baseHash : 0,
					pixelShader ? pixelShader->baseHash : 0,
					(uint32)desc.PrimitiveTopologyType,
					desc.NumRenderTargets,
					(uint32)desc.DSVFormat,
					desc.DepthStencilState.DepthEnable,
					desc.DepthStencilState.StencilEnable,
					logicOpEnable,
					desc.BlendState.IndependentBlendEnable,
					noRasterizationOutput,
					usePointExpansionGS,
					useRectEmulationGS,
					desc.VS.BytecodeLength,
					desc.GS.BytecodeLength,
					desc.PS.BytecodeLength,
					blendEnableMask,
					renderTargetMask);
		D3D12TraceEvent("game_pipeline_create_failed", "pipelineKey=0x{:016x} stateHash=0x{:016x} hr=0x{:08x} primitive={} topologyType={} noRaster={} pointGS={} rectGS={} fbo=0x{:016x} colorCount={} colorMask=0x{:02x} rt0={} rt1={} rt2={} rt3={} rt4={} rt5={} rt6={} rt7={} dsv={} depthEnable={} stencilEnable={} logicOp={} independentBlend={} blendMask=0x{:02x} targetMask=0x{:08x} vs=0x{:016x} gs=0x{:016x} ps=0x{:016x} vsBytes={} gsBytes={} psBytes={}",
						pipelineKey,
						identity.pipelineStateHash,
						(uint32)hr,
						(uint32)primitiveMode,
						(uint32)desc.PrimitiveTopologyType,
						noRasterizationOutput ? 1 : 0,
						usePointExpansionGS ? 1 : 0,
						useRectEmulationGS ? 1 : 0,
						m_activeFBO ? m_activeFBO->key : 0,
						m_activeFBO ? m_activeFBO->GetColorAttachmentCount() : 0,
						m_activeFBO ? m_activeFBO->GetColorAttachmentMask() : 0,
						(uint32)desc.RTVFormats[0],
						(uint32)desc.RTVFormats[1],
						(uint32)desc.RTVFormats[2],
						(uint32)desc.RTVFormats[3],
						(uint32)desc.RTVFormats[4],
						(uint32)desc.RTVFormats[5],
						(uint32)desc.RTVFormats[6],
						(uint32)desc.RTVFormats[7],
						(uint32)desc.DSVFormat,
						desc.DepthStencilState.DepthEnable ? 1 : 0,
						desc.DepthStencilState.StencilEnable ? 1 : 0,
						logicOpEnable ? 1 : 0,
						desc.BlendState.IndependentBlendEnable ? 1 : 0,
						blendEnableMask,
						renderTargetMask,
						vertexShader ? vertexShader->baseHash : 0,
						geometryShader ? geometryShader->baseHash : 0,
						pixelShader ? pixelShader->baseHash : 0,
						(unsigned long long)desc.VS.BytecodeLength,
						(unsigned long long)desc.GS.BytecodeLength,
						(unsigned long long)desc.PS.BytecodeLength);
		for (uint32 i = 0; i < desc.NumRenderTargets && i < std::size(desc.RTVFormats); ++i)
		{
			const auto& rtBlend = desc.BlendState.RenderTarget[i];
			cemuLog_log(LogType::Force,
						"D3D12:   RT{} format={} writeMask=0x{:02x} blend={} src={} dst={} op={} srcA={} dstA={} opA={}.",
						i,
						(uint32)desc.RTVFormats[i],
						(uint32)rtBlend.RenderTargetWriteMask,
						rtBlend.BlendEnable,
						(uint32)rtBlend.SrcBlend,
						(uint32)rtBlend.DestBlend,
						(uint32)rtBlend.BlendOp,
						(uint32)rtBlend.SrcBlendAlpha,
						(uint32)rtBlend.DestBlendAlpha,
						(uint32)rtBlend.BlendOpAlpha);
		}
		LogInfoQueueMessages("CreateGraphicsPipelineState(game)", 16);
		CheckDeviceRemoved(hr, "CreateGraphicsPipelineState(game)");
		if (IsD3D12TransientMemoryFailure(hr))
			m_gamePipelineRetryFrame[pipelineKey] = LatteGPUState.frameCounter + 30;
		else
			m_gamePipelineCache.emplace(pipelineKey, GamePipelineEntry{});
		return nullptr;
	}

	auto& pipelineStableCache = D3D12PipelineStableCache::GetInstance();
	if (pipelineFromContentMatch)
	{
		// no recipe for this key: the next launch prewarms the canonical one, and this key finds it by content again
		pipelineStableCache.MarkPipelineUsed(contentCanonicalKey, !m_allowAsyncShaderSkip);
	}
	else
	{
		// a stored recipe that was not prewarmed yet is still current; only a stale one is rewritten
		if (hasSerializedPipelineRecipe && (!pipelinePromotedFromRecipe || pipelinePromotedFromAsync) && !pipelineRecipeRecorded)
			pipelineStableCache.AddPipelineRecipe(pipelineKey, serializedPipelineRecipe.bytes, storedRecipeIsStale);
		pipelineStableCache.MarkPipelineUsed(pipelineKey, !m_allowAsyncShaderSkip);
		if (hasSerializedPipelineRecipe)
			m_gamePipelineKeyByContent.try_emplace(serializedPipelineRecipe.contentFingerprint, pipelineKey);
	}
	if (createNativePipeline && xboxUwpDevice && !pipelineCreatedFromBlob && pipelineFingerprint != 0)
		StoreGamePipelineBlob(pipelineKey, pipelineFingerprint, pipeline.Get());
	// PC: a pipeline built in the background goes into the native library like a synchronous one
	if (pipelinePromotedFromAsync && !xboxUwpDevice && pipelineLibraryEligible && m_gamePipelineLibrary)
		swprintf_s(pipelineLibraryName, L"cemu-%016llx", (unsigned long long)pipelineKey);
	if ((createNativePipeline || pipelinePromotedFromAsync) && pipelineLibraryEligible && m_gamePipelineLibrary && pipelineLibraryName[0] != L'\0')
	{
		const HRESULT storeHr = m_gamePipelineLibrary->StorePipeline(pipelineLibraryName, pipeline.Get());
		if (SUCCEEDED(storeHr))
		{
			m_gamePipelineLibraryDirty = true;
			m_gamePipelineLibraryStores++;
		}
		else if (IsD3D12DeviceFailure(storeHr))
		{
			CheckDeviceRemoved(storeHr, "StorePipeline(game)");
			throw std::runtime_error("D3D12 device failed while storing a cached game pipeline");
		}
		else if (storeHr != E_INVALIDARG)
		{
			cemuLog_logOnce(LogType::Force, "D3D12 UWP: failed to cache a native game PSO (HRESULT 0x{:08x}).", (uint32)storeHr);
		}
	}
	SetD3D12ObjectName(pipeline.Get(), L"Cemu D3D12 game PSO");
	m_gamePipelineRetryFrame.erase(pipelineKey);
	m_gamePipelineDeferCount.erase(pipelineKey);
	m_asyncPipelinesInFlight.erase(pipelineKey);
	auto [insertedIt, inserted] = m_gamePipelineCache.emplace(pipelineKey, GamePipelineEntry{ std::move(pipeline), LatteGPUState.frameCounter });
	(void)inserted;
	if (hasBakedDepthBias)
		m_depthBiasSiblingPipelines[depthBiasSiblingKey()] = DepthBiasSibling{ pipelineKey, drawBiasSlope, drawBiasOffset, drawBiasClamp };
	D3D12PipelineCompiler::RecordPipelineCreated(identity, pipelineKey);
	D3D12TraceEvent("game_pipeline_create_success", "pipelineKey=0x{:016x} stateHash=0x{:016x} primitive={} topologyType={} noRaster={} pointGS={} rectGS={} fbo=0x{:016x} colorCount={} colorMask=0x{:02x} rt0={} rt1={} rt2={} rt3={} rt4={} rt5={} rt6={} rt7={} dsv={} vs=0x{:016x} gs=0x{:016x} ps=0x{:016x} pipeline={} cacheSize={}",
					pipelineKey,
					identity.pipelineStateHash,
					(uint32)primitiveMode,
					(uint32)desc.PrimitiveTopologyType,
					noRasterizationOutput ? 1 : 0,
					usePointExpansionGS ? 1 : 0,
					useRectEmulationGS ? 1 : 0,
					m_activeFBO ? m_activeFBO->key : 0,
					m_activeFBO ? m_activeFBO->GetColorAttachmentCount() : 0,
					m_activeFBO ? m_activeFBO->GetColorAttachmentMask() : 0,
					(uint32)desc.RTVFormats[0],
					(uint32)desc.RTVFormats[1],
					(uint32)desc.RTVFormats[2],
					(uint32)desc.RTVFormats[3],
					(uint32)desc.RTVFormats[4],
					(uint32)desc.RTVFormats[5],
					(uint32)desc.RTVFormats[6],
					(uint32)desc.RTVFormats[7],
					(uint32)desc.DSVFormat,
					vertexShader ? vertexShader->baseHash : 0,
					geometryShader ? geometryShader->baseHash : 0,
					pixelShader ? pixelShader->baseHash : 0,
					(const void*)insertedIt->second.pipeline.Get(),
					m_gamePipelineCache.size());
	return insertedIt->second.pipeline.Get();
}

RendererShaderD3D12* D3D12Renderer::GetSurfaceCopyShader(RendererShader::ShaderType type, const char* source)
{
	auto& shader = m_surfaceCopyShaders[source];
	if (!shader)
		shader = std::make_unique<RendererShaderD3D12>(type, 0, 0, false, false, source);
	return shader.get();
}

ID3D12PipelineState* D3D12Renderer::GetSurfaceCopyPipeline(SurfaceCopyMode mode, DXGI_FORMAT rtvFormat, DXGI_FORMAT dsvFormat)
{
	if (!m_surfaceCopyRootSignature)
	{
		D3D12TraceEvent("surface_copy_pipeline_unavailable", "reason=missing_root mode={} rtv={} dsv={}", (uint32)mode, (uint32)rtvFormat, (uint32)dsvFormat);
		return nullptr;
	}

	const uint64 key = ((uint64)mode << 56) ^ ((uint64)(uint32)rtvFormat << 24) ^ (uint64)(uint32)dsvFormat;
	auto it = m_surfaceCopyPipelineCache.find(key);
	if (it != m_surfaceCopyPipelineCache.end())
	{
		D3D12TraceEvent("surface_copy_pipeline_cache_hit", "key=0x{:016x} mode={} rtv={} dsv={} pipeline={}",
						key,
						(uint32)mode,
						(uint32)rtvFormat,
						(uint32)dsvFormat,
						(const void*)it->second.Get());
		return it->second.Get();
	}

	const char* pixelShaderSource = kSurfaceCopyColorToColorPixelShader;
	if (mode == SurfaceCopyMode::DepthToColor)
		pixelShaderSource = kSurfaceCopyDepthToColorPixelShader;
	else if (mode == SurfaceCopyMode::ColorToDepth)
		pixelShaderSource = kSurfaceCopyColorToDepthPixelShader;
	else if (mode == SurfaceCopyMode::DepthToDepth)
		pixelShaderSource = kSurfaceCopyDepthToDepthPixelShader;
	else if (mode == SurfaceCopyMode::StencilToStencil)
		pixelShaderSource = kSurfaceCopyStencilToStencilPixelShader;

	RendererShaderD3D12& vertexShader = *GetSurfaceCopyShader(RendererShader::ShaderType::kVertex, kSurfaceCopyVertexShader);
	RendererShaderD3D12& pixelShader = *GetSurfaceCopyShader(RendererShader::ShaderType::kFragment, pixelShaderSource);
	if (!vertexShader.WaitForCompiled() || !pixelShader.WaitForCompiled() || !vertexShader.HasDXIL() || !pixelShader.HasDXIL())
	{
		D3D12TraceEvent("surface_copy_pipeline_unavailable", "reason=shader_dxil mode={} rtv={} dsv={} vsDxil={} psDxil={}",
						(uint32)mode,
						(uint32)rtvFormat,
						(uint32)dsvFormat,
						vertexShader.HasDXIL() ? 1 : 0,
						pixelShader.HasDXIL() ? 1 : 0);
		return nullptr;
	}

	D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
	desc.pRootSignature = m_surfaceCopyRootSignature.Get();
	desc.VS = { vertexShader.GetDXIL().data(), vertexShader.GetDXIL().size() };
	desc.PS = { pixelShader.GetDXIL().data(), pixelShader.GetDXIL().size() };
	desc.BlendState.RenderTarget[0].RenderTargetWriteMask =
		mode == SurfaceCopyMode::DepthToColor ? D3D12_COLOR_WRITE_ENABLE_RED : D3D12_COLOR_WRITE_ENABLE_ALL;
	desc.SampleMask = UINT_MAX;
	desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
	desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
	desc.RasterizerState.FrontCounterClockwise = FALSE;
	desc.RasterizerState.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
	desc.RasterizerState.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
	desc.RasterizerState.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
	desc.RasterizerState.DepthClipEnable = TRUE;
	desc.RasterizerState.MultisampleEnable = FALSE;
	desc.RasterizerState.AntialiasedLineEnable = FALSE;
	desc.RasterizerState.ForcedSampleCount = 0;
	desc.RasterizerState.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
	desc.InputLayout = {};
	desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	desc.SampleDesc.Count = 1;

	const bool writesDepth =
		mode == SurfaceCopyMode::ColorToDepth
		|| mode == SurfaceCopyMode::DepthToDepth
		;
	if (writesDepth)
	{
		desc.DepthStencilState.DepthEnable = TRUE;
		desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
		desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
		desc.DepthStencilState.StencilEnable = FALSE;
		desc.NumRenderTargets = 0;
		desc.DSVFormat = dsvFormat;
	}
	else if (mode == SurfaceCopyMode::StencilToStencil)
	{
		const D3D12_DEPTH_STENCILOP_DESC stencilOp{ D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_REPLACE, D3D12_COMPARISON_FUNC_ALWAYS };
		desc.DepthStencilState.DepthEnable = FALSE;
		desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
		desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
		desc.DepthStencilState.StencilEnable = TRUE;
		desc.DepthStencilState.StencilReadMask = 0xFF;
		desc.DepthStencilState.StencilWriteMask = 0xFF;
		desc.DepthStencilState.FrontFace = stencilOp;
		desc.DepthStencilState.BackFace = stencilOp;
		desc.NumRenderTargets = 0;
		desc.DSVFormat = dsvFormat;
	}
	else
	{
		desc.DepthStencilState.DepthEnable = FALSE;
		desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
		desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
		desc.DepthStencilState.StencilEnable = FALSE;
		desc.NumRenderTargets = 1;
		desc.RTVFormats[0] = rtvFormat;
	}

	D3D12TraceEvent("surface_copy_pipeline_create_begin", "key=0x{:016x} mode={} rtv={} dsv={} vsBytes={} psBytes={} rootSig={}",
					key,
					(uint32)mode,
					(uint32)rtvFormat,
					(uint32)dsvFormat,
					(unsigned long long)desc.VS.BytecodeLength,
					(unsigned long long)desc.PS.BytecodeLength,
					(const void*)m_surfaceCopyRootSignature.Get());
	Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline;
	const HRESULT hr = m_device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pipeline));
	if (FAILED(hr))
	{
		cemuLog_log(LogType::Force, "D3D12: failed to create surface-copy PSO (HRESULT 0x{:08x})", (uint32)hr);
		D3D12TraceEvent("surface_copy_pipeline_create_failed", "key=0x{:016x} hr=0x{:08x} mode={} rtv={} dsv={} vsBytes={} psBytes={}",
						key,
						(uint32)hr,
						(uint32)mode,
						(uint32)rtvFormat,
						(uint32)dsvFormat,
						(unsigned long long)desc.VS.BytecodeLength,
						(unsigned long long)desc.PS.BytecodeLength);
		return nullptr;
	}
	const wchar_t* debugName = L"Cemu D3D12 color-to-color copy PSO";
	if (mode == SurfaceCopyMode::DepthToColor)
		debugName = L"Cemu D3D12 depth-to-color copy PSO";
	else if (mode == SurfaceCopyMode::ColorToDepth)
		debugName = L"Cemu D3D12 color-to-depth copy PSO";
	else if (mode == SurfaceCopyMode::StencilToStencil)
		debugName = L"Cemu D3D12 stencil-to-stencil copy PSO";
	SetD3D12ObjectName(pipeline.Get(), debugName);
	auto [insertedIt, inserted] = m_surfaceCopyPipelineCache.emplace(key, std::move(pipeline));
	(void)inserted;
	D3D12TraceEvent("surface_copy_pipeline_create_success", "key=0x{:016x} mode={} rtv={} dsv={} pipeline={} cacheSize={}",
					key,
					(uint32)mode,
					(uint32)rtvFormat,
					(uint32)dsvFormat,
					(const void*)insertedIt->second.Get(),
					m_surfaceCopyPipelineCache.size());
	return insertedIt->second.Get();
}

void D3D12Renderer::StopUsingPadAndWait()
{
	WaitForIdle();
	m_padSwapchain = {};
}

bool D3D12Renderer::IsPadWindowActive()
{
	return m_mainSwapchain.active && UwpLayoutShowsGamePad();
}

bool D3D12Renderer::GetVRAMInfo(int& usageInMB, int& totalInMB) const
{
	Microsoft::WRL::ComPtr<IDXGIAdapter3> adapter3;
	if (!m_adapter || FAILED(m_adapter.As(&adapter3)))
		return Renderer::GetVRAMInfo(usageInMB, totalInMB);

	DXGI_QUERY_VIDEO_MEMORY_INFO localInfo{};
	if (FAILED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &localInfo)))
		return Renderer::GetVRAMInfo(usageInMB, totalInMB);

	usageInMB = (int)(localInfo.CurrentUsage / (1024ull * 1024ull));
	totalInMB = (int)(localInfo.Budget / (1024ull * 1024ull));
	return true;
}

void D3D12Renderer::AppendOverlayDebugInfo()
{
	D3D12_EXCLUSIVE_RECORDING();
	ImGui::Text("--- Direct3D 12 debug info ---");
	ImGui::Text("Feature level %s, shader model 0x%x", D3D12FeatureLevelName(m_featureLevel), (uint32)m_shaderModel);
	ImGui::Text("Resource binding tier %u, tiled tier %u", (uint32)m_resourceBindingTier, (uint32)m_tiledResourcesTier);
	ImGui::Text("Root signature 1.%u, UMA %s, coherent %s",
				m_rootSignatureVersion >= D3D_ROOT_SIGNATURE_VERSION_1_1 ? 1 : 0,
				m_isUma ? "yes" : "no",
				m_isCacheCoherentUma ? "yes" : "no");
	ImGui::Text("Command slot %u/%u, sync slot %u", m_commandSlotIndex, kCommandBufferPoolSize, m_commandSlotSyncIndex);
	ImGui::Text("Submitted lists %llu", (unsigned long long)m_numSubmittedCommandLists);
	ImGui::Text("Completed lists %llu", (unsigned long long)m_numCompletedCommandLists);
	ImGui::Text("Completed fence %llu", (unsigned long long)m_lastCompletedFenceValue);
	ImGui::Text("Deferred release batches %zu", m_deferredReleaseQueue.size());
	ImGui::Text("Output PSOs %zu", m_outputPipelineCache.size());
	ImGui::Text("Game PSOs %zu, draws submitted %llu, failures %llu",
				m_gamePipelineCache.size(),
				(unsigned long long)m_gameDrawCallsSubmitted,
				(unsigned long long)m_gamePsoCreateFailures);
	ImGui::Text("Cull-all draws skipped %llu", (unsigned long long)m_cullAllDrawsSkipped);
	ImGui::Text("Dropped draws: sequence %llu, logic op %llu, shader code %llu, PSO %llu, index %llu, index upload %llu",
				(unsigned long long)m_droppedDraws[(size_t)DroppedDrawReason::SequenceRejected],
				(unsigned long long)m_droppedDraws[(size_t)DroppedDrawReason::LogicOpUnsupported],
				(unsigned long long)m_droppedDraws[(size_t)DroppedDrawReason::MissingShaderCode],
				(unsigned long long)m_droppedDraws[(size_t)DroppedDrawReason::MissingPipeline],
				(unsigned long long)m_droppedDraws[(size_t)DroppedDrawReason::InvalidIndexBuffer],
				(unsigned long long)m_droppedDraws[(size_t)DroppedDrawReason::IndexUploadFailed]);
	ImGui::Text("No-raster streamout draws %llu", (unsigned long long)m_noRasterStreamoutDraws);
	ImGui::Text("Point expansion GS variants %zu, draws %llu",
				m_pointExpansionGeometryShaders.size(),
				(unsigned long long)m_pointExpansionDraws);
	ImGui::Text("Surface-copy PSOs %zu", m_surfaceCopyPipelineCache.size());
	auto pipelineStats = D3D12PipelineCompiler::GetStats();
	ImGui::Text("Game PSO identities %llu, cache hit/add %llu/%llu, invalid %llu, stable tracked %zu",
				(unsigned long long)pipelineStats.identitiesSeen,
				(unsigned long long)pipelineStats.stableCacheHits,
				(unsigned long long)pipelineStats.stableCacheAdds,
				(unsigned long long)pipelineStats.invalidIdentities,
				D3D12PipelineStableCache::GetInstance().GetCachedPipelineCount());
	ImGui::Text("Feedback loops %llu, scratch copies %llu, live scratch %zu",
				(unsigned long long)m_feedbackLoopDetections,
				(unsigned long long)m_feedbackLoopCopies,
				m_feedbackScratchCopies.size());
#if defined(CEMU_UWP)
	ImGui::Text("UWP native PSO library hit/miss/store %llu/%llu/%llu",
				(unsigned long long)m_gamePipelineLibraryHits,
				(unsigned long long)m_gamePipelineLibraryMisses,
				(unsigned long long)m_gamePipelineLibraryStores);
	ImGui::Text("UWP scratch pool hit/miss %llu/%llu, pooled %zu (%llu MB), retired/evicted %llu/%llu",
				(unsigned long long)m_scratchTexturePoolHits,
				(unsigned long long)m_scratchTexturePoolMisses,
				m_scratchTexturePool.size(),
				(unsigned long long)(m_scratchTexturePoolBytes / (1024ull * 1024ull)),
				(unsigned long long)m_scratchTexturePoolRetired,
				(unsigned long long)m_scratchTexturePoolEvicted);
	ImGui::Text("UWP texture allocation recoveries %llu/%llu",
				(unsigned long long)m_textureAllocationRecoverySuccesses,
				(unsigned long long)m_textureAllocationRecoveryAttempts);
	const auto appMemory = CemuUwp::MemoryBudgetService::GetLatestSnapshot();
	if (appMemory.enabled)
	{
		ImGui::Text("Xbox app memory %llu/%llu MB (expected %llu MB, %s)",
			(unsigned long long)(appMemory.usageBytes / (1024ull * 1024ull)),
			(unsigned long long)(appMemory.limitBytes / (1024ull * 1024ull)),
			(unsigned long long)(appMemory.expectedLimitBytes / (1024ull * 1024ull)),
			CemuUwp::MemoryBudgetService::PressureName(appMemory.pressure));
		ImGui::Text("Xbox memory trims %llu, textures %llu (%llu MB), scratch %llu MB",
			(unsigned long long)m_memoryPressureTrimPasses,
			(unsigned long long)m_memoryPressureTexturesEvicted,
			(unsigned long long)(m_memoryPressureTextureBytes / (1024ull * 1024ull)),
			(unsigned long long)(m_memoryPressureScratchBytes / (1024ull * 1024ull)));
		ImGui::Text("Xbox memory guard %s: heavy passes %llu, pipelines deferred/forced %llu/%llu, alloc failures %llu, heap compactions %llu",
			appMemory.heavyWorkDeferred ? "engaged" : "idle",
			(unsigned long long)m_memoryPressureHeavyPasses,
			(unsigned long long)m_memoryPressureDeferredPipelines,
			(unsigned long long)m_memoryPressureForcedPipelines,
			(unsigned long long)appMemory.allocationFailures,
			(unsigned long long)appMemory.heapCompactions);
	}
	ImGui::Text("UWP descriptor heap binds/skips %llu/%llu",
				(unsigned long long)m_shaderVisibleHeapBinds,
				(unsigned long long)m_shaderVisibleHeapBindSkips);
	ImGui::Text("UWP command state binds/skips %llu/%llu, snapshot SRV update/skip %llu/%llu",
				(unsigned long long)m_commandStateBinds,
				(unsigned long long)m_commandStateBindSkips,
				(unsigned long long)m_uwpSnapshotSrvUpdates,
				(unsigned long long)m_uwpSnapshotSrvSkips);
#endif
	uint32 usedQuerySlots = 0;
	for (const auto& page : m_occlusionQueryPages)
		usedQuerySlots += (uint32)std::count(page.slotsUsed.begin(), page.slotsUsed.end(), true);
	ImGui::Text("Occlusion queries active %zu, heap slots %u/%zu", m_activeOcclusionQueries.size(), usedQuerySlots, m_occlusionQueryPages.size() * kOcclusionQuerySlots);
	auto shaderStats = RendererShaderD3D12::GetStats();
	ImGui::Text("DXIL cache hit/miss %llu/%llu, compiled/failed %llu/%llu",
				(unsigned long long)shaderStats.dxilCacheHits,
				(unsigned long long)shaderStats.dxilCacheMisses,
				(unsigned long long)shaderStats.dxilCompiled,
				(unsigned long long)shaderStats.dxilFailed);
	ImGui::Text("DXIL compiler queue %u, threads %u, DXC objects %llu/%llu",
				shaderStats.queuedShaders,
				shaderStats.compilerThreads,
				(unsigned long long)shaderStats.dxcUtilsCreated,
				(unsigned long long)shaderStats.dxcCompilersCreated);
	ImGui::Text("UWP DXIL prewarm shaders/wait %llu/%llu ms, draw waits %llu",
				(unsigned long long)shaderStats.cacheLoadShadersDiscovered,
				(unsigned long long)shaderStats.cacheLoadPrewarmWaitMilliseconds,
				(unsigned long long)shaderStats.renderThreadCompilationWaits);
	ImGui::Text("Async PSOs queued/created %llu/%llu, in flight %zu, sync fallback %zu, evicted %llu, unused released %llu, bias stand-in draws %llu, bias aliases %llu",
				(unsigned long long)m_asyncPipelinesQueued,
				(unsigned long long)m_asyncPipelinesCreated.load(std::memory_order_relaxed),
				m_asyncPipelinesInFlight.size(),
				m_asyncPipelineFailed.size(),
				(unsigned long long)m_gamePipelinesEvicted,
				(unsigned long long)m_asyncPipelinesReleasedUnused,
				(unsigned long long)m_depthBiasSiblingDraws,
				(unsigned long long)m_depthBiasAliases);
	ImGui::Text("DXIL evicted %llu MB, reloaded %llu; HLSL pruned %llu MB, prune fallbacks %llu",
				(unsigned long long)(shaderStats.dxilEvictedBytes / (1024 * 1024)),
				(unsigned long long)shaderStats.dxilReloads,
				(unsigned long long)(shaderStats.hlslPrunedBytes / (1024 * 1024)),
				(unsigned long long)shaderStats.hlslPruneFallbacks);
	ImGui::Text("RTV descriptors %u", m_nextRtvIndex);
	if (m_descriptorManager)
	{
		auto descriptorStats = m_descriptorManager->GetStats();
		ImGui::Text("Persistent descriptors C/S/U %u, SMP %u, RTV %u, DSV %u",
					descriptorStats.persistentCbvSrvUavUsed,
					descriptorStats.persistentSamplerUsed,
					descriptorStats.persistentRtvUsed,
					descriptorStats.persistentDsvUsed);
		ImGui::Text("Shader descriptors C/S/U %u/%u, SMP %u/%u, rollovers %u",
					descriptorStats.shaderVisibleCbvSrvUavUsed,
					descriptorStats.shaderVisibleCbvSrvUavCapacity,
					descriptorStats.shaderVisibleSamplerUsed,
					descriptorStats.shaderVisibleSamplerCapacity,
					descriptorStats.shaderVisibleRollovers);
		ImGui::Text("Null resource table cached %s, generation %u",
					m_gameNullResourceTable.IsValid() ? "yes" : "no",
					m_gameNullResourceTableGeneration);
		ImGui::Text("Game resource table cache %zu, hit/miss %llu/%llu, generation %u",
					m_gameResourceTableCache.size(),
					(unsigned long long)m_gameResourceTableCacheHits,
					(unsigned long long)m_gameResourceTableCacheMisses,
					m_gameResourceTableCacheGeneration);
		ImGui::Text("Game sampler table cache %zu, hit/miss %llu/%llu, generation %u",
					m_gameSamplerTableCache.size(),
					(unsigned long long)m_gameSamplerTableCacheHits,
					(unsigned long long)m_gameSamplerTableCacheMisses,
					m_gameSamplerTableCacheGeneration);
	}
	if (m_memoryManager)
	{
		auto memoryStats = m_memoryManager->GetStats();
		ImGui::Text("Upload ring buffers %u, %llu MB",
					memoryStats.upload.buffers,
					(unsigned long long)(memoryStats.upload.totalBytes / (1024ull * 1024ull)));
		ImGui::Text("Readback ring buffers %u, %llu MB",
					memoryStats.readback.buffers,
					(unsigned long long)(memoryStats.readback.totalBytes / (1024ull * 1024ull)));
		ImGui::Text("D3D12MA resources default/upload/readback %llu/%llu/%llu",
					(unsigned long long)memoryStats.createdDefaultResources,
					(unsigned long long)memoryStats.createdUploadResources,
					(unsigned long long)memoryStats.createdReadbackResources);
		ImGui::Text("D3D12MA local usage/budget %llu/%llu MB, allocated/block %llu/%llu MB",
			(unsigned long long)(memoryStats.localUsageBytes / (1024ull * 1024ull)),
			(unsigned long long)(memoryStats.localBudgetBytes / (1024ull * 1024ull)),
			(unsigned long long)(memoryStats.allocatorAllocationBytes / (1024ull * 1024ull)),
			(unsigned long long)(memoryStats.allocatorBlockBytes / (1024ull * 1024ull)));
	}
}

// threaded recording: the command list may only be opened or submitted by the recorder or inside an exclusive scope.
// The menu hung once with the recorder on (an overlay command list never finished on the GPU), so log who breaks this.
void D3D12Renderer::CheckRecordingContext(const char* operation)
{
	if (t_recordContext || !m_recordThreadRunning.load(std::memory_order_relaxed))
		return;
	static std::atomic_uint32_t s_violations{ 0 };
	if (s_violations.fetch_add(1, std::memory_order_relaxed) < 16)
		cemuLog_log(LogType::Force, "D3D12: {} outside the recording context (thread {}, command list open={}, fence {}).",
			operation, GetCurrentThreadId(), m_commandListOpen ? 1 : 0, m_nextFenceValue);
}

void D3D12Renderer::ResetCommandList()
{
	CheckRecordingContext("ResetCommandList");
	if (m_commandListOpen)
		ExecuteCurrentCommandList(SubmitResetOpen);
	ProcessFinishedCommandLists();
	WaitForCommandSlot(m_commandSlotIndex);

	auto& slot = GetCurrentCommandSlot();
	D3D12TraceEvent("cmd_reset", "slot={} syncSlot={} completedFence={} activeFbo=0x{:016x} deferredBatches={}",
					m_commandSlotIndex,
					m_commandSlotSyncIndex,
					(unsigned long long)m_lastCompletedFenceValue,
					m_activeFBO ? m_activeFBO->key : 0,
					m_deferredReleaseQueue.size());
	if (FAILED(slot.allocator->Reset()))
		throw std::runtime_error("Failed to reset D3D12 command allocator");
	if (FAILED(slot.commandList->Reset(slot.allocator.Get(), nullptr)))
		throw std::runtime_error("Failed to reset D3D12 command list");
	m_commandListOpen = true;
#if defined(CEMU_UWP_D3D12_DIAGNOSTICS) || defined(CEMU_UWP_D3D12_WORKLOAD_PROFILE) || defined(CEMU_UWP)
	slot.timingPending = false;
	if (m_timestampQueryHeap && m_timestampReadback.resource && m_timestampReadbackMapped)
		slot.commandList->EndQuery(m_timestampQueryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 2 * m_commandSlotIndex);
#endif
	ResetCommandStateCache();
	BindShaderVisibleHeaps();
	SetD3D12Viewport(m_currentViewport);
	SetD3D12Scissor(m_currentScissor);
	if (m_activeFBO)
		BindFramebuffer(m_activeFBO);
	OcclusionQueryNotifyBeginCommandList();
}

void D3D12Renderer::ExecuteCurrentCommandList(uint32 workloadReasons)
{
	if (!m_commandListOpen)
		return;
	CheckRecordingContext("ExecuteCurrentCommandList");
#if defined(CEMU_UWP_D3D12_WORKLOAD_PROFILE)
	if (workloadReasons == SubmitUnspecified)
		++m_uwpWorkloadProfile.unspecifiedSubmits;
	for (size_t i = 0; i < m_uwpWorkloadProfile.submitReasons.size(); ++i)
		if ((workloadReasons & (1u << i)) != 0)
			++m_uwpWorkloadProfile.submitReasons[i];
#else
	(void)workloadReasons;
#endif
#if defined(LATTE_UWP_DETAILED_PROFILING)
	const auto submitProfileStarted = std::chrono::steady_clock::now();
	auto submitProfilePhaseStarted = submitProfileStarted;
	auto finishSubmitProfilePhase = [&](size_t phaseIndex)
	{
		const auto now = std::chrono::steady_clock::now();
		const uint64 elapsedNs = (uint64)std::chrono::duration_cast<std::chrono::nanoseconds>(
			now - submitProfilePhaseStarted).count();
		auto& profile = m_uwpCommandSubmitProfile;
		profile.phaseTimeNs[phaseIndex] += elapsedNs;
		profile.phaseMaxNs[phaseIndex] = std::max(profile.phaseMaxNs[phaseIndex], elapsedNs);
		submitProfilePhaseStarted = now;
		return now;
	};
#endif
	auto& slot = GetCurrentCommandSlot();
	OcclusionQueryNotifyEndCommandList();
#if defined(CEMU_UWP_D3D12_DIAGNOSTICS) || defined(CEMU_UWP_D3D12_WORKLOAD_PROFILE) || defined(CEMU_UWP)
	if (m_timestampQueryHeap && m_timestampReadback.resource && m_timestampReadbackMapped)
	{
		const uint32 queryIndex = 2 * m_commandSlotIndex;
		slot.commandList->EndQuery(m_timestampQueryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, queryIndex + 1);
		slot.commandList->ResolveQueryData(m_timestampQueryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP,
			queryIndex, 2, m_timestampReadback.resource.Get(), (uint64)m_commandSlotIndex * 2 * sizeof(uint64));
		slot.timingPending = true;
	}
#endif
	D3D12TraceEventFlush("cmd_close", "slot={} recordedDraws={} gameDraws={} submitThreshold={} activeFbo=0x{:016x}",
						 m_commandSlotIndex,
						 m_recordedDrawcalls,
						 (unsigned long long)m_gameDrawCallsSubmitted,
						 m_submitThreshold,
						 m_activeFBO ? m_activeFBO->key : 0);
#if defined(LATTE_UWP_DETAILED_PROFILING)
	finishSubmitProfilePhase(0);
#endif
	const HRESULT closeHr = slot.commandList->Close();
	if (FAILED(closeHr))
	{
		cemuLog_log(LogType::Force, "D3D12: Close command list failed with HRESULT 0x{:08x}.", (uint32)closeHr);
		LogInfoQueueMessages("Close command list", 64);
		CheckDeviceRemoved(closeHr, "Close command list");
		throw std::runtime_error("Failed to close D3D12 command list");
	}
#if defined(LATTE_UWP_DETAILED_PROFILING)
	finishSubmitProfilePhase(1);
#endif
	ID3D12CommandList* lists[] = { slot.commandList.Get() };
	m_directQueue->ExecuteCommandLists(1, lists);
	if (ShouldCheckDevice(false) && FAILED(m_device->GetDeviceRemovedReason()))
		ReportDeviceRemovalAfter("ExecuteCommandLists", "submit", fmt::format("fence={} draws={}", m_nextFenceValue, m_recordedDrawcalls));
#if defined(CEMU_UWP_D3D12_DIAGNOSTICS)
	LogInfoQueueMessages("ExecuteCommandLists", 16);
#endif
	const uint64 fenceValue = m_nextFenceValue++;
	const HRESULT signalHr = m_directQueue->Signal(m_fence.Get(), fenceValue);
	if (FAILED(signalHr))
	{
		CheckDeviceRemoved(signalHr, "Signal");
		throw std::runtime_error("Failed to signal D3D12 fence");
	}
#if defined(LATTE_UWP_DETAILED_PROFILING)
	finishSubmitProfilePhase(2);
#endif
	slot.fenceValue = fenceValue;
	m_uwpLastSubmittedFence = fenceValue;
	m_numSubmittedCommandLists++;
	m_gpuWatchdog.Submit(fenceValue, m_recordedDrawcalls);
	// retained feedback copies only live within one command list
	DropRetainedFeedbackCopies();
	// Retire copies recorded before a rejected PSO lookup.
	const bool releaseScratchCopies = !m_feedbackScratchCopies.empty() || !m_commandScratchTextures.empty();
	const size_t pendingScratchCopyCount = m_feedbackScratchCopies.size() + m_commandScratchTextures.size();
	D3D12TraceEventFlush("cmd_submit", "slot={} fence={} submittedLists={} recordedDraws={} feedbackScratchPending={} feedbackScratchReleased={}",
						 m_commandSlotIndex,
						 (unsigned long long)fenceValue,
						 (unsigned long long)m_numSubmittedCommandLists,
						 m_recordedDrawcalls,
						 pendingScratchCopyCount,
						 releaseScratchCopies ? pendingScratchCopyCount : 0);
	m_commandListOpen = false;
	ResetCommandStateCache();
	if (releaseScratchCopies)
	{
		for (auto& scratch : m_feedbackScratchCopies)
		{
			if (scratch.texture.resource)
			{
				RetireScratchTexture(std::move(scratch.texture), scratch.requestedDesc, fenceValue);
				continue;
			}
			QueueDeferredRelease(std::move(scratch.texture));
		}
		m_feedbackScratchCopies.clear();
		for (auto& scratch : m_commandScratchTextures)
			RetireScratchTexture(std::move(scratch.texture), scratch.desc, fenceValue);
		m_commandScratchTextures.clear();
		ResetScratchCopyLookup();
	}
#if defined(LATTE_UWP_DETAILED_PROFILING)
	finishSubmitProfilePhase(3);
#endif

	m_commandSlotIndex = (m_commandSlotIndex + 1) % kCommandBufferPoolSize;
	ProcessFinishedCommandLists();
	WaitForCommandSlot(m_commandSlotIndex);

	m_recordedDrawcalls = 0;
	m_submitThreshold = GetDefaultSubmitThreshold();
	m_submitOnIdle = false;
#if defined(LATTE_UWP_DETAILED_PROFILING)
	const auto submitProfileFinished = finishSubmitProfilePhase(4);
	const uint64 totalNs = (uint64)std::chrono::duration_cast<std::chrono::nanoseconds>(
		submitProfileFinished - submitProfileStarted).count();
	++m_uwpCommandSubmitProfile.submitCount;
	m_uwpCommandSubmitProfile.totalTimeNs += totalNs;
	m_uwpCommandSubmitProfile.totalMaxNs = std::max(m_uwpCommandSubmitProfile.totalMaxNs, totalNs);
#endif
}

void D3D12Renderer::WaitForIdle(std::source_location caller)
{
	m_uwpIdleWaitLine = caller.line();
	if (!m_directQueue || !m_fence)
		return;
	ExecuteCurrentCommandList(SubmitWaitIdle);
	const uint64 fenceValue = m_nextFenceValue++;
	const HRESULT signalHr = m_directQueue->Signal(m_fence.Get(), fenceValue);
	if (FAILED(signalHr))
	{
		CheckDeviceRemoved(signalHr, "Signal idle fence");
		throw std::runtime_error("Failed to signal D3D12 idle fence");
	}
	if (!WaitForFenceValue(fenceValue, "idle fence"))
		throw std::runtime_error("D3D12 device stopped while waiting for idle");
	m_lastCompletedFenceValue = m_fence->GetCompletedValue();
	ProcessFinishedCommandLists();
	ProcessDestructionQueue();
}

void D3D12Renderer::WaitForFence(uint64 fenceValue)
{
	if (!m_fence || fenceValue == 0)
		return;
	if (!WaitForFenceValue(fenceValue, "fence"))
		throw std::runtime_error("D3D12 device stopped while waiting for GPU work");
	ProcessFinishedCommandLists();
}

void D3D12Renderer::WaitForCommandSlot(uint32 slotIndex)
{
	auto& slot = m_commandSlots[slotIndex];
	if (slot.fenceValue == 0)
		return;
	if (const uint64 completed = m_fence->GetCompletedValue(); slot.fenceValue > completed)
	{
		const size_t queued = std::count_if(m_uwpPresentFences.begin(), m_uwpPresentFences.end(), [&](uint64 f) { return f > completed; });
		++m_uwpSlotWaitsByQueuedPresents[std::min<size_t>(queued, 3)];
	}

	if (!WaitForFenceValue(slot.fenceValue, "command allocator fence"))
		throw std::runtime_error("D3D12 device stopped while waiting for a command allocator");
	m_lastCompletedFenceValue = m_fence->GetCompletedValue();
	ProcessFinishedCommandLists();
}

bool D3D12Renderer::WaitForFenceValue(uint64 fenceValue, std::string_view operation)
{
	if (!m_fence || fenceValue == 0)
		return true;

	constexpr auto kMaximumWait = std::chrono::seconds(30);
	const auto deadline = std::chrono::steady_clock::now() + kMaximumWait;
	uint64 completedValue = m_fence->GetCompletedValue();
	if (completedValue == std::numeric_limits<uint64>::max())
	{
		const HRESULT removedReason = m_device ? m_device->GetDeviceRemovedReason() : DXGI_ERROR_DEVICE_REMOVED;
		LogDeviceRemovedData(removedReason, operation);
		return false;
	}
	if (completedValue >= fenceValue)
		return true;
	D3D12GpuWatchdog::ActivityScope watchdogActivity(m_gpuWatchdog, D3D12GpuWatchdog::Activity::FenceWait, fenceValue);
#if defined(LATTE_UWP_DETAILED_PROFILING)
	LatteUwpHitchScope hitchScope(LatteUwpHitchOp::QueueWait);
	const auto waitStarted = std::chrono::steady_clock::now();
	stdx::scope_exit waitTimer([&]()
	{
		const uint64 elapsedUs = (uint64)std::chrono::duration_cast<std::chrono::microseconds>(
			std::chrono::steady_clock::now() - waitStarted).count();
		m_fenceWaitCount++;
		m_fenceWaitTimeUs += elapsedUs;
		const size_t kind = operation == "idle fence" ? 0 : operation == "command allocator fence" ? 1 : operation == "fence" ? 2 : 3;
		m_uwpWaitUs[kind] += elapsedUs;
		++m_uwpWaitCount[kind];
		if (kind == 0)
		{
			auto& byLine = m_uwpIdleWaitByLine[m_uwpIdleWaitLine];
			++byLine.first;
			byLine.second += elapsedUs;
		}
		m_fenceWaitMaxUs = std::max(m_fenceWaitMaxUs, elapsedUs);
	});
#endif
	while (completedValue < fenceValue)
	{
		const HRESULT eventHr = m_fence->SetEventOnCompletion(fenceValue, m_fenceEvent);
		if (FAILED(eventHr))
		{
			LogDeviceRemovedData(eventHr, operation);
			return false;
		}

		const DWORD waitResult = WaitForSingleObject(m_fenceEvent, 1000);
		if (waitResult == WAIT_OBJECT_0)
		{
			completedValue = m_fence->GetCompletedValue();
			if (completedValue == std::numeric_limits<uint64>::max())
			{
				const HRESULT fenceRemovedReason = m_device ? m_device->GetDeviceRemovedReason() : DXGI_ERROR_DEVICE_REMOVED;
				LogDeviceRemovedData(fenceRemovedReason, operation);
				return false;
			}
			continue;
		}
		if (waitResult == WAIT_FAILED)
		{
			cemuLog_log(LogType::Force, "D3D12: wait failed during {} with Win32 error {}.", operation, GetLastError());
			return false;
		}

		const HRESULT removedReason = m_device ? m_device->GetDeviceRemovedReason() : E_FAIL;
		if (FAILED(removedReason))
		{
			LogDeviceRemovedData(removedReason, operation);
			return false;
		}
		if (std::chrono::steady_clock::now() >= deadline)
		{
			cemuLog_log(LogType::Force, "D3D12: timed out waiting 30 seconds for {} (fence {}, completed {}).",
				operation,
				(unsigned long long)fenceValue,
				(unsigned long long)m_fence->GetCompletedValue());
			return false;
		}
		completedValue = m_fence->GetCompletedValue();
		if (completedValue == std::numeric_limits<uint64>::max())
		{
			const HRESULT fenceRemovedReason = m_device ? m_device->GetDeviceRemovedReason() : DXGI_ERROR_DEVICE_REMOVED;
			LogDeviceRemovedData(fenceRemovedReason, operation);
			return false;
		}
	}
	return true;
}

void D3D12Renderer::ProcessFinishedCommandLists()
{
	if (!m_fence)
		return;

	const uint64 completedFence = m_fence->GetCompletedValue();
	m_lastCompletedFenceValue = completedFence;
	while (true)
	{
		auto& slot = m_commandSlots[m_commandSlotSyncIndex];
		if (slot.fenceValue == 0 || slot.fenceValue > completedFence)
			break;

#if defined(CEMU_UWP_D3D12_DIAGNOSTICS) || defined(CEMU_UWP_D3D12_WORKLOAD_PROFILE) || defined(CEMU_UWP)
		if (slot.timingPending && m_timestampReadbackMapped)
		{
			// UINT64_MAX is the device-removal sentinel, not a completed resolve
			if (completedFence != std::numeric_limits<uint64>::max())
			{
				const uint64 start = m_timestampReadbackMapped[(size_t)m_commandSlotSyncIndex * 2];
				const uint64 end = m_timestampReadbackMapped[(size_t)m_commandSlotSyncIndex * 2 + 1];
				if (end >= start)
				{
					const uint64 ticks = end - start;
					m_gpuCommandListSamples++;
					m_gpuCommandListTicks += ticks;
					m_gpuCommandListMaxTicks = std::max(m_gpuCommandListMaxTicks, ticks);
#if defined(CEMU_UWP_D3D12_WORKLOAD_PROFILE)
					m_workloadGpuWindowMaxTicks = std::max(m_workloadGpuWindowMaxTicks, ticks);
#endif
				}
#if defined(CEMU_UWP_D3D12_WORKLOAD_PROFILE)
				else
					++m_workloadGpuInvalidPairs;
#endif
			}
#if defined(CEMU_UWP_D3D12_WORKLOAD_PROFILE)
			else
				++m_workloadGpuInvalidPairs;
#endif
			slot.timingPending = false;
		}
#endif
		slot.fenceValue = 0;
		slot.uniformRingWatermark = 0;
		slot.readbackNotificationCount = 0;
		slot.queryFragmentCount = 0;
		m_commandSlotSyncIndex = (m_commandSlotSyncIndex + 1) % kCommandBufferPoolSize;
		m_numCompletedCommandLists++;
	}
	ProcessRetiredOcclusionQuerySlots();
	ProcessDestructionQueue();
}

void D3D12Renderer::ProcessDestructionQueue()
{
	std::scoped_lock lock(m_deferredReleaseMutex);
	while (!m_deferredReleaseQueue.empty() && m_deferredReleaseQueue.front().fenceValue <= m_lastCompletedFenceValue)
	{
		// UINT64_MAX is the device-removal sentinel; pending reads never completed
		if (m_descriptorManager && m_lastCompletedFenceValue != std::numeric_limits<uint64>::max())
		{
			for (auto& descriptorHeap : m_deferredReleaseQueue.front().descriptorHeaps)
				m_descriptorManager->RecycleShaderVisibleHeap(std::move(descriptorHeap));
		}
		m_deferredReleaseQueue.pop_front();
	}
	while (!m_retiredImguiDescriptors.empty() && m_retiredImguiDescriptors.front().readyFence <= m_lastCompletedFenceValue)
	{
		m_imguiFreeDescriptorIndices.emplace_back(m_retiredImguiDescriptors.front().index);
		m_retiredImguiDescriptors.pop_front();
	}
	if (m_memoryManager)
		m_memoryManager->Cleanup(m_lastCompletedFenceValue);
}

uint64 D3D12Renderer::GetRetirementFence() const
{
	return m_commandListOpen ? m_nextFenceValue : (m_nextFenceValue > 1 ? m_nextFenceValue - 1 : 0);
}

void D3D12Renderer::QueueDeferredRelease(Microsoft::WRL::ComPtr<IUnknown> object)
{
	if (!object)
		return;
	if (m_shutdownComplete)
		return;

	const uint64 releaseFence = GetRetirementFence();
	if (releaseFence == 0 || releaseFence <= m_lastCompletedFenceValue)
		return;

	std::scoped_lock lock(m_deferredReleaseMutex);
	if (!m_deferredReleaseQueue.empty() && m_deferredReleaseQueue.back().fenceValue == releaseFence)
	{
		m_deferredReleaseQueue.back().objects.emplace_back(std::move(object));
		return;
	}

	DeferredReleaseBatch batch;
	batch.fenceValue = releaseFence;
	batch.objects.emplace_back(std::move(object));
	m_deferredReleaseQueue.emplace_back(std::move(batch));
}

void D3D12Renderer::QueueDeferredRelease(D3D12MemoryManager::Resource&& resource)
{
	if (!resource.resource && !resource.allocation)
		return;
	if (m_shutdownComplete)
	{
		resource.Reset();
		return;
	}

	const uint64 releaseFence = GetRetirementFence();
	if (releaseFence == 0 || releaseFence <= m_lastCompletedFenceValue)
	{
		resource.Reset();
		return;
	}

	std::scoped_lock lock(m_deferredReleaseMutex);
	if (!m_deferredReleaseQueue.empty() && m_deferredReleaseQueue.back().fenceValue == releaseFence)
	{
		m_deferredReleaseQueue.back().resources.emplace_back(std::move(resource));
		return;
	}

	DeferredReleaseBatch batch;
	batch.fenceValue = releaseFence;
	batch.resources.emplace_back(std::move(resource));
	m_deferredReleaseQueue.emplace_back(std::move(batch));
}

void D3D12Renderer::QueueDeferredRelease(D3D12DescriptorManager::ShaderVisibleHeapPage&& descriptorHeap)
{
	if (!descriptorHeap.IsValid())
		return;
	if (m_shutdownComplete)
		return;

	const uint64 releaseFence = GetRetirementFence();
	if (releaseFence == 0 || releaseFence <= m_lastCompletedFenceValue)
	{
		if (m_descriptorManager && m_lastCompletedFenceValue != std::numeric_limits<uint64>::max())
			m_descriptorManager->RecycleShaderVisibleHeap(std::move(descriptorHeap));
		return;
	}

	std::scoped_lock lock(m_deferredReleaseMutex);
	if (!m_deferredReleaseQueue.empty() && m_deferredReleaseQueue.back().fenceValue == releaseFence)
	{
		m_deferredReleaseQueue.back().descriptorHeaps.emplace_back(std::move(descriptorHeap));
		return;
	}

	DeferredReleaseBatch batch;
	batch.fenceValue = releaseFence;
	batch.descriptorHeaps.emplace_back(std::move(descriptorHeap));
	m_deferredReleaseQueue.emplace_back(std::move(batch));
}

void D3D12Renderer::NotifyTextureViewDescriptorsReleased(const LatteTextureViewD3D12* view)
{
	if (!view)
		return;

	// alternate views can die while the base texture lives; drop raw view pointers first
	BumpAllTableBindingGenerations();
	for (auto& boundTexture : m_boundTextures)
	{
		if (boundTexture == view)
			boundTexture = nullptr;
	}
	for (auto& cachedOverlap : m_feedbackOverlapCache)
	{
		if (cachedOverlap.sampledView == view || cachedOverlap.attachment == view)
			cachedOverlap = {};
	}
	for (auto& scratch : m_feedbackScratchCopies)
	{
		if (scratch.sampledView == view)
			scratch.sampledView = nullptr;
	}
	if (std::any_of(m_retainedFeedbackCopies.begin(), m_retainedFeedbackCopies.end(),
		[view](const RetainedFeedbackCopy& retained) { return retained.scratch.sampledView == view; }))
		DropRetainedFeedbackCopies();

	// Invalidate cached render-target handles before recycling descriptor slots.
	if (!m_boundRenderTargetsValid)
		return;
	bool matches = false;
	const auto& rtv = view->GetRTV();
	if (rtv.IsValid())
	{
		for (UINT i = 0; i < m_boundRtvCount && !matches; ++i)
			matches = m_boundRtvHandles[i].ptr == rtv.cpu.ptr;
	}
	const auto& dsv = view->GetDSV();
	if (!matches && dsv.IsValid())
		matches = m_boundDsvHandle.ptr == dsv.cpu.ptr;
	if (matches)
		m_boundRenderTargetsValid = false;
}

void D3D12Renderer::ReleaseResourceDeferred(D3D12MemoryManager::Resource& resource)
{
	QueueDeferredRelease(std::move(resource));
}

uint32 D3D12Renderer::GetDefaultSubmitThreshold() const
{
	return kSubmitThresholdUwpDefault;
}

void D3D12Renderer::EnsureShaderVisibleDescriptorSpace(uint32 cbvSrvUavCount, uint32 samplerCount)
{
	if (!m_descriptorManager)
		return;

	const uint32 cbvSrvUavCapacity = m_descriptorManager->GetShaderVisibleCbvSrvUavCapacity();
	const uint32 samplerCapacity = m_descriptorManager->GetShaderVisibleSamplerCapacity();
	if (cbvSrvUavCount > cbvSrvUavCapacity || samplerCount > samplerCapacity)
		throw std::runtime_error("D3D12 shader-visible descriptor request exceeds heap capacity");

	if (m_descriptorManager->GetShaderVisibleCbvSrvUavUsed() + cbvSrvUavCount <= cbvSrvUavCapacity &&
		m_descriptorManager->GetShaderVisibleSamplerUsed() + samplerCount <= samplerCapacity)
		return;

	const auto stats = m_descriptorManager->GetStats();
	LatteUwpHitchScope hitchScope(LatteUwpHitchOp::DescriptorRollover);
	static uint32 s_descriptorRolloverLogCount = 0;
	if (D3D12RuntimeLogCapped(s_descriptorRolloverLogCount, "shader-visible descriptor rollover", 32))
	{
		cemuLog_log(LogType::Force,
					"D3D12: shader-visible descriptor heap rollover creates a new heap page (CBV/SRV/UAV {}/{} + {}, sampler {}/{} + {}).",
					stats.shaderVisibleCbvSrvUavUsed,
					stats.shaderVisibleCbvSrvUavCapacity,
					cbvSrvUavCount,
					stats.shaderVisibleSamplerUsed,
					stats.shaderVisibleSamplerCapacity,
					samplerCount);
	}
	D3D12TraceEvent("descriptor_rollover", "cbvSrvUavUsed={} cbvSrvUavCapacity={} cbvSrvUavRequest={} samplerUsed={} samplerCapacity={} samplerRequest={} rolloversBefore={}",
					stats.shaderVisibleCbvSrvUavUsed,
					stats.shaderVisibleCbvSrvUavCapacity,
					cbvSrvUavCount,
					stats.shaderVisibleSamplerUsed,
					stats.shaderVisibleSamplerCapacity,
					samplerCount,
					stats.shaderVisibleRollovers);

	const bool rollCbvSrvUav = stats.shaderVisibleCbvSrvUavUsed + cbvSrvUavCount > stats.shaderVisibleCbvSrvUavCapacity;
	const bool rollSampler = stats.shaderVisibleSamplerUsed + samplerCount > stats.shaderVisibleSamplerCapacity;
	std::vector<D3D12DescriptorManager::ShaderVisibleHeapPage> retiredHeaps;
	m_descriptorManager->RollShaderVisibleHeaps(m_device.Get(), retiredHeaps, rollCbvSrvUav, rollSampler);
	for (auto& heap : retiredHeaps)
		QueueDeferredRelease(std::move(heap));
	if (rollCbvSrvUav)
	{
#if defined(LATTE_UWP_DETAILED_PROFILING)
		const auto clearStarted = std::chrono::steady_clock::now();
#endif
		InvalidateGameCbvSrvUavDescriptorCaches();
#if defined(LATTE_UWP_DETAILED_PROFILING)
		const uint64 clearNs = (uint64)std::chrono::duration_cast<std::chrono::nanoseconds>(
			std::chrono::steady_clock::now() - clearStarted).count();
		m_uwpDescriptorResourceCacheClearCount++;
		m_uwpDescriptorResourceCacheClearTimeNs += clearNs;
		m_uwpDescriptorResourceCacheClearMaxNs = std::max(m_uwpDescriptorResourceCacheClearMaxNs, clearNs);
#endif
	}
	if (rollSampler)
	{
#if defined(LATTE_UWP_DETAILED_PROFILING)
		const auto clearStarted = std::chrono::steady_clock::now();
#endif
		InvalidateGameSamplerDescriptorCaches();
#if defined(LATTE_UWP_DETAILED_PROFILING)
		const uint64 clearNs = (uint64)std::chrono::duration_cast<std::chrono::nanoseconds>(
			std::chrono::steady_clock::now() - clearStarted).count();
		m_uwpDescriptorSamplerCacheClearCount++;
		m_uwpDescriptorSamplerCacheClearTimeNs += clearNs;
		m_uwpDescriptorSamplerCacheClearMaxNs = std::max(m_uwpDescriptorSamplerCacheClearMaxNs, clearNs);
#endif
	}
	m_boundShaderVisibleHeapGeneration = kInvalidDescriptorIndex;
}

void D3D12Renderer::InvalidateGameRootParameterCache(bool descriptorTablesOnly)
{
	m_gameRootDescriptorTableValid.fill(false);
	if (descriptorTablesOnly)
		return;
	m_gameRootCbvValid.fill(false);
	m_gameRootDrawConstantsValid = false;
}

void D3D12Renderer::ResetCommandStateCache()
{
	m_boundShaderVisibleHeapGeneration = kInvalidDescriptorIndex;
	m_boundPipelineState = nullptr;
	m_boundGraphicsRootSignature = nullptr;
	m_boundPrimitiveTopology = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
	m_boundViewportValid = false;
	m_boundDepthBiasValid = false;
	m_boundScissorValid = false;
	m_boundRenderTargetsValid = false;
	m_boundRtvCount = 0;
	m_boundDsvHandle = {};
	for (auto& handle : m_boundRtvHandles)
		handle = {};
	m_boundBlendFactorValid = false;
	m_boundStencilRefValid = false;
	m_boundIndexBufferValid = false;
	InvalidateGameRootParameterCache();
	// ring addresses are only valid within one command list; the prepared bytes stay
	for (auto& slot : m_gameUniformUploadCache)
		slot.gpuAddress = 0;
}

void D3D12Renderer::BindShaderVisibleHeaps()
{
	if (!m_descriptorManager || !m_commandListOpen)
		return;

	const uint32 heapGeneration = m_descriptorManager->GetShaderVisibleRollovers();
	if (m_boundShaderVisibleHeapGeneration == heapGeneration)
	{
		m_shaderVisibleHeapBindSkips++;
		return;
	}

	m_descriptorManager->BindShaderVisibleHeaps(GetCurrentCommandSlot().commandList.Get());
	m_shaderVisibleHeapBinds++;
	m_boundShaderVisibleHeapGeneration = heapGeneration;
	InvalidateGameRootParameterCache(true);
}

void D3D12Renderer::SetD3D12PipelineState(ID3D12PipelineState* pipeline)
{
	if (!pipeline || !m_commandListOpen)
		return;
	if (m_boundPipelineState == pipeline)
	{
		m_commandStateBindSkips++;
		return;
	}
	GetCurrentCommandSlot().commandList->SetPipelineState(pipeline);
	m_boundPipelineState = pipeline;
	// a pipeline with static depth bias may leave the dynamic value undefined
	m_boundDepthBiasValid = false;
	m_commandStateBinds++;
}

void D3D12Renderer::SetD3D12GraphicsRootSignature(ID3D12RootSignature* rootSignature)
{
	if (!rootSignature || !m_commandListOpen)
		return;
	if (m_boundGraphicsRootSignature == rootSignature)
	{
		m_commandStateBindSkips++;
		return;
	}
	GetCurrentCommandSlot().commandList->SetGraphicsRootSignature(rootSignature);
	m_boundGraphicsRootSignature = rootSignature;
	InvalidateGameRootParameterCache();
	m_commandStateBinds++;
}

void D3D12Renderer::SetD3D12PrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY topology)
{
	if (!m_commandListOpen)
		return;
	if (m_boundPrimitiveTopology == topology)
	{
		m_commandStateBindSkips++;
		return;
	}
	GetCurrentCommandSlot().commandList->IASetPrimitiveTopology(topology);
	m_boundPrimitiveTopology = topology;
	m_commandStateBinds++;
}

void D3D12Renderer::SetD3D12Viewport(const D3D12_VIEWPORT& viewport)
{
	if (!m_commandListOpen)
		return;
	if (m_boundViewportValid && std::memcmp(&m_boundViewport, &viewport, sizeof(viewport)) == 0)
	{
		m_commandStateBindSkips++;
		return;
	}
	GetCurrentCommandSlot().commandList->RSSetViewports(1, &viewport);
	m_boundViewport = viewport;
	m_boundViewportValid = true;
	m_commandStateBinds++;
}

void D3D12Renderer::SetD3D12DepthBias(float depthBias, float depthBiasClamp, float slopeScaledDepthBias)
{
#if defined(__ID3D12GraphicsCommandList9_INTERFACE_DEFINED__)
	if (!m_commandListOpen)
		return;
	auto* commandList9 = GetCurrentCommandSlot().commandList9.Get();
	if (!commandList9)
		return;
	const std::array<float, 3> depthBiasState{ depthBias, depthBiasClamp, slopeScaledDepthBias };
	if (m_boundDepthBiasValid && std::memcmp(m_boundDepthBias.data(), depthBiasState.data(), sizeof(depthBiasState)) == 0)
	{
		m_commandStateBindSkips++;
		return;
	}
	commandList9->RSSetDepthBias(depthBias, depthBiasClamp, slopeScaledDepthBias);
	m_boundDepthBias = depthBiasState;
	m_boundDepthBiasValid = true;
	m_commandStateBinds++;
#else
	(void)depthBias;
	(void)depthBiasClamp;
	(void)slopeScaledDepthBias;
#endif
}

void D3D12Renderer::SetD3D12Scissor(const D3D12_RECT& scissor)
{
	if (!m_commandListOpen)
		return;
	if (m_boundScissorValid && std::memcmp(&m_boundScissor, &scissor, sizeof(scissor)) == 0)
	{
		m_commandStateBindSkips++;
		return;
	}
	GetCurrentCommandSlot().commandList->RSSetScissorRects(1, &scissor);
	m_boundScissor = scissor;
	m_boundScissorValid = true;
	m_commandStateBinds++;
}

void D3D12Renderer::SetD3D12BlendFactor(const float (&blendFactor)[4])
{
	if (!m_commandListOpen)
		return;
	if (m_boundBlendFactorValid && std::memcmp(m_boundBlendFactor.data(), blendFactor, sizeof(blendFactor)) == 0)
	{
		m_commandStateBindSkips++;
		return;
	}
	GetCurrentCommandSlot().commandList->OMSetBlendFactor(blendFactor);
	std::copy(std::begin(blendFactor), std::end(blendFactor), m_boundBlendFactor.begin());
	m_boundBlendFactorValid = true;
	m_commandStateBinds++;
}

void D3D12Renderer::SetD3D12StencilRef(UINT frontRef, UINT backRef)
{
	if (!m_commandListOpen)
		return;
	auto& slot = GetCurrentCommandSlot();
#if defined(__ID3D12GraphicsCommandList8_INTERFACE_DEFINED__)
	if (frontRef != backRef && !slot.commandList8)
		backRef = frontRef;
#else
	backRef = frontRef;
#endif
	if (m_boundStencilRefValid && m_boundStencilRef[0] == frontRef && m_boundStencilRef[1] == backRef)
	{
		m_commandStateBindSkips++;
		return;
	}
#if defined(__ID3D12GraphicsCommandList8_INTERFACE_DEFINED__)
	if (frontRef != backRef)
		slot.commandList8->OMSetFrontAndBackStencilRef(frontRef, backRef);
	else
#endif
		slot.commandList->OMSetStencilRef(frontRef);
	m_boundStencilRef = { frontRef, backRef };
	m_boundStencilRefValid = true;
	m_commandStateBinds++;
}

void D3D12Renderer::SetD3D12IndexBuffer(const D3D12_INDEX_BUFFER_VIEW& indexView)
{
	if (!m_commandListOpen)
		return;
	if (m_boundIndexBufferValid &&
		m_boundIndexBuffer.BufferLocation == indexView.BufferLocation &&
		m_boundIndexBuffer.SizeInBytes == indexView.SizeInBytes &&
		m_boundIndexBuffer.Format == indexView.Format)
	{
		m_commandStateBindSkips++;
		return;
	}
	GetCurrentCommandSlot().commandList->IASetIndexBuffer(&indexView);
	m_boundIndexBuffer = indexView;
	m_boundIndexBufferValid = true;
	m_commandStateBinds++;
}

void D3D12Renderer::SetD3D12RenderTargets(UINT rtvCount, const D3D12_CPU_DESCRIPTOR_HANDLE* rtvHandles, const D3D12_CPU_DESCRIPTOR_HANDLE* dsvHandle)
{
	if (!m_commandListOpen)
		return;

	bool matches = m_boundRenderTargetsValid && m_boundRtvCount == rtvCount && m_boundDsvHandle.ptr == (dsvHandle ? dsvHandle->ptr : 0);
	if (matches)
	{
		for (UINT i = 0; i < rtvCount; ++i)
		{
			const size_t ptr = rtvHandles ? rtvHandles[i].ptr : 0;
			if (m_boundRtvHandles[i].ptr != ptr)
			{
				matches = false;
				break;
			}
		}
	}
	if (matches)
	{
		m_commandStateBindSkips++;
		return;
	}

	GetCurrentCommandSlot().commandList->OMSetRenderTargets(rtvCount, rtvCount != 0 ? rtvHandles : nullptr, FALSE, dsvHandle);
	m_boundRtvCount = rtvCount;
	m_boundDsvHandle.ptr = dsvHandle ? dsvHandle->ptr : 0;
	for (UINT i = 0; i < std::size(m_boundRtvHandles); ++i)
		m_boundRtvHandles[i].ptr = (i < rtvCount && rtvHandles) ? rtvHandles[i].ptr : 0;
	m_boundRenderTargetsValid = true;
	m_commandStateBinds++;
}

void D3D12Renderer::Transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES& state, D3D12_RESOURCE_STATES newState)
{
	if (state == newState)
		return;
	D3D12_RESOURCE_BARRIER barrier{};
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Transition.pResource = resource;
	barrier.Transition.StateBefore = state;
	barrier.Transition.StateAfter = newState;
	barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	GetCurrentCommandSlot().commandList->ResourceBarrier(1, &barrier);
	m_transitionBarrierCalls++;
	m_transitionBarrierCount++;
	state = newState;
}

void D3D12Renderer::TransitionTextureSubresource(LatteTextureD3D12* texture, uint32 subresource, D3D12_RESOURCE_STATES newState)
{
	if (!texture || !texture->GetResource() || subresource >= texture->GetSubresourceCount())
		return;

	const D3D12_RESOURCE_STATES oldState = texture->GetSubresourceState(subresource);
	if (oldState == newState)
		return;

	D3D12_RESOURCE_BARRIER barrier{};
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Transition.pResource = texture->GetResource();
	barrier.Transition.StateBefore = oldState;
	barrier.Transition.StateAfter = newState;
	barrier.Transition.Subresource = subresource;
	GetCurrentCommandSlot().commandList->ResourceBarrier(1, &barrier);
	m_transitionBarrierCalls++;
	m_transitionBarrierCount++;
	texture->SetSubresourceState(subresource, newState);
}

void D3D12Renderer::TransitionTextureSubresources(LatteTextureD3D12* texture, const std::vector<TextureTransition>& transitions)
{
	if (!texture || !texture->GetResource() || transitions.empty())
		return;

	auto& barriers = m_transitionBarrierScratch;
	barriers.clear();
	barriers.reserve(transitions.size());
	for (const auto& transition : transitions)
	{
		if (transition.subresource >= texture->GetSubresourceCount())
			continue;
		const D3D12_RESOURCE_STATES oldState = texture->GetSubresourceState(transition.subresource);
		const auto duplicate = std::find_if(barriers.begin(), barriers.end(),
			[&](const D3D12_RESOURCE_BARRIER& barrier)
			{
				return barrier.Transition.Subresource == transition.subresource;
			});
		if (duplicate != barriers.end())
		{
			duplicate->Transition.StateAfter = transition.state;
			continue;
		}
		if (oldState == transition.state)
			continue;

		D3D12_RESOURCE_BARRIER barrier{};
		barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		barrier.Transition.pResource = texture->GetResource();
		barrier.Transition.StateBefore = oldState;
		barrier.Transition.StateAfter = transition.state;
		barrier.Transition.Subresource = transition.subresource;
		barriers.emplace_back(barrier);
	}

	barriers.erase(std::remove_if(barriers.begin(), barriers.end(),
		[](const D3D12_RESOURCE_BARRIER& barrier)
		{
			return barrier.Transition.StateBefore == barrier.Transition.StateAfter;
		}), barriers.end());
	if (barriers.empty())
		return;
	GetCurrentCommandSlot().commandList->ResourceBarrier((UINT)barriers.size(), barriers.data());
	m_transitionBarrierCalls++;
	m_transitionBarrierCount += barriers.size();
	for (const auto& barrier : barriers)
		texture->SetSubresourceState(barrier.Transition.Subresource, barrier.Transition.StateAfter);
}

void D3D12Renderer::TransitionTextureView(LatteTextureViewD3D12* view, D3D12_RESOURCE_STATES newState, bool includeStencilPlane)
{
	if (!view)
		return;
	auto* texture = view->GetBaseImage();
	if (!texture || !texture->GetResource())
		return;

	const bool useSatisfiedFastPath = PerformanceExperimentEnabled(D3D12PerformanceExperiment::SatisfiedViewTransition);
#if defined(CEMU_UWP_PERFORMANCE_EXPERIMENT)
	const bool alreadySatisfied = texture->AllSubresourcesInState(newState);
	if (alreadySatisfied)
		++m_performanceExperimentWindow.satisfiedTransitionOpportunities;
#else
	const bool alreadySatisfied = useSatisfiedFastPath && texture->AllSubresourcesInState(newState);
#endif
	if (useSatisfiedFastPath && alreadySatisfied)
	{
#if defined(CEMU_UWP_PERFORMANCE_EXPERIMENT)
		++m_performanceExperimentWindow.satisfiedTransitionHits;
#endif
		++m_uwpViewTransitionsFast;
		return;
	}
	++m_uwpViewTransitionsSlow;

	const uint32 mipLevels = std::max<uint32>(texture->GetDesc().MipLevels, 1);
	const uint32 firstMip = (uint32)std::clamp<sint32>(view->firstMip, 0, (sint32)mipLevels - 1);
	const uint32 mipCount = std::min<uint32>(std::max<sint32>(view->numMip, 1), mipLevels - firstMip);
	const uint32 arraySize = texture->Is3DTexture() ? 1u : std::max<uint32>(texture->GetArraySize(), 1);
	const uint32 firstSlice = texture->Is3DTexture() ? 0u : (uint32)std::clamp<sint32>(view->firstSlice, 0, (sint32)arraySize - 1);
	const uint32 sliceCount = texture->Is3DTexture() ? 1u : std::min<uint32>(std::max<sint32>(view->numSlice, 1), arraySize - firstSlice);
	const uint32 planeCount = includeStencilPlane && texture->GetFormatInfo().hasStencil ? 2u : 1u;
	const uint32 planeStride = mipLevels * arraySize;

	D3D12ViewStateRanges::Range viewRange;
	if (PerformanceExperimentEnabled(D3D12PerformanceExperiment::ViewStateRanges) &&
		D3D12ViewStateRanges::Range::TryCreate(firstMip, mipCount, mipLevels,
			firstSlice, sliceCount, arraySize, planeCount, texture->GetSubresourceCount(), viewRange))
	{
		// clamped range has no duplicate subresources, so no dedup is needed
		auto& barriers = m_transitionBarrierScratch;
		barriers.clear();
		barriers.reserve(std::min<size_t>(viewRange.Count(), texture->GetSubresourceCount()));
		viewRange.ForEach([&](uint32 subresource)
		{
			const D3D12_RESOURCE_STATES oldState = texture->GetSubresourceState(subresource);
			if (oldState == newState)
				return;
			D3D12_RESOURCE_BARRIER barrier{};
			barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			barrier.Transition.pResource = texture->GetResource();
			barrier.Transition.StateBefore = oldState;
			barrier.Transition.StateAfter = newState;
			barrier.Transition.Subresource = subresource;
			barriers.emplace_back(barrier);
		});
		if (!barriers.empty())
		{
			GetCurrentCommandSlot().commandList->ResourceBarrier((UINT)barriers.size(), barriers.data());
			m_transitionBarrierCalls++;
			m_transitionBarrierCount += barriers.size();
			for (const auto& barrier : barriers)
				texture->SetSubresourceState(barrier.Transition.Subresource, barrier.Transition.StateAfter);
			texture->TryRestoreUniformState(newState);
		}
		return;
	}
	m_textureTransitionScratch.clear();
	m_textureTransitionScratch.reserve((size_t)planeCount * sliceCount * mipCount);
	for (uint32 plane = 0; plane < planeCount; ++plane)
	{
		for (uint32 slice = 0; slice < sliceCount; ++slice)
		{
			for (uint32 mip = 0; mip < mipCount; ++mip)
			{
				const uint32 subresource = firstMip + mip + (firstSlice + slice) * mipLevels + plane * planeStride;
				m_textureTransitionScratch.push_back({ subresource, newState });
			}
		}
	}
	TransitionTextureSubresources(texture, m_textureTransitionScratch);
	texture->TryRestoreUniformState(newState);
}

void D3D12Renderer::TransitionTextureAll(LatteTextureD3D12* texture, D3D12_RESOURCE_STATES newState)
{
	if (!texture || !texture->GetResource())
		return;
	if (texture->AllSubresourcesInState(newState))
		return;

	const uint32 subresourceCount = texture->GetSubresourceCount();
	const D3D12_RESOURCE_STATES firstState = texture->GetSubresourceState(0);
	bool uniformOldState = true;
	for (uint32 subresource = 1; subresource < subresourceCount; ++subresource)
	{
		if (texture->GetSubresourceState(subresource) != firstState)
		{
			uniformOldState = false;
			break;
		}
	}
	if (uniformOldState)
	{
		D3D12_RESOURCE_BARRIER barrier{};
		barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		barrier.Transition.pResource = texture->GetResource();
		barrier.Transition.StateBefore = firstState;
		barrier.Transition.StateAfter = newState;
		barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		GetCurrentCommandSlot().commandList->ResourceBarrier(1, &barrier);
		m_transitionBarrierCalls++;
		m_transitionBarrierCount++;
		texture->SetAllSubresourceStates(newState);
		return;
	}

	auto& barriers = m_transitionBarrierScratch;
	barriers.clear();
	barriers.reserve(texture->GetSubresourceCount());
	for (uint32 subresource = 0; subresource < texture->GetSubresourceCount(); subresource++)
	{
		const D3D12_RESOURCE_STATES oldState = texture->GetSubresourceState(subresource);
		if (oldState == newState)
			continue;

		D3D12_RESOURCE_BARRIER barrier{};
		barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		barrier.Transition.pResource = texture->GetResource();
		barrier.Transition.StateBefore = oldState;
		barrier.Transition.StateAfter = newState;
		barrier.Transition.Subresource = subresource;
		barriers.emplace_back(barrier);
	}

	if (!barriers.empty())
	{
		GetCurrentCommandSlot().commandList->ResourceBarrier((UINT)barriers.size(), barriers.data());
		m_transitionBarrierCalls++;
		m_transitionBarrierCount += barriers.size();
	}
	texture->SetAllSubresourceStates(newState);
}

#if defined(CEMU_UWP)
uint64 D3D12Renderer::GetTextureAllocationSize(const D3D12_RESOURCE_DESC& desc) const
{
	if (!m_device)
		return 0;

	const D3D12_RESOURCE_ALLOCATION_INFO allocationInfo = m_device->GetResourceAllocationInfo(0, 1, &desc);
	return allocationInfo.SizeInBytes;
}

size_t D3D12Renderer::EvictColdGamePipelines(uint32 minAgeFrames, size_t maxPipelines)
{
	const uint32 currentFrame = LatteGPUState.frameCounter;
	std::vector<std::pair<uint32, uint64>> candidates; // age, key
	auto& pipelineStableCache = D3D12PipelineStableCache::GetInstance();
	for (const auto& [key, entry] : m_gamePipelineCache)
	{
		// failed entries stay so they are not retried every draw
		if (!entry.pipeline)
			continue;
		const uint32 age = currentFrame - entry.lastUsedFrame;
		// kept: a draw that cannot be skipped rebuilds it on the render thread (BOTW: 0.14-1.2 s each when rain
		// returned after the memory guard had evicted its PSOs)
		if (age >= minAgeFrames && !pipelineStableCache.IsMustBeReady(key))
			candidates.emplace_back(age, key);
	}
	const size_t count = std::min(maxPipelines, candidates.size());
	if (count == 0)
		return 0;
	std::partial_sort(candidates.begin(), candidates.begin() + count, candidates.end(),
		[](const auto& lhs, const auto& rhs) { return lhs.first > rhs.first; });
	for (size_t i = 0; i < count; ++i)
	{
		auto it = m_gamePipelineCache.find(candidates[i].second);
		if (m_boundPipelineState == it->second.pipeline.Get())
			m_boundPipelineState = nullptr;
		// the GPU may still reference it from submitted command lists
		QueueDeferredRelease(Microsoft::WRL::ComPtr<IUnknown>(it->second.pipeline));
		m_gamePipelineCache.erase(it);
	}
	m_gamePipelinesEvicted += count;
	return count;
}

D3D12Renderer::UwpTextureEvictionResult D3D12Renderer::EvictUwpTextureCache(
	uint32 maxTextures, uint32 minAgeFrames, uint64 targetBytes)
{
	struct Candidate
	{
		LatteTexture* texture{};
		uint64 bytes{};
		uint32 ageFrames{};
	};

	const std::vector<LatteTexture*> deleteableTextures = LatteTC_GetDeleteableTextures();
	std::vector<Candidate> candidates;
	candidates.reserve(deleteableTextures.size());
	const uint32 currentFrame = LatteGPUState.frameCounter;
	for (LatteTexture* texture : deleteableTextures)
	{
		if (!texture)
			continue;
		const uint32 ageFrames = currentFrame - texture->lastAccessFrameCount;
		if (ageFrames < minAgeFrames)
			continue;
		auto* d3dTexture = static_cast<LatteTextureD3D12*>(texture);
		const uint64 allocationBytes = d3dTexture->GetAllocationSize();
		candidates.push_back({
			texture,
			allocationBytes != 0 ? allocationBytes : GetTextureAllocationSize(d3dTexture->GetDesc()),
			ageFrames });
	}

	const auto compareCandidates = [](const Candidate& lhs, const Candidate& rhs)
	{
		const bool lhsCold = lhs.ageFrames >= 120;
		const bool rhsCold = rhs.ageFrames >= 120;
		if (lhsCold != rhsCold)
			return lhsCold;
		if (lhs.bytes != rhs.bytes)
			return lhs.bytes > rhs.bytes;
		return lhs.ageFrames > rhs.ageFrames;
	};
	const size_t selectedCandidateCount = std::min<size_t>(maxTextures, candidates.size());
	if (selectedCandidateCount != 0)
		std::partial_sort(candidates.begin(), candidates.begin() + selectedCandidateCount, candidates.end(), compareCandidates);

	UwpTextureEvictionResult result{};
	for (size_t candidateIndex = 0; candidateIndex < selectedCandidateCount; ++candidateIndex)
	{
		const Candidate& candidate = candidates[candidateIndex];
		LatteTexture_Delete(candidate.texture);
		result.textures++;
		result.bytes += candidate.bytes;
		if (targetBytes != 0 && result.bytes >= targetBytes)
			break;
	}
	// callers measure or retry allocations right after evicting
	FlushPendingDestroys();
	return result;
}

void D3D12Renderer::LogUwpMemoryBreakdown()
{
	if (!CafeSystem::IsTitleRunning() || !m_memoryManager)
		return;
	const auto memory = CemuUwp::MemoryBudgetService::GetLatestSnapshot();
	if (!memory.enabled || memory.limitBytes == 0)
		return;
	const auto now = std::chrono::steady_clock::now();
	const auto interval = memory.usageBytes * 100 >= memory.limitBytes * 85 ? std::chrono::seconds(5) : std::chrono::seconds(30);
	if (now - m_lastMemoryBreakdownLog < interval)
		return;
	m_lastMemoryBreakdownLog = now;

	uint32 textureCount = 0;
	uint64 textureBytes = 0;
	uint64 renderTargetBytes = 0;
	for (LatteTexture* texture : LatteTexture::GetAllTextures())
	{
		if (!texture)
			continue;
		const uint64 bytes = static_cast<LatteTextureD3D12*>(texture)->GetAllocationSize();
		++textureCount;
		textureBytes += bytes;
		if (texture->isDepth || (static_cast<LatteTextureD3D12*>(texture)->GetDesc().Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET))
			renderTargetBytes += bytes;
	}
	const auto memoryStats = m_memoryManager->GetStats();
	const auto shaderStats = RendererShaderD3D12::GetStats();
	constexpr uint64 kMB = 1024 * 1024;
	cemuLog_log(LogType::Force,
		"D3D12 memory: app {}/{} MB, video {} MB, textures {} ({} MB, render targets {} MB), pipelines {}, shaders compiled {} cached {}, upload rings {} MB, readback rings {} MB, texture upload scratch {} MB",
		memory.usageBytes / kMB, memory.limitBytes / kMB, memoryStats.localUsageBytes / kMB, textureCount, textureBytes / kMB,
		renderTargetBytes / kMB, m_gamePipelineCache.size(), shaderStats.dxilCompiled, shaderStats.dxilCacheHits,
		memoryStats.upload.totalBytes / kMB, memoryStats.readback.totalBytes / kMB, memoryStats.textureUploadScratchBytes / kMB);
}

void D3D12Renderer::ProcessUwpMemoryPressure()
{
	using CemuUwp::MemoryBudgetService::Pressure;
	CemuUwp::MemoryBudgetService::Snapshot snapshot{};
	if (!CemuUwp::MemoryBudgetService::TryPoll(m_memoryBudgetGeneration, snapshot) || !snapshot.enabled)
		return;
	if (snapshot.pressure == Pressure::Normal)
	{
		// End of a pressure episode: the next episode starts with a full heavy pass.
		m_memoryPressureLastHeavyFrame = 0;
		m_memoryPressureLastHeavyUsageBytes = 0;
		return;
	}
	// Soft pressure used to drop every prewarmed PSO no draw had used yet (BOTW: ~1600 at 82% after a teleport), so each
	// new effect or enemy was built again: pop-in, and 0.2-0.5 s stalls for draws that cannot be skipped. Any pressure
	// still stops the background prewarm; unused prewarmed PSOs age out in the trim below, and Hard or Critical drop the
	// rest except those of draws that cannot be skipped.
	if (m_gamePipelineRecipeLoading.load(std::memory_order_acquire))
	{
		// stop background prewarm first so nothing new arrives
		StopGamePipelineRecipePrewarm("memory_pressure_discard");
		m_gamePipelineRecipeLoading = false;
		m_gamePipelineRecipeGeneration.fetch_add(1, std::memory_order_acq_rel);
		{
			std::scoped_lock lock(m_prewarmedGamePipelinesMutex);
			m_prewarmedGamePipelines.clear();
			m_prewarmedGamePipelinesAvailable.store(false, std::memory_order_release);
		}
		cemuLog_log(LogType::Force, "D3D12 UWP: memory pressure {} stopped the background pipeline prewarm ({} prewarmed PSOs kept).",
			CemuUwp::MemoryBudgetService::PressureName(snapshot.pressure), m_pendingGamePipelineRecipes.size());
	}
	if (snapshot.pressure >= Pressure::Hard && !m_pendingGamePipelineRecipes.empty())
	{
		auto& pipelineStableCache = D3D12PipelineStableCache::GetInstance();
		const size_t discardedRecipes = std::erase_if(m_pendingGamePipelineRecipes,
			[&](const auto& entry) { return !entry.second.fromAsync && !pipelineStableCache.IsMustBeReady(entry.first); });
		if (discardedRecipes != 0)
			cemuLog_log(LogType::Force, "D3D12 UWP: memory pressure {} dropped {} unused prewarmed PSOs ({} kept).",
				CemuUwp::MemoryBudgetService::PressureName(snapshot.pressure), discardedRecipes, m_pendingGamePipelineRecipes.size());
		D3D12TraceEvent("game_pipeline_recipe_pressure_discard",
			"pressure={} discarded={} title=0x{:016x}",
			CemuUwp::MemoryBudgetService::PressureName(snapshot.pressure),
			discardedRecipes,
			m_gamePipelineRecipeLoadingTitleId);
	}

	LatteUwpHitchScope hitchScope(LatteUwpHitchOp::MemoryPressure);
	// tell the player once in a while; trimming hides the cause until the title runs out of memory
	if (snapshot.pressure >= Pressure::Hard)
	{
		const auto now = std::chrono::steady_clock::now();
		if (m_memoryPressureLastNotice.time_since_epoch().count() == 0 || now - m_memoryPressureLastNotice >= std::chrono::minutes(5))
		{
			m_memoryPressureLastNotice = now;
			LatteOverlay_pushNotification(_tr("Memory is almost full ({} / {} MB). Lower the resolution or disable heavy graphic packs if the game stutters or closes.",
				snapshot.usageBytes / (1024 * 1024), snapshot.limitBytes / (1024 * 1024)), 8000);
		}
	}
	uint32 maxTextures = 4;
	uint32 minAgeFrames = 120;
	uint64 maxTextureBytes = 64ull * 1024ull * 1024ull;
	if (snapshot.pressure == Pressure::Hard)
	{
		maxTextures = 16;
		minAgeFrames = 30;
		maxTextureBytes = 256ull * 1024ull * 1024ull;
	}
	else if (snapshot.pressure == Pressure::Critical)
	{
		maxTextures = 48;
		minAgeFrames = 3;
		maxTextureBytes = 512ull * 1024ull * 1024ull;
	}

	ProcessFinishedCommandLists();
	uint64 descriptorPoolBytes = m_descriptorManager ? m_descriptorManager->ClearRecycledShaderVisibleHeaps() : 0;
	const uint64 scratchPoolBytes = m_scratchTexturePoolBytes;
	uint64 commandScratchBytes = 0;
	for (const auto& entry : m_commandScratchTextures)
		commandScratchBytes += entry.sizeBytes;
	ReleaseScratchTexturePool();
	const uint64 aliasCopyStagingBytes = m_textureAliasCopyBuffer.size;
	ReleaseResourceDeferred(m_textureAliasCopyBuffer);
	const uint64 uploadScratchBytes = m_memoryManager ? m_memoryManager->TrimTextureUploadBuffer() : 0;
	const uint64 scratchBytes = scratchPoolBytes + commandScratchBytes + uploadScratchBytes + aliasCopyStagingBytes;
	uint64 immediatelyReclaimedBytes = scratchBytes + descriptorPoolBytes;
	const uint64 remainingRequest = snapshot.recommendedReleaseBytes > immediatelyReclaimedBytes ?
		snapshot.recommendedReleaseBytes - immediatelyReclaimedBytes : 0;
	const uint64 targetTextureBytes = std::min(remainingRequest, maxTextureBytes);
	UwpTextureEvictionResult evicted{};
	if (targetTextureBytes != 0 || snapshot.pressure != Pressure::Soft)
		evicted = EvictUwpTextureCache(maxTextures, minAgeFrames, targetTextureBytes);
	// PSOs keep their shader ISA in video memory; with async creation a cold one is cheap to rebuild
	size_t evictedPipelines = 0;
	// launch prewarm keeps thousands of PSOs alive that the current area never draws (BOTW forest: 2.4k at 82%)
	if (snapshot.pressure == Pressure::Soft)
		evictedPipelines = EvictColdGamePipelines(60 * 60 * 5, 128) + ReleaseUnusedPrewarmedPipelines(60 * 60 * 5, 256);
	else if (snapshot.pressure == Pressure::Hard)
		evictedPipelines = EvictColdGamePipelines(60 * 60, 256) + ReleaseUnusedPrewarmedPipelines(60 * 60, 512);
	else if (snapshot.pressure == Pressure::Critical)
		evictedPipelines = EvictColdGamePipelines(60 * 5, 1024) + ReleaseUnusedPrewarmedPipelines(60 * 5, 2048);
	// shader bytecode is only needed for new PSOs and the DXIL cache can give it back
	uint64 evictedDxilBytes = 0;
	if (snapshot.pressure == Pressure::Hard)
		evictedDxilBytes = RendererShaderD3D12::EvictIdleDXIL(60 * 60, 0);
	else if (snapshot.pressure == Pressure::Critical)
		evictedDxilBytes = RendererShaderD3D12::EvictIdleDXIL(60 * 10, 0);
	immediatelyReclaimedBytes += evictedDxilBytes;

	// heavy passes block on GPU retirement and heap compaction; run them sparingly
	const uint32 frameCounter = LatteGPUState.frameCounter;
	const uint32 framesSinceHeavyPass = frameCounter - m_memoryPressureLastHeavyFrame;
	const bool firstHeavyPass = m_memoryPressureLastHeavyFrame == 0;
	const bool usageGrewMaterially = snapshot.usageBytes >= m_memoryPressureLastHeavyUsageBytes + 128ull * 1024ull * 1024ull;
	const bool maintenanceDue = framesSinceHeavyPass >= 300;
	const bool emergency = snapshot.limitBytes != 0 && snapshot.usageBytes >= snapshot.limitBytes - snapshot.limitBytes / 50;
	const bool heavyPass = snapshot.pressure == Pressure::Critical &&
		(firstHeavyPass || maintenanceDue || (framesSinceHeavyPass >= 10 && (usageGrewMaterially || emergency)));
	if (heavyPass)
	{
		WaitForIdle();
		ProcessDestructionQueue();
		const uint64 newlyCompletedDescriptorPoolBytes = m_descriptorManager ? m_descriptorManager->ClearRecycledShaderVisibleHeaps() : 0;
		descriptorPoolBytes += newlyCompletedDescriptorPoolBytes;
		immediatelyReclaimedBytes += newlyCompletedDescriptorPoolBytes;
		// return the freed pages to the Xbox commit budget
		CemuUwp::MemoryBudgetService::CompactProcessHeap();
		m_memoryPressureHeavyPasses++;
		m_memoryPressureLastHeavyFrame = frameCounter != 0 ? frameCounter : 1;
		m_memoryPressureLastHeavyUsageBytes = snapshot.usageBytes;
	}
	else
	{
		ProcessFinishedCommandLists();
		ProcessDestructionQueue();
		const uint64 newlyCompletedDescriptorPoolBytes = m_descriptorManager ? m_descriptorManager->ClearRecycledShaderVisibleHeaps() : 0;
		descriptorPoolBytes += newlyCompletedDescriptorPoolBytes;
		immediatelyReclaimedBytes += newlyCompletedDescriptorPoolBytes;
	}

	m_memoryPressureTrimPasses++;
	m_memoryPressureTexturesEvicted += evicted.textures;
	m_memoryPressureTextureBytes += evicted.bytes;
	m_memoryPressureScratchBytes += scratchBytes;
	if (evicted.bytes == 0 && immediatelyReclaimedBytes == 0 && evictedPipelines == 0)
	{
		if (snapshot.pressure == Pressure::Critical)
		{
			cemuLog_logOnce(LogType::Force,
				"D3D12 Xbox memory pressure is critical, but no safely reconstructible textures or inactive scratch allocations remain.");
		}
		return;
	}
	static uint64 lastLoggedTrimPass = 0;
	if (snapshot.pressure != Pressure::Soft || m_memoryPressureTrimPasses - lastLoggedTrimPass >= 8)
	{
		lastLoggedTrimPass = m_memoryPressureTrimPasses;
		cemuLog_log(LogType::Force,
			"D3D12 Xbox memory trim: pressure={} usage={}/{}MB textures={} estimated={}MB scratch={}MB descriptorPool={}MB requested={}MB pipelines={} shaderBytecode={}MB.",
			CemuUwp::MemoryBudgetService::PressureName(snapshot.pressure),
			(unsigned long long)(snapshot.usageBytes / (1024ull * 1024ull)),
			(unsigned long long)(snapshot.limitBytes / (1024ull * 1024ull)),
			evicted.textures,
			(unsigned long long)(evicted.bytes / (1024ull * 1024ull)),
			(unsigned long long)(scratchBytes / (1024ull * 1024ull)),
			(unsigned long long)(descriptorPoolBytes / (1024ull * 1024ull)),
			(unsigned long long)(snapshot.recommendedReleaseBytes / (1024ull * 1024ull)),
			evictedPipelines,
			(unsigned long long)(evictedDxilBytes / (1024ull * 1024ull)));
	}
}

bool D3D12Renderer::RecoverFromTextureAllocationFailure(const D3D12_RESOURCE_DESC& desc, const char* context)
{
	m_textureAllocationRecoveryAttempts++;

	size_t deferredBatches = 0;
	{
		std::scoped_lock lock(m_deferredReleaseMutex);
		deferredBatches = m_deferredReleaseQueue.size();
	}
	const bool hadFeedbackScratchCopies = !m_feedbackScratchCopies.empty() || !m_retainedFeedbackCopies.empty();

	const uint64 requestedBytes = GetTextureAllocationSize(desc);
	cemuLog_log(LogType::Force,
				"D3D12 UWP: texture allocation recovery for {} requested={}MB scratchPool={}/{}MB commandScratch={} pendingScratchCopies={} deferredBatches={}.",
				context ? context : "unknown",
				(unsigned long long)(requestedBytes / (1024ull * 1024ull)),
				m_scratchTexturePool.size(),
				(unsigned long long)(m_scratchTexturePoolBytes / (1024ull * 1024ull)),
				m_commandScratchTextures.size(),
				m_feedbackScratchCopies.size(),
				deferredBatches);

	const uint64 scratchPoolBytes = m_scratchTexturePoolBytes;
	uint64 commandScratchBytes = 0;
	for (const auto& entry : m_commandScratchTextures)
		commandScratchBytes += entry.sizeBytes;
	ReleaseScratchTexturePool();
	WaitForIdle();
	ReleaseScratchTexturePool();
	ProcessFinishedCommandLists();
	ProcessDestructionQueue();
	const uint64 descriptorPoolBytes = m_descriptorManager ? m_descriptorManager->ClearRecycledShaderVisibleHeaps() : 0;
	const uint64 uploadScratchBytes = m_memoryManager ? m_memoryManager->TrimTextureUploadBuffer() : 0;
	const UwpTextureEvictionResult evicted = EvictUwpTextureCache(
		32, 3, std::max<uint64>(requestedBytes * 2, 128ull * 1024ull * 1024ull));
	ProcessDestructionQueue();
	CemuUwp::MemoryBudgetService::CompactProcessHeap();

	cemuLog_log(LogType::Force,
				"D3D12 UWP: texture allocation recovery complete for {} completedFence={} evicted={}/{}MB scratch={}MB descriptorPool={}MB.",
				context ? context : "unknown",
				(unsigned long long)m_lastCompletedFenceValue,
				evicted.textures,
				(unsigned long long)(evicted.bytes / (1024ull * 1024ull)),
				(unsigned long long)((scratchPoolBytes + commandScratchBytes + uploadScratchBytes) / (1024ull * 1024ull)),
				(unsigned long long)(descriptorPoolBytes / (1024ull * 1024ull)));
	return deferredBatches != 0 || hadFeedbackScratchCopies ||
		scratchPoolBytes != 0 || commandScratchBytes != 0 || uploadScratchBytes != 0 || descriptorPoolBytes != 0 || evicted.textures != 0;
}

void D3D12Renderer::RecoverFromPipelineAllocationFailure()
{
	const bool wasCommandListOpen = m_commandListOpen;
	const uint64 scratchPoolBytes = m_scratchTexturePoolBytes;
	uint64 commandScratchBytes = 0;
	for (const auto& entry : m_commandScratchTextures)
		commandScratchBytes += entry.sizeBytes;
	ReleaseScratchTexturePool();
	WaitForIdle();
	ReleaseScratchTexturePool();
	ProcessFinishedCommandLists();
	ProcessDestructionQueue();
	const uint64 descriptorPoolBytes = m_descriptorManager ? m_descriptorManager->ClearRecycledShaderVisibleHeaps() : 0;
	// on the recording thread the texture cache and the texture upload buffer belong to the Latte side (Phase 4 S3)
	const bool onRecordingThread = m_recordThread.joinable() && std::this_thread::get_id() == m_recordThread.get_id();
	const uint64 uploadScratchBytes = m_memoryManager && !onRecordingThread ? m_memoryManager->TrimTextureUploadBuffer() : 0;
	const UwpTextureEvictionResult evicted = onRecordingThread ? UwpTextureEvictionResult{} : EvictUwpTextureCache(16, 3, 128ull * 1024ull * 1024ull);
	ProcessDestructionQueue();
	CemuUwp::MemoryBudgetService::CompactProcessHeap();
	if (wasCommandListOpen && !m_commandListOpen)
		ResetCommandList();
	cemuLog_log(LogType::Force,
		"D3D12 UWP: pipeline allocation recovery evicted={}/{}MB scratch={}MB descriptorPool={}MB.",
		evicted.textures,
		(unsigned long long)(evicted.bytes / (1024ull * 1024ull)),
		(unsigned long long)((scratchPoolBytes + commandScratchBytes + uploadScratchBytes) / (1024ull * 1024ull)),
		(unsigned long long)(descriptorPoolBytes / (1024ull * 1024ull)));
}

bool D3D12Renderer::ShouldDeferNativePipelineCreation(uint64 pipelineKey)
{
	using CemuUwp::MemoryBudgetService::Pressure;
	if (!CemuUwp::MemoryBudgetService::IsHeavyWorkDeferred())
		return false;

	// the cached sample can be up to 500 ms old; decide on a fresh reading
	auto snapshot = CemuUwp::MemoryBudgetService::SampleNow();
	if (!snapshot.enabled || !snapshot.heavyWorkDeferred)
		return false;

	const uint32 frameCounter = LatteGPUState.frameCounter;
	if (m_memoryPressureLastDeferRecoveryFrame == 0 ||
		frameCounter - m_memoryPressureLastDeferRecoveryFrame >= 60)
	{
		m_memoryPressureLastDeferRecoveryFrame = frameCounter != 0 ? frameCounter : 1;
		RecoverFromPipelineAllocationFailure();
		snapshot = CemuUwp::MemoryBudgetService::SampleNow(true);
		if (!snapshot.enabled || !snapshot.heavyWorkDeferred)
			return false;
	}

	// a title living above the guard threshold would otherwise never get its pipelines
	uint32& deferCount = m_gamePipelineDeferCount[pipelineKey];
	if (deferCount >= 4 && snapshot.pressure < Pressure::Critical)
	{
		m_gamePipelineDeferCount.erase(pipelineKey);
		m_memoryPressureForcedPipelines++;
		return false;
	}
	++deferCount;
	m_memoryPressureDeferredPipelines++;
	m_gamePipelineRetryFrame[pipelineKey] = frameCounter + 30;
	if (m_memoryPressureDeferredPipelines == 1 || (m_memoryPressureDeferredPipelines % 64) == 0)
	{
		cemuLog_log(LogType::Force,
			"D3D12 Xbox memory guard deferred native pipeline 0x{:016x} (usage {}/{} MB, pressure={}, deferred={} forced={}).",
			pipelineKey,
			(unsigned long long)(snapshot.usageBytes / (1024ull * 1024ull)),
			(unsigned long long)(snapshot.limitBytes / (1024ull * 1024ull)),
			CemuUwp::MemoryBudgetService::PressureName(snapshot.pressure),
			(unsigned long long)m_memoryPressureDeferredPipelines,
			(unsigned long long)m_memoryPressureForcedPipelines);
	}
	return true;
}

bool D3D12Renderer::CreateTextureWithRecovery(const D3D12_RESOURCE_DESC& desc, D3D12_RESOURCE_STATES initialState, const D3D12_CLEAR_VALUE* clearValue, D3D12MemoryManager::Resource& outResource, const char* context)
{
	if (!m_memoryManager)
		return false;

	if (m_memoryManager->CreateTexture(desc, initialState, clearValue, outResource))
		return true;

	const bool wasCommandListOpen = m_commandListOpen;
	if (!RecoverFromTextureAllocationFailure(desc, context))
		return false;

	const bool recovered = m_memoryManager->CreateTexture(desc, initialState, clearValue, outResource);
	if (wasCommandListOpen && !m_commandListOpen)
		ResetCommandList();

	if (recovered)
	{
		m_textureAllocationRecoverySuccesses++;
		cemuLog_log(LogType::Force,
					"D3D12 UWP: texture allocation recovered for {} size={}x{} depthOrArray={} format={} flags=0x{:08x}.",
					context ? context : "unknown",
					(unsigned long long)desc.Width,
					(unsigned)desc.Height,
					(unsigned)desc.DepthOrArraySize,
					(unsigned)desc.Format,
					(unsigned)desc.Flags);
	}
	return recovered;
}

bool D3D12Renderer::RefreshRanged3DSamplingProxy(LatteTextureViewD3D12* view)
{
	if (!view || !view->UsesSamplingProxy() || !view->m_samplingProxy.resource)
		return false;
	auto* sourceTexture = view->GetBaseImage();
	if (!sourceTexture || !sourceTexture->GetResource() || sourceTexture->GetDesc().Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE3D)
		return false;
	if (!m_commandListOpen)
		ResetCommandList();

	const auto& sourceDesc = sourceTexture->GetDesc();
	const auto proxyDesc = view->m_samplingProxy.resource->GetDesc();
	const uint32 firstMip = (uint32)std::clamp<sint32>(view->firstMip, 0, (sint32)sourceDesc.MipLevels - 1);
	const uint32 mipCount = std::min<uint32>(std::max<sint32>(view->numMip, 1), sourceDesc.MipLevels - firstMip);
	std::vector<D3D12_RESOURCE_STATES> previousStates(mipCount);

	Transition(view->m_samplingProxy.resource.Get(), view->m_samplingProxy.state, D3D12_RESOURCE_STATE_COPY_DEST);
	auto* commandList = GetCurrentCommandSlot().commandList.Get();
	for (uint32 relativeMip = 0; relativeMip < mipCount; ++relativeMip)
	{
		const uint32 sourceMip = firstMip + relativeMip;
		const uint32 sourceSubresource = sourceTexture->GetSubresourceIndex(0, sourceMip);
		previousStates[relativeMip] = sourceTexture->GetSubresourceState(sourceSubresource);
		TransitionTextureSubresource(sourceTexture, sourceSubresource, D3D12_RESOURCE_STATE_COPY_SOURCE);

		const uint32 sourceWidth = std::max<uint32>((uint32)sourceDesc.Width >> sourceMip, 1);
		const uint32 sourceHeight = std::max<uint32>(sourceDesc.Height >> sourceMip, 1);
		const uint32 sourceDepth = std::max<uint32>((uint32)sourceDesc.DepthOrArraySize >> sourceMip, 1);
		const uint32 copyWidth = std::min<uint32>(sourceWidth, std::max<uint32>((uint32)proxyDesc.Width >> relativeMip, 1));
		const uint32 copyHeight = std::min<uint32>(sourceHeight, std::max<uint32>(proxyDesc.Height >> relativeMip, 1));

		D3D12_TEXTURE_COPY_LOCATION source{};
		source.pResource = sourceTexture->GetResource();
		source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		source.SubresourceIndex = sourceSubresource;

		if (proxyDesc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D)
		{
			const uint32 requestedFirstSlice = (uint32)std::max<sint32>(view->firstSlice, 0) >> relativeMip;
			const uint32 sourceFirstSlice = std::min<uint32>(requestedFirstSlice, sourceDepth - 1);
			const uint32 proxyDepth = std::max<uint32>((uint32)proxyDesc.DepthOrArraySize >> relativeMip, 1);
			const uint32 copyDepth = std::min<uint32>(proxyDepth, sourceDepth - sourceFirstSlice);
			D3D12_BOX sourceBox{0, 0, sourceFirstSlice, copyWidth, copyHeight, sourceFirstSlice + copyDepth};
			D3D12_TEXTURE_COPY_LOCATION destination{};
			destination.pResource = view->m_samplingProxy.resource.Get();
			destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
			destination.SubresourceIndex = relativeMip;
			commandList->CopyTextureRegion(&destination, 0, 0, 0, &source, &sourceBox);
		}
		else
		{
			const uint32 arraySize = std::max<uint32>(proxyDesc.DepthOrArraySize, 1);
			for (uint32 arraySlice = 0; arraySlice < arraySize; ++arraySlice)
			{
				const uint32 requestedSlice = (uint32)std::max<sint32>(view->firstSlice, 0) + arraySlice;
				const uint32 sourceSlice = std::min<uint32>(requestedSlice, sourceDepth - 1);
				D3D12_BOX sourceBox{0, 0, sourceSlice, copyWidth, copyHeight, sourceSlice + 1};
				D3D12_TEXTURE_COPY_LOCATION destination{};
				destination.pResource = view->m_samplingProxy.resource.Get();
				destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
				destination.SubresourceIndex = relativeMip + arraySlice * proxyDesc.MipLevels;
				commandList->CopyTextureRegion(&destination, 0, 0, 0, &source, &sourceBox);
			}
		}
	}

	for (uint32 relativeMip = 0; relativeMip < mipCount; ++relativeMip)
		TransitionTextureSubresource(sourceTexture, sourceTexture->GetSubresourceIndex(0, firstMip + relativeMip), previousStates[relativeMip]);
	Transition(view->m_samplingProxy.resource.Get(), view->m_samplingProxy.state,
		D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	return true;
}

bool D3D12Renderer::TryAcquireScratchTexture(const D3D12_RESOURCE_DESC& desc, D3D12MemoryManager::Resource& outResource)
{
	const auto itr = std::find_if(m_scratchTexturePool.begin(), m_scratchTexturePool.end(),
								  [&](const ScratchTexturePoolEntry& entry)
								  {
									  return entry.readyFence <= m_lastCompletedFenceValue &&
											 D3D12ScratchTextureDescMatches(entry.desc, desc) &&
											 entry.texture.IsValid();
								  });
	if (itr == m_scratchTexturePool.end())
	{
		m_scratchTexturePoolMisses++;
		return false;
	}

	outResource = std::move(itr->texture);
	m_scratchTexturePoolBytes = itr->sizeBytes <= m_scratchTexturePoolBytes ? m_scratchTexturePoolBytes - itr->sizeBytes : 0;
	m_scratchTexturePool.erase(itr);
	m_scratchTexturePoolHits++;
	return true;
}

bool D3D12Renderer::TryAcquireCommandScratchTexture(const D3D12_RESOURCE_DESC& desc, D3D12MemoryManager::Resource& outResource)
{
	const auto itr = std::find_if(m_commandScratchTextures.begin(), m_commandScratchTextures.end(),
		[&](const ScratchTexturePoolEntry& entry)
		{
			return D3D12ScratchTextureDescMatches(entry.desc, desc) && entry.texture.IsValid();
		});
	if (itr != m_commandScratchTextures.end())
	{
		outResource = std::move(itr->texture);
		m_commandScratchTextures.erase(itr);
		m_commandScratchTextureHits++;
		return true;
	}
	return TryAcquireScratchTexture(desc, outResource);
}

void D3D12Renderer::RecycleCommandScratchTexture(D3D12MemoryManager::Resource&& texture, const D3D12_RESOURCE_DESC& desc)
{
	if (!texture.IsValid())
		return;
	ScratchTexturePoolEntry entry;
	entry.texture = std::move(texture);
	entry.desc = desc;
	entry.sizeBytes = entry.texture.size != 0 ? entry.texture.size : GetTextureAllocationSize(desc);
	m_commandScratchTextures.emplace_back(std::move(entry));
	m_commandScratchTextureHighWater = std::max<uint64>(m_commandScratchTextureHighWater, m_commandScratchTextures.size());
}

void D3D12Renderer::RetireScratchTexture(D3D12MemoryManager::Resource&& texture, const D3D12_RESOURCE_DESC& desc, uint64 readyFence)
{
	if (!texture.IsValid())
		return;

	const uint64 sizeBytes = texture.size != 0 ? texture.size : GetTextureAllocationSize(desc);
	if (sizeBytes > kD3D12ScratchTexturePoolMaxBytes)
	{
		QueueDeferredRelease(std::move(texture));
		m_scratchTexturePoolEvicted++;
		return;
	}

	while (!m_scratchTexturePool.empty() &&
		   (m_scratchTexturePool.size() >= kD3D12ScratchTexturePoolMaxEntries ||
			m_scratchTexturePoolBytes + sizeBytes > kD3D12ScratchTexturePoolMaxBytes))
	{
		auto& oldest = m_scratchTexturePool.front();
		m_scratchTexturePoolBytes = oldest.sizeBytes <= m_scratchTexturePoolBytes ? m_scratchTexturePoolBytes - oldest.sizeBytes : 0;
		QueueDeferredRelease(std::move(oldest.texture));
		m_scratchTexturePool.erase(m_scratchTexturePool.begin());
		m_scratchTexturePoolEvicted++;
	}

	ScratchTexturePoolEntry entry;
	entry.texture = std::move(texture);
	entry.desc = desc;
	entry.sizeBytes = sizeBytes;
	entry.readyFence = readyFence;
	m_scratchTexturePool.emplace_back(std::move(entry));
	m_scratchTexturePoolBytes += sizeBytes;
	m_scratchTexturePoolRetired++;
}

void D3D12Renderer::ReleaseScratchTexturePool()
{
	for (auto& entry : m_commandScratchTextures)
		QueueDeferredRelease(std::move(entry.texture));
	m_commandScratchTextures.clear();
	for (auto& entry : m_scratchTexturePool)
	{
		QueueDeferredRelease(std::move(entry.texture));
	}
	m_scratchTexturePool.clear();
	m_scratchTexturePoolBytes = 0;
}

void D3D12Renderer::ResetScratchCopyLookup()
{
	m_feedbackScratchByTextureUnit.fill(kInvalidScratchCopyIndex);
	m_logicOpScratchByTarget.fill(kInvalidScratchCopyIndex);
}

bool D3D12Renderer::IsCommandScratchBudgetReached() const
{
	size_t liveTextureCount = 0;
	uint64 liveTextureBytes = 0;
	for (const auto& entry : m_commandScratchTextures)
	{
		if (!entry.texture.IsValid())
			continue;
		++liveTextureCount;
		liveTextureBytes += entry.sizeBytes;
	}
	for (const auto& scratch : m_feedbackScratchCopies)
	{
		if (!scratch.texture.IsValid())
			continue;
		++liveTextureCount;
		liveTextureBytes += scratch.texture.size != 0 ? scratch.texture.size : GetTextureAllocationSize(scratch.requestedDesc);
	}
	for (const auto& retained : m_retainedFeedbackCopies)
	{
		if (!retained.scratch.texture.IsValid())
			continue;
		++liveTextureCount;
		liveTextureBytes += retained.scratch.texture.size != 0 ? retained.scratch.texture.size : GetTextureAllocationSize(retained.scratch.requestedDesc);
	}
	return liveTextureCount >= kD3D12ScratchTexturePoolMaxEntries ||
		liveTextureBytes >= kD3D12ScratchTexturePoolMaxBytes;
}

const D3D12Renderer::FeedbackScratchCopy* D3D12Renderer::GetFeedbackScratchCopy(uint32 textureUnit) const
{
	if (textureUnit >= m_feedbackScratchByTextureUnit.size())
		return nullptr;
	const uint32 index = m_feedbackScratchByTextureUnit[textureUnit];
	if (index == kInvalidScratchCopyIndex || index >= m_feedbackScratchCopies.size())
		return nullptr;
	const auto& scratch = m_feedbackScratchCopies[index];
	return scratch.texture.IsValid() ? &scratch : nullptr;
}

D3D12Renderer::FeedbackScratchCopy* D3D12Renderer::GetLogicOpScratchCopy(uint32 renderTargetIndex)
{
	if (renderTargetIndex >= m_logicOpScratchByTarget.size())
		return nullptr;
	const uint32 index = m_logicOpScratchByTarget[renderTargetIndex];
	if (index == kInvalidScratchCopyIndex || index >= m_feedbackScratchCopies.size())
		return nullptr;
	auto& scratch = m_feedbackScratchCopies[index];
	return scratch.texture.IsValid() ? &scratch : nullptr;
}

void D3D12Renderer::RecycleScratchCopiesForDraw(uint64 drawId)
{
	constexpr size_t kMaxRetainedFeedbackCopies = 8;
	for (auto it = m_feedbackScratchCopies.begin(); it != m_feedbackScratchCopies.end();)
	{
		if (it->drawId != drawId)
		{
			++it;
			continue;
		}

		if (it->texture.IsValid())
		{
			if (it->retainAcrossDraws && m_activeFBO && m_retainedFeedbackCopies.size() < kMaxRetainedFeedbackCopies)
				m_retainedFeedbackCopies.push_back({ std::move(*it), m_activeFBO, m_activeFBO->key });
			else
				RecycleCommandScratchTexture(std::move(it->texture), it->requestedDesc);
		}
		it = m_feedbackScratchCopies.erase(it);
	}
	ResetScratchCopyLookup();
}

bool D3D12Renderer::TryReuseRetainedFeedbackCopy(uint32 textureUnit, LatteTextureViewD3D12* sampledView)
{
	if (!m_activeFBO || textureUnit >= m_feedbackScratchByTextureUnit.size())
		return false;
	for (auto it = m_retainedFeedbackCopies.begin(); it != m_retainedFeedbackCopies.end(); ++it)
	{
		if (it->scratch.sampledView != sampledView)
			continue;
		if (it->fbo != m_activeFBO || it->fboKey != m_activeFBO->key || !it->scratch.texture.IsValid() ||
			(it->scratch.requireUnchangedSource && FeedbackSourceGeneration(sampledView) != it->scratch.sourceGeneration))
		{
			RecycleCommandScratchTexture(std::move(it->scratch.texture), it->scratch.requestedDesc);
			m_retainedFeedbackCopies.erase(it);
			return false;
		}
		FeedbackScratchCopy scratch = std::move(it->scratch);
		m_retainedFeedbackCopies.erase(it);
		scratch.textureUnit = textureUnit;
		scratch.drawId = m_gameDrawCallsSubmitted + 1;
		m_feedbackScratchCopies.emplace_back(std::move(scratch));
		m_feedbackScratchByTextureUnit[textureUnit] = (uint32)m_feedbackScratchCopies.size() - 1;
		return true;
	}
	return false;
}

void D3D12Renderer::DropRetainedFeedbackCopies(LatteTexture* modifiedTexture)
{
	for (auto it = m_retainedFeedbackCopies.begin(); it != m_retainedFeedbackCopies.end();)
	{
		const auto* sampledView = it->scratch.sampledView;
		if (modifiedTexture && (!sampledView || sampledView->baseTexture != modifiedTexture))
		{
			++it;
			continue;
		}
		RecycleCommandScratchTexture(std::move(it->scratch.texture), it->scratch.requestedDesc);
		it = m_retainedFeedbackCopies.erase(it);
	}
}

void D3D12Renderer::SurfaceSync(Latte::E_COHER_CNTL coher, MPTR address, uint32 size)
{
	D3D12_QUEUE_RECORDING(SurfaceSync(coher, address, size));
	// a guest color-buffer flush makes earlier draws visible to texture reads, so feedback copies are refreshed
	if (static_cast<uint32>(coher & (Latte::E_COHER_CNTL::CB_ACTION_ENA | Latte::E_COHER_CNTL::CB_ALL_DEST_BASE_ENA)) != 0)
		DropRetainedFeedbackCopies();
}

size_t D3D12Renderer::TextureCopyCacheKeyHash::operator()(const TextureCopyCacheKey& key) const noexcept
{
	uint64 hash = 0xd312c09ecac4e001ull;
	hash = MixGameDescriptorTableKey(hash, key.sourceIdentity);
	hash = MixGameDescriptorTableKey(hash, key.destinationIdentity);
	hash = MixGameDescriptorTableKey(hash, (uint32)key.sourceMip);
	hash = MixGameDescriptorTableKey(hash, (uint32)key.sourceX);
	hash = MixGameDescriptorTableKey(hash, (uint32)key.sourceY);
	hash = MixGameDescriptorTableKey(hash, (uint32)key.sourceSlice);
	hash = MixGameDescriptorTableKey(hash, (uint32)key.destinationMip);
	hash = MixGameDescriptorTableKey(hash, (uint32)key.destinationX);
	hash = MixGameDescriptorTableKey(hash, (uint32)key.destinationY);
	hash = MixGameDescriptorTableKey(hash, (uint32)key.destinationSlice);
	hash = MixGameDescriptorTableKey(hash, (uint32)key.width);
	hash = MixGameDescriptorTableKey(hash, (uint32)key.height);
	hash = MixGameDescriptorTableKey(hash, (uint32)key.depth);
	hash = MixGameDescriptorTableKey(hash, key.mode);
	return (size_t)hash;
}

bool D3D12Renderer::CanSkipTextureCopy(const TextureCopyCacheKey& key, const LatteTextureD3D12* source,
	const LatteTextureD3D12* destination, const std::vector<uint32>& sourceSubresources,
	const std::vector<uint32>& destinationSubresources) const
{
	if (!source || !destination)
		return false;
	// overlapping copies within one resource are not idempotent; never cache them
	if (source->GetResource() == destination->GetResource())
		return false;
	const auto it = m_textureCopyCache.find(key);
	if (it == m_textureCopyCache.end())
		return false;
	const auto& entry = it->second;
	if (entry.sourceGenerations.size() != sourceSubresources.size() ||
		entry.destinationGenerations.size() != destinationSubresources.size())
		return false;
	for (size_t i = 0; i < sourceSubresources.size(); ++i)
	{
		if (entry.sourceGenerations[i].subresource != sourceSubresources[i] ||
			entry.sourceGenerations[i].generation != source->GetContentGeneration(sourceSubresources[i]))
			return false;
	}
	for (size_t i = 0; i < destinationSubresources.size(); ++i)
	{
		if (entry.destinationGenerations[i].subresource != destinationSubresources[i] ||
			entry.destinationGenerations[i].generation != destination->GetContentGeneration(destinationSubresources[i]))
			return false;
	}
	return true;
}

void D3D12Renderer::RecordTextureCopy(const TextureCopyCacheKey& key, const LatteTextureD3D12* source,
	LatteTextureD3D12* destination, const std::vector<uint32>& sourceSubresources,
	const std::vector<uint32>& destinationSubresources)
{
	if (!source || !destination)
		return;
	for (uint32 subresource : destinationSubresources)
		destination->MarkSubresourceWritten(subresource);

	if (m_textureCopyCache.size() >= 4096 && m_textureCopyCache.find(key) == m_textureCopyCache.end())
		m_textureCopyCache.clear();
	auto& entry = m_textureCopyCache[key];
	entry.sourceGenerations.clear();
	entry.destinationGenerations.clear();
	entry.sourceGenerations.reserve(sourceSubresources.size());
	entry.destinationGenerations.reserve(destinationSubresources.size());
	for (uint32 subresource : sourceSubresources)
		entry.sourceGenerations.push_back({ subresource, source->GetContentGeneration(subresource) });
	for (uint32 subresource : destinationSubresources)
		entry.destinationGenerations.push_back({ subresource, destination->GetContentGeneration(subresource) });
}

void D3D12Renderer::MarkTextureViewWritten(LatteTextureViewD3D12* view, bool includeStencilPlane, bool includePrimaryPlane)
{
	if (!view)
		return;
	auto* texture = view->GetBaseImage();
	if (!texture)
		return;
	const uint32 mipLevels = std::max<uint32>(texture->GetDesc().MipLevels, 1);
	const uint32 arraySize = texture->Is3DTexture() ? 1u : std::max<uint32>(texture->GetArraySize(), 1);
	const uint32 firstMip = (uint32)std::clamp<sint32>(view->firstMip, 0, (sint32)mipLevels - 1);
	const uint32 mipCount = std::min<uint32>(std::max<sint32>(view->numMip, 1), mipLevels - firstMip);
	const uint32 firstSlice = texture->Is3DTexture() ? 0u : (uint32)std::clamp<sint32>(view->firstSlice, 0, (sint32)arraySize - 1);
	const uint32 sliceCount = texture->Is3DTexture() ? 1u : std::min<uint32>(std::max<sint32>(view->numSlice, 1), arraySize - firstSlice);
	const uint32 planeStride = mipLevels * arraySize;
	const uint32 firstPlane = includePrimaryPlane ? 0u : 1u;
	const uint32 planeCount = (includeStencilPlane && texture->GetFormatInfo().hasStencil) ? 2u : 1u;
	for (uint32 plane = firstPlane; plane < planeCount; ++plane)
	{
		for (uint32 slice = 0; slice < sliceCount; ++slice)
		{
			for (uint32 mip = 0; mip < mipCount; ++mip)
				texture->MarkSubresourceWritten(firstMip + mip + (firstSlice + slice) * mipLevels + plane * planeStride);
		}
	}
}
#endif

void D3D12Renderer::ResolveFeedbackLoopsForDraw(LatteDecompilerShader* vertexShader,
	LatteDecompilerShader* geometryShader, LatteDecompilerShader* pixelShader, bool renderTargetsBound)
{
	if (!m_memoryManager || !m_descriptorManager)
		return;
	const bool scanAttachments = renderTargetsBound && m_activeFBO;

	static uint32 s_feedbackScanLogCount = 0;
	auto resolveTextureUnit = [&](uint32 textureUnit)
	{
		auto* sampledView = m_boundTextures[textureUnit];
		if (!sampledView || !sampledView->GetSRV().IsValid())
			return;

		// the sampling proxy is a separate resource and cannot alias a guest attachment
		if (sampledView->UsesSamplingProxy())
		{
			m_feedbackOverlapCache[textureUnit] = {};
			Transition(sampledView->m_samplingProxy.resource.Get(), sampledView->m_samplingProxy.state,
				D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
			return;
		}

		if (!scanAttachments)
		{
			TransitionTextureView(sampledView, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
			return;
		}

		bool hasFeedback = false;
		LatteTextureViewD3D12* feedbackAttachment = nullptr;
		auto& overlapCache = m_feedbackOverlapCache[textureUnit];
		if (overlapCache.valid && overlapCache.sampledView == sampledView &&
			overlapCache.fbo == m_activeFBO && overlapCache.fboKey == m_activeFBO->key)
		{
			hasFeedback = overlapCache.hasFeedback;
			feedbackAttachment = overlapCache.attachment;
			m_feedbackOverlapCacheHits++;
		}
		else
		{
			for (const auto& attachment : m_activeFBO->GetColorAttachments())
			{
				if (attachment.valid && TextureViewSubresourcesOverlap(sampledView, attachment.view))
				{
					hasFeedback = true;
					feedbackAttachment = attachment.view;
					break;
				}
			}

			if (!hasFeedback && m_activeFBO->HasDepthStencilAttachment())
			{
				auto* depthView = static_cast<LatteTextureViewD3D12*>(m_activeFBO->depthBuffer.texture);
				hasFeedback = TextureViewSubresourcesOverlap(sampledView, depthView);
				if (hasFeedback)
					feedbackAttachment = depthView;
			}
			overlapCache = { sampledView, m_activeFBO, m_activeFBO->key, feedbackAttachment, hasFeedback, true };
			m_feedbackOverlapCacheMisses++;
		}

		if (D3D12RuntimeLogCapped(s_feedbackScanLogCount, "feedback scan", 128))
		{
			auto* sampledTexture = sampledView->GetBaseImage();
			auto* attachmentTexture = feedbackAttachment ? feedbackAttachment->GetBaseImage() : nullptr;
			cemuLog_log(LogType::Force, "D3D12: feedback scan unit={} sampled=0x{:08x}/mip{}+{}/slice{}+{} attachment=0x{:08x}/mip{}+{}/slice{}+{} feedback={}.",
						textureUnit,
						sampledTexture ? sampledTexture->physAddress : 0,
						sampledView->firstMip,
						sampledView->numMip,
						sampledView->firstSlice,
						sampledView->numSlice,
						attachmentTexture ? attachmentTexture->physAddress : 0,
						feedbackAttachment ? feedbackAttachment->firstMip : 0,
						feedbackAttachment ? feedbackAttachment->numMip : 0,
						feedbackAttachment ? feedbackAttachment->firstSlice : 0,
						feedbackAttachment ? feedbackAttachment->numSlice : 0,
						hasFeedback);
		}

		if (!hasFeedback)
		{
			TransitionTextureView(sampledView, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
			return;
		}

		m_feedbackLoopDetections++;
		// depth feedback keeps a fresh copy per draw
		const bool colorFeedback = !m_activeFBO->HasDepthStencilAttachment() ||
			feedbackAttachment != static_cast<LatteTextureViewD3D12*>(m_activeFBO->depthBuffer.texture);
		// depth: reuse the previous copy while the depth buffer was not written since (same data as a fresh copy)
		CreateFeedbackScratchCopy(textureUnit, sampledView, colorFeedback, !colorFeedback);
	};

	// bindings persist across shader changes; walk the same unit lists as the SRV tables
	if (PerformanceExperimentEnabled(D3D12PerformanceExperiment::ActiveFeedbackUnits))
	{
		D3D12ActiveTextureUnits::OrderedSet<kMaxTextureBindings> usedTextureUnits;
		auto addStageTextureUnits = [&](LatteDecompilerShader* shader,
			LatteConst::ShaderType expectedType, uint32 hostBase)
		{
			if (!shader)
				return;
			if (shader->shaderType != expectedType)
			{
				usedTextureUnits.IncludeAll();
				return;
			}
			usedTextureUnits.AddStage(shader->textureUnitList, shader->textureUnitListCount,
				hostBase, LATTE_NUM_MAX_TEX_UNITS);
		};
		if (!vertexShader)
			usedTextureUnits.IncludeAll();
		addStageTextureUnits(vertexShader, LatteConst::ShaderType::Vertex, LATTE_CEMU_VS_TEX_UNIT_BASE);
		addStageTextureUnits(geometryShader, LatteConst::ShaderType::Geometry, LATTE_CEMU_GS_TEX_UNIT_BASE);
		addStageTextureUnits(pixelShader, LatteConst::ShaderType::Pixel, LATTE_CEMU_PS_TEX_UNIT_BASE);
		usedTextureUnits.ForEach(resolveTextureUnit);
	}
	else
	{
		std::array<bool, kMaxTextureBindings> usedTextureUnits{};
		auto addStageTextureUnits = [&](LatteDecompilerShader* shader,
			LatteConst::ShaderType expectedType, uint32 hostBase)
		{
			if (!shader)
				return;
			if (shader->shaderType != expectedType)
			{
				usedTextureUnits.fill(true);
				return;
			}
			D3D12TextureUnitMask::AddStage(usedTextureUnits, shader->textureUnitList,
				shader->textureUnitListCount, hostBase, LATTE_NUM_MAX_TEX_UNITS);
		};
		if (!vertexShader)
			usedTextureUnits.fill(true);
		addStageTextureUnits(vertexShader, LatteConst::ShaderType::Vertex, LATTE_CEMU_VS_TEX_UNIT_BASE);
		addStageTextureUnits(geometryShader, LatteConst::ShaderType::Geometry, LATTE_CEMU_GS_TEX_UNIT_BASE);
		addStageTextureUnits(pixelShader, LatteConst::ShaderType::Pixel, LATTE_CEMU_PS_TEX_UNIT_BASE);
		for (uint32 textureUnit = 0; textureUnit < m_boundTextures.size(); ++textureUnit)
			if (usedTextureUnits[textureUnit])
				resolveTextureUnit(textureUnit);
	}
}

uint64 D3D12Renderer::FeedbackSourceGeneration(LatteTextureViewD3D12* view) const
{
	auto* texture = view ? view->GetBaseImage() : nullptr;
	if (!texture)
		return 0;
	const uint32 mipLevels = std::max<uint32>(texture->GetDesc().MipLevels, 1);
	const uint32 arraySize = texture->Is3DTexture() ? 1u : std::max<uint32>(texture->GetArraySize(), 1);
	const uint32 firstMip = (uint32)std::clamp<sint32>(view->firstMip, 0, (sint32)mipLevels - 1);
	const uint32 mipCount = std::min<uint32>(std::max<sint32>(view->numMip, 1), mipLevels - firstMip);
	const uint32 firstSlice = texture->Is3DTexture() ? 0u : (uint32)std::clamp<sint32>(view->firstSlice, 0, (sint32)arraySize - 1);
	const uint32 sliceCount = texture->Is3DTexture() ? 1u : std::min<uint32>(std::max<sint32>(view->numSlice, 1), arraySize - firstSlice);
	// per-subresource counters only grow, so any write between two calls changes the sum
	uint64 sum = 0;
	for (uint32 slice = 0; slice < sliceCount; ++slice)
		for (uint32 mip = 0; mip < mipCount; ++mip)
			sum += texture->GetContentGeneration(firstMip + mip + (firstSlice + slice) * mipLevels);
	return sum;
}

bool D3D12Renderer::CreateFeedbackScratchCopy(uint32 textureUnit, LatteTextureViewD3D12* sampledView, bool retainAcrossDraws, bool reuseWhileSourceUnchanged)
{
	if (!sampledView || !m_memoryManager || !m_descriptorManager)
		return false;
	auto* sourceTexture = sampledView->GetBaseImage();
	if (!sourceTexture || !sourceTexture->GetResource())
		return false;
	const uint64 drawId = m_gameDrawCallsSubmitted + 1;
	if (textureUnit >= m_feedbackScratchByTextureUnit.size())
		return false;
	if (GetFeedbackScratchCopy(textureUnit))
		return true;
	for (uint32 index = 0; index < m_feedbackScratchCopies.size(); ++index)
	{
		const auto& scratch = m_feedbackScratchCopies[index];
		if (scratch.drawId == drawId && scratch.sampledView == sampledView && scratch.texture.IsValid())
		{
			m_feedbackScratchByTextureUnit[textureUnit] = index;
			return true;
		}
	}
	if ((retainAcrossDraws || reuseWhileSourceUnchanged) && TryReuseRetainedFeedbackCopy(textureUnit, sampledView))
	{
		++m_feedbackCopyReuses;
		return true;
	}
	const auto srvDesc = sampledView->BuildSrvDesc();
	if (srvDesc.Format == DXGI_FORMAT_UNKNOWN)
		return false;

	const D3D12_RESOURCE_DESC sourceDesc = sourceTexture->GetDesc();
	D3D12MemoryManager::Resource scratchTexture;
	if (!TryAcquireCommandScratchTexture(sourceDesc, scratchTexture) &&
		!CreateTextureWithRecovery(sourceDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, scratchTexture, "feedback scratch"))
		return false;

	auto* commandList = GetCurrentCommandSlot().commandList.Get();
	Transition(scratchTexture.resource.Get(), scratchTexture.state, D3D12_RESOURCE_STATE_COPY_DEST);
	const uint32 mipLevels = std::max<uint32>(sourceDesc.MipLevels, 1);
	const uint32 arraySize = sourceTexture->Is3DTexture() ? 1u : std::max<uint32>(sourceTexture->GetArraySize(), 1);
	const uint32 firstMip = (uint32)std::clamp<sint32>(sampledView->firstMip, 0, (sint32)mipLevels - 1);
	const uint32 mipCount = std::min<uint32>(std::max<sint32>(sampledView->numMip, 1), mipLevels - firstMip);
	const uint32 firstSlice = sourceTexture->Is3DTexture() ? 0u : (uint32)std::clamp<sint32>(sampledView->firstSlice, 0, (sint32)arraySize - 1);
	const uint32 sliceCount = sourceTexture->Is3DTexture() ? 1u : std::min<uint32>(std::max<sint32>(sampledView->numSlice, 1), arraySize - firstSlice);
	std::vector<TextureTransition> localPreviousStates;
	std::vector<TextureTransition> localCopyStates;
	const bool retainTransitions = PerformanceExperimentEnabled(D3D12PerformanceExperiment::RetainedFeedbackTransitions);
	auto& previousStates = D3D12PerformanceExperiment::SelectTransitionStorage(
		retainTransitions, localPreviousStates, m_feedbackPreviousStateScratch);
	auto& copyStates = D3D12PerformanceExperiment::SelectTransitionStorage(
		retainTransitions, localCopyStates, m_feedbackCopyStateScratch);
#if defined(CEMU_UWP_PERFORMANCE_EXPERIMENT)
	m_performanceExperimentWindow.transitionAllocationOpportunities += 2;
	if (retainTransitions)
	{
		const uint64 growthAllocations = ((size_t)mipCount * sliceCount > previousStates.capacity() ? 1u : 0u) +
			((size_t)mipCount * sliceCount > copyStates.capacity() ? 1u : 0u);
		m_performanceExperimentWindow.retainedGrowthAllocations += growthAllocations;
		m_performanceExperimentWindow.transitionAllocationsAvoided += 2 - growthAllocations;
	}
#endif
	previousStates.reserve((size_t)mipCount * sliceCount);
	copyStates.reserve((size_t)mipCount * sliceCount);
	for (uint32 slice = 0; slice < sliceCount; ++slice)
	{
		for (uint32 mip = 0; mip < mipCount; ++mip)
		{
			const uint32 subresource = firstMip + mip + (firstSlice + slice) * mipLevels;
			previousStates.push_back({ subresource, sourceTexture->GetSubresourceState(subresource) });
			copyStates.push_back({ subresource, D3D12_RESOURCE_STATE_COPY_SOURCE });
		}
	}
	TransitionTextureSubresources(sourceTexture, copyStates);
	for (uint32 slice = 0; slice < sliceCount; ++slice)
	{
		for (uint32 mip = 0; mip < mipCount; ++mip)
		{
			const uint32 subresource = firstMip + mip + (firstSlice + slice) * mipLevels;
			D3D12_TEXTURE_COPY_LOCATION source{};
			source.pResource = sourceTexture->GetResource();
			source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
			source.SubresourceIndex = subresource;
			D3D12_TEXTURE_COPY_LOCATION destination{};
			destination.pResource = scratchTexture.resource.Get();
			destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
			destination.SubresourceIndex = subresource;
			commandList->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
		}
	}
	Transition(scratchTexture.resource.Get(), scratchTexture.state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	TransitionTextureSubresources(sourceTexture, previousStates);

	FeedbackScratchCopy scratch;
	scratch.texture = std::move(scratchTexture);
	scratch.requestedDesc = sourceDesc;
	scratch.srvDesc = srvDesc;
	scratch.sampledView = sampledView;
	scratch.textureUnit = textureUnit;
	scratch.drawId = drawId;
	scratch.retainAcrossDraws = retainAcrossDraws || reuseWhileSourceUnchanged;
	scratch.requireUnchangedSource = reuseWhileSourceUnchanged && !retainAcrossDraws;
	if (scratch.requireUnchangedSource)
		scratch.sourceGeneration = FeedbackSourceGeneration(sampledView);
	D3D12TraceEvent("feedback_copy", "draw={} hostUnit={} addr=0x{:08x} fmt=0x{:04x} size={}x{}x{} mip={}+{} slice={}+{} srvFormat={} sampledView={} scratchResource={} srvLifetime=shader_visible_draw_table",
					(unsigned long long)scratch.drawId,
					textureUnit,
					sourceTexture->physAddress,
					(uint32)sampledView->format,
					sourceTexture->width,
					sourceTexture->height,
					sourceTexture->depth,
					sampledView->firstMip,
					sampledView->numMip,
					sampledView->firstSlice,
					sampledView->numSlice,
					(uint32)srvDesc.Format,
					(const void*)sampledView,
					(const void*)scratch.texture.resource.Get());
	m_feedbackScratchCopies.emplace_back(std::move(scratch));
	m_feedbackScratchByTextureUnit[textureUnit] = (uint32)m_feedbackScratchCopies.size() - 1;
	m_feedbackLoopCopies++;
	return true;
}

const D3D12_FEATURE_DATA_FORMAT_SUPPORT& D3D12Renderer::GetCachedFormatSupport(DXGI_FORMAT format) const
{
	auto it = m_formatSupportCache.find((uint32)format);
	if (it != m_formatSupportCache.end())
		return it->second;

	D3D12_FEATURE_DATA_FORMAT_SUPPORT support{};
	support.Format = format;
	if (m_device)
		m_device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support));
	return m_formatSupportCache.emplace((uint32)format, support).first->second;
}

bool D3D12Renderer::FormatSupportsOutputMergerLogicOp(DXGI_FORMAT format) const
{
	if (!m_device || format == DXGI_FORMAT_UNKNOWN)
		return false;
	return (GetCachedFormatSupport(format).Support2 & D3D12_FORMAT_SUPPORT2_OUTPUT_MERGER_LOGIC_OP) != 0;
}

bool D3D12Renderer::FormatSupportsBlending(DXGI_FORMAT format) const
{
	if (!m_device || format == DXGI_FORMAT_UNKNOWN)
		return false;
	return (GetCachedFormatSupport(format).Support1 & D3D12_FORMAT_SUPPORT1_BLENDABLE) != 0;
}

bool D3D12Renderer::CreateLogicOpScratchCopy(LatteTextureViewD3D12* renderTargetView, uint32 renderTargetIndex)
{
	if (!renderTargetView || !m_memoryManager || !m_descriptorManager)
		return false;
	if (renderTargetIndex >= m_logicOpScratchByTarget.size())
		return false;
	if (GetLogicOpScratchCopy(renderTargetIndex))
		return true;

	auto* sourceTexture = renderTargetView->GetBaseImage();
	if (!sourceTexture || !sourceTexture->GetResource())
		return false;

	const D3D12_SHADER_RESOURCE_VIEW_DESC sourceSrvDesc = renderTargetView->BuildSrvDesc();
	if (sourceSrvDesc.Format == DXGI_FORMAT_UNKNOWN)
		return false;

	if (!m_commandListOpen)
		ResetCommandList();

	const D3D12_RESOURCE_DESC sourceDesc = sourceTexture->GetDesc();
	D3D12_RESOURCE_DESC scratchDesc = sourceDesc;
	D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = sourceSrvDesc;
	// the logic-op HLSL samples a single-mip, single-slice Texture2D<float4>
	if (sourceDesc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
		sourceDesc.SampleDesc.Count != 1 || renderTargetView->numSlice != 1 ||
		!SupportsUnorm8LogicOpEmulation(sourceSrvDesc.Format))
	{
		return false;
	}
	scratchDesc.Width = std::max<UINT64>(1, sourceDesc.Width >> renderTargetView->firstMip);
	scratchDesc.Height = std::max<UINT>(1, sourceDesc.Height >> renderTargetView->firstMip);
	scratchDesc.DepthOrArraySize = 1;
	scratchDesc.MipLevels = 1;
	scratchDesc.SampleDesc = { 1, 0 };
	scratchDesc.Flags = D3D12_RESOURCE_FLAG_NONE;
	srvDesc = {};
	srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srvDesc.Format = sourceSrvDesc.Format;
	srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	srvDesc.Texture2D.MostDetailedMip = 0;
	srvDesc.Texture2D.MipLevels = 1;
	srvDesc.Texture2D.PlaneSlice = 0;
	if (!m_commandListOpen)
		ResetCommandList();
	D3D12MemoryManager::Resource scratchTexture;
	if (!TryAcquireCommandScratchTexture(scratchDesc, scratchTexture) &&
		!CreateTextureWithRecovery(scratchDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, scratchTexture, "logic-op scratch"))
		return false;

	auto* commandList = GetCurrentCommandSlot().commandList.Get();
	Transition(scratchTexture.resource.Get(), scratchTexture.state, D3D12_RESOURCE_STATE_COPY_DEST);
	TransitionTextureView(renderTargetView, D3D12_RESOURCE_STATE_COPY_SOURCE);
	const uint32 sourceSubresource = sourceTexture->GetSubresourceIndex(renderTargetView->firstSlice, renderTargetView->firstMip);
	D3D12_TEXTURE_COPY_LOCATION source{};
	source.pResource = sourceTexture->GetResource();
	source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	source.SubresourceIndex = sourceSubresource;
	D3D12_TEXTURE_COPY_LOCATION destination{};
	destination.pResource = scratchTexture.resource.Get();
	destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	destination.SubresourceIndex = 0;
	commandList->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
#if defined(CEMU_UWP_D3D12_WORKLOAD_PROFILE)
	++m_uwpWorkloadProfile.initialLogicCopies;
	m_uwpWorkloadProfile.initialLogicPayloadBytes += D3D12WorkloadLogicCopyBytes(scratchDesc, srvDesc.Format);
#endif
	Transition(scratchTexture.resource.Get(), scratchTexture.state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	TransitionTextureView(renderTargetView, D3D12_RESOURCE_STATE_RENDER_TARGET);

	FeedbackScratchCopy scratch;
	scratch.texture = std::move(scratchTexture);
	scratch.requestedDesc = scratchDesc;
	scratch.srvDesc = srvDesc;
	scratch.textureUnit = std::numeric_limits<uint32>::max();
	scratch.logicOpRenderTarget = renderTargetIndex;
	scratch.drawId = m_gameDrawCallsSubmitted + 1;
	D3D12TraceEvent("logic_op_scratch_copy",
					"draw={} rt={} src=0x{:08x} fmt=0x{:04x} srcDxgi={} srvDxgi={} mip={} slice={} scratchSize={}x{} srcResource={} scratchResource={} pendingScratchBefore={} srvLifetime=shader_visible_draw_table",
					(unsigned long long)scratch.drawId,
					renderTargetIndex,
					sourceTexture->physAddress,
					(uint32)renderTargetView->format,
					(uint32)sourceDesc.Format,
					(uint32)srvDesc.Format,
					renderTargetView->firstMip,
					renderTargetView->firstSlice,
					(unsigned long long)scratchDesc.Width,
					scratchDesc.Height,
					(const void*)sourceTexture->GetResource(),
					(const void*)scratch.texture.resource.Get(),
					m_feedbackScratchCopies.size());
	m_feedbackScratchCopies.emplace_back(std::move(scratch));
	m_logicOpScratchByTarget[renderTargetIndex] = (uint32)m_feedbackScratchCopies.size() - 1;
	m_logicOpEmulationCopies++;
	return true;
}

bool D3D12Renderer::RefreshLogicOpScratchCopiesForDraw(uint64 drawId)
{
	if (!m_activeFBO || !m_commandListOpen)
		return false;

	auto* commandList = GetCurrentCommandSlot().commandList.Get();
	const auto& attachments = m_activeFBO->GetColorAttachments();
	bool foundScratch = false;
	for (uint32 renderTargetIndex = 0; renderTargetIndex < m_logicOpScratchByTarget.size(); ++renderTargetIndex)
	{
		auto* scratch = GetLogicOpScratchCopy(renderTargetIndex);
		if (!scratch || scratch->drawId != drawId)
			continue;
		if (scratch->logicOpRenderTarget >= attachments.size())
			return false;

		const auto& attachment = attachments[scratch->logicOpRenderTarget];
		auto* view = attachment.valid ? attachment.view : nullptr;
		auto* texture = view ? view->GetBaseImage() : nullptr;
		if (!view || !texture || !texture->GetResource())
			return false;

		// after the full first snapshot only the scissor can change between primitives
		const auto& sourceDesc = texture->GetDesc();
		const bool validMip = view->firstMip >= 0 && view->firstMip < sourceDesc.MipLevels && view->firstMip < 32;
#if defined(CEMU_UWP_PERFORMANCE_EXPERIMENT)
		const bool scissorBoundedRefresh = PerformanceExperimentEnabled(D3D12PerformanceExperiment::LogicCopyScissor);
#else
		constexpr bool scissorBoundedRefresh = true;
#endif
		const bool boundedAllowed = validMip && m_boundScissorValid && scissorBoundedRefresh &&
			sourceDesc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && sourceDesc.SampleDesc.Count == 1 &&
			(sourceDesc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) == 0 &&
			SupportsUnorm8LogicOpEmulation(scratch->srvDesc.Format);
		const auto region = D3D12LogicOpCopyRegion::Select(boundedAllowed,
			{ m_boundScissor.left, m_boundScissor.top, m_boundScissor.right, m_boundScissor.bottom },
			validMip ? std::max<UINT64>(1, sourceDesc.Width >> view->firstMip) : 0,
			validMip ? std::max<UINT>(1, sourceDesc.Height >> view->firstMip) : 0,
			scratch->requestedDesc.Width, scratch->requestedDesc.Height);
		foundScratch = true;
		if (region.kind == D3D12LogicOpCopyRegion::Kind::Empty)
		{
#if defined(CEMU_UWP_PERFORMANCE_EXPERIMENT)
			if (m_performanceExperiment.Measuring()) ++m_performanceExperimentWindow.logicEmptyRefreshSkips;
#endif
			continue;
		}
		const uint32 sourceSubresource = texture->GetSubresourceIndex(view->firstSlice, view->firstMip);
		Transition(scratch->texture.resource.Get(), scratch->texture.state, D3D12_RESOURCE_STATE_COPY_DEST);
		TransitionTextureView(view, D3D12_RESOURCE_STATE_COPY_SOURCE);

		D3D12_TEXTURE_COPY_LOCATION source{};
		source.pResource = texture->GetResource();
		source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		source.SubresourceIndex = sourceSubresource;
		D3D12_TEXTURE_COPY_LOCATION destination{};
		destination.pResource = scratch->texture.resource.Get();
		destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		destination.SubresourceIndex = 0;
		const bool partial = region.kind == D3D12LogicOpCopyRegion::Kind::Partial;
		const D3D12_BOX box{ region.left, region.top, 0, region.right, region.bottom, 1 };
		commandList->CopyTextureRegion(&destination, partial ? region.left : 0, partial ? region.top : 0, 0,
			&source, partial ? &box : nullptr);
#if defined(CEMU_UWP_D3D12_WORKLOAD_PROFILE) || defined(CEMU_UWP_PERFORMANCE_EXPERIMENT)
		auto payloadDesc = scratch->requestedDesc;
		if (partial)
		{
			payloadDesc.Width = region.right - region.left;
			payloadDesc.Height = region.bottom - region.top;
		}
		const uint64 payloadBytes = D3D12WorkloadLogicCopyBytes(payloadDesc, scratch->srvDesc.Format);
#endif
#if defined(CEMU_UWP_D3D12_WORKLOAD_PROFILE)
		++m_uwpWorkloadProfile.refreshLogicCopies;
		m_uwpWorkloadProfile.refreshLogicPayloadBytes += payloadBytes;
		if (m_logicOpEmulationMode < m_uwpWorkloadProfile.modeRefreshCopies.size())
			++m_uwpWorkloadProfile.modeRefreshCopies[m_logicOpEmulationMode];
#endif
#if defined(CEMU_UWP_PERFORMANCE_EXPERIMENT)
		if (m_performanceExperiment.Measuring())
		{
			++m_performanceExperimentWindow.logicRefreshCopies;
			m_performanceExperimentWindow.logicRefreshPayloadBytes += payloadBytes;
			m_performanceExperimentWindow.logicFullRefreshPayloadBytes +=
				D3D12WorkloadLogicCopyBytes(scratch->requestedDesc, scratch->srvDesc.Format);
		}
#endif

		Transition(scratch->texture.resource.Get(), scratch->texture.state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		TransitionTextureView(view, D3D12_RESOURCE_STATE_RENDER_TARGET);
		foundScratch = true;
	}
	return foundScratch;
}

bool D3D12Renderer::PrepareLogicOpEmulationForDraw()
{
	m_logicOpEmulationMode = 0;
	m_disableNativeLogicOpForDraw = false;

	if (!m_activeFBO)
		return true;

	const auto logicOp = LatteGPUState.contextNew.CB_COLOR_CONTROL.get_ROP();
	D3D12_LOGIC_OP d3dLogicOp = D3D12_LOGIC_OP_COPY;
	if (!TryGetD3D12LogicOp(logicOp, d3dLogicOp))
	{
		cemuLog_logOnce(LogType::Force, "D3D12: unknown GX2 logic operation 0x{:02x} is drawn as COPY.", static_cast<uint32>(logicOp));
		// an enabled logic op still disables blending
		m_disableNativeLogicOpForDraw = true;
		return true;
	}
	const uint32 mode = GetD3D12LogicOpEmulationMode(d3dLogicOp);
	if (mode == 0)
		return true;

	const auto& attachments = m_activeFBO->GetColorAttachments();
	auto* activePixelShader = LatteSHRC_GetActivePixelShader();
	const uint32 outputMask = activePixelShader ? activePixelShader->pixelColorOutputMask : 0u;
	const uint32 targetMask = GetD3D12EffectiveRenderTargetMask(LatteGPUState.contextNew);
	const uint8 nativeWriteMask = static_cast<uint8>(targetMask & 0xF);
	// native logic ops apply slot 0 to every RTV; a sparse FBO has a dummy at slot 0
	bool nativeLogicOpRepresentable =
		m_outputMergerLogicOpSupported &&
		!attachments.empty() &&
		attachments[0].valid;
	std::vector<uint32> logicTargets;
	logicTargets.reserve(std::min<size_t>(attachments.size(), kGameLogicOpDstSrvCount));
	for (uint32 i = 0; i < m_activeFBO->GetColorAttachmentCount() && i < attachments.size() && i < kGameLogicOpDstSrvCount; ++i)
	{
		if (!attachments[i].valid)
			continue;

		const bool shaderWritesTarget = (outputMask & (1u << i)) != 0;
		const uint8 targetWriteMask = static_cast<uint8>((targetMask >> (i * 4)) & 0xF);
		if (!shaderWritesTarget || targetWriteMask == 0)
			continue;
		logicTargets.push_back(i);
		if (!FormatSupportsOutputMergerLogicOp(attachments[i].format))
			nativeLogicOpRepresentable = false;
		if (targetWriteMask != nativeWriteMask)
			nativeLogicOpRepresentable = false;
	}

	if (!m_outputMergerLogicOpSupported)
		cemuLog_logOnce(LogType::Force, "D3D12: output-merger logic ops are not supported by this device; using shader emulation where possible.");
	if (nativeLogicOpRepresentable)
		return true;
	if (logicTargets.empty())
		return true;

	// native logic ops force RenderTarget[0] state onto every MRT
	m_disableNativeLogicOpForDraw = true;

	const bool needsDestination = D3D12LogicOpEmulationNeedsDestination(mode);
	bool hasIgnoringTargets = false;
	bool hasEmulatedTargets = false;
	for (uint32 renderTargetIndex : logicTargets)
	{
		const auto& attachment = attachments[renderTargetIndex];
		if (D3D12LogicOpIgnoresFormat(attachment.format))
		{
			hasIgnoringTargets = true;
			continue;
		}
		if (!attachment.view ||
			(!SupportsUnorm8LogicOpEmulation(attachment.format) && !SupportsLogicOpEmulationWithoutDestination(attachment.format, mode)))
		{
			static uint32 s_unsupportedLogicFormatLogCount = 0;
			if (D3D12RuntimeLogCapped(s_unsupportedLogicFormatLogCount, "unsupported logic-op format", 32))
			{
				cemuLog_log(LogType::Force,
					"D3D12: skipping logic-op draw 0x{:02x}; MRT {} format {} cannot be represented by the UNORM shader fallback.",
					static_cast<uint32>(logicOp), renderTargetIndex, static_cast<uint32>(attachment.format));
			}
			return false;
		}
		hasEmulatedTargets = true;
		if (!needsDestination)
			continue;
		const auto* texture = attachment.view->GetBaseImage();
		if (!texture || texture->GetDesc().Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
			texture->GetDesc().SampleDesc.Count != 1 || attachment.view->numSlice != 1)
		{
			static uint32 s_unsupportedLogicDimensionLogCount = 0;
			if (D3D12RuntimeLogCapped(s_unsupportedLogicDimensionLogCount, "unsupported logic-op dimensions", 32))
			{
				cemuLog_log(LogType::Force,
					"D3D12: skipping logic-op draw 0x{:02x}; MRT {} is not a single-sample, single-slice Texture2D view.",
					static_cast<uint32>(logicOp), renderTargetIndex);
			}
			return false;
		}
	}
	if (!hasEmulatedTargets)
		return true;
	if (hasIgnoringTargets)
	{
		// the shader applies one emulation mode to every color output
		static uint32 s_mixedLogicFormatLogCount = 0;
		if (D3D12RuntimeLogCapped(s_mixedLogicFormatLogCount, "mixed logic-op formats", 32))
		{
			cemuLog_log(LogType::Force,
				"D3D12: skipping logic-op draw 0x{:02x}; it mixes float or sRGB targets with fixed-point targets.",
				static_cast<uint32>(logicOp));
		}
		return false;
	}

	if (!needsDestination)
	{
		m_logicOpEmulationMode = mode;
		m_logicOpEmulatedDraws++;
		return true;
	}

	for (uint32 renderTargetIndex : logicTargets)
	{
		if (!CreateLogicOpScratchCopy(attachments[renderTargetIndex].view, renderTargetIndex))
		{
			static uint32 s_logicScratchFailureLogCount = 0;
			if (D3D12RuntimeLogCapped(s_logicScratchFailureLogCount, "logic-op scratch allocation failure", 32))
			{
				cemuLog_log(LogType::Force,
					"D3D12: skipping logic-op draw 0x{:02x}; destination scratch creation failed for MRT {}.",
					static_cast<uint32>(logicOp), renderTargetIndex);
			}
			return false;
		}
	}

	static uint32 s_logicOpEmulationLogCount = 0;
	if (D3D12RuntimeLogCapped(s_logicOpEmulationLogCount, "logic-op emulation", 64))
		cemuLog_log(LogType::Force, "D3D12: emulating logic op 0x{:02x} across {} MRT target(s) through scratch SRVs t{}..t{}.",
					(uint32)logicOp,
					logicTargets.size(),
					kGameLogicOpDstSrvBinding,
					kGameLogicOpDstSrvBinding + kGameLogicOpDstSrvCount - 1);
	m_logicOpEmulationMode = mode;
	m_logicOpEmulatedDraws++;
	return true;
}

void D3D12Renderer::BindFramebuffer(CachedFBOD3D12* fbo)
{
	if (!m_commandListOpen)
		return;

	if (!fbo)
	{
		SetD3D12RenderTargets(0, nullptr, nullptr);
		return;
	}

	UINT rtvCount = std::min<UINT>(fbo->GetColorAttachmentCount(), 8);
	std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 8> rtvHandles{};
	bool usedNullRtv = false;
	bool missingRealRtv = false;
	{
		const auto& colorAttachments = fbo->GetColorAttachments();
		const auto& fboRtvHandles = fbo->GetRtvHandles();
		for (uint32 i = 0; i < rtvCount && i < colorAttachments.size(); ++i)
		{
			const auto& colorAttachment = colorAttachments[i];
			const bool realRtvValid = colorAttachment.valid && colorAttachment.view && fboRtvHandles[i].ptr != 0;
			if (!realRtvValid)
			{
				missingRealRtv = true;
				if (m_sparseMrtNullRtv.IsValid() && m_sparseMrtNullRtv.cpu.ptr != 0)
				{
					rtvHandles[i] = m_sparseMrtNullRtv.cpu;
					usedNullRtv = true;
				}
				continue;
			}
			rtvHandles[i] = fboRtvHandles[i];
			TransitionTextureView(colorAttachment.view, D3D12_RESOURCE_STATE_RENDER_TARGET);
		}
	}

	if (fbo->GetColorAttachmentCount() > rtvCount)
	{
		static uint32 s_tooManyRtLogCount = 0;
		if (D3D12RuntimeLogCapped(s_tooManyRtLogCount, "FBO RTV overflow", 32))
			cemuLog_log(LogType::Force, "D3D12: FBO key=0x{:016x} exposes {} color attachments; clamping to D3D12 MRT limit {}.",
						fbo->key,
						fbo->GetColorAttachmentCount(),
						rtvCount);
	}
	if (missingRealRtv)
	{
		static uint32 s_missingRtvLogCount = 0;
		if (D3D12RuntimeLogCapped(s_missingRtvLogCount, "missing FBO RTV", 128))
			cemuLog_log(LogType::Force, "D3D12: FBO key=0x{:016x} has missing/sparse RTV slots within count={} mask=0x{:02x}; nullRtv={} usedNull={}.",
						fbo->key,
						rtvCount,
						fbo->GetColorAttachmentMask(),
						m_sparseMrtNullRtv.IsValid() && m_sparseMrtNullRtv.cpu.ptr != 0,
						usedNullRtv);
	}
	if (rtvCount != 0)
	{
		for (uint32 i = 0; i < rtvCount; ++i)
		{
			if (rtvHandles[i].ptr != 0)
				continue;

			static uint32 s_invalidRtvLogCount = 0;
			if (D3D12RuntimeLogCapped(s_invalidRtvLogCount, "invalid FBO RTV", 128))
				cemuLog_log(LogType::Force, "D3D12: refusing to bind FBO key=0x{:016x}; RTV slot {} has no descriptor and no null RTV is available.",
							fbo->key,
							i);
			SetD3D12RenderTargets(0, nullptr, nullptr);
			return;
		}
	}

	D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle{};
	D3D12_CPU_DESCRIPTOR_HANDLE* dsvHandlePtr = nullptr;
	if (fbo->HasDepthStencilAttachment() && fbo->depthBuffer.texture)
	{
		auto* depthView = static_cast<LatteTextureViewD3D12*>(fbo->depthBuffer.texture);
		TransitionTextureView(depthView, D3D12_RESOURCE_STATE_DEPTH_WRITE, true);
		dsvHandle = fbo->GetDsvHandle();
		if (dsvHandle.ptr != 0)
			dsvHandlePtr = &dsvHandle;
	}

	SetD3D12RenderTargets(rtvCount, rtvCount != 0 ? rtvHandles.data() : nullptr, dsvHandlePtr);
}

void D3D12Renderer::LogTextureStats(std::string_view label, LatteTextureViewD3D12* textureView, bool padView, uint32& counter, uint32 limit)
{
	if (!D3D12ShouldChecksumOutput() || !textureView)
		return;

	if (counter >= limit)
		return;

	auto* texture = textureView->GetBaseImage();
	if (!texture || !texture->GetResource())
		return;

	sint32 width = 0;
	sint32 height = 0;
	texture->GetEffectiveSize(width, height, textureView->firstMip);
	if (width <= 0 || height <= 0)
		return;

	std::unique_ptr<LatteTextureReadbackInfo> readback(CreateHostTextureReadback(textureView));
	if (!readback)
		return;

	readback->StartTransfer();
	readback->ForceFinish();
	const uint8* data = readback->GetData();
	if (!data)
		return;

	DXGI_FORMAT viewFormat = textureView->GetViewFormat();
	if (viewFormat == DXGI_FORMAT_UNKNOWN)
		viewFormat = texture->GetFormatInfo().srvFormat;
	if (viewFormat == DXGI_FORMAT_UNKNOWN)
		viewFormat = texture->GetResourceFormat();

	const uint32 bytesPerPixel = D3D12ChecksumBytesPerPixel(viewFormat);
	const bool compressedStats = texture->GetFormatInfo().isCompressed;
	const size_t readbackBytes = readback->GetImageSize();
	const size_t logicalByteCount = (size_t)width * (size_t)height * std::max<uint32>(bytesPerPixel, 1);
	const size_t byteCount = compressedStats ? readbackBytes : std::min<size_t>(logicalByteCount, readbackBytes);
	const size_t inspectedBytes = std::min<size_t>(byteCount, 8ull * 1024ull * 1024ull);
	uint64 byteSum = 0;
	uint64 nonZeroBytes = 0;
	uint64 hash = 1469598103934665603ull;
	for (size_t i = 0; i < inspectedBytes; i++)
	{
		const uint8 value = data[i];
		byteSum += value;
		nonZeroBytes += value != 0;
		hash ^= value;
		hash *= 1099511628211ull;
	}

	uint64 nonZeroRgbPixels = 0;
	if (!compressedStats && bytesPerPixel >= 4)
	{
		const size_t pixelCount = std::min<size_t>((size_t)width * (size_t)height, inspectedBytes / bytesPerPixel);
		for (size_t i = 0; i < pixelCount; i++)
		{
			const uint8* pixel = data + i * bytesPerPixel;
			nonZeroRgbPixels += (pixel[0] | pixel[1] | pixel[2]) != 0;
		}
	}

	counter++;
	cemuLog_log(LogType::Force,
				"D3D12: {} stats #{} {} addr=0x{:08x} fmt=0x{:04x}/dxgi={} size={}x{} bpp={} compressed={} readbackBytes={} logicalBytes={} inspected={} nonZeroBytes={} nonZeroRgbPixels={} sum={} hash=0x{:016x}.",
				label,
				counter,
				padView ? "pad" : "TV",
				texture->physAddress,
				(uint32)texture->format,
				(uint32)viewFormat,
				width,
				height,
				bytesPerPixel,
				compressedStats ? 1 : 0,
				(unsigned long long)readbackBytes,
				(unsigned long long)logicalByteCount,
				(unsigned long long)inspectedBytes,
				(unsigned long long)nonZeroBytes,
				(unsigned long long)nonZeroRgbPixels,
				(unsigned long long)byteSum,
				(unsigned long long)hash);
}

void D3D12Renderer::LogOutputTextureStats(LatteTextureViewD3D12* textureView, bool padView)
{
	static uint32 s_outputChecksumLogCount = 0;
	LogTextureStats("output source", textureView, padView, s_outputChecksumLogCount, 16);
}

void D3D12Renderer::LogActiveRenderTargetStats()
{
	if (!D3D12ShouldChecksumOutput() || !m_activeFBO)
		return;
	static uint32 s_activeRenderTargetChecksumLogCount = 0;
	const auto& colorAttachments = m_activeFBO->GetColorAttachments();
	for (uint32 i = 0; i < colorAttachments.size(); i++)
	{
		if (!colorAttachments[i].valid || !colorAttachments[i].view)
			continue;
		LogTextureStats(fmt::format("active RT{}", i), colorAttachments[i].view, false, s_activeRenderTargetChecksumLogCount, 24);
		return;
	}
}

void D3D12Renderer::TraceTextureSnapshot(std::string_view eventName, std::string_view phase, LatteTextureViewD3D12* textureView, bool padView, std::string_view extra)
{
	if (!D3D12ShouldTrace())
		return;
	auto& traceSink = D3D12TraceSink::Get();
	if (traceSink.PollGpuCaptureArm())
		m_traceTextureStatsCache.clear();

	auto* texture = textureView ? textureView->GetBaseImage() : nullptr;
	if (!texture || !texture->GetResource())
	{
		traceSink.Write(eventName, fmt::format("phase={} pad={} textureView={} texture=null captureGeneration={} {}", phase, padView ? 1 : 0, (const void*)textureView, (unsigned long long)traceSink.GpuCaptureGeneration(), extra));
		return;
	}

	sint32 width = 0;
	sint32 height = 0;
	texture->GetEffectiveSize(width, height, textureView->firstMip);
	DXGI_FORMAT viewFormat = textureView->GetViewFormat();
	if (viewFormat == DXGI_FORMAT_UNKNOWN)
		viewFormat = texture->GetFormatInfo().srvFormat;
	if (viewFormat == DXGI_FORMAT_UNKNOWN)
		viewFormat = texture->GetResourceFormat();

	const uint32 subresource = texture->GetSubresourceIndex(texture->Is3DTexture() ? 0 : textureView->firstSlice, textureView->firstMip);
	const auto textureState = texture->GetSubresourceState(subresource);
	const auto& srv = textureView->GetSRV();
	const auto& rtv = textureView->GetRTV();
	const auto& dsv = textureView->GetDSV();
	const std::string lineageExtra = BuildTextureLineageExtra(texture->physAddress);
	uint64 lineageSerial = 0;
	if (auto lineageIt = m_textureLineage.find(texture->physAddress); lineageIt != m_textureLineage.end())
		lineageSerial = lineageIt->second.serial;
	const auto basePayload = fmt::format("phase={} pad={} addr=0x{:08x} mipAddr=0x{:08x} fmt=0x{:04x} viewDxgi={} resourceDxgi={} dim={} size={}x{}x{} pitch={} mips={} viewMip={} viewMipCount={} viewSlice={} viewSliceCount={} subresource={} state=0x{:08x} swizzle=0x{:08x} tileMode={} isDepth={} hasStencil={} view={} texture={} resource={} srvValid={} srvIndex={} srvCpu=0x{:016x} srvGpu=0x{:016x} rtvValid={} rtvIndex={} rtvCpu=0x{:016x} dsvValid={} dsvIndex={} dsvCpu=0x{:016x} captureGeneration={} {}",
										 phase,
										 padView ? 1 : 0,
										 texture->physAddress,
										 texture->physMipAddress,
										 (uint32)texture->format,
										 (uint32)viewFormat,
										 (uint32)texture->GetResourceFormat(),
										 (uint32)texture->dim,
										 width,
										 height,
										 texture->depth,
										 texture->pitch,
										 texture->mipLevels,
										 textureView->firstMip,
										 textureView->numMip,
										 textureView->firstSlice,
										 textureView->numSlice,
										 subresource,
										 (uint32)textureState,
										 texture->swizzle,
										 (uint32)texture->tileMode,
										 texture->isDepth ? 1 : 0,
										 texture->hasStencil ? 1 : 0,
										 (const void*)textureView,
										 (const void*)texture,
										 (const void*)texture->GetResource(),
										 srv.IsValid() ? 1 : 0,
										 srv.IsValid() ? srv.index : 0,
										 (unsigned long long)(srv.IsValid() ? srv.cpu.ptr : 0),
										 (unsigned long long)(srv.IsValid() ? srv.gpu.ptr : 0),
										 rtv.IsValid() ? 1 : 0,
										 rtv.IsValid() ? rtv.index : 0,
										 (unsigned long long)(rtv.IsValid() ? rtv.cpu.ptr : 0),
										 dsv.IsValid() ? 1 : 0,
										 dsv.IsValid() ? dsv.index : 0,
										 (unsigned long long)(dsv.IsValid() ? dsv.cpu.ptr : 0),
										 (unsigned long long)traceSink.GpuCaptureGeneration(),
										 fmt::format("{} {}", lineageExtra, extra));
	uint64 traceStatsKey = lineageSerial ? 1469598103934665603ull : 0;
	if (traceStatsKey != 0)
	{
		const uint64 keyParts[] =
		{
			(uint64)texture->physAddress,
			(uint64)subresource,
			(uint64)(uint32)viewFormat,
			(uint64)(uint32)textureState,
			(uint64)(uint32)width,
			(uint64)(uint32)height,
			D3D12TraceGpuSamplesOnly() ? 1ull : 0ull,
			lineageSerial,
		};
		for (uint64 part : keyParts)
		{
			traceStatsKey ^= part;
			traceStatsKey *= 1099511628211ull;
		}
		if (traceSink.CanReuseGpuStats(eventName, basePayload, texture->physAddress))
		{
			if (auto cachedStats = m_traceTextureStatsCache.find(traceStatsKey); cachedStats != m_traceTextureStatsCache.end())
			{
				traceSink.Write(eventName, basePayload + cachedStats->second);
				return;
			}
		}
	}
	const auto gpuStatsDecision = traceSink.TryBeginGpuStats(eventName, basePayload, texture->physAddress);
	if (!gpuStatsDecision.capture)
	{
		D3D12TraceSink::Get().Write(eventName, basePayload + fmt::format(" stats=metadata_only reason={}", gpuStatsDecision.reason));
		return;
	}

	if (width <= 0 || height <= 0)
	{
		D3D12TraceSink::Get().Write(eventName, basePayload + " stats=invalid_size");
		return;
	}

	const size_t expectedLogicalByteCount = (size_t)width * (size_t)height * std::max<uint32>(D3D12ChecksumBytesPerPixel(viewFormat), 1);
	D3D12TraceSink::Get().WriteAndFlush("gpu_stats_capture_begin",
										basePayload + fmt::format(" requestedEvent={} decision={} expectedLogicalBytes={} traceStatsKey=0x{:016x}",
																 eventName,
																 gpuStatsDecision.reason,
																 (unsigned long long)expectedLogicalByteCount,
																 (unsigned long long)traceStatsKey));

	std::unique_ptr<LatteTextureReadbackInfo> readback(CreateHostTextureReadback(textureView));
	if (!readback)
	{
		D3D12TraceSink::Get().WriteAndFlush("gpu_stats_capture_end",
											basePayload + fmt::format(" requestedEvent={} result=readback_unavailable", eventName));
		D3D12TraceSink::Get().Write(eventName, basePayload + " stats=readback_unavailable");
		return;
	}

	readback->StartTransfer();
	readback->ForceFinish();
	const size_t readbackBytes = readback->GetImageSize();
	D3D12TraceSink::Get().WriteAndFlush("gpu_stats_capture_end",
										basePayload + fmt::format(" requestedEvent={} result=readback_ready readbackBytes={}",
																 eventName,
																 (unsigned long long)readbackBytes));
	const uint8* data = readback->GetData();
	if (!data)
	{
#if defined(CEMU_UWP_D3D12_DIAGNOSTICS)
		const auto* diagnosticReadback = static_cast<LatteTextureReadbackInfoD3D12*>(readback.get());
		D3D12TraceSink::Get().WriteAndFlush("gpu_stats_readback_validation",
			basePayload + fmt::format(" status={} mapHr=0x{:08x} reservationOffset={} reservationSize={} copyBytes={} fence={}",
				diagnosticReadback->GetDiagnosticReadbackStatus(), (uint32)diagnosticReadback->GetDiagnosticMapResult(),
				diagnosticReadback->GetDiagnosticReservationOffset(), diagnosticReadback->GetDiagnosticReservationSize(),
				diagnosticReadback->GetDiagnosticCopySize(), diagnosticReadback->GetDiagnosticFenceValue()));
#endif
		D3D12TraceSink::Get().WriteAndFlush("gpu_stats_capture_end",
											basePayload + fmt::format(" requestedEvent={} result=readback_null", eventName));
		D3D12TraceSink::Get().Write(eventName, basePayload + " stats=readback_null");
		return;
	}

	const uint32 bytesPerPixel = D3D12ChecksumBytesPerPixel(viewFormat);
	const bool compressedStats = texture->GetFormatInfo().isCompressed;
	const size_t logicalByteCount = (size_t)width * (size_t)height * std::max<uint32>(bytesPerPixel, 1);
	const size_t byteCount = compressedStats ? readbackBytes : std::min<size_t>(logicalByteCount, readbackBytes);
	const std::string samplesPayload = D3D12BuildTraceGpuSamplesPayload(data, byteCount, (uint32)width, (uint32)height, bytesPerPixel, viewFormat, compressedStats);
	std::string gridPayload = D3D12BuildTraceGpuGridPayload(data, byteCount, (uint32)width, (uint32)height, bytesPerPixel, compressedStats);
#if defined(CEMU_UWP_D3D12_DIAGNOSTICS)
	const auto* diagnosticReadback = static_cast<LatteTextureReadbackInfoD3D12*>(readback.get());
	const auto& diagnosticFootprint = diagnosticReadback->GetDiagnosticFootprint();
	gridPayload += fmt::format(" readbackValidation={} mapHr=0x{:08x} footprintDxgi={} footprintSize={}x{}x{} footprintRowPitch={} tightRowBytes={} reservationOffset={} reservationSize={} copyBytes={} fence={} typedInterpretation=declared_view{}",
		diagnosticReadback->GetDiagnosticReadbackStatus(), (uint32)diagnosticReadback->GetDiagnosticMapResult(),
		(uint32)diagnosticFootprint.Format, diagnosticFootprint.Width, diagnosticFootprint.Height,
		diagnosticFootprint.Depth, diagnosticFootprint.RowPitch, diagnosticReadback->GetRowSizeInBytes(),
		diagnosticReadback->GetDiagnosticReservationOffset(), diagnosticReadback->GetDiagnosticReservationSize(),
		diagnosticReadback->GetDiagnosticCopySize(), diagnosticReadback->GetDiagnosticFenceValue(),
		texture->isDepth ? " depthReadbackLayout=unverified" : "");
	// the byte-level RGB/alpha heuristics below are meaningless for float/depth surfaces
	const auto typed = D3D12TypedSurfaceStats::Analyze(data, readbackBytes, (uint32)viewFormat,
		width, height, static_cast<LatteTextureReadbackInfoD3D12*>(readback.get())->GetRowSizeInBytes());
	gridPayload += fmt::format(" typed={} typedPixels={}/{} typedCapped={} typedComponents=[",
		D3D12TypedSurfaceStats::StatusName(typed.status), typed.inspectedPixels, typed.expectedPixels, typed.capped);
	for (uint32 channel = 0; channel < typed.componentCount; ++channel)
	{
		const auto& c = typed.components[channel];
		gridPayload += fmt::format("{}:finite={}/nan={}/inf={}/min={:.9g}/max={:.9g};",
			channel, c.finite, c.nan, c.inf, c.minimum, c.maximum);
	}
	gridPayload += fmt::format("] xyFinite={} xyNonFinite={} xyRadiusAbove1={} xyRadiusAbove1_0001={} (XY interpreted directly; normal test meaningful only for normal surfaces)",
		typed.xyFinite, typed.xyNonFinite, typed.xyRadiusSquaredAboveOne, typed.xyRadiusSquaredAboveTolerance);
#endif
	if (D3D12TraceGpuSamplesOnly())
	{
		const std::string statsPayload = fmt::format(" stats=ok cached=0 samplesOnly=1 bpp={} compressed={} readbackBytes={} logicalBytes={} {}{}",
													 bytesPerPixel,
													 compressedStats ? 1 : 0,
													 (unsigned long long)readbackBytes,
													 (unsigned long long)logicalByteCount,
													 samplesPayload,
													 gridPayload);
		if (traceStatsKey != 0)
		{
			if (m_traceTextureStatsCache.size() > 16384)
				m_traceTextureStatsCache.clear();
			std::string cachedPayload = statsPayload;
			const size_t cachedFlag = cachedPayload.find("cached=0");
			if (cachedFlag != std::string::npos)
				cachedPayload.replace(cachedFlag, std::strlen("cached=0"), "cached=1");
			m_traceTextureStatsCache.emplace(traceStatsKey, std::move(cachedPayload));
		}
		D3D12TraceSink::Get().Write(eventName, basePayload + statsPayload);
		return;
	}
	const size_t inspectedBytes = std::min<size_t>(byteCount, 64ull * 1024ull * 1024ull);
	if (inspectedBytes == 0)
	{
		D3D12TraceSink::Get().WriteAndFlush("gpu_stats_capture_end",
											basePayload + fmt::format(" requestedEvent={} result=empty_readback readbackBytes={} logicalBytes={}",
																	 eventName,
																	 (unsigned long long)readbackBytes,
																	 (unsigned long long)logicalByteCount));
		D3D12TraceSink::Get().Write(eventName,
									basePayload + fmt::format(" stats=empty_readback bpp={} compressed={} readbackBytes={} logicalBytes={}",
															  bytesPerPixel,
															  compressedStats ? 1 : 0,
															  (unsigned long long)readbackBytes,
															  (unsigned long long)logicalByteCount));
		return;
	}
	uint64 byteSum = 0;
	uint64 nonZeroBytes = 0;
	uint64 hash = 1469598103934665603ull;
	uint8 minByte = 255;
	uint8 maxByte = 0;
	for (size_t i = 0; i < inspectedBytes; i++)
	{
		const uint8 value = data[i];
		byteSum += value;
		nonZeroBytes += value != 0;
		minByte = std::min<uint8>(minByte, value);
		maxByte = std::max<uint8>(maxByte, value);
		hash ^= value;
		hash *= 1099511628211ull;
	}

	uint64 nonZeroRgbPixels = 0;
	uint64 solidAlphaPixels = 0;
	if (!compressedStats && bytesPerPixel >= 4)
	{
		const size_t pixelCount = std::min<size_t>((size_t)width * (size_t)height, inspectedBytes / bytesPerPixel);
		for (size_t i = 0; i < pixelCount; i++)
		{
			const uint8* pixel = data + i * bytesPerPixel;
			nonZeroRgbPixels += (pixel[0] | pixel[1] | pixel[2]) != 0;
			solidAlphaPixels += pixel[3] == 255;
		}
	}

	const std::string statsPayload = fmt::format(" stats=ok cached=0 samplesOnly=0 bpp={} compressed={} readbackBytes={} logicalBytes={} inspected={} nonZeroBytes={} nonZeroRgbPixels={} solidAlphaPixels={} sum={} min={} max={} hash=0x{:016x} {}{}",
												 bytesPerPixel,
												 compressedStats ? 1 : 0,
												 (unsigned long long)readbackBytes,
												 (unsigned long long)logicalByteCount,
												 (unsigned long long)inspectedBytes,
												 (unsigned long long)nonZeroBytes,
												 (unsigned long long)nonZeroRgbPixels,
												 (unsigned long long)solidAlphaPixels,
												 (unsigned long long)byteSum,
												 (uint32)minByte,
												 (uint32)maxByte,
												 (unsigned long long)hash,
												 samplesPayload,
												 gridPayload);
	if (traceStatsKey != 0)
	{
		if (m_traceTextureStatsCache.size() > 16384)
			m_traceTextureStatsCache.clear();
		std::string cachedPayload = statsPayload;
		const size_t cachedFlag = cachedPayload.find("cached=0");
		if (cachedFlag != std::string::npos)
			cachedPayload.replace(cachedFlag, std::strlen("cached=0"), "cached=1");
		m_traceTextureStatsCache.emplace(traceStatsKey, std::move(cachedPayload));
	}
	D3D12TraceSink::Get().Write(eventName,
								basePayload + statsPayload);
}

void D3D12Renderer::TraceFboAttachments(std::string_view eventName, std::string_view phase, uint64 drawId, uint64 pipelineHash, std::string_view extra)
{
	if (!D3D12ShouldTrace())
		return;
	if (!m_activeFBO)
	{
		D3D12TraceSink::Get().Write(eventName, fmt::format("phase={} draw={} pipeline=0x{:016x} fbo=null {}", phase, (unsigned long long)drawId, (unsigned long long)pipelineHash, extra));
		return;
	}
	const auto& colorAttachments = m_activeFBO->GetColorAttachments();
	for (uint32 i = 0; i < colorAttachments.size(); i++)
	{
		const auto& attachment = colorAttachments[i];
		const auto slotExtra = fmt::format("draw={} pipeline=0x{:016x} fbo=0x{:016x} fboSize={}x{} attachmentKind=color attachmentIndex={} activeColorCount={} activeColorMask=0x{:02x} drawBuffersMask=0x{:08x} attachmentValid={} attachmentDxgi={} rtvCpu=0x{:016x} sparse={} {}",
										   (unsigned long long)drawId,
										   (unsigned long long)pipelineHash,
										   m_activeFBO->key,
										   m_activeFBO->m_size.x,
										   m_activeFBO->m_size.y,
										   i,
										   m_activeFBO->GetColorAttachmentCount(),
										   m_activeFBO->GetColorAttachmentMask(),
										   m_activeFBO->drawBuffersMask,
										   attachment.valid ? 1 : 0,
										   (uint32)attachment.format,
										   (unsigned long long)attachment.rtv.ptr,
										   m_activeFBO->HasSparseColorAttachments() ? 1 : 0,
										   extra);
		if (!attachment.valid || !attachment.view)
		{
			D3D12TraceSink::Get().Write(eventName, fmt::format("phase={} {} texture=null", phase, slotExtra));
			continue;
		}
		TraceTextureSnapshot(eventName, phase, attachment.view, false, slotExtra);
	}
	if (m_activeFBO->HasDepthStencilAttachment() && m_activeFBO->depthBuffer.texture)
	{
		TraceTextureSnapshot(eventName,
							 phase,
							 static_cast<LatteTextureViewD3D12*>(m_activeFBO->depthBuffer.texture),
							 false,
							 fmt::format("draw={} pipeline=0x{:016x} fbo=0x{:016x} fboSize={}x{} attachmentKind=depth attachmentIndex=depth activeColorCount={} activeColorMask=0x{:02x} drawBuffersMask=0x{:08x} depthDxgi={} stencilDxgi={} dsvCpu=0x{:016x} hasStencil={} {}",
										 (unsigned long long)drawId,
										 (unsigned long long)pipelineHash,
										 m_activeFBO->key,
										 m_activeFBO->m_size.x,
										 m_activeFBO->m_size.y,
										 m_activeFBO->GetColorAttachmentCount(),
										 m_activeFBO->GetColorAttachmentMask(),
										 m_activeFBO->drawBuffersMask,
										 (uint32)m_activeFBO->GetDepthFormat(),
										 (uint32)m_activeFBO->GetStencilFormat(),
										 (unsigned long long)m_activeFBO->GetDsvHandle().ptr,
										 m_activeFBO->depthBuffer.hasStencil ? 1 : 0,
										 extra));
	}
	else
	{
		D3D12TraceSink::Get().Write(eventName,
									fmt::format("phase={} draw={} pipeline=0x{:016x} fbo=0x{:016x} fboSize={}x{} attachmentKind=depth attachmentIndex=depth attachmentValid=0 texture=null hasDepthAttachment=0 {}",
												phase,
												(unsigned long long)drawId,
												(unsigned long long)pipelineHash,
												m_activeFBO->key,
												m_activeFBO->m_size.x,
												m_activeFBO->m_size.y,
												extra));
	}
}

void D3D12Renderer::TraceShaderTextureBindings(uint64 drawId, uint64 pipelineHash, LatteDecompilerShader* vertexShader, LatteDecompilerShader* geometryShader, LatteDecompilerShader* pixelShader)
{
	if (!D3D12ShouldTrace())
		return;

	auto traceStage = [this, drawId, pipelineHash](const char* stageName, uint32 hostTextureUnitBase, const _LatteRegisterSetTextureUnit* texRegs, LatteDecompilerShader* shader)
	{
		if (!shader)
		{
			D3D12TraceSink::Get().Write("draw_texture_binding", fmt::format("draw={} pipeline=0x{:016x} stage={} shader=null", (unsigned long long)drawId, (unsigned long long)pipelineHash, stageName));
			return;
		}

		for (uint32 relativeUnit = 0; relativeUnit < LATTE_NUM_MAX_TEX_UNITS; ++relativeUnit)
		{
			const sint8 bindingPoint = shader->resourceMapping.textureUnitToBindingPoint[relativeUnit];
			if (bindingPoint < 0)
				continue;

			uint32 gpuSamplerSwizzle = 0x06880000;
			uint32 texWord0 = 0;
			uint32 texWord1 = 0;
			uint32 texWord4 = 0;
			if (texRegs)
			{
				const auto& texReg = texRegs[relativeUnit];
				texWord0 = texReg.word0.getRawValue();
				texWord1 = texReg.word1.getRawValue();
				texWord4 = texReg.word4.getRawValue();
				gpuSamplerSwizzle = ((uint32)texReg.word4.get_DST_SEL_X() << 16) |
									((uint32)texReg.word4.get_DST_SEL_Y() << 19) |
									((uint32)texReg.word4.get_DST_SEL_Z() << 22) |
									((uint32)texReg.word4.get_DST_SEL_W() << 25);
			}

			const uint32 hostTextureUnit = hostTextureUnitBase + relativeUnit;
			auto* textureView = hostTextureUnit < m_boundTextures.size() ? m_boundTextures[hostTextureUnit] : nullptr;
			auto* texture = textureView ? textureView->GetBaseImage() : nullptr;
			D3D12DescriptorManager::Allocation samplerSrv{};
			if (textureView)
				samplerSrv = textureView->GetSamplerSRV(gpuSamplerSwizzle);
			const uint32 samplerAssignment = shader->textureUnitSamplerAssignment[relativeUnit];
			uint32 samplerWord0 = 0;
			uint32 samplerWord1 = 0;
			uint32 samplerWord2 = 0;
			uint32 samplerMinFilter = 0;
			uint32 samplerMagFilter = 0;
			uint32 samplerMipFilter = 0;
			uint32 samplerMaxAniso = 0;
			uint32 samplerMinLod = 0;
			uint32 samplerMaxLod = 0;
			sint32 samplerLodBias = 0;
			uint32 samplerClampX = 0;
			uint32 samplerClampY = 0;
			uint32 samplerClampZ = 0;
			uint32 samplerBorderType = 0;
			if (samplerAssignment != LATTE_DECOMPILER_SAMPLER_NONE)
			{
				const uint32 samplerIndex = samplerAssignment + LatteDecompiler_getTextureSamplerBaseIndex(shader->shaderType);
				const _LatteRegisterSetSampler* samplerWords = LatteGPUState.contextNew.SQ_TEX_SAMPLER + samplerIndex;
				samplerWord0 = samplerWords->WORD0.getRawValue();
				samplerWord1 = samplerWords->WORD1.getRawValue();
				samplerWord2 = samplerWords->WORD2.getRawValue();
				samplerMinFilter = (uint32)samplerWords->WORD0.get_XY_MIN_FILTER();
				samplerMagFilter = (uint32)samplerWords->WORD0.get_XY_MAG_FILTER();
				samplerMipFilter = (uint32)samplerWords->WORD0.get_MIP_FILTER();
				samplerMaxAniso = samplerWords->WORD0.get_MAX_ANISO_RATIO();
				samplerMinLod = samplerWords->WORD1.get_MIN_LOD();
				samplerMaxLod = samplerWords->WORD1.get_MAX_LOD();
				samplerLodBias = samplerWords->WORD1.get_LOD_BIAS();
				samplerClampX = (uint32)samplerWords->WORD0.get_CLAMP_X();
				samplerClampY = (uint32)samplerWords->WORD0.get_CLAMP_Y();
				samplerClampZ = (uint32)samplerWords->WORD0.get_CLAMP_Z();
				samplerBorderType = (uint32)samplerWords->WORD0.get_BORDER_COLOR_TYPE();
			}
			std::string extra = fmt::format("draw={} pipeline=0x{:016x} stage={} shader=0x{:016x} aux=0x{:016x} relativeUnit={} hostUnit={} bindingPoint={} samplerAssignment={} depthCompare={} swizzle=0x{:08x} texWord0=0x{:08x} texWord1=0x{:08x} texWord4=0x{:08x} samplerWord0=0x{:08x} samplerWord1=0x{:08x} samplerWord2=0x{:08x} samplerMinFilter={} samplerMagFilter={} samplerMipFilter={} samplerMaxAniso={} samplerMinLod={} samplerMaxLod={} samplerLodBias={} samplerClamp={},{},{} samplerBorderType={} samplerSrvValid={} samplerSrvIndex={} samplerSrvCpu=0x{:016x} samplerSrvGpu=0x{:016x}",
										   (unsigned long long)drawId,
										   (unsigned long long)pipelineHash,
										   stageName,
										   shader->baseHash,
										   shader->auxHash,
										   relativeUnit,
										   hostTextureUnit,
										   (sint32)bindingPoint,
										   samplerAssignment,
										   shader->textureUsesDepthCompare[relativeUnit] ? 1 : 0,
										   gpuSamplerSwizzle,
										   texWord0,
										   texWord1,
										   texWord4,
										   samplerWord0,
										   samplerWord1,
										   samplerWord2,
										   samplerMinFilter,
										   samplerMagFilter,
										   samplerMipFilter,
										   samplerMaxAniso,
										   samplerMinLod,
										   samplerMaxLod,
										   samplerLodBias,
										   samplerClampX,
										   samplerClampY,
										   samplerClampZ,
										   samplerBorderType,
										   samplerSrv.IsValid() ? 1 : 0,
										   samplerSrv.IsValid() ? samplerSrv.index : 0,
										   (unsigned long long)(samplerSrv.IsValid() ? samplerSrv.cpu.ptr : 0),
										   (unsigned long long)(samplerSrv.IsValid() ? samplerSrv.gpu.ptr : 0));
			if (!texture || !textureView)
			{
				D3D12TraceSink::Get().Write("draw_texture_binding", extra + " texture=null");
				continue;
			}
			TraceTextureSnapshot("draw_texture_binding", "before_draw_descriptor_table", textureView, false, extra);
		}
	};

	traceStage("VS", LATTE_CEMU_VS_TEX_UNIT_BASE, LatteGPUState.contextNew.SQ_TEX_START_VS, vertexShader);
	traceStage("PS", LATTE_CEMU_PS_TEX_UNIT_BASE, LatteGPUState.contextNew.SQ_TEX_START_PS, pixelShader);
	traceStage("GS", LATTE_CEMU_GS_TEX_UNIT_BASE, LatteGPUState.contextNew.SQ_TEX_START_GS, geometryShader);
}

void D3D12Renderer::TraceVertexFetchBindings(uint64 drawId, uint64 pipelineHash, LatteFetchShader* fetchShader)
{
	if (!D3D12ShouldTrace())
		return;
	if (!fetchShader)
	{
		D3D12TraceSink::Get().Write("draw_vertex_fetch", fmt::format("draw={} pipeline=0x{:016x} fetchShader=null", (unsigned long long)drawId, (unsigned long long)pipelineHash));
		return;
	}

	D3D12TraceEvent("draw_vertex_fetch_summary", "draw={} pipeline=0x{:016x} fetchShader={} key=0x{:016x} cacheHash=0x{:016x} groups={} invalidGroups={} mainResource={} mainState=0x{:08x} shadowBytes={}",
					(unsigned long long)drawId,
					(unsigned long long)pipelineHash,
					(const void*)fetchShader,
					fetchShader->key,
					fetchShader->m_cacheHash,
					fetchShader->bufferGroups.size(),
					fetchShader->bufferGroupsInvalid.size(),
					(const void*)m_bufferCache.resource.Get(),
					(uint32)m_bufferCache.state,
					(unsigned long long)m_bufferCacheShadow.size());

	for (const auto& group : fetchShader->bufferGroups)
	{
		const uint32 stride = group.getCurrentBufferStride(LatteGPUState.contextRegister);
		const auto binding = group.attributeBufferIndex < m_vertexBindings.size() ? m_vertexBindings[group.attributeBufferIndex] : BoundRange{};
		D3D12TraceEvent("draw_vertex_buffer_group", "draw={} pipeline=0x{:016x} fetchShader={} bufferIndex={} validBinding={} bindOffset={} bindSize={} stride={} minOffset={} maxOffset={} attrCount={} vboStride={} vtxIndexAccess={} instanceIndexAccess={} mainResource={}",
						(unsigned long long)drawId,
						(unsigned long long)pipelineHash,
						(const void*)fetchShader,
						group.attributeBufferIndex,
						binding.valid ? 1 : 0,
						binding.offset,
						binding.size,
						stride,
						group.minOffset,
						group.totalAttribRangeSize,
						group.attribCount,
						group.getCurrentBufferStride(LatteGPUState.contextRegister),
						(group.hasVtxIndexAccess ? 1 : 0),
						group.hasInstanceIndexAccess ? 1 : 0,
						(const void*)m_bufferCache.resource.Get());
		for (sint32 attrIndex = 0; attrIndex < group.attribCount; ++attrIndex)
		{
			const auto& attr = group.attrib[attrIndex];
			D3D12TraceEvent("draw_vertex_attribute", "draw={} pipeline=0x{:016x} fetchShader={} bufferIndex={} attrIndex={} semantic={} fmt=0x{:02x} fetchType={} nfa={} signed={} endian={} divisor={} offset={} ds={},{},{},{} bindOffset={} bindSize={} stride={} byteRange={}..{}",
							(unsigned long long)drawId,
							(unsigned long long)pipelineHash,
							(const void*)fetchShader,
							group.attributeBufferIndex,
							attrIndex,
							attr.semanticId,
							(uint32)attr.format,
							D3D12FetchTypeName(attr.fetchType),
							attr.nfa,
							attr.isSigned,
							D3D12EndianName(attr.endianSwap),
							attr.aluDivisor,
							attr.offset,
							attr.ds[0],
							attr.ds[1],
							attr.ds[2],
							attr.ds[3],
							binding.offset,
							binding.size,
							stride,
							binding.offset + attr.offset,
							binding.offset + group.totalAttribRangeSize);
		}
	}

	for (const auto& group : fetchShader->bufferGroupsInvalid)
	{
		D3D12TraceEvent("draw_vertex_buffer_group_invalid", "draw={} pipeline=0x{:016x} fetchShader={} bufferIndex={} attrCount={} minOffset={} maxOffset={} vboStride={} vtxIndexAccess={} instanceIndexAccess={}",
						(unsigned long long)drawId,
						(unsigned long long)pipelineHash,
						(const void*)fetchShader,
						group.attributeBufferIndex,
						group.attribCount,
						group.minOffset,
						group.totalAttribRangeSize,
						0u, // invalid buffer index: no stride register
						(group.hasVtxIndexAccess ? 1 : 0),
						group.hasInstanceIndexAccess ? 1 : 0);
	}
}

std::string D3D12Renderer::BuildShaderTextureSourceSummary(LatteDecompilerShader* vertexShader, LatteDecompilerShader* geometryShader, LatteDecompilerShader* pixelShader) const
{
	std::string summary;
	auto appendStage = [this, &summary](const char* stageName, uint32 hostTextureUnitBase, LatteDecompilerShader* shader)
	{
		if (!shader)
			return;
		for (uint32 relativeUnit = 0; relativeUnit < LATTE_NUM_MAX_TEX_UNITS; ++relativeUnit)
		{
			const sint8 bindingPoint = shader->resourceMapping.textureUnitToBindingPoint[relativeUnit];
			if (bindingPoint < 0)
				continue;
			const uint32 hostTextureUnit = hostTextureUnitBase + relativeUnit;
			auto* textureView = hostTextureUnit < m_boundTextures.size() ? m_boundTextures[hostTextureUnit] : nullptr;
			auto* texture = textureView ? textureView->GetBaseImage() : nullptr;
			if (!texture)
				continue;
			if (!summary.empty())
				summary += ",";
			summary += fmt::format("{}{}=0x{:08x}/fmt=0x{:04x}/{}x{}/bp{}",
								   stageName,
								   relativeUnit,
								   texture->physAddress,
								   (uint32)texture->format,
								   texture->width,
								   texture->height,
								   (sint32)bindingPoint);
		}
	};

	appendStage("VS", LATTE_CEMU_VS_TEX_UNIT_BASE, vertexShader);
	appendStage("PS", LATTE_CEMU_PS_TEX_UNIT_BASE, pixelShader);
	appendStage("GS", LATTE_CEMU_GS_TEX_UNIT_BASE, geometryShader);
	if (summary.empty())
		return "none";
	return summary;
}

std::string D3D12Renderer::BuildGeometryProbeResourceMetadata(LatteDecompilerShader* vertexShader, LatteDecompilerShader* geometryShader, LatteDecompilerShader* pixelShader) const
{
	// must not prepare textures or create descriptors; only inspect existing state
	if (!m_activeFBO)
		return "probeResources=no_fbo";
	uint32 requestedMask = 0;
	for (uint32 i = 0; i < 8; ++i)
		if (m_activeFBO->colorBuffer[i].texture)
			requestedMask |= 1u << i;
	const uint32 targetMask = GetD3D12EffectiveRenderTargetMask(LatteGPUState.contextNew);
	const uint32 outputMask = pixelShader ? pixelShader->pixelColorOutputMask : 0;
	const auto* depthView = static_cast<const LatteTextureViewD3D12*>(m_activeFBO->depthBuffer.texture);
	const auto* depthTexture = depthView ? depthView->GetBaseImage() : nullptr;
	std::string metadata = fmt::format(
		"title=0x{:016x} fbo={}x{} requestedRT=0x{:02x} validRT=0x{:02x} rtvCount={} dsvFormat={} stencilFormat={} "
		"depthRequested={} depthValid={} depthResource={} depthView=0x{:04x} rtSlots=[",
		GetUwpTitleId(), m_activeFBO->m_size.x, m_activeFBO->m_size.y, requestedMask,
		m_activeFBO->GetColorAttachmentMask(), m_activeFBO->GetColorAttachmentCount(),
		(uint32)m_activeFBO->GetDepthFormat(), (uint32)m_activeFBO->GetStencilFormat(),
		depthView != nullptr, m_activeFBO->HasDepthStencilAttachment(),
		depthTexture ? (uint32)depthTexture->GetDesc().Format : 0, depthView ? (uint32)depthView->format : 0);
	for (uint32 i = 0; i < 8; ++i)
	{
		if ((requestedMask & (1u << i)) == 0)
			continue;
		const auto* view = static_cast<const LatteTextureViewD3D12*>(m_activeFBO->colorBuffer[i].texture);
		const auto* texture = view->GetBaseImage();
		const auto& attachment = m_activeFBO->GetColorAttachments()[i];
		const auto desc = texture ? texture->GetDesc() : D3D12_RESOURCE_DESC{};
		const uint32 writeMask = attachment.valid && (outputMask & (1u << i)) != 0 ? (targetMask >> (i * 4)) & 15u : 0u;
		metadata += fmt::format(
			"{}:addr=0x{:08x}/valid={}/resource={}/rtv={}/view=0x{:04x}/write=0x{:x}/mip={}/slice={}/size={}x{}x{};",
			i, texture ? texture->physAddress : 0, attachment.valid, (uint32)desc.Format,
			(uint32)attachment.format, (uint32)view->format, writeMask, view->firstMip, view->firstSlice,
			desc.Width, desc.Height, desc.DepthOrArraySize);
	}
	metadata += "] cachedSources=[";
	uint32 sourceCount = 0;
	auto appendStage = [&](const char* stage, uint32 hostBase, const _LatteRegisterSetTextureUnit* texRegs, const LatteDecompilerShader* shader)
	{
		if (!shader)
			return;
		for (uint32 unit = 0; unit < LATTE_NUM_MAX_TEX_UNITS; ++unit)
		{
			const sint8 bindingPoint = shader->resourceMapping.textureUnitToBindingPoint[unit];
			if (bindingPoint < 0)
				continue;
			++sourceCount;
			const uint32 hostUnit = hostBase + unit;
			const auto* view = hostUnit < m_boundTextures.size() ? m_boundTextures[hostUnit] : nullptr;
			const auto* texture = view ? view->GetBaseImage() : nullptr;
			const auto* scratch = GetFeedbackScratchCopy(hostUnit);
			const auto& word4 = texRegs[unit].word4;
			const uint32 swizzle = ((uint32)word4.get_DST_SEL_X() << 16) | ((uint32)word4.get_DST_SEL_Y() << 19) |
				((uint32)word4.get_DST_SEL_Z() << 22) | ((uint32)word4.get_DST_SEL_W() << 25);
			if (!view || !texture)
			{
				metadata += fmt::format("{}{}:bp={}/unbound=1/scratch={};", stage, unit, (sint32)bindingPoint, scratch != nullptr);
				continue;
			}
			const auto cached = view->m_samplerSrvs.find(swizzle & 0x0FFF0000);
			const bool cachedValid = cached != view->m_samplerSrvs.end() && cached->second.IsValid();
			const auto desc = texture->GetDesc();
			const auto samplingDesc = view->UsesSamplingProxy() ? view->m_samplingProxy.resource->GetDesc() : desc;
			const auto srvFormat = texture->isDepth ? view->m_viewFormatInfo.depthSrvFormat : view->m_viewFormatInfo.srvFormat;
			metadata += fmt::format(
				"{}{}:bp={}/addr=0x{:08x}/base=0x{:04x}/view=0x{:04x}/res={}/srv={}/dim={},{}/mip={},{}"
				"/slice={},{} /proxy={}/sampleRes={},{} /swz=0x{:08x}/baseSrv={}/swzCached={}/swzValid={}/scratch={}",
				stage, unit, (sint32)bindingPoint, texture->physAddress, (uint32)texture->format, (uint32)view->format,
				(uint32)desc.Format, (uint32)srvFormat, (uint32)desc.Dimension, (uint32)view->dim, view->firstMip, view->numMip,
				view->firstSlice, view->numSlice, view->UsesSamplingProxy(), (uint32)samplingDesc.Format,
				(uint32)samplingDesc.Dimension, swizzle, view->m_srv.IsValid(), cached != view->m_samplerSrvs.end(), cachedValid,
				scratch != nullptr);
			if (scratch)
				metadata += fmt::format("/scratchSrv={},{}", (uint32)scratch->srvDesc.Format, (uint32)scratch->srvDesc.ViewDimension);
			metadata += ";";
		}
	};
	appendStage("VS", LATTE_CEMU_VS_TEX_UNIT_BASE, LatteGPUState.contextNew.SQ_TEX_START_VS, vertexShader);
	appendStage("PS", LATTE_CEMU_PS_TEX_UNIT_BASE, LatteGPUState.contextNew.SQ_TEX_START_PS, pixelShader);
	appendStage("GS", LATTE_CEMU_GS_TEX_UNIT_BASE, LatteGPUState.contextNew.SQ_TEX_START_GS, geometryShader);
	metadata += fmt::format("] sourceCount={} (cached descriptor availability only; not GPU table readback)", sourceCount);
	return metadata;
}

std::string D3D12Renderer::BuildTextureLineageExtra(uint32 physAddress) const
{
	const auto it = m_textureLineage.find(physAddress);
	if (it == m_textureLineage.end())
		return "lineage=unknown";
	const auto& lineage = it->second;
	return fmt::format("lineageSerial={} lineageOp={} lineageDraw={} lineagePipeline=0x{:016x} lineageFbo=0x{:016x} lineageSources={} lineageExtra={}",
					   (unsigned long long)lineage.serial,
					   lineage.operation,
					   (unsigned long long)lineage.drawId,
					   (unsigned long long)lineage.pipelineHash,
					   (unsigned long long)lineage.fboKey,
					   lineage.sourceSummary,
					   lineage.extra);
}

void D3D12Renderer::RecordTextureLineage(std::string_view operation, LatteTextureViewD3D12* textureView, uint64 drawId, uint64 pipelineHash, std::string_view sourceSummary, std::string_view extra)
{
	if (!D3D12ShouldTrace())
		return;
	if (!textureView)
		return;
	auto* texture = textureView->GetBaseImage();
	if (!texture)
		return;

	TextureLineage lineage{};
	lineage.serial = ++m_textureLineageSerial;
	lineage.drawId = drawId;
	lineage.pipelineHash = pipelineHash;
	lineage.fboKey = m_activeFBO ? m_activeFBO->key : 0;
	lineage.operation = std::string(operation);
	lineage.sourceSummary = sourceSummary.empty() ? "none" : std::string(sourceSummary);
	lineage.extra = std::string(extra);
	m_textureLineage[texture->physAddress] = lineage;

	D3D12TraceEvent("texture_lineage_update", "serial={} op={} addr=0x{:08x} fmt=0x{:04x} size={}x{} draw={} pipeline=0x{:016x} fbo=0x{:016x} sources={} extra={}",
					(unsigned long long)lineage.serial,
					lineage.operation,
					texture->physAddress,
					(uint32)texture->format,
					texture->width,
					texture->height,
					(unsigned long long)lineage.drawId,
					(unsigned long long)lineage.pipelineHash,
					(unsigned long long)lineage.fboKey,
					lineage.sourceSummary,
					lineage.extra);
}

void D3D12Renderer::LogVertexBufferPreview(uint64 drawId, uint64 pipelineHash, LatteDecompilerShader* vertexShader, LatteDecompilerShader* pixelShader, Latte::LATTE_VGT_PRIMITIVE_TYPE::E_PRIMITIVE_TYPE primitiveMode, uint32 count, uint32 baseVertex, uint32 baseInstance, uint32 indexMin, uint32 indexMax, Renderer::INDEX_TYPE hostIndexType, uint32 hostIndexCount)
{
	if (!D3D12ShouldTrace() && !D3D12ShouldChecksumOutput())
		return;

	using Primitive = Latte::LATTE_VGT_PRIMITIVE_TYPE::E_PRIMITIVE_TYPE;
	const bool isRelevantCopyOrQuad =
		primitiveMode == Primitive::QUADS ||
		primitiveMode == Primitive::RECTS ||
		(vertexShader && (vertexShader->baseHash == 0xc99c40893a7ac7adull ||
						  vertexShader->baseHash == 0x0ea6cb7fa4004293ull ||
						  vertexShader->baseHash == 0xe2b099d070ad930dull));
	const auto& traceGpuFilter = D3D12TraceGpuFilter();
	const bool focusedDrawMatch =
		D3D12GetTraceGpuStatsMode() == D3D12TraceGpuStatsMode::Focused &&
		(traceGpuFilter.hasDrawMin || traceGpuFilter.hasDrawMax) &&
		(!traceGpuFilter.hasDrawMin || drawId >= traceGpuFilter.drawMin) &&
		(!traceGpuFilter.hasDrawMax || drawId <= traceGpuFilter.drawMax);
	const bool focusedPipelineMatch =
		D3D12GetTraceGpuStatsMode() == D3D12TraceGpuStatsMode::Focused &&
		pipelineHash != 0 &&
		traceGpuFilter.hasPipeline &&
		D3D12TraceGpuPipelineMatches(pipelineHash);
	const bool knownAlphaMaskPass =
		vertexShader && pixelShader &&
		vertexShader->baseHash == 0xb56e79aae93e86a8ull &&
		pixelShader->baseHash == 0x7e1f4fbecda6957full;
	if (!isRelevantCopyOrQuad && !focusedDrawMatch && !focusedPipelineMatch && !knownAlphaMaskPass)
		return;

	auto* fetchShader = LatteSHRC_GetActiveFetchShader();
	if (!fetchShader)
		return;

	if (m_bufferCacheShadow.empty())
	{
		D3D12TraceEvent("draw_vertex_preview_unavailable", "draw={} pipeline=0x{:016x} reason=buffer_cache_shadow_empty primitive={} count={} baseVertex={} baseInstance={} indexMin={} indexMax={} hostIndexType={} hostIndexCount={} vs=0x{:016x} ps=0x{:016x} mainResource={} mainSize={} mainState=0x{:08x}",
						(unsigned long long)drawId,
						(unsigned long long)pipelineHash,
						(uint32)primitiveMode,
						count,
						baseVertex,
						baseInstance,
						indexMin,
						indexMax,
						(uint32)hostIndexType,
						hostIndexCount,
						vertexShader ? vertexShader->baseHash : 0,
						pixelShader ? pixelShader->baseHash : 0,
						(const void*)m_bufferCache.resource.Get(),
						(unsigned long long)m_bufferCache.size,
						(uint32)m_bufferCache.state);
		return;
	}

	static uint32 s_vertexPreviewLogCount = 0;
	const bool writeRuntimeLog = D3D12RuntimeLogCapped(s_vertexPreviewLogCount, "vertex buffer preview", 96);
	if (writeRuntimeLog)
	{
		cemuLog_log(LogType::Force,
					"D3D12: vertex preview draw={} primitive={} count={} baseVertex={} baseInstance={} indexMin={} indexMax={} hostIndexType={} hostIndexCount={} vs={:016x} ps={:016x} shadowBytes={}.",
					(unsigned long long)drawId,
					(uint32)primitiveMode,
					count,
					baseVertex,
					baseInstance,
					indexMin,
					indexMax,
					(uint32)hostIndexType,
					hostIndexCount,
					vertexShader ? vertexShader->baseHash : 0,
					pixelShader ? pixelShader->baseHash : 0,
					(unsigned long long)m_bufferCacheShadow.size());
	}

	for (auto& group : fetchShader->bufferGroups)
	{
		const uint32 stride = group.getCurrentBufferStride(LatteGPUState.contextRegister);
		const auto binding = group.attributeBufferIndex < m_vertexBindings.size() ? m_vertexBindings[group.attributeBufferIndex] : BoundRange{};
		if (!binding.valid || binding.size == 0)
		{
			D3D12TraceEvent("draw_vertex_preview_group", "draw={} pipeline=0x{:016x} primitive={} bufferIndex={} validBinding={} bindOffset={} bindSize={} stride={} attrCount={} skipped=1 reason=missing_binding",
							(unsigned long long)drawId,
							(unsigned long long)pipelineHash,
							(uint32)primitiveMode,
							group.attributeBufferIndex,
							binding.valid ? 1 : 0,
							binding.offset,
							binding.size,
							stride,
							group.attribCount);
			continue;
		}

		D3D12TraceEvent("draw_vertex_preview_group", "draw={} pipeline=0x{:016x} primitive={} count={} baseVertex={} baseInstance={} indexMin={} indexMax={} hostIndexType={} hostIndexCount={} bufferIndex={} bindOffset={} bindSize={} bindEnd={} stride={} lowBits={} groupMinOffset={} groupMaxOffset={} attrCount={} vtxIndexAccess={} instanceIndexAccess={} vs=0x{:016x} ps=0x{:016x}",
						(unsigned long long)drawId,
						(unsigned long long)pipelineHash,
						(uint32)primitiveMode,
						count,
						baseVertex,
						baseInstance,
						indexMin,
						indexMax,
						(uint32)hostIndexType,
						hostIndexCount,
						group.attributeBufferIndex,
						binding.offset,
						binding.size,
						binding.offset + binding.size,
						stride,
						binding.offset & 3,
						group.minOffset,
						group.totalAttribRangeSize,
						group.attribCount,
						(group.hasVtxIndexAccess ? 1 : 0),
						group.hasInstanceIndexAccess ? 1 : 0,
						vertexShader ? vertexShader->baseHash : 0,
						pixelShader ? pixelShader->baseHash : 0);

		for (sint32 attrIndex = 0; attrIndex < group.attribCount; ++attrIndex)
		{
			const auto& attr = group.attrib[attrIndex];
			const uint32 vertexCount = std::min<uint32>(count, 8);
			for (uint32 vertex = 0; vertex < vertexCount; ++vertex)
			{
				uint32 fetchIndex = vertex;
				if (attr.fetchType == LatteConst::VertexFetchType2::INSTANCE_DATA)
				{
					if (attr.aluDivisor > 1)
						fetchIndex = (baseInstance + vertex) / (uint32)attr.aluDivisor;
					else
						fetchIndex = baseInstance + vertex;
				}
				else if (attr.fetchType == LatteConst::VertexFetchType2::NO_INDEX_OFFSET_DATA)
				{
					fetchIndex = 0;
				}
				const uint32 byteOffset = binding.offset + fetchIndex * stride + attr.offset;
				const bool inBounds = (uint64)byteOffset + 16ull <= (uint64)m_bufferCacheShadow.size();
				const bool inBinding = byteOffset >= binding.offset && (uint64)byteOffset + 16ull <= (uint64)binding.offset + binding.size;
				const uint32 raw0 = ReadD3D12ShadowU32(m_bufferCacheShadow, byteOffset + 0, attr.endianSwap);
				const uint32 raw1 = ReadD3D12ShadowU32(m_bufferCacheShadow, byteOffset + 4, attr.endianSwap);
				const uint32 raw2 = ReadD3D12ShadowU32(m_bufferCacheShadow, byteOffset + 8, attr.endianSwap);
				const uint32 raw3 = ReadD3D12ShadowU32(m_bufferCacheShadow, byteOffset + 12, attr.endianSwap);
				D3D12TraceEvent("draw_vertex_preview", "draw={} pipeline=0x{:016x} primitive={} bufferIndex={} attrIndex={} semantic={} vertexSlot={} fetchType={} fetchIndex={} byteOffset={} bindOffset={} bindSize={} inBinding={} inShadow={} stride={} fmt=0x{:02x} nfa={} signed={} endian={} ds={},{},{},{} raw=0x{:08x},0x{:08x},0x{:08x},0x{:08x} float={:.9g},{:.9g},{:.9g},{:.9g}",
								(unsigned long long)drawId,
								(unsigned long long)pipelineHash,
								(uint32)primitiveMode,
								group.attributeBufferIndex,
								attrIndex,
								attr.semanticId,
								vertex,
								D3D12FetchTypeName(attr.fetchType),
								fetchIndex,
								byteOffset,
								binding.offset,
								binding.size,
								inBinding ? 1 : 0,
								inBounds ? 1 : 0,
								stride,
								(uint32)attr.format,
								attr.nfa,
								attr.isSigned,
								D3D12EndianName(attr.endianSwap),
								attr.ds[0],
								attr.ds[1],
								attr.ds[2],
								attr.ds[3],
								raw0,
								raw1,
								raw2,
								raw3,
								std::bit_cast<float>(raw0),
								std::bit_cast<float>(raw1),
								std::bit_cast<float>(raw2),
								std::bit_cast<float>(raw3));
				if (writeRuntimeLog)
				{
					cemuLog_log(LogType::Force,
								"D3D12:   draw={} vb{} attr{} sem={} v{} fetchIndex={} byte={} inBinding={} stride={} fmt=0x{:02x} endian={} raw=0x{:08x},0x{:08x},0x{:08x},0x{:08x} float={:.7g},{:.7g},{:.7g},{:.7g}.",
								(unsigned long long)drawId,
								group.attributeBufferIndex,
								attrIndex,
								attr.semanticId,
								vertex,
								fetchIndex,
								byteOffset,
								inBinding ? 1 : 0,
								stride,
								(uint32)attr.format,
								D3D12EndianName(attr.endianSwap),
								raw0,
								raw1,
								raw2,
								raw3,
								std::bit_cast<float>(raw0),
								std::bit_cast<float>(raw1),
								std::bit_cast<float>(raw2),
								std::bit_cast<float>(raw3));
				}
			}
		}
	}
}

bool D3D12Renderer::IsFenceComplete(uint64 fenceValue) const
{
	if (fenceValue == 0 || !m_fence)
		return true;
	return m_fence->GetCompletedValue() >= fenceValue;
}

void D3D12Renderer::EnsureOcclusionQueryResources()
{
	if (m_occlusionQueryPages.empty())
		CreateOcclusionQueryPage();
}

uint32 D3D12Renderer::CreateOcclusionQueryPage()
{
	OcclusionQueryPage page;

	D3D12_QUERY_HEAP_DESC heapDesc{};
	heapDesc.Type = D3D12_QUERY_HEAP_TYPE_OCCLUSION;
	heapDesc.Count = kOcclusionQuerySlots;
	if (FAILED(m_device->CreateQueryHeap(&heapDesc, IID_PPV_ARGS(&page.heap))))
		throw std::runtime_error("Failed to create D3D12 occlusion query heap");

	if (!m_memoryManager->CreateBuffer(sizeof(uint64) * kOcclusionQuerySlots, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, page.readback))
		throw std::runtime_error("Failed to create D3D12 occlusion query readback buffer");

	m_occlusionQueryPages.emplace_back(std::move(page));
	return (uint32)m_occlusionQueryPages.size() - 1;
}

D3D12Renderer::OcclusionQuerySlot D3D12Renderer::AcquireOcclusionQuerySlot()
{
	EnsureOcclusionQueryResources();
	ProcessFinishedCommandLists();
	for (auto* query : m_activeOcclusionQueries)
	{
		if (query)
			query->handleFinishedFragments();
	}

	for (uint32 pageIndex = 0; pageIndex < m_occlusionQueryPages.size(); pageIndex++)
	{
		auto& page = m_occlusionQueryPages[pageIndex];
		const auto availableSlot = std::find(page.slotsUsed.begin(), page.slotsUsed.end(), false);
		if (availableSlot == page.slotsUsed.end())
			continue;

		*availableSlot = true;
		return { pageIndex, (uint32)std::distance(page.slotsUsed.begin(), availableSlot) };
	}

	// submitting here would recursively restart every active query; grow the heap instead
	const uint32 pageIndex = CreateOcclusionQueryPage();
	m_occlusionQueryPages[pageIndex].slotsUsed[0] = true;
	return { pageIndex, 0 };
}

void D3D12Renderer::ReleaseOcclusionQuerySlot(OcclusionQuerySlot slot)
{
	if (!slot.IsValid() || slot.pageIndex >= m_occlusionQueryPages.size() || slot.queryIndex >= kOcclusionQuerySlots)
		return;
	m_occlusionQueryPages[slot.pageIndex].slotsUsed[slot.queryIndex] = false;
}

void D3D12Renderer::RetireOcclusionQuerySlot(OcclusionQuerySlot slot, uint64 fenceValue)
{
	if (!slot.IsValid() || slot.pageIndex >= m_occlusionQueryPages.size() || slot.queryIndex >= kOcclusionQuerySlots)
		return;
	if (fenceValue == 0 || IsFenceComplete(fenceValue))
	{
		ReleaseOcclusionQuerySlot(slot);
		return;
	}
	m_retiredOcclusionQuerySlots.push_back({ slot, fenceValue });
}

void D3D12Renderer::ProcessRetiredOcclusionQuerySlots()
{
	if (!m_fence)
		return;
	const uint64 completedFence = m_fence->GetCompletedValue();
	for (auto it = m_retiredOcclusionQuerySlots.begin(); it != m_retiredOcclusionQuerySlots.end();)
	{
		if (it->readyFence > completedFence)
		{
			++it;
			continue;
		}
		ReleaseOcclusionQuerySlot(it->slot);
		it = m_retiredOcclusionQuerySlots.erase(it);
	}
}

void D3D12Renderer::ResolveOcclusionQuery(OcclusionQuerySlot slot)
{
	if (!slot.IsValid() || slot.pageIndex >= m_occlusionQueryPages.size() || slot.queryIndex >= kOcclusionQuerySlots)
		return;
	auto& page = m_occlusionQueryPages[slot.pageIndex];
	if (!page.heap || !page.readback.resource)
		return;
	if (!m_commandListOpen)
		ResetCommandList();
	GetCurrentCommandSlot().commandList->ResolveQueryData(page.heap.Get(), D3D12_QUERY_TYPE_OCCLUSION, slot.queryIndex, 1, page.readback.resource.Get(), sizeof(uint64) * slot.queryIndex);
}

void D3D12Renderer::OcclusionQueryNotifyEndCommandList()
{
	for (auto* query : m_activeOcclusionQueries)
	{
		if (query && query->m_hasActiveQuery)
			query->endFragment();
	}
}

void D3D12Renderer::OcclusionQueryNotifyBeginCommandList()
{
	for (auto* query : m_activeOcclusionQueries)
	{
		if (query && query->m_hasActiveQuery && !query->m_hasActiveFragment)
			query->beginFragment();
	}
}

D3D12Renderer::CommandSlot& D3D12Renderer::GetCurrentCommandSlot()
{
	return m_commandSlots[m_commandSlotIndex];
}

const D3D12Renderer::CommandSlot& D3D12Renderer::GetCurrentCommandSlot() const
{
	return m_commandSlots[m_commandSlotIndex];
}

void D3D12Renderer::CheckDeviceRemoved(HRESULT hr, std::string_view operation) const
{
	if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET ||
		hr == DXGI_ERROR_DEVICE_HUNG || hr == DXGI_ERROR_DRIVER_INTERNAL_ERROR)
		LogDeviceRemovedData(hr, operation);
}

void D3D12Renderer::LogInfoQueueMessages(std::string_view operation, uint32 maxMessages) const
{
	if (!m_infoQueueEnabled)
		return;
	Microsoft::WRL::ComPtr<ID3D12InfoQueue> infoQueue;
	if (!m_device || FAILED(m_device.As(&infoQueue)))
		return;

	const uint64 messageCount = infoQueue->GetNumStoredMessagesAllowedByRetrievalFilter();
	if (messageCount == 0)
		return;
	const uint64 firstMessage = messageCount > maxMessages ? messageCount - maxMessages : 0;
	cemuLog_log(LogType::Force, "D3D12 info queue after {}: stored messages={}", operation, (unsigned long long)messageCount);
	for (uint64 i = firstMessage; i < messageCount; ++i)
	{
		SIZE_T messageLength = 0;
		if (FAILED(infoQueue->GetMessage(i, nullptr, &messageLength)) || messageLength == 0)
			continue;
		std::vector<char> messageStorage(messageLength);
		auto* message = reinterpret_cast<D3D12_MESSAGE*>(messageStorage.data());
		if (FAILED(infoQueue->GetMessage(i, message, &messageLength)))
			continue;
		cemuLog_log(LogType::Force, "D3D12 info [{}] id={} severity={}: {}",
					(unsigned long long)i,
					(uint32)message->ID,
					D3D12MessageSeverityName(message->Severity),
					message->pDescription ? message->pDescription : "");
	}
	infoQueue->ClearStoredMessages();
}

bool D3D12Renderer::ShouldCheckDevice(bool frequent) const
{
	return m_device && !m_deviceRemovalReported.load(std::memory_order_relaxed) && (!frequent || m_gpuWatchdog.IsStalled());
}

void D3D12Renderer::ReportDeviceRemovalAfter(const char* call, const char* purpose, const std::string& arguments)
{
	if (m_deviceRemovalReported.exchange(true))
		return;
	cemuLog_log(LogType::Force, "D3D12: device removed (reason 0x{:08x}), first noticed after {} ({}): {}",
		(uint32)m_device->GetDeviceRemovedReason(), call, purpose, arguments);
}

void D3D12Renderer::CreateShaderResourceViewChecked(ID3D12Resource* resource, const D3D12_SHADER_RESOURCE_VIEW_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE destination, const char* purpose, bool frequent)
{
	m_device->CreateShaderResourceView(resource, desc, destination);
	if (ShouldCheckDevice(frequent) && FAILED(m_device->GetDeviceRemovedReason()))
		ReportDeviceRemovalAfter("CreateShaderResourceView", purpose, D3D12DescribeResource(resource) + " " + D3D12DescribeView(desc));
}

void D3D12Renderer::CreateRenderTargetViewChecked(ID3D12Resource* resource, const D3D12_RENDER_TARGET_VIEW_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE destination, const char* purpose)
{
	m_device->CreateRenderTargetView(resource, desc, destination);
	if (ShouldCheckDevice(false) && FAILED(m_device->GetDeviceRemovedReason()))
		ReportDeviceRemovalAfter("CreateRenderTargetView", purpose, D3D12DescribeResource(resource) + " " + D3D12DescribeView(desc));
}

void D3D12Renderer::CreateDepthStencilViewChecked(ID3D12Resource* resource, const D3D12_DEPTH_STENCIL_VIEW_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE destination, const char* purpose)
{
	m_device->CreateDepthStencilView(resource, desc, destination);
	if (ShouldCheckDevice(false) && FAILED(m_device->GetDeviceRemovedReason()))
		ReportDeviceRemovalAfter("CreateDepthStencilView", purpose, D3D12DescribeResource(resource) + " " + D3D12DescribeView(desc));
}

void D3D12Renderer::CreateUnorderedAccessViewChecked(ID3D12Resource* resource, const D3D12_UNORDERED_ACCESS_VIEW_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE destination, const char* purpose)
{
	m_device->CreateUnorderedAccessView(resource, nullptr, desc, destination);
	if (ShouldCheckDevice(true) && FAILED(m_device->GetDeviceRemovedReason()))
		ReportDeviceRemovalAfter("CreateUnorderedAccessView", purpose, D3D12DescribeResource(resource) + " " + D3D12DescribeView(desc));
}

void D3D12Renderer::CreateSamplerChecked(const D3D12_SAMPLER_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE destination, const char* purpose)
{
	m_device->CreateSampler(desc, destination);
	if (ShouldCheckDevice(true) && FAILED(m_device->GetDeviceRemovedReason()))
		ReportDeviceRemovalAfter("CreateSampler", purpose, D3D12DescribeSampler(desc));
}

void D3D12Renderer::CopyDescriptorsSimpleChecked(UINT count, D3D12_CPU_DESCRIPTOR_HANDLE destination, D3D12_CPU_DESCRIPTOR_HANDLE source, D3D12_DESCRIPTOR_HEAP_TYPE type, const char* purpose)
{
	m_device->CopyDescriptorsSimple(count, destination, source, type);
	if (ShouldCheckDevice(true) && FAILED(m_device->GetDeviceRemovedReason()))
	{
		ReportDeviceRemovalAfter("CopyDescriptorsSimple", purpose,
			fmt::format("count={} type={} destination=0x{:x} source=0x{:x}", count, (uint32)type, (uint64)destination.ptr, (uint64)source.ptr));
	}
}

D3D12DescriptorManager::Allocation D3D12Renderer::CopyToShaderVisibleChecked(const D3D12DescriptorManager::Allocation& source, const char* purpose)
{
	D3D12DescriptorManager::Allocation destination = m_descriptorManager->CopyToShaderVisible(m_device.Get(), source);
	if (ShouldCheckDevice(true) && FAILED(m_device->GetDeviceRemovedReason()))
	{
		ReportDeviceRemovalAfter("CopyDescriptorsSimple", purpose,
			fmt::format("count={} type={} source=0x{:x} shaderVisibleSource={}", source.count, (uint32)source.type, (uint64)source.cpu.ptr, source.shaderVisible));
	}
	return destination;
}

void D3D12Renderer::LogDeviceRemovedData(HRESULT hr, std::string_view operation) const
{
	HRESULT removedReason = S_OK;
	if (m_device)
		removedReason = m_device->GetDeviceRemovedReason();

	cemuLog_log(LogType::Force, "D3D12 {} failed with 0x{:08x}; device removed reason 0x{:08x}",
				operation, (uint32)hr, (uint32)removedReason);

	LogInfoQueueMessages(operation, 32);

#ifdef __ID3D12DeviceRemovedExtendedData_INTERFACE_DEFINED__
	Microsoft::WRL::ComPtr<ID3D12DeviceRemovedExtendedData> dred;
	if (!m_device || FAILED(m_device.As(&dred)))
		return;

	D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT breadcrumbs{};
	if (SUCCEEDED(dred->GetAutoBreadcrumbsOutput(&breadcrumbs)))
	{
		uint32 nodeCount = 0;
		for (auto* node = breadcrumbs.pHeadAutoBreadcrumbNode; node; node = node->pNext)
		{
			nodeCount++;
			if (nodeCount <= 16)
			{
				const uint32 lastValue = node->pLastBreadcrumbValue ? *node->pLastBreadcrumbValue : 0;
				const char* commandListName = node->pCommandListDebugNameA ? node->pCommandListDebugNameA : "<unnamed>";
				const char* commandQueueName = node->pCommandQueueDebugNameA ? node->pCommandQueueDebugNameA : "<unnamed>";
				const uint32 opIndex = lastValue > 0 ? std::min<uint32>(lastValue - 1, node->BreadcrumbCount - 1) : 0;
				const uint32 op = node->pCommandHistory && node->BreadcrumbCount != 0 ? (uint32)node->pCommandHistory[opIndex] : 0;
				cemuLog_log(LogType::Force, "D3D12 DRED breadcrumb node {} queue={} list={} completed={}/{} lastOp={}",
							nodeCount,
							commandQueueName,
							commandListName,
							lastValue,
							node->BreadcrumbCount,
							op);
			}
		}
		cemuLog_log(LogType::Force, "D3D12 DRED auto breadcrumb nodes: {}", nodeCount);
	}

	D3D12_DRED_PAGE_FAULT_OUTPUT pageFault{};
	if (SUCCEEDED(dred->GetPageFaultAllocationOutput(&pageFault)) && pageFault.PageFaultVA != 0)
	{
		cemuLog_log(LogType::Force, "D3D12 DRED page fault VA: 0x{:016x}",
					(unsigned long long)pageFault.PageFaultVA);
	}
#endif
}

void D3D12Renderer::RecordClear(SwapchainState& chain, const float clearColor[4], bool submit)
{
	if (!chain.active)
		return;
	static uint32 s_recordClearLogCount = 0;
	if (D3D12RuntimeLogCapped(s_recordClearLogCount, "RecordClear", 16))
	{
		cemuLog_log(LogType::Force, "D3D12: clear swapchain {}x{} color=({}, {}, {}, {}).",
					chain.size.x, chain.size.y, clearColor[0], clearColor[1], clearColor[2], clearColor[3]);
	}
	if (!m_commandListOpen)
		ResetCommandList();
	chain.frameIndex = chain.swapchain->GetCurrentBackBufferIndex();
	auto* resource = chain.backBuffers[chain.frameIndex].Get();
	D3D12TraceEvent("swapchain_clear", "window={} frameIndex={} chainSize={}x{} backBuffer={} stateBefore=0x{:08x} rtvCpu=0x{:016x} color={},{},{},{}",
					(&chain == &m_mainSwapchain) ? "tv" : "pad",
					chain.frameIndex,
					chain.size.x,
					chain.size.y,
					(const void*)resource,
					(uint32)chain.states[chain.frameIndex],
					(unsigned long long)chain.rtvHandles[chain.frameIndex].ptr,
					clearColor[0],
					clearColor[1],
					clearColor[2],
					clearColor[3]);
	Transition(resource, chain.states[chain.frameIndex], D3D12_RESOURCE_STATE_RENDER_TARGET);
	GetCurrentCommandSlot().commandList->ClearRenderTargetView(chain.rtvHandles[chain.frameIndex], clearColor, 0, nullptr);
	Transition(resource, chain.states[chain.frameIndex], D3D12_RESOURCE_STATE_PRESENT);
	// Keep the clear in the open command list until the next present or idle submit.
	if (submit)
		m_submitOnIdle = true;
}

void D3D12Renderer::ClearColorbuffer(bool padView)
{
	D3D12_EXCLUSIVE_RECORDING();
	const float color[4]{ 0.0f, 0.0f, 0.0f, 1.0f };
	RecordClear(GetSwapchain(true), color);
	m_uwpCompositeFrameStarted = false;
	m_uwpCompositeFrameRendered = false;
	m_uwpOutputDrawnThisFrame = {};
	m_uwpOutputPendingThisFrame = {};
}

void D3D12Renderer::DrawEmptyFrame(bool mainWindow)
{
	D3D12_EXCLUSIVE_RECORDING();
	if (mainWindow && CafeSystem::IsTitleRunning())
	{
		BeginUwpCompositeFrame(GetSwapchain(true));
		return;
	}
	const float color[4]{ 0.0f, 0.0f, 0.0f, 1.0f };
	RecordClear(GetSwapchain(mainWindow), color);
}

#if defined(CEMU_UWP_PERFORMANCE_EXPERIMENT)
void D3D12Renderer::InitializePerformanceExperiment()
{
	D3D12PerformanceExperiment::Config config{};
	const auto path = ActiveSettings::GetUserDataPath("d3d12-performance-experiment.txt");
	std::ifstream input(path, std::ios::binary);
	if (input)
	{
		std::array<char, 1025> text{};
		input.read(text.data(), (std::streamsize)text.size());
		const auto length = input.gcount();
		if (length > 1024 || input.bad() ||
			!D3D12PerformanceExperiment::ParseConfig(std::string_view(text.data(), (size_t)length), config))
		{
			config = {};
			cemuLog_log(LogType::Force, "D3D12 UWP experiment CONFIG rejected: malformed/oversized startup file; baseline mask=0.");
		}
	}
	m_performanceExperiment.Configure(config);
	LatteUwpSetPerformanceExperimentPhase(0, m_performanceExperiment.Mask(), false);
	cemuLog_log(LogType::Force,
		"D3D12 UWP experiment CONFIG: auto={} mask={} suite={} cpuMask={} stableFrames=120 settleFrames=30 measureFrames=180 windowFrames=60 phases={}.",
		config.automatic ? 1 : 0, m_performanceExperiment.Mask(), m_performanceExperiment.SuiteName(),
		m_performanceExperiment.CpuMask(), m_performanceExperiment.PhaseCount());
}

void D3D12Renderer::AdvancePerformanceExperiment()
{
	if (m_uwpPresentCount == m_performanceExperimentLastPresentCount)
		return;
	const auto now = std::chrono::steady_clock::now();
	const uint64 frameNs = m_performanceExperimentLastPresent.time_since_epoch().count() != 0 ?
		(uint64)std::chrono::duration_cast<std::chrono::nanoseconds>(now - m_performanceExperimentLastPresent).count() : 0;
	const uint64 frameDraws = m_gameDrawCallsSubmitted - m_performanceExperimentLastDraws;
	const uint64 frameCopies = m_feedbackLoopCopies - m_performanceExperimentLastCopies;
	const uint64 frameLogicCopies = m_logicOpEmulationCopies - m_performanceExperimentLastLogicCopies;
	const uint64 framePsoCreates = m_performanceExperimentRuntimePsoCreates - m_performanceExperimentLastPsoCreates;
	const uint64 frameFenceWaitUs = m_fenceWaitTimeUs - m_performanceExperimentLastFenceWaitUs;
	const uint64 frameResourceHits = m_gameResourceTableCacheHits - m_performanceExperimentLastResourceHits;
	const uint64 frameResourceMisses = m_gameResourceTableCacheMisses - m_performanceExperimentLastResourceMisses;
	m_performanceExperimentLastPresent = now;
	m_performanceExperimentLastPresentCount = m_uwpPresentCount;
	m_performanceExperimentLastDraws = m_gameDrawCallsSubmitted;
	m_performanceExperimentLastCopies = m_feedbackLoopCopies;
	m_performanceExperimentLastLogicCopies = m_logicOpEmulationCopies;
	m_performanceExperimentLastPsoCreates = m_performanceExperimentRuntimePsoCreates;
	m_performanceExperimentLastFenceWaitUs = m_fenceWaitTimeUs;
	m_performanceExperimentLastResourceHits = m_gameResourceTableCacheHits;
	m_performanceExperimentLastResourceMisses = m_gameResourceTableCacheMisses;
	auto& window = m_performanceExperimentWindow;
	if (m_performanceExperiment.Measuring())
	{
		++window.frames;
		window.elapsedNs += frameNs;
		window.maxFrameNs = std::max(window.maxFrameNs, frameNs);
		window.over100ms += frameNs > 100000000 ? 1u : 0u;
		window.draws += frameDraws;
		window.minDraws = std::min(window.minDraws, frameDraws);
		window.maxDraws = std::max(window.maxDraws, frameDraws);
		window.feedbackCopies += frameCopies;
		window.logicInitialCopies += frameLogicCopies;
		window.psoCreates += framePsoCreates;
		window.ineligibleFrames += m_performanceExperiment.EligibleForSuite(frameDraws, framePsoCreates != 0, frameLogicCopies) ? 0u : 1u;
		window.fenceWaitUs += frameFenceWaitUs;
		window.resourceCacheHits += frameResourceHits;
		window.resourceCacheMisses += frameResourceMisses;
	}
	if (m_performanceExperiment.GetState() == D3D12PerformanceExperiment::Schedule::State::Waiting)
	{
		m_performanceExperimentWaitElapsedNs += frameNs;
		++m_performanceExperimentWaitFrames;
		m_performanceExperimentWaitPsoCreates += framePsoCreates;
		m_performanceExperimentWaitMinDraws = std::min(m_performanceExperimentWaitMinDraws, frameDraws);
		m_performanceExperimentWaitMaxDraws = std::max(m_performanceExperimentWaitMaxDraws, frameDraws);
	}
	const auto event = m_performanceExperiment.OnFrame(frameDraws, framePsoCreates != 0, frameLogicCopies);
	if (m_performanceExperiment.GetState() == D3D12PerformanceExperiment::Schedule::State::Waiting &&
		m_performanceExperimentWaitElapsedNs >= 5000000000ull)
	{
		cemuLog_log(LogType::Force,
			"D3D12 UWP experiment WAIT: frames={} elapsed_ms={:.3f} lastDraws={} minDraws={} maxDraws={} stableFrames={} psoCreates={} requiredFrames=120 minimumDrawsExclusive={} lastLogicCopies={}.",
			m_performanceExperimentWaitFrames, (double)m_performanceExperimentWaitElapsedNs / 1000000.0,
			frameDraws, m_performanceExperimentWaitMinDraws, m_performanceExperimentWaitMaxDraws,
			m_performanceExperiment.StableFrames(), m_performanceExperimentWaitPsoCreates,
			m_performanceExperiment.MinimumDrawsExclusive(), frameLogicCopies);
		m_performanceExperimentWaitElapsedNs = 0;
		m_performanceExperimentWaitFrames = 0;
		m_performanceExperimentWaitPsoCreates = 0;
		m_performanceExperimentWaitMinDraws = UINT64_MAX;
		m_performanceExperimentWaitMaxDraws = 0;
	}
	if (event.window)
	{
		cemuLog_log(LogType::Force,
			"D3D12 UWP experiment WINDOW: phase={} mask={} window={} frames={} elapsed_ms={:.6f} fps={:.6f} max_frame_ms={:.6f} over100ms={} "
			"draws={} minDraws={} maxDraws={} feedbackCopies={} psoCreates={} ineligibleFrames={} fenceWait_us={} resourceCacheHits={} resourceCacheMisses={} "
			"stageCacheEligible={} satisfiedTransitionOpportunities={} satisfiedTransitionHits={} transitionAllocationOpportunities={} transitionAllocationsAvoided={} retainedGrowthAllocations={} detailClockReads={} "
			"logicInitialCopies={} logicRefreshCopies={} logicRefreshPayloadBytes={} logicFullRefreshPayloadBytes={} logicEmptyRefreshSkips={} uniformOrEligible={} uniformOrBatched={} nativeDraws={}.",
			event.completedPhase, event.completedMask, event.windowIndex, window.frames, (double)window.elapsedNs / 1000000.0,
			window.elapsedNs ? (double)window.frames * 1000000000.0 / (double)window.elapsedNs : 0.0,
			(double)window.maxFrameNs / 1000000.0, window.over100ms, window.draws, window.minDraws, window.maxDraws,
			window.feedbackCopies, window.psoCreates, window.ineligibleFrames, window.fenceWaitUs, window.resourceCacheHits, window.resourceCacheMisses,
			window.stageCacheEligible, window.satisfiedTransitionOpportunities, window.satisfiedTransitionHits,
			window.transitionAllocationOpportunities, window.transitionAllocationsAvoided, window.retainedGrowthAllocations, window.detailClockReads,
			window.logicInitialCopies, window.logicRefreshCopies, window.logicRefreshPayloadBytes,
			window.logicFullRefreshPayloadBytes, window.logicEmptyRefreshSkips, window.uniformOrEligible, window.uniformOrBatched, window.nativeDraws);
		cemuLog_log(LogType::Force,
			"D3D12 UWP experiment DETAIL: phase={} mask={} window={} ns prepare/feedback/descriptors/renderState/indexResidency={}/{}/{}/{}/{} samples={}/{}/{}/{}/{}.",
			event.completedPhase, event.completedMask, event.windowIndex,
			window.detailTimeNs[0], window.detailTimeNs[1], window.detailTimeNs[2], window.detailTimeNs[3], window.detailTimeNs[4],
			window.detailSamples[0], window.detailSamples[1], window.detailSamples[2], window.detailSamples[3], window.detailSamples[4]);
		cemuLog_log(LogType::Force,
			"D3D12 UWP experiment BROAD: phase={} mask={} window={} ns setup/indexDecode/bufferSync/pipeline/descriptors/draw/retireSubmit={}/{}/{}/{}/{}/{}/{} samples={}/{}/{}/{}/{}/{}/{}.",
			event.completedPhase, event.completedMask, event.windowIndex,
			window.broadTimeNs[0], window.broadTimeNs[1], window.broadTimeNs[2], window.broadTimeNs[3], window.broadTimeNs[4], window.broadTimeNs[5], window.broadTimeNs[6],
			window.broadSamples[0], window.broadSamples[1], window.broadSamples[2], window.broadSamples[3], window.broadSamples[4], window.broadSamples[5], window.broadSamples[6]);
		window = {};
	}
	if (event.end)
		cemuLog_log(LogType::Force, "D3D12 UWP experiment END: phase={} mask={} measuredFrames=180.", event.completedPhase, event.completedMask);
	if (event.beginSettle || event.complete)
	{
		// a timing-only phase changes no CPU feature, so its caches are left alone
		if (!m_performanceExperiment.IsBoundarySuite())
		{
			m_gameResourceTableCache.clear();
			m_gameSamplerTableCache.clear();
			m_resourceTableKeyMemo = {};
			m_samplerTableKeyMemo = {};
		}
		window = {};
		LatteUwpSetPerformanceExperimentPhase(event.complete ? 0 : m_performanceExperiment.Phase(), m_performanceExperiment.Mask(), false);
		if (event.complete)
			cemuLog_log(LogType::Force, "D3D12 UWP experiment COMPLETE: phases={} mask=0 suite={} cpuMask={}.",
				m_performanceExperiment.PhaseCount(), m_performanceExperiment.SuiteName(), m_performanceExperiment.CpuMask());
		else
			cemuLog_log(LogType::Force, "D3D12 UWP experiment BEGIN: phase={} mask={} state=settle.", m_performanceExperiment.Phase(), m_performanceExperiment.Mask());
	}
	if (event.beginMeasure)
	{
		window = {};
		LatteUwpSetPerformanceExperimentPhase(m_performanceExperiment.Phase(), m_performanceExperiment.Mask(), true);
		cemuLog_log(LogType::Force, "D3D12 UWP experiment BEGIN: phase={} mask={} state=measure.", m_performanceExperiment.Phase(), m_performanceExperiment.Mask());
	}
}
#endif

void D3D12Renderer::LogUwpCpuPerformanceSummary()
{
#if defined(LATTE_UWP_DETAILED_PROFILING)
	constexpr uint64 kLogIntervalMs = 5000;
	const uint64 nowMs = GetTickCount64();
	if (m_uwpCpuProfileIntervalStartedMs == 0)
	{
		m_uwpCpuProfileIntervalStartedMs = nowMs;
		m_uwpCpuProfileDraws = 0;
		m_uwpCpuProfileSampledDraws = 0;
		m_uwpCpuProfileStageTimeNs.fill(0);
		m_uwpCpuProfileStageSamples.fill(0);
		m_uwpCpuProfileTailTimeNs.fill(0);
		m_uwpCpuProfileTailSamples.fill(0);
		m_uwpSequenceProfileCalls = 0;
		m_uwpSequenceProfileSamples = 0;
		m_uwpSequenceProfileTimeNs = 0;
		m_uwpBufferProfile = {};
#if defined(CEMU_UWP_D3D12_WORKLOAD_PROFILE)
		m_uwpWorkloadProfile = {};
		m_workloadGpuLastSamples = m_gpuCommandListSamples;
		m_workloadGpuLastTicks = m_gpuCommandListTicks;
		m_workloadGpuLastInvalidPairs = m_workloadGpuInvalidPairs;
		m_workloadGpuWindowMaxTicks = 0;
#endif
		m_uwpCpuProfileLastSubmittedCommandLists = m_numSubmittedCommandLists;
		m_uwpCpuProfileLastFenceWaitCount = m_fenceWaitCount;
		m_uwpCpuProfileLastFenceWaitTimeUs = m_fenceWaitTimeUs;
		m_uwpCpuProfileLastPresentCount = m_uwpPresentCount;
		m_uwpCpuProfileLastPresentTimeUs = m_uwpPresentTimeUs;
		m_uwpCpuProfileLastFeedbackLoopCopies = m_feedbackLoopCopies;
		m_uwpCpuProfileLastCommandScratchTextureHits = m_commandScratchTextureHits;
		m_uwpCpuProfileLastScratchTexturePoolMisses = m_scratchTexturePoolMisses;
		m_uwpDescriptorProfileLastStats = m_descriptorManager ? m_descriptorManager->GetStats() : D3D12DescriptorManager::Stats{};
		m_uwpDescriptorResourceCacheClearCount = 0;
		m_uwpDescriptorResourceCacheClearTimeNs = 0;
		m_uwpDescriptorResourceCacheClearMaxNs = 0;
		m_uwpDescriptorSamplerCacheClearCount = 0;
		m_uwpDescriptorSamplerCacheClearTimeNs = 0;
		m_uwpDescriptorSamplerCacheClearMaxNs = 0;
		return;
	}

	const uint64 intervalMs = nowMs - m_uwpCpuProfileIntervalStartedMs;
	if (intervalMs < kLogIntervalMs)
		return;

	{
		const size_t pipelineStage = (size_t)UwpCpuDrawStage::PipelineLookup;
		m_uwpLastHeadPipelineUs = m_uwpCpuProfileStageSamples[pipelineStage] ?
			(double)m_uwpCpuProfileStageTimeNs[pipelineStage] / (1000.0 * (double)m_uwpCpuProfileStageSamples[pipelineStage]) : 0.0;
	}
	for (uint32 i = 0; i < kUwpCpuDrawStageCount; ++i)
	{
		m_uwpCpuProfileStageTimeNs[i] += std::exchange(m_uwpCpuProfileTailTimeNs[i], 0);
		m_uwpCpuProfileStageSamples[i] += std::exchange(m_uwpCpuProfileTailSamples[i], 0);
	}
	std::array<double, kUwpCpuDrawStageCount> meanStageUs{};
	for (uint32 i = 0; i < kUwpCpuDrawStageCount; ++i)
	{
		if (m_uwpCpuProfileStageSamples[i] != 0)
		{
			meanStageUs[i] = (double)m_uwpCpuProfileStageTimeNs[i] /
				(1000.0 * (double)m_uwpCpuProfileStageSamples[i]);
		}
	}

	const uint64 submittedCommandLists =
		m_numSubmittedCommandLists - m_uwpCpuProfileLastSubmittedCommandLists;
	const uint64 fenceWaits = m_fenceWaitCount - m_uwpCpuProfileLastFenceWaitCount;
	const uint64 fenceWaitUs = m_fenceWaitTimeUs - m_uwpCpuProfileLastFenceWaitTimeUs;
	const uint64 presentCalls = m_uwpPresentCount - m_uwpCpuProfileLastPresentCount;
	const uint64 presentWaitUs = m_uwpPresentTimeUs - m_uwpCpuProfileLastPresentTimeUs;
	const uint64 feedbackCopies = m_feedbackLoopCopies - m_uwpCpuProfileLastFeedbackLoopCopies;
	const uint64 commandScratchHits =
		m_commandScratchTextureHits - m_uwpCpuProfileLastCommandScratchTextureHits;
	const uint64 scratchPoolMisses =
		m_scratchTexturePoolMisses - m_uwpCpuProfileLastScratchTexturePoolMisses;
	{
		const double splitSamples = (double)std::max<uint64>(m_uwpDescriptorSplitSamples, 1);
		const double uniformUs = (double)m_uwpDescriptorSplitNs[1] / 1000.0 / splitSamples;
		cemuLog_log(LogType::Force, "D3D12 UWP descriptor split: samples={} mean_us feedback/uniformPrepare/tables/beforeTables/afterTables={:.2f}/{:.2f}/{:.2f}/{:.2f}/{:.2f} view transitions fast/slow={}/{} table keys reused/built resource={}/{} sampler={}/{} resourceTables hit/miss={}/{} samplerTables hit/miss={}/{}.",
			m_uwpDescriptorSplitSamples, (double)m_uwpDescriptorSplitNs[0] / 1000.0 / splitSamples, uniformUs,
			(double)m_uwpDescriptorSplitNs[2] / 1000.0 / splitSamples, (double)m_uwpDescriptorSplitNs[3] / 1000.0 / splitSamples,
			(double)m_uwpDescriptorSplitNs[4] / 1000.0 / splitSamples,
			std::exchange(m_uwpViewTransitionsFast, 0), std::exchange(m_uwpViewTransitionsSlow, 0),
			std::exchange(m_tableKeyReuses[0], 0), std::exchange(m_tableKeyBuilds[0], 0),
			std::exchange(m_tableKeyReuses[1], 0), std::exchange(m_tableKeyBuilds[1], 0),
			m_gameResourceTableCacheHits - m_uwpProfileLastResourceHits, m_gameResourceTableCacheMisses - m_uwpProfileLastResourceMisses,
			m_gameSamplerTableCacheHits - m_uwpProfileLastSamplerHits, m_gameSamplerTableCacheMisses - m_uwpProfileLastSamplerMisses);
		m_uwpDescriptorSplitNs = {};
		m_uwpDescriptorSplitSamples = 0;
		m_uwpProfileLastResourceHits = m_gameResourceTableCacheHits;
		m_uwpProfileLastResourceMisses = m_gameResourceTableCacheMisses;
		m_uwpProfileLastSamplerHits = m_gameSamplerTableCacheHits;
		m_uwpProfileLastSamplerMisses = m_gameSamplerTableCacheMisses;
		cemuLog_log(LogType::Force, "D3D12 UWP feedback copies: reused={} (new copies are feedbackCopies in the CPU profile line).",
			m_feedbackCopyReuses - m_uwpProfileLastFeedbackReuses);
		m_uwpProfileLastFeedbackReuses = m_feedbackCopyReuses;
		{
			std::vector<std::pair<uint32, std::pair<uint32, uint64>>> idleCallers(m_uwpIdleWaitByLine.begin(), m_uwpIdleWaitByLine.end());
			std::sort(idleCallers.begin(), idleCallers.end(), [](const auto& a, const auto& b) { return a.second.second > b.second.second; });
			std::string callers;
			for (size_t i = 0; i < idleCallers.size() && i < 5; ++i)
				fmt::format_to(std::back_inserter(callers), "{}line {}: {}x {:.1f}ms", i ? ", " : "", idleCallers[i].first, idleCallers[i].second.first, (double)idleCallers[i].second.second / 1000.0);
			cemuLog_log(LogType::Force, "D3D12 UWP GPU waits: idle {}x {:.1f}ms, command slot {}x {:.1f}ms, fence {}x {:.1f}ms, query {}x {:.1f}ms; idle callers [{}]; slot waits by frames queued 0/1/2/3+: {}/{}/{}/{}.",
				m_uwpWaitCount[0], (double)m_uwpWaitUs[0] / 1000.0, m_uwpWaitCount[1], (double)m_uwpWaitUs[1] / 1000.0,
				m_uwpWaitCount[2], (double)m_uwpWaitUs[2] / 1000.0, m_uwpWaitCount[3], (double)m_uwpWaitUs[3] / 1000.0, callers,
				m_uwpSlotWaitsByQueuedPresents[0], m_uwpSlotWaitsByQueuedPresents[1], m_uwpSlotWaitsByQueuedPresents[2], m_uwpSlotWaitsByQueuedPresents[3]);
			m_uwpSlotWaitsByQueuedPresents = {};
			m_uwpWaitUs = {};
			m_uwpWaitCount = {};
			m_uwpIdleWaitByLine.clear();
			std::vector<std::pair<uint32, uint32>> skipTargets(m_uwpAsyncSkipsByTarget.begin(), m_uwpAsyncSkipsByTarget.end());
			std::sort(skipTargets.begin(), skipTargets.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
			std::string skipText;
			for (size_t i = 0; i < skipTargets.size() && i < 5; ++i)
				fmt::format_to(std::back_inserter(skipText), "{}{}x{}: {}", i ? ", " : "", skipTargets[i].first >> 16, skipTargets[i].first & 0xFFFF, skipTargets[i].second);
			size_t queuedAsyncJobs;
			{
				std::scoped_lock asyncLock(m_asyncPipelineMutex);
				queuedAsyncJobs = m_asyncPipelineJobs.size();
			}
			if (m_recordThreadRunning.load(std::memory_order_relaxed))
			{
				// threaded recording: is the recorder the bottleneck (busy all the time, Latte waiting on a full queue),
				// or is the Latte-side head itself the cost?
				uint64 fullWaits, fullWaitNs;
				std::vector<std::pair<const char*, RecordExclusiveStat>> exclusive;
				{
					std::scoped_lock recordLock(m_recordQueueMutex);
					fullWaits = std::exchange(m_recordFullWaits, 0);
					fullWaitNs = std::exchange(m_recordFullWaitNs, 0);
					exclusive.assign(m_recordExclusiveStats.begin(), m_recordExclusiveStats.end());
					m_recordExclusiveStats.clear();
				}
				const double queueCallUs = m_recordQueueSamples ? (double)m_recordQueueSampledNs / 1000.0 / (double)m_recordQueueSamples : 0.0;
				m_recordQueueSampledNs = 0;
				m_recordQueueSamples = 0;
				std::sort(exclusive.begin(), exclusive.end(), [](const auto& a, const auto& b) { return a.second.waitNs > b.second.waitNs; });
				uint64 exclusiveCount = 0, exclusiveNs = 0;
				std::string exclusiveText;
				for (size_t i = 0; i < exclusive.size(); ++i)
				{
					exclusiveCount += exclusive[i].second.count;
					exclusiveNs += exclusive[i].second.waitNs;
					if (i < 5)
					{
						// keep only "D3D12Renderer::name" out of the full signature
						std::string_view name = exclusive[i].first;
						if (const size_t paren = name.find('('); paren != std::string_view::npos)
							name = name.substr(0, paren);
						if (const size_t space = name.rfind(' '); space != std::string_view::npos)
							name = name.substr(space + 1);
						exclusiveText += fmt::format("{}{} {}x {:.0f} ms", exclusiveText.empty() ? "" : ", ", name, exclusive[i].second.count, (double)exclusive[i].second.waitNs / 1e6);
					}
				}
				cemuLog_log(LogType::Force, "D3D12 UWP recorder: busy {:.0f} ms, {} ops; Latte waited on a full queue {}x for {:.0f} ms.",
					(double)m_recordBusyNs.exchange(0, std::memory_order_relaxed) / 1e6, m_recordOps.exchange(0, std::memory_order_relaxed),
					fullWaits, (double)fullWaitNs / 1e6);
				const double collectUs = m_uwpCollectRecordSamples ? (double)m_uwpCollectRecordNs / 1000.0 / (double)m_uwpCollectRecordSamples : 0.0;
				m_uwpCollectRecordNs = 0;
				m_uwpCollectRecordSamples = 0;
				cemuLog_log(LogType::Force, "D3D12 UWP recorder, Latte side: queueing {:.2f} us per op, pipeline stage head {:.2f} us per draw (register delta {:.2f}); exclusive scopes {}x waited {:.0f} ms [{}].",
					queueCallUs, m_uwpLastHeadPipelineUs, collectUs, exclusiveCount, (double)exclusiveNs / 1e6, exclusiveText);
			}
			// queue length and guard state: a long queue above the guard cost FPS in light scenes
			cemuLog_log(LogType::Force, "D3D12 UWP async-skipped draws: {} [{}], async PSO queue {} (running {}), memory guard {}, PSOs reused by identical content {}.", m_uwpAsyncSkips, skipText,
				queuedAsyncJobs, m_asyncPipelineJobsOutstanding.load(std::memory_order_relaxed), CemuUwp::MemoryBudgetService::IsHeavyWorkDeferred() ? "on" : "off",
				m_gamePipelineContentReuses);
			m_uwpAsyncSkips = 0;
			m_uwpAsyncSkipsByTarget.clear();
		}
		// GPU headroom: direct-queue command list spans (lists run back to back, so this is close to GPU busy time)
		const uint64 gpuTicks = m_gpuCommandListTicks - m_uwpProfileLastGpuTicks;
		m_uwpProfileLastGpuTicks = m_gpuCommandListTicks;
		if (m_timestampFrequency != 0)
		{
			const double gpuMs = (double)gpuTicks * 1000.0 / (double)m_timestampFrequency;
			cemuLog_log(LogType::Force, "D3D12 UWP GPU time: busy_ms={:.1f} of {}ms ({:.0f}%), per_present_ms={:.2f}.",
				gpuMs, intervalMs, intervalMs != 0 ? gpuMs * 100.0 / (double)intervalMs : 0.0, presentCalls != 0 ? gpuMs / (double)presentCalls : 0.0);
		}
	}
	cemuLog_log(LogType::Force,
		"D3D12 UWP CPU profile: interval={}ms draws={} samples={} reached={}/{}/{}/{}/{}/{}/{} "
		"mean_us setup/indexDecode/bufferSync/pipeline/descriptors/draw/retireSubmit={:.2f}/{:.2f}/{:.2f}/{:.2f}/{:.2f}/{:.2f}/{:.2f} "
		"commandLists={} presentCalls={} presentWait={:.2f}ms fenceWaits={} fenceWait={:.2f}ms "
		"feedbackCopies={} commandScratchHits={} scratchPoolMisses={} sequences={} sequenceSamples={} sequenceMean_us={:.2f}.",
		(unsigned long long)intervalMs,
		(unsigned long long)m_uwpCpuProfileDraws,
		(unsigned long long)m_uwpCpuProfileSampledDraws,
		(unsigned long long)m_uwpCpuProfileStageSamples[0],
		(unsigned long long)m_uwpCpuProfileStageSamples[1],
		(unsigned long long)m_uwpCpuProfileStageSamples[2],
		(unsigned long long)m_uwpCpuProfileStageSamples[3],
		(unsigned long long)m_uwpCpuProfileStageSamples[4],
		(unsigned long long)m_uwpCpuProfileStageSamples[5],
		(unsigned long long)m_uwpCpuProfileStageSamples[6],
		meanStageUs[0],
		meanStageUs[1],
		meanStageUs[2],
		meanStageUs[3],
		meanStageUs[4],
		meanStageUs[5],
		meanStageUs[6],
		(unsigned long long)submittedCommandLists,
		(unsigned long long)presentCalls,
		(double)presentWaitUs / 1000.0,
		(unsigned long long)fenceWaits,
		(double)fenceWaitUs / 1000.0,
		(unsigned long long)feedbackCopies,
		(unsigned long long)commandScratchHits,
		(unsigned long long)scratchPoolMisses,
		(unsigned long long)m_uwpSequenceProfileCalls,
		(unsigned long long)m_uwpSequenceProfileSamples,
		m_uwpSequenceProfileSamples != 0 ?
			(double)m_uwpSequenceProfileTimeNs / (1000.0 * (double)m_uwpSequenceProfileSamples) : 0.0);
	cemuLog_log(LogType::Force,
		"D3D12 UWP buffer profile: indexReserves={} indexKB={:.1f} indexRingUploads={} sameFenceHits={} indexRingKB={:.1f} bufferUploads={} bufferKB={:.1f}.",
		m_uwpBufferProfile.indexReserves,
		(double)m_uwpBufferProfile.indexRequestedBytes / 1024.0,
		m_uwpBufferProfile.indexRingUploads,
		m_uwpBufferProfile.indexRingSameFenceHits,
		(double)m_uwpBufferProfile.indexRingUploadBytes / 1024.0,
		m_uwpBufferProfile.bufferUploads,
		(double)m_uwpBufferProfile.bufferUploadBytes / 1024.0);
	const auto descriptorStats = m_descriptorManager ? m_descriptorManager->GetStats() : D3D12DescriptorManager::Stats{};
	cemuLog_log(LogType::Force,
		"D3D12 UWP descriptor profile: create/reuse_count cbv={}/{} sampler={}/{} "
		"create_fail cbv/sampler={}/{} create/reuse_ms cbv={:.3f}/{:.3f} sampler={:.3f}/{:.3f} poolFree cbv/sampler={}/{} discards={} "
		"cacheClear_count cbv/sampler={}/{} cacheClear_total_ms={:.3f}/{:.3f} cacheClear_max_ms={:.3f}/{:.3f}.",
		(unsigned long long)(descriptorStats.shaderVisibleCbvSrvUavHeapCreates - m_uwpDescriptorProfileLastStats.shaderVisibleCbvSrvUavHeapCreates),
		(unsigned long long)(descriptorStats.shaderVisibleCbvSrvUavHeapReuses - m_uwpDescriptorProfileLastStats.shaderVisibleCbvSrvUavHeapReuses),
		(unsigned long long)(descriptorStats.shaderVisibleSamplerHeapCreates - m_uwpDescriptorProfileLastStats.shaderVisibleSamplerHeapCreates),
		(unsigned long long)(descriptorStats.shaderVisibleSamplerHeapReuses - m_uwpDescriptorProfileLastStats.shaderVisibleSamplerHeapReuses),
		(unsigned long long)(descriptorStats.shaderVisibleCbvSrvUavHeapCreateFailures - m_uwpDescriptorProfileLastStats.shaderVisibleCbvSrvUavHeapCreateFailures),
		(unsigned long long)(descriptorStats.shaderVisibleSamplerHeapCreateFailures - m_uwpDescriptorProfileLastStats.shaderVisibleSamplerHeapCreateFailures),
		(double)(descriptorStats.shaderVisibleCbvSrvUavHeapCreateTimeNs - m_uwpDescriptorProfileLastStats.shaderVisibleCbvSrvUavHeapCreateTimeNs) / 1000000.0,
		(double)(descriptorStats.shaderVisibleCbvSrvUavHeapReuseTimeNs - m_uwpDescriptorProfileLastStats.shaderVisibleCbvSrvUavHeapReuseTimeNs) / 1000000.0,
		(double)(descriptorStats.shaderVisibleSamplerHeapCreateTimeNs - m_uwpDescriptorProfileLastStats.shaderVisibleSamplerHeapCreateTimeNs) / 1000000.0,
		(double)(descriptorStats.shaderVisibleSamplerHeapReuseTimeNs - m_uwpDescriptorProfileLastStats.shaderVisibleSamplerHeapReuseTimeNs) / 1000000.0,
		descriptorStats.shaderVisibleCbvSrvUavHeapPoolFree,
		descriptorStats.shaderVisibleSamplerHeapPoolFree,
		(unsigned long long)(descriptorStats.shaderVisibleHeapPoolDiscards - m_uwpDescriptorProfileLastStats.shaderVisibleHeapPoolDiscards),
		(unsigned long long)m_uwpDescriptorResourceCacheClearCount,
		(unsigned long long)m_uwpDescriptorSamplerCacheClearCount,
		(double)m_uwpDescriptorResourceCacheClearTimeNs / 1000000.0,
		(double)m_uwpDescriptorSamplerCacheClearTimeNs / 1000000.0,
		(double)m_uwpDescriptorResourceCacheClearMaxNs / 1000000.0,
		(double)m_uwpDescriptorSamplerCacheClearMaxNs / 1000000.0);

#if defined(CEMU_UWP_D3D12_WORKLOAD_PROFILE)
	const auto& workload = m_uwpWorkloadProfile;
	cemuLog_log(LogType::Force,
		"D3D12 UWP workload: title={:016x} interval={}ms presents={} guest/nativeDraws={}/{} "
		"ordered/stencilDraws={}/{} logicalPrimitives={} logicCopies initial/refresh={}/{} payloadMiB={:.3f}/{:.3f} "
		"submitReasons reset/idle/present/flush/drawBudget/readback/scratch/query/other={}/{}/{}/{}/{}/{}/{}/{}/{} "
		"maxDraw native={} indices={} instances={} mode={} pipeline={:016x} vs={:016x} ps={:016x}.",
		CafeSystem::IsTitleRunning() ? CafeSystem::GetForegroundTitleId() : 0ull,
		intervalMs, presentCalls, workload.guestDraws, workload.nativeDraws,
		workload.orderedDraws, workload.stencilDraws, workload.logicalPrimitives,
		workload.initialLogicCopies, workload.refreshLogicCopies,
		(double)workload.initialLogicPayloadBytes / (1024.0 * 1024.0),
		(double)workload.refreshLogicPayloadBytes / (1024.0 * 1024.0),
		workload.submitReasons[0], workload.submitReasons[1], workload.submitReasons[2],
		workload.submitReasons[3], workload.submitReasons[4], workload.submitReasons[5],
		workload.submitReasons[6], workload.submitReasons[7], workload.unspecifiedSubmits,
		workload.maxNativeDraws, workload.maxDrawIndices, workload.maxDrawInstances,
		workload.maxDrawMode, workload.maxDrawPipeline, workload.maxDrawVs, workload.maxDrawPs);
	for (size_t mode = 0; mode < workload.modeDraws.size(); ++mode)
	{
		if (workload.modeDraws[mode] != 0 || workload.modeRefreshCopies[mode] != 0)
			cemuLog_log(LogType::Force, "D3D12 UWP workload mode={}: guest/native/refreshCopies={}/{}/{}.",
				mode, workload.modeDraws[mode], workload.modeNativeDraws[mode], workload.modeRefreshCopies[mode]);
	}
	LogWorkloadGpuTiming();
	m_uwpWorkloadProfile = {};
#endif
	m_uwpCpuProfileIntervalStartedMs = nowMs;
	m_uwpCpuProfileDraws = 0;
	m_uwpCpuProfileSampledDraws = 0;
	m_uwpCpuProfileStageTimeNs.fill(0);
	m_uwpCpuProfileStageSamples.fill(0);
	m_uwpSequenceProfileCalls = 0;
	m_uwpSequenceProfileSamples = 0;
	m_uwpSequenceProfileTimeNs = 0;
	m_uwpBufferProfile = {};
	m_uwpCpuProfileLastSubmittedCommandLists = m_numSubmittedCommandLists;
	m_uwpCpuProfileLastFenceWaitCount = m_fenceWaitCount;
	m_uwpCpuProfileLastFenceWaitTimeUs = m_fenceWaitTimeUs;
	m_uwpCpuProfileLastPresentCount = m_uwpPresentCount;
	m_uwpCpuProfileLastPresentTimeUs = m_uwpPresentTimeUs;
	m_uwpCpuProfileLastFeedbackLoopCopies = m_feedbackLoopCopies;
	m_uwpCpuProfileLastCommandScratchTextureHits = m_commandScratchTextureHits;
	m_uwpCpuProfileLastScratchTexturePoolMisses = m_scratchTexturePoolMisses;
	m_uwpDescriptorProfileLastStats = descriptorStats;
	m_uwpDescriptorResourceCacheClearCount = 0;
	m_uwpDescriptorResourceCacheClearTimeNs = 0;
	m_uwpDescriptorResourceCacheClearMaxNs = 0;
	m_uwpDescriptorSamplerCacheClearCount = 0;
	m_uwpDescriptorSamplerCacheClearTimeNs = 0;
	m_uwpDescriptorSamplerCacheClearMaxNs = 0;
#endif
}

void D3D12Renderer::LogUwpCommandSubmitPerformanceSummary()
{
#if defined(LATTE_UWP_DETAILED_PROFILING)
	constexpr uint64 kLogIntervalMs = 5000;
	const uint64 nowMs = GetTickCount64();
	auto& profile = m_uwpCommandSubmitProfile;
	if (profile.intervalStartedMs == 0)
	{
		profile = {};
		profile.intervalStartedMs = nowMs;
		return;
	}

	const uint64 intervalMs = nowMs - profile.intervalStartedMs;
	if (intervalMs < kLogIntervalMs)
		return;

	std::array<double, UwpCommandSubmitProfile::kPhaseCount> phaseMeanUs{};
	std::array<double, UwpCommandSubmitProfile::kPhaseCount> phaseMaxUs{};
	for (size_t i = 0; i < UwpCommandSubmitProfile::kPhaseCount; ++i)
	{
		if (profile.submitCount != 0)
			phaseMeanUs[i] = (double)profile.phaseTimeNs[i] / (1000.0 * (double)profile.submitCount);
		phaseMaxUs[i] = (double)profile.phaseMaxNs[i] / 1000.0;
	}
	const double totalMeanUs = profile.submitCount != 0
		? (double)profile.totalTimeNs / (1000.0 * (double)profile.submitCount)
		: 0.0;
	const double totalMaxUs = (double)profile.totalMaxNs / 1000.0;

	cemuLog_log(LogType::Force,
		"D3D12 UWP command-submit profile: interval={}ms submits={} "
		"mean_us preClose/close/executeSignal/scratchReset/finishedSlotCleanup/total="
		"{:.2f}/{:.2f}/{:.2f}/{:.2f}/{:.2f}/{:.2f} "
		"max_us={:.2f}/{:.2f}/{:.2f}/{:.2f}/{:.2f}/{:.2f}.",
		(unsigned long long)intervalMs,
		(unsigned long long)profile.submitCount,
		phaseMeanUs[0],
		phaseMeanUs[1],
		phaseMeanUs[2],
		phaseMeanUs[3],
		phaseMeanUs[4],
		totalMeanUs,
		phaseMaxUs[0],
		phaseMaxUs[1],
		phaseMaxUs[2],
		phaseMaxUs[3],
		phaseMaxUs[4],
		totalMaxUs);

	profile = {};
	profile.intervalStartedMs = nowMs;
#endif
}

bool D3D12Renderer::ShouldLogDrawFailure(uint64 signature)
{
	constexpr size_t kMaxLoggedDrawFailures = 256;
	if (m_loggedDrawFailureSignatures.size() >= kMaxLoggedDrawFailures)
		return false;
	return m_loggedDrawFailureSignatures.insert(signature).second;
}

void D3D12Renderer::LogDroppedDrawSummary()
{
	constexpr uint64 kLogIntervalMs = 10000;
	constexpr uint32 kMaxLogLines = 360;
	if (m_droppedDraws == m_droppedDrawsLogged || m_droppedDrawsLogLines >= kMaxLogLines)
		return;
	const uint64 nowMs = GetTickCount64();
	if (m_droppedDrawsLogTimeMs != 0 && nowMs - m_droppedDrawsLogTimeMs < kLogIntervalMs)
		return;
	uint64 newDrops = 0;
	for (size_t i = 0; i < m_droppedDraws.size(); ++i)
		newDrops += m_droppedDraws[i] - m_droppedDrawsLogged[i];
	m_droppedDrawsLogged = m_droppedDraws;
	m_droppedDrawsLogTimeMs = nowMs;
	++m_droppedDrawsLogLines;
	cemuLog_log(LogType::Force,
		"D3D12: {} draws dropped since the last report; totals sequence={} logicOp={} shaderCode={} pso={} index={} indexUpload={} cullAll={}.",
		(unsigned long long)newDrops,
		(unsigned long long)m_droppedDraws[(size_t)DroppedDrawReason::SequenceRejected],
		(unsigned long long)m_droppedDraws[(size_t)DroppedDrawReason::LogicOpUnsupported],
		(unsigned long long)m_droppedDraws[(size_t)DroppedDrawReason::MissingShaderCode],
		(unsigned long long)m_droppedDraws[(size_t)DroppedDrawReason::MissingPipeline],
		(unsigned long long)m_droppedDraws[(size_t)DroppedDrawReason::InvalidIndexBuffer],
		(unsigned long long)m_droppedDraws[(size_t)DroppedDrawReason::IndexUploadFailed],
		(unsigned long long)m_cullAllDrawsSkipped);
}


void D3D12Renderer::SwapBuffers(bool swapTV, bool swapDRC)
{
	D3D12_EXCLUSIVE_RECORDING();
	FlushPendingDestroys();
	D3D12PipelineStableCache::GetInstance().SaveUsageIfDue();
#if defined(CEMU_UWP_D3D12_DIAGNOSTICS)
	if (m_geometryProbe)
		m_geometryProbe->Poll(m_device.Get(), m_memoryManager.get(), m_fence.Get());
#endif
	static uint32 s_swapBuffersLogCount = 0;
	if (D3D12RuntimeLogCapped(s_swapBuffersLogCount, "SwapBuffers", 32))
	{
		cemuLog_log(LogType::Force, "D3D12: SwapBuffers TV={} DRC={} mainActive={} padActive={} recordedDraws={} gameDraws={}.",
					swapTV, swapDRC, m_mainSwapchain.active, m_padSwapchain.active,
					m_recordedDrawcalls, (unsigned long long)m_gameDrawCallsSubmitted);
	}
	const bool presentUwpComposite = (swapTV || swapDRC) && m_mainSwapchain.active;
	if ((swapTV || swapDRC) && CafeSystem::IsTitleRunning())
		m_gpuWatchdog.NotifyGameFrame();
	if (presentUwpComposite && CafeSystem::IsTitleRunning() && !m_uwpCompositeFrameStarted && !m_uwpCompositeFrameRendered)
		BeginUwpCompositeFrame(m_mainSwapchain);
	if (presentUwpComposite && CafeSystem::IsTitleRunning() && !m_uwpCompositeFrameRendered)
		DrawUwpCachedVisibleOutputs();
	if (presentUwpComposite)
	{
		for (size_t i = 0; i < m_uwpOutputPendingThisFrame.size(); i++)
		{
			if (m_uwpOutputPendingThisFrame[i] && !m_uwpOutputDrawnThisFrame[i])
				DrawUwpCachedOutput(i != 0);
		}
	}
	ExecuteCurrentCommandList(SubmitPresent);
	m_uwpPresentFences[m_uwpPresentFenceIndex++ % m_uwpPresentFences.size()] = m_uwpLastSubmittedFence;
	// the settings offer every, every second and every third refresh; the latter paces 30 and 20 fps
	// titles evenly instead of collapsing to every refresh
	const UINT syncInterval = (UINT)std::clamp<sint32>(GetConfig().vsync.GetValue(), 0, 3);
	if (presentUwpComposite)
	{
		const uint32 presentedFrame = m_mainSwapchain.frameIndex;
		D3D12TraceEvent("present_begin", "window=uwp-composite frameIndex={} syncInterval={} size={}x{} backBuffer={} state=0x{:08x} rtvCpu=0x{:016x} swapTV={} swapPad={}",
						presentedFrame,
						syncInterval,
						m_mainSwapchain.size.x,
						m_mainSwapchain.size.y,
						(const void*)m_mainSwapchain.backBuffers[presentedFrame].Get(),
						(uint32)m_mainSwapchain.states[presentedFrame],
						(unsigned long long)m_mainSwapchain.rtvHandles[presentedFrame].ptr,
						swapTV ? 1 : 0,
						swapDRC ? 1 : 0);
#if defined(LATTE_UWP_DETAILED_PROFILING)
		const auto presentStarted = std::chrono::steady_clock::now();
#endif
		HRESULT hr = E_FAIL;
		{
			LatteUwpHitchScope hitchScope(LatteUwpHitchOp::Present);
			D3D12GpuWatchdog::ActivityScope watchdogActivity(m_gpuWatchdog, D3D12GpuWatchdog::Activity::Present, presentedFrame);
			hr = m_mainSwapchain.swapchain->Present(syncInterval, 0);
		}
#if defined(LATTE_UWP_DETAILED_PROFILING)
		const uint64 presentElapsedUs = (uint64)std::chrono::duration_cast<std::chrono::microseconds>(
			std::chrono::steady_clock::now() - presentStarted).count();
		++m_uwpPresentCount;
		m_uwpPresentTimeUs += presentElapsedUs;
#endif
		D3D12TraceEvent("present_end", "window=uwp-composite frameIndex={} hr=0x{:08x} syncInterval={} nextFrame={} size={}x{}",
						presentedFrame,
						(uint32)hr,
						syncInterval,
						SUCCEEDED(hr) ? m_mainSwapchain.swapchain->GetCurrentBackBufferIndex() : presentedFrame,
						m_mainSwapchain.size.x,
						m_mainSwapchain.size.y);
		CheckDeviceRemoved(hr, "Present UWP composite");
		if (FAILED(hr))
			throw std::runtime_error("D3D12 UWP composite present failed");
		m_mainSwapchain.frameIndex = m_mainSwapchain.swapchain->GetCurrentBackBufferIndex();
#if defined(CEMU_UWP_D3D12_CORRECTNESS_TELEMETRY)
		++m_correctnessPresentCount;
		if ((m_correctnessPresentCount % 300) == 0)
			LogCorrectnessTelemetry("periodic");
#endif
	}
	ProcessUwpMemoryPressure();
	ReleaseStaleAsyncGamePipelines();
	LogUwpMemoryBreakdown();
	LogDroppedDrawSummary();
#if defined(CEMU_UWP_PERFORMANCE_EXPERIMENT)
	AdvancePerformanceExperiment();
#endif
#if defined(LATTE_UWP_DETAILED_PROFILING)
	LogUwpCpuPerformanceSummary();
	LogUwpCommandSubmitPerformanceSummary();
#endif
	m_uwpCompositeFrameStarted = false;
	m_uwpCompositeFrameRendered = false;
	m_uwpOutputDrawnThisFrame = {};
	m_uwpOutputPendingThisFrame = {};
	return;
	D3D12TraceEvent("swapbuffers_begin", "swapTV={} swapPad={} syncInterval={} tvActive={} tvFrame={} tvSize={}x{} padActive={} padFrame={} padSize={}x{} submittedDraws={}",
					swapTV ? 1 : 0,
					swapDRC ? 1 : 0,
					syncInterval,
					m_mainSwapchain.active ? 1 : 0,
					m_mainSwapchain.frameIndex,
					m_mainSwapchain.size.x,
					m_mainSwapchain.size.y,
					m_padSwapchain.active ? 1 : 0,
					m_padSwapchain.frameIndex,
					m_padSwapchain.size.x,
					m_padSwapchain.size.y,
					(unsigned long long)m_gameDrawCallsSubmitted);
	if (swapTV && m_mainSwapchain.active)
	{
		const uint32 presentedFrame = m_mainSwapchain.frameIndex;
		D3D12TraceEvent("present_begin", "window=tv frameIndex={} syncInterval={} size={}x{} backBuffer={} state=0x{:08x} rtvCpu=0x{:016x}",
						presentedFrame,
						syncInterval,
						m_mainSwapchain.size.x,
						m_mainSwapchain.size.y,
						(const void*)m_mainSwapchain.backBuffers[presentedFrame].Get(),
						(uint32)m_mainSwapchain.states[presentedFrame],
						(unsigned long long)m_mainSwapchain.rtvHandles[presentedFrame].ptr);
		const HRESULT hr = m_mainSwapchain.swapchain->Present(syncInterval, 0);
		D3D12TraceEvent("present_end", "window=tv frameIndex={} hr=0x{:08x} syncInterval={} nextFrame={} size={}x{}",
						presentedFrame,
						(uint32)hr,
						syncInterval,
						SUCCEEDED(hr) ? m_mainSwapchain.swapchain->GetCurrentBackBufferIndex() : presentedFrame,
						m_mainSwapchain.size.x,
						m_mainSwapchain.size.y);
		CheckDeviceRemoved(hr, "Present TV");
		if (FAILED(hr))
			throw std::runtime_error("D3D12 TV present failed");
		m_mainSwapchain.frameIndex = m_mainSwapchain.swapchain->GetCurrentBackBufferIndex();
	}
	if (swapDRC && m_padSwapchain.active)
	{
		const uint32 presentedFrame = m_padSwapchain.frameIndex;
		D3D12TraceEvent("present_begin", "window=pad frameIndex={} syncInterval={} size={}x{} backBuffer={} state=0x{:08x} rtvCpu=0x{:016x}",
						presentedFrame,
						syncInterval,
						m_padSwapchain.size.x,
						m_padSwapchain.size.y,
						(const void*)m_padSwapchain.backBuffers[presentedFrame].Get(),
						(uint32)m_padSwapchain.states[presentedFrame],
						(unsigned long long)m_padSwapchain.rtvHandles[presentedFrame].ptr);
		const HRESULT hr = m_padSwapchain.swapchain->Present(syncInterval, 0);
		D3D12TraceEvent("present_end", "window=pad frameIndex={} hr=0x{:08x} syncInterval={} nextFrame={} size={}x{}",
						presentedFrame,
						(uint32)hr,
						syncInterval,
						SUCCEEDED(hr) ? m_padSwapchain.swapchain->GetCurrentBackBufferIndex() : presentedFrame,
						m_padSwapchain.size.x,
						m_padSwapchain.size.y);
		CheckDeviceRemoved(hr, "Present pad");
		if (FAILED(hr))
			throw std::runtime_error("D3D12 pad present failed");
		m_padSwapchain.frameIndex = m_padSwapchain.swapchain->GetCurrentBackBufferIndex();
	}
}

bool D3D12Renderer::BeginFrame(bool mainWindow)
{
	D3D12_EXCLUSIVE_RECORDING();
	if (!mainWindow)
		return m_mainSwapchain.active && UwpLayoutShowsGamePad();
	return GetSwapchain(mainWindow).active;
}

void D3D12Renderer::HandleScreenshotRequest(LatteTextureView* texView, bool padView)
{
	D3D12_EXCLUSIVE_RECORDING();
	if (!m_screenshot_requested && m_screenshot_state == ScreenshotState::None)
		return;

	if (IsPadWindowActive())
	{
		if (m_screenshot_state == ScreenshotState::Main && padView)
			return;
		if (m_screenshot_state == ScreenshotState::Pad && !padView)
			return;

		if (m_screenshot_state == ScreenshotState::None)
			m_screenshot_state = padView ? ScreenshotState::Main : ScreenshotState::Pad;
		else
			m_screenshot_state = ScreenshotState::None;
	}
	else
	{
		m_screenshot_state = ScreenshotState::None;
	}

	if (!texView || !texView->baseTexture)
		return;
	if (texView->firstMip != 0)
	{
		cemuLog_log(LogType::Force, "Failed to capture D3D12 screenshot: capturing non-zero mip is not supported");
		return;
	}

	auto* textureView = static_cast<LatteTextureViewD3D12*>(texView);
	auto* baseTexture = textureView->GetBaseImage();
	if (!baseTexture || !baseTexture->GetResource())
		return;

	sint32 width = 0;
	sint32 height = 0;
	baseTexture->GetEffectiveSize(width, height, texView->firstMip);
	if (width <= 0 || height <= 0)
		return;

	std::unique_ptr<LatteTextureReadbackInfo> readback(CreateHostTextureReadback(texView));
	if (!readback)
	{
		cemuLog_log(LogType::Force, "D3D12 screenshot request failed: texture readback could not be created");
		return;
	}

	readback->StartTransfer();
	readback->ForceFinish();
	const uint8* pixels = readback->GetData();
	if (!pixels)
	{
		cemuLog_log(LogType::Force, "D3D12 screenshot request failed: texture readback returned no data");
		return;
	}

	DXGI_FORMAT format = textureView->GetViewFormat();
	if (format == DXGI_FORMAT_UNKNOWN)
		format = baseTexture->GetFormatInfo().srvFormat;
	if (format == DXGI_FORMAT_UNKNOWN)
		format = baseTexture->GetResourceFormat();

	const size_t pixelCount = (size_t)width * (size_t)height;
	std::vector<uint8> rgbData;
	rgbData.reserve(pixelCount * 3);

	bool formatValid = true;
	switch (format)
	{
	case DXGI_FORMAT_R8G8B8A8_UNORM:
	case DXGI_FORMAT_R8G8B8A8_SNORM:
		for (size_t i = 0; i < pixelCount; i++)
		{
			const uint8* src = pixels + i * 4;
			rgbData.emplace_back(src[0]);
			rgbData.emplace_back(src[1]);
			rgbData.emplace_back(src[2]);
		}
		break;
	case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
		for (size_t i = 0; i < pixelCount; i++)
		{
			const uint8* src = pixels + i * 4;
			rgbData.emplace_back(SRGBComponentToRGB(src[0]));
			rgbData.emplace_back(SRGBComponentToRGB(src[1]));
			rgbData.emplace_back(SRGBComponentToRGB(src[2]));
		}
		break;
	case DXGI_FORMAT_B8G8R8A8_UNORM:
		for (size_t i = 0; i < pixelCount; i++)
		{
			const uint8* src = pixels + i * 4;
			rgbData.emplace_back(src[2]);
			rgbData.emplace_back(src[1]);
			rgbData.emplace_back(src[0]);
		}
		break;
	case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
		for (size_t i = 0; i < pixelCount; i++)
		{
			const uint8* src = pixels + i * 4;
			rgbData.emplace_back(SRGBComponentToRGB(src[2]));
			rgbData.emplace_back(SRGBComponentToRGB(src[1]));
			rgbData.emplace_back(SRGBComponentToRGB(src[0]));
		}
		break;
	case DXGI_FORMAT_R10G10B10A2_UNORM:
		for (size_t i = 0; i < pixelCount; i++)
		{
			uint32 packed = 0;
			memcpy(&packed, pixels + i * sizeof(uint32), sizeof(uint32));
			rgbData.emplace_back((uint8)((((packed >> 0) & 0x3FFu) * 255u + 511u) / 1023u));
			rgbData.emplace_back((uint8)((((packed >> 10) & 0x3FFu) * 255u + 511u) / 1023u));
			rgbData.emplace_back((uint8)((((packed >> 20) & 0x3FFu) * 255u + 511u) / 1023u));
		}
		break;
	case DXGI_FORMAT_R16G16B16A16_UNORM:
		for (size_t i = 0; i < pixelCount; i++)
		{
			const uint16* src = reinterpret_cast<const uint16*>(pixels + i * 8);
			rgbData.emplace_back((uint8)((src[0] * 255u + 32767u) / 65535u));
			rgbData.emplace_back((uint8)((src[1] * 255u + 32767u) / 65535u));
			rgbData.emplace_back((uint8)((src[2] * 255u + 32767u) / 65535u));
		}
		break;
	default:
		cemuLog_log(LogType::Force, "Unsupported D3D12 screenshot texture format {}", (uint32)format);
		formatValid = false;
		break;
	}

	if (formatValid)
		SaveScreenshot(rgbData, width, height, !padView);
}

void D3D12Renderer::DrawBackbufferQuad(LatteTextureView* texView, RendererOutputShader* shader, bool useLinearTexFilter, sint32 imageX, sint32 imageY, sint32 imageWidth, sint32 imageHeight, bool padView, bool clearBackground)
{
	D3D12_EXCLUSIVE_RECORDING();
	if (!texView || !shader)
	{
		if (m_uwpDrawingCachedOutput)
			m_uwpCachedOutputs[padView ? 1 : 0].valid = false;
		static uint32 s_missingOutputInputLogCount = 0;
		if (D3D12RuntimeLogCapped(s_missingOutputInputLogCount, "DrawBackbufferQuad missing input", 16))
			cemuLog_log(LogType::Force, "D3D12: DrawBackbufferQuad skipped, texView={} shader={}.", texView != nullptr, shader != nullptr);
		D3D12TraceEvent("output_skip", "reason=missing_input pad={} texView={} shader={} dstXY={},{} dstSize={}x{} linear={} clear={}",
						padView ? 1 : 0,
						(const void*)texView,
						(const void*)shader,
						imageX,
						imageY,
						imageWidth,
						imageHeight,
						useLinearTexFilter ? 1 : 0,
						clearBackground ? 1 : 0);
		return;
	}

	ProcessUwpStreamCapture(texView, padView);

	auto& chain =
		GetSwapchain(true);
	if (!chain.active)
	{
		static uint32 s_inactiveOutputSwapchainLogCount = 0;
		if (D3D12RuntimeLogCapped(s_inactiveOutputSwapchainLogCount, "DrawBackbufferQuad inactive swapchain", 16))
			cemuLog_log(LogType::Force, "D3D12: DrawBackbufferQuad skipped, swapchain inactive for {} view.", padView ? "pad" : "TV");
		D3D12TraceEvent("output_skip", "reason=inactive_swapchain pad={} texView={} shader={} chainSize={}x{} dstXY={},{} dstSize={}x{}",
						padView ? 1 : 0,
						(const void*)texView,
						(const void*)shader,
						chain.size.x,
						chain.size.y,
						imageX,
						imageY,
						imageWidth,
						imageHeight);
		return;
	}

	const auto uwpOutputRect = ResolveUwpOutputRect(padView, chain.size);
	if (uwpOutputRect.visible)
	{
		imageX = uwpOutputRect.x;
		imageY = uwpOutputRect.y;
		imageWidth = uwpOutputRect.w;
		imageHeight = uwpOutputRect.h;
		// a full-screen output covers the background, so skip the clear and underlay draw
		if (!m_uwpCompositeFrameStarted &&
			uwpOutputRect.x <= 0 && uwpOutputRect.y <= 0 &&
			uwpOutputRect.x + uwpOutputRect.w >= chain.size.x &&
			uwpOutputRect.y + uwpOutputRect.h >= chain.size.y &&
			!ResolveUwpOutputRect(!padView, chain.size).visible)
		{
			m_uwpCompositeFrameStarted = true;
		}
		BeginUwpCompositeFrame(chain);
	}

	auto* textureView = static_cast<LatteTextureViewD3D12*>(texView);
	auto* texture = textureView ? textureView->GetBaseImage() : nullptr;
	if (!texture || !texture->GetResource() || !textureView->GetSRV().IsValid())
	{
		if (m_uwpDrawingCachedOutput)
			m_uwpCachedOutputs[padView ? 1 : 0].valid = false;
		static uint32 s_invalidOutputTextureLogCount = 0;
		if (D3D12RuntimeLogCapped(s_invalidOutputTextureLogCount, "DrawBackbufferQuad invalid texture", 32))
		{
			cemuLog_log(LogType::Force, "D3D12: DrawBackbufferQuad skipped, texture={} resource={} srv={}.",
						texture != nullptr,
						texture && texture->GetResource(),
						textureView && textureView->GetSRV().IsValid());
		}
		D3D12TraceEvent("output_skip", "reason=invalid_source pad={} texView={} texture={} resource={} srvValid={} dstXY={},{} dstSize={}x{} chain={}x{}",
						padView ? 1 : 0,
						(const void*)textureView,
						(const void*)texture,
						texture ? (const void*)texture->GetResource() : nullptr,
						textureView && textureView->GetSRV().IsValid() ? 1 : 0,
						imageX,
						imageY,
						imageWidth,
						imageHeight,
						chain.size.x,
						chain.size.y);
		return;
	}

	if (!uwpOutputRect.visible)
	{
		if (!m_uwpDrawingCachedOutput)
			m_uwpCachedOutputs[padView ? 1 : 0].valid = false;
		D3D12TraceEvent("output_skip", "reason=uwp_layout_hidden pad={} layout={} swap={} chainSize={}x{} snapshotRetained=0",
						padView ? 1 : 0,
						ResolveUwpScreenLayout(),
						GetConfig().uwp_display.swap_screens.GetValue() ? 1 : 0,
						chain.size.x,
						chain.size.y);
		return;
	}

	ID3D12PipelineState* pipeline = GetOutputPipeline(shader, padView);
	if (!pipeline)
	{
		if (m_uwpDrawingCachedOutput)
			m_uwpCachedOutputs[padView ? 1 : 0].valid = false;
		static uint32 s_missingOutputPsoLogCount = 0;
		if (D3D12RuntimeLogCapped(s_missingOutputPsoLogCount, "DrawBackbufferQuad missing PSO", 16))
			cemuLog_log(LogType::Force, "D3D12: DrawBackbufferQuad skipped, output PSO unavailable for {} view.", padView ? "pad" : "TV");
		D3D12TraceEvent("output_skip", "reason=no_output_pso pad={} src=0x{:08x} srcFmt=0x{:04x} srcDxgi={} texView={} shader={} dstXY={},{} dstSize={}x{} chain={}x{}",
						padView ? 1 : 0,
						texture->physAddress,
						(uint32)texture->format,
						(uint32)textureView->GetViewFormat(),
						(const void*)textureView,
						(const void*)shader,
						imageX,
						imageY,
						imageWidth,
						imageHeight,
						chain.size.x,
						chain.size.y);
		return;
	}

	if (!m_uwpDrawingCachedOutput)
	{
		const auto peerRect = ResolveUwpOutputRect(!padView, chain.size);
		if (peerRect.visible)
		{
			StoreUwpOutputSnapshot(padView, textureView, texture, shader, useLinearTexFilter);
			// a larger peer is composited underneath, which is only needed once per frame
			if (GetUwpOutputArea(peerRect) > GetUwpOutputArea(uwpOutputRect) && !m_uwpOutputDrawnThisFrame[padView ? 0 : 1])
				DrawUwpCachedOutput(!padView);
		}
		else
		{
			m_uwpCachedOutputs[0].valid = false;
			m_uwpCachedOutputs[1].valid = false;
		}
	}

	static uint32 s_outputDrawLogCount = 0;
	if (D3D12RuntimeLogCapped(s_outputDrawLogCount, "DrawBackbufferQuad", 96))
	{
		cemuLog_log(LogType::Force,
					"D3D12: DrawBackbufferQuad {} src=0x{:08x} fmt=0x{:04x} size={}x{} mip={} slice={} dst=({}, {}) {}x{} chain={}x{} linear={} clear={}.",
					padView ? "pad" : "TV",
					texture->physAddress,
					(uint32)texture->format,
					texture->width,
					texture->height,
					texView->firstMip,
					texView->firstSlice,
					imageX,
					imageY,
					imageWidth,
					imageHeight,
					chain.size.x,
					chain.size.y,
					useLinearTexFilter,
					clearBackground);
	}

	LogOutputTextureStats(textureView, padView);
	if (D3D12ShouldTrace())
	{
		TraceTextureSnapshot("output_source_before", "before_output_blit", textureView, padView,
			fmt::format("window={} dstXY={},{} dstSize={}x{} chain={}x{} frameIndex={} linear={} clear={} shader={} pipeline={}",
				padView ? "pad" : "tv", imageX, imageY, imageWidth, imageHeight, chain.size.x, chain.size.y,
				chain.frameIndex, useLinearTexFilter ? 1 : 0, clearBackground ? 1 : 0,
				(const void*)shader, (const void*)pipeline));
	}

	if (!m_commandListOpen)
		ResetCommandList();

	auto uniforms = shader->FillUniformBlockBuffer(*texView, { imageWidth, imageHeight }, padView);
	auto uniformUpload = m_memoryManager->AllocateUpload(sizeof(uniforms), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT, m_nextFenceValue);
	if (!uniformUpload.IsValid())
		throw std::runtime_error("Failed to allocate D3D12 output uniform upload memory");
	memcpy(uniformUpload.cpuPtr, &uniforms, sizeof(uniforms));

	// a two-pass shader (FSR) upscales into an output-sized target first and then reads that
	D3D12DescriptorManager::Allocation sourceSrv = textureView->GetSRV();
	if (RendererOutputShader* prePass = shader->GetPrePass())
	{
		TransitionTextureView(textureView, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		const auto prePassSrv = DrawOutputPrePass(prePass, sourceSrv, imageWidth, imageHeight, padView,
			uniformUpload.resource->GetGPUVirtualAddress() + uniformUpload.offset);
		if (prePassSrv.IsValid())
			sourceSrv = prePassSrv;
		else if (auto* copyPipeline = GetOutputPipeline(shader == RendererOutputShader::s_fsr_shader_ud ?
			RendererOutputShader::s_copy_shader_ud : RendererOutputShader::s_copy_shader, padView))
			pipeline = copyPipeline;
	}

	const auto outputDescriptorStats = m_descriptorManager->GetStats();
	const bool hasCachedOutputSampler =
		(useLinearTexFilter ? m_shaderVisibleOutputLinearSampler.IsValid() : m_shaderVisibleOutputPointSampler.IsValid()) &&
		m_shaderVisibleOutputSamplerGeneration == outputDescriptorStats.shaderVisibleSamplerRollovers;
	EnsureShaderVisibleDescriptorSpace(1, hasCachedOutputSampler ? 0 : 1);
	auto srv = CopyToShaderVisibleChecked(sourceSrv, "output");
	auto sampler = GetShaderVisibleOutputSampler(useLinearTexFilter);
	BindShaderVisibleHeaps();

	chain.frameIndex = chain.swapchain->GetCurrentBackBufferIndex();
	ID3D12Resource* backBuffer = chain.backBuffers[chain.frameIndex].Get();
	D3D12TraceEvent("output_target_before", "window={} frameIndex={} chainSize={}x{} backBuffer={} state=0x{:08x} rtvCpu=0x{:016x} src=0x{:08x} srcFmt=0x{:04x} srcDxgi={} srvGpu=0x{:016x} samplerGpu=0x{:016x} uniformGpu=0x{:016x} pipeline={}",
					padView ? "pad" : "tv",
					chain.frameIndex,
					chain.size.x,
					chain.size.y,
					(const void*)backBuffer,
					(uint32)chain.states[chain.frameIndex],
					(unsigned long long)chain.rtvHandles[chain.frameIndex].ptr,
					texture->physAddress,
					(uint32)texture->format,
					(uint32)textureView->GetViewFormat(),
					(unsigned long long)srv.gpu.ptr,
					(unsigned long long)sampler.gpu.ptr,
					(unsigned long long)(uniformUpload.resource->GetGPUVirtualAddress() + uniformUpload.offset),
					(const void*)pipeline);
	Transition(backBuffer, chain.states[chain.frameIndex], D3D12_RESOURCE_STATE_RENDER_TARGET);
	TransitionTextureView(textureView, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

	auto* commandList = GetCurrentCommandSlot().commandList.Get();
	SetD3D12PipelineState(pipeline);
	SetD3D12GraphicsRootSignature(m_outputRootSignature.Get());
	commandList->SetGraphicsRootDescriptorTable(0, srv.gpu);
	commandList->SetGraphicsRootDescriptorTable(1, sampler.gpu);
	commandList->SetGraphicsRootConstantBufferView(2, uniformUpload.resource->GetGPUVirtualAddress() + uniformUpload.offset);
	commandList->SetGraphicsRootConstantBufferView(3, uniformUpload.resource->GetGPUVirtualAddress() + uniformUpload.offset);
	SetD3D12RenderTargets(1, &chain.rtvHandles[chain.frameIndex], nullptr);

	D3D12_VIEWPORT viewport{};
	viewport.TopLeftX = (float)imageX;
	viewport.TopLeftY = (float)imageY;
	viewport.Width = (float)std::max<sint32>(imageWidth, 1);
	viewport.Height = (float)std::max<sint32>(imageHeight, 1);
	viewport.MinDepth = 0.0f;
	viewport.MaxDepth = 1.0f;
	D3D12_RECT scissor{};
	scissor.left = 0;
	scissor.top = 0;
	scissor.right = std::max<sint32>(chain.size.x, 1);
	scissor.bottom = std::max<sint32>(chain.size.y, 1);
	SetD3D12Viewport(viewport);
	SetD3D12Scissor(scissor);
	SetD3D12PrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	commandList->DrawInstanced(6, 1, 0, 0);
	m_gpuWatchdog.RecordOperation(D3D12GpuWatchdog::Operation::OutputDraw);
	D3D12TraceEvent("output_draw", "window={} frameIndex={} backBuffer={} rtvCpu=0x{:016x} src=0x{:08x} srcFmt=0x{:04x} srcDxgi={} srcView={} srvGpu=0x{:016x} samplerGpu=0x{:016x} uniformGpu=0x{:016x} vertices=6 dstXY={},{} dstSize={}x{} chain={}x{} viewport={},{}:{}x{} scissor={},{}-{},{} linear={} clear={} shader={} pipeline={}",
					padView ? "pad" : "tv",
					chain.frameIndex,
					(const void*)backBuffer,
					(unsigned long long)chain.rtvHandles[chain.frameIndex].ptr,
					texture->physAddress,
					(uint32)texture->format,
					(uint32)textureView->GetViewFormat(),
					(const void*)textureView,
					(unsigned long long)srv.gpu.ptr,
					(unsigned long long)sampler.gpu.ptr,
					(unsigned long long)(uniformUpload.resource->GetGPUVirtualAddress() + uniformUpload.offset),
					imageX,
					imageY,
					imageWidth,
					imageHeight,
					chain.size.x,
					chain.size.y,
					viewport.TopLeftX,
					viewport.TopLeftY,
					viewport.Width,
					viewport.Height,
					scissor.left,
					scissor.top,
					scissor.right,
					scissor.bottom,
					useLinearTexFilter ? 1 : 0,
					clearBackground ? 1 : 0,
					(const void*)shader,
					(const void*)pipeline);
	Transition(backBuffer, chain.states[chain.frameIndex], D3D12_RESOURCE_STATE_PRESENT);
	m_uwpCompositeFrameRendered = true;
	m_uwpOutputDrawnThisFrame[padView ? 1 : 0] = true;
	if (!m_uwpDrawingCachedOutput)
	{
		const auto peerRect = ResolveUwpOutputRect(!padView, chain.size);
		// a smaller peer stays on top; redraw it only if it is missing or this draw covered it
		if (peerRect.visible && GetUwpOutputArea(peerRect) <= GetUwpOutputArea(uwpOutputRect))
		{
			if (UwpOutputRectsOverlap(peerRect, uwpOutputRect))
				DrawUwpCachedOutput(!padView);
			else if (!m_uwpOutputDrawnThisFrame[padView ? 0 : 1])
				m_uwpOutputPendingThisFrame[padView ? 0 : 1] = true;
		}
	}
}

#if defined(CEMU_UWP)
void D3D12Renderer::ProcessUwpStreamCapture(LatteTextureView* textureView, bool padView)
{
	for (auto& pending : m_uwpStreamReadbacks)
	{
		if (!pending.transfer || !pending.transfer->IsFinished())
			continue;
		uint8* data = pending.transfer->GetData();
		const uint32 size = pending.transfer->GetImageSize();
		if (data && size && pending.width && pending.height && pending.rowBytes)
		{
			std::vector<uint8> pixels = CemuUwp::MediaStreamer::AcquireVideoFrameBuffer(size);
			std::memcpy(pixels.data(), data, size);
			CemuUwp::MediaStreamer::SubmitVideoFrame(pending.padView, pending.width,
				pending.height, pending.rowBytes, (uint32)pending.format, std::move(pixels));
		}
		pending = {};
	}

	auto& capture = m_uwpStreamReadbacks[padView ? 1 : 0];
	if (capture.transfer || !CemuUwp::MediaStreamer::RequestVideoCapture(padView))
		return;
	if (!textureView || !textureView->baseTexture || textureView->firstMip != 0)
		return;
	auto* d3dView = static_cast<LatteTextureViewD3D12*>(textureView);
	auto* texture = d3dView->GetBaseImage();
	if (!texture || !texture->GetResource())
		return;

	sint32 width = 0;
	sint32 height = 0;
	texture->GetEffectiveSize(width, height, textureView->firstMip);
	if (width <= 0 || height <= 0)
		return;
	DXGI_FORMAT format = d3dView->GetViewFormat();
	if (format == DXGI_FORMAT_UNKNOWN)
		format = texture->GetFormatInfo().srvFormat;
	if (format == DXGI_FORMAT_UNKNOWN)
		format = texture->GetResourceFormat();

	auto transfer = std::unique_ptr<LatteTextureReadbackInfoD3D12>(CreateHostTextureReadback(textureView));
	if (!transfer)
		return;
	const uint32 rowBytes = transfer->GetRowSizeInBytes();
	transfer->StartTransfer();
	capture.transfer = std::move(transfer);
	capture.width = (uint32)width;
	capture.height = (uint32)height;
	capture.rowBytes = rowBytes;
	capture.format = format;
	capture.padView = padView;
}
#endif

void D3D12Renderer::DrawUwpConfiguredUnderlay(SwapchainState& chain)
{
	const std::string path = GetUwpUnderlayPath();
	if (path.empty())
	{
		if (m_uwpUnderlayTexture.id)
			DeleteTexture(m_uwpUnderlayTexture.id);
		m_uwpUnderlayTexture = {};
		return;
	}

	if (m_uwpUnderlayTexture.path != path)
	{
		if (m_uwpUnderlayTexture.id)
			DeleteTexture(m_uwpUnderlayTexture.id);
		m_uwpUnderlayTexture = {};
		m_uwpUnderlayTexture.path = path;

		UwpDecodedImage image{};
		if (!DecodeUwpPngFile(_utf8ToPath(path), image))
		{
			m_uwpUnderlayTexture.failed = true;
			cemuLog_log(LogType::Force, "D3D12 UWP: failed to decode underlay PNG {}", path);
			return;
		}

		m_uwpUnderlayTexture.id = GenerateTexture(image.rgba, { image.width, image.height });
		m_uwpUnderlayTexture.size = { image.width, image.height };
		m_uwpUnderlayTexture.failed = m_uwpUnderlayTexture.id == nullptr;
		if (m_uwpUnderlayTexture.failed)
			cemuLog_log(LogType::Force, "D3D12 UWP: failed to upload underlay texture {}", path);
	}

	if (!m_uwpUnderlayTexture.id || m_uwpUnderlayTexture.failed || !m_imguiInitialized)
		return;

	if (!ImguiBegin(true))
		return;
	ImDrawList* draw = ImGui::GetBackgroundDrawList();
	draw->AddImage(m_uwpUnderlayTexture.id, ImVec2(0.0f, 0.0f), ImVec2((float)chain.size.x, (float)chain.size.y));
	ImguiEnd();
}

void D3D12Renderer::ResetUwpOutputSnapshots()
{
	ReleaseUwpCachedOutputs();
	m_uwpCachedOutputTitleId = 0;
	m_uwpCompositeFrameStarted = false;
	m_uwpCompositeFrameRendered = false;
	m_uwpOutputDrawnThisFrame = {};
	m_uwpOutputPendingThisFrame = {};
	m_uwpDrawingCachedOutput = false;
}

void D3D12Renderer::ReleaseUwpCachedOutputs()
{
	for (auto& output : m_uwpCachedOutputs)
	{
		auto persistentSrv = output.srv;
		ReleaseResourceDeferred(output.texture);
		output = {};
		output.srv = persistentSrv;
		output.desc.Format = DXGI_FORMAT_UNKNOWN;
		output.viewFormat = DXGI_FORMAT_UNKNOWN;
	}
}

bool D3D12Renderer::StoreUwpOutputSnapshot(bool padView, LatteTextureViewD3D12* textureView, LatteTextureD3D12* texture, RendererOutputShader* shader, bool useLinearTexFilter)
{
	if (!textureView || !texture || !shader || !m_memoryManager || !m_descriptorManager || !m_device || !texture->GetResource())
		return false;
	const uint64 titleId = GetUwpTitleId();
	if (m_uwpCachedOutputTitleId != titleId)
	{
		ReleaseUwpCachedOutputs();
		m_uwpCachedOutputTitleId = titleId;
		D3D12TraceEvent("uwp_output_snapshots_invalidate", "title=0x{:016x}", titleId);
	}

	DXGI_FORMAT viewFormat = textureView->GetViewFormat();
	if (viewFormat == DXGI_FORMAT_UNKNOWN)
		viewFormat = texture->GetFormatInfo().srvFormat;
	if (viewFormat == DXGI_FORMAT_UNKNOWN)
		viewFormat = texture->GetResourceFormat();
	if (viewFormat == DXGI_FORMAT_UNKNOWN)
		return false;

	const auto& sourceDesc = texture->GetDesc();
	if (sourceDesc.SampleDesc.Count != 1)
	{
		cemuLog_logOnce(LogType::Force, "D3D12 UWP: multisampled output snapshots require an explicit resolve and are not cached.");
		return false;
	}
	const uint32 sourceMipLevels = std::max<uint32>(sourceDesc.MipLevels, 1);
	const uint32 firstMip = std::min<uint32>((uint32)std::max<sint32>(textureView->firstMip, 0), sourceMipLevels - 1);
	const uint32 sourceWidth = std::max<uint32>((uint32)sourceDesc.Width >> firstMip, 1);
	const uint32 sourceHeight = std::max<uint32>(sourceDesc.Height >> firstMip, 1);
	const bool sourceIs3D = sourceDesc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D;
	const uint32 sourceArraySize = sourceIs3D ? 1u : std::max<uint32>(sourceDesc.DepthOrArraySize, 1);
	const uint32 firstSlice = sourceIs3D ? 0u : std::min<uint32>((uint32)std::max<sint32>(textureView->firstSlice, 0), sourceArraySize - 1);
	const uint32 sourceSubresource = texture->GetSubresourceIndex(sourceIs3D ? 0 : firstSlice, firstMip);

	// the output shader samples Texture2D; snapshot only the selected mip/slice
	D3D12_RESOURCE_DESC snapshotDesc{};
	snapshotDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	snapshotDesc.Width = sourceWidth;
	snapshotDesc.Height = sourceHeight;
	snapshotDesc.DepthOrArraySize = 1;
	snapshotDesc.MipLevels = 1;
	snapshotDesc.Format = sourceDesc.Format;
	snapshotDesc.SampleDesc = {1, 0};
	snapshotDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	snapshotDesc.Flags = D3D12_RESOURCE_FLAG_NONE;
	D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = textureView->BuildSrvDesc();
	if (srvDesc.Format == DXGI_FORMAT_UNKNOWN)
		return false;
	srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	srvDesc.Texture2D.MostDetailedMip = 0;
	srvDesc.Texture2D.MipLevels = 1;
	srvDesc.Texture2D.PlaneSlice = 0;
	srvDesc.Texture2D.ResourceMinLODClamp = 0.0f;

	auto& output = m_uwpCachedOutputs[padView ? 1 : 0];
	const bool needsTexture =
		!output.texture.IsValid() ||
		output.desc.Dimension != snapshotDesc.Dimension ||
		output.desc.Width != snapshotDesc.Width ||
		output.desc.Height != snapshotDesc.Height ||
		output.desc.DepthOrArraySize != snapshotDesc.DepthOrArraySize ||
		output.desc.MipLevels != snapshotDesc.MipLevels ||
		output.desc.SampleDesc.Count != snapshotDesc.SampleDesc.Count ||
		output.desc.SampleDesc.Quality != snapshotDesc.SampleDesc.Quality ||
		output.desc.Format != snapshotDesc.Format ||
		output.desc.Flags != snapshotDesc.Flags ||
		output.viewFormat != viewFormat;

	if (needsTexture)
	{
		ReleaseResourceDeferred(output.texture);
		const bool acquiredPooledTexture = TryAcquireScratchTexture(snapshotDesc, output.texture);
		const bool createdTexture = acquiredPooledTexture ||
									CreateTextureWithRecovery(snapshotDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, output.texture, padView ? "uwp pad output snapshot" : "uwp TV output snapshot");
		if (!createdTexture)
		{
			output.valid = false;
			cemuLog_log(LogType::Force,
						"D3D12 UWP: failed to create {} output snapshot {}x{} resourceFormat={} viewFormat={}.",
						padView ? "pad" : "TV",
						(uint32)snapshotDesc.Width,
						(uint32)snapshotDesc.Height,
						(uint32)snapshotDesc.Format,
						(uint32)viewFormat);
			return false;
		}

		if (!output.srv.IsValid())
			output.srv = m_descriptorManager->AllocatePersistent(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

		output.desc = snapshotDesc;
		output.viewFormat = viewFormat;
	}

	if (!output.srv.IsValid())
		output.srv = m_descriptorManager->AllocatePersistent(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	const bool needsSrvUpdate =
		needsTexture ||
		std::memcmp(&output.srvDesc, &srvDesc, sizeof(srvDesc)) != 0;
	if (needsSrvUpdate)
	{
		CreateShaderResourceViewChecked(output.texture.resource.Get(), &srvDesc, output.srv.cpu, "output snapshot", false);
		output.srvDesc = srvDesc;
		m_uwpSnapshotSrvUpdates++;
	}
	else
		m_uwpSnapshotSrvSkips++;

	if (!output.texture.IsValid() || !output.srv.IsValid())
	{
		output.valid = false;
		return false;
	}

	if (!m_commandListOpen)
		ResetCommandList();

	const D3D12_RESOURCE_STATES previousSourceState = texture->GetSubresourceState(sourceSubresource);
	TransitionTextureSubresource(texture, sourceSubresource, D3D12_RESOURCE_STATE_COPY_SOURCE);
	Transition(output.texture.resource.Get(), output.texture.state, D3D12_RESOURCE_STATE_COPY_DEST);

	D3D12_TEXTURE_COPY_LOCATION sourceLocation{};
	sourceLocation.pResource = texture->GetResource();
	sourceLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	sourceLocation.SubresourceIndex = sourceSubresource;
	D3D12_TEXTURE_COPY_LOCATION destinationLocation{};
	destinationLocation.pResource = output.texture.resource.Get();
	destinationLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	destinationLocation.SubresourceIndex = 0;
	if (sourceIs3D)
	{
		const uint32 sourceDepth = std::max<uint32>((uint32)sourceDesc.DepthOrArraySize >> firstMip, 1);
		const uint32 sourceW = std::min<uint32>((uint32)std::max<sint32>(textureView->firstSlice, 0), sourceDepth - 1);
		const D3D12_BOX sourceBox{0, 0, sourceW, sourceWidth, sourceHeight, sourceW + 1};
		GetCurrentCommandSlot().commandList->CopyTextureRegion(&destinationLocation, 0, 0, 0, &sourceLocation, &sourceBox);
	}
	else
		GetCurrentCommandSlot().commandList->CopyTextureRegion(&destinationLocation, 0, 0, 0, &sourceLocation, nullptr);
	Transition(output.texture.resource.Get(), output.texture.state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	TransitionTextureSubresource(texture, sourceSubresource, previousSourceState);

	const auto snapshotUniforms = shader->FillUniformBlockBuffer(
		*textureView, {static_cast<sint32>(sourceWidth), static_cast<sint32>(sourceHeight)}, padView);
	output.shader = shader;
	output.useLinearTexFilter = useLinearTexFilter;
	output.drcColorTransfer = RendererOutputShader::UsesDRCColorTransfer(padView);
	output.sourceResolution = {
		std::max<sint32>(static_cast<sint32>(snapshotUniforms.textureSrcResolution.x), 1),
		std::max<sint32>(static_cast<sint32>(snapshotUniforms.textureSrcResolution.y), 1),
	};
	output.nativeResolution = {
		std::max<sint32>(static_cast<sint32>(snapshotUniforms.nativeResolution.x), 1),
		std::max<sint32>(static_cast<sint32>(snapshotUniforms.nativeResolution.y), 1),
	};
	output.valid = true;
	D3D12TraceEvent("uwp_output_snapshot_store",
					"window={} snapshot={}x{}x{} copiedMips={} copiedSlices={} sampleCount={} sourceRes={}x{} native={}x{} resourceFormat={} viewFormat={} shader={} linear={}",
					padView ? "pad" : "tv",
					(uint32)snapshotDesc.Width,
					(uint32)snapshotDesc.Height,
					(uint32)snapshotDesc.DepthOrArraySize,
					1,
					1,
					(uint32)snapshotDesc.SampleDesc.Count,
					output.sourceResolution.x,
					output.sourceResolution.y,
					output.nativeResolution.x,
					output.nativeResolution.y,
					(uint32)snapshotDesc.Format,
					(uint32)viewFormat,
					(const void*)shader,
					useLinearTexFilter ? 1 : 0);
	return true;
}

void D3D12Renderer::DrawUwpCachedOutput(bool padView)
{
	if (m_uwpDrawingCachedOutput)
		return;

	auto& output = m_uwpCachedOutputs[padView ? 1 : 0];
	if (!output.valid || !output.texture.IsValid() || !output.srv.IsValid() || !output.shader)
		return;

	auto& chain = GetSwapchain(true);
	if (!chain.active || !chain.swapchain)
		return;

	const auto outputRect = ResolveUwpOutputRect(padView, chain.size);
	if (!outputRect.visible)
		return;

	ID3D12PipelineState* pipeline = GetOutputPipeline(output.shader, padView);
	if (!pipeline)
		return;

	chain.frameIndex = chain.swapchain->GetCurrentBackBufferIndex();
	ID3D12Resource* backBuffer = chain.backBuffers[chain.frameIndex].Get();
	if (!backBuffer)
		return;

	const bool previousDrawingCachedOutput = m_uwpDrawingCachedOutput;
	m_uwpDrawingCachedOutput = true;
	try
	{
		BeginUwpCompositeFrame(chain);
		if (!m_commandListOpen)
			ResetCommandList();

		RendererOutputShader::OutputUniformVariables uniforms{};
		uniforms.textureSrcResolution = { (float)std::max<sint32>(output.sourceResolution.x, 1), (float)std::max<sint32>(output.sourceResolution.y, 1) };
		uniforms.nativeResolution = { (float)std::max<sint32>(output.nativeResolution.x, 1), (float)std::max<sint32>(output.nativeResolution.y, 1) };
		uniforms.outputResolution = { (float)std::max<sint32>(outputRect.w, 1), (float)std::max<sint32>(outputRect.h, 1) };
		uniforms.sharpness = RendererOutputShader::GetFsrSharpness(GetConfig().fsr_sharpness.GetValue());
		RendererOutputShader::FillColorTransfer(uniforms, output.drcColorTransfer);

		auto uniformUpload = m_memoryManager->AllocateUpload(sizeof(uniforms), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT, m_nextFenceValue);
		if (!uniformUpload.IsValid())
			throw std::runtime_error("Failed to allocate D3D12 cached output uniform upload memory");
		memcpy(uniformUpload.cpuPtr, &uniforms, sizeof(uniforms));

		// the snapshot is at the source size, so a two-pass shader (FSR) upscales it first like a live frame
		D3D12DescriptorManager::Allocation sourceSrv = output.srv;
		if (RendererOutputShader* prePass = output.shader->GetPrePass())
		{
			Transition(output.texture.resource.Get(), output.texture.state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
			const auto prePassSrv = DrawOutputPrePass(prePass, sourceSrv, std::max<sint32>(outputRect.w, 1), std::max<sint32>(outputRect.h, 1),
				padView, uniformUpload.resource->GetGPUVirtualAddress() + uniformUpload.offset);
			if (prePassSrv.IsValid())
				sourceSrv = prePassSrv;
			else if (auto* copyPipeline = GetOutputPipeline(output.shader == RendererOutputShader::s_fsr_shader_ud ?
				RendererOutputShader::s_copy_shader_ud : RendererOutputShader::s_copy_shader, padView))
				pipeline = copyPipeline;
		}

		const auto outputDescriptorStats = m_descriptorManager->GetStats();
		const bool hasCachedOutputSampler =
			(output.useLinearTexFilter ? m_shaderVisibleOutputLinearSampler.IsValid() : m_shaderVisibleOutputPointSampler.IsValid()) &&
			m_shaderVisibleOutputSamplerGeneration == outputDescriptorStats.shaderVisibleSamplerRollovers;
		EnsureShaderVisibleDescriptorSpace(1, hasCachedOutputSampler ? 0 : 1);
		auto srv = CopyToShaderVisibleChecked(sourceSrv, "output snapshot");
		auto sampler = GetShaderVisibleOutputSampler(output.useLinearTexFilter);
		BindShaderVisibleHeaps();

		chain.frameIndex = chain.swapchain->GetCurrentBackBufferIndex();
		backBuffer = chain.backBuffers[chain.frameIndex].Get();
		if (backBuffer)
		{
			Transition(backBuffer, chain.states[chain.frameIndex], D3D12_RESOURCE_STATE_RENDER_TARGET);
			Transition(output.texture.resource.Get(), output.texture.state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

			auto* commandList = GetCurrentCommandSlot().commandList.Get();
			SetD3D12PipelineState(pipeline);
			SetD3D12GraphicsRootSignature(m_outputRootSignature.Get());
			commandList->SetGraphicsRootDescriptorTable(0, srv.gpu);
			commandList->SetGraphicsRootDescriptorTable(1, sampler.gpu);
			commandList->SetGraphicsRootConstantBufferView(2, uniformUpload.resource->GetGPUVirtualAddress() + uniformUpload.offset);
			commandList->SetGraphicsRootConstantBufferView(3, uniformUpload.resource->GetGPUVirtualAddress() + uniformUpload.offset);
			SetD3D12RenderTargets(1, &chain.rtvHandles[chain.frameIndex], nullptr);

			D3D12_VIEWPORT viewport{};
			viewport.TopLeftX = (float)outputRect.x;
			viewport.TopLeftY = (float)outputRect.y;
			viewport.Width = (float)std::max<sint32>(outputRect.w, 1);
			viewport.Height = (float)std::max<sint32>(outputRect.h, 1);
			viewport.MinDepth = 0.0f;
			viewport.MaxDepth = 1.0f;
			D3D12_RECT scissor{};
			scissor.left = 0;
			scissor.top = 0;
			scissor.right = std::max<sint32>(chain.size.x, 1);
			scissor.bottom = std::max<sint32>(chain.size.y, 1);
			SetD3D12Viewport(viewport);
			SetD3D12Scissor(scissor);
			SetD3D12PrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			commandList->DrawInstanced(6, 1, 0, 0);
			m_gpuWatchdog.RecordOperation(D3D12GpuWatchdog::Operation::SnapshotDraw);
			D3D12TraceEvent("uwp_output_snapshot_draw",
							"window={} frameIndex={} snapshot={}x{} sourceRes={}x{} native={}x{} dstXY={},{} dstSize={}x{} chain={}x{} srvGpu=0x{:016x} samplerGpu=0x{:016x} uniformGpu=0x{:016x} shader={} pipeline={} linear={}",
							padView ? "pad" : "tv",
							chain.frameIndex,
							(uint32)output.desc.Width,
							(uint32)output.desc.Height,
							output.sourceResolution.x,
							output.sourceResolution.y,
							output.nativeResolution.x,
							output.nativeResolution.y,
							outputRect.x,
							outputRect.y,
							outputRect.w,
							outputRect.h,
							chain.size.x,
							chain.size.y,
							(unsigned long long)srv.gpu.ptr,
							(unsigned long long)sampler.gpu.ptr,
							(unsigned long long)(uniformUpload.resource->GetGPUVirtualAddress() + uniformUpload.offset),
							(const void*)output.shader,
							(const void*)pipeline,
							output.useLinearTexFilter ? 1 : 0);
			Transition(backBuffer, chain.states[chain.frameIndex], D3D12_RESOURCE_STATE_PRESENT);
			m_uwpCompositeFrameRendered = true;
			m_uwpOutputDrawnThisFrame[padView ? 1 : 0] = true;
		}
	}
	catch (...)
	{
		m_uwpDrawingCachedOutput = previousDrawingCachedOutput;
		throw;
	}
	m_uwpDrawingCachedOutput = previousDrawingCachedOutput;
}

void D3D12Renderer::DrawUwpCachedVisibleOutputs()
{
	auto& chain = GetSwapchain(true);
	if (!chain.active || !chain.swapchain)
		return;

	const auto tvRect = ResolveUwpOutputRect(false, chain.size);
	const auto padRect = ResolveUwpOutputRect(true, chain.size);
	const uint64 tvArea = GetUwpOutputArea(tvRect);
	const uint64 padArea = GetUwpOutputArea(padRect);

	if (tvArea >= padArea)
	{
		if (tvRect.visible)
			DrawUwpCachedOutput(false);
		if (padRect.visible)
			DrawUwpCachedOutput(true);
	}
	else
	{
		if (padRect.visible)
			DrawUwpCachedOutput(true);
		if (tvRect.visible)
			DrawUwpCachedOutput(false);
	}
}

void D3D12Renderer::BeginUwpCompositeFrame(SwapchainState& chain)
{
	if (m_uwpCompositeFrameStarted)
		return;

	const uint64 titleId = GetUwpTitleId();
	if (m_uwpCachedOutputTitleId != titleId)
	{
		ReleaseUwpCachedOutputs();
		m_uwpCachedOutputTitleId = titleId;
		D3D12TraceEvent("uwp_output_snapshots_invalidate", "title=0x{:016x}", titleId);
	}

	const float color[4]{ 0.0f, 0.0f, 0.0f, 1.0f };
	RecordClear(chain, color, false);
	DrawUwpConfiguredUnderlay(chain);
	m_uwpCompositeFrameStarted = true;
	m_uwpOutputDrawnThisFrame = {};
	m_uwpOutputPendingThisFrame = {};
}

void D3D12Renderer::Flush(bool waitIdle)
{
	if (waitIdle)
	{
		D3D12_EXCLUSIVE_RECORDING();
		WaitForIdle();
		return;
	}
	D3D12_QUEUE_RECORDING(Flush(false));
	if (waitIdle)
		WaitForIdle();
	else
		ExecuteCurrentCommandList(SubmitFlush);
}

void D3D12Renderer::NotifyLatteCommandProcessorIdle()
{
	D3D12_QUEUE_RECORDING(NotifyLatteCommandProcessorIdle());
	if (m_submitOnIdle)
		Flush(false);
	else
		ProcessFinishedCommandLists();
	if (!CafeSystem::IsTitleRunning())
		PersistGamePipelineLibrary();
}

void D3D12Renderer::NotifyDepthBufferExpanded(MPTR depthBufferMPTR, Latte::E_GX2SURFFMT depthBufferFormat, uint32 width, uint32 height, uint32 pitch, uint32 viewMip, uint32 viewFirstSlice, uint32 viewNumSlices)
{
	D3D12_QUEUE_RECORDING(NotifyDepthBufferExpanded(depthBufferMPTR, depthBufferFormat, width, height, pitch, viewMip, viewFirstSlice, viewNumSlices));
	if (!m_commandListOpen)
		ResetCommandList();

	LatteTextureViewD3D12* activeDepthView = nullptr;
	LatteTextureD3D12* activeDepthTexture = nullptr;
	if (m_activeFBO && m_activeFBO->HasDepthStencilAttachment() && m_activeFBO->depthBuffer.texture)
	{
		activeDepthView = static_cast<LatteTextureViewD3D12*>(m_activeFBO->depthBuffer.texture);
		activeDepthTexture = activeDepthView ? activeDepthView->GetBaseImage() : nullptr;
	}

	const bool matchesActiveDepth =
		activeDepthTexture &&
		(activeDepthTexture->physAddress == depthBufferMPTR ||
		 (activeDepthTexture->texDataPtrLow <= depthBufferMPTR && depthBufferMPTR < activeDepthTexture->texDataPtrHigh));

	if (matchesActiveDepth)
		TransitionTextureAll(activeDepthTexture, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

	D3D12TraceEvent("expand_depth_buffer",
					"addr=0x{:08x} fmt=0x{:04x} size={}x{} pitch={} viewMip={} viewSlice={}+{} matchedActive={} activeAddr=0x{:08x} activeSize={}x{} activeFmt=0x{:04x} fbo=0x{:016x}",
					depthBufferMPTR,
					(uint32)depthBufferFormat,
					width,
					height,
					pitch,
					viewMip,
					viewFirstSlice,
					viewNumSlices,
					matchesActiveDepth ? 1 : 0,
					activeDepthTexture ? activeDepthTexture->physAddress : 0,
					activeDepthTexture ? activeDepthTexture->width : 0,
					activeDepthTexture ? activeDepthTexture->height : 0,
					activeDepthTexture ? (uint32)activeDepthTexture->format : 0,
					m_activeFBO ? m_activeFBO->key : 0);

	if (matchesActiveDepth && D3D12ShouldTrace())
		TraceTextureSnapshot("expand_depth_after", "after_expand_depth", activeDepthView, false,
							 fmt::format("addr=0x{:08x} fmt=0x{:04x} requestedSize={}x{} pitch={} viewMip={} viewSlice={}+{}",
										 depthBufferMPTR,
										 (uint32)depthBufferFormat,
										 width,
										 height,
										 pitch,
										 viewMip,
										 viewFirstSlice,
										 viewNumSlices));
}

bool D3D12Renderer::ImguiBegin(bool mainWindow)
{
	D3D12_EXCLUSIVE_RECORDING();
	if (!m_imguiInitialized)
		return false;

	sint32 width = 0;
	sint32 height = 0;
	WindowSystem::GetWindowPhysSize(width, height);
	if (width == 0 || height == 0)
		return false;

	ImGui::SetCurrentContext(mainWindow ? imguiTVContext : imguiPadContext);
	{
		ImGuiIO& io = ImGui::GetIO();
		io.DisplaySize = ImVec2((float)width, (float)height);
		io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
	}
	ImGui_PrecacheFonts();
	ImGui::SetCurrentContext(mainWindow ? imguiTVContext : imguiPadContext);
	ImGuiIO& io = ImGui::GetIO();
	io.DisplaySize = ImVec2((float)width, (float)height);
	io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
	if (!io.Fonts->IsBuilt())
	{
		unsigned char* pixels = nullptr;
		int fontWidth = 0;
		int fontHeight = 0;
		io.Fonts->GetTexDataAsRGBA32(&pixels, &fontWidth, &fontHeight);
	}
#if defined(CEMU_UWP_D3D12_DIAGNOSTICS)
	D3D12TraceEventFlush("imgui_font_atlas_ready",
		"context={} fonts={} atlas={}x{} built={} display={}x{}",
		mainWindow ? "tv" : "pad",
		io.Fonts->Fonts.Size,
		io.Fonts->TexWidth,
		io.Fonts->TexHeight,
		io.Fonts->IsBuilt() ? 1 : 0,
		io.DisplaySize.x,
		io.DisplaySize.y);
#endif

	auto& chain = GetSwapchain(true);
	if (!chain.active)
		return false;

#if defined(CEMU_UWP)
	if (io.Fonts->TexID == 0 && !ImGui_ImplDX12_CreateDeviceObjects())
	{
		cemuLog_log(LogType::Force,
			"D3D12: failed to create ImGui device objects (fonts={}, atlas={}x{}).",
			io.Fonts->Fonts.Size, io.Fonts->TexWidth, io.Fonts->TexHeight);
		throw std::runtime_error(fmt::format("D3D12 failed to create the frontend font texture ({})", ImGui_ImplDX12_GetLastError()));
	}
#endif

	if (!m_commandListOpen)
		ResetCommandList();

	chain.frameIndex = chain.swapchain->GetCurrentBackBufferIndex();
	Transition(chain.backBuffers[chain.frameIndex].Get(), chain.states[chain.frameIndex], D3D12_RESOURCE_STATE_RENDER_TARGET);
	SetD3D12RenderTargets(1, &chain.rtvHandles[chain.frameIndex], nullptr);
	ID3D12DescriptorHeap* heaps[] = { m_imguiSrvHeap.Get() };
	GetCurrentCommandSlot().commandList->SetDescriptorHeaps(1, heaps);
	ResetCommandStateCache();

#if defined(CEMU_UWP_D3D12_DIAGNOSTICS)
	D3D12TraceEventFlush("imgui_backend_new_frame_begin", "context={} frameIndex={}", mainWindow ? "tv" : "pad", chain.frameIndex);
#endif
	ImGui_ImplDX12_NewFrame();
#if defined(CEMU_UWP_D3D12_DIAGNOSTICS)
	D3D12TraceEventFlush("imgui_backend_new_frame_end", "context={} frameIndex={}", mainWindow ? "tv" : "pad", chain.frameIndex);
#endif
	ImGui_UpdateWindowInformation(true);
	{
		const float width = (float)std::max(chain.size.x, 1);
		const float height = (float)std::max(chain.size.y, 1);
		if (io.DisplaySize.x != width || io.DisplaySize.y != height || io.DisplaySize.x < 0.0f || io.DisplaySize.y < 0.0f)
		{
			io.DisplaySize = ImVec2(width, height);
			io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
		}
	}
	ImGui::NewFrame();
	m_imguiActiveSwapchain = &chain;
	return true;
}

void D3D12Renderer::ImguiEnd()
{
	D3D12_EXCLUSIVE_RECORDING();
	if (!m_imguiInitialized || !m_imguiActiveSwapchain)
		return;
	ImGui::Render();
#if defined(CEMU_UWP_D3D12_DIAGNOSTICS)
	const ImDrawData* drawData = ImGui::GetDrawData();
	D3D12TraceEventFlush("imgui_render_begin", "lists={} vertices={} indices={} display={}x{}",
		drawData ? drawData->CmdListsCount : 0,
		drawData ? drawData->TotalVtxCount : 0,
		drawData ? drawData->TotalIdxCount : 0,
		drawData ? drawData->DisplaySize.x : 0.0f,
		drawData ? drawData->DisplaySize.y : 0.0f);
#endif
	m_gpuWatchdog.RecordOperation(D3D12GpuWatchdog::Operation::OverlayDraw);
	ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), GetCurrentCommandSlot().commandList.Get());
#if defined(CEMU_UWP_D3D12_DIAGNOSTICS)
	D3D12TraceEventFlush("imgui_render_end", "frameIndex={}", m_imguiActiveSwapchain->frameIndex);
#endif
	ResetCommandStateCache();
	Transition(m_imguiActiveSwapchain->backBuffers[m_imguiActiveSwapchain->frameIndex].Get(), m_imguiActiveSwapchain->states[m_imguiActiveSwapchain->frameIndex], D3D12_RESOURCE_STATE_PRESENT);
	if (CafeSystem::IsTitleRunning())
	{
		m_uwpCompositeFrameStarted = true;
		m_uwpCompositeFrameRendered = true;
	}
	m_imguiActiveSwapchain = nullptr;
}

ImTextureID D3D12Renderer::GenerateImguiTexture(const std::vector<uint8>& data, const Vector2i& size, DXGI_FORMAT format)
{
	if (!m_memoryManager || size.x <= 0 || size.y <= 0 || data.empty())
		return nullptr;
	try
	{
		if (!m_commandListOpen)
			ResetCommandList();

		D3D12_RESOURCE_DESC desc{};
		desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		desc.Width = (UINT64)size.x;
		desc.Height = (UINT)size.y;
		desc.DepthOrArraySize = 1;
		desc.MipLevels = 1;
		desc.Format = format;
		desc.SampleDesc.Count = 1;
		desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

		D3D12MemoryManager::Resource texture;
		if (!m_memoryManager->CreateTexture(desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, texture))
			throw std::runtime_error("Failed to create D3D12 ImGui texture");

		D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
		UINT numRows = 0;
		UINT64 rowSizeInBytes = 0;
		UINT64 totalBytes = 0;
		m_device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, &numRows, &rowSizeInBytes, &totalBytes);
#if defined(CEMU_UWP_D3D12_DIAGNOSTICS)
		D3D12TraceEventFlush("imgui_texture_upload_begin",
			"size={}x{} format={} sourceBytes={} rows={} rowBytes={} rowPitch={} totalBytes={}",
			size.x,
			size.y,
			(uint32)format,
			data.size(),
			numRows,
			(unsigned long long)rowSizeInBytes,
			footprint.Footprint.RowPitch,
			(unsigned long long)totalBytes);
#endif
		auto upload = m_memoryManager->AllocateUpload((uint32)totalBytes, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT, m_nextFenceValue);
		if (!upload.IsValid())
			throw std::runtime_error("Failed to allocate D3D12 ImGui texture upload memory");

		const uint8* src = data.data();
		const size_t rgbBytes = (size_t)size.x * size.y * 3;
		const size_t rgbaBytes = (size_t)size.x * size.y * 4;
		const bool sourceIsRgb = data.size() >= rgbBytes && data.size() < rgbaBytes;
		for (sint32 y = 0; y < size.y; ++y)
		{
			uint8* dstRow = upload.cpuPtr + y * footprint.Footprint.RowPitch;
			if (sourceIsRgb)
			{
				for (sint32 x = 0; x < size.x; ++x)
				{
					const size_t srcIndex = ((size_t)y * size.x + x) * 3;
					const size_t dstIndex = (size_t)x * 4;
					dstRow[dstIndex + 0] = src[srcIndex + 0];
					dstRow[dstIndex + 1] = src[srcIndex + 1];
					dstRow[dstIndex + 2] = src[srcIndex + 2];
					dstRow[dstIndex + 3] = 0xFF;
				}
			}
			else
			{
				const size_t srcOffset = (size_t)y * size.x * 4;
				if (srcOffset < data.size())
				{
					const size_t copyBytes = std::min<size_t>((size_t)rowSizeInBytes, data.size() - srcOffset);
					memcpy(dstRow, src + srcOffset, copyBytes);
				}
			}
		}
		footprint.Offset = upload.offset;

		D3D12_TEXTURE_COPY_LOCATION source{};
		source.pResource = upload.resource;
		source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
		source.PlacedFootprint = footprint;

		D3D12_TEXTURE_COPY_LOCATION destination{};
		destination.pResource = texture.resource.Get();
		destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		destination.SubresourceIndex = 0;

		GetCurrentCommandSlot().commandList->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
		Transition(texture.resource.Get(), texture.state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

		const uint32 descriptorIndex = AllocateImguiDescriptor();
		D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		srvDesc.Format = format;
		srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Texture2D.MipLevels = 1;
		CreateShaderResourceViewChecked(texture.resource.Get(), &srvDesc, GetImguiCpuHandle(descriptorIndex), "overlay texture", false);
		const auto gpuHandle = GetImguiGpuHandle(descriptorIndex);
		m_imguiTextures.emplace(gpuHandle.ptr, ImGuiTextureResource{ std::move(texture), descriptorIndex, size });
#if defined(CEMU_UWP_D3D12_DIAGNOSTICS)
		D3D12TraceEventFlush("imgui_texture_upload_end", "descriptor={} gpu=0x{:016x}", descriptorIndex, (unsigned long long)gpuHandle.ptr);
#endif
		return (ImTextureID)(uintptr_t)gpuHandle.ptr;
	}
	catch (const std::exception& ex)
	{
		cemuLog_log(LogType::Force, "can't generate D3D12 imgui texture: {}", ex.what());
		return nullptr;
	}
}

bool D3D12Renderer::UpdateImguiTexture(ImTextureID id, const std::vector<uint8>& data, const Vector2i& size, DXGI_FORMAT expectedFormat)
{
	if (!m_memoryManager || size.x <= 0 || size.y <= 0)
		return false;
	auto it = m_imguiTextures.find((uint64)(uintptr_t)id);
	if (it == m_imguiTextures.end())
		return false;
	if (it->second.size.x != size.x || it->second.size.y != size.y)
		return false;
	if (data.size() < (size_t)size.x * (size_t)size.y * 4)
		return false;
	try
	{
		if (!m_commandListOpen)
			ResetCommandList();

		auto& entry = it->second;
		const D3D12_RESOURCE_DESC desc = entry.texture.resource->GetDesc();
		if (desc.Format != expectedFormat)
			return false;

		D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
		UINT numRows = 0;
		UINT64 rowSizeInBytes = 0;
		UINT64 totalBytes = 0;
		m_device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, &numRows, &rowSizeInBytes, &totalBytes);
		auto upload = m_memoryManager->AllocateUpload((uint32)totalBytes, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT, m_nextFenceValue);
		if (!upload.IsValid())
			throw std::runtime_error("Failed to allocate D3D12 ImGui texture upload memory");

		const uint8* src = data.data();
		const size_t srcRowBytes = (size_t)size.x * 4;
		const size_t copyBytes = std::min<size_t>((size_t)rowSizeInBytes, srcRowBytes);
		for (sint32 y = 0; y < size.y; ++y)
			memcpy(upload.cpuPtr + (size_t)y * footprint.Footprint.RowPitch, src + (size_t)y * srcRowBytes, copyBytes);
		footprint.Offset = upload.offset;

		D3D12_TEXTURE_COPY_LOCATION source{};
		source.pResource = upload.resource;
		source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
		source.PlacedFootprint = footprint;

		D3D12_TEXTURE_COPY_LOCATION destination{};
		destination.pResource = entry.texture.resource.Get();
		destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		destination.SubresourceIndex = 0;

		Transition(entry.texture.resource.Get(), entry.texture.state, D3D12_RESOURCE_STATE_COPY_DEST);
		GetCurrentCommandSlot().commandList->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
		Transition(entry.texture.resource.Get(), entry.texture.state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		return true;
	}
	catch (const std::exception& ex)
	{
		cemuLog_log(LogType::Force, "can't update D3D12 imgui texture: {}", ex.what());
		return false;
	}
}

ImTextureID D3D12Renderer::GenerateTexture(const std::vector<uint8>& data, const Vector2i& size)
{
	D3D12_EXCLUSIVE_RECORDING();
	return GenerateImguiTexture(data, size, DXGI_FORMAT_R8G8B8A8_UNORM);
}

bool D3D12Renderer::UpdateTexture(ImTextureID id, const std::vector<uint8>& data, const Vector2i& size)
{
	return UpdateImguiTexture(id, data, size, DXGI_FORMAT_R8G8B8A8_UNORM);
}

ImTextureID D3D12Renderer::GenerateTextureBGRA(const std::vector<uint8>& data, const Vector2i& size)
{
	D3D12_EXCLUSIVE_RECORDING();
	return GenerateImguiTexture(data, size, DXGI_FORMAT_B8G8R8A8_UNORM);
}

bool D3D12Renderer::UpdateTextureBGRA(ImTextureID id, const std::vector<uint8>& data, const Vector2i& size)
{
	return UpdateImguiTexture(id, data, size, DXGI_FORMAT_B8G8R8A8_UNORM);
}

void D3D12Renderer::DeleteTexture(ImTextureID id)
{
	D3D12_EXCLUSIVE_RECORDING();
	const uint64 key = (uint64)(uintptr_t)id;
	auto it = m_imguiTextures.find(key);
	if (it == m_imguiTextures.end())
		return;
	QueueDeferredRelease(std::move(it->second.texture));
	if (it->second.descriptorIndex != 0)
	{
		const uint64 retirementFence = GetRetirementFence();
		if (retirementFence == 0 || retirementFence <= m_lastCompletedFenceValue)
		{
			m_imguiFreeDescriptorIndices.push_back(it->second.descriptorIndex);
		}
		else
		{
			std::scoped_lock lock(m_deferredReleaseMutex);
			m_retiredImguiDescriptors.push_back({ retirementFence, it->second.descriptorIndex });
		}
	}
	m_imguiTextures.erase(it);
}

void D3D12Renderer::DeleteFontTextures()
{
	D3D12_EXCLUSIVE_RECORDING();
	if (!m_imguiInitialized)
		return;
	ImGuiContext* previousContext = ImGui::GetCurrentContext();
	WaitForIdle();
	ImGui::SetCurrentContext(imguiTVContext);
	ImGui_ImplDX12_InvalidateDeviceObjects();
	ImGui::SetCurrentContext(imguiPadContext);
	ImGui_ImplDX12_InvalidateDeviceObjects();
	if (previousContext)
		ImGui::SetCurrentContext(previousContext);
}

void D3D12Renderer::renderTarget_setViewport(float x, float y, float width, float height, float nearZ, float farZ, bool halfZ)
{
	D3D12_QUEUE_RECORDING(renderTarget_setViewport(x, y, width, height, nearZ, farZ, halfZ));
	// halfZ is applied in HLSL SET_POSITION; D3D12 clips to 0..w before the viewport
	(void)halfZ;
	m_currentViewport.TopLeftX = x;
	m_currentViewport.TopLeftY = y;
	m_currentViewport.Width = width;
	m_currentViewport.Height = height;
	m_currentViewport.MinDepth = nearZ;
	m_currentViewport.MaxDepth = farZ;
	if (height < 0.0f && !m_invertedViewportHeightFlipsYSupported)
		cemuLog_logOnce(LogType::Force, "D3D12: guest viewport has a negative height, which this device does not report support for.");
	if (nearZ < 0.0f || nearZ > 1.0f || farZ < 0.0f || farZ > 1.0f)
		cemuLog_logOnce(LogType::Force, "D3D12: guest viewport depth range {}..{} is outside 0..1.", nearZ, farZ);
	if (m_commandListOpen)
		SetD3D12Viewport(m_currentViewport);
}

void D3D12Renderer::renderTarget_setScissor(sint32 scissorX, sint32 scissorY, sint32 scissorWidth, sint32 scissorHeight)
{
	D3D12_QUEUE_RECORDING(renderTarget_setScissor(scissorX, scissorY, scissorWidth, scissorHeight));
	m_currentScissor.left = scissorX;
	m_currentScissor.top = scissorY;
	m_currentScissor.right = scissorX + scissorWidth;
	m_currentScissor.bottom = scissorY + scissorHeight;
	if (m_commandListOpen)
		SetD3D12Scissor(m_currentScissor);
}
LatteCachedFBO* D3D12Renderer::rendertarget_createCachedFBO(uint64 key)
{
	D3D12_EXCLUSIVE_RECORDING();
	static uint32 s_createFboLogCount = 0;
	if (D3D12RuntimeLogCapped(s_createFboLogCount, "rendertarget_createCachedFBO", 64))
		cemuLog_log(LogType::Force, "D3D12: create cached FBO key=0x{:016x}.", key);
	D3D12TraceEvent("fbo_create", "key=0x{:016x}", key);
	return new CachedFBOD3D12(key);
}

void D3D12Renderer::rendertarget_deleteCachedFBO(LatteCachedFBO* cfbo)
{
	static uint32 s_deleteFboLogCount = 0;
	if (D3D12RuntimeLogCapped(s_deleteFboLogCount, "rendertarget_deleteCachedFBO", 64))
		cemuLog_log(LogType::Force, "D3D12: delete cached FBO ptr={} key=0x{:016x}.", (void*)cfbo, cfbo ? cfbo->key : 0);
	D3D12TraceEventFlush("fbo_delete", "ptr={} key=0x{:016x}", (void*)cfbo, cfbo ? cfbo->key : 0);
	// renderer references are dropped when the object is freed (ForgetCachedFBO), it stays valid until then
}

void D3D12Renderer::ForgetCachedFBO(LatteCachedFBO* cfbo)
{
	if (cfbo == m_activeFBO)
		m_activeFBO = nullptr;
	for (auto& cachedOverlap : m_feedbackOverlapCache)
	{
		if (cachedOverlap.fbo == cfbo)
			cachedOverlap = {};
	}
	DropRetainedFeedbackCopies();
}

void D3D12Renderer::rendertarget_destroyCachedFBO(LatteCachedFBO* cfbo)
{
	if (!t_recordContext && m_latteActiveFBO == cfbo)
		m_latteActiveFBO = nullptr;
	D3D12_QUEUE_RECORDING(rendertarget_destroyCachedFBO(cfbo));
	if (m_shutdownComplete)
	{
		delete cfbo;
		return;
	}
	m_pendingFboDestroys.push_back(cfbo);
}

void D3D12Renderer::texture_destroyView(LatteTextureView* view)
{
	if (!t_isRecorderThread)
		m_latteBoundTextures.fill(nullptr);
	D3D12_QUEUE_RECORDING(texture_destroyView(view));
	if (m_shutdownComplete)
	{
		delete view;
		return;
	}
	m_pendingViewDestroys.push_back(view);
}

void D3D12Renderer::texture_destroy(LatteTexture* texture)
{
	if (!t_isRecorderThread)
		m_latteBoundTextures.fill(nullptr);
	D3D12_QUEUE_RECORDING(texture_destroy(texture));
	if (m_shutdownComplete)
	{
		delete texture;
		return;
	}
	m_pendingTextureDestroys.push_back(texture);
}

// Phase 4 S2: frees what the core detached, in the order the core used to free it (an FBO inside its view, views
// before their texture). Runs at the start of a draw sequence, at present and when memory is reclaimed; with a
// recording thread it runs after the last queued draw that can reference these objects.
void D3D12Renderer::FlushPendingDestroys()
{
	while (!m_pendingFboDestroys.empty() || !m_pendingViewDestroys.empty() || !m_pendingTextureDestroys.empty())
	{
		for (LatteCachedFBO* cfbo : std::exchange(m_pendingFboDestroys, {}))
		{
			ForgetCachedFBO(cfbo);
			delete cfbo;
		}
		for (LatteTextureView* view : std::exchange(m_pendingViewDestroys, {}))
			delete view;
		for (LatteTexture* texture : std::exchange(m_pendingTextureDestroys, {}))
			delete texture;
	}
}

// Phase 4 S3: threaded draw recording ------------------------------------------------------------------------------

thread_local bool D3D12Renderer::t_recordContext = false;
thread_local bool D3D12Renderer::t_isRecorderThread = false;

namespace
{
// about a frame of draws and their state calls; the Latte side waits instead of queueing without bound
constexpr size_t kMaxQueuedRecordingOps = 32768;
}

void D3D12Renderer::StartRecordingThread()
{
	if (m_recordThread.joinable())
		return;
	m_recordStop = false;
	m_recordError = nullptr;
	m_latteBoundTextures.fill(nullptr);
	m_recordThread = std::thread([this]() { RecordingThreadMain(); });
	m_recordThreadRunning.store(true, std::memory_order_release);
	cemuLog_log(LogType::Force, "D3D12: threaded draw recording started; draws are recorded on a second thread.");
}

void D3D12Renderer::StopRecordingThread()
{
	if (!m_recordThread.joinable())
		return;
	{
		std::unique_lock lock(m_recordQueueMutex);
		m_recordStateCv.wait(lock, [this]() { return m_recordQueue.empty() && !m_recordBusy; });
		m_recordStop = true;
		// queue empty: from here callers record directly instead of queueing work the exiting thread would drop
		m_recordThreadRunning.store(false, std::memory_order_release);
	}
	m_recordWorkCv.notify_all();
	m_recordThread.join();
	m_recordError = nullptr;
}

void D3D12Renderer::RecordingThreadMain()
{
	SetThreadName("d3d12Record");
	t_isRecorderThread = true;
#if BOOST_OS_WINDOWS
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
#endif
	t_recordContext = true;
	std::vector<std::function<void()>> batch;
	while (true)
	{
		{
			std::unique_lock lock(m_recordQueueMutex);
			// producers wake the recorder only while it waits here; ops queued during a batch are picked up together
			m_recordWaiting = true;
			m_recordWorkCv.wait(lock, [this]() { return m_recordStop || !m_recordQueue.empty(); });
			m_recordWaiting = false;
			if (m_recordQueue.empty())
				break;
			batch.swap(m_recordQueue);
			m_recordBusy = true;
		}
		m_recordStateCv.notify_all();
		const auto batchStarted = std::chrono::steady_clock::now();
		{
			std::scoped_lock exec(m_recordExecMutex);
			for (auto& op : batch)
			{
				try
				{
					op();
				}
				catch (...)
				{
					// the rest of the batch builds on the failed work; the Latte side rethrows at its next call
					std::scoped_lock lock(m_recordQueueMutex);
					m_recordError = std::current_exception();
					break;
				}
			}
		}
		m_recordOps.fetch_add(batch.size(), std::memory_order_relaxed);
		m_recordBusyNs.fetch_add((uint64)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - batchStarted).count(),
			std::memory_order_relaxed);
		batch.clear();
		{
			std::scoped_lock lock(m_recordQueueMutex);
			m_recordBusy = false;
		}
		m_recordStateCv.notify_all();
	}
}

void D3D12Renderer::QueueRecording(std::function<void()> op)
{
	std::exception_ptr error;
	bool wakeRecorder = false;
	const bool sampleCall = (m_recordQueueCalls++ & 63) == 0; // only the Latte thread queues
	const auto callStarted = sampleCall ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
	{
		std::unique_lock lock(m_recordQueueMutex);
		if (m_recordQueue.size() >= kMaxQueuedRecordingOps && !m_recordError)
		{
			// the recorder is behind: this wait is time the Latte thread loses to it
			const auto waitStarted = std::chrono::steady_clock::now();
			m_recordStateCv.wait(lock, [this]() { return m_recordQueue.size() < kMaxQueuedRecordingOps || m_recordError; });
			m_recordFullWaits++;
			m_recordFullWaitNs += (uint64)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - waitStarted).count();
		}
		error = std::exchange(m_recordError, nullptr);
		if (!error)
			m_recordQueue.push_back(std::move(op));
		// a notify per op cost the Latte thread a wake-up call ~380k times a second in BOTW forests (1.9M ops per
		// 5 s, recorder busy only half the time): wake it only when it sleeps
		wakeRecorder = m_recordWaiting;
	}
	if (error)
		std::rethrow_exception(error);
	if (wakeRecorder)
		m_recordWorkCv.notify_one();
	if (sampleCall)
	{
		m_recordQueueSampledNs += (uint64)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - callStarted).count();
		m_recordQueueSamples++;
	}
}

void D3D12Renderer::DrainRecording()
{
	if (t_recordContext || !m_recordThreadRunning.load(std::memory_order_acquire))
		return;
	std::exception_ptr error;
	{
		std::unique_lock lock(m_recordQueueMutex);
		m_recordStateCv.wait(lock, [this]() { return (m_recordQueue.empty() && !m_recordBusy) || m_recordError; });
		error = std::exchange(m_recordError, nullptr);
	}
	if (error)
		std::rethrow_exception(error);
}

D3D12Renderer::RecordingExclusiveScope::RecordingExclusiveScope(D3D12Renderer* renderer, std::source_location caller)
{
	if (!renderer || t_recordContext || !renderer->m_recordThreadRunning.load(std::memory_order_acquire))
		return;
	const auto waitStarted = std::chrono::steady_clock::now();
	renderer->DrainRecording();
	renderer->m_recordExecMutex.lock();
	{
		const uint64 waitNs = (uint64)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - waitStarted).count();
		std::scoped_lock statLock(renderer->m_recordQueueMutex);
		auto& stat = renderer->m_recordExclusiveStats[caller.function_name()];
		stat.count++;
		stat.waitNs += waitNs;
	}
	t_recordContext = true;
	m_renderer = renderer;
}

D3D12Renderer::RecordingExclusiveScope::~RecordingExclusiveScope()
{
	if (!m_renderer)
		return;
	t_recordContext = false;
	m_renderer->m_recordExecMutex.unlock();
}


void D3D12Renderer::rendertarget_bindFramebufferObject(LatteCachedFBO* cfbo)
{
	if (!t_recordContext)
		m_latteActiveFBO = static_cast<CachedFBOD3D12*>(cfbo);
	D3D12_QUEUE_RECORDING(rendertarget_bindFramebufferObject(cfbo));
#if defined(CEMU_UWP_D3D12_DIAGNOSTICS)
	if (m_activeFBO && m_activeFBO != cfbo && D3D12ShouldTraceGpu())
		TraceFboAttachments("fbo_attachment_unbind", "before_unbind", 0, 0,
			"bind_call=rendertarget_bindFramebufferObject completedPass=1");
#endif
	// feedback copies are only shared within one render pass
	if (cfbo != m_activeFBO)
		DropRetainedFeedbackCopies();
	m_activeFBO = static_cast<CachedFBOD3D12*>(cfbo);
	static uint32 s_bindFboLogCount = 0;
	if (D3D12RuntimeLogCapped(s_bindFboLogCount, "rendertarget_bindFramebufferObject", 128))
	{
		if (m_activeFBO)
		{
			cemuLog_log(LogType::Force, "D3D12: bind FBO key=0x{:016x} size={}x{} colors={} mask=0x{:02x} depth={} sparse={}.",
						m_activeFBO->key,
						m_activeFBO->m_size.x,
						m_activeFBO->m_size.y,
						m_activeFBO->GetColorAttachmentCount(),
						m_activeFBO->GetColorAttachmentMask(),
						m_activeFBO->HasDepthStencilAttachment(),
						m_activeFBO->HasSparseColorAttachments());
		}
		else
			cemuLog_log(LogType::Force, "D3D12: bind null FBO.");
	}
	if (m_activeFBO)
	{
		D3D12TraceEvent("fbo_bind", "key=0x{:016x} size={}x{} colors={} mask=0x{:02x} depth={} sparse={}",
						m_activeFBO->key,
						m_activeFBO->m_size.x,
						m_activeFBO->m_size.y,
						m_activeFBO->GetColorAttachmentCount(),
						m_activeFBO->GetColorAttachmentMask(),
						m_activeFBO->HasDepthStencilAttachment() ? 1 : 0,
						m_activeFBO->HasSparseColorAttachments() ? 1 : 0);
		TraceFboAttachments("fbo_attachment_bind", "after_bind", 0, 0, "bind_call=rendertarget_bindFramebufferObject");
	}
	else
		D3D12TraceEvent("fbo_bind", "key=0x0000000000000000 null=1");
	if (m_commandListOpen)
		BindFramebuffer(m_activeFBO);
}

void D3D12Renderer::NotifyTextureDeletion(LatteTexture* texture)
{
	if (!t_isRecorderThread)
		m_latteBoundTextures.fill(nullptr);
	D3D12_QUEUE_RECORDING(NotifyTextureDeletion(texture));
	if (!texture)
		return;
	DropRetainedFeedbackCopies();

	BumpAllTableBindingGenerations();
	for (auto& boundTexture : m_boundTextures)
	{
		if (boundTexture && boundTexture->baseTexture == texture)
			boundTexture = nullptr;
	}
	for (auto& cachedOverlap : m_feedbackOverlapCache)
	{
		if (cachedOverlap.sampledView && cachedOverlap.sampledView->baseTexture == texture)
			cachedOverlap = {};
		else if (cachedOverlap.attachment && cachedOverlap.attachment->baseTexture == texture)
			cachedOverlap = {};
	}

	if (m_activeFBO)
	{
		bool activeFboReferencesTexture = false;
		for (const auto& attachment : m_activeFBO->GetColorAttachments())
		{
			if (attachment.valid && attachment.view && attachment.view->baseTexture == texture)
			{
				activeFboReferencesTexture = true;
				break;
			}
		}
		if (!activeFboReferencesTexture && m_activeFBO->depthBuffer.texture && m_activeFBO->depthBuffer.texture->baseTexture == texture)
			activeFboReferencesTexture = true;
		if (activeFboReferencesTexture)
			m_activeFBO = nullptr;
	}

	const auto referencesTexture = [texture](const auto& entry)
	{
		const auto& refs = entry.second.referencedTextures;
		return std::find(refs.begin(), refs.end(), texture) != refs.end();
	};
	std::erase_if(m_gameResourceTableCache, referencesTexture);
	std::erase_if(m_gameSamplerTableCache, referencesTexture);
	m_resourceTableKeyMemo = {};
	m_samplerTableKeyMemo = {};
}

void* D3D12Renderer::texture_acquireTextureUploadBuffer(uint32 size)
{
	return m_memoryManager ? m_memoryManager->AcquireTextureUploadBuffer(size) : nullptr;
}

void D3D12Renderer::texture_releaseTextureUploadBuffer(uint8* mem)
{
	if (m_memoryManager)
		m_memoryManager->ReleaseTextureUploadBuffer(mem);
}
TextureDecoder* D3D12Renderer::texture_chooseDecodedFormat(Latte::E_GX2SURFFMT format, bool isDepth, Latte::E_DIM dim, uint32 width, uint32 height)
{
	return GetD3D12FormatInfo(format, isDepth, dim, width, height).decoder;
}

void D3D12Renderer::texture_clearSlice(LatteTexture* hostTexture, sint32 sliceIndex, sint32 mipIndex)
{
	D3D12_EXCLUSIVE_RECORDING();
	if (!hostTexture)
		return;
	DropRetainedFeedbackCopies(hostTexture);
	if (hostTexture->isDepth)
		texture_clearDepthSlice(hostTexture, sliceIndex, mipIndex, true, hostTexture->hasStencil, 0.0f, 0);
	else
		texture_clearColorSlice(hostTexture, sliceIndex, mipIndex, 0.0f, 0.0f, 0.0f, 0.0f);
}

void D3D12Renderer::QueueCpuSliceUpload(CpuSliceUploadJob&& job)
{
	constexpr size_t kMaxPendingBytes = 64ull * 1024 * 1024; // bounds the copies held while a 110 MB array uploads
	std::unique_lock lock(m_cpuUploadMutex);
	if (m_cpuUploadThreads.empty())
	{
		const uint32 hardwareThreads = std::max(1u, std::thread::hardware_concurrency());
		// the game is stalled on the reload while these run (BOTW terrain: ~1.8 s of the 2.4 s with 4 threads)
		const uint32 workerCount = std::clamp(hardwareThreads > 2 ? hardwareThreads - 2 : 1u, 1u, 6u);
		m_cpuUploadStop = false;
		for (uint32 i = 0; i < workerCount; ++i)
			m_cpuUploadThreads.emplace_back(&D3D12Renderer::CpuSliceUploadThreadFunc, this);
		cemuLog_log(LogType::Force, "D3D12: {} threads write CPU-writable texture slices.", workerCount);
	}
	while (m_cpuUploadPendingBytes >= kMaxPendingBytes)
	{
		// the caller would only wait here, so it writes a queued slice instead
		if (!m_cpuUploadJobs.empty())
		{
			CpuSliceUploadJob queued = std::move(m_cpuUploadJobs.front());
			m_cpuUploadJobs.pop_front();
			lock.unlock();
			RunCpuSliceUploadJob(queued);
			lock.lock();
			continue;
		}
		m_cpuUploadDone.wait(lock);
	}
	m_cpuUploadPendingBytes += job.data.size();
	m_cpuUploadOutstanding.fetch_add(1, std::memory_order_acq_rel);
	m_cpuUploadJobs.emplace_back(std::move(job));
	m_cpuUploadWork.notify_one();
}

void D3D12Renderer::RunCpuSliceUploadJob(CpuSliceUploadJob& job)
{
	// Map/Unmap are free-threaded; each job writes a different subresource
	HRESULT hr = job.resource->Map(job.subresource, nullptr, nullptr);
	if (SUCCEEDED(hr))
	{
		hr = job.resource->WriteToSubresource(job.subresource, nullptr, job.data.data(), job.rowPitch, job.depthPitch);
		job.resource->Unmap(job.subresource, nullptr);
	}
	if (FAILED(hr) && m_cpuUploadFailures.fetch_add(1, std::memory_order_relaxed) == 0)
		cemuLog_log(LogType::Force, "D3D12: threaded texture slice write failed with 0x{:08x} (subresource {}).", (uint32)hr, job.subresource);
	{
		std::scoped_lock lock(m_cpuUploadMutex);
		m_cpuUploadPendingBytes -= job.data.size();
		m_cpuUploadOutstanding.fetch_sub(1, std::memory_order_acq_rel);
	}
	m_cpuUploadDone.notify_all();
}

void D3D12Renderer::CpuSliceUploadThreadFunc()
{
	SetThreadName("d3d12SliceUpload");
	while (true)
	{
		CpuSliceUploadJob job;
		{
			std::unique_lock lock(m_cpuUploadMutex);
			m_cpuUploadWork.wait(lock, [&]() { return m_cpuUploadStop || !m_cpuUploadJobs.empty(); });
			if (m_cpuUploadJobs.empty())
				return;
			job = std::move(m_cpuUploadJobs.front());
			m_cpuUploadJobs.pop_front();
		}
		RunCpuSliceUploadJob(job);
	}
}

void D3D12Renderer::texture_finishSliceUploads()
{
	D3D12_EXCLUSIVE_RECORDING();
	if (m_cpuUploadOutstanding.load(std::memory_order_acquire) == 0)
		return;
	std::unique_lock lock(m_cpuUploadMutex);
	while (m_cpuUploadOutstanding.load(std::memory_order_acquire) != 0)
	{
		// the caller would only wait here, so it writes slices too
		if (!m_cpuUploadJobs.empty())
		{
			CpuSliceUploadJob job = std::move(m_cpuUploadJobs.front());
			m_cpuUploadJobs.pop_front();
			lock.unlock();
			RunCpuSliceUploadJob(job);
			lock.lock();
			continue;
		}
		m_cpuUploadDone.wait(lock);
	}
}

void D3D12Renderer::StopCpuSliceUploadThreads()
{
	std::vector<std::thread> threads;
	{
		std::scoped_lock lock(m_cpuUploadMutex);
		m_cpuUploadStop = true;
		threads.swap(m_cpuUploadThreads);
	}
	m_cpuUploadWork.notify_all();
	for (auto& thread : threads)
		thread.join();
}

D3D12Renderer::ArrayUploadStaging* D3D12Renderer::GetArrayUploadStaging(DXGI_FORMAT format, uint32 width, uint32 height)
{
	const uint64 key = ((uint64)format << 48) | ((uint64)width << 24) | height;
	if (auto it = m_arrayUploadStaging.find(key); it != m_arrayUploadStaging.end())
		return &it->second;
	ArrayUploadStaging staging;
	staging.desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	staging.desc.Width = width;
	staging.desc.Height = height;
	staging.desc.DepthOrArraySize = 1;
	staging.desc.MipLevels = 1;
	staging.desc.Format = format;
	staging.desc.SampleDesc.Count = 1;
	staging.desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	if (!CreateTextureWithRecovery(staging.desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, staging.texture, "array upload staging"))
		return nullptr;
	staging.state = D3D12_RESOURCE_STATE_COPY_DEST;
	return &m_arrayUploadStaging.emplace(key, std::move(staging)).first->second;
}

void D3D12Renderer::texture_loadSlice(LatteTexture* hostTexture, sint32 width, sint32 height, sint32 depth, void* pixelData, sint32 sliceIndex, sint32 mipIndex, uint32 compressedImageSize)
{
	D3D12_EXCLUSIVE_RECORDING();
	DropRetainedFeedbackCopies(hostTexture);
	stdx::scope_exit deviceCheck([&]() { CheckDeviceAfterOperation("texture upload", [&]() {
		return fmt::format("{} gx2fmt=0x{:04x} mip={} slice={} {}x{}x{} bytes={}", D3D12DescribeResource(hostTexture ? static_cast<LatteTextureD3D12*>(hostTexture)->GetResource() : nullptr),
			hostTexture ? (uint32)hostTexture->format : 0, mipIndex, sliceIndex, width, height, depth, compressedImageSize); }); });
	auto* texture = static_cast<LatteTextureD3D12*>(hostTexture);
	if (!texture || !texture->GetResource() || !pixelData || compressedImageSize == 0)
		return;
	const auto& textureDesc = texture->GetDesc();
	if (mipIndex < 0 || mipIndex >= (sint32)textureDesc.MipLevels || sliceIndex < 0 || width <= 0 || height <= 0)
		return;
	const bool is3DTexture = texture->Is3DTexture();
	const uint32 mipWidth = std::max<uint32>(1, (uint32)(textureDesc.Width >> mipIndex));
	const uint32 mipHeight = std::max<uint32>(1, (uint32)(textureDesc.Height >> mipIndex));
	const uint32 sliceLimit = is3DTexture ?
		std::max<uint32>(1, (uint32)textureDesc.DepthOrArraySize >> mipIndex) :
		std::max<uint32>(1, texture->GetArraySize());
	if ((uint32)sliceIndex >= sliceLimit || (uint32)width != mipWidth || (uint32)height != mipHeight)
	{
		cemuLog_log(LogType::Force,
			"D3D12: rejecting texture upload outside the destination subresource (format=0x{:04x}, mip={}, slice={}, supplied={}x{}, expected={}x{}, slices={}).",
			(uint32)texture->format, mipIndex, sliceIndex, width, height, mipWidth, mipHeight, sliceLimit);
		return;
	}
	if (texture->IsCpuWritable())
	{
		// the GPU may still read a reloaded texture, so let it finish before overwriting
		if (texture->reloadCount > 1 && texture->m_cpuWriteSyncedReload != (uint32)texture->reloadCount)
		{
			WaitForIdle();
			texture->m_cpuWriteSyncedReload = (uint32)texture->reloadCount;
		}
		const uint32 cpuSubresource = texture->GetSubresourceIndex(sliceIndex, mipIndex);
		D3D12_PLACED_SUBRESOURCE_FOOTPRINT cpuFootprint{};
		UINT cpuRows = 0;
		UINT64 cpuRowBytes = 0;
		UINT64 cpuTotalBytes = 0;
		m_device->GetCopyableFootprints(&textureDesc, cpuSubresource, 1, 0, &cpuFootprint, &cpuRows, &cpuRowBytes, &cpuTotalBytes);
		const uint32 cpuSourceRowPitch = cpuRows != 0 ? compressedImageSize / cpuRows : compressedImageSize;
		ID3D12Resource* resource = texture->GetResource();
		if (cpuRows != 0 && cpuSourceRowPitch >= cpuRowBytes && compressedImageSize >= 32 * 1024)
		{
			// large slices go to the upload threads; the reload waits for them in texture_finishSliceUploads
			CpuSliceUploadJob job;
			job.resource = resource;
			job.subresource = cpuSubresource;
			job.rowPitch = cpuSourceRowPitch;
			job.depthPitch = cpuSourceRowPitch * cpuRows;
			const auto* bytes = static_cast<const uint8*>(pixelData);
			job.data.assign(bytes, bytes + (size_t)cpuSourceRowPitch * cpuRows);
			QueueCpuSliceUpload(std::move(job));
			texture->MarkSubresourceWritten(cpuSubresource);
			return;
		}
		if (cpuRows == 0 || cpuSourceRowPitch < cpuRowBytes || FAILED(resource->Map(cpuSubresource, nullptr, nullptr)))
		{
			cemuLog_logOnce(LogType::Force, "D3D12: CPU texture upload failed (format=0x{:04x}, slice={}, mip={}, bytes={}).",
				(uint32)texture->format, sliceIndex, mipIndex, compressedImageSize);
			return;
		}
		const HRESULT hr = resource->WriteToSubresource(cpuSubresource, nullptr, pixelData, cpuSourceRowPitch, cpuSourceRowPitch * cpuRows);
		resource->Unmap(cpuSubresource, nullptr);
		if (ShouldCheckDevice(true) && FAILED(m_device->GetDeviceRemovedReason()))
		{
			ReportDeviceRemovalAfter("WriteToSubresource", "CPU texture upload", fmt::format("{} subresource={} rowPitch={} rows={} bytes={} hr=0x{:08x}",
				D3D12DescribeResource(resource), cpuSubresource, cpuSourceRowPitch, cpuRows, compressedImageSize, (uint32)hr));
		}
		if (FAILED(hr))
		{
			cemuLog_logOnce(LogType::Force, "D3D12: WriteToSubresource failed with 0x{:08x} (format=0x{:04x}).", (uint32)hr, (uint32)texture->format);
			return;
		}
		texture->MarkSubresourceWritten(cpuSubresource);
		return;
	}
	if (!m_commandListOpen)
		ResetCommandList();

	const uint32 subresource = texture->GetSubresourceIndex(is3DTexture ? 0 : sliceIndex, mipIndex);
	if (texture->GetFormatInfo().hasStencil)
	{
		if (is3DTexture || width <= 0 || height <= 0)
			return;

		const uint32 mipLevels = std::max<uint32>(textureDesc.MipLevels, 1);
		const uint32 arraySize = std::max<uint32>(texture->GetArraySize(), 1);
		const uint32 sourceTexelBytes =
			texture->format == Latte::E_GX2SURFFMT::D24_S8_UNORM ? 4u : 8u;
		const uint32 stencilByteOffset = sourceTexelBytes == 4 ? 3u : 4u;
		const uint64 sourceRowPitch = (uint64)width * sourceTexelBytes;
		const uint64 requiredSourceBytes = sourceRowPitch * (uint64)height;
		if (requiredSourceBytes > compressedImageSize)
		{
			cemuLog_log(LogType::Force,
				"D3D12: depth-stencil upload is undersized (format=0x{:04x}, required={}, supplied={}).",
				(uint32)texture->format,
				(unsigned long long)requiredSourceBytes,
				compressedImageSize);
			return;
		}

		const uint8* sourcePixels = static_cast<const uint8*>(pixelData);
		for (uint32 plane = 0; plane < 2; ++plane)
		{
			const uint32 planeSubresource = subresource + plane * mipLevels * arraySize;
			const D3D12_RESOURCE_STATES previousState = texture->GetSubresourceState(planeSubresource);
			D3D12_PLACED_SUBRESOURCE_FOOTPRINT planeFootprint{};
			UINT planeRows = 0;
			UINT64 planeRowBytes = 0;
			UINT64 planeTotalBytes = 0;
			m_device->GetCopyableFootprints(&textureDesc, planeSubresource, 1, 0,
				&planeFootprint, &planeRows, &planeRowBytes, &planeTotalBytes);
			planeFootprint.Footprint.Depth = 1;
			planeTotalBytes = (UINT64)planeFootprint.Footprint.RowPitch * planeRows;
			const UINT64 expectedPlaneRowBytes = plane == 0 ? (UINT64)mipWidth * sizeof(uint32) : (UINT64)mipWidth;
			if (planeRows != mipHeight || planeRowBytes != expectedPlaneRowBytes ||
				planeTotalBytes == 0 || planeTotalBytes > std::numeric_limits<uint32>::max())
			{
				cemuLog_log(LogType::Force,
					"D3D12: unsupported depth-stencil upload footprint (format=0x{:04x}, plane={}, rows={}/{}, rowBytes={}/{}).",
					(uint32)texture->format, plane, planeRows, mipHeight,
					(unsigned long long)planeRowBytes, (unsigned long long)expectedPlaneRowBytes);
				return;
			}

			auto upload = m_memoryManager->AllocateUpload((uint32)planeTotalBytes,
				D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT, m_nextFenceValue);
			if (!upload.IsValid())
				throw std::runtime_error("Failed to allocate D3D12 depth-stencil upload memory");
			memset(upload.cpuPtr, 0, (size_t)planeTotalBytes);

			for (UINT row = 0; row < planeRows; ++row)
			{
				const uint8* sourceRow = sourcePixels + (uint64)row * sourceRowPitch;
				uint8* destinationRow = upload.cpuPtr + (uint64)row * planeFootprint.Footprint.RowPitch;
				if (plane == 0)
				{
					for (UINT x = 0; x < mipWidth; ++x)
						memcpy(destinationRow + (size_t)x * sizeof(uint32), sourceRow + (size_t)x * sourceTexelBytes, sizeof(uint32));
				}
				else
				{
					for (UINT x = 0; x < mipWidth; ++x)
						destinationRow[x] = sourceRow[(size_t)x * sourceTexelBytes + stencilByteOffset];
				}
			}

			planeFootprint.Offset = upload.offset;
			D3D12_TEXTURE_COPY_LOCATION source{};
			source.pResource = upload.resource;
			source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
			source.PlacedFootprint = planeFootprint;
			D3D12_TEXTURE_COPY_LOCATION destination{};
			destination.pResource = texture->GetResource();
			destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
			destination.SubresourceIndex = planeSubresource;

			TransitionTextureSubresource(texture, planeSubresource, D3D12_RESOURCE_STATE_COPY_DEST);
			GetCurrentCommandSlot().commandList->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
			TransitionTextureSubresource(texture, planeSubresource, GetD3D12StateAfterUpload(previousState));
			texture->MarkSubresourceWritten(planeSubresource);
		}
		return;
	}

	D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
	UINT numRows = 0;
	UINT64 rowSizeInBytes = 0;
	UINT64 totalBytes = 0;
	m_device->GetCopyableFootprints(&texture->GetDesc(), subresource, 1, 0, &footprint, &numRows, &rowSizeInBytes, &totalBytes);
	footprint.Footprint.Depth = 1;
	totalBytes = (UINT64)footprint.Footprint.RowPitch * numRows;
	if (totalBytes == 0 || totalBytes > std::numeric_limits<uint32>::max())
		return;

	auto upload = m_memoryManager->AllocateUpload((uint32)totalBytes, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT, m_nextFenceValue);
	if (!upload.IsValid())
		throw std::runtime_error("Failed to allocate D3D12 texture upload memory");

	const uint8* src = static_cast<const uint8*>(pixelData);
	const uint64 sourceRowPitch = numRows != 0 ? compressedImageSize / numRows : compressedImageSize;
	const uint64 copyRowBytes = std::min<uint64>(rowSizeInBytes, sourceRowPitch);
	for (UINT row = 0; row < numRows; row++)
		memcpy(upload.cpuPtr + row * footprint.Footprint.RowPitch, src + row * sourceRowPitch, copyRowBytes);

	footprint.Offset = upload.offset;

	D3D12_TEXTURE_COPY_LOCATION source{};
	source.pResource = upload.resource;
	source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
	source.PlacedFootprint = footprint;

	D3D12_TEXTURE_COPY_LOCATION destination{};
	destination.pResource = texture->GetResource();
	destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	destination.SubresourceIndex = subresource;

	const D3D12_RESOURCE_STATES previousState = texture->GetSubresourceState(subresource);
	ArrayUploadStaging* staging = nullptr;
	if (m_isUma && !is3DTexture && sliceIndex > 0 && texture->GetArraySize() > 1 && GetConfig().d3d12_gpu_array_upload &&
		(textureDesc.Format >= DXGI_FORMAT_BC1_TYPELESS && textureDesc.Format <= DXGI_FORMAT_BC5_SNORM) &&
		(mipWidth % 4) == 0 && (mipHeight % 4) == 0) // a staging BC texture needs whole blocks
		staging = GetArrayUploadStaging(textureDesc.Format, mipWidth, mipHeight);
	if (staging)
	{
		auto* commandList = GetCurrentCommandSlot().commandList.Get();
		// the staging texture has the same format and size as this mip, so the footprint matches
		Transition(staging->texture.resource.Get(), staging->state, D3D12_RESOURCE_STATE_COPY_DEST);
		D3D12_TEXTURE_COPY_LOCATION stagingLocation{};
		stagingLocation.pResource = staging->texture.resource.Get();
		stagingLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		stagingLocation.SubresourceIndex = 0;
		commandList->CopyTextureRegion(&stagingLocation, 0, 0, 0, &source, nullptr);
		Transition(staging->texture.resource.Get(), staging->state, D3D12_RESOURCE_STATE_COPY_SOURCE);
		TransitionTextureSubresource(texture, subresource, D3D12_RESOURCE_STATE_COPY_DEST);
		commandList->CopyTextureRegion(&destination, 0, 0, 0, &stagingLocation, nullptr);
	}
	else
	{
		TransitionTextureSubresource(texture, subresource, D3D12_RESOURCE_STATE_COPY_DEST);
		GetCurrentCommandSlot().commandList->CopyTextureRegion(&destination, 0, 0, is3DTexture ? sliceIndex : 0, &source, nullptr);
	}
	TransitionTextureSubresource(texture, subresource, GetD3D12StateAfterUpload(previousState));
	texture->MarkSubresourceWritten(subresource);
	if (D3D12ShouldTrace())
	{
		auto* view = static_cast<LatteTextureViewD3D12*>(texture->GetOrCreateView(texture->dim, texture->format, mipIndex, 1, sliceIndex, 1));
		RecordTextureLineage("upload", view, 0, 0, "cpu_upload",
							 fmt::format("uploadBytes={} rowPitch={} rows={} copyRowBytes={} slice={} mip={}",
										 compressedImageSize,
										 (uint32)footprint.Footprint.RowPitch,
										 (uint32)numRows,
										 (unsigned long long)copyRowBytes,
										 sliceIndex,
										 mipIndex));
		TraceTextureSnapshot("texture_load", "dst", view, false, fmt::format("uploadBytes={} rowPitch={} rows={} copyRowBytes={} slice={} mip={}",
																			 compressedImageSize,
																			 (uint32)footprint.Footprint.RowPitch,
																			 (uint32)numRows,
																			 (unsigned long long)copyRowBytes,
																			 sliceIndex,
																			 mipIndex));
	}
}

void D3D12Renderer::texture_clearColorSlice(LatteTexture* hostTexture, sint32 sliceIndex, sint32 mipIndex, float r, float g, float b, float a)
{
	auto record = [=, this](LatteTextureViewD3D12* preparedView) mutable
	{
		DropRetainedFeedbackCopies(hostTexture);
		stdx::scope_exit deviceCheck([&]() { CheckDeviceAfterOperation("colour clear", [&]() {
			return fmt::format("{} mip={} slice={}", D3D12DescribeResource(hostTexture ? static_cast<LatteTextureD3D12*>(hostTexture)->GetResource() : nullptr), mipIndex, sliceIndex); }); });
		auto* texture = static_cast<LatteTextureD3D12*>(hostTexture);
		if (!texture || texture->isDepth)
			return;
		// guest mips beyond what the texture size allows have no storage on the host
		if (mipIndex < 0 || mipIndex >= (sint32)texture->GetDesc().MipLevels)
			return;
		if (!m_commandListOpen)
			ResetCommandList();

		auto* view = preparedView ? preparedView : static_cast<LatteTextureViewD3D12*>(texture->GetOrCreateView(texture->dim, texture->format, mipIndex, 1, sliceIndex, 1));
		if (!view || !view->GetRTV().IsValid())
			return;

		const float color[4]{ r, g, b, a };
		static uint32 s_clearColorLogCount = 0;
		if (D3D12RuntimeLogCapped(s_clearColorLogCount, "texture_clearColorSlice", 96))
		{
			cemuLog_log(LogType::Force, "D3D12: clear color texture addr=0x{:08x} fmt=0x{:04x} size={}x{} slice={} mip={} color=({}, {}, {}, {}).",
						texture->physAddress, (uint32)texture->format, texture->width, texture->height,
						sliceIndex, mipIndex, r, g, b, a);
		}
		const uint32 clearSubresource = texture->GetSubresourceIndex(texture->Is3DTexture() ? 0 : sliceIndex, mipIndex);
		TransitionTextureSubresource(texture, clearSubresource, D3D12_RESOURCE_STATE_RENDER_TARGET);
		GetCurrentCommandSlot().commandList->ClearRenderTargetView(view->GetRTV().cpu, color, 0, nullptr);
		texture->MarkSubresourceWritten(clearSubresource);
		if (D3D12ShouldTrace())
		{
			RecordTextureLineage("clear_color", view, 0, 0, "none", fmt::format("rgba={},{},{},{} slice={} mip={}", r, g, b, a, sliceIndex, mipIndex));
			TraceTextureSnapshot("texture_clear_color", "dst", view, false, fmt::format("rgba={},{},{},{} slice={} mip={}", r, g, b, a, sliceIndex, mipIndex));
		}
	};
	if (ShouldQueueRecording())
	{
		// the view is core state: created here, the clear itself is queued
		auto* texture = static_cast<LatteTextureD3D12*>(hostTexture);
		if (!texture || texture->isDepth || mipIndex < 0 || mipIndex >= (sint32)texture->GetDesc().MipLevels)
			return;
		auto* view = static_cast<LatteTextureViewD3D12*>(texture->GetOrCreateView(texture->dim, texture->format, mipIndex, 1, sliceIndex, 1));
		if (!view)
			return;
		QueueRecording([record, view]() mutable { record(view); });
		return;
	}
	D3D12_EXCLUSIVE_RECORDING();
	record(nullptr);
}

void D3D12Renderer::texture_clearDepthSlice(LatteTexture* hostTexture, uint32 sliceIndex, sint32 mipIndex, bool clearDepth, bool clearStencil, float depthValue, uint32 stencilValue)
{
	auto record = [=, this](LatteTextureViewD3D12* preparedView) mutable
	{
		DropRetainedFeedbackCopies(hostTexture);
		stdx::scope_exit deviceCheck([&]() { CheckDeviceAfterOperation("depth clear", [&]() {
			return fmt::format("{} mip={} slice={} depth={} stencil={}", D3D12DescribeResource(hostTexture ? static_cast<LatteTextureD3D12*>(hostTexture)->GetResource() : nullptr), mipIndex, sliceIndex, clearDepth, clearStencil); }); });
		auto* texture = static_cast<LatteTextureD3D12*>(hostTexture);
		if (!texture || !texture->isDepth)
			return;
		// guest mips beyond what the texture size allows have no storage on the host
		if (mipIndex < 0 || mipIndex >= (sint32)texture->GetDesc().MipLevels)
			return;
		if (!m_commandListOpen)
			ResetCommandList();

		clearStencil = clearStencil && texture->GetFormatInfo().hasStencil;
		D3D12_CLEAR_FLAGS clearFlags{};
		if (clearDepth)
			clearFlags |= D3D12_CLEAR_FLAG_DEPTH;
		if (clearStencil)
			clearFlags |= D3D12_CLEAR_FLAG_STENCIL;
		if (clearFlags == 0)
			return;

		auto* view = preparedView ? preparedView : static_cast<LatteTextureViewD3D12*>(texture->GetOrCreateView(texture->dim, texture->format, mipIndex, 1, sliceIndex, 1));
		if (!view || !view->GetDSV().IsValid())
			return;

		static uint32 s_clearDepthLogCount = 0;
		if (D3D12RuntimeLogCapped(s_clearDepthLogCount, "texture_clearDepthSlice", 96))
		{
			cemuLog_log(LogType::Force, "D3D12: clear depth texture addr=0x{:08x} fmt=0x{:04x} size={}x{} slice={} mip={} depth={} stencil={} depthValue={} stencilValue={}.",
						texture->physAddress, (uint32)texture->format, texture->width, texture->height,
						sliceIndex, mipIndex, clearDepth, clearStencil, depthValue, stencilValue);
		}
		TransitionTextureView(view, D3D12_RESOURCE_STATE_DEPTH_WRITE, true);
		GetCurrentCommandSlot().commandList->ClearDepthStencilView(view->GetDSV().cpu, clearFlags, depthValue, (UINT8)stencilValue, 0, nullptr);
		const uint32 clearSubresource = texture->GetSubresourceIndex(texture->Is3DTexture() ? 0 : (sint32)sliceIndex, mipIndex);
		if (clearDepth)
			texture->MarkSubresourceWritten(clearSubresource);
		if (clearStencil)
		{
			const uint32 planeStride = std::max<uint32>(texture->GetDesc().MipLevels, 1) * std::max<uint32>(texture->GetArraySize(), 1);
			texture->MarkSubresourceWritten(clearSubresource + planeStride);
		}
		if (D3D12ShouldTrace())
		{
			RecordTextureLineage("clear_depth", view, 0, 0, "none",
								 fmt::format("clearDepth={} clearStencil={} depthValue={} stencilValue={} slice={} mip={}",
											 clearDepth ? 1 : 0,
											 clearStencil ? 1 : 0,
											 depthValue,
											 stencilValue,
											 sliceIndex,
											 mipIndex));
			TraceTextureSnapshot("texture_clear_depth", "dst", view, false, fmt::format("clearDepth={} clearStencil={} depthValue={} stencilValue={} slice={} mip={}",
																						clearDepth ? 1 : 0,
																						clearStencil ? 1 : 0,
																						depthValue,
																						stencilValue,
																						sliceIndex,
																						mipIndex));
		}
	};
	if (ShouldQueueRecording())
	{
		// the view is core state: created here, the clear itself is queued
		auto* texture = static_cast<LatteTextureD3D12*>(hostTexture);
		if (!texture || !texture->isDepth || mipIndex < 0 || mipIndex >= (sint32)texture->GetDesc().MipLevels)
			return;
		if (!clearDepth && !(clearStencil && texture->GetFormatInfo().hasStencil))
			return;
		auto* view = static_cast<LatteTextureViewD3D12*>(texture->GetOrCreateView(texture->dim, texture->format, mipIndex, 1, sliceIndex, 1));
		if (!view)
			return;
		QueueRecording([record, view]() mutable { record(view); });
		return;
	}
	D3D12_EXCLUSIVE_RECORDING();
	record(nullptr);
}

LatteTexture* D3D12Renderer::texture_createTextureEx(Latte::E_DIM dim, MPTR physAddress, MPTR physMipAddress, Latte::E_GX2SURFFMT format, uint32 width, uint32 height, uint32 depth, uint32 pitch, uint32 mipLevels, uint32 swizzle, Latte::E_HWTILEMODE tileMode, bool isDepth)
{
	D3D12_EXCLUSIVE_RECORDING();
	stdx::scope_exit deviceCheck([&]() { CheckDeviceAfterOperation("texture creation", [&]() {
		return fmt::format("addr=0x{:08x} dim={} gx2fmt=0x{:04x} {}x{}x{} mips={} tile={} depth={}", physAddress, (uint32)dim, (uint32)format,
			width, height, depth, mipLevels, (uint32)tileMode, isDepth); }); });
	auto* texture = new LatteTextureD3D12(this, dim, physAddress, physMipAddress, format, width, height, depth, pitch, mipLevels, swizzle, tileMode, isDepth);
	static uint32 s_createTextureLogCount = 0;
	if (D3D12RuntimeLogCapped(s_createTextureLogCount, "texture_createTextureEx", 160))
	{
		cemuLog_log(LogType::Force, "D3D12: create texture addr=0x{:08x} mipAddr=0x{:08x} dim={} fmt=0x{:04x} size={}x{}x{} pitch={} mips={} depth={} resource={}.",
					physAddress, physMipAddress, (uint32)dim, (uint32)format, width, height, depth,
					pitch, mipLevels, isDepth, texture->GetResource() != nullptr);
	}
	D3D12TraceEvent("texture_create", "addr=0x{:08x} mipAddr=0x{:08x} dim={} fmt=0x{:04x} dxgi={} size={}x{}x{} pitch={} mips={} swizzle=0x{:08x} tileMode={} depth={} resource={} obj={}",
					physAddress,
					physMipAddress,
					(uint32)dim,
					(uint32)format,
					(uint32)texture->GetResourceFormat(),
					width,
					height,
					depth,
					pitch,
					mipLevels,
					swizzle,
					(uint32)tileMode,
					isDepth ? 1 : 0,
					texture->GetResource() != nullptr ? 1 : 0,
					(const void*)texture);
	return texture;
}
void D3D12Renderer::texture_setLatteTexture(LatteTextureView* textureView, uint32 textureUnit)
{
	// Latte rebinds every texture unit for every draw; with threaded recording each bind was a queued op, most of the
	// ~5 per draw. Skip binds the recorder already has (the mirror is cleared whenever a view or texture goes away)
	if (!t_isRecorderThread && m_recordThreadRunning.load(std::memory_order_relaxed) && textureUnit < m_latteBoundTextures.size())
	{
		// an exclusive caller binds directly, so it only refreshes the copy
		if (!t_recordContext && m_latteBoundTextures[textureUnit] == textureView)
			return;
		m_latteBoundTextures[textureUnit] = textureView;
	}
	D3D12_QUEUE_RECORDING(texture_setLatteTexture(textureView, textureUnit));
	if (textureUnit >= m_boundTextures.size())
		return;
	if (m_boundTextures[textureUnit] != textureView)
		BumpTextureBindingGeneration(textureUnit);
	m_boundTextures[textureUnit] = static_cast<LatteTextureViewD3D12*>(textureView);
	static uint32 s_bindTextureLogCount = 0;
	if (D3D12RuntimeLogCapped(s_bindTextureLogCount, "texture_setLatteTexture", 160))
	{
		auto* d3dView = static_cast<LatteTextureViewD3D12*>(textureView);
		auto* texture = d3dView ? d3dView->GetBaseImage() : nullptr;
		// no SRV state here: GetSRV() prepares sampling proxies, which belongs to the recording side (Phase 4)
		cemuLog_log(LogType::Force, "D3D12: bind texture unit={} addr=0x{:08x} fmt=0x{:04x} size={}x{} mip{}+{} slice{}+{}.",
					textureUnit,
					texture ? texture->physAddress : 0,
					texture ? (uint32)texture->format : 0,
					texture ? texture->width : 0,
					texture ? texture->height : 0,
					d3dView ? d3dView->firstMip : 0,
					d3dView ? d3dView->numMip : 0,
					d3dView ? d3dView->firstSlice : 0,
					d3dView ? d3dView->numSlice : 0);
	}
	{
		auto* d3dView = static_cast<LatteTextureViewD3D12*>(textureView);
		auto* texture = d3dView ? d3dView->GetBaseImage() : nullptr;
		D3D12TraceEvent("texture_bind", "unit={} addr=0x{:08x} fmt=0x{:04x} dxgi={} size={}x{} mip={} mipCount={} slice={} sliceCount={} srv={} view={} texture={}",
						textureUnit,
						texture ? texture->physAddress : 0,
						texture ? (uint32)texture->format : 0,
						texture ? (uint32)d3dView->GetViewFormat() : 0,
						texture ? texture->width : 0,
						texture ? texture->height : 0,
						d3dView ? d3dView->firstMip : 0,
						d3dView ? d3dView->numMip : 0,
						d3dView ? d3dView->firstSlice : 0,
						d3dView ? d3dView->numSlice : 0,
						d3dView && d3dView->GetSRV().IsValid() ? 1 : 0,
						(const void*)d3dView,
						(const void*)texture);
	}
}

void D3D12Renderer::texture_copyImageSubData(LatteTexture* src, sint32 srcMip, sint32 effectiveSrcX, sint32 effectiveSrcY, sint32 srcSlice, LatteTexture* dst, sint32 dstMip, sint32 effectiveDstX, sint32 effectiveDstY, sint32 dstSlice, sint32 effectiveCopyWidth, sint32 effectiveCopyHeight, sint32 srcDepth)
{
	if (ShouldQueueRecording())
	{
		// only incompatible formats and depth/multisampled copies can fall back to the surface-copy draw, which
		// creates views and tracks GPU writes in core state; every other copy is recording work only
		const auto* srcTexture = static_cast<LatteTextureD3D12*>(src);
		const auto* dstTexture = static_cast<LatteTextureD3D12*>(dst);
		if (srcTexture && dstTexture && !srcTexture->isDepth && !dstTexture->isDepth &&
			srcTexture->GetDesc().SampleDesc.Count <= 1 && dstTexture->GetDesc().SampleDesc.Count <= 1 &&
			AreD3D12CopyFormatsCompatible(srcTexture->GetDesc().Format, dstTexture->GetDesc().Format))
		{
			QueueRecording([=, this]() { texture_copyImageSubData(src, srcMip, effectiveSrcX, effectiveSrcY, srcSlice, dst, dstMip, effectiveDstX, effectiveDstY,
				dstSlice, effectiveCopyWidth, effectiveCopyHeight, srcDepth); });
			return;
		}
		// a single-slice copy that may fall back to the surface-copy draw: its views and the GPU write tracking are
		// prepared here, the draw (or the plain copy) runs on the recorder
		auto inRange = [](const LatteTextureD3D12* texture, sint32 mip, sint32 slice)
		{
			if (!texture || !texture->GetResource() || mip < 0 || mip >= (sint32)texture->GetDesc().MipLevels || slice < 0)
				return false;
			const auto& desc = texture->GetDesc();
			const uint32 depth = desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ?
				std::max<uint32>(1, (uint32)desc.DepthOrArraySize >> (uint32)mip) : std::max<uint32>(1, (uint32)desc.DepthOrArraySize);
			return (uint32)slice < depth;
		};
		if (srcDepth <= 1 && inRange(srcTexture, srcMip, srcSlice) && inRange(dstTexture, dstMip, dstSlice))
		{
			auto* sourceView = static_cast<LatteTextureViewD3D12*>(src->Is3DTexture() ?
				src->GetOrCreateView(Latte::E_DIM::DIM_2D, src->format, srcMip, 1, srcSlice, 1) :
				src->GetOrCreateView(srcMip, 1, srcSlice, 1));
			auto* destinationView = static_cast<LatteTextureViewD3D12*>(dst->GetOrCreateView(dstMip, 1, dstSlice, 1));
			if (sourceView && destinationView)
			{
				LatteTexture_TrackTextureGPUWrite(dst, dstSlice, dstMip, LatteTexture_getNextUpdateEventCounter());
				QueueRecording([=, this]()
				{
					m_preparedSurfaceCopy = { src, srcMip, srcSlice, dst, dstMip, dstSlice, sourceView, destinationView };
					stdx::scope_exit clearPrepared([this]() { m_preparedSurfaceCopy = {}; });
					texture_copyImageSubData(src, srcMip, effectiveSrcX, effectiveSrcY, srcSlice, dst, dstMip, effectiveDstX, effectiveDstY,
						dstSlice, effectiveCopyWidth, effectiveCopyHeight, srcDepth);
				});
				return;
			}
		}
	}
	D3D12_EXCLUSIVE_RECORDING();
	if (dst)
		DropRetainedFeedbackCopies(dst);
	stdx::scope_exit deviceCheck([&]() { CheckDeviceAfterOperation("texture copy", [&]() {
		return fmt::format("src {} mip={} slice={} at {},{} -> dst {} mip={} slice={} at {},{} size {}x{}x{}", D3D12DescribeResource(src ? static_cast<LatteTextureD3D12*>(src)->GetResource() : nullptr), srcMip, srcSlice,
			effectiveSrcX, effectiveSrcY, D3D12DescribeResource(dst ? static_cast<LatteTextureD3D12*>(dst)->GetResource() : nullptr), dstMip, dstSlice, effectiveDstX, effectiveDstY, effectiveCopyWidth, effectiveCopyHeight, srcDepth); }); });
	auto* srcTexture = static_cast<LatteTextureD3D12*>(src);
	auto* dstTexture = static_cast<LatteTextureD3D12*>(dst);
	if (!srcTexture || !dstTexture || !srcTexture->GetResource() || !dstTexture->GetResource())
		return;
	m_gpuWatchdog.RecordOperation(D3D12GpuWatchdog::Operation::SurfaceCopy);

	const bool srcIs3D = srcTexture->GetDesc().Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D;
	const bool dstIs3D = dstTexture->GetDesc().Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D;
	const auto& srcDesc = srcTexture->GetDesc();
	const auto& dstDesc = dstTexture->GetDesc();
	if (srcMip < 0 || srcMip >= (sint32)srcDesc.MipLevels ||
		dstMip < 0 || dstMip >= (sint32)dstDesc.MipLevels ||
		effectiveSrcX < 0 || effectiveSrcY < 0 || effectiveDstX < 0 || effectiveDstY < 0 ||
		effectiveCopyWidth <= 0 || effectiveCopyHeight <= 0)
	{
		D3D12TraceEvent("texture_copy_skipped", "reason=invalid_region srcMip={} dstMip={} srcXY={},{} dstXY={},{} size={}x{}",
			srcMip, dstMip, effectiveSrcX, effectiveSrcY, effectiveDstX, effectiveDstY,
			effectiveCopyWidth, effectiveCopyHeight);
		return;
	}
	const uint32 srcMipWidth = std::max<uint32>(1, (uint32)(srcDesc.Width >> srcMip));
	const uint32 srcMipHeight = std::max<uint32>(1, (uint32)(srcDesc.Height >> srcMip));
	const uint32 dstMipWidth = std::max<uint32>(1, (uint32)(dstDesc.Width >> dstMip));
	const uint32 dstMipHeight = std::max<uint32>(1, (uint32)(dstDesc.Height >> dstMip));
	const bool srcCompressed = srcTexture->GetFormatInfo().isCompressed;
	const bool dstCompressed = dstTexture->GetFormatInfo().isCompressed;
	// the copy size is in source texels; one uncompressed texel aliases one 4x4 block
	auto getDestinationExtent = [&](sint32 sourceExtent, sint32 destinationOffset, uint32 destinationMipExtent) -> uint64
	{
		if (srcCompressed && !dstCompressed)
			return (uint64)((sourceExtent + 3) / 4);
		if (!srcCompressed && dstCompressed)
		{
			const uint64 blockExtent = (uint64)sourceExtent * 4;
			const uint64 remaining = destinationMipExtent > (uint32)destinationOffset ? destinationMipExtent - (uint32)destinationOffset : 0;
			return blockExtent > remaining && blockExtent - remaining < 4 ? remaining : blockExtent;
		}
		return (uint64)sourceExtent;
	};
	const uint64 dstCopyWidth = getDestinationExtent(effectiveCopyWidth, effectiveDstX, dstMipWidth);
	const uint64 dstCopyHeight = getDestinationExtent(effectiveCopyHeight, effectiveDstY, dstMipHeight);
	if ((uint64)effectiveSrcX + (uint64)effectiveCopyWidth > srcMipWidth ||
		(uint64)effectiveSrcY + (uint64)effectiveCopyHeight > srcMipHeight ||
		(uint64)effectiveDstX + dstCopyWidth > dstMipWidth ||
		(uint64)effectiveDstY + dstCopyHeight > dstMipHeight)
	{
		D3D12TraceEvent("texture_copy_skipped", "reason=region_oob srcExtent={}x{} dstExtent={}x{} srcXY={},{} dstXY={},{} size={}x{}",
			srcMipWidth, srcMipHeight, dstMipWidth, dstMipHeight,
			effectiveSrcX, effectiveSrcY, effectiveDstX, effectiveDstY,
			effectiveCopyWidth, effectiveCopyHeight);
		return;
	}
	const sint32 requestedCopyDepth = std::max<sint32>(srcDepth, 1);
	auto getRemainingDepth = [](const LatteTextureD3D12* texture, sint32 mip, sint32 slice) -> sint32
	{
		if (!texture || mip < 0 || mip >= (sint32)texture->GetDesc().MipLevels || slice < 0)
			return 0;

		const auto& desc = texture->GetDesc();
		if (desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D)
		{
			const uint32 mipDepth = std::max<uint32>(1, (uint32)desc.DepthOrArraySize >> (uint32)mip);
			if ((uint32)slice >= mipDepth)
				return 0;
			return (sint32)(mipDepth - (uint32)slice);
		}

		const uint32 arraySize = std::max<uint32>(1, (uint32)desc.DepthOrArraySize);
		if ((uint32)slice >= arraySize)
			return 0;
		return (sint32)(arraySize - (uint32)slice);
	};
	auto getD3DSubresourceIndex = [](const LatteTextureD3D12* texture, bool is3DResource, sint32 slice, sint32 mip) -> uint32
	{
		if (is3DResource)
			return (uint32)std::max<sint32>(mip, 0);

		const uint32 mipLevels = std::max<uint32>(1, (uint32)texture->GetDesc().MipLevels);
		return (uint32)std::max<sint32>(mip, 0) + (uint32)std::max<sint32>(slice, 0) * mipLevels;
	};
	const sint32 maxSrcCopyDepth = getRemainingDepth(srcTexture, srcMip, srcSlice);
	const sint32 maxDstCopyDepth = getRemainingDepth(dstTexture, dstMip, dstSlice);
	const sint32 copyDepth = std::min(requestedCopyDepth, std::min(maxSrcCopyDepth, maxDstCopyDepth));
	if (copyDepth <= 0)
	{
		static uint32 s_invalidDepthCopyLogCount = 0;
		if (D3D12RuntimeLogCapped(s_invalidDepthCopyLogCount, "invalid texture copy depth", 64))
		{
			cemuLog_log(LogType::Force,
						 "D3D12: skipping texture copy outside source/destination depth bounds src=0x{:08x}/mip={}/slice={}/remaining={} dst=0x{:08x}/mip={}/slice={}/remaining={} requestedDepth={}.",
						 srcTexture->physAddress,
						 srcMip,
						 srcSlice,
						 maxSrcCopyDepth,
						 dstTexture->physAddress,
						 dstMip,
						 dstSlice,
						 maxDstCopyDepth,
						 requestedCopyDepth);
		}
		D3D12TraceEvent("texture_copy_skipped", "reason=depth_oob src=0x{:08x} srcMip={} srcSlice={} srcRemaining={} dst=0x{:08x} dstMip={} dstSlice={} dstRemaining={} requestedDepth={}",
						srcTexture->physAddress,
						srcMip,
						srcSlice,
						maxSrcCopyDepth,
						dstTexture->physAddress,
						dstMip,
						dstSlice,
						maxDstCopyDepth,
						requestedCopyDepth);
		return;
	}
	if (copyDepth != requestedCopyDepth)
	{
		static uint32 s_truncatedDepthCopyLogCount = 0;
		if (D3D12RuntimeLogCapped(s_truncatedDepthCopyLogCount, "truncated texture copy depth", 64))
		{
			cemuLog_log(LogType::Force,
						 "D3D12: clamping texture copy depth from {} to {} src=0x{:08x}/mip={}/slice={}/remaining={} dst=0x{:08x}/mip={}/slice={}/remaining={}.",
						 requestedCopyDepth,
						 copyDepth,
						 srcTexture->physAddress,
						 srcMip,
						 srcSlice,
						 maxSrcCopyDepth,
						 dstTexture->physAddress,
						 dstMip,
						 dstSlice,
						 maxDstCopyDepth);
		}
	}

	const bool copySliceBySlice = copyDepth > 1 && (!srcIs3D || !dstIs3D);
	const uint32 srcSubresource = getD3DSubresourceIndex(srcTexture, srcIs3D, srcSlice, srcMip);
	const uint32 dstSubresource = getD3DSubresourceIndex(dstTexture, dstIs3D, dstSlice, dstMip);
	const bool sameD3DResource = srcTexture->GetResource() == dstTexture->GetResource();
	const bool sameLatteAddress = srcTexture->physAddress == dstTexture->physAddress;
	if (requestedCopyDepth > 1)
	{
		static uint32 s_multiDepthCopyLogCount = 0;
		if (D3D12RuntimeLogCapped(s_multiDepthCopyLogCount, "multi-depth texture copy", 128))
		{
			cemuLog_log(LogType::Force,
						 "D3D12: multi-depth texture copy requested={} effective={} split={} src=0x{:08x}/mip={}/slice={}/d3dDim={}/depthOrArray={} dst=0x{:08x}/mip={}/slice={}/d3dDim={}/depthOrArray={}.",
						 requestedCopyDepth,
						 copyDepth,
						 copySliceBySlice ? 1 : 0,
						 srcTexture->physAddress,
						 srcMip,
						 srcSlice,
						 (uint32)srcTexture->GetDesc().Dimension,
						 (uint32)srcTexture->GetDesc().DepthOrArraySize,
						 dstTexture->physAddress,
						 dstMip,
						 dstSlice,
						 (uint32)dstTexture->GetDesc().Dimension,
						 (uint32)dstTexture->GetDesc().DepthOrArraySize);
		}
	}
	static uint32 s_copyTextureLogCount = 0;
	if (D3D12RuntimeLogCapped(s_copyTextureLogCount, "texture_copyImageSubData", 128))
	{
		cemuLog_log(LogType::Force, "D3D12: copy texture src=0x{:08x}/fmt=0x{:04x}/mip={}/slice={} dst=0x{:08x}/fmt=0x{:04x}/mip={}/slice={} srcXY=({}, {}) dstXY=({}, {}) size={}x{}x{} sameAddr={} sameResource={} srcObj={} dstObj={}.",
					srcTexture->physAddress, (uint32)srcTexture->format, srcMip, srcSlice,
					dstTexture->physAddress, (uint32)dstTexture->format, dstMip, dstSlice,
					effectiveSrcX, effectiveSrcY, effectiveDstX, effectiveDstY,
					effectiveCopyWidth, effectiveCopyHeight, copyDepth,
					sameLatteAddress,
					sameD3DResource,
					(const void*)srcTexture,
					(const void*)dstTexture);
	}
	D3D12TraceEvent("texture_copy_begin", "src=0x{:08x} srcFmt=0x{:04x} srcDxgi={} srcMip={} srcSlice={} dst=0x{:08x} dstFmt=0x{:04x} dstDxgi={} dstMip={} dstSlice={} srcXY={},{} dstXY={},{} size={}x{}x{} sameAddr={} sameResource={} srcObj={} dstObj={}",
					srcTexture->physAddress,
					(uint32)srcTexture->format,
					(uint32)srcTexture->GetDesc().Format,
					srcMip,
					srcSlice,
					dstTexture->physAddress,
					(uint32)dstTexture->format,
					(uint32)dstTexture->GetDesc().Format,
					dstMip,
					dstSlice,
					effectiveSrcX,
					effectiveSrcY,
					effectiveDstX,
					effectiveDstY,
					effectiveCopyWidth,
					effectiveCopyHeight,
					copyDepth,
					sameLatteAddress ? 1 : 0,
					sameD3DResource ? 1 : 0,
					(const void*)srcTexture,
					(const void*)dstTexture);
	if (sameD3DResource &&
		srcMip == dstMip &&
		srcSlice == dstSlice &&
		effectiveSrcX == effectiveDstX &&
		effectiveSrcY == effectiveDstY)
	{
		static uint32 s_sameTextureNoopCopyLogCount = 0;
		if (D3D12RuntimeLogCapped(s_sameTextureNoopCopyLogCount, "same-resource texture no-op copy", 64))
		{
			cemuLog_log(LogType::Force,
						 "D3D12: skipping exact same-resource texture copy src=dst=0x{:08x}/fmt=0x{:04x}/subresource={} xy=({}, {}) size={}x{}.",
						 srcTexture->physAddress,
						 (uint32)srcTexture->format,
						 srcSubresource,
						 effectiveSrcX,
						 effectiveSrcY,
						 effectiveCopyWidth,
						 effectiveCopyHeight);
		}
		D3D12TraceEvent("texture_copy_noop", "addr=0x{:08x} fmt=0x{:04x} subresource={} xy={},{} size={}x{}",
						srcTexture->physAddress,
						(uint32)srcTexture->format,
						srcSubresource,
						effectiveSrcX,
						effectiveSrcY,
						effectiveCopyWidth,
						effectiveCopyHeight);
		return;
	}
	if (!m_commandListOpen)
		ResetCommandList();

	D3D12_TEXTURE_COPY_LOCATION source{};
	source.pResource = srcTexture->GetResource();
	source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	source.SubresourceIndex = srcSubresource;

	D3D12_TEXTURE_COPY_LOCATION destination{};
	destination.pResource = dstTexture->GetResource();
	destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	destination.SubresourceIndex = dstSubresource;

	D3D12_BOX sourceBox{};
	sourceBox.left = (UINT)std::max<sint32>(effectiveSrcX, 0);
	sourceBox.top = (UINT)std::max<sint32>(effectiveSrcY, 0);
	sourceBox.front = srcIs3D ? (UINT)std::max<sint32>(srcSlice, 0) : 0;
	sourceBox.right = sourceBox.left + (UINT)std::max<sint32>(effectiveCopyWidth, 1);
	sourceBox.bottom = sourceBox.top + (UINT)std::max<sint32>(effectiveCopyHeight, 1);
	sourceBox.back = sourceBox.front + (UINT)((srcIs3D && !copySliceBySlice) ? copyDepth : 1);

	const uint32 dstX = (uint32)effectiveDstX;
	const uint32 dstY = (uint32)effectiveDstY;
	// block-compressed copies into uncompressed formats and back are staged through a buffer
	const bool compressedCopy = srcCompressed && dstCompressed;
	const sint32 sourceSubresourceDepth = srcIs3D && !copySliceBySlice ? copyDepth : 1;
	const sint32 destinationSubresourceDepth = dstIs3D && !copySliceBySlice ? copyDepth : 1;
	const bool fullSubresourceCopy =
		IsFullSubresourceTextureCopy(srcTexture, srcMip,
			effectiveSrcX, effectiveSrcY, srcIs3D ? srcSlice : 0,
			effectiveCopyWidth, effectiveCopyHeight, sourceSubresourceDepth) &&
		IsFullSubresourceTextureCopy(dstTexture, dstMip,
			effectiveDstX, effectiveDstY, dstIs3D ? dstSlice : 0,
			effectiveCopyWidth, effectiveCopyHeight, destinationSubresourceDepth);
	if (compressedCopy && !fullSubresourceCopy &&
		!IsValidBCTextureCopyRegion(srcTexture, srcMip, dstTexture, dstMip,
			effectiveSrcX, effectiveSrcY, effectiveDstX, effectiveDstY, effectiveCopyWidth, effectiveCopyHeight))
	{
		static uint32 s_unalignedBcCopyLogCount = 0;
		if (D3D12RuntimeLogCapped(s_unalignedBcCopyLogCount, "unaligned compressed texture copy", 64))
		{
			cemuLog_log(LogType::Force,
						 "D3D12: skipping unaligned BC texture copy src=0x{:08x}/fmt=0x{:04x}/mip={} dst=0x{:08x}/fmt=0x{:04x}/mip={} srcXY=({}, {}) dstXY=({}, {}) size={}x{}.",
						 srcTexture->physAddress,
						 (uint32)srcTexture->format,
						 srcMip,
						 dstTexture->physAddress,
						 (uint32)dstTexture->format,
						 dstMip,
						 effectiveSrcX,
						 effectiveSrcY,
						 effectiveDstX,
						 effectiveDstY,
						 effectiveCopyWidth,
						 effectiveCopyHeight);
		}
		D3D12TraceEvent("texture_copy_skipped", "reason=unaligned_bc src=0x{:08x} dst=0x{:08x} srcXY={},{} dstXY={},{} size={}x{}",
						srcTexture->physAddress,
						dstTexture->physAddress,
						effectiveSrcX,
						effectiveSrcY,
						effectiveDstX,
						effectiveDstY,
						effectiveCopyWidth,
						effectiveCopyHeight);
		return;
	}
	const bool depthStencilCopy = srcTexture->isDepth || dstTexture->isDepth;
	const bool multisampledCopy = srcDesc.SampleDesc.Count > 1 || dstDesc.SampleDesc.Count > 1;
	if (srcDesc.SampleDesc.Count != dstDesc.SampleDesc.Count ||
		srcDesc.SampleDesc.Quality != dstDesc.SampleDesc.Quality)
	{
		static uint32 s_invalidWholeSubresourceCopyLogCount = 0;
		if (D3D12RuntimeLogCapped(s_invalidWholeSubresourceCopyLogCount, "incompatible multisampled texture copy", 64))
		{
			cemuLog_log(LogType::Force,
				"D3D12: rejecting texture copy with incompatible sample descriptions (src=0x{:08x}, dst=0x{:08x}, samples={}:{}/{}:{}).",
				srcTexture->physAddress, dstTexture->physAddress,
				srcDesc.SampleDesc.Count, srcDesc.SampleDesc.Quality,
				dstDesc.SampleDesc.Count, dstDesc.SampleDesc.Quality);
		}
		D3D12TraceEvent("texture_copy_skipped", "reason=sample_mismatch src=0x{:08x} dst=0x{:08x} srcSamples={}:{} dstSamples={}:{}",
			srcTexture->physAddress, dstTexture->physAddress,
			srcDesc.SampleDesc.Count, srcDesc.SampleDesc.Quality,
			dstDesc.SampleDesc.Count, dstDesc.SampleDesc.Quality);
		return;
	}
	if (!AreD3D12CopyFormatsCompatible(srcTexture->GetDesc().Format, dstTexture->GetDesc().Format))
	{
		if (!srcTexture->isDepth && !dstTexture->isDepth && srcTexture->GetBPP() == dstTexture->GetBPP())
		{
			if (TryRawAliasTextureCopy(srcTexture, srcMip, srcSlice, effectiveSrcX, effectiveSrcY,
									   dstTexture, dstMip, dstSlice, effectiveDstX, effectiveDstY,
									   effectiveCopyWidth, effectiveCopyHeight, copyDepth))
			{
				return;
			}
			static bool s_loggedDroppedColorAliasCopy = false;
			if (!s_loggedDroppedColorAliasCopy)
			{
				s_loggedDroppedColorAliasCopy = true;
				cemuLog_log(LogType::Force, "D3D12: dropping texture copy between formats without a raw bit mapping (src fmt=0x{:04x}/dxgi={}, dst fmt=0x{:04x}/dxgi={}).",
					(uint32)srcTexture->format, (uint32)srcTexture->GetDesc().Format,
					(uint32)dstTexture->format, (uint32)dstTexture->GetDesc().Format);
			}
			static uint32 s_incompatibleColorAliasCopyLogCount = 0;
			if (D3D12RuntimeLogCapped(s_incompatibleColorAliasCopyLogCount, "unsupported incompatible color alias texture copy", 96))
			{
				cemuLog_log(LogType::Force,
							 "D3D12: incompatible color alias copy cannot use shader conversion src=0x{:08x}/dxgi={} dst=0x{:08x}/dxgi={} srcXY=({}, {}) dstXY=({}, {}) size={}x{} srcBpp={} dstBpp={}.",
							 srcTexture->physAddress,
							 (uint32)srcTexture->GetDesc().Format,
							 dstTexture->physAddress,
							 (uint32)dstTexture->GetDesc().Format,
							 effectiveSrcX,
							 effectiveSrcY,
							 effectiveDstX,
							 effectiveDstY,
							 effectiveCopyWidth,
							 effectiveCopyHeight,
							 srcTexture->GetBPP(),
							 dstTexture->GetBPP());
			}
			D3D12TraceEvent("texture_copy_skipped", "reason=incompatible_color_alias src=0x{:08x} srcDxgi={} dst=0x{:08x} dstDxgi={} srcXY={},{} dstXY={},{} size={}x{} srcBpp={} dstBpp={}",
							srcTexture->physAddress,
							(uint32)srcTexture->GetDesc().Format,
							dstTexture->physAddress,
							(uint32)dstTexture->GetDesc().Format,
							effectiveSrcX,
							effectiveSrcY,
							effectiveDstX,
							effectiveDstY,
							effectiveCopyWidth,
							effectiveCopyHeight,
							srcTexture->GetBPP(),
							dstTexture->GetBPP());
			return;
		}
		if (copyDepth == 1)
		{
			static uint32 s_incompatibleCopyDrawLogCount = 0;
			if (D3D12RuntimeLogCapped(s_incompatibleCopyDrawLogCount, "incompatible texture copy conversion", 96))
			{
				cemuLog_log(LogType::Force,
							 "D3D12: routing incompatible texture copy through surface-copy draw src=0x{:08x}/dxgi={} dst=0x{:08x}/dxgi={} srcXY=({}, {}) dstXY=({}, {}) size={}x{}.",
							 srcTexture->physAddress,
							 (uint32)srcTexture->GetDesc().Format,
							 dstTexture->physAddress,
							 (uint32)dstTexture->GetDesc().Format,
							 effectiveSrcX,
							 effectiveSrcY,
							 effectiveDstX,
							 effectiveDstY,
							 effectiveCopyWidth,
							 effectiveCopyHeight);
			}
			D3D12TraceEvent("texture_copy_reroute_surface", "src=0x{:08x} srcDxgi={} dst=0x{:08x} dstDxgi={} srcXY={},{} dstXY={},{} size={}x{} sameAddr={} sameResource={}",
							srcTexture->physAddress,
							(uint32)srcTexture->GetDesc().Format,
							dstTexture->physAddress,
							(uint32)dstTexture->GetDesc().Format,
							effectiveSrcX,
							effectiveSrcY,
							effectiveDstX,
							effectiveDstY,
							effectiveCopyWidth,
							effectiveCopyHeight,
							sameLatteAddress ? 1 : 0,
							sameD3DResource ? 1 : 0);
			SurfaceCopyWithFormatConversion(srcTexture, srcMip, srcSlice, dstTexture, dstMip, dstSlice,
											effectiveSrcX, effectiveSrcY, effectiveDstX, effectiveDstY,
											effectiveCopyWidth, effectiveCopyHeight, true);
		}
		else if (!srcIs3D && !dstIs3D)
		{
			for (sint32 slice = 0; slice < copyDepth; ++slice)
			{
				SurfaceCopyWithFormatConversion(srcTexture, srcMip, srcSlice + slice, dstTexture, dstMip, dstSlice + slice,
												effectiveSrcX, effectiveSrcY, effectiveDstX, effectiveDstY,
												effectiveCopyWidth, effectiveCopyHeight, true);
			}
		}
		else
		{
			static uint32 s_incompatibleCopyUnsupportedLogCount = 0;
			if (D3D12RuntimeLogCapped(s_incompatibleCopyUnsupportedLogCount, "unsupported incompatible texture copy", 64))
			{
				cemuLog_log(LogType::Force,
							 "D3D12: incompatible texture copy with depth > 1 needs 3D shader conversion support src=0x{:08x}/dxgi={} dst=0x{:08x}/dxgi={} srcXY=({}, {}) dstXY=({}, {}) size={}x{}x{}.",
							 srcTexture->physAddress,
							 (uint32)srcTexture->GetDesc().Format,
							 dstTexture->physAddress,
							 (uint32)dstTexture->GetDesc().Format,
							 effectiveSrcX,
							 effectiveSrcY,
							 effectiveDstX,
							 effectiveDstY,
							 effectiveCopyWidth,
							 effectiveCopyHeight,
							 copyDepth);
			}
			D3D12TraceEvent("texture_copy_skipped", "reason=incompatible_depth src=0x{:08x} srcDxgi={} dst=0x{:08x} dstDxgi={} srcXY={},{} dstXY={},{} size={}x{}x{}",
							srcTexture->physAddress,
							(uint32)srcTexture->GetDesc().Format,
							dstTexture->physAddress,
							(uint32)dstTexture->GetDesc().Format,
							effectiveSrcX,
							effectiveSrcY,
							effectiveDstX,
							effectiveDstY,
							effectiveCopyWidth,
							effectiveCopyHeight,
							copyDepth);
		}
		return;
	}

	const bool requiresWholeSubresource = depthStencilCopy || multisampledCopy;
	if (requiresWholeSubresource && !fullSubresourceCopy)
	{
		// CopyTextureRegion only takes whole depth-stencil subresources; draw the region instead
		if (!multisampledCopy && srcTexture->isDepth && dstTexture->isDepth && !sameD3DResource && !srcIs3D && !dstIs3D)
		{
			for (sint32 slice = 0; slice < copyDepth; ++slice)
			{
				SurfaceCopyWithFormatConversion(srcTexture, srcMip, srcSlice + slice, dstTexture, dstMip, dstSlice + slice,
												effectiveSrcX, effectiveSrcY, effectiveDstX, effectiveDstY,
												effectiveCopyWidth, effectiveCopyHeight, true, false);
			}
			return;
		}
		static uint32 s_invalidWholeSubresourceCopyLogCount = 0;
		if (D3D12RuntimeLogCapped(s_invalidWholeSubresourceCopyLogCount, "invalid depth-stencil or multisampled texture copy", 64))
		{
			cemuLog_log(LogType::Force,
				"D3D12: rejecting depth-stencil/MSAA CopyTextureRegion that is not a whole-subresource copy (src=0x{:08x}, dst=0x{:08x}, samples={}:{}/{}:{}).",
				srcTexture->physAddress, dstTexture->physAddress,
				srcDesc.SampleDesc.Count, srcDesc.SampleDesc.Quality,
				dstDesc.SampleDesc.Count, dstDesc.SampleDesc.Quality);
		}
		D3D12TraceEvent("texture_copy_skipped", "reason=whole_subresource_required src=0x{:08x} dst=0x{:08x} srcSamples={}:{} dstSamples={}:{}",
			srcTexture->physAddress, dstTexture->physAddress,
			srcDesc.SampleDesc.Count, srcDesc.SampleDesc.Quality,
			dstDesc.SampleDesc.Count, dstDesc.SampleDesc.Quality);
		return;
	}

	if (depthStencilCopy && srcTexture->GetFormatInfo().hasStencil != dstTexture->GetFormatInfo().hasStencil)
	{
		D3D12TraceEvent("texture_copy_skipped", "reason=depth_stencil_plane_mismatch src=0x{:08x} dst=0x{:08x}",
			srcTexture->physAddress, dstTexture->physAddress);
		return;
	}

	struct TextureCopyOperation
	{
		uint32 sourceSubresource{};
		uint32 destinationSubresource{};
		D3D12_BOX sourceBox{};
		UINT destinationX{};
		UINT destinationY{};
		UINT destinationZ{};
		bool wholeSubresource{};
	};
	const sint32 d3dCopyCount = copySliceBySlice ? copyDepth : 1;
	const uint32 planeCount = depthStencilCopy && srcTexture->GetFormatInfo().hasStencil ? 2u : 1u;
	const uint32 srcPlaneStride = std::max<uint32>(srcDesc.MipLevels, 1) *
		(srcIs3D ? 1u : std::max<uint32>(srcTexture->GetArraySize(), 1));
	const uint32 dstPlaneStride = std::max<uint32>(dstDesc.MipLevels, 1) *
		(dstIs3D ? 1u : std::max<uint32>(dstTexture->GetArraySize(), 1));
	const bool useWholeSubresourceCopy = fullSubresourceCopy && (compressedCopy || requiresWholeSubresource);
	std::vector<TextureCopyOperation> copyOperations;
	copyOperations.reserve((size_t)d3dCopyCount * planeCount);
	for (sint32 depthIndex = 0; depthIndex < d3dCopyCount; ++depthIndex)
	{
		D3D12_BOX operationSourceBox = sourceBox;
		uint32 operationSrcSubresource = source.SubresourceIndex;
		uint32 operationDstSubresource = destination.SubresourceIndex;
		UINT operationDstZ = dstIs3D ? (UINT)dstSlice : 0u;

		if (copySliceBySlice)
		{
			if (srcIs3D)
			{
				operationSourceBox.front = (UINT)(srcSlice + depthIndex);
				operationSourceBox.back = operationSourceBox.front + 1;
			}
			else
			{
				operationSrcSubresource = getD3DSubresourceIndex(srcTexture, false, srcSlice + depthIndex, srcMip);
				operationSourceBox.front = 0;
				operationSourceBox.back = 1;
			}

			if (dstIs3D)
				operationDstZ = (UINT)(dstSlice + depthIndex);
			else
				operationDstSubresource = getD3DSubresourceIndex(dstTexture, false, dstSlice + depthIndex, dstMip);
		}

		for (uint32 plane = 0; plane < planeCount; ++plane)
		{
			TextureCopyOperation operation;
			operation.sourceSubresource = operationSrcSubresource + plane * srcPlaneStride;
			operation.destinationSubresource = operationDstSubresource + plane * dstPlaneStride;
			operation.sourceBox = operationSourceBox;
			operation.destinationX = dstX;
			operation.destinationY = dstY;
			operation.destinationZ = operationDstZ;
			operation.wholeSubresource = useWholeSubresourceCopy;
			copyOperations.emplace_back(operation);
		}
	}

	std::vector<uint32> sourceCopySubresources;
	std::vector<uint32> destinationCopySubresources;
	sourceCopySubresources.reserve(copyOperations.size());
	destinationCopySubresources.reserve(copyOperations.size());
	for (const auto& operation : copyOperations)
	{
		if (std::find(sourceCopySubresources.begin(), sourceCopySubresources.end(), operation.sourceSubresource) == sourceCopySubresources.end())
			sourceCopySubresources.push_back(operation.sourceSubresource);
		if (std::find(destinationCopySubresources.begin(), destinationCopySubresources.end(), operation.destinationSubresource) == destinationCopySubresources.end())
			destinationCopySubresources.push_back(operation.destinationSubresource);
	}
	const TextureCopyCacheKey textureCopyKey
	{
		srcTexture->GetContentIdentity(), dstTexture->GetContentIdentity(),
		srcMip, effectiveSrcX, effectiveSrcY, srcSlice,
		dstMip, effectiveDstX, effectiveDstY, dstSlice,
		effectiveCopyWidth, effectiveCopyHeight, copyDepth, 0
	};
	if (CanSkipTextureCopy(textureCopyKey, srcTexture, dstTexture, sourceCopySubresources, destinationCopySubresources))
	{
		m_textureCopyCacheHits++;
		const uint64 bytesPerTexel = std::max<uint64>((srcTexture->GetBPP() + 7) / 8, 1);
		m_textureCopyBytesSkipped += (uint64)effectiveCopyWidth * effectiveCopyHeight * copyDepth * bytesPerTexel;
		D3D12TraceEvent("texture_copy_generation_hit", "src=0x{:08x} dst=0x{:08x} size={}x{}x{} operations={}",
			srcTexture->physAddress, dstTexture->physAddress, effectiveCopyWidth, effectiveCopyHeight, copyDepth, copyOperations.size());
		return;
	}
	m_textureCopyCacheMisses++;

	const bool requiresScratchCopy = sameD3DResource && std::ranges::any_of(copyOperations,
		[&](const TextureCopyOperation& sourceOperation)
		{
			return std::ranges::any_of(copyOperations,
				[&](const TextureCopyOperation& destinationOperation)
				{
					return sourceOperation.sourceSubresource == destinationOperation.destinationSubresource;
				});
		});

	struct SavedTextureState
	{
		LatteTextureD3D12* texture{};
		uint32 subresource{};
		D3D12_RESOURCE_STATES state{};
	};
	std::vector<SavedTextureState> savedStates;
	auto canonicalStateTexture = [&](LatteTextureD3D12* texture)
	{
		return sameD3DResource ? srcTexture : texture;
	};
	auto transitionManyForCopy = [&](LatteTextureD3D12* texture, const std::vector<uint32>& subresources, D3D12_RESOURCE_STATES state)
	{
		texture = canonicalStateTexture(texture);
		std::vector<TextureTransition> transitions;
		transitions.reserve(subresources.size());
		for (uint32 subresource : subresources)
		{
			const auto saved = std::ranges::find_if(savedStates,
				[&](const SavedTextureState& entry)
				{
					return entry.texture == texture && entry.subresource == subresource;
				});
			if (saved == savedStates.end())
				savedStates.push_back({ texture, subresource, texture->GetSubresourceState(subresource) });
			if (std::ranges::none_of(transitions, [subresource](const TextureTransition& transition)
				{ return transition.subresource == subresource; }))
			{
				transitions.push_back({ subresource, state });
			}
		}
		TransitionTextureSubresources(texture, transitions);
	};
	auto restoreCopyStates = [&]()
	{
		std::vector<LatteTextureD3D12*> textures;
		for (const auto& saved : savedStates)
		{
			if (std::find(textures.begin(), textures.end(), saved.texture) == textures.end())
				textures.push_back(saved.texture);
		}
		for (auto* texture : textures)
		{
			std::vector<TextureTransition> transitions;
			for (const auto& saved : savedStates)
			{
				if (saved.texture == texture)
					transitions.push_back({ saved.subresource, saved.state });
			}
			TransitionTextureSubresources(texture, transitions);
		}
		if (sameD3DResource && srcTexture != dstTexture)
		{
			for (const auto& saved : savedStates)
			{
				if (saved.subresource < dstTexture->GetSubresourceCount())
					dstTexture->SetSubresourceState(saved.subresource, saved.state);
			}
		}
	};

	if (requiresScratchCopy)
	{
		D3D12MemoryManager::Resource scratchTexture;
		const D3D12_RESOURCE_DESC scratchDesc = srcTexture->GetDesc();
		const bool scratchReady = TryAcquireCommandScratchTexture(scratchDesc, scratchTexture) ||
			CreateTextureWithRecovery(scratchDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, scratchTexture, "same-resource texture copy");
		if (!scratchReady)
		{
			cemuLog_log(LogType::Force,
				"D3D12: failed to allocate scratch texture for same-resource copy at 0x{:08x}.",
				srcTexture->physAddress);
			return;
		}

		auto* commandList = GetCurrentCommandSlot().commandList.Get();
		Transition(scratchTexture.resource.Get(), scratchTexture.state, D3D12_RESOURCE_STATE_COPY_DEST);
		transitionManyForCopy(srcTexture, sourceCopySubresources, D3D12_RESOURCE_STATE_COPY_SOURCE);
		for (const auto& operation : copyOperations)
		{
			D3D12_TEXTURE_COPY_LOCATION stageSource{};
			stageSource.pResource = srcTexture->GetResource();
			stageSource.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
			stageSource.SubresourceIndex = operation.sourceSubresource;
			D3D12_TEXTURE_COPY_LOCATION stageDestination{};
			stageDestination.pResource = scratchTexture.resource.Get();
			stageDestination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
			stageDestination.SubresourceIndex = operation.sourceSubresource;
			commandList->CopyTextureRegion(
				&stageDestination,
				operation.wholeSubresource ? 0 : operation.sourceBox.left,
				operation.wholeSubresource ? 0 : operation.sourceBox.top,
				operation.wholeSubresource ? 0 : operation.sourceBox.front,
				&stageSource,
				operation.wholeSubresource ? nullptr : &operation.sourceBox);
		}

		Transition(scratchTexture.resource.Get(), scratchTexture.state, D3D12_RESOURCE_STATE_COPY_SOURCE);
		transitionManyForCopy(dstTexture, destinationCopySubresources, D3D12_RESOURCE_STATE_COPY_DEST);
		for (const auto& operation : copyOperations)
		{
			D3D12_TEXTURE_COPY_LOCATION scratchSource{};
			scratchSource.pResource = scratchTexture.resource.Get();
			scratchSource.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
			scratchSource.SubresourceIndex = operation.sourceSubresource;
			D3D12_TEXTURE_COPY_LOCATION copyDestination{};
			copyDestination.pResource = dstTexture->GetResource();
			copyDestination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
			copyDestination.SubresourceIndex = operation.destinationSubresource;
			commandList->CopyTextureRegion(
				&copyDestination,
				operation.wholeSubresource ? 0 : operation.destinationX,
				operation.wholeSubresource ? 0 : operation.destinationY,
				operation.wholeSubresource ? 0 : operation.destinationZ,
				&scratchSource,
				operation.wholeSubresource ? nullptr : &operation.sourceBox);
		}

		restoreCopyStates();
		RecycleCommandScratchTexture(std::move(scratchTexture), scratchDesc);
		D3D12TraceEvent("texture_copy_scratch", "resource={} srcMip={} srcSlice={} dstMip={} dstSlice={} copies={} planes={}",
			(const void*)srcTexture->GetResource(), srcMip, srcSlice, dstMip, dstSlice,
			copyOperations.size(), planeCount);
		RecordTextureCopy(textureCopyKey, srcTexture, dstTexture, sourceCopySubresources, destinationCopySubresources);
		return;
	}

	if (D3D12ShouldTrace())
	{
		auto* srcViewForTrace = static_cast<LatteTextureViewD3D12*>(srcTexture->GetOrCreateView(srcTexture->dim, srcTexture->format, srcMip, 1, srcSlice, 1));
		TraceTextureSnapshot("texture_copy_src_before", "src", srcViewForTrace, false,
			fmt::format("dst=0x{:08x} size={}x{}x{}", dstTexture->physAddress, effectiveCopyWidth, effectiveCopyHeight, copyDepth));
	}
	if (!m_commandListOpen)
		ResetCommandList();
	const auto srcStateBeforeCopy = srcTexture->GetSubresourceState(srcSubresource);
	const auto dstStateBeforeCopy = dstTexture->GetSubresourceState(dstSubresource);
	const uint32 dstZ = dstIs3D ? (uint32)dstSlice : 0u;
	D3D12TraceEvent("texture_copy_execute",
					"src=0x{:08x} srcFmt=0x{:04x} srcDxgi={} srcResource={} srcSubresource={} srcStateBefore=0x{:08x} dst=0x{:08x} dstFmt=0x{:04x} dstDxgi={} dstResource={} dstSubresource={} dstStateBefore=0x{:08x} dstXYZ={},{},{} boxLeftTopFront={},{},{} boxRightBottomBack={},{},{} copyBoxNull={} compressed={} fullSubresource={} d3dCopies={} sameAddr={} sameResource={} commandSlot={} nextFence={}",
					srcTexture->physAddress,
					(uint32)srcTexture->format,
					(uint32)srcTexture->GetDesc().Format,
					(const void*)srcTexture->GetResource(),
					srcSubresource,
					(uint32)srcStateBeforeCopy,
					dstTexture->physAddress,
					(uint32)dstTexture->format,
					(uint32)dstTexture->GetDesc().Format,
					(const void*)dstTexture->GetResource(),
					dstSubresource,
					(uint32)dstStateBeforeCopy,
					dstX,
					dstY,
					dstZ,
					sourceBox.left,
					sourceBox.top,
					sourceBox.front,
					sourceBox.right,
					sourceBox.bottom,
					sourceBox.back,
					useWholeSubresourceCopy ? 1 : 0,
					compressedCopy ? 1 : 0,
					fullSubresourceCopy ? 1 : 0,
					copyOperations.size(),
					sameLatteAddress ? 1 : 0,
					sameD3DResource ? 1 : 0,
					m_commandSlotIndex,
					(unsigned long long)m_nextFenceValue);
	transitionManyForCopy(srcTexture, sourceCopySubresources, D3D12_RESOURCE_STATE_COPY_SOURCE);
	transitionManyForCopy(dstTexture, destinationCopySubresources, D3D12_RESOURCE_STATE_COPY_DEST);

	auto* commandList = GetCurrentCommandSlot().commandList.Get();
	for (const auto& operation : copyOperations)
	{
		D3D12_TEXTURE_COPY_LOCATION copySource{};
		copySource.pResource = srcTexture->GetResource();
		copySource.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		copySource.SubresourceIndex = operation.sourceSubresource;
		D3D12_TEXTURE_COPY_LOCATION copyDestination{};
		copyDestination.pResource = dstTexture->GetResource();
		copyDestination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		copyDestination.SubresourceIndex = operation.destinationSubresource;
		commandList->CopyTextureRegion(
			&copyDestination,
			operation.wholeSubresource ? 0 : operation.destinationX,
			operation.wholeSubresource ? 0 : operation.destinationY,
			operation.wholeSubresource ? 0 : operation.destinationZ,
			&copySource,
			operation.wholeSubresource ? nullptr : &operation.sourceBox);
	}
	restoreCopyStates();
	RecordTextureCopy(textureCopyKey, srcTexture, dstTexture, sourceCopySubresources, destinationCopySubresources);
	if (D3D12ShouldTrace())
	{
		auto* dstViewForTrace = static_cast<LatteTextureViewD3D12*>(dstTexture->GetOrCreateView(dstTexture->dim, dstTexture->format, dstMip, 1, dstSlice, 1));
		RecordTextureLineage("texture_copy", dstViewForTrace, 0, 0, fmt::format("copySrc=0x{:08x}/fmt=0x{:04x}", srcTexture->physAddress, (uint32)srcTexture->format),
							 fmt::format("srcMip={} srcSlice={} dstMip={} dstSlice={} srcXY={},{} dstXY={},{} size={}x{}x{} fullSubresource={} compressed={}",
										 srcMip,
										 srcSlice,
										 dstMip,
										 dstSlice,
										 effectiveSrcX,
										 effectiveSrcY,
										 effectiveDstX,
										 effectiveDstY,
										 effectiveCopyWidth,
										 effectiveCopyHeight,
										 copyDepth,
										 fullSubresourceCopy ? 1 : 0,
										 compressedCopy ? 1 : 0));
		TraceTextureSnapshot("texture_copy_dst_after", "dst", dstViewForTrace, false, fmt::format("src=0x{:08x} fullSubresource={} compressed={}", srcTexture->physAddress, fullSubresourceCopy ? 1 : 0, compressedCopy ? 1 : 0));
	}
}

bool D3D12Renderer::TryRawAliasTextureCopy(LatteTextureD3D12* srcTexture, sint32 srcMip, sint32 srcSlice, sint32 srcX, sint32 srcY,
										   LatteTextureD3D12* dstTexture, sint32 dstMip, sint32 dstSlice, sint32 dstX, sint32 dstY,
										   sint32 width, sint32 height, sint32 depth)
{
	if (!srcTexture || !dstTexture || !m_memoryManager || !m_device || depth < 1 || width <= 0 || height <= 0)
		return false;
	if (srcTexture->isDepth || dstTexture->isDepth || !srcTexture->GetResource() || !dstTexture->GetResource())
		return false;
	const auto& srcDesc = srcTexture->GetDesc();
	const auto& dstDesc = dstTexture->GetDesc();
	if (srcMip < 0 || srcMip >= (sint32)srcDesc.MipLevels || dstMip < 0 || dstMip >= (sint32)dstDesc.MipLevels ||
		srcSlice < 0 || dstSlice < 0 || srcX < 0 || srcY < 0 || dstX < 0 || dstY < 0 ||
		srcDesc.SampleDesc.Count != 1 || dstDesc.SampleDesc.Count != 1)
		return false;
	const D3D12CopyElementLayout srcLayout = GetD3D12CopyElementLayout(srcDesc.Format);
	const D3D12CopyElementLayout dstLayout = GetD3D12CopyElementLayout(dstDesc.Format);
	if (srcLayout.bytes == 0 || srcLayout.bytes != dstLayout.bytes)
		return false;
	if (((uint32)srcX % srcLayout.blockSize) != 0 || ((uint32)srcY % srcLayout.blockSize) != 0 ||
		((uint32)dstX % dstLayout.blockSize) != 0 || ((uint32)dstY % dstLayout.blockSize) != 0)
		return false;

	const uint32 srcMipWidth = std::max<uint32>(1, (uint32)(srcDesc.Width >> srcMip));
	const uint32 srcMipHeight = std::max<uint32>(1, (uint32)(srcDesc.Height >> srcMip));
	const uint32 dstMipWidth = std::max<uint32>(1, (uint32)(dstDesc.Width >> dstMip));
	const uint32 dstMipHeight = std::max<uint32>(1, (uint32)(dstDesc.Height >> dstMip));
	if ((uint32)srcX >= srcMipWidth || (uint32)srcY >= srcMipHeight)
		return false;
	// the size is in source texels; the staging buffer holds whole texels or whole 4x4 blocks
	const uint32 elementsWide = ((uint32)width + srcLayout.blockSize - 1) / srcLayout.blockSize;
	const uint32 elementsHigh = ((uint32)height + srcLayout.blockSize - 1) / srcLayout.blockSize;
	const uint32 sourceBoxWidth = std::min<uint32>(elementsWide * srcLayout.blockSize, srcMipWidth - (uint32)srcX);
	const uint32 sourceBoxHeight = std::min<uint32>(elementsHigh * srcLayout.blockSize, srcMipHeight - (uint32)srcY);
	if (sourceBoxWidth < (uint32)width || sourceBoxHeight < (uint32)height)
		return false;
	const uint32 destinationWidth = elementsWide * dstLayout.blockSize;
	const uint32 destinationHeight = elementsHigh * dstLayout.blockSize;
	// a partial block may only overhang the edge of the destination mip
	if ((uint64)dstX + destinationWidth > AlignUpU32(dstMipWidth, dstLayout.blockSize) ||
		(uint64)dstY + destinationHeight > AlignUpU32(dstMipHeight, dstLayout.blockSize))
		return false;

	const bool srcIs3D = srcDesc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D;
	const bool dstIs3D = dstDesc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D;
	const uint32 srcSliceLimit = srcIs3D ? std::max<uint32>(1, (uint32)srcDesc.DepthOrArraySize >> srcMip) : std::max<uint32>(1, srcDesc.DepthOrArraySize);
	const uint32 dstSliceLimit = dstIs3D ? std::max<uint32>(1, (uint32)dstDesc.DepthOrArraySize >> dstMip) : std::max<uint32>(1, dstDesc.DepthOrArraySize);
	if ((uint64)srcSlice + (uint64)depth > srcSliceLimit || (uint64)dstSlice + (uint64)depth > dstSliceLimit)
		return false;

	struct SliceCopy
	{
		uint32 sourceSubresource{};
		UINT sourceZ{};
		uint32 destinationSubresource{};
		UINT destinationZ{};
	};
	std::vector<SliceCopy> sliceCopies;
	std::vector<uint32> sourceCopySubresources;
	std::vector<uint32> destinationCopySubresources;
	sliceCopies.reserve((size_t)depth);
	for (sint32 slice = 0; slice < depth; ++slice)
	{
		SliceCopy sliceCopy;
		sliceCopy.sourceSubresource = srcIs3D ? (uint32)srcMip : (uint32)srcMip + (uint32)(srcSlice + slice) * srcDesc.MipLevels;
		sliceCopy.sourceZ = srcIs3D ? (UINT)(srcSlice + slice) : 0u;
		sliceCopy.destinationSubresource = dstIs3D ? (uint32)dstMip : (uint32)dstMip + (uint32)(dstSlice + slice) * dstDesc.MipLevels;
		sliceCopy.destinationZ = dstIs3D ? (UINT)(dstSlice + slice) : 0u;
		if (std::find(sourceCopySubresources.begin(), sourceCopySubresources.end(), sliceCopy.sourceSubresource) == sourceCopySubresources.end())
			sourceCopySubresources.push_back(sliceCopy.sourceSubresource);
		if (std::find(destinationCopySubresources.begin(), destinationCopySubresources.end(), sliceCopy.destinationSubresource) == destinationCopySubresources.end())
			destinationCopySubresources.push_back(sliceCopy.destinationSubresource);
		sliceCopies.emplace_back(sliceCopy);
	}

	const uint32 rowPitch = AlignUpU32(elementsWide * srcLayout.bytes, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
	const uint64 sliceBytes = ((uint64)rowPitch * elementsHigh + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1) &
		~(uint64)(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1);
	const uint64 totalBytes = sliceBytes * (uint64)depth;

	const TextureCopyCacheKey textureCopyKey
	{
		srcTexture->GetContentIdentity(), dstTexture->GetContentIdentity(),
		srcMip, srcX, srcY, srcSlice, dstMip, dstX, dstY, dstSlice, width, height, depth, 1
	};
	if (CanSkipTextureCopy(textureCopyKey, srcTexture, dstTexture, sourceCopySubresources, destinationCopySubresources))
	{
		m_textureCopyCacheHits++;
		m_textureCopyBytesSkipped += totalBytes;
		return true;
	}
	m_textureCopyCacheMisses++;

	// direct-queue ordering lets every copy reuse one staging buffer; very large copies get their own
	constexpr uint64 kMaxRetainedStagingBytes = 32ull * 1024ull * 1024ull;
	D3D12MemoryManager::Resource oversizedStagingBuffer;
	D3D12MemoryManager::Resource* stagingBuffer = &m_textureAliasCopyBuffer;
	if (totalBytes > kMaxRetainedStagingBytes)
	{
		if (!m_memoryManager->CreateBuffer(totalBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, oversizedStagingBuffer))
			return false;
		stagingBuffer = &oversizedStagingBuffer;
	}
	else if (!m_textureAliasCopyBuffer.IsValid() || m_textureAliasCopyBuffer.size < totalBytes)
	{
		uint64 stagingSize = std::max<uint64>(1024ull * 1024ull, m_textureAliasCopyBuffer.size * 2);
		while (stagingSize < totalBytes)
			stagingSize *= 2;
		stagingSize = std::min<uint64>(stagingSize, kMaxRetainedStagingBytes);
		ReleaseResourceDeferred(m_textureAliasCopyBuffer);
		if (!m_memoryManager->CreateBuffer(stagingSize, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, m_textureAliasCopyBuffer))
			return false;
	}

	if (!m_commandListOpen)
		ResetCommandList();

	if (D3D12ShouldTrace())
	{
		auto* srcViewForTrace = static_cast<LatteTextureViewD3D12*>(srcTexture->GetOrCreateView(srcTexture->dim, srcTexture->format, srcMip, 1, srcSlice, 1));
		TraceTextureSnapshot("texture_copy_raw_alias_src_before", "src", srcViewForTrace, false,
			fmt::format("dst=0x{:08x} size={}x{}x{}", dstTexture->physAddress, width, height, depth));
	}
	D3D12TraceEvent("texture_copy_raw_alias_execute",
					"src=0x{:08x} srcFmt=0x{:04x} srcDxgi={} srcMip={} srcSlice={} srcXY={},{} dst=0x{:08x} dstFmt=0x{:04x} dstDxgi={} dstMip={} dstSlice={} dstXY={},{} size={}x{}x{} elements={}x{} bytes={} rowPitch={} srcResource={} dstResource={} stagingResource={} commandSlot={} nextFence={}",
					srcTexture->physAddress,
					(uint32)srcTexture->format,
					(uint32)srcDesc.Format,
					srcMip,
					srcSlice,
					srcX,
					srcY,
					dstTexture->physAddress,
					(uint32)dstTexture->format,
					(uint32)dstDesc.Format,
					dstMip,
					dstSlice,
					dstX,
					dstY,
					width,
					height,
					depth,
					elementsWide,
					elementsHigh,
					(unsigned long long)totalBytes,
					rowPitch,
					(const void*)srcTexture->GetResource(),
					(const void*)dstTexture->GetResource(),
					(const void*)stagingBuffer->resource.Get(),
					m_commandSlotIndex,
					(unsigned long long)m_nextFenceValue);

	auto* commandList = GetCurrentCommandSlot().commandList.Get();
	const bool sameResource = srcTexture->GetResource() == dstTexture->GetResource();
	auto* destinationStateTexture = sameResource ? srcTexture : dstTexture;
	std::vector<TextureTransition> sourceRestoreStates;
	std::vector<TextureTransition> sourceCopyStates;
	for (uint32 subresource : sourceCopySubresources)
	{
		sourceRestoreStates.push_back({ subresource, srcTexture->GetSubresourceState(subresource) });
		sourceCopyStates.push_back({ subresource, D3D12_RESOURCE_STATE_COPY_SOURCE });
	}
	Transition(stagingBuffer->resource.Get(), stagingBuffer->state, D3D12_RESOURCE_STATE_COPY_DEST);
	TransitionTextureSubresources(srcTexture, sourceCopyStates);
	for (size_t sliceIndex = 0; sliceIndex < sliceCopies.size(); ++sliceIndex)
	{
		const SliceCopy& sliceCopy = sliceCopies[sliceIndex];
		D3D12_TEXTURE_COPY_LOCATION sourceLocation{};
		sourceLocation.pResource = srcTexture->GetResource();
		sourceLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		sourceLocation.SubresourceIndex = sliceCopy.sourceSubresource;
		D3D12_TEXTURE_COPY_LOCATION stagingLocation{};
		stagingLocation.pResource = stagingBuffer->resource.Get();
		stagingLocation.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
		stagingLocation.PlacedFootprint.Offset = sliceBytes * sliceIndex;
		stagingLocation.PlacedFootprint.Footprint.Format = srcDesc.Format;
		stagingLocation.PlacedFootprint.Footprint.Width = elementsWide * srcLayout.blockSize;
		stagingLocation.PlacedFootprint.Footprint.Height = elementsHigh * srcLayout.blockSize;
		stagingLocation.PlacedFootprint.Footprint.Depth = 1;
		stagingLocation.PlacedFootprint.Footprint.RowPitch = rowPitch;
		const D3D12_BOX sourceBox{ (UINT)srcX, (UINT)srcY, sliceCopy.sourceZ,
			(UINT)srcX + sourceBoxWidth, (UINT)srcY + sourceBoxHeight, sliceCopy.sourceZ + 1 };
		commandList->CopyTextureRegion(&stagingLocation, 0, 0, 0, &sourceLocation, &sourceBox);
	}
	Transition(stagingBuffer->resource.Get(), stagingBuffer->state, D3D12_RESOURCE_STATE_COPY_SOURCE);

	std::vector<TextureTransition> destinationRestoreStates;
	std::vector<TextureTransition> destinationCopyStates;
	for (uint32 subresource : destinationCopySubresources)
	{
		destinationRestoreStates.push_back({ subresource, destinationStateTexture->GetSubresourceState(subresource) });
		destinationCopyStates.push_back({ subresource, D3D12_RESOURCE_STATE_COPY_DEST });
	}
	TransitionTextureSubresources(destinationStateTexture, destinationCopyStates);
	for (size_t sliceIndex = 0; sliceIndex < sliceCopies.size(); ++sliceIndex)
	{
		const SliceCopy& sliceCopy = sliceCopies[sliceIndex];
		D3D12_TEXTURE_COPY_LOCATION destinationLocation{};
		destinationLocation.pResource = dstTexture->GetResource();
		destinationLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		destinationLocation.SubresourceIndex = sliceCopy.destinationSubresource;
		D3D12_TEXTURE_COPY_LOCATION stagingLocation{};
		stagingLocation.pResource = stagingBuffer->resource.Get();
		stagingLocation.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
		stagingLocation.PlacedFootprint.Offset = sliceBytes * sliceIndex;
		stagingLocation.PlacedFootprint.Footprint.Format = dstDesc.Format;
		stagingLocation.PlacedFootprint.Footprint.Width = destinationWidth;
		stagingLocation.PlacedFootprint.Footprint.Height = destinationHeight;
		stagingLocation.PlacedFootprint.Footprint.Depth = 1;
		stagingLocation.PlacedFootprint.Footprint.RowPitch = rowPitch;
		commandList->CopyTextureRegion(&destinationLocation, (UINT)dstX, (UINT)dstY, sliceCopy.destinationZ, &stagingLocation, nullptr);
	}
	TransitionTextureSubresources(destinationStateTexture, destinationRestoreStates);
	TransitionTextureSubresources(srcTexture, sourceRestoreStates);
	if (sameResource && srcTexture != dstTexture)
	{
		for (uint32 subresource : sourceCopySubresources)
		{
			if (subresource < dstTexture->GetSubresourceCount())
				dstTexture->SetSubresourceState(subresource, srcTexture->GetSubresourceState(subresource));
		}
		for (uint32 subresource : destinationCopySubresources)
		{
			if (subresource < dstTexture->GetSubresourceCount())
				dstTexture->SetSubresourceState(subresource, srcTexture->GetSubresourceState(subresource));
		}
	}
	RecordTextureCopy(textureCopyKey, srcTexture, dstTexture, sourceCopySubresources, destinationCopySubresources);

	if (D3D12ShouldTrace())
	{
		auto* dstViewForTrace = static_cast<LatteTextureViewD3D12*>(dstTexture->GetOrCreateView(dstTexture->dim, dstTexture->format, dstMip, 1, dstSlice, 1));
		RecordTextureLineage("texture_copy_raw_alias", dstViewForTrace, 0, 0,
							 fmt::format("copySrc=0x{:08x}/fmt=0x{:04x}", srcTexture->physAddress, (uint32)srcTexture->format),
							 fmt::format("srcDxgi={} dstDxgi={} size={}x{}x{} bytes={} rowPitch={}",
										 (uint32)srcDesc.Format,
										 (uint32)dstDesc.Format,
										 width,
										 height,
										 depth,
										 (unsigned long long)totalBytes,
										 rowPitch));
		TraceTextureSnapshot("texture_copy_raw_alias_dst_after", "dst", dstViewForTrace, false, fmt::format("src=0x{:08x} bytes={}", srcTexture->physAddress, (unsigned long long)totalBytes));
	}
	if (oversizedStagingBuffer.IsValid())
		QueueDeferredRelease(std::move(oversizedStagingBuffer));
	return true;
}

LatteTextureReadbackInfoD3D12* D3D12Renderer::CreateHostTextureReadback(LatteTextureView* textureView)
{
	auto* readback = new LatteTextureReadbackInfoD3D12(this, textureView);
	if (!readback->IsValid())
	{
		delete readback;
		return nullptr;
	}
	return readback;
}

LatteTextureReadbackInfo* D3D12Renderer::texture_createReadback(LatteTextureView* textureView)
{
	D3D12_EXCLUSIVE_RECORDING();
	auto* texture = textureView ? static_cast<LatteTextureD3D12*>(textureView->baseTexture) : nullptr;
	if (!texture)
		return nullptr;
	// readers expect guest texel layout, which substituted and block-compressed host formats do not have
	const auto& formatInfo = texture->GetFormatInfo();
	const uint32 guestTexelBytes = texture->GetBPP() / 8;
	const uint32 hostTexelBytes = GetD3D12CopyElementLayout(texture->GetDesc().Format).bytes;
	const bool hostLayoutMatchesGuest = !formatInfo.isAlternateFormat && !formatInfo.isCompressed && !texture->IsCompressedFormat() &&
		(texture->isDepth || hostTexelBytes == guestTexelBytes);
	if (!hostLayoutMatchesGuest)
	{
		static bool s_loggedUnsupportedReadback = false;
		if (!s_loggedUnsupportedReadback)
		{
			s_loggedUnsupportedReadback = true;
			cemuLog_log(LogType::Force, "D3D12: texture readback is not supported for format 0x{:04x} (host dxgi={}).",
				(uint32)texture->format, (uint32)texture->GetDesc().Format);
		}
		return nullptr;
	}
	auto* readback = CreateHostTextureReadback(textureView);
	if (readback && readback->GetRowSizeInBytes() != std::max<uint64>(texture->GetDesc().Width >> textureView->firstMip, 1) * guestTexelBytes)
	{
		delete readback;
		return nullptr;
	}
	return readback;
}

void D3D12Renderer::surfaceCopy_copySurfaceWithFormatConversion(LatteTexture* sourceTexture, sint32 srcMip, sint32 srcSlice, LatteTexture* destinationTexture, sint32 dstMip, sint32 dstSlice, sint32 width, sint32 height)
{
	auto recordCopy = [=, this](LatteTextureViewD3D12* preparedSourceView, LatteTextureViewD3D12* preparedDestinationView)
	{
		if (destinationTexture)
			DropRetainedFeedbackCopies(destinationTexture);
		stdx::scope_exit deviceCheck([&]() { CheckDeviceAfterOperation("surface copy", [&]() {
			return fmt::format("src {} gx2fmt=0x{:04x} mip={} slice={} -> dst {} gx2fmt=0x{:04x} mip={} slice={} size {}x{}", D3D12DescribeResource(sourceTexture ? static_cast<LatteTextureD3D12*>(sourceTexture)->GetResource() : nullptr),
				sourceTexture ? (uint32)sourceTexture->format : 0, srcMip, srcSlice, D3D12DescribeResource(destinationTexture ? static_cast<LatteTextureD3D12*>(destinationTexture)->GetResource() : nullptr),
				destinationTexture ? (uint32)destinationTexture->format : 0, dstMip, dstSlice, width, height); }); });
		SurfaceCopyWithFormatConversion(sourceTexture, srcMip, srcSlice, destinationTexture, dstMip, dstSlice, 0, 0, 0, 0, width, height, false,
			true, preparedSourceView, preparedDestinationView);
	};
	if (ShouldQueueRecording())
	{
		// a color<->depth copy always draws: the views it samples and renders to, and the GPU write tracking, are core
		// state, so they are done here and the draw is queued
		auto* srcTexture = static_cast<LatteTextureD3D12*>(sourceTexture);
		auto* dstTexture = static_cast<LatteTextureD3D12*>(destinationTexture);
		if (srcTexture && dstTexture && srcTexture->isDepth != dstTexture->isDepth)
		{
			if (!srcTexture->GetResource() || !dstTexture->GetResource() ||
				srcMip < 0 || srcMip >= (sint32)srcTexture->GetDesc().MipLevels ||
				dstMip < 0 || dstMip >= (sint32)dstTexture->GetDesc().MipLevels)
				return;
			sint32 effectiveCopyWidth = width;
			sint32 effectiveCopyHeight = height;
			LatteTexture_scaleToEffectiveSize(sourceTexture, &effectiveCopyWidth, &effectiveCopyHeight, 0);
			if (effectiveCopyWidth <= 0 || effectiveCopyHeight <= 0)
				return;
			auto* sourceView = static_cast<LatteTextureViewD3D12*>(srcTexture->Is3DTexture() ?
				sourceTexture->GetOrCreateView(Latte::E_DIM::DIM_2D, sourceTexture->format, srcMip, 1, srcSlice, 1) :
				sourceTexture->GetOrCreateView(srcMip, 1, srcSlice, 1));
			auto* destinationView = static_cast<LatteTextureViewD3D12*>(destinationTexture->GetOrCreateView(dstMip, 1, dstSlice, 1));
			if (!sourceView || !destinationView)
				return;
			LatteTexture_TrackTextureGPUWrite(dstTexture, dstSlice, dstMip, LatteTexture_getNextUpdateEventCounter());
			QueueRecording([recordCopy, sourceView, destinationView]() { recordCopy(sourceView, destinationView); });
			return;
		}
	}
	D3D12_EXCLUSIVE_RECORDING();
	recordCopy(nullptr, nullptr);
}

void D3D12Renderer::SurfaceCopyWithFormatConversion(LatteTexture* sourceTexture, sint32 srcMip, sint32 srcSlice, LatteTexture* destinationTexture, sint32 dstMip, sint32 dstSlice,
												sint32 srcX, sint32 srcY, sint32 dstX, sint32 dstY, sint32 width, sint32 height, bool coordinatesAreEffective,
												bool allowNativeCopy, LatteTextureViewD3D12* preparedSourceView,
												LatteTextureViewD3D12* preparedDestinationView)
{
	auto* srcTexture = static_cast<LatteTextureD3D12*>(sourceTexture);
	auto* dstTexture = static_cast<LatteTextureD3D12*>(destinationTexture);
	if (!srcTexture || !dstTexture || !srcTexture->GetResource() || !dstTexture->GetResource())
		return;
	// guest mips beyond what the texture size allows have no storage on the host
	if (srcMip < 0 || srcMip >= (sint32)srcTexture->GetDesc().MipLevels ||
		dstMip < 0 || dstMip >= (sint32)dstTexture->GetDesc().MipLevels)
		return;
#if defined(CEMU_UWP_D3D12_CORRECTNESS_TELEMETRY)
	++m_correctnessSurfaceCopyRequests;
#endif
	const char* copyModeName = "color-to-color";
	if (srcTexture->isDepth && !dstTexture->isDepth)
		copyModeName = "depth-to-color";
	else if (!srcTexture->isDepth && dstTexture->isDepth)
		copyModeName = "color-to-depth";
	else if (srcTexture->isDepth && dstTexture->isDepth)
		copyModeName = "depth-to-depth";
	const bool sameLatteAddress = srcTexture->physAddress == dstTexture->physAddress;
	const bool sameD3DResource = srcTexture->GetResource() == dstTexture->GetResource();
	static uint32 s_surfaceCopyLogCount = 0;
	if (D3D12RuntimeLogCapped(s_surfaceCopyLogCount, "surfaceCopy_copySurfaceWithFormatConversion", 96))
	{
		cemuLog_log(LogType::Force, "D3D12: surface copy mode={} src=0x{:08x}/fmt=0x{:04x}/depth={} dst=0x{:08x}/fmt=0x{:04x}/depth={} srcMip={} srcSlice={} dstMip={} dstSlice={} srcXY=({}, {}) dstXY=({}, {}) size={}x{} effectiveCoords={} sameAddr={} sameResource={} srcObj={} dstObj={}.",
					copyModeName,
					srcTexture->physAddress, (uint32)srcTexture->format, srcTexture->isDepth,
					dstTexture->physAddress, (uint32)dstTexture->format, dstTexture->isDepth,
					srcMip, srcSlice, dstMip, dstSlice,
					srcX, srcY, dstX, dstY, width, height,
					coordinatesAreEffective ? 1 : 0,
					sameLatteAddress,
					sameD3DResource,
					(const void*)srcTexture,
					(const void*)dstTexture);
	}
	D3D12TraceEvent("surface_copy_begin", "mode={} src=0x{:08x} srcFmt=0x{:04x} srcDxgi={} srcDepth={} dst=0x{:08x} dstFmt=0x{:04x} dstDxgi={} dstDepth={} srcMip={} srcSlice={} dstMip={} dstSlice={} srcXY={},{} dstXY={},{} size={}x{} effectiveCoords={} sameAddr={} sameResource={} srcObj={} dstObj={}",
					copyModeName,
					srcTexture->physAddress,
					(uint32)srcTexture->format,
					(uint32)srcTexture->GetDesc().Format,
					srcTexture->isDepth ? 1 : 0,
					dstTexture->physAddress,
					(uint32)dstTexture->format,
					(uint32)dstTexture->GetDesc().Format,
					dstTexture->isDepth ? 1 : 0,
					srcMip,
					srcSlice,
					dstMip,
					dstSlice,
					srcX,
					srcY,
					dstX,
					dstY,
					width,
					height,
					coordinatesAreEffective ? 1 : 0,
					sameLatteAddress ? 1 : 0,
					sameD3DResource ? 1 : 0,
					(const void*)srcTexture,
					(const void*)dstTexture);
	sint32 effectiveCopyWidth = width;
	sint32 effectiveCopyHeight = height;
	if (!coordinatesAreEffective)
		LatteTexture_scaleToEffectiveSize(sourceTexture, &effectiveCopyWidth, &effectiveCopyHeight, 0);
	if (effectiveCopyWidth <= 0 || effectiveCopyHeight <= 0)
		return;

	const bool nativeCopyCompatible = AreD3D12CopyFormatsCompatible(srcTexture->GetDesc().Format, dstTexture->GetDesc().Format);
	if (allowNativeCopy && srcTexture->isDepth == dstTexture->isDepth && nativeCopyCompatible)
	{
#if defined(CEMU_UWP_D3D12_CORRECTNESS_TELEMETRY)
		++m_correctnessSurfaceCopyNative;
#endif
		D3D12TraceEvent("surface_copy_native_redirect", "src=0x{:08x} dst=0x{:08x} srcXY={},{} dstXY={},{} requestedSize={}x{} effectiveSize={}x{}",
						srcTexture->physAddress, dstTexture->physAddress,
						srcX, srcY, dstX, dstY,
						width, height,
						effectiveCopyWidth, effectiveCopyHeight);
		texture_copyImageSubData(sourceTexture, srcMip, srcX, srcY, srcSlice, destinationTexture, dstMip, dstX, dstY, dstSlice, effectiveCopyWidth, effectiveCopyHeight, 1);
		return;
	}

	if (srcTexture->isDepth == dstTexture->isDepth)
	{
		// raw reinterpretation only replaces the conversion draw for whole uncompressed 2D mips
		const bool wholeMipCopy =
			srcX == 0 && srcY == 0 && dstX == 0 && dstY == 0 &&
			!srcTexture->Is3DTexture() && !dstTexture->Is3DTexture() &&
			!srcTexture->GetFormatInfo().isCompressed && !dstTexture->GetFormatInfo().isCompressed &&
			effectiveCopyWidth == (sint32)std::max<uint64>(srcTexture->GetDesc().Width >> srcMip, 1) &&
			effectiveCopyHeight == (sint32)std::max<uint32>(srcTexture->GetDesc().Height >> srcMip, 1) &&
			effectiveCopyWidth == (sint32)std::max<uint64>(dstTexture->GetDesc().Width >> dstMip, 1) &&
			effectiveCopyHeight == (sint32)std::max<uint32>(dstTexture->GetDesc().Height >> dstMip, 1);
		if (!srcTexture->isDepth && srcTexture->GetBPP() == dstTexture->GetBPP() && wholeMipCopy)
		{
			if (TryRawAliasTextureCopy(srcTexture, srcMip, srcSlice, srcX, srcY,
									   dstTexture, dstMip, dstSlice, dstX, dstY,
									   effectiveCopyWidth, effectiveCopyHeight, 1))
			{
#if defined(CEMU_UWP_D3D12_CORRECTNESS_TELEMETRY)
				++m_correctnessSurfaceCopyRawAlias;
#endif
				D3D12TraceEvent("surface_copy_raw_alias_redirect", "src=0x{:08x} srcDxgi={} dst=0x{:08x} dstDxgi={} requestedSize={}x{} effectiveSize={}x{}",
								srcTexture->physAddress,
								(uint32)srcTexture->GetDesc().Format,
								dstTexture->physAddress,
								(uint32)dstTexture->GetDesc().Format,
								width,
								height,
								effectiveCopyWidth,
								effectiveCopyHeight);
				return;
			}
		}
		D3D12TraceEvent("surface_copy_shader_conversion",
			"reason=same_aspect_incompatible src=0x{:08x} srcDxgi={} srcDepth={} dst=0x{:08x} dstDxgi={} dstDepth={} requestedSize={}x{} effectiveSize={}x{} srcBpp={} dstBpp={}",
			srcTexture->physAddress,
			(uint32)srcTexture->GetDesc().Format,
			srcTexture->isDepth ? 1 : 0,
			dstTexture->physAddress,
			(uint32)dstTexture->GetDesc().Format,
			dstTexture->isDepth ? 1 : 0,
			width,
			height,
			effectiveCopyWidth,
			effectiveCopyHeight,
			srcTexture->GetBPP(),
			dstTexture->GetBPP());
	}

	// a 2D view of a 3D texture samples the slice through a 2D sampling proxy
	// prepared views come from the Latte thread, which also tracked the GPU write (threaded recording)
	if (!preparedSourceView && m_preparedSurfaceCopy.sourceView &&
		m_preparedSurfaceCopy.source == sourceTexture && m_preparedSurfaceCopy.sourceMip == srcMip && m_preparedSurfaceCopy.sourceSlice == srcSlice &&
		m_preparedSurfaceCopy.destination == destinationTexture && m_preparedSurfaceCopy.destinationMip == dstMip && m_preparedSurfaceCopy.destinationSlice == dstSlice)
	{
		preparedSourceView = m_preparedSurfaceCopy.sourceView;
		preparedDestinationView = m_preparedSurfaceCopy.destinationView;
	}
	auto* sourceView = preparedSourceView ? preparedSourceView : static_cast<LatteTextureViewD3D12*>(srcTexture->Is3DTexture() ?
		sourceTexture->GetOrCreateView(Latte::E_DIM::DIM_2D, sourceTexture->format, srcMip, 1, srcSlice, 1) :
		sourceTexture->GetOrCreateView(srcMip, 1, srcSlice, 1));
	auto* destinationView = preparedDestinationView ? preparedDestinationView :
		static_cast<LatteTextureViewD3D12*>(destinationTexture->GetOrCreateView(dstMip, 1, dstSlice, 1));
	if (!sourceView || !destinationView)
	{
#if defined(CEMU_UWP_D3D12_CORRECTNESS_TELEMETRY)
		++m_correctnessSurfaceCopyInvalidView;
#endif
		return;
	}
	sourceView->PrepareForSampling();
	const D3D12_SHADER_RESOURCE_VIEW_DESC sourceSrvDesc = sourceView->BuildSubresourceArraySrvDesc(false);
	if (sourceSrvDesc.Format == DXGI_FORMAT_UNKNOWN)
	{
#if defined(CEMU_UWP_D3D12_CORRECTNESS_TELEMETRY)
		++m_correctnessSurfaceCopyInvalidView;
#endif
		return;
	}

	SurfaceCopyMode copyMode = SurfaceCopyMode::ColorToColor;
	if (srcTexture->isDepth && !dstTexture->isDepth)
		copyMode = SurfaceCopyMode::DepthToColor;
	else if (!srcTexture->isDepth && dstTexture->isDepth)
		copyMode = SurfaceCopyMode::ColorToDepth;
	else if (srcTexture->isDepth && dstTexture->isDepth)
		copyMode = SurfaceCopyMode::DepthToDepth;
	const bool copyWritesDepth =
		copyMode == SurfaceCopyMode::ColorToDepth
		|| copyMode == SurfaceCopyMode::DepthToDepth
		;

	const uint32 sourceSubresource = srcTexture->GetSubresourceIndex(srcTexture->Is3DTexture() ? 0 : srcSlice, srcMip);
	const uint32 destinationSubresource = dstTexture->GetSubresourceIndex(dstTexture->Is3DTexture() ? 0 : dstSlice, dstMip);
	const std::vector<uint32> sourceCopySubresources{ sourceSubresource };
	const std::vector<uint32> destinationCopySubresources{ destinationSubresource };
	const TextureCopyCacheKey surfaceCopyKey
	{
		srcTexture->GetContentIdentity(), dstTexture->GetContentIdentity(),
		srcMip, srcX, srcY, srcSlice, dstMip, dstX, dstY, dstSlice,
		effectiveCopyWidth, effectiveCopyHeight, 1, 10u + (uint32)copyMode
	};
	if (CanSkipTextureCopy(surfaceCopyKey, srcTexture, dstTexture, sourceCopySubresources, destinationCopySubresources))
	{
		m_surfaceConversionCacheHits++;
		return;
	}

	const DXGI_FORMAT rtvFormat = copyWritesDepth ? DXGI_FORMAT_UNKNOWN : destinationView->GetRTV().IsValid() ? destinationView->GetRTVFormat() : DXGI_FORMAT_UNKNOWN;
	const DXGI_FORMAT dsvFormat = copyWritesDepth ? dstTexture->GetFormatInfo().dsvFormat : DXGI_FORMAT_UNKNOWN;
	if (copyWritesDepth)
	{
		if (!destinationView->GetDSV().IsValid() || dsvFormat == DXGI_FORMAT_UNKNOWN)
		{
#if defined(CEMU_UWP_D3D12_CORRECTNESS_TELEMETRY)
			++m_correctnessSurfaceCopyInvalidOutput;
#endif
			return;
		}
	}
	else
	{
		if (!destinationView->GetRTV().IsValid() || rtvFormat == DXGI_FORMAT_UNKNOWN)
		{
#if defined(CEMU_UWP_D3D12_CORRECTNESS_TELEMETRY)
			++m_correctnessSurfaceCopyInvalidOutput;
#endif
			return;
		}
	}

	ID3D12PipelineState* pipeline = GetSurfaceCopyPipeline(copyMode, rtvFormat, dsvFormat);
	if (!pipeline)
	{
#if defined(CEMU_UWP_D3D12_CORRECTNESS_TELEMETRY)
		++m_correctnessSurfaceCopyNoPso;
#endif
		return;
	}
	ID3D12PipelineState* stencilPipeline = nullptr;
	D3D12_SHADER_RESOURCE_VIEW_DESC stencilSrvDesc{};
	if (copyMode == SurfaceCopyMode::DepthToDepth && srcTexture->GetFormatInfo().hasStencil && dstTexture->GetFormatInfo().hasStencil)
	{
		if (m_psSpecifiedStencilRefSupported)
		{
			stencilSrvDesc = sourceView->BuildSubresourceArraySrvDesc(true);
			if (stencilSrvDesc.Format != DXGI_FORMAT_UNKNOWN)
				stencilPipeline = GetSurfaceCopyPipeline(SurfaceCopyMode::StencilToStencil, DXGI_FORMAT_UNKNOWN, dsvFormat);
		}
		else
		{
			cemuLog_logOnce(LogType::Force, "D3D12: shader-specified stencil reference is unsupported; depth-stencil surface copies do not copy stencil.");
		}
	}
#if defined(CEMU_UWP_D3D12_CORRECTNESS_TELEMETRY)
	++m_correctnessSurfaceCopyShader;
#endif
	m_surfaceConversionCacheMisses++;
	if (D3D12ShouldTrace())
	{
		TraceTextureSnapshot("surface_copy_src_before", "before_surface_copy_draw", sourceView, false,
			fmt::format("mode={} dst=0x{:08x} srcXY={},{} dstXY={},{} requestedSize={}x{} effectiveSize={}x{}",
				copyModeName, dstTexture->physAddress, srcX, srcY, dstX, dstY, width, height, effectiveCopyWidth, effectiveCopyHeight));
	}
	if (!m_commandListOpen)
		ResetCommandList();
	const auto surfaceCopyDescriptorStats = m_descriptorManager->GetStats();
	const bool hasCachedPointSampler =
		m_shaderVisibleOutputPointSampler.IsValid() &&
		m_shaderVisibleOutputSamplerGeneration == surfaceCopyDescriptorStats.shaderVisibleSamplerRollovers;
	EnsureShaderVisibleDescriptorSpace(stencilPipeline ? 2 : 1, hasCachedPointSampler ? 0 : 1);

	// the copy shaders declare Texture2DArray, which the view's own SRV may not match
	auto srv = m_descriptorManager->AllocateShaderVisible(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	CreateShaderResourceViewChecked(sourceView->GetSrvResource(), &sourceSrvDesc, srv.cpu, "surface copy source", true);
	D3D12DescriptorManager::Allocation stencilSrv;
	if (stencilPipeline)
	{
		stencilSrv = m_descriptorManager->AllocateShaderVisible(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
		CreateShaderResourceViewChecked(sourceView->GetSrvResource(), &stencilSrvDesc, stencilSrv.cpu, "surface copy stencil", true);
	}
	auto sampler = GetShaderVisibleOutputSampler(false);
	BindShaderVisibleHeaps();
	D3D12TraceEvent("surface_copy_execute",
					"mode={} src=0x{:08x} srcFmt=0x{:04x} srcDxgi={} srcResource={} dst=0x{:08x} dstFmt=0x{:04x} dstDxgi={} dstResource={} srcXY={},{} dstXY={},{} requestedSize={}x{} effectiveSize={}x{} srcMip={} srcSlice={} dstMip={} dstSlice={} srvGpu=0x{:016x} samplerGpu=0x{:016x} pipeline={} commandSlot={} nextFence={}",
					copyModeName,
					srcTexture->physAddress,
					(uint32)srcTexture->format,
					(uint32)sourceView->GetViewFormat(),
					(const void*)srcTexture->GetResource(),
					dstTexture->physAddress,
					(uint32)dstTexture->format,
					copyWritesDepth ? (uint32)dsvFormat : (uint32)rtvFormat,
					(const void*)dstTexture->GetResource(),
					srcX,
					srcY,
					dstX,
					dstY,
					width,
					height,
					effectiveCopyWidth,
					effectiveCopyHeight,
					srcMip,
					srcSlice,
					dstMip,
					dstSlice,
					(unsigned long long)srv.gpu.ptr,
					(unsigned long long)sampler.gpu.ptr,
					(const void*)pipeline,
					m_commandSlotIndex,
					(unsigned long long)m_nextFenceValue);
	TransitionTextureView(sourceView, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
		stencilPipeline != nullptr);
	if (copyWritesDepth)
		TransitionTextureView(destinationView, D3D12_RESOURCE_STATE_DEPTH_WRITE, true);
	else
		TransitionTextureView(destinationView, D3D12_RESOURCE_STATE_RENDER_TARGET);

	auto* commandList = GetCurrentCommandSlot().commandList.Get();
	SetD3D12PipelineState(pipeline);
	SetD3D12GraphicsRootSignature(m_surfaceCopyRootSignature.Get());
	commandList->SetGraphicsRootDescriptorTable(0, srv.gpu);
	commandList->SetGraphicsRootDescriptorTable(1, sampler.gpu);
	const sint32 copyOffsets[4]
	{
		srcX,
		srcY,
		dstX,
		dstY,
	};
	commandList->SetGraphicsRoot32BitConstants(2, 4, copyOffsets, 0);

	D3D12_VIEWPORT viewport{};
	viewport.TopLeftX = (float)std::max<sint32>(dstX, 0);
	viewport.TopLeftY = (float)std::max<sint32>(dstY, 0);
	viewport.Width = (float)effectiveCopyWidth;
	viewport.Height = (float)effectiveCopyHeight;
	viewport.MinDepth = 0.0f;
	viewport.MaxDepth = 1.0f;
	D3D12_RECT scissor{};
	scissor.left = std::max<sint32>(dstX, 0);
	scissor.top = std::max<sint32>(dstY, 0);
	scissor.right = scissor.left + effectiveCopyWidth;
	scissor.bottom = scissor.top + effectiveCopyHeight;
	SetD3D12Viewport(viewport);
	SetD3D12Scissor(scissor);
	SetD3D12PrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

	if (copyWritesDepth)
	{
		const auto dsv = destinationView->GetDSV().cpu;
		SetD3D12RenderTargets(0, nullptr, &dsv);
	}
	else
	{
		const auto rtv = destinationView->GetRTV().cpu;
		SetD3D12RenderTargets(1, &rtv, nullptr);
	}
	commandList->DrawInstanced(3, 1, 0, 0);
	m_gpuWatchdog.RecordOperation(D3D12GpuWatchdog::Operation::SurfaceCopy);
	if (stencilPipeline)
	{
		SetD3D12PipelineState(stencilPipeline);
		commandList->SetGraphicsRootDescriptorTable(0, stencilSrv.gpu);
		commandList->DrawInstanced(3, 1, 0, 0);
	}
	SetD3D12Viewport(m_currentViewport);
	SetD3D12Scissor(m_currentScissor);
	TransitionTextureView(destinationView, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
		copyWritesDepth);
	if (!preparedDestinationView)
		LatteTexture_TrackTextureGPUWrite(dstTexture, dstSlice, dstMip, LatteTexture_getNextUpdateEventCounter());
	RecordTextureCopy(surfaceCopyKey, srcTexture, dstTexture, sourceCopySubresources, destinationCopySubresources);
	if (D3D12ShouldTrace())
	{
		RecordTextureLineage("surface_copy", destinationView, 0, 0, fmt::format("surfaceSrc=0x{:08x}/fmt=0x{:04x}", srcTexture->physAddress, (uint32)srcTexture->format),
							 fmt::format("mode={} srcMip={} srcSlice={} dstMip={} dstSlice={} srcXY={},{} dstXY={},{} requestedSize={}x{} effectiveSize={}x{} pipeline={}",
										 copyModeName,
										 srcMip,
										 srcSlice,
										 dstMip,
										 dstSlice,
										 srcX,
										 srcY,
										 dstX,
										 dstY,
										 width,
										 height,
										 effectiveCopyWidth,
										 effectiveCopyHeight,
										 (const void*)pipeline));
		TraceTextureSnapshot("surface_copy_dst_after", "after_surface_copy_draw", destinationView, false, fmt::format("mode={} src=0x{:08x} srcXY={},{} dstXY={},{} requestedSize={}x{} effectiveSize={}x{}", copyModeName, srcTexture->physAddress, srcX, srcY, dstX, dstY, width, height, effectiveCopyWidth, effectiveCopyHeight));
	}
}

void D3D12Renderer::DrawHandleSpecialState5()
{
	LatteMRT::UpdateCurrentFBO();
	LatteRenderTarget_updateViewport();

	LatteTextureView* colorBuffer = LatteMRT::GetColorAttachment(0);
	LatteTextureView* depthBuffer = LatteMRT::GetDepthAttachment();
	if (!colorBuffer || !depthBuffer || !colorBuffer->baseTexture || !depthBuffer->baseTexture)
	{
		static uint32 s_specialState5MissingAttachmentLogCount = 0;
		if (D3D12RuntimeLogCapped(s_specialState5MissingAttachmentLogCount, "special state 5 missing attachment", 16))
			cemuLog_log(LogType::Force, "D3D12: special state 5 depth-to-color copy skipped because color/depth attachment is missing.");
		return;
	}

	sint32 vpWidth = 0;
	sint32 vpHeight = 0;
	LatteMRT::GetVirtualViewportDimensions(vpWidth, vpHeight);
	if (vpWidth <= 0 || vpHeight <= 0)
		return;

	static uint32 s_specialState5LogCount = 0;
	if (D3D12RuntimeLogCapped(s_specialState5LogCount, "special state 5 depth-to-color copy", 32))
	{
		cemuLog_log(LogType::Force, "D3D12: special state 5 depth-to-color copy size={}x{} color=0x{:08x} depth=0x{:08x}.",
					vpWidth,
					vpHeight,
					colorBuffer->baseTexture->physAddress,
					depthBuffer->baseTexture->physAddress);
	}

	surfaceCopy_copySurfaceWithFormatConversion(
		depthBuffer->baseTexture, depthBuffer->firstMip, depthBuffer->firstSlice,
		colorBuffer->baseTexture, colorBuffer->firstMip, colorBuffer->firstSlice,
		vpWidth, vpHeight);
}

void D3D12Renderer::bufferCache_init(const sint32 bufferSize)
{
	D3D12_EXCLUSIVE_RECORDING();
	if (!m_memoryManager || bufferSize <= 0)
		return;
	WaitForIdle();
	m_bufferCache = {};
	if (D3D12ShouldKeepBufferShadow())
		m_bufferCacheShadow.assign(std::max<sint32>(bufferSize, 0), 0);
	else
		m_bufferCacheShadow.clear();
	m_streamoutBuffer = {};
	if (!m_memoryManager->CreateBuffer(bufferSize, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON, m_bufferCache))
		throw std::runtime_error("Failed to create D3D12 buffer cache");
	if (!m_memoryManager->CreateBuffer(kStreamoutBufferSize, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON, m_streamoutBuffer))
		throw std::runtime_error("Failed to create D3D12 streamout buffer");
	D3D12TraceEvent("buffer_cache_init", "size={} mainResource={} mainSize={} mainState=0x{:08x} streamoutResource={} streamoutSize={} streamoutState=0x{:08x} shadowBytes={}",
					bufferSize,
					(const void*)m_bufferCache.resource.Get(),
					(unsigned long long)m_bufferCache.size,
					(uint32)m_bufferCache.state,
					(const void*)m_streamoutBuffer.resource.Get(),
					(unsigned long long)m_streamoutBuffer.size,
					(uint32)m_streamoutBuffer.state,
					(unsigned long long)m_bufferCacheShadow.size());
}

void D3D12Renderer::bufferCache_upload(uint8* buffer, sint32 size, uint32 bufferOffset)
{
	if (ShouldQueueRecording())
	{
		if (!buffer || size <= 0)
			return;
		QueueRecording([this, data = std::vector<uint8>(buffer, buffer + size), bufferOffset]() mutable
		{
			bufferCache_upload(data.data(), (sint32)data.size(), bufferOffset);
		});
		return;
	}
	stdx::scope_exit deviceCheck([&]() { CheckDeviceAfterOperation("buffer upload", [&]() {
		return fmt::format("offset={} size={} bufferSize={}", bufferOffset, size, (uint64)m_bufferCache.size); }); });
	if (!m_bufferCache.resource || !buffer || size <= 0)
		return;
	++m_uwpBufferProfile.bufferUploads;
	m_uwpBufferProfile.bufferUploadBytes += (uint32)size;
	if (!m_commandListOpen)
		ResetCommandList();

	auto upload = m_memoryManager->AllocateUpload((uint32)size, 256, m_nextFenceValue);
	if (!upload.IsValid())
		throw std::runtime_error("Failed to allocate D3D12 buffer upload memory");
	memcpy(upload.cpuPtr, buffer, size);
	if ((size_t)bufferOffset + (size_t)size <= m_bufferCacheShadow.size())
		memcpy(m_bufferCacheShadow.data() + bufferOffset, buffer, size);

	uint32 firstBytes[4]{};
	if (size > 0)
		memcpy(firstBytes, buffer, std::min<size_t>((size_t)size, sizeof(firstBytes)));
	D3D12TraceEvent("buffer_cache_upload", "offset={} size={} mainResource={} stateBefore=0x{:08x} uploadResource={} uploadOffset={} uploadSize={} uploadBufferIndex={} firstBytes=0x{:08x},0x{:08x},0x{:08x},0x{:08x} shadowUpdated={}",
					bufferOffset,
					size,
					(const void*)m_bufferCache.resource.Get(),
					(uint32)m_bufferCache.state,
					(const void*)upload.resource,
					(unsigned long long)upload.offset,
					(unsigned long long)upload.size,
					upload.bufferIndex,
					firstBytes[0],
					firstBytes[1],
					firstBytes[2],
					firstBytes[3],
					((size_t)bufferOffset + (size_t)size <= m_bufferCacheShadow.size()) ? 1 : 0);
	Transition(m_bufferCache.resource.Get(), m_bufferCache.state, D3D12_RESOURCE_STATE_COPY_DEST);
	GetCurrentCommandSlot().commandList->CopyBufferRegion(m_bufferCache.resource.Get(), bufferOffset, upload.resource, upload.offset, (UINT64)size);
	Transition(m_bufferCache.resource.Get(), m_bufferCache.state, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER | D3D12_RESOURCE_STATE_INDEX_BUFFER | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}

void D3D12Renderer::bufferCache_copy(uint32 srcOffset, uint32 dstOffset, uint32 size)
{
	D3D12_QUEUE_RECORDING(bufferCache_copy(srcOffset, dstOffset, size));
	stdx::scope_exit deviceCheck([&]() { CheckDeviceAfterOperation("buffer copy", [&]() {
		return fmt::format("src={} dst={} size={} bufferSize={}", srcOffset, dstOffset, size, (uint64)m_bufferCache.size); }); });
	if (!m_bufferCache.resource || size == 0)
		return;
	if (!m_commandListOpen)
		ResetCommandList();

	// Direct-queue ordering permits reuse of one staging buffer across calls.
	if (!m_bufferCacheCopyTemp.IsValid() || m_bufferCacheCopyTemp.size < size)
	{
		ReleaseResourceDeferred(m_bufferCacheCopyTemp);
		uint64 tempSize = std::max<uint64>(64u * 1024u, m_bufferCacheCopyTemp.size * 2);
		while (tempSize < size)
			tempSize *= 2;
		if (!m_memoryManager->CreateBuffer(tempSize, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, m_bufferCacheCopyTemp))
			throw std::runtime_error("Failed to create D3D12 temporary buffer-cache copy resource");
	}
	D3D12MemoryManager::Resource& tempBuffer = m_bufferCacheCopyTemp;

	D3D12TraceEvent("buffer_cache_copy", "srcOffset={} dstOffset={} size={} mainResource={} mainStateBefore=0x{:08x} tempResource={} tempSize={} shadowCopy={}",
					srcOffset,
					dstOffset,
					size,
					(const void*)m_bufferCache.resource.Get(),
					(uint32)m_bufferCache.state,
					(const void*)tempBuffer.resource.Get(),
					(unsigned long long)tempBuffer.size,
					((size_t)srcOffset + (size_t)size <= m_bufferCacheShadow.size() &&
					 (size_t)dstOffset + (size_t)size <= m_bufferCacheShadow.size()) ? 1 : 0);
	Transition(tempBuffer.resource.Get(), tempBuffer.state, D3D12_RESOURCE_STATE_COPY_DEST);
	Transition(m_bufferCache.resource.Get(), m_bufferCache.state, D3D12_RESOURCE_STATE_COPY_SOURCE);
	GetCurrentCommandSlot().commandList->CopyBufferRegion(tempBuffer.resource.Get(), 0, m_bufferCache.resource.Get(), srcOffset, size);
	Transition(tempBuffer.resource.Get(), tempBuffer.state, D3D12_RESOURCE_STATE_COPY_SOURCE);
	Transition(m_bufferCache.resource.Get(), m_bufferCache.state, D3D12_RESOURCE_STATE_COPY_DEST);
	GetCurrentCommandSlot().commandList->CopyBufferRegion(m_bufferCache.resource.Get(), dstOffset, tempBuffer.resource.Get(), 0, size);
	if ((size_t)srcOffset + (size_t)size <= m_bufferCacheShadow.size() &&
		(size_t)dstOffset + (size_t)size <= m_bufferCacheShadow.size())
	{
		memmove(m_bufferCacheShadow.data() + dstOffset, m_bufferCacheShadow.data() + srcOffset, size);
	}
	Transition(m_bufferCache.resource.Get(), m_bufferCache.state, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER | D3D12_RESOURCE_STATE_INDEX_BUFFER | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}

void D3D12Renderer::bufferCache_copyStreamoutToMainBuffer(uint32 srcOffset, uint32 dstOffset, uint32 size)
{
	D3D12_QUEUE_RECORDING(bufferCache_copyStreamoutToMainBuffer(srcOffset, dstOffset, size));
	if (!m_streamoutBuffer.resource || !m_bufferCache.resource || size == 0)
		return;
	if (!m_commandListOpen)
		ResetCommandList();

	D3D12TraceEvent("streamout_copy_to_main", "srcOffset={} dstOffset={} size={} streamoutResource={} streamoutStateBefore=0x{:08x} mainResource={} mainStateBefore=0x{:08x}",
					srcOffset,
					dstOffset,
					size,
					(const void*)m_streamoutBuffer.resource.Get(),
					(uint32)m_streamoutBuffer.state,
					(const void*)m_bufferCache.resource.Get(),
					(uint32)m_bufferCache.state);
	Transition(m_streamoutBuffer.resource.Get(), m_streamoutBuffer.state, D3D12_RESOURCE_STATE_COPY_SOURCE);
	Transition(m_bufferCache.resource.Get(), m_bufferCache.state, D3D12_RESOURCE_STATE_COPY_DEST);
	GetCurrentCommandSlot().commandList->CopyBufferRegion(m_bufferCache.resource.Get(), dstOffset, m_streamoutBuffer.resource.Get(), srcOffset, size);
	Transition(m_bufferCache.resource.Get(), m_bufferCache.state, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER | D3D12_RESOURCE_STATE_INDEX_BUFFER | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}

void D3D12Renderer::buffer_bindVertexBuffers(std::span<BindBufferParam> bindings)
{
	if (ShouldQueueRecording())
	{
		QueueRecording([this, copy = std::vector<BindBufferParam>(bindings.begin(), bindings.end())]() mutable
		{
			buffer_bindVertexBuffers(copy);
		});
		return;
	}
	// vertex fetch reads the buffer cache through SRVs, so the stride is not needed here
	for (const auto& binding : bindings)
		buffer_bindVertexBuffer(binding.index, binding.bindOffset, binding.bindSize);
}

void D3D12Renderer::buffer_bindVertexBuffer(uint32 bufferIndex, uint32 offset, uint32 size)
{
	if (bufferIndex >= m_vertexBindings.size())
		return;
	m_vertexBindings[bufferIndex] = { offset, size, true };
	++m_vertexBindingGeneration;
	D3D12TraceEvent("vertex_buffer_bind", "bufferIndex={} offset={} size={} mainResource={} mainState=0x{:08x} shadowBytes={}",
					bufferIndex,
					offset,
					size,
					(const void*)m_bufferCache.resource.Get(),
					(uint32)m_bufferCache.state,
					(unsigned long long)m_bufferCacheShadow.size());
}

void D3D12Renderer::buffer_bindUniformBuffer(LatteConst::ShaderType shaderType, uint32 bufferIndex, uint32 offset, uint32 size)
{
	D3D12_QUEUE_RECORDING(buffer_bindUniformBuffer(shaderType, bufferIndex, offset, size));
	const size_t stageIndex = (size_t)shaderType;
	if (stageIndex >= m_uniformBindings.size() || bufferIndex >= m_uniformBindings[stageIndex].size())
		return;
	m_uniformBindings[stageIndex][bufferIndex] = { offset, size, true };
	++m_uniformBindingGeneration[stageIndex];
	D3D12TraceEvent("uniform_buffer_bind", "stage={} bufferIndex={} offset={} size={} mainResource={} mainState=0x{:08x}",
					(uint32)shaderType,
					bufferIndex,
					offset,
					size,
					(const void*)m_bufferCache.resource.Get(),
					(uint32)m_bufferCache.state);
}
RendererShader* D3D12Renderer::shader_create(RendererShader::ShaderType type, uint64 baseHash, uint64 auxHash, const std::string& source, bool isGameShader, bool isGfxPackSource)
{
	D3D12_EXCLUSIVE_RECORDING();
	static uint32 s_shaderCreateLogCount = 0;
	if (D3D12RuntimeLogCapped(s_shaderCreateLogCount, "shader_create", 160))
	{
		cemuLog_log(LogType::Force, "D3D12: create shader type={} hash={:016x}_{:016x} game={} gfxPack={} sourceBytes={}.",
					(uint32)type, baseHash, auxHash, isGameShader, isGfxPackSource, source.size());
	}
	D3D12TraceEvent("shader_create", "type={} baseHash=0x{:016x} auxHash=0x{:016x} game={} gfxPack={} sourceBytes={}",
					(uint32)type,
					baseHash,
					auxHash,
					isGameShader ? 1 : 0,
					isGfxPackSource ? 1 : 0,
					source.size());
	D3D12DumpHLSL(type, baseHash, auxHash, isGameShader, source);
	return new RendererShaderD3D12(type, baseHash, auxHash, isGameShader, isGfxPackSource, source);
}
void D3D12Renderer::streamout_setupXfbBuffer(uint32 bufferIndex, sint32 ringBufferOffset, uint32 rangeAddr, uint32 rangeSize)
{
	if (!t_recordContext && bufferIndex < m_latteStreamoutRingOffsets.size())
		m_latteStreamoutRingOffsets[bufferIndex] = ringBufferOffset;
	D3D12_QUEUE_RECORDING(streamout_setupXfbBuffer(bufferIndex, ringBufferOffset, rangeAddr, rangeSize));
	if (bufferIndex >= m_streamoutBindings.size())
		return;
	m_streamoutBindings[bufferIndex] = { true, ringBufferOffset, rangeAddr, rangeSize };
	D3D12TraceEvent("streamout_bind", "bufferIndex={} ringOffset={} rangeAddr=0x{:08x} rangeSize={} streamoutResource={} streamoutState=0x{:08x}",
					bufferIndex,
					ringBufferOffset,
					rangeAddr,
					rangeSize,
					(const void*)m_streamoutBuffer.resource.Get(),
					(uint32)m_streamoutBuffer.state);
}

void D3D12Renderer::streamout_begin()
{
	D3D12_QUEUE_RECORDING(streamout_begin());
	if (!m_commandListOpen)
		ResetCommandList();
}

void D3D12Renderer::streamout_rendererFinishDrawcall()
{
	D3D12_QUEUE_RECORDING(streamout_rendererFinishDrawcall());
	for (auto& binding : m_streamoutBindings)
		binding.enabled = false;
}

void D3D12Renderer::draw_beginSequence()
{
	if (m_threadedRecordingWanted && !t_recordContext && !m_recordThreadRunning.load(std::memory_order_relaxed) && CafeSystem::IsTitleRunning())
		StartRecordingThread();
#if defined(LATTE_UWP_DETAILED_PROFILING)
	++m_uwpSequenceProfileCalls;
	m_uwpSequenceProfileRandomState ^= m_uwpSequenceProfileRandomState << 13;
	m_uwpSequenceProfileRandomState ^= m_uwpSequenceProfileRandomState >> 17;
	m_uwpSequenceProfileRandomState ^= m_uwpSequenceProfileRandomState << 5;
	const bool sampleSequence = (m_uwpSequenceProfileRandomState & 63u) == 0;
	std::chrono::steady_clock::time_point sequenceStarted{};
	if (sampleSequence)
		sequenceStarted = std::chrono::steady_clock::now();
	stdx::scope_exit sequenceTimer([&]()
	{
		if (!sampleSequence)
			return;
		++m_uwpSequenceProfileSamples;
		m_uwpSequenceProfileTimeNs += (uint64)std::chrono::duration_cast<std::chrono::nanoseconds>(
			std::chrono::steady_clock::now() - sequenceStarted).count();
	});
#endif
	// recorder half: a texture whose slices are still being written must not be sampled, detached objects are freed,
	// and a command list is open for the draws
	auto beginRecording = [this]()
	{
		texture_finishSliceUploads();
		FlushPendingDestroys();
		if (!m_commandListOpen)
			ResetCommandList();
	};
	if (ShouldQueueRecording())
		QueueRecording(beginRecording);
	else
		beginRecording();
	m_drawSequenceActive = true;
	m_drawSequenceSkip = false;

	const bool streamoutEnable = LatteGPUState.contextRegister[mmVGT_STRMOUT_EN] != 0;

	LatteSHRC_UpdateActiveShaders();
	if (LatteGPUState.activeShaderHasError)
	{
#if defined(CEMU_UWP_D3D12_CORRECTNESS_TELEMETRY)
		++m_correctnessSequenceShaderError;
#endif
		static uint32 s_shaderErrorLogCount = 0;
		if (D3D12RuntimeLogCapped(s_shaderErrorLogCount, "draw_beginSequence shader error", 16))
			cemuLog_log(LogType::Force, "D3D12: skipping draw sequence due to active shader error.");
		m_drawSequenceSkip = true;
		cemu_assert_debug(false);
		return;
	}

	LatteGPUState.requiresTextureBarrier = false;
	while (true)
	{
		LatteGPUState.repeatTextureInitialization = false;
		if (!LatteMRT::UpdateCurrentFBO())
		{
#if defined(CEMU_UWP_D3D12_CORRECTNESS_TELEMETRY)
			++m_correctnessSequenceInvalidFbo;
#endif
			static uint32 s_invalidRenderTargetLogCount = 0;
			if (D3D12RuntimeLogCapped(s_invalidRenderTargetLogCount, "draw_beginSequence invalid render target", 32))
				cemuLog_log(LogType::Force, "D3D12: skipping draw sequence because current FBO update failed.");
			m_drawSequenceSkip = true;
			return;
		}

		if (!hasValidFramebufferAttached && !streamoutEnable)
		{
#if defined(CEMU_UWP_D3D12_CORRECTNESS_TELEMETRY)
			++m_correctnessSequenceMissingAttachment;
#endif
			static uint32 s_missingAttachmentLogCount = 0;
			if (D3D12RuntimeLogCapped(s_missingAttachmentLogCount, "draw_beginSequence missing attachments", 32))
				cemuLog_log(LogType::Force, "D3D12: skipping draw sequence because no color/depth framebuffer is attached.");
			m_drawSequenceSkip = true;
			return;
		}

		LatteTexture_updateTextures();
		if (!LatteGPUState.repeatTextureInitialization)
			break;
	}

	LatteMRT::ApplyCurrentState();
	LatteRenderTarget_updateViewport();
	LatteRenderTarget_updateScissorBox();

	bool rasterizerEnable = LatteGPUState.contextNew.PA_CL_CLIP_CNTL.get_DX_RASTERIZATION_KILL() == false;
	if (!LatteGPUState.contextNew.PA_CL_VTE_CNTL.get_VPORT_X_OFFSET_ENA())
		rasterizerEnable = true;
	if (!rasterizerEnable && !streamoutEnable)
	{
#if defined(CEMU_UWP_D3D12_CORRECTNESS_TELEMETRY)
		++m_correctnessSequenceRasterDisabled;
#endif
		static uint32 s_rasterizerDisabledLogCount = 0;
		if (D3D12RuntimeLogCapped(s_rasterizerDisabledLogCount, "draw_beginSequence rasterizer disabled", 32))
			cemuLog_log(LogType::Force, "D3D12: skipping draw sequence because rasterization is disabled and streamout is off.");
		m_drawSequenceSkip = true;
	}
}

void D3D12Renderer::draw_execute(uint32 baseVertex, uint32 baseInstance, uint32 instanceCount, uint32 count, MPTR indexDataMPTR, Latte::LATTE_VGT_DMA_INDEX_TYPE::E_INDEX_TYPE indexType, const LatteDrawcallContext& drawcallContext)
{
	stdx::scope_exit drawAccounting([&]() { LatteGPUState.drawCallCounter++; });
	stdx::scope_exit deviceCheck([&]() { CheckDeviceAfterOperation("draw", [&]() {
		const auto* vertexShader = LatteSHRC_GetActiveVertexShader();
		const auto* geometryShader = LatteSHRC_GetActiveGeometryShader();
		const auto* pixelShader = LatteSHRC_GetActivePixelShader();
		return fmt::format("vs={:016x} gs={:016x} ps={:016x} count={} instances={} indexType={} fbo=0x{:016x}",
			vertexShader ? vertexShader->baseHash : 0, geometryShader ? geometryShader->baseHash : 0, pixelShader ? pixelShader->baseHash : 0,
			count, instanceCount, (uint32)indexType, m_latteActiveFBO ? m_latteActiveFBO->key : 0); }); });
#if defined(LATTE_UWP_DETAILED_PROFILING)
	++m_uwpCpuProfileDraws;
	uint32 profileRandom = m_uwpCpuProfileRandomState;
	profileRandom ^= profileRandom << 13;
	profileRandom ^= profileRandom >> 17;
	profileRandom ^= profileRandom << 5;
	m_uwpCpuProfileRandomState = profileRandom;
	const bool profileThisDraw = (profileRandom & 63u) == 0;
#if defined(CEMU_UWP_PERFORMANCE_EXPERIMENT)
	const bool experimentProfileThisDraw = profileThisDraw && m_performanceExperiment.Measuring();
#endif
	UwpCpuDrawStage profileStage = UwpCpuDrawStage::Setup;
	std::chrono::steady_clock::time_point profileStageStarted{};
	if (profileThisDraw)
	{
		++m_uwpCpuProfileSampledDraws;
		profileStageStarted = std::chrono::steady_clock::now();
	}
	bool profileHeadDone = false;
	auto finishProfileStage = [&]()
	{
		if (!profileThisDraw || profileHeadDone)
			return;
		const auto now = std::chrono::steady_clock::now();
		const size_t stageIndex = (size_t)profileStage;
		const uint64 elapsedNs = (uint64)std::chrono::duration_cast<std::chrono::nanoseconds>(now - profileStageStarted).count();
		m_uwpCpuProfileStageTimeNs[stageIndex] += elapsedNs;
		++m_uwpCpuProfileStageSamples[stageIndex];
#if defined(CEMU_UWP_PERFORMANCE_EXPERIMENT)
		if (experimentProfileThisDraw)
		{
			m_performanceExperimentWindow.broadTimeNs[stageIndex] += elapsedNs;
			++m_performanceExperimentWindow.broadSamples[stageIndex];
		}
#endif
		profileStageStarted = now;
	};
	auto switchProfileStage = [&](UwpCpuDrawStage nextStage)
	{
		if (!profileThisDraw)
			return;
		finishProfileStage();
		profileStage = nextStage;
	};
	stdx::scope_exit profileFinalizer([&]() { finishProfileStage(); });
#if defined(CEMU_UWP_PERFORMANCE_EXPERIMENT)
	bool experimentDetailActive = false;
	ExperimentDetailStage experimentDetailStage = ExperimentDetailStage::Prepare;
	std::chrono::steady_clock::time_point experimentDetailStarted{};
	auto finishExperimentDetail = [&]()
	{
		if (!experimentDetailActive)
			return;
		const auto now = std::chrono::steady_clock::now();
		const size_t stage = (size_t)experimentDetailStage;
		m_performanceExperimentWindow.detailTimeNs[stage] += (uint64)std::chrono::duration_cast<std::chrono::nanoseconds>(now - experimentDetailStarted).count();
		++m_performanceExperimentWindow.detailSamples[stage];
		++m_performanceExperimentWindow.detailClockReads;
		experimentDetailStarted = now;
	};
	auto switchExperimentDetail = [&](ExperimentDetailStage nextStage)
	{
		if (!experimentProfileThisDraw)
			return;
		if (experimentDetailActive)
			finishExperimentDetail();
		else
		{
			experimentDetailStarted = std::chrono::steady_clock::now();
			++m_performanceExperimentWindow.detailClockReads;
		}
		experimentDetailActive = true;
		experimentDetailStage = nextStage;
	};
	stdx::scope_exit experimentDetailFinalizer([&]() { finishExperimentDetail(); });
#endif
#else
	auto switchProfileStage = [](UwpCpuDrawStage) {};
#endif
#if defined(CEMU_UWP_D3D12_CORRECTNESS_TELEMETRY)
	++m_correctnessDrawAttempts;
#endif
	if (m_drawSequenceSkip)
	{
#if defined(CEMU_UWP_D3D12_CORRECTNESS_TELEMETRY)
		++m_correctnessDrawSkippedSequence;
#endif
		++m_droppedDraws[(size_t)DroppedDrawReason::SequenceRejected];
		return;
	}

	if (LatteGPUState.contextNew.GetSpecialStateValues()[8] != 0)
	{
#if defined(CEMU_UWP_D3D12_CORRECTNESS_TELEMETRY)
		++m_correctnessSpecialStateDepthClear;
#endif
		D3D12_EXCLUSIVE_RECORDING();
		LatteDraw_handleSpecialState8_clearAsDepth();
		return;
	}
	else if (LatteGPUState.contextNew.GetSpecialStateValues()[5] != 0)
	{
#if defined(CEMU_UWP_D3D12_CORRECTNESS_TELEMETRY)
		++m_correctnessSpecialStateDepthCopy;
#endif
		D3D12_EXCLUSIVE_RECORDING();
		DrawHandleSpecialState5();
		return;
	}

	// draw number for Latte-side traces; the recording side numbers draws itself and recycles their scratch copies
	const uint64 nextDrawId = (uint64)LatteGPUState.drawCallCounter + 1;

	const LattePrimitiveMode primitiveMode = LatteGPUState.contextNew.VGT_PRIMITIVE_TYPE.get_PRIMITIVE_MODE();
#if defined(CEMU_UWP_D3D12_CORRECTNESS_TELEMETRY)
	const uint32 primitiveValue = (uint32)primitiveMode;
	if (primitiveValue < 64)
		m_correctnessPrimitiveMask |= 1ull << primitiveValue;
#endif
	const D3D_PRIMITIVE_TOPOLOGY d3dPrimitiveTopology = D3D12PipelineCompiler::GetPrimitiveTopology(primitiveMode);
	const auto& polygonControlReg = LatteGPUState.contextNew.PA_SU_SC_MODE_CNTL;
	const auto cullState = GetD3D12EffectiveCullState(primitiveMode, polygonControlReg);
	const bool cullAll = cullState.cullFront && cullState.cullBack;
	const bool streamoutEnable = LatteGPUState.contextRegister[mmVGT_STRMOUT_EN] != 0;
	bool rasterizerEnable = LatteGPUState.contextNew.PA_CL_CLIP_CNTL.get_DX_RASTERIZATION_KILL() == false;
	if (!LatteGPUState.contextNew.PA_CL_VTE_CNTL.get_VPORT_X_OFFSET_ENA())
		rasterizerEnable = true;
	// the command processor clears the dirty masks after every draw, synced or not
	const auto deferBufferSync = [&]()
	{
		if (drawcallContext.isFirst || drawcallContext.vertexBufferDirtyMask != 0 ||
			drawcallContext.vsUniformBufferDirtyMask != 0 || drawcallContext.psUniformBufferDirtyMask != 0 ||
			drawcallContext.gsUniformBufferDirtyMask != 0)
			m_drawNeedsFullBufferSync = true;
	};
	if (cullAll && !streamoutEnable && D3D12PrimitiveUsesFaceCulling(primitiveMode))
	{
		static uint32 s_cullAllSkipLogCount = 0;
		if (D3D12RuntimeLogCapped(s_cullAllSkipLogCount, "front-and-back cull skip", 32))
		{
			cemuLog_log(LogType::Force, "D3D12: skipping draw because front-and-back face culling rejects primitive={} and streamout is off.",
						(uint32)primitiveMode);
		}
		m_cullAllDrawsSkipped++;
		deferBufferSync();
		return;
	}
	const bool noRasterizationOutput = streamoutEnable &&
		((!rasterizerEnable) || (cullAll && D3D12PrimitiveUsesFaceCulling(primitiveMode)));
	if (noRasterizationOutput)
	{
		static uint32 s_noRasterStreamoutLogCount = 0;
		if (D3D12RuntimeLogCapped(s_noRasterStreamoutLogCount, "no-raster streamout", 32))
		{
			cemuLog_log(LogType::Force, "D3D12: using no-output PSO for streamout draw primitive={} rasterizerEnable={} cullAll={}.",
						(uint32)primitiveMode,
						rasterizerEnable,
						cullAll);
		}
	}

	// an active logic op may need destination copies, which use recording state: such a draw takes exclusive access
	// and its tail runs right here. Plain COPY (the common case) decides "no emulation" without touching that state.
	std::optional<RecordingExclusiveScope> logicOpExclusive;
	uint32 drawLogicOpMode = 0;
	bool drawDisableNativeLogicOp = false;
	if (!noRasterizationOutput)
	{
		D3D12_LOGIC_OP drawLogicOp = D3D12_LOGIC_OP_COPY;
		const bool knownLogicOp = TryGetD3D12LogicOp(LatteGPUState.contextNew.CB_COLOR_CONTROL.get_ROP(), drawLogicOp);
		if (!knownLogicOp || drawLogicOp != D3D12_LOGIC_OP_COPY)
		{
			logicOpExclusive.emplace(this);
			// destination copies are recorded into the current command list
			if (!m_commandListOpen)
				ResetCommandList();
			if (!PrepareLogicOpEmulationForDraw())
			{
#if defined(CEMU_UWP_D3D12_CORRECTNESS_TELEMETRY)
				++m_correctnessDrawSkippedLogicOp;
#endif
				++m_droppedDraws[(size_t)DroppedDrawReason::LogicOpUnsupported];
				deferBufferSync();
				return;
			}
			drawLogicOpMode = m_logicOpEmulationMode;
			drawDisableNativeLogicOp = m_disableNativeLogicOpForDraw;
		}
	}
	switchProfileStage(UwpCpuDrawStage::IndexDecode);
	Renderer::INDEX_TYPE hostIndexType{};
	uint32 hostIndexCount{};
	uint32 indexMin{};
	uint32 indexMax{};
	Renderer::IndexAllocation indexAllocation;
	uint32 vertexIdRemapMode = 0;
	LatteIndices_decode(memory_getPointerFromVirtualOffset(indexDataMPTR), indexType, count, primitiveMode, indexMax, hostIndexType, hostIndexCount, indexAllocation);
	if ((D3D12ShouldTrace() || D3D12ShouldChecksumOutput()) && indexAllocation.mem && hostIndexCount != 0)
	{
		const bool primitiveRestartEnabled = GetD3D12PrimitiveRestartEnabled(LatteGPUState.contextNew, primitiveMode, hostIndexType);
		const uint32 hostRestartIndex = hostIndexType == Renderer::INDEX_TYPE::U16 ?
			std::numeric_limits<uint16>::max() : std::numeric_limits<uint32>::max();
		indexMin = std::numeric_limits<uint32>::max();
		if (hostIndexType == Renderer::INDEX_TYPE::U16)
		{
			const auto* indices = static_cast<const uint16*>(indexAllocation.mem);
			for (uint32 i = 0; i < hostIndexCount; ++i)
			{
				if (!primitiveRestartEnabled || indices[i] != static_cast<uint16>(hostRestartIndex))
					indexMin = std::min<uint32>(indexMin, indices[i]);
			}
		}
		else if (hostIndexType == Renderer::INDEX_TYPE::U32)
		{
			const auto* indices = static_cast<const uint32*>(indexAllocation.mem);
			for (uint32 i = 0; i < hostIndexCount; ++i)
			{
				if (!primitiveRestartEnabled || indices[i] != hostRestartIndex)
					indexMin = std::min(indexMin, indices[i]);
			}
		}
		if (indexMin == std::numeric_limits<uint32>::max())
			indexMin = 0;
	}
	switchProfileStage(UwpCpuDrawStage::BufferSync);
	uint8 stageUniformModifiedMask = 0;
	if (drawcallContext.isFirst || m_drawNeedsFullBufferSync)
	{
		m_drawNeedsFullBufferSync = false;
		LatteBufferCache_Sync(indexMax + baseVertex, baseInstance, instanceCount,
			0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, stageUniformModifiedMask);
	}
	else
	{
		LatteBufferCache_Sync(indexMax + baseVertex, baseInstance, instanceCount,
			drawcallContext.vertexBufferDirtyMask,
			drawcallContext.vsUniformBufferDirtyMask,
			drawcallContext.psUniformBufferDirtyMask,
			drawcallContext.gsUniformBufferDirtyMask,
			stageUniformModifiedMask, true);
	}

	switchProfileStage(UwpCpuDrawStage::PipelineLookup);
	auto* vertexShader = LatteSHRC_GetActiveVertexShader();
	auto* geometryShader = LatteSHRC_GetActiveGeometryShader();
	auto* pixelShader = LatteSHRC_GetActivePixelShader();
	auto* fetchShader = LatteSHRC_GetActiveFetchShader();
#if defined(CEMU_UWP_D3D12_CORRECTNESS_TELEMETRY)
	if (pixelShader && pixelShader->pixelColorOutputMask == 0)
		++m_correctnessColorlessPixelDraws;
	bool drawUsesDepthCompare = false;
	bool drawUsesUnsupportedDepthCompare = false;
	auto recordDepthCompareUsage = [&](const LatteDecompilerShader* shader)
	{
		if (!shader)
			return;
		for (uint32 relativeUnit = 0; relativeUnit < LATTE_NUM_MAX_TEX_UNITS; ++relativeUnit)
		{
			if (!shader->textureUsesDepthCompare[relativeUnit])
				continue;
			drawUsesDepthCompare = true;
			++m_correctnessDepthCompareUnitUses;
			const uint32 dimension = (uint32)shader->textureUnitDim[relativeUnit];
			if (dimension < 64)
				m_correctnessDepthCompareDimMask |= 1ull << dimension;
			if (shader->textureUnitDim[relativeUnit] == Latte::E_DIM::DIM_3D ||
				shader->textureUnitDim[relativeUnit] == Latte::E_DIM::DIM_CUBEMAP)
				drawUsesUnsupportedDepthCompare = true;
		}
	};
	recordDepthCompareUsage(vertexShader);
	recordDepthCompareUsage(geometryShader);
	recordDepthCompareUsage(pixelShader);
	if (drawUsesDepthCompare)
		++m_correctnessDepthCompareDraws;
	if (drawUsesUnsupportedDepthCompare)
		++m_correctnessUnsupportedDepthCompareDraws;

	if (fetchShader)
	{
		if (!fetchShader->bufferGroupsInvalid.empty())
			++m_correctnessInvalidFetchDraws;
		bool hasUnboundFetchBuffer = false;
		for (const auto& group : fetchShader->bufferGroups)
		{
			if (group.attributeBufferIndex >= m_vertexBindings.size() || !m_vertexBindings[group.attributeBufferIndex].valid)
				hasUnboundFetchBuffer = true;
			for (sint32 i = 0; i < group.attribCount; ++i)
			{
				const uint32 fetchFormat = (uint32)group.attrib[i].format;
				if (fetchFormat < 64)
					m_correctnessFetchFormatMask |= 1ull << fetchFormat;
				else
					++m_correctnessFetchFormatOverflow;
			}
		}
		if (hasUnboundFetchBuffer)
			++m_correctnessUnboundFetchDraws;
	}
#endif
	const uint32 rawRenderTargetMask = LatteGPUState.contextNew.CB_TARGET_MASK.get_MASK();
	const uint32 effectiveRenderTargetMask = GetD3D12EffectiveRenderTargetMask(LatteGPUState.contextNew);
	auto identity = D3D12PipelineCompiler::RecordCurrentState(m_latteActiveFBO, noRasterizationOutput);
	if (identity.valid && drawLogicOpMode != 0)
		identity.pipelineStateHash = std::rotl(identity.pipelineStateHash, 11) ^ 0xD31210C0EAA17E0Dull;
	else if (identity.valid && drawDisableNativeLogicOp)
		identity.pipelineStateHash = std::rotl(identity.pipelineStateHash, 11) ^ 0xD31210C0D15AB1EDull;
	const uint64 tracePipelineHash = identity.valid ? identity.pipelineStateHash : 0;
#if defined(CEMU_UWP_D3D12_DIAGNOSTICS)
	const bool priorityStencilDraw = D3D12TraceSink::Get().BeginPriorityStencilDraw(nextDrawId, tracePipelineHash,
		!noRasterizationOutput && m_latteActiveFBO && m_latteActiveFBO->HasDepthStencilAttachment() &&
		m_latteActiveFBO->GetStencilFormat() != DXGI_FORMAT_UNKNOWN &&
		LatteGPUState.contextNew.DB_DEPTH_CONTROL.get_STENCIL_ENABLE());
	stdx::scope_exit priorityStencilFinalizer([]() { D3D12TraceSink::Get().EndPriorityStencilDraw(); });
	if (priorityStencilDraw)
	{
		D3D12TracePriorityUniformSources(vertexShader, nextDrawId, tracePipelineHash);
		D3D12TracePriorityUniformSources(geometryShader, nextDrawId, tracePipelineHash);
		D3D12TracePriorityUniformSources(pixelShader, nextDrawId, tracePipelineHash);
	}
#endif
	D3D12TraceEvent("draw_prepare", "draw={} pipeline=0x{:016x} latteDrawCounter={} submittedDrawNext={} baseVertex={} baseInstance={} instances={} count={} indexType={} primitive={} d3dTopology={} hostIndexType={} hostIndexCount={} indexMin={} indexMax={} fbo=0x{:016x} fetch={} vs=0x{:016x} gs=0x{:016x} ps=0x{:016x} noRaster={} streamout={} cullAll={} logicMode={} viewport={},{}:{}x{} depth={}..{} scissor={},{}-{},{} targetMask=0x{:08x} rawTargetMask=0x{:08x} colorSpecialOp={} blendMask=0x{:02x} zEnable={} zWrite={} zFunc={} stencilEnable={} stencilFuncF={} stencilFuncB={} frontFace={} cullFront={} cullBack={} effectiveCullFront={} effectiveCullBack={} zClipNearDisable={} zClipFarDisable={} rasterKill={} alphaTest={} alphaFunc={} alphaRefRaw=0x{:08x}",
					(unsigned long long)nextDrawId,
					(unsigned long long)tracePipelineHash,
					LatteGPUState.drawCallCounter,
					(unsigned long long)nextDrawId,
					baseVertex,
					baseInstance,
					instanceCount,
					count,
					(uint32)indexType,
					(uint32)primitiveMode,
					(uint32)d3dPrimitiveTopology,
					(uint32)hostIndexType,
					hostIndexCount,
					indexMin,
					indexMax,
					m_latteActiveFBO ? m_latteActiveFBO->key : 0,
					fetchShader ? 1 : 0,
					vertexShader ? vertexShader->baseHash : 0,
					geometryShader ? geometryShader->baseHash : 0,
					pixelShader ? pixelShader->baseHash : 0,
					noRasterizationOutput ? 1 : 0,
					streamoutEnable ? 1 : 0,
					cullAll ? 1 : 0,
					drawLogicOpMode,
					m_currentViewport.TopLeftX,
					m_currentViewport.TopLeftY,
					m_currentViewport.Width,
					m_currentViewport.Height,
					m_currentViewport.MinDepth,
					m_currentViewport.MaxDepth,
					m_currentScissor.left,
					m_currentScissor.top,
					m_currentScissor.right,
					m_currentScissor.bottom,
					effectiveRenderTargetMask,
					rawRenderTargetMask,
					(uint32)LatteGPUState.contextNew.CB_COLOR_CONTROL.get_SPECIAL_OP(),
					LatteGPUState.contextNew.CB_COLOR_CONTROL.get_BLEND_MASK(),
					LatteGPUState.contextNew.DB_DEPTH_CONTROL.get_Z_ENABLE() ? 1 : 0,
					LatteGPUState.contextNew.DB_DEPTH_CONTROL.get_Z_WRITE_ENABLE() ? 1 : 0,
					(uint32)LatteGPUState.contextNew.DB_DEPTH_CONTROL.get_Z_FUNC(),
					LatteGPUState.contextNew.DB_DEPTH_CONTROL.get_STENCIL_ENABLE() ? 1 : 0,
					(uint32)LatteGPUState.contextNew.DB_DEPTH_CONTROL.get_STENCIL_FUNC_F(),
					(uint32)LatteGPUState.contextNew.DB_DEPTH_CONTROL.get_STENCIL_FUNC_B(),
					(uint32)polygonControlReg.get_FRONT_FACE(),
					polygonControlReg.get_CULL_FRONT() ? 1 : 0,
					polygonControlReg.get_CULL_BACK() ? 1 : 0,
					cullState.cullFront ? 1 : 0,
					cullState.cullBack ? 1 : 0,
					LatteGPUState.contextNew.PA_CL_CLIP_CNTL.get_ZCLIP_NEAR_DISABLE() ? 1 : 0,
					LatteGPUState.contextNew.PA_CL_CLIP_CNTL.get_ZCLIP_FAR_DISABLE() ? 1 : 0,
					LatteGPUState.contextNew.PA_CL_CLIP_CNTL.get_DX_RASTERIZATION_KILL() ? 1 : 0,
					LatteGPUState.contextNew.SX_ALPHA_TEST_CONTROL.get_ALPHA_TEST_ENABLE() ? 1 : 0,
					(uint32)LatteGPUState.contextNew.SX_ALPHA_TEST_CONTROL.get_ALPHA_FUNC(),
					LatteGPUState.contextNew.SX_ALPHA_REF.getRawValue());
	TraceShaderTextureBindings(nextDrawId, tracePipelineHash, vertexShader, geometryShader, pixelShader);
	TraceVertexFetchBindings(nextDrawId, tracePipelineHash, fetchShader);
	LogVertexBufferPreview(nextDrawId, tracePipelineHash, vertexShader, pixelShader, primitiveMode, count, baseVertex, baseInstance, indexMin, indexMax, hostIndexType, hostIndexCount);
	if (fetchShader)
	{
		static uint32 s_fetchShaderDetailLogCount = 0;
		if (D3D12RuntimeLogCapped(s_fetchShaderDetailLogCount, "fetch shader details", 48))
		{
			cemuLog_log(LogType::Force, "D3D12: fetch shader has {} valid buffer groups and {} invalid groups.",
						(uint32)fetchShader->bufferGroups.size(),
						(uint32)fetchShader->bufferGroupsInvalid.size());
			for (auto& group : fetchShader->bufferGroups)
			{
				const uint32 stride = group.getCurrentBufferStride(LatteGPUState.contextRegister);
				const auto binding = group.attributeBufferIndex < m_vertexBindings.size() ? m_vertexBindings[group.attributeBufferIndex] : BoundRange{};
				cemuLog_log(LogType::Force, "D3D12:   vb{} stride={} cacheOffset={} cacheSize={} valid={} offsetLowBits={} attrCount={} range=[{},{}].",
							group.attributeBufferIndex,
							stride,
							binding.offset,
							binding.size,
							binding.valid,
							binding.offset & 3,
							group.attribCount,
							group.minOffset,
							group.totalAttribRangeSize);
				for (sint32 i = 0; i < group.attribCount; ++i)
				{
					const auto& attr = group.attrib[i];
					cemuLog_log(LogType::Force, "D3D12:     attr semantic={} fmt=0x{:02x} fetch={} nfa={} signed={} endian={} divisor={} offset={} ds={},{},{},{}.",
								attr.semanticId,
								(uint32)attr.format,
								D3D12FetchTypeName(attr.fetchType),
								attr.nfa,
								attr.isSigned,
								D3D12EndianName(attr.endianSwap),
								attr.aluDivisor,
								attr.offset,
								attr.ds[0],
								attr.ds[1],
								attr.ds[2],
								attr.ds[3]);
				}
			}
			for (auto& group : fetchShader->bufferGroupsInvalid)
			{
				cemuLog_log(LogType::Force, "D3D12:   invalid-vb{} attrCount={} range=[{},{}].",
							group.attributeBufferIndex,
							group.attribCount,
							group.minOffset,
							group.totalAttribRangeSize);
			}
		}
	}
	static uint32 s_drawExecuteLogCount = 0;
	if (D3D12RuntimeLogCapped(s_drawExecuteLogCount, "draw_execute", 192))
	{
		cemuLog_log(LogType::Force, "D3D12: draw_execute baseVertex={} baseInstance={} instances={} count={} indexType={} primitive={} fbo={} fetch={} vs={:016x} gs={:016x} ps={:016x} activeShaderHasError={}.",
					baseVertex,
					baseInstance,
					instanceCount,
					count,
					(uint32)indexType,
					(uint32)primitiveMode,
					m_latteActiveFBO ? "yes" : "no",
					fetchShader ? "yes" : "no",
					vertexShader ? vertexShader->baseHash : 0,
					geometryShader ? geometryShader->baseHash : 0,
					pixelShader ? pixelShader->baseHash : 0,
					LatteGPUState.activeShaderHasError);
	}
	if (drawLogicOpMode != 0 ||
		primitiveMode == Latte::LATTE_VGT_PRIMITIVE_TYPE::E_PRIMITIVE_TYPE::RECTS ||
		primitiveMode == Latte::LATTE_VGT_PRIMITIVE_TYPE::E_PRIMITIVE_TYPE::QUADS)
	{
		static uint32 s_specialDrawDetailLogCount = 0;
		if (D3D12RuntimeLogCapped(s_specialDrawDetailLogCount, "logic-op/rect draw detail", 192))
		{
			const auto& lcr = LatteGPUState.contextNew;
			const auto logicOp = lcr.CB_COLOR_CONTROL.get_ROP();
			const uint32 outputMask = pixelShader ? pixelShader->pixelColorOutputMask : 0;
			const uint32 targetMask = GetD3D12EffectiveRenderTargetMask(lcr);
			const uint32 blendMask = lcr.CB_COLOR_CONTROL.get_BLEND_MASK();
			const uint32 blend0Control = lcr.CB_BLENDN_CONTROL[0].getRawValue();
			uint32 rt0Addr = 0;
			uint32 rt0Width = 0;
			uint32 rt0Height = 0;
			uint32 rt0Format = 0;
			if (m_latteActiveFBO && !m_latteActiveFBO->GetColorAttachments().empty())
			{
				const auto& rt0 = m_latteActiveFBO->GetColorAttachments()[0];
				if (rt0.valid && rt0.view && rt0.view->baseTexture)
				{
					rt0Addr = rt0.view->baseTexture->physAddress;
					rt0Width = rt0.view->baseTexture->width;
					rt0Height = rt0.view->baseTexture->height;
					rt0Format = (uint32)rt0.view->baseTexture->format;
				}
			}
			cemuLog_log(LogType::Force,
						"D3D12: special draw primitive={} logicMode={} rop=0x{:02x} count={} hostCount={} instances={} baseVertex={} fbo={}x{} rt0=0x{:08x}/{}x{}/fmt=0x{:04x} viewport=({}, {}) {}x{} depth={}..{} scissor=({}, {})-({}, {}) targetMask=0x{:08x} blendMask=0x{:02x} blend0=0x{:08x} psOut=0x{:02x} vs={:016x} gs={:016x} ps={:016x}.",
						(uint32)primitiveMode,
						drawLogicOpMode,
						(uint32)logicOp,
						count,
						hostIndexCount,
						instanceCount,
						baseVertex,
						m_latteActiveFBO ? m_latteActiveFBO->m_size.x : 0,
						m_latteActiveFBO ? m_latteActiveFBO->m_size.y : 0,
						rt0Addr,
						rt0Width,
						rt0Height,
						rt0Format,
						m_currentViewport.TopLeftX,
						m_currentViewport.TopLeftY,
						m_currentViewport.Width,
						m_currentViewport.Height,
						m_currentViewport.MinDepth,
						m_currentViewport.MaxDepth,
						m_currentScissor.left,
						m_currentScissor.top,
						m_currentScissor.right,
						m_currentScissor.bottom,
						targetMask,
						blendMask,
						blend0Control,
						outputMask,
						vertexShader ? vertexShader->baseHash : 0,
						geometryShader ? geometryShader->baseHash : 0,
						pixelShader ? pixelShader->baseHash : 0);
		}
	}
	const bool usesPointExpansionGS = !noRasterizationOutput && ShouldUsePointExpansionGeometryShader(geometryShader, primitiveMode);
	const bool rasterizesPointQuads = usesPointExpansionGS || (geometryShader && LatteDecompiler_HLSLGeometryShaderExpandsPoints(LatteGPUState.contextRegister));
	bool useStencilTwoPass = false;
	bool stencilPassesOrderIndependent = false;
	if (!noRasterizationOutput && !rasterizesPointQuads && m_latteActiveFBO && D3D12PrimitiveUsesFaceCulling(primitiveMode))
	{
		const auto drawStencilState = GetD3D12SelectedStencilState(LatteGPUState.contextNew, cullState);
		const bool stencilEnabled =
			m_latteActiveFBO->HasDepthStencilAttachment() &&
			m_latteActiveFBO->GetStencilFormat() != DXGI_FORMAT_UNKNOWN &&
			LatteGPUState.contextNew.DB_DEPTH_CONTROL.get_STENCIL_ENABLE();
		bool independentStencilAvailable = false;
#if defined(__ID3D12GraphicsCommandList8_INTERFACE_DEFINED__)
		independentStencilAvailable =
			m_independentFrontBackStencilSupported; // every command slot has the list-8 interface when this is set
#endif
		useStencilTwoPass =
			stencilEnabled &&
			!cullState.cullFront && !cullState.cullBack &&
			drawStencilState.needsIndependentFrontBackState &&
			!independentStencilAvailable;
		if (useStencilTwoPass)
		{
#if defined(CEMU_UWP_D3D12_CORRECTNESS_TELEMETRY)
			++m_correctnessStencilFallbackDraws;
#endif
			cemuLog_logOnce(LogType::Force,
				"D3D12 UWP: using front/back stencil passes because independent stencil masks/references are unavailable.");
			const auto& depthControl = LatteGPUState.contextNew.DB_DEPTH_CONTROL;
			bool drawWritesColor = false;
			if (pixelShader)
			{
				const auto& attachments = m_latteActiveFBO->GetColorAttachments();
				for (uint32 i = 0; i < attachments.size() && i < 8; ++i)
				{
					if (attachments[i].valid && (pixelShader->pixelColorOutputMask & (1u << i)) != 0 &&
						((effectiveRenderTargetMask >> (i * 4)) & 0xFu) != 0)
						drawWritesColor = true;
				}
			}
			stencilPassesOrderIndependent =
				!drawWritesColor && !(pixelShader && pixelShader->hasStreamoutBufferWrite) &&
				!(depthControl.get_Z_ENABLE() && depthControl.get_Z_WRITE_ENABLE()) &&
				D3D12StencilFacesOrderIndependent(LatteGPUState.contextNew, drawStencilState, depthControl.get_Z_ENABLE());
		}
	}
	// a uniform color makes C | dst idempotent at overlaps, so one snapshot suffices
	const bool uniformOrEligible = !noRasterizationOutput && !useStencilTwoPass && !streamoutEnable &&
		!geometryShader && drawLogicOpMode == 2 && pixelShader &&
		pixelShader->pixelColorOutputIsUniform && !pixelShader->isCustomShader &&
		!pixelShader->hasError && !pixelShader->depthMask && !pixelShader->hasStreamoutBufferWrite;
	const bool uniformOrBatch = uniformOrEligible &&
		PerformanceExperimentEnabled(D3D12PerformanceExperiment::UniformOrBatch);
	const bool logicOpNeedsOrderedDestination =
		!noRasterizationOutput && !uniformOrBatch && D3D12LogicOpEmulationNeedsDestination(drawLogicOpMode);
	// front-then-back replay would reorder depth/stencil/blend; keep primitive order
	const bool useOrderedPrimitiveFallback = (useStencilTwoPass && !stencilPassesOrderIndependent) || logicOpNeedsOrderedDestination;
#if defined(CEMU_UWP_D3D12_CORRECTNESS_TELEMETRY)
	if (logicOpNeedsOrderedDestination)
		++m_correctnessOrderedLogicOpDraws;
#endif
	const bool needsFlippedOrderedPipeline = useOrderedPrimitiveFallback &&
		(d3dPrimitiveTopology == D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP ||
		 d3dPrimitiveTopology == D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP_ADJ);
	const StencilFallbackPass firstStencilPass = useStencilTwoPass ? StencilFallbackPass::Front : StencilFallbackPass::None;
	const StencilFallbackPass flippedStencilPass = useStencilTwoPass ? StencilFallbackPass::FrontFlipped : StencilFallbackPass::WindingFlipped;
	// streamout ring space and uniform data come from guest state (registers, guest memory), so both are prepared
	// before the split; a draw dropped after it (no PSO, invalid index buffer) still finishes its streamout
	m_streamoutVerticesPerInstance = (sint32)count;
	LatteStreamout_PrepareDrawcall(count, instanceCount);
	// runs when the head returns, after the tail is queued: its buffer copies are queued behind the draw
	stdx::scope_exit streamoutFinalizer([&]() { LatteStreamout_FinishDrawcall(false); });
	std::array<UniformUpdate, (size_t)LatteConst::ShaderType::TotalCount> uniformUpdates;
	{
#if defined(LATTE_UWP_DETAILED_PROFILING)
		const auto uniformStarted = profileThisDraw ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
#endif
		for (LatteDecompilerShader* shader : { vertexShader, pixelShader, geometryShader })
		{
			if (shader && (size_t)shader->shaderType < uniformUpdates.size())
				PrepareGameUniformVars(shader, nextDrawId, tracePipelineHash, uniformUpdates[(size_t)shader->shaderType]);
		}
#if defined(LATTE_UWP_DETAILED_PROFILING)
		if (profileThisDraw)
			m_uwpDescriptorSplitNs[1] += (uint64)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - uniformStarted).count();
#endif
	}
	// Phase 4 split point: from here on (pipeline lookup, descriptors, recording) the emulated GPU state is read
	// only through RecordRegs()/s_record*, never LatteGPUState. With threaded recording this tail runs on the
	// recording thread, with the register changes and uniform bytes of this draw travelling along with it.
	const bool queueTail = ShouldQueueRecording();
	RecordStateDelta recordDelta;
	if (queueTail)
	{
#if defined(LATTE_UWP_DETAILED_PROFILING)
		const auto collectStarted = profileThisDraw ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
#endif
		CollectRecordState(recordDelta, fetchShader);
#if defined(LATTE_UWP_DETAILED_PROFILING)
		if (profileThisDraw)
		{
			m_uwpCollectRecordNs += (uint64)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - collectStarted).count();
			m_uwpCollectRecordSamples++;
		}
#endif
	}
	else
		SyncRecordState(fetchShader);
#if defined(LATTE_UWP_DETAILED_PROFILING)
	finishProfileStage();
	profileHeadDone = true;
#endif
	auto recordDraw = [=, this, recordDelta = std::move(recordDelta), uniformUpdates = std::move(uniformUpdates)]() mutable
	{
	if (queueTail)
	{
		ApplyRecordState(recordDelta);
		s_recordWordPool.Give(std::move(recordDelta.chunks));
		s_recordWordPool.Give(std::move(recordDelta.words));
	}
	for (auto& update : uniformUpdates)
		ApplyGameUniformUpdate(update);
	m_logicOpEmulationMode = drawLogicOpMode;
	m_disableNativeLogicOpForDraw = drawDisableNativeLogicOp;
	// the recording side numbers draws; scratch copies made for this draw are recycled when it is recorded
	const uint64 nextDrawId = m_gameDrawCallsSubmitted + 1;
	stdx::scope_exit unusedScratchFinalizer([&]() { RecycleScratchCopiesForDraw(nextDrawId); });
#if defined(LATTE_UWP_DETAILED_PROFILING)
	// the tail measures its own stages (the head's timers are gone when it runs on the recording thread); the
	// pipeline stage was already counted as reached by the head
	UwpCpuDrawStage tailProfileStage = UwpCpuDrawStage::PipelineLookup;
	auto tailProfileStarted = profileThisDraw ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
	auto finishTailProfileStage = [&]()
	{
		if (!profileThisDraw)
			return;
		const auto now = std::chrono::steady_clock::now();
		m_uwpCpuProfileTailTimeNs[(size_t)tailProfileStage] += (uint64)std::chrono::duration_cast<std::chrono::nanoseconds>(now - tailProfileStarted).count();
		if (tailProfileStage != UwpCpuDrawStage::PipelineLookup)
			++m_uwpCpuProfileTailSamples[(size_t)tailProfileStage];
		tailProfileStarted = now;
	};
	auto switchProfileStage = [&](UwpCpuDrawStage nextStage)
	{
		finishTailProfileStage();
		tailProfileStage = nextStage;
	};
	stdx::scope_exit tailProfileFinalizer([&]() { finishTailProfileStage(); });
#else
	auto switchProfileStage = [](UwpCpuDrawStage) {};
#endif
#if defined(CEMU_UWP_PERFORMANCE_EXPERIMENT)
	auto switchExperimentDetail = [](ExperimentDetailStage) {};
	auto finishExperimentDetail = []() {};
#endif
	// same heuristic as Vulkan: full-viewport quads (<=6 indices) often build persistent textures and Splatoon ink uses 1600x1600 targets, so those stay synchronous
	m_gamePipelineAsyncPending = false;
	// bits say why a draw may not be skipped; logged with slow synchronous pipeline builds
	m_asyncSkipBlockReasons = 0;
	if (!GetConfig().async_compile)
		m_asyncSkipBlockReasons |= 1u << 0;
	if (!m_activeFBO)
		m_asyncSkipBlockReasons |= 1u << 1;
	else if (m_activeFBO->m_size.x == 1600 && m_activeFBO->m_size.y == 1600)
		m_asyncSkipBlockReasons |= 1u << 2;
	else if (!m_activeFBO->HasDepthStencilAttachment() && count <= 6)
		m_asyncSkipBlockReasons |= 1u << 3;
	// a skipped draw would report zero samples or leave stream-out data unwritten, and the game reads both back
	if (std::any_of(m_activeOcclusionQueries.begin(), m_activeOcclusionQueries.end(),
		[](const LatteQueryObjectD3D12* query) { return query && query->m_hasActiveQuery; }))
		m_asyncSkipBlockReasons |= 1u << 4;
	if ((vertexShader && vertexShader->hasStreamoutBufferWrite) || (geometryShader && geometryShader->hasStreamoutBufferWrite))
		m_asyncSkipBlockReasons |= 1u << 5;
	m_allowAsyncShaderSkip = m_asyncSkipBlockReasons == 0;
	m_gamePipelineShaderCodeMissing = false;
	ID3D12PipelineState* pipeline = GetGamePipeline(identity, vertexShader, geometryShader, pixelShader, primitiveMode, noRasterizationOutput, hostIndexType, firstStencilPass);
	ID3D12PipelineState* flippedPipeline = needsFlippedOrderedPipeline ?
		GetGamePipeline(identity, vertexShader, geometryShader, pixelShader, primitiveMode, noRasterizationOutput, hostIndexType, flippedStencilPass) : nullptr;
	ID3D12PipelineState* backStencilPipeline = useStencilTwoPass ?
		GetGamePipeline(identity, vertexShader, geometryShader, pixelShader, primitiveMode, noRasterizationOutput, hostIndexType, StencilFallbackPass::Back) : nullptr;
	ID3D12PipelineState* backFlippedStencilPipeline = useStencilTwoPass && needsFlippedOrderedPipeline ?
		GetGamePipeline(identity, vertexShader, geometryShader, pixelShader, primitiveMode, noRasterizationOutput, hostIndexType, StencilFallbackPass::BackFlipped) : nullptr;
	if (!pipeline ||
		(needsFlippedOrderedPipeline && !flippedPipeline) ||
		(useStencilTwoPass && !backStencilPipeline) ||
		(useStencilTwoPass && needsFlippedOrderedPipeline && !backFlippedStencilPipeline))
	{
		if (m_gamePipelineAsyncPending)
		{
			// shaders still compiling in the background; expected, counted for the 5 s log
			++m_uwpAsyncSkips;
			++m_uwpAsyncSkipsByTarget[m_activeFBO ? ((uint32)m_activeFBO->m_size.x << 16) | (uint32)m_activeFBO->m_size.y : 0];
			return;
		}
#if defined(CEMU_UWP_D3D12_CORRECTNESS_TELEMETRY)
		++m_correctnessDrawSkippedNoPso;
#endif
		++m_droppedDraws[(size_t)(m_gamePipelineShaderCodeMissing ?
			DroppedDrawReason::MissingShaderCode : DroppedDrawReason::MissingPipeline)];
		uint64 skipSignature = identity.valid ? identity.pipelineStateHash : 0;
		skipSignature = std::rotl(skipSignature, 7) ^ (m_activeFBO ? 0x10001ull : 0);
		skipSignature = std::rotl(skipSignature, 7) ^ (vertexShader ? vertexShader->baseHash : 0);
		skipSignature = std::rotl(skipSignature, 7) ^ (geometryShader ? geometryShader->baseHash : 0);
		skipSignature = std::rotl(skipSignature, 7) ^ (pixelShader ? pixelShader->baseHash : 0);
		skipSignature = std::rotl(skipSignature, 7) ^ 0xD3125C1Bull;
		if (ShouldLogDrawFailure(skipSignature))
		{
			cemuLog_log(LogType::Force, "D3D12: draw skipped because no valid game PSO is available yet (identity={}, fbo={}, vs={:016x}, gs={:016x}, ps={:016x}).",
						identity.valid ? "valid" : "invalid",
						m_activeFBO ? "yes" : "no",
						vertexShader ? vertexShader->baseHash : 0,
						geometryShader ? geometryShader->baseHash : 0,
						pixelShader ? pixelShader->baseHash : 0);
		}
		D3D12TraceEvent("draw_skip", "reason=no_pso identity={} fbo=0x{:016x} vs=0x{:016x} gs=0x{:016x} ps=0x{:016x} stateHash=0x{:016x}",
						identity.valid ? 1 : 0,
						m_activeFBO ? m_activeFBO->key : 0,
						vertexShader ? vertexShader->baseHash : 0,
						geometryShader ? geometryShader->baseHash : 0,
						pixelShader ? pixelShader->baseHash : 0,
						identity.valid ? identity.pipelineStateHash : 0);
		return;
	}

	uint32 isolationRtAddress = 0;
	if (m_activeFBO && !m_activeFBO->GetColorAttachments().empty())
	{
		const auto& rt0 = m_activeFBO->GetColorAttachments()[0];
		if (rt0.valid && rt0.view && rt0.view->baseTexture)
			isolationRtAddress = rt0.view->baseTexture->physAddress;
	}
	if (D3D12DrawIsolationMatches(nextDrawId,
								  identity.pipelineStateHash,
								  vertexShader ? vertexShader->baseHash : 0,
								  geometryShader ? geometryShader->baseHash : 0,
								  pixelShader ? pixelShader->baseHash : 0,
								  isolationRtAddress))
	{
		D3D12TraceEventFlush("draw_isolation_skip",
							 "draw={} latteDraw={} pipeline=0x{:016x} rt0=0x{:08x} fbo=0x{:016x} vs=0x{:016x} gs=0x{:016x} ps=0x{:016x} primitive={} hostIndexType={} hostIndexCount={} count={} viewport={},{}:{}x{} scissor={},{}-{},{}",
							 (unsigned long long)nextDrawId,
							 LatteGPUState.drawCallCounter,
							 identity.pipelineStateHash,
							 isolationRtAddress,
							 m_activeFBO ? m_activeFBO->key : 0,
							 vertexShader ? vertexShader->baseHash : 0,
							 geometryShader ? geometryShader->baseHash : 0,
							 pixelShader ? pixelShader->baseHash : 0,
							 (uint32)primitiveMode,
							 (uint32)hostIndexType,
							 hostIndexCount,
							 count,
							 m_currentViewport.TopLeftX,
							 m_currentViewport.TopLeftY,
							 m_currentViewport.Width,
							 m_currentViewport.Height,
							 m_currentScissor.left,
							 m_currentScissor.top,
							 m_currentScissor.right,
							 m_currentScissor.bottom);
		cemuLog_log(LogType::Force,
					"D3D12 isolation: skipped draw={} latteDraw={} pipeline=0x{:016x} rt0=0x{:08x} vs={:016x} gs={:016x} ps={:016x} primitive={} hostCount={}.",
					(unsigned long long)nextDrawId,
					LatteGPUState.drawCallCounter,
					identity.pipelineStateHash,
					isolationRtAddress,
					vertexShader ? vertexShader->baseHash : 0,
					geometryShader ? geometryShader->baseHash : 0,
					pixelShader ? pixelShader->baseHash : 0,
					(uint32)primitiveMode,
					hostIndexCount);
		m_gameDrawCallsSubmitted++;
		return;
	}

	auto* d3dIndexAllocation = static_cast<D3D12IndexAllocation*>(indexAllocation.rendererInternal);
	const DXGI_FORMAT indexFormat = GetD3D12IndexFormat(hostIndexType);
	const uint32 hostIndexStride = hostIndexType == Renderer::INDEX_TYPE::U16 ? sizeof(uint16) :
		(hostIndexType == Renderer::INDEX_TYPE::U32 ? sizeof(uint32) : 0);
	const uint64 indexDataSize64 = (uint64)hostIndexCount * hostIndexStride;
	const uint32 indexDataSize = indexDataSize64 <= std::numeric_limits<uint32>::max() ?
		(uint32)indexDataSize64 : 0;
	bool indexAllocationValid = d3dIndexAllocation != nullptr;
	if (d3dIndexAllocation)
		indexAllocationValid = indexDataSize != 0 &&
			indexDataSize <= d3dIndexAllocation->size &&
			d3dIndexAllocation->GetDecodedCapacity() >= d3dIndexAllocation->size;
	if (hostIndexType != Renderer::INDEX_TYPE::NONE &&
		(!indexAllocationValid || indexFormat == DXGI_FORMAT_UNKNOWN))
	{
#if defined(CEMU_UWP_D3D12_CORRECTNESS_TELEMETRY)
		++m_correctnessDrawSkippedInvalidIndex;
#endif
		++m_droppedDraws[(size_t)DroppedDrawReason::InvalidIndexBuffer];
		static uint32 s_invalidIndexLogCount = 0;
		if (D3D12RuntimeLogCapped(s_invalidIndexLogCount, "invalid index buffer", 32))
		{
			cemuLog_log(LogType::Force, "D3D12: skipping indexed draw because decoded index buffer is unavailable (hostIndexType={} hostIndexCount={} count={} primitive={}).",
						(uint32)hostIndexType,
						hostIndexCount,
						count,
						(uint32)primitiveMode);
		}
		return;
	}

	switchProfileStage(UwpCpuDrawStage::DescriptorAndFeedbackSetup);
#if defined(CEMU_UWP_PERFORMANCE_EXPERIMENT)
	switchExperimentDetail(ExperimentDetailStage::Prepare);
#endif
	if (!noRasterizationOutput)
		TraceFboAttachments("draw_fbo_attachment_before", "before_draw", nextDrawId, identity.pipelineStateHash, "call=draw_execute stage=pre_state_bind");

	if (!m_commandListOpen)
		ResetCommandList();

	auto* commandList = GetCurrentCommandSlot().commandList.Get();
#if defined(CEMU_UWP_PERFORMANCE_EXPERIMENT)
	switchExperimentDetail(ExperimentDetailStage::Feedback);
#endif
#if defined(LATTE_UWP_DETAILED_PROFILING)
	m_uwpProfileThisDraw = profileThisDraw;
	const auto feedbackStarted = profileThisDraw ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
#endif
	ResolveFeedbackLoopsForDraw(vertexShader, geometryShader, noRasterizationOutput ? nullptr : pixelShader, !noRasterizationOutput);
#if defined(LATTE_UWP_DETAILED_PROFILING)
	if (profileThisDraw)
	{
		m_uwpDescriptorSplitNs[0] += (uint64)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - feedbackStarted).count();
		++m_uwpDescriptorSplitSamples;
	}
#endif
	// Scratch-allocation recovery can submit and reopen on another command slot.
#if defined(LATTE_UWP_DETAILED_PROFILING)
	const auto preTablesStarted = profileThisDraw ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
#endif
	if (!m_commandListOpen)
		ResetCommandList();
	commandList = GetCurrentCommandSlot().commandList.Get();
#if defined(CEMU_UWP_PERFORMANCE_EXPERIMENT)
	switchExperimentDetail(ExperimentDetailStage::Descriptors);
#endif
	++m_tableKeyMemoEpoch;
	const uint32 resourceDescriptorGeneration = m_descriptorManager->GetShaderVisibleCbvSrvUavRollovers();
	const uint32 samplerDescriptorGeneration = m_descriptorManager->GetShaderVisibleSamplerRollovers();
	const bool hasReusableGameNullResourceTable =
		m_gameNullResourceTable.IsValid() &&
		m_gameNullResourceTableGeneration == resourceDescriptorGeneration;
	const bool hasReusableGameNullUavTable =
		m_gameNullUavTable.IsValid() &&
		m_gameNullUavTableGeneration == resourceDescriptorGeneration;
	uint32 gameResourceDescriptorCount = 0;
	auto countResourceTableMiss = [this, hasReusableGameNullResourceTable, resourceDescriptorGeneration, &gameResourceDescriptorCount](LatteDecompilerShader* shader)
	{
		if (!shader)
		{
			if (!hasReusableGameNullResourceTable)
				gameResourceDescriptorCount += GetGameResourceTableDescriptorCount(nullptr);
			return;
		}
		if (!HasCachedGameResourceTable(shader, resourceDescriptorGeneration))
			gameResourceDescriptorCount += GetGameResourceTableDescriptorCount(shader);
	};
	countResourceTableMiss(vertexShader);
	countResourceTableMiss(pixelShader);
	countResourceTableMiss(geometryShader);
	uint32 gameUavDescriptorCount = 0;
	bool needsNullUavTable = useStencilTwoPass && streamoutEnable;
	auto countUavTable = [this, &gameUavDescriptorCount, &needsNullUavTable](LatteDecompilerShader* shader)
	{
		const uint32 count = GetGameUavTableDescriptorCount(shader);
		if (count != 0 && m_streamoutBuffer.resource)
			gameUavDescriptorCount += count;
		else
			needsNullUavTable = true;
	};
	countUavTable(vertexShader);
	countUavTable(pixelShader);
	countUavTable(geometryShader);
	if (needsNullUavTable && !hasReusableGameNullUavTable)
		gameUavDescriptorCount += kGameUavDescriptorCount;
	uint32 gameSamplerDescriptorCount = 0;
	auto countSamplerTableMiss = [this, samplerDescriptorGeneration, &gameSamplerDescriptorCount](LatteDecompilerShader* shader)
	{
		if (!HasCachedGameSamplerTable(shader, samplerDescriptorGeneration))
			gameSamplerDescriptorCount += GetGameSamplerTableDescriptorCount(shader);
	};
	countSamplerTableMiss(vertexShader);
	countSamplerTableMiss(pixelShader);
	countSamplerTableMiss(geometryShader);
	EnsureShaderVisibleDescriptorSpace(gameResourceDescriptorCount + gameUavDescriptorCount,
									   gameSamplerDescriptorCount);
	SetD3D12PipelineState(pipeline);
	m_gpuWatchdog.RecordGamePipeline(tracePipelineHash,
		vertexShader ? vertexShader->baseHash : 0,
		geometryShader ? geometryShader->baseHash : 0,
		pixelShader ? pixelShader->baseHash : 0);
#if defined(LATTE_UWP_DETAILED_PROFILING)
	const auto tablesStarted = profileThisDraw ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
	if (profileThisDraw)
		m_uwpDescriptorSplitNs[3] += (uint64)std::chrono::duration_cast<std::chrono::nanoseconds>(tablesStarted - preTablesStarted).count();
#endif
	BindGameDescriptorTables(vertexShader, geometryShader, pixelShader, baseVertex, baseInstance, vertexIdRemapMode, tracePipelineHash);
#if defined(LATTE_UWP_DETAILED_PROFILING)
	const auto tablesFinished = profileThisDraw ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
	if (profileThisDraw)
		m_uwpDescriptorSplitNs[2] += (uint64)std::chrono::duration_cast<std::chrono::nanoseconds>(tablesFinished - tablesStarted).count();
	m_uwpProfileThisDraw = false;
#endif
#if defined(CEMU_UWP_PERFORMANCE_EXPERIMENT)
	switchExperimentDetail(ExperimentDetailStage::RenderState);
#endif
	commandList = GetCurrentCommandSlot().commandList.Get();
	D3D12DescriptorManager::Allocation stencilNullUavTable;
	D3D12_GPU_DESCRIPTOR_HANDLE stencilVsUavTable{};
	D3D12_GPU_DESCRIPTOR_HANDLE stencilPsUavTable{};
	D3D12_GPU_DESCRIPTOR_HANDLE stencilGsUavTable{};
	if (useStencilTwoPass && streamoutEnable)
	{
		stencilVsUavTable = m_gameRootDescriptorTables[10];
		stencilPsUavTable = m_gameRootDescriptorTables[11];
		stencilGsUavTable = m_gameRootDescriptorTables[12];
		stencilNullUavTable = BuildGameUavTable(nullptr);
	}
	if (noRasterizationOutput)
		SetD3D12RenderTargets(0, nullptr, nullptr);
	else
		BindFramebuffer(m_activeFBO);
	SetD3D12Viewport(m_currentViewport);
	SetD3D12Scissor(m_currentScissor);
	const float blendConstantAlpha = RecordRegs().CB_BLEND_ALPHA.get_ALPHA();
	const bool splatBlendConstantAlpha = !m_alphaBlendFactorSupported &&
		D3D12BlendConstantNeedsAlphaSplat(RecordRegs(), noRasterizationOutput ? nullptr : m_activeFBO);
	const float blendFactor[4] =
	{
		splatBlendConstantAlpha ? blendConstantAlpha : RecordRegs().CB_BLEND_RED.get_RED(),
		splatBlendConstantAlpha ? blendConstantAlpha : RecordRegs().CB_BLEND_GREEN.get_GREEN(),
		splatBlendConstantAlpha ? blendConstantAlpha : RecordRegs().CB_BLEND_BLUE.get_BLUE(),
		blendConstantAlpha,
	};
	SetD3D12BlendFactor(blendFactor);
	const auto selectedStencilState = GetD3D12SelectedStencilState(RecordRegs(),
		GetD3D12StencilFaceCullState(cullState, rasterizesPointQuads));
#if defined(__ID3D12GraphicsCommandList8_INTERFACE_DEFINED__)
	auto& commandSlot = GetCurrentCommandSlot();
#endif
	if (useStencilTwoPass)
	{
		SetD3D12StencilRef(selectedStencilState.frontRef, selectedStencilState.frontRef);
	}
#if defined(__ID3D12GraphicsCommandList8_INTERFACE_DEFINED__)
	else if (selectedStencilState.needsIndependentFrontBackState &&
		m_independentFrontBackStencilSupported && commandSlot.commandList8)
	{
		SetD3D12StencilRef(selectedStencilState.frontRef, selectedStencilState.backRef);
	}
	else
#endif
	{
		SetD3D12StencilRef(selectedStencilState.ref, selectedStencilState.ref);
	}

#if defined(CEMU_UWP_D3D12_DIAGNOSTICS)
	if (priorityStencilDraw && D3D12TraceSink::Get().IsPriorityStencilDraw(nextDrawId))
	{
		const auto& depth = RecordRegs().DB_DEPTH_CONTROL;
		const bool back = depth.get_BACK_STENCIL_ENABLE();
		bool nativeIndependentRefs = false;
#if defined(__ID3D12GraphicsCommandList8_INTERFACE_DEFINED__)
		nativeIndependentRefs = !useStencilTwoPass && selectedStencilState.needsIndependentFrontBackState &&
			m_independentFrontBackStencilSupported && commandSlot.commandList8;
#endif
		const bool nativeIndependentMasks = nativeIndependentRefs &&
			(selectedStencilState.frontReadMask != selectedStencilState.backReadMask ||
			 selectedStencilState.frontWriteMask != selectedStencilState.backWriteMask);
		D3D12TraceEvent("draw_stencil_state", "draw={} pipeline=0x{:016x} phase=after_om_bind priority=stencil_enabled snapshotKind=cpu_selected_state gpuStateVerified=0 vs=0x{:016x} gs=0x{:016x} ps=0x{:016x} rawDepthControl=0x{:08x} rawStencilF=0x{:08x} rawStencilB=0x{:08x} nativeDepthEnable={} nativeDepthWrite={} nativeDepthFunc={} stencilEnable={} backEnable={} selectedBack={} selectedRef={} selectedRead=0x{:02x} selectedWrite=0x{:02x} frontRef={} frontRead=0x{:02x} frontWrite=0x{:02x} backRef={} backRead=0x{:02x} backWrite=0x{:02x} independentNeeded={} nativeIndependentRefs={} nativeIndependentMasks={} twoPass={} firstPassRef={} nativeFrontOps=func:{}/fail:{}/zfail:{}/pass:{} nativeBackOps=func:{}/fail:{}/zfail:{}/pass:{} rawColorControl=0x{:08x} rawBlend0=0x{:08x} blendFactor={:.9g},{:.9g},{:.9g},{:.9g} effectiveTargetMask=0x{:08x} dsvFormat={} stencilFormat={}",
			nextDrawId, tracePipelineHash, vertexShader ? vertexShader->baseHash : 0,
			geometryShader ? geometryShader->baseHash : 0, pixelShader ? pixelShader->baseHash : 0,
			depth.getRawValue(), RecordRegs().DB_STENCILREFMASK.getRawValue(),
			RecordRegs().DB_STENCILREFMASK_BF.getRawValue(),
			depth.get_Z_ENABLE(), depth.get_Z_WRITE_ENABLE(), (uint32)GetD3D12CompareFunc(depth.get_Z_FUNC()),
			depth.get_STENCIL_ENABLE(), back, selectedStencilState.usedBackFace,
			selectedStencilState.ref, selectedStencilState.readMask, selectedStencilState.writeMask,
			selectedStencilState.frontRef, selectedStencilState.frontReadMask, selectedStencilState.frontWriteMask,
			selectedStencilState.backRef, selectedStencilState.backReadMask, selectedStencilState.backWriteMask,
			selectedStencilState.needsIndependentFrontBackState, nativeIndependentRefs, nativeIndependentMasks,
			useStencilTwoPass, useStencilTwoPass ? selectedStencilState.frontRef : selectedStencilState.ref,
			(uint32)GetD3D12CompareFunc(depth.get_STENCIL_FUNC_F()), (uint32)GetD3D12StencilOp(depth.get_STENCIL_FAIL_F()),
			(uint32)GetD3D12StencilOp(depth.get_STENCIL_ZFAIL_F()), (uint32)GetD3D12StencilOp(depth.get_STENCIL_ZPASS_F()),
			(uint32)GetD3D12CompareFunc(back ? depth.get_STENCIL_FUNC_B() : depth.get_STENCIL_FUNC_F()),
			(uint32)GetD3D12StencilOp(back ? depth.get_STENCIL_FAIL_B() : depth.get_STENCIL_FAIL_F()),
			(uint32)GetD3D12StencilOp(back ? depth.get_STENCIL_ZFAIL_B() : depth.get_STENCIL_ZFAIL_F()),
			(uint32)GetD3D12StencilOp(back ? depth.get_STENCIL_ZPASS_B() : depth.get_STENCIL_ZPASS_F()),
			RecordRegs().CB_COLOR_CONTROL.getRawValue(), RecordRegs().CB_BLENDN_CONTROL[0].getRawValue(),
			blendFactor[0], blendFactor[1], blendFactor[2], blendFactor[3], effectiveRenderTargetMask,
			(uint32)m_activeFBO->GetDepthFormat(), (uint32)m_activeFBO->GetStencilFormat());
	}
#endif

#if defined(CEMU_UWP_PERFORMANCE_EXPERIMENT)
	switchExperimentDetail(ExperimentDetailStage::IndexResidency);
#endif
	if (hostIndexType != Renderer::INDEX_TYPE::NONE &&
		!EnsureIndexResident(*d3dIndexAllocation, indexDataSize))
	{
#if defined(CEMU_UWP_D3D12_CORRECTNESS_TELEMETRY)
		++m_correctnessDrawSkippedInvalidIndex;
#endif
		++m_droppedDraws[(size_t)DroppedDrawReason::IndexUploadFailed];
		static uint32 s_indexResidencyFailureLogCount = 0;
		if (D3D12RuntimeLogCapped(s_indexResidencyFailureLogCount, "index upload-ring residency failure", 32))
		{
			cemuLog_log(LogType::Force,
				"D3D12 UWP: skipping indexed draw because {} decoded index bytes could not be made resident for fence {}.",
				indexDataSize,
				(unsigned long long)m_nextFenceValue);
		}
		return;
	}
	commandList = GetCurrentCommandSlot().commandList.Get();
#if defined(CEMU_UWP_PERFORMANCE_EXPERIMENT)
	finishExperimentDetail();
	experimentDetailActive = false;
#endif
#if defined(LATTE_UWP_DETAILED_PROFILING)
	if (profileThisDraw)
		m_uwpDescriptorSplitNs[4] += (uint64)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - tablesFinished).count();
#endif
	switchProfileStage(UwpCpuDrawStage::DrawRecording);
	if (hostIndexType != Renderer::INDEX_TYPE::NONE)
	{
		D3D12_INDEX_BUFFER_VIEW indexView{};
		ID3D12Resource* indexResource = d3dIndexAllocation->residentResource;
		indexView.BufferLocation = indexResource->GetGPUVirtualAddress() +
			d3dIndexAllocation->residentOffset;
		indexView.SizeInBytes = indexDataSize;
		indexView.Format = indexFormat;
		D3D12TraceEvent("draw_index_buffer", "draw={} pipeline=0x{:016x} allocation={} resource={} gpuVA=0x{:016x} size={} format={} hostIndexType={} hostIndexCount={} baseVertex={} indexMin={} indexMax={}",
						(unsigned long long)nextDrawId,
						(unsigned long long)tracePipelineHash,
						(const void*)d3dIndexAllocation,
						(const void*)indexResource,
						(unsigned long long)indexView.BufferLocation,
						indexView.SizeInBytes,
						(uint32)indexView.Format,
						(uint32)hostIndexType,
						hostIndexCount,
						baseVertex,
						indexMin,
						indexMax);
		if (D3D12ShouldTrace())
		{
			const uint32 previewCount = std::min<uint32>(hostIndexCount, 24);
			std::string indexValues;
			const uint8* previewData = d3dIndexAllocation->previewData.data();
			const size_t previewDataSize = d3dIndexAllocation->previewDataSize;
			for (uint32 i = 0; i < previewCount; ++i)
			{
				uint32 value = 0;
				if (hostIndexType == Renderer::INDEX_TYPE::U16 && (size_t)i * sizeof(uint16) + sizeof(uint16) <= previewDataSize)
					memcpy(&value, previewData + (size_t)i * sizeof(uint16), sizeof(uint16));
				else if (hostIndexType == Renderer::INDEX_TYPE::U32 && (size_t)i * sizeof(uint32) + sizeof(uint32) <= previewDataSize)
					memcpy(&value, previewData + (size_t)i * sizeof(uint32), sizeof(uint32));
				if (!indexValues.empty())
					indexValues.push_back(',');
				fmt::format_to(std::back_inserter(indexValues), "{}", value);
			}
			D3D12TraceEvent("draw_index_preview", "draw={} pipeline=0x{:016x} hostIndexType={} hostIndexCount={} previewCount={} previewBytes={} baseVertex={} indexMin={} indexMax={} values={}",
							(unsigned long long)nextDrawId,
							(unsigned long long)tracePipelineHash,
							(uint32)hostIndexType,
							hostIndexCount,
							previewCount,
							previewDataSize,
							baseVertex,
							indexMin,
							indexMax,
							indexValues);
		}
		SetD3D12IndexBuffer(indexView);
	}
	else
	{
		D3D12TraceEvent("draw_index_buffer", "draw={} pipeline=0x{:016x} allocation=null nonIndexed=1 hostIndexType={} hostIndexCount={} baseVertex={} count={}",
						(unsigned long long)nextDrawId,
						(unsigned long long)tracePipelineHash,
						(uint32)hostIndexType,
						hostIndexCount,
						baseVertex,
						count);
	}
	SetD3D12PrimitiveTopology(d3dPrimitiveTopology);
	const bool dynamicDepthBias = UsesDynamicDepthBias(rasterizesPointQuads);
	const float dynamicDepthBiasConstant = RecordRegs().PA_SU_POLY_OFFSET_FRONT_OFFSET.get_OFFSET();
	const float dynamicDepthBiasClamp = RecordRegs().PA_SU_POLY_OFFSET_CLAMP.get_CLAMP();
	const float dynamicDepthBiasSlope = RecordRegs().PA_SU_POLY_OFFSET_FRONT_SCALE.get_SCALE() / 16.0f;
	const auto setDrawOffsetConstant = [&](uint32 constantIndex, uint32 value)
	{
		if (m_gameRootDrawConstantsValid && m_gameRootDrawConstants[constantIndex] == value)
			return;
		commandList->SetGraphicsRoot32BitConstant(6, value, constantIndex);
		m_gameRootDrawConstants[constantIndex] = value;
	};
	// base vertex/instance offsets are applied in HLSL; IA offsets stay zero
	const auto issueDraw = [&](uint32 start, uint32 drawCount, uint32 startInstance, uint32 drawInstanceCount)
	{
		if (dynamicDepthBias)
			SetD3D12DepthBias(dynamicDepthBiasConstant, dynamicDepthBiasClamp, dynamicDepthBiasSlope);
		const bool indexed = hostIndexType != Renderer::INDEX_TYPE::NONE;
		setDrawOffsetConstant(1, indexed ? baseVertex : baseVertex + start);
		setDrawOffsetConstant(2, baseInstance + startInstance);
		if (indexed)
			commandList->DrawIndexedInstanced(drawCount, drawInstanceCount, start, 0, 0);
		else
			commandList->DrawInstanced(drawCount, drawInstanceCount, 0, 0);
	};
	uint32 recordedNativeDraws = 0;
#if defined(CEMU_UWP_D3D12_DIAGNOSTICS)
	int geometryProbeIndex = -1;
	// exclude draws that may submit or restart command lists between query Begin/End
	const auto& probeDepth = RecordRegs().DB_DEPTH_CONTROL;
	const bool probeDepthWrite = probeDepth.get_Z_ENABLE() && probeDepth.get_Z_WRITE_ENABLE();
	const bool probeReadonlyColor = probeDepth.get_Z_ENABLE() && !probeDepthWrite;
	if (m_geometryProbe && !noRasterizationOutput && !streamoutEnable && !geometryShader &&
		!usesPointExpansionGS && !useOrderedPrimitiveFallback && !useStencilTwoPass &&
		primitiveMode == Latte::LATTE_VGT_PRIMITIVE_TYPE::E_PRIMITIVE_TYPE::TRIANGLES &&
		hostIndexCount >= 3 && instanceCount != 0 && m_activeFBO &&
		m_activeFBO->depthBuffer.texture && (probeDepthWrite || probeReadonlyColor) &&
		m_geometryProbe->WantsSample(tracePipelineHash, probeDepthWrite))
	{
		const auto* depthView = m_activeFBO->depthBuffer.texture;
		std::string metadata = fmt::format(
			"draw={} pipeline=0x{:016x} vs=0x{:016x} ps=0x{:016x} depthAddr=0x{:08x} indices={} instances={} "
			"baseVertex={} indexMax={} zWrite={} zFunc={} alpha={} alphaFunc={} alphaRef=0x{:08x} "
			"targetMask=0x{:08x} psOutputMask=0x{:02x} viewport={},{}:{}x{} depthRange={}..{} "
			"scissor={},{}-{},{} cullFront={} cullBack={} frontFace={} sources={}",
			nextDrawId, tracePipelineHash, vertexShader ? vertexShader->baseHash : 0,
			pixelShader ? pixelShader->baseHash : 0, depthView->baseTexture->physAddress,
			hostIndexCount, instanceCount, baseVertex, indexMax, probeDepthWrite,
			(uint32)probeDepth.get_Z_FUNC(),
			RecordRegs().SX_ALPHA_TEST_CONTROL.get_ALPHA_TEST_ENABLE(),
			(uint32)RecordRegs().SX_ALPHA_TEST_CONTROL.get_ALPHA_FUNC(),
			RecordRegs().SX_ALPHA_REF.getRawValue(), effectiveRenderTargetMask,
			pixelShader ? pixelShader->pixelColorOutputMask : 0,
			m_currentViewport.TopLeftX, m_currentViewport.TopLeftY,
			m_currentViewport.Width, m_currentViewport.Height,
			m_currentViewport.MinDepth, m_currentViewport.MaxDepth,
			m_currentScissor.left, m_currentScissor.top, m_currentScissor.right, m_currentScissor.bottom,
			cullState.cullFront, cullState.cullBack, (uint32)polygonControlReg.get_FRONT_FACE(),
			BuildShaderTextureSourceSummary(vertexShader, geometryShader, pixelShader));
		metadata += " " + BuildGeometryProbeResourceMetadata(vertexShader, geometryShader, pixelShader);
		geometryProbeIndex = m_geometryProbe->Begin(commandList, tracePipelineHash, probeDepthWrite,
			m_activeOcclusionQueries.empty(), metadata);
	}
#endif
	auto setUavTable = [this, commandList](uint32 rootIndex, D3D12_GPU_DESCRIPTOR_HANDLE table)
	{
		if (rootIndex < m_gameRootDescriptorTableValid.size() &&
			m_gameRootDescriptorTableValid[rootIndex] &&
			m_gameRootDescriptorTables[rootIndex].ptr == table.ptr)
		{
			return;
		}
		commandList->SetGraphicsRootDescriptorTable(rootIndex, table);
		if (rootIndex < m_gameRootDescriptorTableValid.size())
		{
			m_gameRootDescriptorTables[rootIndex] = table;
			m_gameRootDescriptorTableValid[rootIndex] = true;
		}
	};
	const auto bindFirstPassUavs = [&]()
	{
		if (!useStencilTwoPass || !streamoutEnable)
			return;
		setUavTable(10, stencilVsUavTable);
		setUavTable(11, stencilPsUavTable);
		setUavTable(12, stencilGsUavTable);
	};
	const auto bindNullUavs = [&]()
	{
		if (!useStencilTwoPass || !streamoutEnable || !stencilNullUavTable.IsValid())
			return;
		setUavTable(10, stencilNullUavTable.gpu);
		setUavTable(11, stencilNullUavTable.gpu);
		setUavTable(12, stencilNullUavTable.gpu);
	};
	if (useOrderedPrimitiveFallback)
	{
		const bool primitiveRestartEnabled = GetD3D12PrimitiveRestartEnabled(RecordRegs(), primitiveMode, hostIndexType);
		const auto primitiveSpans = BuildD3D12OrderedPrimitiveSpans(
			d3dPrimitiveTopology,
			hostIndexCount,
			hostIndexType,
			d3dIndexAllocation ? d3dIndexAllocation->GetDecodedData() : nullptr,
			primitiveRestartEnabled);
		if (primitiveSpans.empty() && hostIndexCount != 0)
		{
			cemuLog_log(LogType::Force,
				"D3D12 UWP: ordered draw fallback could not split topology {} with {} elements; preserving the original draw.",
				(uint32)d3dPrimitiveTopology, hostIndexCount);
			issueDraw(0, hostIndexCount, 0, instanceCount);
			recordedNativeDraws = instanceCount != 0 ? 1 : 0;
		}
		else
		{
			const uint64 totalLogicalPrimitives = (uint64)primitiveSpans.size() * instanceCount;
#if defined(CEMU_UWP_D3D12_WORKLOAD_PROFILE)
			m_uwpWorkloadProfile.logicalPrimitives += totalLogicalPrimitives;
#endif
			uint64 completedLogicalPrimitives = 0;
			bool refreshLogicDestination = logicOpNeedsOrderedDestination;
			for (uint32 instance = 0; instance < instanceCount; ++instance)
			{
				for (const auto& span : primitiveSpans)
				{
					ID3D12PipelineState* firstPipeline = span.flipWinding ? flippedPipeline : pipeline;
					SetD3D12PipelineState(firstPipeline);
					if (useStencilTwoPass)
						SetD3D12StencilRef(selectedStencilState.frontRef, selectedStencilState.frontRef);
					bindFirstPassUavs();
					issueDraw(span.start, span.count, instance, 1);
					++recordedNativeDraws;

					if (useStencilTwoPass)
					{
						SetD3D12PipelineState(span.flipWinding ? backFlippedStencilPipeline : backStencilPipeline);
						SetD3D12StencilRef(selectedStencilState.backRef, selectedStencilState.backRef);
						bindNullUavs();
						issueDraw(span.start, span.count, instance, 1);
						++recordedNativeDraws;
					}

					++completedLogicalPrimitives;
					if (refreshLogicDestination && completedLogicalPrimitives < totalLogicalPrimitives &&
						!RefreshLogicOpScratchCopiesForDraw(nextDrawId))
					{
						cemuLog_logOnce(LogType::Force,
							"D3D12 UWP: logic-op destination refresh failed; later primitives will use the last valid destination snapshot.");
						refreshLogicDestination = false;
					}
				}
			}
			bindFirstPassUavs();
		}
	}
	else if (useStencilTwoPass)
	{
		issueDraw(0, hostIndexCount, 0, instanceCount);
		recordedNativeDraws = instanceCount != 0 ? 1 : 0;
		SetD3D12PipelineState(backStencilPipeline);
		SetD3D12StencilRef(selectedStencilState.backRef, selectedStencilState.backRef);
		bindNullUavs();
		issueDraw(0, hostIndexCount, 0, instanceCount);
		if (instanceCount != 0)
			++recordedNativeDraws;
		bindFirstPassUavs();
	}
	else
	{
		issueDraw(0, hostIndexCount, 0, instanceCount);
		recordedNativeDraws = instanceCount != 0 ? 1 : 0;
	}
#if defined(CEMU_UWP_D3D12_DIAGNOSTICS)
	if (geometryProbeIndex >= 0)
		m_geometryProbe->End(commandList, geometryProbeIndex, m_nextFenceValue);
#endif
#if defined(CEMU_UWP_PERFORMANCE_EXPERIMENT)
	if (m_performanceExperiment.Measuring() && recordedNativeDraws != 0)
	{
		m_performanceExperimentWindow.uniformOrEligible += uniformOrEligible ? 1u : 0u;
		m_performanceExperimentWindow.uniformOrBatched += uniformOrBatch ? 1u : 0u;
		m_performanceExperimentWindow.nativeDraws += recordedNativeDraws;
	}
#endif
	switchProfileStage(UwpCpuDrawStage::RetirementReadbackAndSubmit);
	if (recordedNativeDraws != 0 && !noRasterizationOutput && m_activeFBO)
	{
		const uint32 pixelOutputMask = pixelShader ? pixelShader->pixelColorOutputMask : 0u;
		const auto& attachments = m_activeFBO->GetColorAttachments();
		for (uint32 i = 0; i < attachments.size() && i < 8; ++i)
		{
			const bool shaderWritesTarget = (pixelOutputMask & (1u << i)) != 0;
			const bool targetWriteEnabled = ((effectiveRenderTargetMask >> (i * 4)) & 0xFu) != 0;
			if (shaderWritesTarget && targetWriteEnabled && attachments[i].valid && attachments[i].view)
				MarkTextureViewWritten(attachments[i].view);
		}
		if (m_activeFBO->HasDepthStencilAttachment() && m_activeFBO->depthBuffer.texture)
		{
			const auto& depthControl = RecordRegs().DB_DEPTH_CONTROL;
			const bool depthWriteEnabled = depthControl.get_Z_ENABLE() && depthControl.get_Z_WRITE_ENABLE();
			const bool stencilWriteEnabled = m_activeFBO->depthBuffer.hasStencil &&
				depthControl.get_STENCIL_ENABLE() &&
				(selectedStencilState.frontWriteMask | selectedStencilState.backWriteMask) != 0;
			if (depthWriteEnabled || stencilWriteEnabled)
			{
				MarkTextureViewWritten(static_cast<LatteTextureViewD3D12*>(m_activeFBO->depthBuffer.texture),
					stencilWriteEnabled, depthWriteEnabled);
			}
		}
	}
	D3D12TraceEventFlush("draw_submit", "draw={} pipeline=0x{:016x} hostIndexType={} hostIndexCount={} instances={} primitive={} d3dTopology={} fbo=0x{:016x} noRaster={} pointGS={} rectOrQuad={} descriptorRollovers={} resourceCacheHitMiss={}/{} samplerCacheHitMiss={}/{}",
						 (unsigned long long)nextDrawId,
						 identity.pipelineStateHash,
						 (uint32)hostIndexType,
						 hostIndexCount,
						 instanceCount,
						 (uint32)primitiveMode,
						 (uint32)d3dPrimitiveTopology,
						 m_activeFBO ? m_activeFBO->key : 0,
						 noRasterizationOutput ? 1 : 0,
						 usesPointExpansionGS ? 1 : 0,
						 (primitiveMode == Latte::LATTE_VGT_PRIMITIVE_TYPE::E_PRIMITIVE_TYPE::RECTS || primitiveMode == Latte::LATTE_VGT_PRIMITIVE_TYPE::E_PRIMITIVE_TYPE::QUADS) ? 1 : 0,
						 m_descriptorManager ? m_descriptorManager->GetShaderVisibleRollovers() : 0,
						 (unsigned long long)m_gameResourceTableCacheHits,
						 (unsigned long long)m_gameResourceTableCacheMisses,
						 (unsigned long long)m_gameSamplerTableCacheHits,
						 (unsigned long long)m_gameSamplerTableCacheMisses);
	if (D3D12ShouldTrace() && !noRasterizationOutput && m_activeFBO)
	{
		const std::string sourceSummary = BuildShaderTextureSourceSummary(vertexShader, geometryShader, pixelShader);
		const auto& colorAttachments = m_activeFBO->GetColorAttachments();
		for (uint32 i = 0; i < colorAttachments.size(); ++i)
		{
			const auto& attachment = colorAttachments[i];
			if (!attachment.valid || !attachment.view)
				continue;
			RecordTextureLineage("draw", attachment.view, nextDrawId, identity.pipelineStateHash, sourceSummary,
								 fmt::format("primitive={} d3dTopology={} rtIndex={} hostIndexType={} hostIndexCount={} viewport={},{}:{}x{} scissor={},{}-{},{}",
											 (uint32)primitiveMode,
											 (uint32)d3dPrimitiveTopology,
											 i,
											 (uint32)hostIndexType,
											 hostIndexCount,
											 m_currentViewport.TopLeftX,
											 m_currentViewport.TopLeftY,
											 m_currentViewport.Width,
											 m_currentViewport.Height,
											 m_currentScissor.left,
											 m_currentScissor.top,
											 m_currentScissor.right,
											 m_currentScissor.bottom));
		}
		if (m_activeFBO->HasDepthStencilAttachment() && m_activeFBO->depthBuffer.texture)
		{
			const auto& depthControl = RecordRegs().DB_DEPTH_CONTROL;
			const bool depthWriteEnabled = depthControl.get_Z_ENABLE() && depthControl.get_Z_WRITE_ENABLE();
			const bool stencilWriteEnabled = m_activeFBO->depthBuffer.hasStencil &&
				depthControl.get_STENCIL_ENABLE() &&
				selectedStencilState.writeMask != 0;
			if (depthWriteEnabled || stencilWriteEnabled)
			{
				RecordTextureLineage("draw_depth", static_cast<LatteTextureViewD3D12*>(m_activeFBO->depthBuffer.texture), nextDrawId, identity.pipelineStateHash, sourceSummary,
									 fmt::format("primitive={} hostIndexType={} hostIndexCount={} zWrite={} stencilWrite={} stencilMask=0x{:02x}",
												 (uint32)primitiveMode,
												 (uint32)hostIndexType,
												 hostIndexCount,
												 depthWriteEnabled ? 1 : 0,
												 stencilWriteEnabled ? 1 : 0,
												 (uint32)selectedStencilState.writeMask));
			}
			else
			{
				D3D12TraceEvent("draw_depth_readonly",
								"draw={} pipeline=0x{:016x} primitive={} hostIndexType={} hostIndexCount={} zEnable={} zWrite={} stencilEnable={} stencilMask=0x{:02x}",
								(unsigned long long)nextDrawId,
								identity.pipelineStateHash,
								(uint32)primitiveMode,
								(uint32)hostIndexType,
								hostIndexCount,
								depthControl.get_Z_ENABLE() ? 1 : 0,
								depthControl.get_Z_WRITE_ENABLE() ? 1 : 0,
								depthControl.get_STENCIL_ENABLE() ? 1 : 0,
								(uint32)selectedStencilState.writeMask);
			}
		}
	}

#if defined(CEMU_UWP_D3D12_WORKLOAD_PROFILE)
	auto& workload = m_uwpWorkloadProfile;
	// diagnostic state only, never a rendering predicate
	static std::array<std::pair<uint64, uint32>, 32> s_orObservedPipelines{};
	static uint32 s_orObservedPipelineCount = 0;
	static uint32 s_orPassRecordCount = 0;
	if (m_logicOpEmulationMode == 2 && useOrderedPrimitiveFallback && m_activeFBO &&
		s_orPassRecordCount < s_orObservedPipelines.size())
	{
		auto observed = std::find_if(s_orObservedPipelines.begin(),
			s_orObservedPipelines.begin() + s_orObservedPipelineCount,
			[&](const auto& entry) { return entry.first == identity.pipelineStateHash; });
		const bool firstObservation = observed == s_orObservedPipelines.begin() + s_orObservedPipelineCount;
		if (firstObservation || recordedNativeDraws > observed->second)
		{
			if (firstObservation)
				++s_orObservedPipelineCount;
			*observed = { identity.pipelineStateHash, recordedNativeDraws };
			const uint32 record = ++s_orPassRecordCount;
			const auto& lcr = RecordRegs();
			const auto& depth = lcr.DB_DEPTH_CONTROL;
			const bool hasDepth = m_activeFBO->HasDepthStencilAttachment();
			const bool hasStencil = hasDepth && m_activeFBO->GetStencilFormat() != DXGI_FORMAT_UNKNOWN;
			const uint32 outputMask = pixelShader ? pixelShader->pixelColorOutputMask : 0;
			const uint32 targetMask = GetD3D12EffectiveRenderTargetMask(lcr);
			sint32 sourceContainsDiscard = -1;
			if (pixelShader && pixelShader->strBuf_shaderSource && pixelShader->strBuf_shaderSource->getLen() != 0)
			{
				const std::string_view source(pixelShader->strBuf_shaderSource->c_str(), pixelShader->strBuf_shaderSource->getLen());
				sourceContainsDiscard = source.find("discard") != std::string_view::npos ? 1 : 0;
			}
			cemuLog_log(LogType::Force,
				"D3D12 UWP OR pass: record={} pipeline={:016x} native={} indices={} instances={} primitive={} vs={:016x} ps={:016x} gs={:016x} streamout={} stencilSplit={} depthExport={} sourceDiscard={} customPS={} psOut=0x{:02x} targetMask=0x{:08x} scratchEntries={} activeOcclusionQueries={}.",
				record, identity.pipelineStateHash, recordedNativeDraws, hostIndexCount, instanceCount, (uint32)primitiveMode,
				vertexShader ? vertexShader->baseHash : 0, pixelShader ? pixelShader->baseHash : 0,
				geometryShader ? geometryShader->baseHash : 0, streamoutEnable, useStencilTwoPass,
				pixelShader ? pixelShader->depthMask : false, sourceContainsDiscard,
				pixelShader ? pixelShader->isCustomShader : false, outputMask, targetMask, m_feedbackScratchCopies.size(),
				m_activeOcclusionQueries.size());
			cemuLog_log(LogType::Force,
				"D3D12 UWP OR raster: record={} viewportValid={} viewport=({}, {}) {}x{} depth={}..{} scissorValid={} scissor=({}, {})-({}, {}) depthAttached={} zEnable={} zWrite={} zFunc={} stencilAttached={} stencilEnable={} backEnable={} stencilFuncF/B={}/{} opsF_fail/zfail/zpass={}/{}/{} opsB={}/{}/{} masksF_read/write=0x{:02x}/0x{:02x} masksB=0x{:02x}/0x{:02x} refsF/B={}/{} alphaTest={} alphaFunc={} alphaRefRaw=0x{:08x}.",
				record, m_boundViewportValid, m_boundViewport.TopLeftX, m_boundViewport.TopLeftY,
				m_boundViewport.Width, m_boundViewport.Height, m_boundViewport.MinDepth, m_boundViewport.MaxDepth,
				m_boundScissorValid, m_boundScissor.left, m_boundScissor.top, m_boundScissor.right, m_boundScissor.bottom,
				hasDepth, depth.get_Z_ENABLE(), depth.get_Z_WRITE_ENABLE(), (uint32)depth.get_Z_FUNC(),
				hasStencil, depth.get_STENCIL_ENABLE(), depth.get_BACK_STENCIL_ENABLE(),
				(uint32)depth.get_STENCIL_FUNC_F(), (uint32)depth.get_STENCIL_FUNC_B(),
				(uint32)depth.get_STENCIL_FAIL_F(), (uint32)depth.get_STENCIL_ZFAIL_F(), (uint32)depth.get_STENCIL_ZPASS_F(),
				(uint32)depth.get_STENCIL_FAIL_B(), (uint32)depth.get_STENCIL_ZFAIL_B(), (uint32)depth.get_STENCIL_ZPASS_B(),
				selectedStencilState.frontReadMask, selectedStencilState.frontWriteMask,
				selectedStencilState.backReadMask, selectedStencilState.backWriteMask,
				selectedStencilState.frontRef, selectedStencilState.backRef,
				lcr.SX_ALPHA_TEST_CONTROL.get_ALPHA_TEST_ENABLE(), (uint32)lcr.SX_ALPHA_TEST_CONTROL.get_ALPHA_FUNC(),
				lcr.SX_ALPHA_REF.getRawValue());
			const auto& attachments = m_activeFBO->GetColorAttachments();
			for (uint32 target = 0; target < attachments.size() && target < 8; ++target)
			{
				const auto& attachment = attachments[target];
				if (!attachment.valid || !attachment.view || (outputMask & (1u << target)) == 0 ||
					((targetMask >> (target * 4)) & 0xF) == 0)
					continue;
				auto* texture = attachment.view->GetBaseImage();
				if (!texture)
					continue;
				const auto& resourceDesc = texture->GetDesc();
				const auto* scratch = GetLogicOpScratchCopy(target);
				cemuLog_log(LogType::Force,
					"D3D12 UWP OR target: record={} target={} resourceFormat={} rtvFormat={} scratchSrvFormat={} resourceSize={}x{} arrayDepth={} mips={} samples={} viewMip={}+{} viewSlice={}+{} scratchSize={}x{} writeMask=0x{:x}.",
					record, target, (uint32)resourceDesc.Format, (uint32)attachment.format,
					scratch ? (uint32)scratch->srvDesc.Format : 0,
					resourceDesc.Width, resourceDesc.Height, resourceDesc.DepthOrArraySize, resourceDesc.MipLevels,
					resourceDesc.SampleDesc.Count, attachment.view->firstMip, attachment.view->numMip,
					attachment.view->firstSlice, attachment.view->numSlice,
					scratch ? scratch->requestedDesc.Width : 0, scratch ? scratch->requestedDesc.Height : 0,
					(targetMask >> (target * 4)) & 0xF);
			}
		}
	}
	++workload.guestDraws;
	workload.nativeDraws += recordedNativeDraws;
	workload.orderedDraws += useOrderedPrimitiveFallback ? 1u : 0u;
	workload.stencilDraws += useStencilTwoPass ? 1u : 0u;
	if (m_logicOpEmulationMode < workload.modeDraws.size())
	{
		++workload.modeDraws[m_logicOpEmulationMode];
		workload.modeNativeDraws[m_logicOpEmulationMode] += recordedNativeDraws;
	}
	if (recordedNativeDraws > workload.maxNativeDraws)
	{
		workload.maxNativeDraws = recordedNativeDraws;
		workload.maxDrawPipeline = identity.pipelineStateHash;
		workload.maxDrawVs = vertexShader ? vertexShader->baseHash : 0;
		workload.maxDrawPs = pixelShader ? pixelShader->baseHash : 0;
		workload.maxDrawMode = m_logicOpEmulationMode;
		workload.maxDrawIndices = hostIndexCount;
		workload.maxDrawInstances = instanceCount;
	}
#endif
	// the submit budget counts guest draws; ordered replays split one draw into many native draws
	m_recordedDrawcalls += useOrderedPrimitiveFallback ? std::min<uint32>(recordedNativeDraws, 1u) : recordedNativeDraws;
	m_gameDrawCallsSubmitted++;
	if (noRasterizationOutput)
		m_noRasterStreamoutDraws++;
	if (usesPointExpansionGS)
		m_pointExpansionDraws++;
	if (!noRasterizationOutput)
		LogActiveRenderTargetStats();
	if (!noRasterizationOutput)
		TraceFboAttachments("draw_fbo_attachment_after", "after_draw", m_gameDrawCallsSubmitted, identity.pipelineStateHash, "call=draw_execute");
	static uint32 s_gameDrawSubmittedLogCount = 0;
	if (D3D12RuntimeLogCapped(s_gameDrawSubmittedLogCount, "game draw submitted", 96))
	{
		cemuLog_log(LogType::Force, "D3D12: submitted game draw #{} hostIndexCount={} hostIndexType={} instances={} pipeline=0x{:016x}.",
					(unsigned long long)m_gameDrawCallsSubmitted,
					hostIndexCount,
					(uint32)hostIndexType,
					instanceCount,
					identity.pipelineStateHash);
	}
	const bool scratchBudgetReached = IsCommandScratchBudgetReached();
	if (m_recordedDrawcalls >= m_submitThreshold || scratchBudgetReached)
		ExecuteCurrentCommandList((m_recordedDrawcalls >= m_submitThreshold ? SubmitDrawBudget : 0u) |
			(scratchBudgetReached ? SubmitScratchBudget : 0u));
	};
	if (queueTail)
		QueueRecording(std::move(recordDraw));
	else
		recordDraw();

	// Latte-side bookkeeping once the draw is queued: render targets count as GPU-written, and a readback that
	// started needs its command list submitted
	if (pixelShader && !noRasterizationOutput)
		LatteRenderTarget_trackUpdates();
	if (LatteTextureReadback_Update())
	{
		auto submitReadback = [this]() { ExecuteCurrentCommandList(SubmitReadback); };
		if (ShouldQueueRecording())
			QueueRecording(submitReadback);
		else
			submitReadback();
	}
}

void D3D12Renderer::draw_endSequence()
{
	m_drawSequenceActive = false;
}

bool D3D12Renderer::EnsureIndexResident(D3D12IndexAllocation& allocation, uint32 dataSize)
{
	if (!m_commandListOpen || dataSize == 0 || dataSize > allocation.size ||
		allocation.GetDecodedCapacity() < dataSize)
		return false;

	if (allocation.residentResource && allocation.residentFence == m_nextFenceValue &&
		allocation.residentSize == dataSize)
	{
		++m_uwpBufferProfile.indexRingSameFenceHits;
		return true;
	}

	// never carry a resident pointer from a completed command list into a later one
	allocation.residentResource = nullptr;
	allocation.residentOffset = 0;
	allocation.residentFence = 0;
	allocation.residentSize = 0;
	const auto upload = m_memoryManager->AllocateUpload(dataSize, 32, m_nextFenceValue);
	if (!upload.IsValid())
		return false;

	memcpy(upload.cpuPtr, allocation.GetDecodedData(), dataSize);
	allocation.residentResource = upload.resource;
	allocation.residentOffset = upload.offset;
	allocation.residentFence = m_nextFenceValue;
	allocation.residentSize = dataSize;
	++m_uwpBufferProfile.indexRingUploads;
	m_uwpBufferProfile.indexRingUploadBytes += dataSize;
	D3D12TraceEvent("index_ring_resident",
		"allocation={} size={} resource={} offset={} fence={} bufferIndex={}",
		(const void*)&allocation,
		dataSize,
		(const void*)upload.resource,
		(unsigned long long)upload.offset,
		(unsigned long long)m_nextFenceValue,
		upload.bufferIndex);
	return true;
}

D3D12Renderer::IndexAllocation D3D12Renderer::indexData_reserveIndexMemory(uint32 size)
{
	++m_uwpBufferProfile.indexReserves;
	m_uwpBufferProfile.indexRequestedBytes += size;
	auto* allocation = new (std::nothrow) D3D12IndexAllocation();
	if (!allocation)
		return {};
	try
	{
		allocation->decodedData.resize(((size_t)size + sizeof(D3D12IndexAllocation::DecodedIndexBlock) - 1) /
			sizeof(D3D12IndexAllocation::DecodedIndexBlock));
	}
	catch (const std::bad_alloc&)
	{
		delete allocation;
		return {};
	}
	allocation->size = size;
	D3D12TraceEvent("index_reserve", "size={} allocation={} cpuPtr={} storage=cpu_staging",
		size,
		(const void*)allocation,
		(const void*)allocation->GetDecodedData());
	return { allocation->GetDecodedData(), allocation };
}

void D3D12Renderer::indexData_releaseIndexMemory(IndexAllocation& allocation)
{
	if (ShouldQueueRecording())
	{
		IndexAllocation queued = allocation;
		allocation = {};
		QueueRecording([this, queued]() mutable { indexData_releaseIndexMemory(queued); });
		return;
	}
	auto* d3dAllocation = static_cast<D3D12IndexAllocation*>(allocation.rendererInternal);
	if (d3dAllocation)
	{
		D3D12TraceEvent("index_release",
			"allocation={} size={} cpuPtr={} residentResource={} residentOffset={} residentFence={} storage=cpu_staging",
			(const void*)d3dAllocation,
			d3dAllocation->size,
			(const void*)d3dAllocation->GetDecodedData(),
			(const void*)d3dAllocation->residentResource,
			(unsigned long long)d3dAllocation->residentOffset,
			(unsigned long long)d3dAllocation->residentFence);
		delete d3dAllocation;
	}
	allocation = {};
}

void D3D12Renderer::indexData_uploadIndexMemory(IndexAllocation& allocation)
{
	if (ShouldQueueRecording())
	{
		IndexAllocation queued = allocation;
		QueueRecording([this, queued]() mutable { indexData_uploadIndexMemory(queued); });
		return;
	}
	auto* d3dAllocation = static_cast<D3D12IndexAllocation*>(allocation.rendererInternal);
	if (!d3dAllocation || d3dAllocation->GetDecodedCapacity() < d3dAllocation->size)
		return;
	// a completed decode changes the CPU source; force the next draw to re-upload
	d3dAllocation->residentResource = nullptr;
	d3dAllocation->residentOffset = 0;
	d3dAllocation->residentFence = 0;
	d3dAllocation->residentSize = 0;
	const uint8* decodedData = d3dAllocation->GetDecodedData();
	ID3D12Resource* traceBuffer = d3dAllocation->residentResource;
	if (D3D12ShouldTrace())
	{
		d3dAllocation->previewDataSize = (uint32)std::min<size_t>(d3dAllocation->previewData.size(), d3dAllocation->size);
		if (d3dAllocation->previewDataSize != 0)
			memcpy(d3dAllocation->previewData.data(), decodedData, d3dAllocation->previewDataSize);
		uint32 firstBytes[4]{};
		if (d3dAllocation->size != 0)
			memcpy(firstBytes, decodedData, std::min<size_t>(d3dAllocation->size, sizeof(firstBytes)));
		D3D12TraceEvent("index_upload", "allocation={} size={} buffer={} firstBytes=0x{:08x},0x{:08x},0x{:08x},0x{:08x}",
						(const void*)d3dAllocation,
						d3dAllocation->size,
						(const void*)traceBuffer,
						firstBytes[0],
						firstBytes[1],
						firstBytes[2],
						firstBytes[3]);
	}
}

LatteQueryObject* D3D12Renderer::occlusionQuery_create()
{
	// the object holds no recording state until begin(), which is queued behind this
	auto* query = new LatteQueryObjectD3D12(this);
	if (ShouldQueueRecording())
	{
		QueueRecording([this, query]() { m_activeOcclusionQueries.emplace_back(query); });
		return query;
	}
	m_activeOcclusionQueries.emplace_back(query);
	return query;
}

void D3D12Renderer::occlusionQuery_destroy(LatteQueryObject* queryObj)
{
	D3D12_QUEUE_RECORDING(occlusionQuery_destroy(queryObj));
	auto* d3dQuery = static_cast<LatteQueryObjectD3D12*>(queryObj);
	m_activeOcclusionQueries.erase(std::remove(m_activeOcclusionQueries.begin(), m_activeOcclusionQueries.end(), d3dQuery), m_activeOcclusionQueries.end());
	delete d3dQuery;
}

void D3D12Renderer::occlusionQuery_flush()
{
	D3D12_EXCLUSIVE_RECORDING();
	if (m_commandListOpen && GetCurrentCommandSlot().queryFragmentCount != 0)
		ExecuteCurrentCommandList(SubmitQueryFlush);
	if (m_lastQueryFenceValue != 0 && !WaitForFenceValue(m_lastQueryFenceValue, "occlusion query flush"))
		throw std::runtime_error("D3D12 device stopped while flushing occlusion queries");
	ProcessFinishedCommandLists();
}

void D3D12Renderer::occlusionQuery_updateState()
{
	D3D12_QUEUE_RECORDING(occlusionQuery_updateState());
	ProcessFinishedCommandLists();
	for (auto* query : m_activeOcclusionQueries)
	{
		if (query)
			query->handleFinishedFragments();
	}
}

D3D12Renderer::SwapchainState& D3D12Renderer::GetSwapchain(bool mainWindow)
{
	return mainWindow ? m_mainSwapchain : m_padSwapchain;
}

const D3D12Renderer::SwapchainState& D3D12Renderer::GetSwapchain(bool mainWindow) const
{
	return mainWindow ? m_mainSwapchain : m_padSwapchain;
}
