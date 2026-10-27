#include "Cafe/HW/Latte/Renderer/D3D12/D3D12Renderer.h"
#include "Common/UwpProfiling.h"
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
#include <intrin.h>
#include "Cafe/HW/Espresso/PPCState.h"
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
	do { if (ShouldQueueRecording()) { QueueRecording([=, this, opName_ = __func__]() { m_gpuWatchdog.NoteCall(opName_); __VA_ARGS__; }); return; } } while (false)
#define D3D12_QUEUE_RECORDING_KIND(kind, ...) do { if (ShouldQueueRecording()) { QueueRecording([=, this, opName_ = __func__]() { m_gpuWatchdog.NoteCall(opName_); __VA_ARGS__; }, RecordOpKind::kind); return; } } while (false)
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
constexpr uint32 kRecordChunkBytes = LATTE_REGISTER_CHUNK_WORDS * sizeof(uint32);
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
				memcpy(dst + chunk * LATTE_REGISTER_CHUNK_WORDS, src + chunk * LATTE_REGISTER_CHUNK_WORDS, kRecordChunkBytes);
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
// travels as data of the draw's ring entry: this header, then chunkCount chunk indices, then their words
// (LATTE_REGISTER_CHUNK_WORDS each), or the whole register file when full
struct RecordStateDelta
{
	bool full;
	uint32 chunkCount;
	std::array<uint32, kRecordSpecialWords> special;
	LatteFetchShader* fetchShader;
	LatteShaderPSInputTable psInputTable;
};
static_assert(std::is_trivially_copyable_v<RecordStateDelta>);
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
RecordVectorPool<uint8> s_recordBytePool;

// Phase 5 S0: split of the draw tail into what a "prepare" stage (CPU only: state, PSO lookup, descriptor tables)
// and an "emit" stage (command list: feedback copies, barriers, uploads, draw) would take. Measurement only
struct UwpRecorderStageSplit
{
	std::atomic<uint64> draws{}, total{}, apply{}, pso{}, feedback{}, tablePrep{}, tables{}, tableTransitions{}, tableUniformCommit{}, emit{};
	void Add(std::atomic<uint64>& counter, uint64 ticks) { if (ticks) counter.fetch_add(ticks, std::memory_order_relaxed); }
};
UwpRecorderStageSplit s_uwpRecorderSplit;
// Latte side of draw_execute after the buffer sync (Kakariko: ~1.8-2 us per draw). Latte thread only
enum UwpHeadPart : uint32 { HeadMisc, HeadIdentity, HeadState, HeadUniformPrepare, HeadCollect, HeadClosure, HeadDeltaWrite, HeadQueue, HeadPost, HeadPartCount };
size_t s_uwpDrawClosureBytes = 0;
uint64 s_uwpHeadTicks[HeadPartCount]{};
uint64 s_uwpHeadDraws = 0;
thread_local bool t_uwpInDrawTables = false;

// which 4096-register pages the per-draw register delta carries (register write bases: config 0x2000, context 0xA000,
// ALU constants 0xC000, resources 0xE000, samplers/control/loop 0xF000). Latte thread only, logged every 5 s
uint64 s_uwpDeltaDraws = 0;
uint64 s_uwpDeltaEmpty = 0;
uint64 s_uwpDeltaFull = 0;
uint64 s_uwpDeltaChunksByPage[16]{};

uint32 s_latteDeltaChunks = 0; // dirty chunks counted by RecordDeltaBytes for WriteRecordDelta

// bytes of the next delta; WriteRecordDelta must follow before any register write
size_t RecordDeltaBytes()
{
	if (!s_latteSentFullRegisters)
		return sizeof(RecordStateDelta) + LATTE_MAX_REGISTER * sizeof(uint32);
	uint32 dirtyChunks = 0;
	for (uint64 bits : LatteGPUState.registerChunkDirty)
		dirtyChunks += (uint32)std::popcount(bits);
	s_latteDeltaChunks = dirtyChunks;
	return sizeof(RecordStateDelta) + (size_t)dirtyChunks * (sizeof(uint32) + kRecordChunkBytes);
}

