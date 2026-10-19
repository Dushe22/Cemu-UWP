#include "Cafe/HW/Latte/Renderer/D3D12/D3D12DepthReadbackProbe.h"

#if defined(CEMU_UWP_D3D12_DIAGNOSTICS)

#include "Cemu/Logging/CemuLogging.h"
#include "config/ActiveSettings.h"

#include <Windows.h>
#include <d3d12.h>
#include <dxcapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>

namespace D3D12DepthReadbackProbe
{
namespace
{
using Microsoft::WRL::ComPtr;
constexpr UINT kWidth = 8, kHeight = 8, kPixels = kWidth * kHeight;
constexpr DWORD kWaitMilliseconds = 2000;
constexpr UINT64 kMaximumReadbackBytes = 64 * 1024;
constexpr std::array<float, 4> kDepthValues{0.0f, 0.25f, 0.75f, 1.0f};

void Check(HRESULT result, const char* operation)
{
    if (FAILED(result))
        throw std::runtime_error(fmt::format("{} hr=0x{:08x}", operation, static_cast<uint32_t>(result)));
}

void Require(bool condition, const char* operation)
{
    if (!condition) throw std::runtime_error(operation);
}

struct Readback
{
    ComPtr<ID3D12Resource> resource;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT rows{};
    UINT64 rowBytes{}, bytes{};
};

struct Context
{
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pipeline;
    ComPtr<ID3D12DescriptorHeap> dsvHeap, rtvHeap, srvHeap;
    ComPtr<ID3D12Resource> depth, sampled;
    std::array<Readback, kDepthValues.size()> rawReadbacks, sampledReadbacks;
    HANDLE event{};
    bool submitted{}, completed{};
    ~Context() { if (event) CloseHandle(event); }
};

ComPtr<ID3D12Resource> CreateResource(ID3D12Device* device, const D3D12_RESOURCE_DESC& desc,
    D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE* clear = nullptr)
{
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = type;
    heap.CreationNodeMask = heap.VisibleNodeMask = 1;
    ComPtr<ID3D12Resource> resource;
    Check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, clear,
        IID_PPV_ARGS(&resource)), "CreateCommittedResource");
    return resource;
}

D3D12_RESOURCE_DESC TextureDesc(DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags)
{
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = kWidth;
    desc.Height = kHeight;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Flags = flags;
    return desc;
}

ComPtr<ID3D12DescriptorHeap> CreateHeap(ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_TYPE type, bool visible)
{
    D3D12_DESCRIPTOR_HEAP_DESC desc{};
    desc.Type = type;
    desc.NumDescriptors = 1;
    desc.Flags = visible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    ComPtr<ID3D12DescriptorHeap> heap;
    Check(device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&heap)), "CreateDescriptorHeap");
    return heap;
}

ComPtr<IDxcBlob> Compile(IDxcCompiler3* compiler, const char* source, const wchar_t* entry, const wchar_t* profile)
{
    const DxcBuffer buffer{source, std::strlen(source), DXC_CP_UTF8};
    std::array<LPCWSTR, 9> args{L"-E", entry, L"-T", profile, L"-HV", L"2021", L"-O3", L"-Qstrip_reflect", L"-Qstrip_debug"};
    ComPtr<IDxcResult> result;
    Check(compiler->Compile(&buffer, args.data(), static_cast<UINT32>(args.size()), nullptr,
        IID_PPV_ARGS(&result)), "DXC Compile");
    Require(result != nullptr, "DXC returned null result");
    HRESULT status = E_FAIL;
    Check(result->GetStatus(&status), "DXC GetStatus");
    Check(status, "DXC shader status");
    ComPtr<IDxcBlob> object;
    Check(result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&object), nullptr), "DXC object");
    Require(object && object->GetBufferSize() != 0, "DXC empty object");
    return object;
}

void Transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource,
    D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    list->ResourceBarrier(1, &barrier);
}

