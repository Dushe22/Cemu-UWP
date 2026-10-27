// Part of D3D12Renderer.cpp (included there, same translation unit): output and helper pipelines, geometry shaders for points/rects, pipeline recipes, prewarm, async PSO creation and GetGamePipeline.
// Not a standalone file: it relies on the includes and file-local helpers of D3D12Renderer.cpp.

ID3D12PipelineState* D3D12Renderer::GetOutputPipeline(RendererOutputShader* shader, bool padView)
{
	if (!shader || !m_outputRootSignature)
	{
		D3D12TraceEvent("output_pipeline_unavailable", "reason=missing_shader_or_root shader={} rootSig={} pad={}",
						(const void*)shader,
						m_outputRootSignature ? 1 : 0,
						padView ? 1 : 0);
		return nullptr;
	}

	auto* vertexShader = static_cast<RendererShaderD3D12*>(shader->GetVertexShader());
	auto* pixelShader = static_cast<RendererShaderD3D12*>(shader->GetFragmentShader());
	if (!vertexShader || !pixelShader || !vertexShader->WaitForCompiled() || !pixelShader->WaitForCompiled())
	{
		D3D12TraceEvent("output_pipeline_unavailable", "reason=shader_compile_incomplete shader={} vs={} ps={} pad={}",
						(const void*)shader,
						(const void*)vertexShader,
						(const void*)pixelShader,
						padView ? 1 : 0);
		return nullptr;
	}
	if (!vertexShader->HasDXIL() || !pixelShader->HasDXIL())
	{
		D3D12TraceEvent("output_pipeline_unavailable", "reason=missing_dxil shader={} vs={} ps={} vsDxil={} psDxil={} pad={}",
						(const void*)shader,
						(const void*)vertexShader,
						(const void*)pixelShader,
						vertexShader->HasDXIL() ? 1 : 0,
						pixelShader->HasDXIL() ? 1 : 0,
						padView ? 1 : 0);
		return nullptr;
	}

	const uint64 key = ((uint64)(uintptr_t)vertexShader) ^ std::rotl((uint64)(uintptr_t)pixelShader, 17) ^ ((uint64)padView << 3);
	auto it = m_outputPipelineCache.find(key);
	if (it != m_outputPipelineCache.end())
	{
		D3D12TraceEvent("output_pipeline_cache_hit", "key=0x{:016x} shader={} vs={} ps={} pad={} pipeline={}",
						key,
						(const void*)shader,
						(const void*)vertexShader,
						(const void*)pixelShader,
						padView ? 1 : 0,
						(const void*)it->second.Get());
		return it->second.Get();
	}

	D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
	desc.pRootSignature = m_outputRootSignature.Get();
	desc.VS = { vertexShader->GetDXIL().data(), vertexShader->GetDXIL().size() };
	desc.PS = { pixelShader->GetDXIL().data(), pixelShader->GetDXIL().size() };
	desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
	desc.SampleMask = UINT_MAX;
	desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
	desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
	desc.RasterizerState.FrontCounterClockwise = FALSE;
	desc.RasterizerState.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
	desc.RasterizerState.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
	desc.RasterizerState.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
	desc.RasterizerState.DepthClipEnable = TRUE;
	desc.RasterizerState.MultisampleEnable = FALSE;
	desc.RasterizerState.AntialiasedLineEnable = FALSE;
	desc.RasterizerState.ForcedSampleCount = 0;
	desc.RasterizerState.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
	desc.DepthStencilState.DepthEnable = FALSE;
	desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
	desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
	desc.DepthStencilState.StencilEnable = FALSE;
	desc.InputLayout = {};
	desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	desc.NumRenderTargets = 1;
	desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
	desc.SampleDesc.Count = 1;

	D3D12TraceEvent("output_pipeline_create_begin", "key=0x{:016x} shader={} vs={} ps={} pad={} vsBytes={} psBytes={} rtv0={} rootSig={}",
					key,
					(const void*)shader,
					(const void*)vertexShader,
					(const void*)pixelShader,
					padView ? 1 : 0,
					(unsigned long long)desc.VS.BytecodeLength,
					(unsigned long long)desc.PS.BytecodeLength,
					(uint32)desc.RTVFormats[0],
					(const void*)m_outputRootSignature.Get());
	Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline;
	const HRESULT hr = m_device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pipeline));
	if (FAILED(hr))
	{
		cemuLog_log(LogType::Force, "D3D12: failed to create output blit PSO (HRESULT 0x{:08x})", (uint32)hr);
		D3D12TraceEvent("output_pipeline_create_failed", "key=0x{:016x} hr=0x{:08x} shader={} vs={} ps={} pad={} vsBytes={} psBytes={} rtv0={}",
						key,
						(uint32)hr,
						(const void*)shader,
						(const void*)vertexShader,
						(const void*)pixelShader,
						padView ? 1 : 0,
						(unsigned long long)desc.VS.BytecodeLength,
						(unsigned long long)desc.PS.BytecodeLength,
						(uint32)desc.RTVFormats[0]);
		return nullptr;
	}
	SetD3D12ObjectName(pipeline.Get(), L"Cemu D3D12 output blit PSO");
	auto [insertedIt, inserted] = m_outputPipelineCache.emplace(key, std::move(pipeline));
	// keep the PSO names apart in captures: the FSR upscale pass draws into its own target, not the swapchain
	if (shader == RendererOutputShader::s_fsr_upscale_shader)
		SetD3D12ObjectName(insertedIt->second.Get(), L"Cemu D3D12 output upscale PSO");
	(void)inserted;
	D3D12TraceEvent("output_pipeline_create_success", "key=0x{:016x} shader={} vs={} ps={} pad={} pipeline={} cacheSize={}",
					key,
					(const void*)shader,
					(const void*)vertexShader,
					(const void*)pixelShader,
					padView ? 1 : 0,
					(const void*)insertedIt->second.Get(),
					m_outputPipelineCache.size());
	return insertedIt->second.Get();
}

