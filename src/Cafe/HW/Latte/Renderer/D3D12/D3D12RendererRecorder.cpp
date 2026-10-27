// D3D12Renderer: render target state, two-phase destroys and the draw recording threads.
#define D3D12_RENDERER_SEPARATE_PART
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12RendererInternal.h"

void D3D12Renderer::renderTarget_setViewport(float x, float y, float width, float height, float nearZ, float farZ, bool halfZ)
{
	D3D12_QUEUE_RECORDING(renderTarget_setViewport(x, y, width, height, nearZ, farZ, halfZ));
	// halfZ is applied in HLSL SET_POSITION; D3D12 clips to 0..w before the viewport
	(void)halfZ;
	m_currentViewport.TopLeftX = x;
	m_currentViewport.TopLeftY = y;
	m_currentViewport.Width = width;
	m_currentViewport.Height = height;
	m_currentViewport.MinDepth = nearZ;
	m_currentViewport.MaxDepth = farZ;
	if (height < 0.0f && !m_invertedViewportHeightFlipsYSupported)
		cemuLog_logOnce(LogType::Force, "D3D12: guest viewport has a negative height, which this device does not report support for.");
	if (nearZ < 0.0f || nearZ > 1.0f || farZ < 0.0f || farZ > 1.0f)
		cemuLog_logOnce(LogType::Force, "D3D12: guest viewport depth range {}..{} is outside 0..1.", nearZ, farZ);
	if (m_commandListOpen)
		SetD3D12Viewport(m_currentViewport);
}

void D3D12Renderer::renderTarget_setScissor(sint32 scissorX, sint32 scissorY, sint32 scissorWidth, sint32 scissorHeight)
{
	D3D12_QUEUE_RECORDING(renderTarget_setScissor(scissorX, scissorY, scissorWidth, scissorHeight));
	m_currentScissor.left = scissorX;
	m_currentScissor.top = scissorY;
	m_currentScissor.right = scissorX + scissorWidth;
	m_currentScissor.bottom = scissorY + scissorHeight;
	if (m_commandListOpen)
		SetD3D12Scissor(m_currentScissor);
}
LatteCachedFBO* D3D12Renderer::rendertarget_createCachedFBO(uint64 key)
{
	D3D12_EXCLUSIVE_RECORDING();
	static uint32 s_createFboLogCount = 0;
	if (D3D12RuntimeLogCapped(s_createFboLogCount, "rendertarget_createCachedFBO", 64))
		cemuLog_log(LogType::Force, "D3D12: create cached FBO key=0x{:016x}.", key);
	D3D12TraceEvent("fbo_create", "key=0x{:016x}", key);
	return new CachedFBOD3D12(key);
}

void D3D12Renderer::rendertarget_deleteCachedFBO(LatteCachedFBO* cfbo)
{
	static uint32 s_deleteFboLogCount = 0;
	if (D3D12RuntimeLogCapped(s_deleteFboLogCount, "rendertarget_deleteCachedFBO", 64))
		cemuLog_log(LogType::Force, "D3D12: delete cached FBO ptr={} key=0x{:016x}.", (void*)cfbo, cfbo ? cfbo->key : 0);
	D3D12TraceEventFlush("fbo_delete", "ptr={} key=0x{:016x}", (void*)cfbo, cfbo ? cfbo->key : 0);
	// renderer references are dropped when the object is freed (ForgetCachedFBO), it stays valid until then
}

void D3D12Renderer::ForgetCachedFBO(LatteCachedFBO* cfbo)
{
	if (cfbo == m_activeFBO)
		m_activeFBO = nullptr;
	for (auto& cachedOverlap : m_feedbackOverlapCache)
	{
		if (cachedOverlap.fbo == cfbo)
			cachedOverlap = {};
	}
	DropRetainedFeedbackCopies();
}

void D3D12Renderer::rendertarget_destroyCachedFBO(LatteCachedFBO* cfbo)
{
	if (!t_recordContext && m_latteActiveFBO == cfbo)
		m_latteActiveFBO = nullptr;
	D3D12_QUEUE_RECORDING_KIND(Sync, rendertarget_destroyCachedFBO(cfbo));
	if (m_shutdownComplete)
	{
		delete cfbo;
		return;
	}
	m_pendingFboDestroys.push_back(cfbo);
}

void D3D12Renderer::texture_destroyView(LatteTextureView* view)
{
	if (!t_isRecorderThread)
		m_latteBoundTextures.fill(nullptr);
	D3D12_QUEUE_RECORDING_KIND(Sync, texture_destroyView(view));
	if (m_shutdownComplete)
	{
		delete view;
		return;
	}
	m_pendingViewDestroys.push_back(view);
}

