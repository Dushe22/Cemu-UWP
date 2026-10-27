// Part of D3D12Renderer.cpp (included there, same translation unit): descriptor tables, their cache keys and the uniform buffer upload.
// Not a standalone file: it relies on the includes and file-local helpers of D3D12Renderer.cpp.

void D3D12Renderer::InvalidateGameCbvSrvUavDescriptorCaches()
{
	m_gameNullResourceTable = {};
	m_gameNullResourceTableGeneration = kInvalidDescriptorIndex;
	m_gameNullUavTable = {};
	m_gameNullUavTableGeneration = kInvalidDescriptorIndex;
	m_gameResourceTableCache.clear();
	m_gameResourceTableCacheGeneration = kInvalidDescriptorIndex;
	m_resourceTableKeyMemo = {};
	m_gameStreamoutUavTable = {};
	m_gameStreamoutUavTableKey = 0;
	m_gameStreamoutUavTableGeneration = kInvalidDescriptorIndex;
}

void D3D12Renderer::InvalidateGameSamplerDescriptorCaches()
{
	m_gameSamplerTableCache.clear();
	m_gameSamplerTableCacheGeneration = kInvalidDescriptorIndex;
	m_samplerTableKeyMemo = {};
	m_shaderVisibleOutputLinearSampler = {};
	m_shaderVisibleOutputPointSampler = {};
	m_shaderVisibleOutputSamplerGeneration = kInvalidDescriptorIndex;
}

void D3D12Renderer::InvalidateGameShaderVisibleDescriptorCaches()
{
	InvalidateGameCbvSrvUavDescriptorCaches();
	InvalidateGameSamplerDescriptorCaches();
}

uint32 D3D12Renderer::GetGameResourceTableDescriptorCount(LatteDecompilerShader* shader) const
{
	if (!shader)
		return 1;

	uint32 descriptorCount = 1;
	auto requireDescriptor = [&descriptorCount](uint32 descriptorIndex)
	{
		descriptorCount = std::max(descriptorCount, descriptorIndex + 1);
	};

	for (uint32 textureListIndex = 0; textureListIndex < shader->textureUnitListCount; ++textureListIndex)
	{
		const uint32 relativeUnit = shader->textureUnitList[textureListIndex];
		const sint8 bindingPoint = shader->resourceMapping.textureUnitToBindingPoint[relativeUnit];
		if (bindingPoint >= 0 && (uint32)bindingPoint < kGameTextureSrvDescriptorCount)
			requireDescriptor((uint32)bindingPoint);
	}

	for (uint32 bufferIndex = 0; bufferIndex < LATTE_NUM_MAX_UNIFORM_BUFFERS; ++bufferIndex)
	{
		const sint8 bindingPoint = shader->resourceMapping.uniformBuffersBindingPoint[bufferIndex];
		if (bindingPoint >= 0 && (uint32)bindingPoint < kGameTextureSrvDescriptorCount)
			requireDescriptor((uint32)bindingPoint);
	}

	if (shader->shaderType == LatteConst::ShaderType::Vertex)
	{
		auto* fetchShader = s_recordFetchShader;
		if (fetchShader)
		{
			for (auto& group : fetchShader->bufferGroups)
			{
				const uint32 bindingPoint = kGameVertexBufferSrvBase + group.attributeBufferIndex;
				if (bindingPoint < kGameTextureSrvDescriptorCount)
					requireDescriptor(bindingPoint);
			}
		}
	}

	if (shader->shaderType == LatteConst::ShaderType::Pixel && m_logicOpEmulationMode != 0)
		requireDescriptor(kGameLogicOpDstSrvBinding + kGameLogicOpDstSrvCount - 1);

	return std::min<uint32>(descriptorCount, kGameTextureSrvDescriptorCount);
}

uint32 D3D12Renderer::GetGameUavTableDescriptorCount(LatteDecompilerShader* shader) const
{
	return shader && shader->resourceMapping.tfStorageBindingPoint >= 0 &&
		(uint32)shader->resourceMapping.tfStorageBindingPoint < kGameUavDescriptorCount ? kGameUavDescriptorCount : 0;
}

uint32 D3D12Renderer::GetGameSamplerTableDescriptorCount(LatteDecompilerShader* shader) const
{
	if (!shader)
		return 1;

	uint32 descriptorCount = 1;
	for (uint32 textureListIndex = 0; textureListIndex < shader->textureUnitListCount; ++textureListIndex)
	{
		const uint32 relativeUnit = shader->textureUnitList[textureListIndex];
		const sint8 bindingPoint = shader->resourceMapping.textureUnitToBindingPoint[relativeUnit];
		if (bindingPoint >= 0 && (uint32)bindingPoint < kGameSamplerDescriptorCount)
			descriptorCount = std::max<uint32>(descriptorCount, (uint32)bindingPoint + 1);
	}
	return descriptorCount;
}

uint64 D3D12Renderer::BuildGameResourceTableCacheKey(LatteDecompilerShader* shader, uint32 descriptorGeneration, bool& cacheable)
{
	cacheable = false;
	if (!shader || !m_descriptorManager)
		return 0;

	bool hasCurrentDrawFeedbackScratch = std::any_of(m_feedbackScratchByTextureUnit.begin(),
		m_feedbackScratchByTextureUnit.end(), [](uint32 index) { return index != kInvalidScratchCopyIndex; });
	bool checkStageFeedback = PerformanceExperimentEnabled(D3D12PerformanceExperiment::StageFeedbackCache);
	if (hasCurrentDrawFeedbackScratch && checkStageFeedback)
	{
		uint32 stageHostBase = 0;
		bool validStage = true;
		switch (shader->shaderType)
		{
		case LatteConst::ShaderType::Vertex: stageHostBase = LATTE_CEMU_VS_TEX_UNIT_BASE; break;
		case LatteConst::ShaderType::Pixel: stageHostBase = LATTE_CEMU_PS_TEX_UNIT_BASE; break;
		case LatteConst::ShaderType::Geometry: stageHostBase = LATTE_CEMU_GS_TEX_UNIT_BASE; break;
		default: validStage = false; break;
		}
		const bool stageFeedback = D3D12PerformanceExperiment::HasStageFeedback(m_feedbackScratchByTextureUnit,
			kInvalidScratchCopyIndex, shader->textureUnitList, shader->textureUnitListCount,
			stageHostBase, LATTE_NUM_MAX_TEX_UNITS, validStage);
		if (PerformanceExperimentEnabled(D3D12PerformanceExperiment::StageFeedbackCache))
			hasCurrentDrawFeedbackScratch = stageFeedback;
	}
	if (hasCurrentDrawFeedbackScratch)
		return 0;
	if (shader->shaderType == LatteConst::ShaderType::Pixel && m_logicOpEmulationMode != 0)
		return 0;

	uint64 key = 0xd312d15c0de00001ull;
	key = MixGameDescriptorTableKey(key, descriptorGeneration);
	key = MixGameDescriptorTableKey(key, (uint64)shader->shaderType);
	key = MixGameDescriptorTableKey(key, shader->baseHash);
	key = MixGameDescriptorTableKey(key, shader->auxHash);
	key = MixGameDescriptorTableKey(key, (uint64)reinterpret_cast<size_t>(shader));
	key = MixGameDescriptorTableKey(key, GetGameResourceTableDescriptorCount(shader));
	key = MixGameDescriptorTableKey(key, (uint64)reinterpret_cast<size_t>(m_bufferCache.resource.Get()));

	const size_t stageIndex = (size_t)shader->shaderType;
	if (stageIndex < m_uniformBindings.size())
	{
		for (uint32 bufferIndex = 0; bufferIndex < LATTE_NUM_MAX_UNIFORM_BUFFERS; ++bufferIndex)
		{
			const sint8 bindingPoint = shader->resourceMapping.uniformBuffersBindingPoint[bufferIndex];
			if (bindingPoint < 0 || (uint32)bindingPoint >= kGameTextureSrvDescriptorCount)
				continue;
			const auto& binding = m_uniformBindings[stageIndex][bufferIndex];
			key = MixGameDescriptorTableKey(key, 0x10000000ull + bufferIndex);
			key = MixGameDescriptorTableKey(key, (uint64)(uint8)bindingPoint);
			key = MixGameDescriptorTableKey(key, binding.valid ? 1 : 0);
			key = MixGameDescriptorTableKey(key, binding.offset);
			key = MixGameDescriptorTableKey(key, binding.size);
		}
	}

	if (shader->shaderType == LatteConst::ShaderType::Vertex)
	{
		auto* fetchShader = s_recordFetchShader;
		key = MixGameDescriptorTableKey(key, (uint64)reinterpret_cast<size_t>(fetchShader));
		if (fetchShader)
		{
			for (auto& group : fetchShader->bufferGroups)
			{
				const uint32 bufferIndex = group.attributeBufferIndex;
				const uint32 bindingPoint = kGameVertexBufferSrvBase + bufferIndex;
				if (bufferIndex >= m_vertexBindings.size() || bindingPoint >= kGameTextureSrvDescriptorCount)
					continue;
				const auto& binding = m_vertexBindings[bufferIndex];
				key = MixGameDescriptorTableKey(key, 0x20000000ull + bufferIndex);
				key = MixGameDescriptorTableKey(key, binding.valid ? 1 : 0);
				key = MixGameDescriptorTableKey(key, binding.offset);
				key = MixGameDescriptorTableKey(key, binding.size);
			}
		}
	}

	uint32 hostTextureUnitBase = 0;
	const _LatteRegisterSetTextureUnit* texRegs = nullptr;
	switch (shader->shaderType)
	{
	case LatteConst::ShaderType::Vertex:
		hostTextureUnitBase = LATTE_CEMU_VS_TEX_UNIT_BASE;
		texRegs = RecordRegs().SQ_TEX_START_VS;
		break;
	case LatteConst::ShaderType::Pixel:
		hostTextureUnitBase = LATTE_CEMU_PS_TEX_UNIT_BASE;
		texRegs = RecordRegs().SQ_TEX_START_PS;
		break;
	case LatteConst::ShaderType::Geometry:
		hostTextureUnitBase = LATTE_CEMU_GS_TEX_UNIT_BASE;
		texRegs = RecordRegs().SQ_TEX_START_GS;
		break;
	default:
		return 0;
	}

	for (uint32 textureListIndex = 0; textureListIndex < shader->textureUnitListCount; ++textureListIndex)
	{
		const uint32 relativeUnit = shader->textureUnitList[textureListIndex];
		const sint8 bindingPoint = shader->resourceMapping.textureUnitToBindingPoint[relativeUnit];
		if (bindingPoint < 0 || (uint32)bindingPoint >= kGameTextureSrvDescriptorCount)
			continue;

		uint32 gpuSamplerSwizzle = 0x06880000;
		if (texRegs)
		{
			const auto& word4 = texRegs[relativeUnit].word4;
			gpuSamplerSwizzle = ((uint32)word4.get_DST_SEL_X() << 16) |
								((uint32)word4.get_DST_SEL_Y() << 19) |
								((uint32)word4.get_DST_SEL_Z() << 22) |
								((uint32)word4.get_DST_SEL_W() << 25);
		}

		const uint32 hostTextureUnit = hostTextureUnitBase + relativeUnit;
		auto* texture = hostTextureUnit < m_boundTextures.size() ? m_boundTextures[hostTextureUnit] : nullptr;
		key = MixGameDescriptorTableKey(key, 0x30000000ull + relativeUnit);
		key = MixGameDescriptorTableKey(key, (uint64)(uint8)bindingPoint);
		key = MixGameDescriptorTableKey(key, hostTextureUnit);
		key = MixGameDescriptorTableKey(key, gpuSamplerSwizzle);
		key = MixGameDescriptorTableKey(key, (uint64)reinterpret_cast<size_t>(texture));
		if (!texture)
		{
			key = MixGameDescriptorTableKey(key, 0);
			continue;
		}

		const auto& samplerSrv = texture->GetSamplerSRV(gpuSamplerSwizzle);
		key = MixGameDescriptorTableKey(key, texture->baseTexture ? texture->baseTexture->physAddress : 0);
		key = MixGameDescriptorTableKey(key, (uint64)texture->format);
		key = MixGameDescriptorTableKey(key, (uint64)texture->dim);
		key = MixGameDescriptorTableKey(key, texture->firstMip);
		key = MixGameDescriptorTableKey(key, texture->numMip);
		key = MixGameDescriptorTableKey(key, texture->firstSlice);
		key = MixGameDescriptorTableKey(key, texture->numSlice);
		key = MixGameDescriptorTableKey(key, samplerSrv.IsValid() ? samplerSrv.index + 1ull : 0);
	}

	cacheable = true;
	return key;
}