D3D12DescriptorManager::Allocation D3D12Renderer::DrawOutputPrePass(RendererOutputShader* prePass, const D3D12DescriptorManager::Allocation& sourceSrv,
	sint32 width, sint32 height, bool padView, D3D12_GPU_VIRTUAL_ADDRESS uniformAddress)
{
	if (!prePass || !sourceSrv.IsValid() || width <= 0 || height <= 0)
		return {};
	OutputPrePassTarget& target = m_outputPrePassTargets[padView ? 1 : 0];
	if (!target.texture.IsValid() || target.width != width || target.height != height)
	{
		if (target.texture.IsValid())
			ReleaseResourceDeferred(target.texture);
		if (target.rtv.IsValid())
			m_descriptorManager->FreePersistent(target.rtv);
		if (target.srv.IsValid())
			m_descriptorManager->FreePersistent(target.srv);
		target = {};

		// the backbuffer format, so the output pipelines fit both passes
		D3D12_RESOURCE_DESC desc{};
		desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		desc.Width = (UINT64)width;
		desc.Height = (UINT)height;
		desc.DepthOrArraySize = 1;
		desc.MipLevels = 1;
		desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		desc.SampleDesc.Count = 1;
		desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
		desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
		if (!CreateTextureWithRecovery(desc, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, target.texture, "output upscale target"))
		{
			cemuLog_log(LogType::Force, "D3D12: could not create the {}x{} output upscale target; using the plain copy instead.", width, height);
			target = {};
			return {};
		}
		target.rtv = m_descriptorManager->AllocatePersistent(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
		target.srv = m_descriptorManager->AllocatePersistent(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
		if (!target.rtv.IsValid() || !target.srv.IsValid())
		{
			ReleaseResourceDeferred(target.texture);
			if (target.rtv.IsValid())
				m_descriptorManager->FreePersistent(target.rtv);
			if (target.srv.IsValid())
				m_descriptorManager->FreePersistent(target.srv);
			target = {};
			return {};
		}
		CreateRenderTargetViewChecked(target.texture.resource.Get(), nullptr, target.rtv.cpu, "output pre-pass");
		CreateShaderResourceViewChecked(target.texture.resource.Get(), nullptr, target.srv.cpu, "output pre-pass", false);
		SetD3D12ObjectName(target.texture.resource.Get(), padView ? L"Cemu D3D12 pad output upscale target" : L"Cemu D3D12 TV output upscale target");
		target.width = width;
		target.height = height;
	}

	ID3D12PipelineState* pipeline = GetOutputPipeline(prePass, padView);
	if (!pipeline)
		return {};

	const auto descriptorStats = m_descriptorManager->GetStats();
	const bool hasCachedPointSampler = m_shaderVisibleOutputPointSampler.IsValid() &&
		m_shaderVisibleOutputSamplerGeneration == descriptorStats.shaderVisibleSamplerRollovers;
	EnsureShaderVisibleDescriptorSpace(1, hasCachedPointSampler ? 0 : 1);
	const auto srv = CopyToShaderVisibleChecked(sourceSrv, "output pre-pass");
	// the upscale gathers its own taps, so the filter does not matter
	const auto sampler = GetShaderVisibleOutputSampler(false);
	BindShaderVisibleHeaps();

	Transition(target.texture.resource.Get(), target.texture.state, D3D12_RESOURCE_STATE_RENDER_TARGET);
	auto* commandList = GetCurrentCommandSlot().commandList.Get();
	SetD3D12PipelineState(pipeline);
	SetD3D12GraphicsRootSignature(m_outputRootSignature.Get());
	commandList->SetGraphicsRootDescriptorTable(0, srv.gpu);
	commandList->SetGraphicsRootDescriptorTable(1, sampler.gpu);
	commandList->SetGraphicsRootConstantBufferView(2, uniformAddress);
	commandList->SetGraphicsRootConstantBufferView(3, uniformAddress);
	SetD3D12RenderTargets(1, &target.rtv.cpu, nullptr);
	const D3D12_VIEWPORT viewport{ 0.0f, 0.0f, (float)width, (float)height, 0.0f, 1.0f };
	const D3D12_RECT scissor{ 0, 0, width, height };
	SetD3D12Viewport(viewport);
	SetD3D12Scissor(scissor);
	SetD3D12PrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	commandList->DrawInstanced(6, 1, 0, 0);
	Transition(target.texture.resource.Get(), target.texture.state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	D3D12TraceEvent("output_prepass", "window={} size={}x{} target={} pipeline={}",
		padView ? "pad" : "tv", width, height, (const void*)target.texture.resource.Get(), (const void*)pipeline);
	return target.srv;
}

void D3D12Renderer::ReleaseOutputPrePassTargets()
{
	for (auto& target : m_outputPrePassTargets)
	{
		if (target.texture.IsValid())
			ReleaseResourceDeferred(target.texture);
		if (m_descriptorManager)
		{
			if (target.rtv.IsValid())
				m_descriptorManager->FreePersistent(target.rtv);
			if (target.srv.IsValid())
				m_descriptorManager->FreePersistent(target.srv);
		}
		target = {};
	}
}

bool D3D12Renderer::ShouldUsePointExpansionGeometryShader(LatteDecompilerShader* geometryShader,
														  Latte::LATTE_VGT_PRIMITIVE_TYPE::E_PRIMITIVE_TYPE primitiveMode) const
{
	return geometryShader == nullptr && D3D12PrimitiveUsesPointExpansion(primitiveMode);
}

bool D3D12Renderer::UsesDynamicDepthBias(bool usePointExpansionGS) const
{
	return m_dynamicDepthBiasSupported && !usePointExpansionGS &&
		RecordRegs().PA_SU_SC_MODE_CNTL.get_OFFSET_FRONT_ENABLED();
}

RendererShaderD3D12* D3D12Renderer::GetPointExpansionGeometryShader(LatteDecompilerShader* vertexShader, LatteDecompilerShader* pixelShader)
{
	if (!vertexShader)
		return nullptr;

	struct SemanticMapping
	{
		uint32 fullSemantic{};
		uint32 semanticIndex{};
		const char* interpolationModifier{};
	};
	std::vector<SemanticMapping> semantics;
	LatteShaderPSInputTable* psInputTable = (&s_recordPsInputTable);
	if (psInputTable)
	{
		for (sint32 i = 0; i < psInputTable->count; ++i)
		{
			const uint32 semanticId = psInputTable->import[i].semanticId;
			if (semanticId > LATTE_ANALYZER_IMPORT_INDEX_PARAM_MAX)
				continue;
			semantics.push_back({ semanticId, (uint32)i, D3D12InterpolationModifier(&psInputTable->import[i]) });
		}
	}
	else
	{
		bool emittedLocal[128]{};
		const uint32 parameterMask = vertexShader->outputParameterMask;
		for (uint32 i = 0; i < 32; ++i)
		{
			if ((parameterMask & (1u << i)) == 0)
				continue;
			const sint32 semanticId = LatteShaderPSInputTable::getVertexShaderOutParamSemanticId(RecordRegWords(), i);
			if (semanticId < 0 || semanticId == 0xFF)
				continue;
			const uint32 localSemantic = (uint32)semanticId & 0x7F;
			if (localSemantic >= std::size(emittedLocal) || emittedLocal[localSemantic])
				continue;
			emittedLocal[localSemantic] = true;
			semantics.push_back({ (uint32)semanticId, localSemantic, "" });
		}
	}

	uint64 key = 0xD3129017u + kD3D12SyntheticGsGeneratorVersion;
	key = std::rotl(key, 7) ^ vertexShader->outputParameterMask;
	key = std::rotl(key, 7) ^ (pixelShader ? pixelShader->baseHash : 0);
	const bool shaderWritesPointSize = vertexShader->uniform.loc_pointSize < 0;
	key = std::rotl(key, 7) ^ (shaderWritesPointSize ? 0x50535752u : 0x50534658u);
	for (const auto& semantic : semantics)
	{
		key = std::rotl(key, 5) ^ semantic.fullSemantic;
		key = std::rotl(key, 5) ^ semantic.semanticIndex;
		key = std::rotl(key, 5) ^ (semantic.interpolationModifier[0] == 'n' ? (semantic.interpolationModifier[2] == 'i' ? 0xF147u : 0xF0A5u) : 0u);
	}

	auto it = m_pointExpansionGeometryShaders.find(key);
	if (it != m_pointExpansionGeometryShaders.end())
		return it->second.get();

	std::string hlsl;
	hlsl.reserve(8192);
	hlsl += fmt::format("// D3D12 synthetic point expansion geometry shader {:016x}\n", key);
	hlsl += "cbuffer LatteD3D12DrawConstants : register(b1, space1)\n";
	hlsl += "{\n";
	hlsl += "\tuint uf_d3d12LogicOpMode;\n";
	hlsl += "\tuint uf_d3d12BaseVertex;\n";
	hlsl += "\tuint uf_d3d12BaseInstance;\n";
	hlsl += "\tuint uf_d3d12Reserved0;\n";
	hlsl += "\tfloat2 uf_d3d12InvViewportSize;\n";
	hlsl += "\tuint2 uf_d3d12DrawConstantsPadding;\n";
	hlsl += "\tuint4 uf_d3d12VertexStrides[4];\n";
	hlsl += "};\n";
	hlsl += "struct GSIn\n{\n";
	hlsl += "\tfloat4 position : SV_Position;\n";
	for (const auto& semantic : semantics)
		hlsl += fmt::format("\t{}float4 passParameterSem{} : TEXCOORD{};\n", semantic.interpolationModifier, semantic.fullSemantic, semantic.semanticIndex);
	// Match VSOut field order on Xbox and preserve shader-written point sizes.
	hlsl += "\tfloat pointSize : POINTSIZE;\n";
	hlsl += "};\n";
	hlsl += "struct GSOut\n{\n";
	hlsl += "\tfloat4 position : SV_Position;\n";
	for (const auto& semantic : semantics)
		hlsl += fmt::format("\t{}float4 passParameterSem{} : TEXCOORD{};\n", semantic.interpolationModifier, semantic.fullSemantic, semantic.semanticIndex);
	// pointCoord must come after the varyings to match the PS signature order
	hlsl += "\tfloat2 pointCoord : CEMUPOINTCOORD;\n";
	hlsl += "};\n";
	hlsl += "[maxvertexcount(4)]\n";
	hlsl += "void main(point GSIn input[1], inout TriangleStream<GSOut> stream)\n";
	hlsl += "{\n";
	hlsl += "\tfloat4 center = input[0].position;\n";
	if (shaderWritesPointSize)
		hlsl += "\tfloat2 pointSize = max(input[0].pointSize.xx, float2(0.125f, 0.125f));\n";
	else
		hlsl += "\tfloat2 pointSize = max(float2(asfloat(uf_d3d12DrawConstantsPadding.x), asfloat(uf_d3d12DrawConstantsPadding.y)), float2(0.125f, 0.125f));\n";
	hlsl += "\tfloat2 halfClip = pointSize * uf_d3d12InvViewportSize * center.w;\n";
	hlsl += "\tfloat2 offsets[4] = { float2(-halfClip.x, -halfClip.y), float2(-halfClip.x, halfClip.y), float2(halfClip.x, -halfClip.y), float2(halfClip.x, halfClip.y) };\n";
	hlsl += "\tfloat2 coords[4] = { float2(0.0f, 1.0f), float2(0.0f, 0.0f), float2(1.0f, 1.0f), float2(1.0f, 0.0f) };\n";
	hlsl += "\t[unroll] for (uint i = 0; i < 4; ++i)\n";
	hlsl += "\t{\n";
	hlsl += "\t\tGSOut output = (GSOut)0;\n";
	hlsl += "\t\toutput.position = center;\n";
	hlsl += "\t\toutput.position.xy += offsets[i];\n";
	hlsl += "\t\toutput.pointCoord = coords[i];\n";
	for (const auto& semantic : semantics)
		hlsl += fmt::format("\t\toutput.passParameterSem{} = input[0].passParameterSem{};\n", semantic.fullSemantic, semantic.fullSemantic);
	hlsl += "\t\tstream.Append(output);\n";
	hlsl += "\t}\n";
	hlsl += "\tstream.RestartStrip();\n";
	hlsl += "}\n";

	D3D12DumpHLSL(RendererShader::ShaderType::kGeometry, key, 0xD3129017ull, true, hlsl);
	auto shader = std::make_unique<RendererShaderD3D12>(RendererShader::ShaderType::kGeometry, key, 0xD3129017u, false, false, hlsl);
	auto* shaderPtr = shader.get();
	m_pointExpansionGeometryShaders.emplace(key, std::move(shader));
	cemuLog_logOnce(LogType::Force, "D3D12: using synthetic geometry shaders for native point-size expansion.");
	return shaderPtr;
}

RendererShaderD3D12* D3D12Renderer::GetRectEmulationGeometryShader(LatteDecompilerShader* vertexShader, LatteDecompilerShader* pixelShader)
{
	if (!vertexShader)
		return nullptr;

	struct SemanticMapping
	{
		uint32 fullSemantic{};
		uint32 semanticIndex{};
		const char* interpolationModifier{};
	};
	std::vector<SemanticMapping> semantics;
	LatteShaderPSInputTable* psInputTable = (&s_recordPsInputTable);
	if (psInputTable)
	{
		for (sint32 i = 0; i < psInputTable->count; ++i)
		{
			const uint32 semanticId = psInputTable->import[i].semanticId;
			if (semanticId > LATTE_ANALYZER_IMPORT_INDEX_PARAM_MAX)
				continue;
			semantics.push_back({ semanticId, (uint32)i, D3D12InterpolationModifier(&psInputTable->import[i]) });
		}
	}
	else
	{
		bool emittedLocal[128]{};
		const uint32 parameterMask = vertexShader->outputParameterMask;
		for (uint32 i = 0; i < 32; ++i)
		{
			if ((parameterMask & (1u << i)) == 0)
				continue;
			const sint32 semanticId = LatteShaderPSInputTable::getVertexShaderOutParamSemanticId(RecordRegWords(), i);
			if (semanticId < 0 || semanticId == 0xFF)
				continue;
			const uint32 localSemantic = (uint32)semanticId & 0x7F;
			if (localSemantic >= std::size(emittedLocal) || emittedLocal[localSemantic])
				continue;
			emittedLocal[localSemantic] = true;
			semantics.push_back({ (uint32)semanticId, localSemantic, "" });
		}
	}

	uint64 key = 0xD3120EC7ull + kD3D12SyntheticGsGeneratorVersion;
	key = std::rotl(key, 7) ^ vertexShader->outputParameterMask;
	key = std::rotl(key, 7) ^ (pixelShader ? pixelShader->baseHash : 0);
	for (const auto& semantic : semantics)
	{
		key = std::rotl(key, 5) ^ semantic.fullSemantic;
		key = std::rotl(key, 5) ^ semantic.semanticIndex;
		key = std::rotl(key, 5) ^ (semantic.interpolationModifier[0] == 'n' ? (semantic.interpolationModifier[2] == 'i' ? 0xF147u : 0xF0A5u) : 0u);
	}

	auto it = m_rectEmulationGeometryShaders.find(key);
	if (it != m_rectEmulationGeometryShaders.end())
		return it->second.get();

	std::string hlsl;
	hlsl.reserve(8192);
	hlsl += fmt::format("// D3D12 synthetic rectangle emulation geometry shader {:016x}\n", key);
	hlsl += "struct GSIn\n{\n";
	hlsl += "\tfloat4 position : SV_Position;\n";
	for (const auto& semantic : semantics)
		hlsl += fmt::format("\t{}float4 passParameterSem{} : TEXCOORD{};\n", semantic.interpolationModifier, semantic.fullSemantic, semantic.semanticIndex);
	hlsl += "};\n";
	hlsl += "struct GSOut\n{\n";
	hlsl += "\tfloat4 position : SV_Position;\n";
	for (const auto& semantic : semantics)
		hlsl += fmt::format("\t{}float4 passParameterSem{} : TEXCOORD{};\n", semantic.interpolationModifier, semantic.fullSemantic, semantic.semanticIndex);
	hlsl += "};\n";
	hlsl += "float4 gen4thVertexA(float4 a, float4 b, float4 c)\n{\n\treturn b - (c - a);\n}\n";
	hlsl += "float4 gen4thVertexB(float4 a, float4 b, float4 c)\n{\n\treturn c - (b - a);\n}\n";
	hlsl += "float4 gen4thVertexC(float4 a, float4 b, float4 c)\n{\n\treturn c + (b - a);\n}\n";
	hlsl += "void emitSingleVertex(uint index, GSIn input[3], inout TriangleStream<GSOut> stream)\n{\n";
	hlsl += "\tGSOut output = (GSOut)0;\n";
	hlsl += "\toutput.position = input[index].position;\n";
	for (const auto& semantic : semantics)
		hlsl += fmt::format("\toutput.passParameterSem{} = input[index].passParameterSem{};\n", semantic.fullSemantic, semantic.fullSemantic);
	hlsl += "\tstream.Append(output);\n";
	hlsl += "}\n";
	hlsl += "void emitGeneratedVertex(uint variant, GSIn input[3], inout TriangleStream<GSOut> stream)\n{\n";
	hlsl += "\tGSOut output = (GSOut)0;\n";
	hlsl += "\tif (variant == 0) output.position = gen4thVertexA(input[0].position, input[1].position, input[2].position);\n";
	hlsl += "\telse if (variant == 1) output.position = gen4thVertexB(input[0].position, input[1].position, input[2].position);\n";
	hlsl += "\telse output.position = gen4thVertexC(input[0].position, input[1].position, input[2].position);\n";
	for (const auto& semantic : semantics)
	{
		hlsl += fmt::format("\tif (variant == 0) output.passParameterSem{} = gen4thVertexA(input[0].passParameterSem{}, input[1].passParameterSem{}, input[2].passParameterSem{});\n",
							semantic.fullSemantic, semantic.fullSemantic, semantic.fullSemantic, semantic.fullSemantic);
		hlsl += fmt::format("\telse if (variant == 1) output.passParameterSem{} = gen4thVertexB(input[0].passParameterSem{}, input[1].passParameterSem{}, input[2].passParameterSem{});\n",
							semantic.fullSemantic, semantic.fullSemantic, semantic.fullSemantic, semantic.fullSemantic);
		hlsl += fmt::format("\telse output.passParameterSem{} = gen4thVertexC(input[0].passParameterSem{}, input[1].passParameterSem{}, input[2].passParameterSem{});\n",
							semantic.fullSemantic, semantic.fullSemantic, semantic.fullSemantic, semantic.fullSemantic);
	}
	hlsl += "\tstream.Append(output);\n";
	hlsl += "}\n";
	hlsl += "void emitRectVertex(uint vertex, uint variant, GSIn input[3], inout TriangleStream<GSOut> stream)\n{\n";
	hlsl += "\tif (vertex == 3) emitGeneratedVertex(variant, input, stream);\n";
	hlsl += "\telse emitSingleVertex(vertex, input, stream);\n";
	hlsl += "}\n";
	hlsl += "void emitRect(uint p0, uint p1, uint p2, uint p3, uint variant, GSIn input[3], inout TriangleStream<GSOut> stream)\n{\n";
	hlsl += "\temitRectVertex(p0, variant, input, stream);\n";
	hlsl += "\temitRectVertex(p1, variant, input, stream);\n";
	hlsl += "\temitRectVertex(p2, variant, input, stream);\n";
	hlsl += "\temitRectVertex(p3, variant, input, stream);\n";
	hlsl += "\tstream.RestartStrip();\n";
	hlsl += "}\n";
	hlsl += "[maxvertexcount(4)]\n";
	hlsl += "void main(triangle GSIn input[3], inout TriangleStream<GSOut> stream)\n{\n";
	hlsl += "\tfloat dist0_1 = length(input[1].position.xy - input[0].position.xy);\n";
	hlsl += "\tfloat dist0_2 = length(input[2].position.xy - input[0].position.xy);\n";
	hlsl += "\tfloat dist1_2 = length(input[2].position.xy - input[1].position.xy);\n";
	hlsl += "\tif (dist0_1 > dist0_2 && dist0_1 > dist1_2)\n";
	hlsl += "\t\temitRect(2, 1, 0, 3, 0, input, stream);\n";
	hlsl += "\telse if (dist0_2 > dist0_1 && dist0_2 > dist1_2)\n";
	hlsl += "\t\temitRect(1, 2, 0, 3, 1, input, stream);\n";
	hlsl += "\telse\n";
	hlsl += "\t\temitRect(0, 1, 2, 3, 2, input, stream);\n";
	hlsl += "}\n";

	auto shader = std::make_unique<RendererShaderD3D12>(RendererShader::ShaderType::kGeometry, key, 0xD3120EC7ull, false, false, hlsl);
	auto* shaderPtr = shader.get();
	m_rectEmulationGeometryShaders.emplace(key, std::move(shader));
	cemuLog_logOnce(LogType::Force, "D3D12: using synthetic geometry shaders for native rectangle primitive emulation.");
	return shaderPtr;
}

#if defined(CEMU_UWP)
void D3D12Renderer::PrepareGamePipelineLibrary(uint64 titleId)
{
	if (m_gamePipelineLibraryTitleId != 0 && m_gamePipelineLibraryTitleId != titleId)
		ClearPendingGamePipelineRecipes();
	EnsureGamePipelineLibrary(titleId);
}

void D3D12Renderer::BeginGamePipelineRecipeLoading(uint64 titleId)
{
	ClearPendingGamePipelineRecipes();
	PrepareGamePipelineLibrary(titleId);
	m_gamePipelineRecipePrewarmAttempts = 0;
	m_gamePipelineRecipePrewarmAccepted = 0;
	m_gamePipelineRecipePrewarmRejected = 0;
	m_gamePipelineRecipePrewarmFailure = S_OK;
	m_gamePipelineRecipeBackground = false;
	m_gamePipelineRecipePrewarmStartTime = std::chrono::steady_clock::now();
	const auto memory = CemuUwp::MemoryBudgetService::SampleNow(true);
	m_gamePipelineRecipePrewarmStartUsage = memory.usageBytes;
	m_gamePipelineRecipePrewarmPeakUsage = memory.usageBytes;
	m_gamePipelineRecipePrewarmStopReason = "complete";
	m_gamePipelineRecipeLoadingTitleId = titleId;
	const bool loading = m_device && m_gameRootSignature && titleId != 0;
	m_gamePipelineRecipePrewarmStopped = !loading;
	m_gamePipelineRecipeLoading = loading;
	D3D12TraceEvent("game_pipeline_recipe_loading_begin", "title=0x{:016x} enabled={} cap={}",
		titleId,
		loading ? 1 : 0,
		D3D12PipelinePrewarmPolicy::kMaxRecipeCacheEntries);
	cemuLog_log(LogType::Force,
		"D3D12 UWP pipeline prewarm admission: cap={} startUsageMB={} limitMB={} reservedHeadroomMB={} maxGrowthMB={}.",
		D3D12PipelinePrewarmPolicy::kMaxRecipeCacheEntries, memory.usageBytes / (1024 * 1024),
		memory.limitBytes / (1024 * 1024), D3D12PipelinePrewarmPolicy::kPrewarmMemoryHeadroom / (1024 * 1024),
		D3D12PipelinePrewarmPolicy::kMaxPrewarmMemoryGrowth / (1024 * 1024));
}

void D3D12Renderer::StopGamePipelineRecipePrewarm(const char* reason)
{
	// the first reason wins; later stops are consequences of it
	if (!m_gamePipelineRecipePrewarmStopped.exchange(true, std::memory_order_acq_rel))
		m_gamePipelineRecipePrewarmStopReason.store(reason, std::memory_order_release);
}

void D3D12Renderer::RecordGamePipelineRecipePrewarmUsage(uint64 usageBytes)
{
	uint64 peak = m_gamePipelineRecipePrewarmPeakUsage.load(std::memory_order_relaxed);
	while (usageBytes > peak && !m_gamePipelineRecipePrewarmPeakUsage.compare_exchange_weak(peak, usageBytes, std::memory_order_relaxed))
	{
	}
}

D3D12Renderer::RecipePrewarmResult D3D12Renderer::PrewarmGamePipelineRecipe(uint64 expectedPipelineKey, std::span<const uint8> bytes)
{
	// Runs on prewarm workers next to the render thread, so only atomics and the hand-off list are shared.
	// The pending map and the live pipeline cache belong to the render thread; AdoptPrewarmedGamePipelines
	// drops results whose key went live meanwhile.
	const uint64 generation = m_gamePipelineRecipeGeneration.load(std::memory_order_acquire);
	if (IsGamePipelineRecipePrewarmStopped() || !m_device || !m_gameRootSignature)
		return RecipePrewarmResult::Stopped;
	if (expectedPipelineKey == 0 || bytes.empty())
		return RecipePrewarmResult::Rejected;

	m_gamePipelineRecipePrewarmAttempts.fetch_add(1, std::memory_order_relaxed);
	D3D12PipelineRecipe::OwnedRecipe recipe;
	const D3D12PipelineRecipe::Status decodeStatus = D3D12PipelineRecipe::Decode(bytes, recipe);
	if (decodeStatus != D3D12PipelineRecipe::Status::Success || recipe.pipelineKey != expectedPipelineKey)
	{
		m_gamePipelineRecipePrewarmRejected.fetch_add(1, std::memory_order_relaxed);
		if (decodeStatus == D3D12PipelineRecipe::Status::OutOfMemory)
			StopGamePipelineRecipePrewarm("decode_out_of_memory");
		static std::atomic_uint32_t s_recipeDecodeLogCount{ 0 };
		if (s_recipeDecodeLogCount.fetch_add(1, std::memory_order_relaxed) < 16)
		{
			cemuLog_log(LogType::Force,
				"D3D12 UWP: rejecting pipeline recipe key {:016x} (status={}, embedded key {:016x}).",
				expectedPipelineKey,
				D3D12PipelineRecipe::StatusName(decodeStatus),
				recipe.pipelineKey);
		}
		return decodeStatus == D3D12PipelineRecipe::Status::OutOfMemory ? RecipePrewarmResult::Stopped : RecipePrewarmResult::Rejected;
	}
#if defined(__ID3D12GraphicsCommandList9_INTERFACE_DEFINED__)
	if ((recipe.desc.Flags & D3D12_PIPELINE_STATE_FLAG_DYNAMIC_DEPTH_BIAS) != 0 && !m_dynamicDepthBiasSupported)
	{
		m_gamePipelineRecipePrewarmRejected.fetch_add(1, std::memory_order_relaxed);
		return RecipePrewarmResult::Rejected;
	}
#endif

	recipe.BindRootSignature(m_gameRootSignature.Get());
	Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline;
	HRESULT hr = E_FAIL;
	// every pipeline created this session counts toward the record limit, adopted or not
	const size_t createdPipelines = (size_t)m_gamePipelineRecipePrewarmAccepted.load(std::memory_order_relaxed);
	{
		RendererShaderD3D12::NativeCompilationSlot nativeCompilationSlot;
		const auto memory = CemuUwp::MemoryBudgetService::SampleNow(true);
		RecordGamePipelineRecipePrewarmUsage(memory.usageBytes);
		const uint64 effectiveLimit = memory.expectedLimitBytes != 0 ?
			std::min(memory.limitBytes, memory.expectedLimitBytes) : memory.limitBytes;
		const uint64 growthBaseline = m_gamePipelineRecipeBackground.load(std::memory_order_acquire) ?
			memory.usageBytes : m_gamePipelineRecipePrewarmStartUsage;
		const auto decision = D3D12PipelinePrewarmPolicy::CheckAdmission(memory.enabled || IsXboxConsole(),
			memory.heavyWorkDeferred, memory.pressure != CemuUwp::MemoryBudgetService::Pressure::Normal,
			memory.usageBytes, effectiveLimit, growthBaseline, createdPipelines);
		if (decision != D3D12PipelinePrewarmPolicy::Decision::Allow)
		{
			m_gamePipelineRecipePrewarmRejected.fetch_add(1, std::memory_order_relaxed);
			StopGamePipelineRecipePrewarm(D3D12PipelinePrewarmPolicy::DecisionName(decision));
			cemuLog_log(LogType::Force,
				"D3D12 UWP pipeline prewarm stopped: reason={} created={} usageMB={} limitMB={}.",
				D3D12PipelinePrewarmPolicy::DecisionName(decision), createdPipelines,
				memory.usageBytes / (1024 * 1024), memory.limitBytes / (1024 * 1024));
			return RecipePrewarmResult::Stopped;
		}
#if defined(__ID3D12GraphicsCommandList8_INTERFACE_DEFINED__)
		if (recipe.hasIndependentDepthStencil)
		{
			if (!m_independentFrontBackStencilSupported || !m_independentStencilDevice)
				hr = E_NOINTERFACE;
			else
				hr = CreateGraphicsPipelineWithIndependentStencil(
					m_independentStencilDevice.Get(), recipe.desc, recipe.independentDepthStencil, pipeline);
		}
		else
#endif
		{
			hr = m_device->CreateGraphicsPipelineState(&recipe.desc, IID_PPV_ARGS(&pipeline));
		}
		const auto afterMemory = CemuUwp::MemoryBudgetService::SampleNow(true);
		RecordGamePipelineRecipePrewarmUsage(afterMemory.usageBytes);
		const uint64 afterLimit = afterMemory.expectedLimitBytes != 0 ?
			std::min(afterMemory.limitBytes, afterMemory.expectedLimitBytes) : afterMemory.limitBytes;
		const auto afterDecision = D3D12PipelinePrewarmPolicy::CheckAdmission(afterMemory.enabled || IsXboxConsole(),
			afterMemory.heavyWorkDeferred, afterMemory.pressure != CemuUwp::MemoryBudgetService::Pressure::Normal,
			afterMemory.usageBytes, afterLimit, growthBaseline, createdPipelines);
		if (SUCCEEDED(hr) && pipeline && afterDecision != D3D12PipelinePrewarmPolicy::Decision::Allow)
		{
			// release the object that crossed the budget; EndLoading rechecks afterwards
			pipeline.Reset();
			m_gamePipelineRecipePrewarmRejected.fetch_add(1, std::memory_order_relaxed);
			StopGamePipelineRecipePrewarm(D3D12PipelinePrewarmPolicy::DecisionName(afterDecision));
			cemuLog_log(LogType::Force,
				"D3D12 UWP pipeline prewarm stopped after a creation: reason={} created={} usageMB={} limitMB={}.",
				D3D12PipelinePrewarmPolicy::DecisionName(afterDecision), createdPipelines,
				afterMemory.usageBytes / (1024 * 1024), afterMemory.limitBytes / (1024 * 1024));
			return RecipePrewarmResult::Stopped;
		}
	}
	if (FAILED(hr) || !pipeline)
	{
		m_gamePipelineRecipePrewarmRejected.fetch_add(1, std::memory_order_relaxed);
		StopGamePipelineRecipePrewarm("native_creation_failure");
		// device loss is handled on the render thread when it adopts the results
		HRESULT expected = S_OK;
		m_gamePipelineRecipePrewarmFailure.compare_exchange_strong(expected, FAILED(hr) ? hr : E_FAIL);
		m_prewarmedGamePipelinesAvailable.store(true, std::memory_order_release);
		static std::atomic_uint32_t s_recipeCreateFailureLogCount{ 0 };
		if (s_recipeCreateFailureLogCount.fetch_add(1, std::memory_order_relaxed) < 16)
		{
			cemuLog_log(LogType::Force,
				"D3D12 UWP: stopping pipeline recipe prewarm after key {:016x} failed (HRESULT 0x{:08x}, created {}).",
				expectedPipelineKey,
				(uint32)hr,
				createdPipelines);
		}
		return RecipePrewarmResult::Stopped;
	}

	SetD3D12ObjectName(pipeline.Get(), L"Cemu D3D12 prewarmed game PSO");
	try
	{
		PrewarmedGamePipeline prewarmed;
		prewarmed.generation = generation;
		prewarmed.pipelineKey = expectedPipelineKey;
		prewarmed.recipe.pipeline = std::move(pipeline);
		prewarmed.recipe.fingerprint = recipe.fingerprint;
		prewarmed.recipe.contentFingerprint = recipe.contentFingerprint;
		std::scoped_lock lock(m_prewarmedGamePipelinesMutex);
		m_prewarmedGamePipelines.emplace_back(std::move(prewarmed));
		m_prewarmedGamePipelinesAvailable.store(true, std::memory_order_release);
	}
	catch (const std::bad_alloc&)
	{
		m_gamePipelineRecipePrewarmRejected.fetch_add(1, std::memory_order_relaxed);
		StopGamePipelineRecipePrewarm("bookkeeping_out_of_memory");
		return RecipePrewarmResult::Stopped;
	}

	const uint64 accepted = m_gamePipelineRecipePrewarmAccepted.fetch_add(1, std::memory_order_relaxed) + 1;
	if ((accepted % 512) == 0)
		cemuLog_log(LogType::Force, "D3D12 UWP pipeline prewarm progress: created={} sampledPeakUsageMB={} elapsed_ms={}.",
			accepted, m_gamePipelineRecipePrewarmPeakUsage.load(std::memory_order_relaxed) / (1024 * 1024),
			std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - m_gamePipelineRecipePrewarmStartTime).count());
	return RecipePrewarmResult::Created;
}

ID3D12PipelineState* D3D12Renderer::FindGamePipelineByContent(const std::array<uint8, 32>& contentFingerprint, uint64& canonicalKey)
{
	const auto it = m_gamePipelineKeyByContent.find(contentFingerprint);
	if (it == m_gamePipelineKeyByContent.end())
		return nullptr;
	canonicalKey = it->second;
	if (const auto live = m_gamePipelineCache.find(canonicalKey); live != m_gamePipelineCache.end() && live->second.pipeline)
		return live->second.pipeline.Get();
	if (const auto pending = m_pendingGamePipelineRecipes.find(canonicalKey); pending != m_pendingGamePipelineRecipes.end() && pending->second.pipeline)
		return pending->second.pipeline.Get();
	// evicted or released since; the next build of this content registers itself again
	m_gamePipelineKeyByContent.erase(it);
	return nullptr;
}

size_t D3D12Renderer::AdoptPrewarmedGamePipelines()
{
	if (!m_prewarmedGamePipelinesAvailable.load(std::memory_order_acquire))
		return 0;
	std::vector<PrewarmedGamePipeline> prewarmed;
	{
		std::scoped_lock lock(m_prewarmedGamePipelinesMutex);
		prewarmed.swap(m_prewarmedGamePipelines);
		m_prewarmedGamePipelinesAvailable.store(false, std::memory_order_release);
	}
	const HRESULT failure = m_gamePipelineRecipePrewarmFailure.exchange(S_OK);
	if (FAILED(failure))
	{
		if (IsD3D12TransientMemoryFailure(failure))
			m_pendingGamePipelineRecipes.clear();
		CheckDeviceRemoved(failure, "CreateGraphicsPipelineState(recipe prewarm)");
	}

	const uint64 generation = m_gamePipelineRecipeGeneration.load(std::memory_order_acquire);
	size_t adopted = 0;
	for (auto& entry : prewarmed)
	{
		// a draw may have created the pipeline itself while the worker was busy with it
		if (entry.generation != generation || m_gamePipelineCache.contains(entry.pipelineKey) ||
			m_pendingGamePipelineRecipes.contains(entry.pipelineKey))
			continue;
		try
		{
			entry.recipe.adoptedFrame = LatteGPUState.frameCounter;
			if (entry.recipe.contentFingerprint != std::array<uint8, 32>{})
				m_gamePipelineKeyByContent.try_emplace(entry.recipe.contentFingerprint, entry.pipelineKey);
			m_pendingGamePipelineRecipes.emplace(entry.pipelineKey, std::move(entry.recipe));
			++adopted;
		}
		catch (const std::bad_alloc&)
		{
			m_pendingGamePipelineRecipes.clear();
			StopGamePipelineRecipePrewarm("bookkeeping_out_of_memory");
			break;
		}
	}
	return adopted;
}

void D3D12Renderer::EndGamePipelineRecipeLoading(bool continuesInBackground)
{
	AdoptPrewarmedGamePipelines();
	const auto memory = CemuUwp::MemoryBudgetService::SampleNow(true);
	RecordGamePipelineRecipePrewarmUsage(memory.usageBytes);
	const uint64 effectiveLimit = memory.expectedLimitBytes != 0 ?
		std::min(memory.limitBytes, memory.expectedLimitBytes) : memory.limitBytes;
	// Ignore the record count here: completing a full inventory is success.
	const auto decision = D3D12PipelinePrewarmPolicy::CheckAdmission(memory.enabled || IsXboxConsole(),
		memory.heavyWorkDeferred, memory.pressure != CemuUwp::MemoryBudgetService::Pressure::Normal,
		memory.usageBytes, effectiveLimit, m_gamePipelineRecipePrewarmStartUsage, 0);
	if (decision != D3D12PipelinePrewarmPolicy::Decision::Allow)
	{
		// none of the pending objects reached the GPU, so the whole set can be dropped
		m_pendingGamePipelineRecipes.clear();
		StopGamePipelineRecipePrewarm(D3D12PipelinePrewarmPolicy::DecisionName(decision));
	}
	const bool stopped = m_gamePipelineRecipePrewarmStopped.load(std::memory_order_acquire);
	D3D12TraceEvent("game_pipeline_recipe_loading_end",
		"title=0x{:016x} attempts={} accepted={} rejected={} pending={} stopped={} background={}",
		m_gamePipelineRecipeLoadingTitleId,
		m_gamePipelineRecipePrewarmAttempts.load(),
		m_gamePipelineRecipePrewarmAccepted.load(),
		m_gamePipelineRecipePrewarmRejected.load(),
		m_pendingGamePipelineRecipes.size(),
		stopped ? 1 : 0,
		continuesInBackground ? 1 : 0);
	cemuLog_log(LogType::Force,
		"D3D12 UWP pipeline recipe prewarm: title={:016x} attempts={} nativeCreated={} retained={} rejected={} stopped={} reason={} elapsed_ms={} usageMB={} sampledPeakUsageMB={} limitMB={}{}.",
		m_gamePipelineRecipeLoadingTitleId,
		m_gamePipelineRecipePrewarmAttempts.load(),
		m_gamePipelineRecipePrewarmAccepted.load(),
		m_pendingGamePipelineRecipes.size(),
		m_gamePipelineRecipePrewarmRejected.load(),
		stopped ? 1 : 0,
		m_gamePipelineRecipePrewarmStopReason.load(),
		std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - m_gamePipelineRecipePrewarmStartTime).count(),
		memory.usageBytes / (1024 * 1024), m_gamePipelineRecipePrewarmPeakUsage.load() / (1024 * 1024), memory.limitBytes / (1024 * 1024),
		continuesInBackground && !stopped ? "; the rest continues in the background" : "");
	if (!continuesInBackground || stopped)
		m_gamePipelineRecipeLoading = false;
	else
		m_gamePipelineRecipeBackground = true;
}

void D3D12Renderer::FinishBackgroundGamePipelineRecipePrewarm()
{
	cemuLog_log(LogType::Force,
		"D3D12 UWP background pipeline prewarm finished: title={:016x} attempts={} nativeCreated={} rejected={} stopped={} reason={} elapsed_ms={} sampledPeakUsageMB={}.",
		m_gamePipelineRecipeLoadingTitleId,
		m_gamePipelineRecipePrewarmAttempts.load(),
		m_gamePipelineRecipePrewarmAccepted.load(),
		m_gamePipelineRecipePrewarmRejected.load(),
		m_gamePipelineRecipePrewarmStopped.load() ? 1 : 0,
		m_gamePipelineRecipePrewarmStopReason.load(),
		std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - m_gamePipelineRecipePrewarmStartTime).count(),
		m_gamePipelineRecipePrewarmPeakUsage.load() / (1024 * 1024));
	m_gamePipelineRecipeLoading = false;
	m_gamePipelineRecipeBackground = false;
}