void D3D12Renderer::texture_destroy(LatteTexture* texture)
{
	if (!t_isRecorderThread)
		m_latteBoundTextures.fill(nullptr);
	D3D12_QUEUE_RECORDING_KIND(Sync, texture_destroy(texture));
	if (m_shutdownComplete)
	{
		delete texture;
		return;
	}
	m_pendingTextureDestroys.push_back(texture);
}

// Phase 4 S2: frees what the core detached, in the order the core used to free it (an FBO inside its view, views
// before their texture). Runs at the start of a draw sequence, at present and when memory is reclaimed; with a
// recording thread it runs after the last queued draw that can reference these objects.
void D3D12Renderer::FlushPendingDestroys()
{
	while (!m_pendingFboDestroys.empty() || !m_pendingViewDestroys.empty() || !m_pendingTextureDestroys.empty())
	{
		for (LatteCachedFBO* cfbo : std::exchange(m_pendingFboDestroys, {}))
		{
			ForgetCachedFBO(cfbo);
			delete cfbo;
		}
		for (LatteTextureView* view : std::exchange(m_pendingViewDestroys, {}))
			delete view;
		for (LatteTexture* texture : std::exchange(m_pendingTextureDestroys, {}))
			delete texture;
	}
}

// Phase 4 S3: threaded draw recording ------------------------------------------------------------------------------

thread_local bool D3D12Renderer::t_recordContext = false;
thread_local bool D3D12Renderer::t_isRecorderThread = false;

namespace
{
// about a frame of draws and their state calls; the Latte side waits instead of queueing without bound
// op ring of the threaded recorder: a dense draw is ~1.5-3 KB with its register delta, so 16 MB holds a few thousand
// draws (about a frame); the first delta (whole register file, ~256 KB) also fits
constexpr size_t kRecordRingBytes = 16 * 1024 * 1024;
}

void D3D12Renderer::StartRecordingThread()
{
	if (m_recordThread.joinable())
		return;
	if (!m_recordRing)
		m_recordRing = std::make_unique<D3D12RecordRing>(kRecordRingBytes);
	if (m_twoStageWanted && !m_emitRing)
		m_emitRing = std::make_unique<D3D12RecordRing>(kRecordRingBytes);
	m_recordStop = false;
	m_recordError = nullptr;
	m_recordHasError.store(false, std::memory_order_relaxed);
	m_latteBoundTextures.fill(nullptr);
	// the ring has a single producer: the thread that starts recording (Latte); other threads record under exclusive access
	m_recordProducerThread = std::this_thread::get_id();
	m_recordThread = std::thread([this]() { RecordingThreadMain(); });
	if (m_emitRing)
		m_prepareThread = std::thread([this]() { PrepareThreadMain(); });
	m_recordThreadRunning.store(true, std::memory_order_release);
	cemuLog_log(LogType::Force, "D3D12: threaded draw recording started; draws are recorded on a second thread{}.",
		m_emitRing ? " behind a preparation thread (two-stage)" : "");
}

void D3D12Renderer::StopRecordingThread()
{
	if (!m_recordThread.joinable())
		return;
	WakePrepareStage();
	{
		std::unique_lock lock(m_recordQueueMutex);
		m_recordWaiters.fetch_add(1, std::memory_order_seq_cst);
		m_recordStateCv.wait(lock, [this]() { return RecordingIdle(); });
		m_recordWaiters.fetch_sub(1, std::memory_order_relaxed);
		m_recordStop = true;
		// ring empty: from here callers record directly instead of queueing work the exiting thread would drop
		m_recordThreadRunning.store(false, std::memory_order_release);
		m_recordWorkCv.notify_all();
		m_prepareWorkCv.notify_all();
	}
	if (m_prepareThread.joinable())
		m_prepareThread.join();
	m_recordThread.join();
	m_recordError = nullptr;
	m_recordHasError.store(false, std::memory_order_relaxed);
}

