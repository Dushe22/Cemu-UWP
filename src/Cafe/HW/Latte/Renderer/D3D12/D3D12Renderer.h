#pragma once

#include <chrono>

#include "Cafe/HW/Latte/Renderer/Renderer.h"
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12DescriptorManager.h"
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12GpuWatchdog.h"
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12MemoryManager.h"
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12PipelineCompiler.h"
#include "gui/interface/WindowSystem.h"
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12PerformanceExperiment.h"

#if BOOST_OS_WINDOWS
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#endif

#include <array>
#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

bool InitializeGlobalD3D12();
extern bool g_d3d12_available;

class CachedFBOD3D12;
class LatteTextureD3D12;
class LatteTextureViewD3D12;
class LatteQueryObjectD3D12;
class LatteTextureReadbackInfoD3D12;
class RendererShaderD3D12;
class D3D12GeometryProbe;
struct LatteDecompilerShader;
struct LatteFetchShader;

class D3D12Renderer : public Renderer
{
public:
	struct DeviceInfo
	{
		DeviceInfo(const std::string& name, const LUID& luid);

		std::string name;
		std::array<uint8, sizeof(LUID)> luid{};
	};

	static std::vector<DeviceInfo> GetDevices();

	D3D12Renderer();
	~D3D12Renderer() override;

	ID3D12Device* GetDevice() const { return m_device.Get(); }
	D3D12DescriptorManager* GetDescriptorManager() const { return m_descriptorManager.get(); }
	// D3D12 reports some invalid arguments only by removing the device. These descriptor calls log the first call
	// after which the device is gone, with its arguments. Frequent calls are checked only while frames are stalled.
	void CreateShaderResourceViewChecked(ID3D12Resource* resource, const D3D12_SHADER_RESOURCE_VIEW_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE destination, const char* purpose, bool frequent);
	void CreateRenderTargetViewChecked(ID3D12Resource* resource, const D3D12_RENDER_TARGET_VIEW_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE destination, const char* purpose);
	void CreateDepthStencilViewChecked(ID3D12Resource* resource, const D3D12_DEPTH_STENCIL_VIEW_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE destination, const char* purpose);
	void CreateUnorderedAccessViewChecked(ID3D12Resource* resource, const D3D12_UNORDERED_ACCESS_VIEW_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE destination, const char* purpose);
	void CreateSamplerChecked(const D3D12_SAMPLER_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE destination, const char* purpose);
	void CopyDescriptorsSimpleChecked(UINT count, D3D12_CPU_DESCRIPTOR_HANDLE destination, D3D12_CPU_DESCRIPTOR_HANDLE source, D3D12_DESCRIPTOR_HEAP_TYPE type, const char* purpose);
	D3D12DescriptorManager::Allocation CopyToShaderVisibleChecked(const D3D12DescriptorManager::Allocation& source, const char* purpose);
	// for other calls: true when the caller should check the device now
	bool ShouldCheckDevice(bool frequent) const;
	void ReportDeviceRemovalAfter(const char* call, const char* purpose, const std::string& arguments);
	// while frames are stalled, names the renderer operation after which the device is gone
	template<typename Describe>
	void CheckDeviceAfterOperation(const char* operation, Describe&& describe) noexcept
	{
		if (!ShouldCheckDevice(true) || SUCCEEDED(m_device->GetDeviceRemovedReason()))
			return;
		try
		{
			ReportDeviceRemovalAfter(operation, "renderer operation", describe());
		}
		catch (...)
		{
		}
	}
	D3D12MemoryManager* GetMemoryManager() const { return m_memoryManager.get(); }
	bool IsUma() const { return m_isUma; }
	void ReleaseResourceDeferred(D3D12MemoryManager::Resource& resource);
	void NotifyTextureViewDescriptorsReleased(const class LatteTextureViewD3D12* view);
	bool CreateTextureWithRecovery(const D3D12_RESOURCE_DESC& desc, D3D12_RESOURCE_STATES initialState, const D3D12_CLEAR_VALUE* clearValue, D3D12MemoryManager::Resource& outResource, const char* context);
	bool RefreshRanged3DSamplingProxy(class LatteTextureViewD3D12* view);
#if defined(CEMU_UWP_D3D12_CORRECTNESS_TELEMETRY)
	void RecordIncompatibleTextureView(DXGI_FORMAT resourceFormat, DXGI_FORMAT viewFormat);
#endif
	void PrepareGamePipelineLibrary(uint64 titleId);
	void BeginGamePipelineRecipeLoading(uint64 titleId);
	enum class RecipePrewarmResult
	{
		Created,
		Rejected, // the recipe can not be used and should be replaced
		Stopped,
	};
	// safe on prewarm worker threads: builds the pipeline a recipe describes and hands it to the render thread
	RecipePrewarmResult PrewarmGamePipelineRecipe(uint64 expectedPipelineKey, std::span<const uint8> bytes);
	// render thread: makes handed-over pipelines available to draws
	size_t AdoptPrewarmedGamePipelines();
	bool HasLiveGamePipeline(uint64 pipelineKey) const { return m_gamePipelineCache.contains(pipelineKey); }
	// end of the part of the prewarm that launches wait for; the rest may continue in the background
	void EndGamePipelineRecipeLoading(bool continuesInBackground);
	// safe on prewarm worker threads
	void FinishBackgroundGamePipelineRecipePrewarm();
	void ClearPendingGamePipelineRecipes();
	const char* GetGamePipelineRecipePrewarmStopReason() const { return m_gamePipelineRecipePrewarmStopReason.load(); }
	bool IsGamePipelineRecipePrewarmStopped() const
	{
		return !m_gamePipelineRecipeLoading.load(std::memory_order_acquire) ||
			m_gamePipelineRecipePrewarmStopped.load(std::memory_order_acquire);
	}
	void PrepareForTitleShutdown();
	void FinishTitleShutdown();

	static D3D12Renderer* GetInstance();

	void Initialize() override;
	void Shutdown() override;

	void InitializeSurface(const Vector2i& size, bool mainWindow);
	void ResizeSurface(const Vector2i& size, bool mainWindow);
	void StopUsingPadAndWait();
	bool IsPadWindowActive() override;
	// where the TV or GamePad picture sits in the window for the current screen layout, in pixels
	bool GetScreenOutputRect(bool padView, sint32& x, sint32& y, sint32& width, sint32& height) const;

	bool GetVRAMInfo(int& usageInMB, int& totalInMB) const override;
	void AppendOverlayDebugInfo() override;

	void ClearColorbuffer(bool padView) override;
	void DrawEmptyFrame(bool mainWindow) override;
	void SwapBuffers(bool swapTV, bool swapDRC) override;
	bool BeginFrame(bool mainWindow) override;
	void HandleScreenshotRequest(LatteTextureView* texView, bool padView) override;
	void DrawBackbufferQuad(LatteTextureView* texView, RendererOutputShader* shader, bool useLinearTexFilter,
							sint32 imageX, sint32 imageY, sint32 imageWidth, sint32 imageHeight,
							bool padView, bool clearBackground) override;

	void Flush(bool waitIdle = false) override;
	void NotifyLatteCommandProcessorIdle() override;
	void NotifyDepthBufferExpanded(MPTR depthBufferMPTR, Latte::E_GX2SURFFMT depthBufferFormat, uint32 width, uint32 height, uint32 pitch, uint32 viewMip, uint32 viewFirstSlice, uint32 viewNumSlices) override;
	void NotifyTextureDeletion(LatteTexture* texture) override;
	void SurfaceSync(Latte::E_COHER_CNTL coher, MPTR address, uint32 size) override;

	bool ImguiBegin(bool mainWindow) override;
	void ImguiEnd() override;
	ImTextureID GenerateTexture(const std::vector<uint8>& data, const Vector2i& size) override;
	void DeleteTexture(ImTextureID id) override;
	bool UpdateTexture(ImTextureID id, const std::vector<uint8>& data, const Vector2i& size);
	ImTextureID GenerateTextureBGRA(const std::vector<uint8>& data, const Vector2i& size);
	bool UpdateTextureBGRA(ImTextureID id, const std::vector<uint8>& data, const Vector2i& size);
	void DeleteFontTextures() override;

	void renderTarget_setViewport(float x, float y, float width, float height, float nearZ, float farZ, bool halfZ = false) override;
	void renderTarget_setScissor(sint32 scissorX, sint32 scissorY, sint32 scissorWidth, sint32 scissorHeight) override;
	LatteCachedFBO* rendertarget_createCachedFBO(uint64 key) override;
	void rendertarget_deleteCachedFBO(LatteCachedFBO* cfbo) override;
	void rendertarget_bindFramebufferObject(LatteCachedFBO* cfbo) override;

