#include "input/api/WGI/WGIGamepadController.h"

#if defined(CEMU_UWP)

#include "Cemu/Logging/CemuLogging.h"
#include "uwp/UwpFrontend.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <glm/geometric.hpp>
#include <limits>
#include <optional>

namespace
{
using winrt::Windows::Gaming::Input::GamepadButtons;
using winrt::Windows::Gaming::Input::GamepadReading;
using winrt::Windows::Gaming::Input::GamepadVibration;

bool HasButton(GamepadReading const& reading, GamepadButtons button)
{
	return (static_cast<uint32>(reading.Buttons) & static_cast<uint32>(button)) != 0;
}

float ClampStickValue(double value)
{
	return std::clamp(static_cast<float>(value), -1.0f, 1.0f);
}

float ClampTriggerValue(double value)
{
	return std::clamp(static_cast<float>(value), 0.0f, 1.0f);
}

// counts gamepad arrivals and removals; a gamepad that sleeps or disconnects comes back as a new object,
// while the old one keeps returning empty readings without failing
std::atomic_uint32_t s_gamepadListVersion{ 0 };

uint32 GamepadListVersion()
{
	static std::once_flag s_watchStarted;
	std::call_once(s_watchStarted, []()
	{
		using winrt::Windows::Gaming::Input::Gamepad;
		try
		{
			Gamepad::GamepadAdded([](auto&&, Gamepad const&) { s_gamepadListVersion.fetch_add(1, std::memory_order_release); });
			Gamepad::GamepadRemoved([](auto&&, Gamepad const&) { s_gamepadListVersion.fetch_add(1, std::memory_order_release); });
		}
		catch (const winrt::hresult_error& ex)
		{
			cemuLog_log(LogType::Force, "WGIGamepad: could not watch for gamepad changes (0x{:08x})", (uint32)ex.code());
		}
	});
	return s_gamepadListVersion.load(std::memory_order_acquire);
}

constexpr std::string_view kEncodedStablePrefix = "stable-v1:";
constexpr std::string_view kLegacyStablePrefix = "stable:";

std::string EncodeStableIdentity(std::string_view identity)
{
	constexpr char kHex[] = "0123456789abcdef";
	std::string result;
	result.reserve(kEncodedStablePrefix.size() + identity.size() * 2);
	result += kEncodedStablePrefix;
	for (const unsigned char value : identity)
	{
		result.push_back(kHex[value >> 4]);
		result.push_back(kHex[value & 0x0f]);
	}
	return result;
}

std::optional<std::string> DecodeStableIdentity(std::string_view encoded)
{
	if (encoded.size() % 2 != 0)
		return std::nullopt;
	std::string result;
	result.reserve(encoded.size() / 2);
	for (std::size_t offset = 0; offset < encoded.size(); offset += 2)
	{
		unsigned value = 0;
		const auto parsed = std::from_chars(encoded.data() + offset,
			encoded.data() + offset + 2, value, 16);
		if (parsed.ec != std::errc{} || parsed.ptr != encoded.data() + offset + 2)
			return std::nullopt;
		result.push_back(static_cast<char>(value));
	}
	return result;
}

}

WGIGamepadController::WGIGamepadController(uint32 index, Binding binding)
	: base_type(fmt::format("{}", index), fmt::format("Xbox Gamepad {}", index + 1)), m_index(index)
{
	m_settings.axis.deadzone = m_settings.rotation.deadzone = m_settings.trigger.deadzone = 0.15f;
	m_identityFrozen = binding == Binding::Position;
	connect();
	m_identityFrozen = true;
}

WGIGamepadController::WGIGamepadController(std::string_view uuid,
	std::string_view displayName)
	: base_type(uuid, "Xbox Gamepad"),
	  m_index(std::numeric_limits<uint32>::max())
{
	(void)displayName;
	if (uuid.starts_with(kEncodedStablePrefix))
	{
		if (auto decoded = DecodeStableIdentity(uuid.substr(kEncodedStablePrefix.size())))
			m_nonRoamableId = std::move(*decoded);
	}
	else if (uuid.starts_with(kLegacyStablePrefix))
		m_nonRoamableId = uuid.substr(kLegacyStablePrefix.size());
	else
	{
		uint32 index = 0;
		const auto parsed = std::from_chars(uuid.data(), uuid.data() + uuid.size(), index);
		if (parsed.ec == std::errc{} && parsed.ptr == uuid.data() + uuid.size())
			m_index = index;
	}
	m_settings.axis.deadzone = m_settings.rotation.deadzone = m_settings.trigger.deadzone = 0.15f;
	connect();
	m_identityFrozen = true;
}

