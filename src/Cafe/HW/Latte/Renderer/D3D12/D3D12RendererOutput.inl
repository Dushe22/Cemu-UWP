// Part of D3D12Renderer.cpp (included there, same translation unit): frame output: swap, backbuffer quad, screenshots, stream capture, cached outputs for the UWP shell.
// Not a standalone file: it relies on the includes and file-local helpers of D3D12Renderer.cpp.

void D3D12Renderer::SwapBuffers(bool swapTV, bool swapDRC)
{
	if (ShouldQueueRecording() && GetUwpUnderlayPath().empty())
	{
		// memory trims touch core textures, so they stay on Latte (exclusive when there is pressure)
		ProcessUwpMemoryPressure();
		// the present runs on the recorder; Latte starts the next frame but never gets more than one frame ahead
		WaitForQueuedSwaps();
		const uint64 swapSequence = ++m_queuedSwaps;
		QueueRecording([=, this]()
		{
			SwapBuffers(swapTV, swapDRC);
			m_recordedSwaps.store(swapSequence, std::memory_order_release);
		}, RecordOpKind::Sync);
		WakePrepareStage(); // the frame is complete: let it reach the recorder now
		return;
	}
	const auto swapStarted = std::chrono::steady_clock::now();
	D3D12_EXCLUSIVE_RECORDING();
	const uint64 drainNs = UwpElapsedNs(swapStarted);
	uint64 submitNs = 0;
	uint64 outputPresentNs = 0;
	FlushPendingDestroys();
	D3D12PipelineStableCache::GetInstance().SaveUsageIfDue();
#if defined(CEMU_UWP_D3D12_DIAGNOSTICS)
	if (m_geometryProbe)
		m_geometryProbe->Poll(m_device.Get(), m_memoryManager.get(), m_fence.Get());
#endif
	static uint32 s_swapBuffersLogCount = 0;
	if (D3D12RuntimeLogCapped(s_swapBuffersLogCount, "SwapBuffers", 32))
	{
		cemuLog_log(LogType::Force, "D3D12: SwapBuffers TV={} DRC={} mainActive={} padActive={} recordedDraws={} gameDraws={}.",
					swapTV, swapDRC, m_mainSwapchain.active, m_padSwapchain.active,
					m_recordedDrawcalls, (unsigned long long)m_gameDrawCallsSubmitted);
	}
	const bool presentUwpComposite = (swapTV || swapDRC) && m_mainSwapchain.active;
	if ((swapTV || swapDRC) && CafeSystem::IsTitleRunning())
		m_gpuWatchdog.NotifyGameFrame();
	if (presentUwpComposite && CafeSystem::IsTitleRunning() && !m_uwpCompositeFrameStarted && !m_uwpCompositeFrameRendered)
		BeginUwpCompositeFrame(m_mainSwapchain);
	if (presentUwpComposite && CafeSystem::IsTitleRunning() && !m_uwpCompositeFrameRendered)
		DrawUwpCachedVisibleOutputs();
	if (presentUwpComposite)
	{
		for (size_t i = 0; i < m_uwpOutputPendingThisFrame.size(); i++)
		{
			if (m_uwpOutputPendingThisFrame[i] && !m_uwpOutputDrawnThisFrame[i])
				DrawUwpCachedOutput(i != 0);
		}
	}
	{
		const auto submitStarted = std::chrono::steady_clock::now();
		ExecuteCurrentCommandList(SubmitPresent);
		submitNs = UwpElapsedNs(submitStarted);
	}
	m_uwpPresentFences[m_uwpPresentFenceIndex++ % m_uwpPresentFences.size()] = m_uwpLastSubmittedFence;
	// the settings offer every, every second and every third refresh; the latter paces 30 and 20 fps
	// titles evenly instead of collapsing to every refresh
	const UINT syncInterval = (UINT)std::clamp<sint32>(GetConfig().vsync.GetValue(), 0, 3);
	if (presentUwpComposite)
	{
		const uint32 presentedFrame = m_mainSwapchain.frameIndex;
		D3D12TraceEvent("present_begin", "window=uwp-composite frameIndex={} syncInterval={} size={}x{} backBuffer={} state=0x{:08x} rtvCpu=0x{:016x} swapTV={} swapPad={}",
						presentedFrame,
						syncInterval,
						m_mainSwapchain.size.x,
						m_mainSwapchain.size.y,
						(const void*)m_mainSwapchain.backBuffers[presentedFrame].Get(),
						(uint32)m_mainSwapchain.states[presentedFrame],
						(unsigned long long)m_mainSwapchain.rtvHandles[presentedFrame].ptr,
						swapTV ? 1 : 0,
						swapDRC ? 1 : 0);
#if defined(LATTE_UWP_DETAILED_PROFILING)
		const auto presentStarted = std::chrono::steady_clock::now();
#endif
		HRESULT hr = E_FAIL;
		{
			LatteUwpHitchScope hitchScope(LatteUwpHitchOp::Present);
			D3D12GpuWatchdog::ActivityScope watchdogActivity(m_gpuWatchdog, D3D12GpuWatchdog::Activity::Present, presentedFrame);
			const auto outputPresentStarted = std::chrono::steady_clock::now();
			hr = m_mainSwapchain.swapchain->Present(syncInterval, 0);
			outputPresentNs = UwpElapsedNs(outputPresentStarted);
		}
#if defined(LATTE_UWP_DETAILED_PROFILING)
		const uint64 presentElapsedUs = (uint64)std::chrono::duration_cast<std::chrono::microseconds>(
			std::chrono::steady_clock::now() - presentStarted).count();
		++m_uwpPresentCount;
		m_uwpPresentTimeUs += presentElapsedUs;
#endif
		D3D12TraceEvent("present_end", "window=uwp-composite frameIndex={} hr=0x{:08x} syncInterval={} nextFrame={} size={}x{}",
						presentedFrame,
						(uint32)hr,
						syncInterval,
						SUCCEEDED(hr) ? m_mainSwapchain.swapchain->GetCurrentBackBufferIndex() : presentedFrame,
						m_mainSwapchain.size.x,
						m_mainSwapchain.size.y);
		CheckDeviceRemoved(hr, "Present UWP composite");
		if (FAILED(hr))
			throw std::runtime_error("D3D12 UWP composite present failed");
		m_mainSwapchain.frameIndex = m_mainSwapchain.swapchain->GetCurrentBackBufferIndex();
#if defined(CEMU_UWP_D3D12_CORRECTNESS_TELEMETRY)
		++m_correctnessPresentCount;
		if ((m_correctnessPresentCount % 300) == 0)
			LogCorrectnessTelemetry("periodic");
#endif
	}
	if (!t_isRecorderThread)
		ProcessUwpMemoryPressure();
	ReleaseStaleAsyncGamePipelines();
	LogUwpMemoryBreakdown();
	LogDroppedDrawSummary();
#if defined(LATTE_UWP_DETAILED_PROFILING)
	LogUwpCpuPerformanceSummary();
	LogUwpCommandSubmitPerformanceSummary();
#endif
	m_uwpCompositeFrameStarted = false;
	m_uwpCompositeFrameRendered = false;
	m_uwpOutputDrawnThisFrame = {};
	m_uwpOutputPendingThisFrame = {};
	LogUwpOutputCost(UwpElapsedNs(swapStarted), drainNs, submitNs, outputPresentNs);
	return;
	D3D12TraceEvent("swapbuffers_begin", "swapTV={} swapPad={} syncInterval={} tvActive={} tvFrame={} tvSize={}x{} padActive={} padFrame={} padSize={}x{} submittedDraws={}",
					swapTV ? 1 : 0,
					swapDRC ? 1 : 0,
					syncInterval,
					m_mainSwapchain.active ? 1 : 0,
					m_mainSwapchain.frameIndex,
					m_mainSwapchain.size.x,
					m_mainSwapchain.size.y,
					m_padSwapchain.active ? 1 : 0,
					m_padSwapchain.frameIndex,
					m_padSwapchain.size.x,
					m_padSwapchain.size.y,
					(unsigned long long)m_gameDrawCallsSubmitted);
	if (swapTV && m_mainSwapchain.active)
	{
		const uint32 presentedFrame = m_mainSwapchain.frameIndex;
		D3D12TraceEvent("present_begin", "window=tv frameIndex={} syncInterval={} size={}x{} backBuffer={} state=0x{:08x} rtvCpu=0x{:016x}",
						presentedFrame,
						syncInterval,
						m_mainSwapchain.size.x,
						m_mainSwapchain.size.y,
						(const void*)m_mainSwapchain.backBuffers[presentedFrame].Get(),
						(uint32)m_mainSwapchain.states[presentedFrame],
						(unsigned long long)m_mainSwapchain.rtvHandles[presentedFrame].ptr);
		const HRESULT hr = m_mainSwapchain.swapchain->Present(syncInterval, 0);
		D3D12TraceEvent("present_end", "window=tv frameIndex={} hr=0x{:08x} syncInterval={} nextFrame={} size={}x{}",
						presentedFrame,
						(uint32)hr,
						syncInterval,
						SUCCEEDED(hr) ? m_mainSwapchain.swapchain->GetCurrentBackBufferIndex() : presentedFrame,
						m_mainSwapchain.size.x,
						m_mainSwapchain.size.y);
		CheckDeviceRemoved(hr, "Present TV");
		if (FAILED(hr))
			throw std::runtime_error("D3D12 TV present failed");
		m_mainSwapchain.frameIndex = m_mainSwapchain.swapchain->GetCurrentBackBufferIndex();
	}
	if (swapDRC && m_padSwapchain.active)
	{
		const uint32 presentedFrame = m_padSwapchain.frameIndex;
		D3D12TraceEvent("present_begin", "window=pad frameIndex={} syncInterval={} size={}x{} backBuffer={} state=0x{:08x} rtvCpu=0x{:016x}",
						presentedFrame,
						syncInterval,
						m_padSwapchain.size.x,
						m_padSwapchain.size.y,
						(const void*)m_padSwapchain.backBuffers[presentedFrame].Get(),
						(uint32)m_padSwapchain.states[presentedFrame],
						(unsigned long long)m_padSwapchain.rtvHandles[presentedFrame].ptr);
		const HRESULT hr = m_padSwapchain.swapchain->Present(syncInterval, 0);
		D3D12TraceEvent("present_end", "window=pad frameIndex={} hr=0x{:08x} syncInterval={} nextFrame={} size={}x{}",
						presentedFrame,
						(uint32)hr,
						syncInterval,
						SUCCEEDED(hr) ? m_padSwapchain.swapchain->GetCurrentBackBufferIndex() : presentedFrame,
						m_padSwapchain.size.x,
						m_padSwapchain.size.y);
		CheckDeviceRemoved(hr, "Present pad");
		if (FAILED(hr))
			throw std::runtime_error("D3D12 pad present failed");
		m_padSwapchain.frameIndex = m_padSwapchain.swapchain->GetCurrentBackBufferIndex();
	}
}

