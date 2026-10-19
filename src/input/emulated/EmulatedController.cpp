#include "input/emulated/EmulatedController.h"

#include "input/api/Controller.h"

#include <algorithm>
#include <cmath>
#include <limits>

#if defined(CEMU_UWP)
#include "UwpImGuiFrontend/InputMapping.h"
#include "input/api/WGI/WGIGamepadController.h"
#if HAS_DSU
#include "input/api/DSU/DSUController.h"
#endif
#include <charconv>

namespace
{
class CemuExpressionValues final : public UwpImGuiFrontend::IInputValueSource
{
public:
	explicit CemuExpressionValues(
		const std::vector<std::shared_ptr<ControllerBase>>& controllers)
		: m_controllers(controllers) {}

	float ReadInput(std::string_view reference) const override
	{
		std::string_view device;
		std::string_view control = reference;
		if (const std::size_t separator = reference.find_last_of('/');
			separator != std::string_view::npos)
		{
			device = reference.substr(0, separator);
			control = reference.substr(separator + 1);
		}

		for (const auto& controller : m_controllers)
		{
			const std::string qualifiedDevice = fmt::format("{}:{}",
				controller->api_name(), controller->uuid());
			if (!device.empty() && controller->uuid() != device && qualifiedDevice != device)
			{
				bool legacyWgiMatch = false;
				if (const auto wgi = std::dynamic_pointer_cast<WGIGamepadController>(controller);
					wgi && wgi->enumeration_index() != std::numeric_limits<uint32>::max())
				{
					legacyWgiMatch = device == fmt::format("{}:{}",
						controller->api_name(), wgi->enumeration_index());
				}
				const std::size_t slotSeparator = controller->uuid().find_last_of(':');
				const std::string legacyDevice = controller->api() != InputAPI::DSUClient ||
					slotSeparator == std::string::npos ?
					std::string{} : fmt::format("{}:{}", controller->api_name(),
						controller->uuid().substr(slotSeparator + 1));
				if (!legacyWgiMatch && device != legacyDevice)
					continue;
			}
			if (control == "Pointer Press")
				return controller->has_position() ? 1.0f : 0.0f;
			if (control == "Pointer X" || control == "Pointer Y")
			{
				if (!controller->has_position())
					continue;
				const glm::vec2 position = controller->get_position();
				return control == "Pointer X" ? position.x : position.y;
			}
#if HAS_DSU
			if (control.starts_with("Touch "))
			{
				const auto dsu = std::dynamic_pointer_cast<DSUController>(controller);
				if (!dsu)
					continue;
				const std::size_t touchIndex = control.size() > 6 && control[6] == '2' ? 1 : 0;
				glm::vec2 position{};
				const bool active = dsu->get_touch_position(
					static_cast<uint32>(touchIndex), position);
				if (control.ends_with("Press"))
					return active ? 1.0f : 0.0f;
				if (!active)
					return 0.0f;
				return control.ends_with(" X") ? position.x : position.y;
			}
#endif
			if (control.starts_with("Accel ") || control.starts_with("Gyro "))
			{
				if (!controller->use_motion())
					continue;
				float values[3]{};
				auto sample = controller->get_motion_sample();
				if (control.starts_with("Accel "))
					sample.getAccelerometer(values);
				else
					sample.getGyrometer(values);
				const char axis = control.back();
				const std::size_t index = axis == 'Y' ? 1 : axis == 'Z' ? 2 : 0;
				return values[index];
			}
			uint64 button = 0;
			const auto [end, error] = std::from_chars(control.data(),
				control.data() + control.size(), button);
			if (error == std::errc{} && end == control.data() + control.size())
				return controller->get_axis_value(button);
			for (button = 0; button < kButtonMAX; ++button)
			{
				if (controller->get_button_name(button) == control)
					return controller->get_axis_value(button);
			}
		}
		return 0.0f;
	}

private:
	const std::vector<std::shared_ptr<ControllerBase>>& m_controllers;
};

}

class EmulatedController::MappingExpressionHolder
{
public:
	UwpImGuiFrontend::MappingExpression expression;
};
#endif

#ifdef SUPPORTS_WIIMOTE
#include "input/api/Wiimote/NativeWiimoteController.h"
#endif

std::string_view EmulatedController::type_to_string(Type type)
{
	switch (type)
	{
	case VPAD: return "Wii U GamePad";
	case Pro: return "Wii U Pro Controller";
	case Classic: return "Wii U Classic Controller";
	case Wiimote: return "Wiimote";
	}

	throw std::runtime_error(fmt::format("unknown emulated controller: {}", stdx::to_underlying(type)));
}

