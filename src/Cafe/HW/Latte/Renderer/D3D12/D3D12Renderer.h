#pragma once

#include <chrono>

#include "Cafe/HW/Latte/Renderer/Renderer.h"
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12DescriptorManager.h"
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12GpuWatchdog.h"
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12MemoryManager.h"
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12PipelineCompiler.h"
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12RecordRing.h"
#include "gui/interface/WindowSystem.h"
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12PerformanceExperiment.h"

#if BOOST_OS_WINDOWS
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#endif

#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <source_location>
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
	bool IsCacheCoherentUma() const { return m_isCacheCoherentUma; }
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
	void rendertarget_destroyCachedFBO(LatteCachedFBO* cfbo) override;
	void texture_destroyView(LatteTextureView* view) override;
	void texture_destroy(LatteTexture* texture) override;
	void rendertarget_bindFramebufferObject(LatteCachedFBO* cfbo) override;

	void* texture_acquireTextureUploadBuffer(uint32 size) override;
	void texture_releaseTextureUploadBuffer(uint8* mem) override;
	TextureDecoder* texture_chooseDecodedFormat(Latte::E_GX2SURFFMT format, bool isDepth, Latte::E_DIM dim, uint32 width, uint32 height) override;
	void texture_clearSlice(LatteTexture* hostTexture, sint32 sliceIndex, sint32 mipIndex) override;
	void texture_finishSliceUploads() override;
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
	void buffer_bindVertexBuffers(std::span<BindBufferParam> bindings) override;
	void buffer_bindVertexBuffer(uint32 bufferIndex, uint32 offset, uint32 size);
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

	// Phase 4 S3: with "Threaded draw recording" the draw tail and the ordered renderer operations run on a recording
	// thread. Calls from the Latte side either queue (ShouldQueueRecording) or take exclusive access (RecordingExclusiveScope)
	void DrainRecording() override;
	bool ShouldQueueRecording() const { return !t_recordContext && m_recordThreadRunning.load(std::memory_order_relaxed); }
	// phase 5 S2c: what a queued op touches, for a later split of the recorder into a preparation and an emission thread.
	// State: only bindings the preparation reads; Command: only the command list and GPU resources; Both: needs a state
	// half and a command half; Sync: frees or retires objects both halves may still use (both must have passed it)
	enum class RecordOpKind : uint8 { State, Command, Both, Sync, Count };
	std::array<std::atomic<uint64>, (size_t)RecordOpKind::Count> m_recordOpKinds{};
	// queues op() for the recording thread (op runs in place in a ring: no allocation per op)
	template<typename F>
	void QueueRecording(F&& op, RecordOpKind kind = RecordOpKind::Both)
	{
		QueueRecordingWithData(0, [](uint8_t*) {}, [op = std::forward<F>(op)](const uint8_t*) mutable { op(); }, kind);
	}
	// queues op(data) where fill(data) writes dataBytes bytes now (on the calling thread); data lives until op returns
	template<typename Fill, typename F>
	void QueueRecordingWithData(size_t dataBytes, Fill&& fill, F&& op, RecordOpKind kind = RecordOpKind::Both)
	{
		m_recordOpKinds[(size_t)kind].fetch_add(1, std::memory_order_relaxed);
		if (std::this_thread::get_id() != m_recordProducerThread)
		{
			// not the ring's producer: run it now with exclusive access, ordered against queued work by the drain
			std::vector<uint8_t> data(dataBytes);
			if (dataBytes)
				fill(data.data());
			RecordingExclusiveScope exclusive(this);
			op(dataBytes ? data.data() : nullptr);
			return;
		}
		PrepareRecordingPush(D3D12RecordRing::EntryBytes<std::decay_t<F>>(dataBytes));
		m_recordRing->Push(dataBytes, std::forward<Fill>(fill), std::forward<F>(op));
		FinishRecordingPush();
	}
	// logs (capped) when the command list is opened or submitted outside the recorder or an exclusive scope
	void CheckRecordingContext(const char* operation);
	// waits for queued recording work, then lets the caller's scope use the recording state directly
	class RecordingExclusiveScope
	{
	public:
		explicit RecordingExclusiveScope(D3D12Renderer* renderer, std::source_location caller = std::source_location::current());
		~RecordingExclusiveScope();
		RecordingExclusiveScope(const RecordingExclusiveScope&) = delete;
		RecordingExclusiveScope& operator=(const RecordingExclusiveScope&) = delete;
	private:
		D3D12Renderer* m_renderer{};
	};
	// true on the recording thread and inside an exclusive scope: renderer calls run directly
	static thread_local bool t_recordContext;
	static thread_local bool t_isRecorderThread;

