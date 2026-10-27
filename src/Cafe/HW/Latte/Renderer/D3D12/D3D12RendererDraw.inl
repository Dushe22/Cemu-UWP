// Part of D3D12Renderer.cpp (included there, same translation unit): buffer cache, streamout, draw sequences, draw_execute, index data and occlusion query objects.
// Not a standalone file: it relies on the includes and file-local helpers of D3D12Renderer.cpp.

void D3D12Renderer::bufferCache_init(const sint32 bufferSize)
{
	D3D12_EXCLUSIVE_RECORDING();
	if (!m_memoryManager || bufferSize <= 0)
		return;
	WaitForIdle();
	m_bufferCache = {};
	if (D3D12ShouldKeepBufferShadow())
		m_bufferCacheShadow.assign(std::max<sint32>(bufferSize, 0), 0);
	else
		m_bufferCacheShadow.clear();
	m_streamoutBuffer = {};
	if (!m_memoryManager->CreateBuffer(bufferSize, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON, m_bufferCache))
		throw std::runtime_error("Failed to create D3D12 buffer cache");
	if (!m_memoryManager->CreateBuffer(kStreamoutBufferSize, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON, m_streamoutBuffer))
		throw std::runtime_error("Failed to create D3D12 streamout buffer");
	D3D12TraceEvent("buffer_cache_init", "size={} mainResource={} mainSize={} mainState=0x{:08x} streamoutResource={} streamoutSize={} streamoutState=0x{:08x} shadowBytes={}",
					bufferSize,
					(const void*)m_bufferCache.resource.Get(),
					(unsigned long long)m_bufferCache.size,
					(uint32)m_bufferCache.state,
					(const void*)m_streamoutBuffer.resource.Get(),
					(unsigned long long)m_streamoutBuffer.size,
					(uint32)m_streamoutBuffer.state,
					(unsigned long long)m_bufferCacheShadow.size());
}