EmulatedController::Type EmulatedController::type_from_string(std::string_view str)
{
	if (str == "Wii U GamePad")
		return VPAD;
	else if (str == "Wii U Pro Controller")
		return Pro;
	else if (str == "Wii U Classic Controller")
		return Classic;
	else if (str == "Wiimote")
		return Wiimote;

	throw std::runtime_error(fmt::format("unknown emulated controller: {}", str));
}

EmulatedController::EmulatedController(size_t player_index)
	: m_player_index(player_index)
{
}

void EmulatedController::load(const pugi::xml_node& node)
{
#if defined(CEMU_UWP)
	MotionMappingSettings settings;
	if (const auto motion = node.child("uwp_motion"))
	{
		settings.accelerometer_sensitivity = motion.child("accelerometer_sensitivity")
			.text().as_float(settings.accelerometer_sensitivity);
		settings.gyroscope_sensitivity = motion.child("gyroscope_sensitivity")
			.text().as_float(settings.gyroscope_sensitivity);
		settings.gyroscope_deadzone_degrees = motion.child("gyroscope_deadzone_degrees")
			.text().as_float(settings.gyroscope_deadzone_degrees);
		settings.gyroscope_calibration_period = motion.child("gyroscope_calibration_period")
			.text().as_float(settings.gyroscope_calibration_period);
		settings.pointer_horizontal_sensitivity = motion.child("pointer_horizontal_sensitivity")
			.text().as_float(settings.pointer_horizontal_sensitivity);
		settings.pointer_vertical_sensitivity = motion.child("pointer_vertical_sensitivity")
			.text().as_float(settings.pointer_vertical_sensitivity);
		settings.invert_pointer_x = motion.child("invert_pointer_x")
			.text().as_bool(settings.invert_pointer_x);
		settings.invert_pointer_y = motion.child("invert_pointer_y")
			.text().as_bool(settings.invert_pointer_y);
	}
	set_motion_mapping_settings(settings);
#else
	(void)node;
#endif
}

void EmulatedController::save(pugi::xml_node& node)
{
#if defined(CEMU_UWP)
	node.remove_child("uwp_motion");
	const MotionMappingSettings settings = get_motion_mapping_settings();
	auto motion = node.append_child("uwp_motion");
	motion.append_child("accelerometer_sensitivity").text().set(
		settings.accelerometer_sensitivity);
	motion.append_child("gyroscope_sensitivity").text().set(
		settings.gyroscope_sensitivity);
	motion.append_child("gyroscope_deadzone_degrees").text().set(
		settings.gyroscope_deadzone_degrees);
	motion.append_child("gyroscope_calibration_period").text().set(
		settings.gyroscope_calibration_period);
	motion.append_child("pointer_horizontal_sensitivity").text().set(
		settings.pointer_horizontal_sensitivity);
	motion.append_child("pointer_vertical_sensitivity").text().set(
		settings.pointer_vertical_sensitivity);
	motion.append_child("invert_pointer_x").text().set(settings.invert_pointer_x);
	motion.append_child("invert_pointer_y").text().set(settings.invert_pointer_y);
#else
	(void)node;
#endif
}

void EmulatedController::calibrate()
{
	std::shared_lock lock(m_mutex);
	for (const auto& controller : m_controllers)
	{
		controller->calibrate();
	}
}

void EmulatedController::connect()
{
	std::shared_lock lock(m_mutex);
	for (const auto& controller : m_controllers)
	{
		controller->connect();
	}
}

void EmulatedController::update()
{
	std::shared_lock lock(m_mutex);
	for(const auto& controller : m_controllers)
	{
		controller->update();
	}
}

void EmulatedController::controllers_update_states()
{
	std::shared_lock lock(m_mutex);
	for (const auto& controller : m_controllers)
	{
		controller->update_state();
	}
}

void EmulatedController::start_rumble()
{
	m_rumble = true;
	std::shared_lock lock(m_mutex);
	for (const auto& controller : m_controllers)
	{
		controller->start_rumble();
	}
}

void EmulatedController::stop_rumble()
{
	if (!m_rumble)
		return;

	m_rumble = false;

	std::shared_lock lock(m_mutex);
	for (const auto& controller : m_controllers)
	{
		controller->stop_rumble();
	}
}

bool EmulatedController::is_battery_low() const
{
	std::shared_lock lock(m_mutex);
	return std::any_of(m_controllers.cbegin(), m_controllers.cend(), [](const auto& c) {return c->has_low_battery(); });
}

bool EmulatedController::has_motion() const
{
#if defined(CEMU_UWP)
	if (has_active_expression_mapping(kExpressionAccelX) ||
		has_active_expression_mapping(kExpressionAccelY) ||
		has_active_expression_mapping(kExpressionAccelZ) ||
		has_active_expression_mapping(kExpressionGyroX) ||
		has_active_expression_mapping(kExpressionGyroY) ||
		has_active_expression_mapping(kExpressionGyroZ))
		return true;
	{
		std::shared_lock lock(m_mutex);
		if (const auto controller = m_route_motion_controller.lock())
			return controller->use_motion();
	}
#endif
	std::shared_lock lock(m_mutex);
	return std::any_of(m_controllers.cbegin(), m_controllers.cend(), [](const auto& c) {return c->use_motion(); });
}

