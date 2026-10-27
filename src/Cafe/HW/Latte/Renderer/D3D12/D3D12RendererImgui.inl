// Part of D3D12Renderer.cpp (included there, same translation unit): ImGui frames and textures.
// Not a standalone file: it relies on the includes and file-local helpers of D3D12Renderer.cpp.

// ImGui state is built on the calling thread; the command list part goes to RecordImguiFrame in ImguiEnd
bool D3D12Renderer::ImguiBegin(bool mainWindow)
{
	if (!m_imguiInitialized)
		return false;

	sint32 width = 0;
	sint32 height = 0;
	WindowSystem::GetWindowPhysSize(width, height);
	if (width == 0 || height == 0)
		return false;

	ImGui::SetCurrentContext(mainWindow ? imguiTVContext : imguiPadContext);
	{
		ImGuiIO& io = ImGui::GetIO();
		io.DisplaySize = ImVec2((float)width, (float)height);
		io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
	}
	ImGui_PrecacheFonts();
	ImGui::SetCurrentContext(mainWindow ? imguiTVContext : imguiPadContext);
	ImGuiIO& io = ImGui::GetIO();
	io.DisplaySize = ImVec2((float)width, (float)height);
	io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
	if (!io.Fonts->IsBuilt())
	{
		unsigned char* pixels = nullptr;
		int fontWidth = 0;
		int fontHeight = 0;
		io.Fonts->GetTexDataAsRGBA32(&pixels, &fontWidth, &fontHeight);
	}
#if defined(CEMU_UWP_D3D12_DIAGNOSTICS)
	D3D12TraceEventFlush("imgui_font_atlas_ready",
		"context={} fonts={} atlas={}x{} built={} display={}x{}",
		mainWindow ? "tv" : "pad",
		io.Fonts->Fonts.Size,
		io.Fonts->TexWidth,
		io.Fonts->TexHeight,
		io.Fonts->IsBuilt() ? 1 : 0,
		io.DisplaySize.x,
		io.DisplaySize.y);
#endif

	auto& chain = GetSwapchain(true);
	if (!chain.active)
		return false;

#if defined(CEMU_UWP)
	std::optional<RecordingExclusiveScope> deviceObjectsExclusive;
	if (io.Fonts->TexID == 0)
		deviceObjectsExclusive.emplace(this);
	if (io.Fonts->TexID == 0 && !ImGui_ImplDX12_CreateDeviceObjects())
	{
		cemuLog_log(LogType::Force,
			"D3D12: failed to create ImGui device objects (fonts={}, atlas={}x{}).",
			io.Fonts->Fonts.Size, io.Fonts->TexWidth, io.Fonts->TexHeight);
		throw std::runtime_error(fmt::format("D3D12 failed to create the frontend font texture ({})", ImGui_ImplDX12_GetLastError()));
	}
#endif

	ImGui_ImplDX12_NewFrame();
	deviceObjectsExclusive.reset();
	ImGui_UpdateWindowInformation(true);
	{
		const float width = (float)std::max(chain.size.x, 1);
		const float height = (float)std::max(chain.size.y, 1);
		if (io.DisplaySize.x != width || io.DisplaySize.y != height || io.DisplaySize.x < 0.0f || io.DisplaySize.y < 0.0f)
		{
			io.DisplaySize = ImVec2(width, height);
			io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
		}
	}
	ImGui::NewFrame();
	m_imguiActiveSwapchain = &chain;
	return true;
}