bool WGIGamepadController::connect()
{
	std::scoped_lock lock(m_gamepadMutex);
	return TryConnectLocked(true);
}

void WGIGamepadController::DropRemovedGamepadLocked()
{
	const uint32 version = GamepadListVersion();
	if (!m_gamepad || version == m_seenGamepadListVersion)
		return;
	m_seenGamepadListVersion = version;
	try
	{
		for (const auto& gamepad : winrt::Windows::Gaming::Input::Gamepad::Gamepads())
		{
			if (gamepad == m_gamepad)
				return;
		}
	}
	catch (const winrt::hresult_error&)
	{
	}
	// reconnecting picks up the object the gamepad came back as
	m_gamepad = nullptr;
	m_nextReconnectAttempt = {};
	SetConnectionStateLocked(false);
}

bool WGIGamepadController::TryConnectLocked(bool force)
{
	DropRemovedGamepadLocked();
	if (m_gamepad)
		return true;
	const auto now = std::chrono::steady_clock::now();
	if (!force && now < m_nextReconnectAttempt)
		return false;
	try
	{
		// after its first connection the controller waits for that gamepad instead of taking whichever one moved
		// into its position, which may belong to another player
		m_gamepad = m_sessionGamepadId.empty() ? m_provider->GetGamepad(m_nonRoamableId, m_index) :
			m_provider->GetGamepad(m_sessionGamepadId, m_index);
	}
	catch (const winrt::hresult_error&)
	{
		m_gamepad = nullptr;
	}
	const bool connected = m_gamepad != nullptr;
	m_nextReconnectAttempt = connected ? std::chrono::steady_clock::time_point{} :
		now + std::chrono::seconds(1);
	if (connected)
	{
		try
		{
			AdoptStableIdentity(m_gamepad);
			if (m_sessionGamepadId.empty())
				m_sessionGamepadId = WGIGamepadControllerProvider::GetNonRoamableId(m_gamepad);
		}
		catch (const winrt::hresult_error&)
		{
			// identity metadata is optional, input stays usable without it
		}
	}
	SetConnectionStateLocked(connected);
	return connected;
}

void WGIGamepadController::SetConnectionStateLocked(bool connected)
{
	if (!m_hasLoggedConnectionState || connected != m_lastLoggedConnected)
	{
		cemuLog_log(LogType::Force, "WGIGamepad[{}] {}", m_index, connected ? "connected" : "not connected");
		m_hasLoggedConnectionState = true;
		m_lastLoggedConnected = connected;
	}
}

void WGIGamepadController::MarkDisconnectedLocked()
{
	m_gamepad = nullptr;
	m_nextReconnectAttempt = std::chrono::steady_clock::now() + std::chrono::seconds(1);
	SetConnectionStateLocked(false);
}

void WGIGamepadController::AdoptStableIdentity(
	const winrt::Windows::Gaming::Input::Gamepad& gamepad)
{
	if (m_identityFrozen)
		return;
	const std::string identity = WGIGamepadControllerProvider::GetNonRoamableId(gamepad);
	if (!identity.empty())
	{
		m_nonRoamableId = identity;
		m_uuid = EncodeStableIdentity(identity);
	}
	const auto gamepads = winrt::Windows::Gaming::Input::Gamepad::Gamepads();
	for (uint32 index = 0; index < gamepads.Size(); ++index)
	{
		if (WGIGamepadControllerProvider::GetNonRoamableId(gamepads.GetAt(index)) ==
			identity)
		{
			m_index = index;
			break;
		}
	}
	m_display_name = m_index == std::numeric_limits<uint32>::max() ?
		"Xbox Gamepad" : fmt::format("Xbox Gamepad {}", m_index + 1);
}

bool WGIGamepadController::is_connected()
{
	std::scoped_lock lock(m_gamepadMutex);
	return TryConnectLocked(false);
}

std::string WGIGamepadController::session_gamepad_id() const
{
	std::scoped_lock lock(m_gamepadMutex);
	return m_sessionGamepadId;
}

void WGIGamepadController::use_gamepad_for_session(std::string_view nonRoamableId)
{
	std::scoped_lock lock(m_gamepadMutex);
	m_sessionGamepadId = nonRoamableId;
	m_gamepad = nullptr;
	m_nextReconnectAttempt = {};
}