MotionSample EmulatedController::get_motion_data() const
{
#if defined(CEMU_UWP)
	const bool usesExpressions = has_active_expression_mapping(kExpressionAccelX) ||
		has_active_expression_mapping(kExpressionAccelY) ||
		has_active_expression_mapping(kExpressionAccelZ) ||
		has_active_expression_mapping(kExpressionGyroX) ||
		has_active_expression_mapping(kExpressionGyroY) ||
		has_active_expression_mapping(kExpressionGyroZ);
	std::shared_ptr<ControllerBase> nativeExpressionSource;
	if (usesExpressions)
	{
		std::shared_lock controllerLock(m_mutex);
		std::scoped_lock expressionLock(m_expression_mutex);
		for (const auto& controller : m_controllers)
		{
			if (!controller || !controller->use_motion())
				continue;
			const std::string prefix = fmt::format("{}:{}/", controller->api_name(),
				controller->uuid());
			const bool isDefaultController = !m_controllers.empty() &&
				m_controllers.front() == controller;
			auto isDirect = [&](uint64 mapping, std::string_view control) {
				const std::string expression = effective_expression_locked(mapping);
				return expression == UwpImGuiFrontend::EscapeInputControlReference(
					prefix + std::string(control)) ||
					(isDefaultController && expression ==
						UwpImGuiFrontend::EscapeInputControlReference(control));
			};
			if (isDirect(kExpressionAccelX, "Accel X") &&
				isDirect(kExpressionAccelY, "Accel Y") &&
				isDirect(kExpressionAccelZ, "Accel Z") &&
				isDirect(kExpressionGyroX, "Gyro X") &&
				isDirect(kExpressionGyroY, "Gyro Y") &&
				isDirect(kExpressionGyroZ, "Gyro Z"))
			{
				nativeExpressionSource = controller;
				break;
			}
		}
	}
	if (usesExpressions)
	{
		if (!nativeExpressionSource)
		{
			std::shared_lock controllerLock(m_mutex);
			std::scoped_lock motionLock(m_expression_motion_mutex);
			const auto now = std::chrono::steady_clock::now();
			float deltaSeconds = 1.0f / 120.0f;
			if (m_expression_motion_timestamp.time_since_epoch().count() != 0)
			{
				deltaSeconds = std::clamp(
					std::chrono::duration<float>(now - m_expression_motion_timestamp).count(),
					1.0f / 1000.0f, 0.05f);
			}
			m_expression_motion_timestamp = now;
			float gyroscope[3]{
				evaluate_expression(kExpressionGyroX),
				evaluate_expression(kExpressionGyroY),
				evaluate_expression(kExpressionGyroZ),
			};
			float accelerometer[3]{
				evaluate_expression(kExpressionAccelX),
				evaluate_expression(kExpressionAccelY),
				evaluate_expression(kExpressionAccelZ),
			};
			apply_motion_settings_locked(gyroscope, accelerometer, now);
			m_expression_motion_handler.processMotionSample(deltaSeconds,
				gyroscope[0], gyroscope[1], gyroscope[2],
				accelerometer[0], accelerometer[1], accelerometer[2]);
			return m_expression_motion_handler.getMotionSample();
		}
	}

	MotionSample source;
	bool hasSource = nativeExpressionSource != nullptr;
	if (nativeExpressionSource)
		source = nativeExpressionSource->get_motion_sample();
	{
		std::shared_lock lock(m_mutex);
		if (!hasSource)
		{
			if (const auto controller = m_route_motion_controller.lock();
				controller && controller->use_motion())
			{
				source = controller->get_motion_sample();
				hasSource = true;
			}
			else
			{
				for (const auto& controller : m_controllers)
				{
					if (!controller->use_motion())
						continue;
					source = controller->get_motion_sample();
					hasSource = true;
					break;
				}
			}
		}
	}
	if (!hasSource)
		return {};

	const auto now = std::chrono::steady_clock::now();
	float accelerometer[3]{};
	float gyroscope[3]{};
	source.getAccelerometer(accelerometer);
	source.getGyrometer(gyroscope);
	{
		std::scoped_lock motionLock(m_expression_motion_mutex);
		const auto& settings = m_motion_mapping_settings;
		const bool passthrough = settings.accelerometer_sensitivity == 1.0f &&
			settings.gyroscope_sensitivity == 1.0f &&
			settings.gyroscope_deadzone_degrees == 0.0f &&
			settings.gyroscope_calibration_period == 0.0f;
		if (passthrough)
			return source;
		float deltaSeconds = 1.0f / 120.0f;
		if (m_expression_motion_timestamp.time_since_epoch().count() != 0)
		{
			deltaSeconds = std::clamp(
				std::chrono::duration<float>(now - m_expression_motion_timestamp).count(),
				1.0f / 1000.0f, 0.05f);
		}
		m_expression_motion_timestamp = now;
		apply_motion_settings_locked(gyroscope, accelerometer, now);
		m_expression_motion_handler.processMotionSample(deltaSeconds,
			gyroscope[0], gyroscope[1], gyroscope[2],
			accelerometer[0], accelerometer[1], accelerometer[2]);
		return m_expression_motion_handler.getMotionSample();
	}
#endif

#if !defined(CEMU_UWP)
	std::shared_lock lock(m_mutex);
	for (const auto& controller : m_controllers)
	{
		if (controller->use_motion())
			return controller->get_motion_sample();
	}

	return {};
#endif
}

