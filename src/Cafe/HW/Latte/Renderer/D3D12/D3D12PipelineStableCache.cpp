#include "Cafe/HW/Latte/Renderer/D3D12/D3D12PipelineStableCache.h"

#include "Cafe/HW/Latte/Core/LatteShaderCache.h"
#include "Cafe/HW/Latte/LegacyShaderDecompiler/LatteDecompilerEmitHLSL.hpp"
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12Renderer.h"
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12PipelineRecipe.h"
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12PipelinePrewarmPolicy.h"
#include "Cafe/HW/Latte/Renderer/D3D12/RendererShaderD3D12.h"
#include "Cafe/HW/Latte/Renderer/Renderer.h"
#include "Cemu/FileCache/FileCache.h"
#include "config/ActiveSettings.h"
#include "util/helpers/Serializer.h"
#include "util/helpers/helpers.h"

#include <algorithm>
#include <fstream>

namespace
{
constexpr uint8 kLegacyPipelineEntryVersion = 1;
constexpr uint8 kPipelineEntryVersion = 2;
constexpr uint32 kUwpPipelineCacheFormatVersion = 0xD3120003u;
constexpr uint64 kRecipeFileTag = 0x4433313250534f31ull;

constexpr uint32 kRecipeUsageMagic = 0x31555244u;
constexpr uint32 kRecipeUsageVersion = 1;
// launches wait for pipelines used in one of the last few sessions; the rest are built after launch
constexpr uint32 kRecentSessions = 3;
// recipes nobody used for this many sessions are deleted
constexpr uint32 kPruneAfterSessions = 10;
// usage is saved during play too, since a closed app may never reach Close
constexpr uint32 kUsageSaveInterval = 128;
constexpr auto kUsageSaveDelay = std::chrono::seconds(10);

struct RecipeUsageHeader
{
	uint32 magic;
	uint32 version;
	uint32 shaderGeneratorVersion;
	uint32 session;
	uint32 count;
};

struct RecipeUsageRecord
{
	uint64 pipelineKey;
	uint32 lastSession;
	uint32 sessionsUsed;
};
static_assert(sizeof(RecipeUsageHeader) == 20 && sizeof(RecipeUsageRecord) == 16);
}

D3D12PipelineStableCache g_d3d12PipelineStableCacheInstance;

D3D12PipelineStableCache& D3D12PipelineStableCache::GetInstance()
{
	return g_d3d12PipelineStableCacheInstance;
}

