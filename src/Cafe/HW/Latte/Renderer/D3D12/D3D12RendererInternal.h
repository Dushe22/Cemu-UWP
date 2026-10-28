#pragma once

// Shared by the D3D12Renderer source files: includes, recording macros and the capped runtime log.
// D3D12Renderer.cpp includes most of the class as .inl parts (same translation unit, shared file-local helpers);
// D3D12RendererImgui.cpp and D3D12RendererRecorder.cpp compile on their own and only need what is here.

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

// Queue the call with value-captured arguments, then return from the caller when queued.
// Keep AND_RETURN in these names: code below the macro only runs on the inline path.
#define D3D12_QUEUE_RECORDING_AND_RETURN(...) \
	do { if (ShouldQueueRecording()) { QueueRecording([=, this, opName_ = __func__]() { m_gpuWatchdog.NoteCall(opName_); __VA_ARGS__; }); return; } } while (false)
#define D3D12_QUEUE_RECORDING_KIND_AND_RETURN(kind, ...) do { if (ShouldQueueRecording()) { QueueRecording([=, this, opName_ = __func__]() { m_gpuWatchdog.NoteCall(opName_); __VA_ARGS__; }, RecordOpKind::kind); return; } } while (false)
// the rest of the function uses recording state directly: wait for queued work and keep the recorder out
#define D3D12_EXCLUSIVE_RECORDING() RecordingExclusiveScope recordingExclusiveScope(this)


// trace events are compiled only into D3D12Renderer.cpp (diagnostic builds); separate files drop them
#if defined(D3D12_RENDERER_SEPARATE_PART)
// D3D12RuntimeLogCapped lives in an anonymous namespace of D3D12Renderer.cpp; other files reach it through this wrapper
bool D3D12RuntimeLogCappedShared(uint32& counter, const char* eventName, uint32 limit = 64);
#define D3D12RuntimeLogCapped D3D12RuntimeLogCappedShared
#define D3D12TraceEvent(...) ((void)0)
#define D3D12TraceEventFlush(...) ((void)0)
#endif