bool EmulatedController::has_second_motion() const
{
	int motion = 0;
	std::shared_lock lock(m_mutex);
	for(const auto& controller : m_controllers)
	{
		if(controller->use_motion())
		{
			// if wiimote has nunchuck connected, we use its acceleration
            #if SUPPORTS_WIIMOTE
            if(controller->api() == InputAPI::Wiimote)
			{
				if(((NativeWiimoteController*)controller.get())->get_extension() == NativeWiimoteController::Nunchuck)
				{
					return true;
				}
			}
            #endif
			motion++;
		}
	}

	return motion >= 2;
}

MotionSample EmulatedController::get_second_motion_data() const
{
	int motion = 0;
	std::shared_lock lock(m_mutex);
	for (const auto& controller : m_controllers)
	{
		if (controller->use_motion())
		{
			// if wiimote has nunchuck connected, we use its acceleration
            #ifdef SUPPORTS_WIIMOTE
			if (controller->api() == InputAPI::Wiimote)
			{
				if (((NativeWiimoteController*)controller.get())->get_extension() == NativeWiimoteController::Nunchuck)
				{
					return ((NativeWiimoteController*)controller.get())->get_nunchuck_motion_sample();
				}
			}
			#endif

			motion++;
			if(motion == 2)
			{
				return controller->get_motion_sample();
			}
		}
	}

	return {};
}

bool EmulatedController::has_position() const
{
#if defined(CEMU_UWP)
	if (has_active_expression_mapping(kExpressionPointerPress))
	{
		std::shared_lock lock(m_mutex);
		return evaluate_expression(kExpressionPointerPress) >= 0.5f;
	}
	if (has_active_expression_mapping(kExpressionPointerX) ||
		has_active_expression_mapping(kExpressionPointerY))
		return true;
	{
		std::shared_lock lock(m_mutex);
		if (const auto controller = m_route_pointer_controller.lock())
			return controller->has_position();
	}
#endif
	std::shared_lock lock(m_mutex);
	return std::any_of(m_controllers.cbegin(), m_controllers.cend(), [](const auto& c) {return c->has_position(); });
}

glm::vec2 EmulatedController::get_position() const
{
#if defined(CEMU_UWP)
	if (has_active_expression_mapping(kExpressionPointerX) ||
		has_active_expression_mapping(kExpressionPointerY))
	{
		std::shared_lock controllerLock(m_mutex);
		glm::vec2 position{
			std::clamp(evaluate_expression(kExpressionPointerX), 0.0f, 1.0f),
			std::clamp(evaluate_expression(kExpressionPointerY), 0.0f, 1.0f),
		};
		std::scoped_lock pointerLock(m_expression_motion_mutex);
		position = apply_pointer_settings_locked(position);
		m_expression_previous_pointer_position = m_expression_pointer_position;
		m_expression_pointer_position = position;
		return position;
	}
	{
		std::shared_lock lock(m_mutex);
		if (const auto controller = m_route_pointer_controller.lock())
		{
			const glm::vec2 position = controller->get_position();
			std::scoped_lock pointerLock(m_expression_motion_mutex);
			return apply_pointer_settings_locked(position);
		}
	}
#endif
	std::shared_lock lock(m_mutex);
	for (const auto& controller : m_controllers)
	{
		if (controller->has_position())
		{
#if defined(CEMU_UWP)
			const glm::vec2 position = controller->get_position();
			std::scoped_lock pointerLock(m_expression_motion_mutex);
			return apply_pointer_settings_locked(position);
#else
			return controller->get_position();
#endif
		}
	}

	return {};
}

