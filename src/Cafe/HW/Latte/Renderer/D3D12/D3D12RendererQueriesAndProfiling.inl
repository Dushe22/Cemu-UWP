// Part of D3D12Renderer.cpp (included there, same translation unit): occlusion queries, device-removed checks, checked view creation, clears and the UWP performance logs.
// Not a standalone file: it relies on the includes and file-local helpers of D3D12Renderer.cpp.

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
				cemuLog_log(LogType::Force, "D3D12 UWP recorder: busy {:.0f} ms, {} ops; Latte waited on a full queue {}x for {:.0f} ms; preparation stage busy {:.0f} ms, {} ops moved.",
					(double)m_recordBusyNs.exchange(0, std::memory_order_relaxed) / 1e6, m_recordOps.exchange(0, std::memory_order_relaxed),
					fullWaits, (double)fullWaitNs / 1e6,
					(double)m_prepareBusyNs.exchange(0, std::memory_order_relaxed) / 1e6, m_prepareMoved.exchange(0, std::memory_order_relaxed));
				const double collectUs = m_uwpCollectRecordSamples ? (double)m_uwpCollectRecordNs / 1000.0 / (double)m_uwpCollectRecordSamples : 0.0;
				m_uwpCollectRecordNs = 0;
				m_uwpCollectRecordSamples = 0;
				cemuLog_log(LogType::Force, "D3D12 UWP recorder, Latte side: queueing {:.2f} us per op, pipeline stage head {:.2f} us per draw (register delta {:.2f}); exclusive scopes {}x waited {:.0f} ms [{}].",
					queueCallUs, m_uwpLastHeadPipelineUs, collectUs, exclusiveCount, (double)exclusiveNs / 1e6, exclusiveText);
				auto takeKind = [this](RecordOpKind kind) { return m_recordOpKinds[(size_t)kind].exchange(0, std::memory_order_relaxed); };
				const uint64 stateOps = takeKind(RecordOpKind::State), commandOps = takeKind(RecordOpKind::Command);
				const uint64 bothOps = takeKind(RecordOpKind::Both), syncOps = takeKind(RecordOpKind::Sync);
				cemuLog_log(LogType::Force, "UWP recorder op kinds (S2c): state {}, command {}, both {} (draws included), sync {}.",
					stateOps, commandOps, bothOps, syncOps);
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


void LatteIndices_UwpLogStats();
extern uint64 g_uwpTextureLookupTicks;
extern uint64 g_uwpBufSyncTicks[6];
extern uint64 g_uwpBufSyncCounts[3];
extern uint64 g_uwpBufSyncBytes[2];
extern uint64 g_uwpTextureChangeTicks;
extern uint64 g_uwpTextureReloadTicks;
extern uint64 g_uwpTextureReloadCount;
extern uint64 g_uwpTexHashTicks[6];
extern uint64 g_uwpTexHashCount[6];
extern uint64 g_uwpTexHashBytes[6];
extern uint64 g_uwpTextureLookupMemoHits;
extern uint64 g_uwpTextureLookupMemoMisses;
extern uint64 g_uwpOutputCopyNs;
extern uint64 g_uwpOutputOverlayNs;
extern uint64 g_uwpOutputCopies;

namespace
{
	// render thread cost of putting a frame on screen, to decide if a separate present thread pays off
	struct UwpOutputCost
	{
		uint64 intervalStartMs = 0;
		uint64 swaps = 0;
		uint64 swapNs = 0;
		uint64 drainNs = 0;
		uint64 submitNs = 0;
		uint64 presentNs = 0;
		uint64 maxSwapNs = 0;
	} s_uwpOutputCost;

	// draw_beginSequence split on the Latte thread (Kakariko: ~2.8 us per sequence, 17 % of the thread)
	enum UwpSeqPart : uint32 { SeqQueue, SeqShaders, SeqFbo, SeqTextures, SeqApply, SeqPartCount };
	uint64 s_uwpSeqTicks[SeqPartCount]{};
	uint64 s_uwpSeqCount = 0;
	uint64 s_uwpSeqTextureLoops = 0;