private:
	// recorder load for the 5 s log: time spent running ops, ops run, and Latte-side waits on a full queue (queue mutex)
	std::atomic<uint64> m_recordBusyNs{};
	std::atomic<uint64> m_recordOps{};
	uint64 m_recordFullWaits{};
	uint64 m_recordFullWaitNs{};
	// Latte-side cost of the split for the 5 s log (queue mutex): exclusive scopes that drained the queue, by caller,
	// and sampled QueueRecording calls (1 of 64)
	struct RecordExclusiveStat { uint64 count{}; uint64 waitNs{}; };
	std::unordered_map<const char*, RecordExclusiveStat> m_recordExclusiveStats;
	uint64 m_recordQueueCalls{};
	uint64 m_recordQueueSampledNs{};
	uint64 m_recordQueueSamples{};
	uint64 m_uwpCollectRecordNs{}; // sampled CollectRecordState time on the Latte side
	uint64 m_uwpCollectRecordSamples{};
	double m_uwpLastHeadPipelineUs{}; // pipeline stage measured on the Latte side only (before the tail is merged in)
	bool m_threadedRecordingWanted{}; // setting read at init; the recorder runs only while a title draws
	void StartRecordingThread();
	void StopRecordingThread();
	void RecordingThreadMain();
	std::thread m_recordThread;
	std::atomic_bool m_recordThreadRunning{};
	std::mutex m_recordQueueMutex;
	std::condition_variable m_recordWorkCv;
	std::condition_variable m_recordStateCv;
	std::unique_ptr<D3D12RecordRing> m_recordRing;
	// phase 5 S2d: with two-stage recording Latte fills m_recordRing, a preparation thread moves each op into
	// m_emitRing and the recording thread runs them from there (S2d-1: the preparation stage only forwards)
	bool m_twoStageWanted{};
	std::unique_ptr<D3D12RecordRing> m_emitRing;
	std::thread m_prepareThread;
	std::condition_variable m_prepareWorkCv;
	std::atomic<bool> m_prepareSleeping{};
	std::atomic<uint64> m_prepareBusyNs{};
	std::atomic<uint64> m_prepareMoved{};
	void PrepareThreadMain();
	// Latte wakes the preparation thread only every kPrepareWakeBytes of queued work or when it waits for the rings:
	// that thread empties its ring at once and sleeps, so a wake per op doubled Latte's cost per op (0.17 -> 0.36 us)
	size_t m_prepareUnwokenBytes = 0;
	size_t m_lastPushBytes = 0;
	void WakePrepareStage();
	D3D12RecordRing& EmitRing() { return m_emitRing ? *m_emitRing : *m_recordRing; }
	bool RecordingIdle() const { return m_recordRing->IsEmpty() && (!m_emitRing || m_emitRing->IsEmpty()); }
	std::thread::id m_recordProducerThread;
	std::atomic<bool> m_recordSleeping{}; // recorder waits on m_recordWorkCv
	std::atomic<uint32> m_recordWaiters{}; // threads waiting on m_recordStateCv for space or an empty ring
	std::atomic<bool> m_recordHasError{};
	// output and present are queued too; Latte keeps at most one frame ahead of the recorder
	uint64 m_queuedSwaps{}; // Latte side
	std::atomic<uint64> m_recordedSwaps{}; // set by the queued swap to its sequence number
	void WaitForQueuedSwaps();
	bool m_recordPushSampled{};
	std::chrono::steady_clock::time_point m_recordPushSampleStart;
	void PrepareRecordingPush(size_t entryBytes);
	void FinishRecordingPush();
	bool m_recordStop{};
	std::exception_ptr m_recordError;
	// held by whoever runs recording work: the recording thread or an exclusive scope
	std::mutex m_recordExecMutex;
	// Latte-side copies of state the draw head reads while the recording thread owns the real one
	CachedFBOD3D12* m_latteActiveFBO{};
	std::array<sint32, LATTE_NUM_STREAMOUT_BUFFER> m_latteStreamoutRingOffsets{};
	// uniform sources the Latte side last sent per stage (the recorder keeps the bytes; both reset at title shutdown)
	struct LatteUniformSource
	{
		LatteDecompilerShader* shader{};
		std::vector<uint8> sentBytes; // last bytes sent; empty for a stage without a uniform buffer
		bool sent{};
	};
	std::vector<uint8> m_latteUniformScratch; // bytes built for the current draw, compared with sentBytes
	std::array<LatteUniformSource, (size_t)LatteConst::ShaderType::TotalCount> m_latteUniformSources{};
	// per-draw uniform bytes for the recorder; send=false keeps the bytes it has
	struct UniformUpdate
	{
		LatteDecompilerShader* shader{};
		bool send{};
		std::vector<uint8> data;
	};

	friend class LatteQueryObjectD3D12;
	friend class LatteTextureReadbackInfoD3D12;

	static constexpr uint32 kBackBufferCount = 3;
	static constexpr uint32 kCommandBufferPoolSize = 16; // forest records ~15 lists per frame; 8 left the render thread waiting on the GPU
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
#if defined(CEMU_UWP_D3D12_DIAGNOSTICS) || defined(CEMU_UWP_D3D12_WORKLOAD_PROFILE) || defined(CEMU_UWP)
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

