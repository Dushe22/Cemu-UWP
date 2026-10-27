// Part of D3D12Renderer.cpp (included there, same translation unit): texture/FBO/shader binding traces and lineage logs (diagnostic builds).
// Not a standalone file: it relies on the includes and file-local helpers of D3D12Renderer.cpp.

void D3D12Renderer::LogTextureStats(std::string_view label, LatteTextureViewD3D12* textureView, bool padView, uint32& counter, uint32 limit)
{
	if (!D3D12ShouldChecksumOutput() || !textureView)
		return;

	if (counter >= limit)
		return;

	auto* texture = textureView->GetBaseImage();
	if (!texture || !texture->GetResource())
		return;

	sint32 width = 0;
	sint32 height = 0;
	texture->GetEffectiveSize(width, height, textureView->firstMip);
	if (width <= 0 || height <= 0)
		return;

	std::unique_ptr<LatteTextureReadbackInfo> readback(CreateHostTextureReadback(textureView));
	if (!readback)
		return;

	readback->StartTransfer();
	readback->ForceFinish();
	const uint8* data = readback->GetData();
	if (!data)
		return;

	DXGI_FORMAT viewFormat = textureView->GetViewFormat();
	if (viewFormat == DXGI_FORMAT_UNKNOWN)
		viewFormat = texture->GetFormatInfo().srvFormat;
	if (viewFormat == DXGI_FORMAT_UNKNOWN)
		viewFormat = texture->GetResourceFormat();

	const uint32 bytesPerPixel = D3D12ChecksumBytesPerPixel(viewFormat);
	const bool compressedStats = texture->GetFormatInfo().isCompressed;
	const size_t readbackBytes = readback->GetImageSize();
	const size_t logicalByteCount = (size_t)width * (size_t)height * std::max<uint32>(bytesPerPixel, 1);
	const size_t byteCount = compressedStats ? readbackBytes : std::min<size_t>(logicalByteCount, readbackBytes);
	const size_t inspectedBytes = std::min<size_t>(byteCount, 8ull * 1024ull * 1024ull);
	uint64 byteSum = 0;
	uint64 nonZeroBytes = 0;
	uint64 hash = 1469598103934665603ull;
	for (size_t i = 0; i < inspectedBytes; i++)
	{
		const uint8 value = data[i];
		byteSum += value;
		nonZeroBytes += value != 0;
		hash ^= value;
		hash *= 1099511628211ull;
	}

	uint64 nonZeroRgbPixels = 0;
	if (!compressedStats && bytesPerPixel >= 4)
	{
		const size_t pixelCount = std::min<size_t>((size_t)width * (size_t)height, inspectedBytes / bytesPerPixel);
		for (size_t i = 0; i < pixelCount; i++)
		{
			const uint8* pixel = data + i * bytesPerPixel;
			nonZeroRgbPixels += (pixel[0] | pixel[1] | pixel[2]) != 0;
		}
	}

	counter++;
	cemuLog_log(LogType::Force,
				"D3D12: {} stats #{} {} addr=0x{:08x} fmt=0x{:04x}/dxgi={} size={}x{} bpp={} compressed={} readbackBytes={} logicalBytes={} inspected={} nonZeroBytes={} nonZeroRgbPixels={} sum={} hash=0x{:016x}.",
				label,
				counter,
				padView ? "pad" : "TV",
				texture->physAddress,
				(uint32)texture->format,
				(uint32)viewFormat,
				width,
				height,
				bytesPerPixel,
				compressedStats ? 1 : 0,
				(unsigned long long)readbackBytes,
				(unsigned long long)logicalByteCount,
				(unsigned long long)inspectedBytes,
				(unsigned long long)nonZeroBytes,
				(unsigned long long)nonZeroRgbPixels,
				(unsigned long long)byteSum,
				(unsigned long long)hash);
}

void D3D12Renderer::LogOutputTextureStats(LatteTextureViewD3D12* textureView, bool padView)
{
	static uint32 s_outputChecksumLogCount = 0;
	LogTextureStats("output source", textureView, padView, s_outputChecksumLogCount, 16);
}

void D3D12Renderer::LogActiveRenderTargetStats()
{
	if (!D3D12ShouldChecksumOutput() || !m_activeFBO)
		return;
	static uint32 s_activeRenderTargetChecksumLogCount = 0;
	const auto& colorAttachments = m_activeFBO->GetColorAttachments();
	for (uint32 i = 0; i < colorAttachments.size(); i++)
	{
		if (!colorAttachments[i].valid || !colorAttachments[i].view)
			continue;
		LogTextureStats(fmt::format("active RT{}", i), colorAttachments[i].view, false, s_activeRenderTargetChecksumLogCount, 24);
		return;
	}
}

