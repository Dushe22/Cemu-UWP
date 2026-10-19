#include "Cafe/HW/Latte/Renderer/D3D12/RendererShaderD3D12.h"

#include "Cemu/FileCache/FileCache.h"
#include "Cafe/HW/Latte/LegacyShaderDecompiler/LatteDecompilerEmitHLSL.hpp"
#include "config/ActiveSettings.h"
#include "util/helpers/helpers.h"
#include "Cafe/HW/Latte/Core/LattePerformanceMonitor.h"
#if defined(CEMU_UWP)
#include "uwp/UwpMemoryBudgetService.h"
#endif

#include <Windows.h>
#include <Objbase.h>
#include <OleAuto.h>
#include <dxcapi.h>
#include <fmt/format.h>
#include <wrl/client.h>

#include <array>
#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <semaphore>
#include <thread>

namespace
{
std::atomic_bool s_isLoadingShadersD3D12{ false };
FileCache* s_dxilCache{ nullptr };
std::atomic_uint64_t s_dxilCacheHits{ 0 };
std::atomic_uint64_t s_dxilCacheMisses{ 0 };
std::atomic_uint64_t s_dxilCompiled{ 0 };
std::atomic_uint64_t s_dxilFailed{ 0 };
std::atomic_uint64_t s_dxcUtilsCreated{ 0 };
std::atomic_uint64_t s_dxcCompilersCreated{ 0 };
std::atomic_uint64_t s_cacheLoadShadersDiscovered{ 0 };
std::atomic_uint64_t s_cacheLoadPrewarmWaitMilliseconds{ 0 };
std::atomic_uint64_t s_renderThreadCompilationWaits{ 0 };

std::counting_semaphore<8>& NativeCompilationSlots()
{
	static std::counting_semaphore<8> slots(RendererShaderD3D12::GetNativeCompilationConcurrency());
	return slots;
}
}

extern std::atomic_int g_compiled_shaders_async;

class _ShaderD3D12ThreadPool
{
public:
	void StartThreads()
	{
		if (m_threadsActive.exchange(true))
			return;

		// more threads than compilation slots would only wait
		const uint32 threadCount = RendererShaderD3D12::GetNativeCompilationConcurrency();
		for (uint32 i = 0; i < threadCount; ++i)
			m_threads.emplace_back(&_ShaderD3D12ThreadPool::CompilerThreadFunc, this);
		cemuLog_log(LogType::Force, "D3D12: {} shader compiler threads ({} hardware threads).",
			threadCount, std::thread::hardware_concurrency());
	}

	void StopThreads()
	{
		if (m_threadsActive.load(std::memory_order_acquire))
			WaitUntilIdle();
		if (!m_threadsActive.exchange(false))
			return;

		for (uint32 i = 0; i < m_threads.size(); ++i)
			m_compilationQueueCount.increment();
		for (auto& thread : m_threads)
			thread.join();
		m_threads.clear();
	}

	~_ShaderD3D12ThreadPool()
	{
		StopThreads();
	}

	void Queue(RendererShaderD3D12* shader)
	{
		std::scoped_lock lock(m_compilationQueueMutex);
		shader->m_compilationState.setValue(RendererShaderD3D12::COMPILATION_STATE::QUEUED);
		m_compilationQueue.push_back(shader);
		m_pendingJobs.fetch_add(1, std::memory_order_release);
		m_compilationQueueCount.increment();
	}

	void PrioritizeQueuedJob(RendererShaderD3D12* shader)
	{
		std::scoped_lock lock(m_compilationQueueMutex);
		if (!shader->m_compilationState.hasState(RendererShaderD3D12::COMPILATION_STATE::QUEUED))
			return;

		auto it = std::find(m_compilationQueue.begin(), m_compilationQueue.end(), shader);
		if (it == m_compilationQueue.end() || it == m_compilationQueue.begin())
			return;
		m_compilationQueue.erase(it);
		m_compilationQueue.push_front(shader);
	}