	void* texture_acquireTextureUploadBuffer(uint32 size) override;
	void texture_releaseTextureUploadBuffer(uint8* mem) override;
	TextureDecoder* texture_chooseDecodedFormat(Latte::E_GX2SURFFMT format, bool isDepth, Latte::E_DIM dim, uint32 width, uint32 height) override;
	void texture_clearSlice(LatteTexture* hostTexture, sint32 sliceIndex, sint32 mipIndex) override;
	void texture_loadSlice(LatteTexture* hostTexture, sint32 width, sint32 height, sint32 depth, void* pixelData, sint32 sliceIndex, sint32 mipIndex, uint32 compressedImageSize) override;
	void texture_clearColorSlice(LatteTexture* hostTexture, sint32 sliceIndex, sint32 mipIndex, float r, float g, float b, float a) override;
	void texture_clearDepthSlice(LatteTexture* hostTexture, uint32 sliceIndex, sint32 mipIndex, bool clearDepth, bool clearStencil, float depthValue, uint32 stencilValue) override;
	LatteTexture* texture_createTextureEx(Latte::E_DIM dim, MPTR physAddress, MPTR physMipAddress, Latte::E_GX2SURFFMT format, uint32 width, uint32 height, uint32 depth, uint32 pitch, uint32 mipLevels, uint32 swizzle, Latte::E_HWTILEMODE tileMode, bool isDepth) override;
	void texture_setLatteTexture(LatteTextureView* textureView, uint32 textureUnit) override;
	void texture_copyImageSubData(LatteTexture* src, sint32 srcMip, sint32 effectiveSrcX, sint32 effectiveSrcY, sint32 srcSlice, LatteTexture* dst, sint32 dstMip, sint32 effectiveDstX, sint32 effectiveDstY, sint32 dstSlice, sint32 effectiveCopyWidth, sint32 effectiveCopyHeight, sint32 srcDepth) override;
	LatteTextureReadbackInfo* texture_createReadback(LatteTextureView* textureView) override;

	void surfaceCopy_copySurfaceWithFormatConversion(LatteTexture* sourceTexture, sint32 srcMip, sint32 srcSlice, LatteTexture* destinationTexture, sint32 dstMip, sint32 dstSlice, sint32 width, sint32 height) override;

	void bufferCache_init(const sint32 bufferSize) override;
	void bufferCache_upload(uint8* buffer, sint32 size, uint32 bufferOffset) override;
	void bufferCache_copy(uint32 srcOffset, uint32 dstOffset, uint32 size) override;
	void bufferCache_copyStreamoutToMainBuffer(uint32 srcOffset, uint32 dstOffset, uint32 size) override;
	void buffer_bindVertexBuffer(uint32 bufferIndex, uint32 offset, uint32 size) override;
	void buffer_bindUniformBuffer(LatteConst::ShaderType shaderType, uint32 bufferIndex, uint32 offset, uint32 size) override;

	RendererShader* shader_create(RendererShader::ShaderType type, uint64 baseHash, uint64 auxHash, const std::string& source, bool isGameShader, bool isGfxPackSource) override;

	void streamout_setupXfbBuffer(uint32 bufferIndex, sint32 ringBufferOffset, uint32 rangeAddr, uint32 rangeSize) override;
	void streamout_begin() override;
	void streamout_rendererFinishDrawcall() override;

	void draw_beginSequence() override;
	void draw_execute(uint32 baseVertex, uint32 baseInstance, uint32 instanceCount, uint32 count, MPTR indexDataMPTR, Latte::LATTE_VGT_DMA_INDEX_TYPE::E_INDEX_TYPE indexType, const LatteDrawcallContext& drawcallContext) override;
	void draw_endSequence() override;

	IndexAllocation indexData_reserveIndexMemory(uint32 size) override;
	void indexData_releaseIndexMemory(IndexAllocation& allocation) override;
	void indexData_uploadIndexMemory(IndexAllocation& allocation) override;

	LatteQueryObject* occlusionQuery_create() override;
	void occlusionQuery_destroy(LatteQueryObject* queryObj) override;
	void occlusionQuery_flush() override;
	void occlusionQuery_updateState() override;

	bool UseTFViaSSBO() const override { return true; }

private:
	friend class LatteQueryObjectD3D12;
	friend class LatteTextureReadbackInfoD3D12;

	static constexpr uint32 kBackBufferCount = 3;
	static constexpr uint32 kCommandBufferPoolSize = 8;
	static constexpr uint32 kUwpCpuDrawStageCount = 7;
	static constexpr uint32 kSubmitThresholdDefault = 300;
	static constexpr uint32 kSubmitThresholdUwpDefault = 480;
	static constexpr uint32 kSubmitSoonDrawDelay = 10;
	static constexpr uint32 kInvalidDescriptorIndex = 0xFFFFFFFFu;
	static constexpr uint32 kMaxTextureBindings = LATTE_CEMU_GS_TEX_UNIT_BASE + LATTE_NUM_MAX_TEX_UNITS;
	static constexpr uint32 kStreamoutBufferSize = 16u * 1024u * 1024u;
	static constexpr uint32 kOcclusionQuerySlots = 1024;

	enum class SurfaceCopyMode : uint32
	{
		ColorToColor,
		DepthToColor,
		ColorToDepth,
		DepthToDepth,
		StencilToStencil,
	};

	struct SwapchainState
	{
		bool active{};
		void* surface{};
		WindowSystem::WindowHandleInfo::Backend backend{};
		Vector2i size{};
		Microsoft::WRL::ComPtr<IDXGISwapChain3> swapchain;
		std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, kBackBufferCount> backBuffers;
		std::array<D3D12_CPU_DESCRIPTOR_HANDLE, kBackBufferCount> rtvHandles{};
		std::array<D3D12_RESOURCE_STATES, kBackBufferCount> states{};
		uint32 frameIndex{};
		uint32 rtvBaseIndex{ kInvalidDescriptorIndex };
	};

	struct CommandSlot
	{
		Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
		Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> commandList;
#if defined(__ID3D12GraphicsCommandList8_INTERFACE_DEFINED__)
		Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList8> commandList8;
#endif
#if defined(__ID3D12GraphicsCommandList9_INTERFACE_DEFINED__)
		Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList9> commandList9;
#endif
		uint64 fenceValue{};
		size_t uniformRingWatermark{};
#if defined(CEMU_UWP_D3D12_DIAGNOSTICS) || defined(CEMU_UWP_D3D12_WORKLOAD_PROFILE)
		bool timingPending{};
#endif
		uint32 readbackNotificationCount{};
		uint32 queryFragmentCount{};
	};

	struct DeferredReleaseBatch
	{
		uint64 fenceValue{};
		std::vector<Microsoft::WRL::ComPtr<IUnknown>> objects;
		std::vector<D3D12MemoryManager::Resource> resources;
		std::vector<D3D12DescriptorManager::ShaderVisibleHeapPage> descriptorHeaps;
	};

	struct BoundRange
	{
		uint32 offset{};
		uint32 size{};
		bool valid{};
	};

	struct StreamoutBinding
	{
		bool enabled{};
		sint32 ringBufferOffset{};
		uint32 rangeAddr{};
		uint32 rangeSize{};
	};

	struct D3D12IndexAllocation
	{
		std::array<uint8, 96> previewData{};
		uint32 previewDataSize{};
		struct alignas(32) DecodedIndexBlock
		{
			std::array<uint8, 32> bytes;
		};
		static_assert(alignof(DecodedIndexBlock) >= 32);
		std::vector<DecodedIndexBlock> decodedData;
		uint8* GetDecodedData() { return reinterpret_cast<uint8*>(decodedData.data()); }
		const uint8* GetDecodedData() const { return reinterpret_cast<const uint8*>(decodedData.data()); }
		size_t GetDecodedCapacity() const { return decodedData.size() * sizeof(DecodedIndexBlock); }
		ID3D12Resource* residentResource{};
		uint64 residentOffset{};
		uint64 residentFence{};
		uint32 residentSize{};
		uint32 size{};
	};

	struct ImGuiTextureResource
	{
		D3D12MemoryManager::Resource texture;
		uint32 descriptorIndex{};
		Vector2i size{};
	};

	struct RetiredImguiDescriptor
	{
		uint64 readyFence{};
		uint32 index{};
	};

	struct OcclusionQuerySlot
	{
		uint32 pageIndex{ kInvalidDescriptorIndex };
		uint32 queryIndex{ kInvalidDescriptorIndex };

		bool IsValid() const
		{
			return pageIndex != kInvalidDescriptorIndex && queryIndex != kInvalidDescriptorIndex;
		}
	};

	struct OcclusionQueryPage
	{
		Microsoft::WRL::ComPtr<ID3D12QueryHeap> heap;
		D3D12MemoryManager::Resource readback;
		std::array<bool, kOcclusionQuerySlots> slotsUsed{};
	};

	struct RetiredOcclusionQuerySlot
	{
		OcclusionQuerySlot slot;
		uint64 readyFence{};
	};

	struct UwpUnderlayTexture
	{
		std::string path;
		ImTextureID id{};
		Vector2i size{};
		bool failed{};
	};