void D3D12Renderer::TraceTextureSnapshot(std::string_view eventName, std::string_view phase, LatteTextureViewD3D12* textureView, bool padView, std::string_view extra)
{
	if (!D3D12ShouldTrace())
		return;
	auto& traceSink = D3D12TraceSink::Get();
	if (traceSink.PollGpuCaptureArm())
		m_traceTextureStatsCache.clear();

	auto* texture = textureView ? textureView->GetBaseImage() : nullptr;
	if (!texture || !texture->GetResource())
	{
		traceSink.Write(eventName, fmt::format("phase={} pad={} textureView={} texture=null captureGeneration={} {}", phase, padView ? 1 : 0, (const void*)textureView, (unsigned long long)traceSink.GpuCaptureGeneration(), extra));
		return;
	}

	sint32 width = 0;
	sint32 height = 0;
	texture->GetEffectiveSize(width, height, textureView->firstMip);
	DXGI_FORMAT viewFormat = textureView->GetViewFormat();
	if (viewFormat == DXGI_FORMAT_UNKNOWN)
		viewFormat = texture->GetFormatInfo().srvFormat;
	if (viewFormat == DXGI_FORMAT_UNKNOWN)
		viewFormat = texture->GetResourceFormat();

	const uint32 subresource = texture->GetSubresourceIndex(texture->Is3DTexture() ? 0 : textureView->firstSlice, textureView->firstMip);
	const auto textureState = texture->GetSubresourceState(subresource);
	const auto& srv = textureView->GetSRV();
	const auto& rtv = textureView->GetRTV();
	const auto& dsv = textureView->GetDSV();
	const std::string lineageExtra = BuildTextureLineageExtra(texture->physAddress);
	uint64 lineageSerial = 0;
	if (auto lineageIt = m_textureLineage.find(texture->physAddress); lineageIt != m_textureLineage.end())
		lineageSerial = lineageIt->second.serial;
	const auto basePayload = fmt::format("phase={} pad={} addr=0x{:08x} mipAddr=0x{:08x} fmt=0x{:04x} viewDxgi={} resourceDxgi={} dim={} size={}x{}x{} pitch={} mips={} viewMip={} viewMipCount={} viewSlice={} viewSliceCount={} subresource={} state=0x{:08x} swizzle=0x{:08x} tileMode={} isDepth={} hasStencil={} view={} texture={} resource={} srvValid={} srvIndex={} srvCpu=0x{:016x} srvGpu=0x{:016x} rtvValid={} rtvIndex={} rtvCpu=0x{:016x} dsvValid={} dsvIndex={} dsvCpu=0x{:016x} captureGeneration={} {}",
										 phase,
										 padView ? 1 : 0,
										 texture->physAddress,
										 texture->physMipAddress,
										 (uint32)texture->format,
										 (uint32)viewFormat,
										 (uint32)texture->GetResourceFormat(),
										 (uint32)texture->dim,
										 width,
										 height,
										 texture->depth,
										 texture->pitch,
										 texture->mipLevels,
										 textureView->firstMip,
										 textureView->numMip,
										 textureView->firstSlice,
										 textureView->numSlice,
										 subresource,
										 (uint32)textureState,
										 texture->swizzle,
										 (uint32)texture->tileMode,
										 texture->isDepth ? 1 : 0,
										 texture->hasStencil ? 1 : 0,
										 (const void*)textureView,
										 (const void*)texture,
										 (const void*)texture->GetResource(),
										 srv.IsValid() ? 1 : 0,
										 srv.IsValid() ? srv.index : 0,
										 (unsigned long long)(srv.IsValid() ? srv.cpu.ptr : 0),
										 (unsigned long long)(srv.IsValid() ? srv.gpu.ptr : 0),
										 rtv.IsValid() ? 1 : 0,
										 rtv.IsValid() ? rtv.index : 0,
										 (unsigned long long)(rtv.IsValid() ? rtv.cpu.ptr : 0),
										 dsv.IsValid() ? 1 : 0,
										 dsv.IsValid() ? dsv.index : 0,
										 (unsigned long long)(dsv.IsValid() ? dsv.cpu.ptr : 0),
										 (unsigned long long)traceSink.GpuCaptureGeneration(),
										 fmt::format("{} {}", lineageExtra, extra));
	uint64 traceStatsKey = lineageSerial ? 1469598103934665603ull : 0;
	if (traceStatsKey != 0)
	{
		const uint64 keyParts[] =
		{
			(uint64)texture->physAddress,
			(uint64)subresource,
			(uint64)(uint32)viewFormat,
			(uint64)(uint32)textureState,
			(uint64)(uint32)width,
			(uint64)(uint32)height,
			D3D12TraceGpuSamplesOnly() ? 1ull : 0ull,
			lineageSerial,
		};
		for (uint64 part : keyParts)
		{
			traceStatsKey ^= part;
			traceStatsKey *= 1099511628211ull;
		}
		if (traceSink.CanReuseGpuStats(eventName, basePayload, texture->physAddress))
		{
			if (auto cachedStats = m_traceTextureStatsCache.find(traceStatsKey); cachedStats != m_traceTextureStatsCache.end())
			{
				traceSink.Write(eventName, basePayload + cachedStats->second);
				return;
			}
		}
	}
	const auto gpuStatsDecision = traceSink.TryBeginGpuStats(eventName, basePayload, texture->physAddress);
	if (!gpuStatsDecision.capture)
	{
		D3D12TraceSink::Get().Write(eventName, basePayload + fmt::format(" stats=metadata_only reason={}", gpuStatsDecision.reason));
		return;
	}

	if (width <= 0 || height <= 0)
	{
		D3D12TraceSink::Get().Write(eventName, basePayload + " stats=invalid_size");
		return;
	}

	const size_t expectedLogicalByteCount = (size_t)width * (size_t)height * std::max<uint32>(D3D12ChecksumBytesPerPixel(viewFormat), 1);
	D3D12TraceSink::Get().WriteAndFlush("gpu_stats_capture_begin",
										basePayload + fmt::format(" requestedEvent={} decision={} expectedLogicalBytes={} traceStatsKey=0x{:016x}",
																 eventName,
																 gpuStatsDecision.reason,
																 (unsigned long long)expectedLogicalByteCount,
																 (unsigned long long)traceStatsKey));

	std::unique_ptr<LatteTextureReadbackInfo> readback(CreateHostTextureReadback(textureView));
	if (!readback)
	{
		D3D12TraceSink::Get().WriteAndFlush("gpu_stats_capture_end",
											basePayload + fmt::format(" requestedEvent={} result=readback_unavailable", eventName));
		D3D12TraceSink::Get().Write(eventName, basePayload + " stats=readback_unavailable");
		return;
	}

	readback->StartTransfer();
	readback->ForceFinish();
	const size_t readbackBytes = readback->GetImageSize();
	D3D12TraceSink::Get().WriteAndFlush("gpu_stats_capture_end",
										basePayload + fmt::format(" requestedEvent={} result=readback_ready readbackBytes={}",
																 eventName,
																 (unsigned long long)readbackBytes));
	const uint8* data = readback->GetData();
	if (!data)
	{
#if defined(CEMU_UWP_D3D12_DIAGNOSTICS)
		const auto* diagnosticReadback = static_cast<LatteTextureReadbackInfoD3D12*>(readback.get());
		D3D12TraceSink::Get().WriteAndFlush("gpu_stats_readback_validation",
			basePayload + fmt::format(" status={} mapHr=0x{:08x} reservationOffset={} reservationSize={} copyBytes={} fence={}",
				diagnosticReadback->GetDiagnosticReadbackStatus(), (uint32)diagnosticReadback->GetDiagnosticMapResult(),
				diagnosticReadback->GetDiagnosticReservationOffset(), diagnosticReadback->GetDiagnosticReservationSize(),
				diagnosticReadback->GetDiagnosticCopySize(), diagnosticReadback->GetDiagnosticFenceValue()));
#endif
		D3D12TraceSink::Get().WriteAndFlush("gpu_stats_capture_end",
											basePayload + fmt::format(" requestedEvent={} result=readback_null", eventName));
		D3D12TraceSink::Get().Write(eventName, basePayload + " stats=readback_null");
		return;
	}

	const uint32 bytesPerPixel = D3D12ChecksumBytesPerPixel(viewFormat);
	const bool compressedStats = texture->GetFormatInfo().isCompressed;
	const size_t logicalByteCount = (size_t)width * (size_t)height * std::max<uint32>(bytesPerPixel, 1);
	const size_t byteCount = compressedStats ? readbackBytes : std::min<size_t>(logicalByteCount, readbackBytes);
	const std::string samplesPayload = D3D12BuildTraceGpuSamplesPayload(data, byteCount, (uint32)width, (uint32)height, bytesPerPixel, viewFormat, compressedStats);
	std::string gridPayload = D3D12BuildTraceGpuGridPayload(data, byteCount, (uint32)width, (uint32)height, bytesPerPixel, compressedStats);
#if defined(CEMU_UWP_D3D12_DIAGNOSTICS)
	const auto* diagnosticReadback = static_cast<LatteTextureReadbackInfoD3D12*>(readback.get());
	const auto& diagnosticFootprint = diagnosticReadback->GetDiagnosticFootprint();
	gridPayload += fmt::format(" readbackValidation={} mapHr=0x{:08x} footprintDxgi={} footprintSize={}x{}x{} footprintRowPitch={} tightRowBytes={} reservationOffset={} reservationSize={} copyBytes={} fence={} typedInterpretation=declared_view{}",
		diagnosticReadback->GetDiagnosticReadbackStatus(), (uint32)diagnosticReadback->GetDiagnosticMapResult(),
		(uint32)diagnosticFootprint.Format, diagnosticFootprint.Width, diagnosticFootprint.Height,
		diagnosticFootprint.Depth, diagnosticFootprint.RowPitch, diagnosticReadback->GetRowSizeInBytes(),
		diagnosticReadback->GetDiagnosticReservationOffset(), diagnosticReadback->GetDiagnosticReservationSize(),
		diagnosticReadback->GetDiagnosticCopySize(), diagnosticReadback->GetDiagnosticFenceValue(),
		texture->isDepth ? " depthReadbackLayout=unverified" : "");
	// the byte-level RGB/alpha heuristics below are meaningless for float/depth surfaces
	const auto typed = D3D12TypedSurfaceStats::Analyze(data, readbackBytes, (uint32)viewFormat,
		width, height, static_cast<LatteTextureReadbackInfoD3D12*>(readback.get())->GetRowSizeInBytes());
	gridPayload += fmt::format(" typed={} typedPixels={}/{} typedCapped={} typedComponents=[",
		D3D12TypedSurfaceStats::StatusName(typed.status), typed.inspectedPixels, typed.expectedPixels, typed.capped);
	for (uint32 channel = 0; channel < typed.componentCount; ++channel)
	{
		const auto& c = typed.components[channel];
		gridPayload += fmt::format("{}:finite={}/nan={}/inf={}/min={:.9g}/max={:.9g};",
			channel, c.finite, c.nan, c.inf, c.minimum, c.maximum);
	}
	gridPayload += fmt::format("] xyFinite={} xyNonFinite={} xyRadiusAbove1={} xyRadiusAbove1_0001={} (XY interpreted directly; normal test meaningful only for normal surfaces)",
		typed.xyFinite, typed.xyNonFinite, typed.xyRadiusSquaredAboveOne, typed.xyRadiusSquaredAboveTolerance);
#endif
	if (D3D12TraceGpuSamplesOnly())
	{
		const std::string statsPayload = fmt::format(" stats=ok cached=0 samplesOnly=1 bpp={} compressed={} readbackBytes={} logicalBytes={} {}{}",
													 bytesPerPixel,
													 compressedStats ? 1 : 0,
													 (unsigned long long)readbackBytes,
													 (unsigned long long)logicalByteCount,
													 samplesPayload,
													 gridPayload);
		if (traceStatsKey != 0)
		{
			if (m_traceTextureStatsCache.size() > 16384)
				m_traceTextureStatsCache.clear();
			std::string cachedPayload = statsPayload;
			const size_t cachedFlag = cachedPayload.find("cached=0");
			if (cachedFlag != std::string::npos)
				cachedPayload.replace(cachedFlag, std::strlen("cached=0"), "cached=1");
			m_traceTextureStatsCache.emplace(traceStatsKey, std::move(cachedPayload));
		}
		D3D12TraceSink::Get().Write(eventName, basePayload + statsPayload);
		return;
	}
	const size_t inspectedBytes = std::min<size_t>(byteCount, 64ull * 1024ull * 1024ull);
	if (inspectedBytes == 0)
	{
		D3D12TraceSink::Get().WriteAndFlush("gpu_stats_capture_end",
											basePayload + fmt::format(" requestedEvent={} result=empty_readback readbackBytes={} logicalBytes={}",
																	 eventName,
																	 (unsigned long long)readbackBytes,
																	 (unsigned long long)logicalByteCount));
		D3D12TraceSink::Get().Write(eventName,
									basePayload + fmt::format(" stats=empty_readback bpp={} compressed={} readbackBytes={} logicalBytes={}",
															  bytesPerPixel,
															  compressedStats ? 1 : 0,
															  (unsigned long long)readbackBytes,
															  (unsigned long long)logicalByteCount));
		return;
	}
	uint64 byteSum = 0;
	uint64 nonZeroBytes = 0;
	uint64 hash = 1469598103934665603ull;
	uint8 minByte = 255;
	uint8 maxByte = 0;
	for (size_t i = 0; i < inspectedBytes; i++)
	{
		const uint8 value = data[i];
		byteSum += value;
		nonZeroBytes += value != 0;
		minByte = std::min<uint8>(minByte, value);
		maxByte = std::max<uint8>(maxByte, value);
		hash ^= value;
		hash *= 1099511628211ull;
	}

	uint64 nonZeroRgbPixels = 0;
	uint64 solidAlphaPixels = 0;
	if (!compressedStats && bytesPerPixel >= 4)
	{
		const size_t pixelCount = std::min<size_t>((size_t)width * (size_t)height, inspectedBytes / bytesPerPixel);
		for (size_t i = 0; i < pixelCount; i++)
		{
			const uint8* pixel = data + i * bytesPerPixel;
			nonZeroRgbPixels += (pixel[0] | pixel[1] | pixel[2]) != 0;
			solidAlphaPixels += pixel[3] == 255;
		}
	}

	const std::string statsPayload = fmt::format(" stats=ok cached=0 samplesOnly=0 bpp={} compressed={} readbackBytes={} logicalBytes={} inspected={} nonZeroBytes={} nonZeroRgbPixels={} solidAlphaPixels={} sum={} min={} max={} hash=0x{:016x} {}{}",
												 bytesPerPixel,
												 compressedStats ? 1 : 0,
												 (unsigned long long)readbackBytes,
												 (unsigned long long)logicalByteCount,
												 (unsigned long long)inspectedBytes,
												 (unsigned long long)nonZeroBytes,
												 (unsigned long long)nonZeroRgbPixels,
												 (unsigned long long)solidAlphaPixels,
												 (unsigned long long)byteSum,
												 (uint32)minByte,
												 (uint32)maxByte,
												 (unsigned long long)hash,
												 samplesPayload,
												 gridPayload);
	if (traceStatsKey != 0)
	{
		if (m_traceTextureStatsCache.size() > 16384)
			m_traceTextureStatsCache.clear();
		std::string cachedPayload = statsPayload;
		const size_t cachedFlag = cachedPayload.find("cached=0");
		if (cachedFlag != std::string::npos)
			cachedPayload.replace(cachedFlag, std::strlen("cached=0"), "cached=1");
		m_traceTextureStatsCache.emplace(traceStatsKey, std::move(cachedPayload));
	}
	D3D12TraceSink::Get().Write(eventName,
								basePayload + statsPayload);
}