bool D3D12Renderer::BeginFrame(bool mainWindow)
{
	D3D12_EXCLUSIVE_RECORDING();
	if (!mainWindow)
		return m_mainSwapchain.active && UwpLayoutShowsGamePad();
	return GetSwapchain(mainWindow).active;
}

void D3D12Renderer::HandleScreenshotRequest(LatteTextureView* texView, bool padView)
{
	if (!m_screenshot_requested && m_screenshot_state == ScreenshotState::None)
		return;
	D3D12_EXCLUSIVE_RECORDING();

	if (IsPadWindowActive())
	{
		if (m_screenshot_state == ScreenshotState::Main && padView)
			return;
		if (m_screenshot_state == ScreenshotState::Pad && !padView)
			return;

		if (m_screenshot_state == ScreenshotState::None)
			m_screenshot_state = padView ? ScreenshotState::Main : ScreenshotState::Pad;
		else
			m_screenshot_state = ScreenshotState::None;
	}
	else
	{
		m_screenshot_state = ScreenshotState::None;
	}

	if (!texView || !texView->baseTexture)
		return;
	if (texView->firstMip != 0)
	{
		cemuLog_log(LogType::Force, "Failed to capture D3D12 screenshot: capturing non-zero mip is not supported");
		return;
	}

	auto* textureView = static_cast<LatteTextureViewD3D12*>(texView);
	auto* baseTexture = textureView->GetBaseImage();
	if (!baseTexture || !baseTexture->GetResource())
		return;

	sint32 width = 0;
	sint32 height = 0;
	baseTexture->GetEffectiveSize(width, height, texView->firstMip);
	if (width <= 0 || height <= 0)
		return;

	std::unique_ptr<LatteTextureReadbackInfo> readback(CreateHostTextureReadback(texView));
	if (!readback)
	{
		cemuLog_log(LogType::Force, "D3D12 screenshot request failed: texture readback could not be created");
		return;
	}

	readback->StartTransfer();
	readback->ForceFinish();
	const uint8* pixels = readback->GetData();
	if (!pixels)
	{
		cemuLog_log(LogType::Force, "D3D12 screenshot request failed: texture readback returned no data");
		return;
	}

	DXGI_FORMAT format = textureView->GetViewFormat();
	if (format == DXGI_FORMAT_UNKNOWN)
		format = baseTexture->GetFormatInfo().srvFormat;
	if (format == DXGI_FORMAT_UNKNOWN)
		format = baseTexture->GetResourceFormat();

	const size_t pixelCount = (size_t)width * (size_t)height;
	std::vector<uint8> rgbData;
	rgbData.reserve(pixelCount * 3);

	bool formatValid = true;
	switch (format)
	{
	case DXGI_FORMAT_R8G8B8A8_UNORM:
	case DXGI_FORMAT_R8G8B8A8_SNORM:
		for (size_t i = 0; i < pixelCount; i++)
		{
			const uint8* src = pixels + i * 4;
			rgbData.emplace_back(src[0]);
			rgbData.emplace_back(src[1]);
			rgbData.emplace_back(src[2]);
		}
		break;
	case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
		for (size_t i = 0; i < pixelCount; i++)
		{
			const uint8* src = pixels + i * 4;
			rgbData.emplace_back(SRGBComponentToRGB(src[0]));
			rgbData.emplace_back(SRGBComponentToRGB(src[1]));
			rgbData.emplace_back(SRGBComponentToRGB(src[2]));
		}
		break;
	case DXGI_FORMAT_B8G8R8A8_UNORM:
		for (size_t i = 0; i < pixelCount; i++)
		{
			const uint8* src = pixels + i * 4;
			rgbData.emplace_back(src[2]);
			rgbData.emplace_back(src[1]);
			rgbData.emplace_back(src[0]);
		}
		break;
	case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
		for (size_t i = 0; i < pixelCount; i++)
		{
			const uint8* src = pixels + i * 4;
			rgbData.emplace_back(SRGBComponentToRGB(src[2]));
			rgbData.emplace_back(SRGBComponentToRGB(src[1]));
			rgbData.emplace_back(SRGBComponentToRGB(src[0]));
		}
		break;
	case DXGI_FORMAT_R10G10B10A2_UNORM:
		for (size_t i = 0; i < pixelCount; i++)
		{
			uint32 packed = 0;
			memcpy(&packed, pixels + i * sizeof(uint32), sizeof(uint32));
			rgbData.emplace_back((uint8)((((packed >> 0) & 0x3FFu) * 255u + 511u) / 1023u));
			rgbData.emplace_back((uint8)((((packed >> 10) & 0x3FFu) * 255u + 511u) / 1023u));
			rgbData.emplace_back((uint8)((((packed >> 20) & 0x3FFu) * 255u + 511u) / 1023u));
		}
		break;
	case DXGI_FORMAT_R16G16B16A16_UNORM:
		for (size_t i = 0; i < pixelCount; i++)
		{
			const uint16* src = reinterpret_cast<const uint16*>(pixels + i * 8);
			rgbData.emplace_back((uint8)((src[0] * 255u + 32767u) / 65535u));
			rgbData.emplace_back((uint8)((src[1] * 255u + 32767u) / 65535u));
			rgbData.emplace_back((uint8)((src[2] * 255u + 32767u) / 65535u));
		}
		break;
	default:
		cemuLog_log(LogType::Force, "Unsupported D3D12 screenshot texture format {}", (uint32)format);
		formatValid = false;
		break;
	}

	if (formatValid)
		SaveScreenshot(rgbData, width, height, !padView);
}