glm::vec2 EmulatedController::get_prev_position() const
{
#if defined(CEMU_UWP)
	if (has_active_expression_mapping(kExpressionPointerX) ||
		has_active_expression_mapping(kExpressionPointerY))
	{
		std::scoped_lock pointerLock(m_expression_motion_mutex);
		return m_expression_previous_pointer_position;
	}
	{
		std::shared_lock lock(m_mutex);
		if (const auto controller = m_route_pointer_controller.lock())
		{
			const glm::vec2 position = controller->get_prev_position();
			std::scoped_lock pointerLock(m_expression_motion_mutex);
			return apply_pointer_settings_locked(position);
		}
	}
#endif
	std::shared_lock lock(m_mutex);
	for (const auto& controller : m_controllers)
	{
		if (controller->has_position())
		{
#if defined(CEMU_UWP)
			const glm::vec2 position = controller->get_prev_position();
			std::scoped_lock pointerLock(m_expression_motion_mutex);
			return apply_pointer_settings_locked(position);
#else
			return controller->get_prev_position();
#endif
		}
	}

	return {};
}

PositionVisibility EmulatedController::GetPositionVisibility() const
{
#if defined(CEMU_UWP)
	if (has_active_expression_mapping(kExpressionPointerPress))
	{
		std::shared_lock lock(m_mutex);
		return evaluate_expression(kExpressionPointerPress) >= 0.5f ?
			PositionVisibility::FULL : PositionVisibility::NONE;
	}
	if (has_active_expression_mapping(kExpressionPointerX) ||
		has_active_expression_mapping(kExpressionPointerY))
		return PositionVisibility::FULL;
	{
		std::shared_lock lock(m_mutex);
		if (const auto controller = m_route_pointer_controller.lock())
			return controller->GetPositionVisibility();
	}
#endif
	std::shared_lock lock(m_mutex);
	for (const auto& controller : m_controllers)
	{
		if (controller->has_position())
			return controller->GetPositionVisibility();
	}
	return PositionVisibility::NONE;
}

#if defined(CEMU_UWP)
std::optional<EmulatedController::PointerCursor> EmulatedController::get_pointer_cursor() const
{
	const bool touching = has_position();
	if (has_active_expression_mapping(kExpressionPointerX) ||
		has_active_expression_mapping(kExpressionPointerY))
	{
		// evaluated here rather than through get_position, which also records the previous position for the game
		std::shared_lock controllerLock(m_mutex);
		const glm::vec2 position{
			std::clamp(evaluate_expression(kExpressionPointerX), 0.0f, 1.0f),
			std::clamp(evaluate_expression(kExpressionPointerY), 0.0f, 1.0f),
		};
		std::scoped_lock pointerLock(m_expression_motion_mutex);
		return PointerCursor{ apply_pointer_settings_locked(position), touching };
	}
	if (touching)
		return PointerCursor{ get_position(), true };

	std::shared_lock lock(m_mutex);
	// a routed touchscreen has no position between touches
	if (m_route_pointer_controller.lock())
		return std::nullopt;
	// a stick driven pointer keeps its position while it is not touching
	for (const auto& controller : m_controllers)
	{
		auto* gamepad = dynamic_cast<WGIGamepadController*>(controller.get());
		if (!gamepad || !gamepad->virtual_position_enabled())
			continue;
		const glm::vec2 position = gamepad->get_position();
		std::scoped_lock pointerLock(m_expression_motion_mutex);
		return PointerCursor{ apply_pointer_settings_locked(position), false };
	}
	return std::nullopt;
}
#endif

void EmulatedController::add_controller(std::shared_ptr<ControllerBase> controller)
{
	controller->connect();

    #ifdef SUPPORTS_WIIMOTE
    if (const auto wiimote = std::dynamic_pointer_cast<NativeWiimoteController>(controller)) {
		wiimote->set_player_index(m_player_index);
	}
    #endif
	std::scoped_lock lock(m_mutex);
	m_controllers.emplace_back(std::move(controller));
}

void EmulatedController::remove_controller(const std::shared_ptr<ControllerBase>& controller)
{
	std::scoped_lock lock(m_mutex);
	const auto it = std::find(m_controllers.cbegin(), m_controllers.cend(), controller);
	if (it != m_controllers.cend())
	{
		m_controllers.erase(it);

		for(auto m = m_mappings.begin(); m != m_mappings.end();)
		{
			if(auto mc = m->second.controller.lock())
			{
				if(*mc == *controller)
				{
					m = m_mappings.erase(m);
					continue;
				}
			}

			++m;
		}
	}
}

void EmulatedController::clear_controllers()
{
	std::scoped_lock lock(m_mutex);
	m_controllers.clear();
}

float EmulatedController::get_axis_value(uint64 mapping) const
{
	std::shared_lock lock(m_mutex);
#if defined(CEMU_UWP)
	if (has_expression_mapping(mapping))
		return evaluate_expression(mapping);
#endif
	const auto it = m_mappings.find(mapping);
	if (it != m_mappings.cend())
	{
		if (const auto controller = it->second.controller.lock()) {
			return controller->get_axis_value(it->second.button);
		}
	}

	return 0;
}

