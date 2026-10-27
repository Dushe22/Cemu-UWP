// Part of D3D12Renderer.cpp (included there, same translation unit): framebuffer binding, texture uploads, clears, copies, readbacks and surface copies.
// Not a standalone file: it relies on the includes and file-local helpers of D3D12Renderer.cpp.

void D3D12Renderer::rendertarget_bindFramebufferObject(LatteCachedFBO* cfbo)
{
	if (!t_recordContext)
		m_latteActiveFBO = static_cast<CachedFBOD3D12*>(cfbo);
	D3D12_QUEUE_RECORDING(rendertarget_bindFramebufferObject(cfbo));
#if defined(CEMU_UWP_D3D12_DIAGNOSTICS)
	if (m_activeFBO && m_activeFBO != cfbo && D3D12ShouldTraceGpu())
		TraceFboAttachments("fbo_attachment_unbind", "before_unbind", 0, 0,
			"bind_call=rendertarget_bindFramebufferObject completedPass=1");
#endif
	// feedback copies are only shared within one render pass
	if (cfbo != m_activeFBO)
		DropRetainedFeedbackCopies();
	m_activeFBO = static_cast<CachedFBOD3D12*>(cfbo);
	static uint32 s_bindFboLogCount = 0;
	if (D3D12RuntimeLogCapped(s_bindFboLogCount, "rendertarget_bindFramebufferObject", 128))
	{
		if (m_activeFBO)
		{
			cemuLog_log(LogType::Force, "D3D12: bind FBO key=0x{:016x} size={}x{} colors={} mask=0x{:02x} depth={} sparse={}.",
						m_activeFBO->key,
						m_activeFBO->m_size.x,
						m_activeFBO->m_size.y,
						m_activeFBO->GetColorAttachmentCount(),
						m_activeFBO->GetColorAttachmentMask(),
						m_activeFBO->HasDepthStencilAttachment(),
						m_activeFBO->HasSparseColorAttachments());
		}
		else
			cemuLog_log(LogType::Force, "D3D12: bind null FBO.");
	}
	if (m_activeFBO)
	{
		D3D12TraceEvent("fbo_bind", "key=0x{:016x} size={}x{} colors={} mask=0x{:02x} depth={} sparse={}",
						m_activeFBO->key,
						m_activeFBO->m_size.x,
						m_activeFBO->m_size.y,
						m_activeFBO->GetColorAttachmentCount(),
						m_activeFBO->GetColorAttachmentMask(),
						m_activeFBO->HasDepthStencilAttachment() ? 1 : 0,
						m_activeFBO->HasSparseColorAttachments() ? 1 : 0);
		TraceFboAttachments("fbo_attachment_bind", "after_bind", 0, 0, "bind_call=rendertarget_bindFramebufferObject");
	}
	else
		D3D12TraceEvent("fbo_bind", "key=0x0000000000000000 null=1");
	if (m_commandListOpen)
		BindFramebuffer(m_activeFBO);
}

void D3D12Renderer::NotifyTextureDeletion(LatteTexture* texture)
{
	if (!t_isRecorderThread)
		m_latteBoundTextures.fill(nullptr);
	D3D12_QUEUE_RECORDING_KIND(Sync, NotifyTextureDeletion(texture));
	if (!texture)
		return;
	DropRetainedFeedbackCopies();

	BumpAllTableBindingGenerations();
	for (auto& boundTexture : m_boundTextures)
	{
		if (boundTexture && boundTexture->baseTexture == texture)
			boundTexture = nullptr;
	}
	for (auto& cachedOverlap : m_feedbackOverlapCache)
	{
		if (cachedOverlap.sampledView && cachedOverlap.sampledView->baseTexture == texture)
			cachedOverlap = {};
		else if (cachedOverlap.attachment && cachedOverlap.attachment->baseTexture == texture)
			cachedOverlap = {};
	}

	if (m_activeFBO)
	{
		bool activeFboReferencesTexture = false;
		for (const auto& attachment : m_activeFBO->GetColorAttachments())
		{
			if (attachment.valid && attachment.view && attachment.view->baseTexture == texture)
			{
				activeFboReferencesTexture = true;
				break;
			}
		}
		if (!activeFboReferencesTexture && m_activeFBO->depthBuffer.texture && m_activeFBO->depthBuffer.texture->baseTexture == texture)
			activeFboReferencesTexture = true;
		if (activeFboReferencesTexture)
			m_activeFBO = nullptr;
	}

	const auto referencesTexture = [texture](const auto& entry)
	{
		const auto& refs = entry.second.referencedTextures;
		return std::find(refs.begin(), refs.end(), texture) != refs.end();
	};
	std::erase_if(m_gameResourceTableCache, referencesTexture);
	std::erase_if(m_gameSamplerTableCache, referencesTexture);
	m_resourceTableKeyMemo = {};
	m_samplerTableKeyMemo = {};
}

void* D3D12Renderer::texture_acquireTextureUploadBuffer(uint32 size)
{
	return m_memoryManager ? m_memoryManager->AcquireTextureUploadBuffer(size) : nullptr;
}

void D3D12Renderer::texture_releaseTextureUploadBuffer(uint8* mem)
{
	if (m_memoryManager)
		m_memoryManager->ReleaseTextureUploadBuffer(mem);
}
TextureDecoder* D3D12Renderer::texture_chooseDecodedFormat(Latte::E_GX2SURFFMT format, bool isDepth, Latte::E_DIM dim, uint32 width, uint32 height)
{
	return GetD3D12FormatInfo(format, isDepth, dim, width, height).decoder;
}

void D3D12Renderer::texture_clearSlice(LatteTexture* hostTexture, sint32 sliceIndex, sint32 mipIndex)
{
	D3D12_EXCLUSIVE_RECORDING();
	if (!hostTexture)
		return;
	DropRetainedFeedbackCopies(hostTexture);
	if (hostTexture->isDepth)
		texture_clearDepthSlice(hostTexture, sliceIndex, mipIndex, true, hostTexture->hasStencil, 0.0f, 0);
	else
		texture_clearColorSlice(hostTexture, sliceIndex, mipIndex, 0.0f, 0.0f, 0.0f, 0.0f);
}

void D3D12Renderer::QueueCpuSliceUpload(CpuSliceUploadJob&& job)
{
	constexpr size_t kMaxPendingBytes = 64ull * 1024 * 1024; // bounds the copies held while a 110 MB array uploads
	std::unique_lock lock(m_cpuUploadMutex);
	if (m_cpuUploadThreads.empty())
	{
		const uint32 hardwareThreads = std::max(1u, std::thread::hardware_concurrency());
		// the game is stalled on the reload while these run (BOTW terrain: ~1.8 s of the 2.4 s with 4 threads)
		const uint32 workerCount = std::clamp(hardwareThreads > 2 ? hardwareThreads - 2 : 1u, 1u, 6u);
		m_cpuUploadStop = false;
		for (uint32 i = 0; i < workerCount; ++i)
			m_cpuUploadThreads.emplace_back(&D3D12Renderer::CpuSliceUploadThreadFunc, this);
		cemuLog_log(LogType::Force, "D3D12: {} threads write CPU-writable texture slices.", workerCount);
	}
	while (m_cpuUploadPendingBytes >= kMaxPendingBytes)
	{
		// the caller would only wait here, so it writes a queued slice instead
		if (!m_cpuUploadJobs.empty())
		{
			CpuSliceUploadJob queued = std::move(m_cpuUploadJobs.front());
			m_cpuUploadJobs.pop_front();
			lock.unlock();
			RunCpuSliceUploadJob(queued);
			lock.lock();
			continue;
		}
		m_cpuUploadDone.wait(lock);
	}
	m_cpuUploadPendingBytes += job.data.size();
	m_cpuUploadOutstanding.fetch_add(1, std::memory_order_acq_rel);
	m_cpuUploadJobs.emplace_back(std::move(job));
	m_cpuUploadWork.notify_one();
}

void D3D12Renderer::RunCpuSliceUploadJob(CpuSliceUploadJob& job)
{
	// Map/Unmap are free-threaded; each job writes a different subresource
	HRESULT hr = job.resource->Map(job.subresource, nullptr, nullptr);
	if (SUCCEEDED(hr))
	{
		hr = job.resource->WriteToSubresource(job.subresource, nullptr, job.data.data(), job.rowPitch, job.depthPitch);
		job.resource->Unmap(job.subresource, nullptr);
	}
	if (FAILED(hr) && m_cpuUploadFailures.fetch_add(1, std::memory_order_relaxed) == 0)
		cemuLog_log(LogType::Force, "D3D12: threaded texture slice write failed with 0x{:08x} (subresource {}).", (uint32)hr, job.subresource);
	{
		std::scoped_lock lock(m_cpuUploadMutex);
		m_cpuUploadPendingBytes -= job.data.size();
		m_cpuUploadOutstanding.fetch_sub(1, std::memory_order_acq_rel);
	}
	m_cpuUploadDone.notify_all();
}

void D3D12Renderer::CpuSliceUploadThreadFunc()
{
	SetThreadName("d3d12SliceUpload");
	while (true)
	{
		CpuSliceUploadJob job;
		{
			std::unique_lock lock(m_cpuUploadMutex);
			m_cpuUploadWork.wait(lock, [&]() { return m_cpuUploadStop || !m_cpuUploadJobs.empty(); });
			if (m_cpuUploadJobs.empty())
				return;
			job = std::move(m_cpuUploadJobs.front());
			m_cpuUploadJobs.pop_front();
		}
		RunCpuSliceUploadJob(job);
	}
}

void D3D12Renderer::texture_finishSliceUploads()
{
	D3D12_EXCLUSIVE_RECORDING();
	if (m_cpuUploadOutstanding.load(std::memory_order_acquire) == 0)
		return;
	std::unique_lock lock(m_cpuUploadMutex);
	while (m_cpuUploadOutstanding.load(std::memory_order_acquire) != 0)
	{
		// the caller would only wait here, so it writes slices too
		if (!m_cpuUploadJobs.empty())
		{
			CpuSliceUploadJob job = std::move(m_cpuUploadJobs.front());
			m_cpuUploadJobs.pop_front();
			lock.unlock();
			RunCpuSliceUploadJob(job);
			lock.lock();
			continue;
		}
		m_cpuUploadDone.wait(lock);
	}
}

void D3D12Renderer::StopCpuSliceUploadThreads()
{
	std::vector<std::thread> threads;
	{
		std::scoped_lock lock(m_cpuUploadMutex);
		m_cpuUploadStop = true;
		threads.swap(m_cpuUploadThreads);
	}
	m_cpuUploadWork.notify_all();
	for (auto& thread : threads)
		thread.join();
}

D3D12Renderer::ArrayUploadStaging* D3D12Renderer::GetArrayUploadStaging(DXGI_FORMAT format, uint32 width, uint32 height)
{
	const uint64 key = ((uint64)format << 48) | ((uint64)width << 24) | height;
	if (auto it = m_arrayUploadStaging.find(key); it != m_arrayUploadStaging.end())
		return &it->second;
	ArrayUploadStaging staging;
	staging.desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	staging.desc.Width = width;
	staging.desc.Height = height;
	staging.desc.DepthOrArraySize = 1;
	staging.desc.MipLevels = 1;
	staging.desc.Format = format;
	staging.desc.SampleDesc.Count = 1;
	staging.desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	if (!CreateTextureWithRecovery(staging.desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, staging.texture, "array upload staging"))
		return nullptr;
	staging.state = D3D12_RESOURCE_STATE_COPY_DEST;
	return &m_arrayUploadStaging.emplace(key, std::move(staging)).first->second;
}

