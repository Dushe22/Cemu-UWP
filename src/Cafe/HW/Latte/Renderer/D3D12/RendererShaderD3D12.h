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
	};

	static void Init();
	static void Shutdown();
	static void ShaderCacheLoading_begin(uint64 cacheTitleId);
	static void ShaderCacheLoading_end();
	static void ShaderCacheLoading_Close();
	static Stats GetStats();

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

	RendererShaderD3D12(ShaderType type, uint64 baseHash, uint64 auxHash, bool isGameShader, bool isGfxPackShader, const std::string& hlslCode);
	~RendererShaderD3D12() override = default;

	void PreponeCompilation(bool isRenderThread) override;
	void RequestCompilationPriority();
	bool IsCompiled() override;
	bool WaitForCompiled() override;

	const std::vector<uint8>& GetDXIL() const { return m_dxil; }
	bool HasDXIL() const { return !m_dxil.empty(); }

private:
	void CompileInternal();
	const wchar_t* GetTargetProfile() const;
	bool TryLoadFromCache();
	void StoreToCache() const;

	std::string m_hlslCode;
	uint64 m_hlslSourceHash{};
	std::vector<uint8> m_dxil;
	StateSemaphore<COMPILATION_STATE> m_compilationState{ COMPILATION_STATE::NONE };
	bool m_compilationFinished{false};
	bool m_compileSucceeded{false};
};