uint64 D3D12Renderer::GetMemoizedTableCacheKey(std::array<TableKeyMemo, (size_t)LatteConst::ShaderType::TotalCount>& memoArray,
											   bool isSamplerTable, LatteDecompilerShader* shader, uint32 descriptorGeneration, bool& cacheable)
{
	if (!shader)
		return isSamplerTable ? BuildGameSamplerTableCacheKey(shader, descriptorGeneration, cacheable)
							  : BuildGameResourceTableCacheKey(shader, descriptorGeneration, cacheable);
	if (auto* memo = GetCurrentTableKeyMemo(memoArray, shader, descriptorGeneration))
	{
		cacheable = memo->cacheable;
		return memo->key;
	}
	auto& memo = memoArray[(size_t)shader->shaderType];
	memo = {};
	// across draws: the key only changes with the shader, the bindings and the sampling registers it reads. Resource
	// keys of draws with feedback copies or pixel logic op are never cached (they are built uncacheable)
	const size_t stage = (size_t)shader->shaderType;
	auto& persistent = (isSamplerTable ? m_samplerStageKeyCache : m_resourceStageKeyCache)[stage];
	const bool persistentAllowed = isSamplerTable ||
		(!(shader->shaderType == LatteConst::ShaderType::Pixel && m_logicOpEmulationMode != 0) &&
		 std::none_of(m_feedbackScratchByTextureUnit.begin(), m_feedbackScratchByTextureUnit.end(),
			[](uint32 index) { return index != kInvalidScratchCopyIndex; }));
	ID3D12Resource* bufferResource = m_bufferCache.resource.Get();
	// sampler keys read the stage's bound textures and sampler registers; resource keys the bound textures, texture
	// resource registers and the stage's uniform buffers (vertex shaders also the vertex buffers)
	const size_t genStage = std::min<size_t>(stage, 2);
	const uint64 bindingGeneration = isSamplerTable ? m_textureBindingGeneration[genStage] :
		m_textureBindingGeneration[genStage] * 0x9E3779B97F4A7C15ull + m_uniformBindingGeneration[stage] * 0xC2B2AE3D27D4EB4Full +
		(shader->shaderType == LatteConst::ShaderType::Vertex ? m_vertexBindingGeneration : 0);
	const uint64 registerGeneration = isSamplerTable ? s_recordSamplerRegGeneration[genStage] : s_recordTextureRegGeneration[genStage];
	if (persistentAllowed && persistent.valid && persistent.shader == shader && persistent.fetchShader == s_recordFetchShader &&
		persistent.descriptorGeneration == descriptorGeneration && persistent.bindingGeneration == bindingGeneration &&
		persistent.registerGeneration == registerGeneration && persistent.bufferResource == bufferResource)
	{
		cacheable = persistent.cacheable;
		memo.key = persistent.key;
		m_tableKeyReuses[isSamplerTable ? 1 : 0]++;
	}
	else
	{
		memo.key = isSamplerTable ? BuildGameSamplerTableCacheKey(shader, descriptorGeneration, cacheable)
								  : BuildGameResourceTableCacheKey(shader, descriptorGeneration, cacheable);
		m_tableKeyBuilds[isSamplerTable ? 1 : 0]++;
		if (persistentAllowed)
			persistent = { true, shader, s_recordFetchShader, descriptorGeneration, bindingGeneration,
				registerGeneration, bufferResource, memo.key, cacheable };
		else
			persistent.valid = false;
	}
	memo.cacheable = cacheable;
	memo.shader = shader;
	memo.generation = descriptorGeneration;
	memo.epoch = m_tableKeyMemoEpoch;
	return memo.key;
}

D3D12Renderer::TableKeyMemo* D3D12Renderer::GetCurrentTableKeyMemo(std::array<TableKeyMemo, (size_t)LatteConst::ShaderType::TotalCount>& memoArray,
																	LatteDecompilerShader* shader, uint32 descriptorGeneration)
{
	if (!shader || m_tableKeyMemoEpoch == 0)
		return nullptr;
	auto& memo = memoArray[(size_t)shader->shaderType];
	if (memo.shader != shader || memo.generation != descriptorGeneration || memo.epoch != m_tableKeyMemoEpoch)
		return nullptr;
	return &memo;
}

bool D3D12Renderer::HasCachedGameResourceTable(LatteDecompilerShader* shader, uint32 descriptorGeneration)
{
	if (!shader || !m_descriptorManager)
		return false;
	if (m_gameResourceTableCacheGeneration != descriptorGeneration)
		return false;
	bool cacheable = false;
	const uint64 key = GetMemoizedTableCacheKey(m_resourceTableKeyMemo, false, shader, descriptorGeneration, cacheable);
	if (!cacheable)
		return false;
	auto* memo = GetCurrentTableKeyMemo(m_resourceTableKeyMemo, shader, descriptorGeneration);
	if (memo && memo->lookupDone)
		return memo->lookupHit;
	const auto it = m_gameResourceTableCache.find(key);
	const bool found = it != m_gameResourceTableCache.end();
	if (memo)
	{
		memo->lookupDone = true;
		memo->lookupHit = found;
		memo->cachedAllocation = found ? it->second.allocation : D3D12DescriptorManager::Allocation{};
	}
	return found;
}