void D3D12Renderer::TraceFboAttachments(std::string_view eventName, std::string_view phase, uint64 drawId, uint64 pipelineHash, std::string_view extra)
{
	if (!D3D12ShouldTrace())
		return;
	if (!m_activeFBO)
	{
		D3D12TraceSink::Get().Write(eventName, fmt::format("phase={} draw={} pipeline=0x{:016x} fbo=null {}", phase, (unsigned long long)drawId, (unsigned long long)pipelineHash, extra));
		return;
	}
	const auto& colorAttachments = m_activeFBO->GetColorAttachments();
	for (uint32 i = 0; i < colorAttachments.size(); i++)
	{
		const auto& attachment = colorAttachments[i];
		const auto slotExtra = fmt::format("draw={} pipeline=0x{:016x} fbo=0x{:016x} fboSize={}x{} attachmentKind=color attachmentIndex={} activeColorCount={} activeColorMask=0x{:02x} drawBuffersMask=0x{:08x} attachmentValid={} attachmentDxgi={} rtvCpu=0x{:016x} sparse={} {}",
										   (unsigned long long)drawId,
										   (unsigned long long)pipelineHash,
										   m_activeFBO->key,
										   m_activeFBO->m_size.x,
										   m_activeFBO->m_size.y,
										   i,
										   m_activeFBO->GetColorAttachmentCount(),
										   m_activeFBO->GetColorAttachmentMask(),
										   m_activeFBO->drawBuffersMask,
										   attachment.valid ? 1 : 0,
										   (uint32)attachment.format,
										   (unsigned long long)attachment.rtv.ptr,
										   m_activeFBO->HasSparseColorAttachments() ? 1 : 0,
										   extra);
		if (!attachment.valid || !attachment.view)
		{
			D3D12TraceSink::Get().Write(eventName, fmt::format("phase={} {} texture=null", phase, slotExtra));
			continue;
		}
		TraceTextureSnapshot(eventName, phase, attachment.view, false, slotExtra);
	}
	if (m_activeFBO->HasDepthStencilAttachment() && m_activeFBO->depthBuffer.texture)
	{
		TraceTextureSnapshot(eventName,
							 phase,
							 static_cast<LatteTextureViewD3D12*>(m_activeFBO->depthBuffer.texture),
							 false,
							 fmt::format("draw={} pipeline=0x{:016x} fbo=0x{:016x} fboSize={}x{} attachmentKind=depth attachmentIndex=depth activeColorCount={} activeColorMask=0x{:02x} drawBuffersMask=0x{:08x} depthDxgi={} stencilDxgi={} dsvCpu=0x{:016x} hasStencil={} {}",
										 (unsigned long long)drawId,
										 (unsigned long long)pipelineHash,
										 m_activeFBO->key,
										 m_activeFBO->m_size.x,
										 m_activeFBO->m_size.y,
										 m_activeFBO->GetColorAttachmentCount(),
										 m_activeFBO->GetColorAttachmentMask(),
										 m_activeFBO->drawBuffersMask,
										 (uint32)m_activeFBO->GetDepthFormat(),
										 (uint32)m_activeFBO->GetStencilFormat(),
										 (unsigned long long)m_activeFBO->GetDsvHandle().ptr,
										 m_activeFBO->depthBuffer.hasStencil ? 1 : 0,
										 extra));
	}
	else
	{
		D3D12TraceSink::Get().Write(eventName,
									fmt::format("phase={} draw={} pipeline=0x{:016x} fbo=0x{:016x} fboSize={}x{} attachmentKind=depth attachmentIndex=depth attachmentValid=0 texture=null hasDepthAttachment=0 {}",
												phase,
												(unsigned long long)drawId,
												(unsigned long long)pipelineHash,
												m_activeFBO->key,
												m_activeFBO->m_size.x,
												m_activeFBO->m_size.y,
												extra));
	}
}

