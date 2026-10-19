#include "UwpVideoPlayer.h"

#include "Cemu/Logging/CemuLogging.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iterator>
#include <mutex>
#include <optional>
#include <set>
#include <string>

#include <d3d10_1.h>
#include <d3d11_4.h>
#include <dxgi1_2.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <wrl/client.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/Windows.Media.Core.h>
#include <winrt/Windows.Media.Playback.h>
#include <winrt/Windows.Storage.h>

namespace fs = std::filesystem;

namespace CemuUwp::VideoPlayer
{
namespace
{
using Microsoft::WRL::ComPtr;
using winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DSurface;
using winrt::Windows::Media::Core::MediaSource;
using winrt::Windows::Media::Playback::MediaPlayer;
using winrt::Windows::Media::Playback::MediaPlayerFailedEventArgs;

constexpr uint32 kMaxWidth = 960;
constexpr uint32 kMaxHeight = 540;
constexpr auto kCaptureInterval = std::chrono::microseconds(33333);
constexpr auto kCaptureTolerance = std::chrono::milliseconds(2);
constexpr auto kCaptureResetThreshold = std::chrono::milliseconds(250);

std::mutex s_playerMutex;
std::mutex s_frameMutex;
std::mutex s_failedMutex;
std::atomic<bool> s_open{ false };
uint64 s_generation = 0;
fs::path s_currentPath;
// the path an async open is resolving, so a repeated request does not restart it
fs::path s_requestedPath;

MediaPlayer s_player{ nullptr };
MediaSource s_source{ nullptr };
winrt::event_token s_mediaOpenedToken{};
winrt::event_token s_mediaFailedToken{};
winrt::event_token s_videoFrameToken{};

ComPtr<ID3D11Device> s_d3dDevice;
ComPtr<ID3D11DeviceContext> s_d3dContext;
ComPtr<ID3D11Texture2D> s_videoTexture;
ComPtr<ID3D11Texture2D> s_readbackTexture;
IDirect3DSurface s_videoSurface{ nullptr };
uint32 s_videoWidth = 0;
uint32 s_videoHeight = 0;
std::chrono::steady_clock::time_point s_nextCapture{};
bool s_loggedFrameFailure = false;

std::vector<uint8> s_readyFrame;
int s_readyWidth = 0;
int s_readyHeight = 0;
bool s_frameReady = false;

std::set<fs::path> s_failedPaths;

void ClearReadyFrame()
{
	std::lock_guard lock(s_frameMutex);
	s_readyFrame.clear();
	s_readyWidth = 0;
	s_readyHeight = 0;
	s_frameReady = false;
}

void PublishFrame(std::vector<uint8>&& frame, uint32 width, uint32 height)
{
	std::lock_guard lock(s_frameMutex);
	s_readyFrame = std::move(frame);
	s_readyWidth = (int)width;
	s_readyHeight = (int)height;
	s_frameReady = true;
}

bool EnsureD3D11Device()
{
	if (s_d3dDevice && s_d3dContext)
		return true;

	constexpr D3D_FEATURE_LEVEL featureLevels[] = {
		D3D_FEATURE_LEVEL_11_1,
		D3D_FEATURE_LEVEL_11_0,
		D3D_FEATURE_LEVEL_10_1,
		D3D_FEATURE_LEVEL_10_0,
	};
	D3D_FEATURE_LEVEL selectedLevel{};
	const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
	HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
		featureLevels, (UINT)std::size(featureLevels), D3D11_SDK_VERSION,
		&s_d3dDevice, &selectedLevel, &s_d3dContext);
	if (FAILED(hr))
	{
		cemuLog_log(LogType::Force, "MediaPlayer: D3D11 video device creation failed (hr=0x{:08x})", (uint32)hr);
		s_d3dDevice.Reset();
		s_d3dContext.Reset();
		return false;
	}

	ComPtr<ID3D10Multithread> multithread;
	if (SUCCEEDED(s_d3dDevice.As(&multithread)) && multithread)
		multithread->SetMultithreadProtected(TRUE);

