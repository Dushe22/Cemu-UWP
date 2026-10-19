#include "UwpInputProvider.h"

#include "Cemu/Logging/CemuLogging.h"
#include "UwpFrontend.h"
#include "WindowSystem.h"
#include "imgui.h"
#include "input/InputManager.h"

#include <winrt/Windows.Devices.Input.h>

#include <atomic>
#include <cmath>
#include <optional>

namespace
{
std::atomic_bool s_coreTextInputActive{ false };
std::atomic_bool s_coreTextShiftDown{ false };
std::atomic<double> s_pointerScaleX{ 1.0 };
std::atomic<double> s_pointerScaleY{ 1.0 };
std::atomic<float> s_pointerLogicalX{ 0.0f };
std::atomic<float> s_pointerLogicalY{ 0.0f };
std::atomic_bool s_pointerPositionValid{ false };

std::optional<CemuUwp::Frontend::TextEditingCommand> TextEditingCommandForKey(winrt::Windows::System::VirtualKey key)
{
	using Command = CemuUwp::Frontend::TextEditingCommand;
	using winrt::Windows::System::VirtualKey;
	switch (key)
	{
	case VirtualKey::Back:
		return Command::Backspace;
	case VirtualKey::Delete:
		return Command::Delete;
	case VirtualKey::Left:
		return Command::MoveLeft;
	case VirtualKey::Right:
		return Command::MoveRight;
	case VirtualKey::Home:
		return Command::MoveHome;
	case VirtualKey::End:
		return Command::MoveEnd;
	case VirtualKey::Enter:
		return Command::Submit;
	default:
		return std::nullopt;
	}
}

void UpdateMouseInfo(InputManager::MouseInfo& mouseInfo, const winrt::Windows::UI::Input::PointerPoint& point)
{
	const auto position = point.Position();
	const auto properties = point.Properties();
	const bool isMouse = point.PointerDevice().PointerDeviceType() == winrt::Windows::Devices::Input::PointerDeviceType::Mouse;
	const bool leftDown = isMouse ? properties.IsLeftButtonPressed() : point.IsInContact();
	const bool rightDown = isMouse && properties.IsRightButtonPressed();
	const double scaleX = s_pointerScaleX.load(std::memory_order_acquire);
	const double scaleY = s_pointerScaleY.load(std::memory_order_acquire);
	s_pointerLogicalX.store(position.X, std::memory_order_release);
	s_pointerLogicalY.store(position.Y, std::memory_order_release);
	s_pointerPositionValid.store(true, std::memory_order_release);

	std::unique_lock lock(mouseInfo.m_mutex);
	mouseInfo.position = {
		(int)std::lround((double)position.X * scaleX),
		(int)std::lround((double)position.Y * scaleY),
	};
	if (leftDown && !mouseInfo.left_down)
		mouseInfo.left_down_toggle = true;
	if (rightDown && !mouseInfo.right_down)
		mouseInfo.right_down_toggle = true;
	mouseInfo.left_down = leftDown;
	mouseInfo.right_down = rightDown;
}
}

namespace CemuUwp::InputProvider
{
void SetKeyState(winrt::Windows::System::VirtualKey key, bool isDown)
{
	if (s_coreTextInputActive.load(std::memory_order_acquire))
	{
		// CoreWindow keeps delivering key events while CoreText owns the session
		WindowSystem::GetWindowInfo().set_keystate(static_cast<uint32>(key), false);
		if (key == winrt::Windows::System::VirtualKey::Shift)
		{
			s_coreTextShiftDown.store(isDown, std::memory_order_release);
			return;
		}
		if (const auto command = TextEditingCommandForKey(key))
		{
			if (isDown)
			{
				const bool extendSelection = s_coreTextShiftDown.load(std::memory_order_acquire);
				cemuLog_logDebug(LogType::Force, "UWP CoreText editor command: virtualKey={} extendSelection={}",
					(uint32)key, extendSelection);
				CemuUwp::Frontend::HandleTextEditingCommand(*command, extendSelection);
			}
		}
		return;
	}
	WindowSystem::GetWindowInfo().set_keystate(static_cast<uint32>(key), isDown);
}

void SetCoreTextInputActive(bool active)
{
	s_coreTextInputActive.store(active, std::memory_order_release);
	if (!active)
		s_coreTextShiftDown.store(false, std::memory_order_release);
}

bool IsCoreTextInputActive()
{
	return s_coreTextInputActive.load(std::memory_order_acquire);
}

void AddTextCharacter(uint32 character)
{
	if (character < 0x20 || character == 0x7f)
		return;
	// a focused CoreTextEditContext delivers text through TextUpdating instead
	if (s_coreTextInputActive.load(std::memory_order_acquire))
		return;
	if (!ImGui::GetCurrentContext())
		return;
	ImGui::GetIO().AddInputCharacter(character);
}

void SetPointerScale(double scaleX, double scaleY)
{
	scaleX = std::isfinite(scaleX) && scaleX > 0.0 ? scaleX : 1.0;
	scaleY = std::isfinite(scaleY) && scaleY > 0.0 ? scaleY : 1.0;
	s_pointerScaleX.store(scaleX, std::memory_order_release);
	s_pointerScaleY.store(scaleY, std::memory_order_release);

	// no pointer event follows a DPI change, so reproject the last position
	if (!s_pointerPositionValid.load(std::memory_order_acquire))
		return;
	auto& mouseInfo = InputManager::instance().m_main_mouse;
	std::unique_lock lock(mouseInfo.m_mutex);
	mouseInfo.position = {
		(int)std::lround((double)s_pointerLogicalX.load(std::memory_order_acquire) * scaleX),
		(int)std::lround((double)s_pointerLogicalY.load(std::memory_order_acquire) * scaleY),
	};
}

void SetPointerState(const winrt::Windows::UI::Core::PointerEventArgs& args)
{
	UpdateMouseInfo(InputManager::instance().m_main_mouse, args.CurrentPoint());
}

void ClearPointerState()
{
	s_pointerPositionValid.store(false, std::memory_order_release);
	auto& mouseInfo = InputManager::instance().m_main_mouse;
	std::unique_lock lock(mouseInfo.m_mutex);
	mouseInfo.left_down = false;
	mouseInfo.right_down = false;
}
}