void D3D12Renderer::TraceShaderTextureBindings(uint64 drawId, uint64 pipelineHash, LatteDecompilerShader* vertexShader, LatteDecompilerShader* geometryShader, LatteDecompilerShader* pixelShader)
{
	if (!D3D12ShouldTrace())
		return;

	auto traceStage = [this, drawId, pipelineHash](const char* stageName, uint32 hostTextureUnitBase, const _LatteRegisterSetTextureUnit* texRegs, LatteDecompilerShader* shader)
	{
		if (!shader)
		{
			D3D12TraceSink::Get().Write("draw_texture_binding", fmt::format("draw={} pipeline=0x{:016x} stage={} shader=null", (unsigned long long)drawId, (unsigned long long)pipelineHash, stageName));
			return;
		}

		for (uint32 relativeUnit = 0; relativeUnit < LATTE_NUM_MAX_TEX_UNITS; ++relativeUnit)
		{
			const sint8 bindingPoint = shader->resourceMapping.textureUnitToBindingPoint[relativeUnit];
			if (bindingPoint < 0)
				continue;

			uint32 gpuSamplerSwizzle = 0x06880000;
			uint32 texWord0 = 0;
			uint32 texWord1 = 0;
			uint32 texWord4 = 0;
			if (texRegs)
			{
				const auto& texReg = texRegs[relativeUnit];
				texWord0 = texReg.word0.getRawValue();
				texWord1 = texReg.word1.getRawValue();
				texWord4 = texReg.word4.getRawValue();
				gpuSamplerSwizzle = ((uint32)texReg.word4.get_DST_SEL_X() << 16) |
									((uint32)texReg.word4.get_DST_SEL_Y() << 19) |
									((uint32)texReg.word4.get_DST_SEL_Z() << 22) |
									((uint32)texReg.word4.get_DST_SEL_W() << 25);
			}

			const uint32 hostTextureUnit = hostTextureUnitBase + relativeUnit;
			auto* textureView = hostTextureUnit < m_boundTextures.size() ? m_boundTextures[hostTextureUnit] : nullptr;
			auto* texture = textureView ? textureView->GetBaseImage() : nullptr;
			D3D12DescriptorManager::Allocation samplerSrv{};
			if (textureView)
				samplerSrv = textureView->GetSamplerSRV(gpuSamplerSwizzle);
			const uint32 samplerAssignment = shader->textureUnitSamplerAssignment[relativeUnit];
			uint32 samplerWord0 = 0;
			uint32 samplerWord1 = 0;
			uint32 samplerWord2 = 0;
			uint32 samplerMinFilter = 0;
			uint32 samplerMagFilter = 0;
			uint32 samplerMipFilter = 0;
			uint32 samplerMaxAniso = 0;
			uint32 samplerMinLod = 0;
			uint32 samplerMaxLod = 0;
			sint32 samplerLodBias = 0;
			uint32 samplerClampX = 0;
			uint32 samplerClampY = 0;
			uint32 samplerClampZ = 0;
			uint32 samplerBorderType = 0;
			if (samplerAssignment != LATTE_DECOMPILER_SAMPLER_NONE)
			{
				const uint32 samplerIndex = samplerAssignment + LatteDecompiler_getTextureSamplerBaseIndex(shader->shaderType);
				const _LatteRegisterSetSampler* samplerWords = LatteGPUState.contextNew.SQ_TEX_SAMPLER + samplerIndex;
				samplerWord0 = samplerWords->WORD0.getRawValue();
				samplerWord1 = samplerWords->WORD1.getRawValue();
				samplerWord2 = samplerWords->WORD2.getRawValue();
				samplerMinFilter = (uint32)samplerWords->WORD0.get_XY_MIN_FILTER();
				samplerMagFilter = (uint32)samplerWords->WORD0.get_XY_MAG_FILTER();
				samplerMipFilter = (uint32)samplerWords->WORD0.get_MIP_FILTER();
				samplerMaxAniso = samplerWords->WORD0.get_MAX_ANISO_RATIO();
				samplerMinLod = samplerWords->WORD1.get_MIN_LOD();
				samplerMaxLod = samplerWords->WORD1.get_MAX_LOD();
				samplerLodBias = samplerWords->WORD1.get_LOD_BIAS();
				samplerClampX = (uint32)samplerWords->WORD0.get_CLAMP_X();
				samplerClampY = (uint32)samplerWords->WORD0.get_CLAMP_Y();
				samplerClampZ = (uint32)samplerWords->WORD0.get_CLAMP_Z();
				samplerBorderType = (uint32)samplerWords->WORD0.get_BORDER_COLOR_TYPE();
			}
			std::string extra = fmt::format("draw={} pipeline=0x{:016x} stage={} shader=0x{:016x} aux=0x{:016x} relativeUnit={} hostUnit={} bindingPoint={} samplerAssignment={} depthCompare={} swizzle=0x{:08x} texWord0=0x{:08x} texWord1=0x{:08x} texWord4=0x{:08x} samplerWord0=0x{:08x} samplerWord1=0x{:08x} samplerWord2=0x{:08x} samplerMinFilter={} samplerMagFilter={} samplerMipFilter={} samplerMaxAniso={} samplerMinLod={} samplerMaxLod={} samplerLodBias={} samplerClamp={},{},{} samplerBorderType={} samplerSrvValid={} samplerSrvIndex={} samplerSrvCpu=0x{:016x} samplerSrvGpu=0x{:016x}",
										   (unsigned long long)drawId,
										   (unsigned long long)pipelineHash,
										   stageName,
										   shader->baseHash,
										   shader->auxHash,
										   relativeUnit,
										   hostTextureUnit,
										   (sint32)bindingPoint,
										   samplerAssignment,
										   shader->textureUsesDepthCompare[relativeUnit] ? 1 : 0,
										   gpuSamplerSwizzle,
										   texWord0,
										   texWord1,
										   texWord4,
										   samplerWord0,
										   samplerWord1,
										   samplerWord2,
										   samplerMinFilter,
										   samplerMagFilter,
										   samplerMipFilter,
										   samplerMaxAniso,
										   samplerMinLod,
										   samplerMaxLod,
										   samplerLodBias,
										   samplerClampX,
										   samplerClampY,
										   samplerClampZ,
										   samplerBorderType,
										   samplerSrv.IsValid() ? 1 : 0,
										   samplerSrv.IsValid() ? samplerSrv.index : 0,
										   (unsigned long long)(samplerSrv.IsValid() ? samplerSrv.cpu.ptr : 0),
										   (unsigned long long)(samplerSrv.IsValid() ? samplerSrv.gpu.ptr : 0));
			if (!texture || !textureView)
			{
				D3D12TraceSink::Get().Write("draw_texture_binding", extra + " texture=null");
				continue;
			}
			TraceTextureSnapshot("draw_texture_binding", "before_draw_descriptor_table", textureView, false, extra);
		}
	};

	traceStage("VS", LATTE_CEMU_VS_TEX_UNIT_BASE, LatteGPUState.contextNew.SQ_TEX_START_VS, vertexShader);
	traceStage("PS", LATTE_CEMU_PS_TEX_UNIT_BASE, LatteGPUState.contextNew.SQ_TEX_START_PS, pixelShader);
	traceStage("GS", LATTE_CEMU_GS_TEX_UNIT_BASE, LatteGPUState.contextNew.SQ_TEX_START_GS, geometryShader);
}