uint32 D3D12PipelineStableCache::BeginLoading(uint64 cacheTitleId)
{
	StopPrewarm();
	m_prewarmStop = false;
	if (auto* renderer = D3D12Renderer::GetInstance())
	{
		std::unique_lock<std::recursive_mutex> rendererLock(g_rendererMutex);
		renderer->PrepareGamePipelineLibrary(cacheTitleId);
		renderer->BeginGamePipelineRecipeLoading(cacheTitleId);
	}

	std::vector<PrewarmJob> inventory;
	{
		std::lock_guard cacheLock(m_cacheLock);
		std::error_code ec;
		fs::create_directories(ActiveSettings::GetCachePath("shaderCache/transferable"), ec);
		const auto pathCacheFile = ActiveSettings::GetCachePath("shaderCache/transferable/{:016x}_d3d12recipes.bin", cacheTitleId);
		m_recipeSizes.clear();
		m_recipeStorageDisabled = false;
		m_recipeBytes = 0;
		m_recipeWritesQueued = 0;
		m_recipeWriteFailures = 0;
		m_recipeBudgetSkips = 0;
		m_recipeInventory = 0;
		m_recipesPruned = 0;
		m_recipesPrewarmed = 0;
		m_recipesSkipped = 0;
		m_recipesRejected = 0;
		m_prewarmJobs.clear();
		m_blockingJobCount = 0;
		m_nextPrewarmJob = 0;
		m_finishedBlockingJobs = 0;
		m_pipelineMaxFileIndex = 0;
		{
			std::lock_guard pipelineLock(m_pipelineIsCachedLock);
			m_pipelineIsCached.clear();
		}

		delete m_cache;
		m_cache = nullptr;
		uint32 cacheExtraVersion = LatteShaderCache_getPipelineCacheExtraVersion(cacheTitleId);
		cacheExtraVersion ^= kUwpPipelineCacheFormatVersion;
		try
		{
			m_cache = FileCache::Open(pathCacheFile, true, cacheExtraVersion);
		}
		catch (const std::exception& ex)
		{
			cemuLog_log(LogType::Force, "D3D12 UWP optional pipeline recipe cache could not open: {}", ex.what());
			return 0;
		}
		if (!m_cache)
		{
			cemuLog_log(LogType::Force, "Failed to open or create D3D12 pipeline cache file: {}", _pathToUtf8(pathCacheFile));
			return 0;
		}

		m_cache->UseCompression(false);
		m_pipelineMaxFileIndex = m_cache->GetMaximumFileIndex();
		const uint32 fileCount = (uint32)m_cache->GetFileCount();
		try
		{
			// index every entry up front so budget accounting survives an early prewarm stop
			for (sint32 index = 0; index < m_pipelineMaxFileIndex; ++index)
			{
				uint64 nameA = 0, nameB = 0;
				uint32 size = 0;
				if (!m_cache->GetUncompressedFileInfoByIndexBounded(index, nameA, nameB, size, D3D12PipelineRecipe::kMaxRecordBytes) ||
					nameA != kRecipeFileTag || m_recipeSizes.contains(nameB))
					continue;
				if (m_recipeSizes.size() >= D3D12PipelinePrewarmPolicy::kMaxRecipeCacheEntries ||
					size > D3D12PipelinePrewarmPolicy::kMaxRecipeCacheBytes - m_recipeBytes)
				{
					m_recipeStorageDisabled = true;
					++m_recipeBudgetSkips;
					continue;
				}
				m_recipeSizes.emplace(nameB, size);
				m_recipeBytes += size;
				inventory.push_back({ index, nameB });
			}
		}
		catch (const std::exception& ex)
		{
			cemuLog_log(LogType::Force, "D3D12 UWP optional recipe index unavailable: {}", ex.what());
			m_recipeStorageDisabled = true;
			inventory.clear();
		}
		m_recipeInventory = (uint32)inventory.size();
		cemuLog_log(LogType::Force, "D3D12 UWP pipeline recipes: opened title={:016x} records={} inventory={} entryCap={} byteCapMB={} inventoryBudgetSkips={} path={} (full PSO inputs; first-live fingerprint verification enabled).",
			cacheTitleId, fileCount, inventory.size(),
			D3D12PipelinePrewarmPolicy::kMaxRecipeCacheEntries,
			D3D12PipelinePrewarmPolicy::kMaxRecipeCacheBytes / (1024ull * 1024ull),
			m_recipeBudgetSkips, _pathToUtf8(pathCacheFile));
	}

	// pipelines still alive from an earlier run of this title in the same process need no prewarm
	std::unordered_set<uint64> livePipelines;
	if (auto* renderer = D3D12Renderer::GetInstance())
	{
		std::unique_lock<std::recursive_mutex> rendererLock(g_rendererMutex);
		for (const PrewarmJob& job : inventory)
		{
			if (renderer->HasLiveGamePipeline(job.pipelineKey))
				livePipelines.emplace(job.pipelineKey);
		}
	}

	{
		std::scoped_lock locks(m_cacheLock, m_usageLock);
		LoadRecipeUsageLocked(cacheTitleId);
		PlanPrewarmLocked(inventory, livePipelines);
	}

	std::lock_guard controlLock(m_prewarmControlLock);
	const size_t workerCount = std::min<size_t>(RendererShaderD3D12::GetNativeCompilationConcurrency(), m_blockingJobCount);
	for (size_t i = 0; i < workerCount; ++i)
		m_prewarmWorkers.emplace_back(&D3D12PipelineStableCache::RunPrewarmWorker, this, m_blockingJobCount, false);
	return (uint32)m_blockingJobCount;
}