void D3D12Renderer::RecordingThreadMain()
{
	SetThreadName("d3d12Record");
	t_isRecorderThread = true;
#if BOOST_OS_WINDOWS
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
#endif
	t_recordContext = true;
	auto onError = [this](std::exception_ptr error)
	{
		// the rest of the published ops build on the failed work; the Latte side rethrows at its next call
		std::scoped_lock lock(m_recordQueueMutex);
		m_recordError = error;
		m_recordHasError.store(true, std::memory_order_release);
	};
	auto afterEach = [this]()
	{
		// a producer waits for space or for the ring to empty (drain, exclusive scope, stop)
		if (m_recordWaiters.load(std::memory_order_seq_cst) != 0)
		{
			std::scoped_lock lock(m_recordQueueMutex);
			m_recordStateCv.notify_all();
		}
	};
	while (true)
	{
		D3D12RecordRing& ring = EmitRing();
		if (!ring.HasWork())
		{
			std::unique_lock lock(m_recordQueueMutex);
			// producers wake the recorder only while it sleeps here; seq_cst pairs with the ring's head publish
			m_recordSleeping.store(true, std::memory_order_seq_cst);
			m_recordWorkCv.wait(lock, [this, &ring]() { return m_recordStop || ring.HasWork(); });
			m_recordSleeping.store(false, std::memory_order_relaxed);
			if (m_recordStop && !ring.HasWork())
				break;
		}
		const auto batchStarted = std::chrono::steady_clock::now();
		size_t ran;
		{
			std::scoped_lock exec(m_recordExecMutex);
			ran = ring.RunAvailable(onError, afterEach);
		}
		m_recordOps.fetch_add(ran, std::memory_order_relaxed);
		m_recordBusyNs.fetch_add((uint64)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - batchStarted).count(),
			std::memory_order_relaxed);
	}
}

void D3D12Renderer::PrepareThreadMain()
{
	SetThreadName("d3d12Prepare");
#if BOOST_OS_WINDOWS
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
#endif
	auto wakeRecorder = [this]()
	{
		if (m_recordSleeping.load(std::memory_order_seq_cst))
		{
			std::scoped_lock lock(m_recordQueueMutex);
			m_recordWorkCv.notify_one();
		}
	};
	auto afterEach = [this, &wakeRecorder]()
	{
		wakeRecorder();
		// Latte may wait for space in the first ring or for both rings to empty
		if (m_recordWaiters.load(std::memory_order_seq_cst) != 0)
		{
			std::scoped_lock lock(m_recordQueueMutex);
			m_recordStateCv.notify_all();
		}
	};
	auto waitForSpace = [this, &wakeRecorder](size_t entryBytes)
	{
		wakeRecorder();
		std::unique_lock lock(m_recordQueueMutex);
		m_recordWaiters.fetch_add(1, std::memory_order_seq_cst);
		m_recordStateCv.wait(lock, [this, entryBytes]() { return m_emitRing->CanPush(entryBytes); });
		m_recordWaiters.fetch_sub(1, std::memory_order_relaxed);
	};
	while (true)
	{
		if (!m_recordRing->HasWork())
		{
			std::unique_lock lock(m_recordQueueMutex);
			m_prepareSleeping.store(true, std::memory_order_seq_cst);
			m_prepareWorkCv.wait(lock, [this]() { return m_recordStop || m_recordRing->HasWork(); });
			m_prepareSleeping.store(false, std::memory_order_relaxed);
			if (m_recordStop && !m_recordRing->HasWork())
				break;
		}
		const auto batchStarted = std::chrono::steady_clock::now();
		const size_t moved = m_recordRing->TransferAvailable(*m_emitRing, waitForSpace, afterEach);
		m_prepareMoved.fetch_add(moved, std::memory_order_relaxed);
		m_prepareBusyNs.fetch_add((uint64)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - batchStarted).count(),
			std::memory_order_relaxed);
	}
}

void D3D12Renderer::WakePrepareStage()
{
	m_prepareUnwokenBytes = 0;
	if (m_emitRing && m_prepareSleeping.load(std::memory_order_seq_cst))
	{
		std::scoped_lock lock(m_recordQueueMutex);
		m_prepareWorkCv.notify_one();
	}
}