	bool StealQueuedJob(RendererShaderD3D12* shader)
	{
		std::scoped_lock lock(m_compilationQueueMutex);
		if (!shader->m_compilationState.hasState(RendererShaderD3D12::COMPILATION_STATE::QUEUED))
			return false;

		m_compilationQueue.erase(std::remove(m_compilationQueue.begin(), m_compilationQueue.end(), shader), m_compilationQueue.end());
		shader->m_compilationState.setValue(RendererShaderD3D12::COMPILATION_STATE::COMPILING);
		return true;
	}

	bool HasThreadsRunning() const
	{
		return m_threadsActive.load(std::memory_order::relaxed);
	}

	uint32 GetQueuedCount()
	{
		std::scoped_lock lock(m_compilationQueueMutex);
		return (uint32)m_compilationQueue.size();
	}

	uint32 GetThreadCount() const
	{
		return (uint32)m_threads.size();
	}

	uint32 GetPendingCount() const
	{
		return m_pendingJobs.load(std::memory_order_acquire);
	}

	void WaitUntilIdle()
	{
		std::unique_lock lock(m_idleMutex);
		m_idleCondition.wait(lock, [this]() { return m_pendingJobs.load(std::memory_order_acquire) == 0; });
	}

	void CompleteJob()
	{
		const uint32 previous = m_pendingJobs.fetch_sub(1, std::memory_order_acq_rel);
		cemu_assert_debug(previous != 0);
		if (previous == 1)
		{
			// the notify must not slip between WaitUntilIdle's predicate check and wait
			std::scoped_lock lock(m_idleMutex);
			m_idleCondition.notify_all();
		}
	}

private:
	void CompilerThreadFunc()
	{
		SetThreadName("d3d12ShaderComp");
		SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
		while (true)
		{
			m_compilationQueueCount.decrementWithWait();

			RendererShaderD3D12* job = nullptr;
			{
				std::scoped_lock lock(m_compilationQueueMutex);
				if (m_compilationQueue.empty())
				{
					if (!m_threadsActive.load(std::memory_order::relaxed))
						break;
					continue;
				}

				job = m_compilationQueue.front();
				m_compilationQueue.pop_front();
				cemu_assert_debug(job->m_compilationState.getValue() == RendererShaderD3D12::COMPILATION_STATE::QUEUED);
				job->m_compilationState.setValue(RendererShaderD3D12::COMPILATION_STATE::COMPILING);
			}

			job->CompileInternal();
			++g_compiled_shaders_async;
			cemu_assert_debug(job->m_compilationState.getValue() == RendererShaderD3D12::COMPILATION_STATE::COMPILING);
			job->m_compilationState.setValue(RendererShaderD3D12::COMPILATION_STATE::DONE);
			CompleteJob();
		}
	}

	std::atomic<bool> m_threadsActive{ false };
	std::vector<std::thread> m_threads;
	std::deque<RendererShaderD3D12*> m_compilationQueue;
	CounterSemaphore m_compilationQueueCount;
	std::mutex m_compilationQueueMutex;
	std::atomic_uint32_t m_pendingJobs{ 0 };
	std::mutex m_idleMutex;
	std::condition_variable m_idleCondition;
};

_ShaderD3D12ThreadPool ShaderD3D12ThreadPool;