public:
	// public so the index allocation pool in the .cpp can name it
	struct D3D12IndexAllocation
	{
		std::array<uint8, 96> previewData{};
		uint32 previewDataSize{};
		struct alignas(32) DecodedIndexBlock
		{
			std::array<uint8, 32> bytes;
		};
		static_assert(alignof(DecodedIndexBlock) >= 32);
		// left uninitialized: the decoder writes it before anything reads it (allocations are pooled, see
		// indexData_reserveIndexMemory)
		std::unique_ptr<DecodedIndexBlock[]> decodedData;
		size_t decodedBlocks{};
		uint8* GetDecodedData() { return reinterpret_cast<uint8*>(decodedData.get()); }
		const uint8* GetDecodedData() const { return reinterpret_cast<const uint8*>(decodedData.get()); }
		size_t GetDecodedCapacity() const { return decodedBlocks * sizeof(DecodedIndexBlock); }
		ID3D12Resource* residentResource{};
		uint64 residentOffset{};
		uint64 residentFence{};
		uint32 residentSize{};
		uint32 size{};
	};
private:

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
		bool requireUnchangedSource{}; // depth copy: reused only while FeedbackSourceGeneration matches
		uint64 sourceGeneration{};
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

	struct DrawIndices;
	struct DrawTailPacket;
	struct DrawTailState;
	DrawIndices DecodeDrawIndices(MPTR indexDataMPTR, Latte::LATTE_VGT_DMA_INDEX_TYPE::E_INDEX_TYPE indexType, uint32 count, Latte::LATTE_VGT_PRIMITIVE_TYPE::E_PRIMITIVE_TYPE primitiveMode);
	void PrepareDrawHead(uint32 baseVertex, uint32 baseInstance, uint32 instanceCount, uint32 count, MPTR indexDataMPTR, Latte::LATTE_VGT_DMA_INDEX_TYPE::E_INDEX_TYPE indexType, const LatteDrawcallContext& drawcallContext);
	void RecordDrawTail(DrawTailPacket& packet, const uint8* recordDeltaData);
	void PrepareDrawTailTables(const DrawTailPacket& packet, DrawTailState& state);
	bool PrepareDrawTail(const DrawTailPacket& packet, DrawTailState& state);
	bool EmitDrawTail(const DrawTailPacket& packet, DrawTailState& state);
	void RetireDrawTail(const DrawTailPacket& packet, DrawTailState& state);
	void FinishDrawOnLatte(bool hasPixelShader, bool noRasterizationOutput);

	void CreateDevice();
	void CreateCommandObjects();
	void CreateRtvHeap();
	void CreateDescriptorManager();
	void CreateMemoryManager();