	struct UwpCachedOutput
	{
		D3D12MemoryManager::Resource texture;
		D3D12DescriptorManager::Allocation srv;
		D3D12_RESOURCE_DESC desc{};
		D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		RendererOutputShader* shader{};
		bool useLinearTexFilter{};
		bool drcColorTransfer{};
		Vector2i sourceResolution{};
		Vector2i nativeResolution{};
		DXGI_FORMAT viewFormat{ DXGI_FORMAT_UNKNOWN };
		bool valid{};
	};

	struct UwpStreamReadback
	{
		std::unique_ptr<LatteTextureReadbackInfo> transfer;
		uint32 width{};
		uint32 height{};
		uint32 rowBytes{};
		DXGI_FORMAT format{ DXGI_FORMAT_UNKNOWN };
		bool padView{};
	};

	struct FeedbackScratchCopy
	{
		D3D12MemoryManager::Resource texture;
		// pool key; GetDesc() resolves default alignment and would not match
		D3D12_RESOURCE_DESC requestedDesc{};
		D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		LatteTextureViewD3D12* sampledView{};
		uint32 textureUnit{};
		uint32 logicOpRenderTarget{};
		uint64 drawId{};
		bool retainAcrossDraws{}; // color feedback copy that later draws of the same render pass may sample
	};

	// a color feedback copy kept after its draw; like the Vulkan backend's barrier skip, later draws of the
	// same render pass sample it until the game requests a color-buffer sync
	struct RetainedFeedbackCopy
	{
		FeedbackScratchCopy scratch;
		CachedFBOD3D12* fbo{};
		uint64 fboKey{};
	};

	struct ScratchTexturePoolEntry
	{
		D3D12MemoryManager::Resource texture;
		D3D12_RESOURCE_DESC desc{};
		uint64 sizeBytes{};
		uint64 readyFence{};
	};

	struct TextureTransition
	{
		uint32 subresource{};
		D3D12_RESOURCE_STATES state{};
	};

	struct FeedbackOverlapCacheEntry
	{
		LatteTextureViewD3D12* sampledView{};
		CachedFBOD3D12* fbo{};
		uint64 fboKey{};
		LatteTextureViewD3D12* attachment{};
		bool hasFeedback{};
		bool valid{};
	};

	struct TextureCopyCacheKey
	{
		uint64 sourceIdentity{};
		uint64 destinationIdentity{};
		sint32 sourceMip{};
		sint32 sourceX{};
		sint32 sourceY{};
		sint32 sourceSlice{};
		sint32 destinationMip{};
		sint32 destinationX{};
		sint32 destinationY{};
		sint32 destinationSlice{};
		sint32 width{};
		sint32 height{};
		sint32 depth{};
		uint32 mode{};

		bool operator==(const TextureCopyCacheKey&) const = default;
	};

	struct TextureCopyCacheKeyHash
	{
		size_t operator()(const TextureCopyCacheKey& key) const noexcept;
	};

	struct TextureSubresourceGeneration
	{
		uint32 subresource{};
		uint64 generation{};
	};

	struct TextureCopyCacheEntry
	{
		std::vector<TextureSubresourceGeneration> sourceGenerations;
		std::vector<TextureSubresourceGeneration> destinationGenerations;
	};

	enum class StencilFallbackPass : uint8
	{
		None,
		WindingFlipped,
		Front,
		FrontFlipped,
		Back,
		BackFlipped,
	};