void D3D12PipelineStableCache::LoadRecipeUsageLocked(uint64 cacheTitleId)
{
	m_recipeUsage.clear();
	m_newUsesSinceSave = 0;
	m_usageHistoryFound = false;
	m_shaderGeneratorChanged = false;
	m_session = 1;
	m_usagePath = ActiveSettings::GetCachePath("shaderCache/transferable/{:016x}_d3d12recipes_usage.bin", cacheTitleId);

	std::ifstream file(m_usagePath, std::ios::binary);
	RecipeUsageHeader header{};
	if (!file || !file.read(reinterpret_cast<char*>(&header), sizeof(header)) ||
		header.magic != kRecipeUsageMagic || header.version != kRecipeUsageVersion ||
		header.count > D3D12PipelinePrewarmPolicy::kMaxRecipeCacheEntries * 4)
		return;
	std::vector<RecipeUsageRecord> records(header.count);
	if (header.count != 0 && !file.read(reinterpret_cast<char*>(records.data()), records.size() * sizeof(RecipeUsageRecord)))
		return;

	m_usageHistoryFound = true;
	m_session = header.session + 1;
	if (header.shaderGeneratorVersion != LATTE_DECOMPILER_HLSL_EMITTER_VERSION)
	{
		m_shaderGeneratorChanged = true;
		return;
	}
	m_recipeUsage.reserve(records.size());
	for (const RecipeUsageRecord& record : records)
	{
		if (record.lastSession <= header.session)
			m_recipeUsage[record.pipelineKey] = { record.lastSession, record.sessionsUsed };
	}
}

void D3D12PipelineStableCache::SaveRecipeUsageLocked()
{
	if (m_session == 0 || m_usagePath.empty())
		return;
	std::vector<RecipeUsageRecord> records;
	records.reserve(m_recipeUsage.size());
	for (const auto& [pipelineKey, usage] : m_recipeUsage)
		records.push_back({ pipelineKey, usage.lastSession, usage.sessionsUsed });
	const RecipeUsageHeader header{ kRecipeUsageMagic, kRecipeUsageVersion, LATTE_DECOMPILER_HLSL_EMITTER_VERSION, m_session, (uint32)records.size() };
	// a torn write fails validation on the next load and costs only one session of history
	std::ofstream file(m_usagePath, std::ios::binary | std::ios::trunc);
	if (!file)
		return;
	file.write(reinterpret_cast<const char*>(&header), sizeof(header));
	if (!records.empty())
		file.write(reinterpret_cast<const char*>(records.data()), records.size() * sizeof(RecipeUsageRecord));
	m_newUsesSinceSave = 0;
	m_usageUnsaved = false;
	m_lastUsageSaveTicks = std::chrono::steady_clock::now().time_since_epoch().count();
}

void D3D12PipelineStableCache::SaveUsageIfDue()
{
	if (!m_usageUnsaved.load(std::memory_order_relaxed))
		return;
	const auto lastSave = std::chrono::steady_clock::time_point(std::chrono::steady_clock::duration(m_lastUsageSaveTicks.load()));
	if (std::chrono::steady_clock::now() - lastSave < kUsageSaveDelay)
		return;
	SaveUsage();
}

void D3D12PipelineStableCache::SaveUsage()
{
	std::lock_guard usageLock(m_usageLock);
	if (m_usageUnsaved.load())
		SaveRecipeUsageLocked();
}