void D3D12Renderer::ImguiEnd()
{
	if (!m_imguiInitialized || !m_imguiActiveSwapchain)
		return;
	ImGui::Render();
	SwapchainState* chain = std::exchange(m_imguiActiveSwapchain, nullptr);
	const bool titleRunning = CafeSystem::IsTitleRunning();
	ImDrawData* drawData = ImGui::GetDrawData();
	if (drawData && drawData->TotalVtxCount <= 0)
		drawData = nullptr;
	void* imguiBackend = ImGui_ImplDX12_GetCurrentBackendData();
	if (!ShouldQueueRecording())
	{
		RecordImguiFrame(chain, drawData, imguiBackend, titleRunning);
		return;
	}
	// the context's draw lists are rebuilt next frame: the recorder gets its own copy
	struct DrawDataCopy
	{
		ImDrawData data{};
		std::vector<ImDrawList*> lists;
		~DrawDataCopy() { for (ImDrawList* list : lists) IM_DELETE(list); }
	};
	std::shared_ptr<DrawDataCopy> copy;
	if (drawData)
	{
		copy = std::make_shared<DrawDataCopy>();
		copy->data = *drawData;
		copy->lists.reserve(drawData->CmdListsCount);
		for (int i = 0; i < drawData->CmdListsCount; i++)
			copy->lists.push_back(drawData->CmdLists[i]->CloneOutput());
		copy->data.CmdLists = copy->lists.data();
	}
	QueueRecording([=, this]() { RecordImguiFrame(chain, copy ? &copy->data : nullptr, imguiBackend, titleRunning); }, RecordOpKind::Command);
}

void D3D12Renderer::RecordImguiFrame(SwapchainState* chain, ImDrawData* drawData, void* imguiBackend, bool titleRunning)
{
	if (!m_commandListOpen)
		ResetCommandList();
	chain->frameIndex = chain->swapchain->GetCurrentBackBufferIndex();
	Transition(chain->backBuffers[chain->frameIndex].Get(), chain->states[chain->frameIndex], D3D12_RESOURCE_STATE_RENDER_TARGET);
	if (drawData)
	{
		SetD3D12RenderTargets(1, &chain->rtvHandles[chain->frameIndex], nullptr);
		ID3D12DescriptorHeap* heaps[] = { m_imguiSrvHeap.Get() };
		GetCurrentCommandSlot().commandList->SetDescriptorHeaps(1, heaps);
		ResetCommandStateCache();
		m_gpuWatchdog.RecordOperation(D3D12GpuWatchdog::Operation::OverlayDraw);
		ImGui_ImplDX12_RenderDrawDataWithBackend(imguiBackend, drawData, GetCurrentCommandSlot().commandList.Get());
		ResetCommandStateCache();
	}
	Transition(chain->backBuffers[chain->frameIndex].Get(), chain->states[chain->frameIndex], D3D12_RESOURCE_STATE_PRESENT);
	if (titleRunning)
	{
		m_uwpCompositeFrameStarted = true;
		m_uwpCompositeFrameRendered = true;
	}
}

