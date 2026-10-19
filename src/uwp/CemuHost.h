#pragma once

#include <stdexcept>
#include <string>
#include <string_view>

#include <winrt/Windows.UI.Core.h>

namespace CemuHost
{
	class GraphicsFailure final : public std::runtime_error
	{
	public:
		using std::runtime_error::runtime_error;
	};

	bool Initialize(const winrt::Windows::UI::Core::CoreWindow& window);
	bool LaunchPath(const fs::path& launchPath);
	void EnsureAudioInitialized();
	void EnsureGraphicPacksLoaded();
	bool RestartTitle();
	void SetTitlePaused(bool paused);
	void SetWindowVisible(bool visible);
	void SetWindowActive(bool active);
	bool IsGraphicsDeviceLost();
	bool HandleGraphicsFailure(std::string_view message);
	void Trace(std::string_view message);
	void ShutdownTitle();
	void Shutdown();
	// returns true when a frontend frame was presented
	bool TickFrontend();
};