void D3D12Renderer::bufferCache_upload(uint8* buffer, sint32 size, uint32 bufferOffset)
{
	if (ShouldQueueRecording())
	{
		if (!buffer || size <= 0)
			return;
		QueueRecording([this, data = std::vector<uint8>(buffer, buffer + size), bufferOffset]() mutable
		{
			bufferCache_upload(data.data(), (sint32)data.size(), bufferOffset);
		}, RecordOpKind::Command);
		return;
	}
	stdx::scope_exit deviceCheck([&]() { CheckDeviceAfterOperation("buffer upload", [&]() {
		return fmt::format("offset={} size={} bufferSize={}", bufferOffset, size, (uint64)m_bufferCache.size); }); });
	if (!m_bufferCache.resource || !buffer || size <= 0)
		return;
	++m_uwpBufferProfile.bufferUploads;
	m_uwpBufferProfile.bufferUploadBytes += (uint32)size;
	if (!m_commandListOpen)
		ResetCommandList();

	auto upload = m_memoryManager->AllocateUpload((uint32)size, 256, m_nextFenceValue);
	if (!upload.IsValid())
		throw std::runtime_error("Failed to allocate D3D12 buffer upload memory");
	memcpy(upload.cpuPtr, buffer, size);
	if ((size_t)bufferOffset + (size_t)size <= m_bufferCacheShadow.size())
		memcpy(m_bufferCacheShadow.data() + bufferOffset, buffer, size);

	uint32 firstBytes[4]{};
	if (size > 0)
		memcpy(firstBytes, buffer, std::min<size_t>((size_t)size, sizeof(firstBytes)));
	D3D12TraceEvent("buffer_cache_upload", "offset={} size={} mainResource={} stateBefore=0x{:08x} uploadResource={} uploadOffset={} uploadSize={} uploadBufferIndex={} firstBytes=0x{:08x},0x{:08x},0x{:08x},0x{:08x} shadowUpdated={}",
					bufferOffset,
					size,
					(const void*)m_bufferCache.resource.Get(),
					(uint32)m_bufferCache.state,
					(const void*)upload.resource,
					(unsigned long long)upload.offset,
					(unsigned long long)upload.size,
					upload.bufferIndex,
					firstBytes[0],
					firstBytes[1],
					firstBytes[2],
					firstBytes[3],
					((size_t)bufferOffset + (size_t)size <= m_bufferCacheShadow.size()) ? 1 : 0);
	Transition(m_bufferCache.resource.Get(), m_bufferCache.state, D3D12_RESOURCE_STATE_COPY_DEST);
	GetCurrentCommandSlot().commandList->CopyBufferRegion(m_bufferCache.resource.Get(), bufferOffset, upload.resource, upload.offset, (UINT64)size);
	Transition(m_bufferCache.resource.Get(), m_bufferCache.state, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER | D3D12_RESOURCE_STATE_INDEX_BUFFER | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}

void D3D12Renderer::bufferCache_copy(uint32 srcOffset, uint32 dstOffset, uint32 size)
{
	D3D12_QUEUE_RECORDING_KIND(Command, bufferCache_copy(srcOffset, dstOffset, size));
	stdx::scope_exit deviceCheck([&]() { CheckDeviceAfterOperation("buffer copy", [&]() {
		return fmt::format("src={} dst={} size={} bufferSize={}", srcOffset, dstOffset, size, (uint64)m_bufferCache.size); }); });
	if (!m_bufferCache.resource || size == 0)
		return;
	if (!m_commandListOpen)
		ResetCommandList();

	// Direct-queue ordering permits reuse of one staging buffer across calls.
	if (!m_bufferCacheCopyTemp.IsValid() || m_bufferCacheCopyTemp.size < size)
	{
		ReleaseResourceDeferred(m_bufferCacheCopyTemp);
		uint64 tempSize = std::max<uint64>(64u * 1024u, m_bufferCacheCopyTemp.size * 2);
		while (tempSize < size)
			tempSize *= 2;
		if (!m_memoryManager->CreateBuffer(tempSize, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, m_bufferCacheCopyTemp))
			throw std::runtime_error("Failed to create D3D12 temporary buffer-cache copy resource");
	}
	D3D12MemoryManager::Resource& tempBuffer = m_bufferCacheCopyTemp;

	D3D12TraceEvent("buffer_cache_copy", "srcOffset={} dstOffset={} size={} mainResource={} mainStateBefore=0x{:08x} tempResource={} tempSize={} shadowCopy={}",
					srcOffset,
					dstOffset,
					size,
					(const void*)m_bufferCache.resource.Get(),
					(uint32)m_bufferCache.state,
					(const void*)tempBuffer.resource.Get(),
					(unsigned long long)tempBuffer.size,
					((size_t)srcOffset + (size_t)size <= m_bufferCacheShadow.size() &&
					 (size_t)dstOffset + (size_t)size <= m_bufferCacheShadow.size()) ? 1 : 0);
	Transition(tempBuffer.resource.Get(), tempBuffer.state, D3D12_RESOURCE_STATE_COPY_DEST);
	Transition(m_bufferCache.resource.Get(), m_bufferCache.state, D3D12_RESOURCE_STATE_COPY_SOURCE);
	GetCurrentCommandSlot().commandList->CopyBufferRegion(tempBuffer.resource.Get(), 0, m_bufferCache.resource.Get(), srcOffset, size);
	Transition(tempBuffer.resource.Get(), tempBuffer.state, D3D12_RESOURCE_STATE_COPY_SOURCE);
	Transition(m_bufferCache.resource.Get(), m_bufferCache.state, D3D12_RESOURCE_STATE_COPY_DEST);
	GetCurrentCommandSlot().commandList->CopyBufferRegion(m_bufferCache.resource.Get(), dstOffset, tempBuffer.resource.Get(), 0, size);
	if ((size_t)srcOffset + (size_t)size <= m_bufferCacheShadow.size() &&
		(size_t)dstOffset + (size_t)size <= m_bufferCacheShadow.size())
	{
		memmove(m_bufferCacheShadow.data() + dstOffset, m_bufferCacheShadow.data() + srcOffset, size);
	}
	Transition(m_bufferCache.resource.Get(), m_bufferCache.state, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER | D3D12_RESOURCE_STATE_INDEX_BUFFER | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}

void D3D12Renderer::bufferCache_copyStreamoutToMainBuffer(uint32 srcOffset, uint32 dstOffset, uint32 size)
{
	D3D12_QUEUE_RECORDING_KIND(Command, bufferCache_copyStreamoutToMainBuffer(srcOffset, dstOffset, size));
	if (!m_streamoutBuffer.resource || !m_bufferCache.resource || size == 0)
		return;
	if (!m_commandListOpen)
		ResetCommandList();

	D3D12TraceEvent("streamout_copy_to_main", "srcOffset={} dstOffset={} size={} streamoutResource={} streamoutStateBefore=0x{:08x} mainResource={} mainStateBefore=0x{:08x}",
					srcOffset,
					dstOffset,
					size,
					(const void*)m_streamoutBuffer.resource.Get(),
					(uint32)m_streamoutBuffer.state,
					(const void*)m_bufferCache.resource.Get(),
					(uint32)m_bufferCache.state);
	Transition(m_streamoutBuffer.resource.Get(), m_streamoutBuffer.state, D3D12_RESOURCE_STATE_COPY_SOURCE);
	Transition(m_bufferCache.resource.Get(), m_bufferCache.state, D3D12_RESOURCE_STATE_COPY_DEST);
	GetCurrentCommandSlot().commandList->CopyBufferRegion(m_bufferCache.resource.Get(), dstOffset, m_streamoutBuffer.resource.Get(), srcOffset, size);
	Transition(m_bufferCache.resource.Get(), m_bufferCache.state, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER | D3D12_RESOURCE_STATE_INDEX_BUFFER | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}

void D3D12Renderer::buffer_bindVertexBuffers(std::span<BindBufferParam> bindings)
{
	if (ShouldQueueRecording())
	{
		QueueRecording([this, copy = std::vector<BindBufferParam>(bindings.begin(), bindings.end())]() mutable
		{
			buffer_bindVertexBuffers(copy);
		});
		return;
	}
	// vertex fetch reads the buffer cache through SRVs, so the stride is not needed here
	for (const auto& binding : bindings)
		buffer_bindVertexBuffer(binding.index, binding.bindOffset, binding.bindSize);
}

void D3D12Renderer::buffer_bindVertexBuffer(uint32 bufferIndex, uint32 offset, uint32 size)
{
	if (bufferIndex >= m_vertexBindings.size())
		return;
	m_vertexBindings[bufferIndex] = { offset, size, true };
	++m_vertexBindingGeneration;
	D3D12TraceEvent("vertex_buffer_bind", "bufferIndex={} offset={} size={} mainResource={} mainState=0x{:08x} shadowBytes={}",
					bufferIndex,
					offset,
					size,
					(const void*)m_bufferCache.resource.Get(),
					(uint32)m_bufferCache.state,
					(unsigned long long)m_bufferCacheShadow.size());
}

void D3D12Renderer::buffer_bindUniformBuffer(LatteConst::ShaderType shaderType, uint32 bufferIndex, uint32 offset, uint32 size)
{
	D3D12_QUEUE_RECORDING(buffer_bindUniformBuffer(shaderType, bufferIndex, offset, size));
	const size_t stageIndex = (size_t)shaderType;
	if (stageIndex >= m_uniformBindings.size() || bufferIndex >= m_uniformBindings[stageIndex].size())
		return;
	m_uniformBindings[stageIndex][bufferIndex] = { offset, size, true };
	++m_uniformBindingGeneration[stageIndex];
	D3D12TraceEvent("uniform_buffer_bind", "stage={} bufferIndex={} offset={} size={} mainResource={} mainState=0x{:08x}",
					(uint32)shaderType,
					bufferIndex,
					offset,
					size,
					(const void*)m_bufferCache.resource.Get(),
					(uint32)m_bufferCache.state);
}
RendererShader* D3D12Renderer::shader_create(RendererShader::ShaderType type, uint64 baseHash, uint64 auxHash, const std::string& source, bool isGameShader, bool isGfxPackSource)
{
	D3D12_EXCLUSIVE_RECORDING();
	static uint32 s_shaderCreateLogCount = 0;
	if (D3D12RuntimeLogCapped(s_shaderCreateLogCount, "shader_create", 160))
	{
		cemuLog_log(LogType::Force, "D3D12: create shader type={} hash={:016x}_{:016x} game={} gfxPack={} sourceBytes={}.",
					(uint32)type, baseHash, auxHash, isGameShader, isGfxPackSource, source.size());
	}
	D3D12TraceEvent("shader_create", "type={} baseHash=0x{:016x} auxHash=0x{:016x} game={} gfxPack={} sourceBytes={}",
					(uint32)type,
					baseHash,
					auxHash,
					isGameShader ? 1 : 0,
					isGfxPackSource ? 1 : 0,
					source.size());
	D3D12DumpHLSL(type, baseHash, auxHash, isGameShader, source);
	return new RendererShaderD3D12(type, baseHash, auxHash, isGameShader, isGfxPackSource, source);
}
void D3D12Renderer::streamout_setupXfbBuffer(uint32 bufferIndex, sint32 ringBufferOffset, uint32 rangeAddr, uint32 rangeSize)
{
	if (!t_recordContext && bufferIndex < m_latteStreamoutRingOffsets.size())
		m_latteStreamoutRingOffsets[bufferIndex] = ringBufferOffset;
	D3D12_QUEUE_RECORDING(streamout_setupXfbBuffer(bufferIndex, ringBufferOffset, rangeAddr, rangeSize));
	if (bufferIndex >= m_streamoutBindings.size())
		return;
	m_streamoutBindings[bufferIndex] = { true, ringBufferOffset, rangeAddr, rangeSize };
	D3D12TraceEvent("streamout_bind", "bufferIndex={} ringOffset={} rangeAddr=0x{:08x} rangeSize={} streamoutResource={} streamoutState=0x{:08x}",
					bufferIndex,
					ringBufferOffset,
					rangeAddr,
					rangeSize,
					(const void*)m_streamoutBuffer.resource.Get(),
					(uint32)m_streamoutBuffer.state);
}

void D3D12Renderer::streamout_begin()
{
	D3D12_QUEUE_RECORDING_KIND(Command, streamout_begin());
	if (!m_commandListOpen)
		ResetCommandList();
}

void D3D12Renderer::streamout_rendererFinishDrawcall()
{
	D3D12_QUEUE_RECORDING(streamout_rendererFinishDrawcall());
	for (auto& binding : m_streamoutBindings)
		binding.enabled = false;
}

void D3D12Renderer::draw_beginSequence()
{
	if (m_threadedRecordingWanted && !t_recordContext && !m_recordThreadRunning.load(std::memory_order_relaxed) && CafeSystem::IsTitleRunning())
		StartRecordingThread();
#if defined(LATTE_UWP_DETAILED_PROFILING)
	++m_uwpSequenceProfileCalls;
	m_uwpSequenceProfileRandomState ^= m_uwpSequenceProfileRandomState << 13;
	m_uwpSequenceProfileRandomState ^= m_uwpSequenceProfileRandomState >> 17;
	m_uwpSequenceProfileRandomState ^= m_uwpSequenceProfileRandomState << 5;
	const bool sampleSequence = (m_uwpSequenceProfileRandomState & 63u) == 0;
	std::chrono::steady_clock::time_point sequenceStarted{};
	if (sampleSequence)
		sequenceStarted = std::chrono::steady_clock::now();
	stdx::scope_exit sequenceTimer([&]()
	{
		if (!sampleSequence)
			return;
		++m_uwpSequenceProfileSamples;
		m_uwpSequenceProfileTimeNs += (uint64)std::chrono::duration_cast<std::chrono::nanoseconds>(
			std::chrono::steady_clock::now() - sequenceStarted).count();
	});
#endif
	// recorder half: a texture whose slices are still being written must not be sampled, detached objects are freed,
	// and a command list is open for the draws
	auto beginRecording = [this]()
	{
		texture_finishSliceUploads();
		FlushPendingDestroys();
		if (!m_commandListOpen)
			ResetCommandList();
	};
	++s_uwpSeqCount;
	uint64 seqTick = UwpProfTick();
	auto seqLap = [&seqTick](uint32 part) { const uint64 now = UwpProfTick(); s_uwpSeqTicks[part] += now - seqTick; seqTick = now; };
	if (ShouldQueueRecording())
		QueueRecording(beginRecording, RecordOpKind::Sync);
	else
		beginRecording();
	m_drawSequenceActive = true;
	m_drawSequenceSkip = false;

	const bool streamoutEnable = LatteGPUState.contextRegister[mmVGT_STRMOUT_EN] != 0;
	seqLap(SeqQueue);

	LatteSHRC_UpdateActiveShaders();
	seqLap(SeqShaders);
	if (LatteGPUState.activeShaderHasError)
	{
#if defined(CEMU_UWP_D3D12_CORRECTNESS_TELEMETRY)
		++m_correctnessSequenceShaderError;
#endif
		static uint32 s_shaderErrorLogCount = 0;
		if (D3D12RuntimeLogCapped(s_shaderErrorLogCount, "draw_beginSequence shader error", 16))
			cemuLog_log(LogType::Force, "D3D12: skipping draw sequence due to active shader error.");
		m_drawSequenceSkip = true;
		cemu_assert_debug(false);
		return;
	}

	LatteGPUState.requiresTextureBarrier = false;
	while (true)
	{
		LatteGPUState.repeatTextureInitialization = false;
		const bool fboUpdated = LatteMRT::UpdateCurrentFBO();
		seqLap(SeqFbo);
		if (!fboUpdated)
		{
#if defined(CEMU_UWP_D3D12_CORRECTNESS_TELEMETRY)
			++m_correctnessSequenceInvalidFbo;
#endif
			static uint32 s_invalidRenderTargetLogCount = 0;
			if (D3D12RuntimeLogCapped(s_invalidRenderTargetLogCount, "draw_beginSequence invalid render target", 32))
				cemuLog_log(LogType::Force, "D3D12: skipping draw sequence because current FBO update failed.");
			m_drawSequenceSkip = true;
			return;
		}

		if (!hasValidFramebufferAttached && !streamoutEnable)
		{
#if defined(CEMU_UWP_D3D12_CORRECTNESS_TELEMETRY)
			++m_correctnessSequenceMissingAttachment;
#endif
			static uint32 s_missingAttachmentLogCount = 0;
			if (D3D12RuntimeLogCapped(s_missingAttachmentLogCount, "draw_beginSequence missing attachments", 32))
				cemuLog_log(LogType::Force, "D3D12: skipping draw sequence because no color/depth framebuffer is attached.");
			m_drawSequenceSkip = true;
			return;
		}

		LatteTexture_updateTextures();
		++s_uwpSeqTextureLoops;
		seqLap(SeqTextures);
		if (!LatteGPUState.repeatTextureInitialization)
			break;
	}

	LatteMRT::ApplyCurrentState();
	LatteRenderTarget_updateViewport();
	LatteRenderTarget_updateScissorBox();
	seqLap(SeqApply);

	bool rasterizerEnable = LatteGPUState.contextNew.PA_CL_CLIP_CNTL.get_DX_RASTERIZATION_KILL() == false;
	if (!LatteGPUState.contextNew.PA_CL_VTE_CNTL.get_VPORT_X_OFFSET_ENA())
		rasterizerEnable = true;
	if (!rasterizerEnable && !streamoutEnable)
	{
#if defined(CEMU_UWP_D3D12_CORRECTNESS_TELEMETRY)
		++m_correctnessSequenceRasterDisabled;
#endif
		static uint32 s_rasterizerDisabledLogCount = 0;
		if (D3D12RuntimeLogCapped(s_rasterizerDisabledLogCount, "draw_beginSequence rasterizer disabled", 32))
			cemuLog_log(LogType::Force, "D3D12: skipping draw sequence because rasterization is disabled and streamout is off.");
		m_drawSequenceSkip = true;
	}
}

void D3D12Renderer::draw_execute(uint32 baseVertex, uint32 baseInstance, uint32 instanceCount, uint32 count, MPTR indexDataMPTR, Latte::LATTE_VGT_DMA_INDEX_TYPE::E_INDEX_TYPE indexType, const LatteDrawcallContext& drawcallContext)
{
	stdx::scope_exit drawAccounting([&]() { LatteGPUState.drawCallCounter++; });
	stdx::scope_exit deviceCheck([&]() { CheckDeviceAfterOperation("draw", [&]() {
		const auto* vertexShader = LatteSHRC_GetActiveVertexShader();
		const auto* geometryShader = LatteSHRC_GetActiveGeometryShader();
		const auto* pixelShader = LatteSHRC_GetActivePixelShader();
		return fmt::format("vs={:016x} gs={:016x} ps={:016x} count={} instances={} indexType={} fbo=0x{:016x}",
			vertexShader ? vertexShader->baseHash : 0, geometryShader ? geometryShader->baseHash : 0, pixelShader ? pixelShader->baseHash : 0,
			count, instanceCount, (uint32)indexType, m_latteActiveFBO ? m_latteActiveFBO->key : 0); }); });
#if defined(LATTE_UWP_DETAILED_PROFILING)
	++m_uwpCpuProfileDraws;
	uint32 profileRandom = m_uwpCpuProfileRandomState;
	profileRandom ^= profileRandom << 13;
	profileRandom ^= profileRandom >> 17;
	profileRandom ^= profileRandom << 5;
	m_uwpCpuProfileRandomState = profileRandom;
	const bool profileThisDraw = (profileRandom & 63u) == 0;
	UwpCpuDrawStage profileStage = UwpCpuDrawStage::Setup;
	std::chrono::steady_clock::time_point profileStageStarted{};
	if (profileThisDraw)
	{
		++m_uwpCpuProfileSampledDraws;
		profileStageStarted = std::chrono::steady_clock::now();
	}
	bool profileHeadDone = false;
	auto finishProfileStage = [&]()
	{
		if (!profileThisDraw || profileHeadDone)
			return;
		const auto now = std::chrono::steady_clock::now();
		const size_t stageIndex = (size_t)profileStage;
		const uint64 elapsedNs = (uint64)std::chrono::duration_cast<std::chrono::nanoseconds>(now - profileStageStarted).count();
		m_uwpCpuProfileStageTimeNs[stageIndex] += elapsedNs;
		++m_uwpCpuProfileStageSamples[stageIndex];
		profileStageStarted = now;
	};
	auto switchProfileStage = [&](UwpCpuDrawStage nextStage)
	{
		if (!profileThisDraw)
			return;
		finishProfileStage();
		profileStage = nextStage;
	};
	stdx::scope_exit profileFinalizer([&]() { finishProfileStage(); });
#else
	auto switchProfileStage = [](UwpCpuDrawStage) {};
#endif
#if defined(CEMU_UWP_D3D12_CORRECTNESS_TELEMETRY)
	++m_correctnessDrawAttempts;
#endif
	if (m_drawSequenceSkip)
	{
#if defined(CEMU_UWP_D3D12_CORRECTNESS_TELEMETRY)
		++m_correctnessDrawSkippedSequence;
#endif
		++m_droppedDraws[(size_t)DroppedDrawReason::SequenceRejected];
		return;
	}

	if (LatteGPUState.contextNew.GetSpecialStateValues()[8] != 0)
	{
#if defined(CEMU_UWP_D3D12_CORRECTNESS_TELEMETRY)
		++m_correctnessSpecialStateDepthClear;
#endif
		D3D12_EXCLUSIVE_RECORDING();
		LatteDraw_handleSpecialState8_clearAsDepth();
		return;
	}
	else if (LatteGPUState.contextNew.GetSpecialStateValues()[5] != 0)
	{
#if defined(CEMU_UWP_D3D12_CORRECTNESS_TELEMETRY)
		++m_correctnessSpecialStateDepthCopy;
#endif
		D3D12_EXCLUSIVE_RECORDING();
		DrawHandleSpecialState5();
		return;
	}

	// draw number for Latte-side traces; the recording side numbers draws itself and recycles their scratch copies
	const uint64 nextDrawId = (uint64)LatteGPUState.drawCallCounter + 1;

	const LattePrimitiveMode primitiveMode = LatteGPUState.contextNew.VGT_PRIMITIVE_TYPE.get_PRIMITIVE_MODE();
#if defined(CEMU_UWP_D3D12_CORRECTNESS_TELEMETRY)
	const uint32 primitiveValue = (uint32)primitiveMode;
	if (primitiveValue < 64)
		m_correctnessPrimitiveMask |= 1ull << primitiveValue;
#endif
	const D3D_PRIMITIVE_TOPOLOGY d3dPrimitiveTopology = D3D12PipelineCompiler::GetPrimitiveTopology(primitiveMode);
	const auto& polygonControlReg = LatteGPUState.contextNew.PA_SU_SC_MODE_CNTL;
	const auto cullState = GetD3D12EffectiveCullState(primitiveMode, polygonControlReg);
	const bool cullAll = cullState.cullFront && cullState.cullBack;
	const bool streamoutEnable = LatteGPUState.contextRegister[mmVGT_STRMOUT_EN] != 0;
	bool rasterizerEnable = LatteGPUState.contextNew.PA_CL_CLIP_CNTL.get_DX_RASTERIZATION_KILL() == false;
	if (!LatteGPUState.contextNew.PA_CL_VTE_CNTL.get_VPORT_X_OFFSET_ENA())
		rasterizerEnable = true;
	// the command processor clears the dirty masks after every draw, synced or not
	const auto deferBufferSync = [&]()
	{
		if (drawcallContext.isFirst || drawcallContext.vertexBufferDirtyMask != 0 ||
			drawcallContext.vsUniformBufferDirtyMask != 0 || drawcallContext.psUniformBufferDirtyMask != 0 ||
			drawcallContext.gsUniformBufferDirtyMask != 0)
			m_drawNeedsFullBufferSync = true;
	};
	if (cullAll && !streamoutEnable && D3D12PrimitiveUsesFaceCulling(primitiveMode))
	{
		static uint32 s_cullAllSkipLogCount = 0;
		if (D3D12RuntimeLogCapped(s_cullAllSkipLogCount, "front-and-back cull skip", 32))
		{
			cemuLog_log(LogType::Force, "D3D12: skipping draw because front-and-back face culling rejects primitive={} and streamout is off.",
						(uint32)primitiveMode);
		}
		m_cullAllDrawsSkipped++;
		deferBufferSync();
		return;
	}
	const bool noRasterizationOutput = streamoutEnable &&
		((!rasterizerEnable) || (cullAll && D3D12PrimitiveUsesFaceCulling(primitiveMode)));
	if (noRasterizationOutput)
	{
		static uint32 s_noRasterStreamoutLogCount = 0;
		if (D3D12RuntimeLogCapped(s_noRasterStreamoutLogCount, "no-raster streamout", 32))
		{
			cemuLog_log(LogType::Force, "D3D12: using no-output PSO for streamout draw primitive={} rasterizerEnable={} cullAll={}.",
						(uint32)primitiveMode,
						rasterizerEnable,
						cullAll);
		}
	}

	// an active logic op may need destination copies, which use recording state: such a draw takes exclusive access
	// and its tail runs right here. Plain COPY (the common case) decides "no emulation" without touching that state.
	std::optional<RecordingExclusiveScope> logicOpExclusive;
	uint32 drawLogicOpMode = 0;
	bool drawDisableNativeLogicOp = false;
	if (!noRasterizationOutput)
	{
		D3D12_LOGIC_OP drawLogicOp = D3D12_LOGIC_OP_COPY;
		const bool knownLogicOp = TryGetD3D12LogicOp(LatteGPUState.contextNew.CB_COLOR_CONTROL.get_ROP(), drawLogicOp);
		if (!knownLogicOp || drawLogicOp != D3D12_LOGIC_OP_COPY)
		{
			logicOpExclusive.emplace(this);
			// destination copies are recorded into the current command list
			if (!m_commandListOpen)
				ResetCommandList();
			if (!PrepareLogicOpEmulationForDraw())
			{
#if defined(CEMU_UWP_D3D12_CORRECTNESS_TELEMETRY)
				++m_correctnessDrawSkippedLogicOp;
#endif
				++m_droppedDraws[(size_t)DroppedDrawReason::LogicOpUnsupported];
				deferBufferSync();
				return;
			}
			drawLogicOpMode = m_logicOpEmulationMode;
			drawDisableNativeLogicOp = m_disableNativeLogicOpForDraw;
		}
	}
	switchProfileStage(UwpCpuDrawStage::IndexDecode);
	Renderer::INDEX_TYPE hostIndexType{};
	uint32 hostIndexCount{};
	uint32 indexMin{};
	uint32 indexMax{};
	Renderer::IndexAllocation indexAllocation;
	uint32 vertexIdRemapMode = 0;
	LatteIndices_decode(memory_getPointerFromVirtualOffset(indexDataMPTR), indexType, count, primitiveMode, indexMax, hostIndexType, hostIndexCount, indexAllocation);
	if ((D3D12ShouldTrace() || D3D12ShouldChecksumOutput()) && indexAllocation.mem && hostIndexCount != 0)
	{
		const bool primitiveRestartEnabled = GetD3D12PrimitiveRestartEnabled(LatteGPUState.contextNew, primitiveMode, hostIndexType);
		const uint32 hostRestartIndex = hostIndexType == Renderer::INDEX_TYPE::U16 ?
			std::numeric_limits<uint16>::max() : std::numeric_limits<uint32>::max();
		indexMin = std::numeric_limits<uint32>::max();
		if (hostIndexType == Renderer::INDEX_TYPE::U16)
		{
			const auto* indices = static_cast<const uint16*>(indexAllocation.mem);
			for (uint32 i = 0; i < hostIndexCount; ++i)
			{
				if (!primitiveRestartEnabled || indices[i] != static_cast<uint16>(hostRestartIndex))
					indexMin = std::min<uint32>(indexMin, indices[i]);
			}
		}
		else if (hostIndexType == Renderer::INDEX_TYPE::U32)
		{
			const auto* indices = static_cast<const uint32*>(indexAllocation.mem);
			for (uint32 i = 0; i < hostIndexCount; ++i)
			{
				if (!primitiveRestartEnabled || indices[i] != hostRestartIndex)
					indexMin = std::min(indexMin, indices[i]);
			}
		}
		if (indexMin == std::numeric_limits<uint32>::max())
			indexMin = 0;
	}
	switchProfileStage(UwpCpuDrawStage::BufferSync);
	uint8 stageUniformModifiedMask = 0;
	if (drawcallContext.isFirst || m_drawNeedsFullBufferSync)
	{
		m_drawNeedsFullBufferSync = false;
		LatteBufferCache_Sync(indexMax + baseVertex, baseInstance, instanceCount,
			0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, stageUniformModifiedMask);
	}
	else
	{
		LatteBufferCache_Sync(indexMax + baseVertex, baseInstance, instanceCount,
			drawcallContext.vertexBufferDirtyMask,
			drawcallContext.vsUniformBufferDirtyMask,
			drawcallContext.psUniformBufferDirtyMask,
			drawcallContext.gsUniformBufferDirtyMask,
			stageUniformModifiedMask, true);
	}

	switchProfileStage(UwpCpuDrawStage::PipelineLookup);
	++s_uwpHeadDraws;
	uint64 uwpHeadMark = UwpProfTick();
	auto uwpHeadLap = [&uwpHeadMark](uint32 part) { const uint64 now = UwpProfTick(); s_uwpHeadTicks[part] += now - uwpHeadMark; uwpHeadMark = now; };
	auto* vertexShader = LatteSHRC_GetActiveVertexShader();
	auto* geometryShader = LatteSHRC_GetActiveGeometryShader();
	auto* pixelShader = LatteSHRC_GetActivePixelShader();
	auto* fetchShader = LatteSHRC_GetActiveFetchShader();
#if defined(CEMU_UWP_D3D12_CORRECTNESS_TELEMETRY)
	if (pixelShader && pixelShader->pixelColorOutputMask == 0)
		++m_correctnessColorlessPixelDraws;
	bool drawUsesDepthCompare = false;
	bool drawUsesUnsupportedDepthCompare = false;
	auto recordDepthCompareUsage = [&](const LatteDecompilerShader* shader)
	{
		if (!shader)
			return;
		for (uint32 relativeUnit = 0; relativeUnit < LATTE_NUM_MAX_TEX_UNITS; ++relativeUnit)
		{
			if (!shader->textureUsesDepthCompare[relativeUnit])
				continue;
			drawUsesDepthCompare = true;
			++m_correctnessDepthCompareUnitUses;
			const uint32 dimension = (uint32)shader->textureUnitDim[relativeUnit];
			if (dimension < 64)
				m_correctnessDepthCompareDimMask |= 1ull << dimension;
			if (shader->textureUnitDim[relativeUnit] == Latte::E_DIM::DIM_3D ||
				shader->textureUnitDim[relativeUnit] == Latte::E_DIM::DIM_CUBEMAP)
				drawUsesUnsupportedDepthCompare = true;
		}
	};
	recordDepthCompareUsage(vertexShader);
	recordDepthCompareUsage(geometryShader);
	recordDepthCompareUsage(pixelShader);
	if (drawUsesDepthCompare)
		++m_correctnessDepthCompareDraws;
	if (drawUsesUnsupportedDepthCompare)
		++m_correctnessUnsupportedDepthCompareDraws;

	if (fetchShader)
	{
		if (!fetchShader->bufferGroupsInvalid.empty())
			++m_correctnessInvalidFetchDraws;
		bool hasUnboundFetchBuffer = false;
		for (const auto& group : fetchShader->bufferGroups)
		{
			if (group.attributeBufferIndex >= m_vertexBindings.size() || !m_vertexBindings[group.attributeBufferIndex].valid)
				hasUnboundFetchBuffer = true;
			for (sint32 i = 0; i < group.attribCount; ++i)
			{
				const uint32 fetchFormat = (uint32)group.attrib[i].format;
				if (fetchFormat < 64)
					m_correctnessFetchFormatMask |= 1ull << fetchFormat;
				else
					++m_correctnessFetchFormatOverflow;
			}
		}
		if (hasUnboundFetchBuffer)
			++m_correctnessUnboundFetchDraws;
	}
#endif
	const uint32 rawRenderTargetMask = LatteGPUState.contextNew.CB_TARGET_MASK.get_MASK();
	const uint32 effectiveRenderTargetMask = GetD3D12EffectiveRenderTargetMask(LatteGPUState.contextNew);
	uwpHeadLap(HeadMisc);
	auto identity = D3D12PipelineCompiler::RecordCurrentState(m_latteActiveFBO, noRasterizationOutput);
	uwpHeadLap(HeadIdentity);
	if (identity.valid && drawLogicOpMode != 0)
		identity.pipelineStateHash = std::rotl(identity.pipelineStateHash, 11) ^ 0xD31210C0EAA17E0Dull;
	else if (identity.valid && drawDisableNativeLogicOp)
		identity.pipelineStateHash = std::rotl(identity.pipelineStateHash, 11) ^ 0xD31210C0D15AB1EDull;
	const uint64 tracePipelineHash = identity.valid ? identity.pipelineStateHash : 0;
#if defined(CEMU_UWP_D3D12_DIAGNOSTICS)
	const bool priorityStencilDraw = D3D12TraceSink::Get().BeginPriorityStencilDraw(nextDrawId, tracePipelineHash,
		!noRasterizationOutput && m_latteActiveFBO && m_latteActiveFBO->HasDepthStencilAttachment() &&
		m_latteActiveFBO->GetStencilFormat() != DXGI_FORMAT_UNKNOWN &&
		LatteGPUState.contextNew.DB_DEPTH_CONTROL.get_STENCIL_ENABLE());
	stdx::scope_exit priorityStencilFinalizer([]() { D3D12TraceSink::Get().EndPriorityStencilDraw(); });
	if (priorityStencilDraw)
	{
		D3D12TracePriorityUniformSources(vertexShader, nextDrawId, tracePipelineHash);
		D3D12TracePriorityUniformSources(geometryShader, nextDrawId, tracePipelineHash);
		D3D12TracePriorityUniformSources(pixelShader, nextDrawId, tracePipelineHash);
	}
#endif
	D3D12TraceEvent("draw_prepare", "draw={} pipeline=0x{:016x} latteDrawCounter={} submittedDrawNext={} baseVertex={} baseInstance={} instances={} count={} indexType={} primitive={} d3dTopology={} hostIndexType={} hostIndexCount={} indexMin={} indexMax={} fbo=0x{:016x} fetch={} vs=0x{:016x} gs=0x{:016x} ps=0x{:016x} noRaster={} streamout={} cullAll={} logicMode={} viewport={},{}:{}x{} depth={}..{} scissor={},{}-{},{} targetMask=0x{:08x} rawTargetMask=0x{:08x} colorSpecialOp={} blendMask=0x{:02x} zEnable={} zWrite={} zFunc={} stencilEnable={} stencilFuncF={} stencilFuncB={} frontFace={} cullFront={} cullBack={} effectiveCullFront={} effectiveCullBack={} zClipNearDisable={} zClipFarDisable={} rasterKill={} alphaTest={} alphaFunc={} alphaRefRaw=0x{:08x}",
					(unsigned long long)nextDrawId,
					(unsigned long long)tracePipelineHash,
					LatteGPUState.drawCallCounter,
					(unsigned long long)nextDrawId,
					baseVertex,
					baseInstance,
					instanceCount,
					count,
					(uint32)indexType,
					(uint32)primitiveMode,
					(uint32)d3dPrimitiveTopology,
					(uint32)hostIndexType,
					hostIndexCount,
					indexMin,
					indexMax,
					m_latteActiveFBO ? m_latteActiveFBO->key : 0,
					fetchShader ? 1 : 0,
					vertexShader ? vertexShader->baseHash : 0,
					geometryShader ? geometryShader->baseHash : 0,
					pixelShader ? pixelShader->baseHash : 0,
					noRasterizationOutput ? 1 : 0,
					streamoutEnable ? 1 : 0,
					cullAll ? 1 : 0,
					drawLogicOpMode,
					m_currentViewport.TopLeftX,
					m_currentViewport.TopLeftY,
					m_currentViewport.Width,
					m_currentViewport.Height,
					m_currentViewport.MinDepth,
					m_currentViewport.MaxDepth,
					m_currentScissor.left,
					m_currentScissor.top,
					m_currentScissor.right,
					m_currentScissor.bottom,
					effectiveRenderTargetMask,
					rawRenderTargetMask,
					(uint32)LatteGPUState.contextNew.CB_COLOR_CONTROL.get_SPECIAL_OP(),
					LatteGPUState.contextNew.CB_COLOR_CONTROL.get_BLEND_MASK(),
					LatteGPUState.contextNew.DB_DEPTH_CONTROL.get_Z_ENABLE() ? 1 : 0,
					LatteGPUState.contextNew.DB_DEPTH_CONTROL.get_Z_WRITE_ENABLE() ? 1 : 0,
					(uint32)LatteGPUState.contextNew.DB_DEPTH_CONTROL.get_Z_FUNC(),
					LatteGPUState.contextNew.DB_DEPTH_CONTROL.get_STENCIL_ENABLE() ? 1 : 0,
					(uint32)LatteGPUState.contextNew.DB_DEPTH_CONTROL.get_STENCIL_FUNC_F(),
					(uint32)LatteGPUState.contextNew.DB_DEPTH_CONTROL.get_STENCIL_FUNC_B(),
					(uint32)polygonControlReg.get_FRONT_FACE(),
					polygonControlReg.get_CULL_FRONT() ? 1 : 0,
					polygonControlReg.get_CULL_BACK() ? 1 : 0,
					cullState.cullFront ? 1 : 0,
					cullState.cullBack ? 1 : 0,
					LatteGPUState.contextNew.PA_CL_CLIP_CNTL.get_ZCLIP_NEAR_DISABLE() ? 1 : 0,
					LatteGPUState.contextNew.PA_CL_CLIP_CNTL.get_ZCLIP_FAR_DISABLE() ? 1 : 0,
					LatteGPUState.contextNew.PA_CL_CLIP_CNTL.get_DX_RASTERIZATION_KILL() ? 1 : 0,
					LatteGPUState.contextNew.SX_ALPHA_TEST_CONTROL.get_ALPHA_TEST_ENABLE() ? 1 : 0,
					(uint32)LatteGPUState.contextNew.SX_ALPHA_TEST_CONTROL.get_ALPHA_FUNC(),
					LatteGPUState.contextNew.SX_ALPHA_REF.getRawValue());
	TraceShaderTextureBindings(nextDrawId, tracePipelineHash, vertexShader, geometryShader, pixelShader);
	TraceVertexFetchBindings(nextDrawId, tracePipelineHash, fetchShader);
	LogVertexBufferPreview(nextDrawId, tracePipelineHash, vertexShader, pixelShader, primitiveMode, count, baseVertex, baseInstance, indexMin, indexMax, hostIndexType, hostIndexCount);
	if (fetchShader)
	{
		static uint32 s_fetchShaderDetailLogCount = 0;
		if (D3D12RuntimeLogCapped(s_fetchShaderDetailLogCount, "fetch shader details", 48))
		{
			cemuLog_log(LogType::Force, "D3D12: fetch shader has {} valid buffer groups and {} invalid groups.",
						(uint32)fetchShader->bufferGroups.size(),
						(uint32)fetchShader->bufferGroupsInvalid.size());
			for (auto& group : fetchShader->bufferGroups)
			{
				const uint32 stride = group.getCurrentBufferStride(LatteGPUState.contextRegister);
				const auto binding = group.attributeBufferIndex < m_vertexBindings.size() ? m_vertexBindings[group.attributeBufferIndex] : BoundRange{};
				cemuLog_log(LogType::Force, "D3D12:   vb{} stride={} cacheOffset={} cacheSize={} valid={} offsetLowBits={} attrCount={} range=[{},{}].",
							group.attributeBufferIndex,
							stride,
							binding.offset,
							binding.size,
							binding.valid,
							binding.offset & 3,
							group.attribCount,
							group.minOffset,
							group.totalAttribRangeSize);
				for (sint32 i = 0; i < group.attribCount; ++i)
				{
					const auto& attr = group.attrib[i];
					cemuLog_log(LogType::Force, "D3D12:     attr semantic={} fmt=0x{:02x} fetch={} nfa={} signed={} endian={} divisor={} offset={} ds={},{},{},{}.",
								attr.semanticId,
								(uint32)attr.format,
								D3D12FetchTypeName(attr.fetchType),
								attr.nfa,
								attr.isSigned,
								D3D12EndianName(attr.endianSwap),
								attr.aluDivisor,
								attr.offset,
								attr.ds[0],
								attr.ds[1],
								attr.ds[2],
								attr.ds[3]);
				}
			}
			for (auto& group : fetchShader->bufferGroupsInvalid)
			{
				cemuLog_log(LogType::Force, "D3D12:   invalid-vb{} attrCount={} range=[{},{}].",
							group.attributeBufferIndex,
							group.attribCount,
							group.minOffset,
							group.totalAttribRangeSize);
			}
		}
	}
	static uint32 s_drawExecuteLogCount = 0;
	if (D3D12RuntimeLogCapped(s_drawExecuteLogCount, "draw_execute", 192))
	{
		cemuLog_log(LogType::Force, "D3D12: draw_execute baseVertex={} baseInstance={} instances={} count={} indexType={} primitive={} fbo={} fetch={} vs={:016x} gs={:016x} ps={:016x} activeShaderHasError={}.",
					baseVertex,
					baseInstance,
					instanceCount,
					count,
					(uint32)indexType,
					(uint32)primitiveMode,
					m_latteActiveFBO ? "yes" : "no",
					fetchShader ? "yes" : "no",
					vertexShader ? vertexShader->baseHash : 0,
					geometryShader ? geometryShader->baseHash : 0,
					pixelShader ? pixelShader->baseHash : 0,
					LatteGPUState.activeShaderHasError);
	}
	if (drawLogicOpMode != 0 ||
		primitiveMode == Latte::LATTE_VGT_PRIMITIVE_TYPE::E_PRIMITIVE_TYPE::RECTS ||
		primitiveMode == Latte::LATTE_VGT_PRIMITIVE_TYPE::E_PRIMITIVE_TYPE::QUADS)
	{
		static uint32 s_specialDrawDetailLogCount = 0;
		if (D3D12RuntimeLogCapped(s_specialDrawDetailLogCount, "logic-op/rect draw detail", 192))
		{
			const auto& lcr = LatteGPUState.contextNew;
			const auto logicOp = lcr.CB_COLOR_CONTROL.get_ROP();
			const uint32 outputMask = pixelShader ? pixelShader->pixelColorOutputMask : 0;
			const uint32 targetMask = GetD3D12EffectiveRenderTargetMask(lcr);
			const uint32 blendMask = lcr.CB_COLOR_CONTROL.get_BLEND_MASK();
			const uint32 blend0Control = lcr.CB_BLENDN_CONTROL[0].getRawValue();
			uint32 rt0Addr = 0;
			uint32 rt0Width = 0;
			uint32 rt0Height = 0;
			uint32 rt0Format = 0;
			if (m_latteActiveFBO && !m_latteActiveFBO->GetColorAttachments().empty())
			{
				const auto& rt0 = m_latteActiveFBO->GetColorAttachments()[0];
				if (rt0.valid && rt0.view && rt0.view->baseTexture)
				{
					rt0Addr = rt0.view->baseTexture->physAddress;
					rt0Width = rt0.view->baseTexture->width;
					rt0Height = rt0.view->baseTexture->height;
					rt0Format = (uint32)rt0.view->baseTexture->format;
				}
			}
			cemuLog_log(LogType::Force,
						"D3D12: special draw primitive={} logicMode={} rop=0x{:02x} count={} hostCount={} instances={} baseVertex={} fbo={}x{} rt0=0x{:08x}/{}x{}/fmt=0x{:04x} viewport=({}, {}) {}x{} depth={}..{} scissor=({}, {})-({}, {}) targetMask=0x{:08x} blendMask=0x{:02x} blend0=0x{:08x} psOut=0x{:02x} vs={:016x} gs={:016x} ps={:016x}.",
						(uint32)primitiveMode,
						drawLogicOpMode,
						(uint32)logicOp,
						count,
						hostIndexCount,
						instanceCount,
						baseVertex,
						m_latteActiveFBO ? m_latteActiveFBO->m_size.x : 0,
						m_latteActiveFBO ? m_latteActiveFBO->m_size.y : 0,
						rt0Addr,
						rt0Width,
						rt0Height,
						rt0Format,
						m_currentViewport.TopLeftX,
						m_currentViewport.TopLeftY,
						m_currentViewport.Width,
						m_currentViewport.Height,
						m_currentViewport.MinDepth,
						m_currentViewport.MaxDepth,
						m_currentScissor.left,
						m_currentScissor.top,
						m_currentScissor.right,
						m_currentScissor.bottom,
						targetMask,
						blendMask,
						blend0Control,
						outputMask,
						vertexShader ? vertexShader->baseHash : 0,
						geometryShader ? geometryShader->baseHash : 0,
						pixelShader ? pixelShader->baseHash : 0);
		}
	}
	const bool usesPointExpansionGS = !noRasterizationOutput && ShouldUsePointExpansionGeometryShader(geometryShader, primitiveMode);
	const bool rasterizesPointQuads = usesPointExpansionGS || (geometryShader && LatteDecompiler_HLSLGeometryShaderExpandsPoints(LatteGPUState.contextRegister));
	bool useStencilTwoPass = false;
	bool stencilPassesOrderIndependent = false;
	if (!noRasterizationOutput && !rasterizesPointQuads && m_latteActiveFBO && D3D12PrimitiveUsesFaceCulling(primitiveMode))
	{
		const auto drawStencilState = GetD3D12SelectedStencilState(LatteGPUState.contextNew, cullState);
		const bool stencilEnabled =
			m_latteActiveFBO->HasDepthStencilAttachment() &&
			m_latteActiveFBO->GetStencilFormat() != DXGI_FORMAT_UNKNOWN &&
			LatteGPUState.contextNew.DB_DEPTH_CONTROL.get_STENCIL_ENABLE();
		bool independentStencilAvailable = false;
#if defined(__ID3D12GraphicsCommandList8_INTERFACE_DEFINED__)
		independentStencilAvailable =
			m_independentFrontBackStencilSupported; // every command slot has the list-8 interface when this is set
#endif
		useStencilTwoPass =
			stencilEnabled &&
			!cullState.cullFront && !cullState.cullBack &&
			drawStencilState.needsIndependentFrontBackState &&
			!independentStencilAvailable;
		if (useStencilTwoPass)
		{
#if defined(CEMU_UWP_D3D12_CORRECTNESS_TELEMETRY)
			++m_correctnessStencilFallbackDraws;
#endif
			cemuLog_logOnce(LogType::Force,
				"D3D12 UWP: using front/back stencil passes because independent stencil masks/references are unavailable.");
			const auto& depthControl = LatteGPUState.contextNew.DB_DEPTH_CONTROL;
			bool drawWritesColor = false;
			if (pixelShader)
			{
				const auto& attachments = m_latteActiveFBO->GetColorAttachments();
				for (uint32 i = 0; i < attachments.size() && i < 8; ++i)
				{
					if (attachments[i].valid && (pixelShader->pixelColorOutputMask & (1u << i)) != 0 &&
						((effectiveRenderTargetMask >> (i * 4)) & 0xFu) != 0)
						drawWritesColor = true;
				}
			}
			stencilPassesOrderIndependent =
				!drawWritesColor && !(pixelShader && pixelShader->hasStreamoutBufferWrite) &&
				!(depthControl.get_Z_ENABLE() && depthControl.get_Z_WRITE_ENABLE()) &&
				D3D12StencilFacesOrderIndependent(LatteGPUState.contextNew, drawStencilState, depthControl.get_Z_ENABLE());
		}
	}
	// a uniform color makes C | dst idempotent at overlaps, so one snapshot suffices
	const bool uniformOrEligible = !noRasterizationOutput && !useStencilTwoPass && !streamoutEnable &&
		!geometryShader && drawLogicOpMode == 2 && pixelShader &&
		pixelShader->pixelColorOutputIsUniform && !pixelShader->isCustomShader &&
		!pixelShader->hasError && !pixelShader->depthMask && !pixelShader->hasStreamoutBufferWrite;
	const bool uniformOrBatch = uniformOrEligible &&
		PerformanceExperimentEnabled(D3D12PerformanceExperiment::UniformOrBatch);
	const bool logicOpNeedsOrderedDestination =
		!noRasterizationOutput && !uniformOrBatch && D3D12LogicOpEmulationNeedsDestination(drawLogicOpMode);
	// front-then-back replay would reorder depth/stencil/blend; keep primitive order
	const bool useOrderedPrimitiveFallback = (useStencilTwoPass && !stencilPassesOrderIndependent) || logicOpNeedsOrderedDestination;
#if defined(CEMU_UWP_D3D12_CORRECTNESS_TELEMETRY)
	if (logicOpNeedsOrderedDestination)
		++m_correctnessOrderedLogicOpDraws;
#endif
	const bool needsFlippedOrderedPipeline = useOrderedPrimitiveFallback &&
		(d3dPrimitiveTopology == D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP ||
		 d3dPrimitiveTopology == D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP_ADJ);
	const StencilFallbackPass firstStencilPass = useStencilTwoPass ? StencilFallbackPass::Front : StencilFallbackPass::None;
	const StencilFallbackPass flippedStencilPass = useStencilTwoPass ? StencilFallbackPass::FrontFlipped : StencilFallbackPass::WindingFlipped;
	// streamout ring space and uniform data come from guest state (registers, guest memory), so both are prepared
	// before the split; a draw dropped after it (no PSO, invalid index buffer) still finishes its streamout
	m_streamoutVerticesPerInstance = (sint32)count;
	LatteStreamout_PrepareDrawcall(count, instanceCount);
	// runs when the head returns, after the tail is queued: its buffer copies are queued behind the draw
	stdx::scope_exit streamoutFinalizer([&]() { LatteStreamout_FinishDrawcall(false); });
	uwpHeadLap(HeadState);
	std::array<UniformUpdate, (size_t)LatteConst::ShaderType::TotalCount> uniformUpdates;
	{
#if defined(LATTE_UWP_DETAILED_PROFILING)
		const auto uniformStarted = profileThisDraw ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
#endif
		for (LatteDecompilerShader* shader : { vertexShader, pixelShader, geometryShader })
		{
			if (shader && (size_t)shader->shaderType < uniformUpdates.size())
				PrepareGameUniformVars(shader, nextDrawId, tracePipelineHash, uniformUpdates[(size_t)shader->shaderType]);
		}
#if defined(LATTE_UWP_DETAILED_PROFILING)
		if (profileThisDraw)
			m_uwpDescriptorSplitNs[1] += (uint64)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - uniformStarted).count();
#endif
	}
	// Phase 4 split point: from here on (pipeline lookup, descriptors, recording) the emulated GPU state is read
	// only through RecordRegs()/s_record*, never LatteGPUState. With threaded recording this tail runs on the
	// recording thread, with the register changes and uniform bytes of this draw travelling along with it.
	uwpHeadLap(HeadUniformPrepare);
	const bool queueTail = ShouldQueueRecording();
	// queued: the register delta is written straight into the draw's ring entry when it is queued below
	if (!queueTail)
		SyncRecordState(fetchShader);
#if defined(LATTE_UWP_DETAILED_PROFILING)
	finishProfileStage();
	profileHeadDone = true;
#endif
	uwpHeadLap(HeadCollect);
	auto recordDraw = [=, this, uniformUpdates = std::move(uniformUpdates)](const uint8* recordDeltaData) mutable
	{
	m_gpuWatchdog.NoteCall("draw");
	const uint64 s0Start = UwpProfTick();
	uint64 s0Mark = s0Start;
	auto s0Lap = [&s0Mark](std::atomic<uint64>& counter) { const uint64 now = UwpProfTick(); s_uwpRecorderSplit.Add(counter, now - s0Mark); s0Mark = now; };
	stdx::scope_exit s0Total([&]() { if (s0Start) { s_uwpRecorderSplit.Add(s_uwpRecorderSplit.total, UwpProfTick() - s0Start); s_uwpRecorderSplit.draws.fetch_add(1, std::memory_order_relaxed); } });
	if (queueTail)
		ApplyRecordState(recordDeltaData);
	for (auto& update : uniformUpdates)
		ApplyGameUniformUpdate(update);
	m_logicOpEmulationMode = drawLogicOpMode;
	m_disableNativeLogicOpForDraw = drawDisableNativeLogicOp;
	s0Lap(s_uwpRecorderSplit.apply);
	// the recording side numbers draws; scratch copies made for this draw are recycled when it is recorded
	const uint64 nextDrawId = m_gameDrawCallsSubmitted + 1;
	stdx::scope_exit unusedScratchFinalizer([&]() { RecycleScratchCopiesForDraw(nextDrawId); });
#if defined(LATTE_UWP_DETAILED_PROFILING)
	// the tail measures its own stages (the head's timers are gone when it runs on the recording thread); the
	// pipeline stage was already counted as reached by the head
	UwpCpuDrawStage tailProfileStage = UwpCpuDrawStage::PipelineLookup;
	auto tailProfileStarted = profileThisDraw ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
	auto finishTailProfileStage = [&]()
	{
		if (!profileThisDraw)
			return;
		const auto now = std::chrono::steady_clock::now();
		m_uwpCpuProfileTailTimeNs[(size_t)tailProfileStage] += (uint64)std::chrono::duration_cast<std::chrono::nanoseconds>(now - tailProfileStarted).count();
		if (tailProfileStage != UwpCpuDrawStage::PipelineLookup)
			++m_uwpCpuProfileTailSamples[(size_t)tailProfileStage];
		tailProfileStarted = now;
	};
	auto switchProfileStage = [&](UwpCpuDrawStage nextStage)
	{
		finishTailProfileStage();
		tailProfileStage = nextStage;
	};
	stdx::scope_exit tailProfileFinalizer([&]() { finishTailProfileStage(); });
#else
	auto switchProfileStage = [](UwpCpuDrawStage) {};
#endif
	// same heuristic as Vulkan: full-viewport quads (<=6 indices) often build persistent textures and Splatoon ink uses 1600x1600 targets, so those stay synchronous
	m_gamePipelineAsyncPending = false;
	// bits say why a draw may not be skipped; logged with slow synchronous pipeline builds
	m_asyncSkipBlockReasons = 0;
	if (!GetConfig().async_compile)
		m_asyncSkipBlockReasons |= 1u << 0;
	if (!m_activeFBO)
		m_asyncSkipBlockReasons |= 1u << 1;
	else if (m_activeFBO->m_size.x == 1600 && m_activeFBO->m_size.y == 1600)
		m_asyncSkipBlockReasons |= 1u << 2;
	else if (!m_activeFBO->HasDepthStencilAttachment() && count <= 6)
		m_asyncSkipBlockReasons |= 1u << 3;
	// a skipped draw would report zero samples or leave stream-out data unwritten, and the game reads both back
	if (std::any_of(m_activeOcclusionQueries.begin(), m_activeOcclusionQueries.end(),
		[](const LatteQueryObjectD3D12* query) { return query && query->m_hasActiveQuery; }))
		m_asyncSkipBlockReasons |= 1u << 4;
	if ((vertexShader && vertexShader->hasStreamoutBufferWrite) || (geometryShader && geometryShader->hasStreamoutBufferWrite))
		m_asyncSkipBlockReasons |= 1u << 5;
	m_allowAsyncShaderSkip = m_asyncSkipBlockReasons == 0;
	m_gamePipelineShaderCodeMissing = false;
	s0Mark = UwpProfTick();
	ID3D12PipelineState* pipeline = GetGamePipeline(identity, vertexShader, geometryShader, pixelShader, primitiveMode, noRasterizationOutput, hostIndexType, firstStencilPass);
	ID3D12PipelineState* flippedPipeline = needsFlippedOrderedPipeline ?
		GetGamePipeline(identity, vertexShader, geometryShader, pixelShader, primitiveMode, noRasterizationOutput, hostIndexType, flippedStencilPass) : nullptr;
	ID3D12PipelineState* backStencilPipeline = useStencilTwoPass ?
		GetGamePipeline(identity, vertexShader, geometryShader, pixelShader, primitiveMode, noRasterizationOutput, hostIndexType, StencilFallbackPass::Back) : nullptr;
	ID3D12PipelineState* backFlippedStencilPipeline = useStencilTwoPass && needsFlippedOrderedPipeline ?
		GetGamePipeline(identity, vertexShader, geometryShader, pixelShader, primitiveMode, noRasterizationOutput, hostIndexType, StencilFallbackPass::BackFlipped) : nullptr;
	s0Lap(s_uwpRecorderSplit.pso);
	if (!pipeline ||
		(needsFlippedOrderedPipeline && !flippedPipeline) ||
		(useStencilTwoPass && !backStencilPipeline) ||
		(useStencilTwoPass && needsFlippedOrderedPipeline && !backFlippedStencilPipeline))
	{
		if (m_gamePipelineAsyncPending)
		{
			// shaders still compiling in the background; expected, counted for the 5 s log
			++m_uwpAsyncSkips;
			++m_uwpAsyncSkipsByTarget[m_activeFBO ? ((uint32)m_activeFBO->m_size.x << 16) | (uint32)m_activeFBO->m_size.y : 0];
			return;
		}
#if defined(CEMU_UWP_D3D12_CORRECTNESS_TELEMETRY)
		++m_correctnessDrawSkippedNoPso;
#endif
		++m_droppedDraws[(size_t)(m_gamePipelineShaderCodeMissing ?
			DroppedDrawReason::MissingShaderCode : DroppedDrawReason::MissingPipeline)];
		uint64 skipSignature = identity.valid ? identity.pipelineStateHash : 0;
		skipSignature = std::rotl(skipSignature, 7) ^ (m_activeFBO ? 0x10001ull : 0);
		skipSignature = std::rotl(skipSignature, 7) ^ (vertexShader ? vertexShader->baseHash : 0);
		skipSignature = std::rotl(skipSignature, 7) ^ (geometryShader ? geometryShader->baseHash : 0);
		skipSignature = std::rotl(skipSignature, 7) ^ (pixelShader ? pixelShader->baseHash : 0);
		skipSignature = std::rotl(skipSignature, 7) ^ 0xD3125C1Bull;
		if (ShouldLogDrawFailure(skipSignature))
		{
			cemuLog_log(LogType::Force, "D3D12: draw skipped because no valid game PSO is available yet (identity={}, fbo={}, vs={:016x}, gs={:016x}, ps={:016x}).",
						identity.valid ? "valid" : "invalid",
						m_activeFBO ? "yes" : "no",
						vertexShader ? vertexShader->baseHash : 0,
						geometryShader ? geometryShader->baseHash : 0,
						pixelShader ? pixelShader->baseHash : 0);
		}
		D3D12TraceEvent("draw_skip", "reason=no_pso identity={} fbo=0x{:016x} vs=0x{:016x} gs=0x{:016x} ps=0x{:016x} stateHash=0x{:016x}",
						identity.valid ? 1 : 0,
						m_activeFBO ? m_activeFBO->key : 0,
						vertexShader ? vertexShader->baseHash : 0,
						geometryShader ? geometryShader->baseHash : 0,
						pixelShader ? pixelShader->baseHash : 0,
						identity.valid ? identity.pipelineStateHash : 0);
		return;
	}

	uint32 isolationRtAddress = 0;
	if (m_activeFBO && !m_activeFBO->GetColorAttachments().empty())
	{
		const auto& rt0 = m_activeFBO->GetColorAttachments()[0];
		if (rt0.valid && rt0.view && rt0.view->baseTexture)
			isolationRtAddress = rt0.view->baseTexture->physAddress;
	}
	if (D3D12DrawIsolationMatches(nextDrawId,
								  identity.pipelineStateHash,
								  vertexShader ? vertexShader->baseHash : 0,
								  geometryShader ? geometryShader->baseHash : 0,
								  pixelShader ? pixelShader->baseHash : 0,
								  isolationRtAddress))
	{
		D3D12TraceEventFlush("draw_isolation_skip",
							 "draw={} latteDraw={} pipeline=0x{:016x} rt0=0x{:08x} fbo=0x{:016x} vs=0x{:016x} gs=0x{:016x} ps=0x{:016x} primitive={} hostIndexType={} hostIndexCount={} count={} viewport={},{}:{}x{} scissor={},{}-{},{}",
							 (unsigned long long)nextDrawId,
							 LatteGPUState.drawCallCounter,
							 identity.pipelineStateHash,
							 isolationRtAddress,
							 m_activeFBO ? m_activeFBO->key : 0,
							 vertexShader ? vertexShader->baseHash : 0,
							 geometryShader ? geometryShader->baseHash : 0,
							 pixelShader ? pixelShader->baseHash : 0,
							 (uint32)primitiveMode,
							 (uint32)hostIndexType,
							 hostIndexCount,
							 count,
							 m_currentViewport.TopLeftX,
							 m_currentViewport.TopLeftY,
							 m_currentViewport.Width,
							 m_currentViewport.Height,
							 m_currentScissor.left,
							 m_currentScissor.top,
							 m_currentScissor.right,
							 m_currentScissor.bottom);
		cemuLog_log(LogType::Force,
					"D3D12 isolation: skipped draw={} latteDraw={} pipeline=0x{:016x} rt0=0x{:08x} vs={:016x} gs={:016x} ps={:016x} primitive={} hostCount={}.",
					(unsigned long long)nextDrawId,
					LatteGPUState.drawCallCounter,
					identity.pipelineStateHash,
					isolationRtAddress,
					vertexShader ? vertexShader->baseHash : 0,
					geometryShader ? geometryShader->baseHash : 0,
					pixelShader ? pixelShader->baseHash : 0,
					(uint32)primitiveMode,
					hostIndexCount);
		m_gameDrawCallsSubmitted++;
		return;
	}

	auto* d3dIndexAllocation = static_cast<D3D12IndexAllocation*>(indexAllocation.rendererInternal);
	const DXGI_FORMAT indexFormat = GetD3D12IndexFormat(hostIndexType);
	const uint32 hostIndexStride = hostIndexType == Renderer::INDEX_TYPE::U16 ? sizeof(uint16) :
		(hostIndexType == Renderer::INDEX_TYPE::U32 ? sizeof(uint32) : 0);
	const uint64 indexDataSize64 = (uint64)hostIndexCount * hostIndexStride;
	const uint32 indexDataSize = indexDataSize64 <= std::numeric_limits<uint32>::max() ?
		(uint32)indexDataSize64 : 0;
	bool indexAllocationValid = d3dIndexAllocation != nullptr;
	if (d3dIndexAllocation)
		indexAllocationValid = indexDataSize != 0 &&
			indexDataSize <= d3dIndexAllocation->size &&
			d3dIndexAllocation->GetDecodedCapacity() >= d3dIndexAllocation->size;
	if (hostIndexType != Renderer::INDEX_TYPE::NONE &&
		(!indexAllocationValid || indexFormat == DXGI_FORMAT_UNKNOWN))
	{
#if defined(CEMU_UWP_D3D12_CORRECTNESS_TELEMETRY)
		++m_correctnessDrawSkippedInvalidIndex;
#endif
		++m_droppedDraws[(size_t)DroppedDrawReason::InvalidIndexBuffer];
		static uint32 s_invalidIndexLogCount = 0;
		if (D3D12RuntimeLogCapped(s_invalidIndexLogCount, "invalid index buffer", 32))
		{
			cemuLog_log(LogType::Force, "D3D12: skipping indexed draw because decoded index buffer is unavailable (hostIndexType={} hostIndexCount={} count={} primitive={}).",
						(uint32)hostIndexType,
						hostIndexCount,
						count,
						(uint32)primitiveMode);
		}
		return;
	}

	switchProfileStage(UwpCpuDrawStage::DescriptorAndFeedbackSetup);
	if (!noRasterizationOutput)
		TraceFboAttachments("draw_fbo_attachment_before", "before_draw", nextDrawId, identity.pipelineStateHash, "call=draw_execute stage=pre_state_bind");

	// phase 5 S2b: preparation (feedback detection, descriptor space, tables) runs before any command is recorded;
	// draws that copy a feedback source need the copies first, so their tables are built after the emission of those
#if defined(LATTE_UWP_DETAILED_PROFILING)
	m_uwpProfileThisDraw = profileThisDraw;
#endif
	s0Mark = UwpProfTick();
	FeedbackPlan feedbackPlan;
	PlanFeedbackForDraw(vertexShader, geometryShader, noRasterizationOutput ? nullptr : pixelShader, !noRasterizationOutput, feedbackPlan);
	GameDescriptorTables drawTables{};
	auto prepareTables = [&]()
	{
#if defined(LATTE_UWP_DETAILED_PROFILING)
		const auto preTablesStarted = profileThisDraw ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
#endif
		++m_tableKeyMemoEpoch;
		const uint32 resourceDescriptorGeneration = m_descriptorManager->GetShaderVisibleCbvSrvUavRollovers();
		const uint32 samplerDescriptorGeneration = m_descriptorManager->GetShaderVisibleSamplerRollovers();
		const bool hasReusableGameNullResourceTable =
			m_gameNullResourceTable.IsValid() &&
			m_gameNullResourceTableGeneration == resourceDescriptorGeneration;
		const bool hasReusableGameNullUavTable =
			m_gameNullUavTable.IsValid() &&
			m_gameNullUavTableGeneration == resourceDescriptorGeneration;
		uint32 gameResourceDescriptorCount = 0;
		auto countResourceTableMiss = [this, hasReusableGameNullResourceTable, resourceDescriptorGeneration, &gameResourceDescriptorCount](LatteDecompilerShader* shader)
		{
			if (!shader)
			{
				if (!hasReusableGameNullResourceTable)
					gameResourceDescriptorCount += GetGameResourceTableDescriptorCount(nullptr);
				return;
			}
			if (!HasCachedGameResourceTable(shader, resourceDescriptorGeneration))
				gameResourceDescriptorCount += GetGameResourceTableDescriptorCount(shader);
		};
		countResourceTableMiss(vertexShader);
		countResourceTableMiss(pixelShader);
		countResourceTableMiss(geometryShader);
		uint32 gameUavDescriptorCount = 0;
		bool needsNullUavTable = useStencilTwoPass && streamoutEnable;
		auto countUavTable = [this, &gameUavDescriptorCount, &needsNullUavTable](LatteDecompilerShader* shader)
		{
			const uint32 count = GetGameUavTableDescriptorCount(shader);
			if (count != 0 && m_streamoutBuffer.resource)
				gameUavDescriptorCount += count;
			else
				needsNullUavTable = true;
		};
		countUavTable(vertexShader);
		countUavTable(pixelShader);
		countUavTable(geometryShader);
		if (needsNullUavTable && !hasReusableGameNullUavTable)
			gameUavDescriptorCount += kGameUavDescriptorCount;
		uint32 gameSamplerDescriptorCount = 0;
		auto countSamplerTableMiss = [this, samplerDescriptorGeneration, &gameSamplerDescriptorCount](LatteDecompilerShader* shader)
		{
			if (!HasCachedGameSamplerTable(shader, samplerDescriptorGeneration))
				gameSamplerDescriptorCount += GetGameSamplerTableDescriptorCount(shader);
		};
		countSamplerTableMiss(vertexShader);
		countSamplerTableMiss(pixelShader);
		countSamplerTableMiss(geometryShader);
		EnsureShaderVisibleDescriptorSpace(gameResourceDescriptorCount + gameUavDescriptorCount,
										   gameSamplerDescriptorCount);
#if defined(LATTE_UWP_DETAILED_PROFILING)
		const auto tablesStarted = profileThisDraw ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
		if (profileThisDraw)
			m_uwpDescriptorSplitNs[3] += (uint64)std::chrono::duration_cast<std::chrono::nanoseconds>(tablesStarted - preTablesStarted).count();
#endif
		s0Lap(s_uwpRecorderSplit.tablePrep);
		t_uwpInDrawTables = true;
		if (m_gameRootSignature)
			drawTables = BuildGameDescriptorTables(vertexShader, geometryShader, pixelShader, tracePipelineHash);
		t_uwpInDrawTables = false;
		s0Lap(s_uwpRecorderSplit.tables);
#if defined(LATTE_UWP_DETAILED_PROFILING)
		if (profileThisDraw)
			m_uwpDescriptorSplitNs[2] += (uint64)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - tablesStarted).count();
#endif
	};
	if (!feedbackPlan.anyCopy)
		prepareTables();

	// emission
	if (!m_commandListOpen)
		ResetCommandList();
#if defined(LATTE_UWP_DETAILED_PROFILING)
	const auto feedbackStarted = profileThisDraw ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
#endif
	s0Mark = UwpProfTick();
	EmitFeedbackForDraw(feedbackPlan);
	s0Lap(s_uwpRecorderSplit.feedback);
#if defined(LATTE_UWP_DETAILED_PROFILING)
	if (profileThisDraw)
	{
		m_uwpDescriptorSplitNs[0] += (uint64)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - feedbackStarted).count();
		++m_uwpDescriptorSplitSamples;
	}
#endif
	// Scratch-allocation recovery can submit and reopen on another command slot.
	if (!m_commandListOpen)
		ResetCommandList();
	if (feedbackPlan.anyCopy)
		prepareTables();
	auto* commandList = GetCurrentCommandSlot().commandList.Get();
	SetD3D12PipelineState(pipeline);
	m_gpuWatchdog.RecordGamePipeline(tracePipelineHash,
		vertexShader ? vertexShader->baseHash : 0,
		geometryShader ? geometryShader->baseHash : 0,
		pixelShader ? pixelShader->baseHash : 0);
	if (m_commandListOpen && m_gameRootSignature)
		EmitGameDescriptorTables(drawTables, vertexShader, geometryShader, pixelShader, baseVertex, baseInstance, vertexIdRemapMode, tracePipelineHash);
#if defined(LATTE_UWP_DETAILED_PROFILING)
	const auto tablesFinished = profileThisDraw ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
#endif
	stdx::scope_exit s0Emit([&]() { s0Lap(s_uwpRecorderSplit.emit); });
#if defined(LATTE_UWP_DETAILED_PROFILING)
	m_uwpProfileThisDraw = false;
#endif
	commandList = GetCurrentCommandSlot().commandList.Get();
	D3D12DescriptorManager::Allocation stencilNullUavTable;
	D3D12_GPU_DESCRIPTOR_HANDLE stencilVsUavTable{};
	D3D12_GPU_DESCRIPTOR_HANDLE stencilPsUavTable{};
	D3D12_GPU_DESCRIPTOR_HANDLE stencilGsUavTable{};
	if (useStencilTwoPass && streamoutEnable)
	{
		stencilVsUavTable = m_gameRootDescriptorTables[10];
		stencilPsUavTable = m_gameRootDescriptorTables[11];
		stencilGsUavTable = m_gameRootDescriptorTables[12];
		stencilNullUavTable = BuildGameUavTable(nullptr);
	}
	if (noRasterizationOutput)
		SetD3D12RenderTargets(0, nullptr, nullptr);
	else
		BindFramebuffer(m_activeFBO);
	SetD3D12Viewport(m_currentViewport);
	SetD3D12Scissor(m_currentScissor);
	const float blendConstantAlpha = RecordRegs().CB_BLEND_ALPHA.get_ALPHA();
	const bool splatBlendConstantAlpha = !m_alphaBlendFactorSupported &&
		D3D12BlendConstantNeedsAlphaSplat(RecordRegs(), noRasterizationOutput ? nullptr : m_activeFBO);
	const float blendFactor[4] =
	{
		splatBlendConstantAlpha ? blendConstantAlpha : RecordRegs().CB_BLEND_RED.get_RED(),
		splatBlendConstantAlpha ? blendConstantAlpha : RecordRegs().CB_BLEND_GREEN.get_GREEN(),
		splatBlendConstantAlpha ? blendConstantAlpha : RecordRegs().CB_BLEND_BLUE.get_BLUE(),
		blendConstantAlpha,
	};
	SetD3D12BlendFactor(blendFactor);
	const auto selectedStencilState = GetD3D12SelectedStencilState(RecordRegs(),
		GetD3D12StencilFaceCullState(cullState, rasterizesPointQuads));
#if defined(__ID3D12GraphicsCommandList8_INTERFACE_DEFINED__)
	auto& commandSlot = GetCurrentCommandSlot();
#endif
	if (useStencilTwoPass)
	{
		SetD3D12StencilRef(selectedStencilState.frontRef, selectedStencilState.frontRef);
	}
#if defined(__ID3D12GraphicsCommandList8_INTERFACE_DEFINED__)
	else if (selectedStencilState.needsIndependentFrontBackState &&
		m_independentFrontBackStencilSupported && commandSlot.commandList8)
	{
		SetD3D12StencilRef(selectedStencilState.frontRef, selectedStencilState.backRef);
	}
	else
#endif
	{
		SetD3D12StencilRef(selectedStencilState.ref, selectedStencilState.ref);
	}

#if defined(CEMU_UWP_D3D12_DIAGNOSTICS)
	if (priorityStencilDraw && D3D12TraceSink::Get().IsPriorityStencilDraw(nextDrawId))
	{
		const auto& depth = RecordRegs().DB_DEPTH_CONTROL;
		const bool back = depth.get_BACK_STENCIL_ENABLE();
		bool nativeIndependentRefs = false;
#if defined(__ID3D12GraphicsCommandList8_INTERFACE_DEFINED__)
		nativeIndependentRefs = !useStencilTwoPass && selectedStencilState.needsIndependentFrontBackState &&
			m_independentFrontBackStencilSupported && commandSlot.commandList8;
#endif
		const bool nativeIndependentMasks = nativeIndependentRefs &&
			(selectedStencilState.frontReadMask != selectedStencilState.backReadMask ||
			 selectedStencilState.frontWriteMask != selectedStencilState.backWriteMask);
		D3D12TraceEvent("draw_stencil_state", "draw={} pipeline=0x{:016x} phase=after_om_bind priority=stencil_enabled snapshotKind=cpu_selected_state gpuStateVerified=0 vs=0x{:016x} gs=0x{:016x} ps=0x{:016x} rawDepthControl=0x{:08x} rawStencilF=0x{:08x} rawStencilB=0x{:08x} nativeDepthEnable={} nativeDepthWrite={} nativeDepthFunc={} stencilEnable={} backEnable={} selectedBack={} selectedRef={} selectedRead=0x{:02x} selectedWrite=0x{:02x} frontRef={} frontRead=0x{:02x} frontWrite=0x{:02x} backRef={} backRead=0x{:02x} backWrite=0x{:02x} independentNeeded={} nativeIndependentRefs={} nativeIndependentMasks={} twoPass={} firstPassRef={} nativeFrontOps=func:{}/fail:{}/zfail:{}/pass:{} nativeBackOps=func:{}/fail:{}/zfail:{}/pass:{} rawColorControl=0x{:08x} rawBlend0=0x{:08x} blendFactor={:.9g},{:.9g},{:.9g},{:.9g} effectiveTargetMask=0x{:08x} dsvFormat={} stencilFormat={}",
			nextDrawId, tracePipelineHash, vertexShader ? vertexShader->baseHash : 0,
			geometryShader ? geometryShader->baseHash : 0, pixelShader ? pixelShader->baseHash : 0,
			depth.getRawValue(), RecordRegs().DB_STENCILREFMASK.getRawValue(),
			RecordRegs().DB_STENCILREFMASK_BF.getRawValue(),
			depth.get_Z_ENABLE(), depth.get_Z_WRITE_ENABLE(), (uint32)GetD3D12CompareFunc(depth.get_Z_FUNC()),
			depth.get_STENCIL_ENABLE(), back, selectedStencilState.usedBackFace,
			selectedStencilState.ref, selectedStencilState.readMask, selectedStencilState.writeMask,
			selectedStencilState.frontRef, selectedStencilState.frontReadMask, selectedStencilState.frontWriteMask,
			selectedStencilState.backRef, selectedStencilState.backReadMask, selectedStencilState.backWriteMask,
			selectedStencilState.needsIndependentFrontBackState, nativeIndependentRefs, nativeIndependentMasks,
			useStencilTwoPass, useStencilTwoPass ? selectedStencilState.frontRef : selectedStencilState.ref,
			(uint32)GetD3D12CompareFunc(depth.get_STENCIL_FUNC_F()), (uint32)GetD3D12StencilOp(depth.get_STENCIL_FAIL_F()),
			(uint32)GetD3D12StencilOp(depth.get_STENCIL_ZFAIL_F()), (uint32)GetD3D12StencilOp(depth.get_STENCIL_ZPASS_F()),
			(uint32)GetD3D12CompareFunc(back ? depth.get_STENCIL_FUNC_B() : depth.get_STENCIL_FUNC_F()),
			(uint32)GetD3D12StencilOp(back ? depth.get_STENCIL_FAIL_B() : depth.get_STENCIL_FAIL_F()),
			(uint32)GetD3D12StencilOp(back ? depth.get_STENCIL_ZFAIL_B() : depth.get_STENCIL_ZFAIL_F()),
			(uint32)GetD3D12StencilOp(back ? depth.get_STENCIL_ZPASS_B() : depth.get_STENCIL_ZPASS_F()),
			RecordRegs().CB_COLOR_CONTROL.getRawValue(), RecordRegs().CB_BLENDN_CONTROL[0].getRawValue(),
			blendFactor[0], blendFactor[1], blendFactor[2], blendFactor[3], effectiveRenderTargetMask,
			(uint32)m_activeFBO->GetDepthFormat(), (uint32)m_activeFBO->GetStencilFormat());
	}
#endif

	if (hostIndexType != Renderer::INDEX_TYPE::NONE &&
		!EnsureIndexResident(*d3dIndexAllocation, indexDataSize))
	{
#if defined(CEMU_UWP_D3D12_CORRECTNESS_TELEMETRY)
		++m_correctnessDrawSkippedInvalidIndex;
#endif
		++m_droppedDraws[(size_t)DroppedDrawReason::IndexUploadFailed];
		static uint32 s_indexResidencyFailureLogCount = 0;
		if (D3D12RuntimeLogCapped(s_indexResidencyFailureLogCount, "index upload-ring residency failure", 32))
		{
			cemuLog_log(LogType::Force,
				"D3D12 UWP: skipping indexed draw because {} decoded index bytes could not be made resident for fence {}.",
				indexDataSize,
				(unsigned long long)m_nextFenceValue);
		}
		return;
	}
	commandList = GetCurrentCommandSlot().commandList.Get();
#if defined(LATTE_UWP_DETAILED_PROFILING)
	if (profileThisDraw)
		m_uwpDescriptorSplitNs[4] += (uint64)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - tablesFinished).count();
#endif
	switchProfileStage(UwpCpuDrawStage::DrawRecording);
	if (hostIndexType != Renderer::INDEX_TYPE::NONE)
	{
		D3D12_INDEX_BUFFER_VIEW indexView{};
		ID3D12Resource* indexResource = d3dIndexAllocation->residentResource;
		indexView.BufferLocation = indexResource->GetGPUVirtualAddress() +
			d3dIndexAllocation->residentOffset;
		indexView.SizeInBytes = indexDataSize;
		indexView.Format = indexFormat;
		D3D12TraceEvent("draw_index_buffer", "draw={} pipeline=0x{:016x} allocation={} resource={} gpuVA=0x{:016x} size={} format={} hostIndexType={} hostIndexCount={} baseVertex={} indexMin={} indexMax={}",
						(unsigned long long)nextDrawId,
						(unsigned long long)tracePipelineHash,
						(const void*)d3dIndexAllocation,
						(const void*)indexResource,
						(unsigned long long)indexView.BufferLocation,
						indexView.SizeInBytes,
						(uint32)indexView.Format,
						(uint32)hostIndexType,
						hostIndexCount,
						baseVertex,
						indexMin,
						indexMax);
		if (D3D12ShouldTrace())
		{
			const uint32 previewCount = std::min<uint32>(hostIndexCount, 24);
			std::string indexValues;
			const uint8* previewData = d3dIndexAllocation->previewData.data();
			const size_t previewDataSize = d3dIndexAllocation->previewDataSize;
			for (uint32 i = 0; i < previewCount; ++i)
			{
				uint32 value = 0;
				if (hostIndexType == Renderer::INDEX_TYPE::U16 && (size_t)i * sizeof(uint16) + sizeof(uint16) <= previewDataSize)
					memcpy(&value, previewData + (size_t)i * sizeof(uint16), sizeof(uint16));
				else if (hostIndexType == Renderer::INDEX_TYPE::U32 && (size_t)i * sizeof(uint32) + sizeof(uint32) <= previewDataSize)
					memcpy(&value, previewData + (size_t)i * sizeof(uint32), sizeof(uint32));
				if (!indexValues.empty())
					indexValues.push_back(',');
				fmt::format_to(std::back_inserter(indexValues), "{}", value);
			}
			D3D12TraceEvent("draw_index_preview", "draw={} pipeline=0x{:016x} hostIndexType={} hostIndexCount={} previewCount={} previewBytes={} baseVertex={} indexMin={} indexMax={} values={}",
							(unsigned long long)nextDrawId,
							(unsigned long long)tracePipelineHash,
							(uint32)hostIndexType,
							hostIndexCount,
							previewCount,
							previewDataSize,
							baseVertex,
							indexMin,
							indexMax,
							indexValues);
		}
		SetD3D12IndexBuffer(indexView);
	}
	else
	{
		D3D12TraceEvent("draw_index_buffer", "draw={} pipeline=0x{:016x} allocation=null nonIndexed=1 hostIndexType={} hostIndexCount={} baseVertex={} count={}",
						(unsigned long long)nextDrawId,
						(unsigned long long)tracePipelineHash,
						(uint32)hostIndexType,
						hostIndexCount,
						baseVertex,
						count);
	}
	SetD3D12PrimitiveTopology(d3dPrimitiveTopology);
	const bool dynamicDepthBias = UsesDynamicDepthBias(rasterizesPointQuads);
	const float dynamicDepthBiasConstant = RecordRegs().PA_SU_POLY_OFFSET_FRONT_OFFSET.get_OFFSET();
	const float dynamicDepthBiasClamp = RecordRegs().PA_SU_POLY_OFFSET_CLAMP.get_CLAMP();
	const float dynamicDepthBiasSlope = RecordRegs().PA_SU_POLY_OFFSET_FRONT_SCALE.get_SCALE() / 16.0f;
	const auto setDrawOffsetConstant = [&](uint32 constantIndex, uint32 value)
	{
		if (m_gameRootDrawConstantsValid && m_gameRootDrawConstants[constantIndex] == value)
			return;
		commandList->SetGraphicsRoot32BitConstant(6, value, constantIndex);
		m_gameRootDrawConstants[constantIndex] = value;
	};
	// base vertex/instance offsets are applied in HLSL; IA offsets stay zero
	const auto issueDraw = [&](uint32 start, uint32 drawCount, uint32 startInstance, uint32 drawInstanceCount)
	{
		if (dynamicDepthBias)
			SetD3D12DepthBias(dynamicDepthBiasConstant, dynamicDepthBiasClamp, dynamicDepthBiasSlope);
		const bool indexed = hostIndexType != Renderer::INDEX_TYPE::NONE;
		setDrawOffsetConstant(1, indexed ? baseVertex : baseVertex + start);
		setDrawOffsetConstant(2, baseInstance + startInstance);
		if (indexed)
			commandList->DrawIndexedInstanced(drawCount, drawInstanceCount, start, 0, 0);
		else
			commandList->DrawInstanced(drawCount, drawInstanceCount, 0, 0);
	};
	uint32 recordedNativeDraws = 0;
#if defined(CEMU_UWP_D3D12_DIAGNOSTICS)
	int geometryProbeIndex = -1;
	// exclude draws that may submit or restart command lists between query Begin/End
	const auto& probeDepth = RecordRegs().DB_DEPTH_CONTROL;
	const bool probeDepthWrite = probeDepth.get_Z_ENABLE() && probeDepth.get_Z_WRITE_ENABLE();
	const bool probeReadonlyColor = probeDepth.get_Z_ENABLE() && !probeDepthWrite;
	if (m_geometryProbe && !noRasterizationOutput && !streamoutEnable && !geometryShader &&
		!usesPointExpansionGS && !useOrderedPrimitiveFallback && !useStencilTwoPass &&
		primitiveMode == Latte::LATTE_VGT_PRIMITIVE_TYPE::E_PRIMITIVE_TYPE::TRIANGLES &&
		hostIndexCount >= 3 && instanceCount != 0 && m_activeFBO &&
		m_activeFBO->depthBuffer.texture && (probeDepthWrite || probeReadonlyColor) &&
		m_geometryProbe->WantsSample(tracePipelineHash, probeDepthWrite))
	{
		const auto* depthView = m_activeFBO->depthBuffer.texture;
		std::string metadata = fmt::format(
			"draw={} pipeline=0x{:016x} vs=0x{:016x} ps=0x{:016x} depthAddr=0x{:08x} indices={} instances={} "
			"baseVertex={} indexMax={} zWrite={} zFunc={} alpha={} alphaFunc={} alphaRef=0x{:08x} "
			"targetMask=0x{:08x} psOutputMask=0x{:02x} viewport={},{}:{}x{} depthRange={}..{} "
			"scissor={},{}-{},{} cullFront={} cullBack={} frontFace={} sources={}",
			nextDrawId, tracePipelineHash, vertexShader ? vertexShader->baseHash : 0,
			pixelShader ? pixelShader->baseHash : 0, depthView->baseTexture->physAddress,
			hostIndexCount, instanceCount, baseVertex, indexMax, probeDepthWrite,
			(uint32)probeDepth.get_Z_FUNC(),
			RecordRegs().SX_ALPHA_TEST_CONTROL.get_ALPHA_TEST_ENABLE(),
			(uint32)RecordRegs().SX_ALPHA_TEST_CONTROL.get_ALPHA_FUNC(),
			RecordRegs().SX_ALPHA_REF.getRawValue(), effectiveRenderTargetMask,
			pixelShader ? pixelShader->pixelColorOutputMask : 0,
			m_currentViewport.TopLeftX, m_currentViewport.TopLeftY,
			m_currentViewport.Width, m_currentViewport.Height,
			m_currentViewport.MinDepth, m_currentViewport.MaxDepth,
			m_currentScissor.left, m_currentScissor.top, m_currentScissor.right, m_currentScissor.bottom,
			cullState.cullFront, cullState.cullBack, (uint32)polygonControlReg.get_FRONT_FACE(),
			BuildShaderTextureSourceSummary(vertexShader, geometryShader, pixelShader));
		metadata += " " + BuildGeometryProbeResourceMetadata(vertexShader, geometryShader, pixelShader);
		geometryProbeIndex = m_geometryProbe->Begin(commandList, tracePipelineHash, probeDepthWrite,
			m_activeOcclusionQueries.empty(), metadata);
	}
#endif
	auto setUavTable = [this, commandList](uint32 rootIndex, D3D12_GPU_DESCRIPTOR_HANDLE table)
	{
		if (rootIndex < m_gameRootDescriptorTableValid.size() &&
			m_gameRootDescriptorTableValid[rootIndex] &&
			m_gameRootDescriptorTables[rootIndex].ptr == table.ptr)
		{
			return;
		}
		commandList->SetGraphicsRootDescriptorTable(rootIndex, table);
		if (rootIndex < m_gameRootDescriptorTableValid.size())
		{
			m_gameRootDescriptorTables[rootIndex] = table;
			m_gameRootDescriptorTableValid[rootIndex] = true;
		}
	};
	const auto bindFirstPassUavs = [&]()
	{
		if (!useStencilTwoPass || !streamoutEnable)
			return;
		setUavTable(10, stencilVsUavTable);
		setUavTable(11, stencilPsUavTable);
		setUavTable(12, stencilGsUavTable);
	};
	const auto bindNullUavs = [&]()
	{
		if (!useStencilTwoPass || !streamoutEnable || !stencilNullUavTable.IsValid())
			return;
		setUavTable(10, stencilNullUavTable.gpu);
		setUavTable(11, stencilNullUavTable.gpu);
		setUavTable(12, stencilNullUavTable.gpu);
	};
	if (useOrderedPrimitiveFallback)
	{
		const bool primitiveRestartEnabled = GetD3D12PrimitiveRestartEnabled(RecordRegs(), primitiveMode, hostIndexType);
		const auto primitiveSpans = BuildD3D12OrderedPrimitiveSpans(
			d3dPrimitiveTopology,
			hostIndexCount,
			hostIndexType,
			d3dIndexAllocation ? d3dIndexAllocation->GetDecodedData() : nullptr,
			primitiveRestartEnabled);
		if (primitiveSpans.empty() && hostIndexCount != 0)
		{
			cemuLog_log(LogType::Force,
				"D3D12 UWP: ordered draw fallback could not split topology {} with {} elements; preserving the original draw.",
				(uint32)d3dPrimitiveTopology, hostIndexCount);
			issueDraw(0, hostIndexCount, 0, instanceCount);
			recordedNativeDraws = instanceCount != 0 ? 1 : 0;
		}
		else
		{
			const uint64 totalLogicalPrimitives = (uint64)primitiveSpans.size() * instanceCount;
#if defined(CEMU_UWP_D3D12_WORKLOAD_PROFILE)
			m_uwpWorkloadProfile.logicalPrimitives += totalLogicalPrimitives;
#endif
			uint64 completedLogicalPrimitives = 0;
			bool refreshLogicDestination = logicOpNeedsOrderedDestination;
			for (uint32 instance = 0; instance < instanceCount; ++instance)
			{
				for (const auto& span : primitiveSpans)
				{
					ID3D12PipelineState* firstPipeline = span.flipWinding ? flippedPipeline : pipeline;
					SetD3D12PipelineState(firstPipeline);
					if (useStencilTwoPass)
						SetD3D12StencilRef(selectedStencilState.frontRef, selectedStencilState.frontRef);
					bindFirstPassUavs();
					issueDraw(span.start, span.count, instance, 1);
					++recordedNativeDraws;

					if (useStencilTwoPass)
					{
						SetD3D12PipelineState(span.flipWinding ? backFlippedStencilPipeline : backStencilPipeline);
						SetD3D12StencilRef(selectedStencilState.backRef, selectedStencilState.backRef);
						bindNullUavs();
						issueDraw(span.start, span.count, instance, 1);
						++recordedNativeDraws;
					}

					++completedLogicalPrimitives;
					if (refreshLogicDestination && completedLogicalPrimitives < totalLogicalPrimitives &&
						!RefreshLogicOpScratchCopiesForDraw(nextDrawId))
					{
						cemuLog_logOnce(LogType::Force,
							"D3D12 UWP: logic-op destination refresh failed; later primitives will use the last valid destination snapshot.");
						refreshLogicDestination = false;
					}
				}
			}
			bindFirstPassUavs();
		}
	}
	else if (useStencilTwoPass)
	{
		issueDraw(0, hostIndexCount, 0, instanceCount);
		recordedNativeDraws = instanceCount != 0 ? 1 : 0;
		SetD3D12PipelineState(backStencilPipeline);
		SetD3D12StencilRef(selectedStencilState.backRef, selectedStencilState.backRef);
		bindNullUavs();
		issueDraw(0, hostIndexCount, 0, instanceCount);
		if (instanceCount != 0)
			++recordedNativeDraws;
		bindFirstPassUavs();
	}
	else
	{
		issueDraw(0, hostIndexCount, 0, instanceCount);
		recordedNativeDraws = instanceCount != 0 ? 1 : 0;
	}
#if defined(CEMU_UWP_D3D12_DIAGNOSTICS)
	if (geometryProbeIndex >= 0)
		m_geometryProbe->End(commandList, geometryProbeIndex, m_nextFenceValue);
#endif
	switchProfileStage(UwpCpuDrawStage::RetirementReadbackAndSubmit);
	if (recordedNativeDraws != 0 && !noRasterizationOutput && m_activeFBO)
	{
		const uint32 pixelOutputMask = pixelShader ? pixelShader->pixelColorOutputMask : 0u;
		const auto& attachments = m_activeFBO->GetColorAttachments();
		for (uint32 i = 0; i < attachments.size() && i < 8; ++i)
		{
			const bool shaderWritesTarget = (pixelOutputMask & (1u << i)) != 0;
			const bool targetWriteEnabled = ((effectiveRenderTargetMask >> (i * 4)) & 0xFu) != 0;
			if (shaderWritesTarget && targetWriteEnabled && attachments[i].valid && attachments[i].view)
				MarkTextureViewWritten(attachments[i].view);
		}
		if (m_activeFBO->HasDepthStencilAttachment() && m_activeFBO->depthBuffer.texture)
		{
			const auto& depthControl = RecordRegs().DB_DEPTH_CONTROL;
			const bool depthWriteEnabled = depthControl.get_Z_ENABLE() && depthControl.get_Z_WRITE_ENABLE();
			const bool stencilWriteEnabled = m_activeFBO->depthBuffer.hasStencil &&
				depthControl.get_STENCIL_ENABLE() &&
				(selectedStencilState.frontWriteMask | selectedStencilState.backWriteMask) != 0;
			if (depthWriteEnabled || stencilWriteEnabled)
			{
				MarkTextureViewWritten(static_cast<LatteTextureViewD3D12*>(m_activeFBO->depthBuffer.texture),
					stencilWriteEnabled, depthWriteEnabled);
			}
		}
	}
	D3D12TraceEventFlush("draw_submit", "draw={} pipeline=0x{:016x} hostIndexType={} hostIndexCount={} instances={} primitive={} d3dTopology={} fbo=0x{:016x} noRaster={} pointGS={} rectOrQuad={} descriptorRollovers={} resourceCacheHitMiss={}/{} samplerCacheHitMiss={}/{}",
						 (unsigned long long)nextDrawId,
						 identity.pipelineStateHash,
						 (uint32)hostIndexType,
						 hostIndexCount,
						 instanceCount,
						 (uint32)primitiveMode,
						 (uint32)d3dPrimitiveTopology,
						 m_activeFBO ? m_activeFBO->key : 0,
						 noRasterizationOutput ? 1 : 0,
						 usesPointExpansionGS ? 1 : 0,
						 (primitiveMode == Latte::LATTE_VGT_PRIMITIVE_TYPE::E_PRIMITIVE_TYPE::RECTS || primitiveMode == Latte::LATTE_VGT_PRIMITIVE_TYPE::E_PRIMITIVE_TYPE::QUADS) ? 1 : 0,
						 m_descriptorManager ? m_descriptorManager->GetShaderVisibleRollovers() : 0,
						 (unsigned long long)m_gameResourceTableCacheHits,
						 (unsigned long long)m_gameResourceTableCacheMisses,
						 (unsigned long long)m_gameSamplerTableCacheHits,
						 (unsigned long long)m_gameSamplerTableCacheMisses);
	if (D3D12ShouldTrace() && !noRasterizationOutput && m_activeFBO)
	{
		const std::string sourceSummary = BuildShaderTextureSourceSummary(vertexShader, geometryShader, pixelShader);
		const auto& colorAttachments = m_activeFBO->GetColorAttachments();
		for (uint32 i = 0; i < colorAttachments.size(); ++i)
		{
			const auto& attachment = colorAttachments[i];
			if (!attachment.valid || !attachment.view)
				continue;
			RecordTextureLineage("draw", attachment.view, nextDrawId, identity.pipelineStateHash, sourceSummary,
								 fmt::format("primitive={} d3dTopology={} rtIndex={} hostIndexType={} hostIndexCount={} viewport={},{}:{}x{} scissor={},{}-{},{}",
											 (uint32)primitiveMode,
											 (uint32)d3dPrimitiveTopology,
											 i,
											 (uint32)hostIndexType,
											 hostIndexCount,
											 m_currentViewport.TopLeftX,
											 m_currentViewport.TopLeftY,
											 m_currentViewport.Width,
											 m_currentViewport.Height,
											 m_currentScissor.left,
											 m_currentScissor.top,
											 m_currentScissor.right,
											 m_currentScissor.bottom));
		}
		if (m_activeFBO->HasDepthStencilAttachment() && m_activeFBO->depthBuffer.texture)
		{
			const auto& depthControl = RecordRegs().DB_DEPTH_CONTROL;
			const bool depthWriteEnabled = depthControl.get_Z_ENABLE() && depthControl.get_Z_WRITE_ENABLE();
			const bool stencilWriteEnabled = m_activeFBO->depthBuffer.hasStencil &&
				depthControl.get_STENCIL_ENABLE() &&
				selectedStencilState.writeMask != 0;
			if (depthWriteEnabled || stencilWriteEnabled)
			{
				RecordTextureLineage("draw_depth", static_cast<LatteTextureViewD3D12*>(m_activeFBO->depthBuffer.texture), nextDrawId, identity.pipelineStateHash, sourceSummary,
									 fmt::format("primitive={} hostIndexType={} hostIndexCount={} zWrite={} stencilWrite={} stencilMask=0x{:02x}",
												 (uint32)primitiveMode,
												 (uint32)hostIndexType,
												 hostIndexCount,
												 depthWriteEnabled ? 1 : 0,
												 stencilWriteEnabled ? 1 : 0,
												 (uint32)selectedStencilState.writeMask));
			}
			else
			{
				D3D12TraceEvent("draw_depth_readonly",
								"draw={} pipeline=0x{:016x} primitive={} hostIndexType={} hostIndexCount={} zEnable={} zWrite={} stencilEnable={} stencilMask=0x{:02x}",
								(unsigned long long)nextDrawId,
								identity.pipelineStateHash,
								(uint32)primitiveMode,
								(uint32)hostIndexType,
								hostIndexCount,
								depthControl.get_Z_ENABLE() ? 1 : 0,
								depthControl.get_Z_WRITE_ENABLE() ? 1 : 0,
								depthControl.get_STENCIL_ENABLE() ? 1 : 0,
								(uint32)selectedStencilState.writeMask);
			}
		}
	}

#if defined(CEMU_UWP_D3D12_WORKLOAD_PROFILE)
	auto& workload = m_uwpWorkloadProfile;
	// diagnostic state only, never a rendering predicate
	static std::array<std::pair<uint64, uint32>, 32> s_orObservedPipelines{};
	static uint32 s_orObservedPipelineCount = 0;
	static uint32 s_orPassRecordCount = 0;
	if (m_logicOpEmulationMode == 2 && useOrderedPrimitiveFallback && m_activeFBO &&
		s_orPassRecordCount < s_orObservedPipelines.size())
	{
		auto observed = std::find_if(s_orObservedPipelines.begin(),
			s_orObservedPipelines.begin() + s_orObservedPipelineCount,
			[&](const auto& entry) { return entry.first == identity.pipelineStateHash; });
		const bool firstObservation = observed == s_orObservedPipelines.begin() + s_orObservedPipelineCount;
		if (firstObservation || recordedNativeDraws > observed->second)
		{
			if (firstObservation)
				++s_orObservedPipelineCount;
			*observed = { identity.pipelineStateHash, recordedNativeDraws };
			const uint32 record = ++s_orPassRecordCount;
			const auto& lcr = RecordRegs();
			const auto& depth = lcr.DB_DEPTH_CONTROL;
			const bool hasDepth = m_activeFBO->HasDepthStencilAttachment();
			const bool hasStencil = hasDepth && m_activeFBO->GetStencilFormat() != DXGI_FORMAT_UNKNOWN;
			const uint32 outputMask = pixelShader ? pixelShader->pixelColorOutputMask : 0;
			const uint32 targetMask = GetD3D12EffectiveRenderTargetMask(lcr);
			sint32 sourceContainsDiscard = -1;
			if (pixelShader && pixelShader->strBuf_shaderSource && pixelShader->strBuf_shaderSource->getLen() != 0)
			{
				const std::string_view source(pixelShader->strBuf_shaderSource->c_str(), pixelShader->strBuf_shaderSource->getLen());
				sourceContainsDiscard = source.find("discard") != std::string_view::npos ? 1 : 0;
			}
			cemuLog_log(LogType::Force,
				"D3D12 UWP OR pass: record={} pipeline={:016x} native={} indices={} instances={} primitive={} vs={:016x} ps={:016x} gs={:016x} streamout={} stencilSplit={} depthExport={} sourceDiscard={} customPS={} psOut=0x{:02x} targetMask=0x{:08x} scratchEntries={} activeOcclusionQueries={}.",
				record, identity.pipelineStateHash, recordedNativeDraws, hostIndexCount, instanceCount, (uint32)primitiveMode,
				vertexShader ? vertexShader->baseHash : 0, pixelShader ? pixelShader->baseHash : 0,
				geometryShader ? geometryShader->baseHash : 0, streamoutEnable, useStencilTwoPass,
				pixelShader ? pixelShader->depthMask : false, sourceContainsDiscard,
				pixelShader ? pixelShader->isCustomShader : false, outputMask, targetMask, m_feedbackScratchCopies.size(),
				m_activeOcclusionQueries.size());
			cemuLog_log(LogType::Force,
				"D3D12 UWP OR raster: record={} viewportValid={} viewport=({}, {}) {}x{} depth={}..{} scissorValid={} scissor=({}, {})-({}, {}) depthAttached={} zEnable={} zWrite={} zFunc={} stencilAttached={} stencilEnable={} backEnable={} stencilFuncF/B={}/{} opsF_fail/zfail/zpass={}/{}/{} opsB={}/{}/{} masksF_read/write=0x{:02x}/0x{:02x} masksB=0x{:02x}/0x{:02x} refsF/B={}/{} alphaTest={} alphaFunc={} alphaRefRaw=0x{:08x}.",
				record, m_boundViewportValid, m_boundViewport.TopLeftX, m_boundViewport.TopLeftY,
				m_boundViewport.Width, m_boundViewport.Height, m_boundViewport.MinDepth, m_boundViewport.MaxDepth,
				m_boundScissorValid, m_boundScissor.left, m_boundScissor.top, m_boundScissor.right, m_boundScissor.bottom,
				hasDepth, depth.get_Z_ENABLE(), depth.get_Z_WRITE_ENABLE(), (uint32)depth.get_Z_FUNC(),
				hasStencil, depth.get_STENCIL_ENABLE(), depth.get_BACK_STENCIL_ENABLE(),
				(uint32)depth.get_STENCIL_FUNC_F(), (uint32)depth.get_STENCIL_FUNC_B(),
				(uint32)depth.get_STENCIL_FAIL_F(), (uint32)depth.get_STENCIL_ZFAIL_F(), (uint32)depth.get_STENCIL_ZPASS_F(),
				(uint32)depth.get_STENCIL_FAIL_B(), (uint32)depth.get_STENCIL_ZFAIL_B(), (uint32)depth.get_STENCIL_ZPASS_B(),
				selectedStencilState.frontReadMask, selectedStencilState.frontWriteMask,
				selectedStencilState.backReadMask, selectedStencilState.backWriteMask,
				selectedStencilState.frontRef, selectedStencilState.backRef,
				lcr.SX_ALPHA_TEST_CONTROL.get_ALPHA_TEST_ENABLE(), (uint32)lcr.SX_ALPHA_TEST_CONTROL.get_ALPHA_FUNC(),
				lcr.SX_ALPHA_REF.getRawValue());
			const auto& attachments = m_activeFBO->GetColorAttachments();
			for (uint32 target = 0; target < attachments.size() && target < 8; ++target)
			{
				const auto& attachment = attachments[target];
				if (!attachment.valid || !attachment.view || (outputMask & (1u << target)) == 0 ||
					((targetMask >> (target * 4)) & 0xF) == 0)
					continue;
				auto* texture = attachment.view->GetBaseImage();
				if (!texture)
					continue;
				const auto& resourceDesc = texture->GetDesc();
				const auto* scratch = GetLogicOpScratchCopy(target);
				cemuLog_log(LogType::Force,
					"D3D12 UWP OR target: record={} target={} resourceFormat={} rtvFormat={} scratchSrvFormat={} resourceSize={}x{} arrayDepth={} mips={} samples={} viewMip={}+{} viewSlice={}+{} scratchSize={}x{} writeMask=0x{:x}.",
					record, target, (uint32)resourceDesc.Format, (uint32)attachment.format,
					scratch ? (uint32)scratch->srvDesc.Format : 0,
					resourceDesc.Width, resourceDesc.Height, resourceDesc.DepthOrArraySize, resourceDesc.MipLevels,
					resourceDesc.SampleDesc.Count, attachment.view->firstMip, attachment.view->numMip,
					attachment.view->firstSlice, attachment.view->numSlice,
					scratch ? scratch->requestedDesc.Width : 0, scratch ? scratch->requestedDesc.Height : 0,
					(targetMask >> (target * 4)) & 0xF);
			}
		}
	}
	++workload.guestDraws;
	workload.nativeDraws += recordedNativeDraws;
	workload.orderedDraws += useOrderedPrimitiveFallback ? 1u : 0u;
	workload.stencilDraws += useStencilTwoPass ? 1u : 0u;
	if (m_logicOpEmulationMode < workload.modeDraws.size())
	{
		++workload.modeDraws[m_logicOpEmulationMode];
		workload.modeNativeDraws[m_logicOpEmulationMode] += recordedNativeDraws;
	}
	if (recordedNativeDraws > workload.maxNativeDraws)
	{
		workload.maxNativeDraws = recordedNativeDraws;
		workload.maxDrawPipeline = identity.pipelineStateHash;
		workload.maxDrawVs = vertexShader ? vertexShader->baseHash : 0;
		workload.maxDrawPs = pixelShader ? pixelShader->baseHash : 0;
		workload.maxDrawMode = m_logicOpEmulationMode;
		workload.maxDrawIndices = hostIndexCount;
		workload.maxDrawInstances = instanceCount;
	}
#endif
	// the submit budget counts guest draws; ordered replays split one draw into many native draws
	m_recordedDrawcalls += useOrderedPrimitiveFallback ? std::min<uint32>(recordedNativeDraws, 1u) : recordedNativeDraws;
	m_gameDrawCallsSubmitted++;
	if (noRasterizationOutput)
		m_noRasterStreamoutDraws++;
	if (usesPointExpansionGS)
		m_pointExpansionDraws++;
	if (!noRasterizationOutput)
		LogActiveRenderTargetStats();
	if (!noRasterizationOutput)
		TraceFboAttachments("draw_fbo_attachment_after", "after_draw", m_gameDrawCallsSubmitted, identity.pipelineStateHash, "call=draw_execute");
	static uint32 s_gameDrawSubmittedLogCount = 0;
	if (D3D12RuntimeLogCapped(s_gameDrawSubmittedLogCount, "game draw submitted", 96))
	{
		cemuLog_log(LogType::Force, "D3D12: submitted game draw #{} hostIndexCount={} hostIndexType={} instances={} pipeline=0x{:016x}.",
					(unsigned long long)m_gameDrawCallsSubmitted,
					hostIndexCount,
					(uint32)hostIndexType,
					instanceCount,
					identity.pipelineStateHash);
	}
	const bool scratchBudgetReached = IsCommandScratchBudgetReached();
	if (m_recordedDrawcalls >= m_submitThreshold || scratchBudgetReached)
		ExecuteCurrentCommandList((m_recordedDrawcalls >= m_submitThreshold ? SubmitDrawBudget : 0u) |
			(scratchBudgetReached ? SubmitScratchBudget : 0u));
	};
	s_uwpDrawClosureBytes = sizeof(recordDraw);
	uwpHeadLap(HeadClosure);
	if (queueTail)
	{
		const size_t deltaBytes = RecordDeltaBytes();
		QueueRecordingWithData(deltaBytes, [fetchShader](uint8* data)
		{
			const uint64 fillStart = UwpProfTick();
			WriteRecordDelta(data, fetchShader);
			s_uwpHeadTicks[HeadDeltaWrite] += UwpProfTick() - fillStart;
		}, std::move(recordDraw));
	}
	else
		recordDraw(nullptr);
	uwpHeadLap(HeadQueue);

	// Latte-side bookkeeping once the draw is queued: render targets count as GPU-written, and a readback that
	// started needs its command list submitted
	if (pixelShader && !noRasterizationOutput)
		LatteRenderTarget_trackUpdates();
	if (LatteTextureReadback_Update())
	{
		auto submitReadback = [this]() { ExecuteCurrentCommandList(SubmitReadback); };
		if (ShouldQueueRecording())
			QueueRecording(submitReadback, RecordOpKind::Command);
		else
			submitReadback();
	}
	uwpHeadLap(HeadPost);
}