void D3D12Renderer::ClearPendingGamePipelineRecipes()
{
	// results still in flight from a worker belong to the old generation and are dropped on adoption
	m_gamePipelineRecipeGeneration.fetch_add(1, std::memory_order_acq_rel);
	{
		std::scoped_lock lock(m_prewarmedGamePipelinesMutex);
		m_prewarmedGamePipelines.clear();
		m_prewarmedGamePipelinesAvailable.store(false, std::memory_order_release);
	}
	m_pendingGamePipelineRecipes.clear();
	m_gamePipelineRecipeLoadingTitleId = 0;
	m_gamePipelineRecipeLoading = false;
	m_gamePipelineRecipeBackground = false;
	m_gamePipelineRecipePrewarmStopped = false;
	{
		std::scoped_lock lock(m_asyncPipelineMutex);
		m_asyncPipelineJobs.clear();
		m_asyncPipelineFailedKeys.clear();
	}
	m_asyncPipelinesInFlight.clear();
	m_asyncPipelineFailed.clear();
}

bool D3D12Renderer::QueueAsyncGamePipeline(uint64 pipelineKey, std::vector<uint8>&& recipe)
{
	try
	{
		std::scoped_lock lock(m_asyncPipelineMutex);
		if (m_asyncPipelineThreads.empty())
		{
			// PSO creation is mostly single threaded in the driver; two workers keep a burst moving
			// without taking cores from the shader compilers or the emulated CPU
			const uint32 threadCount = std::clamp<uint32>(RendererShaderD3D12::GetNativeCompilationConcurrency() / 2, 1u, 2u);
			m_asyncPipelineStop = false;
			for (uint32 i = 0; i < threadCount; ++i)
				m_asyncPipelineThreads.emplace_back(&D3D12Renderer::AsyncPipelineThreadFunc, this);
			cemuLog_log(LogType::Force, "D3D12: {} async pipeline creation threads.", threadCount);
		}
		AsyncPipelineJob job;
		job.generation = m_gamePipelineRecipeGeneration.load(std::memory_order_acquire);
		job.pipelineKey = pipelineKey;
		job.recipe = std::move(recipe);
		job.rootSignature = m_gameRootSignature;
		m_asyncPipelinesInFlight[pipelineKey] = job.generation;
		m_asyncPipelineJobs.emplace_back(std::move(job));
	}
	catch (const std::exception&)
	{
		m_asyncPipelinesInFlight.erase(pipelineKey);
		return false;
	}
	m_asyncPipelineCondition.notify_one();
	m_asyncPipelinesQueued++;
	return true;
}

void D3D12Renderer::DrainAsyncGamePipelineFailures()
{
	std::vector<uint64> failed;
	{
		std::scoped_lock lock(m_asyncPipelineMutex);
		if (m_asyncPipelineFailedKeys.empty())
			return;
		failed.swap(m_asyncPipelineFailedKeys);
	}
	for (const uint64 key : failed)
	{
		m_asyncPipelinesInFlight.erase(key);
		m_asyncPipelineFailed.insert(key);
	}
}

bool D3D12Renderer::IsAsyncGamePipelinePending(uint64 pipelineKey)
{
	auto it = m_asyncPipelinesInFlight.find(pipelineKey);
	if (it == m_asyncPipelinesInFlight.end())
		return false;
	DrainAsyncGamePipelineFailures();
	AdoptPrewarmedGamePipelines();
	it = m_asyncPipelinesInFlight.find(pipelineKey);
	if (it == m_asyncPipelinesInFlight.end())
		return false;
	// a dropped generation (title change, memory pressure) discards the result, so it has to be queued again
	if (it->second != m_gamePipelineRecipeGeneration.load(std::memory_order_acquire))
	{
		m_asyncPipelinesInFlight.erase(it);
		return false;
	}
	return !m_pendingGamePipelineRecipes.contains(pipelineKey);
}

void D3D12Renderer::ReleaseStaleAsyncGamePipelines()
{
	constexpr uint32 kSweepIntervalFrames = 300;
	constexpr uint32 kUnusedFrames = 1800; // ~30 s at 60 Hz
	static uint32 s_lastSweepFrame = 0;
	const uint32 frame = LatteGPUState.frameCounter;
	if (frame - s_lastSweepFrame < kSweepIntervalFrames)
		return;
	s_lastSweepFrame = frame;
	size_t released = 0;
	for (auto it = m_pendingGamePipelineRecipes.begin(); it != m_pendingGamePipelineRecipes.end();)
	{
		if (it->second.fromAsync && frame - it->second.adoptedFrame >= kUnusedFrames)
		{
			// a later draw with this key queues it again instead of waiting on a result that is gone
			m_asyncPipelinesInFlight.erase(it->first);
			it = m_pendingGamePipelineRecipes.erase(it); // never bound, so no GPU reference
			released++;
		}
		else
			++it;
	}
	m_asyncPipelinesReleasedUnused += released;
	if (released != 0)
	{
		cemuLog_log(LogType::Force, "D3D12 UWP: released {} async PSOs no draw used again within {} frames (total {}, pending {}).",
			released, kUnusedFrames, m_asyncPipelinesReleasedUnused, m_pendingGamePipelineRecipes.size());
	}
}

size_t D3D12Renderer::ReleaseUnusedPrewarmedPipelines(uint32 minAgeFrames, size_t maxPipelines, bool highPressure)
{
	const uint32 frame = LatteGPUState.frameCounter;
	size_t released = 0;
	auto& pipelineStableCache = D3D12PipelineStableCache::GetInstance();
	for (auto it = m_pendingGamePipelineRecipes.begin(); it != m_pendingGamePipelineRecipes.end() && released < maxPipelines;)
	{
		// kept: its draw cannot be skipped, so without it the render thread stalls 0.2-1.2 s (rain, Sheikah Slate)
		if (frame - it->second.adoptedFrame >= minAgeFrames && !pipelineStableCache.KeepUnderMemoryPressure(it->first, highPressure))
		{
			m_asyncPipelinesInFlight.erase(it->first);
			it = m_pendingGamePipelineRecipes.erase(it); // never bound, so no GPU reference
			++released;
		}
		else
			++it;
	}
	m_asyncPipelinesReleasedUnused += released;
	return released;
}

void D3D12Renderer::StopAsyncPipelineThreads()
{
	std::vector<std::thread> threads;
	{
		std::scoped_lock lock(m_asyncPipelineMutex);
		m_asyncPipelineStop = true;
		m_asyncPipelineJobs.clear();
		threads.swap(m_asyncPipelineThreads);
	}
	m_asyncPipelineCondition.notify_all();
	for (auto& thread : threads)
		thread.join();
}