void D3D12Renderer::TraceVertexFetchBindings(uint64 drawId, uint64 pipelineHash, LatteFetchShader* fetchShader)
{
	if (!D3D12ShouldTrace())
		return;
	if (!fetchShader)
	{
		D3D12TraceSink::Get().Write("draw_vertex_fetch", fmt::format("draw={} pipeline=0x{:016x} fetchShader=null", (unsigned long long)drawId, (unsigned long long)pipelineHash));
		return;
	}

	D3D12TraceEvent("draw_vertex_fetch_summary", "draw={} pipeline=0x{:016x} fetchShader={} key=0x{:016x} cacheHash=0x{:016x} groups={} invalidGroups={} mainResource={} mainState=0x{:08x} shadowBytes={}",
					(unsigned long long)drawId,
					(unsigned long long)pipelineHash,
					(const void*)fetchShader,
					fetchShader->key,
					fetchShader->m_cacheHash,
					fetchShader->bufferGroups.size(),
					fetchShader->bufferGroupsInvalid.size(),
					(const void*)m_bufferCache.resource.Get(),
					(uint32)m_bufferCache.state,
					(unsigned long long)m_bufferCacheShadow.size());

	for (const auto& group : fetchShader->bufferGroups)
	{
		const uint32 stride = group.getCurrentBufferStride(LatteGPUState.contextRegister);
		const auto binding = group.attributeBufferIndex < m_vertexBindings.size() ? m_vertexBindings[group.attributeBufferIndex] : BoundRange{};
		D3D12TraceEvent("draw_vertex_buffer_group", "draw={} pipeline=0x{:016x} fetchShader={} bufferIndex={} validBinding={} bindOffset={} bindSize={} stride={} minOffset={} maxOffset={} attrCount={} vboStride={} vtxIndexAccess={} instanceIndexAccess={} mainResource={}",
						(unsigned long long)drawId,
						(unsigned long long)pipelineHash,
						(const void*)fetchShader,
						group.attributeBufferIndex,
						binding.valid ? 1 : 0,
						binding.offset,
						binding.size,
						stride,
						group.minOffset,
						group.totalAttribRangeSize,
						group.attribCount,
						group.getCurrentBufferStride(LatteGPUState.contextRegister),
						(group.hasVtxIndexAccess ? 1 : 0),
						group.hasInstanceIndexAccess ? 1 : 0,
						(const void*)m_bufferCache.resource.Get());
		for (sint32 attrIndex = 0; attrIndex < group.attribCount; ++attrIndex)
		{
			const auto& attr = group.attrib[attrIndex];
			D3D12TraceEvent("draw_vertex_attribute", "draw={} pipeline=0x{:016x} fetchShader={} bufferIndex={} attrIndex={} semantic={} fmt=0x{:02x} fetchType={} nfa={} signed={} endian={} divisor={} offset={} ds={},{},{},{} bindOffset={} bindSize={} stride={} byteRange={}..{}",
							(unsigned long long)drawId,
							(unsigned long long)pipelineHash,
							(const void*)fetchShader,
							group.attributeBufferIndex,
							attrIndex,
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
							attr.ds[3],
							binding.offset,
							binding.size,
							stride,
							binding.offset + attr.offset,
							binding.offset + group.totalAttribRangeSize);
		}
	}

	for (const auto& group : fetchShader->bufferGroupsInvalid)
	{
		D3D12TraceEvent("draw_vertex_buffer_group_invalid", "draw={} pipeline=0x{:016x} fetchShader={} bufferIndex={} attrCount={} minOffset={} maxOffset={} vboStride={} vtxIndexAccess={} instanceIndexAccess={}",
						(unsigned long long)drawId,
						(unsigned long long)pipelineHash,
						(const void*)fetchShader,
						group.attributeBufferIndex,
						group.attribCount,
						group.minOffset,
						group.totalAttribRangeSize,
						0u, // invalid buffer index: no stride register
						(group.hasVtxIndexAccess ? 1 : 0),
						group.hasInstanceIndexAccess ? 1 : 0);
	}
}

