// Part of D3D12Renderer.cpp (included there, same translation unit): memory pressure: pipeline and texture eviction, allocation recovery, scratch textures and copy tracking (UWP only).
// Not a standalone file: it relies on the includes and file-local helpers of D3D12Renderer.cpp.

#if defined(CEMU_UWP)
uint64 D3D12Renderer::GetTextureAllocationSize(const D3D12_RESOURCE_DESC& desc) const
{
	if (!m_device)
		return 0;

	const D3D12_RESOURCE_ALLOCATION_INFO allocationInfo = m_device->GetResourceAllocationInfo(0, 1, &desc);
	return allocationInfo.SizeInBytes;
}

size_t D3D12Renderer::EvictColdGamePipelines(uint32 minAgeFrames, size_t maxPipelines, bool highPressure)
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
		if (age >= minAgeFrames && !pipelineStableCache.KeepUnderMemoryPressure(key, highPressure))
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
	uint32 maxTextures, uint32 minAgeFrames, uint64 targetBytes, bool keepExpensiveReloads)
{
	// BOTW's terrain material arrays (1024x1024x83 BC3 + 512x512x83, ~130 MB) are the same for the whole map, but the
	// game stops drawing them for a few seconds on the map screen, loading screens, sleeping and teleports. Picked as
	// the largest cold texture, they were deleted and rebuilt identical 1.3-2 s later (7 times in 15 min)
	constexpr uint32 kExpensiveReloadMinAgeFrames = 60 * 60 * 2; // 2 min at 60 FPS, 4 at 30
	constexpr uint64 kExpensiveReloadMinBytes = 8ull * 1024ull * 1024ull; // the 512x512x83 BC1 companion is ~14.5 MB
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
		const uint64 bytes = allocationBytes != 0 ? allocationBytes : GetTextureAllocationSize(d3dTexture->GetDesc());
		// large arrays loaded from guest memory: every slice is decoded and written by the CPU again on reload
		const bool expensiveReload = texture->dim == Latte::E_DIM::DIM_2D_ARRAY && texture->depth >= 16 &&
			!texture->isUpdatedOnGPU && bytes >= kExpensiveReloadMinBytes;
		if (keepExpensiveReloads && expensiveReload && ageFrames < kExpensiveReloadMinAgeFrames)
		{
			++m_uwpExpensiveTexturesKept;
			continue;
		}
		candidates.push_back({ texture, bytes, ageFrames });
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
	// called from Latte while the recorder runs: trims change PSO caches and textures the recorder uses
	RecordingExclusiveScope pressureExclusive(this);
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
			[&](const auto& entry) { return !entry.second.fromAsync && !pipelineStableCache.KeepUnderMemoryPressure(entry.first, true); });
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
		evicted = EvictUwpTextureCache(maxTextures, minAgeFrames, targetTextureBytes, snapshot.pressure == Pressure::Soft);
	// PSOs keep their shader ISA in video memory; with async creation a cold one is cheap to rebuild
	size_t evictedPipelines = 0;
	// launch prewarm keeps thousands of PSOs alive that the current area never draws (BOTW forest: 2.4k at 82%)
	if (snapshot.pressure == Pressure::Soft)
	{
		// prioritized caching keeps the whole cache live, so soft pressure arrives sooner and the memory guard then builds
		// new pipelines one at a time (BOTW teleport: 150 queued, ~25 s of skipped draws). Occasional pipelines go after
		// 3600 frames (1 min at 60 FPS, 2 at 30) instead of five times that, unused prewarmed ones first; core and frequent
		// stay (KeepUnderMemoryPressure), and anything a draw used within those frames stays
		if (D3D12PipelineStableCache::GetInstance().IsPrioritized())
		{
			evictedPipelines += ReleaseUnusedPrewarmedPipelines(60 * 60, 512, false);
			evictedPipelines += EvictColdGamePipelines(60 * 60, 256, false);
		}
		evictedPipelines += EvictColdGamePipelines(60 * 60 * 5, 128, false) + ReleaseUnusedPrewarmedPipelines(60 * 60 * 5, 256, false);
	}
	else if (snapshot.pressure == Pressure::Hard)
		evictedPipelines = EvictColdGamePipelines(60 * 60, 256, true) + ReleaseUnusedPrewarmedPipelines(60 * 60, 512, true);
	else if (snapshot.pressure == Pressure::Critical)
		evictedPipelines = EvictColdGamePipelines(60 * 5, 1024, true) + ReleaseUnusedPrewarmedPipelines(60 * 5, 2048, true);
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
			"D3D12 Xbox memory trim: pressure={} usage={}/{}MB textures={} estimated={}MB scratch={}MB descriptorPool={}MB requested={}MB pipelines={} shaderBytecode={}MB large arrays kept={}.",
			CemuUwp::MemoryBudgetService::PressureName(snapshot.pressure),
			(unsigned long long)(snapshot.usageBytes / (1024ull * 1024ull)),
			(unsigned long long)(snapshot.limitBytes / (1024ull * 1024ull)),
			evicted.textures,
			(unsigned long long)(evicted.bytes / (1024ull * 1024ull)),
			(unsigned long long)(scratchBytes / (1024ull * 1024ull)),
			(unsigned long long)(descriptorPoolBytes / (1024ull * 1024ull)),
			(unsigned long long)(snapshot.recommendedReleaseBytes / (1024ull * 1024ull)),
			evictedPipelines,
			(unsigned long long)(evictedDxilBytes / (1024ull * 1024ull)), m_uwpExpensiveTexturesKept);
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
	D3D12_QUEUE_RECORDING_KIND(Both, SurfaceSync(coher, address, size));
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