	void CreateDevice();
	void CreateCommandObjects();
	void CreateRtvHeap();
	void CreateDescriptorManager();
	void CreateMemoryManager();
#if defined(CEMU_UWP_D3D12_DIAGNOSTICS) || defined(CEMU_UWP_D3D12_WORKLOAD_PROFILE)
	void CreateTimingResources();
	void ReleaseTimingResources();
#endif
#if defined(CEMU_UWP_D3D12_DIAGNOSTICS)
	void LogPerformanceSummary(std::string_view phase) const;
#endif
#if defined(CEMU_UWP_D3D12_WORKLOAD_PROFILE)
	void LogWorkloadGpuTiming();
#endif
#if defined(CEMU_UWP_D3D12_CORRECTNESS_TELEMETRY)
	void LogCorrectnessTelemetry(std::string_view phase) const;
#endif
	void CreateSparseMrtNullRtv();
	void CreateOutputRootSignature();
	void CreateOutputSamplers();
	void CreateGameRootSignature();
	void CreateSurfaceCopyRootSignature();
	void CreateImguiDescriptorHeap();
	void CreateOrResizeSwapchain(SwapchainState& chain, const Vector2i& size, WindowSystem::WindowHandleInfo::Backend backend, void* surface);
	void CreateBackBufferViews(SwapchainState& chain);
	ID3D12PipelineState* GetOutputPipeline(RendererOutputShader* shader, bool padView);
	void DrawUwpConfiguredUnderlay(SwapchainState& chain);
	void BeginUwpCompositeFrame(SwapchainState& chain);
	void ResetUwpOutputSnapshots();
	void ReleaseUwpCachedOutputs();
	bool StoreUwpOutputSnapshot(bool padView, LatteTextureViewD3D12* textureView, LatteTextureD3D12* texture, RendererOutputShader* shader, bool useLinearTexFilter);
	void DrawUwpCachedOutput(bool padView);
	void DrawUwpCachedVisibleOutputs();
	void ProcessUwpStreamCapture(LatteTextureView* textureView, bool padView);
	ID3D12PipelineState* GetGamePipeline(const D3D12PipelineCompiler::PipelineIdentity& identity,
										 LatteDecompilerShader* vertexShader,
										 LatteDecompilerShader* geometryShader,
										 LatteDecompilerShader* pixelShader,
										 Latte::LATTE_VGT_PRIMITIVE_TYPE::E_PRIMITIVE_TYPE primitiveMode,
										 bool noRasterizationOutput,
										 Renderer::INDEX_TYPE hostIndexType,
										 StencilFallbackPass stencilFallbackPass);
	bool ShouldUsePointExpansionGeometryShader(LatteDecompilerShader* geometryShader,
											   Latte::LATTE_VGT_PRIMITIVE_TYPE::E_PRIMITIVE_TYPE primitiveMode) const;
	bool UsesDynamicDepthBias(bool usePointExpansionGS) const;
	RendererShaderD3D12* GetPointExpansionGeometryShader(LatteDecompilerShader* vertexShader, LatteDecompilerShader* pixelShader);
	RendererShaderD3D12* GetRectEmulationGeometryShader(LatteDecompilerShader* vertexShader, LatteDecompilerShader* pixelShader);
	ID3D12PipelineState* GetSurfaceCopyPipeline(SurfaceCopyMode mode, DXGI_FORMAT rtvFormat, DXGI_FORMAT dsvFormat);
	void SurfaceCopyWithFormatConversion(LatteTexture* sourceTexture, sint32 srcMip, sint32 srcSlice, LatteTexture* destinationTexture, sint32 dstMip, sint32 dstSlice,
										 sint32 srcX, sint32 srcY, sint32 dstX, sint32 dstY, sint32 width, sint32 height, bool coordinatesAreEffective,
										 bool allowNativeCopy = true);
	void BindGameDescriptorTables(LatteDecompilerShader* vertexShader, LatteDecompilerShader* geometryShader, LatteDecompilerShader* pixelShader, uint32 baseVertex, uint32 baseInstance, uint32 vertexIdRemapMode, uint64 pipelineHash);
	D3D12_GPU_VIRTUAL_ADDRESS UploadGameUniformVars(LatteDecompilerShader* shader, uint64 drawId, uint64 pipelineHash);
	D3D12DescriptorManager::Allocation BuildGameResourceTable(LatteDecompilerShader* shader, D3D12_GPU_VIRTUAL_ADDRESS& uniformCbvAddress, uint64 drawId, uint64 pipelineHash);
	D3D12DescriptorManager::Allocation BuildGameUavTable(LatteDecompilerShader* shader);
	D3D12DescriptorManager::Allocation BuildGameSamplerTable(LatteDecompilerShader* shader);
	D3D12DescriptorManager::Allocation GetShaderVisibleOutputSampler(bool linear);
	uint32 GetGameResourceTableDescriptorCount(LatteDecompilerShader* shader) const;
	uint32 GetGameUavTableDescriptorCount(LatteDecompilerShader* shader) const;
	uint32 GetGameSamplerTableDescriptorCount(LatteDecompilerShader* shader) const;
	uint64 BuildGameResourceTableCacheKey(LatteDecompilerShader* shader, uint32 descriptorGeneration, bool& cacheable);
	uint64 BuildGameSamplerTableCacheKey(LatteDecompilerShader* shader, uint32 descriptorGeneration, bool& cacheable);
	struct TableKeyMemo
	{
		LatteDecompilerShader* shader{};
		uint32 generation{ kInvalidDescriptorIndex };
		uint64 epoch{};
		uint64 key{};
		bool cacheable{};
		bool lookupDone{};
		bool lookupHit{};
		D3D12DescriptorManager::Allocation cachedAllocation{};
	};
	uint64 GetMemoizedTableCacheKey(std::array<TableKeyMemo, (size_t)LatteConst::ShaderType::TotalCount>& memoArray,
									bool isSamplerTable, LatteDecompilerShader* shader, uint32 descriptorGeneration, bool& cacheable);
	TableKeyMemo* GetCurrentTableKeyMemo(std::array<TableKeyMemo, (size_t)LatteConst::ShaderType::TotalCount>& memoArray,
										 LatteDecompilerShader* shader, uint32 descriptorGeneration);
	bool HasCachedGameResourceTable(LatteDecompilerShader* shader, uint32 descriptorGeneration);
	bool HasCachedGameSamplerTable(LatteDecompilerShader* shader, uint32 descriptorGeneration);
	uint64 BuildGameUniformVarsSourceKey(LatteDecompilerShader* shader) const;
	uint64 BuildGameUavTableCacheKey(LatteDecompilerShader* shader, uint32 descriptorGeneration) const;
	void InvalidateGameRootParameterCache(bool descriptorTablesOnly = false);
	bool EnsureGamePipelineLibrary(uint64 titleId);
	void PersistGamePipelineLibrary();
	bool EnsureSinglePsoCacheSupport();
	uint64 BuildGamePipelineFingerprint(uint64 pipelineKey, const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc) const;
	bool LoadGamePipelineBlob(uint64 pipelineKey, uint64 fingerprint, std::vector<uint8>& blob);
	void StoreGamePipelineBlob(uint64 pipelineKey, uint64 fingerprint, ID3D12PipelineState* pipeline);
	void RemoveGamePipelineBlob(uint64 pipelineKey, uint64 fingerprint);
	void InvalidateGameShaderVisibleDescriptorCaches();
	void InvalidateGameCbvSrvUavDescriptorCaches();
	void InvalidateGameSamplerDescriptorCaches();
	void DrawHandleSpecialState5();
	uint32 AllocateImguiDescriptor();
	ImTextureID GenerateImguiTexture(const std::vector<uint8>& data, const Vector2i& size, DXGI_FORMAT format);
	bool UpdateImguiTexture(ImTextureID id, const std::vector<uint8>& data, const Vector2i& size, DXGI_FORMAT expectedFormat);
	D3D12_CPU_DESCRIPTOR_HANDLE GetImguiCpuHandle(uint32 index) const;
	D3D12_GPU_DESCRIPTOR_HANDLE GetImguiGpuHandle(uint32 index) const;
	void RecordClear(SwapchainState& chain, const float clearColor[4], bool submit = true);
	enum WorkloadSubmitReason : uint32
	{
		SubmitUnspecified = 0,
		SubmitResetOpen = 1u << 0,
		SubmitWaitIdle = 1u << 1,
		SubmitPresent = 1u << 2,
		SubmitFlush = 1u << 3,
		SubmitDrawBudget = 1u << 4,
		SubmitReadback = 1u << 5,
		SubmitScratchBudget = 1u << 6,
		SubmitQueryFlush = 1u << 7,
	};
	void ExecuteCurrentCommandList(uint32 workloadReasons = SubmitUnspecified);
	void ResetCommandList();
	void WaitForIdle();
	void WaitForFence(uint64 fenceValue);
	void WaitForCommandSlot(uint32 slotIndex);
	void ProcessFinishedCommandLists();
	void ProcessDestructionQueue();
	void QueueDeferredRelease(Microsoft::WRL::ComPtr<IUnknown> object);
	void QueueDeferredRelease(D3D12MemoryManager::Resource&& resource);
	void QueueDeferredRelease(D3D12DescriptorManager::ShaderVisibleHeapPage&& descriptorHeap);
	uint64 GetRetirementFence() const;
	bool WaitForFenceValue(uint64 fenceValue, std::string_view operation);
	uint32 GetDefaultSubmitThreshold() const;
	void EnsureShaderVisibleDescriptorSpace(uint32 cbvSrvUavCount, uint32 samplerCount);
	void ResetCommandStateCache();
	void BindShaderVisibleHeaps();
	void SetD3D12PipelineState(ID3D12PipelineState* pipeline);
	void SetD3D12GraphicsRootSignature(ID3D12RootSignature* rootSignature);
	void SetD3D12PrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY topology);
	void SetD3D12Viewport(const D3D12_VIEWPORT& viewport);
	void SetD3D12DepthBias(float depthBias, float depthBiasClamp, float slopeScaledDepthBias);
	void SetD3D12Scissor(const D3D12_RECT& scissor);
	void SetD3D12BlendFactor(const float (&blendFactor)[4]);
	void SetD3D12StencilRef(UINT frontRef, UINT backRef);
	void SetD3D12IndexBuffer(const D3D12_INDEX_BUFFER_VIEW& indexView);
	void SetD3D12RenderTargets(UINT rtvCount, const D3D12_CPU_DESCRIPTOR_HANDLE* rtvHandles, const D3D12_CPU_DESCRIPTOR_HANDLE* dsvHandle);
	void Transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES& state, D3D12_RESOURCE_STATES newState);
	void TransitionTextureAll(class LatteTextureD3D12* texture, D3D12_RESOURCE_STATES newState);
	void TransitionTextureSubresource(class LatteTextureD3D12* texture, uint32 subresource, D3D12_RESOURCE_STATES newState);
	void TransitionTextureView(class LatteTextureViewD3D12* view, D3D12_RESOURCE_STATES newState, bool includeStencilPlane = false);
	void TransitionTextureSubresources(class LatteTextureD3D12* texture, const std::vector<TextureTransition>& transitions);
	void ResolveFeedbackLoopsForDraw(LatteDecompilerShader* vertexShader,
		LatteDecompilerShader* geometryShader, LatteDecompilerShader* pixelShader, bool renderTargetsBound);
	enum class UwpCpuDrawStage : uint32
	{
		Setup,
		IndexDecode,
		BufferSync,
		PipelineLookup,
		DescriptorAndFeedbackSetup,
		DrawRecording,
		RetirementReadbackAndSubmit,
	};

	bool TryAcquireScratchTexture(const D3D12_RESOURCE_DESC& desc, D3D12MemoryManager::Resource& outResource);
	bool TryAcquireCommandScratchTexture(const D3D12_RESOURCE_DESC& desc, D3D12MemoryManager::Resource& outResource);
	void RecycleCommandScratchTexture(D3D12MemoryManager::Resource&& texture, const D3D12_RESOURCE_DESC& desc);
	void RetireScratchTexture(D3D12MemoryManager::Resource&& texture, const D3D12_RESOURCE_DESC& desc, uint64 readyFence);
	void ReleaseScratchTexturePool();
	void RecycleScratchCopiesForDraw(uint64 drawId);
	void ResetScratchCopyLookup();
	bool IsCommandScratchBudgetReached() const;
	bool EnsureIndexResident(D3D12IndexAllocation& allocation, uint32 dataSize);
	const FeedbackScratchCopy* GetFeedbackScratchCopy(uint32 textureUnit) const;
	FeedbackScratchCopy* GetLogicOpScratchCopy(uint32 renderTargetIndex);
	bool CanSkipTextureCopy(const TextureCopyCacheKey& key, const LatteTextureD3D12* source,
		const LatteTextureD3D12* destination, const std::vector<uint32>& sourceSubresources,
		const std::vector<uint32>& destinationSubresources) const;
	void RecordTextureCopy(const TextureCopyCacheKey& key, const LatteTextureD3D12* source,
		LatteTextureD3D12* destination, const std::vector<uint32>& sourceSubresources,
		const std::vector<uint32>& destinationSubresources);
	void MarkTextureViewWritten(LatteTextureViewD3D12* view, bool includeStencilPlane = false, bool includePrimaryPlane = true);
	uint64 GetTextureAllocationSize(const D3D12_RESOURCE_DESC& desc) const;
	struct UwpTextureEvictionResult
	{
		uint32 textures{};
		uint64 bytes{};
	};
	UwpTextureEvictionResult EvictUwpTextureCache(uint32 maxTextures, uint32 minAgeFrames, uint64 targetBytes);
	void ProcessUwpMemoryPressure();
	// periodic breakdown of where the app's memory goes
	void LogUwpMemoryBreakdown();
	std::chrono::steady_clock::time_point m_lastMemoryBreakdownLog{};
	bool ShouldDeferNativePipelineCreation(uint64 pipelineKey);
	bool RecoverFromTextureAllocationFailure(const D3D12_RESOURCE_DESC& desc, const char* context);
	void RecoverFromPipelineAllocationFailure();
	void LogUwpCpuPerformanceSummary();
#if defined(CEMU_UWP_PERFORMANCE_EXPERIMENT)
	void InitializePerformanceExperiment();
	void AdvancePerformanceExperiment();