bool WGIGamepadController::has_motion()
{
	return false;
}

MotionSample WGIGamepadController::get_motion_sample()
{
	return {};
}

bool WGIGamepadController::has_position()
{
	std::scoped_lock lock(m_virtualPositionMutex);
	return m_virtualPositionActive;
}

glm::vec2 WGIGamepadController::get_position()
{
	std::scoped_lock lock(m_virtualPositionMutex);
	return m_virtualPosition;
}

glm::vec2 WGIGamepadController::get_prev_position()
{
	std::scoped_lock lock(m_virtualPositionMutex);
	return m_previousVirtualPosition;
}

PositionVisibility WGIGamepadController::GetPositionVisibility()
{
	return has_position() ? PositionVisibility::FULL : PositionVisibility::NONE;
}

void WGIGamepadController::save(pugi::xml_node& node)
{
	base_type::save(node);

	std::scoped_lock lock(m_virtualPositionMutex);
	auto virtualPosition = node.append_child("uwp_virtual_position");
	virtualPosition.append_child("enabled").append_child(pugi::node_pcdata).set_value(
		fmt::format("{}", (int)m_virtualPositionEnabled).c_str());
	virtualPosition.append_child("speed").append_child(pugi::node_pcdata).set_value(
		fmt::format("{}", m_virtualPositionSpeed).c_str());
	virtualPosition.append_child("deadzone").append_child(pugi::node_pcdata).set_value(
		fmt::format("{}", m_virtualPositionDeadzone).c_str());
	virtualPosition.append_child("stick").append_child(pugi::node_pcdata).set_value(
		fmt::format("{}", (int)m_virtualPositionStick).c_str());
	virtualPosition.append_child("activation").append_child(pugi::node_pcdata).set_value(
		fmt::format("{}", (int)m_virtualPositionActivation).c_str());
}

void WGIGamepadController::load(const pugi::xml_node& node)
{
	base_type::load(node);

	const auto virtualPosition = node.child("uwp_virtual_position");
	if (!virtualPosition)
		return;

	std::scoped_lock lock(m_virtualPositionMutex);
	if (const auto enabled = virtualPosition.child("enabled"))
		m_virtualPositionEnabled = ConvertString<bool>(enabled.child_value());
	if (const auto speed = virtualPosition.child("speed"))
		m_virtualPositionSpeed = std::clamp(ConvertString<float>(speed.child_value()), 0.15f, 2.5f);
	if (const auto deadzone = virtualPosition.child("deadzone"))
		m_virtualPositionDeadzone = std::clamp(
			ConvertString<float>(deadzone.child_value()), 0.0f, 0.95f);
	if (const auto stick = virtualPosition.child("stick"))
		m_virtualPositionStick = ConvertString<int>(stick.child_value()) == 1 ?
			VirtualPositionStick::Left : VirtualPositionStick::Right;
	if (const auto activation = virtualPosition.child("activation"))
		m_virtualPositionActivation = ConvertString<int>(activation.child_value()) == 1 ?
			VirtualPositionActivation::Always : VirtualPositionActivation::HoldStickButton;
	if (!m_virtualPositionEnabled)
		m_virtualPositionActive = false;
}

bool WGIGamepadController::virtual_position_enabled() const
{
	std::scoped_lock lock(m_virtualPositionMutex);
	return m_virtualPositionEnabled;
}

void WGIGamepadController::set_virtual_position_enabled(bool enabled)
{
	std::scoped_lock lock(m_virtualPositionMutex);
	m_virtualPositionEnabled = enabled;
	if (!enabled)
	{
		m_virtualPositionActive = false;
		m_previousVirtualPosition = m_virtualPosition;
	}
}

float WGIGamepadController::virtual_position_speed() const
{
	std::scoped_lock lock(m_virtualPositionMutex);
	return m_virtualPositionSpeed;
}

void WGIGamepadController::set_virtual_position_speed(float speed)
{
	std::scoped_lock lock(m_virtualPositionMutex);
	m_virtualPositionSpeed = std::clamp(speed, 0.15f, 2.5f);
}

float WGIGamepadController::virtual_position_deadzone() const
{
	std::scoped_lock lock(m_virtualPositionMutex);
	return m_virtualPositionDeadzone;
}