void D3D12Renderer::draw_endSequence()
{
	m_drawSequenceActive = false;
}

bool D3D12Renderer::EnsureIndexResident(D3D12IndexAllocation& allocation, uint32 dataSize)
{
	if (!m_commandListOpen || dataSize == 0 || dataSize > allocation.size ||
		allocation.GetDecodedCapacity() < dataSize)
		return false;

	if (allocation.residentResource && allocation.residentFence == m_nextFenceValue &&
		allocation.residentSize == dataSize)
	{
		++m_uwpBufferProfile.indexRingSameFenceHits;
		return true;
	}

	// never carry a resident pointer from a completed command list into a later one
	allocation.residentResource = nullptr;
	allocation.residentOffset = 0;
	allocation.residentFence = 0;
	allocation.residentSize = 0;
	const auto upload = m_memoryManager->AllocateUpload(dataSize, 32, m_nextFenceValue);
	if (!upload.IsValid())
		return false;

	memcpy(upload.cpuPtr, allocation.GetDecodedData(), dataSize);
	allocation.residentResource = upload.resource;
	allocation.residentOffset = upload.offset;
	allocation.residentFence = m_nextFenceValue;
	allocation.residentSize = dataSize;
	++m_uwpBufferProfile.indexRingUploads;
	m_uwpBufferProfile.indexRingUploadBytes += dataSize;
	D3D12TraceEvent("index_ring_resident",
		"allocation={} size={} resource={} offset={} fence={} bufferIndex={}",
		(const void*)&allocation,
		dataSize,
		(const void*)upload.resource,
		(unsigned long long)upload.offset,
		(unsigned long long)m_nextFenceValue,
		upload.bufferIndex);
	return true;
}