void D3D12PipelineStableCache::PlanPrewarmLocked(const std::vector<PrewarmJob>& inventory, const std::unordered_set<uint64>& livePipelines)
{
	if (m_shaderGeneratorChanged)
	{
		// every stored recipe embeds shaders from the old generator, so none would match a draw any more
		for (const PrewarmJob& job : inventory)
			DeleteRecipeLocked(job.pipelineKey);
		m_recipesPruned = (uint32)inventory.size();
		m_recipeUsage.clear();
		cemuLog_log(LogType::Force, "D3D12 UWP pipeline recipes: the shader generator changed, so {} stored recipes were deleted.", inventory.size());
		return;
	}

	struct Candidate
	{
		PrewarmJob job;
		RecipeUsage usage;
	};
	std::vector<Candidate> recent;
	std::vector<Candidate> others;
	bool hasHistory = false;
	std::unordered_set<uint64> stored;
	stored.reserve(inventory.size());
	for (const PrewarmJob& job : inventory)
	{
		stored.emplace(job.pipelineKey);
		// recipes without history get the full grace period
		const RecipeUsage usage = m_recipeUsage.try_emplace(job.pipelineKey, RecipeUsage{ m_session, 0 }).first->second;
		if (m_session - usage.lastSession >= kPruneAfterSessions)
		{
			DeleteRecipeLocked(job.pipelineKey);
			m_recipeUsage.erase(job.pipelineKey);
			++m_recipesPruned;
			continue;
		}
		hasHistory = hasHistory || usage.sessionsUsed != 0;
		if (livePipelines.contains(job.pipelineKey))
			continue;
		const bool recentlyUsed = usage.sessionsUsed != 0 && m_session - usage.lastSession <= kRecentSessions;
		(recentlyUsed ? recent : others).push_back({ job, usage });
	}
	// history of pipelines whose recipe is gone is of no further use
	std::erase_if(m_recipeUsage, [&](const auto& entry) { return !stored.contains(entry.first); });

	// most used first, so an early stop keeps the most valuable ones
	std::stable_sort(recent.begin(), recent.end(), [](const Candidate& a, const Candidate& b) {
		return a.usage.sessionsUsed != b.usage.sessionsUsed ? a.usage.sessionsUsed > b.usage.sessionsUsed : a.usage.lastSession > b.usage.lastSession;
	});
	std::stable_sort(others.begin(), others.end(), [](const Candidate& a, const Candidate& b) {
		return a.usage.lastSession != b.usage.lastSession ? a.usage.lastSession > b.usage.lastSession : a.usage.sessionsUsed > b.usage.sessionsUsed;
	});
	m_prewarmJobs.reserve(recent.size() + others.size());
	for (const Candidate& candidate : recent)
		m_prewarmJobs.push_back(candidate.job);
	for (const Candidate& candidate : others)
		m_prewarmJobs.push_back(candidate.job);
	// without any history yet, nothing tells which pipelines matter, so launches wait for all of them as before
	m_blockingJobCount = hasHistory ? recent.size() : m_prewarmJobs.size();

	cemuLog_log(LogType::Force, "D3D12 UWP pipeline recipes: session {}{}: launch waits for {} recently used, {} built after launch, {} already live, {} pruned after {} unused sessions.",
		m_session, hasHistory ? "" : " (no usage history yet)", m_blockingJobCount, m_prewarmJobs.size() - m_blockingJobCount,
		livePipelines.size(), m_recipesPruned, kPruneAfterSessions);
}

void D3D12PipelineStableCache::DeleteRecipeLocked(uint64 pipelineKey)
{
	const auto it = m_recipeSizes.find(pipelineKey);
	if (it == m_recipeSizes.end())
		return;
	m_recipeBytes -= std::min<uint64>(m_recipeBytes, it->second);
	m_recipeSizes.erase(it);
	try
	{
		if (m_cache)
			m_cache->DeleteFile({ kRecipeFileTag, pipelineKey });
	}
	catch (const std::exception& ex)
	{
		cemuLog_log(LogType::Force, "D3D12 UWP pipeline recipe {:016x} could not be deleted: {}", pipelineKey, ex.what());
	}
}

bool D3D12PipelineStableCache::WasUsedThisSession(uint64 pipelineKey)
{
	std::lock_guard usageLock(m_usageLock);
	const auto it = m_recipeUsage.find(pipelineKey);
	return it != m_recipeUsage.end() && it->second.lastSession == m_session && it->second.sessionsUsed != 0;
}

void D3D12PipelineStableCache::PrewarmRecipe(const PrewarmJob& job)
{
	// a draw that needed the pipeline before its turn already created it
	if (WasUsedThisSession(job.pipelineKey))
	{
		++m_recipesSkipped;
		return;
	}
	uint64 nameA = 0;
	uint64 nameB = 0;
	std::vector<uint8> fileData;
	bool read = false;
	try
	{
		// the file cache locks internally; m_cache stays valid until StopPrewarm has joined this thread
		read = m_cache && m_cache->GetUncompressedFileByIndexBounded(job.fileIndex, &nameA, &nameB, fileData, D3D12PipelineRecipe::kMaxRecordBytes);
	}
	catch (const std::exception& ex)
	{
		cemuLog_log(LogType::Force, "D3D12 UWP pipeline recipe read failed: {}", ex.what());
	}
	// entries can move when recipes are added or deleted meanwhile
	if (!read || nameA != kRecipeFileTag || nameB != job.pipelineKey)
	{
		++m_recipesSkipped;
		return;
	}

	auto* renderer = D3D12Renderer::GetInstance();
	if (!renderer)
	{
		m_prewarmStop = true;
		return;
	}
	switch (renderer->PrewarmGamePipelineRecipe(job.pipelineKey, fileData))
	{
	case D3D12Renderer::RecipePrewarmResult::Created:
		++m_recipesPrewarmed;
		try
		{
			std::lock_guard pipelineLock(m_pipelineIsCachedLock);
			m_pipelineIsCached.emplace(0, job.pipelineKey);
		}
		catch (const std::bad_alloc&) {}
		break;
	case D3D12Renderer::RecipePrewarmResult::Rejected:
	{
		// a draw records a fresh recipe once the stored one is gone
		++m_recipesRejected;
		std::scoped_lock locks(m_cacheLock, m_usageLock);
		DeleteRecipeLocked(job.pipelineKey);
		break;
	}
	case D3D12Renderer::RecipePrewarmResult::Stopped:
		++m_recipesSkipped;
		if (renderer->IsGamePipelineRecipePrewarmStopped())
			m_prewarmStop = true;
		break;
	}
}