std::string D3D12Renderer::BuildShaderTextureSourceSummary(LatteDecompilerShader* vertexShader, LatteDecompilerShader* geometryShader, LatteDecompilerShader* pixelShader) const
{
	std::string summary;
	auto appendStage = [this, &summary](const char* stageName, uint32 hostTextureUnitBase, LatteDecompilerShader* shader)
	{
		if (!shader)
			return;
		for (uint32 relativeUnit = 0; relativeUnit < LATTE_NUM_MAX_TEX_UNITS; ++relativeUnit)
		{
			const sint8 bindingPoint = shader->resourceMapping.textureUnitToBindingPoint[relativeUnit];
			if (bindingPoint < 0)
				continue;
			const uint32 hostTextureUnit = hostTextureUnitBase + relativeUnit;
			auto* textureView = hostTextureUnit < m_boundTextures.size() ? m_boundTextures[hostTextureUnit] : nullptr;
			auto* texture = textureView ? textureView->GetBaseImage() : nullptr;
			if (!texture)
				continue;
			if (!summary.empty())
				summary += ",";
			summary += fmt::format("{}{}=0x{:08x}/fmt=0x{:04x}/{}x{}/bp{}",
								   stageName,
								   relativeUnit,
								   texture->physAddress,
								   (uint32)texture->format,
								   texture->width,
								   texture->height,
								   (sint32)bindingPoint);
		}
	};

	appendStage("VS", LATTE_CEMU_VS_TEX_UNIT_BASE, vertexShader);
	appendStage("PS", LATTE_CEMU_PS_TEX_UNIT_BASE, pixelShader);
	appendStage("GS", LATTE_CEMU_GS_TEX_UNIT_BASE, geometryShader);
	if (summary.empty())
		return "none";
	return summary;
}

std::string D3D12Renderer::BuildGeometryProbeResourceMetadata(LatteDecompilerShader* vertexShader, LatteDecompilerShader* geometryShader, LatteDecompilerShader* pixelShader) const
{
	// must not prepare textures or create descriptors; only inspect existing state
	if (!m_activeFBO)
		return "probeResources=no_fbo";
	uint32 requestedMask = 0;
	for (uint32 i = 0; i < 8; ++i)
		if (m_activeFBO->colorBuffer[i].texture)
			requestedMask |= 1u << i;
	const uint32 targetMask = GetD3D12EffectiveRenderTargetMask(LatteGPUState.contextNew);
	const uint32 outputMask = pixelShader ? pixelShader->pixelColorOutputMask : 0;
	const auto* depthView = static_cast<const LatteTextureViewD3D12*>(m_activeFBO->depthBuffer.texture);
	const auto* depthTexture = depthView ? depthView->GetBaseImage() : nullptr;
	std::string metadata = fmt::format(
		"title=0x{:016x} fbo={}x{} requestedRT=0x{:02x} validRT=0x{:02x} rtvCount={} dsvFormat={} stencilFormat={} "
		"depthRequested={} depthValid={} depthResource={} depthView=0x{:04x} rtSlots=[",
		GetUwpTitleId(), m_activeFBO->m_size.x, m_activeFBO->m_size.y, requestedMask,
		m_activeFBO->GetColorAttachmentMask(), m_activeFBO->GetColorAttachmentCount(),
		(uint32)m_activeFBO->GetDepthFormat(), (uint32)m_activeFBO->GetStencilFormat(),
		depthView != nullptr, m_activeFBO->HasDepthStencilAttachment(),
		depthTexture ? (uint32)depthTexture->GetDesc().Format : 0, depthView ? (uint32)depthView->format : 0);
	for (uint32 i = 0; i < 8; ++i)
	{
		if ((requestedMask & (1u << i)) == 0)
			continue;
		const auto* view = static_cast<const LatteTextureViewD3D12*>(m_activeFBO->colorBuffer[i].texture);
		const auto* texture = view->GetBaseImage();
		const auto& attachment = m_activeFBO->GetColorAttachments()[i];
		const auto desc = texture ? texture->GetDesc() : D3D12_RESOURCE_DESC{};
		const uint32 writeMask = attachment.valid && (outputMask & (1u << i)) != 0 ? (targetMask >> (i * 4)) & 15u : 0u;
		metadata += fmt::format(
			"{}:addr=0x{:08x}/valid={}/resource={}/rtv={}/view=0x{:04x}/write=0x{:x}/mip={}/slice={}/size={}x{}x{};",
			i, texture ? texture->physAddress : 0, attachment.valid, (uint32)desc.Format,
			(uint32)attachment.format, (uint32)view->format, writeMask, view->firstMip, view->firstSlice,
			desc.Width, desc.Height, desc.DepthOrArraySize);
	}
	metadata += "] cachedSources=[";
	uint32 sourceCount = 0;
	auto appendStage = [&](const char* stage, uint32 hostBase, const _LatteRegisterSetTextureUnit* texRegs, const LatteDecompilerShader* shader)
	{
		if (!shader)
			return;
		for (uint32 unit = 0; unit < LATTE_NUM_MAX_TEX_UNITS; ++unit)
		{
			const sint8 bindingPoint = shader->resourceMapping.textureUnitToBindingPoint[unit];
			if (bindingPoint < 0)
				continue;
			++sourceCount;
			const uint32 hostUnit = hostBase + unit;
			const auto* view = hostUnit < m_boundTextures.size() ? m_boundTextures[hostUnit] : nullptr;
			const auto* texture = view ? view->GetBaseImage() : nullptr;
			const auto* scratch = GetFeedbackScratchCopy(hostUnit);
			const auto& word4 = texRegs[unit].word4;
			const uint32 swizzle = ((uint32)word4.get_DST_SEL_X() << 16) | ((uint32)word4.get_DST_SEL_Y() << 19) |
				((uint32)word4.get_DST_SEL_Z() << 22) | ((uint32)word4.get_DST_SEL_W() << 25);
			if (!view || !texture)
			{
				metadata += fmt::format("{}{}:bp={}/unbound=1/scratch={};", stage, unit, (sint32)bindingPoint, scratch != nullptr);
				continue;
			}
			const auto cached = view->m_samplerSrvs.find(swizzle & 0x0FFF0000);
			const bool cachedValid = cached != view->m_samplerSrvs.end() && cached->second.IsValid();
			const auto desc = texture->GetDesc();
			const auto samplingDesc = view->UsesSamplingProxy() ? view->m_samplingProxy.resource->GetDesc() : desc;
			const auto srvFormat = texture->isDepth ? view->m_viewFormatInfo.depthSrvFormat : view->m_viewFormatInfo.srvFormat;
			metadata += fmt::format(
				"{}{}:bp={}/addr=0x{:08x}/base=0x{:04x}/view=0x{:04x}/res={}/srv={}/dim={},{}/mip={},{}"
				"/slice={},{} /proxy={}/sampleRes={},{} /swz=0x{:08x}/baseSrv={}/swzCached={}/swzValid={}/scratch={}",
				stage, unit, (sint32)bindingPoint, texture->physAddress, (uint32)texture->format, (uint32)view->format,
				(uint32)desc.Format, (uint32)srvFormat, (uint32)desc.Dimension, (uint32)view->dim, view->firstMip, view->numMip,
				view->firstSlice, view->numSlice, view->UsesSamplingProxy(), (uint32)samplingDesc.Format,
				(uint32)samplingDesc.Dimension, swizzle, view->m_srv.IsValid(), cached != view->m_samplerSrvs.end(), cachedValid,
				scratch != nullptr);
			if (scratch)
				metadata += fmt::format("/scratchSrv={},{}", (uint32)scratch->srvDesc.Format, (uint32)scratch->srvDesc.ViewDimension);
			metadata += ";";
		}
	};
	appendStage("VS", LATTE_CEMU_VS_TEX_UNIT_BASE, LatteGPUState.contextNew.SQ_TEX_START_VS, vertexShader);
	appendStage("PS", LATTE_CEMU_PS_TEX_UNIT_BASE, LatteGPUState.contextNew.SQ_TEX_START_PS, pixelShader);
	appendStage("GS", LATTE_CEMU_GS_TEX_UNIT_BASE, LatteGPUState.contextNew.SQ_TEX_START_GS, geometryShader);
	metadata += fmt::format("] sourceCount={} (cached descriptor availability only; not GPU table readback)", sourceCount);
	return metadata;
}