namespace
{
uint32 GenerateDXILCacheMagic()
{
	uint32 v = 0;
	const char* s = EMULATOR_VERSION_SUFFIX;
	while (*s)
	{
		v = std::rotl<uint32>(v, 7);
		v += (uint32)(*s);
		s++;
	}
	v += (EMULATOR_VERSION_MAJOR * 1000000u);
	v += (EMULATOR_VERSION_MINOR * 10000u);
	v += (EMULATOR_VERSION_PATCH * 100u);
	v += LATTE_DECOMPILER_HLSL_EMITTER_VERSION * 17u;
	v ^= 0xD3126404u;
	v ^= 0xC3120001u;
	return v;
}

uint64 HashHLSLSource(const std::string& source)
{
	uint64 hash = 0xcbf29ce484222325ull;
	for (const char c : source)
	{
		hash ^= (uint8)c;
		hash *= 0x100000001b3ull;
	}
	return hash;
}

// the generated HLSL can depend on state the shader hashes do not cover, so cached DXIL is keyed by the source too
void GenerateDXILCacheKey(RendererShader::ShaderType type, uint64 baseHash, uint64 auxHash, uint64 sourceHash, uint64& h1, uint64& h2)
{
	h1 = baseHash ^ std::rotl(sourceHash, 29);
	h2 = auxHash ^ (sourceHash * 0x9E3779B97F4A7C15ull);
	if (type == RendererShader::ShaderType::kVertex)
		h2 += 0xA16374cull;
	else if (type == RendererShader::ShaderType::kFragment)
		h2 += 0x8752deull;
	else if (type == RendererShader::ShaderType::kGeometry)
		h2 += 0x65a035ull;
	h2 += 0xD3126404ull;
	h2 ^= ((uint64)LATTE_DECOMPILER_HLSL_EMITTER_VERSION << 48);
}

bool IsValidDxilContainer(const std::vector<uint8>& data)
{
	constexpr size_t headerSize = 32;
	if (data.size() < headerSize || std::memcmp(data.data(), "DXBC", 4) != 0)
		return false;

	const auto readUint32 = [&data](size_t offset)
	{
		uint32 value = 0;
		std::memcpy(&value, data.data() + offset, sizeof(value));
		return value;
	};
	const uint32 containerSize = readUint32(24);
	const uint32 partCount = readUint32(28);
	if (containerSize != data.size() || partCount == 0 || partCount > 64 ||
		headerSize + static_cast<size_t>(partCount) * sizeof(uint32) > data.size())
	{
		return false;
	}

	bool hasDxilPart = false;
	for (uint32 partIndex = 0; partIndex < partCount; ++partIndex)
	{
		const size_t partOffset = readUint32(headerSize + static_cast<size_t>(partIndex) * sizeof(uint32));
		if (partOffset > data.size() - 8)
			return false;
		const uint32 partSize = readUint32(partOffset + 4);
		if (partSize > data.size() - partOffset - 8)
			return false;
		if (std::memcmp(data.data() + partOffset, "DXIL", 4) == 0)
			hasDxilPart = true;
	}
	return hasDxilPart;
}

std::string BlobToString(IDxcBlobUtf8* blob)
{
	if (!blob || blob->GetStringLength() == 0)
		return {};
	return std::string(blob->GetStringPointer(), blob->GetStringLength());
}

const char* StageName(RendererShader::ShaderType type)
{
	switch (type)
	{
	case RendererShader::ShaderType::kVertex:
		return "vertex";
	case RendererShader::ShaderType::kFragment:
		return "pixel";
	case RendererShader::ShaderType::kGeometry:
		return "geometry";
	default:
		return "unknown";
	}
}

struct DxcThreadCompiler
{
	Microsoft::WRL::ComPtr<IDxcUtils> utils;
	Microsoft::WRL::ComPtr<IDxcCompiler3> compiler;

	bool Ensure(uint64 baseHash, uint64 auxHash)
	{
		if (!utils)
		{
			const HRESULT hr = DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&utils));
			if (FAILED(hr))
			{
				cemuLog_log(LogType::Force, "D3D12: Failed to create DXC utils for {:016x}_{:016x} (HRESULT 0x{:08x})", baseHash, auxHash, (uint32)hr);
				return false;
			}
			s_dxcUtilsCreated++;
		}

		if (!compiler)
		{
			const HRESULT hr = DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler));
			if (FAILED(hr))
			{
				cemuLog_log(LogType::Force, "D3D12: Failed to create DXC compiler for {:016x}_{:016x} (HRESULT 0x{:08x})", baseHash, auxHash, (uint32)hr);
				return false;
			}
			s_dxcCompilersCreated++;
		}

		return true;
	}
};