void D3D12Renderer::texture_loadSlice(LatteTexture* hostTexture, sint32 width, sint32 height, sint32 depth, void* pixelData, sint32 sliceIndex, sint32 mipIndex, uint32 compressedImageSize)
{
	D3D12_EXCLUSIVE_RECORDING();
	DropRetainedFeedbackCopies(hostTexture);
	stdx::scope_exit deviceCheck([&]() { CheckDeviceAfterOperation("texture upload", [&]() {
		return fmt::format("{} gx2fmt=0x{:04x} mip={} slice={} {}x{}x{} bytes={}", D3D12DescribeResource(hostTexture ? static_cast<LatteTextureD3D12*>(hostTexture)->GetResource() : nullptr),
			hostTexture ? (uint32)hostTexture->format : 0, mipIndex, sliceIndex, width, height, depth, compressedImageSize); }); });
	auto* texture = static_cast<LatteTextureD3D12*>(hostTexture);
	if (!texture || !texture->GetResource() || !pixelData || compressedImageSize == 0)
		return;
	const auto& textureDesc = texture->GetDesc();
	if (mipIndex < 0 || mipIndex >= (sint32)textureDesc.MipLevels || sliceIndex < 0 || width <= 0 || height <= 0)
		return;
	const bool is3DTexture = texture->Is3DTexture();
	const uint32 mipWidth = std::max<uint32>(1, (uint32)(textureDesc.Width >> mipIndex));
	const uint32 mipHeight = std::max<uint32>(1, (uint32)(textureDesc.Height >> mipIndex));
	const uint32 sliceLimit = is3DTexture ?
		std::max<uint32>(1, (uint32)textureDesc.DepthOrArraySize >> mipIndex) :
		std::max<uint32>(1, texture->GetArraySize());
	if ((uint32)sliceIndex >= sliceLimit || (uint32)width != mipWidth || (uint32)height != mipHeight)
	{
		cemuLog_log(LogType::Force,
			"D3D12: rejecting texture upload outside the destination subresource (format=0x{:04x}, mip={}, slice={}, supplied={}x{}, expected={}x{}, slices={}).",
			(uint32)texture->format, mipIndex, sliceIndex, width, height, mipWidth, mipHeight, sliceLimit);
		return;
	}
	if (texture->IsCpuWritable())
	{
		// the GPU may still read a reloaded texture, so let it finish before overwriting
		if (texture->reloadCount > 1 && texture->m_cpuWriteSyncedReload != (uint32)texture->reloadCount)
		{
			WaitForIdle();
			texture->m_cpuWriteSyncedReload = (uint32)texture->reloadCount;
		}
		const uint32 cpuSubresource = texture->GetSubresourceIndex(sliceIndex, mipIndex);
		D3D12_PLACED_SUBRESOURCE_FOOTPRINT cpuFootprint{};
		UINT cpuRows = 0;
		UINT64 cpuRowBytes = 0;
		UINT64 cpuTotalBytes = 0;
		m_device->GetCopyableFootprints(&textureDesc, cpuSubresource, 1, 0, &cpuFootprint, &cpuRows, &cpuRowBytes, &cpuTotalBytes);
		const uint32 cpuSourceRowPitch = cpuRows != 0 ? compressedImageSize / cpuRows : compressedImageSize;
		ID3D12Resource* resource = texture->GetResource();
		if (cpuRows != 0 && cpuSourceRowPitch >= cpuRowBytes && compressedImageSize >= 32 * 1024)
		{
			// large slices go to the upload threads; the reload waits for them in texture_finishSliceUploads
			CpuSliceUploadJob job;
			job.resource = resource;
			job.subresource = cpuSubresource;
			job.rowPitch = cpuSourceRowPitch;
			job.depthPitch = cpuSourceRowPitch * cpuRows;
			const auto* bytes = static_cast<const uint8*>(pixelData);
			job.data.assign(bytes, bytes + (size_t)cpuSourceRowPitch * cpuRows);
			QueueCpuSliceUpload(std::move(job));
			texture->MarkSubresourceWritten(cpuSubresource);
			return;
		}
		if (cpuRows == 0 || cpuSourceRowPitch < cpuRowBytes || FAILED(resource->Map(cpuSubresource, nullptr, nullptr)))
		{
			cemuLog_logOnce(LogType::Force, "D3D12: CPU texture upload failed (format=0x{:04x}, slice={}, mip={}, bytes={}).",
				(uint32)texture->format, sliceIndex, mipIndex, compressedImageSize);
			return;
		}
		const HRESULT hr = resource->WriteToSubresource(cpuSubresource, nullptr, pixelData, cpuSourceRowPitch, cpuSourceRowPitch * cpuRows);
		resource->Unmap(cpuSubresource, nullptr);
		if (ShouldCheckDevice(true) && FAILED(m_device->GetDeviceRemovedReason()))
		{
			ReportDeviceRemovalAfter("WriteToSubresource", "CPU texture upload", fmt::format("{} subresource={} rowPitch={} rows={} bytes={} hr=0x{:08x}",
				D3D12DescribeResource(resource), cpuSubresource, cpuSourceRowPitch, cpuRows, compressedImageSize, (uint32)hr));
		}
		if (FAILED(hr))
		{
			cemuLog_logOnce(LogType::Force, "D3D12: WriteToSubresource failed with 0x{:08x} (format=0x{:04x}).", (uint32)hr, (uint32)texture->format);
			return;
		}
		texture->MarkSubresourceWritten(cpuSubresource);
		return;
	}
	if (!m_commandListOpen)
		ResetCommandList();

	const uint32 subresource = texture->GetSubresourceIndex(is3DTexture ? 0 : sliceIndex, mipIndex);
	if (texture->GetFormatInfo().hasStencil)
	{
		if (is3DTexture || width <= 0 || height <= 0)
			return;

		const uint32 mipLevels = std::max<uint32>(textureDesc.MipLevels, 1);
		const uint32 arraySize = std::max<uint32>(texture->GetArraySize(), 1);
		const uint32 sourceTexelBytes =
			texture->format == Latte::E_GX2SURFFMT::D24_S8_UNORM ? 4u : 8u;
		const uint32 stencilByteOffset = sourceTexelBytes == 4 ? 3u : 4u;
		const uint64 sourceRowPitch = (uint64)width * sourceTexelBytes;
		const uint64 requiredSourceBytes = sourceRowPitch * (uint64)height;
		if (requiredSourceBytes > compressedImageSize)
		{
			cemuLog_log(LogType::Force,
				"D3D12: depth-stencil upload is undersized (format=0x{:04x}, required={}, supplied={}).",
				(uint32)texture->format,
				(unsigned long long)requiredSourceBytes,
				compressedImageSize);
			return;
		}

		const uint8* sourcePixels = static_cast<const uint8*>(pixelData);
		for (uint32 plane = 0; plane < 2; ++plane)
		{
			const uint32 planeSubresource = subresource + plane * mipLevels * arraySize;
			const D3D12_RESOURCE_STATES previousState = texture->GetSubresourceState(planeSubresource);
			D3D12_PLACED_SUBRESOURCE_FOOTPRINT planeFootprint{};
			UINT planeRows = 0;
			UINT64 planeRowBytes = 0;
			UINT64 planeTotalBytes = 0;
			m_device->GetCopyableFootprints(&textureDesc, planeSubresource, 1, 0,
				&planeFootprint, &planeRows, &planeRowBytes, &planeTotalBytes);
			planeFootprint.Footprint.Depth = 1;
			planeTotalBytes = (UINT64)planeFootprint.Footprint.RowPitch * planeRows;
			const UINT64 expectedPlaneRowBytes = plane == 0 ? (UINT64)mipWidth * sizeof(uint32) : (UINT64)mipWidth;
			if (planeRows != mipHeight || planeRowBytes != expectedPlaneRowBytes ||
				planeTotalBytes == 0 || planeTotalBytes > std::numeric_limits<uint32>::max())
			{
				cemuLog_log(LogType::Force,
					"D3D12: unsupported depth-stencil upload footprint (format=0x{:04x}, plane={}, rows={}/{}, rowBytes={}/{}).",
					(uint32)texture->format, plane, planeRows, mipHeight,
					(unsigned long long)planeRowBytes, (unsigned long long)expectedPlaneRowBytes);
				return;
			}

			auto upload = m_memoryManager->AllocateUpload((uint32)planeTotalBytes,
				D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT, m_nextFenceValue);
			if (!upload.IsValid())
				throw std::runtime_error("Failed to allocate D3D12 depth-stencil upload memory");
			memset(upload.cpuPtr, 0, (size_t)planeTotalBytes);

			for (UINT row = 0; row < planeRows; ++row)
			{
				const uint8* sourceRow = sourcePixels + (uint64)row * sourceRowPitch;
				uint8* destinationRow = upload.cpuPtr + (uint64)row * planeFootprint.Footprint.RowPitch;
				if (plane == 0)
				{
					for (UINT x = 0; x < mipWidth; ++x)
						memcpy(destinationRow + (size_t)x * sizeof(uint32), sourceRow + (size_t)x * sourceTexelBytes, sizeof(uint32));
				}
				else
				{
					for (UINT x = 0; x < mipWidth; ++x)
						destinationRow[x] = sourceRow[(size_t)x * sourceTexelBytes + stencilByteOffset];
				}
			}

			planeFootprint.Offset = upload.offset;
			D3D12_TEXTURE_COPY_LOCATION source{};
			source.pResource = upload.resource;
			source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
			source.PlacedFootprint = planeFootprint;
			D3D12_TEXTURE_COPY_LOCATION destination{};
			destination.pResource = texture->GetResource();
			destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
			destination.SubresourceIndex = planeSubresource;

			TransitionTextureSubresource(texture, planeSubresource, D3D12_RESOURCE_STATE_COPY_DEST);
			GetCurrentCommandSlot().commandList->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
			TransitionTextureSubresource(texture, planeSubresource, GetD3D12StateAfterUpload(previousState));
			texture->MarkSubresourceWritten(planeSubresource);
		}
		return;
	}

	D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
	UINT numRows = 0;
	UINT64 rowSizeInBytes = 0;
	UINT64 totalBytes = 0;
	m_device->GetCopyableFootprints(&texture->GetDesc(), subresource, 1, 0, &footprint, &numRows, &rowSizeInBytes, &totalBytes);
	footprint.Footprint.Depth = 1;
	totalBytes = (UINT64)footprint.Footprint.RowPitch * numRows;
	if (totalBytes == 0 || totalBytes > std::numeric_limits<uint32>::max())
		return;

	auto upload = m_memoryManager->AllocateUpload((uint32)totalBytes, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT, m_nextFenceValue);
	if (!upload.IsValid())
		throw std::runtime_error("Failed to allocate D3D12 texture upload memory");

	const uint8* src = static_cast<const uint8*>(pixelData);
	const uint64 sourceRowPitch = numRows != 0 ? compressedImageSize / numRows : compressedImageSize;
	const uint64 copyRowBytes = std::min<uint64>(rowSizeInBytes, sourceRowPitch);
	for (UINT row = 0; row < numRows; row++)
		memcpy(upload.cpuPtr + row * footprint.Footprint.RowPitch, src + row * sourceRowPitch, copyRowBytes);

	footprint.Offset = upload.offset;

	D3D12_TEXTURE_COPY_LOCATION source{};
	source.pResource = upload.resource;
	source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
	source.PlacedFootprint = footprint;

	D3D12_TEXTURE_COPY_LOCATION destination{};
	destination.pResource = texture->GetResource();
	destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	destination.SubresourceIndex = subresource;

	const D3D12_RESOURCE_STATES previousState = texture->GetSubresourceState(subresource);
	ArrayUploadStaging* staging = nullptr;
	if (m_isUma && !is3DTexture && sliceIndex > 0 && texture->GetArraySize() > 1 && GetConfig().d3d12_gpu_array_upload &&
		(textureDesc.Format >= DXGI_FORMAT_BC1_TYPELESS && textureDesc.Format <= DXGI_FORMAT_BC5_SNORM) &&
		(mipWidth % 4) == 0 && (mipHeight % 4) == 0) // a staging BC texture needs whole blocks
		staging = GetArrayUploadStaging(textureDesc.Format, mipWidth, mipHeight);
	if (staging)
	{
		auto* commandList = GetCurrentCommandSlot().commandList.Get();
		// the staging texture has the same format and size as this mip, so the footprint matches
		Transition(staging->texture.resource.Get(), staging->state, D3D12_RESOURCE_STATE_COPY_DEST);
		D3D12_TEXTURE_COPY_LOCATION stagingLocation{};
		stagingLocation.pResource = staging->texture.resource.Get();
		stagingLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		stagingLocation.SubresourceIndex = 0;
		commandList->CopyTextureRegion(&stagingLocation, 0, 0, 0, &source, nullptr);
		Transition(staging->texture.resource.Get(), staging->state, D3D12_RESOURCE_STATE_COPY_SOURCE);
		TransitionTextureSubresource(texture, subresource, D3D12_RESOURCE_STATE_COPY_DEST);
		commandList->CopyTextureRegion(&destination, 0, 0, 0, &stagingLocation, nullptr);
	}
	else
	{
		TransitionTextureSubresource(texture, subresource, D3D12_RESOURCE_STATE_COPY_DEST);
		GetCurrentCommandSlot().commandList->CopyTextureRegion(&destination, 0, 0, is3DTexture ? sliceIndex : 0, &source, nullptr);
	}
	TransitionTextureSubresource(texture, subresource, GetD3D12StateAfterUpload(previousState));
	texture->MarkSubresourceWritten(subresource);
	if (D3D12ShouldTrace())
	{
		auto* view = static_cast<LatteTextureViewD3D12*>(texture->GetOrCreateView(texture->dim, texture->format, mipIndex, 1, sliceIndex, 1));
		RecordTextureLineage("upload", view, 0, 0, "cpu_upload",
							 fmt::format("uploadBytes={} rowPitch={} rows={} copyRowBytes={} slice={} mip={}",
										 compressedImageSize,
										 (uint32)footprint.Footprint.RowPitch,
										 (uint32)numRows,
										 (unsigned long long)copyRowBytes,
										 sliceIndex,
										 mipIndex));
		TraceTextureSnapshot("texture_load", "dst", view, false, fmt::format("uploadBytes={} rowPitch={} rows={} copyRowBytes={} slice={} mip={}",
																			 compressedImageSize,
																			 (uint32)footprint.Footprint.RowPitch,
																			 (uint32)numRows,
																			 (unsigned long long)copyRowBytes,
																			 sliceIndex,
																			 mipIndex));
	}
}

void D3D12Renderer::texture_clearColorSlice(LatteTexture* hostTexture, sint32 sliceIndex, sint32 mipIndex, float r, float g, float b, float a)
{
	auto record = [=, this](LatteTextureViewD3D12* preparedView) mutable
	{
		DropRetainedFeedbackCopies(hostTexture);
		stdx::scope_exit deviceCheck([&]() { CheckDeviceAfterOperation("colour clear", [&]() {
			return fmt::format("{} mip={} slice={}", D3D12DescribeResource(hostTexture ? static_cast<LatteTextureD3D12*>(hostTexture)->GetResource() : nullptr), mipIndex, sliceIndex); }); });
		auto* texture = static_cast<LatteTextureD3D12*>(hostTexture);
		if (!texture || texture->isDepth)
			return;
		// guest mips beyond what the texture size allows have no storage on the host
		if (mipIndex < 0 || mipIndex >= (sint32)texture->GetDesc().MipLevels)
			return;
		if (!m_commandListOpen)
			ResetCommandList();

		auto* view = preparedView ? preparedView : static_cast<LatteTextureViewD3D12*>(texture->GetOrCreateView(texture->dim, texture->format, mipIndex, 1, sliceIndex, 1));
		if (!view || !view->GetRTV().IsValid())
			return;

		const float color[4]{ r, g, b, a };
		static uint32 s_clearColorLogCount = 0;
		if (D3D12RuntimeLogCapped(s_clearColorLogCount, "texture_clearColorSlice", 96))
		{
			cemuLog_log(LogType::Force, "D3D12: clear color texture addr=0x{:08x} fmt=0x{:04x} size={}x{} slice={} mip={} color=({}, {}, {}, {}).",
						texture->physAddress, (uint32)texture->format, texture->width, texture->height,
						sliceIndex, mipIndex, r, g, b, a);
		}
		const uint32 clearSubresource = texture->GetSubresourceIndex(texture->Is3DTexture() ? 0 : sliceIndex, mipIndex);
		TransitionTextureSubresource(texture, clearSubresource, D3D12_RESOURCE_STATE_RENDER_TARGET);
		GetCurrentCommandSlot().commandList->ClearRenderTargetView(view->GetRTV().cpu, color, 0, nullptr);
		texture->MarkSubresourceWritten(clearSubresource);
		if (D3D12ShouldTrace())
		{
			RecordTextureLineage("clear_color", view, 0, 0, "none", fmt::format("rgba={},{},{},{} slice={} mip={}", r, g, b, a, sliceIndex, mipIndex));
			TraceTextureSnapshot("texture_clear_color", "dst", view, false, fmt::format("rgba={},{},{},{} slice={} mip={}", r, g, b, a, sliceIndex, mipIndex));
		}
	};
	if (ShouldQueueRecording())
	{
		// the view is core state: created here, the clear itself is queued
		auto* texture = static_cast<LatteTextureD3D12*>(hostTexture);
		if (!texture || texture->isDepth || mipIndex < 0 || mipIndex >= (sint32)texture->GetDesc().MipLevels)
			return;
		auto* view = static_cast<LatteTextureViewD3D12*>(texture->GetOrCreateView(texture->dim, texture->format, mipIndex, 1, sliceIndex, 1));
		if (!view)
			return;
		QueueRecording([record, view]() mutable { record(view); }, RecordOpKind::Command);
		return;
	}
	D3D12_EXCLUSIVE_RECORDING();
	record(nullptr);
}

void D3D12Renderer::texture_clearDepthSlice(LatteTexture* hostTexture, uint32 sliceIndex, sint32 mipIndex, bool clearDepth, bool clearStencil, float depthValue, uint32 stencilValue)
{
	auto record = [=, this](LatteTextureViewD3D12* preparedView) mutable
	{
		DropRetainedFeedbackCopies(hostTexture);
		stdx::scope_exit deviceCheck([&]() { CheckDeviceAfterOperation("depth clear", [&]() {
			return fmt::format("{} mip={} slice={} depth={} stencil={}", D3D12DescribeResource(hostTexture ? static_cast<LatteTextureD3D12*>(hostTexture)->GetResource() : nullptr), mipIndex, sliceIndex, clearDepth, clearStencil); }); });
		auto* texture = static_cast<LatteTextureD3D12*>(hostTexture);
		if (!texture || !texture->isDepth)
			return;
		// guest mips beyond what the texture size allows have no storage on the host
		if (mipIndex < 0 || mipIndex >= (sint32)texture->GetDesc().MipLevels)
			return;
		if (!m_commandListOpen)
			ResetCommandList();

		clearStencil = clearStencil && texture->GetFormatInfo().hasStencil;
		D3D12_CLEAR_FLAGS clearFlags{};
		if (clearDepth)
			clearFlags |= D3D12_CLEAR_FLAG_DEPTH;
		if (clearStencil)
			clearFlags |= D3D12_CLEAR_FLAG_STENCIL;
		if (clearFlags == 0)
			return;

		auto* view = preparedView ? preparedView : static_cast<LatteTextureViewD3D12*>(texture->GetOrCreateView(texture->dim, texture->format, mipIndex, 1, sliceIndex, 1));
		if (!view || !view->GetDSV().IsValid())
			return;

		static uint32 s_clearDepthLogCount = 0;
		if (D3D12RuntimeLogCapped(s_clearDepthLogCount, "texture_clearDepthSlice", 96))
		{
			cemuLog_log(LogType::Force, "D3D12: clear depth texture addr=0x{:08x} fmt=0x{:04x} size={}x{} slice={} mip={} depth={} stencil={} depthValue={} stencilValue={}.",
						texture->physAddress, (uint32)texture->format, texture->width, texture->height,
						sliceIndex, mipIndex, clearDepth, clearStencil, depthValue, stencilValue);
		}
		TransitionTextureView(view, D3D12_RESOURCE_STATE_DEPTH_WRITE, true);
		GetCurrentCommandSlot().commandList->ClearDepthStencilView(view->GetDSV().cpu, clearFlags, depthValue, (UINT8)stencilValue, 0, nullptr);
		const uint32 clearSubresource = texture->GetSubresourceIndex(texture->Is3DTexture() ? 0 : (sint32)sliceIndex, mipIndex);
		if (clearDepth)
			texture->MarkSubresourceWritten(clearSubresource);
		if (clearStencil)
		{
			const uint32 planeStride = std::max<uint32>(texture->GetDesc().MipLevels, 1) * std::max<uint32>(texture->GetArraySize(), 1);
			texture->MarkSubresourceWritten(clearSubresource + planeStride);
		}
		if (D3D12ShouldTrace())
		{
			RecordTextureLineage("clear_depth", view, 0, 0, "none",
								 fmt::format("clearDepth={} clearStencil={} depthValue={} stencilValue={} slice={} mip={}",
											 clearDepth ? 1 : 0,
											 clearStencil ? 1 : 0,
											 depthValue,
											 stencilValue,
											 sliceIndex,
											 mipIndex));
			TraceTextureSnapshot("texture_clear_depth", "dst", view, false, fmt::format("clearDepth={} clearStencil={} depthValue={} stencilValue={} slice={} mip={}",
																						clearDepth ? 1 : 0,
																						clearStencil ? 1 : 0,
																						depthValue,
																						stencilValue,
																						sliceIndex,
																						mipIndex));
		}
	};
	if (ShouldQueueRecording())
	{
		// the view is core state: created here, the clear itself is queued
		auto* texture = static_cast<LatteTextureD3D12*>(hostTexture);
		if (!texture || !texture->isDepth || mipIndex < 0 || mipIndex >= (sint32)texture->GetDesc().MipLevels)
			return;
		if (!clearDepth && !(clearStencil && texture->GetFormatInfo().hasStencil))
			return;
		auto* view = static_cast<LatteTextureViewD3D12*>(texture->GetOrCreateView(texture->dim, texture->format, mipIndex, 1, sliceIndex, 1));
		if (!view)
			return;
		QueueRecording([record, view]() mutable { record(view); }, RecordOpKind::Command);
		return;
	}
	D3D12_EXCLUSIVE_RECORDING();
	record(nullptr);
}