void D3D12PipelineStableCache::RunPrewarmWorker(size_t endJob, bool background)
{
	SetThreadName(background ? "d3d12PsoPrewarmBg" : "d3d12PsoPrewarm");
	// after launch the emulated game comes first
	SetThreadPriority(GetCurrentThread(), background ? THREAD_PRIORITY_LOWEST : THREAD_PRIORITY_NORMAL);
	bool completed = true;
	while (true)
	{
		if (m_prewarmStop.load(std::memory_order_acquire))
		{
			completed = false;
			break;
		}
		const size_t index = m_nextPrewarmJob.fetch_add(1, std::memory_order_relaxed);
		if (index >= endJob)
			break;
		PrewarmRecipe(m_prewarmJobs[index]);
		if (!background)
		{
			m_finishedBlockingJobs.fetch_add(1, std::memory_order_release);
			m_prewarmProgress.notify_all();
		}
	}
	m_prewarmProgress.notify_all();
	if (background)
	{
		if (completed)
		{
			if (auto* renderer = D3D12Renderer::GetInstance())
				renderer->FinishBackgroundGamePipelineRecipePrewarm();
		}
		else
		{
			auto* renderer = D3D12Renderer::GetInstance();
			cemuLog_log(LogType::Force, "D3D12 UWP background pipeline prewarm stopped after {} of {} recipes (reason={}).",
				std::min(m_nextPrewarmJob.load(), endJob) - m_blockingJobCount, endJob - m_blockingJobCount,
				renderer && renderer->IsGamePipelineRecipePrewarmStopped() ? renderer->GetGamePipelineRecipePrewarmStopReason() : "title_closed");
		}
	}
}

bool D3D12PipelineStableCache::UpdateLoading(uint32& pipelinesLoadedTotal, uint32& pipelinesMissingShaders)
{
	{
		std::unique_lock progressLock(m_prewarmProgressLock);
		const size_t before = m_finishedBlockingJobs.load(std::memory_order_acquire);
		m_prewarmProgress.wait_for(progressLock, std::chrono::milliseconds(30), [&]() {
			return before >= m_blockingJobCount || m_prewarmStop.load(std::memory_order_acquire) ||
				m_finishedBlockingJobs.load(std::memory_order_acquire) != before;
		});
	}
	// the loading thread is the render thread, which owns the pipelines draws can use
	if (auto* renderer = D3D12Renderer::GetInstance())
	{
		std::unique_lock<std::recursive_mutex> rendererLock(g_rendererMutex);
		renderer->AdoptPrewarmedGamePipelines();
	}
	const size_t finished = std::min(m_finishedBlockingJobs.load(std::memory_order_acquire), m_blockingJobCount);
	pipelinesLoadedTotal = (uint32)finished;
	pipelinesMissingShaders = m_recipesSkipped.load() + m_recipesRejected.load();
	return finished < m_blockingJobCount && !m_prewarmStop.load(std::memory_order_acquire);
}