thread_local DxcThreadCompiler t_dxcCompiler;
}

uint32 RendererShaderD3D12::GetNativeCompilationConcurrency()
{
	// one hardware thread stays with the emulated CPU and the render thread; beyond four the memory cost outweighs the gain
	static const uint32 concurrency = std::clamp<uint32>(std::thread::hardware_concurrency(), 2u, 5u) - 1u;
	return concurrency;
}

RendererShaderD3D12::NativeCompilationSlot::NativeCompilationSlot()
{
	NativeCompilationSlots().acquire();
}

RendererShaderD3D12::NativeCompilationSlot::~NativeCompilationSlot()
{
	NativeCompilationSlots().release();
}

RendererShaderD3D12::RendererShaderD3D12(ShaderType type, uint64 baseHash, uint64 auxHash, bool isGameShader, bool isGfxPackShader, const std::string& hlslCode)
	: RendererShader(type, baseHash, auxHash, isGameShader, isGfxPackShader), m_hlslCode(hlslCode), m_hlslSourceHash(HashHLSLSource(hlslCode))
{
	if (isGameShader && s_isLoadingShadersD3D12.load(std::memory_order_acquire))
		s_cacheLoadShadersDiscovered.fetch_add(1, std::memory_order_relaxed);

	if (ShaderD3D12ThreadPool.HasThreadsRunning())
		ShaderD3D12ThreadPool.Queue(this);
	else
	{
		m_compilationState.setValue(COMPILATION_STATE::COMPILING);
		CompileInternal();
		m_compilationState.setValue(COMPILATION_STATE::DONE);
	}
}

const wchar_t* RendererShaderD3D12::GetTargetProfile() const
{
	switch (GetType())
	{
	case ShaderType::kVertex:
		return L"vs_6_4";
	case ShaderType::kFragment:
		return L"ps_6_4";
	case ShaderType::kGeometry:
		return L"gs_6_4";
	default:
		cemu_assert_debug(false);
		return L"ps_6_4";
	}
}

bool RendererShaderD3D12::TryLoadFromCache()
{
	if (!s_dxilCache || m_isGfxPackShader || m_baseHash == 0)
		return false;
	// after cache loading game shaders are new; synthetic ones may load anytime
	if (m_isGameShader && !s_isLoadingShadersD3D12.load(std::memory_order_acquire))
		return false;

	uint64 h1, h2;
	GenerateDXILCacheKey(m_type, m_baseHash, m_auxHash, m_hlslSourceHash, h1, h2);
	std::vector<uint8> cacheFileData;
	if (!s_dxilCache->GetFile({ h1, h2 }, cacheFileData) || !IsValidDxilContainer(cacheFileData))
	{
		s_dxilCacheMisses++;
		return false;
	}

	m_dxil = std::move(cacheFileData);
	m_compileSucceeded = true;
	s_dxilCacheHits++;
	return true;
}

void RendererShaderD3D12::StoreToCache() const
{
	if (!s_dxilCache || m_isGfxPackShader || m_baseHash == 0 || m_dxil.empty())
		return;

	uint64 h1, h2;
	GenerateDXILCacheKey(m_type, m_baseHash, m_auxHash, m_hlslSourceHash, h1, h2);
	s_dxilCache->AddFile({ h1, h2 }, m_dxil.data(), (sint32)m_dxil.size());
}