#endif
	bool PerformanceExperimentEnabled(D3D12PerformanceExperiment::Feature feature) const
	{
#if defined(CEMU_UWP_PERFORMANCE_EXPERIMENT)
		return D3D12PerformanceExperiment::Enabled(m_performanceExperiment.FeatureMask(), feature);
#else
		return D3D12PerformanceExperiment::Enabled(D3D12PerformanceExperiment::kDefaultMask, feature);
#endif
	}
	void LogUwpCommandSubmitPerformanceSummary();
	bool CreateFeedbackScratchCopy(uint32 textureUnit, LatteTextureViewD3D12* sampledView, bool retainAcrossDraws = false);
	bool TryReuseRetainedFeedbackCopy(uint32 textureUnit, LatteTextureViewD3D12* sampledView);
	void DropRetainedFeedbackCopies(LatteTexture* modifiedTexture = nullptr); // nullptr drops all
	bool PrepareLogicOpEmulationForDraw();
	bool CreateLogicOpScratchCopy(LatteTextureViewD3D12* renderTargetView, uint32 renderTargetIndex);
	bool RefreshLogicOpScratchCopiesForDraw(uint64 drawId);
	const D3D12_FEATURE_DATA_FORMAT_SUPPORT& GetCachedFormatSupport(DXGI_FORMAT format) const;
	bool FormatSupportsOutputMergerLogicOp(DXGI_FORMAT format) const;
	bool FormatSupportsBlending(DXGI_FORMAT format) const;
	void BindFramebuffer(CachedFBOD3D12* fbo);
	void LogTextureStats(std::string_view label, LatteTextureViewD3D12* textureView, bool padView, uint32& counter, uint32 limit);
	void LogOutputTextureStats(LatteTextureViewD3D12* textureView, bool padView);
	void LogActiveRenderTargetStats();
	void LogVertexBufferPreview(uint64 drawId, uint64 pipelineHash, LatteDecompilerShader* vertexShader, LatteDecompilerShader* pixelShader, Latte::LATTE_VGT_PRIMITIVE_TYPE::E_PRIMITIVE_TYPE primitiveMode, uint32 count, uint32 baseVertex, uint32 baseInstance, uint32 indexMin, uint32 indexMax, Renderer::INDEX_TYPE hostIndexType, uint32 hostIndexCount);
	void TraceTextureSnapshot(std::string_view eventName, std::string_view phase, LatteTextureViewD3D12* textureView, bool padView, std::string_view extra = {});
	void TraceFboAttachments(std::string_view eventName, std::string_view phase, uint64 drawId, uint64 pipelineHash = 0, std::string_view extra = {});
	void TraceShaderTextureBindings(uint64 drawId, uint64 pipelineHash, LatteDecompilerShader* vertexShader, LatteDecompilerShader* geometryShader, LatteDecompilerShader* pixelShader);
	void TraceVertexFetchBindings(uint64 drawId, uint64 pipelineHash, LatteFetchShader* fetchShader);
	std::string BuildShaderTextureSourceSummary(LatteDecompilerShader* vertexShader, LatteDecompilerShader* geometryShader, LatteDecompilerShader* pixelShader) const;
	std::string BuildGeometryProbeResourceMetadata(LatteDecompilerShader* vertexShader, LatteDecompilerShader* geometryShader, LatteDecompilerShader* pixelShader) const;
	std::string BuildTextureLineageExtra(uint32 physAddress) const;
	void RecordTextureLineage(std::string_view operation, LatteTextureViewD3D12* textureView, uint64 drawId, uint64 pipelineHash, std::string_view sourceSummary, std::string_view extra = {});
	bool TryRawAliasTextureCopy(LatteTextureD3D12* srcTexture, sint32 srcMip, sint32 srcSlice, sint32 srcX, sint32 srcY,
								LatteTextureD3D12* dstTexture, sint32 dstMip, sint32 dstSlice, sint32 dstX, sint32 dstY,
								sint32 width, sint32 height, sint32 depth);
	class LatteTextureReadbackInfoD3D12* CreateHostTextureReadback(LatteTextureView* textureView);
	bool IsFenceComplete(uint64 fenceValue) const;
	void EnsureOcclusionQueryResources();
	uint32 CreateOcclusionQueryPage();
	OcclusionQuerySlot AcquireOcclusionQuerySlot();
	void ReleaseOcclusionQuerySlot(OcclusionQuerySlot slot);
	void RetireOcclusionQuerySlot(OcclusionQuerySlot slot, uint64 fenceValue);
	void ProcessRetiredOcclusionQuerySlots();
	void ResolveOcclusionQuery(OcclusionQuerySlot slot);
	void OcclusionQueryNotifyEndCommandList();
	void OcclusionQueryNotifyBeginCommandList();
	CommandSlot& GetCurrentCommandSlot();
	const CommandSlot& GetCurrentCommandSlot() const;
	SwapchainState& GetSwapchain(bool mainWindow);
	const SwapchainState& GetSwapchain(bool mainWindow) const;
	void CheckDeviceRemoved(HRESULT hr, std::string_view operation) const;
	void LogInfoQueueMessages(std::string_view operation, uint32 maxMessages = 32) const;
	void LogDeviceRemovedData(HRESULT hr, std::string_view operation) const;

	Microsoft::WRL::ComPtr<IDXGIFactory6> m_factory;
	Microsoft::WRL::ComPtr<IDXGIAdapter1> m_adapter;
	LUID m_adapterLuid{};
	Microsoft::WRL::ComPtr<ID3D12Device> m_device;
	Microsoft::WRL::ComPtr<ID3D12CommandQueue> m_directQueue;
	std::array<CommandSlot, kCommandBufferPoolSize> m_commandSlots;
	uint32 m_commandSlotIndex{};
	uint32 m_commandSlotSyncIndex{};
	bool m_commandListOpen{};
	Microsoft::WRL::ComPtr<ID3D12Fence> m_fence;
	HANDLE m_fenceEvent{};
	D3D12GpuWatchdog m_gpuWatchdog;
	std::atomic<bool> m_deviceRemovalReported{};
	uint64 m_nextFenceValue{1};
	uint64 m_lastCompletedFenceValue{};
	uint64 m_lastQueryFenceValue{};
	uint64 m_numSubmittedCommandLists{};
	uint64 m_numCompletedCommandLists{};
	uint64 m_feedbackLoopDetections{};
	uint64 m_feedbackLoopCopies{};
	static constexpr uint32 kInvalidScratchCopyIndex = 0xFFFFFFFFu;
	uint64 m_scratchTexturePoolHits{};
	uint64 m_scratchTexturePoolMisses{};
	uint64 m_commandScratchTextureHits{};
	uint64 m_scratchTexturePoolRetired{};
	uint64 m_scratchTexturePoolEvicted{};
	uint64 m_scratchTexturePoolBytes{};
	uint64 m_commandScratchTextureHighWater{};
	uint64 m_feedbackOverlapCacheHits{};
	uint64 m_feedbackOverlapCacheMisses{};
	uint64 m_transitionBarrierCalls{};
	uint64 m_transitionBarrierCount{};
	uint64 m_textureCopyCacheHits{};
	uint64 m_textureCopyCacheMisses{};
	uint64 m_textureCopyBytesSkipped{};
	uint64 m_surfaceConversionCacheHits{};
	uint64 m_surfaceConversionCacheMisses{};
	uint64 m_textureAllocationRecoveryAttempts{};
	uint64 m_textureAllocationRecoverySuccesses{};
	uint64 m_memoryBudgetGeneration{};
	uint64 m_memoryPressureTrimPasses{};
	uint64 m_memoryPressureTexturesEvicted{};
	uint64 m_memoryPressureTextureBytes{};
	uint64 m_memoryPressureScratchBytes{};
	uint64 m_memoryPressureHeavyPasses{};
	uint64 m_memoryPressureDeferredPipelines{};
	uint64 m_memoryPressureForcedPipelines{};
	uint64 m_memoryPressureLastHeavyUsageBytes{};
	uint32 m_memoryPressureLastHeavyFrame{};
	uint32 m_memoryPressureLastDeferRecoveryFrame{};
	std::unordered_map<uint64, uint32> m_gamePipelineDeferCount;
	uint64 m_fenceWaitCount{};
	uint64 m_fenceWaitTimeUs{};
	uint64 m_fenceWaitMaxUs{};
	uint64 m_uwpPresentCount{};
	uint64 m_uwpPresentTimeUs{};
	uint64 m_uwpCpuProfileIntervalStartedMs{};