void D3D12Renderer::AsyncPipelineThreadFunc()
{
	SetThreadName("d3d12AsyncPSO");
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
	while (true)
	{
		AsyncPipelineJob job;
		{
			std::unique_lock lock(m_asyncPipelineMutex);
			m_asyncPipelineCondition.wait(lock, [this]() { return m_asyncPipelineStop || !m_asyncPipelineJobs.empty(); });
			if (m_asyncPipelineStop)
				return;
			// above the memory guard a long queue gets one worker at a time: after a teleport 500-1200 queued PSOs kept
			// both workers busy and the game ran at ~24 FPS in light scenes until the queue drained (0.2.8.5 turned async
			// off instead). Short bursts (enemy deaths, drops, a new ability) keep both, or they pop in for seconds.
			constexpr size_t kGuardedQueueForOneWorker = 32;
			// prioritized caching keeps the whole cache live, so the guard is on most of the time after a few teleports and
			// a new area's pipelines took ~20 s one at a time (BOTW: 138 queued); with it, both workers run while the
			// pressure is still soft
			const bool bothWorkersUnderGuard = D3D12PipelineStableCache::GetInstance().IsPrioritized() &&
				CemuUwp::MemoryBudgetService::GetLatestSnapshot().pressure <= CemuUwp::MemoryBudgetService::Pressure::Soft;
			if (CemuUwp::MemoryBudgetService::IsHeavyWorkDeferred() && !bothWorkersUnderGuard &&
				m_asyncPipelineJobs.size() > kGuardedQueueForOneWorker &&
				m_asyncPipelineJobsOutstanding.load(std::memory_order_acquire) != 0)
			{
				m_asyncPipelineCondition.wait_for(lock, std::chrono::milliseconds(20));
				continue;
			}
			job = std::move(m_asyncPipelineJobs.front());
			m_asyncPipelineJobs.pop_front();
			// counted before the lock drops, so the queue and this count never both read empty mid-job
			m_asyncPipelineJobsOutstanding.fetch_add(1, std::memory_order_acq_rel);
		}
		stdx::scope_exit jobDone([this]() { m_asyncPipelineJobsOutstanding.fetch_sub(1, std::memory_order_acq_rel); });
		// stale jobs are dropped; the render thread notices the generation change and queues again
		if (job.generation != m_gamePipelineRecipeGeneration.load(std::memory_order_acquire))
			continue;

		HRESULT hr = E_FAIL;
		Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline;
		D3D12PipelineRecipe::OwnedRecipe recipe;
		try
		{
			if (D3D12PipelineRecipe::Decode(job.recipe, recipe) == D3D12PipelineRecipe::Status::Success &&
				recipe.pipelineKey == job.pipelineKey)
			{
				recipe.BindRootSignature(job.rootSignature.Get());
				// the render thread is blocked on a pipeline of its own: do not queue ahead of it in the driver. This used to
				// give up after 2 s, and a render-thread PSO then took 15 s behind a queue of 141 background builds (BOTW)
				uint32 waitedMs = 0;
				while (m_renderThreadPipelineCreates.load(std::memory_order_acquire) != 0 && !m_asyncPipelineStop)
				{
					std::this_thread::sleep_for(std::chrono::milliseconds(1));
					++waitedMs;
				}
				if (waitedMs >= 2000 && m_slowAsyncPipelineLogs.fetch_add(1, std::memory_order_relaxed) < 100)
					cemuLog_log(LogType::Force, "D3D12: background PSO waited {} ms for a render-thread PSO.", waitedMs);
				const auto createStarted = std::chrono::steady_clock::now();
				hr = m_device->CreateGraphicsPipelineState(&recipe.desc, IID_PPV_ARGS(&pipeline));
				const uint64 createMs = (uint64)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - createStarted).count();
				if (createMs >= 500 && m_slowAsyncPipelineLogs.fetch_add(1, std::memory_order_relaxed) < 100)
				{
					size_t queuedJobs;
					{
						std::scoped_lock lock(m_asyncPipelineMutex);
						queuedJobs = m_asyncPipelineJobs.size();
					}
					cemuLog_log(LogType::Force, "D3D12: background PSO took {} ms (queued jobs {}).", createMs, queuedJobs);
				}
			}
		}
		catch (const std::exception&)
		{
			hr = E_OUTOFMEMORY;
		}

		bool delivered = false;
		if (SUCCEEDED(hr) && pipeline)
		{
			SetD3D12ObjectName(pipeline.Get(), L"Cemu D3D12 async game PSO");
			try
			{
				PrewarmedGamePipeline result;
				result.generation = job.generation;
				result.pipelineKey = job.pipelineKey;
				result.recipe.pipeline = std::move(pipeline);
				result.recipe.fingerprint = recipe.fingerprint;
				result.recipe.contentFingerprint = recipe.contentFingerprint;
				result.recipe.fromAsync = true;
				std::scoped_lock lock(m_prewarmedGamePipelinesMutex);
				m_prewarmedGamePipelines.emplace_back(std::move(result));
				m_prewarmedGamePipelinesAvailable.store(true, std::memory_order_release);
				delivered = true;
				m_asyncPipelinesCreated.fetch_add(1, std::memory_order_relaxed);
			}
			catch (const std::exception&)
			{
			}
		}
		if (!delivered)
		{
			// the synchronous path retries it and handles device loss or memory recovery
			std::scoped_lock lock(m_asyncPipelineMutex);
			m_asyncPipelineFailedKeys.push_back(job.pipelineKey);
		}
	}
}

bool D3D12Renderer::EnsureGamePipelineLibrary(uint64 titleId)
{
	if (!m_device || titleId == 0)
		return false;
	if (m_gamePipelineLibraryTitleId == titleId)
		return m_gamePipelineLibrary != nullptr;

	PersistGamePipelineLibrary();
	m_gamePipelineLibrary.Reset();
	m_gamePipelineLibraryBlob.clear();
	m_gamePipelineLibraryTitleId = titleId;
	m_gamePipelineLibraryDirty = false;
	if (IsXboxConsole())
	{
		// Native pipeline-library operations remove the graphics device on Xbox UWP.
		EnsureSinglePsoCacheSupport();
		cemuLog_logOnce(LogType::Force,
			"D3D12 UWP: native pipeline-cache APIs are unavailable on Xbox; using direct PSO creation plus Cemu's stable shader cache.");
		D3D12TraceEvent("game_pipeline_library_xbox_veto", "title=0x{:016x}", titleId);
		return false;
	}

	Microsoft::WRL::ComPtr<ID3D12Device1> device1;
	if (FAILED(m_device.As(&device1)))
		return false;

	D3D12_FEATURE_DATA_SHADER_CACHE shaderCacheSupport{};
	const HRESULT supportHr = m_device->CheckFeatureSupport(
		D3D12_FEATURE_SHADER_CACHE, &shaderCacheSupport, sizeof(shaderCacheSupport));
	if (IsD3D12DeviceFailure(supportHr))
	{
		CheckDeviceRemoved(supportHr, "CheckFeatureSupport(D3D12_FEATURE_SHADER_CACHE)");
		throw std::runtime_error("D3D12 device failed while querying native pipeline-library support");
	}
	if (FAILED(supportHr) ||
		(shaderCacheSupport.SupportFlags & D3D12_SHADER_CACHE_SUPPORT_LIBRARY) == 0)
	{
		cemuLog_logOnce(LogType::Force,
			"D3D12 UWP: native pipeline libraries are not supported by this device "
			"(HRESULT 0x{:08x}, support flags 0x{:08x}); using the regular PSO and stable shader caches.",
			(uint32)supportHr, (uint32)shaderCacheSupport.SupportFlags);
		D3D12TraceEvent("game_pipeline_library_unsupported", "title=0x{:016x} hr=0x{:08x} supportFlags=0x{:08x}",
			titleId, (uint32)supportHr, (uint32)shaderCacheSupport.SupportFlags);
		return false;
	}

	const fs::path path = ActiveSettings::GetCachePath("shaderCache/transferable/{:016x}_d3d12_native_pso.bin", titleId);
	std::ifstream input(path, std::ios::binary | std::ios::ate);
	if (input)
	{
		const std::streamoff size = input.tellg();
		constexpr std::streamoff kMaxPipelineLibraryBytes = 512ll * 1024ll * 1024ll;
		if (size > 0 && size <= kMaxPipelineLibraryBytes)
		{
			m_gamePipelineLibraryBlob.resize((size_t)size);
			input.seekg(0, std::ios::beg);
			if (!input.read(reinterpret_cast<char*>(m_gamePipelineLibraryBlob.data()), size))
				m_gamePipelineLibraryBlob.clear();
		}
	}
	HRESULT hr = device1->CreatePipelineLibrary(
		m_gamePipelineLibraryBlob.empty() ? nullptr : m_gamePipelineLibraryBlob.data(),
		m_gamePipelineLibraryBlob.size(), IID_PPV_ARGS(&m_gamePipelineLibrary));
	if (IsD3D12DeviceFailure(hr))
	{
		CheckDeviceRemoved(hr, "CreatePipelineLibrary(game)");
		m_gamePipelineLibrary.Reset();
		m_gamePipelineLibraryBlob.clear();
		throw std::runtime_error("D3D12 device failed while opening the native pipeline library");
	}
	if (FAILED(hr) && !m_gamePipelineLibraryBlob.empty() && IsStalePipelineLibraryError(hr))
	{
		// driver/runtime updates invalidate serialized libraries; start a fresh one
		m_gamePipelineLibrary.Reset();
		m_gamePipelineLibraryBlob.clear();
		hr = device1->CreatePipelineLibrary(nullptr, 0, IID_PPV_ARGS(&m_gamePipelineLibrary));
		if (IsD3D12DeviceFailure(hr))
		{
			CheckDeviceRemoved(hr, "CreatePipelineLibrary(game replacement)");
			m_gamePipelineLibrary.Reset();
			throw std::runtime_error("D3D12 device failed while replacing a stale native pipeline library");
		}
		m_gamePipelineLibraryDirty = SUCCEEDED(hr);
	}
	if (FAILED(hr))
	{
		m_gamePipelineLibrary.Reset();
		m_gamePipelineLibraryBlob.clear();
		cemuLog_logOnce(LogType::Force, "D3D12 UWP: native pipeline-library caching is unavailable (HRESULT 0x{:08x}).", (uint32)hr);
		return false;
	}

	D3D12TraceEvent("game_pipeline_library_open", "title=0x{:016x} serializedBytes={} staleReplaced={}",
		titleId, m_gamePipelineLibraryBlob.size(), m_gamePipelineLibraryDirty ? 1 : 0);
	return true;
}

void D3D12Renderer::PersistGamePipelineLibrary()
{
	if (!m_gamePipelineLibrary || !m_gamePipelineLibraryDirty || m_gamePipelineLibraryTitleId == 0)
		return;

	const SIZE_T size = m_gamePipelineLibrary->GetSerializedSize();
	if (size == 0 || size > 512ull * 1024ull * 1024ull)
		return;
	std::vector<uint8> serialized(size);
	const HRESULT hr = m_gamePipelineLibrary->Serialize(serialized.data(), serialized.size());
	if (FAILED(hr))
	{
		CheckDeviceRemoved(hr, "SerializePipelineLibrary(game)");
		cemuLog_log(LogType::Force, "D3D12 UWP: failed to serialize the native pipeline library (HRESULT 0x{:08x}).", (uint32)hr);
		return;
	}

	const fs::path path = ActiveSettings::GetCachePath("shaderCache/transferable/{:016x}_d3d12_native_pso.bin", m_gamePipelineLibraryTitleId);
	const fs::path temporaryPath = path.wstring() + L".tmp";
	std::error_code ec;
	fs::create_directories(path.parent_path(), ec);
	std::ofstream output(temporaryPath, std::ios::binary | std::ios::trunc);
	if (!output || !output.write(reinterpret_cast<const char*>(serialized.data()), serialized.size()))
	{
		cemuLog_log(LogType::Force, "D3D12 UWP: failed to write native pipeline library {}.", _pathToUtf8(path));
		return;
	}
	output.close();
	ec.clear();
	const bool destinationExists = fs::exists(path, ec);
	if (ec)
	{
		cemuLog_log(LogType::Force, "D3D12 UWP: failed to install native pipeline library {}: {}.", _pathToUtf8(path), ec.message());
		return;
	}

	const BOOL installed = destinationExists ?
		ReplaceFileFromAppW(path.c_str(), temporaryPath.c_str(), nullptr, 0, nullptr, nullptr) :
		MoveFileFromAppW(temporaryPath.c_str(), path.c_str());
	if (!installed)
	{
		const std::error_code installError((int)GetLastError(), std::system_category());
		cemuLog_log(LogType::Force, "D3D12 UWP: failed to atomically install native pipeline library {}: {}.",
			_pathToUtf8(path), installError.message());
		return;
	}
	m_gamePipelineLibraryDirty = false;
	D3D12TraceEvent("game_pipeline_library_saved", "title=0x{:016x} serializedBytes={} entries={}",
		m_gamePipelineLibraryTitleId, serialized.size(), m_gamePipelineLibraryStores);
}

bool D3D12Renderer::EnsureSinglePsoCacheSupport()
{
	if (m_singlePsoCacheSupportQueried)
		return m_singlePsoCacheSupported;
	m_singlePsoCacheSupportQueried = true;
	m_singlePsoCacheSupported = false;
	m_singlePsoCacheStoreEnabled = false;
	if (!m_device)
		return false;
	if (IsXboxConsole())
	{
		// GetCachedBlob removes the graphics device on Xbox despite advertised support.
		D3D12TraceEvent("game_pso_blob_xbox_veto", "reason=GetCachedBlob_device_removed");
		return false;
	}

	D3D12_FEATURE_DATA_SHADER_CACHE support{};
	const HRESULT hr = m_device->CheckFeatureSupport(D3D12_FEATURE_SHADER_CACHE, &support, sizeof(support));
	if (IsD3D12DeviceFailure(hr))
	{
		CheckDeviceRemoved(hr, "CheckFeatureSupport(D3D12_FEATURE_SHADER_CACHE single PSO)");
		throw std::runtime_error("D3D12 device failed while querying per-PSO cache support");
	}
	m_singlePsoCacheSupported = SUCCEEDED(hr) &&
		(support.SupportFlags & D3D12_SHADER_CACHE_SUPPORT_SINGLE_PSO) != 0;
	m_singlePsoCacheStoreEnabled = m_singlePsoCacheSupported;
	D3D12TraceEvent("game_pso_blob_support", "hr=0x{:08x} supportFlags=0x{:08x} enabled={}",
		(uint32)hr, (uint32)support.SupportFlags, m_singlePsoCacheSupported ? 1 : 0);
	return m_singlePsoCacheSupported;
}

uint64 D3D12Renderer::BuildGamePipelineFingerprint(uint64 pipelineKey, const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc) const
{
	uint64 fingerprint = MixGameDescriptorTableKey(0xd312ca5e9f501001ull, kD3D12GamePsoSchemaVersion);
	fingerprint = MixGameDescriptorTableKey(fingerprint, pipelineKey);
	const auto mixBytecode = [&fingerprint](const D3D12_SHADER_BYTECODE& bytecode)
	{
		fingerprint = MixGameDescriptorTableKey(fingerprint, bytecode.BytecodeLength);
		if (bytecode.pShaderBytecode && bytecode.BytecodeLength != 0)
			fingerprint = MixGameStateBytes(fingerprint, bytecode.pShaderBytecode, bytecode.BytecodeLength);
	};
	mixBytecode(desc.VS);
	mixBytecode(desc.GS);
	mixBytecode(desc.HS);
	mixBytecode(desc.DS);
	mixBytecode(desc.PS);
	return fingerprint;
}

bool D3D12Renderer::LoadGamePipelineBlob(uint64 pipelineKey, uint64 fingerprint, std::vector<uint8>& blob)
{
	blob.clear();
	if (!m_singlePsoCacheSupported || m_gamePipelineLibraryTitleId == 0)
		return false;

	const fs::path path = ActiveSettings::GetCachePath(
		"shaderCache/driver/d3d12/{:016x}/{:016x}_{:016x}.bin",
		m_gamePipelineLibraryTitleId, pipelineKey, fingerprint);
	std::ifstream input(path, std::ios::binary | std::ios::ate);
	if (!input)
	{
		m_singlePsoCacheMisses++;
		return false;
	}

	const std::streamoff fileSize = input.tellg();
	input.seekg(0, std::ios::beg);
	D3D12GamePsoBlobHeader header{};
	const bool headerRead = fileSize >= (std::streamoff)sizeof(header) &&
		!!input.read(reinterpret_cast<char*>(&header), sizeof(header));
	const bool valid = headerRead &&
		header.magic == kD3D12GamePsoBlobMagic &&
		header.version == kD3D12GamePsoBlobVersion &&
		header.headerSize == sizeof(header) &&
		header.titleId == m_gamePipelineLibraryTitleId &&
		header.pipelineKey == pipelineKey &&
		header.fingerprint == fingerprint &&
		header.adapterLuidLow == m_adapterLuid.LowPart &&
		header.adapterLuidHigh == m_adapterLuid.HighPart &&
		header.blobSize != 0 && header.blobSize <= kD3D12GamePsoBlobMaxBytes &&
		(uint64)fileSize == sizeof(header) + header.blobSize;
	if (!valid)
	{
		input.close();
		std::error_code ec;
		fs::remove(path, ec);
		m_singlePsoCacheRejects++;
		m_singlePsoCacheMisses++;
		return false;
	}

	blob.resize((size_t)header.blobSize);
	if (!input.read(reinterpret_cast<char*>(blob.data()), (std::streamsize)blob.size()))
	{
		blob.clear();
		input.close();
		std::error_code ec;
		fs::remove(path, ec);
		m_singlePsoCacheRejects++;
		m_singlePsoCacheMisses++;
		return false;
	}
	return true;
}

void D3D12Renderer::RemoveGamePipelineBlob(uint64 pipelineKey, uint64 fingerprint)
{
	if (m_gamePipelineLibraryTitleId == 0)
		return;
	const fs::path path = ActiveSettings::GetCachePath(
		"shaderCache/driver/d3d12/{:016x}/{:016x}_{:016x}.bin",
		m_gamePipelineLibraryTitleId, pipelineKey, fingerprint);
	std::error_code ec;
	fs::remove(path, ec);
}