uint64 D3D12Renderer::BuildGameSamplerTableCacheKey(LatteDecompilerShader* shader, uint32 descriptorGeneration, bool& cacheable)
{
	cacheable = false;
	if (!m_descriptorManager)
		return 0;

	uint64 key = 0xd3125a7cafe00001ull;
	key = MixGameDescriptorTableKey(key, descriptorGeneration);
	if (!shader)
	{
		cacheable = true;
		return MixGameDescriptorTableKey(key, 0);
	}

	key = MixGameDescriptorTableKey(key, (uint64)shader->shaderType);
	key = MixGameDescriptorTableKey(key, shader->baseHash);
	key = MixGameDescriptorTableKey(key, shader->auxHash);
	key = MixGameDescriptorTableKey(key, (uint64)reinterpret_cast<size_t>(shader));
	key = MixGameDescriptorTableKey(key, GetGameSamplerTableDescriptorCount(shader));
	uint32 hostTextureUnitBase = 0;
	switch (shader->shaderType)
	{
	case LatteConst::ShaderType::Vertex:
		hostTextureUnitBase = LATTE_CEMU_VS_TEX_UNIT_BASE;
		break;
	case LatteConst::ShaderType::Pixel:
		hostTextureUnitBase = LATTE_CEMU_PS_TEX_UNIT_BASE;
		break;
	case LatteConst::ShaderType::Geometry:
		hostTextureUnitBase = LATTE_CEMU_GS_TEX_UNIT_BASE;
		break;
	default:
		return 0;
	}

	for (uint32 textureListIndex = 0; textureListIndex < shader->textureUnitListCount; ++textureListIndex)
	{
		const uint32 relativeUnit = shader->textureUnitList[textureListIndex];
		const sint8 bindingPoint = shader->resourceMapping.textureUnitToBindingPoint[relativeUnit];
		if (bindingPoint < 0 || (uint32)bindingPoint >= kGameSamplerDescriptorCount)
			continue;

		const uint32 hostTextureUnit = hostTextureUnitBase + relativeUnit;
		LatteTextureViewD3D12* textureView = hostTextureUnit < m_boundTextures.size() ? m_boundTextures[hostTextureUnit] : nullptr;
		LatteTexture* baseTexture = textureView ? textureView->baseTexture : nullptr;
		const uint32 stageSamplerIndex = shader->textureUnitSamplerAssignment[relativeUnit];

		key = MixGameDescriptorTableKey(key, 0x51000000ull + relativeUnit);
		key = MixGameDescriptorTableKey(key, (uint64)(uint8)bindingPoint);
		key = MixGameDescriptorTableKey(key, stageSamplerIndex);
		key = MixGameDescriptorTableKey(key, shader->textureUsesDepthCompare[relativeUnit] ? 1 : 0);
		// key on sampler overrides, not view identity, so identical tables are shared
		key = D3D12SamplerTableState::MixTextureOverrides(key, baseTexture, MixGameDescriptorTableKey);

		if (stageSamplerIndex == LATTE_DECOMPILER_SAMPLER_NONE)
			continue;

		const uint32 samplerIndex = stageSamplerIndex + LatteDecompiler_getTextureSamplerBaseIndex(shader->shaderType);
		const _LatteRegisterSetSampler* samplerWords = RecordRegs().SQ_TEX_SAMPLER + samplerIndex;
		key = MixGameDescriptorTableKey(key, samplerWords->WORD0.getRawValue());
		key = MixGameDescriptorTableKey(key, samplerWords->WORD1.getRawValue());
		key = MixGameDescriptorTableKey(key, samplerWords->WORD2.getRawValue());

		if (samplerWords->WORD0.get_BORDER_COLOR_TYPE() == Latte::LATTE_SQ_TEX_SAMPLER_WORD0_0::E_BORDER_COLOR_TYPE::REGISTER)
		{
			auto* borderColorReg = GetD3D12SamplerBorderColorRegisters(shader->shaderType, stageSamplerIndex);
			key = MixGameDescriptorTableKey(key, std::bit_cast<uint32>(borderColorReg->red.get_channelValue()));
			key = MixGameDescriptorTableKey(key, std::bit_cast<uint32>(borderColorReg->green.get_channelValue()));
			key = MixGameDescriptorTableKey(key, std::bit_cast<uint32>(borderColorReg->blue.get_channelValue()));
			key = MixGameDescriptorTableKey(key, std::bit_cast<uint32>(borderColorReg->alpha.get_channelValue()));
		}
	}

	cacheable = true;
	return key;
}

bool D3D12Renderer::HasCachedGameSamplerTable(LatteDecompilerShader* shader, uint32 descriptorGeneration)
{
	if (!m_descriptorManager)
		return false;
	if (m_gameSamplerTableCacheGeneration != descriptorGeneration)
		return false;
	bool cacheable = false;
	const uint64 key = GetMemoizedTableCacheKey(m_samplerTableKeyMemo, true, shader, descriptorGeneration, cacheable);
	if (!cacheable)
		return false;
	auto* memo = GetCurrentTableKeyMemo(m_samplerTableKeyMemo, shader, descriptorGeneration);
	if (memo && memo->lookupDone)
		return memo->lookupHit;
	const auto it = m_gameSamplerTableCache.find(key);
	const bool found = it != m_gameSamplerTableCache.end();
	if (memo)
	{
		memo->lookupDone = true;
		memo->lookupHit = found;
		memo->cachedAllocation = found ? it->second.allocation : D3D12DescriptorManager::Allocation{};
	}
	return found;
}

uint64 D3D12Renderer::BuildGameUavTableCacheKey(LatteDecompilerShader* shader, uint32 descriptorGeneration) const
{
	if (!shader || shader->resourceMapping.tfStorageBindingPoint < 0 || !m_streamoutBuffer.resource)
		return 0;

	uint64 key = 0xd3125a0a7eed0001ull;
	key = MixGameDescriptorTableKey(key, descriptorGeneration);
	key = MixGameDescriptorTableKey(key, (uint64)reinterpret_cast<size_t>(m_streamoutBuffer.resource.Get()));
	key = MixGameDescriptorTableKey(key, (uint32)shader->resourceMapping.tfStorageBindingPoint);
	key = MixGameDescriptorTableKey(key, m_streamoutBuffer.size);
	return key;
}

#if defined(CEMU_UWP_D3D12_DIAGNOSTICS)
static void D3D12TracePriorityUniformSources(LatteDecompilerShader* shader, uint64 drawId, uint64 pipelineHash)
{
	if (!shader || !D3D12TraceSink::Get().IsPriorityStencilDraw(drawId))
		return;
	constexpr uint32 maxBytes = D3D12DiagnosticDrawContextPolicy::kMaxUniformSourceBytes;
	uint32 aluOffset = 0;
	uint32 uniformRegisterBase = 0;
	switch (shader->shaderType)
	{
	case LatteConst::ShaderType::Vertex: aluOffset = 0x400; uniformRegisterBase = mmSQ_VTX_UNIFORM_BLOCK_START; break;
	case LatteConst::ShaderType::Pixel: uniformRegisterBase = mmSQ_PS_UNIFORM_BLOCK_START; break;
	case LatteConst::ShaderType::Geometry: uniformRegisterBase = mmSQ_GS_UNIFORM_BLOCK_START; break;
	default: return;
	}
	if (shader->uniform.uniformRangeSize > maxBytes || shader->uniform.count_uniformRegister > 256)
	{
		D3D12TraceEvent("uniform_priority_source_skipped", "draw={} pipeline=0x{:016x} shader=0x{:016x} reason=source_limit rangeSize={} registerCount={}",
			drawId, pipelineHash, shader->baseHash, shader->uniform.uniformRangeSize, shader->uniform.count_uniformRegister);
		return;
	}
	uint32 remappedBytes = 0;
	auto includeRemapped = [&](const auto& entry)
	{
		if ((entry.mappedIndexOffset & 15) != 0 || (uint64)entry.mappedIndexOffset + 16 > maxBytes)
			return false;
		remappedBytes = std::max(remappedBytes, (uint32)entry.mappedIndexOffset + 16);
		return true;
	};
	for (const auto& entry : shader->list_remappedUniformEntries_register)
		if ((entry.indexOffset & 15) != 0 || (uint64)entry.indexOffset + 16 > 256 * 16 || !includeRemapped(entry)) return;
	for (const auto& group : shader->list_remappedUniformEntries_bufferGroups)
	{
		if (group.bufferId >= LATTE_NUM_MAX_UNIFORM_BUFFERS || group.kcacheBankIdOffset != group.bufferId * (7 * 4))
			return;
		const MPTR base = LatteGPUState.contextRegister[uniformRegisterBase + group.kcacheBankIdOffset / 4];
		for (const auto& entry : group.entries)
		{
			if ((entry.indexOffset & 15) != 0 || !includeRemapped(entry)) return;
			// A zero CB address is intentionally zero-filled by the shared loader.
			const uint64 address = (uint64)base + entry.indexOffset;
			if (base && (address > std::numeric_limits<MPTR>::max() - 15 ||
				!memory_isAddressRangeAccessible((MPTR)address, 16)))
				return;
		}
	}
	std::vector<uint32> remapped((remappedBytes + 3) / 4, 0);
	if (!remapped.empty())
		LatteBufferCache_LoadRemappedUniforms(shader, reinterpret_cast<float*>(remapped.data()), true,
			(1 << LATTE_NUM_MAX_UNIFORM_BUFFERS) - 1);
	const uint32* registers = LatteGPUState.contextRegister + mmSQ_ALU_CONSTANT0_0 + aluOffset;
	const uint32 registerWords = shader->uniform.loc_uniformRegister >= 0 ? shader->uniform.count_uniformRegister * 4 : 0;
	auto formatWords = [](const uint32* words, uint32 count)
	{
		std::string result;
		result.reserve((size_t)count * 11);
		for (uint32 i = 0; i < count; ++i)
			fmt::format_to(std::back_inserter(result), "{}0x{:08x}", i ? "," : "", words[i]);
		return result.empty() ? std::string("none") : result;
	};
	D3D12TraceEvent("uniform_priority_source", "draw={} pipeline=0x{:016x} stage={} shader=0x{:016x} aux=0x{:016x} snapshotKind=cpu_alu_and_remapped_sources cacheBehavior=unchanged gpuBytesVerified=0 rangeSize={} locRemapped={} locUniformRegister={} remappedWordCount={} uniformRegisterWordCount={} remappedWords={} uniformRegisterWords={}",
		drawId, pipelineHash, (uint32)shader->shaderType, shader->baseHash, shader->auxHash,
		shader->uniform.uniformRangeSize, shader->uniform.loc_remapped, shader->uniform.loc_uniformRegister,
		remapped.size(), registerWords, formatWords(remapped.data(), (uint32)remapped.size()), formatWords(registers, registerWords));
}
#endif

