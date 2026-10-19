#pragma once

#include <string>

#include <winrt/Windows.UI.Core.h>

namespace CemuUwp::WindowHost
{
	void SetCoreWindow(const winrt::Windows::UI::Core::CoreWindow& window);
	void SetVisible(bool visible);
	void SetActive(bool active);
	bool ConsumeResize();
	bool HasLaunchOnExitUri();
	void RecordCaughtAllocationFailure() noexcept;
	void RequestGameplayGraphicsRecovery(std::string message) noexcept;
};