#if defined(CEMU_UWP_D3D12_DIAGNOSTICS) || defined(CEMU_UWP_D3D12_WORKLOAD_PROFILE) || defined(CEMU_UWP)
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
										 bool allowNativeCopy = true, LatteTextureViewD3D12* preparedSourceView = nullptr,
										 LatteTextureViewD3D12* preparedDestinationView = nullptr);
	// threaded recording: views a queued texture copy may draw with, created on the Latte thread (recording state)
	struct PreparedSurfaceCopy
	{
		LatteTexture* source{};
		sint32 sourceMip{};
		sint32 sourceSlice{};
		LatteTexture* destination{};
		sint32 destinationMip{};
		sint32 destinationSlice{};
		LatteTextureViewD3D12* sourceView{};
		LatteTextureViewD3D12* destinationView{};
	};
	PreparedSurfaceCopy m_preparedSurfaceCopy;
	void BindGameDescriptorTables(LatteDecompilerShader* vertexShader, LatteDecompilerShader* geometryShader, LatteDecompilerShader* pixelShader, uint32 baseVertex, uint32 baseInstance, uint32 vertexIdRemapMode, uint64 pipelineHash);
	// phase 5 S2b: the preparation half only writes descriptors; the emission half records into the command list
	struct GameDescriptorTables
	{
		D3D12DescriptorManager::Allocation resource[3]; // vs, ps, gs
		D3D12DescriptorManager::Allocation uav[3];
		D3D12DescriptorManager::Allocation sampler[3];
	};
	GameDescriptorTables BuildGameDescriptorTables(LatteDecompilerShader* vertexShader, LatteDecompilerShader* geometryShader, LatteDecompilerShader* pixelShader, uint64 pipelineHash);
	void EmitGameDescriptorTables(const GameDescriptorTables& tables, LatteDecompilerShader* vertexShader, LatteDecompilerShader* geometryShader, LatteDecompilerShader* pixelShader, uint32 baseVertex, uint32 baseInstance, uint32 vertexIdRemapMode, uint64 pipelineHash);
	bool UsesStreamoutUavTable(LatteDecompilerShader* shader) const;
	void PrepareGameUniformVars(LatteDecompilerShader* shader, uint64 drawId, uint64 pipelineHash, UniformUpdate& update);
	void ApplyGameUniformUpdate(UniformUpdate& update);
	D3D12_GPU_VIRTUAL_ADDRESS CommitGameUniformVars(LatteDecompilerShader* shader, uint64 drawId, uint64 pipelineHash);
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
	// the caller's line attributes idle waits in the GPU wait log
	void WaitForIdle(std::source_location caller = std::source_location::current());
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
	// phase 5 S2b: feedback detection (preparation) is split from its barriers and copies (emission)
	struct FeedbackPlan
	{
		enum : uint8 { Transition = 0, ProxyTransition, ColorCopy, DepthCopy };
		uint32 count = 0;
		bool anyCopy = false;
		uint8 action[kMaxTextureBindings];
		uint8 unit[kMaxTextureBindings];
		LatteTextureViewD3D12* view[kMaxTextureBindings];
	};
	void PlanFeedbackForDraw(LatteDecompilerShader* vertexShader, LatteDecompilerShader* geometryShader, LatteDecompilerShader* pixelShader, bool renderTargetsBound, FeedbackPlan& plan);
	void EmitFeedbackForDraw(const FeedbackPlan& plan);
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
	UwpTextureEvictionResult EvictUwpTextureCache(uint32 maxTextures, uint32 minAgeFrames, uint64 targetBytes, bool keepExpensiveReloads = false);
	void ProcessUwpMemoryPressure();
	// periodic breakdown of where the app's memory goes
	void LogUwpMemoryBreakdown();
	std::chrono::steady_clock::time_point m_lastMemoryBreakdownLog{};
	bool ShouldDeferNativePipelineCreation(uint64 pipelineKey);
	bool RecoverFromTextureAllocationFailure(const D3D12_RESOURCE_DESC& desc, const char* context);
	void RecoverFromPipelineAllocationFailure();
	void LogUwpCpuPerformanceSummary();
	bool PerformanceExperimentEnabled(D3D12PerformanceExperiment::Feature feature) const
	{
		return D3D12PerformanceExperiment::Enabled(D3D12PerformanceExperiment::kDefaultMask, feature);
	}
	void LogUwpCommandSubmitPerformanceSummary();
	// reuseWhileSourceUnchanged: keep the copy for later draws, valid while the copied subresources were not written
	bool CreateFeedbackScratchCopy(uint32 textureUnit, LatteTextureViewD3D12* sampledView, bool retainAcrossDraws = false, bool reuseWhileSourceUnchanged = false);
	uint64 FeedbackSourceGeneration(LatteTextureViewD3D12* view) const;
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
	uint64 m_feedbackCopyReuses{};
	uint64 m_uwpProfileLastFeedbackReuses{};
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
	std::chrono::steady_clock::time_point m_memoryPressureLastNotice{};
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
	uint64 m_uwpCpuProfileDraws{};
	// sampled draws: descriptor stage split into feedback copies / uniform upload / table building
	bool m_uwpProfileThisDraw{};
	uint64 m_uwpProfileLastGpuTicks{};
	// GPU waits per 5 s by kind (idle, command slot, fence, query) and, for idle waits, by calling line
	std::array<uint64, 4> m_uwpWaitUs{};
	std::array<uint32, 4> m_uwpWaitCount{};
	// fence of the last submit before each present, to tell at a command slot wait how many frames the GPU still has queued
	std::array<uint64, 4> m_uwpPresentFences{};
	uint32 m_uwpPresentFenceIndex{};
	uint64 m_uwpLastSubmittedFence{};
	std::array<uint32, 4> m_uwpSlotWaitsByQueuedPresents{};
	uint32 m_uwpIdleWaitLine{};
	std::unordered_map<uint32, std::pair<uint32, uint64>> m_uwpIdleWaitByLine;
	// draws skipped while their PSO builds async, per 5 s, by render target size (w << 16 | h)
	std::unordered_map<uint32, uint32> m_uwpAsyncSkipsByTarget;
	uint32 m_uwpAsyncSkips{};
	std::array<uint64, 5> m_uwpDescriptorSplitNs{}; // feedback, uniform prepare (Latte), tables, before tables, after tables
	uint64 m_uwpViewTransitionsFast{}; // TransitionTextureView calls already satisfied / walking subresources
	uint64 m_uwpViewTransitionsSlow{};
	uint64 m_uwpDescriptorSplitSamples{};
	uint64 m_uwpProfileLastResourceHits{}, m_uwpProfileLastResourceMisses{}, m_uwpProfileLastSamplerHits{}, m_uwpProfileLastSamplerMisses{};
	uint64 m_uwpCpuProfileSampledDraws{};
	std::array<uint64, kUwpCpuDrawStageCount> m_uwpCpuProfileStageTimeNs{};
	std::array<uint64, kUwpCpuDrawStageCount> m_uwpCpuProfileStageSamples{};
	// stages measured by the draw tail, which may run on the recording thread; summed with the above in the log
	std::array<uint64, kUwpCpuDrawStageCount> m_uwpCpuProfileTailTimeNs{};
	std::array<uint64, kUwpCpuDrawStageCount> m_uwpCpuProfileTailSamples{};
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