void D3D12PipelineStableCache::EndLoading()
{
	// the control lock is never held while taking the renderer lock; shutdown takes them the other way round
	{
		std::lock_guard controlLock(m_prewarmControlLock);
		for (auto& worker : m_prewarmWorkers)
			worker.join();
		m_prewarmWorkers.clear();
	}

	bool background = !m_prewarmStop.load() && m_prewarmJobs.size() > m_blockingJobCount;
	if (auto* renderer = D3D12Renderer::GetInstance())
	{
		std::unique_lock<std::recursive_mutex> rendererLock(g_rendererMutex);
		renderer->EndGamePipelineRecipeLoading(background);
		background = background && !renderer->IsGamePipelineRecipePrewarmStopped();
	}
	{
		std::lock_guard cacheLock(m_cacheLock);
		cemuLog_log(LogType::Force, "D3D12 UWP pipeline recipes: session={} inventory={} launchWaitedFor={} builtAfterLaunch={} nativePrewarmed={} skipped={} rejected={} pruned={} cachedRecipeMB={:.2f} entryCap={} storageDisabled={}.",
			m_session, m_recipeInventory, m_blockingJobCount, background ? m_prewarmJobs.size() - m_blockingJobCount : 0,
			m_recipesPrewarmed.load(), m_recipesSkipped.load(), m_recipesRejected.load(), m_recipesPruned,
			(double)m_recipeBytes / (1024.0 * 1024.0), D3D12PipelinePrewarmPolicy::kMaxRecipeCacheEntries,
			m_recipeStorageDisabled ? 1 : 0);
	}
	{
		// count the session even if the app is closed without reaching Close
		std::lock_guard usageLock(m_usageLock);
		SaveRecipeUsageLocked();
	}
	if (background)
	{
		std::lock_guard controlLock(m_prewarmControlLock);
		// a shutdown may have stopped the prewarm meanwhile
		if (!m_prewarmStop.load())
		{
			m_nextPrewarmJob = m_blockingJobCount;
			m_backgroundPrewarmWorker = std::thread(&D3D12PipelineStableCache::RunPrewarmWorker, this, m_prewarmJobs.size(), true);
		}
	}
}

void D3D12PipelineStableCache::StopPrewarm()
{
	std::lock_guard controlLock(m_prewarmControlLock);
	m_prewarmStop = true;
	m_prewarmProgress.notify_all();
	for (auto& worker : m_prewarmWorkers)
	{
		if (worker.joinable())
			worker.join();
	}
	m_prewarmWorkers.clear();
	if (m_backgroundPrewarmWorker.joinable())
		m_backgroundPrewarmWorker.join();
}

void D3D12PipelineStableCache::Close()
{
	StopPrewarm();
	{
		std::unique_lock<std::recursive_mutex> rendererLock(g_rendererMutex);
		if (auto* renderer = D3D12Renderer::GetInstance())
			renderer->ClearPendingGamePipelineRecipes();
	}
	std::scoped_lock locks(m_cacheLock, m_usageLock);
	SaveRecipeUsageLocked();
	m_recipeUsage.clear();
	m_session = 0;
	m_usagePath.clear();
	delete m_cache;
	m_cache = nullptr;
	m_pipelineMaxFileIndex = 0;
	m_prewarmJobs.clear();
	m_blockingJobCount = 0;
	{
		std::lock_guard pipelineLock(m_pipelineIsCachedLock);
		m_pipelineIsCached.clear();
	}
	cemuLog_log(LogType::Force, "D3D12 UWP pipeline recipes: close after queuedWrites={} writeQueueFailures={} budgetSkips={} (pending file-cache writes drained).",
		m_recipeWritesQueued, m_recipeWriteFailures, m_recipeBudgetSkips);
	m_recipeSizes.clear();
	m_recipeBytes = 0;
}

void D3D12PipelineStableCache::MarkPipelineUsed(uint64 pipelineStateHash)
{
	std::lock_guard usageLock(m_usageLock);
	if (m_session == 0)
		return;
	RecipeUsage& usage = m_recipeUsage[pipelineStateHash];
	if (usage.lastSession == m_session && usage.sessionsUsed != 0)
		return;
	usage.lastSession = m_session;
	++usage.sessionsUsed;
	m_usageUnsaved = true;
	if (++m_newUsesSinceSave >= kUsageSaveInterval)
		SaveRecipeUsageLocked();
}

bool D3D12PipelineStableCache::AddPipelineToCache(uint64 baseHash, uint64 pipelineStateHash)
{
	// UWP records full recipes through AddPipelineRecipe instead
	return false;
}