LatteTexture* D3D12Renderer::texture_createTextureEx(Latte::E_DIM dim, MPTR physAddress, MPTR physMipAddress, Latte::E_GX2SURFFMT format, uint32 width, uint32 height, uint32 depth, uint32 pitch, uint32 mipLevels, uint32 swizzle, Latte::E_HWTILEMODE tileMode, bool isDepth)
{
	D3D12_EXCLUSIVE_RECORDING();
	stdx::scope_exit deviceCheck([&]() { CheckDeviceAfterOperation("texture creation", [&]() {
		return fmt::format("addr=0x{:08x} dim={} gx2fmt=0x{:04x} {}x{}x{} mips={} tile={} depth={}", physAddress, (uint32)dim, (uint32)format,
			width, height, depth, mipLevels, (uint32)tileMode, isDepth); }); });
	auto* texture = new LatteTextureD3D12(this, dim, physAddress, physMipAddress, format, width, height, depth, pitch, mipLevels, swizzle, tileMode, isDepth);
	static uint32 s_createTextureLogCount = 0;
	if (D3D12RuntimeLogCapped(s_createTextureLogCount, "texture_createTextureEx", 160))
	{
		cemuLog_log(LogType::Force, "D3D12: create texture addr=0x{:08x} mipAddr=0x{:08x} dim={} fmt=0x{:04x} size={}x{}x{} pitch={} mips={} depth={} resource={}.",
					physAddress, physMipAddress, (uint32)dim, (uint32)format, width, height, depth,
					pitch, mipLevels, isDepth, texture->GetResource() != nullptr);
	}
	D3D12TraceEvent("texture_create", "addr=0x{:08x} mipAddr=0x{:08x} dim={} fmt=0x{:04x} dxgi={} size={}x{}x{} pitch={} mips={} swizzle=0x{:08x} tileMode={} depth={} resource={} obj={}",
					physAddress,
					physMipAddress,
					(uint32)dim,
					(uint32)format,
					(uint32)texture->GetResourceFormat(),
					width,
					height,
					depth,
					pitch,
					mipLevels,
					swizzle,
					(uint32)tileMode,
					isDepth ? 1 : 0,
					texture->GetResource() != nullptr ? 1 : 0,
					(const void*)texture);
	return texture;
}
void D3D12Renderer::texture_setLatteTexture(LatteTextureView* textureView, uint32 textureUnit)
{
	// Latte rebinds every texture unit for every draw; with threaded recording each bind was a queued op, most of the
	// ~5 per draw. Skip binds the recorder already has (the mirror is cleared whenever a view or texture goes away)
	if (!t_isRecorderThread && m_recordThreadRunning.load(std::memory_order_relaxed) && textureUnit < m_latteBoundTextures.size())
	{
		// an exclusive caller binds directly, so it only refreshes the copy
		if (!t_recordContext && m_latteBoundTextures[textureUnit] == textureView)
			return;
		m_latteBoundTextures[textureUnit] = textureView;
	}
	D3D12_QUEUE_RECORDING_KIND(State, texture_setLatteTexture(textureView, textureUnit));
	if (textureUnit >= m_boundTextures.size())
		return;
	if (m_boundTextures[textureUnit] != textureView)
		BumpTextureBindingGeneration(textureUnit);
	m_boundTextures[textureUnit] = static_cast<LatteTextureViewD3D12*>(textureView);
	static uint32 s_bindTextureLogCount = 0;
	if (D3D12RuntimeLogCapped(s_bindTextureLogCount, "texture_setLatteTexture", 160))
	{
		auto* d3dView = static_cast<LatteTextureViewD3D12*>(textureView);
		auto* texture = d3dView ? d3dView->GetBaseImage() : nullptr;
		// no SRV state here: GetSRV() prepares sampling proxies, which belongs to the recording side (Phase 4)
		cemuLog_log(LogType::Force, "D3D12: bind texture unit={} addr=0x{:08x} fmt=0x{:04x} size={}x{} mip{}+{} slice{}+{}.",
					textureUnit,
					texture ? texture->physAddress : 0,
					texture ? (uint32)texture->format : 0,
					texture ? texture->width : 0,
					texture ? texture->height : 0,
					d3dView ? d3dView->firstMip : 0,
					d3dView ? d3dView->numMip : 0,
					d3dView ? d3dView->firstSlice : 0,
					d3dView ? d3dView->numSlice : 0);
	}
	{
		auto* d3dView = static_cast<LatteTextureViewD3D12*>(textureView);
		auto* texture = d3dView ? d3dView->GetBaseImage() : nullptr;
		D3D12TraceEvent("texture_bind", "unit={} addr=0x{:08x} fmt=0x{:04x} dxgi={} size={}x{} mip={} mipCount={} slice={} sliceCount={} srv={} view={} texture={}",
						textureUnit,
						texture ? texture->physAddress : 0,
						texture ? (uint32)texture->format : 0,
						texture ? (uint32)d3dView->GetViewFormat() : 0,
						texture ? texture->width : 0,
						texture ? texture->height : 0,
						d3dView ? d3dView->firstMip : 0,
						d3dView ? d3dView->numMip : 0,
						d3dView ? d3dView->firstSlice : 0,
						d3dView ? d3dView->numSlice : 0,
						d3dView && d3dView->GetSRV().IsValid() ? 1 : 0,
						(const void*)d3dView,
						(const void*)texture);
	}
}

