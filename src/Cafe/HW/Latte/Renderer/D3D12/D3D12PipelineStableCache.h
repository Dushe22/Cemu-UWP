#pragma once

#include "util/helpers/fspinlock.h"

#include <atomic>
#include <bit>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <span>
#include <thread>
#include <unordered_set>
#include <unordered_map>
#include <vector>

class D3D12PipelineStableCache
{
	struct PipelineHash
	{
		PipelineHash(uint64 baseHash, uint64 pipelineStateHash)
			: baseHash(baseHash), pipelineStateHash(pipelineStateHash) {}

		uint64 baseHash;
		uint64 pipelineStateHash;

		bool operator==(const PipelineHash& rhs) const
		{
			return baseHash == rhs.baseHash && pipelineStateHash == rhs.pipelineStateHash;
		}

		struct HashFunc
		{
			size_t operator()(const PipelineHash& value) const
			{
				static_assert(sizeof(uint64) == sizeof(size_t));
				return value.baseHash ^ std::rotl(value.pipelineStateHash, 17);
			}
		};
	};

public:
	static D3D12PipelineStableCache& GetInstance();

	uint32 BeginLoading(uint64 cacheTitleId);
	bool UpdateLoading(uint32& pipelinesLoadedTotal, uint32& pipelinesMissingShaders);
	void EndLoading();
	void Close();
	// stops the prewarm workers, including the one that continues after launch
	void StopPrewarm();

	bool AddPipelineToCache(uint64 baseHash, uint64 pipelineStateHash);
	// a stored recipe is kept unless replaceExisting says it turned out stale
	bool AddPipelineRecipe(uint64 pipelineStateHash, std::span<const uint8> bytes, bool replaceExisting);
	// render thread: a draw used this pipeline in the current session
	void MarkPipelineUsed(uint64 pipelineStateHash);
	// the app may be closed without Close, so usage is written soon after it changes
	void SaveUsageIfDue();
	void SaveUsage();
	size_t GetCachedPipelineCount();

private:
	struct RecipeUsage
	{
		uint32 lastSession{};
		uint32 sessionsUsed{};
	};
	struct PrewarmJob
	{
		sint32 fileIndex{};
		uint64 pipelineKey{};
	};

	bool DecodePipelineHashFromCache(std::span<uint8> fileData, PipelineHash& hash) const;
	bool LoadPipelineHashFromCache(std::span<uint8> fileData);
	std::vector<uint8> SerializePipelineHash(uint64 baseHash, uint64 pipelineStateHash) const;

	void LoadRecipeUsageLocked(uint64 cacheTitleId);
	void SaveRecipeUsageLocked();
	void PlanPrewarmLocked(const std::vector<PrewarmJob>& inventory, const std::unordered_set<uint64>& livePipelines);
	void DeleteRecipeLocked(uint64 pipelineKey);
	void RunPrewarmWorker(size_t endJob, bool background);
	void PrewarmRecipe(const PrewarmJob& job);
	bool WasUsedThisSession(uint64 pipelineKey);

	class FileCache* m_cache{};
	std::mutex m_cacheLock;
	sint32 m_pipelineMaxFileIndex{};
	std::unordered_set<PipelineHash, PipelineHash::HashFunc> m_pipelineIsCached;
	FSpinlock m_pipelineIsCachedLock;
	std::unordered_map<uint64, size_t> m_recipeSizes;
	bool m_recipeStorageDisabled{};
	uint64 m_recipeBytes{};
	uint32 m_recipeWritesQueued{};
	uint32 m_recipeWriteFailures{};
	uint32 m_recipeBudgetSkips{};
	uint32 m_recipeInventory{};
	uint32 m_recipesPruned{};

	// which session each pipeline was last used in, so launches wait only for pipelines in recent use
	std::mutex m_usageLock;
	std::unordered_map<uint64, RecipeUsage> m_recipeUsage;
	std::filesystem::path m_usagePath;
	uint32 m_session{};
	uint32 m_newUsesSinceSave{};
	std::atomic_bool m_usageUnsaved{};
	std::atomic<sint64> m_lastUsageSaveTicks{};
	bool m_usageHistoryFound{};
	bool m_shaderGeneratorChanged{};

	// prewarm: m_prewarmJobs holds the recipes launches wait for first, then the background ones
	std::mutex m_prewarmControlLock;
	std::vector<PrewarmJob> m_prewarmJobs;
	size_t m_blockingJobCount{};
	std::atomic_size_t m_nextPrewarmJob{};
	std::atomic_size_t m_finishedBlockingJobs{};
	std::atomic_bool m_prewarmStop{};
	std::vector<std::thread> m_prewarmWorkers;
	std::thread m_backgroundPrewarmWorker;
	std::mutex m_prewarmProgressLock;
	std::condition_variable m_prewarmProgress;
	std::atomic_uint32_t m_recipesPrewarmed{};
	std::atomic_uint32_t m_recipesSkipped{};
	std::atomic_uint32_t m_recipesRejected{};
};
