#include "input/api/DSU/DSUController.h"

#if defined(CEMU_UWP)
#include "uwp/UwpFrontend.h"
#endif

#include <boost/program_options/value_semantic.hpp>

#if defined(CEMU_UWP)
namespace
{
bool IsGameplayInputBlocked()
{
	return CemuUwp::Frontend::IsGameplayInputCaptured() &&
		!CemuUwp::Frontend::IsInputMappingReadActive();
}
}
#endif

DSUController::DSUController(uint32 index)
	: base_type(fmt::format("{}", index), fmt::format("Controller {}", index + 1)), m_index(index)
{
	if (index >= DSUControllerProvider::kMaxClients)
		throw std::runtime_error(fmt::format("max {} dsu controllers are supported! given index: {}",
		                                     DSUControllerProvider::kMaxClients, index));
#if defined(CEMU_UWP)
	if (!m_provider)
		throw std::runtime_error("the configured DSU server is unavailable");
#endif
}

DSUController::DSUController(uint32 index, const DSUProviderSettings& settings)
	: base_type(
#if defined(CEMU_UWP)
		fmt::format("{}:{}", settings.endpoint_id.empty() ?
			fmt::format("{}:{}", settings.ip, settings.port) : settings.endpoint_id, index),
		fmt::format("{} - Controller {}", settings.display_name.empty() ?
			settings.ip : settings.display_name, index + 1),
#else
		fmt::format("{}", index), fmt::format("Controller {}", index + 1),
#endif
		settings), m_index(index)
{
	if (index >= DSUControllerProvider::kMaxClients)
		throw std::runtime_error(fmt::format("max {} dsu controllers are supported! given index: {}",
		                                     DSUControllerProvider::kMaxClients, index));
#if defined(CEMU_UWP)
	if (!m_provider)
		throw std::runtime_error("the configured DSU server is unavailable");
#endif
}

void DSUController::save(pugi::xml_node& node)
{
	base_type::save(node);

	node.append_child("ip").append_child(pugi::node_pcdata).set_value(
		fmt::format("{}", m_provider->get_settings().ip).c_str());
	node.append_child("port").append_child(pugi::node_pcdata).set_value(
		fmt::format("{}", m_provider->get_settings().port).c_str());
#if defined(CEMU_UWP)
	node.append_child("endpoint_id").append_child(pugi::node_pcdata).set_value(
		m_provider->get_settings().endpoint_id.c_str());
	node.append_child("endpoint_name").append_child(pugi::node_pcdata).set_value(
		m_provider->get_settings().display_name.c_str());
	if (m_route_managed.load(std::memory_order_relaxed))
	{
		auto route = node.append_child("route_features");
		route.append_attribute("gamepad") = m_gamepad_enabled.load(std::memory_order_relaxed);
		route.append_attribute("motion") = m_motion_enabled.load(std::memory_order_relaxed);
		route.append_attribute("touch") = m_touch_enabled.load(std::memory_order_relaxed);
	}
#endif
}

void DSUController::load(const pugi::xml_node& node)
{
	base_type::load(node);

	DSUProviderSettings settings;
	if (const auto value = node.child("ip"))
		settings.ip = value.child_value();
	if (const auto value = node.child("port"))
		settings.port = ConvertString<uint16>(value.child_value());
#if defined(CEMU_UWP)
	if (const auto value = node.child("endpoint_id"))
		settings.endpoint_id = value.child_value();
	if (const auto value = node.child("endpoint_name"))
		settings.display_name = value.child_value();
	const std::string endpointIdentity = settings.endpoint_id.empty() ?
		fmt::format("{}:{}", settings.ip, settings.port) : settings.endpoint_id;
	m_uuid = fmt::format("{}:{}", endpointIdentity, m_index);
	m_display_name = fmt::format("{} - Controller {}",
		settings.display_name.empty() ? settings.ip : settings.display_name,
		m_index + 1);
	if (const auto route = node.child("route_features"))
	{
		m_route_managed.store(true, std::memory_order_relaxed);
		m_gamepad_enabled.store(route.attribute("gamepad").as_bool(true),
			std::memory_order_relaxed);
		m_motion_enabled.store(route.attribute("motion").as_bool(true),
			std::memory_order_relaxed);
		m_touch_enabled.store(route.attribute("touch").as_bool(true),
			std::memory_order_relaxed);
	}
#endif

#if defined(CEMU_UWP)
	if (!rebind_provider(settings))
		throw std::runtime_error("the controller profile references a disabled or missing DSU server");
#else
	const auto provider = InputManager::instance().get_api_provider(api(), settings);
	update_provider(std::dynamic_pointer_cast<DSUControllerProvider>(provider));
	connect();
#endif
}

