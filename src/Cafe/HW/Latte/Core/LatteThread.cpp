#include "Cafe/HW/Latte/ISA/RegDefines.h"
#include "Cafe/OS/libs/gx2/GX2.h" // todo - remove dependency
#include "Cafe/HW/Latte/Core/Latte.h"
#include "Cafe/HW/Latte/Core/LatteDraw.h"
#include "Cafe/HW/Latte/Core/LatteShader.h"
#include "Cafe/HW/Latte/Core/LatteAsyncCommands.h"
#include "Cafe/GameProfile/GameProfile.h"
#include "Cafe/GraphicPack/GraphicPack2.h"
#include "WindowSystem.h"

#include "Cafe/HW/Latte/Core/LatteBufferCache.h"

#include "Cafe/HW/Latte/Renderer/Renderer.h"
#include "Cafe/HW/Latte/Core/LatteTexture.h"
#include "util/helpers/helpers.h"

#include <imgui.h>
#include "config/ActiveSettings.h"

#include "Cafe/CafeSystem.h"

#if defined(CEMU_UWP)
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12Renderer.h"
#include "uwp/UwpWindowSystem.h"

#include <fmt/format.h>
#include <new>
#include <winrt/base.h>
#endif

LatteGPUState_t LatteGPUState = {};

std::atomic_bool sLatteThreadRunning = false;
std::atomic_bool sLatteThreadFinishedInit = false;
#if defined(CEMU_UWP)
std::atomic_bool sLatteThreadFailureCleanupPending = false;
enum class UwpLatteStartupState : uint8
{
	Pending,
	Ready,
	Failed,
};
std::atomic<UwpLatteStartupState> sLatteThreadStartupState{ UwpLatteStartupState::Pending };
#endif

void LatteThread_Exit();

void Latte_LoadInitialRegisters()
{
	LatteGPUState.contextNew.CB_TARGET_MASK.set_MASK(0xFFFFFFFF);
	LatteGPUState.contextNew.VGT_MULTI_PRIM_IB_RESET_INDX.set_RESTART_INDEX(0xFFFFFFFF);
	LatteGPUState.contextNew.VGT_DMA_NUM_INSTANCES.set_NUM_INSTANCES(1);
	LatteGPUState.contextRegister[Latte::REGADDR::PA_CL_CLIP_CNTL] = 0;
	*(float*)&LatteGPUState.contextRegister[mmDB_DEPTH_CLEAR] = 1.0f;
}

extern bool gx2WriteGatherInited;

LatteTextureView* osScreenTVTex[2] = { nullptr };
LatteTextureView* osScreenDRCTex[2] = { nullptr };

LatteTextureView* LatteHandleOSScreen_getOrCreateScreenTex(MPTR physAddress, uint32 width, uint32 height, uint32 pitch)
{
	LatteTextureView* texView = LatteTextureViewLookupCache::lookup(physAddress, width, height, 1, pitch, 0, 1, 0, 1, Latte::E_GX2SURFFMT::R8_G8_B8_A8_UNORM, Latte::E_DIM::DIM_2D);
	if (texView)
		return texView;
	return LatteTexture_CreateTexture(Latte::E_DIM::DIM_2D, physAddress, 0, Latte::E_GX2SURFFMT::R8_G8_B8_A8_UNORM, width, height, 1, pitch, 1, 0, Latte::E_HWTILEMODE::TM_LINEAR_ALIGNED, false);
}

void LatteHandleOSScreen_prepareTextures()
{
	osScreenTVTex[0] = LatteHandleOSScreen_getOrCreateScreenTex(LatteGPUState.osScreen.screen[0].physPtr, 1280, 720, 1280);
	osScreenTVTex[1] = LatteHandleOSScreen_getOrCreateScreenTex(LatteGPUState.osScreen.screen[0].physPtr + 1280 * 720 * 4, 1280, 720, 1280);
	osScreenDRCTex[0] = LatteHandleOSScreen_getOrCreateScreenTex(LatteGPUState.osScreen.screen[1].physPtr, 854, 480, 0x380);
	osScreenDRCTex[1] = LatteHandleOSScreen_getOrCreateScreenTex(LatteGPUState.osScreen.screen[1].physPtr + 896 * 480 * 4, 854, 480, 0x380);
}