#if defined(CEMU_UWP_PERFORMANCE_EXPERIMENT)
	enum class ExperimentDetailStage : uint32 { Prepare, Feedback, Descriptors, RenderState, IndexResidency, Count };
	static constexpr size_t kExperimentDetailStageCount = (size_t)ExperimentDetailStage::Count;
	struct PerformanceExperimentWindow
	{
		uint64 frames{}, elapsedNs{}, maxFrameNs{}, over100ms{}, draws{}, minDraws{ UINT64_MAX }, maxDraws{};
		uint64 feedbackCopies{}, psoCreates{}, ineligibleFrames{}, fenceWaitUs{}, resourceCacheHits{}, resourceCacheMisses{};
		uint64 stageCacheEligible{}, satisfiedTransitionOpportunities{}, satisfiedTransitionHits{};
		uint64 transitionAllocationOpportunities{}, transitionAllocationsAvoided{}, retainedGrowthAllocations{};
		uint64 detailClockReads{};
		uint64 logicInitialCopies{}, logicRefreshCopies{}, logicRefreshPayloadBytes{}, logicFullRefreshPayloadBytes{}, logicEmptyRefreshSkips{};
		uint64 uniformOrEligible{}, uniformOrBatched{}, nativeDraws{};
		std::array<uint64, kExperimentDetailStageCount> detailTimeNs{}, detailSamples{};
		std::array<uint64, kUwpCpuDrawStageCount> broadTimeNs{}, broadSamples{};
	};
	D3D12PerformanceExperiment::Schedule m_performanceExperiment;
	PerformanceExperimentWindow m_performanceExperimentWindow;
	std::chrono::steady_clock::time_point m_performanceExperimentLastPresent{};
	uint64 m_performanceExperimentLastPresentCount{};
	uint64 m_performanceExperimentRuntimePsoCreates{};
	uint64 m_performanceExperimentWaitElapsedNs{}, m_performanceExperimentWaitFrames{}, m_performanceExperimentWaitPsoCreates{};
	uint64 m_performanceExperimentWaitMinDraws{ UINT64_MAX }, m_performanceExperimentWaitMaxDraws{};
	uint64 m_performanceExperimentLastDraws{}, m_performanceExperimentLastCopies{}, m_performanceExperimentLastPsoCreates{};
	uint64 m_performanceExperimentLastLogicCopies{};
	uint64 m_performanceExperimentLastFenceWaitUs{}, m_performanceExperimentLastResourceHits{}, m_performanceExperimentLastResourceMisses{};
#endif
	uint64 m_uwpCpuProfileDraws{};
	uint64 m_uwpCpuProfileSampledDraws{};
	std::array<uint64, kUwpCpuDrawStageCount> m_uwpCpuProfileStageTimeNs{};
	std::array<uint64, kUwpCpuDrawStageCount> m_uwpCpuProfileStageSamples{};
	uint64 m_uwpCpuProfileLastSubmittedCommandLists{};
	uint64 m_uwpCpuProfileLastFenceWaitCount{};
	uint64 m_uwpCpuProfileLastFenceWaitTimeUs{};
	uint64 m_uwpCpuProfileLastPresentCount{};
	uint64 m_uwpCpuProfileLastPresentTimeUs{};
	uint64 m_uwpCpuProfileLastFeedbackLoopCopies{};
	uint64 m_uwpCpuProfileLastCommandScratchTextureHits{};
	uint64 m_uwpCpuProfileLastScratchTexturePoolMisses{};
	D3D12DescriptorManager::Stats m_uwpDescriptorProfileLastStats{};
	uint64 m_uwpDescriptorResourceCacheClearCount{};
	uint64 m_uwpDescriptorResourceCacheClearTimeNs{};
	uint64 m_uwpDescriptorResourceCacheClearMaxNs{};
	uint64 m_uwpDescriptorSamplerCacheClearCount{};
	uint64 m_uwpDescriptorSamplerCacheClearTimeNs{};
	uint64 m_uwpDescriptorSamplerCacheClearMaxNs{};
	uint32 m_uwpCpuProfileRandomState{ 0x9e3779b9u };
	uint32 m_uwpSequenceProfileRandomState{ 0x6a09e667u };
	uint64 m_uwpSequenceProfileCalls{};
	uint64 m_uwpSequenceProfileSamples{};
	uint64 m_uwpSequenceProfileTimeNs{};
	struct UwpBufferProfile
	{
		uint64 indexReserves{};
		uint64 indexRequestedBytes{};
		uint64 indexRingUploads{};
		uint64 indexRingSameFenceHits{};
		uint64 indexRingUploadBytes{};
		uint64 bufferUploads{};
		uint64 bufferUploadBytes{};
	} m_uwpBufferProfile;
	struct UwpCommandSubmitProfile
	{
		static constexpr size_t kPhaseCount = 5;
		uint64 intervalStartedMs{};
		uint64 submitCount{};
		std::array<uint64, kPhaseCount> phaseTimeNs{};
		std::array<uint64, kPhaseCount> phaseMaxNs{};
		uint64 totalTimeNs{};
		uint64 totalMaxNs{};
	} m_uwpCommandSubmitProfile;
#if defined(CEMU_UWP_D3D12_WORKLOAD_PROFILE)
	struct UwpWorkloadProfile
	{
		uint64 guestDraws{};
		uint64 nativeDraws{};
		uint64 orderedDraws{};
		uint64 stencilDraws{};
		uint64 logicalPrimitives{};
		uint64 initialLogicCopies{};
		uint64 initialLogicPayloadBytes{};
		uint64 refreshLogicCopies{};
		uint64 refreshLogicPayloadBytes{};
		std::array<uint64, 16> modeDraws{};
		std::array<uint64, 16> modeNativeDraws{};
		std::array<uint64, 16> modeRefreshCopies{};
		std::array<uint64, 8> submitReasons{};
		uint64 unspecifiedSubmits{};
		uint64 maxNativeDraws{};
		uint64 maxDrawPipeline{};
		uint64 maxDrawVs{};
		uint64 maxDrawPs{};
		uint32 maxDrawMode{};
		uint32 maxDrawIndices{};
		uint32 maxDrawInstances{};
	} m_uwpWorkloadProfile;
#endif

#if defined(CEMU_UWP_D3D12_DIAGNOSTICS) || defined(CEMU_UWP_D3D12_WORKLOAD_PROFILE)
	uint64 m_gpuCommandListSamples{};
	uint64 m_gpuCommandListTicks{};
	uint64 m_gpuCommandListMaxTicks{};
#endif
#if defined(CEMU_UWP_D3D12_WORKLOAD_PROFILE)
	uint64 m_workloadGpuLastSamples{};
	uint64 m_workloadGpuLastTicks{};
	uint64 m_workloadGpuWindowMaxTicks{};
	uint64 m_workloadGpuInvalidPairs{};
	uint64 m_workloadGpuLastInvalidPairs{};
#endif
	uint32 m_recordedDrawcalls{};
	uint32 m_submitThreshold{ kSubmitThresholdDefault };
	bool m_submitOnIdle{};
	uint64 m_shaderVisibleHeapBinds{};
	uint64 m_shaderVisibleHeapBindSkips{};
	uint64 m_commandStateBinds{};
	uint64 m_commandStateBindSkips{};
	uint64 m_uwpSnapshotSrvUpdates{};
	uint64 m_uwpSnapshotSrvSkips{};
	uint32 m_boundShaderVisibleHeapGeneration{ kInvalidDescriptorIndex };
	ID3D12PipelineState* m_boundPipelineState{};
	ID3D12RootSignature* m_boundGraphicsRootSignature{};
	D3D_PRIMITIVE_TOPOLOGY m_boundPrimitiveTopology{ D3D_PRIMITIVE_TOPOLOGY_UNDEFINED };
	D3D12_VIEWPORT m_boundViewport{};
	D3D12_RECT m_boundScissor{};
	bool m_boundViewportValid{};
	bool m_boundScissorValid{};
	std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 8> m_boundRtvHandles{};
	UINT m_boundRtvCount{};
	D3D12_CPU_DESCRIPTOR_HANDLE m_boundDsvHandle{};
	bool m_boundRenderTargetsValid{};
	std::array<float, 4> m_boundBlendFactor{};
	bool m_boundBlendFactorValid{};
	std::array<UINT, 2> m_boundStencilRef{};
	bool m_boundStencilRefValid{};
	D3D12_INDEX_BUFFER_VIEW m_boundIndexBuffer{};
	bool m_boundIndexBufferValid{};
	std::deque<DeferredReleaseBatch> m_deferredReleaseQueue;
	std::mutex m_deferredReleaseMutex;
	Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
	uint32 m_rtvDescriptorSize{};
	uint32 m_nextRtvIndex{};
	std::unique_ptr<D3D12DescriptorManager> m_descriptorManager;
	std::unique_ptr<D3D12MemoryManager> m_memoryManager;
	std::unique_ptr<D3D12GeometryProbe> m_geometryProbe;
#if defined(CEMU_UWP_D3D12_DIAGNOSTICS) || defined(CEMU_UWP_D3D12_WORKLOAD_PROFILE)
	Microsoft::WRL::ComPtr<ID3D12QueryHeap> m_timestampQueryHeap;
	D3D12MemoryManager::Resource m_timestampReadback;
	const uint64* m_timestampReadbackMapped{};
	uint64 m_timestampFrequency{};