namespace
{
	// index allocations are taken on the Latte thread and released on the recorder: a new object plus a zero-filled
	// vector per draw, freed on the other thread, cost ~1.5 us per decoded draw (~9 % of the Latte thread in Kakariko).
	// Free allocations are kept by power-of-two capacity
	class IndexAllocationPool
	{
	public:
		using Allocation = D3D12Renderer::D3D12IndexAllocation;
		static constexpr uint32 kMinShift = 6;
		static constexpr uint32 kMaxShift = 20;
		static constexpr size_t kMaxPooledBytes = 32 * 1024 * 1024;

		Allocation* Take(uint32 size)
		{
			const uint32 shift = ShiftFor(size);
			if (shift <= kMaxShift)
			{
				std::scoped_lock lock(m_mutex);
				auto& list = m_free[shift - kMinShift];
				if (!list.empty())
				{
					Allocation* allocation = list.back();
					list.pop_back();
					m_pooledBytes -= allocation->GetDecodedCapacity();
					return allocation;
				}
			}
			auto* allocation = new (std::nothrow) Allocation();
			if (!allocation)
				return nullptr;
			const size_t bytes = shift <= kMaxShift ? ((size_t)1 << shift) : (size_t)size;
			const size_t blocks = (bytes + sizeof(Allocation::DecodedIndexBlock) - 1) / sizeof(Allocation::DecodedIndexBlock);
			allocation->decodedData.reset(new (std::nothrow) Allocation::DecodedIndexBlock[blocks]);
			if (!allocation->decodedData)
			{
				delete allocation;
				return nullptr;
			}
			allocation->decodedBlocks = blocks;
			return allocation;
		}