void D3D12Renderer::StoreGamePipelineBlob(uint64 pipelineKey, uint64 fingerprint, ID3D12PipelineState* pipeline)
{
	if (!pipeline || !m_singlePsoCacheSupported || !m_singlePsoCacheStoreEnabled || m_gamePipelineLibraryTitleId == 0)
		return;

	Microsoft::WRL::ComPtr<ID3DBlob> cachedBlob;
	const HRESULT blobHr = pipeline->GetCachedBlob(&cachedBlob);
	if (IsD3D12DeviceFailure(blobHr))
	{
		CheckDeviceRemoved(blobHr, "GetCachedBlob(game)");
		throw std::runtime_error("D3D12 device failed while retrieving a cached game PSO");
	}
	if (FAILED(blobHr) || !cachedBlob || cachedBlob->GetBufferSize() == 0 ||
		cachedBlob->GetBufferSize() > kD3D12GamePsoBlobMaxBytes)
	{
		m_singlePsoCacheStoreEnabled = false;
		cemuLog_logOnce(LogType::Force,
			"D3D12 UWP: per-PSO driver blob persistence is unavailable for this session (HRESULT 0x{:08x}).",
			(uint32)blobHr);
		return;
	}

	D3D12GamePsoBlobHeader header{};
	header.magic = kD3D12GamePsoBlobMagic;
	header.version = kD3D12GamePsoBlobVersion;
	header.headerSize = sizeof(header);
	header.titleId = m_gamePipelineLibraryTitleId;
	header.pipelineKey = pipelineKey;
	header.fingerprint = fingerprint;
	header.adapterLuidLow = m_adapterLuid.LowPart;
	header.adapterLuidHigh = m_adapterLuid.HighPart;
	header.blobSize = cachedBlob->GetBufferSize();

	const fs::path path = ActiveSettings::GetCachePath(
		"shaderCache/driver/d3d12/{:016x}/{:016x}_{:016x}.bin",
		m_gamePipelineLibraryTitleId, pipelineKey, fingerprint);
	const fs::path temporaryPath = path.wstring() + L".tmp";
	std::error_code ec;
	fs::create_directories(path.parent_path(), ec);
	std::ofstream output(temporaryPath, std::ios::binary | std::ios::trunc);
	if (!output ||
		!output.write(reinterpret_cast<const char*>(&header), sizeof(header)) ||
		!output.write(reinterpret_cast<const char*>(cachedBlob->GetBufferPointer()), (std::streamsize)cachedBlob->GetBufferSize()))
	{
		output.close();
		fs::remove(temporaryPath, ec);
		return;
	}
	output.close();
	ec.clear();
	const bool destinationExists = fs::exists(path, ec);
	if (ec)
	{
		fs::remove(temporaryPath, ec);
		return;
	}
	const BOOL installed = destinationExists ?
		ReplaceFileFromAppW(path.c_str(), temporaryPath.c_str(), nullptr, 0, nullptr, nullptr) :
		MoveFileFromAppW(temporaryPath.c_str(), path.c_str());
	if (!installed)
	{
		fs::remove(temporaryPath, ec);
		return;
	}
	m_singlePsoCacheStores++;
}
#endif