ImTextureID D3D12Renderer::GenerateImguiTexture(const std::vector<uint8>& data, const Vector2i& size, DXGI_FORMAT format)
{
	if (!m_memoryManager || size.x <= 0 || size.y <= 0 || data.empty())
		return nullptr;
	try
	{
		if (!m_commandListOpen)
			ResetCommandList();

		D3D12_RESOURCE_DESC desc{};
		desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		desc.Width = (UINT64)size.x;
		desc.Height = (UINT)size.y;
		desc.DepthOrArraySize = 1;
		desc.MipLevels = 1;
		desc.Format = format;
		desc.SampleDesc.Count = 1;
		desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

		D3D12MemoryManager::Resource texture;
		if (!m_memoryManager->CreateTexture(desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, texture))
			throw std::runtime_error("Failed to create D3D12 ImGui texture");

		D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
		UINT numRows = 0;
		UINT64 rowSizeInBytes = 0;
		UINT64 totalBytes = 0;
		m_device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, &numRows, &rowSizeInBytes, &totalBytes);
#if defined(CEMU_UWP_D3D12_DIAGNOSTICS)
		D3D12TraceEventFlush("imgui_texture_upload_begin",
			"size={}x{} format={} sourceBytes={} rows={} rowBytes={} rowPitch={} totalBytes={}",
			size.x,
			size.y,
			(uint32)format,
			data.size(),
			numRows,
			(unsigned long long)rowSizeInBytes,
			footprint.Footprint.RowPitch,
			(unsigned long long)totalBytes);
#endif
		auto upload = m_memoryManager->AllocateUpload((uint32)totalBytes, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT, m_nextFenceValue);
		if (!upload.IsValid())
			throw std::runtime_error("Failed to allocate D3D12 ImGui texture upload memory");

		const uint8* src = data.data();
		const size_t rgbBytes = (size_t)size.x * size.y * 3;
		const size_t rgbaBytes = (size_t)size.x * size.y * 4;
		const bool sourceIsRgb = data.size() >= rgbBytes && data.size() < rgbaBytes;
		for (sint32 y = 0; y < size.y; ++y)
		{
			uint8* dstRow = upload.cpuPtr + y * footprint.Footprint.RowPitch;
			if (sourceIsRgb)
			{
				for (sint32 x = 0; x < size.x; ++x)
				{
					const size_t srcIndex = ((size_t)y * size.x + x) * 3;
					const size_t dstIndex = (size_t)x * 4;
					dstRow[dstIndex + 0] = src[srcIndex + 0];
					dstRow[dstIndex + 1] = src[srcIndex + 1];
					dstRow[dstIndex + 2] = src[srcIndex + 2];
					dstRow[dstIndex + 3] = 0xFF;
				}
			}
			else
			{
				const size_t srcOffset = (size_t)y * size.x * 4;
				if (srcOffset < data.size())
				{
					const size_t copyBytes = std::min<size_t>((size_t)rowSizeInBytes, data.size() - srcOffset);
					memcpy(dstRow, src + srcOffset, copyBytes);
				}
			}
		}
		footprint.Offset = upload.offset;

		D3D12_TEXTURE_COPY_LOCATION source{};
		source.pResource = upload.resource;
		source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
		source.PlacedFootprint = footprint;

		D3D12_TEXTURE_COPY_LOCATION destination{};
		destination.pResource = texture.resource.Get();
		destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		destination.SubresourceIndex = 0;

		GetCurrentCommandSlot().commandList->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
		Transition(texture.resource.Get(), texture.state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

		const uint32 descriptorIndex = AllocateImguiDescriptor();
		D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		srvDesc.Format = format;
		srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Texture2D.MipLevels = 1;
		CreateShaderResourceViewChecked(texture.resource.Get(), &srvDesc, GetImguiCpuHandle(descriptorIndex), "overlay texture", false);
		const auto gpuHandle = GetImguiGpuHandle(descriptorIndex);
		m_imguiTextures.emplace(gpuHandle.ptr, ImGuiTextureResource{ std::move(texture), descriptorIndex, size });
#if defined(CEMU_UWP_D3D12_DIAGNOSTICS)
		D3D12TraceEventFlush("imgui_texture_upload_end", "descriptor={} gpu=0x{:016x}", descriptorIndex, (unsigned long long)gpuHandle.ptr);
#endif
		return (ImTextureID)(uintptr_t)gpuHandle.ptr;
	}
	catch (const std::exception& ex)
	{
		cemuLog_log(LogType::Force, "can't generate D3D12 imgui texture: {}", ex.what());
		return nullptr;
	}
}