		void Give(Allocation* allocation)
		{
			const size_t capacity = allocation->GetDecodedCapacity();
			const uint32 shift = ShiftFor((uint32)std::min<size_t>(capacity, UINT32_MAX));
			if (shift > kMaxShift || ((size_t)1 << shift) != capacity)
			{
				delete allocation;
				return;
			}
			allocation->previewDataSize = 0;
			allocation->residentResource = nullptr;
			allocation->residentOffset = 0;
			allocation->residentFence = 0;
			allocation->residentSize = 0;
			allocation->size = 0;
			{
				std::scoped_lock lock(m_mutex);
				if (m_pooledBytes + capacity <= kMaxPooledBytes)
				{
					m_free[shift - kMinShift].push_back(allocation);
					m_pooledBytes += capacity;
					return;
				}
			}
			delete allocation;
		}

		~IndexAllocationPool()
		{
			for (auto& list : m_free)
				for (Allocation* allocation : list)
					delete allocation;
		}

	private:
		static uint32 ShiftFor(uint32 size)
		{
			uint32 shift = kMinShift;
			while (shift <= kMaxShift && ((size_t)1 << shift) < size)
				++shift;
			return shift;
		}

		std::mutex m_mutex;
		std::array<std::vector<Allocation*>, kMaxShift - kMinShift + 1> m_free;
		size_t m_pooledBytes = 0;
	};
	IndexAllocationPool s_indexAllocationPool;
}

