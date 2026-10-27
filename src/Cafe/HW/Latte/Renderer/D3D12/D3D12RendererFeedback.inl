// Part of D3D12Renderer.cpp (included there, same translation unit): feedback-loop copies, logic op emulation and framebuffer binding.
// Not a standalone file: it relies on the includes and file-local helpers of D3D12Renderer.cpp.

void D3D12Renderer::ResolveFeedbackLoopsForDraw(LatteDecompilerShader* vertexShader,
	LatteDecompilerShader* geometryShader, LatteDecompilerShader* pixelShader, bool renderTargetsBound)
{
	FeedbackPlan plan;
	PlanFeedbackForDraw(vertexShader, geometryShader, pixelShader, renderTargetsBound, plan);
	EmitFeedbackForDraw(plan);
}

void D3D12Renderer::EmitFeedbackForDraw(const FeedbackPlan& plan)
{
	constexpr D3D12_RESOURCE_STATES kSampled = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
	for (uint32 i = 0; i < plan.count; i++)
	{
		LatteTextureViewD3D12* view = plan.view[i];
		switch (plan.action[i])
		{
		case FeedbackPlan::ProxyTransition:
			Transition(view->m_samplingProxy.resource.Get(), view->m_samplingProxy.state, kSampled);
			break;
		case FeedbackPlan::Transition:
			TransitionTextureView(view, kSampled);
			break;
		default:
			// depth: reuse the previous copy while the depth buffer was not written since (same data as a fresh copy)
			CreateFeedbackScratchCopy(plan.unit[i], view, plan.action[i] == FeedbackPlan::ColorCopy, plan.action[i] == FeedbackPlan::DepthCopy);
			break;
		}
	}
}