ID3D12PipelineState* D3D12Renderer::GetGamePipeline(const D3D12PipelineCompiler::PipelineIdentity& identity,
													LatteDecompilerShader* vertexShader,
													LatteDecompilerShader* geometryShader,
													LatteDecompilerShader* pixelShader,
													Latte::LATTE_VGT_PRIMITIVE_TYPE::E_PRIMITIVE_TYPE primitiveMode,
											bool noRasterizationOutput,
											Renderer::INDEX_TYPE hostIndexType,
											StencilFallbackPass stencilFallbackPass)
{
	const auto logUnavailable = [this, &identity, vertexShader, geometryShader, pixelShader](uint32 reason, const char* message)
	{
		D3D12TraceEvent("game_pipeline_unavailable", "reason={} message={} identity={} stateHash=0x{:016x} rootSig={} fbo=0x{:016x} fboSize={}x{} colorCount={} colorMask=0x{:02x} hasDepth={} vs=0x{:016x} gs=0x{:016x} ps=0x{:016x}",
						reason,
						message,
						identity.valid ? 1 : 0,
						identity.valid ? identity.pipelineStateHash : 0,
						m_gameRootSignature ? 1 : 0,
						m_activeFBO ? m_activeFBO->key : 0,
						m_activeFBO ? m_activeFBO->m_size.x : 0,
						m_activeFBO ? m_activeFBO->m_size.y : 0,
						m_activeFBO ? m_activeFBO->GetColorAttachmentCount() : 0,
						m_activeFBO ? m_activeFBO->GetColorAttachmentMask() : 0,
						m_activeFBO && m_activeFBO->HasDepthStencilAttachment() ? 1 : 0,
						vertexShader ? vertexShader->baseHash : 0,
						geometryShader ? geometryShader->baseHash : 0,
						pixelShader ? pixelShader->baseHash : 0);
		uint64 signature = reason;
		signature = std::rotl(signature, 7) ^ (identity.valid ? identity.pipelineStateHash : 0);
		signature = std::rotl(signature, 7) ^ (m_gameRootSignature ? 0x10001ull : 0);
		signature = std::rotl(signature, 7) ^ (m_activeFBO ? 0x20002ull : 0);
		signature = std::rotl(signature, 7) ^ (vertexShader ? vertexShader->baseHash : 0);
		signature = std::rotl(signature, 7) ^ (geometryShader ? geometryShader->baseHash : 0);
		signature = std::rotl(signature, 7) ^ (pixelShader ? pixelShader->baseHash : 0);
		if (reason >= 2)
			m_gamePipelineShaderCodeMissing = true;
		if (!ShouldLogDrawFailure(signature))
			return;
		cemuLog_log(LogType::Force, "D3D12: game PSO unavailable: {} (identity={}, rootSig={}, fbo={}, vs={:016x}, gs={:016x}, ps={:016x}).",
					message,
					identity.valid ? "valid" : "invalid",
					m_gameRootSignature ? "yes" : "no",
					m_activeFBO ? "yes" : "no",
					vertexShader ? vertexShader->baseHash : 0,
					geometryShader ? geometryShader->baseHash : 0,
					pixelShader ? pixelShader->baseHash : 0);
	};

	if (!identity.valid || !m_gameRootSignature || (!m_activeFBO && !noRasterizationOutput) || !vertexShader)
	{
		logUnavailable(1, "state prerequisites are incomplete");
		return nullptr;
	}

	const bool usePointExpansionGS = !noRasterizationOutput && ShouldUsePointExpansionGeometryShader(geometryShader, primitiveMode);
	const bool rasterizesPointQuads = usePointExpansionGS || (geometryShader && LatteDecompiler_HLSLGeometryShaderExpandsPoints(RecordRegWords()));
	const bool useRectEmulationGS = !noRasterizationOutput && geometryShader == nullptr && primitiveMode == Latte::LATTE_VGT_PRIMITIVE_TYPE::E_PRIMITIVE_TYPE::RECTS;
	using LattePrimitive = Latte::LATTE_VGT_PRIMITIVE_TYPE::E_PRIMITIVE_TYPE;
	const bool usesFixedRestartTopology = primitiveMode == LattePrimitive::TRIANGLE_STRIP ||
								 primitiveMode == LattePrimitive::TRIANGLE_STRIP_ADJACENT ||
								 primitiveMode == LattePrimitive::LINE_STRIP ||
								 primitiveMode == LattePrimitive::LINE_LOOP ||
								 primitiveMode == LattePrimitive::LINE_STRIP_ADJACENT ||
								 primitiveMode == LattePrimitive::TRIANGLE_FAN;
	const bool primitiveRestartEnabled = GetD3D12PrimitiveRestartEnabled(RecordRegs(), primitiveMode, hostIndexType);
	const uint32 primitiveRestartIndex = RecordRegs().VGT_MULTI_PRIM_IB_RESET_INDX.get_RESTART_INDEX();
	const bool hostStreamContainsRestartCuts = hostIndexType == Renderer::INDEX_TYPE::U32 ||
		(hostIndexType == Renderer::INDEX_TYPE::U16 && primitiveRestartIndex <= std::numeric_limits<uint16>::max());
	D3D12_INDEX_BUFFER_STRIP_CUT_VALUE stripCutValue = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;
	if (primitiveRestartEnabled && usesFixedRestartTopology && hostStreamContainsRestartCuts && hostIndexType == Renderer::INDEX_TYPE::U16)
		stripCutValue = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFF;
	else if (primitiveRestartEnabled && usesFixedRestartTopology && hostStreamContainsRestartCuts && hostIndexType == Renderer::INDEX_TYPE::U32)
		stripCutValue = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFFFFFF;
	const auto derivePipelineKey = [&](uint64 key)
	{
		if (stripCutValue != D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED)
			key = std::rotl(key, 13) ^ (0xD312C071ull + (uint64)stripCutValue);
		if (noRasterizationOutput)
			key = std::rotl(key, 11) ^ 0xD3125EEDull;
		if (stencilFallbackPass != StencilFallbackPass::None)
			key = std::rotl(key, 13) ^ (0xD31257E0C1000000ull + static_cast<uint64>(stencilFallbackPass));
		if (usePointExpansionGS)
		{
			key = std::rotl(key, 9) ^ 0xD3129017ull;
			key = std::rotl(key, 7) ^ vertexShader->outputParameterMask;
			key = std::rotl(key, 7) ^ (pixelShader ? pixelShader->baseHash : 0);
		}
		if (useRectEmulationGS)
		{
			key = std::rotl(key, 9) ^ 0xD3120EC7ull;
			key = std::rotl(key, 7) ^ vertexShader->outputParameterMask;
			key = std::rotl(key, 7) ^ (pixelShader ? pixelShader->baseHash : 0);
		}

		return key;
	};
	const uint64 pipelineKey = derivePipelineKey(identity.pipelineStateHash);
	// a ready pipeline differing only in baked depth bias stands in while the exact one is built
	const bool hasBakedDepthBias = RecordRegs().PA_SU_SC_MODE_CNTL.get_OFFSET_FRONT_ENABLED();
	const auto depthBiasSiblingKey = [&]()
	{
		return derivePipelineKey(D3D12PipelineCompiler::CalculateGraphicsPipelineHash(s_recordFetchShader,
			vertexShader, geometryShader, pixelShader, m_activeFBO, RecordRegs(), true));
	};
	const float drawBiasSlope = RecordRegs().PA_SU_POLY_OFFSET_FRONT_SCALE.get_SCALE();
	const float drawBiasOffset = RecordRegs().PA_SU_POLY_OFFSET_FRONT_OFFSET.get_OFFSET();
	const float drawBiasClamp = RecordRegs().PA_SU_POLY_OFFSET_CLAMP.get_CLAMP();
	// BOTW drifts its shadow bias every frame; within 1/16 (offset: or one depth unit) the difference is invisible
	const auto biasClose = [](float a, float b, float absoluteSlack)
	{
		return std::fabs(a - b) <= std::max(std::max(std::fabs(a), std::fabs(b)) * (1.0f / 16.0f), absoluteSlack);
	};
	const auto findDepthBiasSibling = [&](bool requireClose = false) -> ID3D12PipelineState*
	{
		if (!hasBakedDepthBias)
			return nullptr;
		const auto siblingIt = m_depthBiasSiblingPipelines.find(depthBiasSiblingKey());
		if (siblingIt == m_depthBiasSiblingPipelines.end())
			return nullptr;
		const auto& sibling = siblingIt->second;
		if (requireClose && !(biasClose(sibling.slope, drawBiasSlope, 0.0f) && biasClose(sibling.offset, drawBiasOffset, 1.0f) && sibling.clamp == drawBiasClamp))
			return nullptr;
		const auto pipelineIt = m_gamePipelineCache.find(sibling.pipelineKey);
		if (pipelineIt == m_gamePipelineCache.end() || !pipelineIt->second.pipeline)
			return nullptr; // evicted or failed
		pipelineIt->second.lastUsedFrame = LatteGPUState.frameCounter;
		if (!requireClose)
			++m_depthBiasSiblingDraws;
		return pipelineIt->second.pipeline.Get();
	};

	auto it = m_gamePipelineCache.find(pipelineKey);
	if (it != m_gamePipelineCache.end())
	{
		D3D12TraceEvent("game_pipeline_cache_hit", "pipelineKey=0x{:016x} stateHash=0x{:016x} primitive={} noRaster={} pointGS={} rectGS={} fbo=0x{:016x} vs=0x{:016x} gs=0x{:016x} ps=0x{:016x} pipeline={}",
						pipelineKey,
						identity.pipelineStateHash,
						(uint32)primitiveMode,
						noRasterizationOutput ? 1 : 0,
						usePointExpansionGS ? 1 : 0,
						useRectEmulationGS ? 1 : 0,
						m_activeFBO ? m_activeFBO->key : 0,
						vertexShader ? vertexShader->baseHash : 0,
						geometryShader ? geometryShader->baseHash : 0,
						pixelShader ? pixelShader->baseHash : 0,
						(const void*)it->second.pipeline.Get());
		it->second.lastUsedFrame = LatteGPUState.frameCounter;
		return it->second.pipeline.Get();
	}
	if (auto retryIt = m_gamePipelineRetryFrame.find(pipelineKey); retryIt != m_gamePipelineRetryFrame.end())
	{
		if (static_cast<sint32>(LatteGPUState.frameCounter - retryIt->second) < 0)
			return nullptr;
		m_gamePipelineRetryFrame.erase(retryIt);
	}

	auto* d3dVertexShader = static_cast<RendererShaderD3D12*>(vertexShader->shader);
	auto* d3dGeometryShader = geometryShader ? static_cast<RendererShaderD3D12*>(geometryShader->shader) : nullptr;
	auto* d3dPointExpansionShader = usePointExpansionGS ? GetPointExpansionGeometryShader(vertexShader, pixelShader) : nullptr;
	auto* d3dRectEmulationShader = useRectEmulationGS ? GetRectEmulationGeometryShader(vertexShader, pixelShader) : nullptr;
	auto* d3dPixelShader = pixelShader ? static_cast<RendererShaderD3D12*>(pixelShader->shader) : nullptr;
	if (!d3dVertexShader)
	{
		logUnavailable(2, "vertex renderer shader is missing");
		return nullptr;
	}
	d3dVertexShader->RequestCompilationPriority();
	if (d3dGeometryShader)
		d3dGeometryShader->RequestCompilationPriority();
	if (d3dPointExpansionShader)
		d3dPointExpansionShader->RequestCompilationPriority();
	if (d3dRectEmulationShader)
		d3dRectEmulationShader->RequestCompilationPriority();
	if (d3dPixelShader && !noRasterizationOutput)
		d3dPixelShader->RequestCompilationPriority();

	// a ready pipeline with a near-identical bias serves this key for good: no build, no recipe
	if (auto* closeSibling = !m_pendingGamePipelineRecipes.contains(pipelineKey) ? findDepthBiasSibling(true) : nullptr)
	{
		m_gamePipelineCache.emplace(pipelineKey, GamePipelineEntry{ Microsoft::WRL::ComPtr<ID3D12PipelineState>(closeSibling), LatteGPUState.frameCounter });
		++m_depthBiasAliases;
		return closeSibling;
	}

	if (m_allowAsyncShaderSkip)
	{
		const auto pending = [](RendererShaderD3D12* shader) { return shader && !shader->IsCompiled(); };
		if (pending(d3dVertexShader) || pending(d3dGeometryShader) || pending(d3dPointExpansionShader) ||
			pending(d3dRectEmulationShader) || (!noRasterizationOutput && pending(d3dPixelShader)))
		{
			m_gamePipelineAsyncPending = true;
			return nullptr;
		}
		if (IsAsyncGamePipelinePending(pipelineKey))
		{
			if (auto* sibling = findDepthBiasSibling())
				return sibling;
			m_gamePipelineAsyncPending = true;
			return nullptr;
		}
	}

	if (!d3dVertexShader->WaitForCompiled() || !d3dVertexShader->HasDXIL())
	{
		logUnavailable(3, "vertex DXIL is missing");
		return nullptr;
	}
	if (d3dGeometryShader && (!d3dGeometryShader->WaitForCompiled() || !d3dGeometryShader->HasDXIL()))
	{
		logUnavailable(4, "geometry DXIL is missing");
		return nullptr;
	}
	if (usePointExpansionGS && (!d3dPointExpansionShader || !d3dPointExpansionShader->WaitForCompiled() || !d3dPointExpansionShader->HasDXIL()))
	{
		logUnavailable(6, "point expansion geometry DXIL is missing");
		return nullptr;
	}
	if (useRectEmulationGS && (!d3dRectEmulationShader || !d3dRectEmulationShader->WaitForCompiled() || !d3dRectEmulationShader->HasDXIL()))
	{
		logUnavailable(7, "rectangle emulation geometry DXIL is missing");
		return nullptr;
	}
	if (d3dPixelShader && !noRasterizationOutput && (!d3dPixelShader->WaitForCompiled() || !d3dPixelShader->HasDXIL()))
	{
		logUnavailable(5, "pixel DXIL is missing");
		return nullptr;
	}

	D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
	desc.pRootSignature = m_gameRootSignature.Get();

	const auto& latteRegister = RecordRegs();
	const auto& colorControlReg = latteRegister.CB_COLOR_CONTROL;
	const uint32 blendEnableMask = colorControlReg.get_BLEND_MASK();
	const uint32 rawRenderTargetMask = latteRegister.CB_TARGET_MASK.get_MASK();
	const uint32 renderTargetMask = GetD3D12EffectiveRenderTargetMask(latteRegister);
	const bool logicOpRequested = colorControlReg.get_ROP() != Latte::LATTE_CB_COLOR_CONTROL::E_LOGICOP::COPY;
	bool logicOpEnable = !noRasterizationOutput && m_outputMergerLogicOpSupported && logicOpRequested &&
		m_logicOpEmulationMode == 0 && !m_disableNativeLogicOpForDraw;
	D3D12_LOGIC_OP d3dLogicOp = D3D12_LOGIC_OP_COPY;
	const bool hasExactD3D12LogicOp = TryGetD3D12LogicOp(colorControlReg.get_ROP(), d3dLogicOp);
	logicOpEnable = logicOpEnable && hasExactD3D12LogicOp;
	if (logicOpEnable)
	{
		const auto& colorAttachments = m_activeFBO->GetColorAttachments();
		const uint32 pixelOutputMask = pixelShader ? pixelShader->pixelColorOutputMask : 0u;
		for (uint32 i = 0; i < m_activeFBO->GetColorAttachmentCount() && i < colorAttachments.size(); ++i)
		{
			if (!colorAttachments[i].valid || ((renderTargetMask >> (i * 4)) & 0xF) == 0)
				continue;
			if ((pixelOutputMask & (1u << i)) == 0)
				continue;
			if (!FormatSupportsOutputMergerLogicOp(colorAttachments[i].format))
			{
				logicOpEnable = false;
				break;
			}
		}
	}
	desc.BlendState.AlphaToCoverageEnable = FALSE;
	// D3D12 rejects logic ops with IndependentBlendEnable set
	desc.BlendState.IndependentBlendEnable = (logicOpEnable || noRasterizationOutput) ? FALSE : TRUE;
	for (auto& rtBlend : desc.BlendState.RenderTarget)
	{
		rtBlend.BlendEnable = FALSE;
		rtBlend.LogicOpEnable = logicOpEnable;
		rtBlend.SrcBlend = D3D12_BLEND_ONE;
		rtBlend.DestBlend = D3D12_BLEND_ZERO;
		rtBlend.BlendOp = D3D12_BLEND_OP_ADD;
		rtBlend.SrcBlendAlpha = D3D12_BLEND_ONE;
		rtBlend.DestBlendAlpha = D3D12_BLEND_ZERO;
		rtBlend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
		rtBlend.LogicOp = D3D12_LOGIC_OP_COPY;
		rtBlend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
	}
	if (hasExactD3D12LogicOp)
	{
		for (auto& rtBlend : desc.BlendState.RenderTarget)
			rtBlend.LogicOp = d3dLogicOp;
	}
	static uint32 s_unsupportedRtBlendLogCount = 0;
	for (uint32 i = 0; i < std::size(desc.BlendState.RenderTarget); ++i)
	{
		auto& rtBlend = desc.BlendState.RenderTarget[i];
		const auto& blendControlReg = latteRegister.CB_BLENDN_CONTROL[i];
		const bool rtSlotActive =
			!noRasterizationOutput &&
			m_activeFBO &&
			i < m_activeFBO->GetColorAttachmentCount() &&
			i < m_activeFBO->GetColorAttachments().size() &&
			m_activeFBO->GetColorAttachments()[i].valid;
		const DXGI_FORMAT rtFormat = rtSlotActive ? m_activeFBO->GetColorAttachments()[i].format : DXGI_FORMAT_UNKNOWN;
		const bool integerRenderTarget = rtSlotActive && IsD3D12IntegerFormat(rtFormat);
		const bool blendRequested = rtSlotActive && !integerRenderTarget && !logicOpEnable &&
			m_logicOpEmulationMode == 0 && !m_disableNativeLogicOpForDraw && ((blendEnableMask & (1u << i)) != 0);
		const bool blendSupported = !blendRequested || FormatSupportsBlending(rtFormat);
		rtBlend.BlendEnable = blendRequested && blendSupported;
		rtBlend.SrcBlend = GetD3D12ColorBlendFactor(blendControlReg.get_COLOR_SRCBLEND(), m_alphaBlendFactorSupported);
		rtBlend.DestBlend = GetD3D12ColorBlendFactor(blendControlReg.get_COLOR_DSTBLEND(), m_alphaBlendFactorSupported);
		rtBlend.BlendOp = GetD3D12BlendOp(blendControlReg.get_COLOR_COMB_FCN());
		if (blendControlReg.get_SEPARATE_ALPHA_BLEND())
		{
			rtBlend.SrcBlendAlpha = GetD3D12AlphaBlendFactor(blendControlReg.get_ALPHA_SRCBLEND());
			rtBlend.DestBlendAlpha = GetD3D12AlphaBlendFactor(blendControlReg.get_ALPHA_DSTBLEND());
			rtBlend.BlendOpAlpha = GetD3D12BlendOp(blendControlReg.get_ALPHA_COMB_FCN());
		}
		else
		{
			rtBlend.SrcBlendAlpha = GetD3D12AlphaBlendFactor(blendControlReg.get_COLOR_SRCBLEND());
			rtBlend.DestBlendAlpha = GetD3D12AlphaBlendFactor(blendControlReg.get_COLOR_DSTBLEND());
			rtBlend.BlendOpAlpha = rtBlend.BlendOp;
		}
		const bool shaderWritesTarget = pixelShader && (pixelShader->pixelColorOutputMask & (1u << i)) != 0;
		rtBlend.RenderTargetWriteMask = noRasterizationOutput || !shaderWritesTarget ?
			0 : (UINT8)((renderTargetMask >> (i * 4)) & 0xF);
		if (blendRequested && !blendSupported)
		{
#if defined(CEMU_UWP_D3D12_CORRECTNESS_TELEMETRY)
			++m_correctnessUnsupportedBlendStates;
#endif
			if (s_unsupportedRtBlendLogCount < 16)
			{
				cemuLog_log(LogType::Force,
							"D3D12: disabling unsupported native RT blend for PSO 0x{:016x} rt={} format={} src={} dst={} op={} srcA={} dstA={} opA={}.",
							identity.pipelineStateHash,
							i,
							(uint32)rtFormat,
							(uint32)rtBlend.SrcBlend,
							(uint32)rtBlend.DestBlend,
							(uint32)rtBlend.BlendOp,
							(uint32)rtBlend.SrcBlendAlpha,
							(uint32)rtBlend.DestBlendAlpha,
							(uint32)rtBlend.BlendOpAlpha);
			}
			else if (s_unsupportedRtBlendLogCount == 16)
				cemuLog_log(LogType::Force, "D3D12: suppressing repeated unsupported native RT blend logs.");
			s_unsupportedRtBlendLogCount++;
			D3D12TraceEvent("game_pipeline_unsupported_rt_blend",
							"pipelineKey=0x{:016x} stateHash=0x{:016x} rt={} format={} src={} dst={} op={} srcA={} dstA={} opA={}",
							pipelineKey,
							identity.pipelineStateHash,
							i,
							(uint32)rtFormat,
							(uint32)rtBlend.SrcBlend,
							(uint32)rtBlend.DestBlend,
							(uint32)rtBlend.BlendOp,
							(uint32)rtBlend.SrcBlendAlpha,
							(uint32)rtBlend.DestBlendAlpha,
							(uint32)rtBlend.BlendOpAlpha);
		}
	}
	desc.SampleMask = UINT_MAX;

	const auto& polygonControlReg = latteRegister.PA_SU_SC_MODE_CNTL;
	desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
	const auto frontFace = polygonControlReg.get_FRONT_FACE();
	const auto cullState = GetD3D12EffectiveCullState(primitiveMode, polygonControlReg);
	auto selectedStencilState = GetD3D12SelectedStencilState(latteRegister, GetD3D12StencilFaceCullState(cullState, rasterizesPointQuads));
	const bool stencilFrontPass =
		stencilFallbackPass == StencilFallbackPass::Front ||
		stencilFallbackPass == StencilFallbackPass::FrontFlipped;
	const bool stencilBackPass =
		stencilFallbackPass == StencilFallbackPass::Back ||
		stencilFallbackPass == StencilFallbackPass::BackFlipped;
	const bool flipFallbackWinding =
		stencilFallbackPass == StencilFallbackPass::WindingFlipped ||
		stencilFallbackPass == StencilFallbackPass::FrontFlipped ||
		stencilFallbackPass == StencilFallbackPass::BackFlipped;
	if (stencilFrontPass)
	{
		selectedStencilState.readMask = selectedStencilState.frontReadMask;
		selectedStencilState.writeMask = selectedStencilState.frontWriteMask;
		selectedStencilState.ref = selectedStencilState.frontRef;
		selectedStencilState.usedBackFace = false;
	}
	else if (stencilBackPass)
	{
		selectedStencilState.readMask = selectedStencilState.backReadMask;
		selectedStencilState.writeMask = selectedStencilState.backWriteMask;
		selectedStencilState.ref = selectedStencilState.backRef;
		selectedStencilState.usedBackFace = true;
	}
	if (stencilFrontPass)
		desc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
	else if (stencilBackPass)
		desc.RasterizerState.CullMode = D3D12_CULL_MODE_FRONT;
	else if (rasterizesPointQuads)
		desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
	else if (cullState.cullFront && cullState.cullBack)
	{
		cemuLog_logOnce(LogType::Force, "D3D12: front-and-back culling skips non-streamout draws; streamout draws use the no-raster pipeline variant.");
		desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
	}
	else if (cullState.cullFront)
		desc.RasterizerState.CullMode = D3D12_CULL_MODE_FRONT;
	else if (cullState.cullBack)
		desc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
	else
		desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
	bool frontCounterClockwise = D3D12FrontCounterClockwise(frontFace);
	// Fan-to-strip conversion reverses triangle orientation; compensate in the PSO.
	if (flipFallbackWinding)
		frontCounterClockwise = !frontCounterClockwise;
	// the point expansion shader emits clockwise quads
	if (rasterizesPointQuads)
		frontCounterClockwise = false;
	desc.RasterizerState.FrontCounterClockwise = frontCounterClockwise;
	const bool dynamicDepthBias = UsesDynamicDepthBias(rasterizesPointQuads);
#if defined(__ID3D12GraphicsCommandList9_INTERFACE_DEFINED__)
	if (dynamicDepthBias)
		desc.Flags |= D3D12_PIPELINE_STATE_FLAG_DYNAMIC_DEPTH_BIAS;
#endif
	// polygon offset does not apply to points
	if (polygonControlReg.get_OFFSET_FRONT_ENABLED() && !rasterizesPointQuads && !dynamicDepthBias)
	{
		// quantised exactly like the pipeline key (see D3D12PipelineCompiler::QuantizeDepthBias)
		const float frontScale = D3D12PipelineCompiler::QuantizeDepthBias(latteRegister.PA_SU_POLY_OFFSET_FRONT_SCALE.get_SCALE()) / 16.0f;
		const float frontOffset = latteRegister.PA_SU_POLY_OFFSET_FRONT_OFFSET.get_OFFSET();
		const float offsetClamp = D3D12PipelineCompiler::QuantizeDepthBias(latteRegister.PA_SU_POLY_OFFSET_CLAMP.get_CLAMP());
		desc.RasterizerState.DepthBias = (INT)std::clamp<float>(std::round(frontOffset), (float)std::numeric_limits<INT>::min(), (float)std::numeric_limits<INT>::max());
		desc.RasterizerState.DepthBiasClamp = offsetClamp;
		desc.RasterizerState.SlopeScaledDepthBias = frontScale;
	}
	else
	{
		desc.RasterizerState.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
		desc.RasterizerState.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
		desc.RasterizerState.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
	}
	desc.RasterizerState.DepthClipEnable =
		!latteRegister.PA_CL_CLIP_CNTL.get_ZCLIP_NEAR_DISABLE() &&
		!latteRegister.PA_CL_CLIP_CNTL.get_ZCLIP_FAR_DISABLE();
	desc.RasterizerState.MultisampleEnable = FALSE;
	desc.RasterizerState.AntialiasedLineEnable = FALSE;
	desc.RasterizerState.ForcedSampleCount = 0;
	desc.RasterizerState.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;

	const bool hasDepth = !noRasterizationOutput && m_activeFBO->HasDepthStencilAttachment();
	const auto& depthControlReg = latteRegister.DB_DEPTH_CONTROL;
	desc.DepthStencilState.DepthEnable = hasDepth && depthControlReg.get_Z_ENABLE();
	desc.DepthStencilState.DepthWriteMask = hasDepth && depthControlReg.get_Z_WRITE_ENABLE() ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
	desc.DepthStencilState.DepthFunc = GetD3D12CompareFunc(depthControlReg.get_Z_FUNC());
	const bool hasStencil = hasDepth && m_activeFBO->GetStencilFormat() != DXGI_FORMAT_UNKNOWN;
	desc.DepthStencilState.StencilEnable = hasStencil && depthControlReg.get_STENCIL_ENABLE();
	desc.DepthStencilState.StencilReadMask = selectedStencilState.readMask;
	desc.DepthStencilState.StencilWriteMask = selectedStencilState.writeMask;
	desc.DepthStencilState.FrontFace.StencilFunc = GetD3D12CompareFunc(depthControlReg.get_STENCIL_FUNC_F());
	desc.DepthStencilState.FrontFace.StencilFailOp = GetD3D12StencilOp(depthControlReg.get_STENCIL_FAIL_F());
	desc.DepthStencilState.FrontFace.StencilDepthFailOp = GetD3D12StencilOp(depthControlReg.get_STENCIL_ZFAIL_F());
	desc.DepthStencilState.FrontFace.StencilPassOp = GetD3D12StencilOp(depthControlReg.get_STENCIL_ZPASS_F());
	if (depthControlReg.get_BACK_STENCIL_ENABLE())
	{
		desc.DepthStencilState.BackFace.StencilFunc = GetD3D12CompareFunc(depthControlReg.get_STENCIL_FUNC_B());
		desc.DepthStencilState.BackFace.StencilFailOp = GetD3D12StencilOp(depthControlReg.get_STENCIL_FAIL_B());
		desc.DepthStencilState.BackFace.StencilDepthFailOp = GetD3D12StencilOp(depthControlReg.get_STENCIL_ZFAIL_B());
		desc.DepthStencilState.BackFace.StencilPassOp = GetD3D12StencilOp(depthControlReg.get_STENCIL_ZPASS_B());
	}
	else
		desc.DepthStencilState.BackFace = desc.DepthStencilState.FrontFace;

#if defined(__ID3D12GraphicsCommandList8_INTERFACE_DEFINED__)
	const bool needsIndependentStencilState =
		desc.DepthStencilState.StencilEnable &&
		!cullState.cullFront && !cullState.cullBack &&
		!stencilFrontPass && !stencilBackPass &&
		selectedStencilState.needsIndependentFrontBackState;
	const bool useIndependentStencilMasks =
		needsIndependentStencilState &&
		m_independentFrontBackStencilSupported &&
		(selectedStencilState.frontReadMask != selectedStencilState.backReadMask ||
		 selectedStencilState.frontWriteMask != selectedStencilState.backWriteMask);
	D3D12_DEPTH_STENCIL_DESC2 independentDepthStencil{};
	if (useIndependentStencilMasks)
	{
		independentDepthStencil.DepthEnable = desc.DepthStencilState.DepthEnable;
		independentDepthStencil.DepthWriteMask = desc.DepthStencilState.DepthWriteMask;
		independentDepthStencil.DepthFunc = desc.DepthStencilState.DepthFunc;
		independentDepthStencil.StencilEnable = desc.DepthStencilState.StencilEnable;
		independentDepthStencil.FrontFace = {
			desc.DepthStencilState.FrontFace.StencilFailOp,
			desc.DepthStencilState.FrontFace.StencilDepthFailOp,
			desc.DepthStencilState.FrontFace.StencilPassOp,
			desc.DepthStencilState.FrontFace.StencilFunc,
			selectedStencilState.frontReadMask,
			selectedStencilState.frontWriteMask,
		};
		independentDepthStencil.BackFace = {
			desc.DepthStencilState.BackFace.StencilFailOp,
			desc.DepthStencilState.BackFace.StencilDepthFailOp,
			desc.DepthStencilState.BackFace.StencilPassOp,
			desc.DepthStencilState.BackFace.StencilFunc,
			selectedStencilState.backReadMask,
			selectedStencilState.backWriteMask,
		};
		independentDepthStencil.DepthBoundsTestEnable = FALSE;
	}
#endif

	desc.InputLayout = {};
	desc.PrimitiveTopologyType = D3D12PipelineCompiler::GetPrimitiveTopologyType(primitiveMode);
	desc.IBStripCutValue = stripCutValue;
	desc.NumRenderTargets = noRasterizationOutput ? 0 : std::min<UINT>(m_activeFBO->GetColorAttachmentCount(), 8);
	if (!noRasterizationOutput)
	{
		const auto& colorAttachments = m_activeFBO->GetColorAttachments();
		for (uint32 i = 0; i < desc.NumRenderTargets && i < colorAttachments.size(); ++i)
		{
			desc.RTVFormats[i] = colorAttachments[i].valid ? colorAttachments[i].format : DXGI_FORMAT_R8G8B8A8_UNORM;
			if (!colorAttachments[i].valid)
				desc.BlendState.RenderTarget[i].RenderTargetWriteMask = 0;
		}
	}
	desc.DSVFormat = hasDepth ? m_activeFBO->GetDepthFormat() : DXGI_FORMAT_UNKNOWN;
	desc.SampleDesc.Count = 1;

	desc.VS ={ d3dVertexShader->GetDXIL().data(), d3dVertexShader->GetDXIL().size() };
	if (d3dGeometryShader)
		desc.GS = { d3dGeometryShader->GetDXIL().data(), d3dGeometryShader->GetDXIL().size() };
	else if (d3dPointExpansionShader)
		desc.GS = { d3dPointExpansionShader->GetDXIL().data(), d3dPointExpansionShader->GetDXIL().size() };
	else if (d3dRectEmulationShader)
		desc.GS = { d3dRectEmulationShader->GetDXIL().data(), d3dRectEmulationShader->GetDXIL().size() };
	if (d3dPixelShader && !noRasterizationOutput)
		desc.PS = { d3dPixelShader->GetDXIL().data(), d3dPixelShader->GetDXIL().size() };

	D3D12TraceEvent("game_pipeline_create_begin", "pipelineKey=0x{:016x} stateHash=0x{:016x} primitive={} topologyType={} noRaster={} pointGS={} rectGS={} fbo=0x{:016x} fboSize={}x{} colorCount={} colorMask=0x{:02x} drawBuffersMask=0x{:08x} rt0={} rt1={} rt2={} rt3={} rt4={} rt5={} rt6={} rt7={} dsv={} depthEnable={} depthWrite={} depthFunc={} latteZEnable={} latteZWrite={} latteZFunc={} stencilEnable={} stencilFuncF={} stencilFuncB={} stencilRef={} stencilReadMask=0x{:02x} stencilWriteMask=0x{:02x} stencilBackSelected={} stencilTwoSidedStateMismatch={} cullMode={} frontCCW={} latteFrontFace={} latteCullFront={} latteCullBack={} depthClip={} zClipNearDisable={} zClipFarDisable={} rasterKill={} depthBias={} slopeBias={} logicOp={} independentBlend={} blendMask=0x{:02x} targetMask=0x{:08x} rawTargetMask=0x{:08x} colorSpecialOp={} alphaTest={} alphaFunc={} vs=0x{:016x} gs=0x{:016x} ps=0x{:016x} vsBytes={} gsBytes={} psBytes={}",
					pipelineKey,
					identity.pipelineStateHash,
					(uint32)primitiveMode,
					(uint32)desc.PrimitiveTopologyType,
					noRasterizationOutput ? 1 : 0,
					usePointExpansionGS ? 1 : 0,
					useRectEmulationGS ? 1 : 0,
					m_activeFBO ? m_activeFBO->key : 0,
					m_activeFBO ? m_activeFBO->m_size.x : 0,
					m_activeFBO ? m_activeFBO->m_size.y : 0,
					m_activeFBO ? m_activeFBO->GetColorAttachmentCount() : 0,
					m_activeFBO ? m_activeFBO->GetColorAttachmentMask() : 0,
					m_activeFBO ? m_activeFBO->drawBuffersMask : 0,
					(uint32)desc.RTVFormats[0],
					(uint32)desc.RTVFormats[1],
					(uint32)desc.RTVFormats[2],
					(uint32)desc.RTVFormats[3],
					(uint32)desc.RTVFormats[4],
					(uint32)desc.RTVFormats[5],
					(uint32)desc.RTVFormats[6],
					(uint32)desc.RTVFormats[7],
					(uint32)desc.DSVFormat,
					desc.DepthStencilState.DepthEnable ? 1 : 0,
					desc.DepthStencilState.DepthWriteMask == D3D12_DEPTH_WRITE_MASK_ALL ? 1 : 0,
					(uint32)desc.DepthStencilState.DepthFunc,
					depthControlReg.get_Z_ENABLE() ? 1 : 0,
					depthControlReg.get_Z_WRITE_ENABLE() ? 1 : 0,
					(uint32)depthControlReg.get_Z_FUNC(),
					desc.DepthStencilState.StencilEnable ? 1 : 0,
					(uint32)depthControlReg.get_STENCIL_FUNC_F(),
					(uint32)depthControlReg.get_STENCIL_FUNC_B(),
					selectedStencilState.ref,
					(uint32)selectedStencilState.readMask,
					(uint32)selectedStencilState.writeMask,
					selectedStencilState.usedBackFace ? 1 : 0,
					selectedStencilState.needsIndependentFrontBackState ? 1 : 0,
					(uint32)desc.RasterizerState.CullMode,
					desc.RasterizerState.FrontCounterClockwise ? 1 : 0,
					(uint32)frontFace,
					polygonControlReg.get_CULL_FRONT() ? 1 : 0,
					polygonControlReg.get_CULL_BACK() ? 1 : 0,
					desc.RasterizerState.DepthClipEnable ? 1 : 0,
					latteRegister.PA_CL_CLIP_CNTL.get_ZCLIP_NEAR_DISABLE() ? 1 : 0,
					latteRegister.PA_CL_CLIP_CNTL.get_ZCLIP_FAR_DISABLE() ? 1 : 0,
					latteRegister.PA_CL_CLIP_CNTL.get_DX_RASTERIZATION_KILL() ? 1 : 0,
					desc.RasterizerState.DepthBias,
					desc.RasterizerState.SlopeScaledDepthBias,
					logicOpEnable ? 1 : 0,
					desc.BlendState.IndependentBlendEnable ? 1 : 0,
					blendEnableMask,
					renderTargetMask,
					rawRenderTargetMask,
					(uint32)colorControlReg.get_SPECIAL_OP(),
					latteRegister.SX_ALPHA_TEST_CONTROL.get_ALPHA_TEST_ENABLE() ? 1 : 0,
					(uint32)latteRegister.SX_ALPHA_TEST_CONTROL.get_ALPHA_FUNC(),
					vertexShader ? vertexShader->baseHash : 0,
					geometryShader ? geometryShader->baseHash : 0,
					pixelShader ? pixelShader->baseHash : 0,
					(unsigned long long)desc.VS.BytecodeLength,
					(unsigned long long)desc.GS.BytecodeLength,
					(unsigned long long)desc.PS.BytecodeLength);
	for (uint32 i = 0; i < std::size(desc.BlendState.RenderTarget); ++i)
	{
		const auto& rtBlend = desc.BlendState.RenderTarget[i];
		const auto& blendControlReg = latteRegister.CB_BLENDN_CONTROL[i];
		D3D12TraceEvent("game_pipeline_rt_state", "pipelineKey=0x{:016x} stateHash=0x{:016x} rtIndex={} active={} format={} writeMask=0x{:02x} blendEnable={} logicOpEnable={} logicOp={} srcColor={} dstColor={} colorOp={} srcAlpha={} dstAlpha={} alphaOp={} fbo=0x{:016x} colorMask=0x{:02x} targetMask=0x{:08x} latteSrcColor={} latteDstColor={} latteColorOp={} latteSrcAlpha={} latteDstAlpha={} latteAlphaOp={} latteSeparateAlpha={}",
						pipelineKey,
						identity.pipelineStateHash,
						i,
						i < desc.NumRenderTargets ? 1 : 0,
						(uint32)desc.RTVFormats[i],
						(uint32)rtBlend.RenderTargetWriteMask,
						rtBlend.BlendEnable ? 1 : 0,
						rtBlend.LogicOpEnable ? 1 : 0,
						(uint32)rtBlend.LogicOp,
						(uint32)rtBlend.SrcBlend,
						(uint32)rtBlend.DestBlend,
						(uint32)rtBlend.BlendOp,
						(uint32)rtBlend.SrcBlendAlpha,
						(uint32)rtBlend.DestBlendAlpha,
						(uint32)rtBlend.BlendOpAlpha,
						m_activeFBO ? m_activeFBO->key : 0,
						m_activeFBO ? m_activeFBO->GetColorAttachmentMask() : 0,
						renderTargetMask,
						(uint32)blendControlReg.get_COLOR_SRCBLEND(),
						(uint32)blendControlReg.get_COLOR_DSTBLEND(),
						(uint32)blendControlReg.get_COLOR_COMB_FCN(),
						(uint32)blendControlReg.get_ALPHA_SRCBLEND(),
						(uint32)blendControlReg.get_ALPHA_DSTBLEND(),
						(uint32)blendControlReg.get_ALPHA_COMB_FCN(),
						blendControlReg.get_SEPARATE_ALPHA_BLEND() ? 1 : 0);
	}
	Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline;
	HRESULT hr = E_FAIL;
	bool createNativePipeline = true;
	const uint64 currentTitleId = GetUwpTitleId();
	// title id is briefly 0 during startup handoff, so only a non-zero change counts
	if (currentTitleId != 0 && m_gamePipelineRecipeLoadingTitleId != 0 &&
		m_gamePipelineRecipeLoadingTitleId != currentTitleId)
		ClearPendingGamePipelineRecipes();
	D3D12PipelineRecipe::SerializeOptions recipeOptions{};
#if defined(__ID3D12GraphicsCommandList8_INTERFACE_DEFINED__)
	if (useIndependentStencilMasks)
		recipeOptions.independentDepthStencil = &independentDepthStencil;
#endif
	D3D12PipelineRecipe::Serialized serializedPipelineRecipe;
	const D3D12PipelineRecipe::Status recipeStatus = D3D12PipelineRecipe::Serialize(
		pipelineKey, desc, recipeOptions, serializedPipelineRecipe);
	const bool hasSerializedPipelineRecipe = recipeStatus == D3D12PipelineRecipe::Status::Success;
	bool pipelinePromotedFromRecipe = false;
	bool pipelinePromotedFromAsync = false;
	bool storedRecipeIsStale = false;
	DrainAsyncGamePipelineFailures();
	AdoptPrewarmedGamePipelines();
	if (auto pendingIt = m_pendingGamePipelineRecipes.find(pipelineKey);
		pendingIt != m_pendingGamePipelineRecipes.end())
	{
		if (hasSerializedPipelineRecipe && pendingIt->second.pipeline &&
			pendingIt->second.fingerprint == serializedPipelineRecipe.fingerprint)
		{
			pipeline = std::move(pendingIt->second.pipeline);
			m_pendingGamePipelineRecipes.erase(pendingIt);
			hr = S_OK;
			createNativePipeline = false;
			pipelinePromotedFromRecipe = true;
			pipelinePromotedFromAsync = m_asyncPipelinesInFlight.erase(pipelineKey) != 0;
			m_gamePipelineRecipePromotions++;
			if (m_gamePipelineRecipePromotions == 1 || (m_gamePipelineRecipePromotions % 64) == 0)
			{
				cemuLog_log(LogType::Force,
					"D3D12 UWP pipeline recipes: exact live promotions={} pending={} latest={:016x}.",
					m_gamePipelineRecipePromotions,
					m_pendingGamePipelineRecipes.size(),
					pipelineKey);
			}
			D3D12TraceEvent("game_pipeline_recipe_promoted",
				"pipelineKey=0x{:016x} pending={} cacheSize={}",
				pipelineKey,
				m_pendingGamePipelineRecipes.size(),
				m_gamePipelineCache.size());
		}
		else
		{
			m_pendingGamePipelineRecipes.erase(pendingIt);
			m_gamePipelineRecipeMismatches++;
			storedRecipeIsStale = true;
			static uint32 s_recipeMismatchLogCount = 0;
			if (D3D12RuntimeLogCapped(s_recipeMismatchLogCount, "pipeline recipe fingerprint mismatch", 16))
			{
				cemuLog_log(LogType::Force,
					"D3D12 UWP: rejected stale pipeline recipe {:016x} on exact live fingerprint mismatch (status={}, total mismatches={}).",
					pipelineKey,
					D3D12PipelineRecipe::StatusName(recipeStatus),
					m_gamePipelineRecipeMismatches);
			}
			D3D12TraceEvent("game_pipeline_recipe_mismatch",
				"pipelineKey=0x{:016x} serializeStatus={} pending={}",
				pipelineKey,
				D3D12PipelineRecipe::StatusName(recipeStatus),
				m_pendingGamePipelineRecipes.size());
		}
	}
	else if (!hasSerializedPipelineRecipe)
	{
		static uint32 s_recipeSerializeLogCount = 0;
		if (D3D12RuntimeLogCapped(s_recipeSerializeLogCount, "pipeline recipe serialization rejection", 16))
		{
			cemuLog_log(LogType::Force,
				"D3D12 UWP: pipeline {:016x} cannot be persisted as a recipe (status={}); native creation continues.",
				pipelineKey,
				D3D12PipelineRecipe::StatusName(recipeStatus));
		}
	}

	const bool xboxUwpDevice = IsXboxConsole();
	EnsureGamePipelineLibrary(currentTitleId);
	bool pipelineLibraryEligible = true;
#if defined(__ID3D12GraphicsCommandList8_INTERFACE_DEFINED__)
	pipelineLibraryEligible = !useIndependentStencilMasks;
#endif
	wchar_t pipelineLibraryName[40]{};
	if (createNativePipeline && !xboxUwpDevice && pipelineLibraryEligible && m_gamePipelineLibrary)
	{
		swprintf_s(pipelineLibraryName, L"cemu-%016llx", (unsigned long long)pipelineKey);
		hr = m_gamePipelineLibrary->LoadGraphicsPipeline(pipelineLibraryName, &desc, IID_PPV_ARGS(&pipeline));
		if (SUCCEEDED(hr))
		{
			createNativePipeline = false;
			m_gamePipelineLibraryHits++;
			D3D12TraceEvent("game_pipeline_library_hit", "pipelineKey=0x{:016x} title=0x{:016x}",
				pipelineKey, m_gamePipelineLibraryTitleId);
		}
		else
		{
			m_gamePipelineLibraryMisses++;
			if (IsD3D12DeviceFailure(hr))
			{
				CheckDeviceRemoved(hr, "LoadGraphicsPipeline(game)");
				throw std::runtime_error("D3D12 device failed while loading a cached game pipeline");
			}
		}
	}

	uint64 pipelineFingerprint = 0;
	std::vector<uint8> cachedPipelineBlob;
	bool cachedPipelineBlobLoaded = false;
	bool pipelineCreatedFromBlob = false;
	if (createNativePipeline && xboxUwpDevice && EnsureSinglePsoCacheSupport() && m_gamePipelineLibraryTitleId != 0)
	{
		pipelineFingerprint = BuildGamePipelineFingerprint(pipelineKey, desc);
		cachedPipelineBlobLoaded = LoadGamePipelineBlob(pipelineKey, pipelineFingerprint, cachedPipelineBlob);
		if (cachedPipelineBlobLoaded)
			desc.CachedPSO = { cachedPipelineBlob.data(), cachedPipelineBlob.size() };
	}
	// async_compile: build the PSO on a worker and skip this draw instead of stalling the render thread.
	// Pipelines with a driver blob are cheap to create here. Memory pressure does not turn this off: a synchronous
	// build costs the same memory, and after a teleport above the 84% guard every new PSO blocked the render thread
	// (1-2 FPS, ~500 async jobs starved behind the render-thread builds) until usage fell below 78%.
	uint64 contentCanonicalKey = 0;
	bool pipelineFromContentMatch = false;
	if (createNativePipeline && hasSerializedPipelineRecipe)
	{
		if (ID3D12PipelineState* sameContent = FindGamePipelineByContent(serializedPipelineRecipe.contentFingerprint, contentCanonicalKey))
		{
			pipeline = sameContent;
			hr = S_OK;
			createNativePipeline = false;
			pipelineFromContentMatch = true;
			m_gamePipelineContentReuses++;
		}
	}
	bool asyncPipelineEligible = createNativePipeline && m_allowAsyncShaderSkip && hasSerializedPipelineRecipe &&
		!cachedPipelineBlobLoaded && !m_asyncPipelineFailed.contains(pipelineKey);
#if defined(__ID3D12GraphicsCommandList8_INTERFACE_DEFINED__)
	asyncPipelineEligible = asyncPipelineEligible && !useIndependentStencilMasks;
#endif
	bool pipelineRecipeRecorded = false;
	if (asyncPipelineEligible)
	{
		// the recipe is stored when a draw adopts the result (below), so variants no draw ever used (drifting shadow
		// bias, short-lived effects) do not fill the 16384-entry cache that the launch prewarm rebuilds every session
		if (QueueAsyncGamePipeline(pipelineKey, std::move(serializedPipelineRecipe.bytes)))
		{
			D3D12TraceEvent("game_pipeline_async_queued", "pipelineKey=0x{:016x} queued={}", pipelineKey, m_asyncPipelinesQueued);
			if (auto* sibling = findDepthBiasSibling())
				return sibling;
			m_gamePipelineAsyncPending = true;
			return nullptr;
		}
	}
	if (createNativePipeline)
	{
		const auto createPipeline = [&]() -> HRESULT
		{
			LatteUwpHitchScope hitchScope(LatteUwpHitchOp::PipelineCreate);
			const auto slowStarted = std::chrono::steady_clock::now();
			m_renderThreadPipelineCreates.fetch_add(1, std::memory_order_acq_rel);
			stdx::scope_exit renderThreadCreateDone([&]() { m_renderThreadPipelineCreates.fetch_sub(1, std::memory_order_acq_rel); });
		#if defined(CEMU_UWP_D3D12_DIAGNOSTICS)
			const auto started = std::chrono::steady_clock::now();
		#endif
			// no compilation slot: a draw must not queue behind shader compiles or background prewarm,
			// and the render thread creates one pipeline at a time anyway
			HRESULT createHr;
			{
				LatteUwpHitchScope driverScope(LatteUwpHitchOp::PipelineDriverCreate);
				D3D12GpuWatchdog::ActivityScope watchdogActivity(m_gpuWatchdog, D3D12GpuWatchdog::Activity::PipelineCreate, identity.pipelineStateHash);
#if defined(__ID3D12GraphicsCommandList8_INTERFACE_DEFINED__)
				createHr = useIndependentStencilMasks ?
				CreateGraphicsPipelineWithIndependentStencil(m_independentStencilDevice.Get(), desc, independentDepthStencil, pipeline) :
				m_device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pipeline));
#else
				createHr = m_device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pipeline));