D3D12Renderer::IndexAllocation D3D12Renderer::indexData_reserveIndexMemory(uint32 size)
{
	++m_uwpBufferProfile.indexReserves;
	m_uwpBufferProfile.indexRequestedBytes += size;
	auto* allocation = s_indexAllocationPool.Take(size);
	if (!allocation)
		return {};
	allocation->size = size;
	D3D12TraceEvent("index_reserve", "size={} allocation={} cpuPtr={} storage=cpu_staging",
		size,
		(const void*)allocation,
		(const void*)allocation->GetDecodedData());
	return { allocation->GetDecodedData(), allocation };
}

void D3D12Renderer::indexData_releaseIndexMemory(IndexAllocation& allocation)
{
	if (ShouldQueueRecording())
	{
		IndexAllocation queued = allocation;
		allocation = {};
		QueueRecording([this, queued]() mutable { indexData_releaseIndexMemory(queued); }, RecordOpKind::Command);
		return;
	}
	auto* d3dAllocation = static_cast<D3D12IndexAllocation*>(allocation.rendererInternal);
	if (d3dAllocation)
	{
		D3D12TraceEvent("index_release",
			"allocation={} size={} cpuPtr={} residentResource={} residentOffset={} residentFence={} storage=cpu_staging",
			(const void*)d3dAllocation,
			d3dAllocation->size,
			(const void*)d3dAllocation->GetDecodedData(),
			(const void*)d3dAllocation->residentResource,
			(unsigned long long)d3dAllocation->residentOffset,
			(unsigned long long)d3dAllocation->residentFence);
		s_indexAllocationPool.Give(d3dAllocation);
	}
	allocation = {};
}