	const uint32 major = selectedLevel >> 12;
	const uint32 minor = (selectedLevel >> 8) & 0xF;
	cemuLog_logDebug(LogType::Force, "MediaPlayer: initialized D3D11 frame server at feature level {}_{}", major, minor);
	return true;
}

void ReleaseVideoSurfaces()
{
	s_videoSurface = nullptr;
	s_readbackTexture.Reset();
	s_videoTexture.Reset();
	s_videoWidth = 0;
	s_videoHeight = 0;
	s_nextCapture = {};
	s_loggedFrameFailure = false;
}

bool CreateVideoSurfaces(uint32 naturalWidth, uint32 naturalHeight)
{
	ReleaseVideoSurfaces();
	if (!s_d3dDevice || !s_d3dContext || naturalWidth == 0 || naturalHeight == 0)
		return false;

	const double scale = std::min({ 1.0, (double)kMaxWidth / naturalWidth, (double)kMaxHeight / naturalHeight });
	uint32 width = std::max<uint32>(2, (uint32)std::lround(naturalWidth * scale));
	uint32 height = std::max<uint32>(2, (uint32)std::lround(naturalHeight * scale));
	// Keep dimensions even for chroma-aligned decoders.
	width &= ~1u;
	height &= ~1u;

	D3D11_TEXTURE2D_DESC videoDesc{};
	videoDesc.Width = width;
	videoDesc.Height = height;
	videoDesc.MipLevels = 1;
	videoDesc.ArraySize = 1;
	videoDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	videoDesc.SampleDesc.Count = 1;
	videoDesc.Usage = D3D11_USAGE_DEFAULT;
	videoDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

	HRESULT hr = s_d3dDevice->CreateTexture2D(&videoDesc, nullptr, &s_videoTexture);
	if (FAILED(hr))
	{
		cemuLog_log(LogType::Force, "MediaPlayer: video surface creation failed (hr=0x{:08x})", (uint32)hr);
		ReleaseVideoSurfaces();
		return false;
	}

	D3D11_TEXTURE2D_DESC readbackDesc = videoDesc;
	readbackDesc.Usage = D3D11_USAGE_STAGING;
	readbackDesc.BindFlags = 0;
	readbackDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	hr = s_d3dDevice->CreateTexture2D(&readbackDesc, nullptr, &s_readbackTexture);
	if (FAILED(hr))
	{
		cemuLog_log(LogType::Force, "MediaPlayer: video readback surface creation failed (hr=0x{:08x})", (uint32)hr);
		ReleaseVideoSurfaces();
		return false;
	}

	ComPtr<IDXGISurface> dxgiSurface;
	ComPtr<IInspectable> inspectableSurface;
	if (FAILED(s_videoTexture.As(&dxgiSurface)) || !dxgiSurface ||
		FAILED(CreateDirect3D11SurfaceFromDXGISurface(dxgiSurface.Get(), &inspectableSurface)) || !inspectableSurface)
	{
		cemuLog_log(LogType::Force, "MediaPlayer: could not expose the D3D11 video surface to WinRT");
		ReleaseVideoSurfaces();
		return false;
	}

	s_videoSurface = IDirect3DSurface{ inspectableSurface.Detach(), winrt::take_ownership_from_abi };
	s_videoWidth = width;
	s_videoHeight = height;
	return true;
}

std::string PercentEncodePath(std::string_view path)
{
	static constexpr char hex[] = "0123456789ABCDEF";
	std::string encoded;
	encoded.reserve(path.size());
	for (const unsigned char ch : path)
	{
		const bool unreserved = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
			(ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.' || ch == '~' || ch == '/';
		if (unreserved)
		{
			encoded.push_back((char)ch);
		}
		else
		{
			encoded.push_back('%');
			encoded.push_back(hex[ch >> 4]);
			encoded.push_back(hex[ch & 0xF]);
		}
	}
	return encoded;
}

std::optional<winrt::Windows::Foundation::Uri> LocalMediaUri(const fs::path& path)
{
	try
	{
		const fs::path localRoot{ std::wstring(winrt::Windows::Storage::ApplicationData::Current().LocalFolder().Path()) };
		const fs::path relative = path.lexically_normal().lexically_relative(localRoot.lexically_normal());
		if (relative.empty() || *relative.begin() == L"..")
			return std::nullopt;

		const std::string relativeUtf8 = winrt::to_string(winrt::hstring(relative.generic_wstring()));
		const std::string uriUtf8 = "ms-appdata:///local/" + PercentEncodePath(relativeUtf8);
		return winrt::Windows::Foundation::Uri(winrt::to_hstring(uriUtf8));
	}
	catch (const winrt::hresult_error& ex)
	{
		cemuLog_log(LogType::Force, "MediaPlayer: could not create a local media URI (hr=0x{:08x})", (uint32)ex.code());
		return std::nullopt;
	}
}

void LogFrameFailureOnce(std::string_view operation, HRESULT hr)
{
	if (s_loggedFrameFailure)
		return;
	s_loggedFrameFailure = true;
	cemuLog_log(LogType::Force, "MediaPlayer: {} failed (hr=0x{:08x}); suppressing repeated frame errors", operation, (uint32)hr);
}

void OnVideoFrameAvailable(const MediaPlayer& sender, uint64 generation) noexcept
{
	try
	{
		std::vector<uint8> frame;
		uint32 width = 0;
		uint32 height = 0;
		{
			std::lock_guard lock(s_playerMutex);
			if (generation != s_generation || !s_open.load() || !s_videoSurface ||
				!s_videoTexture || !s_readbackTexture || !s_d3dContext)
				return;

			const auto now = std::chrono::steady_clock::now();
			if (s_nextCapture != std::chrono::steady_clock::time_point{} && now + kCaptureTolerance < s_nextCapture)
				return;
			if (s_nextCapture == std::chrono::steady_clock::time_point{} || now > s_nextCapture + kCaptureResetThreshold)
				s_nextCapture = now;
			s_nextCapture += kCaptureInterval;

			sender.CopyFrameToVideoSurface(s_videoSurface);
			s_d3dContext->CopyResource(s_readbackTexture.Get(), s_videoTexture.Get());

			D3D11_MAPPED_SUBRESOURCE mapped{};
			const HRESULT mapHr = s_d3dContext->Map(s_readbackTexture.Get(), 0, D3D11_MAP_READ, 0, &mapped);
			if (FAILED(mapHr) || !mapped.pData)
			{
				LogFrameFailureOnce("D3D11 video readback", mapHr);
				return;
			}

			width = s_videoWidth;
			height = s_videoHeight;
			const size_t rowBytes = (size_t)width * 4;
			frame.resize(rowBytes * height);
			for (uint32 y = 0; y < height; ++y)
				std::memcpy(frame.data() + (size_t)y * rowBytes,
					static_cast<const uint8*>(mapped.pData) + (size_t)y * mapped.RowPitch, rowBytes);
			s_d3dContext->Unmap(s_readbackTexture.Get(), 0);
		}

		if (!frame.empty())
			PublishFrame(std::move(frame), width, height);
	}
	catch (const winrt::hresult_error& ex)
	{
		std::lock_guard lock(s_playerMutex);
		if (generation == s_generation)
			LogFrameFailureOnce("video-frame copy", ex.code());
	}
	catch (...)
	{
		std::lock_guard lock(s_playerMutex);
		if (generation == s_generation && !s_loggedFrameFailure)
		{
			s_loggedFrameFailure = true;
			cemuLog_log(LogType::Force, "MediaPlayer: unexpected video-frame copy failure; suppressing repeats");
		}
	}
}

void OnMediaOpened(const MediaPlayer& sender, uint64 generation, const fs::path& path) noexcept
{
	try
	{
		std::lock_guard lock(s_playerMutex);
		if (generation != s_generation || !s_open.load())
			return;

		const auto session = sender.PlaybackSession();
		const uint32 naturalWidth = session.NaturalVideoWidth();
		const uint32 naturalHeight = session.NaturalVideoHeight();
		if (!CreateVideoSurfaces(naturalWidth, naturalHeight))
		{
			s_open.store(false);
			std::lock_guard failedLock(s_failedMutex);
			s_failedPaths.insert(path);
			return;
		}

		cemuLog_log(LogType::Force,
			"MediaPlayer: playing {} at {}x{} -> {}x{} frame-server output (system clock, 30 fps capture ceiling)",
			_pathToUtf8(path), naturalWidth, naturalHeight, s_videoWidth, s_videoHeight);
	}
	catch (const winrt::hresult_error& ex)
	{
		cemuLog_log(LogType::Force, "MediaPlayer: MediaOpened setup failed for {} (hr=0x{:08x})",
			_pathToUtf8(path), (uint32)ex.code());
		std::lock_guard lock(s_playerMutex);
		if (generation == s_generation)
			s_open.store(false);
		std::lock_guard failedLock(s_failedMutex);
		s_failedPaths.insert(path);
	}
}

void OnMediaFailed(uint64 generation, const fs::path& path, const MediaPlayerFailedEventArgs& args) noexcept
{
	uint32 errorCode = 0;
	std::string message;
	try
	{
		errorCode = (uint32)args.ExtendedErrorCode();
		message = winrt::to_string(args.ErrorMessage());
	}
	catch (...)
	{
	}

	{
		std::lock_guard lock(s_playerMutex);
		if (generation != s_generation)
			return;
		s_open.store(false);
		ReleaseVideoSurfaces();
	}
	{
		std::lock_guard lock(s_failedMutex);
		s_failedPaths.insert(path);
	}
	ClearReadyFrame();
	cemuLog_log(LogType::Force, "MediaPlayer: playback failed for {} (hr=0x{:08x}, {})",
		_pathToUtf8(path), errorCode, message.empty() ? "no message" : message);
}

void StopPlayer()
{
	MediaPlayer player{ nullptr };
	MediaSource source{ nullptr };
	winrt::event_token mediaOpenedToken{};
	winrt::event_token mediaFailedToken{};
	winrt::event_token videoFrameToken{};
	{
		std::lock_guard lock(s_playerMutex);
		++s_generation;
		s_open.store(false);
		player = s_player;
		source = s_source;
		mediaOpenedToken = s_mediaOpenedToken;
		mediaFailedToken = s_mediaFailedToken;
		videoFrameToken = s_videoFrameToken;
		s_player = nullptr;
		s_source = nullptr;
		s_mediaOpenedToken = {};
		s_mediaFailedToken = {};
		s_videoFrameToken = {};
		s_currentPath.clear();
		ReleaseVideoSurfaces();
	}

	if (player)
	{
		try
		{
			if (videoFrameToken)
				player.VideoFrameAvailable(videoFrameToken);
			if (mediaOpenedToken)
				player.MediaOpened(mediaOpenedToken);
			if (mediaFailedToken)
				player.MediaFailed(mediaFailedToken);
			player.Pause();
			player.Source(nullptr);
			player.Close();
		}
		catch (...)
		{
		}
	}
	if (source)
	{
		try
		{
			source.Close();
		}
		catch (...)
		{
		}
	}
	ClearReadyFrame();
}

bool StartPlayback(const fs::path& path, const MediaSource& source);

winrt::fire_and_forget OpenStorageFile(fs::path path)
{
	try
	{
		const auto file = co_await winrt::Windows::Storage::StorageFile::GetFileFromPathAsync(
			path.wstring());
		StartPlayback(path, MediaSource::CreateFromStorageFile(file));
	}
	catch (const winrt::hresult_error& ex)
	{
		cemuLog_log(LogType::Force, "MediaPlayer: cannot open {} (hr=0x{:08x})",
			_pathToUtf8(path), (uint32)ex.code());
		{
			std::lock_guard lock(s_playerMutex);
			if (s_requestedPath == path)
				s_requestedPath.clear();
		}
		std::lock_guard lock(s_failedMutex);
		s_failedPaths.insert(path);
	}
}
} // namespace

bool Open(const fs::path& path)
{
	if (path.empty() || !fs::exists(path))
		return false;
	{
		std::lock_guard lock(s_failedMutex);
		if (s_failedPaths.contains(path))
			return false;
	}
	{
		std::lock_guard lock(s_playerMutex);
		if ((s_open.load() && s_currentPath == path) || s_requestedPath == path)
			return true;
		s_requestedPath = path;
	}

	// media outside LocalState needs an explicit StorageFile, which only resolves asynchronously
	const auto uri = LocalMediaUri(path);
	if (!uri)
	{
		OpenStorageFile(path);
		return true;
	}

	return StartPlayback(path, MediaSource::CreateFromUri(*uri));
}

namespace
{
bool StartPlayback(const fs::path& path, const MediaSource& source)
{
	StopPlayer();
	try
	{
		std::lock_guard lock(s_playerMutex);
		if (!EnsureD3D11Device())
			return false;

		const uint64 generation = s_generation;
		MediaPlayer player;
		player.CommandManager().IsEnabled(false);
		player.IsMuted(true);
		player.IsLoopingEnabled(true);
		player.IsVideoFrameServerEnabled(true);

		s_currentPath = path;
		s_player = player;
		s_source = source;
		s_open.store(true);
		s_mediaOpenedToken = player.MediaOpened([generation, path](const MediaPlayer& sender, const winrt::Windows::Foundation::IInspectable&) {
			OnMediaOpened(sender, generation, path);
		});
		s_mediaFailedToken = player.MediaFailed([generation, path](const MediaPlayer&, const MediaPlayerFailedEventArgs& args) {
			OnMediaFailed(generation, path, args);
		});
		s_videoFrameToken = player.VideoFrameAvailable([generation](const MediaPlayer& sender, const winrt::Windows::Foundation::IInspectable&) {
			OnVideoFrameAvailable(sender, generation);
		});

		player.Source(source);
		player.Play();
		return true;
	}
	catch (const winrt::hresult_error& ex)
	{
		cemuLog_log(LogType::Force, "MediaPlayer: cannot open {} (hr=0x{:08x})", _pathToUtf8(path), (uint32)ex.code());
		StopPlayer();
		{
			std::lock_guard lock(s_playerMutex);
			if (s_requestedPath == path)
				s_requestedPath.clear();
		}
		std::lock_guard lock(s_failedMutex);
		s_failedPaths.insert(path);
		return false;
	}
}
} // namespace

void Close()
{
	{
		std::lock_guard lock(s_playerMutex);
		s_requestedPath.clear();
	}
	StopPlayer();
}

bool IsOpen()
{
	return s_open.load();
}

fs::path CurrentPath()
{
	std::lock_guard lock(s_playerMutex);
	return s_requestedPath.empty() ? s_currentPath : s_requestedPath;
}

bool AcquireFrame(std::vector<uint8>& bgra, int& width, int& height)
{
	std::lock_guard lock(s_frameMutex);
	if (!s_frameReady || s_readyWidth <= 0 || s_readyHeight <= 0)
		return false;
	bgra.swap(s_readyFrame);
	s_readyFrame.clear();
	width = s_readyWidth;
	height = s_readyHeight;
	s_frameReady = false;
	return true;
}

void Shutdown()
{
	StopPlayer();
	{
		std::lock_guard lock(s_failedMutex);
		s_failedPaths.clear();
	}
	std::lock_guard lock(s_playerMutex);
	s_requestedPath.clear();
	s_d3dContext.Reset();
	s_d3dDevice.Reset();
}
}