Readback CreateReadback(ID3D12Device* device, ID3D12Resource* source)
{
    Readback readback;
    const auto sourceDesc = source->GetDesc();
    device->GetCopyableFootprints(&sourceDesc, 0, 1, 0, &readback.footprint,
        &readback.rows, &readback.rowBytes, &readback.bytes);
    const auto& footprint = readback.footprint.Footprint;
    Require(footprint.Width == kWidth && footprint.Height == kHeight && footprint.Depth == 1 &&
        readback.rows == kHeight && readback.rowBytes == kWidth * sizeof(uint32_t) &&
        footprint.RowPitch >= readback.rowBytes && readback.footprint.Offset == 0 &&
        readback.bytes != 0 && readback.bytes <= kMaximumReadbackBytes &&
        static_cast<UINT64>(footprint.RowPitch) * (kHeight - 1) + readback.rowBytes <= readback.bytes,
        "Unexpected/bounded readback layout");
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = readback.bytes;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    readback.resource = CreateResource(device, desc, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    return readback;
}

void CopyToReadback(ID3D12GraphicsCommandList* list, ID3D12Resource* source, const Readback& readback)
{
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = source;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0; // depth plane only
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = readback.resource.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = readback.footprint;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
}

std::array<uint32_t, kPixels> ReadWords(const Readback& readback)
{
    std::array<uint32_t, kPixels> words{};
    const D3D12_RANGE readRange{0, static_cast<SIZE_T>(readback.bytes)};
    const D3D12_RANGE writtenRange{0, 0};
    void* base = nullptr;
    const HRESULT result = readback.resource->Map(0, &readRange, &base);
    if (FAILED(result)) Check(result, "Map after fence");
    if (!base)
    {
        readback.resource->Unmap(0, &writtenRange);
        throw std::runtime_error("Map after fence returned null");
    }
    // must not throw between Map and Unmap
    for (UINT y = 0; y < kHeight; ++y)
        std::memcpy(words.data() + y * kWidth,
            static_cast<const uint8_t*>(base) + y * readback.footprint.Footprint.RowPitch,
            kWidth * sizeof(uint32_t));
    readback.resource->Unmap(0, &writtenRange);
    return words;
}

void Prepare(Context& context)
{
    auto* device = context.device.Get();
    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    Check(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&context.queue)), "CreateCommandQueue");
    Check(device->CreateCommandAllocator(queueDesc.Type, IID_PPV_ARGS(&context.allocator)), "CreateCommandAllocator");
    Check(device->CreateCommandList(0, queueDesc.Type, context.allocator.Get(), nullptr,
        IID_PPV_ARGS(&context.list)), "CreateCommandList");
    Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&context.fence)), "CreateFence");
    context.event = CreateEventExW(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
    Require(context.event != nullptr, "CreateEventExW");
    context.dsvHeap = CreateHeap(device, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, false);
    context.rtvHeap = CreateHeap(device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, false);
    context.srvHeap = CreateHeap(device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, true);

    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 1;
    D3D12_ROOT_PARAMETER parameter{};
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameter.DescriptorTable = {1, &range};
    parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC rootDesc{};
    rootDesc.NumParameters = 1;
    rootDesc.pParameters = &parameter;
    rootDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ComPtr<ID3DBlob> rootBytes, rootErrors;
    Check(D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &rootBytes, &rootErrors), "SerializeRootSignature");
    Check(device->CreateRootSignature(0, rootBytes->GetBufferPointer(), rootBytes->GetBufferSize(),
        IID_PPV_ARGS(&context.root)), "CreateRootSignature");

    constexpr const char* source = R"(
float4 VS(uint id : SV_VertexID) : SV_Position {
    float2 p[3] = { float2(-1,-1), float2(-1,3), float2(3,-1) };
    return float4(p[id], 0.5, 1);
}
Texture2D<float> sourceTexture : register(t0);
float PS(float4 position : SV_Position) : SV_Target0 {
    return sourceTexture.Load(int3(int2(position.xy), 0));
}
)";
    ComPtr<IDxcCompiler3> compiler;
    Check(DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler)), "DxcCreateInstance");
    const auto vs = Compile(compiler.Get(), source, L"VS", L"vs_6_4");
    const auto ps = Compile(compiler.Get(), source, L"PS", L"ps_6_4");
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pipeline{};
    pipeline.pRootSignature = context.root.Get();
    pipeline.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    pipeline.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    pipeline.SampleDesc.Count = 1;
    pipeline.SampleMask = UINT_MAX;
    pipeline.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pipeline.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pipeline.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pipeline.RasterizerState.DepthClipEnable = TRUE;
    pipeline.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    pipeline.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    pipeline.DepthStencilState.FrontFace = {D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP,
        D3D12_STENCIL_OP_KEEP, D3D12_COMPARISON_FUNC_ALWAYS};
    pipeline.DepthStencilState.BackFace = pipeline.DepthStencilState.FrontFace;
    for (auto& target : pipeline.BlendState.RenderTarget)
    {
        target.SrcBlend = target.SrcBlendAlpha = D3D12_BLEND_ONE;
        target.DestBlend = target.DestBlendAlpha = D3D12_BLEND_ZERO;
        target.BlendOp = target.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        target.LogicOp = D3D12_LOGIC_OP_NOOP;
        target.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    }
    pipeline.NumRenderTargets = 1;
    pipeline.RTVFormats[0] = DXGI_FORMAT_R32_FLOAT;
    Check(device->CreateGraphicsPipelineState(&pipeline, IID_PPV_ARGS(&context.pipeline)), "CreateGraphicsPipelineState");

    D3D12_CLEAR_VALUE optimizedClear{};
    optimizedClear.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    optimizedClear.DepthStencil = {1.0f, 0x5a};
    context.depth = CreateResource(device, TextureDesc(DXGI_FORMAT_R24G8_TYPELESS, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL),
        D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_DEPTH_WRITE, &optimizedClear);
    context.sampled = CreateResource(device, TextureDesc(DXGI_FORMAT_R32_TYPELESS, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET),
        D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};
    dsv.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    device->CreateDepthStencilView(context.depth.Get(), &dsv, context.dsvHeap->GetCPUDescriptorHandleForHeapStart());
    D3D12_RENDER_TARGET_VIEW_DESC rtv{};
    rtv.Format = DXGI_FORMAT_R32_FLOAT;
    rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    device->CreateRenderTargetView(context.sampled.Get(), &rtv, context.rtvHeap->GetCPUDescriptorHandleForHeapStart());
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(context.depth.Get(), &srv, context.srvHeap->GetCPUDescriptorHandleForHeapStart());
    for (size_t index = 0; index < kDepthValues.size(); ++index)
    {
        context.rawReadbacks[index] = CreateReadback(device, context.depth.Get());
        context.sampledReadbacks[index] = CreateReadback(device, context.sampled.Get());
    }
}