void D3D12Renderer::texture_copyImageSubData(LatteTexture* src, sint32 srcMip, sint32 effectiveSrcX, sint32 effectiveSrcY, sint32 srcSlice, LatteTexture* dst, sint32 dstMip, sint32 effectiveDstX, sint32 effectiveDstY, sint32 dstSlice, sint32 effectiveCopyWidth, sint32 effectiveCopyHeight, sint32 srcDepth)
{
	if (ShouldQueueRecording())
	{
		// only incompatible formats and depth/multisampled copies can fall back to the surface-copy draw, which
		// creates views and tracks GPU writes in core state; every other copy is recording work only
		const auto* srcTexture = static_cast<LatteTextureD3D12*>(src);
		const auto* dstTexture = static_cast<LatteTextureD3D12*>(dst);
		if (srcTexture && dstTexture && !srcTexture->isDepth && !dstTexture->isDepth &&
			srcTexture->GetDesc().SampleDesc.Count <= 1 && dstTexture->GetDesc().SampleDesc.Count <= 1 &&
			AreD3D12CopyFormatsCompatible(srcTexture->GetDesc().Format, dstTexture->GetDesc().Format))
		{
			QueueRecording([=, this]() { texture_copyImageSubData(src, srcMip, effectiveSrcX, effectiveSrcY, srcSlice, dst, dstMip, effectiveDstX, effectiveDstY,
				dstSlice, effectiveCopyWidth, effectiveCopyHeight, srcDepth); }, RecordOpKind::Command);
			return;
		}
		// a single-slice copy that may fall back to the surface-copy draw: its views and the GPU write tracking are
		// prepared here, the draw (or the plain copy) runs on the recorder
		auto inRange = [](const LatteTextureD3D12* texture, sint32 mip, sint32 slice)
		{
			if (!texture || !texture->GetResource() || mip < 0 || mip >= (sint32)texture->GetDesc().MipLevels || slice < 0)
				return false;
			const auto& desc = texture->GetDesc();
			const uint32 depth = desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ?
				std::max<uint32>(1, (uint32)desc.DepthOrArraySize >> (uint32)mip) : std::max<uint32>(1, (uint32)desc.DepthOrArraySize);
			return (uint32)slice < depth;
		};
		if (srcDepth <= 1 && inRange(srcTexture, srcMip, srcSlice) && inRange(dstTexture, dstMip, dstSlice))
		{
			auto* sourceView = static_cast<LatteTextureViewD3D12*>(src->Is3DTexture() ?
				src->GetOrCreateView(Latte::E_DIM::DIM_2D, src->format, srcMip, 1, srcSlice, 1) :
				src->GetOrCreateView(srcMip, 1, srcSlice, 1));
			auto* destinationView = static_cast<LatteTextureViewD3D12*>(dst->GetOrCreateView(dstMip, 1, dstSlice, 1));
			if (sourceView && destinationView)
			{
				LatteTexture_TrackTextureGPUWrite(dst, dstSlice, dstMip, LatteTexture_getNextUpdateEventCounter());
				QueueRecording([=, this]()
				{
					m_preparedSurfaceCopy = { src, srcMip, srcSlice, dst, dstMip, dstSlice, sourceView, destinationView };
					stdx::scope_exit clearPrepared([this]() { m_preparedSurfaceCopy = {}; });
					texture_copyImageSubData(src, srcMip, effectiveSrcX, effectiveSrcY, srcSlice, dst, dstMip, effectiveDstX, effectiveDstY,
						dstSlice, effectiveCopyWidth, effectiveCopyHeight, srcDepth);
				}, RecordOpKind::Command);
				return;
			}
		}
	}
	D3D12_EXCLUSIVE_RECORDING();
	if (dst)
		DropRetainedFeedbackCopies(dst);
	stdx::scope_exit deviceCheck([&]() { CheckDeviceAfterOperation("texture copy", [&]() {
		return fmt::format("src {} mip={} slice={} at {},{} -> dst {} mip={} slice={} at {},{} size {}x{}x{}", D3D12DescribeResource(src ? static_cast<LatteTextureD3D12*>(src)->GetResource() : nullptr), srcMip, srcSlice,
			effectiveSrcX, effectiveSrcY, D3D12DescribeResource(dst ? static_cast<LatteTextureD3D12*>(dst)->GetResource() : nullptr), dstMip, dstSlice, effectiveDstX, effectiveDstY, effectiveCopyWidth, effectiveCopyHeight, srcDepth); }); });
	auto* srcTexture = static_cast<LatteTextureD3D12*>(src);
	auto* dstTexture = static_cast<LatteTextureD3D12*>(dst);
	if (!srcTexture || !dstTexture || !srcTexture->GetResource() || !dstTexture->GetResource())
		return;
	m_gpuWatchdog.RecordOperation(D3D12GpuWatchdog::Operation::SurfaceCopy);

	const bool srcIs3D = srcTexture->GetDesc().Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D;
	const bool dstIs3D = dstTexture->GetDesc().Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D;
	const auto& srcDesc = srcTexture->GetDesc();
	const auto& dstDesc = dstTexture->GetDesc();
	if (srcMip < 0 || srcMip >= (sint32)srcDesc.MipLevels ||
		dstMip < 0 || dstMip >= (sint32)dstDesc.MipLevels ||
		effectiveSrcX < 0 || effectiveSrcY < 0 || effectiveDstX < 0 || effectiveDstY < 0 ||
		effectiveCopyWidth <= 0 || effectiveCopyHeight <= 0)
	{
		D3D12TraceEvent("texture_copy_skipped", "reason=invalid_region srcMip={} dstMip={} srcXY={},{} dstXY={},{} size={}x{}",
			srcMip, dstMip, effectiveSrcX, effectiveSrcY, effectiveDstX, effectiveDstY,
			effectiveCopyWidth, effectiveCopyHeight);
		return;
	}
	const uint32 srcMipWidth = std::max<uint32>(1, (uint32)(srcDesc.Width >> srcMip));
	const uint32 srcMipHeight = std::max<uint32>(1, (uint32)(srcDesc.Height >> srcMip));
	const uint32 dstMipWidth = std::max<uint32>(1, (uint32)(dstDesc.Width >> dstMip));
	const uint32 dstMipHeight = std::max<uint32>(1, (uint32)(dstDesc.Height >> dstMip));
	const bool srcCompressed = srcTexture->GetFormatInfo().isCompressed;
	const bool dstCompressed = dstTexture->GetFormatInfo().isCompressed;
	// the copy size is in source texels; one uncompressed texel aliases one 4x4 block
	auto getDestinationExtent = [&](sint32 sourceExtent, sint32 destinationOffset, uint32 destinationMipExtent) -> uint64
	{
		if (srcCompressed && !dstCompressed)
			return (uint64)((sourceExtent + 3) / 4);
		if (!srcCompressed && dstCompressed)
		{
			const uint64 blockExtent = (uint64)sourceExtent * 4;
			const uint64 remaining = destinationMipExtent > (uint32)destinationOffset ? destinationMipExtent - (uint32)destinationOffset : 0;
			return blockExtent > remaining && blockExtent - remaining < 4 ? remaining : blockExtent;
		}
		return (uint64)sourceExtent;
	};
	const uint64 dstCopyWidth = getDestinationExtent(effectiveCopyWidth, effectiveDstX, dstMipWidth);
	const uint64 dstCopyHeight = getDestinationExtent(effectiveCopyHeight, effectiveDstY, dstMipHeight);
	if ((uint64)effectiveSrcX + (uint64)effectiveCopyWidth > srcMipWidth ||
		(uint64)effectiveSrcY + (uint64)effectiveCopyHeight > srcMipHeight ||
		(uint64)effectiveDstX + dstCopyWidth > dstMipWidth ||
		(uint64)effectiveDstY + dstCopyHeight > dstMipHeight)
	{
		D3D12TraceEvent("texture_copy_skipped", "reason=region_oob srcExtent={}x{} dstExtent={}x{} srcXY={},{} dstXY={},{} size={}x{}",
			srcMipWidth, srcMipHeight, dstMipWidth, dstMipHeight,
			effectiveSrcX, effectiveSrcY, effectiveDstX, effectiveDstY,
			effectiveCopyWidth, effectiveCopyHeight);
		return;
	}
	const sint32 requestedCopyDepth = std::max<sint32>(srcDepth, 1);
	auto getRemainingDepth = [](const LatteTextureD3D12* texture, sint32 mip, sint32 slice) -> sint32
	{
		if (!texture || mip < 0 || mip >= (sint32)texture->GetDesc().MipLevels || slice < 0)
			return 0;

		const auto& desc = texture->GetDesc();
		if (desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D)
		{
			const uint32 mipDepth = std::max<uint32>(1, (uint32)desc.DepthOrArraySize >> (uint32)mip);
			if ((uint32)slice >= mipDepth)
				return 0;
			return (sint32)(mipDepth - (uint32)slice);
		}

		const uint32 arraySize = std::max<uint32>(1, (uint32)desc.DepthOrArraySize);
		if ((uint32)slice >= arraySize)
			return 0;
		return (sint32)(arraySize - (uint32)slice);
	};
	auto getD3DSubresourceIndex = [](const LatteTextureD3D12* texture, bool is3DResource, sint32 slice, sint32 mip) -> uint32
	{
		if (is3DResource)
			return (uint32)std::max<sint32>(mip, 0);

		const uint32 mipLevels = std::max<uint32>(1, (uint32)texture->GetDesc().MipLevels);
		return (uint32)std::max<sint32>(mip, 0) + (uint32)std::max<sint32>(slice, 0) * mipLevels;
	};
	const sint32 maxSrcCopyDepth = getRemainingDepth(srcTexture, srcMip, srcSlice);
	const sint32 maxDstCopyDepth = getRemainingDepth(dstTexture, dstMip, dstSlice);
	const sint32 copyDepth = std::min(requestedCopyDepth, std::min(maxSrcCopyDepth, maxDstCopyDepth));
	if (copyDepth <= 0)
	{
		static uint32 s_invalidDepthCopyLogCount = 0;
		if (D3D12RuntimeLogCapped(s_invalidDepthCopyLogCount, "invalid texture copy depth", 64))
		{
			cemuLog_log(LogType::Force,
						 "D3D12: skipping texture copy outside source/destination depth bounds src=0x{:08x}/mip={}/slice={}/remaining={} dst=0x{:08x}/mip={}/slice={}/remaining={} requestedDepth={}.",
						 srcTexture->physAddress,
						 srcMip,
						 srcSlice,
						 maxSrcCopyDepth,
						 dstTexture->physAddress,
						 dstMip,
						 dstSlice,
						 maxDstCopyDepth,
						 requestedCopyDepth);
		}
		D3D12TraceEvent("texture_copy_skipped", "reason=depth_oob src=0x{:08x} srcMip={} srcSlice={} srcRemaining={} dst=0x{:08x} dstMip={} dstSlice={} dstRemaining={} requestedDepth={}",
						srcTexture->physAddress,
						srcMip,
						srcSlice,
						maxSrcCopyDepth,
						dstTexture->physAddress,
						dstMip,
						dstSlice,
						maxDstCopyDepth,
						requestedCopyDepth);
		return;
	}
	if (copyDepth != requestedCopyDepth)
	{
		static uint32 s_truncatedDepthCopyLogCount = 0;
		if (D3D12RuntimeLogCapped(s_truncatedDepthCopyLogCount, "truncated texture copy depth", 64))
		{
			cemuLog_log(LogType::Force,
						 "D3D12: clamping texture copy depth from {} to {} src=0x{:08x}/mip={}/slice={}/remaining={} dst=0x{:08x}/mip={}/slice={}/remaining={}.",
						 requestedCopyDepth,
						 copyDepth,
						 srcTexture->physAddress,
						 srcMip,
						 srcSlice,
						 maxSrcCopyDepth,
						 dstTexture->physAddress,
						 dstMip,
						 dstSlice,
						 maxDstCopyDepth);
		}
	}

	const bool copySliceBySlice = copyDepth > 1 && (!srcIs3D || !dstIs3D);
	const uint32 srcSubresource = getD3DSubresourceIndex(srcTexture, srcIs3D, srcSlice, srcMip);
	const uint32 dstSubresource = getD3DSubresourceIndex(dstTexture, dstIs3D, dstSlice, dstMip);
	const bool sameD3DResource = srcTexture->GetResource() == dstTexture->GetResource();
	const bool sameLatteAddress = srcTexture->physAddress == dstTexture->physAddress;
	if (requestedCopyDepth > 1)
	{
		static uint32 s_multiDepthCopyLogCount = 0;
		if (D3D12RuntimeLogCapped(s_multiDepthCopyLogCount, "multi-depth texture copy", 128))
		{
			cemuLog_log(LogType::Force,
						 "D3D12: multi-depth texture copy requested={} effective={} split={} src=0x{:08x}/mip={}/slice={}/d3dDim={}/depthOrArray={} dst=0x{:08x}/mip={}/slice={}/d3dDim={}/depthOrArray={}.",
						 requestedCopyDepth,
						 copyDepth,
						 copySliceBySlice ? 1 : 0,
						 srcTexture->physAddress,
						 srcMip,
						 srcSlice,
						 (uint32)srcTexture->GetDesc().Dimension,
						 (uint32)srcTexture->GetDesc().DepthOrArraySize,
						 dstTexture->physAddress,
						 dstMip,
						 dstSlice,
						 (uint32)dstTexture->GetDesc().Dimension,
						 (uint32)dstTexture->GetDesc().DepthOrArraySize);
		}
	}
	static uint32 s_copyTextureLogCount = 0;
	if (D3D12RuntimeLogCapped(s_copyTextureLogCount, "texture_copyImageSubData", 128))
	{
		cemuLog_log(LogType::Force, "D3D12: copy texture src=0x{:08x}/fmt=0x{:04x}/mip={}/slice={} dst=0x{:08x}/fmt=0x{:04x}/mip={}/slice={} srcXY=({}, {}) dstXY=({}, {}) size={}x{}x{} sameAddr={} sameResource={} srcObj={} dstObj={}.",
					srcTexture->physAddress, (uint32)srcTexture->format, srcMip, srcSlice,
					dstTexture->physAddress, (uint32)dstTexture->format, dstMip, dstSlice,
					effectiveSrcX, effectiveSrcY, effectiveDstX, effectiveDstY,
					effectiveCopyWidth, effectiveCopyHeight, copyDepth,
					sameLatteAddress,
					sameD3DResource,
					(const void*)srcTexture,
					(const void*)dstTexture);
	}
	D3D12TraceEvent("texture_copy_begin", "src=0x{:08x} srcFmt=0x{:04x} srcDxgi={} srcMip={} srcSlice={} dst=0x{:08x} dstFmt=0x{:04x} dstDxgi={} dstMip={} dstSlice={} srcXY={},{} dstXY={},{} size={}x{}x{} sameAddr={} sameResource={} srcObj={} dstObj={}",
					srcTexture->physAddress,
					(uint32)srcTexture->format,
					(uint32)srcTexture->GetDesc().Format,
					srcMip,
					srcSlice,
					dstTexture->physAddress,
					(uint32)dstTexture->format,
					(uint32)dstTexture->GetDesc().Format,
					dstMip,
					dstSlice,
					effectiveSrcX,
					effectiveSrcY,
					effectiveDstX,
					effectiveDstY,
					effectiveCopyWidth,
					effectiveCopyHeight,
					copyDepth,
					sameLatteAddress ? 1 : 0,
					sameD3DResource ? 1 : 0,
					(const void*)srcTexture,
					(const void*)dstTexture);
	if (sameD3DResource &&
		srcMip == dstMip &&
		srcSlice == dstSlice &&
		effectiveSrcX == effectiveDstX &&
		effectiveSrcY == effectiveDstY)
	{
		static uint32 s_sameTextureNoopCopyLogCount = 0;
		if (D3D12RuntimeLogCapped(s_sameTextureNoopCopyLogCount, "same-resource texture no-op copy", 64))
		{
			cemuLog_log(LogType::Force,
						 "D3D12: skipping exact same-resource texture copy src=dst=0x{:08x}/fmt=0x{:04x}/subresource={} xy=({}, {}) size={}x{}.",
						 srcTexture->physAddress,
						 (uint32)srcTexture->format,
						 srcSubresource,
						 effectiveSrcX,
						 effectiveSrcY,
						 effectiveCopyWidth,
						 effectiveCopyHeight);
		}
		D3D12TraceEvent("texture_copy_noop", "addr=0x{:08x} fmt=0x{:04x} subresource={} xy={},{} size={}x{}",
						srcTexture->physAddress,
						(uint32)srcTexture->format,
						srcSubresource,
						effectiveSrcX,
						effectiveSrcY,
						effectiveCopyWidth,
						effectiveCopyHeight);
		return;
	}
	if (!m_commandListOpen)
		ResetCommandList();

	D3D12_TEXTURE_COPY_LOCATION source{};
	source.pResource = srcTexture->GetResource();
	source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	source.SubresourceIndex = srcSubresource;

	D3D12_TEXTURE_COPY_LOCATION destination{};
	destination.pResource = dstTexture->GetResource();
	destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	destination.SubresourceIndex = dstSubresource;

	D3D12_BOX sourceBox{};
	sourceBox.left = (UINT)std::max<sint32>(effectiveSrcX, 0);
	sourceBox.top = (UINT)std::max<sint32>(effectiveSrcY, 0);
	sourceBox.front = srcIs3D ? (UINT)std::max<sint32>(srcSlice, 0) : 0;
	sourceBox.right = sourceBox.left + (UINT)std::max<sint32>(effectiveCopyWidth, 1);
	sourceBox.bottom = sourceBox.top + (UINT)std::max<sint32>(effectiveCopyHeight, 1);
	sourceBox.back = sourceBox.front + (UINT)((srcIs3D && !copySliceBySlice) ? copyDepth : 1);

	const uint32 dstX = (uint32)effectiveDstX;
	const uint32 dstY = (uint32)effectiveDstY;
	// block-compressed copies into uncompressed formats and back are staged through a buffer
	const bool compressedCopy = srcCompressed && dstCompressed;
	const sint32 sourceSubresourceDepth = srcIs3D && !copySliceBySlice ? copyDepth : 1;
	const sint32 destinationSubresourceDepth = dstIs3D && !copySliceBySlice ? copyDepth : 1;
	const bool fullSubresourceCopy =
		IsFullSubresourceTextureCopy(srcTexture, srcMip,
			effectiveSrcX, effectiveSrcY, srcIs3D ? srcSlice : 0,
			effectiveCopyWidth, effectiveCopyHeight, sourceSubresourceDepth) &&
		IsFullSubresourceTextureCopy(dstTexture, dstMip,
			effectiveDstX, effectiveDstY, dstIs3D ? dstSlice : 0,
			effectiveCopyWidth, effectiveCopyHeight, destinationSubresourceDepth);
	if (compressedCopy && !fullSubresourceCopy &&
		!IsValidBCTextureCopyRegion(srcTexture, srcMip, dstTexture, dstMip,
			effectiveSrcX, effectiveSrcY, effectiveDstX, effectiveDstY, effectiveCopyWidth, effectiveCopyHeight))
	{
		static uint32 s_unalignedBcCopyLogCount = 0;
		if (D3D12RuntimeLogCapped(s_unalignedBcCopyLogCount, "unaligned compressed texture copy", 64))
		{
			cemuLog_log(LogType::Force,
						 "D3D12: skipping unaligned BC texture copy src=0x{:08x}/fmt=0x{:04x}/mip={} dst=0x{:08x}/fmt=0x{:04x}/mip={} srcXY=({}, {}) dstXY=({}, {}) size={}x{}.",
						 srcTexture->physAddress,
						 (uint32)srcTexture->format,
						 srcMip,
						 dstTexture->physAddress,
						 (uint32)dstTexture->format,
						 dstMip,
						 effectiveSrcX,
						 effectiveSrcY,
						 effectiveDstX,
						 effectiveDstY,
						 effectiveCopyWidth,
						 effectiveCopyHeight);
		}
		D3D12TraceEvent("texture_copy_skipped", "reason=unaligned_bc src=0x{:08x} dst=0x{:08x} srcXY={},{} dstXY={},{} size={}x{}",
						srcTexture->physAddress,
						dstTexture->physAddress,
						effectiveSrcX,
						effectiveSrcY,
						effectiveDstX,
						effectiveDstY,
						effectiveCopyWidth,
						effectiveCopyHeight);
		return;
	}
	const bool depthStencilCopy = srcTexture->isDepth || dstTexture->isDepth;
	const bool multisampledCopy = srcDesc.SampleDesc.Count > 1 || dstDesc.SampleDesc.Count > 1;
	if (srcDesc.SampleDesc.Count != dstDesc.SampleDesc.Count ||
		srcDesc.SampleDesc.Quality != dstDesc.SampleDesc.Quality)
	{
		static uint32 s_invalidWholeSubresourceCopyLogCount = 0;
		if (D3D12RuntimeLogCapped(s_invalidWholeSubresourceCopyLogCount, "incompatible multisampled texture copy", 64))
		{
			cemuLog_log(LogType::Force,
				"D3D12: rejecting texture copy with incompatible sample descriptions (src=0x{:08x}, dst=0x{:08x}, samples={}:{}/{}:{}).",
				srcTexture->physAddress, dstTexture->physAddress,
				srcDesc.SampleDesc.Count, srcDesc.SampleDesc.Quality,
				dstDesc.SampleDesc.Count, dstDesc.SampleDesc.Quality);
		}
		D3D12TraceEvent("texture_copy_skipped", "reason=sample_mismatch src=0x{:08x} dst=0x{:08x} srcSamples={}:{} dstSamples={}:{}",
			srcTexture->physAddress, dstTexture->physAddress,
			srcDesc.SampleDesc.Count, srcDesc.SampleDesc.Quality,
			dstDesc.SampleDesc.Count, dstDesc.SampleDesc.Quality);
		return;
	}
	if (!AreD3D12CopyFormatsCompatible(srcTexture->GetDesc().Format, dstTexture->GetDesc().Format))
	{
		if (!srcTexture->isDepth && !dstTexture->isDepth && srcTexture->GetBPP() == dstTexture->GetBPP())
		{
			if (TryRawAliasTextureCopy(srcTexture, srcMip, srcSlice, effectiveSrcX, effectiveSrcY,
									   dstTexture, dstMip, dstSlice, effectiveDstX, effectiveDstY,
									   effectiveCopyWidth, effectiveCopyHeight, copyDepth))
			{
				return;
			}
			static bool s_loggedDroppedColorAliasCopy = false;
			if (!s_loggedDroppedColorAliasCopy)
			{
				s_loggedDroppedColorAliasCopy = true;
				cemuLog_log(LogType::Force, "D3D12: dropping texture copy between formats without a raw bit mapping (src fmt=0x{:04x}/dxgi={}, dst fmt=0x{:04x}/dxgi={}).",
					(uint32)srcTexture->format, (uint32)srcTexture->GetDesc().Format,
					(uint32)dstTexture->format, (uint32)dstTexture->GetDesc().Format);
			}
			static uint32 s_incompatibleColorAliasCopyLogCount = 0;
			if (D3D12RuntimeLogCapped(s_incompatibleColorAliasCopyLogCount, "unsupported incompatible color alias texture copy", 96))
			{
				cemuLog_log(LogType::Force,
							 "D3D12: incompatible color alias copy cannot use shader conversion src=0x{:08x}/dxgi={} dst=0x{:08x}/dxgi={} srcXY=({}, {}) dstXY=({}, {}) size={}x{} srcBpp={} dstBpp={}.",
							 srcTexture->physAddress,
							 (uint32)srcTexture->GetDesc().Format,
							 dstTexture->physAddress,
							 (uint32)dstTexture->GetDesc().Format,
							 effectiveSrcX,
							 effectiveSrcY,
							 effectiveDstX,
							 effectiveDstY,
							 effectiveCopyWidth,
							 effectiveCopyHeight,
							 srcTexture->GetBPP(),
							 dstTexture->GetBPP());
			}
			D3D12TraceEvent("texture_copy_skipped", "reason=incompatible_color_alias src=0x{:08x} srcDxgi={} dst=0x{:08x} dstDxgi={} srcXY={},{} dstXY={},{} size={}x{} srcBpp={} dstBpp={}",
							srcTexture->physAddress,
							(uint32)srcTexture->GetDesc().Format,
							dstTexture->physAddress,
							(uint32)dstTexture->GetDesc().Format,
							effectiveSrcX,
							effectiveSrcY,
							effectiveDstX,
							effectiveDstY,
							effectiveCopyWidth,
							effectiveCopyHeight,
							srcTexture->GetBPP(),
							dstTexture->GetBPP());
			return;
		}
		if (copyDepth == 1)
		{
			static uint32 s_incompatibleCopyDrawLogCount = 0;
			if (D3D12RuntimeLogCapped(s_incompatibleCopyDrawLogCount, "incompatible texture copy conversion", 96))
			{
				cemuLog_log(LogType::Force,
							 "D3D12: routing incompatible texture copy through surface-copy draw src=0x{:08x}/dxgi={} dst=0x{:08x}/dxgi={} srcXY=({}, {}) dstXY=({}, {}) size={}x{}.",
							 srcTexture->physAddress,
							 (uint32)srcTexture->GetDesc().Format,
							 dstTexture->physAddress,
							 (uint32)dstTexture->GetDesc().Format,
							 effectiveSrcX,
							 effectiveSrcY,
							 effectiveDstX,
							 effectiveDstY,
							 effectiveCopyWidth,
							 effectiveCopyHeight);
			}
			D3D12TraceEvent("texture_copy_reroute_surface", "src=0x{:08x} srcDxgi={} dst=0x{:08x} dstDxgi={} srcXY={},{} dstXY={},{} size={}x{} sameAddr={} sameResource={}",
							srcTexture->physAddress,
							(uint32)srcTexture->GetDesc().Format,
							dstTexture->physAddress,
							(uint32)dstTexture->GetDesc().Format,
							effectiveSrcX,
							effectiveSrcY,
							effectiveDstX,
							effectiveDstY,
							effectiveCopyWidth,
							effectiveCopyHeight,
							sameLatteAddress ? 1 : 0,
							sameD3DResource ? 1 : 0);
			SurfaceCopyWithFormatConversion(srcTexture, srcMip, srcSlice, dstTexture, dstMip, dstSlice,
											effectiveSrcX, effectiveSrcY, effectiveDstX, effectiveDstY,
											effectiveCopyWidth, effectiveCopyHeight, true);
		}
		else if (!srcIs3D && !dstIs3D)
		{
			for (sint32 slice = 0; slice < copyDepth; ++slice)
			{
				SurfaceCopyWithFormatConversion(srcTexture, srcMip, srcSlice + slice, dstTexture, dstMip, dstSlice + slice,
												effectiveSrcX, effectiveSrcY, effectiveDstX, effectiveDstY,
												effectiveCopyWidth, effectiveCopyHeight, true);
			}
		}
		else
		{
			static uint32 s_incompatibleCopyUnsupportedLogCount = 0;
			if (D3D12RuntimeLogCapped(s_incompatibleCopyUnsupportedLogCount, "unsupported incompatible texture copy", 64))
			{
				cemuLog_log(LogType::Force,
							 "D3D12: incompatible texture copy with depth > 1 needs 3D shader conversion support src=0x{:08x}/dxgi={} dst=0x{:08x}/dxgi={} srcXY=({}, {}) dstXY=({}, {}) size={}x{}x{}.",
							 srcTexture->physAddress,
							 (uint32)srcTexture->GetDesc().Format,
							 dstTexture->physAddress,
							 (uint32)dstTexture->GetDesc().Format,
							 effectiveSrcX,
							 effectiveSrcY,
							 effectiveDstX,
							 effectiveDstY,
							 effectiveCopyWidth,
							 effectiveCopyHeight,
							 copyDepth);
			}
			D3D12TraceEvent("texture_copy_skipped", "reason=incompatible_depth src=0x{:08x} srcDxgi={} dst=0x{:08x} dstDxgi={} srcXY={},{} dstXY={},{} size={}x{}x{}",
							srcTexture->physAddress,
							(uint32)srcTexture->GetDesc().Format,
							dstTexture->physAddress,
							(uint32)dstTexture->GetDesc().Format,
							effectiveSrcX,
							effectiveSrcY,
							effectiveDstX,
							effectiveDstY,
							effectiveCopyWidth,
							effectiveCopyHeight,
							copyDepth);
		}
		return;
	}

	const bool requiresWholeSubresource = depthStencilCopy || multisampledCopy;
	if (requiresWholeSubresource && !fullSubresourceCopy)
	{
		// CopyTextureRegion only takes whole depth-stencil subresources; draw the region instead
		if (!multisampledCopy && srcTexture->isDepth && dstTexture->isDepth && !sameD3DResource && !srcIs3D && !dstIs3D)
		{
			for (sint32 slice = 0; slice < copyDepth; ++slice)
			{
				SurfaceCopyWithFormatConversion(srcTexture, srcMip, srcSlice + slice, dstTexture, dstMip, dstSlice + slice,
												effectiveSrcX, effectiveSrcY, effectiveDstX, effectiveDstY,
												effectiveCopyWidth, effectiveCopyHeight, true, false);
			}
			return;
		}
		static uint32 s_invalidWholeSubresourceCopyLogCount = 0;
		if (D3D12RuntimeLogCapped(s_invalidWholeSubresourceCopyLogCount, "invalid depth-stencil or multisampled texture copy", 64))
		{
			cemuLog_log(LogType::Force,
				"D3D12: rejecting depth-stencil/MSAA CopyTextureRegion that is not a whole-subresource copy (src=0x{:08x}, dst=0x{:08x}, samples={}:{}/{}:{}).",
				srcTexture->physAddress, dstTexture->physAddress,
				srcDesc.SampleDesc.Count, srcDesc.SampleDesc.Quality,
				dstDesc.SampleDesc.Count, dstDesc.SampleDesc.Quality);
		}
		D3D12TraceEvent("texture_copy_skipped", "reason=whole_subresource_required src=0x{:08x} dst=0x{:08x} srcSamples={}:{} dstSamples={}:{}",
			srcTexture->physAddress, dstTexture->physAddress,
			srcDesc.SampleDesc.Count, srcDesc.SampleDesc.Quality,
			dstDesc.SampleDesc.Count, dstDesc.SampleDesc.Quality);
		return;
	}

	if (depthStencilCopy && srcTexture->GetFormatInfo().hasStencil != dstTexture->GetFormatInfo().hasStencil)
	{
		D3D12TraceEvent("texture_copy_skipped", "reason=depth_stencil_plane_mismatch src=0x{:08x} dst=0x{:08x}",
			srcTexture->physAddress, dstTexture->physAddress);
		return;
	}

	struct TextureCopyOperation
	{
		uint32 sourceSubresource{};
		uint32 destinationSubresource{};
		D3D12_BOX sourceBox{};
		UINT destinationX{};
		UINT destinationY{};
		UINT destinationZ{};
		bool wholeSubresource{};
	};
	const sint32 d3dCopyCount = copySliceBySlice ? copyDepth : 1;
	const uint32 planeCount = depthStencilCopy && srcTexture->GetFormatInfo().hasStencil ? 2u : 1u;
	const uint32 srcPlaneStride = std::max<uint32>(srcDesc.MipLevels, 1) *
		(srcIs3D ? 1u : std::max<uint32>(srcTexture->GetArraySize(), 1));
	const uint32 dstPlaneStride = std::max<uint32>(dstDesc.MipLevels, 1) *
		(dstIs3D ? 1u : std::max<uint32>(dstTexture->GetArraySize(), 1));
	const bool useWholeSubresourceCopy = fullSubresourceCopy && (compressedCopy || requiresWholeSubresource);
	std::vector<TextureCopyOperation> copyOperations;
	copyOperations.reserve((size_t)d3dCopyCount * planeCount);
	for (sint32 depthIndex = 0; depthIndex < d3dCopyCount; ++depthIndex)
	{
		D3D12_BOX operationSourceBox = sourceBox;
		uint32 operationSrcSubresource = source.SubresourceIndex;
		uint32 operationDstSubresource = destination.SubresourceIndex;
		UINT operationDstZ = dstIs3D ? (UINT)dstSlice : 0u;

		if (copySliceBySlice)
		{
			if (srcIs3D)
			{
				operationSourceBox.front = (UINT)(srcSlice + depthIndex);
				operationSourceBox.back = operationSourceBox.front + 1;
			}
			else
			{
				operationSrcSubresource = getD3DSubresourceIndex(srcTexture, false, srcSlice + depthIndex, srcMip);
				operationSourceBox.front = 0;
				operationSourceBox.back = 1;
			}

			if (dstIs3D)
				operationDstZ = (UINT)(dstSlice + depthIndex);
			else
				operationDstSubresource = getD3DSubresourceIndex(dstTexture, false, dstSlice + depthIndex, dstMip);
		}

		for (uint32 plane = 0; plane < planeCount; ++plane)
		{
			TextureCopyOperation operation;
			operation.sourceSubresource = operationSrcSubresource + plane * srcPlaneStride;
			operation.destinationSubresource = operationDstSubresource + plane * dstPlaneStride;
			operation.sourceBox = operationSourceBox;
			operation.destinationX = dstX;
			operation.destinationY = dstY;
			operation.destinationZ = operationDstZ;
			operation.wholeSubresource = useWholeSubresourceCopy;
			copyOperations.emplace_back(operation);
		}
	}

	std::vector<uint32> sourceCopySubresources;
	std::vector<uint32> destinationCopySubresources;
	sourceCopySubresources.reserve(copyOperations.size());
	destinationCopySubresources.reserve(copyOperations.size());
	for (const auto& operation : copyOperations)
	{
		if (std::find(sourceCopySubresources.begin(), sourceCopySubresources.end(), operation.sourceSubresource) == sourceCopySubresources.end())
			sourceCopySubresources.push_back(operation.sourceSubresource);
		if (std::find(destinationCopySubresources.begin(), destinationCopySubresources.end(), operation.destinationSubresource) == destinationCopySubresources.end())
			destinationCopySubresources.push_back(operation.destinationSubresource);
	}
	const TextureCopyCacheKey textureCopyKey
	{
		srcTexture->GetContentIdentity(), dstTexture->GetContentIdentity(),
		srcMip, effectiveSrcX, effectiveSrcY, srcSlice,
		dstMip, effectiveDstX, effectiveDstY, dstSlice,
		effectiveCopyWidth, effectiveCopyHeight, copyDepth, 0
	};
	if (CanSkipTextureCopy(textureCopyKey, srcTexture, dstTexture, sourceCopySubresources, destinationCopySubresources))
	{
		m_textureCopyCacheHits++;
		const uint64 bytesPerTexel = std::max<uint64>((srcTexture->GetBPP() + 7) / 8, 1);
		m_textureCopyBytesSkipped += (uint64)effectiveCopyWidth * effectiveCopyHeight * copyDepth * bytesPerTexel;
		D3D12TraceEvent("texture_copy_generation_hit", "src=0x{:08x} dst=0x{:08x} size={}x{}x{} operations={}",
			srcTexture->physAddress, dstTexture->physAddress, effectiveCopyWidth, effectiveCopyHeight, copyDepth, copyOperations.size());
		return;
	}
	m_textureCopyCacheMisses++;

	const bool requiresScratchCopy = sameD3DResource && std::ranges::any_of(copyOperations,
		[&](const TextureCopyOperation& sourceOperation)
		{
			return std::ranges::any_of(copyOperations,
				[&](const TextureCopyOperation& destinationOperation)
				{
					return sourceOperation.sourceSubresource == destinationOperation.destinationSubresource;
				});
		});

	struct SavedTextureState
	{
		LatteTextureD3D12* texture{};
		uint32 subresource{};
		D3D12_RESOURCE_STATES state{};
	};
	std::vector<SavedTextureState> savedStates;
	auto canonicalStateTexture = [&](LatteTextureD3D12* texture)
	{
		return sameD3DResource ? srcTexture : texture;
	};
	auto transitionManyForCopy = [&](LatteTextureD3D12* texture, const std::vector<uint32>& subresources, D3D12_RESOURCE_STATES state)
	{
		texture = canonicalStateTexture(texture);
		std::vector<TextureTransition> transitions;
		transitions.reserve(subresources.size());
		for (uint32 subresource : subresources)
		{
			const auto saved = std::ranges::find_if(savedStates,
				[&](const SavedTextureState& entry)
				{
					return entry.texture == texture && entry.subresource == subresource;
				});
			if (saved == savedStates.end())
				savedStates.push_back({ texture, subresource, texture->GetSubresourceState(subresource) });
			if (std::ranges::none_of(transitions, [subresource](const TextureTransition& transition)
				{ return transition.subresource == subresource; }))
			{
				transitions.push_back({ subresource, state });
			}
		}
		TransitionTextureSubresources(texture, transitions);
	};
	auto restoreCopyStates = [&]()
	{
		std::vector<LatteTextureD3D12*> textures;
		for (const auto& saved : savedStates)
		{
			if (std::find(textures.begin(), textures.end(), saved.texture) == textures.end())
				textures.push_back(saved.texture);
		}
		for (auto* texture : textures)
		{
			std::vector<TextureTransition> transitions;
			for (const auto& saved : savedStates)
			{
				if (saved.texture == texture)
					transitions.push_back({ saved.subresource, saved.state });
			}
			TransitionTextureSubresources(texture, transitions);
		}
		if (sameD3DResource && srcTexture != dstTexture)
		{
			for (const auto& saved : savedStates)
			{
				if (saved.subresource < dstTexture->GetSubresourceCount())
					dstTexture->SetSubresourceState(saved.subresource, saved.state);
			}
		}
	};

	if (requiresScratchCopy)
	{
		D3D12MemoryManager::Resource scratchTexture;
		const D3D12_RESOURCE_DESC scratchDesc = srcTexture->GetDesc();
		const bool scratchReady = TryAcquireCommandScratchTexture(scratchDesc, scratchTexture) ||
			CreateTextureWithRecovery(scratchDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, scratchTexture, "same-resource texture copy");
		if (!scratchReady)
		{
			cemuLog_log(LogType::Force,
				"D3D12: failed to allocate scratch texture for same-resource copy at 0x{:08x}.",
				srcTexture->physAddress);
			return;
		}

		auto* commandList = GetCurrentCommandSlot().commandList.Get();
		Transition(scratchTexture.resource.Get(), scratchTexture.state, D3D12_RESOURCE_STATE_COPY_DEST);
		transitionManyForCopy(srcTexture, sourceCopySubresources, D3D12_RESOURCE_STATE_COPY_SOURCE);
		for (const auto& operation : copyOperations)
		{
			D3D12_TEXTURE_COPY_LOCATION stageSource{};
			stageSource.pResource = srcTexture->GetResource();
			stageSource.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
			stageSource.SubresourceIndex = operation.sourceSubresource;
			D3D12_TEXTURE_COPY_LOCATION stageDestination{};
			stageDestination.pResource = scratchTexture.resource.Get();
			stageDestination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
			stageDestination.SubresourceIndex = operation.sourceSubresource;
			commandList->CopyTextureRegion(
				&stageDestination,
				operation.wholeSubresource ? 0 : operation.sourceBox.left,
				operation.wholeSubresource ? 0 : operation.sourceBox.top,
				operation.wholeSubresource ? 0 : operation.sourceBox.front,
				&stageSource,
				operation.wholeSubresource ? nullptr : &operation.sourceBox);
		}

		Transition(scratchTexture.resource.Get(), scratchTexture.state, D3D12_RESOURCE_STATE_COPY_SOURCE);
		transitionManyForCopy(dstTexture, destinationCopySubresources, D3D12_RESOURCE_STATE_COPY_DEST);
		for (const auto& operation : copyOperations)
		{
			D3D12_TEXTURE_COPY_LOCATION scratchSource{};
			scratchSource.pResource = scratchTexture.resource.Get();
			scratchSource.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
			scratchSource.SubresourceIndex = operation.sourceSubresource;
			D3D12_TEXTURE_COPY_LOCATION copyDestination{};
			copyDestination.pResource = dstTexture->GetResource();
			copyDestination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
			copyDestination.SubresourceIndex = operation.destinationSubresource;
			commandList->CopyTextureRegion(
				&copyDestination,
				operation.wholeSubresource ? 0 : operation.destinationX,
				operation.wholeSubresource ? 0 : operation.destinationY,
				operation.wholeSubresource ? 0 : operation.destinationZ,
				&scratchSource,
				operation.wholeSubresource ? nullptr : &operation.sourceBox);
		}

		restoreCopyStates();
		RecycleCommandScratchTexture(std::move(scratchTexture), scratchDesc);
		D3D12TraceEvent("texture_copy_scratch", "resource={} srcMip={} srcSlice={} dstMip={} dstSlice={} copies={} planes={}",
			(const void*)srcTexture->GetResource(), srcMip, srcSlice, dstMip, dstSlice,
			copyOperations.size(), planeCount);
		RecordTextureCopy(textureCopyKey, srcTexture, dstTexture, sourceCopySubresources, destinationCopySubresources);
		return;
	}

	if (D3D12ShouldTrace())
	{
		auto* srcViewForTrace = static_cast<LatteTextureViewD3D12*>(srcTexture->GetOrCreateView(srcTexture->dim, srcTexture->format, srcMip, 1, srcSlice, 1));
		TraceTextureSnapshot("texture_copy_src_before", "src", srcViewForTrace, false,
			fmt::format("dst=0x{:08x} size={}x{}x{}", dstTexture->physAddress, effectiveCopyWidth, effectiveCopyHeight, copyDepth));
	}
	if (!m_commandListOpen)
		ResetCommandList();
	const auto srcStateBeforeCopy = srcTexture->GetSubresourceState(srcSubresource);
	const auto dstStateBeforeCopy = dstTexture->GetSubresourceState(dstSubresource);
	const uint32 dstZ = dstIs3D ? (uint32)dstSlice : 0u;
	D3D12TraceEvent("texture_copy_execute",
					"src=0x{:08x} srcFmt=0x{:04x} srcDxgi={} srcResource={} srcSubresource={} srcStateBefore=0x{:08x} dst=0x{:08x} dstFmt=0x{:04x} dstDxgi={} dstResource={} dstSubresource={} dstStateBefore=0x{:08x} dstXYZ={},{},{} boxLeftTopFront={},{},{} boxRightBottomBack={},{},{} copyBoxNull={} compressed={} fullSubresource={} d3dCopies={} sameAddr={} sameResource={} commandSlot={} nextFence={}",
					srcTexture->physAddress,
					(uint32)srcTexture->format,
					(uint32)srcTexture->GetDesc().Format,
					(const void*)srcTexture->GetResource(),
					srcSubresource,
					(uint32)srcStateBeforeCopy,
					dstTexture->physAddress,
					(uint32)dstTexture->format,
					(uint32)dstTexture->GetDesc().Format,
					(const void*)dstTexture->GetResource(),
					dstSubresource,
					(uint32)dstStateBeforeCopy,
					dstX,
					dstY,
					dstZ,
					sourceBox.left,
					sourceBox.top,
					sourceBox.front,
					sourceBox.right,
					sourceBox.bottom,
					sourceBox.back,
					useWholeSubresourceCopy ? 1 : 0,
					compressedCopy ? 1 : 0,
					fullSubresourceCopy ? 1 : 0,
					copyOperations.size(),
					sameLatteAddress ? 1 : 0,
					sameD3DResource ? 1 : 0,
					m_commandSlotIndex,
					(unsigned long long)m_nextFenceValue);
	transitionManyForCopy(srcTexture, sourceCopySubresources, D3D12_RESOURCE_STATE_COPY_SOURCE);
	transitionManyForCopy(dstTexture, destinationCopySubresources, D3D12_RESOURCE_STATE_COPY_DEST);

	auto* commandList = GetCurrentCommandSlot().commandList.Get();
	for (const auto& operation : copyOperations)
	{
		D3D12_TEXTURE_COPY_LOCATION copySource{};
		copySource.pResource = srcTexture->GetResource();
		copySource.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		copySource.SubresourceIndex = operation.sourceSubresource;
		D3D12_TEXTURE_COPY_LOCATION copyDestination{};
		copyDestination.pResource = dstTexture->GetResource();
		copyDestination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		copyDestination.SubresourceIndex = operation.destinationSubresource;
		commandList->CopyTextureRegion(
			&copyDestination,
			operation.wholeSubresource ? 0 : operation.destinationX,
			operation.wholeSubresource ? 0 : operation.destinationY,
			operation.wholeSubresource ? 0 : operation.destinationZ,
			&copySource,
			operation.wholeSubresource ? nullptr : &operation.sourceBox);
	}
	restoreCopyStates();
	RecordTextureCopy(textureCopyKey, srcTexture, dstTexture, sourceCopySubresources, destinationCopySubresources);
	if (D3D12ShouldTrace())
	{
		auto* dstViewForTrace = static_cast<LatteTextureViewD3D12*>(dstTexture->GetOrCreateView(dstTexture->dim, dstTexture->format, dstMip, 1, dstSlice, 1));
		RecordTextureLineage("texture_copy", dstViewForTrace, 0, 0, fmt::format("copySrc=0x{:08x}/fmt=0x{:04x}", srcTexture->physAddress, (uint32)srcTexture->format),
							 fmt::format("srcMip={} srcSlice={} dstMip={} dstSlice={} srcXY={},{} dstXY={},{} size={}x{}x{} fullSubresource={} compressed={}",
										 srcMip,
										 srcSlice,
										 dstMip,
										 dstSlice,
										 effectiveSrcX,
										 effectiveSrcY,
										 effectiveDstX,
										 effectiveDstY,
										 effectiveCopyWidth,
										 effectiveCopyHeight,
										 copyDepth,
										 fullSubresourceCopy ? 1 : 0,
										 compressedCopy ? 1 : 0));
		TraceTextureSnapshot("texture_copy_dst_after", "dst", dstViewForTrace, false, fmt::format("src=0x{:08x} fullSubresource={} compressed={}", srcTexture->physAddress, fullSubresourceCopy ? 1 : 0, compressedCopy ? 1 : 0));
	}
}