void D3D12Renderer::DrawBackbufferQuad(LatteTextureView* texView, RendererOutputShader* shader, bool useLinearTexFilter, sint32 imageX, sint32 imageY, sint32 imageWidth, sint32 imageHeight, bool padView, bool clearBackground)
{
	// the configured underlay is drawn with ImGui, whose context Latte uses meanwhile: then the output stays exclusive
	if (ShouldQueueRecording() && GetUwpUnderlayPath().empty())
	{
		QueueRecording([=, this]() { DrawBackbufferQuad(texView, shader, useLinearTexFilter, imageX, imageY, imageWidth, imageHeight, padView, clearBackground); }, RecordOpKind::Command);
		return;
	}
	D3D12_EXCLUSIVE_RECORDING();
	if (!texView || !shader)
	{
		if (m_uwpDrawingCachedOutput)
			m_uwpCachedOutputs[padView ? 1 : 0].valid = false;
		static uint32 s_missingOutputInputLogCount = 0;
		if (D3D12RuntimeLogCapped(s_missingOutputInputLogCount, "DrawBackbufferQuad missing input", 16))
			cemuLog_log(LogType::Force, "D3D12: DrawBackbufferQuad skipped, texView={} shader={}.", texView != nullptr, shader != nullptr);
		D3D12TraceEvent("output_skip", "reason=missing_input pad={} texView={} shader={} dstXY={},{} dstSize={}x{} linear={} clear={}",
						padView ? 1 : 0,
						(const void*)texView,
						(const void*)shader,
						imageX,
						imageY,
						imageWidth,
						imageHeight,
						useLinearTexFilter ? 1 : 0,
						clearBackground ? 1 : 0);
		return;
	}

	ProcessUwpStreamCapture(texView, padView);

	auto& chain =
		GetSwapchain(true);
	if (!chain.active)
	{
		static uint32 s_inactiveOutputSwapchainLogCount = 0;
		if (D3D12RuntimeLogCapped(s_inactiveOutputSwapchainLogCount, "DrawBackbufferQuad inactive swapchain", 16))
			cemuLog_log(LogType::Force, "D3D12: DrawBackbufferQuad skipped, swapchain inactive for {} view.", padView ? "pad" : "TV");
		D3D12TraceEvent("output_skip", "reason=inactive_swapchain pad={} texView={} shader={} chainSize={}x{} dstXY={},{} dstSize={}x{}",
						padView ? 1 : 0,
						(const void*)texView,
						(const void*)shader,
						chain.size.x,
						chain.size.y,
						imageX,
						imageY,
						imageWidth,
						imageHeight);
		return;
	}

	const auto uwpOutputRect = ResolveUwpOutputRect(padView, chain.size);
	if (uwpOutputRect.visible)
	{
		imageX = uwpOutputRect.x;
		imageY = uwpOutputRect.y;
		imageWidth = uwpOutputRect.w;
		imageHeight = uwpOutputRect.h;
		// a full-screen output covers the background, so skip the clear and underlay draw
		if (!m_uwpCompositeFrameStarted &&
			uwpOutputRect.x <= 0 && uwpOutputRect.y <= 0 &&
			uwpOutputRect.x + uwpOutputRect.w >= chain.size.x &&
			uwpOutputRect.y + uwpOutputRect.h >= chain.size.y &&
			!ResolveUwpOutputRect(!padView, chain.size).visible)
		{
			m_uwpCompositeFrameStarted = true;
		}
		BeginUwpCompositeFrame(chain);
	}

	auto* textureView = static_cast<LatteTextureViewD3D12*>(texView);
	auto* texture = textureView ? textureView->GetBaseImage() : nullptr;
	if (!texture || !texture->GetResource() || !textureView->GetSRV().IsValid())
	{
		if (m_uwpDrawingCachedOutput)
			m_uwpCachedOutputs[padView ? 1 : 0].valid = false;
		static uint32 s_invalidOutputTextureLogCount = 0;
		if (D3D12RuntimeLogCapped(s_invalidOutputTextureLogCount, "DrawBackbufferQuad invalid texture", 32))
		{
			cemuLog_log(LogType::Force, "D3D12: DrawBackbufferQuad skipped, texture={} resource={} srv={}.",
						texture != nullptr,
						texture && texture->GetResource(),
						textureView && textureView->GetSRV().IsValid());
		}
		D3D12TraceEvent("output_skip", "reason=invalid_source pad={} texView={} texture={} resource={} srvValid={} dstXY={},{} dstSize={}x{} chain={}x{}",
						padView ? 1 : 0,
						(const void*)textureView,
						(const void*)texture,
						texture ? (const void*)texture->GetResource() : nullptr,
						textureView && textureView->GetSRV().IsValid() ? 1 : 0,
						imageX,
						imageY,
						imageWidth,
						imageHeight,
						chain.size.x,
						chain.size.y);
		return;
	}

	if (!uwpOutputRect.visible)
	{
		if (!m_uwpDrawingCachedOutput)
			m_uwpCachedOutputs[padView ? 1 : 0].valid = false;
		D3D12TraceEvent("output_skip", "reason=uwp_layout_hidden pad={} layout={} swap={} chainSize={}x{} snapshotRetained=0",
						padView ? 1 : 0,
						ResolveUwpScreenLayout(),
						GetConfig().uwp_display.swap_screens.GetValue() ? 1 : 0,
						chain.size.x,
						chain.size.y);
		return;
	}

	ID3D12PipelineState* pipeline = GetOutputPipeline(shader, padView);
	if (!pipeline)
	{
		if (m_uwpDrawingCachedOutput)
			m_uwpCachedOutputs[padView ? 1 : 0].valid = false;
		static uint32 s_missingOutputPsoLogCount = 0;
		if (D3D12RuntimeLogCapped(s_missingOutputPsoLogCount, "DrawBackbufferQuad missing PSO", 16))
			cemuLog_log(LogType::Force, "D3D12: DrawBackbufferQuad skipped, output PSO unavailable for {} view.", padView ? "pad" : "TV");
		D3D12TraceEvent("output_skip", "reason=no_output_pso pad={} src=0x{:08x} srcFmt=0x{:04x} srcDxgi={} texView={} shader={} dstXY={},{} dstSize={}x{} chain={}x{}",
						padView ? 1 : 0,
						texture->physAddress,
						(uint32)texture->format,
						(uint32)textureView->GetViewFormat(),
						(const void*)textureView,
						(const void*)shader,
						imageX,
						imageY,
						imageWidth,
						imageHeight,
						chain.size.x,
						chain.size.y);
		return;
	}

	if (!m_uwpDrawingCachedOutput)
	{
		const auto peerRect = ResolveUwpOutputRect(!padView, chain.size);
		if (peerRect.visible)
		{
			StoreUwpOutputSnapshot(padView, textureView, texture, shader, useLinearTexFilter);
			// a larger peer is composited underneath, which is only needed once per frame
			if (GetUwpOutputArea(peerRect) > GetUwpOutputArea(uwpOutputRect) && !m_uwpOutputDrawnThisFrame[padView ? 0 : 1])
				DrawUwpCachedOutput(!padView);
		}
		else
		{
			m_uwpCachedOutputs[0].valid = false;
			m_uwpCachedOutputs[1].valid = false;
		}
	}

	static uint32 s_outputDrawLogCount = 0;
	if (D3D12RuntimeLogCapped(s_outputDrawLogCount, "DrawBackbufferQuad", 96))
	{
		cemuLog_log(LogType::Force,
					"D3D12: DrawBackbufferQuad {} src=0x{:08x} fmt=0x{:04x} size={}x{} mip={} slice={} dst=({}, {}) {}x{} chain={}x{} linear={} clear={}.",
					padView ? "pad" : "TV",
					texture->physAddress,
					(uint32)texture->format,
					texture->width,
					texture->height,
					texView->firstMip,
					texView->firstSlice,
					imageX,
					imageY,
					imageWidth,
					imageHeight,
					chain.size.x,
					chain.size.y,
					useLinearTexFilter,
					clearBackground);
	}

	LogOutputTextureStats(textureView, padView);
	if (D3D12ShouldTrace())
	{
		TraceTextureSnapshot("output_source_before", "before_output_blit", textureView, padView,
			fmt::format("window={} dstXY={},{} dstSize={}x{} chain={}x{} frameIndex={} linear={} clear={} shader={} pipeline={}",
				padView ? "pad" : "tv", imageX, imageY, imageWidth, imageHeight, chain.size.x, chain.size.y,
				chain.frameIndex, useLinearTexFilter ? 1 : 0, clearBackground ? 1 : 0,
				(const void*)shader, (const void*)pipeline));
	}

	if (!m_commandListOpen)
		ResetCommandList();

	auto uniforms = shader->FillUniformBlockBuffer(*texView, { imageWidth, imageHeight }, padView);
	auto uniformUpload = m_memoryManager->AllocateUpload(sizeof(uniforms), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT, m_nextFenceValue);
	if (!uniformUpload.IsValid())
		throw std::runtime_error("Failed to allocate D3D12 output uniform upload memory");
	memcpy(uniformUpload.cpuPtr, &uniforms, sizeof(uniforms));

	// a two-pass shader (FSR) upscales into an output-sized target first and then reads that
	D3D12DescriptorManager::Allocation sourceSrv = textureView->GetSRV();
	if (RendererOutputShader* prePass = shader->GetPrePass())
	{
		TransitionTextureView(textureView, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		const auto prePassSrv = DrawOutputPrePass(prePass, sourceSrv, imageWidth, imageHeight, padView,
			uniformUpload.resource->GetGPUVirtualAddress() + uniformUpload.offset);
		if (prePassSrv.IsValid())
			sourceSrv = prePassSrv;
		else if (auto* copyPipeline = GetOutputPipeline(shader == RendererOutputShader::s_fsr_shader_ud ?
			RendererOutputShader::s_copy_shader_ud : RendererOutputShader::s_copy_shader, padView))
			pipeline = copyPipeline;
	}

	const auto outputDescriptorStats = m_descriptorManager->GetStats();
	const bool hasCachedOutputSampler =
		(useLinearTexFilter ? m_shaderVisibleOutputLinearSampler.IsValid() : m_shaderVisibleOutputPointSampler.IsValid()) &&
		m_shaderVisibleOutputSamplerGeneration == outputDescriptorStats.shaderVisibleSamplerRollovers;
	EnsureShaderVisibleDescriptorSpace(1, hasCachedOutputSampler ? 0 : 1);
	auto srv = CopyToShaderVisibleChecked(sourceSrv, "output");
	auto sampler = GetShaderVisibleOutputSampler(useLinearTexFilter);
	BindShaderVisibleHeaps();

	chain.frameIndex = chain.swapchain->GetCurrentBackBufferIndex();
	ID3D12Resource* backBuffer = chain.backBuffers[chain.frameIndex].Get();
	D3D12TraceEvent("output_target_before", "window={} frameIndex={} chainSize={}x{} backBuffer={} state=0x{:08x} rtvCpu=0x{:016x} src=0x{:08x} srcFmt=0x{:04x} srcDxgi={} srvGpu=0x{:016x} samplerGpu=0x{:016x} uniformGpu=0x{:016x} pipeline={}",
					padView ? "pad" : "tv",
					chain.frameIndex,
					chain.size.x,
					chain.size.y,
					(const void*)backBuffer,
					(uint32)chain.states[chain.frameIndex],
					(unsigned long long)chain.rtvHandles[chain.frameIndex].ptr,
					texture->physAddress,
					(uint32)texture->format,
					(uint32)textureView->GetViewFormat(),
					(unsigned long long)srv.gpu.ptr,
					(unsigned long long)sampler.gpu.ptr,
					(unsigned long long)(uniformUpload.resource->GetGPUVirtualAddress() + uniformUpload.offset),
					(const void*)pipeline);
	Transition(backBuffer, chain.states[chain.frameIndex], D3D12_RESOURCE_STATE_RENDER_TARGET);
	TransitionTextureView(textureView, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

	auto* commandList = GetCurrentCommandSlot().commandList.Get();
	SetD3D12PipelineState(pipeline);
	SetD3D12GraphicsRootSignature(m_outputRootSignature.Get());
	commandList->SetGraphicsRootDescriptorTable(0, srv.gpu);
	commandList->SetGraphicsRootDescriptorTable(1, sampler.gpu);
	commandList->SetGraphicsRootConstantBufferView(2, uniformUpload.resource->GetGPUVirtualAddress() + uniformUpload.offset);
	commandList->SetGraphicsRootConstantBufferView(3, uniformUpload.resource->GetGPUVirtualAddress() + uniformUpload.offset);
	SetD3D12RenderTargets(1, &chain.rtvHandles[chain.frameIndex], nullptr);

	D3D12_VIEWPORT viewport{};
	viewport.TopLeftX = (float)imageX;
	viewport.TopLeftY = (float)imageY;
	viewport.Width = (float)std::max<sint32>(imageWidth, 1);
	viewport.Height = (float)std::max<sint32>(imageHeight, 1);
	viewport.MinDepth = 0.0f;
	viewport.MaxDepth = 1.0f;
	D3D12_RECT scissor{};
	scissor.left = 0;
	scissor.top = 0;
	scissor.right = std::max<sint32>(chain.size.x, 1);
	scissor.bottom = std::max<sint32>(chain.size.y, 1);
	SetD3D12Viewport(viewport);
	SetD3D12Scissor(scissor);
	SetD3D12PrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	commandList->DrawInstanced(6, 1, 0, 0);
	m_gpuWatchdog.RecordOperation(D3D12GpuWatchdog::Operation::OutputDraw);
	D3D12TraceEvent("output_draw", "window={} frameIndex={} backBuffer={} rtvCpu=0x{:016x} src=0x{:08x} srcFmt=0x{:04x} srcDxgi={} srcView={} srvGpu=0x{:016x} samplerGpu=0x{:016x} uniformGpu=0x{:016x} vertices=6 dstXY={},{} dstSize={}x{} chain={}x{} viewport={},{}:{}x{} scissor={},{}-{},{} linear={} clear={} shader={} pipeline={}",
					padView ? "pad" : "tv",
					chain.frameIndex,
					(const void*)backBuffer,
					(unsigned long long)chain.rtvHandles[chain.frameIndex].ptr,
					texture->physAddress,
					(uint32)texture->format,
					(uint32)textureView->GetViewFormat(),
					(const void*)textureView,
					(unsigned long long)srv.gpu.ptr,
					(unsigned long long)sampler.gpu.ptr,
					(unsigned long long)(uniformUpload.resource->GetGPUVirtualAddress() + uniformUpload.offset),
					imageX,
					imageY,
					imageWidth,
					imageHeight,
					chain.size.x,
					chain.size.y,
					viewport.TopLeftX,
					viewport.TopLeftY,
					viewport.Width,
					viewport.Height,
					scissor.left,
					scissor.top,
					scissor.right,
					scissor.bottom,
					useLinearTexFilter ? 1 : 0,
					clearBackground ? 1 : 0,
					(const void*)shader,
					(const void*)pipeline);
	Transition(backBuffer, chain.states[chain.frameIndex], D3D12_RESOURCE_STATE_PRESENT);
	m_uwpCompositeFrameRendered = true;
	m_uwpOutputDrawnThisFrame[padView ? 1 : 0] = true;
	if (!m_uwpDrawingCachedOutput)
	{
		const auto peerRect = ResolveUwpOutputRect(!padView, chain.size);
		// a smaller peer stays on top; redraw it only if it is missing or this draw covered it
		if (peerRect.visible && GetUwpOutputArea(peerRect) <= GetUwpOutputArea(uwpOutputRect))
		{
			if (UwpOutputRectsOverlap(peerRect, uwpOutputRect))
				DrawUwpCachedOutput(!padView);
			else if (!m_uwpOutputDrawnThisFrame[padView ? 0 : 1])
				m_uwpOutputPendingThisFrame[padView ? 0 : 1] = true;
		}
	}
}

#if defined(CEMU_UWP)
void D3D12Renderer::ProcessUwpStreamCapture(LatteTextureView* textureView, bool padView)
{
	for (auto& pending : m_uwpStreamReadbacks)
	{
		if (!pending.transfer || !pending.transfer->IsFinished())
			continue;
		uint8* data = pending.transfer->GetData();
		const uint32 size = pending.transfer->GetImageSize();
		if (data && size && pending.width && pending.height && pending.rowBytes)
		{
			std::vector<uint8> pixels = CemuUwp::MediaStreamer::AcquireVideoFrameBuffer(size);
			std::memcpy(pixels.data(), data, size);
			CemuUwp::MediaStreamer::SubmitVideoFrame(pending.padView, pending.width,
				pending.height, pending.rowBytes, (uint32)pending.format, std::move(pixels));
		}
		pending = {};
	}

	auto& capture = m_uwpStreamReadbacks[padView ? 1 : 0];
	if (capture.transfer || !CemuUwp::MediaStreamer::RequestVideoCapture(padView))
		return;
	if (!textureView || !textureView->baseTexture || textureView->firstMip != 0)
		return;
	auto* d3dView = static_cast<LatteTextureViewD3D12*>(textureView);
	auto* texture = d3dView->GetBaseImage();
	if (!texture || !texture->GetResource())
		return;

	sint32 width = 0;
	sint32 height = 0;
	texture->GetEffectiveSize(width, height, textureView->firstMip);
	if (width <= 0 || height <= 0)
		return;
	DXGI_FORMAT format = d3dView->GetViewFormat();
	if (format == DXGI_FORMAT_UNKNOWN)
		format = texture->GetFormatInfo().srvFormat;
	if (format == DXGI_FORMAT_UNKNOWN)
		format = texture->GetResourceFormat();

	auto transfer = std::unique_ptr<LatteTextureReadbackInfoD3D12>(CreateHostTextureReadback(textureView));
	if (!transfer)
		return;
	const uint32 rowBytes = transfer->GetRowSizeInBytes();
	transfer->StartTransfer();
	capture.transfer = std::move(transfer);
	capture.width = (uint32)width;
	capture.height = (uint32)height;
	capture.rowBytes = rowBytes;
	capture.format = format;
	capture.padView = padView;
}
#endif

void D3D12Renderer::DrawUwpConfiguredUnderlay(SwapchainState& chain)
{
	const std::string path = GetUwpUnderlayPath();
	if (path.empty())
	{
		if (m_uwpUnderlayTexture.id)
			DeleteTexture(m_uwpUnderlayTexture.id);
		m_uwpUnderlayTexture = {};
		return;
	}

	if (m_uwpUnderlayTexture.path != path)
	{
		if (m_uwpUnderlayTexture.id)
			DeleteTexture(m_uwpUnderlayTexture.id);
		m_uwpUnderlayTexture = {};
		m_uwpUnderlayTexture.path = path;

		UwpDecodedImage image{};
		if (!DecodeUwpPngFile(_utf8ToPath(path), image))
		{
			m_uwpUnderlayTexture.failed = true;
			cemuLog_log(LogType::Force, "D3D12 UWP: failed to decode underlay PNG {}", path);
			return;
		}

		m_uwpUnderlayTexture.id = GenerateTexture(image.rgba, { image.width, image.height });
		m_uwpUnderlayTexture.size = { image.width, image.height };
		m_uwpUnderlayTexture.failed = m_uwpUnderlayTexture.id == nullptr;
		if (m_uwpUnderlayTexture.failed)
			cemuLog_log(LogType::Force, "D3D12 UWP: failed to upload underlay texture {}", path);
	}

	if (!m_uwpUnderlayTexture.id || m_uwpUnderlayTexture.failed || !m_imguiInitialized)
		return;

	if (!ImguiBegin(true))
		return;
	ImDrawList* draw = ImGui::GetBackgroundDrawList();
	draw->AddImage(m_uwpUnderlayTexture.id, ImVec2(0.0f, 0.0f), ImVec2((float)chain.size.x, (float)chain.size.y));
	ImguiEnd();
}

void D3D12Renderer::ResetUwpOutputSnapshots()
{
	ReleaseUwpCachedOutputs();
	m_uwpCachedOutputTitleId = 0;
	m_uwpCompositeFrameStarted = false;
	m_uwpCompositeFrameRendered = false;
	m_uwpOutputDrawnThisFrame = {};
	m_uwpOutputPendingThisFrame = {};
	m_uwpDrawingCachedOutput = false;
}

void D3D12Renderer::ReleaseUwpCachedOutputs()
{
	for (auto& output : m_uwpCachedOutputs)
	{
		auto persistentSrv = output.srv;
		ReleaseResourceDeferred(output.texture);
		output = {};
		output.srv = persistentSrv;
		output.desc.Format = DXGI_FORMAT_UNKNOWN;
		output.viewFormat = DXGI_FORMAT_UNKNOWN;
	}
}

bool D3D12Renderer::StoreUwpOutputSnapshot(bool padView, LatteTextureViewD3D12* textureView, LatteTextureD3D12* texture, RendererOutputShader* shader, bool useLinearTexFilter)
{
	if (!textureView || !texture || !shader || !m_memoryManager || !m_descriptorManager || !m_device || !texture->GetResource())
		return false;
	const uint64 titleId = GetUwpTitleId();
	if (m_uwpCachedOutputTitleId != titleId)
	{
		ReleaseUwpCachedOutputs();
		m_uwpCachedOutputTitleId = titleId;
		D3D12TraceEvent("uwp_output_snapshots_invalidate", "title=0x{:016x}", titleId);
	}

	DXGI_FORMAT viewFormat = textureView->GetViewFormat();
	if (viewFormat == DXGI_FORMAT_UNKNOWN)
		viewFormat = texture->GetFormatInfo().srvFormat;
	if (viewFormat == DXGI_FORMAT_UNKNOWN)
		viewFormat = texture->GetResourceFormat();
	if (viewFormat == DXGI_FORMAT_UNKNOWN)
		return false;

	const auto& sourceDesc = texture->GetDesc();
	if (sourceDesc.SampleDesc.Count != 1)
	{
		cemuLog_logOnce(LogType::Force, "D3D12 UWP: multisampled output snapshots require an explicit resolve and are not cached.");
		return false;
	}
	const uint32 sourceMipLevels = std::max<uint32>(sourceDesc.MipLevels, 1);
	const uint32 firstMip = std::min<uint32>((uint32)std::max<sint32>(textureView->firstMip, 0), sourceMipLevels - 1);
	const uint32 sourceWidth = std::max<uint32>((uint32)sourceDesc.Width >> firstMip, 1);
	const uint32 sourceHeight = std::max<uint32>(sourceDesc.Height >> firstMip, 1);
	const bool sourceIs3D = sourceDesc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D;
	const uint32 sourceArraySize = sourceIs3D ? 1u : std::max<uint32>(sourceDesc.DepthOrArraySize, 1);
	const uint32 firstSlice = sourceIs3D ? 0u : std::min<uint32>((uint32)std::max<sint32>(textureView->firstSlice, 0), sourceArraySize - 1);
	const uint32 sourceSubresource = texture->GetSubresourceIndex(sourceIs3D ? 0 : firstSlice, firstMip);

	// the output shader samples Texture2D; snapshot only the selected mip/slice
	D3D12_RESOURCE_DESC snapshotDesc{};
	snapshotDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	snapshotDesc.Width = sourceWidth;
	snapshotDesc.Height = sourceHeight;
	snapshotDesc.DepthOrArraySize = 1;
	snapshotDesc.MipLevels = 1;
	snapshotDesc.Format = sourceDesc.Format;
	snapshotDesc.SampleDesc = {1, 0};
	snapshotDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	snapshotDesc.Flags = D3D12_RESOURCE_FLAG_NONE;
	D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = textureView->BuildSrvDesc();
	if (srvDesc.Format == DXGI_FORMAT_UNKNOWN)
		return false;
	srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	srvDesc.Texture2D.MostDetailedMip = 0;
	srvDesc.Texture2D.MipLevels = 1;
	srvDesc.Texture2D.PlaneSlice = 0;
	srvDesc.Texture2D.ResourceMinLODClamp = 0.0f;

	auto& output = m_uwpCachedOutputs[padView ? 1 : 0];
	const bool needsTexture =
		!output.texture.IsValid() ||
		output.desc.Dimension != snapshotDesc.Dimension ||
		output.desc.Width != snapshotDesc.Width ||
		output.desc.Height != snapshotDesc.Height ||
		output.desc.DepthOrArraySize != snapshotDesc.DepthOrArraySize ||
		output.desc.MipLevels != snapshotDesc.MipLevels ||
		output.desc.SampleDesc.Count != snapshotDesc.SampleDesc.Count ||
		output.desc.SampleDesc.Quality != snapshotDesc.SampleDesc.Quality ||
		output.desc.Format != snapshotDesc.Format ||
		output.desc.Flags != snapshotDesc.Flags ||
		output.viewFormat != viewFormat;

	if (needsTexture)
	{
		ReleaseResourceDeferred(output.texture);
		const bool acquiredPooledTexture = TryAcquireScratchTexture(snapshotDesc, output.texture);
		const bool createdTexture = acquiredPooledTexture ||
									CreateTextureWithRecovery(snapshotDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, output.texture, padView ? "uwp pad output snapshot" : "uwp TV output snapshot");
		if (!createdTexture)
		{
			output.valid = false;
			cemuLog_log(LogType::Force,
						"D3D12 UWP: failed to create {} output snapshot {}x{} resourceFormat={} viewFormat={}.",
						padView ? "pad" : "TV",
						(uint32)snapshotDesc.Width,
						(uint32)snapshotDesc.Height,
						(uint32)snapshotDesc.Format,
						(uint32)viewFormat);
			return false;
		}

		if (!output.srv.IsValid())
			output.srv = m_descriptorManager->AllocatePersistent(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

		output.desc = snapshotDesc;
		output.viewFormat = viewFormat;
	}

	if (!output.srv.IsValid())
		output.srv = m_descriptorManager->AllocatePersistent(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	const bool needsSrvUpdate =
		needsTexture ||
		std::memcmp(&output.srvDesc, &srvDesc, sizeof(srvDesc)) != 0;
	if (needsSrvUpdate)
	{
		CreateShaderResourceViewChecked(output.texture.resource.Get(), &srvDesc, output.srv.cpu, "output snapshot", false);
		output.srvDesc = srvDesc;
		m_uwpSnapshotSrvUpdates++;
	}
	else
		m_uwpSnapshotSrvSkips++;

	if (!output.texture.IsValid() || !output.srv.IsValid())
	{
		output.valid = false;
		return false;
	}

	if (!m_commandListOpen)
		ResetCommandList();

	const D3D12_RESOURCE_STATES previousSourceState = texture->GetSubresourceState(sourceSubresource);
	TransitionTextureSubresource(texture, sourceSubresource, D3D12_RESOURCE_STATE_COPY_SOURCE);
	Transition(output.texture.resource.Get(), output.texture.state, D3D12_RESOURCE_STATE_COPY_DEST);

	D3D12_TEXTURE_COPY_LOCATION sourceLocation{};
	sourceLocation.pResource = texture->GetResource();
	sourceLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	sourceLocation.SubresourceIndex = sourceSubresource;
	D3D12_TEXTURE_COPY_LOCATION destinationLocation{};
	destinationLocation.pResource = output.texture.resource.Get();
	destinationLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	destinationLocation.SubresourceIndex = 0;
	if (sourceIs3D)
	{
		const uint32 sourceDepth = std::max<uint32>((uint32)sourceDesc.DepthOrArraySize >> firstMip, 1);
		const uint32 sourceW = std::min<uint32>((uint32)std::max<sint32>(textureView->firstSlice, 0), sourceDepth - 1);
		const D3D12_BOX sourceBox{0, 0, sourceW, sourceWidth, sourceHeight, sourceW + 1};
		GetCurrentCommandSlot().commandList->CopyTextureRegion(&destinationLocation, 0, 0, 0, &sourceLocation, &sourceBox);
	}
	else
		GetCurrentCommandSlot().commandList->CopyTextureRegion(&destinationLocation, 0, 0, 0, &sourceLocation, nullptr);
	Transition(output.texture.resource.Get(), output.texture.state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	TransitionTextureSubresource(texture, sourceSubresource, previousSourceState);

	const auto snapshotUniforms = shader->FillUniformBlockBuffer(
		*textureView, {static_cast<sint32>(sourceWidth), static_cast<sint32>(sourceHeight)}, padView);
	output.shader = shader;
	output.useLinearTexFilter = useLinearTexFilter;
	output.drcColorTransfer = RendererOutputShader::UsesDRCColorTransfer(padView);
	output.sourceResolution = {
		std::max<sint32>(static_cast<sint32>(snapshotUniforms.textureSrcResolution.x), 1),
		std::max<sint32>(static_cast<sint32>(snapshotUniforms.textureSrcResolution.y), 1),
	};
	output.nativeResolution = {
		std::max<sint32>(static_cast<sint32>(snapshotUniforms.nativeResolution.x), 1),
		std::max<sint32>(static_cast<sint32>(snapshotUniforms.nativeResolution.y), 1),
	};
	output.valid = true;
	D3D12TraceEvent("uwp_output_snapshot_store",
					"window={} snapshot={}x{}x{} copiedMips={} copiedSlices={} sampleCount={} sourceRes={}x{} native={}x{} resourceFormat={} viewFormat={} shader={} linear={}",
					padView ? "pad" : "tv",
					(uint32)snapshotDesc.Width,
					(uint32)snapshotDesc.Height,
					(uint32)snapshotDesc.DepthOrArraySize,
					1,
					1,
					(uint32)snapshotDesc.SampleDesc.Count,
					output.sourceResolution.x,
					output.sourceResolution.y,
					output.nativeResolution.x,
					output.nativeResolution.y,
					(uint32)snapshotDesc.Format,
					(uint32)viewFormat,
					(const void*)shader,
					useLinearTexFilter ? 1 : 0);
	return true;
}

void D3D12Renderer::DrawUwpCachedOutput(bool padView)
{
	if (m_uwpDrawingCachedOutput)
		return;

	auto& output = m_uwpCachedOutputs[padView ? 1 : 0];
	if (!output.valid || !output.texture.IsValid() || !output.srv.IsValid() || !output.shader)
		return;

	auto& chain = GetSwapchain(true);
	if (!chain.active || !chain.swapchain)
		return;

	const auto outputRect = ResolveUwpOutputRect(padView, chain.size);
	if (!outputRect.visible)
		return;

	ID3D12PipelineState* pipeline = GetOutputPipeline(output.shader, padView);
	if (!pipeline)
		return;

	chain.frameIndex = chain.swapchain->GetCurrentBackBufferIndex();
	ID3D12Resource* backBuffer = chain.backBuffers[chain.frameIndex].Get();
	if (!backBuffer)
		return;

	const bool previousDrawingCachedOutput = m_uwpDrawingCachedOutput;
	m_uwpDrawingCachedOutput = true;
	try
	{
		BeginUwpCompositeFrame(chain);
		if (!m_commandListOpen)
			ResetCommandList();

		RendererOutputShader::OutputUniformVariables uniforms{};
		uniforms.textureSrcResolution = { (float)std::max<sint32>(output.sourceResolution.x, 1), (float)std::max<sint32>(output.sourceResolution.y, 1) };
		uniforms.nativeResolution = { (float)std::max<sint32>(output.nativeResolution.x, 1), (float)std::max<sint32>(output.nativeResolution.y, 1) };
		uniforms.outputResolution = { (float)std::max<sint32>(outputRect.w, 1), (float)std::max<sint32>(outputRect.h, 1) };
		uniforms.sharpness = RendererOutputShader::GetFsrSharpness(GetConfig().fsr_sharpness.GetValue());
		RendererOutputShader::FillColorTransfer(uniforms, output.drcColorTransfer);

		auto uniformUpload = m_memoryManager->AllocateUpload(sizeof(uniforms), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT, m_nextFenceValue);
		if (!uniformUpload.IsValid())
			throw std::runtime_error("Failed to allocate D3D12 cached output uniform upload memory");
		memcpy(uniformUpload.cpuPtr, &uniforms, sizeof(uniforms));

		// the snapshot is at the source size, so a two-pass shader (FSR) upscales it first like a live frame
		D3D12DescriptorManager::Allocation sourceSrv = output.srv;
		if (RendererOutputShader* prePass = output.shader->GetPrePass())
		{
			Transition(output.texture.resource.Get(), output.texture.state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
			const auto prePassSrv = DrawOutputPrePass(prePass, sourceSrv, std::max<sint32>(outputRect.w, 1), std::max<sint32>(outputRect.h, 1),
				padView, uniformUpload.resource->GetGPUVirtualAddress() + uniformUpload.offset);
			if (prePassSrv.IsValid())
				sourceSrv = prePassSrv;
			else if (auto* copyPipeline = GetOutputPipeline(output.shader == RendererOutputShader::s_fsr_shader_ud ?
				RendererOutputShader::s_copy_shader_ud : RendererOutputShader::s_copy_shader, padView))
				pipeline = copyPipeline;
		}

		const auto outputDescriptorStats = m_descriptorManager->GetStats();
		const bool hasCachedOutputSampler =
			(output.useLinearTexFilter ? m_shaderVisibleOutputLinearSampler.IsValid() : m_shaderVisibleOutputPointSampler.IsValid()) &&
			m_shaderVisibleOutputSamplerGeneration == outputDescriptorStats.shaderVisibleSamplerRollovers;
		EnsureShaderVisibleDescriptorSpace(1, hasCachedOutputSampler ? 0 : 1);
		auto srv = CopyToShaderVisibleChecked(sourceSrv, "output snapshot");
		auto sampler = GetShaderVisibleOutputSampler(output.useLinearTexFilter);
		BindShaderVisibleHeaps();

		chain.frameIndex = chain.swapchain->GetCurrentBackBufferIndex();
		backBuffer = chain.backBuffers[chain.frameIndex].Get();
		if (backBuffer)
		{
			Transition(backBuffer, chain.states[chain.frameIndex], D3D12_RESOURCE_STATE_RENDER_TARGET);
			Transition(output.texture.resource.Get(), output.texture.state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

			auto* commandList = GetCurrentCommandSlot().commandList.Get();
			SetD3D12PipelineState(pipeline);
			SetD3D12GraphicsRootSignature(m_outputRootSignature.Get());
			commandList->SetGraphicsRootDescriptorTable(0, srv.gpu);
			commandList->SetGraphicsRootDescriptorTable(1, sampler.gpu);
			commandList->SetGraphicsRootConstantBufferView(2, uniformUpload.resource->GetGPUVirtualAddress() + uniformUpload.offset);
			commandList->SetGraphicsRootConstantBufferView(3, uniformUpload.resource->GetGPUVirtualAddress() + uniformUpload.offset);
			SetD3D12RenderTargets(1, &chain.rtvHandles[chain.frameIndex], nullptr);

			D3D12_VIEWPORT viewport{};
			viewport.TopLeftX = (float)outputRect.x;
			viewport.TopLeftY = (float)outputRect.y;
			viewport.Width = (float)std::max<sint32>(outputRect.w, 1);
			viewport.Height = (float)std::max<sint32>(outputRect.h, 1);
			viewport.MinDepth = 0.0f;
			viewport.MaxDepth = 1.0f;
			D3D12_RECT scissor{};
			scissor.left = 0;
			scissor.top = 0;
			scissor.right = std::max<sint32>(chain.size.x, 1);
			scissor.bottom = std::max<sint32>(chain.size.y, 1);
			SetD3D12Viewport(viewport);
			SetD3D12Scissor(scissor);
			SetD3D12PrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			commandList->DrawInstanced(6, 1, 0, 0);
			m_gpuWatchdog.RecordOperation(D3D12GpuWatchdog::Operation::SnapshotDraw);
			D3D12TraceEvent("uwp_output_snapshot_draw",
							"window={} frameIndex={} snapshot={}x{} sourceRes={}x{} native={}x{} dstXY={},{} dstSize={}x{} chain={}x{} srvGpu=0x{:016x} samplerGpu=0x{:016x} uniformGpu=0x{:016x} shader={} pipeline={} linear={}",
							padView ? "pad" : "tv",
							chain.frameIndex,
							(uint32)output.desc.Width,
							(uint32)output.desc.Height,
							output.sourceResolution.x,
							output.sourceResolution.y,
							output.nativeResolution.x,
							output.nativeResolution.y,
							outputRect.x,
							outputRect.y,
							outputRect.w,
							outputRect.h,
							chain.size.x,
							chain.size.y,
							(unsigned long long)srv.gpu.ptr,
							(unsigned long long)sampler.gpu.ptr,
							(unsigned long long)(uniformUpload.resource->GetGPUVirtualAddress() + uniformUpload.offset),
							(const void*)output.shader,
							(const void*)pipeline,
							output.useLinearTexFilter ? 1 : 0);
			Transition(backBuffer, chain.states[chain.frameIndex], D3D12_RESOURCE_STATE_PRESENT);
			m_uwpCompositeFrameRendered = true;
			m_uwpOutputDrawnThisFrame[padView ? 1 : 0] = true;
		}
	}
	catch (...)
	{
		m_uwpDrawingCachedOutput = previousDrawingCachedOutput;
		throw;
	}
	m_uwpDrawingCachedOutput = previousDrawingCachedOutput;
}

void D3D12Renderer::DrawUwpCachedVisibleOutputs()
{
	auto& chain = GetSwapchain(true);
	if (!chain.active || !chain.swapchain)
		return;

	const auto tvRect = ResolveUwpOutputRect(false, chain.size);
	const auto padRect = ResolveUwpOutputRect(true, chain.size);
	const uint64 tvArea = GetUwpOutputArea(tvRect);
	const uint64 padArea = GetUwpOutputArea(padRect);

	if (tvArea >= padArea)
	{
		if (tvRect.visible)
			DrawUwpCachedOutput(false);
		if (padRect.visible)
			DrawUwpCachedOutput(true);
	}
	else
	{
		if (padRect.visible)
			DrawUwpCachedOutput(true);
		if (tvRect.visible)
			DrawUwpCachedOutput(false);
	}
}

void D3D12Renderer::BeginUwpCompositeFrame(SwapchainState& chain)
{
	if (m_uwpCompositeFrameStarted)
		return;

	const uint64 titleId = GetUwpTitleId();
	if (m_uwpCachedOutputTitleId != titleId)
	{
		ReleaseUwpCachedOutputs();
		m_uwpCachedOutputTitleId = titleId;
		D3D12TraceEvent("uwp_output_snapshots_invalidate", "title=0x{:016x}", titleId);
	}

	const float color[4]{ 0.0f, 0.0f, 0.0f, 1.0f };
	RecordClear(chain, color, false);
	DrawUwpConfiguredUnderlay(chain);
	m_uwpCompositeFrameStarted = true;
	m_uwpOutputDrawnThisFrame = {};
	m_uwpOutputPendingThisFrame = {};
}

void D3D12Renderer::Flush(bool waitIdle)
{
	if (waitIdle)
	{
		D3D12_EXCLUSIVE_RECORDING();
		WaitForIdle();
		return;
	}
	D3D12_QUEUE_RECORDING_KIND(Command, Flush(false));
	if (waitIdle)
		WaitForIdle();
	else
		ExecuteCurrentCommandList(SubmitFlush);
}

void D3D12Renderer::NotifyLatteCommandProcessorIdle()
{
	D3D12_QUEUE_RECORDING_KIND(Command, NotifyLatteCommandProcessorIdle());
	if (m_submitOnIdle)
		Flush(false);
	else
		ProcessFinishedCommandLists();
	if (!CafeSystem::IsTitleRunning())
		PersistGamePipelineLibrary();
}

void D3D12Renderer::NotifyDepthBufferExpanded(MPTR depthBufferMPTR, Latte::E_GX2SURFFMT depthBufferFormat, uint32 width, uint32 height, uint32 pitch, uint32 viewMip, uint32 viewFirstSlice, uint32 viewNumSlices)
{
	D3D12_QUEUE_RECORDING_KIND(Command, NotifyDepthBufferExpanded(depthBufferMPTR, depthBufferFormat, width, height, pitch, viewMip, viewFirstSlice, viewNumSlices));
	if (!m_commandListOpen)
		ResetCommandList();

	LatteTextureViewD3D12* activeDepthView = nullptr;
	LatteTextureD3D12* activeDepthTexture = nullptr;
	if (m_activeFBO && m_activeFBO->HasDepthStencilAttachment() && m_activeFBO->depthBuffer.texture)
	{
		activeDepthView = static_cast<LatteTextureViewD3D12*>(m_activeFBO->depthBuffer.texture);
		activeDepthTexture = activeDepthView ? activeDepthView->GetBaseImage() : nullptr;
	}

	const bool matchesActiveDepth =
		activeDepthTexture &&
		(activeDepthTexture->physAddress == depthBufferMPTR ||
		 (activeDepthTexture->texDataPtrLow <= depthBufferMPTR && depthBufferMPTR < activeDepthTexture->texDataPtrHigh));

	if (matchesActiveDepth)
		TransitionTextureAll(activeDepthTexture, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

	D3D12TraceEvent("expand_depth_buffer",
					"addr=0x{:08x} fmt=0x{:04x} size={}x{} pitch={} viewMip={} viewSlice={}+{} matchedActive={} activeAddr=0x{:08x} activeSize={}x{} activeFmt=0x{:04x} fbo=0x{:016x}",
					depthBufferMPTR,
					(uint32)depthBufferFormat,
					width,
					height,
					pitch,
					viewMip,
					viewFirstSlice,
					viewNumSlices,
					matchesActiveDepth ? 1 : 0,
					activeDepthTexture ? activeDepthTexture->physAddress : 0,
					activeDepthTexture ? activeDepthTexture->width : 0,
					activeDepthTexture ? activeDepthTexture->height : 0,
					activeDepthTexture ? (uint32)activeDepthTexture->format : 0,
					m_activeFBO ? m_activeFBO->key : 0);

	if (matchesActiveDepth && D3D12ShouldTrace())
		TraceTextureSnapshot("expand_depth_after", "after_expand_depth", activeDepthView, false,
							 fmt::format("addr=0x{:08x} fmt=0x{:04x} requestedSize={}x{} pitch={} viewMip={} viewSlice={}+{}",
										 depthBufferMPTR,
										 (uint32)depthBufferFormat,
										 width,
										 height,
										 pitch,
										 viewMip,
										 viewFirstSlice,
										 viewNumSlices));
}