bool EmulatedController::is_mapping_down(uint64 mapping) const
{
	std::shared_lock lock(m_mutex);
#if defined(CEMU_UWP)
	if (has_expression_mapping(mapping))
		return evaluate_expression(mapping) >= 0.5f;
#endif
	const auto it = m_mappings.find(mapping);
	if (it != m_mappings.cend())
	{
		if (const auto controller = it->second.controller.lock())
			return controller->get_state().buttons.GetButtonState(
				static_cast<uint32>(it->second.button));
	}
	return false;
}


std::string EmulatedController::get_mapping_name(uint64 mapping) const
{
	std::shared_lock lock(m_mutex);
#if defined(CEMU_UWP)
	{
		std::scoped_lock expressionLock(m_expression_mutex);
		if (const auto expression = m_expression_sources.find(mapping);
			expression != m_expression_sources.end())
			return expression->second;
	}
#endif
	const auto it = m_mappings.find(mapping);
	if (it != m_mappings.cend())
	{
		if (const auto controller = it->second.controller.lock()) {
			return controller->get_button_name(it->second.button);
		}
	}

	return {};
}

std::shared_ptr<ControllerBase> EmulatedController::get_mapping_controller(uint64 mapping) const
{
	std::shared_lock lock(m_mutex);
	const auto it = m_mappings.find(mapping);
	if (it != m_mappings.cend())
	{
		if (const auto controller = it->second.controller.lock()) {
			return controller;
		}
	}

	return {};
}

void EmulatedController::delete_mapping(uint64 mapping)
{
	std::scoped_lock lock(m_mutex);
	m_mappings.erase(mapping);
#if defined(CEMU_UWP)
	std::scoped_lock expressionLock(m_expression_mutex);
	m_expression_sources.erase(mapping);
	recompile_expression_locked(mapping);
#endif
}

void EmulatedController::clear_mappings()
{
	std::scoped_lock lock(m_mutex);
	m_mappings.clear();
#if defined(CEMU_UWP)
	std::scoped_lock expressionLock(m_expression_mutex);
	m_expression_sources.clear();
	m_runtime_expression_sources.clear();
	m_compiled_expressions.clear();
#endif
}

void EmulatedController::set_mapping(uint64 mapping, const std::shared_ptr<ControllerBase>& controller,
                                     uint64 button)
{
	std::scoped_lock lock(m_mutex);
	m_mappings[mapping] = { controller, button };
#if defined(CEMU_UWP)
	const std::string expression = make_control_reference(*controller, button);
	std::scoped_lock expressionLock(m_expression_mutex);
	m_expression_sources[mapping] = expression;
	recompile_expression_locked(mapping);
#endif
}

#if defined(CEMU_UWP)
std::string EmulatedController::make_control_reference(
	const ControllerBase& controller, uint64 button) const
{
	return UwpImGuiFrontend::EscapeInputControlReference(
		fmt::format("{}:{}/{}", controller.api_name(), controller.uuid(), button));
}

void EmulatedController::set_expression_mapping(uint64 mapping, std::string expression)
{
	std::scoped_lock lock(m_expression_mutex);
	m_expression_sources[mapping] = std::move(expression);
	recompile_expression_locked(mapping);
}

std::string EmulatedController::get_expression_mapping(uint64 mapping) const
{
	std::scoped_lock lock(m_expression_mutex);
	const auto expression = m_expression_sources.find(mapping);
	return expression == m_expression_sources.end() ? std::string{} : expression->second;
}

std::unordered_map<uint64, std::string> EmulatedController::get_expression_mappings() const
{
	std::scoped_lock lock(m_expression_mutex);
	return m_expression_sources;
}

bool EmulatedController::has_expression_mapping(uint64 mapping) const
{
	std::scoped_lock lock(m_expression_mutex);
	return m_expression_sources.contains(mapping) ||
		m_runtime_expression_sources.contains(mapping);
}

bool EmulatedController::has_active_expression_mapping(uint64 mapping) const
{
	std::scoped_lock lock(m_expression_mutex);
	return !effective_expression_locked(mapping).empty();
}

std::string EmulatedController::effective_expression_locked(uint64 mapping) const
{
	const auto profile = m_expression_sources.find(mapping);
	const auto runtime = m_runtime_expression_sources.find(mapping);
	if (runtime == m_runtime_expression_sources.end())
		return profile == m_expression_sources.end() ? std::string{} : profile->second;
	if (runtime->second.replaceProfileMapping || profile == m_expression_sources.end() ||
		profile->second.empty())
		return runtime->second.expression;
	if (runtime->second.expression.empty())
		return profile->second;
	return fmt::format("({}) | ({})", profile->second, runtime->second.expression);
}