void LatteRenderTarget_copyToBackbuffer(LatteTextureView* textureView, bool isPadView);

bool LatteHandleOSScreen_TV()
{
	if (!LatteGPUState.osScreen.screen[0].isEnabled)
		return false;
	if (LatteGPUState.osScreen.screen[0].flipExecuteCount == LatteGPUState.osScreen.screen[0].flipRequestCount)
		return false;
	LatteHandleOSScreen_prepareTextures();

	sint32 bufferDisplayTV = (LatteGPUState.osScreen.screen[0].flipRequestCount & 1) ^ 1;
	sint32 bufferDisplayDRC = (LatteGPUState.osScreen.screen[1].flipRequestCount & 1) ^ 1;

	const uint32 bufferIndexTV = (bufferDisplayTV);
	const uint32 bufferIndexDRC = bufferDisplayDRC;

	LatteTexture_ReloadData(osScreenTVTex[bufferIndexTV]->baseTexture);

	// TV screen
	LatteRenderTarget_copyToBackbuffer(osScreenTVTex[bufferIndexTV]->baseTexture->baseView, false);
	
	if (LatteGPUState.osScreen.screen[0].flipExecuteCount != LatteGPUState.osScreen.screen[0].flipRequestCount)
		LatteGPUState.osScreen.screen[0].flipExecuteCount.store(LatteGPUState.osScreen.screen[0].flipRequestCount);
	return true;
}

bool LatteHandleOSScreen_DRC()
{
	if (!LatteGPUState.osScreen.screen[1].isEnabled)
		return false;
	if (LatteGPUState.osScreen.screen[1].flipExecuteCount == LatteGPUState.osScreen.screen[1].flipRequestCount)
		return false;
	LatteHandleOSScreen_prepareTextures();

	sint32 bufferDisplayDRC = (LatteGPUState.osScreen.screen[1].flipRequestCount & 1) ^ 1;

	const uint32 bufferIndexDRC = bufferDisplayDRC;

	LatteTexture_ReloadData(osScreenDRCTex[bufferIndexDRC]->baseTexture);

	// GamePad screen
	LatteRenderTarget_copyToBackbuffer(osScreenDRCTex[bufferIndexDRC]->baseTexture->baseView, true);

	if (LatteGPUState.osScreen.screen[1].flipExecuteCount != LatteGPUState.osScreen.screen[1].flipRequestCount)
		LatteGPUState.osScreen.screen[1].flipExecuteCount.store(LatteGPUState.osScreen.screen[1].flipRequestCount);
	return true;
}

void LatteThread_HandleOSScreen()
{
#if defined(CEMU_UWP)
	std::unique_lock<std::recursive_mutex> rendererLock(g_rendererMutex);
#endif
	bool swapTV = LatteHandleOSScreen_TV();
	bool swapDRC = LatteHandleOSScreen_DRC();
	if(swapTV || swapDRC)
		g_renderer->SwapBuffers(swapTV, swapDRC);
}

