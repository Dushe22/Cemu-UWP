#pragma once

#include "Cafe/HW/Latte/Renderer/RendererShader.h"
#include "util/helpers/Semaphore.h"

#include <mutex>

class RendererShaderD3D12 : public RendererShader
{
	friend class _ShaderD3D12ThreadPool;

	enum class COMPILATION_STATE : uint32
	{
		NONE,
		QUEUED,
		COMPILING,
		DONE
	};

public:
	struct Stats
	{
		uint64 dxilCacheHits{};
		uint64 dxilCacheMisses{};
		uint64 dxilCompiled{};
		uint64 dxilFailed{};
		uint64 dxcUtilsCreated{};
		uint64 dxcCompilersCreated{};
		uint64 cacheLoadShadersDiscovered{};
		uint64 cacheLoadPrewarmWaitMilliseconds{};
		uint64 renderThreadCompilationWaits{};
		uint32 queuedShaders{};
		uint32 compilerThreads{};
		uint64 dxilEvictedBytes{};
		uint64 dxilReloads{};
		uint64 hlslPrunedBytes{};
		uint64 hlslPruneFallbacks{};
	};

	static void Init();
	static void Shutdown();
	static void ShaderCacheLoading_begin(uint64 cacheTitleId);
	static void ShaderCacheLoading_end();
	static void ShaderCacheLoading_Close();
	static Stats GetStats();
	// empty when DXC works; otherwise a user-facing reason (probed once)
	static const std::string& GetCompilerError();

	// DXC compiles and driver pipeline compiles each hold a lot of transient memory, so only a few run at once
	class NativeCompilationSlot
	{
	public:
		NativeCompilationSlot();
		~NativeCompilationSlot();
		NativeCompilationSlot(const NativeCompilationSlot&) = delete;
		NativeCompilationSlot& operator=(const NativeCompilationSlot&) = delete;
	};
	static uint32 GetNativeCompilationConcurrency();
	// higher limit used from shader cache loading until the blocking pipeline prewarm ends
	static uint32 GetLoadingCompilationConcurrency();
	static void SetTitleLoading(bool loading);

	RendererShaderD3D12(ShaderType type, uint64 baseHash, uint64 auxHash, bool isGameShader, bool isGfxPackShader, const std::string& hlslCode);
	~RendererShaderD3D12() override;

	void PreponeCompilation(bool isRenderThread) override;
	void RequestCompilationPriority();
	bool IsCompiled() override;
	bool WaitForCompiled() override;

	// render thread. DXIL is only needed to create PSOs, so under memory pressure idle game shaders
	// drop it (EvictIdleDXIL) and these reload it from the DXIL cache on demand
	const std::vector<uint8>& GetDXIL() { EnsureDXILResident(); return m_dxil; }
	bool HasDXIL() { return EnsureDXILResident(); }
	// render thread; returns the bytes released
	static uint64 EvictIdleDXIL(uint32 minIdleFrames, uint64 targetBytes);

private:
	void CompileInternal();
	const wchar_t* GetTargetProfile() const;
	bool TryLoadFromCache();
	void StoreToCache() const;
	bool EnsureDXILResident();

	std::string m_hlslCode;
	uint64 m_hlslSourceHash{};
	std::vector<uint8> m_dxil;
	bool m_dxilEvicted{};
	uint32 m_dxilLastUseFrame{};
	StateSemaphore<COMPILATION_STATE> m_compilationState{ COMPILATION_STATE::NONE };
	bool m_compilationFinished{false};
	bool m_compileSucceeded{false};
};