void D3D12Renderer::PlanFeedbackForDraw(LatteDecompilerShader* vertexShader,
	LatteDecompilerShader* geometryShader, LatteDecompilerShader* pixelShader, bool renderTargetsBound, FeedbackPlan& plan)
{
	plan.count = 0;
	plan.anyCopy = false;
	if (!m_memoryManager || !m_descriptorManager)
		return;
	const bool scanAttachments = renderTargetsBound && m_activeFBO;
	auto addAction = [&plan](uint32 textureUnit, LatteTextureViewD3D12* view, uint8 action)
	{
		plan.action[plan.count] = action;
		plan.unit[plan.count] = (uint8)textureUnit;
		plan.view[plan.count] = view;
		plan.count++;
		plan.anyCopy |= action == FeedbackPlan::ColorCopy || action == FeedbackPlan::DepthCopy;
	};

	static uint32 s_feedbackScanLogCount = 0;
	auto resolveTextureUnit = [&](uint32 textureUnit)
	{
		auto* sampledView = m_boundTextures[textureUnit];
		if (!sampledView || !sampledView->GetSRV().IsValid())
			return;

		// the sampling proxy is a separate resource and cannot alias a guest attachment
		if (sampledView->UsesSamplingProxy())
		{
			m_feedbackOverlapCache[textureUnit] = {};
			addAction(textureUnit, sampledView, FeedbackPlan::ProxyTransition);
			return;
		}

		if (!scanAttachments)
		{
			addAction(textureUnit, sampledView, FeedbackPlan::Transition);
			return;
		}

		bool hasFeedback = false;
		LatteTextureViewD3D12* feedbackAttachment = nullptr;
		auto& overlapCache = m_feedbackOverlapCache[textureUnit];
		if (overlapCache.valid && overlapCache.sampledView == sampledView &&
			overlapCache.fbo == m_activeFBO && overlapCache.fboKey == m_activeFBO->key)
		{
			hasFeedback = overlapCache.hasFeedback;
			feedbackAttachment = overlapCache.attachment;
			m_feedbackOverlapCacheHits++;
		}
		else
		{
			for (const auto& attachment : m_activeFBO->GetColorAttachments())
			{
				if (attachment.valid && TextureViewSubresourcesOverlap(sampledView, attachment.view))
				{
					hasFeedback = true;
					feedbackAttachment = attachment.view;
					break;
				}
			}

			if (!hasFeedback && m_activeFBO->HasDepthStencilAttachment())
			{
				auto* depthView = static_cast<LatteTextureViewD3D12*>(m_activeFBO->depthBuffer.texture);
				hasFeedback = TextureViewSubresourcesOverlap(sampledView, depthView);
				if (hasFeedback)
					feedbackAttachment = depthView;
			}
			overlapCache = { sampledView, m_activeFBO, m_activeFBO->key, feedbackAttachment, hasFeedback, true };
			m_feedbackOverlapCacheMisses++;
		}

		if (D3D12RuntimeLogCapped(s_feedbackScanLogCount, "feedback scan", 128))
		{
			auto* sampledTexture = sampledView->GetBaseImage();
			auto* attachmentTexture = feedbackAttachment ? feedbackAttachment->GetBaseImage() : nullptr;
			cemuLog_log(LogType::Force, "D3D12: feedback scan unit={} sampled=0x{:08x}/mip{}+{}/slice{}+{} attachment=0x{:08x}/mip{}+{}/slice{}+{} feedback={}.",
						textureUnit,
						sampledTexture ? sampledTexture->physAddress : 0,
						sampledView->firstMip,
						sampledView->numMip,
						sampledView->firstSlice,
						sampledView->numSlice,
						attachmentTexture ? attachmentTexture->physAddress : 0,
						feedbackAttachment ? feedbackAttachment->firstMip : 0,
						feedbackAttachment ? feedbackAttachment->numMip : 0,
						feedbackAttachment ? feedbackAttachment->firstSlice : 0,
						feedbackAttachment ? feedbackAttachment->numSlice : 0,
						hasFeedback);
		}

		if (!hasFeedback)
		{
			addAction(textureUnit, sampledView, FeedbackPlan::Transition);
			return;
		}

		m_feedbackLoopDetections++;
		const bool colorFeedback = !m_activeFBO->HasDepthStencilAttachment() ||
			feedbackAttachment != static_cast<LatteTextureViewD3D12*>(m_activeFBO->depthBuffer.texture);
		addAction(textureUnit, sampledView, colorFeedback ? FeedbackPlan::ColorCopy : FeedbackPlan::DepthCopy);
	};

	// bindings persist across shader changes; walk the same unit lists as the SRV tables
	if (PerformanceExperimentEnabled(D3D12PerformanceExperiment::ActiveFeedbackUnits))
	{
		D3D12ActiveTextureUnits::OrderedSet<kMaxTextureBindings> usedTextureUnits;
		auto addStageTextureUnits = [&](LatteDecompilerShader* shader,
			LatteConst::ShaderType expectedType, uint32 hostBase)
		{
			if (!shader)
				return;
			if (shader->shaderType != expectedType)
			{
				usedTextureUnits.IncludeAll();
				return;
			}
			usedTextureUnits.AddStage(shader->textureUnitList, shader->textureUnitListCount,
				hostBase, LATTE_NUM_MAX_TEX_UNITS);
		};
		if (!vertexShader)
			usedTextureUnits.IncludeAll();
		addStageTextureUnits(vertexShader, LatteConst::ShaderType::Vertex, LATTE_CEMU_VS_TEX_UNIT_BASE);
		addStageTextureUnits(geometryShader, LatteConst::ShaderType::Geometry, LATTE_CEMU_GS_TEX_UNIT_BASE);
		addStageTextureUnits(pixelShader, LatteConst::ShaderType::Pixel, LATTE_CEMU_PS_TEX_UNIT_BASE);
		usedTextureUnits.ForEach(resolveTextureUnit);
	}
	else
	{
		std::array<bool, kMaxTextureBindings> usedTextureUnits{};
		auto addStageTextureUnits = [&](LatteDecompilerShader* shader,
			LatteConst::ShaderType expectedType, uint32 hostBase)
		{
			if (!shader)
				return;
			if (shader->shaderType != expectedType)
			{
				usedTextureUnits.fill(true);
				return;
			}
			D3D12TextureUnitMask::AddStage(usedTextureUnits, shader->textureUnitList,
				shader->textureUnitListCount, hostBase, LATTE_NUM_MAX_TEX_UNITS);
		};
		if (!vertexShader)
			usedTextureUnits.fill(true);
		addStageTextureUnits(vertexShader, LatteConst::ShaderType::Vertex, LATTE_CEMU_VS_TEX_UNIT_BASE);
		addStageTextureUnits(geometryShader, LatteConst::ShaderType::Geometry, LATTE_CEMU_GS_TEX_UNIT_BASE);
		addStageTextureUnits(pixelShader, LatteConst::ShaderType::Pixel, LATTE_CEMU_PS_TEX_UNIT_BASE);
		for (uint32 textureUnit = 0; textureUnit < m_boundTextures.size(); ++textureUnit)
			if (usedTextureUnits[textureUnit])
				resolveTextureUnit(textureUnit);
	}
}