int Latte_ThreadEntry()
{
	SetThreadName("LatteThread");
	sint32 w,h;
	WindowSystem::GetWindowPhysSize(w,h);

	// renderer
	g_renderer->Initialize();
	RendererOutputShader::InitializeStatic();

	LatteTiming_Init();
	LatteTexture_init();
	LatteTC_Init();
	LatteBufferCache_init(164 * 1024 * 1024);
	LatteQuery_Init();
	LatteSHRC_Init();
	LatteStreamout_InitCache();

	g_renderer->renderTarget_setViewport(0, 0, w, h, 0.0f, 1.0f);
	
	// enable GLSL gl_PointSize support
	// glEnable(GL_PROGRAM_POINT_SIZE); // breaks shader caching on AMD (as of 2018)
	
	LatteGPUState.glVendor = GLVENDOR_UNKNOWN;
	switch(g_renderer->GetVendor())
	{
	case GfxVendor::AMD: 
		LatteGPUState.glVendor = GLVENDOR_AMD;
		break;
	case GfxVendor::Intel:
		LatteGPUState.glVendor = GLVENDOR_INTEL; 
		break;
	case GfxVendor::Nvidia: 
		LatteGPUState.glVendor = GLVENDOR_NVIDIA; 
		break;
	case GfxVendor::Apple:
		LatteGPUState.glVendor = GLVENDOR_APPLE;
	default:
		break;
	}

	sLatteThreadFinishedInit = true;
#if defined(CEMU_UWP)
	sLatteThreadStartupState.store(UwpLatteStartupState::Ready, std::memory_order_release);
#endif

	// register debug handler
	if (cemuLog_isLoggingEnabled(LogType::OpenGLLogging))
		g_renderer->EnableDebugMode();

	// wait till a game is started
	while( true )
	{
		if( CafeSystem::IsTitleRunning() )
			break;

		{
#if defined(CEMU_UWP)
			std::unique_lock<std::recursive_mutex> rendererLock(g_rendererMutex);
#endif
			g_renderer->DrawEmptyFrame(true);
			g_renderer->DrawEmptyFrame(false);
			g_renderer->CancelScreenshotRequest(); // keep the screenshot request queue empty
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(1000/60));
	}

	{
#if defined(CEMU_UWP)
		std::unique_lock<std::recursive_mutex> rendererLock(g_rendererMutex);
#endif
		g_renderer->DrawEmptyFrame(true);
	}

	// before doing anything with game specific shaders, we need to wait for graphic packs to finish loading
	GraphicPack2::WaitUntilReady();
	// if legacy packs are enabled we cannot use the colorbuffer resolution optimization
	LatteGPUState.allowFramebufferSizeOptimization = true;
	for(auto& pack : GraphicPack2::GetActiveGraphicPacks())
	{
		if(pack->AllowRendertargetSizeOptimization())
			continue;
		for(auto& rule : pack->GetTextureRules())
		{
			if(rule.filter_settings.width >= 0 || rule.filter_settings.height >= 0 || rule.filter_settings.depth >= 0 ||
				rule.overwrite_settings.width >= 0 || rule.overwrite_settings.height >= 0 || rule.overwrite_settings.depth >= 0)
			{
				LatteGPUState.allowFramebufferSizeOptimization = false;
				cemuLog_log(LogType::Force, "Graphic pack \"{}\" prevents rendertarget size optimization. This warning can be ignored and is intended for graphic pack developers", pack->GetName());
				break;
			}
		}
	}
	// load disk shader cache
    LatteShaderCache_Load();
	// init registers
	Latte_LoadInitialRegisters();
	// let CPU thread know the GPU is done initializing
	g_isGPUInitFinished.store(true, std::memory_order_release);
	// wait until CPU has called GX2Init()
	while (LatteGPUState.gx2InitCalled == 0)
	{
		std::this_thread::yield();
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
		LatteThread_HandleOSScreen();
		if (Latte_GetStopSignal())
			LatteThread_Exit();
	}
	LatteCP_ProcessRingbuffer();
	cemu_assert_debug(false); // should never reach
	return 0;
}