#if defined(CEMU_UWP)
bool DSUController::rebind_provider(const DSUProviderSettings& settings) noexcept
{
	try
	{
		const auto provider = std::dynamic_pointer_cast<DSUControllerProvider>(
			InputManager::instance().get_api_provider(api(), settings));
		if (!provider)
			return false;
		update_provider(provider);
		const auto& active = provider->get_settings();
		const std::string endpointIdentity = active.endpoint_id.empty() ?
			fmt::format("{}:{}", active.ip, active.port) : active.endpoint_id;
		m_uuid = fmt::format("{}:{}", endpointIdentity, m_index);
		m_display_name = fmt::format("{} - Controller {}",
			active.display_name.empty() ? active.ip : active.display_name,
			m_index + 1);
		(void)connect();
		return true;
	}
	catch (const std::exception&)
	{
		return false;
	}
}
#endif

void DSUController::configure_route_features(bool gamepad, bool motion, bool touch) noexcept
{
	configure_runtime_features(gamepad, motion, touch);
	m_route_managed.store(true, std::memory_order_relaxed);
}

void DSUController::configure_runtime_features(bool gamepad, bool motion, bool touch) noexcept
{
#if defined(CEMU_UWP)
	if (!m_runtime_feature_override)
	{
		m_saved_use_motion = get_settings().motion;
		m_runtime_feature_override = true;
	}
#endif
	m_gamepad_enabled.store(gamepad, std::memory_order_relaxed);
	m_motion_enabled.store(motion, std::memory_order_relaxed);
	m_touch_enabled.store(touch, std::memory_order_relaxed);
	set_use_motion(motion);
}

void DSUController::reset_runtime_features() noexcept
{
	if (m_route_managed.load(std::memory_order_relaxed))
		return;
#if defined(CEMU_UWP)
	if (m_runtime_feature_override)
	{
		set_use_motion(m_saved_use_motion);
		m_runtime_feature_override = false;
	}
#endif
	m_gamepad_enabled.store(true, std::memory_order_relaxed);
	m_motion_enabled.store(true, std::memory_order_relaxed);
	m_touch_enabled.store(true, std::memory_order_relaxed);
}

bool DSUController::connect()
{
	if (is_connected())
		return true;

	m_provider->request_pad_data(m_index);
	return is_connected();
}

bool DSUController::is_connected()
{
	return m_provider->is_connected(m_index);
}

MotionSample DSUController::get_motion_sample()
{
	if (!m_motion_enabled.load(std::memory_order_relaxed)
#if defined(CEMU_UWP)
		|| IsGameplayInputBlocked() || !is_connected()
#endif
	)
		return {};
	return m_provider->get_motion_sample(m_index);
}

bool DSUController::has_position()
{
	if (!m_touch_enabled.load(std::memory_order_relaxed)
#if defined(CEMU_UWP)
		|| IsGameplayInputBlocked() || !is_connected()
#endif
	)
		return false;
	const auto state = m_provider->get_state(m_index);
	return state.data.tpad1.active || state.data.tpad2.active;
}

glm::vec2 DSUController::get_position()
{
	if (!m_touch_enabled.load(std::memory_order_relaxed)
#if defined(CEMU_UWP)
		|| IsGameplayInputBlocked() || !is_connected()
#endif
	)
		return {};
	// touchpad resolution is 1920x942
	const auto state = m_provider->get_state(m_index);
	if (state.data.tpad1.active)
		return glm::vec2{(float)state.data.tpad1.x / 1920.0f, (float)state.data.tpad1.y / 942.0f};

	if (state.data.tpad2.active)
		return glm::vec2{(float)state.data.tpad2.x / 1920.0f, (float)state.data.tpad2.y / 942.0f};

	return {};
}

glm::vec2 DSUController::get_prev_position()
{
	if (!m_touch_enabled.load(std::memory_order_relaxed)
#if defined(CEMU_UWP)
		|| IsGameplayInputBlocked() || !is_connected()
#endif
	)
		return {};
	const auto state = m_provider->get_prev_state(m_index);
	if (state.data.tpad1.active)
		return glm::vec2{(float)state.data.tpad1.x / 1920.0f, (float)state.data.tpad1.y / 942.0f};

	if (state.data.tpad2.active)
		return glm::vec2{(float)state.data.tpad2.x / 1920.0f, (float)state.data.tpad2.y / 942.0f};

	return {};
}