void D3D12Renderer::indexData_uploadIndexMemory(IndexAllocation& allocation)
{
	if (ShouldQueueRecording())
	{
		IndexAllocation queued = allocation;
		QueueRecording([this, queued]() mutable { indexData_uploadIndexMemory(queued); }, RecordOpKind::Command);
		return;
	}
	auto* d3dAllocation = static_cast<D3D12IndexAllocation*>(allocation.rendererInternal);
	if (!d3dAllocation || d3dAllocation->GetDecodedCapacity() < d3dAllocation->size)
		return;
	// a completed decode changes the CPU source; force the next draw to re-upload
	d3dAllocation->residentResource = nullptr;
	d3dAllocation->residentOffset = 0;
	d3dAllocation->residentFence = 0;
	d3dAllocation->residentSize = 0;
	const uint8* decodedData = d3dAllocation->GetDecodedData();
	ID3D12Resource* traceBuffer = d3dAllocation->residentResource;
	if (D3D12ShouldTrace())
	{
		d3dAllocation->previewDataSize = (uint32)std::min<size_t>(d3dAllocation->previewData.size(), d3dAllocation->size);
		if (d3dAllocation->previewDataSize != 0)
			memcpy(d3dAllocation->previewData.data(), decodedData, d3dAllocation->previewDataSize);
		uint32 firstBytes[4]{};
		if (d3dAllocation->size != 0)
			memcpy(firstBytes, decodedData, std::min<size_t>(d3dAllocation->size, sizeof(firstBytes)));
		D3D12TraceEvent("index_upload", "allocation={} size={} buffer={} firstBytes=0x{:08x},0x{:08x},0x{:08x},0x{:08x}",
						(const void*)d3dAllocation,
						d3dAllocation->size,
						(const void*)traceBuffer,
						firstBytes[0],
						firstBytes[1],
						firstBytes[2],
						firstBytes[3]);
	}
}