uint64 D3D12Renderer::FeedbackSourceGeneration(LatteTextureViewD3D12* view) const
{
	auto* texture = view ? view->GetBaseImage() : nullptr;
	if (!texture)
		return 0;
	const uint32 mipLevels = std::max<uint32>(texture->GetDesc().MipLevels, 1);
	const uint32 arraySize = texture->Is3DTexture() ? 1u : std::max<uint32>(texture->GetArraySize(), 1);
	const uint32 firstMip = (uint32)std::clamp<sint32>(view->firstMip, 0, (sint32)mipLevels - 1);
	const uint32 mipCount = std::min<uint32>(std::max<sint32>(view->numMip, 1), mipLevels - firstMip);
	const uint32 firstSlice = texture->Is3DTexture() ? 0u : (uint32)std::clamp<sint32>(view->firstSlice, 0, (sint32)arraySize - 1);
	const uint32 sliceCount = texture->Is3DTexture() ? 1u : std::min<uint32>(std::max<sint32>(view->numSlice, 1), arraySize - firstSlice);
	// per-subresource counters only grow, so any write between two calls changes the sum
	uint64 sum = 0;
	for (uint32 slice = 0; slice < sliceCount; ++slice)
		for (uint32 mip = 0; mip < mipCount; ++mip)
			sum += texture->GetContentGeneration(firstMip + mip + (firstSlice + slice) * mipLevels);
	return sum;
}

bool D3D12Renderer::CreateFeedbackScratchCopy(uint32 textureUnit, LatteTextureViewD3D12* sampledView, bool retainAcrossDraws, bool reuseWhileSourceUnchanged)
{
	if (!sampledView || !m_memoryManager || !m_descriptorManager)
		return false;
	auto* sourceTexture = sampledView->GetBaseImage();
	if (!sourceTexture || !sourceTexture->GetResource())
		return false;
	const uint64 drawId = m_gameDrawCallsSubmitted + 1;
	if (textureUnit >= m_feedbackScratchByTextureUnit.size())
		return false;
	if (GetFeedbackScratchCopy(textureUnit))
		return true;
	for (uint32 index = 0; index < m_feedbackScratchCopies.size(); ++index)
	{
		const auto& scratch = m_feedbackScratchCopies[index];
		if (scratch.drawId == drawId && scratch.sampledView == sampledView && scratch.texture.IsValid())
		{
			m_feedbackScratchByTextureUnit[textureUnit] = index;
			return true;
		}
	}
	if ((retainAcrossDraws || reuseWhileSourceUnchanged) && TryReuseRetainedFeedbackCopy(textureUnit, sampledView))
	{
		++m_feedbackCopyReuses;
		return true;
	}
	const auto srvDesc = sampledView->BuildSrvDesc();
	if (srvDesc.Format == DXGI_FORMAT_UNKNOWN)
		return false;

	const D3D12_RESOURCE_DESC sourceDesc = sourceTexture->GetDesc();
	D3D12MemoryManager::Resource scratchTexture;
	if (!TryAcquireCommandScratchTexture(sourceDesc, scratchTexture) &&
		!CreateTextureWithRecovery(sourceDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, scratchTexture, "feedback scratch"))
		return false;

	auto* commandList = GetCurrentCommandSlot().commandList.Get();
	Transition(scratchTexture.resource.Get(), scratchTexture.state, D3D12_RESOURCE_STATE_COPY_DEST);
	const uint32 mipLevels = std::max<uint32>(sourceDesc.MipLevels, 1);
	const uint32 arraySize = sourceTexture->Is3DTexture() ? 1u : std::max<uint32>(sourceTexture->GetArraySize(), 1);
	const uint32 firstMip = (uint32)std::clamp<sint32>(sampledView->firstMip, 0, (sint32)mipLevels - 1);
	const uint32 mipCount = std::min<uint32>(std::max<sint32>(sampledView->numMip, 1), mipLevels - firstMip);
	const uint32 firstSlice = sourceTexture->Is3DTexture() ? 0u : (uint32)std::clamp<sint32>(sampledView->firstSlice, 0, (sint32)arraySize - 1);
	const uint32 sliceCount = sourceTexture->Is3DTexture() ? 1u : std::min<uint32>(std::max<sint32>(sampledView->numSlice, 1), arraySize - firstSlice);
	std::vector<TextureTransition> localPreviousStates;
	std::vector<TextureTransition> localCopyStates;
	const bool retainTransitions = PerformanceExperimentEnabled(D3D12PerformanceExperiment::RetainedFeedbackTransitions);
	auto& previousStates = D3D12PerformanceExperiment::SelectTransitionStorage(
		retainTransitions, localPreviousStates, m_feedbackPreviousStateScratch);
	auto& copyStates = D3D12PerformanceExperiment::SelectTransitionStorage(
		retainTransitions, localCopyStates, m_feedbackCopyStateScratch);
	previousStates.reserve((size_t)mipCount * sliceCount);
	copyStates.reserve((size_t)mipCount * sliceCount);
	for (uint32 slice = 0; slice < sliceCount; ++slice)
	{
		for (uint32 mip = 0; mip < mipCount; ++mip)
		{
			const uint32 subresource = firstMip + mip + (firstSlice + slice) * mipLevels;
			previousStates.push_back({ subresource, sourceTexture->GetSubresourceState(subresource) });
			copyStates.push_back({ subresource, D3D12_RESOURCE_STATE_COPY_SOURCE });
		}
	}
	TransitionTextureSubresources(sourceTexture, copyStates);
	for (uint32 slice = 0; slice < sliceCount; ++slice)
	{
		for (uint32 mip = 0; mip < mipCount; ++mip)
		{
			const uint32 subresource = firstMip + mip + (firstSlice + slice) * mipLevels;
			D3D12_TEXTURE_COPY_LOCATION source{};
			source.pResource = sourceTexture->GetResource();
			source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
			source.SubresourceIndex = subresource;
			D3D12_TEXTURE_COPY_LOCATION destination{};
			destination.pResource = scratchTexture.resource.Get();
			destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
			destination.SubresourceIndex = subresource;
			commandList->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
		}
	}
	Transition(scratchTexture.resource.Get(), scratchTexture.state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	TransitionTextureSubresources(sourceTexture, previousStates);

	FeedbackScratchCopy scratch;
	scratch.texture = std::move(scratchTexture);
	scratch.requestedDesc = sourceDesc;
	scratch.srvDesc = srvDesc;
	scratch.sampledView = sampledView;
	scratch.textureUnit = textureUnit;
	scratch.drawId = drawId;
	scratch.retainAcrossDraws = retainAcrossDraws || reuseWhileSourceUnchanged;
	scratch.requireUnchangedSource = reuseWhileSourceUnchanged && !retainAcrossDraws;
	if (scratch.requireUnchangedSource)
		scratch.sourceGeneration = FeedbackSourceGeneration(sampledView);
	D3D12TraceEvent("feedback_copy", "draw={} hostUnit={} addr=0x{:08x} fmt=0x{:04x} size={}x{}x{} mip={}+{} slice={}+{} srvFormat={} sampledView={} scratchResource={} srvLifetime=shader_visible_draw_table",
					(unsigned long long)scratch.drawId,
					textureUnit,
					sourceTexture->physAddress,
					(uint32)sampledView->format,
					sourceTexture->width,
					sourceTexture->height,
					sourceTexture->depth,
					sampledView->firstMip,
					sampledView->numMip,
					sampledView->firstSlice,
					sampledView->numSlice,
					(uint32)srvDesc.Format,
					(const void*)sampledView,
					(const void*)scratch.texture.resource.Get());
	m_feedbackScratchCopies.emplace_back(std::move(scratch));
	m_feedbackScratchByTextureUnit[textureUnit] = (uint32)m_feedbackScratchCopies.size() - 1;
	m_feedbackLoopCopies++;
	return true;
}

const D3D12_FEATURE_DATA_FORMAT_SUPPORT& D3D12Renderer::GetCachedFormatSupport(DXGI_FORMAT format) const
{
	auto it = m_formatSupportCache.find((uint32)format);
	if (it != m_formatSupportCache.end())
		return it->second;

	D3D12_FEATURE_DATA_FORMAT_SUPPORT support{};
	support.Format = format;
	if (m_device)
		m_device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support));
	return m_formatSupportCache.emplace((uint32)format, support).first->second;
}