void WGIGamepadController::set_virtual_position_deadzone(float deadzone)
{
	std::scoped_lock lock(m_virtualPositionMutex);
	m_virtualPositionDeadzone = std::clamp(deadzone, 0.0f, 0.95f);
}

WGIGamepadController::VirtualPositionStick WGIGamepadController::virtual_position_stick() const
{
	std::scoped_lock lock(m_virtualPositionMutex);
	return m_virtualPositionStick;
}

void WGIGamepadController::set_virtual_position_stick(VirtualPositionStick stick)
{
	std::scoped_lock lock(m_virtualPositionMutex);
	m_virtualPositionStick = stick;
}

WGIGamepadController::VirtualPositionActivation WGIGamepadController::virtual_position_activation() const
{
	std::scoped_lock lock(m_virtualPositionMutex);
	return m_virtualPositionActivation;
}

void WGIGamepadController::set_virtual_position_activation(VirtualPositionActivation activation)
{
	std::scoped_lock lock(m_virtualPositionMutex);
	m_virtualPositionActivation = activation;
}

bool WGIGamepadController::UpdateVirtualPosition(const GamepadReading& reading)
{
	const auto now = std::chrono::steady_clock::now();
	std::scoped_lock lock(m_virtualPositionMutex);

	const bool useLeftStick = m_virtualPositionStick == VirtualPositionStick::Left;
	const bool activated = m_virtualPositionActivation == VirtualPositionActivation::Always ||
		HasButton(reading, useLeftStick ? GamepadButtons::LeftThumbstick : GamepadButtons::RightThumbstick);
	const bool wasActive = m_virtualPositionActive;
	m_virtualPositionActive = m_virtualPositionEnabled && activated;
	if (!m_virtualPositionActive)
	{
		m_previousVirtualPosition = m_virtualPosition;
		m_lastVirtualPositionUpdate = now;
		return false;
	}

	float elapsed = 0.0f;
	if (wasActive && m_lastVirtualPositionUpdate != std::chrono::steady_clock::time_point{})
		elapsed = std::clamp(std::chrono::duration<float>(now - m_lastVirtualPositionUpdate).count(), 0.0f, 0.05f);
	m_lastVirtualPositionUpdate = now;
	m_previousVirtualPosition = m_virtualPosition;

	glm::vec2 direction{
		ClampStickValue(useLeftStick ? reading.LeftThumbstickX : reading.RightThumbstickX),
		-ClampStickValue(useLeftStick ? reading.LeftThumbstickY : reading.RightThumbstickY),
	};
	const float magnitude = glm::length(direction);
	if (magnitude > m_virtualPositionDeadzone)
	{
		const float scaledMagnitude = std::min(
			(magnitude - m_virtualPositionDeadzone) /
				(1.0f - m_virtualPositionDeadzone), 1.0f);
		direction *= scaledMagnitude / magnitude;
		m_virtualPosition += direction * (m_virtualPositionSpeed * elapsed);
		m_virtualPosition.x = std::clamp(m_virtualPosition.x, 0.0f, 1.0f);
		m_virtualPosition.y = std::clamp(m_virtualPosition.y, 0.0f, 1.0f);
	}
	return true;
}

void WGIGamepadController::DeactivateVirtualPosition()
{
	std::scoped_lock lock(m_virtualPositionMutex);
	m_virtualPositionActive = false;
	m_previousVirtualPosition = m_virtualPosition;
	m_lastVirtualPositionUpdate = std::chrono::steady_clock::now();
}

void WGIGamepadController::start_rumble()
{
	const float strength = get_settings().rumble;
	if (strength <= 0)
		return;
	std::scoped_lock lock(m_gamepadMutex);
	if (!m_gamepad)
		return;
	try
	{
		GamepadVibration vibration{};
		vibration.LeftMotor = strength;
		vibration.RightMotor = strength;
		m_gamepad.Vibration(vibration);
	}
	catch (const winrt::hresult_error&)
	{
		MarkDisconnectedLocked();
	}
}

void WGIGamepadController::stop_rumble()
{
	std::scoped_lock lock(m_gamepadMutex);
	if (!m_gamepad)
		return;
	try
	{
		m_gamepad.Vibration({});
	}
	catch (const winrt::hresult_error&)
	{
		MarkDisconnectedLocked();
	}
}