bool D3D12PipelineStableCache::AddPipelineRecipe(uint64 pipelineStateHash, std::span<const uint8> bytes, bool replaceExisting)
{
	std::lock_guard cacheLock(m_cacheLock);
	if (!m_cache || m_recipeStorageDisabled || bytes.empty() || bytes.size() > D3D12PipelineRecipe::kMaxRecordBytes)
		return false;
	const auto existing = m_recipeSizes.find(pipelineStateHash);
	// the stored recipe still describes this pipeline; it was only not prewarmed yet
	if (existing != m_recipeSizes.end() && !replaceExisting)
		return true;
	const uint64 previousBytes = existing == m_recipeSizes.end() ? 0 : existing->second;
	if (!D3D12PipelinePrewarmPolicy::CanStoreRecipe(m_recipeSizes.size(), m_recipeBytes,
		previousBytes, bytes.size(), existing != m_recipeSizes.end()))
	{
		if (++m_recipeBudgetSkips == 1)
			cemuLog_log(LogType::Force, "D3D12 UWP pipeline recipe storage budget reached; uncached draws still create pipelines normally.");
		return false;
	}
	try
	{
		// reserve the budget before enqueueing so the write is never unaccounted
		const auto [sizeIt, inserted] = m_recipeSizes.try_emplace(pipelineStateHash, previousBytes);
		// AddFileAsync copies the payload
		try
		{
			m_cache->AddFileAsync({ kRecipeFileTag, pipelineStateHash }, bytes.data(), (sint32)bytes.size());
		}
		catch (...)
		{
			if (inserted)
				m_recipeSizes.erase(sizeIt);
			throw;
		}
		sizeIt->second = bytes.size();
		m_recipeBytes = m_recipeBytes - previousBytes + bytes.size();
		++m_recipeWritesQueued;
		std::lock_guard pipelineLock(m_pipelineIsCachedLock);
		m_pipelineIsCached.emplace(0, pipelineStateHash);
		if (m_recipeWritesQueued == 1 || (m_recipeWritesQueued % 64) == 0)
			cemuLog_log(LogType::Force, "D3D12 UWP pipeline recipes: queuedWrites={} entries={} recipeMB={:.2f}.",
				m_recipeWritesQueued, m_recipeSizes.size(), (double)m_recipeBytes / (1024.0 * 1024.0));
		return true;
	}
	catch (const std::exception& ex)
	{
		if (++m_recipeWriteFailures == 1)
			cemuLog_log(LogType::Force, "D3D12 UWP optional pipeline recipe write failed: {}", ex.what());
		return false;
	}
}

size_t D3D12PipelineStableCache::GetCachedPipelineCount()
{
	std::lock_guard pipelineLock(m_pipelineIsCachedLock);
	const size_t count = m_pipelineIsCached.size();
	return count;
}

bool D3D12PipelineStableCache::DecodePipelineHashFromCache(std::span<uint8> fileData, PipelineHash& hash) const
{
	if (fileData.size() < 17)
		return false;

	MemStreamReader reader(fileData.data(), (sint32)fileData.size());
	const uint8 version = reader.readBE<uint8>();
	if (version != kPipelineEntryVersion)
		return false;

	const uint64 baseHash = reader.readBE<uint64>();
	const uint64 pipelineStateHash = reader.readBE<uint64>();
	if (reader.hasError())
		return false;

	hash = PipelineHash(baseHash, pipelineStateHash);
	return true;
}

bool D3D12PipelineStableCache::LoadPipelineHashFromCache(std::span<uint8> fileData)
{
	PipelineHash hash{ 0, 0 };
	if (!DecodePipelineHashFromCache(fileData, hash))
		return false;

	std::lock_guard pipelineLock(m_pipelineIsCachedLock);
	m_pipelineIsCached.emplace(hash);
	return true;
}

std::vector<uint8> D3D12PipelineStableCache::SerializePipelineHash(uint64 baseHash, uint64 pipelineStateHash) const
{
	MemStreamWriter writer(32);
	writer.writeBE<uint8>(kPipelineEntryVersion);
	writer.writeBE<uint64>(baseHash);
	writer.writeBE<uint64>(pipelineStateHash);
	std::vector<uint8> result;
	writer.getResultAndReset(result);
	return result;
}