#if defined(CEMU_UWP)
namespace
{
void LatteThread_CancelPendingTitleGpuWork()
{
	LatteQuery_CancelAllForTitle();
	LatteTextureReadback_CancelAllForTitle();
}

void LatteThread_CleanupAfterFailure()
{
	std::unique_lock<std::recursive_mutex> rendererLock(g_rendererMutex);
	bool rendererPrepared = false;
	if (g_renderer)
	{
		try
		{
			static_cast<D3D12Renderer*>(g_renderer.get())->PrepareForTitleShutdown();
			rendererPrepared = true;
		}
		catch (const std::exception& ex)
		{
			cemuLog_log(LogType::Force, "UWP Latte cleanup continued after renderer preparation failed: {}", ex.what());
		}
		catch (...)
		{
			cemuLog_log(LogType::Force, "UWP Latte cleanup continued after an unknown renderer preparation failure.");
		}
	}
	LatteThread_CancelPendingTitleGpuWork();
	LatteBufferCache_UnloadAll();
	LatteTC_UnloadAllTextures();
	LatteSHRC_UnloadAll();
	LatteShaderCache_Close();
	RendererOutputShader::ShutdownStatic();
	if (g_renderer && rendererPrepared)
	{
		try
		{
			static_cast<D3D12Renderer*>(g_renderer.get())->FinishTitleShutdown();
		}
		catch (const std::exception& ex)
		{
			cemuLog_log(LogType::Force, "UWP Latte cleanup continued after renderer title cleanup failed: {}", ex.what());
		}
		catch (...)
		{
			cemuLog_log(LogType::Force, "UWP Latte cleanup continued after an unknown renderer title cleanup failure.");
		}
	}
	if (g_renderer)
	{
		Renderer* renderer = g_renderer.get();
		delete renderer;
		g_renderer.release();
	}
	std::memset(&LatteGPUState, 0, sizeof(LatteGPUState));
}

void ReportLatteThreadFailure(std::string message) noexcept
{
	sLatteThreadFailureCleanupPending.store(true, std::memory_order_release);
	sLatteThreadFinishedInit.store(true, std::memory_order_release);
	sLatteThreadStartupState.store(UwpLatteStartupState::Failed, std::memory_order_release);
	try
	{
		cemuLog_log(LogType::Force, "UWP Latte thread failure: {}", message);
	}
	catch (...)
	{
	}
	CemuUwp::WindowHost::RequestGameplayGraphicsRecovery(std::move(message));
}

void LatteThread_UwpEntry() noexcept
{
	try
	{
		(void)Latte_ThreadEntry();
		return;
	}
	catch (const winrt::hresult_error& ex)
	{
		try
		{
			ReportLatteThreadFailure(fmt::format(
				"Latte gameplay thread raised WinRT error 0x{:08X}: {}",
				(uint32)ex.code(), winrt::to_string(ex.message())));
			return;
		}
		catch (...)
		{
		}
	}
	catch (const std::bad_alloc& ex)
	{
		CemuUwp::WindowHost::RecordCaughtAllocationFailure();
		try
		{
			ReportLatteThreadFailure(fmt::format(
				"Latte gameplay thread raised a renderer exception: {}", ex.what()));
			return;
		}
		catch (...)
		{
		}
	}
	catch (const std::exception& ex)
	{
		try
		{
			ReportLatteThreadFailure(fmt::format(
				"Latte gameplay thread raised a renderer exception: {}", ex.what()));
			return;
		}
		catch (...)
		{
		}
	}
	catch (...)
	{
	}

	ReportLatteThreadFailure("Latte gameplay thread raised an unknown renderer exception");
}
}
#endif

std::thread sLatteThread;
std::mutex sLatteThreadStateMutex;

// initializes GPU thread which in turn also activates graphic packs
// does not return until the thread finished initialization
#if defined(CEMU_UWP)
bool Latte_Start()
#else
void Latte_Start()
#endif
{
	std::unique_lock _lock(sLatteThreadStateMutex);
	cemu_assert_debug(!sLatteThreadRunning);
	sLatteThreadRunning = true;
	sLatteThreadFinishedInit = false;
#if defined(CEMU_UWP)
	sLatteThreadFailureCleanupPending.store(false, std::memory_order_release);
	g_isGPUInitFinished.store(false, std::memory_order_release);
	sLatteThreadStartupState.store(UwpLatteStartupState::Pending, std::memory_order_release);
	sLatteThread = std::thread(LatteThread_UwpEntry);
#else
	sLatteThread = std::thread(Latte_ThreadEntry);
#endif
	// wait until initialized
#if defined(CEMU_UWP)
	while (sLatteThreadStartupState.load(std::memory_order_acquire) == UwpLatteStartupState::Pending)
#else
	while (!sLatteThreadFinishedInit)
#endif
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
#if defined(CEMU_UWP)
	return sLatteThreadStartupState.load(std::memory_order_acquire) == UwpLatteStartupState::Ready;
#endif
}