// Latte side of the uniform upload (reads registers and guest memory): builds the stage's bytes and sends them when
// they differ from the last ones sent; otherwise the recorder keeps the bytes it has. Comparing the bytes replaced a
// serial hash of every source word (~0.8 us per draw in Kakariko) and is exact
void D3D12Renderer::PrepareGameUniformVars(LatteDecompilerShader* shader, uint64 drawId, uint64 pipelineHash, UniformUpdate& update)
{
	if (!shader || !m_memoryManager || (size_t)shader->shaderType >= m_latteUniformSources.size())
		return;
	auto& source = m_latteUniformSources[(size_t)shader->shaderType];
	update.shader = shader;
	if (shader->resourceMapping.uniformVarsBufferBindingPoint != 0 || shader->uniform.uniformRangeSize == 0)
	{
		// no uniform buffer: the recorder's bytes for this stage are dropped
		if (!source.sent || source.shader != shader || !source.sentBytes.empty())
		{
			update.send = true;
			source.shader = shader;
			source.sentBytes.clear();
			source.sent = true;
		}
		return;
	}

	auto& uniformData = m_latteUniformScratch;
	uniformData.assign(std::max<uint32>(shader->uniform.uniformRangeSize, 16), 0);
	auto getUniformDataPtr = [&uniformData](size_t index) -> float*
	{
		return reinterpret_cast<float*>(uniformData.data() + index);
	};

	sint32 shaderAluConst = 0;
	switch (shader->shaderType)
	{
	case LatteConst::ShaderType::Vertex:
		shaderAluConst = 0x400;
		break;
	case LatteConst::ShaderType::Pixel:
	case LatteConst::ShaderType::Geometry:
		shaderAluConst = 0;
		break;
	default:
		break;
	}

	for (auto& entry : shader->uniform.list_ufTexRescale)
	{
		if (entry.uniformLocation < 0 || entry.uniformLocation + (sint32)sizeof(float) * 2 > (sint32)uniformData.size())
			continue;
		float* xyScale = LatteTexture_getEffectiveTextureScale(shader->shaderType, entry.texUnit);
		memcpy(getUniformDataPtr(entry.uniformLocation), xyScale, sizeof(float) * 2);
	}
	if (shader->uniform.loc_alphaTestRef >= 0 && shader->uniform.loc_alphaTestRef + (sint32)sizeof(float) <= (sint32)uniformData.size())
		*getUniformDataPtr(shader->uniform.loc_alphaTestRef) = LatteGPUState.contextNew.SX_ALPHA_REF.get_ALPHA_TEST_REF();
	if (shader->uniform.loc_pointSize >= 0 && shader->uniform.loc_pointSize + (sint32)sizeof(float) <= (sint32)uniformData.size())
	{
		const auto& pointSizeReg = LatteGPUState.contextNew.PA_SU_POINT_SIZE;
		float pointWidth = (float)pointSizeReg.get_WIDTH() / 8.0f;
		if (pointWidth == 0.0f)
			pointWidth = 1.0f / 8.0f;
		*getUniformDataPtr(shader->uniform.loc_pointSize) = pointWidth;
	}
	if (shader->uniform.loc_remapped >= 0 && shader->uniform.loc_remapped < (sint32)uniformData.size())
	{
		LatteBufferCache_LoadRemappedUniforms(shader, getUniformDataPtr(shader->uniform.loc_remapped), true, (1 << LATTE_NUM_MAX_UNIFORM_BUFFERS) - 1);
		if (D3D12ShouldChecksumOutput() && shader->shaderType == LatteConst::ShaderType::Pixel)
		{
			static uint32 s_psRemappedUniformLogCount = 0;
			if (D3D12RuntimeLogCapped(s_psRemappedUniformLogCount, "PS remapped uniform", 48))
			{
				const uint32* u = reinterpret_cast<const uint32*>(uniformData.data() + shader->uniform.loc_remapped);
				const float* f = reinterpret_cast<const float*>(u);
				cemuLog_log(LogType::Force,
							 "D3D12: PS remapped[0] shader={:016x} raw=0x{:08x},0x{:08x},0x{:08x},0x{:08x} float={:.6g},{:.6g},{:.6g},{:.6g}.",
							 shader->baseHash,
							 u[0], u[1], u[2], u[3],
							 f[0], f[1], f[2], f[3]);
			}
		}
	}
	if (shader->uniform.loc_uniformRegister >= 0 && shader->uniform.loc_uniformRegister + shader->uniform.count_uniformRegister * 16 <= (sint32)uniformData.size())
	{
		uint32* uniformRegData = LatteGPUState.contextRegister + mmSQ_ALU_CONSTANT0_0 + shaderAluConst;
		memcpy(getUniformDataPtr(shader->uniform.loc_uniformRegister), uniformRegData, shader->uniform.count_uniformRegister * 16);
	}
	if (shader->uniform.loc_windowSpaceToClipSpaceTransform >= 0 && shader->uniform.loc_windowSpaceToClipSpaceTransform + (sint32)sizeof(float) * 2 <= (sint32)uniformData.size())
	{
		sint32 viewportWidth = 1;
		sint32 viewportHeight = 1;
		LatteRenderTarget_GetCurrentVirtualViewportSize(&viewportWidth, &viewportHeight);
		float* v = getUniformDataPtr(shader->uniform.loc_windowSpaceToClipSpaceTransform);
		v[0] = 2.0f / (float)std::max<sint32>(viewportWidth, 1);
		v[1] = 2.0f / (float)std::max<sint32>(viewportHeight, 1);
	}
	float fragCoordScale[4]{};
	bool hasFragCoordScale = false;
	if (shader->uniform.loc_fragCoordScale >= 0 && shader->uniform.loc_fragCoordScale + (sint32)sizeof(float) * 4 <= (sint32)uniformData.size())
	{
		float* scale = getUniformDataPtr(shader->uniform.loc_fragCoordScale);
		LatteMRT::GetCurrentFragCoordScale(scale);
		memcpy(fragCoordScale, scale, sizeof(fragCoordScale));
		hasFragCoordScale = true;
	}
	else if (shader->uniform.loc_fragCoordScale >= 0)
	{
		D3D12TraceEvent("uniform_fragcoord_scale_skipped", "draw={} pipeline=0x{:016x} stage={} shader=0x{:016x} aux=0x{:016x} rangeSize={} locFragCoordScale={} requiredBytes=16",
						(unsigned long long)drawId,
						(unsigned long long)pipelineHash,
						(uint32)shader->shaderType,
						shader->baseHash,
						shader->auxHash,
						shader->uniform.uniformRangeSize,
						shader->uniform.loc_fragCoordScale);
	}
	if (shader->uniform.loc_verticesPerInstance >= 0 &&
		shader->uniform.loc_verticesPerInstance + (sint32)sizeof(sint32) <= (sint32)uniformData.size())
	{
		memcpy(uniformData.data() + shader->uniform.loc_verticesPerInstance,
			&m_streamoutVerticesPerInstance, sizeof(m_streamoutVerticesPerInstance));
	}
	// Latte thread: m_streamoutBindings is recorder state, so with threaded recording it held the previous draw's ring
	// offset and particles were simulated into the wrong place (BOTW rain drawn as screen-wide streaks)
	for (uint32 bufferIndex = 0; bufferIndex < m_latteStreamoutRingOffsets.size(); ++bufferIndex)
	{
		const sint32 location = shader->uniform.loc_streamoutBufferBase[bufferIndex];
		if (location < 0 || location + (sint32)sizeof(sint32) > (sint32)uniformData.size())
			continue;
		const sint32 ringBufferOffset = m_latteStreamoutRingOffsets[bufferIndex];
		memcpy(uniformData.data() + location, &ringBufferOffset, sizeof(ringBufferOffset));
	}

	if (source.sent && source.shader == shader && source.sentBytes == uniformData)
		return;
	if (update.data.capacity() == 0)
		update.data = s_recordBytePool.Take();
	update.data.assign(uniformData.begin(), uniformData.end());
	source.shader = shader;
	source.sentBytes.assign(uniformData.begin(), uniformData.end());
	source.sent = true;
	update.send = true;
}

// recording side: installs the bytes sent with the draw (an empty send means the stage has no uniform buffer)
void D3D12Renderer::ApplyGameUniformUpdate(UniformUpdate& update)
{
	if (!update.shader || !update.send || (size_t)update.shader->shaderType >= m_gameUniformUploadCache.size())
		return;
	auto& slot = m_gameUniformUploadCache[(size_t)update.shader->shaderType];
	slot.shader = update.shader;
	std::swap(slot.data, update.data);
	s_recordBytePool.Give(std::move(update.data)); // the previous bytes go back to PrepareGameUniformVars
	slot.gpuAddress = 0;
}