void D3D12Renderer::PrepareRecordingPush(size_t entryBytes)
{
	m_lastPushBytes = entryBytes;
	const bool sampleCall = (m_recordQueueCalls++ & 63) == 0; // only the Latte thread queues
	if (sampleCall)
		m_recordPushSampleStart = std::chrono::steady_clock::now();
	m_recordPushSampled = sampleCall;
	if (m_recordHasError.load(std::memory_order_acquire))
	{
		std::exception_ptr error;
		{
			std::scoped_lock lock(m_recordQueueMutex);
			error = std::exchange(m_recordError, nullptr);
			m_recordHasError.store(false, std::memory_order_relaxed);
		}
		if (error)
			std::rethrow_exception(error);
	}
	if (!m_recordRing->CanPush(entryBytes))
	{
		// the recorder is behind: this wait is time the Latte thread loses to it
		WakePrepareStage();
		const auto waitStarted = std::chrono::steady_clock::now();
		std::unique_lock lock(m_recordQueueMutex);
		m_recordWaiters.fetch_add(1, std::memory_order_seq_cst);
		m_recordStateCv.wait(lock, [this, entryBytes]() { return m_recordRing->CanPush(entryBytes) || m_recordHasError.load(std::memory_order_acquire); });
		m_recordWaiters.fetch_sub(1, std::memory_order_relaxed);
		m_recordFullWaits++;
		m_recordFullWaitNs += (uint64)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - waitStarted).count();
		std::exception_ptr error = std::exchange(m_recordError, nullptr);
		m_recordHasError.store(false, std::memory_order_relaxed);
		if (error)
			std::rethrow_exception(error);
	}
}

void D3D12Renderer::FinishRecordingPush()
{
	// a wake-up per op cost the Latte thread a call ~380k times a second in BOTW forests: wake it only when it sleeps
	if (m_emitRing)
	{
		constexpr size_t kPrepareWakeBytes = 64 * 1024;
		m_prepareUnwokenBytes += m_lastPushBytes;
		if (m_prepareUnwokenBytes >= kPrepareWakeBytes)
			WakePrepareStage();
	}
	else if (m_recordSleeping.load(std::memory_order_seq_cst))
	{
		std::scoped_lock lock(m_recordQueueMutex);
		m_recordWorkCv.notify_one();
	}
	if (m_recordPushSampled)
	{
		m_recordQueueSampledNs += (uint64)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - m_recordPushSampleStart).count();
		m_recordQueueSamples++;
	}
}

void D3D12Renderer::DrainRecording()
{
	if (t_recordContext || !m_recordThreadRunning.load(std::memory_order_acquire))
		return;
	WakePrepareStage();
	std::exception_ptr error;
	{
		std::unique_lock lock(m_recordQueueMutex);
		m_recordWaiters.fetch_add(1, std::memory_order_seq_cst);
		m_recordStateCv.wait(lock, [this]() { return RecordingIdle(); });
		m_recordWaiters.fetch_sub(1, std::memory_order_relaxed);
		error = std::exchange(m_recordError, nullptr);
		m_recordHasError.store(false, std::memory_order_relaxed);
	}
	if (error)
		std::rethrow_exception(error);
}

void D3D12Renderer::WaitForQueuedSwaps()
{
	const uint64 target = m_queuedSwaps;
	WakePrepareStage();
	if (m_recordedSwaps.load(std::memory_order_acquire) >= target)
		return;
	const auto waitStarted = std::chrono::steady_clock::now();
	std::exception_ptr error;
	{
		std::unique_lock lock(m_recordQueueMutex);
		m_recordWaiters.fetch_add(1, std::memory_order_seq_cst);
		// an empty ring also ends the wait: a failed batch drops the swap op without running it
		m_recordStateCv.wait(lock, [&]() { return m_recordedSwaps.load(std::memory_order_acquire) >= target || RecordingIdle(); });
		m_recordWaiters.fetch_sub(1, std::memory_order_relaxed);
		error = std::exchange(m_recordError, nullptr);
		m_recordHasError.store(false, std::memory_order_relaxed);
		auto& stat = m_recordExclusiveStats["previous frame (swap wait)"];
		stat.count++;
		stat.waitNs += (uint64)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - waitStarted).count();
	}
	if (error)
		std::rethrow_exception(error);
}

D3D12Renderer::RecordingExclusiveScope::RecordingExclusiveScope(D3D12Renderer* renderer, std::source_location caller)
{
	if (!renderer || t_recordContext || !renderer->m_recordThreadRunning.load(std::memory_order_acquire))
		return;
	const auto waitStarted = std::chrono::steady_clock::now();
	renderer->DrainRecording();
	renderer->m_recordExecMutex.lock();
	{
		const uint64 waitNs = (uint64)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - waitStarted).count();
		std::scoped_lock statLock(renderer->m_recordQueueMutex);
		auto& stat = renderer->m_recordExclusiveStats[caller.function_name()];
		stat.count++;
		stat.waitNs += waitNs;
	}
	t_recordContext = true;
	m_renderer = renderer;
	renderer->m_gpuWatchdog.NoteCall(caller.function_name());
}

D3D12Renderer::RecordingExclusiveScope::~RecordingExclusiveScope()
{
	if (!m_renderer)
		return;
	t_recordContext = false;
	m_renderer->m_recordExecMutex.unlock();
}