std::string D3D12Renderer::BuildTextureLineageExtra(uint32 physAddress) const
{
	const auto it = m_textureLineage.find(physAddress);
	if (it == m_textureLineage.end())
		return "lineage=unknown";
	const auto& lineage = it->second;
	return fmt::format("lineageSerial={} lineageOp={} lineageDraw={} lineagePipeline=0x{:016x} lineageFbo=0x{:016x} lineageSources={} lineageExtra={}",
					   (unsigned long long)lineage.serial,
					   lineage.operation,
					   (unsigned long long)lineage.drawId,
					   (unsigned long long)lineage.pipelineHash,
					   (unsigned long long)lineage.fboKey,
					   lineage.sourceSummary,
					   lineage.extra);
}

void D3D12Renderer::RecordTextureLineage(std::string_view operation, LatteTextureViewD3D12* textureView, uint64 drawId, uint64 pipelineHash, std::string_view sourceSummary, std::string_view extra)
{
	if (!D3D12ShouldTrace())
		return;
	if (!textureView)
		return;
	auto* texture = textureView->GetBaseImage();
	if (!texture)
		return;

	TextureLineage lineage{};
	lineage.serial = ++m_textureLineageSerial;
	lineage.drawId = drawId;
	lineage.pipelineHash = pipelineHash;
	lineage.fboKey = m_activeFBO ? m_activeFBO->key : 0;
	lineage.operation = std::string(operation);
	lineage.sourceSummary = sourceSummary.empty() ? "none" : std::string(sourceSummary);
	lineage.extra = std::string(extra);
	m_textureLineage[texture->physAddress] = lineage;

	D3D12TraceEvent("texture_lineage_update", "serial={} op={} addr=0x{:08x} fmt=0x{:04x} size={}x{} draw={} pipeline=0x{:016x} fbo=0x{:016x} sources={} extra={}",
					(unsigned long long)lineage.serial,
					lineage.operation,
					texture->physAddress,
					(uint32)texture->format,
					texture->width,
					texture->height,
					(unsigned long long)lineage.drawId,
					(unsigned long long)lineage.pipelineHash,
					(unsigned long long)lineage.fboKey,
					lineage.sourceSummary,
					lineage.extra);
}