// recording side: copies the prepared bytes into the upload ring once per command list (ring memory is retired by fence)
D3D12_GPU_VIRTUAL_ADDRESS D3D12Renderer::CommitGameUniformVars(LatteDecompilerShader* shader, uint64 drawId, uint64 pipelineHash)
{
	struct S0Timer
	{
		uint64 start = t_uwpInDrawTables ? UwpProfTick() : 0;
		~S0Timer() { if (start) s_uwpRecorderSplit.Add(s_uwpRecorderSplit.tableUniformCommit, UwpProfTick() - start); }
	} s0Timer;
	if (!shader || !m_memoryManager || (size_t)shader->shaderType >= m_gameUniformUploadCache.size())
		return 0;
	auto& slot = m_gameUniformUploadCache[(size_t)shader->shaderType];
	if (slot.shader != shader || slot.data.empty())
		return 0;
	if (slot.gpuAddress != 0)
		return slot.gpuAddress;

	const uint32 cbvSize = AlignUpU32((uint32)slot.data.size(), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
	auto upload = m_memoryManager->AllocateUpload(cbvSize, D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT, m_nextFenceValue);
	if (!upload.IsValid())
	{
		D3D12TraceEvent("uniform_vars_upload_failed", "draw={} pipeline=0x{:016x} stage={} shader=0x{:016x} aux=0x{:016x} rangeSize={} cbvSize={} nextFence={}",
						(unsigned long long)drawId,
						(unsigned long long)pipelineHash,
						(uint32)shader->shaderType,
						shader->baseHash,
						shader->auxHash,
						shader->uniform.uniformRangeSize,
						cbvSize,
						(unsigned long long)m_nextFenceValue);
		return 0;
	}

	memcpy(upload.cpuPtr, slot.data.data(), slot.data.size());
	if (cbvSize > slot.data.size())
		memset(upload.cpuPtr + slot.data.size(), 0, cbvSize - slot.data.size());
	slot.gpuAddress = upload.resource->GetGPUVirtualAddress() + upload.offset;
	const D3D12_GPU_VIRTUAL_ADDRESS uniformCbvAddress = slot.gpuAddress;
	uint32 firstUniformWords[4]{};
	memcpy(firstUniformWords, slot.data.data(), std::min<size_t>(slot.data.size(), sizeof(firstUniformWords)));
	D3D12TraceEvent("uniform_vars_upload", "draw={} pipeline=0x{:016x} stage={} shader=0x{:016x} aux=0x{:016x} rangeSize={} cbvSize={} uploadResource={} uploadOffset={} gpuVA=0x{:016x} locFragCoordScale={} firstWords=0x{:08x},0x{:08x},0x{:08x},0x{:08x}",
					(unsigned long long)drawId,
					(unsigned long long)pipelineHash,
					(uint32)shader->shaderType,
					shader->baseHash,
					shader->auxHash,
					shader->uniform.uniformRangeSize,
					cbvSize,
					(const void*)upload.resource,
					(unsigned long long)upload.offset,
					(unsigned long long)uniformCbvAddress,
					shader->uniform.loc_fragCoordScale,
					firstUniformWords[0],
					firstUniformWords[1],
					firstUniformWords[2],
					firstUniformWords[3]);
	if (D3D12ShouldTrace() && slot.data.size() <= 128)
	{
		std::string words;
		std::string floats;
		const uint32 wordCount = (uint32)(slot.data.size() / sizeof(uint32));
		words.reserve(wordCount * 11);
		floats.reserve(wordCount * 14);
		for (uint32 i = 0; i < wordCount; ++i)
		{
			uint32 word = 0;
			memcpy(&word, slot.data.data() + (size_t)i * sizeof(uint32), sizeof(uint32));
			if (i != 0)
			{
				words.push_back(',');
				floats.push_back(',');
			}
			fmt::format_to(std::back_inserter(words), "0x{:08x}", word);
			fmt::format_to(std::back_inserter(floats), "{:.9g}", std::bit_cast<float>(word));
		}
		D3D12TraceEvent("uniform_vars_upload_full", "draw={} pipeline=0x{:016x} stage={} shader=0x{:016x} aux=0x{:016x} rangeSize={} cbvSize={} wordCount={} locRemapped={} locUniformRegister={} uniformRegisterCount={} locWindowToClip={} locFragCoordScale={} locAlphaRef={} locPointSize={} words={} floats={}",
						(unsigned long long)drawId,
						(unsigned long long)pipelineHash,
						(uint32)shader->shaderType,
						shader->baseHash,
						shader->auxHash,
						shader->uniform.uniformRangeSize,
						cbvSize,
						wordCount,
						shader->uniform.loc_remapped,
						shader->uniform.loc_uniformRegister,
						shader->uniform.count_uniformRegister,
						shader->uniform.loc_windowSpaceToClipSpaceTransform,
						shader->uniform.loc_fragCoordScale,
						shader->uniform.loc_alphaTestRef,
						shader->uniform.loc_pointSize,
						words,
						floats);
	}
	return uniformCbvAddress;
}

D3D12DescriptorManager::Allocation D3D12Renderer::BuildGameResourceTable(LatteDecompilerShader* shader, D3D12_GPU_VIRTUAL_ADDRESS& uniformCbvAddress, uint64 drawId, uint64 pipelineHash)
{
	uniformCbvAddress = 0;
	const uint32 descriptorGeneration = m_descriptorManager->GetShaderVisibleCbvSrvUavRollovers();
	if (!shader)
	{
		if (m_gameNullResourceTable.IsValid() &&
			m_gameNullResourceTableGeneration == descriptorGeneration)
		{
			return m_gameNullResourceTable;
		}
	}
	else
	{
		if (m_gameResourceTableCacheGeneration != descriptorGeneration)
		{
			m_gameResourceTableCache.clear();
			m_gameResourceTableCacheGeneration = descriptorGeneration;
		}

		bool cacheable = false;
		const uint64 cacheKey = GetMemoizedTableCacheKey(m_resourceTableKeyMemo, false, shader, descriptorGeneration, cacheable);
		if (cacheable)
		{
			const auto* memo = GetCurrentTableKeyMemo(m_resourceTableKeyMemo, shader, descriptorGeneration);
			if (memo && memo->lookupDone)
			{
				if (memo->lookupHit)
				{
					m_gameResourceTableCacheHits++;
					return memo->cachedAllocation;
				}
			}
			else if (auto it = m_gameResourceTableCache.find(cacheKey); it != m_gameResourceTableCache.end())
			{
				m_gameResourceTableCacheHits++;
				return it->second.allocation;
			}
			m_gameResourceTableCacheMisses++;
		}
	}
	m_tableBuildTextureRefs.clear();

	const uint32 descriptorCount = GetGameResourceTableDescriptorCount(shader);
	auto allocation = m_descriptorManager->AllocateShaderVisible(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, descriptorCount);
	const uint32 descriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	const uint32 srvDescriptorCount = std::min<uint32>(descriptorCount, kGameTextureSrvDescriptorCount);
	if (srvDescriptorCount != 0)
		CopyDescriptorsSimpleChecked(srvDescriptorCount, OffsetCpuHandle(allocation.cpu, descriptorSize, kGameSrvDescriptorOffset), m_nullSrvDescriptorRange.cpu, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, "null texture table");

	if (!shader)
	{
		m_gameNullResourceTable = allocation;
		m_gameNullResourceTableGeneration = descriptorGeneration;
		return allocation;
	}
	bool cacheable = false;
	const uint64 cacheKey = GetMemoizedTableCacheKey(m_resourceTableKeyMemo, false, shader, descriptorGeneration, cacheable);

	const size_t stageIndex = (size_t)shader->shaderType;
	if (shader->shaderType == LatteConst::ShaderType::Pixel && m_logicOpEmulationMode != 0 &&
		kGameSrvDescriptorOffset + kGameLogicOpDstSrvBinding + kGameLogicOpDstSrvCount <= allocation.count)
	{
		for (uint32 renderTargetIndex = 0; renderTargetIndex < kGameLogicOpDstSrvCount; ++renderTargetIndex)
		{
			const FeedbackScratchCopy* logicScratch = GetLogicOpScratchCopy(renderTargetIndex);
			if (!logicScratch)
				continue;

			const uint32 bindingPoint = kGameLogicOpDstSrvBinding + renderTargetIndex;
			const auto tableSlot = OffsetCpuHandle(allocation.cpu, descriptorSize, kGameSrvDescriptorOffset + bindingPoint);
			CreateShaderResourceViewChecked(logicScratch->texture.resource.Get(), &logicScratch->srvDesc, tableSlot, "logic-op destination copy", true);
			D3D12TraceEvent("logic_op_scratch_srv_bind",
							"draw={} pipeline=0x{:016x} stage={} shader=0x{:016x} rt={} bindingPoint={} tableIndex={} tableGpu=0x{:016x} srvFormat={} scratchResource={}",
							(unsigned long long)drawId,
							(unsigned long long)pipelineHash,
							(uint32)shader->shaderType,
							shader->baseHash,
							renderTargetIndex,
							bindingPoint,
							allocation.index + kGameSrvDescriptorOffset + bindingPoint,
							(unsigned long long)allocation.gpu.ptr,
							(uint32)logicScratch->srvDesc.Format,
							(const void*)logicScratch->texture.resource.Get());
		}
	}

	if (stageIndex < m_uniformBindings.size() && m_bufferCache.resource)
	{
		for (uint32 bufferIndex = 0; bufferIndex < LATTE_NUM_MAX_UNIFORM_BUFFERS; ++bufferIndex)
		{
			const sint8 bindingPoint = shader->resourceMapping.uniformBuffersBindingPoint[bufferIndex];
			if (bindingPoint < 0 || (uint32)bindingPoint >= kGameTextureSrvDescriptorCount || (uint32)bindingPoint >= allocation.count)
				continue;
			const auto& binding = m_uniformBindings[stageIndex][bufferIndex];
			if (!binding.valid || binding.size == 0)
				continue;

			D3D12_SHADER_RESOURCE_VIEW_DESC uniformSrv{};
			uniformSrv.Format = DXGI_FORMAT_UNKNOWN;
			uniformSrv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
			uniformSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			uniformSrv.Buffer.FirstElement = binding.offset / 16;
			uniformSrv.Buffer.NumElements = std::max<UINT>(1, AlignUpU32(binding.size + (binding.offset & 15), 16) / 16);
			uniformSrv.Buffer.StructureByteStride = 16;
			uniformSrv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
			CreateShaderResourceViewChecked(m_bufferCache.resource.Get(), &uniformSrv, OffsetCpuHandle(allocation.cpu, descriptorSize, kGameSrvDescriptorOffset + (uint32)bindingPoint), "uniform buffer", true);
			D3D12TraceEvent("uniform_buffer_srv", "draw={} pipeline=0x{:016x} stage={} shader=0x{:016x} bufferIndex={} bindingPoint={} tableIndex={} tableGpu=0x{:016x} offset={} size={} firstElement={} numElements={} mainResource={}",
							(unsigned long long)drawId,
							(unsigned long long)pipelineHash,
							(uint32)shader->shaderType,
							shader->baseHash,
							bufferIndex,
							(sint32)bindingPoint,
							allocation.index + kGameSrvDescriptorOffset + (uint32)bindingPoint,
							(unsigned long long)allocation.gpu.ptr,
							binding.offset,
							binding.size,
							uniformSrv.Buffer.FirstElement,
							uniformSrv.Buffer.NumElements,
							(const void*)m_bufferCache.resource.Get());
		}
	}

	if (shader->shaderType == LatteConst::ShaderType::Vertex && m_bufferCache.resource)
	{
		auto* fetchShader = s_recordFetchShader;
		if (fetchShader)
		{
			for (auto& group : fetchShader->bufferGroups)
			{
				const uint32 bufferIndex = group.attributeBufferIndex;
				const uint32 bindingPoint = kGameVertexBufferSrvBase + bufferIndex;
				if (bufferIndex >= m_vertexBindings.size() || bindingPoint >= kGameTextureSrvDescriptorCount || bindingPoint >= allocation.count)
					continue;

				const auto& binding = m_vertexBindings[bufferIndex];
				if (!binding.valid || binding.size == 0)
					continue;

				D3D12_SHADER_RESOURCE_VIEW_DESC vertexSrv{};
				vertexSrv.Format = DXGI_FORMAT_R32_UINT;
				vertexSrv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
				vertexSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
				vertexSrv.Buffer.FirstElement = binding.offset / 4;
				// the view reaches the end of the buffer cache, like Vulkan's vertex buffer bind: BOTW's short grass
				// fetches past the size computed from the index/instance range, and D3D12 returned zeros there
				// (collapsed vertices, so that grass layer was missing or flickered)
				vertexSrv.Buffer.NumElements = std::max<UINT>(1, AlignUpU32(binding.size + (binding.offset & 3), 4) / 4);
				if (m_bufferCache.size > binding.offset)
					vertexSrv.Buffer.NumElements = std::max<UINT>(vertexSrv.Buffer.NumElements,
						(UINT)std::min<uint64>((m_bufferCache.size - binding.offset) / 4, std::numeric_limits<UINT>::max()));
				vertexSrv.Buffer.StructureByteStride = 0;
				vertexSrv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
				CreateShaderResourceViewChecked(m_bufferCache.resource.Get(), &vertexSrv, OffsetCpuHandle(allocation.cpu, descriptorSize, kGameSrvDescriptorOffset + bindingPoint), "vertex buffer", true);
				D3D12TraceEvent("vertex_buffer_srv", "draw={} pipeline=0x{:016x} stage={} shader=0x{:016x} attributeBufferIndex={} bindingPoint={} tableIndex={} tableGpu=0x{:016x} offset={} size={} firstElement={} numElements={} mainResource={}",
								(unsigned long long)drawId,
								(unsigned long long)pipelineHash,
								(uint32)shader->shaderType,
								shader->baseHash,
								bufferIndex,
								bindingPoint,
								allocation.index + kGameSrvDescriptorOffset + bindingPoint,
								(unsigned long long)allocation.gpu.ptr,
								binding.offset,
								binding.size,
								vertexSrv.Buffer.FirstElement,
								vertexSrv.Buffer.NumElements,
								(const void*)m_bufferCache.resource.Get());
			}
		}
	}

	uint32 hostTextureUnitBase = 0;
	const _LatteRegisterSetTextureUnit* texRegs = nullptr;
	switch (shader->shaderType)
	{
	case LatteConst::ShaderType::Vertex:
		hostTextureUnitBase = LATTE_CEMU_VS_TEX_UNIT_BASE;
		texRegs = RecordRegs().SQ_TEX_START_VS;
		break;
	case LatteConst::ShaderType::Pixel:
		hostTextureUnitBase = LATTE_CEMU_PS_TEX_UNIT_BASE;
		texRegs = RecordRegs().SQ_TEX_START_PS;
		break;
	case LatteConst::ShaderType::Geometry:
		hostTextureUnitBase = LATTE_CEMU_GS_TEX_UNIT_BASE;
		texRegs = RecordRegs().SQ_TEX_START_GS;
		break;
	default:
		return allocation;
	}

	for (uint32 textureListIndex = 0; textureListIndex < shader->textureUnitListCount; ++textureListIndex)
	{
		const uint32 relativeUnit = shader->textureUnitList[textureListIndex];
		const sint8 bindingPoint = shader->resourceMapping.textureUnitToBindingPoint[relativeUnit];
		if (bindingPoint < 0 || (uint32)bindingPoint >= kGameTextureSrvDescriptorCount || (uint32)bindingPoint >= allocation.count)
			continue;

		const uint32 hostTextureUnit = hostTextureUnitBase + relativeUnit;
		if (hostTextureUnit >= m_boundTextures.size())
			continue;

		D3D12_CPU_DESCRIPTOR_HANDLE source{};
		auto* texture = m_boundTextures[hostTextureUnit];
		// a view whose dimension differs from the shader declaration stays unbound
		const Latte::E_DIM shaderDim = shader->textureUnitDim[relativeUnit];
		if (texture &&
			((shaderDim == Latte::E_DIM::DIM_1D && texture->dim != Latte::E_DIM::DIM_1D) ||
			 (shaderDim == Latte::E_DIM::DIM_2D && texture->dim != Latte::E_DIM::DIM_2D && texture->dim != Latte::E_DIM::DIM_2D_MSAA)))
		{
			continue;
		}
		const FeedbackScratchCopy* feedbackScratch = GetFeedbackScratchCopy(hostTextureUnit);
		uint32 gpuSamplerSwizzle = 0x06880000;
		if (texRegs)
		{
			const auto& word4 = texRegs[relativeUnit].word4;
			gpuSamplerSwizzle = ((uint32)word4.get_DST_SEL_X() << 16) |
								((uint32)word4.get_DST_SEL_Y() << 19) |
								((uint32)word4.get_DST_SEL_Z() << 22) |
								((uint32)word4.get_DST_SEL_W() << 25);
		}

		if (!feedbackScratch && texture)
		{
			const auto& samplerSrv = texture->GetSamplerSRV(gpuSamplerSwizzle);
			if (samplerSrv.IsValid())
				source = samplerSrv.cpu;
			if (D3D12ShouldChecksumOutput() && shader->shaderType == LatteConst::ShaderType::Pixel && relativeUnit == 0)
			{
				static uint32 s_psTexture0StatsLogCount = 0;
				LogTextureStats(fmt::format("PS t0 shader={:016x} swizzle=0x{:08x}", shader->baseHash, gpuSamplerSwizzle),
								texture,
								false,
								s_psTexture0StatsLogCount,
								24);
			}
		}
		if (!feedbackScratch && !source.ptr)
			continue;

		const auto tableSlot = OffsetCpuHandle(allocation.cpu, descriptorSize, kGameSrvDescriptorOffset + (uint32)bindingPoint);
		if (feedbackScratch)
		{
			D3D12_SHADER_RESOURCE_VIEW_DESC scratchSrvDesc = feedbackScratch->srvDesc;
			if (feedbackScratch->sampledView)
				scratchSrvDesc.Shader4ComponentMapping = feedbackScratch->sampledView->GetShaderComponentMapping(gpuSamplerSwizzle);
			CreateShaderResourceViewChecked(feedbackScratch->texture.resource.Get(), &scratchSrvDesc, tableSlot, "feedback copy", true);
		}
		else
		{
			CopyDescriptorsSimpleChecked(1, tableSlot, source, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, "texture");
			if (texture && texture->baseTexture)
				m_tableBuildTextureRefs.push_back(texture->baseTexture);
		}
		D3D12TraceEvent("texture_srv_copy", "draw={} pipeline=0x{:016x} stage={} shader=0x{:016x} relativeUnit={} hostUnit={} bindingPoint={} tableIndex={} tableGpu=0x{:016x} sourceCpu=0x{:016x} feedbackScratch={} feedbackDraw={} feedbackScratchResource={} textureView={} texture={} addr=0x{:08x}",
						(unsigned long long)drawId,
						(unsigned long long)pipelineHash,
						(uint32)shader->shaderType,
						shader->baseHash,
						relativeUnit,
						hostTextureUnit,
						(sint32)bindingPoint,
						allocation.index + kGameSrvDescriptorOffset + (uint32)bindingPoint,
						(unsigned long long)allocation.gpu.ptr,
						(unsigned long long)source.ptr,
						feedbackScratch ? 1 : 0,
						feedbackScratch ? (unsigned long long)feedbackScratch->drawId : 0ull,
						feedbackScratch ? (const void*)feedbackScratch->texture.resource.Get() : nullptr,
						(const void*)texture,
						texture ? (const void*)texture->GetBaseImage() : nullptr,
						texture && texture->baseTexture ? texture->baseTexture->physAddress : 0);
	}

	if (cacheable)
	{
		m_gameResourceTableCache[cacheKey] = { allocation, m_tableBuildTextureRefs };
		m_gameResourceTableCacheGeneration = m_descriptorManager->GetShaderVisibleCbvSrvUavRollovers();
		if (auto* memo = GetCurrentTableKeyMemo(m_resourceTableKeyMemo, shader, descriptorGeneration))
		{
			memo->lookupDone = true;
			memo->lookupHit = true;
			memo->cachedAllocation = allocation;
		}
	}
	return allocation;
}

bool D3D12Renderer::UsesStreamoutUavTable(LatteDecompilerShader* shader) const
{
	const sint32 streamoutBindingPoint = shader ? shader->resourceMapping.tfStorageBindingPoint : -1;
	return streamoutBindingPoint >= 0 && (uint32)streamoutBindingPoint < kGameUavDescriptorCount && m_streamoutBuffer.resource;
}

D3D12DescriptorManager::Allocation D3D12Renderer::BuildGameUavTable(LatteDecompilerShader* shader)
{
	const uint32 descriptorGeneration = m_descriptorManager->GetShaderVisibleCbvSrvUavRollovers();
	const sint32 streamoutBindingPoint = shader ? shader->resourceMapping.tfStorageBindingPoint : -1;
	if (!UsesStreamoutUavTable(shader))
	{
		if (m_gameNullUavTable.IsValid() &&
			m_gameNullUavTableGeneration == descriptorGeneration)
		{
			return m_gameNullUavTable;
		}

		auto allocation = m_descriptorManager->AllocateShaderVisible(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kGameUavDescriptorCount);
		const uint32 descriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
		for (uint32 i = 0; i < kGameUavDescriptorCount; ++i)
			CopyDescriptorsSimpleChecked(1, OffsetCpuHandle(allocation.cpu, descriptorSize, i), m_nullUavDescriptor.cpu, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, "null UAV");
		m_gameNullUavTable = allocation;
		m_gameNullUavTableGeneration = descriptorGeneration;
		return allocation;
	}

	// the streamout buffer is made writable when the tables are emitted (EmitGameDescriptorTables)
	const uint64 cacheKey = BuildGameUavTableCacheKey(shader, descriptorGeneration);
	if (m_gameStreamoutUavTable.IsValid() &&
		m_gameStreamoutUavTableGeneration == descriptorGeneration &&
		m_gameStreamoutUavTableKey == cacheKey)
	{
		return m_gameStreamoutUavTable;
	}

	auto allocation = m_descriptorManager->AllocateShaderVisible(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kGameUavDescriptorCount);
	const uint32 descriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	for (uint32 i = 0; i < kGameUavDescriptorCount; ++i)
		CopyDescriptorsSimpleChecked(1, OffsetCpuHandle(allocation.cpu, descriptorSize, i), m_nullUavDescriptor.cpu, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, "null UAV");

	D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
	uavDesc.Format = DXGI_FORMAT_UNKNOWN;
	uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
	uavDesc.Buffer.FirstElement = 0;
	uavDesc.Buffer.NumElements = std::max<UINT>(1, (UINT)(m_streamoutBuffer.size / 4));
	uavDesc.Buffer.StructureByteStride = 4;
	uavDesc.Buffer.CounterOffsetInBytes = 0;
	uavDesc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;
	CreateUnorderedAccessViewChecked(m_streamoutBuffer.resource.Get(), &uavDesc,
		OffsetCpuHandle(allocation.cpu, descriptorSize, (uint32)streamoutBindingPoint), "streamout");
	D3D12TraceEvent("streamout_uav", "stage={} shader=0x{:016x} bindingPoint={} tableIndex={} tableGpu=0x{:016x} firstElement={} numElements={} streamoutResource={}",
					shader ? (uint32)shader->shaderType : 0,
					shader ? shader->baseHash : 0,
					streamoutBindingPoint,
					allocation.index + (uint32)streamoutBindingPoint,
					(unsigned long long)allocation.gpu.ptr,
					uavDesc.Buffer.FirstElement,
					uavDesc.Buffer.NumElements,
					(const void*)m_streamoutBuffer.resource.Get());
	m_gameStreamoutUavTable = allocation;
	m_gameStreamoutUavTableKey = cacheKey;
	m_gameStreamoutUavTableGeneration = descriptorGeneration;
	return allocation;
}

D3D12DescriptorManager::Allocation D3D12Renderer::BuildGameSamplerTable(LatteDecompilerShader* shader)
{
	const uint32 descriptorGeneration = m_descriptorManager->GetShaderVisibleSamplerRollovers();
	if (m_gameSamplerTableCacheGeneration != descriptorGeneration)
	{
		m_gameSamplerTableCache.clear();
		m_gameSamplerTableCacheGeneration = descriptorGeneration;
	}

	bool cacheable = false;
	const uint64 cacheKey = GetMemoizedTableCacheKey(m_samplerTableKeyMemo, true, shader, descriptorGeneration, cacheable);
	if (cacheable)
	{
		const auto* memo = GetCurrentTableKeyMemo(m_samplerTableKeyMemo, shader, descriptorGeneration);
		if (memo && memo->lookupDone)
		{
			if (memo->lookupHit)
			{
				m_gameSamplerTableCacheHits++;
				return memo->cachedAllocation;
			}
		}
		else if (auto it = m_gameSamplerTableCache.find(cacheKey); it != m_gameSamplerTableCache.end())
		{
			m_gameSamplerTableCacheHits++;
			return it->second.allocation;
		}
		m_gameSamplerTableCacheMisses++;
	}
	m_tableBuildTextureRefs.clear();

	const uint32 descriptorCount = GetGameSamplerTableDescriptorCount(shader);
	auto allocation = m_descriptorManager->AllocateShaderVisible(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, descriptorCount);
	const uint32 descriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
	const D3D12_SAMPLER_DESC defaultSamplerDesc = BuildD3D12DefaultGameSamplerDesc();
	D3D12SamplerTableState::FinalSlots<D3D12_SAMPLER_DESC, kGameSamplerDescriptorCount>
		finalSamplers(descriptorCount, defaultSamplerDesc);

	if (shader)
	{
		uint32 hostTextureUnitBase = 0;
		switch (shader->shaderType)
		{
		case LatteConst::ShaderType::Vertex:
			hostTextureUnitBase = LATTE_CEMU_VS_TEX_UNIT_BASE;
			break;
		case LatteConst::ShaderType::Pixel:
			hostTextureUnitBase = LATTE_CEMU_PS_TEX_UNIT_BASE;
			break;
		case LatteConst::ShaderType::Geometry:
			hostTextureUnitBase = LATTE_CEMU_GS_TEX_UNIT_BASE;
			break;
		default:
			break;
		}

		for (uint32 textureListIndex = 0; textureListIndex < shader->textureUnitListCount; ++textureListIndex)
		{
			const uint32 relativeUnit = shader->textureUnitList[textureListIndex];
			const sint8 bindingPoint = shader->resourceMapping.textureUnitToBindingPoint[relativeUnit];
			if (bindingPoint < 0 || (uint32)bindingPoint >= descriptorCount)
				continue;

			const uint32 hostTextureUnit = hostTextureUnitBase + relativeUnit;
			LatteTextureViewD3D12* textureView = hostTextureUnit < m_boundTextures.size() ? m_boundTextures[hostTextureUnit] : nullptr;
			D3D12_SAMPLER_DESC samplerDesc = BuildD3D12GameSamplerDesc(shader, relativeUnit, textureView, m_anisoFilterWithPointMipSupported);
			finalSamplers.Set((uint32)bindingPoint, samplerDesc);
			// Sampler tables retain state derived from each referenced texture view.
			if (textureView && textureView->baseTexture)
				m_tableBuildTextureRefs.push_back(textureView->baseTexture);
		}
	}

	finalSamplers.ForEach([&](size_t index, const D3D12_SAMPLER_DESC& samplerDesc)
	{
		CreateSamplerChecked(&samplerDesc, OffsetCpuHandle(allocation.cpu, descriptorSize, (uint32)index), "game sampler");
	});

	if (cacheable)
	{
		m_gameSamplerTableCache[cacheKey] = { allocation, m_tableBuildTextureRefs };
		if (auto* memo = GetCurrentTableKeyMemo(m_samplerTableKeyMemo, shader, descriptorGeneration))
		{
			memo->lookupDone = true;
			memo->lookupHit = true;
			memo->cachedAllocation = allocation;
		}
	}
	return allocation;
}

D3D12DescriptorManager::Allocation D3D12Renderer::GetShaderVisibleOutputSampler(bool linear)
{
	if (!m_descriptorManager)
		return {};

	const auto stats = m_descriptorManager->GetStats();
	if (m_shaderVisibleOutputSamplerGeneration != stats.shaderVisibleSamplerRollovers)
	{
		m_shaderVisibleOutputLinearSampler = {};
		m_shaderVisibleOutputPointSampler = {};
		m_shaderVisibleOutputSamplerGeneration = stats.shaderVisibleSamplerRollovers;
	}

	auto& cachedSampler = linear ? m_shaderVisibleOutputLinearSampler : m_shaderVisibleOutputPointSampler;
	if (cachedSampler.IsValid())
		return cachedSampler;

	const auto& sourceSampler = linear ? m_outputLinearSampler : m_outputPointSampler;
	if (!sourceSampler.IsValid())
		return {};
	cachedSampler = CopyToShaderVisibleChecked(sourceSampler, "output sampler");
	return cachedSampler;
}

void D3D12Renderer::BindGameDescriptorTables(LatteDecompilerShader* vertexShader, LatteDecompilerShader* geometryShader, LatteDecompilerShader* pixelShader, uint32 baseVertex, uint32 baseInstance, uint32 vertexIdRemapMode, uint64 pipelineHash)
{
	if (!m_commandListOpen || !m_gameRootSignature)
		return;
	EmitGameDescriptorTables(BuildGameDescriptorTables(vertexShader, geometryShader, pixelShader, pipelineHash),
		vertexShader, geometryShader, pixelShader, baseVertex, baseInstance, vertexIdRemapMode, pipelineHash);
}

D3D12Renderer::GameDescriptorTables D3D12Renderer::BuildGameDescriptorTables(LatteDecompilerShader* vertexShader, LatteDecompilerShader* geometryShader, LatteDecompilerShader* pixelShader, uint64 pipelineHash)
{
	GameDescriptorTables tables{};
	D3D12_GPU_VIRTUAL_ADDRESS unusedCbv = 0;
	const uint64 drawId = m_gameDrawCallsSubmitted + 1;
	LatteDecompilerShader* stages[3] = { vertexShader, pixelShader, geometryShader };
	for (uint32 i = 0; i < 3; i++)
		tables.resource[i] = BuildGameResourceTable(stages[i], unusedCbv, drawId, pipelineHash);
	for (uint32 i = 0; i < 3; i++)
		tables.uav[i] = BuildGameUavTable(stages[i]);
	for (uint32 i = 0; i < 3; i++)
		tables.sampler[i] = BuildGameSamplerTable(stages[i]);
	return tables;
}

void D3D12Renderer::EmitGameDescriptorTables(const GameDescriptorTables& tables, LatteDecompilerShader* vertexShader, LatteDecompilerShader* geometryShader, LatteDecompilerShader* pixelShader, uint32 baseVertex, uint32 baseInstance, uint32 vertexIdRemapMode, uint64 pipelineHash)
{
	if (!m_commandListOpen || !m_gameRootSignature)
		return;
	auto* commandList = GetCurrentCommandSlot().commandList.Get();
	SetD3D12GraphicsRootSignature(m_gameRootSignature.Get());
	const uint64 drawId = m_gameDrawCallsSubmitted + 1;
	const auto& vsTable = tables.resource[0];
	const auto& psTable = tables.resource[1];
	const auto& gsTable = tables.resource[2];
	// uniform bytes were prepared before the split (PrepareGameUniformVars reads guest state)
	const D3D12_GPU_VIRTUAL_ADDRESS vsUniformCbv = vertexShader ? CommitGameUniformVars(vertexShader, drawId, pipelineHash) : 0;
	const D3D12_GPU_VIRTUAL_ADDRESS psUniformCbv = pixelShader ? CommitGameUniformVars(pixelShader, drawId, pipelineHash) : 0;
	const D3D12_GPU_VIRTUAL_ADDRESS gsUniformCbv = geometryShader ? CommitGameUniformVars(geometryShader, drawId, pipelineHash) : 0;
	// tables may point at the buffer cache through uniform/vertex buffer SRVs (cached tables too): make it readable
	if (m_bufferCache.resource)
		Transition(m_bufferCache.resource.Get(), m_bufferCache.state,
				   D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER |
					   D3D12_RESOURCE_STATE_INDEX_BUFFER |
					   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
					   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	if (UsesStreamoutUavTable(vertexShader) || UsesStreamoutUavTable(pixelShader) || UsesStreamoutUavTable(geometryShader))
		Transition(m_streamoutBuffer.resource.Get(), m_streamoutBuffer.state, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	const auto& vsUavTable = tables.uav[0];
	const auto& psUavTable = tables.uav[1];
	const auto& gsUavTable = tables.uav[2];
	const auto& vsSamplerTable = tables.sampler[0];
	const auto& psSamplerTable = tables.sampler[1];
	const auto& gsSamplerTable = tables.sampler[2];
	// building tables may roll the heap page; bind only after all tables are resolved
	BindShaderVisibleHeaps();

	const float invViewportWidth = 1.0f / std::max<float>(m_currentViewport.Width, 1.0f);
	const float invViewportHeight = 1.0f / std::max<float>(std::abs(m_currentViewport.Height), 1.0f);
	const auto& pointSizeReg = RecordRegs().PA_SU_POINT_SIZE;
	const float pointWidth = (float)pointSizeReg.get_WIDTH() / 8.0f;
	const float pointHeight = (float)pointSizeReg.get_HEIGHT() / 8.0f;
	uint32 drawConstants[kGameDrawConstantCount] =
	{
		m_logicOpEmulationMode,
		baseVertex,
		baseInstance,
		vertexIdRemapMode,
		std::bit_cast<uint32>(invViewportWidth),
		std::bit_cast<uint32>(invViewportHeight),
		std::bit_cast<uint32>(pointWidth),
		std::bit_cast<uint32>(pointHeight),
	};
	if (auto* fetchShader = s_recordFetchShader)
	{
		for (auto& group : fetchShader->bufferGroups)
		{
			if (group.attributeBufferIndex < LATTE_MAX_VERTEX_BUFFERS)
				drawConstants[8 + group.attributeBufferIndex] = group.getCurrentBufferStride(RecordRegWords());
		}
	}
	for (uint32 bufferIndex = 0; bufferIndex < LATTE_MAX_VERTEX_BUFFERS; ++bufferIndex)
	{
		if (bufferIndex < m_vertexBindings.size() && m_vertexBindings[bufferIndex].valid)
			drawConstants[24] |= (m_vertexBindings[bufferIndex].offset & 3u) << (bufferIndex * 2);
	}
	if (D3D12ShouldTrace())
	{
		std::string strideSummary;
		for (uint32 i = 0; i < LATTE_MAX_VERTEX_BUFFERS; ++i)
		{
			if (!strideSummary.empty())
				strideSummary.push_back(',');
			fmt::format_to(std::back_inserter(strideSummary), "{}:{}", i, drawConstants[8 + i]);
		}
		D3D12TraceEvent("draw_vertex_stride_constants", "draw={} pipeline=0x{:016x} strides={}",
						(unsigned long long)drawId,
						(unsigned long long)pipelineHash,
						strideSummary);
	}
	D3D12TraceEvent("draw_descriptor_tables", "draw={} pipeline=0x{:016x} vs=0x{:016x} ps=0x{:016x} gs=0x{:016x} rootResourceTables=0,1,2 rootCbvs=3,4,5 rootDrawConstants=6 rootSamplerTables=7,8,9 rootUavTables=10,11,12 vsTableGpu=0x{:016x} psTableGpu=0x{:016x} gsTableGpu=0x{:016x} vsTableIndex={} psTableIndex={} gsTableIndex={} vsCbv=0x{:016x} psCbv=0x{:016x} gsCbv=0x{:016x} drawConstLogicMode={} drawConstBaseVertex={} drawConstBaseInstance={} drawConstReserved={} invViewportBits=0x{:08x},0x{:08x} invViewportFloat={:.9g},{:.9g} vsSamplerGpu=0x{:016x} psSamplerGpu=0x{:016x} gsSamplerGpu=0x{:016x} vsUavGpu=0x{:016x} psUavGpu=0x{:016x} gsUavGpu=0x{:016x}",
					(unsigned long long)drawId,
					(unsigned long long)pipelineHash,
					vertexShader ? vertexShader->baseHash : 0,
					pixelShader ? pixelShader->baseHash : 0,
					geometryShader ? geometryShader->baseHash : 0,
					(unsigned long long)vsTable.gpu.ptr,
					(unsigned long long)psTable.gpu.ptr,
					(unsigned long long)gsTable.gpu.ptr,
					vsTable.index,
					psTable.index,
					gsTable.index,
					(unsigned long long)vsUniformCbv,
					(unsigned long long)psUniformCbv,
					(unsigned long long)gsUniformCbv,
					drawConstants[0],
					drawConstants[1],
					drawConstants[2],
					drawConstants[3],
					drawConstants[4],
					drawConstants[5],
					invViewportWidth,
					invViewportHeight,
					(unsigned long long)vsSamplerTable.gpu.ptr,
					(unsigned long long)psSamplerTable.gpu.ptr,
					(unsigned long long)gsSamplerTable.gpu.ptr,
					(unsigned long long)vsUavTable.gpu.ptr,
					(unsigned long long)psUavTable.gpu.ptr,
					(unsigned long long)gsUavTable.gpu.ptr);
	auto traceMissingUniformCbv = [drawId, pipelineHash](const char* stageName, LatteDecompilerShader* shader, D3D12_GPU_VIRTUAL_ADDRESS cbvAddress)
	{
		if (!shader || cbvAddress != 0 || shader->uniform.uniformRangeSize == 0 || shader->resourceMapping.uniformVarsBufferBindingPoint != 0)
			return;
		D3D12TraceEvent("draw_uniform_cbv_missing", "draw={} pipeline=0x{:016x} stage={} shader=0x{:016x} aux=0x{:016x} rangeSize={} bindingPoint={} locRemapped={} locUniformRegister={} uniformRegisterCount={} locWindowToClip={} locFragCoordScale={} locAlphaRef={} locPointSize={}",
						(unsigned long long)drawId,
						(unsigned long long)pipelineHash,
						stageName,
						shader->baseHash,
						shader->auxHash,
						shader->uniform.uniformRangeSize,
						(sint32)shader->resourceMapping.uniformVarsBufferBindingPoint,
						shader->uniform.loc_remapped,
						shader->uniform.loc_uniformRegister,
						shader->uniform.count_uniformRegister,
						shader->uniform.loc_windowSpaceToClipSpaceTransform,
						shader->uniform.loc_fragCoordScale,
						shader->uniform.loc_alphaTestRef,
						shader->uniform.loc_pointSize);
	};
	traceMissingUniformCbv("VS", vertexShader, vsUniformCbv);
	traceMissingUniformCbv("PS", pixelShader, psUniformCbv);
	traceMissingUniformCbv("GS", geometryShader, gsUniformCbv);
	auto setRootDescriptorTable = [this, commandList](uint32 rootIndex, D3D12_GPU_DESCRIPTOR_HANDLE table)
	{
		if (rootIndex < m_gameRootDescriptorTableValid.size() &&
			m_gameRootDescriptorTableValid[rootIndex] &&
			m_gameRootDescriptorTables[rootIndex].ptr == table.ptr)
		{
			m_commandStateBindSkips++;
			return;
		}
		commandList->SetGraphicsRootDescriptorTable(rootIndex, table);
		if (rootIndex < m_gameRootDescriptorTableValid.size())
		{
			m_gameRootDescriptorTables[rootIndex] = table;
			m_gameRootDescriptorTableValid[rootIndex] = true;
		}
		m_commandStateBinds++;
	};
	auto setRootCbv = [this, commandList](uint32 rootIndex, D3D12_GPU_VIRTUAL_ADDRESS address)
	{
		if (address == 0)
			return;
		if (rootIndex < m_gameRootCbvValid.size() &&
			m_gameRootCbvValid[rootIndex] &&
			m_gameRootCbvs[rootIndex] == address)
		{
			m_commandStateBindSkips++;
			return;
		}
		commandList->SetGraphicsRootConstantBufferView(rootIndex, address);
		if (rootIndex < m_gameRootCbvValid.size())
		{
			m_gameRootCbvs[rootIndex] = address;
			m_gameRootCbvValid[rootIndex] = true;
		}
		m_commandStateBinds++;
	};
	auto setRootDrawConstants = [this, commandList](const uint32 (&constants)[kGameDrawConstantCount])
	{
		if (m_gameRootDrawConstantsValid &&
			std::memcmp(m_gameRootDrawConstants.data(), constants, sizeof(constants)) == 0)
		{
			m_commandStateBindSkips++;
			return;
		}
		commandList->SetGraphicsRoot32BitConstants(6, kGameDrawConstantCount, constants, 0);
		std::copy(std::begin(constants), std::end(constants), m_gameRootDrawConstants.begin());
		m_gameRootDrawConstantsValid = true;
		m_commandStateBinds++;
	};
	setRootDescriptorTable(0, vsTable.gpu);
	setRootDescriptorTable(1, psTable.gpu);
	setRootDescriptorTable(2, gsTable.gpu);
	setRootCbv(3, vsUniformCbv);
	setRootCbv(4, psUniformCbv);
	setRootCbv(5, gsUniformCbv);
	setRootDrawConstants(drawConstants);
	setRootDescriptorTable(7, vsSamplerTable.gpu);
	setRootDescriptorTable(8, psSamplerTable.gpu);
	setRootDescriptorTable(9, gsSamplerTable.gpu);
	setRootDescriptorTable(10, vsUavTable.gpu);
	setRootDescriptorTable(11, psUavTable.gpu);
	setRootDescriptorTable(12, gsUavTable.gpu);
}