#if defined(CEMU_UWP_D3D12_DIAGNOSTICS) || defined(CEMU_UWP_D3D12_WORKLOAD_PROFILE) || defined(CEMU_UWP)
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
#if defined(CEMU_UWP_D3D12_DIAGNOSTICS) || defined(CEMU_UWP_D3D12_WORKLOAD_PROFILE) || defined(CEMU_UWP)
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
	struct GamePipelineEntry
	{
		Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline;
		uint32 lastUsedFrame{};
	};
	std::unordered_map<uint64, GamePipelineEntry> m_gamePipelineCache;
	// memory pressure: drop PSOs unused for minAgeFrames; they come back through async creation
	size_t EvictColdGamePipelines(uint32 minAgeFrames, size_t maxPipelines, bool highPressure);
	uint64 m_gamePipelinesEvicted{};
	uint64 m_uwpExpensiveTexturesKept{}; // soft memory trims that skipped a large array (terrain) instead of deleting it
	struct PendingGamePipelineRecipe
	{
		Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline;
		std::array<uint8, 32> fingerprint{};
		std::array<uint8, 32> contentFingerprint{}; // D3D12PipelineRecipe::Serialized::contentFingerprint
		uint32 adoptedFrame{};
		bool fromAsync{}; // built for a skipped draw, not by the launch prewarm
	};
	// async results no draw asked for again within this many frames are released
	void ReleaseStaleAsyncGamePipelines();
	// memory pressure: drop prewarmed PSOs no draw adopted for minAgeFrames (their recipes rebuild them on demand)
	size_t ReleaseUnusedPrewarmedPipelines(uint32 minAgeFrames, size_t maxPipelines, bool highPressure);
	uint64 m_asyncPipelinesReleasedUnused{};
	// key without depth bias values -> a ready pipeline that differs at most in depth bias; drawn with
	// while the exact variant is built in the background, so the object does not vanish for a few frames
	struct DepthBiasSibling
	{
		uint64 pipelineKey{};
		float slope{}, offset{}, clamp{};
	};
	std::unordered_map<uint64, DepthBiasSibling> m_depthBiasSiblingPipelines;
	uint64 m_depthBiasSiblingDraws{};
	uint64 m_depthBiasAliases{}; // keys served by a near-identical bias pipeline instead of building their own
	uint32 m_asyncSkipBlockReasons{}; // why the current draw may not be skipped (bits, see the draw path)
	uint32 m_slowSyncPipelineLogs{};
	// Xbox: buffer copies into slices > 0 of block-compressed arrays land wrong, so with d3d12_gpu_array_upload each
	// slice goes buffer -> single-slice staging texture -> array slice (texture copy) instead of CPU WriteToSubresource
	struct ArrayUploadStaging
	{
		D3D12MemoryManager::Resource texture;
		D3D12_RESOURCE_DESC desc{};
		D3D12_RESOURCE_STATES state{ D3D12_RESOURCE_STATE_COPY_DEST };
	};
	std::unordered_map<uint64, ArrayUploadStaging> m_arrayUploadStaging;
	ArrayUploadStaging* GetArrayUploadStaging(DXGI_FORMAT format, uint32 width, uint32 height);
	// non-zero while the render thread creates a PSO; async workers hold off so the driver serves it first
	std::atomic_uint32_t m_renderThreadPipelineCreates{};
	std::atomic_uint32_t m_slowAsyncPipelineLogs{};
	std::atomic_uint32_t m_asyncPipelineJobsOutstanding{};
