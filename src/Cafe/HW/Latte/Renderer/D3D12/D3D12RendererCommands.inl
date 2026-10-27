// Part of D3D12Renderer.cpp (included there, same translation unit): command lists, fences, deferred releases, cached command state and resource transitions.
// Not a standalone file: it relies on the includes and file-local helpers of D3D12Renderer.cpp.

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
		cemuLog_log(LogType::Force, "D3D12: Close command list failed with HRESULT 0x{:08x} (recorder thread {}, record context {}, reasons 0x{:x}, draws {}).",
			(uint32)closeHr, t_isRecorderThread, t_recordContext, workloadReasons, m_recordedDrawcalls);
		m_gpuWatchdog.LogOpenList("close failed");
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
	struct S0Timer
	{
		uint64 start = t_uwpInDrawTables ? UwpProfTick() : 0;
		~S0Timer() { if (start) s_uwpRecorderSplit.Add(s_uwpRecorderSplit.tableTransitions, UwpProfTick() - start); }
	} s0Timer;
	if (!view)
		return;
	auto* texture = view->GetBaseImage();
	if (!texture || !texture->GetResource())
		return;

	const bool useSatisfiedFastPath = PerformanceExperimentEnabled(D3D12PerformanceExperiment::SatisfiedViewTransition);
	const bool alreadySatisfied = useSatisfiedFastPath && texture->AllSubresourcesInState(newState);
	if (useSatisfiedFastPath && alreadySatisfied)
	{
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