	uint64 UwpElapsedNs(std::chrono::steady_clock::time_point since)
	{
		return (uint64)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - since).count();
	}

	void LogUwpOutputCost(uint64 swapNs, uint64 drainNs, uint64 submitNs, uint64 presentNs)
	{
		auto& c = s_uwpOutputCost;
		const uint64 nowMs = GetTickCount64();
		if (c.intervalStartMs == 0)
			c.intervalStartMs = nowMs;
		++c.swaps;
		c.swapNs += swapNs;
		c.drainNs += drainNs;
		c.submitNs += submitNs;
		c.presentNs += presentNs;
		c.maxSwapNs = std::max(c.maxSwapNs, swapNs);
		const uint64 intervalMs = nowMs - c.intervalStartMs;
		if (intervalMs < 5000)
			return;
		if (g_uwpDetailedProfiling && CafeSystem::IsTitleRunning() && s_uwpSeqCount != 0)
		{
			const double seqs = (double)s_uwpSeqCount;
			auto us = [&](uint32 part) { return (double)PPCTimer_tscToMicroseconds(s_uwpSeqTicks[part]) / seqs; };
			auto ms = [&](uint32 part) { return (double)PPCTimer_tscToMicroseconds(s_uwpSeqTicks[part]) / 1000.0; };
			cemuLog_log(LogType::Force,
				"UWP sequence begin split: {} sequences, texture passes {}; us per sequence (ms total) queue {:.2f} ({:.0f}), shaders {:.2f} ({:.0f}), fbo {:.2f} ({:.0f}), textures {:.2f} ({:.0f}), apply+viewport+scissor {:.2f} ({:.0f})",
				s_uwpSeqCount, s_uwpSeqTextureLoops, us(SeqQueue), ms(SeqQueue), us(SeqShaders), ms(SeqShaders), us(SeqFbo), ms(SeqFbo),
				us(SeqTextures), ms(SeqTextures), us(SeqApply), ms(SeqApply));
			cemuLog_log(LogType::Force, "UWP texture units: view lookup {:.0f} ms (memo hits {}, lookups {}), change check+reload+bookkeeping {:.0f} ms",
				(double)PPCTimer_tscToMicroseconds(g_uwpTextureLookupTicks) / 1000.0, g_uwpTextureLookupMemoHits, g_uwpTextureLookupMemoMisses,
				(double)PPCTimer_tscToMicroseconds(g_uwpTextureChangeTicks) / 1000.0);
			static const char* kHashPaths[6] = { "light", "tiny", "small", "medium", "huge", "r11g11b10/empty" };
			std::string hashText;
			for (uint32 i = 0; i < 6; ++i)
				if (g_uwpTexHashCount[i])
					hashText += fmt::format(" {} {:.1f} ms ({}x, {} MB)", kHashPaths[i], (double)PPCTimer_tscToMicroseconds(g_uwpTexHashTicks[i]) / 1000.0,
						g_uwpTexHashCount[i], g_uwpTexHashBytes[i] >> 20);
			cemuLog_log(LogType::Force, "UWP texture change: reloads {:.0f} ms ({}x); hashes:{}",
				(double)PPCTimer_tscToMicroseconds(g_uwpTextureReloadTicks) / 1000.0, g_uwpTextureReloadCount, hashText);
		}
		g_uwpTextureReloadTicks = 0;
		g_uwpTextureReloadCount = 0;
		std::fill(g_uwpTexHashTicks, g_uwpTexHashTicks + 6, 0);
		std::fill(g_uwpTexHashCount, g_uwpTexHashCount + 6, 0);
		std::fill(g_uwpTexHashBytes, g_uwpTexHashBytes + 6, 0);
		g_uwpTextureLookupTicks = 0;
		g_uwpTextureChangeTicks = 0;
		g_uwpTextureLookupMemoHits = 0;
		g_uwpTextureLookupMemoMisses = 0;
		if (CafeSystem::IsTitleRunning())
		{
			LatteIndices_UwpLogStats();
			auto& r = s_uwpRecorderSplit;
			const uint64 draws = r.draws.exchange(0);
			auto take = [](std::atomic<uint64>& v) { return (double)PPCTimer_tscToMicroseconds(v.exchange(0)) / 1000.0; };
			const double total = take(r.total), apply = take(r.apply), pso = take(r.pso), feedback = take(r.feedback),
				tablePrep = take(r.tablePrep), tables = take(r.tables), transitions = take(r.tableTransitions),
				uniformCommit = take(r.tableUniformCommit), emit = take(r.emit);
			const double prepare = apply + pso + tablePrep + (tables - transitions - uniformCommit);
			const double emitSide = feedback + transitions + uniformCommit + emit;
			if (s_uwpHeadDraws != 0)
			{
				auto hms = [](uint64 ticks) { return (double)PPCTimer_tscToMicroseconds(ticks) / 1000.0; };
				const double hd = (double)s_uwpHeadDraws;
				auto hus = [&](uint64 ticks) { return hms(ticks) * 1000.0 / hd; };
				cemuLog_log(LogType::Force,
					"UWP draw head split: {} draws; us per draw (ms total) shaders+misc {:.2f} ({:.0f}), pso identity {:.2f} ({:.0f}), state/stencil {:.2f} ({:.0f}), uniform prepare {:.2f} ({:.0f}), register delta {:.2f} ({:.0f}), closure {:.2f} ({:.0f}, {} bytes), delta write {:.2f} ({:.0f}), push {:.2f} ({:.0f}), post (rt tracking, readbacks) {:.2f} ({:.0f})",
					s_uwpHeadDraws, hus(s_uwpHeadTicks[HeadMisc]), hms(s_uwpHeadTicks[HeadMisc]), hus(s_uwpHeadTicks[HeadIdentity]), hms(s_uwpHeadTicks[HeadIdentity]),
					hus(s_uwpHeadTicks[HeadState]), hms(s_uwpHeadTicks[HeadState]), hus(s_uwpHeadTicks[HeadUniformPrepare]), hms(s_uwpHeadTicks[HeadUniformPrepare]),
					hus(s_uwpHeadTicks[HeadCollect]), hms(s_uwpHeadTicks[HeadCollect]),
					hus(s_uwpHeadTicks[HeadClosure]), hms(s_uwpHeadTicks[HeadClosure]), s_uwpDrawClosureBytes,
					hus(s_uwpHeadTicks[HeadDeltaWrite]), hms(s_uwpHeadTicks[HeadDeltaWrite]),
					hus(s_uwpHeadTicks[HeadQueue] - s_uwpHeadTicks[HeadDeltaWrite]), hms(s_uwpHeadTicks[HeadQueue] - s_uwpHeadTicks[HeadDeltaWrite]),
					hus(s_uwpHeadTicks[HeadPost]), hms(s_uwpHeadTicks[HeadPost]));
				cemuLog_log(LogType::Force,
					"UWP buffer sync split: {} calls {:.0f} ms; queues {:.0f} ms, vertex retrieve {:.0f} ms ({} buffers, {} MB), vertex bind {:.0f} ms, uniform retrieve {:.0f} ms ({} buffers, {} MB), uniform bind {:.0f} ms",
					g_uwpBufSyncCounts[0], hms(g_uwpBufSyncTicks[5]), hms(g_uwpBufSyncTicks[0]), hms(g_uwpBufSyncTicks[1]), g_uwpBufSyncCounts[1],
					g_uwpBufSyncBytes[0] >> 20, hms(g_uwpBufSyncTicks[2]), hms(g_uwpBufSyncTicks[3]), g_uwpBufSyncCounts[2], g_uwpBufSyncBytes[1] >> 20,
					hms(g_uwpBufSyncTicks[4]));
			}
			if (s_uwpDeltaDraws != 0)
			{
				uint64 chunks = 0;
				std::string pages;
				for (uint32 i = 0; i < 16; ++i)
				{
					chunks += s_uwpDeltaChunksByPage[i];
					if (s_uwpDeltaChunksByPage[i])
						pages += fmt::format(" 0x{:X}000: {} ({:.2f}/draw)", i, s_uwpDeltaChunksByPage[i], (double)s_uwpDeltaChunksByPage[i] / (double)s_uwpDeltaDraws);
				}
				cemuLog_log(LogType::Force, "UWP register delta: {} draws, {} with no change, {} full; {} chunks of {} registers ({:.2f} per draw, {} KB); by page:{}",
					s_uwpDeltaDraws, s_uwpDeltaEmpty, s_uwpDeltaFull, chunks, LATTE_REGISTER_CHUNK_WORDS, (double)chunks / (double)s_uwpDeltaDraws, chunks * kRecordChunkBytes / 1024, pages);
			}
			s_uwpDeltaDraws = 0;
			s_uwpDeltaEmpty = 0;
			s_uwpDeltaFull = 0;
			std::fill(std::begin(s_uwpDeltaChunksByPage), std::end(s_uwpDeltaChunksByPage), 0);
			std::fill(std::begin(s_uwpHeadTicks), std::end(s_uwpHeadTicks), 0);
			s_uwpHeadDraws = 0;
			std::fill(g_uwpBufSyncTicks, g_uwpBufSyncTicks + 6, 0);
			std::fill(g_uwpBufSyncCounts, g_uwpBufSyncCounts + 3, 0);
			std::fill(g_uwpBufSyncBytes, g_uwpBufSyncBytes + 2, 0);
			if (draws != 0)
				cemuLog_log(LogType::Force,
					"UWP recorder stage split (S0): {} draw tails {:.0f} ms ({:.2f} us each); prepare {:.0f} ms [apply {:.0f}, pso {:.0f}, table prep {:.0f}, tables {:.0f}], emit {:.0f} ms [feedback {:.0f}, table transitions {:.0f}, uniform commit {:.0f}, rest {:.0f}], unassigned {:.0f} ms",
					draws, total, total * 1000.0 / (double)draws, prepare, apply, pso, tablePrep, tables - transitions - uniformCommit,
					emitSide, feedback, transitions, uniformCommit, emit, total - prepare - emitSide);
		}
		std::fill(std::begin(s_uwpSeqTicks), std::end(s_uwpSeqTicks), 0);
		s_uwpSeqCount = 0;
		s_uwpSeqTextureLoops = 0;
		if (CafeSystem::IsTitleRunning())
		{
			const double n = (double)c.swaps;
			const double copyMs = (double)g_uwpOutputCopyNs / 1e6;
			const double overlayMs = (double)g_uwpOutputOverlayNs / 1e6;
			const double totalMs = copyMs + (double)c.swapNs / 1e6;
			cemuLog_log(LogType::Force,
				"UWP output cost: {} swaps, {} backbuffer copies in {} ms; per swap: copy {:.2f} ms (ImGui overlay {:.2f}), swap {:.2f} ms (recorder drain {:.2f}, submit {:.2f}, Present {:.2f}), max swap {:.2f} ms; render thread {:.0f} ms ({:.1f}%)",
				c.swaps, g_uwpOutputCopies, intervalMs, (copyMs - overlayMs) / n, overlayMs / n, (double)c.swapNs / 1e6 / n,
				(double)c.drainNs / 1e6 / n, (double)c.submitNs / 1e6 / n, (double)c.presentNs / 1e6 / n, (double)c.maxSwapNs / 1e6,
				totalMs, totalMs * 100.0 / (double)intervalMs);
		}
		c = {};
		c.intervalStartMs = nowMs;
		g_uwpOutputCopyNs = 0;
		g_uwpOutputOverlayNs = 0;
		g_uwpOutputCopies = 0;
	}
}