void D3D12Renderer::LogVertexBufferPreview(uint64 drawId, uint64 pipelineHash, LatteDecompilerShader* vertexShader, LatteDecompilerShader* pixelShader, Latte::LATTE_VGT_PRIMITIVE_TYPE::E_PRIMITIVE_TYPE primitiveMode, uint32 count, uint32 baseVertex, uint32 baseInstance, uint32 indexMin, uint32 indexMax, Renderer::INDEX_TYPE hostIndexType, uint32 hostIndexCount)
{
	if (!D3D12ShouldTrace() && !D3D12ShouldChecksumOutput())
		return;

	using Primitive = Latte::LATTE_VGT_PRIMITIVE_TYPE::E_PRIMITIVE_TYPE;
	const bool isRelevantCopyOrQuad =
		primitiveMode == Primitive::QUADS ||
		primitiveMode == Primitive::RECTS ||
		(vertexShader && (vertexShader->baseHash == 0xc99c40893a7ac7adull ||
						  vertexShader->baseHash == 0x0ea6cb7fa4004293ull ||
						  vertexShader->baseHash == 0xe2b099d070ad930dull));
	const auto& traceGpuFilter = D3D12TraceGpuFilter();
	const bool focusedDrawMatch =
		D3D12GetTraceGpuStatsMode() == D3D12TraceGpuStatsMode::Focused &&
		(traceGpuFilter.hasDrawMin || traceGpuFilter.hasDrawMax) &&
		(!traceGpuFilter.hasDrawMin || drawId >= traceGpuFilter.drawMin) &&
		(!traceGpuFilter.hasDrawMax || drawId <= traceGpuFilter.drawMax);
	const bool focusedPipelineMatch =
		D3D12GetTraceGpuStatsMode() == D3D12TraceGpuStatsMode::Focused &&
		pipelineHash != 0 &&
		traceGpuFilter.hasPipeline &&
		D3D12TraceGpuPipelineMatches(pipelineHash);
	const bool knownAlphaMaskPass =
		vertexShader && pixelShader &&
		vertexShader->baseHash == 0xb56e79aae93e86a8ull &&
		pixelShader->baseHash == 0x7e1f4fbecda6957full;
	if (!isRelevantCopyOrQuad && !focusedDrawMatch && !focusedPipelineMatch && !knownAlphaMaskPass)
		return;

	auto* fetchShader = LatteSHRC_GetActiveFetchShader();
	if (!fetchShader)
		return;

	if (m_bufferCacheShadow.empty())
	{
		D3D12TraceEvent("draw_vertex_preview_unavailable", "draw={} pipeline=0x{:016x} reason=buffer_cache_shadow_empty primitive={} count={} baseVertex={} baseInstance={} indexMin={} indexMax={} hostIndexType={} hostIndexCount={} vs=0x{:016x} ps=0x{:016x} mainResource={} mainSize={} mainState=0x{:08x}",
						(unsigned long long)drawId,
						(unsigned long long)pipelineHash,
						(uint32)primitiveMode,
						count,
						baseVertex,
						baseInstance,
						indexMin,
						indexMax,
						(uint32)hostIndexType,
						hostIndexCount,
						vertexShader ? vertexShader->baseHash : 0,
						pixelShader ? pixelShader->baseHash : 0,
						(const void*)m_bufferCache.resource.Get(),
						(unsigned long long)m_bufferCache.size,
						(uint32)m_bufferCache.state);
		return;
	}

	static uint32 s_vertexPreviewLogCount = 0;
	const bool writeRuntimeLog = D3D12RuntimeLogCapped(s_vertexPreviewLogCount, "vertex buffer preview", 96);
	if (writeRuntimeLog)
	{
		cemuLog_log(LogType::Force,
					"D3D12: vertex preview draw={} primitive={} count={} baseVertex={} baseInstance={} indexMin={} indexMax={} hostIndexType={} hostIndexCount={} vs={:016x} ps={:016x} shadowBytes={}.",
					(unsigned long long)drawId,
					(uint32)primitiveMode,
					count,
					baseVertex,
					baseInstance,
					indexMin,
					indexMax,
					(uint32)hostIndexType,
					hostIndexCount,
					vertexShader ? vertexShader->baseHash : 0,
					pixelShader ? pixelShader->baseHash : 0,
					(unsigned long long)m_bufferCacheShadow.size());
	}

	for (auto& group : fetchShader->bufferGroups)
	{
		const uint32 stride = group.getCurrentBufferStride(LatteGPUState.contextRegister);
		const auto binding = group.attributeBufferIndex < m_vertexBindings.size() ? m_vertexBindings[group.attributeBufferIndex] : BoundRange{};
		if (!binding.valid || binding.size == 0)
		{
			D3D12TraceEvent("draw_vertex_preview_group", "draw={} pipeline=0x{:016x} primitive={} bufferIndex={} validBinding={} bindOffset={} bindSize={} stride={} attrCount={} skipped=1 reason=missing_binding",
							(unsigned long long)drawId,
							(unsigned long long)pipelineHash,
							(uint32)primitiveMode,
							group.attributeBufferIndex,
							binding.valid ? 1 : 0,
							binding.offset,
							binding.size,
							stride,
							group.attribCount);
			continue;
		}

		D3D12TraceEvent("draw_vertex_preview_group", "draw={} pipeline=0x{:016x} primitive={} count={} baseVertex={} baseInstance={} indexMin={} indexMax={} hostIndexType={} hostIndexCount={} bufferIndex={} bindOffset={} bindSize={} bindEnd={} stride={} lowBits={} groupMinOffset={} groupMaxOffset={} attrCount={} vtxIndexAccess={} instanceIndexAccess={} vs=0x{:016x} ps=0x{:016x}",
						(unsigned long long)drawId,
						(unsigned long long)pipelineHash,
						(uint32)primitiveMode,
						count,
						baseVertex,
						baseInstance,
						indexMin,
						indexMax,
						(uint32)hostIndexType,
						hostIndexCount,
						group.attributeBufferIndex,
						binding.offset,
						binding.size,
						binding.offset + binding.size,
						stride,
						binding.offset & 3,
						group.minOffset,
						group.totalAttribRangeSize,
						group.attribCount,
						(group.hasVtxIndexAccess ? 1 : 0),
						group.hasInstanceIndexAccess ? 1 : 0,
						vertexShader ? vertexShader->baseHash : 0,
						pixelShader ? pixelShader->baseHash : 0);

		for (sint32 attrIndex = 0; attrIndex < group.attribCount; ++attrIndex)
		{
			const auto& attr = group.attrib[attrIndex];
			const uint32 vertexCount = std::min<uint32>(count, 8);
			for (uint32 vertex = 0; vertex < vertexCount; ++vertex)
			{
				uint32 fetchIndex = vertex;
				if (attr.fetchType == LatteConst::VertexFetchType2::INSTANCE_DATA)
				{
					if (attr.aluDivisor > 1)
						fetchIndex = (baseInstance + vertex) / (uint32)attr.aluDivisor;
					else
						fetchIndex = baseInstance + vertex;
				}
				else if (attr.fetchType == LatteConst::VertexFetchType2::NO_INDEX_OFFSET_DATA)
				{
					fetchIndex = 0;
				}
				const uint32 byteOffset = binding.offset + fetchIndex * stride + attr.offset;
				const bool inBounds = (uint64)byteOffset + 16ull <= (uint64)m_bufferCacheShadow.size();
				const bool inBinding = byteOffset >= binding.offset && (uint64)byteOffset + 16ull <= (uint64)binding.offset + binding.size;
				const uint32 raw0 = ReadD3D12ShadowU32(m_bufferCacheShadow, byteOffset + 0, attr.endianSwap);
				const uint32 raw1 = ReadD3D12ShadowU32(m_bufferCacheShadow, byteOffset + 4, attr.endianSwap);
				const uint32 raw2 = ReadD3D12ShadowU32(m_bufferCacheShadow, byteOffset + 8, attr.endianSwap);
				const uint32 raw3 = ReadD3D12ShadowU32(m_bufferCacheShadow, byteOffset + 12, attr.endianSwap);
				D3D12TraceEvent("draw_vertex_preview", "draw={} pipeline=0x{:016x} primitive={} bufferIndex={} attrIndex={} semantic={} vertexSlot={} fetchType={} fetchIndex={} byteOffset={} bindOffset={} bindSize={} inBinding={} inShadow={} stride={} fmt=0x{:02x} nfa={} signed={} endian={} ds={},{},{},{} raw=0x{:08x},0x{:08x},0x{:08x},0x{:08x} float={:.9g},{:.9g},{:.9g},{:.9g}",
								(unsigned long long)drawId,
								(unsigned long long)pipelineHash,
								(uint32)primitiveMode,
								group.attributeBufferIndex,
								attrIndex,
								attr.semanticId,
								vertex,
								D3D12FetchTypeName(attr.fetchType),
								fetchIndex,
								byteOffset,
								binding.offset,
								binding.size,
								inBinding ? 1 : 0,
								inBounds ? 1 : 0,
								stride,
								(uint32)attr.format,
								attr.nfa,
								attr.isSigned,
								D3D12EndianName(attr.endianSwap),
								attr.ds[0],
								attr.ds[1],
								attr.ds[2],
								attr.ds[3],
								raw0,
								raw1,
								raw2,
								raw3,
								std::bit_cast<float>(raw0),
								std::bit_cast<float>(raw1),
								std::bit_cast<float>(raw2),
								std::bit_cast<float>(raw3));
				if (writeRuntimeLog)
				{
					cemuLog_log(LogType::Force,
								"D3D12:   draw={} vb{} attr{} sem={} v{} fetchIndex={} byte={} inBinding={} stride={} fmt=0x{:02x} endian={} raw=0x{:08x},0x{:08x},0x{:08x},0x{:08x} float={:.7g},{:.7g},{:.7g},{:.7g}.",
								(unsigned long long)drawId,
								group.attributeBufferIndex,
								attrIndex,
								attr.semanticId,
								vertex,
								fetchIndex,
								byteOffset,
								inBinding ? 1 : 0,
								stride,
								(uint32)attr.format,
								D3D12EndianName(attr.endianSwap),
								raw0,
								raw1,
								raw2,
								raw3,
								std::bit_cast<float>(raw0),
								std::bit_cast<float>(raw1),
								std::bit_cast<float>(raw2),
								std::bit_cast<float>(raw3));
				}
			}
		}
	}
}