void RendererShaderD3D12::CompileInternal()
{
	if (m_compilationFinished)
		return;

	m_dxil.clear();
	m_compileSucceeded = false;

	if (TryLoadFromCache())
	{
		m_hlslCode.clear();
		m_hlslCode.shrink_to_fit();
		m_compilationFinished = true;
		return;
	}

	if (m_hlslCode.empty())
	{
		s_dxilFailed++;
		m_compilationFinished = true;
		return;
	}

	if (!t_dxcCompiler.Ensure(m_baseHash, m_auxHash))
	{
		s_dxilFailed++;
		m_compilationFinished = true;
		return;
	}

	DxcBuffer source{};
	source.Ptr = m_hlslCode.data();
	source.Size = m_hlslCode.size();
	source.Encoding = DXC_CP_UTF8;

	std::array<LPCWSTR, 9> args =
	{
		L"-E", L"main",
		L"-T", GetTargetProfile(),
		L"-HV", L"2021",
		L"-O3",
		L"-Qstrip_reflect",
		L"-Qstrip_debug",
	};

	Microsoft::WRL::ComPtr<IDxcResult> result;
	HRESULT hr = E_FAIL;
	{
#if defined(CEMU_UWP)
		NativeCompilationSlot nativeCompilationSlot;
		// never block on the memory guard, the render thread may wait for this shader
		if (CemuUwp::MemoryBudgetService::IsHeavyWorkDeferred())
		{
			static std::atomic_uint32_t s_guardedCompiles{ 0 };
			if ((s_guardedCompiles.fetch_add(1, std::memory_order_relaxed) & 15u) == 0)
				CemuUwp::MemoryBudgetService::CompactProcessHeap();
		}
#endif
		hr = t_dxcCompiler.compiler->Compile(&source, args.data(), (uint32)args.size(), nullptr, IID_PPV_ARGS(&result));
	}
	if (FAILED(hr) || !result)
	{
		cemuLog_log(LogType::Force, "D3D12: DXC compile call failed for {} shader {:016x}_{:016x} (HRESULT 0x{:08x})",
					StageName(m_type), m_baseHash, m_auxHash, (uint32)hr);
		s_dxilFailed++;
		m_compilationFinished = true;
		return;
	}

	Microsoft::WRL::ComPtr<IDxcBlobUtf8> errors;
	result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr);
	const std::string diagnostics = BlobToString(errors.Get());

	HRESULT status = S_OK;
	result->GetStatus(&status);
	if (FAILED(status))
	{
		cemuLog_log(LogType::Force, "D3D12: DXC failed for {} shader {:016x}_{:016x} (game={}, gfxPack={}):\n{}",
					StageName(m_type),
					m_baseHash,
					m_auxHash,
					m_isGameShader ? "true" : "false",
					m_isGfxPackShader ? "true" : "false",
					diagnostics.empty() ? "<no diagnostics>" : diagnostics);
		s_dxilFailed++;
		m_compilationFinished = true;
		return;
	}

	Microsoft::WRL::ComPtr<IDxcBlob> object;
	result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&object), nullptr);
	if (!object || object->GetBufferSize() == 0)
	{
		cemuLog_log(LogType::Force, "D3D12: DXC produced no bytecode for {} shader {:016x}_{:016x}", StageName(m_type), m_baseHash, m_auxHash);
		s_dxilFailed++;
		m_compilationFinished = true;
		return;
	}

	const auto bytecodeSize = object->GetBufferSize();
	m_dxil.resize(bytecodeSize);
	std::memcpy(m_dxil.data(), object->GetBufferPointer(), bytecodeSize);
	if (!IsValidDxilContainer(m_dxil))
	{
		cemuLog_log(LogType::Force, "D3D12: DXC produced an invalid container for {} shader {:016x}_{:016x}",
			StageName(m_type), m_baseHash, m_auxHash);
		m_dxil.clear();
		s_dxilFailed++;
		m_compilationFinished = true;
		return;
	}
	m_compileSucceeded = true;
	s_dxilCompiled++;
	StoreToCache();

	m_hlslCode.clear();
	m_hlslCode.shrink_to_fit();
	m_compilationFinished = true;
}

void RendererShaderD3D12::PreponeCompilation(bool isRenderThread)
{
	if (m_compilationState.hasState(COMPILATION_STATE::DONE))
		return;

	LatteUwpHitchScope shaderCompileScope(LatteUwpHitchOp::ShaderCompile);
	if (isRenderThread)
		s_renderThreadCompilationWaits.fetch_add(1, std::memory_order_relaxed);

	if (ShaderD3D12ThreadPool.StealQueuedJob(this))
	{
		CompileInternal();
		m_compilationState.setValue(COMPILATION_STATE::DONE);
		ShaderD3D12ThreadPool.CompleteJob();
		return;
	}

	m_compilationState.waitUntilValue(COMPILATION_STATE::DONE);
	--g_compiled_shaders_async;
}