bool D3D12Renderer::FormatSupportsOutputMergerLogicOp(DXGI_FORMAT format) const
{
	if (!m_device || format == DXGI_FORMAT_UNKNOWN)
		return false;
	return (GetCachedFormatSupport(format).Support2 & D3D12_FORMAT_SUPPORT2_OUTPUT_MERGER_LOGIC_OP) != 0;
}

bool D3D12Renderer::FormatSupportsBlending(DXGI_FORMAT format) const
{
	if (!m_device || format == DXGI_FORMAT_UNKNOWN)
		return false;
	return (GetCachedFormatSupport(format).Support1 & D3D12_FORMAT_SUPPORT1_BLENDABLE) != 0;
}

bool D3D12Renderer::CreateLogicOpScratchCopy(LatteTextureViewD3D12* renderTargetView, uint32 renderTargetIndex)
{
	if (!renderTargetView || !m_memoryManager || !m_descriptorManager)
		return false;
	if (renderTargetIndex >= m_logicOpScratchByTarget.size())
		return false;
	if (GetLogicOpScratchCopy(renderTargetIndex))
		return true;

	auto* sourceTexture = renderTargetView->GetBaseImage();
	if (!sourceTexture || !sourceTexture->GetResource())
		return false;

	const D3D12_SHADER_RESOURCE_VIEW_DESC sourceSrvDesc = renderTargetView->BuildSrvDesc();
	if (sourceSrvDesc.Format == DXGI_FORMAT_UNKNOWN)
		return false;

	if (!m_commandListOpen)
		ResetCommandList();

	const D3D12_RESOURCE_DESC sourceDesc = sourceTexture->GetDesc();
	D3D12_RESOURCE_DESC scratchDesc = sourceDesc;
	D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = sourceSrvDesc;
	// the logic-op HLSL samples a single-mip, single-slice Texture2D<float4>
	if (sourceDesc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
		sourceDesc.SampleDesc.Count != 1 || renderTargetView->numSlice != 1 ||
		!SupportsUnorm8LogicOpEmulation(sourceSrvDesc.Format))
	{
		return false;
	}
	scratchDesc.Width = std::max<UINT64>(1, sourceDesc.Width >> renderTargetView->firstMip);
	scratchDesc.Height = std::max<UINT>(1, sourceDesc.Height >> renderTargetView->firstMip);
	scratchDesc.DepthOrArraySize = 1;
	scratchDesc.MipLevels = 1;
	scratchDesc.SampleDesc = { 1, 0 };
	scratchDesc.Flags = D3D12_RESOURCE_FLAG_NONE;
	srvDesc = {};
	srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srvDesc.Format = sourceSrvDesc.Format;
	srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	srvDesc.Texture2D.MostDetailedMip = 0;
	srvDesc.Texture2D.MipLevels = 1;
	srvDesc.Texture2D.PlaneSlice = 0;
	if (!m_commandListOpen)
		ResetCommandList();
	D3D12MemoryManager::Resource scratchTexture;
	if (!TryAcquireCommandScratchTexture(scratchDesc, scratchTexture) &&
		!CreateTextureWithRecovery(scratchDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, scratchTexture, "logic-op scratch"))
		return false;

	auto* commandList = GetCurrentCommandSlot().commandList.Get();
	Transition(scratchTexture.resource.Get(), scratchTexture.state, D3D12_RESOURCE_STATE_COPY_DEST);
	TransitionTextureView(renderTargetView, D3D12_RESOURCE_STATE_COPY_SOURCE);
	const uint32 sourceSubresource = sourceTexture->GetSubresourceIndex(renderTargetView->firstSlice, renderTargetView->firstMip);
	D3D12_TEXTURE_COPY_LOCATION source{};
	source.pResource = sourceTexture->GetResource();
	source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	source.SubresourceIndex = sourceSubresource;
	D3D12_TEXTURE_COPY_LOCATION destination{};
	destination.pResource = scratchTexture.resource.Get();
	destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	destination.SubresourceIndex = 0;
	commandList->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
#if defined(CEMU_UWP_D3D12_WORKLOAD_PROFILE)
	++m_uwpWorkloadProfile.initialLogicCopies;
	m_uwpWorkloadProfile.initialLogicPayloadBytes += D3D12WorkloadLogicCopyBytes(scratchDesc, srvDesc.Format);
#endif
	Transition(scratchTexture.resource.Get(), scratchTexture.state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	TransitionTextureView(renderTargetView, D3D12_RESOURCE_STATE_RENDER_TARGET);

	FeedbackScratchCopy scratch;
	scratch.texture = std::move(scratchTexture);
	scratch.requestedDesc = scratchDesc;
	scratch.srvDesc = srvDesc;
	scratch.textureUnit = std::numeric_limits<uint32>::max();
	scratch.logicOpRenderTarget = renderTargetIndex;
	scratch.drawId = m_gameDrawCallsSubmitted + 1;
	D3D12TraceEvent("logic_op_scratch_copy",
					"draw={} rt={} src=0x{:08x} fmt=0x{:04x} srcDxgi={} srvDxgi={} mip={} slice={} scratchSize={}x{} srcResource={} scratchResource={} pendingScratchBefore={} srvLifetime=shader_visible_draw_table",
					(unsigned long long)scratch.drawId,
					renderTargetIndex,
					sourceTexture->physAddress,
					(uint32)renderTargetView->format,
					(uint32)sourceDesc.Format,
					(uint32)srvDesc.Format,
					renderTargetView->firstMip,
					renderTargetView->firstSlice,
					(unsigned long long)scratchDesc.Width,
					scratchDesc.Height,
					(const void*)sourceTexture->GetResource(),
					(const void*)scratch.texture.resource.Get(),
					m_feedbackScratchCopies.size());
	m_feedbackScratchCopies.emplace_back(std::move(scratch));
	m_logicOpScratchByTarget[renderTargetIndex] = (uint32)m_feedbackScratchCopies.size() - 1;
	m_logicOpEmulationCopies++;
	return true;
}

bool D3D12Renderer::RefreshLogicOpScratchCopiesForDraw(uint64 drawId)
{
	if (!m_activeFBO || !m_commandListOpen)
		return false;

	auto* commandList = GetCurrentCommandSlot().commandList.Get();
	const auto& attachments = m_activeFBO->GetColorAttachments();
	bool foundScratch = false;
	for (uint32 renderTargetIndex = 0; renderTargetIndex < m_logicOpScratchByTarget.size(); ++renderTargetIndex)
	{
		auto* scratch = GetLogicOpScratchCopy(renderTargetIndex);
		if (!scratch || scratch->drawId != drawId)
			continue;
		if (scratch->logicOpRenderTarget >= attachments.size())
			return false;

		const auto& attachment = attachments[scratch->logicOpRenderTarget];
		auto* view = attachment.valid ? attachment.view : nullptr;
		auto* texture = view ? view->GetBaseImage() : nullptr;
		if (!view || !texture || !texture->GetResource())
			return false;

		// after the full first snapshot only the scissor can change between primitives
		const auto& sourceDesc = texture->GetDesc();
		const bool validMip = view->firstMip >= 0 && view->firstMip < sourceDesc.MipLevels && view->firstMip < 32;
		constexpr bool scissorBoundedRefresh = true;
		const bool boundedAllowed = validMip && m_boundScissorValid && scissorBoundedRefresh &&
			sourceDesc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && sourceDesc.SampleDesc.Count == 1 &&
			(sourceDesc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) == 0 &&
			SupportsUnorm8LogicOpEmulation(scratch->srvDesc.Format);
		const auto region = D3D12LogicOpCopyRegion::Select(boundedAllowed,
			{ m_boundScissor.left, m_boundScissor.top, m_boundScissor.right, m_boundScissor.bottom },
			validMip ? std::max<UINT64>(1, sourceDesc.Width >> view->firstMip) : 0,
			validMip ? std::max<UINT>(1, sourceDesc.Height >> view->firstMip) : 0,
			scratch->requestedDesc.Width, scratch->requestedDesc.Height);
		foundScratch = true;
		if (region.kind == D3D12LogicOpCopyRegion::Kind::Empty)
		{
			continue;
		}
		const uint32 sourceSubresource = texture->GetSubresourceIndex(view->firstSlice, view->firstMip);
		Transition(scratch->texture.resource.Get(), scratch->texture.state, D3D12_RESOURCE_STATE_COPY_DEST);
		TransitionTextureView(view, D3D12_RESOURCE_STATE_COPY_SOURCE);

		D3D12_TEXTURE_COPY_LOCATION source{};
		source.pResource = texture->GetResource();
		source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		source.SubresourceIndex = sourceSubresource;
		D3D12_TEXTURE_COPY_LOCATION destination{};
		destination.pResource = scratch->texture.resource.Get();
		destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		destination.SubresourceIndex = 0;
		const bool partial = region.kind == D3D12LogicOpCopyRegion::Kind::Partial;
		const D3D12_BOX box{ region.left, region.top, 0, region.right, region.bottom, 1 };
		commandList->CopyTextureRegion(&destination, partial ? region.left : 0, partial ? region.top : 0, 0,
			&source, partial ? &box : nullptr);
#if defined(CEMU_UWP_D3D12_WORKLOAD_PROFILE)
		auto payloadDesc = scratch->requestedDesc;
		if (partial)
		{
			payloadDesc.Width = region.right - region.left;
			payloadDesc.Height = region.bottom - region.top;
		}
		const uint64 payloadBytes = D3D12WorkloadLogicCopyBytes(payloadDesc, scratch->srvDesc.Format);
#endif
#if defined(CEMU_UWP_D3D12_WORKLOAD_PROFILE)
		++m_uwpWorkloadProfile.refreshLogicCopies;
		m_uwpWorkloadProfile.refreshLogicPayloadBytes += payloadBytes;
		if (m_logicOpEmulationMode < m_uwpWorkloadProfile.modeRefreshCopies.size())
			++m_uwpWorkloadProfile.modeRefreshCopies[m_logicOpEmulationMode];
#endif

		Transition(scratch->texture.resource.Get(), scratch->texture.state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		TransitionTextureView(view, D3D12_RESOURCE_STATE_RENDER_TARGET);
		foundScratch = true;
	}
	return foundScratch;
}

bool D3D12Renderer::PrepareLogicOpEmulationForDraw()
{
	m_logicOpEmulationMode = 0;
	m_disableNativeLogicOpForDraw = false;

	if (!m_activeFBO)
		return true;

	const auto logicOp = LatteGPUState.contextNew.CB_COLOR_CONTROL.get_ROP();
	D3D12_LOGIC_OP d3dLogicOp = D3D12_LOGIC_OP_COPY;
	if (!TryGetD3D12LogicOp(logicOp, d3dLogicOp))
	{
		cemuLog_logOnce(LogType::Force, "D3D12: unknown GX2 logic operation 0x{:02x} is drawn as COPY.", static_cast<uint32>(logicOp));
		// an enabled logic op still disables blending
		m_disableNativeLogicOpForDraw = true;
		return true;
	}
	const uint32 mode = GetD3D12LogicOpEmulationMode(d3dLogicOp);
	if (mode == 0)
		return true;

	const auto& attachments = m_activeFBO->GetColorAttachments();
	auto* activePixelShader = LatteSHRC_GetActivePixelShader();
	const uint32 outputMask = activePixelShader ? activePixelShader->pixelColorOutputMask : 0u;
	const uint32 targetMask = GetD3D12EffectiveRenderTargetMask(LatteGPUState.contextNew);
	const uint8 nativeWriteMask = static_cast<uint8>(targetMask & 0xF);
	// native logic ops apply slot 0 to every RTV; a sparse FBO has a dummy at slot 0
	bool nativeLogicOpRepresentable =
		m_outputMergerLogicOpSupported &&
		!attachments.empty() &&
		attachments[0].valid;
	std::vector<uint32> logicTargets;
	logicTargets.reserve(std::min<size_t>(attachments.size(), kGameLogicOpDstSrvCount));
	for (uint32 i = 0; i < m_activeFBO->GetColorAttachmentCount() && i < attachments.size() && i < kGameLogicOpDstSrvCount; ++i)
	{
		if (!attachments[i].valid)
			continue;

		const bool shaderWritesTarget = (outputMask & (1u << i)) != 0;
		const uint8 targetWriteMask = static_cast<uint8>((targetMask >> (i * 4)) & 0xF);
		if (!shaderWritesTarget || targetWriteMask == 0)
			continue;
		logicTargets.push_back(i);
		if (!FormatSupportsOutputMergerLogicOp(attachments[i].format))
			nativeLogicOpRepresentable = false;
		if (targetWriteMask != nativeWriteMask)
			nativeLogicOpRepresentable = false;
	}

	if (!m_outputMergerLogicOpSupported)
		cemuLog_logOnce(LogType::Force, "D3D12: output-merger logic ops are not supported by this device; using shader emulation where possible.");
	if (nativeLogicOpRepresentable)
		return true;
	if (logicTargets.empty())
		return true;

	// native logic ops force RenderTarget[0] state onto every MRT
	m_disableNativeLogicOpForDraw = true;

	const bool needsDestination = D3D12LogicOpEmulationNeedsDestination(mode);
	bool hasIgnoringTargets = false;
	bool hasEmulatedTargets = false;
	for (uint32 renderTargetIndex : logicTargets)
	{
		const auto& attachment = attachments[renderTargetIndex];
		if (D3D12LogicOpIgnoresFormat(attachment.format))
		{
			hasIgnoringTargets = true;
			continue;
		}
		if (!attachment.view ||
			(!SupportsUnorm8LogicOpEmulation(attachment.format) && !SupportsLogicOpEmulationWithoutDestination(attachment.format, mode)))
		{
			static uint32 s_unsupportedLogicFormatLogCount = 0;
			if (D3D12RuntimeLogCapped(s_unsupportedLogicFormatLogCount, "unsupported logic-op format", 32))
			{
				cemuLog_log(LogType::Force,
					"D3D12: skipping logic-op draw 0x{:02x}; MRT {} format {} cannot be represented by the UNORM shader fallback.",
					static_cast<uint32>(logicOp), renderTargetIndex, static_cast<uint32>(attachment.format));
			}
			return false;
		}
		hasEmulatedTargets = true;
		if (!needsDestination)
			continue;
		const auto* texture = attachment.view->GetBaseImage();
		if (!texture || texture->GetDesc().Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
			texture->GetDesc().SampleDesc.Count != 1 || attachment.view->numSlice != 1)
		{
			static uint32 s_unsupportedLogicDimensionLogCount = 0;
			if (D3D12RuntimeLogCapped(s_unsupportedLogicDimensionLogCount, "unsupported logic-op dimensions", 32))
			{
				cemuLog_log(LogType::Force,
					"D3D12: skipping logic-op draw 0x{:02x}; MRT {} is not a single-sample, single-slice Texture2D view.",
					static_cast<uint32>(logicOp), renderTargetIndex);
			}
			return false;
		}
	}
	if (!hasEmulatedTargets)
		return true;
	if (hasIgnoringTargets)
	{
		// the shader applies one emulation mode to every color output
		static uint32 s_mixedLogicFormatLogCount = 0;
		if (D3D12RuntimeLogCapped(s_mixedLogicFormatLogCount, "mixed logic-op formats", 32))
		{
			cemuLog_log(LogType::Force,
				"D3D12: skipping logic-op draw 0x{:02x}; it mixes float or sRGB targets with fixed-point targets.",
				static_cast<uint32>(logicOp));
		}
		return false;
	}

	if (!needsDestination)
	{
		m_logicOpEmulationMode = mode;
		m_logicOpEmulatedDraws++;
		return true;
	}

	for (uint32 renderTargetIndex : logicTargets)
	{
		if (!CreateLogicOpScratchCopy(attachments[renderTargetIndex].view, renderTargetIndex))
		{
			static uint32 s_logicScratchFailureLogCount = 0;
			if (D3D12RuntimeLogCapped(s_logicScratchFailureLogCount, "logic-op scratch allocation failure", 32))
			{
				cemuLog_log(LogType::Force,
					"D3D12: skipping logic-op draw 0x{:02x}; destination scratch creation failed for MRT {}.",
					static_cast<uint32>(logicOp), renderTargetIndex);
			}
			return false;
		}
	}

	static uint32 s_logicOpEmulationLogCount = 0;
	if (D3D12RuntimeLogCapped(s_logicOpEmulationLogCount, "logic-op emulation", 64))
		cemuLog_log(LogType::Force, "D3D12: emulating logic op 0x{:02x} across {} MRT target(s) through scratch SRVs t{}..t{}.",
					(uint32)logicOp,
					logicTargets.size(),
					kGameLogicOpDstSrvBinding,
					kGameLogicOpDstSrvBinding + kGameLogicOpDstSrvCount - 1);
	m_logicOpEmulationMode = mode;
	m_logicOpEmulatedDraws++;
	return true;
}

void D3D12Renderer::BindFramebuffer(CachedFBOD3D12* fbo)
{
	if (!m_commandListOpen)
		return;

	if (!fbo)
	{
		SetD3D12RenderTargets(0, nullptr, nullptr);
		return;
	}

	UINT rtvCount = std::min<UINT>(fbo->GetColorAttachmentCount(), 8);
	std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 8> rtvHandles{};
	bool usedNullRtv = false;
	bool missingRealRtv = false;
	{
		const auto& colorAttachments = fbo->GetColorAttachments();
		const auto& fboRtvHandles = fbo->GetRtvHandles();
		for (uint32 i = 0; i < rtvCount && i < colorAttachments.size(); ++i)
		{
			const auto& colorAttachment = colorAttachments[i];
			const bool realRtvValid = colorAttachment.valid && colorAttachment.view && fboRtvHandles[i].ptr != 0;
			if (!realRtvValid)
			{
				missingRealRtv = true;
				if (m_sparseMrtNullRtv.IsValid() && m_sparseMrtNullRtv.cpu.ptr != 0)
				{
					rtvHandles[i] = m_sparseMrtNullRtv.cpu;
					usedNullRtv = true;
				}
				continue;
			}
			rtvHandles[i] = fboRtvHandles[i];
			TransitionTextureView(colorAttachment.view, D3D12_RESOURCE_STATE_RENDER_TARGET);
		}
	}

	if (fbo->GetColorAttachmentCount() > rtvCount)
	{
		static uint32 s_tooManyRtLogCount = 0;
		if (D3D12RuntimeLogCapped(s_tooManyRtLogCount, "FBO RTV overflow", 32))
			cemuLog_log(LogType::Force, "D3D12: FBO key=0x{:016x} exposes {} color attachments; clamping to D3D12 MRT limit {}.",
						fbo->key,
						fbo->GetColorAttachmentCount(),
						rtvCount);
	}
	if (missingRealRtv)
	{
		static uint32 s_missingRtvLogCount = 0;
		if (D3D12RuntimeLogCapped(s_missingRtvLogCount, "missing FBO RTV", 128))
			cemuLog_log(LogType::Force, "D3D12: FBO key=0x{:016x} has missing/sparse RTV slots within count={} mask=0x{:02x}; nullRtv={} usedNull={}.",
						fbo->key,
						rtvCount,
						fbo->GetColorAttachmentMask(),
						m_sparseMrtNullRtv.IsValid() && m_sparseMrtNullRtv.cpu.ptr != 0,
						usedNullRtv);
	}
	if (rtvCount != 0)
	{
		for (uint32 i = 0; i < rtvCount; ++i)
		{
			if (rtvHandles[i].ptr != 0)
				continue;

			static uint32 s_invalidRtvLogCount = 0;
			if (D3D12RuntimeLogCapped(s_invalidRtvLogCount, "invalid FBO RTV", 128))
				cemuLog_log(LogType::Force, "D3D12: refusing to bind FBO key=0x{:016x}; RTV slot {} has no descriptor and no null RTV is available.",
							fbo->key,
							i);
			SetD3D12RenderTargets(0, nullptr, nullptr);
			return;
		}
	}

	D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle{};
	D3D12_CPU_DESCRIPTOR_HANDLE* dsvHandlePtr = nullptr;
	if (fbo->HasDepthStencilAttachment() && fbo->depthBuffer.texture)
	{
		auto* depthView = static_cast<LatteTextureViewD3D12*>(fbo->depthBuffer.texture);
		TransitionTextureView(depthView, D3D12_RESOURCE_STATE_DEPTH_WRITE, true);
		dsvHandle = fbo->GetDsvHandle();
		if (dsvHandle.ptr != 0)
			dsvHandlePtr = &dsvHandle;
	}

	SetD3D12RenderTargets(rtvCount, rtvCount != 0 ? rtvHandles.data() : nullptr, dsvHandlePtr);
}