#if defined(CEMU_UWP)
bool Latte_WaitForGPUInit()
{
	while (!g_isGPUInitFinished.load(std::memory_order_acquire))
	{
		if (sLatteThreadStartupState.load(std::memory_order_acquire) == UwpLatteStartupState::Failed)
			return false;
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	return true;
}
#endif

void Latte_Stop()
{
	std::unique_lock _lock(sLatteThreadStateMutex);
	if (!sLatteThreadRunning)
		return;
	sLatteThreadRunning = false;
	_lock.unlock();
	sLatteThread.join();
#if defined(CEMU_UWP)
	if (sLatteThreadFailureCleanupPending.exchange(false, std::memory_order_acq_rel))
		LatteThread_CleanupAfterFailure();
#endif
}

bool Latte_GetStopSignal()
{
	return !sLatteThreadRunning;
}

void LatteThread_Exit()
{
#if defined(CEMU_UWP)
	auto* uwpRenderer = static_cast<D3D12Renderer*>(g_renderer.get());
	bool rendererReusable = uwpRenderer != nullptr;
	if (uwpRenderer)
	{
		try
		{
			uwpRenderer->PrepareForTitleShutdown();
		}
		catch (const std::exception& ex)
		{
			rendererReusable = false;
			cemuLog_log(LogType::Force, "UWP title shutdown will rebuild the renderer after preparation failed: {}", ex.what());
		}
		catch (...)
		{
			rendererReusable = false;
			cemuLog_log(LogType::Force, "UWP title shutdown will rebuild the renderer after an unknown preparation failure.");
		}
	}
	LatteThread_CancelPendingTitleGpuWork();
#else
	if (g_renderer)
		g_renderer->Shutdown();
#endif
    // clean up vertex/uniform cache
    LatteBufferCache_UnloadAll();
	// clean up texture cache
	LatteTC_UnloadAllTextures();
	// clean up runtime shader cache
    LatteSHRC_UnloadAll();
    // close disk cache
	LatteShaderCache_Close();
	RendererOutputShader::ShutdownStatic();
#if defined(CEMU_UWP)
	if (uwpRenderer && rendererReusable)
	{
		try
		{
			uwpRenderer->FinishTitleShutdown();
		}
		catch (const std::exception& ex)
		{
			rendererReusable = false;
			cemuLog_log(LogType::Force, "UWP title shutdown will rebuild the renderer after title cleanup failed: {}", ex.what());
		}
		catch (...)
		{
			rendererReusable = false;
			cemuLog_log(LogType::Force, "UWP title shutdown will rebuild the renderer after an unknown title cleanup failure.");
		}
	}
	if (uwpRenderer && !rendererReusable && g_renderer)
	{
		Renderer* renderer = g_renderer.get();
		delete renderer;
		g_renderer.release();
	}
#else
    // destroy renderer but make sure that g_renderer remains valid until the destructor has finished
	if (g_renderer)
	{
		Renderer* renderer = g_renderer.get();
		delete renderer;
		g_renderer.release();
	}
#endif
	// reset GPU7 state
	std::memset(&LatteGPUState, 0, sizeof(LatteGPUState));
	#if BOOST_OS_WINDOWS
	ExitThread(0);
	#else
	pthread_exit(nullptr);
	#endif
	cemu_assert_unimplemented();
}