bool D3D12Renderer::TryRawAliasTextureCopy(LatteTextureD3D12* srcTexture, sint32 srcMip, sint32 srcSlice, sint32 srcX, sint32 srcY,
										   LatteTextureD3D12* dstTexture, sint32 dstMip, sint32 dstSlice, sint32 dstX, sint32 dstY,
										   sint32 width, sint32 height, sint32 depth)
{
	if (!srcTexture || !dstTexture || !m_memoryManager || !m_device || depth < 1 || width <= 0 || height <= 0)
		return false;
	if (srcTexture->isDepth || dstTexture->isDepth || !srcTexture->GetResource() || !dstTexture->GetResource())
		return false;
	const auto& srcDesc = srcTexture->GetDesc();
	const auto& dstDesc = dstTexture->GetDesc();
	if (srcMip < 0 || srcMip >= (sint32)srcDesc.MipLevels || dstMip < 0 || dstMip >= (sint32)dstDesc.MipLevels ||
		srcSlice < 0 || dstSlice < 0 || srcX < 0 || srcY < 0 || dstX < 0 || dstY < 0 ||
		srcDesc.SampleDesc.Count != 1 || dstDesc.SampleDesc.Count != 1)
		return false;
	const D3D12CopyElementLayout srcLayout = GetD3D12CopyElementLayout(srcDesc.Format);
	const D3D12CopyElementLayout dstLayout = GetD3D12CopyElementLayout(dstDesc.Format);
	if (srcLayout.bytes == 0 || srcLayout.bytes != dstLayout.bytes)
		return false;
	if (((uint32)srcX % srcLayout.blockSize) != 0 || ((uint32)srcY % srcLayout.blockSize) != 0 ||
		((uint32)dstX % dstLayout.blockSize) != 0 || ((uint32)dstY % dstLayout.blockSize) != 0)
		return false;

	const uint32 srcMipWidth = std::max<uint32>(1, (uint32)(srcDesc.Width >> srcMip));
	const uint32 srcMipHeight = std::max<uint32>(1, (uint32)(srcDesc.Height >> srcMip));
	const uint32 dstMipWidth = std::max<uint32>(1, (uint32)(dstDesc.Width >> dstMip));
	const uint32 dstMipHeight = std::max<uint32>(1, (uint32)(dstDesc.Height >> dstMip));
	if ((uint32)srcX >= srcMipWidth || (uint32)srcY >= srcMipHeight)
		return false;
	// the size is in source texels; the staging buffer holds whole texels or whole 4x4 blocks
	const uint32 elementsWide = ((uint32)width + srcLayout.blockSize - 1) / srcLayout.blockSize;
	const uint32 elementsHigh = ((uint32)height + srcLayout.blockSize - 1) / srcLayout.blockSize;
	const uint32 sourceBoxWidth = std::min<uint32>(elementsWide * srcLayout.blockSize, srcMipWidth - (uint32)srcX);
	const uint32 sourceBoxHeight = std::min<uint32>(elementsHigh * srcLayout.blockSize, srcMipHeight - (uint32)srcY);
	if (sourceBoxWidth < (uint32)width || sourceBoxHeight < (uint32)height)
		return false;
	const uint32 destinationWidth = elementsWide * dstLayout.blockSize;
	const uint32 destinationHeight = elementsHigh * dstLayout.blockSize;
	// a partial block may only overhang the edge of the destination mip
	if ((uint64)dstX + destinationWidth > AlignUpU32(dstMipWidth, dstLayout.blockSize) ||
		(uint64)dstY + destinationHeight > AlignUpU32(dstMipHeight, dstLayout.blockSize))
		return false;

	const bool srcIs3D = srcDesc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D;
	const bool dstIs3D = dstDesc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D;
	const uint32 srcSliceLimit = srcIs3D ? std::max<uint32>(1, (uint32)srcDesc.DepthOrArraySize >> srcMip) : std::max<uint32>(1, srcDesc.DepthOrArraySize);
	const uint32 dstSliceLimit = dstIs3D ? std::max<uint32>(1, (uint32)dstDesc.DepthOrArraySize >> dstMip) : std::max<uint32>(1, dstDesc.DepthOrArraySize);
	if ((uint64)srcSlice + (uint64)depth > srcSliceLimit || (uint64)dstSlice + (uint64)depth > dstSliceLimit)
		return false;

	struct SliceCopy
	{
		uint32 sourceSubresource{};
		UINT sourceZ{};
		uint32 destinationSubresource{};
		UINT destinationZ{};
	};
	std::vector<SliceCopy> sliceCopies;
	std::vector<uint32> sourceCopySubresources;
	std::vector<uint32> destinationCopySubresources;
	sliceCopies.reserve((size_t)depth);
	for (sint32 slice = 0; slice < depth; ++slice)
	{
		SliceCopy sliceCopy;
		sliceCopy.sourceSubresource = srcIs3D ? (uint32)srcMip : (uint32)srcMip + (uint32)(srcSlice + slice) * srcDesc.MipLevels;
		sliceCopy.sourceZ = srcIs3D ? (UINT)(srcSlice + slice) : 0u;
		sliceCopy.destinationSubresource = dstIs3D ? (uint32)dstMip : (uint32)dstMip + (uint32)(dstSlice + slice) * dstDesc.MipLevels;
		sliceCopy.destinationZ = dstIs3D ? (UINT)(dstSlice + slice) : 0u;
		if (std::find(sourceCopySubresources.begin(), sourceCopySubresources.end(), sliceCopy.sourceSubresource) == sourceCopySubresources.end())
			sourceCopySubresources.push_back(sliceCopy.sourceSubresource);
		if (std::find(destinationCopySubresources.begin(), destinationCopySubresources.end(), sliceCopy.destinationSubresource) == destinationCopySubresources.end())
			destinationCopySubresources.push_back(sliceCopy.destinationSubresource);
		sliceCopies.emplace_back(sliceCopy);
	}

	const uint32 rowPitch = AlignUpU32(elementsWide * srcLayout.bytes, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
	const uint64 sliceBytes = ((uint64)rowPitch * elementsHigh + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1) &
		~(uint64)(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1);
	const uint64 totalBytes = sliceBytes * (uint64)depth;

	const TextureCopyCacheKey textureCopyKey
	{
		srcTexture->GetContentIdentity(), dstTexture->GetContentIdentity(),
		srcMip, srcX, srcY, srcSlice, dstMip, dstX, dstY, dstSlice, width, height, depth, 1
	};
	if (CanSkipTextureCopy(textureCopyKey, srcTexture, dstTexture, sourceCopySubresources, destinationCopySubresources))
	{
		m_textureCopyCacheHits++;
		m_textureCopyBytesSkipped += totalBytes;
		return true;
	}
	m_textureCopyCacheMisses++;

	// direct-queue ordering lets every copy reuse one staging buffer; very large copies get their own
	constexpr uint64 kMaxRetainedStagingBytes = 32ull * 1024ull * 1024ull;
	D3D12MemoryManager::Resource oversizedStagingBuffer;
	D3D12MemoryManager::Resource* stagingBuffer = &m_textureAliasCopyBuffer;
	if (totalBytes > kMaxRetainedStagingBytes)
	{
		if (!m_memoryManager->CreateBuffer(totalBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, oversizedStagingBuffer))
			return false;
		stagingBuffer = &oversizedStagingBuffer;
	}
	else if (!m_textureAliasCopyBuffer.IsValid() || m_textureAliasCopyBuffer.size < totalBytes)
	{
		uint64 stagingSize = std::max<uint64>(1024ull * 1024ull, m_textureAliasCopyBuffer.size * 2);
		while (stagingSize < totalBytes)
			stagingSize *= 2;
		stagingSize = std::min<uint64>(stagingSize, kMaxRetainedStagingBytes);
		ReleaseResourceDeferred(m_textureAliasCopyBuffer);
		if (!m_memoryManager->CreateBuffer(stagingSize, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, m_textureAliasCopyBuffer))
			return false;
	}

	if (!m_commandListOpen)
		ResetCommandList();

	if (D3D12ShouldTrace())
	{
		auto* srcViewForTrace = static_cast<LatteTextureViewD3D12*>(srcTexture->GetOrCreateView(srcTexture->dim, srcTexture->format, srcMip, 1, srcSlice, 1));
		TraceTextureSnapshot("texture_copy_raw_alias_src_before", "src", srcViewForTrace, false,
			fmt::format("dst=0x{:08x} size={}x{}x{}", dstTexture->physAddress, width, height, depth));
	}
	D3D12TraceEvent("texture_copy_raw_alias_execute",
					"src=0x{:08x} srcFmt=0x{:04x} srcDxgi={} srcMip={} srcSlice={} srcXY={},{} dst=0x{:08x} dstFmt=0x{:04x} dstDxgi={} dstMip={} dstSlice={} dstXY={},{} size={}x{}x{} elements={}x{} bytes={} rowPitch={} srcResource={} dstResource={} stagingResource={} commandSlot={} nextFence={}",
					srcTexture->physAddress,
					(uint32)srcTexture->format,
					(uint32)srcDesc.Format,
					srcMip,
					srcSlice,
					srcX,
					srcY,
					dstTexture->physAddress,
					(uint32)dstTexture->format,
					(uint32)dstDesc.Format,
					dstMip,
					dstSlice,
					dstX,
					dstY,
					width,
					height,
					depth,
					elementsWide,
					elementsHigh,
					(unsigned long long)totalBytes,
					rowPitch,
					(const void*)srcTexture->GetResource(),
					(const void*)dstTexture->GetResource(),
					(const void*)stagingBuffer->resource.Get(),
					m_commandSlotIndex,
					(unsigned long long)m_nextFenceValue);

	auto* commandList = GetCurrentCommandSlot().commandList.Get();
	const bool sameResource = srcTexture->GetResource() == dstTexture->GetResource();
	auto* destinationStateTexture = sameResource ? srcTexture : dstTexture;
	std::vector<TextureTransition> sourceRestoreStates;
	std::vector<TextureTransition> sourceCopyStates;
	for (uint32 subresource : sourceCopySubresources)
	{
		sourceRestoreStates.push_back({ subresource, srcTexture->GetSubresourceState(subresource) });
		sourceCopyStates.push_back({ subresource, D3D12_RESOURCE_STATE_COPY_SOURCE });
	}
	Transition(stagingBuffer->resource.Get(), stagingBuffer->state, D3D12_RESOURCE_STATE_COPY_DEST);
	TransitionTextureSubresources(srcTexture, sourceCopyStates);
	for (size_t sliceIndex = 0; sliceIndex < sliceCopies.size(); ++sliceIndex)
	{
		const SliceCopy& sliceCopy = sliceCopies[sliceIndex];
		D3D12_TEXTURE_COPY_LOCATION sourceLocation{};
		sourceLocation.pResource = srcTexture->GetResource();
		sourceLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		sourceLocation.SubresourceIndex = sliceCopy.sourceSubresource;
		D3D12_TEXTURE_COPY_LOCATION stagingLocation{};
		stagingLocation.pResource = stagingBuffer->resource.Get();
		stagingLocation.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
		stagingLocation.PlacedFootprint.Offset = sliceBytes * sliceIndex;
		stagingLocation.PlacedFootprint.Footprint.Format = srcDesc.Format;
		stagingLocation.PlacedFootprint.Footprint.Width = elementsWide * srcLayout.blockSize;
		stagingLocation.PlacedFootprint.Footprint.Height = elementsHigh * srcLayout.blockSize;
		stagingLocation.PlacedFootprint.Footprint.Depth = 1;
		stagingLocation.PlacedFootprint.Footprint.RowPitch = rowPitch;
		const D3D12_BOX sourceBox{ (UINT)srcX, (UINT)srcY, sliceCopy.sourceZ,
			(UINT)srcX + sourceBoxWidth, (UINT)srcY + sourceBoxHeight, sliceCopy.sourceZ + 1 };
		commandList->CopyTextureRegion(&stagingLocation, 0, 0, 0, &sourceLocation, &sourceBox);
	}
	Transition(stagingBuffer->resource.Get(), stagingBuffer->state, D3D12_RESOURCE_STATE_COPY_SOURCE);

	std::vector<TextureTransition> destinationRestoreStates;
	std::vector<TextureTransition> destinationCopyStates;
	for (uint32 subresource : destinationCopySubresources)
	{
		destinationRestoreStates.push_back({ subresource, destinationStateTexture->GetSubresourceState(subresource) });
		destinationCopyStates.push_back({ subresource, D3D12_RESOURCE_STATE_COPY_DEST });
	}
	TransitionTextureSubresources(destinationStateTexture, destinationCopyStates);
	for (size_t sliceIndex = 0; sliceIndex < sliceCopies.size(); ++sliceIndex)
	{
		const SliceCopy& sliceCopy = sliceCopies[sliceIndex];
		D3D12_TEXTURE_COPY_LOCATION destinationLocation{};
		destinationLocation.pResource = dstTexture->GetResource();
		destinationLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		destinationLocation.SubresourceIndex = sliceCopy.destinationSubresource;
		D3D12_TEXTURE_COPY_LOCATION stagingLocation{};
		stagingLocation.pResource = stagingBuffer->resource.Get();
		stagingLocation.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
		stagingLocation.PlacedFootprint.Offset = sliceBytes * sliceIndex;
		stagingLocation.PlacedFootprint.Footprint.Format = dstDesc.Format;
		stagingLocation.PlacedFootprint.Footprint.Width = destinationWidth;
		stagingLocation.PlacedFootprint.Footprint.Height = destinationHeight;
		stagingLocation.PlacedFootprint.Footprint.Depth = 1;
		stagingLocation.PlacedFootprint.Footprint.RowPitch = rowPitch;
		commandList->CopyTextureRegion(&destinationLocation, (UINT)dstX, (UINT)dstY, sliceCopy.destinationZ, &stagingLocation, nullptr);
	}
	TransitionTextureSubresources(destinationStateTexture, destinationRestoreStates);
	TransitionTextureSubresources(srcTexture, sourceRestoreStates);
	if (sameResource && srcTexture != dstTexture)
	{
		for (uint32 subresource : sourceCopySubresources)
		{
			if (subresource < dstTexture->GetSubresourceCount())
				dstTexture->SetSubresourceState(subresource, srcTexture->GetSubresourceState(subresource));
		}
		for (uint32 subresource : destinationCopySubresources)
		{
			if (subresource < dstTexture->GetSubresourceCount())
				dstTexture->SetSubresourceState(subresource, srcTexture->GetSubresourceState(subresource));
		}
	}
	RecordTextureCopy(textureCopyKey, srcTexture, dstTexture, sourceCopySubresources, destinationCopySubresources);

	if (D3D12ShouldTrace())
	{
		auto* dstViewForTrace = static_cast<LatteTextureViewD3D12*>(dstTexture->GetOrCreateView(dstTexture->dim, dstTexture->format, dstMip, 1, dstSlice, 1));
		RecordTextureLineage("texture_copy_raw_alias", dstViewForTrace, 0, 0,
							 fmt::format("copySrc=0x{:08x}/fmt=0x{:04x}", srcTexture->physAddress, (uint32)srcTexture->format),
							 fmt::format("srcDxgi={} dstDxgi={} size={}x{}x{} bytes={} rowPitch={}",
										 (uint32)srcDesc.Format,
										 (uint32)dstDesc.Format,
										 width,
										 height,
										 depth,
										 (unsigned long long)totalBytes,
										 rowPitch));
		TraceTextureSnapshot("texture_copy_raw_alias_dst_after", "dst", dstViewForTrace, false, fmt::format("src=0x{:08x} bytes={}", srcTexture->physAddress, (unsigned long long)totalBytes));
	}
	if (oversizedStagingBuffer.IsValid())
		QueueDeferredRelease(std::move(oversizedStagingBuffer));
	return true;
}

LatteTextureReadbackInfoD3D12* D3D12Renderer::CreateHostTextureReadback(LatteTextureView* textureView)
{
	auto* readback = new LatteTextureReadbackInfoD3D12(this, textureView);
	if (!readback->IsValid())
	{
		delete readback;
		return nullptr;
	}
	return readback;
}

LatteTextureReadbackInfo* D3D12Renderer::texture_createReadback(LatteTextureView* textureView)
{
	// only computes the copy footprint (immutable texture description); the copy is recorded by StartTransfer
	auto* texture = textureView ? static_cast<LatteTextureD3D12*>(textureView->baseTexture) : nullptr;
	if (!texture)
		return nullptr;
	// readers expect guest texel layout, which substituted and block-compressed host formats do not have
	const auto& formatInfo = texture->GetFormatInfo();
	const uint32 guestTexelBytes = texture->GetBPP() / 8;
	const uint32 hostTexelBytes = GetD3D12CopyElementLayout(texture->GetDesc().Format).bytes;
	const bool hostLayoutMatchesGuest = !formatInfo.isAlternateFormat && !formatInfo.isCompressed && !texture->IsCompressedFormat() &&
		(texture->isDepth || hostTexelBytes == guestTexelBytes);
	if (!hostLayoutMatchesGuest)
	{
		static bool s_loggedUnsupportedReadback = false;
		if (!s_loggedUnsupportedReadback)
		{
			s_loggedUnsupportedReadback = true;
			cemuLog_log(LogType::Force, "D3D12: texture readback is not supported for format 0x{:04x} (host dxgi={}).",
				(uint32)texture->format, (uint32)texture->GetDesc().Format);
		}
		return nullptr;
	}
	auto* readback = CreateHostTextureReadback(textureView);
	if (readback && readback->GetRowSizeInBytes() != std::max<uint64>(texture->GetDesc().Width >> textureView->firstMip, 1) * guestTexelBytes)
	{
		delete readback;
		return nullptr;
	}
	return readback;
}

void D3D12Renderer::surfaceCopy_copySurfaceWithFormatConversion(LatteTexture* sourceTexture, sint32 srcMip, sint32 srcSlice, LatteTexture* destinationTexture, sint32 dstMip, sint32 dstSlice, sint32 width, sint32 height)
{
	auto recordCopy = [=, this](LatteTextureViewD3D12* preparedSourceView, LatteTextureViewD3D12* preparedDestinationView)
	{
		if (destinationTexture)
			DropRetainedFeedbackCopies(destinationTexture);
		stdx::scope_exit deviceCheck([&]() { CheckDeviceAfterOperation("surface copy", [&]() {
			return fmt::format("src {} gx2fmt=0x{:04x} mip={} slice={} -> dst {} gx2fmt=0x{:04x} mip={} slice={} size {}x{}", D3D12DescribeResource(sourceTexture ? static_cast<LatteTextureD3D12*>(sourceTexture)->GetResource() : nullptr),
				sourceTexture ? (uint32)sourceTexture->format : 0, srcMip, srcSlice, D3D12DescribeResource(destinationTexture ? static_cast<LatteTextureD3D12*>(destinationTexture)->GetResource() : nullptr),
				destinationTexture ? (uint32)destinationTexture->format : 0, dstMip, dstSlice, width, height); }); });
		SurfaceCopyWithFormatConversion(sourceTexture, srcMip, srcSlice, destinationTexture, dstMip, dstSlice, 0, 0, 0, 0, width, height, false,
			true, preparedSourceView, preparedDestinationView);
	};
	if (ShouldQueueRecording())
	{
		// a color<->depth copy always draws: the views it samples and renders to, and the GPU write tracking, are core
		// state, so they are done here and the draw is queued
		auto* srcTexture = static_cast<LatteTextureD3D12*>(sourceTexture);
		auto* dstTexture = static_cast<LatteTextureD3D12*>(destinationTexture);
		if (srcTexture && dstTexture && srcTexture->isDepth != dstTexture->isDepth)
		{
			if (!srcTexture->GetResource() || !dstTexture->GetResource() ||
				srcMip < 0 || srcMip >= (sint32)srcTexture->GetDesc().MipLevels ||
				dstMip < 0 || dstMip >= (sint32)dstTexture->GetDesc().MipLevels)
				return;
			sint32 effectiveCopyWidth = width;
			sint32 effectiveCopyHeight = height;
			LatteTexture_scaleToEffectiveSize(sourceTexture, &effectiveCopyWidth, &effectiveCopyHeight, 0);
			if (effectiveCopyWidth <= 0 || effectiveCopyHeight <= 0)
				return;
			auto* sourceView = static_cast<LatteTextureViewD3D12*>(srcTexture->Is3DTexture() ?
				sourceTexture->GetOrCreateView(Latte::E_DIM::DIM_2D, sourceTexture->format, srcMip, 1, srcSlice, 1) :
				sourceTexture->GetOrCreateView(srcMip, 1, srcSlice, 1));
			auto* destinationView = static_cast<LatteTextureViewD3D12*>(destinationTexture->GetOrCreateView(dstMip, 1, dstSlice, 1));
			if (!sourceView || !destinationView)
				return;
			LatteTexture_TrackTextureGPUWrite(dstTexture, dstSlice, dstMip, LatteTexture_getNextUpdateEventCounter());
			QueueRecording([recordCopy, sourceView, destinationView]() { recordCopy(sourceView, destinationView); }, RecordOpKind::Command);
			return;
		}
	}
	D3D12_EXCLUSIVE_RECORDING();
	recordCopy(nullptr, nullptr);
}

void D3D12Renderer::SurfaceCopyWithFormatConversion(LatteTexture* sourceTexture, sint32 srcMip, sint32 srcSlice, LatteTexture* destinationTexture, sint32 dstMip, sint32 dstSlice,
												sint32 srcX, sint32 srcY, sint32 dstX, sint32 dstY, sint32 width, sint32 height, bool coordinatesAreEffective,
												bool allowNativeCopy, LatteTextureViewD3D12* preparedSourceView,
												LatteTextureViewD3D12* preparedDestinationView)
{
	auto* srcTexture = static_cast<LatteTextureD3D12*>(sourceTexture);
	auto* dstTexture = static_cast<LatteTextureD3D12*>(destinationTexture);
	if (!srcTexture || !dstTexture || !srcTexture->GetResource() || !dstTexture->GetResource())
		return;
	// guest mips beyond what the texture size allows have no storage on the host
	if (srcMip < 0 || srcMip >= (sint32)srcTexture->GetDesc().MipLevels ||
		dstMip < 0 || dstMip >= (sint32)dstTexture->GetDesc().MipLevels)
		return;
#if defined(CEMU_UWP_D3D12_CORRECTNESS_TELEMETRY)
	++m_correctnessSurfaceCopyRequests;
#endif
	const char* copyModeName = "color-to-color";
	if (srcTexture->isDepth && !dstTexture->isDepth)
		copyModeName = "depth-to-color";
	else if (!srcTexture->isDepth && dstTexture->isDepth)
		copyModeName = "color-to-depth";
	else if (srcTexture->isDepth && dstTexture->isDepth)
		copyModeName = "depth-to-depth";
	const bool sameLatteAddress = srcTexture->physAddress == dstTexture->physAddress;
	const bool sameD3DResource = srcTexture->GetResource() == dstTexture->GetResource();
	static uint32 s_surfaceCopyLogCount = 0;
	if (D3D12RuntimeLogCapped(s_surfaceCopyLogCount, "surfaceCopy_copySurfaceWithFormatConversion", 96))
	{
		cemuLog_log(LogType::Force, "D3D12: surface copy mode={} src=0x{:08x}/fmt=0x{:04x}/depth={} dst=0x{:08x}/fmt=0x{:04x}/depth={} srcMip={} srcSlice={} dstMip={} dstSlice={} srcXY=({}, {}) dstXY=({}, {}) size={}x{} effectiveCoords={} sameAddr={} sameResource={} srcObj={} dstObj={}.",
					copyModeName,
					srcTexture->physAddress, (uint32)srcTexture->format, srcTexture->isDepth,
					dstTexture->physAddress, (uint32)dstTexture->format, dstTexture->isDepth,
					srcMip, srcSlice, dstMip, dstSlice,
					srcX, srcY, dstX, dstY, width, height,
					coordinatesAreEffective ? 1 : 0,
					sameLatteAddress,
					sameD3DResource,
					(const void*)srcTexture,
					(const void*)dstTexture);
	}
	D3D12TraceEvent("surface_copy_begin", "mode={} src=0x{:08x} srcFmt=0x{:04x} srcDxgi={} srcDepth={} dst=0x{:08x} dstFmt=0x{:04x} dstDxgi={} dstDepth={} srcMip={} srcSlice={} dstMip={} dstSlice={} srcXY={},{} dstXY={},{} size={}x{} effectiveCoords={} sameAddr={} sameResource={} srcObj={} dstObj={}",
					copyModeName,
					srcTexture->physAddress,
					(uint32)srcTexture->format,
					(uint32)srcTexture->GetDesc().Format,
					srcTexture->isDepth ? 1 : 0,
					dstTexture->physAddress,
					(uint32)dstTexture->format,
					(uint32)dstTexture->GetDesc().Format,
					dstTexture->isDepth ? 1 : 0,
					srcMip,
					srcSlice,
					dstMip,
					dstSlice,
					srcX,
					srcY,
					dstX,
					dstY,
					width,
					height,
					coordinatesAreEffective ? 1 : 0,
					sameLatteAddress ? 1 : 0,
					sameD3DResource ? 1 : 0,
					(const void*)srcTexture,
					(const void*)dstTexture);
	sint32 effectiveCopyWidth = width;
	sint32 effectiveCopyHeight = height;
	if (!coordinatesAreEffective)
		LatteTexture_scaleToEffectiveSize(sourceTexture, &effectiveCopyWidth, &effectiveCopyHeight, 0);
	if (effectiveCopyWidth <= 0 || effectiveCopyHeight <= 0)
		return;

	const bool nativeCopyCompatible = AreD3D12CopyFormatsCompatible(srcTexture->GetDesc().Format, dstTexture->GetDesc().Format);
	if (allowNativeCopy && srcTexture->isDepth == dstTexture->isDepth && nativeCopyCompatible)
	{
#if defined(CEMU_UWP_D3D12_CORRECTNESS_TELEMETRY)
		++m_correctnessSurfaceCopyNative;
#endif
		D3D12TraceEvent("surface_copy_native_redirect", "src=0x{:08x} dst=0x{:08x} srcXY={},{} dstXY={},{} requestedSize={}x{} effectiveSize={}x{}",
						srcTexture->physAddress, dstTexture->physAddress,
						srcX, srcY, dstX, dstY,
						width, height,
						effectiveCopyWidth, effectiveCopyHeight);
		texture_copyImageSubData(sourceTexture, srcMip, srcX, srcY, srcSlice, destinationTexture, dstMip, dstX, dstY, dstSlice, effectiveCopyWidth, effectiveCopyHeight, 1);
		return;
	}

	if (srcTexture->isDepth == dstTexture->isDepth)
	{
		// raw reinterpretation only replaces the conversion draw for whole uncompressed 2D mips
		const bool wholeMipCopy =
			srcX == 0 && srcY == 0 && dstX == 0 && dstY == 0 &&
			!srcTexture->Is3DTexture() && !dstTexture->Is3DTexture() &&
			!srcTexture->GetFormatInfo().isCompressed && !dstTexture->GetFormatInfo().isCompressed &&
			effectiveCopyWidth == (sint32)std::max<uint64>(srcTexture->GetDesc().Width >> srcMip, 1) &&
			effectiveCopyHeight == (sint32)std::max<uint32>(srcTexture->GetDesc().Height >> srcMip, 1) &&
			effectiveCopyWidth == (sint32)std::max<uint64>(dstTexture->GetDesc().Width >> dstMip, 1) &&
			effectiveCopyHeight == (sint32)std::max<uint32>(dstTexture->GetDesc().Height >> dstMip, 1);
		if (!srcTexture->isDepth && srcTexture->GetBPP() == dstTexture->GetBPP() && wholeMipCopy)
		{
			if (TryRawAliasTextureCopy(srcTexture, srcMip, srcSlice, srcX, srcY,
									   dstTexture, dstMip, dstSlice, dstX, dstY,
									   effectiveCopyWidth, effectiveCopyHeight, 1))
			{
#if defined(CEMU_UWP_D3D12_CORRECTNESS_TELEMETRY)
				++m_correctnessSurfaceCopyRawAlias;
#endif
				D3D12TraceEvent("surface_copy_raw_alias_redirect", "src=0x{:08x} srcDxgi={} dst=0x{:08x} dstDxgi={} requestedSize={}x{} effectiveSize={}x{}",
								srcTexture->physAddress,
								(uint32)srcTexture->GetDesc().Format,
								dstTexture->physAddress,
								(uint32)dstTexture->GetDesc().Format,
								width,
								height,
								effectiveCopyWidth,
								effectiveCopyHeight);
				return;
			}
		}
		D3D12TraceEvent("surface_copy_shader_conversion",
			"reason=same_aspect_incompatible src=0x{:08x} srcDxgi={} srcDepth={} dst=0x{:08x} dstDxgi={} dstDepth={} requestedSize={}x{} effectiveSize={}x{} srcBpp={} dstBpp={}",
			srcTexture->physAddress,
			(uint32)srcTexture->GetDesc().Format,
			srcTexture->isDepth ? 1 : 0,
			dstTexture->physAddress,
			(uint32)dstTexture->GetDesc().Format,
			dstTexture->isDepth ? 1 : 0,
			width,
			height,
			effectiveCopyWidth,
			effectiveCopyHeight,
			srcTexture->GetBPP(),
			dstTexture->GetBPP());
	}

	// a 2D view of a 3D texture samples the slice through a 2D sampling proxy
	// prepared views come from the Latte thread, which also tracked the GPU write (threaded recording)
	if (!preparedSourceView && m_preparedSurfaceCopy.sourceView &&
		m_preparedSurfaceCopy.source == sourceTexture && m_preparedSurfaceCopy.sourceMip == srcMip && m_preparedSurfaceCopy.sourceSlice == srcSlice &&
		m_preparedSurfaceCopy.destination == destinationTexture && m_preparedSurfaceCopy.destinationMip == dstMip && m_preparedSurfaceCopy.destinationSlice == dstSlice)
	{
		preparedSourceView = m_preparedSurfaceCopy.sourceView;
		preparedDestinationView = m_preparedSurfaceCopy.destinationView;
	}
	auto* sourceView = preparedSourceView ? preparedSourceView : static_cast<LatteTextureViewD3D12*>(srcTexture->Is3DTexture() ?
		sourceTexture->GetOrCreateView(Latte::E_DIM::DIM_2D, sourceTexture->format, srcMip, 1, srcSlice, 1) :
		sourceTexture->GetOrCreateView(srcMip, 1, srcSlice, 1));
	auto* destinationView = preparedDestinationView ? preparedDestinationView :
		static_cast<LatteTextureViewD3D12*>(destinationTexture->GetOrCreateView(dstMip, 1, dstSlice, 1));
	if (!sourceView || !destinationView)
	{
#if defined(CEMU_UWP_D3D12_CORRECTNESS_TELEMETRY)
		++m_correctnessSurfaceCopyInvalidView;
#endif
		return;
	}
	sourceView->PrepareForSampling();
	const D3D12_SHADER_RESOURCE_VIEW_DESC sourceSrvDesc = sourceView->BuildSubresourceArraySrvDesc(false);
	if (sourceSrvDesc.Format == DXGI_FORMAT_UNKNOWN)
	{
#if defined(CEMU_UWP_D3D12_CORRECTNESS_TELEMETRY)
		++m_correctnessSurfaceCopyInvalidView;
#endif
		return;
	}

	SurfaceCopyMode copyMode = SurfaceCopyMode::ColorToColor;
	if (srcTexture->isDepth && !dstTexture->isDepth)
		copyMode = SurfaceCopyMode::DepthToColor;
	else if (!srcTexture->isDepth && dstTexture->isDepth)
		copyMode = SurfaceCopyMode::ColorToDepth;
	else if (srcTexture->isDepth && dstTexture->isDepth)
		copyMode = SurfaceCopyMode::DepthToDepth;
	const bool copyWritesDepth =
		copyMode == SurfaceCopyMode::ColorToDepth
		|| copyMode == SurfaceCopyMode::DepthToDepth
		;

	const uint32 sourceSubresource = srcTexture->GetSubresourceIndex(srcTexture->Is3DTexture() ? 0 : srcSlice, srcMip);
	const uint32 destinationSubresource = dstTexture->GetSubresourceIndex(dstTexture->Is3DTexture() ? 0 : dstSlice, dstMip);
	const std::vector<uint32> sourceCopySubresources{ sourceSubresource };
	const std::vector<uint32> destinationCopySubresources{ destinationSubresource };
	const TextureCopyCacheKey surfaceCopyKey
	{
		srcTexture->GetContentIdentity(), dstTexture->GetContentIdentity(),
		srcMip, srcX, srcY, srcSlice, dstMip, dstX, dstY, dstSlice,
		effectiveCopyWidth, effectiveCopyHeight, 1, 10u + (uint32)copyMode
	};
	if (CanSkipTextureCopy(surfaceCopyKey, srcTexture, dstTexture, sourceCopySubresources, destinationCopySubresources))
	{
		m_surfaceConversionCacheHits++;
		return;
	}

	const DXGI_FORMAT rtvFormat = copyWritesDepth ? DXGI_FORMAT_UNKNOWN : destinationView->GetRTV().IsValid() ? destinationView->GetRTVFormat() : DXGI_FORMAT_UNKNOWN;
	const DXGI_FORMAT dsvFormat = copyWritesDepth ? dstTexture->GetFormatInfo().dsvFormat : DXGI_FORMAT_UNKNOWN;
	if (copyWritesDepth)
	{
		if (!destinationView->GetDSV().IsValid() || dsvFormat == DXGI_FORMAT_UNKNOWN)
		{
#if defined(CEMU_UWP_D3D12_CORRECTNESS_TELEMETRY)
			++m_correctnessSurfaceCopyInvalidOutput;
#endif
			return;
		}
	}
	else
	{
		if (!destinationView->GetRTV().IsValid() || rtvFormat == DXGI_FORMAT_UNKNOWN)
		{
#if defined(CEMU_UWP_D3D12_CORRECTNESS_TELEMETRY)
			++m_correctnessSurfaceCopyInvalidOutput;
#endif
			return;
		}
	}

	ID3D12PipelineState* pipeline = GetSurfaceCopyPipeline(copyMode, rtvFormat, dsvFormat);
	if (!pipeline)
	{
#if defined(CEMU_UWP_D3D12_CORRECTNESS_TELEMETRY)
		++m_correctnessSurfaceCopyNoPso;
#endif
		return;
	}
	ID3D12PipelineState* stencilPipeline = nullptr;
	D3D12_SHADER_RESOURCE_VIEW_DESC stencilSrvDesc{};
	if (copyMode == SurfaceCopyMode::DepthToDepth && srcTexture->GetFormatInfo().hasStencil && dstTexture->GetFormatInfo().hasStencil)
	{
		if (m_psSpecifiedStencilRefSupported)
		{
			stencilSrvDesc = sourceView->BuildSubresourceArraySrvDesc(true);
			if (stencilSrvDesc.Format != DXGI_FORMAT_UNKNOWN)
				stencilPipeline = GetSurfaceCopyPipeline(SurfaceCopyMode::StencilToStencil, DXGI_FORMAT_UNKNOWN, dsvFormat);
		}
		else
		{
			cemuLog_logOnce(LogType::Force, "D3D12: shader-specified stencil reference is unsupported; depth-stencil surface copies do not copy stencil.");
		}
	}
#if defined(CEMU_UWP_D3D12_CORRECTNESS_TELEMETRY)
	++m_correctnessSurfaceCopyShader;
#endif
	m_surfaceConversionCacheMisses++;
	if (D3D12ShouldTrace())
	{
		TraceTextureSnapshot("surface_copy_src_before", "before_surface_copy_draw", sourceView, false,
			fmt::format("mode={} dst=0x{:08x} srcXY={},{} dstXY={},{} requestedSize={}x{} effectiveSize={}x{}",
				copyModeName, dstTexture->physAddress, srcX, srcY, dstX, dstY, width, height, effectiveCopyWidth, effectiveCopyHeight));
	}
	if (!m_commandListOpen)
		ResetCommandList();
	const auto surfaceCopyDescriptorStats = m_descriptorManager->GetStats();
	const bool hasCachedPointSampler =
		m_shaderVisibleOutputPointSampler.IsValid() &&
		m_shaderVisibleOutputSamplerGeneration == surfaceCopyDescriptorStats.shaderVisibleSamplerRollovers;
	EnsureShaderVisibleDescriptorSpace(stencilPipeline ? 2 : 1, hasCachedPointSampler ? 0 : 1);

	// the copy shaders declare Texture2DArray, which the view's own SRV may not match
	auto srv = m_descriptorManager->AllocateShaderVisible(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	CreateShaderResourceViewChecked(sourceView->GetSrvResource(), &sourceSrvDesc, srv.cpu, "surface copy source", true);
	D3D12DescriptorManager::Allocation stencilSrv;
	if (stencilPipeline)
	{
		stencilSrv = m_descriptorManager->AllocateShaderVisible(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
		CreateShaderResourceViewChecked(sourceView->GetSrvResource(), &stencilSrvDesc, stencilSrv.cpu, "surface copy stencil", true);
	}
	auto sampler = GetShaderVisibleOutputSampler(false);
	BindShaderVisibleHeaps();
	D3D12TraceEvent("surface_copy_execute",
					"mode={} src=0x{:08x} srcFmt=0x{:04x} srcDxgi={} srcResource={} dst=0x{:08x} dstFmt=0x{:04x} dstDxgi={} dstResource={} srcXY={},{} dstXY={},{} requestedSize={}x{} effectiveSize={}x{} srcMip={} srcSlice={} dstMip={} dstSlice={} srvGpu=0x{:016x} samplerGpu=0x{:016x} pipeline={} commandSlot={} nextFence={}",
					copyModeName,
					srcTexture->physAddress,
					(uint32)srcTexture->format,
					(uint32)sourceView->GetViewFormat(),
					(const void*)srcTexture->GetResource(),
					dstTexture->physAddress,
					(uint32)dstTexture->format,
					copyWritesDepth ? (uint32)dsvFormat : (uint32)rtvFormat,
					(const void*)dstTexture->GetResource(),
					srcX,
					srcY,
					dstX,
					dstY,
					width,
					height,
					effectiveCopyWidth,
					effectiveCopyHeight,
					srcMip,
					srcSlice,
					dstMip,
					dstSlice,
					(unsigned long long)srv.gpu.ptr,
					(unsigned long long)sampler.gpu.ptr,
					(const void*)pipeline,
					m_commandSlotIndex,
					(unsigned long long)m_nextFenceValue);
	TransitionTextureView(sourceView, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
		stencilPipeline != nullptr);
	if (copyWritesDepth)
		TransitionTextureView(destinationView, D3D12_RESOURCE_STATE_DEPTH_WRITE, true);
	else
		TransitionTextureView(destinationView, D3D12_RESOURCE_STATE_RENDER_TARGET);

	auto* commandList = GetCurrentCommandSlot().commandList.Get();
	SetD3D12PipelineState(pipeline);
	SetD3D12GraphicsRootSignature(m_surfaceCopyRootSignature.Get());
	commandList->SetGraphicsRootDescriptorTable(0, srv.gpu);
	commandList->SetGraphicsRootDescriptorTable(1, sampler.gpu);
	const sint32 copyOffsets[4]
	{
		srcX,
		srcY,
		dstX,
		dstY,
	};
	commandList->SetGraphicsRoot32BitConstants(2, 4, copyOffsets, 0);

	D3D12_VIEWPORT viewport{};
	viewport.TopLeftX = (float)std::max<sint32>(dstX, 0);
	viewport.TopLeftY = (float)std::max<sint32>(dstY, 0);
	viewport.Width = (float)effectiveCopyWidth;
	viewport.Height = (float)effectiveCopyHeight;
	viewport.MinDepth = 0.0f;
	viewport.MaxDepth = 1.0f;
	D3D12_RECT scissor{};
	scissor.left = std::max<sint32>(dstX, 0);
	scissor.top = std::max<sint32>(dstY, 0);
	scissor.right = scissor.left + effectiveCopyWidth;
	scissor.bottom = scissor.top + effectiveCopyHeight;
	SetD3D12Viewport(viewport);
	SetD3D12Scissor(scissor);
	SetD3D12PrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

	if (copyWritesDepth)
	{
		const auto dsv = destinationView->GetDSV().cpu;
		SetD3D12RenderTargets(0, nullptr, &dsv);
	}
	else
	{
		const auto rtv = destinationView->GetRTV().cpu;
		SetD3D12RenderTargets(1, &rtv, nullptr);
	}
	commandList->DrawInstanced(3, 1, 0, 0);
	m_gpuWatchdog.RecordOperation(D3D12GpuWatchdog::Operation::SurfaceCopy);
	if (stencilPipeline)
	{
		SetD3D12PipelineState(stencilPipeline);
		commandList->SetGraphicsRootDescriptorTable(0, stencilSrv.gpu);
		commandList->DrawInstanced(3, 1, 0, 0);
	}
	SetD3D12Viewport(m_currentViewport);
	SetD3D12Scissor(m_currentScissor);
	TransitionTextureView(destinationView, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
		copyWritesDepth);
	if (!preparedDestinationView)
		LatteTexture_TrackTextureGPUWrite(dstTexture, dstSlice, dstMip, LatteTexture_getNextUpdateEventCounter());
	RecordTextureCopy(surfaceCopyKey, srcTexture, dstTexture, sourceCopySubresources, destinationCopySubresources);
	if (D3D12ShouldTrace())
	{
		RecordTextureLineage("surface_copy", destinationView, 0, 0, fmt::format("surfaceSrc=0x{:08x}/fmt=0x{:04x}", srcTexture->physAddress, (uint32)srcTexture->format),
							 fmt::format("mode={} srcMip={} srcSlice={} dstMip={} dstSlice={} srcXY={},{} dstXY={},{} requestedSize={}x{} effectiveSize={}x{} pipeline={}",
										 copyModeName,
										 srcMip,
										 srcSlice,
										 dstMip,
										 dstSlice,
										 srcX,
										 srcY,
										 dstX,
										 dstY,
										 width,
										 height,
										 effectiveCopyWidth,
										 effectiveCopyHeight,
										 (const void*)pipeline));
		TraceTextureSnapshot("surface_copy_dst_after", "after_surface_copy_draw", destinationView, false, fmt::format("mode={} src=0x{:08x} srcXY={},{} dstXY={},{} requestedSize={}x{} effectiveSize={}x{}", copyModeName, srcTexture->physAddress, srcX, srcY, dstX, dstY, width, height, effectiveCopyWidth, effectiveCopyHeight));
	}
}

void D3D12Renderer::DrawHandleSpecialState5()
{
	LatteMRT::UpdateCurrentFBO();
	LatteRenderTarget_updateViewport();

	LatteTextureView* colorBuffer = LatteMRT::GetColorAttachment(0);
	LatteTextureView* depthBuffer = LatteMRT::GetDepthAttachment();
	if (!colorBuffer || !depthBuffer || !colorBuffer->baseTexture || !depthBuffer->baseTexture)
	{
		static uint32 s_specialState5MissingAttachmentLogCount = 0;
		if (D3D12RuntimeLogCapped(s_specialState5MissingAttachmentLogCount, "special state 5 missing attachment", 16))
			cemuLog_log(LogType::Force, "D3D12: special state 5 depth-to-color copy skipped because color/depth attachment is missing.");
		return;
	}

	sint32 vpWidth = 0;
	sint32 vpHeight = 0;
	LatteMRT::GetVirtualViewportDimensions(vpWidth, vpHeight);
	if (vpWidth <= 0 || vpHeight <= 0)
		return;

	static uint32 s_specialState5LogCount = 0;
	if (D3D12RuntimeLogCapped(s_specialState5LogCount, "special state 5 depth-to-color copy", 32))
	{
		cemuLog_log(LogType::Force, "D3D12: special state 5 depth-to-color copy size={}x{} color=0x{:08x} depth=0x{:08x}.",
					vpWidth,
					vpHeight,
					colorBuffer->baseTexture->physAddress,
					depthBuffer->baseTexture->physAddress);
	}

	surfaceCopy_copySurfaceWithFormatConversion(
		depthBuffer->baseTexture, depthBuffer->firstMip, depthBuffer->firstSlice,
		colorBuffer->baseTexture, colorBuffer->firstMip, colorBuffer->firstSlice,
		vpWidth, vpHeight);
}