void EmulatedController::recompile_expression_locked(uint64 mapping)
{
	const std::string expression = effective_expression_locked(mapping);
	if (expression.empty())
	{
		m_compiled_expressions.erase(mapping);
		return;
	}
	auto compiled = std::make_shared<MappingExpressionHolder>();
	(void)compiled->expression.Compile(expression);
	m_compiled_expressions.insert_or_assign(mapping, std::move(compiled));
}

void EmulatedController::set_runtime_expression_mapping(uint64 mapping,
	std::string expression, bool replaceProfileMapping)
{
	std::scoped_lock lock(m_expression_mutex);
	m_runtime_expression_sources.insert_or_assign(mapping,
		RuntimeExpressionMapping{ std::move(expression), replaceProfileMapping });
	recompile_expression_locked(mapping);
}

void EmulatedController::clear_runtime_expression_mappings()
{
	std::scoped_lock lock(m_expression_mutex);
	std::vector<uint64> mappings;
	mappings.reserve(m_runtime_expression_sources.size());
	for (const auto& [mapping, expression] : m_runtime_expression_sources)
		mappings.push_back(mapping);
	m_runtime_expression_sources.clear();
	for (const uint64 mapping : mappings)
		recompile_expression_locked(mapping);
}

void EmulatedController::set_route_motion_controller(
	const std::shared_ptr<ControllerBase>& controller)
{
	std::scoped_lock lock(m_mutex);
	m_route_motion_controller = controller;
}

void EmulatedController::set_route_pointer_controller(
	const std::shared_ptr<ControllerBase>& controller)
{
	std::scoped_lock lock(m_mutex);
	m_route_pointer_controller = controller;
}

void EmulatedController::clear_route_controllers()
{
	std::scoped_lock lock(m_mutex);
	m_route_motion_controller.reset();
	m_route_pointer_controller.reset();
}

void EmulatedController::set_motion_mapping_settings(
	const MotionMappingSettings& requested)
{
	auto finiteOr = [](float value, float fallback) {
		return std::isfinite(value) ? value : fallback;
	};
	MotionMappingSettings settings = requested;
	settings.accelerometer_sensitivity = std::clamp(
		finiteOr(settings.accelerometer_sensitivity, 1.0f), 0.25f, 3.0f);
	settings.gyroscope_sensitivity = std::clamp(
		finiteOr(settings.gyroscope_sensitivity, 1.0f), 0.25f, 3.0f);
	settings.gyroscope_deadzone_degrees = std::clamp(
		finiteOr(settings.gyroscope_deadzone_degrees, 0.0f), 0.0f, 180.0f);
	settings.gyroscope_calibration_period = std::clamp(
		finiteOr(settings.gyroscope_calibration_period, 0.0f), 0.0f, 30.0f);
	settings.pointer_horizontal_sensitivity = std::clamp(
		finiteOr(settings.pointer_horizontal_sensitivity, 1.0f), 0.25f, 3.0f);
	settings.pointer_vertical_sensitivity = std::clamp(
		finiteOr(settings.pointer_vertical_sensitivity, 1.0f), 0.25f, 3.0f);

	std::scoped_lock lock(m_expression_motion_mutex);
	m_motion_mapping_settings = settings;
	m_expression_motion_handler = {};
	m_expression_motion_timestamp = {};
	reset_motion_calibration_locked();
	m_expression_pointer_position = {};
	m_expression_previous_pointer_position = {};
}

EmulatedController::MotionMappingSettings
EmulatedController::get_motion_mapping_settings() const
{
	std::scoped_lock lock(m_expression_motion_mutex);
	return m_motion_mapping_settings;
}

void EmulatedController::reset_motion_calibration_locked() const
{
	m_gyro_calibration_start = {};
	m_gyro_calibration_last_update = {};
	m_gyro_calibration_bias.fill(0.0f);
	m_gyro_calibration_sum.fill(0.0);
	m_gyro_calibration_samples = 0;
	m_gyro_has_calibration = false;
}

