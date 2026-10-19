#pragma once

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.Input.h>

namespace CemuUwp::InputProvider
{
	void SetKeyState(winrt::Windows::System::VirtualKey key, bool isDown);
	void SetCoreTextInputActive(bool active);
	bool IsCoreTextInputActive();
	void AddTextCharacter(uint32 character);
	void SetPointerScale(double scaleX, double scaleY);
	void SetPointerState(const winrt::Windows::UI::Core::PointerEventArgs& args);
	void ClearPointerState();
};