#endif
	D3D12MemoryManager::Resource m_bufferCache;
	D3D12MemoryManager::Resource m_bufferCacheCopyTemp;
	D3D12MemoryManager::Resource m_textureAliasCopyBuffer;
	std::vector<uint8> m_bufferCacheShadow;
	D3D12MemoryManager::Resource m_streamoutBuffer;
	Microsoft::WRL::ComPtr<ID3D12RootSignature> m_outputRootSignature;
	// output-sized target of a two-pass output shader (FSR), one per view
	struct OutputPrePassTarget
	{
		D3D12MemoryManager::Resource texture;
		D3D12DescriptorManager::Allocation rtv;
		D3D12DescriptorManager::Allocation srv;
		sint32 width{};
		sint32 height{};
	};
	std::array<OutputPrePassTarget, 2> m_outputPrePassTargets;
	// draws prePass from sourceSrv into the view's target and returns the target's SRV, invalid on failure
	D3D12DescriptorManager::Allocation DrawOutputPrePass(RendererOutputShader* prePass, const D3D12DescriptorManager::Allocation& sourceSrv,
		sint32 width, sint32 height, bool padView, D3D12_GPU_VIRTUAL_ADDRESS uniformAddress);
	void ReleaseOutputPrePassTargets();
	D3D12DescriptorManager::Allocation m_outputLinearSampler;
	D3D12DescriptorManager::Allocation m_outputPointSampler;
	D3D12DescriptorManager::Allocation m_shaderVisibleOutputLinearSampler;
	D3D12DescriptorManager::Allocation m_shaderVisibleOutputPointSampler;
	uint32 m_shaderVisibleOutputSamplerGeneration{ kInvalidDescriptorIndex };
	D3D12DescriptorManager::Allocation m_nullCbvDescriptor;
	D3D12DescriptorManager::Allocation m_nullSrvDescriptor;
	D3D12DescriptorManager::Allocation m_nullSrvDescriptorRange;
	D3D12DescriptorManager::Allocation m_nullUavDescriptor;
	D3D12DescriptorManager::Allocation m_gameNullResourceTable;
	uint32 m_gameNullResourceTableGeneration{ kInvalidDescriptorIndex };
	D3D12DescriptorManager::Allocation m_gameNullUavTable;
	uint32 m_gameNullUavTableGeneration{ kInvalidDescriptorIndex };
	struct CachedDescriptorTable
	{
		D3D12DescriptorManager::Allocation allocation;
		std::vector<const LatteTexture*> referencedTextures;
	};
	std::unordered_map<uint64, CachedDescriptorTable> m_gameResourceTableCache;
	uint32 m_gameResourceTableCacheGeneration{ kInvalidDescriptorIndex };
	uint64 m_gameResourceTableCacheHits{};
	uint64 m_gameResourceTableCacheMisses{};
	std::unordered_map<uint64, CachedDescriptorTable> m_gameSamplerTableCache;
	uint32 m_gameSamplerTableCacheGeneration{ kInvalidDescriptorIndex };
	uint64 m_gameSamplerTableCacheHits{};
	uint64 m_gameSamplerTableCacheMisses{};
	D3D12DescriptorManager::Allocation m_sparseMrtNullRtv;
	std::unordered_map<uint64, Microsoft::WRL::ComPtr<ID3D12PipelineState>> m_outputPipelineCache;
	Microsoft::WRL::ComPtr<ID3D12RootSignature> m_gameRootSignature;
	std::unordered_map<uint64, Microsoft::WRL::ComPtr<ID3D12PipelineState>> m_gamePipelineCache;
	struct PendingGamePipelineRecipe
	{
		Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline;
		std::array<uint8, 32> fingerprint{};
	};
	// render thread only
	std::unordered_map<uint64, PendingGamePipelineRecipe> m_pendingGamePipelineRecipes;
	struct PrewarmedGamePipeline
	{
		uint64 generation{};
		uint64 pipelineKey{};
		PendingGamePipelineRecipe recipe;
	};
	// filled by prewarm workers, emptied by the render thread
	std::mutex m_prewarmedGamePipelinesMutex;
	std::vector<PrewarmedGamePipeline> m_prewarmedGamePipelines;
	std::atomic_bool m_prewarmedGamePipelinesAvailable{};
	// bumped whenever pending pipelines are dropped, so late worker results are discarded too
	std::atomic_uint64_t m_gamePipelineRecipeGeneration{};
	std::atomic<HRESULT> m_gamePipelineRecipePrewarmFailure{ S_OK };
	uint64 m_gamePipelineRecipeLoadingTitleId{};
	std::atomic_uint64_t m_gamePipelineRecipePrewarmAttempts{};
	std::atomic_uint64_t m_gamePipelineRecipePrewarmAccepted{};
	std::atomic_uint64_t m_gamePipelineRecipePrewarmRejected{};
	uint64 m_gamePipelineRecipePrewarmStartUsage{};
	std::atomic_uint64_t m_gamePipelineRecipePrewarmPeakUsage{};
	std::chrono::steady_clock::time_point m_gamePipelineRecipePrewarmStartTime{};
	std::atomic<const char*> m_gamePipelineRecipePrewarmStopReason{ "complete" };
	uint64 m_gamePipelineRecipePromotions{};
	uint64 m_gamePipelineRecipeMismatches{};
	std::atomic_bool m_gamePipelineRecipeLoading{};
	std::atomic_bool m_gamePipelineRecipePrewarmStopped{};
	// after launch the game's own allocations grow memory, so only each pipeline's own growth is charged
	std::atomic_bool m_gamePipelineRecipeBackground{};
	void StopGamePipelineRecipePrewarm(const char* reason);
	void RecordGamePipelineRecipePrewarmUsage(uint64 usageBytes);
	std::unordered_map<uint64, uint32> m_gamePipelineRetryFrame;
	Microsoft::WRL::ComPtr<ID3D12PipelineLibrary> m_gamePipelineLibrary;
	// CreatePipelineLibrary may retain the source blob until the library is released
	std::vector<uint8> m_gamePipelineLibraryBlob;
	uint64 m_gamePipelineLibraryTitleId{};
	uint64 m_gamePipelineLibraryHits{};
	uint64 m_gamePipelineLibraryMisses{};
	uint64 m_gamePipelineLibraryStores{};
	bool m_gamePipelineLibraryDirty{};
	bool m_singlePsoCacheSupportQueried{};
	bool m_singlePsoCacheSupported{};
	bool m_singlePsoCacheStoreEnabled{ true };
	uint64 m_singlePsoCacheHits{};
	uint64 m_singlePsoCacheMisses{};
	uint64 m_singlePsoCacheStores{};
	uint64 m_singlePsoCacheRejects{};
#if defined(CEMU_UWP_D3D12_DIAGNOSTICS)
	uint64 m_gamePsoCreateCount{};
	uint64 m_gamePsoCreateTimeUs{};
	uint64 m_gamePsoCreateMaxUs{};
#endif
	std::unordered_map<uint64, std::unique_ptr<RendererShaderD3D12>> m_pointExpansionGeometryShaders;
	std::unordered_map<uint64, std::unique_ptr<RendererShaderD3D12>> m_rectEmulationGeometryShaders;
	uint64 m_gameDrawCallsSubmitted{};
	uint64 m_gamePsoCreateFailures{};
	uint64 m_cullAllDrawsSkipped{};
	enum class DroppedDrawReason : uint32
	{
		SequenceRejected,
		LogicOpUnsupported,
		MissingShaderCode,
		MissingPipeline,
		InvalidIndexBuffer,
		IndexUploadFailed,
		Count,
	};
	std::array<uint64, (size_t)DroppedDrawReason::Count> m_droppedDraws{};
	std::array<uint64, (size_t)DroppedDrawReason::Count> m_droppedDrawsLogged{};
	uint64 m_droppedDrawsLogTimeMs{};
	uint32 m_droppedDrawsLogLines{};
	bool m_gamePipelineShaderCodeMissing{};
	std::unordered_set<uint64> m_loggedDrawFailureSignatures;
	bool ShouldLogDrawFailure(uint64 signature);
	void LogDroppedDrawSummary();
	uint64 m_noRasterStreamoutDraws{};
	uint64 m_pointExpansionDraws{};
	uint64 m_logicOpEmulatedDraws{};
	uint64 m_logicOpEmulationCopies{};