public:
	// a draw is waiting for a pipeline (render-thread build or queued async job): background prewarm holds off
	bool HasUrgentPipelineWork()
	{
		if (m_renderThreadPipelineCreates.load(std::memory_order_acquire) != 0 ||
			m_asyncPipelineJobsOutstanding.load(std::memory_order_acquire) != 0)
			return true;
		std::scoped_lock lock(m_asyncPipelineMutex);
		return !m_asyncPipelineJobs.empty();
	}
private:
	// CPU-writable textures (BC arrays on Xbox): WriteToSubresource swizzles on the CPU at ~30 MB/s, so a texture's
	// slices are written by several threads; the reload waits for all of them before the texture can be drawn
	struct CpuSliceUploadJob
	{
		Microsoft::WRL::ComPtr<ID3D12Resource> resource;
		UINT subresource{};
		UINT rowPitch{};
		UINT depthPitch{};
		std::vector<uint8> data;
	};
	std::mutex m_cpuUploadMutex;
	std::condition_variable m_cpuUploadWork;
	std::condition_variable m_cpuUploadDone;
	std::deque<CpuSliceUploadJob> m_cpuUploadJobs;
	size_t m_cpuUploadPendingBytes{};
	std::atomic_uint32_t m_cpuUploadOutstanding{};
	bool m_cpuUploadStop{};
	std::vector<std::thread> m_cpuUploadThreads;
	std::atomic_uint32_t m_cpuUploadFailures{};
	void QueueCpuSliceUpload(CpuSliceUploadJob&& job);
	void CpuSliceUploadThreadFunc();
	void RunCpuSliceUploadJob(CpuSliceUploadJob& job);
	void StopCpuSliceUploadThreads();
	// render thread only
	std::unordered_map<uint64, PendingGamePipelineRecipe> m_pendingGamePipelineRecipes;
	// pipeline keys differ in registers that do not reach the PSO desc (BOTW: one stream-out pass had 16 keys for 4
	// distinct PSOs, each built on the render thread in ~1.2 s), so a live or prewarmed PSO with the same desc and
	// shaders serves the new key instead of a new build
	struct ContentFingerprintHash
	{
		size_t operator()(const std::array<uint8, 32>& value) const { size_t h; memcpy(&h, value.data(), sizeof(h)); return h; }
	};
	std::unordered_map<std::array<uint8, 32>, uint64, ContentFingerprintHash> m_gamePipelineKeyByContent;
	uint64 m_gamePipelineContentReuses{};
	ID3D12PipelineState* FindGamePipelineByContent(const std::array<uint8, 32>& contentFingerprint, uint64& canonicalKey);
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
	bool m_allowAsyncShaderSkip{}; // async_compile: skip this draw instead of waiting on shader compilation
	bool m_gamePipelineAsyncPending{};

	// async_compile: a missing game PSO is created on a worker from its recipe while its draws are skipped.
	// Results come back through m_prewarmedGamePipelines and go live via the recipe promotion path.
	struct AsyncPipelineJob
	{
		uint64 generation{};
		uint64 pipelineKey{};
		std::vector<uint8> recipe;
		Microsoft::WRL::ComPtr<ID3D12RootSignature> rootSignature;
	};
	bool QueueAsyncGamePipeline(uint64 pipelineKey, std::vector<uint8>&& recipe);
	// render thread: true while pipelineKey is still being created in the background
	bool IsAsyncGamePipelinePending(uint64 pipelineKey);
	void DrainAsyncGamePipelineFailures();
	void StopAsyncPipelineThreads();
	void AsyncPipelineThreadFunc();
	std::mutex m_asyncPipelineMutex;
	std::condition_variable m_asyncPipelineCondition;
	std::deque<AsyncPipelineJob> m_asyncPipelineJobs;
	std::vector<uint64> m_asyncPipelineFailedKeys; // worker -> render thread, under m_asyncPipelineMutex
	std::vector<std::thread> m_asyncPipelineThreads;
	bool m_asyncPipelineStop{};
	// render thread only: key -> generation it was queued in
	std::unordered_map<uint64, uint64> m_asyncPipelinesInFlight;
	// render thread only: background creation failed, so these are created synchronously to surface the error
	std::unordered_set<uint64> m_asyncPipelineFailed;
	std::atomic_uint64_t m_asyncPipelinesCreated{};
	uint64 m_asyncPipelinesQueued{};
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
	// compiled once per source; every mode/format pair used to compile both shaders again
	std::unordered_map<const char*, std::unique_ptr<class RendererShaderD3D12>> m_surfaceCopyShaders;
	class RendererShaderD3D12* GetSurfaceCopyShader(RendererShader::ShaderType type, const char* source);
	Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_imguiSrvHeap;
	uint32 m_imguiDescriptorSize{};
	uint32 m_imguiDescriptorCapacity{ 512 };
	uint32 m_imguiNextDescriptorIndex{ 1 };
	std::vector<uint32> m_imguiFreeDescriptorIndices;
	std::deque<RetiredImguiDescriptor> m_retiredImguiDescriptors;
	bool m_imguiInitialized{};
	SwapchainState* m_imguiActiveSwapchain{};
	void RecordImguiFrame(SwapchainState* chain, struct ImDrawData* drawData, void* imguiBackend, bool titleRunning);
	std::unordered_map<uint64, ImGuiTextureResource> m_imguiTextures;

	std::array<LatteTextureViewD3D12*, kMaxTextureBindings> m_boundTextures{};

	// Latte-side copy of the last bind queued per unit, so threaded recording skips binds that change nothing

	std::array<LatteTextureView*, kMaxTextureBindings> m_latteBoundTextures{};
	std::vector<FeedbackScratchCopy> m_feedbackScratchCopies;
	std::vector<RetainedFeedbackCopy> m_retainedFeedbackCopies;
	std::vector<D3D12_RESOURCE_BARRIER> m_transitionBarrierScratch;
	std::vector<const LatteTexture*> m_tableBuildTextureRefs;
	std::array<TableKeyMemo, (size_t)LatteConst::ShaderType::TotalCount> m_resourceTableKeyMemo{};
	// table keys kept across draws (recording state); see GetMemoizedTableCacheKey
	struct StageTableKey
	{
		bool valid{};
		LatteDecompilerShader* shader{};
		LatteFetchShader* fetchShader{};
		uint32 descriptorGeneration{};
		uint64 bindingGeneration{};
		uint64 registerGeneration{};
		ID3D12Resource* bufferResource{};
		uint64 key{};
		bool cacheable{};
	};
	std::array<StageTableKey, (size_t)LatteConst::ShaderType::TotalCount> m_resourceStageKeyCache{};
	std::array<StageTableKey, (size_t)LatteConst::ShaderType::TotalCount> m_samplerStageKeyCache{};
	// bumped by changes to bound textures (per stage), uniform buffers (per stage) and vertex buffers
	std::array<uint64, 3> m_textureBindingGeneration{ 1, 1, 1 };
	std::array<uint64, (size_t)LatteConst::ShaderType::TotalCount> m_uniformBindingGeneration{};
	uint64 m_vertexBindingGeneration{ 1 };
	void BumpTextureBindingGeneration(uint32 hostTextureUnit)
	{
		if (hostTextureUnit >= LATTE_CEMU_GS_TEX_UNIT_BASE)
			++m_textureBindingGeneration[(size_t)LatteConst::ShaderType::Geometry];
		else if (hostTextureUnit >= LATTE_CEMU_VS_TEX_UNIT_BASE)
			++m_textureBindingGeneration[(size_t)LatteConst::ShaderType::Vertex];
		else
			++m_textureBindingGeneration[(size_t)LatteConst::ShaderType::Pixel];
	}
	void BumpAllTableBindingGenerations()
	{
		for (auto& generation : m_textureBindingGeneration)
			++generation;
		for (auto& generation : m_uniformBindingGeneration)
			++generation;
		++m_vertexBindingGeneration;
	}
	std::array<uint64, 2> m_tableKeyReuses{}; // resource, sampler
	std::array<uint64, 2> m_tableKeyBuilds{};
	std::array<TableKeyMemo, (size_t)LatteConst::ShaderType::TotalCount> m_samplerTableKeyMemo{};
	uint64 m_tableKeyMemoEpoch{};
	static constexpr uint32 kGameDrawConstantCount = 25;
	struct GameUniformUploadCache
	{
		LatteDecompilerShader* shader{};
		uint64 sourceKey{};
		std::vector<uint8> data; // built before the Phase 4 split point; empty when the stage has no uniform buffer
		D3D12_GPU_VIRTUAL_ADDRESS gpuAddress{}; // ring copy of data, valid only within the current command list
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
	// Phase 4 S2: objects the core detached, freed once no recorded work can reference them (FlushPendingDestroys)
	std::vector<LatteCachedFBO*> m_pendingFboDestroys;
	std::vector<LatteTextureView*> m_pendingViewDestroys;
	std::vector<LatteTexture*> m_pendingTextureDestroys;
	void FlushPendingDestroys();
	void ForgetCachedFBO(LatteCachedFBO* cfbo);
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