void WriteRecordDelta(uint8* data, LatteFetchShader* fetchShader)
{
	++s_uwpDeltaDraws;
	const uint32* src = reinterpret_cast<const uint32*>(&LatteGPUState.contextNew);
	RecordStateDelta header{};
	header.fetchShader = fetchShader;
	std::copy_n(src + LATTE_MAX_REGISTER, kRecordSpecialWords, header.special.begin());
	if (const LatteShaderPSInputTable* psInputTable = LatteSHRC_GetPSInputTable())
		header.psInputTable = *psInputTable;
	uint8* body = data + sizeof(RecordStateDelta);
	if (!s_latteSentFullRegisters)
	{
		header.full = true;
		memcpy(body, src, LATTE_MAX_REGISTER * sizeof(uint32));
		++s_uwpDeltaFull;
		std::fill(std::begin(LatteGPUState.registerChunkDirty), std::end(LatteGPUState.registerChunkDirty), 0);
		s_latteSentFullRegisters = true;
	}
	else
	{
		header.chunkCount = s_latteDeltaChunks;
		if (s_latteDeltaChunks == 0)
			++s_uwpDeltaEmpty;
		uint32* chunks = reinterpret_cast<uint32*>(body);
		uint8* words = body + (size_t)s_latteDeltaChunks * sizeof(uint32);
		uint32 written = 0;
		for (uint32 word = 0; word < std::size(LatteGPUState.registerChunkDirty); ++word)
		{
			uint64 bits = std::exchange(LatteGPUState.registerChunkDirty[word], 0);
			while (bits)
			{
				const uint32 chunk = word * 64 + (uint32)std::countr_zero(bits);
				bits &= bits - 1;
				cemu_assert(written < s_latteDeltaChunks);
				chunks[written] = chunk;
				memcpy(words + (size_t)written * kRecordChunkBytes, src + chunk * LATTE_REGISTER_CHUNK_WORDS, kRecordChunkBytes);
				++written;
				++s_uwpDeltaChunksByPage[(chunk * LATTE_REGISTER_CHUNK_WORDS) >> 12];
			}
		}
		cemu_assert(written == s_latteDeltaChunks);
	}
	memcpy(data, &header, sizeof(header));
}

void ApplyRecordState(const uint8* data)
{
	if (!s_recordRegs)
		s_recordRegs = std::make_unique<LatteContextRegister>();
	RecordStateDelta delta;
	memcpy(&delta, data, sizeof(delta));
	const uint8* body = data + sizeof(RecordStateDelta);
	uint32* dst = RecordRegWords();
	if (delta.full)
	{
		memcpy(dst, body, LATTE_MAX_REGISTER * sizeof(uint32));
		s_recordRegsComplete = true;
		BumpAllRecordSamplingGenerations();
	}
	else
	{
		const uint32* chunks = reinterpret_cast<const uint32*>(body);
		const uint8* words = body + (size_t)delta.chunkCount * sizeof(uint32);
		for (uint32 i = 0; i < delta.chunkCount; ++i)
		{
			memcpy(dst + chunks[i] * LATTE_REGISTER_CHUNK_WORDS, words + (size_t)i * kRecordChunkBytes, kRecordChunkBytes);
			NoteRecordChunkChanged(chunks[i]);
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

#if defined(CEMU_UWP_D3D12_WORKLOAD_PROFILE)
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
	{
		// the shader model above is capped at 6.4 by its query; this asks for the real maximum, plus features that
		// decide whether bindless descriptors, enhanced barriers or work graphs are options on this device
		D3D_SHADER_MODEL maxShaderModel = D3D_SHADER_MODEL_6_4;
		for (D3D_SHADER_MODEL candidate : { D3D_SHADER_MODEL_6_9, D3D_SHADER_MODEL_6_8, D3D_SHADER_MODEL_6_7, D3D_SHADER_MODEL_6_6, D3D_SHADER_MODEL_6_5 })
		{
			D3D12_FEATURE_DATA_SHADER_MODEL query{ candidate };
			if (SUCCEEDED(m_device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &query, sizeof(query))))
			{
				maxShaderModel = query.HighestShaderModel;
				break;
			}
		}
		D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
		D3D12_FEATURE_DATA_D3D12_OPTIONS7 options7{};
		D3D12_FEATURE_DATA_D3D12_OPTIONS12 options12{};
		D3D12_FEATURE_DATA_D3D12_OPTIONS21 options21{};
		const bool hasOptions = SUCCEEDED(m_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options)));
		const bool hasOptions7 = SUCCEEDED(m_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &options7, sizeof(options7)));
		const bool hasOptions12 = SUCCEEDED(m_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS12, &options12, sizeof(options12)));
		const bool hasOptions21 = SUCCEEDED(m_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS21, &options21, sizeof(options21)));
		cemuLog_log(LogType::Force, "D3D12 device capabilities: max shader model 0x{:x}, resource heap tier {}, mesh shader tier {}, enhanced barriers {}, work graphs tier {}",
			(uint32)maxShaderModel, hasOptions ? std::to_string((uint32)options.ResourceHeapTier) : "?",
			hasOptions7 ? std::to_string((uint32)options7.MeshShaderTier) : "?",
			hasOptions12 ? (options12.EnhancedBarriersSupported ? "yes" : "no") : "?",
			hasOptions21 ? std::to_string((uint32)options21.WorkGraphsTier) : "?");
	}
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
	m_twoStageWanted = m_threadedRecordingWanted && GetConfig().d3d12_two_stage_recording;
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

#include "D3D12RendererDescriptors.inl"
#include "D3D12RendererPipelines.inl"
#include "D3D12RendererCommands.inl"
#include "D3D12RendererMemory.inl"
#include "D3D12RendererFeedback.inl"
#include "D3D12RendererDiagnostics.inl"
#include "D3D12RendererQueriesAndProfiling.inl"
#include "D3D12RendererOutput.inl"
#include "D3D12RendererImgui.inl"
#include "D3D12RendererRecorder.inl"
#include "D3D12RendererTextures.inl"
#include "D3D12RendererDraw.inl"