void RendererShaderD3D12::RequestCompilationPriority()
{
	ShaderD3D12ThreadPool.PrioritizeQueuedJob(this);
}

bool RendererShaderD3D12::IsCompiled()
{
	return m_compilationState.hasState(COMPILATION_STATE::DONE);
}

bool RendererShaderD3D12::WaitForCompiled()
{
	PreponeCompilation(true);
	return m_compileSucceeded;
}

void RendererShaderD3D12::Init()
{
	ShaderD3D12ThreadPool.StartThreads();
}

void RendererShaderD3D12::Shutdown()
{
	ShaderD3D12ThreadPool.StopThreads();
}

void RendererShaderD3D12::ShaderCacheLoading_begin(uint64 cacheTitleId)
{
	delete s_dxilCache;
	s_dxilCache = nullptr;

	const std::string cacheFilename = fmt::format("{:016x}_dxil.bin", cacheTitleId);
	const fs::path cachePath = ActiveSettings::GetCachePath("shaderCache/precompiled/{}", cacheFilename);
	s_dxilCache = FileCache::Open(cachePath, true, GenerateDXILCacheMagic());
	if (s_dxilCache == nullptr)
		cemuLog_log(LogType::Force, "Unable to open DXIL cache {}", cacheFilename);
	s_cacheLoadShadersDiscovered.store(0, std::memory_order_relaxed);
	s_cacheLoadPrewarmWaitMilliseconds.store(0, std::memory_order_relaxed);
	s_isLoadingShadersD3D12.store(true, std::memory_order_release);
}

void RendererShaderD3D12::ShaderCacheLoading_end()
{
	// game shaders only consult the DXIL cache in this phase; drain the queue first
	const uint32 pendingAtEnd = ShaderD3D12ThreadPool.GetPendingCount();
	const auto waitStart = std::chrono::steady_clock::now();
	ShaderD3D12ThreadPool.WaitUntilIdle();
	const uint64 waitMilliseconds = (uint64)std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now() - waitStart).count();
	s_cacheLoadPrewarmWaitMilliseconds.store(waitMilliseconds, std::memory_order_relaxed);
	if (pendingAtEnd != 0 || s_cacheLoadShadersDiscovered.load(std::memory_order_relaxed) != 0)
	{
		cemuLog_log(LogType::Force,
			"D3D12: prewarmed {} cached game shaders before launch ({} pending at cache end, waited {} ms).",
			s_cacheLoadShadersDiscovered.load(std::memory_order_relaxed),
			pendingAtEnd,
			waitMilliseconds);
	}
	s_isLoadingShadersD3D12.store(false, std::memory_order_release);
}

void RendererShaderD3D12::ShaderCacheLoading_Close()
{
	delete s_dxilCache;
	s_dxilCache = nullptr;
}

RendererShaderD3D12::Stats RendererShaderD3D12::GetStats()
{
	return {
		s_dxilCacheHits.load(std::memory_order_relaxed),
		s_dxilCacheMisses.load(std::memory_order_relaxed),
		s_dxilCompiled.load(std::memory_order_relaxed),
		s_dxilFailed.load(std::memory_order_relaxed),
		s_dxcUtilsCreated.load(std::memory_order_relaxed),
		s_dxcCompilersCreated.load(std::memory_order_relaxed),
		s_cacheLoadShadersDiscovered.load(std::memory_order_relaxed),
		s_cacheLoadPrewarmWaitMilliseconds.load(std::memory_order_relaxed),
		s_renderThreadCompilationWaits.load(std::memory_order_relaxed),
		ShaderD3D12ThreadPool.GetQueuedCount(),
		ShaderD3D12ThreadPool.GetThreadCount(),
	};
}