void EmulatedController::apply_motion_settings_locked(float gyroscope[3],
	float accelerometer[3], std::chrono::steady_clock::time_point now) const
{
	for (std::size_t axis = 0; axis < 3; ++axis)
	{
		if (!std::isfinite(gyroscope[axis]))
			gyroscope[axis] = 0.0f;
		if (!std::isfinite(accelerometer[axis]))
			accelerometer[axis] = 0.0f;
	}

	constexpr float kRadiansPerDegree = 0.017453292519943295f;
	const float deadzone =
		m_motion_mapping_settings.gyroscope_deadzone_degrees * kRadiansPerDegree;
	// calibration needs a small noise tolerance even when the deadzone is zero
	const float calibrationTolerance = std::max(deadzone, 0.5f * kRadiansPerDegree);
	const float calibrationPeriod =
		m_motion_mapping_settings.gyroscope_calibration_period;
	if (calibrationPeriod <= 0.0f)
	{
		reset_motion_calibration_locked();
	}
	else
	{
		auto beginCalibration = [&] {
			m_gyro_calibration_start = now;
			m_gyro_calibration_last_update = now;
			m_gyro_calibration_samples = 1;
			for (std::size_t axis = 0; axis < 3; ++axis)
			{
				m_gyro_calibration_sum[axis] = gyroscope[axis];
				if (!m_gyro_has_calibration)
					m_gyro_calibration_bias[axis] = gyroscope[axis];
			}
			m_gyro_has_calibration = true;
		};
		const bool interrupted = m_gyro_calibration_last_update !=
			std::chrono::steady_clock::time_point{} &&
			now - m_gyro_calibration_last_update > std::chrono::milliseconds(100);
		if (m_gyro_calibration_samples == 0 || interrupted)
		{
			beginCalibration();
		}
		else
		{
			bool stable = true;
			for (std::size_t axis = 0; axis < 3; ++axis)
			{
				const float mean = static_cast<float>(
					m_gyro_calibration_sum[axis] /
					static_cast<double>(m_gyro_calibration_samples));
				stable = stable &&
					std::abs(gyroscope[axis] - mean) <= calibrationTolerance;
			}
			if (!stable)
			{
				beginCalibration();
			}
			else
			{
				for (std::size_t axis = 0; axis < 3; ++axis)
					m_gyro_calibration_sum[axis] += gyroscope[axis];
				++m_gyro_calibration_samples;
				m_gyro_calibration_last_update = now;
				if (now - m_gyro_calibration_start >=
					std::chrono::duration<float>(calibrationPeriod))
				{
					for (std::size_t axis = 0; axis < 3; ++axis)
					{
						m_gyro_calibration_bias[axis] = static_cast<float>(
							m_gyro_calibration_sum[axis] /
							static_cast<double>(m_gyro_calibration_samples));
					}
				}
			}
		}
	}

	for (std::size_t axis = 0; axis < 3; ++axis)
	{
		const float calibrated = gyroscope[axis] - m_gyro_calibration_bias[axis];
		gyroscope[axis] = std::abs(calibrated) <= deadzone ? 0.0f :
			calibrated * m_motion_mapping_settings.gyroscope_sensitivity;
		accelerometer[axis] *= m_motion_mapping_settings.accelerometer_sensitivity;
	}
}

glm::vec2 EmulatedController::apply_pointer_settings_locked(glm::vec2 position) const
{
	if (!std::isfinite(position.x) || !std::isfinite(position.y))
		return { 0.5f, 0.5f };
	if (m_motion_mapping_settings.invert_pointer_x)
		position.x = 1.0f - position.x;
	if (m_motion_mapping_settings.invert_pointer_y)
		position.y = 1.0f - position.y;
	position.x = 0.5f + (position.x - 0.5f) *
		m_motion_mapping_settings.pointer_horizontal_sensitivity;
	position.y = 0.5f + (position.y - 0.5f) *
		m_motion_mapping_settings.pointer_vertical_sensitivity;
	return glm::clamp(position, glm::vec2(0.0f), glm::vec2(1.0f));
}

float EmulatedController::evaluate_expression(uint64 mapping) const
{
	std::scoped_lock lock(m_expression_mutex);
	auto compiled = m_compiled_expressions.find(mapping);
	if (compiled == m_compiled_expressions.end())
	{
		const std::string source = effective_expression_locked(mapping);
		if (source.empty())
			return 0.0f;
		auto expression = std::make_shared<MappingExpressionHolder>();
		(void)expression->expression.Compile(source);
		compiled = m_compiled_expressions.emplace(mapping, std::move(expression)).first;
	}
	const double now = std::chrono::duration<double>(
		std::chrono::steady_clock::now().time_since_epoch()).count();
	return compiled->second->expression.Evaluate(CemuExpressionValues(m_controllers), now);
}

void EmulatedController::reset_expression_state()
{
	{
		std::scoped_lock lock(m_expression_mutex);
		for (auto& [mapping, expression] : m_compiled_expressions)
			expression->expression.Reset();
	}
	std::scoped_lock motionLock(m_expression_motion_mutex);
	m_expression_motion_handler = {};
	m_expression_motion_timestamp = {};
	reset_motion_calibration_locked();
	m_expression_pointer_position = {};
	m_expression_previous_pointer_position = {};
}
#endif

bool EmulatedController::operator==(const EmulatedController& o) const
{
	return type() == o.type() && m_player_index == o.m_player_index;
}

bool EmulatedController::operator!=(const EmulatedController& o) const
{
	return !(*this == o);
}
