#pragma once

#if defined(CEMU_UWP)

#include "input/api/Controller.h"
#include "input/api/WGI/WGIGamepadControllerProvider.h"

#include <chrono>
#include <mutex>

class WGIGamepadController : public Controller<WGIGamepadControllerProvider>
{
public:
	enum class Binding : uint8
	{
		Gamepad, // takes on the identity of the gamepad found at the position
		Position, // follows whichever gamepad is at the position in the system list
	};

	explicit WGIGamepadController(uint32 index, Binding binding = Binding::Gamepad);
	WGIGamepadController(std::string_view uuid, std::string_view displayName);

	std::string_view api_name() const override
	{
		return to_string(InputAPI::WGIGamepad);
	}
	InputAPI::Type api() const override { return InputAPI::WGIGamepad; }

	bool connect() override;
	bool is_connected() override;
	bool has_motion() override;
	MotionSample get_motion_sample() override;
	bool has_position() override;
	glm::vec2 get_position() override;
	glm::vec2 get_prev_position() override;
	PositionVisibility GetPositionVisibility() override;
	void save(pugi::xml_node& node) override;
	void load(const pugi::xml_node& node) override;

	enum class VirtualPositionStick : uint8
	{
		Right,
		Left,
	};
	enum class VirtualPositionActivation : uint8
	{
		HoldStickButton,
		Always,
	};

	bool virtual_position_enabled() const;
	void set_virtual_position_enabled(bool enabled);
	float virtual_position_speed() const;
	void set_virtual_position_speed(float speed);
	float virtual_position_deadzone() const;
	void set_virtual_position_deadzone(float deadzone);
	VirtualPositionStick virtual_position_stick() const;
	void set_virtual_position_stick(VirtualPositionStick stick);
	VirtualPositionActivation virtual_position_activation() const;
	void set_virtual_position_activation(VirtualPositionActivation activation);
	[[nodiscard]] uint32 enumeration_index() const noexcept { return m_index; }
	[[nodiscard]] const std::string& non_roamable_id() const noexcept
	{
		return m_nonRoamableId;
	}
	// the gamepad this controller first connected to this session, empty until then
	[[nodiscard]] std::string session_gamepad_id() const;
	// moves this controller to another gamepad for the rest of the session
	void use_gamepad_for_session(std::string_view nonRoamableId);

	bool has_rumble() override { return true; }
	void start_rumble() override;
	void stop_rumble() override;

	std::string get_button_name(uint64 button) const override;

protected:
	ControllerState raw_state() override;

private:
	bool TryConnectLocked(bool force);
	void SetConnectionStateLocked(bool connected);
	void MarkDisconnectedLocked();
	void DropRemovedGamepadLocked();
	bool UpdateVirtualPosition(const winrt::Windows::Gaming::Input::GamepadReading& reading);
	void DeactivateVirtualPosition();
	void AdoptStableIdentity(
		const winrt::Windows::Gaming::Input::Gamepad& gamepad);

	uint32 m_index{};
	std::string m_nonRoamableId;
	std::string m_sessionGamepadId;
	bool m_identityFrozen{};
	mutable std::mutex m_gamepadMutex;
	winrt::Windows::Gaming::Input::Gamepad m_gamepad{ nullptr };
	uint32 m_seenGamepadListVersion{};
	std::chrono::steady_clock::time_point m_nextReconnectAttempt{};
	bool m_hasLoggedConnectionState{};
	bool m_lastLoggedConnected{};

	mutable std::mutex m_virtualPositionMutex;
	glm::vec2 m_virtualPosition{ 0.5f, 0.5f };
	glm::vec2 m_previousVirtualPosition{ 0.5f, 0.5f };
	std::chrono::steady_clock::time_point m_lastVirtualPositionUpdate{};
	bool m_virtualPositionActive{};
	bool m_virtualPositionEnabled{ true };
	float m_virtualPositionSpeed{ 0.65f };
	float m_virtualPositionDeadzone{ 0.18f };
	VirtualPositionStick m_virtualPositionStick{ VirtualPositionStick::Right };
	VirtualPositionActivation m_virtualPositionActivation{ VirtualPositionActivation::HoldStickButton };
};

#endif