bool D3D12Renderer::UpdateImguiTexture(ImTextureID id, const std::vector<uint8>& data, const Vector2i& size, DXGI_FORMAT expectedFormat)
{
	if (!m_memoryManager || size.x <= 0 || size.y <= 0)
		return false;
	auto it = m_imguiTextures.find((uint64)(uintptr_t)id);
	if (it == m_imguiTextures.end())
		return false;
	if (it->second.size.x != size.x || it->second.size.y != size.y)
		return false;
	if (data.size() < (size_t)size.x * (size_t)size.y * 4)
		return false;
	try
	{
		if (!m_commandListOpen)
			ResetCommandList();

		auto& entry = it->second;
		const D3D12_RESOURCE_DESC desc = entry.texture.resource->GetDesc();
		if (desc.Format != expectedFormat)
			return false;

		D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
		UINT numRows = 0;
		UINT64 rowSizeInBytes = 0;
		UINT64 totalBytes = 0;
		m_device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, &numRows, &rowSizeInBytes, &totalBytes);
		auto upload = m_memoryManager->AllocateUpload((uint32)totalBytes, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT, m_nextFenceValue);
		if (!upload.IsValid())
			throw std::runtime_error("Failed to allocate D3D12 ImGui texture upload memory");

		const uint8* src = data.data();
		const size_t srcRowBytes = (size_t)size.x * 4;
		const size_t copyBytes = std::min<size_t>((size_t)rowSizeInBytes, srcRowBytes);
		for (sint32 y = 0; y < size.y; ++y)
			memcpy(upload.cpuPtr + (size_t)y * footprint.Footprint.RowPitch, src + (size_t)y * srcRowBytes, copyBytes);
		footprint.Offset = upload.offset;

		D3D12_TEXTURE_COPY_LOCATION source{};
		source.pResource = upload.resource;
		source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
		source.PlacedFootprint = footprint;

		D3D12_TEXTURE_COPY_LOCATION destination{};
		destination.pResource = entry.texture.resource.Get();
		destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		destination.SubresourceIndex = 0;

		Transition(entry.texture.resource.Get(), entry.texture.state, D3D12_RESOURCE_STATE_COPY_DEST);
		GetCurrentCommandSlot().commandList->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
		Transition(entry.texture.resource.Get(), entry.texture.state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		return true;
	}
	catch (const std::exception& ex)
	{
		cemuLog_log(LogType::Force, "can't update D3D12 imgui texture: {}", ex.what());
		return false;
	}
}

ImTextureID D3D12Renderer::GenerateTexture(const std::vector<uint8>& data, const Vector2i& size)
{
	D3D12_EXCLUSIVE_RECORDING();
	return GenerateImguiTexture(data, size, DXGI_FORMAT_R8G8B8A8_UNORM);
}

bool D3D12Renderer::UpdateTexture(ImTextureID id, const std::vector<uint8>& data, const Vector2i& size)
{
	return UpdateImguiTexture(id, data, size, DXGI_FORMAT_R8G8B8A8_UNORM);
}

ImTextureID D3D12Renderer::GenerateTextureBGRA(const std::vector<uint8>& data, const Vector2i& size)
{
	D3D12_EXCLUSIVE_RECORDING();
	return GenerateImguiTexture(data, size, DXGI_FORMAT_B8G8R8A8_UNORM);
}

bool D3D12Renderer::UpdateTextureBGRA(ImTextureID id, const std::vector<uint8>& data, const Vector2i& size)
{
	return UpdateImguiTexture(id, data, size, DXGI_FORMAT_B8G8R8A8_UNORM);
}

void D3D12Renderer::DeleteTexture(ImTextureID id)
{
	D3D12_EXCLUSIVE_RECORDING();
	const uint64 key = (uint64)(uintptr_t)id;
	auto it = m_imguiTextures.find(key);
	if (it == m_imguiTextures.end())
		return;
	QueueDeferredRelease(std::move(it->second.texture));
	if (it->second.descriptorIndex != 0)
	{
		const uint64 retirementFence = GetRetirementFence();
		if (retirementFence == 0 || retirementFence <= m_lastCompletedFenceValue)
		{
			m_imguiFreeDescriptorIndices.push_back(it->second.descriptorIndex);
		}
		else
		{
			std::scoped_lock lock(m_deferredReleaseMutex);
			m_retiredImguiDescriptors.push_back({ retirementFence, it->second.descriptorIndex });
		}
	}
	m_imguiTextures.erase(it);
}

void D3D12Renderer::DeleteFontTextures()
{
	D3D12_EXCLUSIVE_RECORDING();
	if (!m_imguiInitialized)
		return;
	ImGuiContext* previousContext = ImGui::GetCurrentContext();
	WaitForIdle();
	ImGui::SetCurrentContext(imguiTVContext);
	ImGui_ImplDX12_InvalidateDeviceObjects();
	ImGui::SetCurrentContext(imguiPadContext);
	ImGui_ImplDX12_InvalidateDeviceObjects();
	if (previousContext)
		ImGui::SetCurrentContext(previousContext);
}