PositionVisibility DSUController::GetPositionVisibility()
{
	if (!m_touch_enabled.load(std::memory_order_relaxed)
#if defined(CEMU_UWP)
		|| IsGameplayInputBlocked() || !is_connected()
#endif
	)
		return PositionVisibility::NONE;
#if defined(CEMU_UWP)
	const auto state = m_provider->get_state(m_index);
#else
	const auto state = m_provider->get_prev_state(m_index);
#endif

	return (state.data.tpad1.active || state.data.tpad2.active) ? PositionVisibility::FULL : PositionVisibility::NONE;
}

#if defined(CEMU_UWP)
bool DSUController::get_touch_position(uint32 index, glm::vec2& position)
{
	position = {};
	if (!m_touch_enabled.load(std::memory_order_relaxed) ||
		IsGameplayInputBlocked() || !is_connected() || index > 1)
		return false;
	const auto state = m_provider->get_state(m_index);
	const auto& touch = index == 0 ? state.data.tpad1 : state.data.tpad2;
	if (!touch.active)
		return false;
	position = {
		std::clamp(static_cast<float>(touch.x) / 1920.0f, 0.0f, 1.0f),
		std::clamp(static_cast<float>(touch.y) / 942.0f, 0.0f, 1.0f),
	};
	return true;
}
#endif

std::string DSUController::get_button_name(uint64 button) const
{
	switch (button)
	{
	case kButton0: return "Share";
	case kButton1: return "Stick L";
	case kButton2: return "Stick R";
	case kButton3: return "Options";
	case kButton4: return "Up";
	case kButton5: return "Right";
	case kButton6: return "Down";
	case kButton7: return "Left";

	case kButton8: return "ZL";
	case kButton9: return "ZR";
	case kButton10: return "L";
	case kButton11: return "R";
	case kButton12: return "Triangle";
	case kButton13: return "Circle";
	case kButton14: return "Cross";
	case kButton15: return "Square";

	case kButton16: return "Touch";
#if defined(CEMU_UWP)
	case kButton17: return "Home";
#endif
	case kAxisXP: return "Left Stick Right";
	case kAxisXN: return "Left Stick Left";
	case kAxisYP: return "Left Stick Up";
	case kAxisYN: return "Left Stick Down";
	case kRotationXP: return "Right Stick Right";
	case kRotationXN: return "Right Stick Left";
	case kRotationYP: return "Right Stick Up";
	case kRotationYN: return "Right Stick Down";
	case kTriggerXP: return "L2 Analog";
	case kTriggerYP: return "R2 Analog";
	}
	return base_type::get_button_name(button);
}

ControllerState DSUController::raw_state()
{
	ControllerState result{};
	if (!m_gamepad_enabled.load(std::memory_order_relaxed)
#if defined(CEMU_UWP)
		|| IsGameplayInputBlocked()
#endif
	)
		return result;

	if (!is_connected())
		return result;

	const auto state = m_provider->get_state(m_index);
	// didn't read any data from the controller yet
	if (state.info.state != DsState::Connected)
		return result;

	int bitindex = 0;
	for (int i = 0; i < 8; ++i, ++bitindex)
	{
		if (HAS_BIT(state.data.state1, i))
		{
			result.buttons.SetButtonState(bitindex, true);
		}
	}

	for (int i = 0; i < 8; ++i, ++bitindex)
	{
		if (HAS_BIT(state.data.state2, i))
		{
			result.buttons.SetButtonState(bitindex, true);
		}
	}

	if (state.data.touch)
		result.buttons.SetButtonState(kButton16, true);
#if defined(CEMU_UWP)
	if (state.data.ps)
		result.buttons.SetButtonState(kButton17, true);
#endif

	result.axis.x = (float)state.data.lx / std::numeric_limits<uint8>::max();
	result.axis.x = (result.axis.x * 2.0f) - 1.0f;

	result.axis.y = (float)state.data.ly / std::numeric_limits<uint8>::max();
	result.axis.y = (result.axis.y * 2.0f) - 1.0f;

	result.rotation.x = (float)state.data.rx / std::numeric_limits<uint8>::max();
	result.rotation.x = (result.rotation.x * 2.0f) - 1.0f;

	result.rotation.y = (float)state.data.ry / std::numeric_limits<uint8>::max();
	result.rotation.y = (result.rotation.y * 2.0f) - 1.0f;

    result.trigger.x = (float)state.data.l2 / std::numeric_limits<uint8>::max();
    result.trigger.y = (float)state.data.r2 / std::numeric_limits<uint8>::max();

    return result;
}