#endif
			}
			const uint64 slowElapsedMs = (uint64)std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now() - slowStarted).count();
			if (slowElapsedMs >= 100 && m_slowSyncPipelineLogs < 200)
			{
				++m_slowSyncPipelineLogs;
				cemuLog_log(LogType::Force, "D3D12: render thread built a PSO in {} ms (skip blocked=0x{:02x} [1 async off, 2 no FBO, 4 1600x1600, 8 no depth <=6 indices, 16 occlusion query, 32 stream-out], async eligible={}, recipe={}, heavy work deferred={}, async in flight={}, vs={:016x} ps={:016x}, key={:016x}, recipe cache: {}).",
					slowElapsedMs, m_asyncSkipBlockReasons, asyncPipelineEligible ? 1 : 0, hasSerializedPipelineRecipe ? 1 : 0,
					CemuUwp::MemoryBudgetService::IsHeavyWorkDeferred() ? 1 : 0, m_asyncPipelinesInFlight.size(),
					vertexShader ? vertexShader->baseHash : 0, pixelShader ? pixelShader->baseHash : 0,
					// tells why the launch prewarm did not cover it (the rain PSO was rebuilt every session)
					pipelineKey, D3D12PipelineStableCache::GetInstance().DescribePipeline(pipelineKey));
			}
		#if defined(CEMU_UWP_D3D12_DIAGNOSTICS)
			const uint64 elapsedUs = (uint64)std::chrono::duration_cast<std::chrono::microseconds>(
				std::chrono::steady_clock::now() - started).count();
			m_gamePsoCreateCount++;
			m_gamePsoCreateTimeUs += elapsedUs;
			m_gamePsoCreateMaxUs = std::max(m_gamePsoCreateMaxUs, elapsedUs);
		#endif
			return createHr;
		};
		const auto discardCachedPipelineBlob = [&]()
		{
			RemoveGamePipelineBlob(pipelineKey, pipelineFingerprint);
			m_singlePsoCacheRejects++;
			m_singlePsoCacheMisses++;
			pipeline.Reset();
			cachedPipelineBlob.clear();
			desc.CachedPSO = {};
			cachedPipelineBlobLoaded = false;
		};
		hr = createPipeline();
		if (cachedPipelineBlobLoaded && FAILED(hr) &&
			!IsD3D12DeviceFailure(hr) && !IsD3D12TransientMemoryFailure(hr))
		{
			discardCachedPipelineBlob();
			hr = createPipeline();
		}
		if (IsD3D12TransientMemoryFailure(hr))
		{
			RecoverFromPipelineAllocationFailure();
			pipeline.Reset();
			hr = createPipeline();
			if (cachedPipelineBlobLoaded && FAILED(hr) &&
				!IsD3D12DeviceFailure(hr) && !IsD3D12TransientMemoryFailure(hr))
			{
				discardCachedPipelineBlob();
				hr = createPipeline();
			}
		}
		if (cachedPipelineBlobLoaded && SUCCEEDED(hr))
		{
			pipelineCreatedFromBlob = true;
			m_singlePsoCacheHits++;
		}
	}
	if (FAILED(hr))
	{
		m_gamePsoCreateFailures++;
		cemuLog_log(LogType::Force,
					"D3D12: failed to create game PSO 0x{:016x} (HRESULT 0x{:08x}) vs={:016x} gs={:016x} ps={:016x} topology={} rts={} dsv={} depth={} stencil={} logicOp={} independentBlend={} noRaster={} pointGS={} rectGS={} shaderBytes={}/{}/{} blendMask=0x{:02x} targetMask=0x{:08x}.",
					identity.pipelineStateHash,
					(uint32)hr,
					vertexShader ? vertexShader->baseHash : 0,
					geometryShader ? geometryShader->baseHash : 0,
					pixelShader ? pixelShader->baseHash : 0,
					(uint32)desc.PrimitiveTopologyType,
					desc.NumRenderTargets,
					(uint32)desc.DSVFormat,
					desc.DepthStencilState.DepthEnable,
					desc.DepthStencilState.StencilEnable,
					logicOpEnable,
					desc.BlendState.IndependentBlendEnable,
					noRasterizationOutput,
					usePointExpansionGS,
					useRectEmulationGS,
					desc.VS.BytecodeLength,
					desc.GS.BytecodeLength,
					desc.PS.BytecodeLength,
					blendEnableMask,
					renderTargetMask);
		D3D12TraceEvent("game_pipeline_create_failed", "pipelineKey=0x{:016x} stateHash=0x{:016x} hr=0x{:08x} primitive={} topologyType={} noRaster={} pointGS={} rectGS={} fbo=0x{:016x} colorCount={} colorMask=0x{:02x} rt0={} rt1={} rt2={} rt3={} rt4={} rt5={} rt6={} rt7={} dsv={} depthEnable={} stencilEnable={} logicOp={} independentBlend={} blendMask=0x{:02x} targetMask=0x{:08x} vs=0x{:016x} gs=0x{:016x} ps=0x{:016x} vsBytes={} gsBytes={} psBytes={}",
						pipelineKey,
						identity.pipelineStateHash,
						(uint32)hr,
						(uint32)primitiveMode,
						(uint32)desc.PrimitiveTopologyType,
						noRasterizationOutput ? 1 : 0,
						usePointExpansionGS ? 1 : 0,
						useRectEmulationGS ? 1 : 0,
						m_activeFBO ? m_activeFBO->key : 0,
						m_activeFBO ? m_activeFBO->GetColorAttachmentCount() : 0,
						m_activeFBO ? m_activeFBO->GetColorAttachmentMask() : 0,
						(uint32)desc.RTVFormats[0],
						(uint32)desc.RTVFormats[1],
						(uint32)desc.RTVFormats[2],
						(uint32)desc.RTVFormats[3],
						(uint32)desc.RTVFormats[4],
						(uint32)desc.RTVFormats[5],
						(uint32)desc.RTVFormats[6],
						(uint32)desc.RTVFormats[7],
						(uint32)desc.DSVFormat,
						desc.DepthStencilState.DepthEnable ? 1 : 0,
						desc.DepthStencilState.StencilEnable ? 1 : 0,
						logicOpEnable ? 1 : 0,
						desc.BlendState.IndependentBlendEnable ? 1 : 0,
						blendEnableMask,
						renderTargetMask,
						vertexShader ? vertexShader->baseHash : 0,
						geometryShader ? geometryShader->baseHash : 0,
						pixelShader ? pixelShader->baseHash : 0,
						(unsigned long long)desc.VS.BytecodeLength,
						(unsigned long long)desc.GS.BytecodeLength,
						(unsigned long long)desc.PS.BytecodeLength);
		for (uint32 i = 0; i < desc.NumRenderTargets && i < std::size(desc.RTVFormats); ++i)
		{
			const auto& rtBlend = desc.BlendState.RenderTarget[i];
			cemuLog_log(LogType::Force,
						"D3D12:   RT{} format={} writeMask=0x{:02x} blend={} src={} dst={} op={} srcA={} dstA={} opA={}.",
						i,
						(uint32)desc.RTVFormats[i],
						(uint32)rtBlend.RenderTargetWriteMask,
						rtBlend.BlendEnable,
						(uint32)rtBlend.SrcBlend,
						(uint32)rtBlend.DestBlend,
						(uint32)rtBlend.BlendOp,
						(uint32)rtBlend.SrcBlendAlpha,
						(uint32)rtBlend.DestBlendAlpha,
						(uint32)rtBlend.BlendOpAlpha);
		}
		LogInfoQueueMessages("CreateGraphicsPipelineState(game)", 16);
		CheckDeviceRemoved(hr, "CreateGraphicsPipelineState(game)");
		if (IsD3D12TransientMemoryFailure(hr))
			m_gamePipelineRetryFrame[pipelineKey] = LatteGPUState.frameCounter + 30;
		else
			m_gamePipelineCache.emplace(pipelineKey, GamePipelineEntry{});
		return nullptr;
	}

	auto& pipelineStableCache = D3D12PipelineStableCache::GetInstance();
	if (pipelineFromContentMatch)
	{
		// no recipe for this key: the next launch prewarms the canonical one, and this key finds it by content again
		pipelineStableCache.MarkPipelineUsed(contentCanonicalKey, !m_allowAsyncShaderSkip);
	}
	else
	{
		// a stored recipe that was not prewarmed yet is still current; only a stale one is rewritten
		if (hasSerializedPipelineRecipe && (!pipelinePromotedFromRecipe || pipelinePromotedFromAsync) && !pipelineRecipeRecorded)
			pipelineStableCache.AddPipelineRecipe(pipelineKey, serializedPipelineRecipe.bytes, storedRecipeIsStale);
		pipelineStableCache.MarkPipelineUsed(pipelineKey, !m_allowAsyncShaderSkip);
		if (hasSerializedPipelineRecipe)
			m_gamePipelineKeyByContent.try_emplace(serializedPipelineRecipe.contentFingerprint, pipelineKey);
	}
	if (createNativePipeline && xboxUwpDevice && !pipelineCreatedFromBlob && pipelineFingerprint != 0)
		StoreGamePipelineBlob(pipelineKey, pipelineFingerprint, pipeline.Get());
	// PC: a pipeline built in the background goes into the native library like a synchronous one
	if (pipelinePromotedFromAsync && !xboxUwpDevice && pipelineLibraryEligible && m_gamePipelineLibrary)
		swprintf_s(pipelineLibraryName, L"cemu-%016llx", (unsigned long long)pipelineKey);
	if ((createNativePipeline || pipelinePromotedFromAsync) && pipelineLibraryEligible && m_gamePipelineLibrary && pipelineLibraryName[0] != L'\0')
	{
		const HRESULT storeHr = m_gamePipelineLibrary->StorePipeline(pipelineLibraryName, pipeline.Get());
		if (SUCCEEDED(storeHr))
		{
			m_gamePipelineLibraryDirty = true;
			m_gamePipelineLibraryStores++;
		}
		else if (IsD3D12DeviceFailure(storeHr))
		{
			CheckDeviceRemoved(storeHr, "StorePipeline(game)");
			throw std::runtime_error("D3D12 device failed while storing a cached game pipeline");
		}
		else if (storeHr != E_INVALIDARG)
		{
			cemuLog_logOnce(LogType::Force, "D3D12 UWP: failed to cache a native game PSO (HRESULT 0x{:08x}).", (uint32)storeHr);
		}
	}
	SetD3D12ObjectName(pipeline.Get(), L"Cemu D3D12 game PSO");
	m_gamePipelineRetryFrame.erase(pipelineKey);
	m_gamePipelineDeferCount.erase(pipelineKey);
	m_asyncPipelinesInFlight.erase(pipelineKey);
	auto [insertedIt, inserted] = m_gamePipelineCache.emplace(pipelineKey, GamePipelineEntry{ std::move(pipeline), LatteGPUState.frameCounter });
	(void)inserted;
	if (hasBakedDepthBias)
		m_depthBiasSiblingPipelines[depthBiasSiblingKey()] = DepthBiasSibling{ pipelineKey, drawBiasSlope, drawBiasOffset, drawBiasClamp };
	D3D12PipelineCompiler::RecordPipelineCreated(identity, pipelineKey);
	D3D12TraceEvent("game_pipeline_create_success", "pipelineKey=0x{:016x} stateHash=0x{:016x} primitive={} topologyType={} noRaster={} pointGS={} rectGS={} fbo=0x{:016x} colorCount={} colorMask=0x{:02x} rt0={} rt1={} rt2={} rt3={} rt4={} rt5={} rt6={} rt7={} dsv={} vs=0x{:016x} gs=0x{:016x} ps=0x{:016x} pipeline={} cacheSize={}",
					pipelineKey,
					identity.pipelineStateHash,
					(uint32)primitiveMode,
					(uint32)desc.PrimitiveTopologyType,
					noRasterizationOutput ? 1 : 0,
					usePointExpansionGS ? 1 : 0,
					useRectEmulationGS ? 1 : 0,
					m_activeFBO ? m_activeFBO->key : 0,
					m_activeFBO ? m_activeFBO->GetColorAttachmentCount() : 0,
					m_activeFBO ? m_activeFBO->GetColorAttachmentMask() : 0,
					(uint32)desc.RTVFormats[0],
					(uint32)desc.RTVFormats[1],
					(uint32)desc.RTVFormats[2],
					(uint32)desc.RTVFormats[3],
					(uint32)desc.RTVFormats[4],
					(uint32)desc.RTVFormats[5],
					(uint32)desc.RTVFormats[6],
					(uint32)desc.RTVFormats[7],
					(uint32)desc.DSVFormat,
					vertexShader ? vertexShader->baseHash : 0,
					geometryShader ? geometryShader->baseHash : 0,
					pixelShader ? pixelShader->baseHash : 0,
					(const void*)insertedIt->second.pipeline.Get(),
					m_gamePipelineCache.size());
	return insertedIt->second.pipeline.Get();
}