#if defined(CEMU_UWP_D3D12_CORRECTNESS_TELEMETRY)
	uint64 m_correctnessPresentCount{};
	uint64 m_correctnessDrawAttempts{};
	uint64 m_correctnessDrawSkippedSequence{};
	uint64 m_correctnessDrawSkippedNoPso{};
	uint64 m_correctnessDrawSkippedInvalidIndex{};
	uint64 m_correctnessDrawSkippedLogicOp{};
	uint64 m_correctnessSequenceShaderError{};
	uint64 m_correctnessSequenceInvalidFbo{};
	uint64 m_correctnessSequenceMissingAttachment{};
	uint64 m_correctnessSequenceRasterDisabled{};
	uint64 m_correctnessSpecialStateDepthClear{};
	uint64 m_correctnessSpecialStateDepthCopy{};
	uint64 m_correctnessStencilFallbackDraws{};
	uint64 m_correctnessOrderedLogicOpDraws{};
	uint64 m_correctnessInvalidFetchDraws{};
	uint64 m_correctnessUnboundFetchDraws{};
	uint64 m_correctnessUnsupportedBlendStates{};
	uint64 m_correctnessIncompatibleTextureViews{};
	uint64 m_correctnessSurfaceCopyRequests{};
	uint64 m_correctnessSurfaceCopyNative{};
	uint64 m_correctnessSurfaceCopyRawAlias{};
	uint64 m_correctnessSurfaceCopyShader{};
	uint64 m_correctnessSurfaceCopyInvalidView{};
	uint64 m_correctnessSurfaceCopyInvalidOutput{};
	uint64 m_correctnessSurfaceCopyNoPso{};
	uint64 m_correctnessPrimitiveMask{};
	uint64 m_correctnessFetchFormatMask{};
	uint64 m_correctnessFetchFormatOverflow{};
	uint64 m_correctnessColorlessPixelDraws{};
	uint64 m_correctnessDepthCompareDraws{};
	uint64 m_correctnessDepthCompareUnitUses{};
	uint64 m_correctnessDepthCompareDimMask{};
	uint64 m_correctnessUnsupportedDepthCompareDraws{};
#endif
	Microsoft::WRL::ComPtr<ID3D12RootSignature> m_surfaceCopyRootSignature;
	std::unordered_map<uint64, Microsoft::WRL::ComPtr<ID3D12PipelineState>> m_surfaceCopyPipelineCache;
	Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_imguiSrvHeap;
	uint32 m_imguiDescriptorSize{};
	uint32 m_imguiDescriptorCapacity{ 512 };
	uint32 m_imguiNextDescriptorIndex{ 1 };
	std::vector<uint32> m_imguiFreeDescriptorIndices;
	std::deque<RetiredImguiDescriptor> m_retiredImguiDescriptors;
	bool m_imguiInitialized{};
	SwapchainState* m_imguiActiveSwapchain{};
	std::unordered_map<uint64, ImGuiTextureResource> m_imguiTextures;

	std::array<LatteTextureViewD3D12*, kMaxTextureBindings> m_boundTextures{};
	std::vector<FeedbackScratchCopy> m_feedbackScratchCopies;
	std::vector<RetainedFeedbackCopy> m_retainedFeedbackCopies;
	std::vector<D3D12_RESOURCE_BARRIER> m_transitionBarrierScratch;
	std::vector<uint8> m_uniformVarsScratch;
	std::vector<const LatteTexture*> m_tableBuildTextureRefs;
	std::array<TableKeyMemo, (size_t)LatteConst::ShaderType::TotalCount> m_resourceTableKeyMemo{};
	std::array<TableKeyMemo, (size_t)LatteConst::ShaderType::TotalCount> m_samplerTableKeyMemo{};
	uint64 m_tableKeyMemoEpoch{};
	static constexpr uint32 kGameDrawConstantCount = 25;
	struct GameUniformUploadCache
	{
		LatteDecompilerShader* shader{};
		uint64 sourceKey{};
		D3D12_GPU_VIRTUAL_ADDRESS gpuAddress{};
	};
	std::array<GameUniformUploadCache, (size_t)LatteConst::ShaderType::TotalCount> m_gameUniformUploadCache{};
	D3D12DescriptorManager::Allocation m_gameStreamoutUavTable;
	uint64 m_gameStreamoutUavTableKey{};
	uint32 m_gameStreamoutUavTableGeneration{ kInvalidDescriptorIndex };
	static constexpr uint32 kGameRootParameterCount = 13;
	std::array<D3D12_GPU_DESCRIPTOR_HANDLE, kGameRootParameterCount> m_gameRootDescriptorTables{};
	std::array<bool, kGameRootParameterCount> m_gameRootDescriptorTableValid{};
	std::array<D3D12_GPU_VIRTUAL_ADDRESS, kGameRootParameterCount> m_gameRootCbvs{};
	std::array<bool, kGameRootParameterCount> m_gameRootCbvValid{};
	std::array<uint32, kGameDrawConstantCount> m_gameRootDrawConstants{};
	bool m_gameRootDrawConstantsValid{};
	std::vector<ScratchTexturePoolEntry> m_scratchTexturePool;
	std::vector<ScratchTexturePoolEntry> m_commandScratchTextures;
	std::array<uint32, kMaxTextureBindings> m_feedbackScratchByTextureUnit{};
	std::array<uint32, 8> m_logicOpScratchByTarget{};
	std::array<FeedbackOverlapCacheEntry, kMaxTextureBindings> m_feedbackOverlapCache{};
	std::vector<TextureTransition> m_textureTransitionScratch;
	// separate scratch; saved states must survive the copy transitions
	std::vector<TextureTransition> m_feedbackPreviousStateScratch;
	std::vector<TextureTransition> m_feedbackCopyStateScratch;
	std::unordered_map<TextureCopyCacheKey, TextureCopyCacheEntry, TextureCopyCacheKeyHash> m_textureCopyCache;
	std::array<BoundRange, LATTE_MAX_VERTEX_BUFFERS> m_vertexBindings{};
	std::array<std::array<BoundRange, LATTE_NUM_MAX_UNIFORM_BUFFERS>, (size_t)LatteConst::ShaderType::TotalCount> m_uniformBindings{};
	std::array<StreamoutBinding, LATTE_NUM_STREAMOUT_BUFFER> m_streamoutBindings{};
	sint32 m_streamoutVerticesPerInstance{};
	struct TextureLineage
	{
		uint64 serial{};
		uint64 drawId{};
		uint64 pipelineHash{};
		uint64 fboKey{};
		std::string operation;
		std::string sourceSummary;
		std::string extra;
	};
	std::unordered_map<uint32, TextureLineage> m_textureLineage;
	mutable std::unordered_map<uint32, D3D12_FEATURE_DATA_FORMAT_SUPPORT> m_formatSupportCache;
	uint64 m_textureLineageSerial{};
	std::unordered_map<uint64, std::string> m_traceTextureStatsCache;

	std::vector<OcclusionQueryPage> m_occlusionQueryPages;
	std::deque<RetiredOcclusionQuerySlot> m_retiredOcclusionQuerySlots;
	std::vector<LatteQueryObjectD3D12*> m_activeOcclusionQueries;

	SwapchainState m_mainSwapchain;
	SwapchainState m_padSwapchain;
	bool m_uwpCompositeFrameStarted = false;
	bool m_uwpCompositeFrameRendered = false;
	UwpUnderlayTexture m_uwpUnderlayTexture;
	std::array<UwpCachedOutput, 2> m_uwpCachedOutputs;
	std::array<bool, 2> m_uwpOutputDrawnThisFrame{};
	std::array<bool, 2> m_uwpOutputPendingThisFrame{};
	std::array<UwpStreamReadback, 2> m_uwpStreamReadbacks;
	uint64 m_uwpCachedOutputTitleId = 0;
	bool m_uwpDrawingCachedOutput = false;
	D3D12_VIEWPORT m_currentViewport{ 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f };
	D3D12_RECT m_currentScissor{ 0, 0, 1, 1 };
	CachedFBOD3D12* m_activeFBO{};
	bool m_drawSequenceActive{};
	bool m_drawSequenceSkip{};
	bool m_drawNeedsFullBufferSync{};
	uint32 m_logicOpEmulationMode{};
	bool m_disableNativeLogicOpForDraw{};

	bool m_initialized{};
	bool m_shutdownComplete{};
	D3D_FEATURE_LEVEL m_featureLevel{D3D_FEATURE_LEVEL_12_0};
	D3D_SHADER_MODEL m_shaderModel{D3D_SHADER_MODEL_NONE};
	D3D12_RESOURCE_BINDING_TIER m_resourceBindingTier{D3D12_RESOURCE_BINDING_TIER_1};
	D3D12_TILED_RESOURCES_TIER m_tiledResourcesTier{D3D12_TILED_RESOURCES_TIER_NOT_SUPPORTED};
	D3D_ROOT_SIGNATURE_VERSION m_rootSignatureVersion{D3D_ROOT_SIGNATURE_VERSION_1_0};
	bool m_debugLayerEnabled{};
	bool m_infoQueueEnabled{};
	bool m_dredAvailable{};
	bool m_isUma{};
	bool m_isCacheCoherentUma{};
	bool m_outputMergerLogicOpSupported{};
	bool m_psSpecifiedStencilRefSupported{};
	bool m_alphaBlendFactorSupported{};
	bool m_anisoFilterWithPointMipSupported{};
	bool m_invertedViewportHeightFlipsYSupported{};
	bool m_dynamicDepthBiasSupported{};
	std::array<float, 3> m_boundDepthBias{};
	bool m_boundDepthBiasValid{};
#if defined(__ID3D12GraphicsCommandList8_INTERFACE_DEFINED__)
	Microsoft::WRL::ComPtr<ID3D12Device2> m_independentStencilDevice;
	bool m_independentFrontBackStencilSupported{};
#endif
};