std::string WGIGamepadController::get_button_name(uint64 button) const
{
	switch (button)
	{
	case kButton0: return "Xbox D-Pad Up";
	case kButton1: return "Xbox D-Pad Down";
	case kButton2: return "Xbox D-Pad Left";
	case kButton3: return "Xbox D-Pad Right";
	case kButton4: return "Xbox Menu";
	case kButton5: return "Xbox View";
	case kButton6: return "Xbox Left Stick Click";
	case kButton7: return "Xbox Right Stick Click";
	case kButton8: return "Xbox Left Bumper (LB)";
	case kButton9: return "Xbox Right Bumper (RB)";
	case kButton12: return "Xbox A";
	case kButton13: return "Xbox B";
	case kButton14: return "Xbox X";
	case kButton15: return "Xbox Y";
	case kAxisXP: return "Xbox Left Stick Right";
	case kAxisYP: return "Xbox Left Stick Up";
	case kAxisXN: return "Xbox Left Stick Left";
	case kAxisYN: return "Xbox Left Stick Down";
	case kRotationXP: return "Xbox Right Stick Right";
	case kRotationYP: return "Xbox Right Stick Up";
	case kRotationXN: return "Xbox Right Stick Left";
	case kRotationYN: return "Xbox Right Stick Down";
	case kTriggerXP: return "Xbox Left Trigger (LT)";
	case kTriggerYP: return "Xbox Right Trigger (RT)";
	default:
		return Controller::get_button_name(button);
	}
}

ControllerState WGIGamepadController::raw_state()
{
	ControllerState result{};
	if (CemuUwp::Frontend::IsGameplayInputCaptured() &&
		!CemuUwp::Frontend::IsInputMappingReadActive())
	{
		DeactivateVirtualPosition();
		return result;
	}
	bool virtualPositionActive = false;
	bool connected = false;
	{
		std::scoped_lock lock(m_gamepadMutex);
		(void)TryConnectLocked(false);
		if (m_gamepad)
		{
			try
			{
				const auto reading = m_gamepad.GetCurrentReading();
				virtualPositionActive = UpdateVirtualPosition(reading);
				result.buttons.SetButtonState(0, HasButton(reading, GamepadButtons::DPadUp));
				result.buttons.SetButtonState(1, HasButton(reading, GamepadButtons::DPadDown));
				result.buttons.SetButtonState(2, HasButton(reading, GamepadButtons::DPadLeft));
				result.buttons.SetButtonState(3, HasButton(reading, GamepadButtons::DPadRight));
				result.buttons.SetButtonState(4, HasButton(reading, GamepadButtons::Menu));
				result.buttons.SetButtonState(5, HasButton(reading, GamepadButtons::View));
				result.buttons.SetButtonState(6, HasButton(reading, GamepadButtons::LeftThumbstick));
				result.buttons.SetButtonState(7, HasButton(reading, GamepadButtons::RightThumbstick));
				result.buttons.SetButtonState(8, HasButton(reading, GamepadButtons::LeftShoulder));
				result.buttons.SetButtonState(9, HasButton(reading, GamepadButtons::RightShoulder));
				result.buttons.SetButtonState(12, HasButton(reading, GamepadButtons::A));
				result.buttons.SetButtonState(13, HasButton(reading, GamepadButtons::B));
				result.buttons.SetButtonState(14, HasButton(reading, GamepadButtons::X));
				result.buttons.SetButtonState(15, HasButton(reading, GamepadButtons::Y));
				result.axis = { ClampStickValue(reading.LeftThumbstickX), ClampStickValue(reading.LeftThumbstickY) };
				result.rotation = { ClampStickValue(reading.RightThumbstickX), ClampStickValue(reading.RightThumbstickY) };
				result.trigger = { ClampTriggerValue(reading.LeftTrigger), ClampTriggerValue(reading.RightTrigger) };
			}
			catch (const winrt::hresult_error&)
			{
				MarkDisconnectedLocked();
			}
		}
		connected = m_gamepad != nullptr;
	}
	if (!connected)
	{
		DeactivateVirtualPosition();
	}

	if (virtualPositionActive)
	{
		const bool useLeftStick = virtual_position_stick() == VirtualPositionStick::Left;
		// the stick click only drives the pointer while it is the activation button
		if (virtual_position_activation() == VirtualPositionActivation::HoldStickButton)
			result.buttons.SetButtonState(useLeftStick ? 6 : 7, false);
		if (useLeftStick)
			result.axis = {};
		else
			result.rotation = {};
	}
	return result;
}

#endif