LatteQueryObject* D3D12Renderer::occlusionQuery_create()
{
	// the object holds no recording state until begin(), which is queued behind this
	auto* query = new LatteQueryObjectD3D12(this);
	if (ShouldQueueRecording())
	{
		QueueRecording([this, query]() { m_activeOcclusionQueries.emplace_back(query); });
		return query;
	}
	m_activeOcclusionQueries.emplace_back(query);
	return query;
}

void D3D12Renderer::occlusionQuery_destroy(LatteQueryObject* queryObj)
{
	D3D12_QUEUE_RECORDING_KIND(Sync, occlusionQuery_destroy(queryObj));
	auto* d3dQuery = static_cast<LatteQueryObjectD3D12*>(queryObj);
	m_activeOcclusionQueries.erase(std::remove(m_activeOcclusionQueries.begin(), m_activeOcclusionQueries.end(), d3dQuery), m_activeOcclusionQueries.end());
	delete d3dQuery;
}

void D3D12Renderer::occlusionQuery_flush()
{
	D3D12_EXCLUSIVE_RECORDING();
	if (m_commandListOpen && GetCurrentCommandSlot().queryFragmentCount != 0)
		ExecuteCurrentCommandList(SubmitQueryFlush);
	if (m_lastQueryFenceValue != 0 && !WaitForFenceValue(m_lastQueryFenceValue, "occlusion query flush"))
		throw std::runtime_error("D3D12 device stopped while flushing occlusion queries");
	ProcessFinishedCommandLists();
}

void D3D12Renderer::occlusionQuery_updateState()
{
	D3D12_QUEUE_RECORDING_KIND(Command, occlusionQuery_updateState());
	ProcessFinishedCommandLists();
	for (auto* query : m_activeOcclusionQueries)
	{
		if (query)
			query->handleFinishedFragments();
	}
}

D3D12Renderer::SwapchainState& D3D12Renderer::GetSwapchain(bool mainWindow)
{
	return mainWindow ? m_mainSwapchain : m_padSwapchain;
}

const D3D12Renderer::SwapchainState& D3D12Renderer::GetSwapchain(bool mainWindow) const
{
	return mainWindow ? m_mainSwapchain : m_padSwapchain;
}