RendererShaderD3D12* D3D12Renderer::GetSurfaceCopyShader(RendererShader::ShaderType type, const char* source)
{
	auto& shader = m_surfaceCopyShaders[source];
	if (!shader)
		shader = std::make_unique<RendererShaderD3D12>(type, 0, 0, false, false, source);
	return shader.get();
}

ID3D12PipelineState* D3D12Renderer::GetSurfaceCopyPipeline(SurfaceCopyMode mode, DXGI_FORMAT rtvFormat, DXGI_FORMAT dsvFormat)
{
	if (!m_surfaceCopyRootSignature)
	{
		D3D12TraceEvent("surface_copy_pipeline_unavailable", "reason=missing_root mode={} rtv={} dsv={}", (uint32)mode, (uint32)rtvFormat, (uint32)dsvFormat);
		return nullptr;
	}

	const uint64 key = ((uint64)mode << 56) ^ ((uint64)(uint32)rtvFormat << 24) ^ (uint64)(uint32)dsvFormat;
	auto it = m_surfaceCopyPipelineCache.find(key);
	if (it != m_surfaceCopyPipelineCache.end())
	{
		D3D12TraceEvent("surface_copy_pipeline_cache_hit", "key=0x{:016x} mode={} rtv={} dsv={} pipeline={}",
						key,
						(uint32)mode,
						(uint32)rtvFormat,
						(uint32)dsvFormat,
						(const void*)it->second.Get());
		return it->second.Get();
	}

	const char* pixelShaderSource = kSurfaceCopyColorToColorPixelShader;
	if (mode == SurfaceCopyMode::DepthToColor)
		pixelShaderSource = kSurfaceCopyDepthToColorPixelShader;
	else if (mode == SurfaceCopyMode::ColorToDepth)
		pixelShaderSource = kSurfaceCopyColorToDepthPixelShader;
	else if (mode == SurfaceCopyMode::DepthToDepth)
		pixelShaderSource = kSurfaceCopyDepthToDepthPixelShader;
	else if (mode == SurfaceCopyMode::StencilToStencil)
		pixelShaderSource = kSurfaceCopyStencilToStencilPixelShader;

	RendererShaderD3D12& vertexShader = *GetSurfaceCopyShader(RendererShader::ShaderType::kVertex, kSurfaceCopyVertexShader);
	RendererShaderD3D12& pixelShader = *GetSurfaceCopyShader(RendererShader::ShaderType::kFragment, pixelShaderSource);
	if (!vertexShader.WaitForCompiled() || !pixelShader.WaitForCompiled() || !vertexShader.HasDXIL() || !pixelShader.HasDXIL())
	{
		D3D12TraceEvent("surface_copy_pipeline_unavailable", "reason=shader_dxil mode={} rtv={} dsv={} vsDxil={} psDxil={}",
						(uint32)mode,
						(uint32)rtvFormat,
						(uint32)dsvFormat,
						vertexShader.HasDXIL() ? 1 : 0,
						pixelShader.HasDXIL() ? 1 : 0);
		return nullptr;
	}

	D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
	desc.pRootSignature = m_surfaceCopyRootSignature.Get();
	desc.VS = { vertexShader.GetDXIL().data(), vertexShader.GetDXIL().size() };
	desc.PS = { pixelShader.GetDXIL().data(), pixelShader.GetDXIL().size() };
	desc.BlendState.RenderTarget[0].RenderTargetWriteMask =
		mode == SurfaceCopyMode::DepthToColor ? D3D12_COLOR_WRITE_ENABLE_RED : D3D12_COLOR_WRITE_ENABLE_ALL;
	desc.SampleMask = UINT_MAX;
	desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
	desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
	desc.RasterizerState.FrontCounterClockwise = FALSE;
	desc.RasterizerState.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
	desc.RasterizerState.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
	desc.RasterizerState.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
	desc.RasterizerState.DepthClipEnable = TRUE;
	desc.RasterizerState.MultisampleEnable = FALSE;
	desc.RasterizerState.AntialiasedLineEnable = FALSE;
	desc.RasterizerState.ForcedSampleCount = 0;
	desc.RasterizerState.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
	desc.InputLayout = {};
	desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	desc.SampleDesc.Count = 1;

	const bool writesDepth =
		mode == SurfaceCopyMode::ColorToDepth
		|| mode == SurfaceCopyMode::DepthToDepth
		;
	if (writesDepth)
	{
		desc.DepthStencilState.DepthEnable = TRUE;
		desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
		desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
		desc.DepthStencilState.StencilEnable = FALSE;
		desc.NumRenderTargets = 0;
		desc.DSVFormat = dsvFormat;
	}
	else if (mode == SurfaceCopyMode::StencilToStencil)
	{
		const D3D12_DEPTH_STENCILOP_DESC stencilOp{ D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_REPLACE, D3D12_COMPARISON_FUNC_ALWAYS };
		desc.DepthStencilState.DepthEnable = FALSE;
		desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
		desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
		desc.DepthStencilState.StencilEnable = TRUE;
		desc.DepthStencilState.StencilReadMask = 0xFF;
		desc.DepthStencilState.StencilWriteMask = 0xFF;
		desc.DepthStencilState.FrontFace = stencilOp;
		desc.DepthStencilState.BackFace = stencilOp;
		desc.NumRenderTargets = 0;
		desc.DSVFormat = dsvFormat;
	}
	else
	{
		desc.DepthStencilState.DepthEnable = FALSE;
		desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
		desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
		desc.DepthStencilState.StencilEnable = FALSE;
		desc.NumRenderTargets = 1;
		desc.RTVFormats[0] = rtvFormat;
	}

	D3D12TraceEvent("surface_copy_pipeline_create_begin", "key=0x{:016x} mode={} rtv={} dsv={} vsBytes={} psBytes={} rootSig={}",
					key,
					(uint32)mode,
					(uint32)rtvFormat,
					(uint32)dsvFormat,
					(unsigned long long)desc.VS.BytecodeLength,
					(unsigned long long)desc.PS.BytecodeLength,
					(const void*)m_surfaceCopyRootSignature.Get());
	Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline;
	const HRESULT hr = m_device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pipeline));
	if (FAILED(hr))
	{
		cemuLog_log(LogType::Force, "D3D12: failed to create surface-copy PSO (HRESULT 0x{:08x})", (uint32)hr);
		D3D12TraceEvent("surface_copy_pipeline_create_failed", "key=0x{:016x} hr=0x{:08x} mode={} rtv={} dsv={} vsBytes={} psBytes={}",
						key,
						(uint32)hr,
						(uint32)mode,
						(uint32)rtvFormat,
						(uint32)dsvFormat,
						(unsigned long long)desc.VS.BytecodeLength,
						(unsigned long long)desc.PS.BytecodeLength);
		return nullptr;
	}
	const wchar_t* debugName = L"Cemu D3D12 color-to-color copy PSO";
	if (mode == SurfaceCopyMode::DepthToColor)
		debugName = L"Cemu D3D12 depth-to-color copy PSO";
	else if (mode == SurfaceCopyMode::ColorToDepth)
		debugName = L"Cemu D3D12 color-to-depth copy PSO";
	else if (mode == SurfaceCopyMode::StencilToStencil)
		debugName = L"Cemu D3D12 stencil-to-stencil copy PSO";
	SetD3D12ObjectName(pipeline.Get(), debugName);
	auto [insertedIt, inserted] = m_surfaceCopyPipelineCache.emplace(key, std::move(pipeline));
	(void)inserted;
	D3D12TraceEvent("surface_copy_pipeline_create_success", "key=0x{:016x} mode={} rtv={} dsv={} pipeline={} cacheSize={}",
					key,
					(uint32)mode,
					(uint32)rtvFormat,
					(uint32)dsvFormat,
					(const void*)insertedIt->second.Get(),
					m_surfaceCopyPipelineCache.size());
	return insertedIt->second.Get();
}