void Record(Context& context)
{
    auto* list = context.list.Get();
    list->SetGraphicsRootSignature(context.root.Get());
    ID3D12DescriptorHeap* heaps[]{context.srvHeap.Get()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetGraphicsRootDescriptorTable(0, context.srvHeap->GetGPUDescriptorHandleForHeapStart());
    list->SetPipelineState(context.pipeline.Get());
    const D3D12_VIEWPORT viewport{0, 0, static_cast<float>(kWidth), static_cast<float>(kHeight), 0, 1};
    const D3D12_RECT scissor{0, 0, kWidth, kHeight};
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &scissor);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    const auto rtv = context.rtvHeap->GetCPUDescriptorHandleForHeapStart();
    const auto dsv = context.dsvHeap->GetCPUDescriptorHandleForHeapStart();
    for (size_t index = 0; index < kDepthValues.size(); ++index)
    {
        if (index != 0)
        {
            Transition(list, context.depth.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
            Transition(list, context.sampled.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        }
        list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, kDepthValues[index], 0x5a, 0, nullptr);
        Transition(list, context.depth.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_COPY_SOURCE);
        CopyToReadback(list, context.depth.Get(), context.rawReadbacks[index]);
        Transition(list, context.depth.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        // sentinel makes a skipped or failed copy observable
        const float sentinel[4]{-7, -7, -7, -7};
        list->ClearRenderTargetView(rtv, sentinel, 0, nullptr);
        list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        list->DrawInstanced(3, 1, 0, 0);
        list->OMSetRenderTargets(0, nullptr, FALSE, nullptr);
        Transition(list, context.sampled.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        CopyToReadback(list, context.sampled.Get(), context.sampledReadbacks[index]);
    }
    Check(list->Close(), "CloseCommandList");
}

void LogResult(const Context& context, size_t index)
{
    const auto raw = ReadWords(context.rawReadbacks[index]);
    const auto sampled = ReadWords(context.sampledReadbacks[index]);
    const double expected = kDepthValues[index];
    double lowMin = 1, lowMax = 0;
    double rawMin = std::numeric_limits<double>::max(), rawMax = -rawMin;
    double sampleMin = rawMin, sampleMax = rawMax;
    UINT lowMatches = 0, rawMatches = 0, sampleMatches = 0, identical = 0, highBytes = 0;
    UINT rawFinite = 0, sampleFinite = 0;
    for (UINT pixel = 0; pixel < kPixels; ++pixel)
    {
        const double low = static_cast<double>(raw[pixel] & 0xffffffu) / 16777215.0;
        float rawFloat{}, sampleFloat{};
        std::memcpy(&rawFloat, &raw[pixel], sizeof(float));
        std::memcpy(&sampleFloat, &sampled[pixel], sizeof(float));
        lowMin = (std::min)(lowMin, low); lowMax = (std::max)(lowMax, low);
        lowMatches += std::abs(low - expected) <= 1.0 / 16777215.0;
        if (std::isfinite(rawFloat))
        {
            ++rawFinite;
            rawMin = (std::min)(rawMin, static_cast<double>(rawFloat)); rawMax = (std::max)(rawMax, static_cast<double>(rawFloat));
            rawMatches += std::abs(static_cast<double>(rawFloat) - expected) <= 1e-6;
        }
        if (std::isfinite(sampleFloat))
        {
            ++sampleFinite;
            sampleMin = (std::min)(sampleMin, static_cast<double>(sampleFloat)); sampleMax = (std::max)(sampleMax, static_cast<double>(sampleFloat));
            sampleMatches += std::abs(static_cast<double>(sampleFloat) - expected) <= 1e-6;
        }
        identical += raw[pixel] == sampled[pixel];
        highBytes += (raw[pixel] >> 24) != 0;
    }
    const auto& rawReadback = context.rawReadbacks[index];
    const auto& sampledReadback = context.sampledReadbacks[index];
    cemuLog_log(LogType::Force,
        "D3D12 UWP depth probe: case={} expected={} pixels={} map=after_fence rawResourceDxgi={} sampledResourceDxgi={} rawFootprintDxgi={} sampledFootprintDxgi={} rawRowBytes={} sampledRowBytes={} rawRowPitch={} sampledRowPitch={} rawBytes={} sampledBytes={} raw0=0x{:08x} sampled0=0x{:08x} low24MinMax={:.9g}/{:.9g} rawFloatMinMax={:.9g}/{:.9g} sampledFloatMinMax={:.9g}/{:.9g} low24Expected={} rawFloatExpected={} sampledExpected={} rawFinite={} sampledFinite={} identicalWords={} nonzeroHighBytes={} interpretation=observations_only.",
        index, expected, kPixels, static_cast<UINT>(context.depth->GetDesc().Format), static_cast<UINT>(context.sampled->GetDesc().Format),
        static_cast<UINT>(rawReadback.footprint.Footprint.Format), static_cast<UINT>(sampledReadback.footprint.Footprint.Format),
        rawReadback.rowBytes, sampledReadback.rowBytes, rawReadback.footprint.Footprint.RowPitch, sampledReadback.footprint.Footprint.RowPitch,
        rawReadback.bytes, sampledReadback.bytes, raw[0], sampled[0], lowMin, lowMax,
        rawFinite ? rawMin : 0, rawFinite ? rawMax : 0, sampleFinite ? sampleMin : 0, sampleFinite ? sampleMax : 0,
        lowMatches, rawMatches, sampleMatches, rawFinite, sampleFinite, identical, highBytes);
}
}

void RunOnce(ID3D12Device* device) noexcept
{
    static std::atomic_flag attempted = ATOMIC_FLAG_INIT;
    if (!device || attempted.test_and_set(std::memory_order_relaxed)) return;
    std::unique_ptr<Context> context;
    try
    {
        std::error_code error;
        if (!std::filesystem::exists(ActiveSettings::GetUserDataPath("diagnostics/d3d12_gpu_capture.txt"), error) || error)
            return;
        cemuLog_log(LogType::Force, "D3D12 UWP depth probe: begin cases=4 size=8x8 privateQueue=1 waitLimitMs={} titleResources=0.", kWaitMilliseconds);
        context = std::make_unique<Context>();
        context->device = device;
        Prepare(*context);
        Record(*context);
        ID3D12CommandList* lists[]{context->list.Get()};
        context->submitted = true;
        context->queue->ExecuteCommandLists(1, lists);
        Check(context->queue->Signal(context->fence.Get(), 1), "Queue Signal");
        Check(context->fence->SetEventOnCompletion(1, context->event), "SetEventOnCompletion");
        const DWORD waitResult = WaitForSingleObjectEx(context->event, kWaitMilliseconds, FALSE);
        const UINT64 completedFence = context->fence->GetCompletedValue();
        context->completed = completedFence != UINT64_MAX && completedFence >= 1;
        if (!context->completed)
            throw std::runtime_error(fmt::format("GPU completion uncertain wait={} fence={} deviceHr=0x{:08x}",
                waitResult, completedFence, static_cast<uint32_t>(device->GetDeviceRemovedReason())));
        for (size_t index = 0; index < kDepthValues.size(); ++index) LogResult(*context, index);
        cemuLog_log(LogType::Force, "D3D12 UWP depth probe: complete cases=4 fence=1 nativeLayoutNotAssumed=1 titleResources=0.");
    }
    catch (const std::exception& exception)
    {
        cemuLog_log(LogType::Force, "D3D12 UWP depth probe: failed {}.", exception.what());
    }
    catch (...)
    {
        cemuLog_log(LogType::Force, "D3D12 UWP depth probe: failed unknown exception.");
    }
    if (context && context->submitted && !context->completed)
    {
        // a timed-out queue may still touch these objects; leak rather than release
        (void)context.release();
        cemuLog_log(LogType::Force, "D3D12 UWP depth probe: retained one private bundle until process exit because GPU completion is uncertain.");
    }
}
}
#endif
