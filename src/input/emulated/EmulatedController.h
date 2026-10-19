#pragma once
#include <pugixml.hpp>

#include <array>
#include <chrono>
#include <optional>

class ControllerBase;

#include "input/motion/MotionSample.h"
#if defined(CEMU_UWP)
#include "input/motion/MotionHandler.h"
#endif

#include "input/api/ControllerState.h"
#include "input/api/InputAPI.h"

#include "util/helpers/helpers.h"

// mapping = wii u controller button id
// button = api button id

class EmulatedController
{
	friend class InputManager;
public:
	EmulatedController(size_t player_index);
	virtual ~EmulatedController() = default;

	virtual void load(const pugi::xml_node& node);
	virtual void save(pugi::xml_node& node);

	enum Type
	{
		VPAD,
		Pro,
		Classic,
		Wiimote,

		MAX
	};
	virtual Type type() const = 0;
	std::string_view type_string() const { return type_to_string(type()); }

	static std::string_view type_to_string(Type type);
	static Type type_from_string(std::string_view str);

	size_t player_index() const { return m_player_index; }
	const std::string& get_profile_name() const { return m_profile_name; }
	bool has_profile_name() const { return !m_profile_name.empty() && m_profile_name != "default"; }

	void calibrate();

	void connect();
	virtual void update();
	void controllers_update_states();

	virtual glm::vec2 get_axis() const = 0;
	virtual glm::vec2 get_rotation() const = 0;
	virtual glm::vec2 get_trigger() const = 0;

	void start_rumble();
	void stop_rumble();

	bool is_battery_low() const;

	bool has_motion() const;
	MotionSample get_motion_data() const;

	// some controllers (nunchuck) provide extra motion data
	bool has_second_motion() const;
	MotionSample get_second_motion_data() const;

	bool has_position() const;
	glm::vec2 get_position() const;
	glm::vec2 get_prev_position() const;
	PositionVisibility GetPositionVisibility() const;

	void add_controller(std::shared_ptr<ControllerBase> controller);
	void remove_controller(const std::shared_ptr<ControllerBase>& controller);
	void clear_controllers();
	const std::vector<std::shared_ptr<ControllerBase>>& get_controllers() const { return m_controllers; }

	bool is_mapping_down(uint64 mapping) const;
	std::string get_mapping_name(uint64 mapping) const;
	std::shared_ptr<ControllerBase> get_mapping_controller(uint64 mapping) const;
	void delete_mapping(uint64 mapping);
	void clear_mappings();
	void set_mapping(uint64 mapping, const std::shared_ptr<ControllerBase>& controller_base, uint64 button);
#if defined(CEMU_UWP)
	static constexpr uint64 kExpressionAccelX = 0x10000;
	static constexpr uint64 kExpressionAccelY = 0x10001;
	static constexpr uint64 kExpressionAccelZ = 0x10002;
	static constexpr uint64 kExpressionGyroX = 0x10003;
	static constexpr uint64 kExpressionGyroY = 0x10004;
	static constexpr uint64 kExpressionGyroZ = 0x10005;
	static constexpr uint64 kExpressionPointerX = 0x10006;
	static constexpr uint64 kExpressionPointerY = 0x10007;
	static constexpr uint64 kExpressionPointerPress = 0x10008;
	struct MotionMappingSettings
	{
		float accelerometer_sensitivity = 1.0f;
		float gyroscope_sensitivity = 1.0f;
		float gyroscope_deadzone_degrees = 0.0f;
		float gyroscope_calibration_period = 0.0f;
		float pointer_horizontal_sensitivity = 1.0f;
		float pointer_vertical_sensitivity = 1.0f;
		bool invert_pointer_x = false;
		bool invert_pointer_y = false;
	};
	struct DsuRouteOverride
	{
		bool enabled = false;
		std::string server_id;
		uint8 remote_slot = 0;
		bool merge = false;
		bool gamepad = true;
		bool motion = true;
		bool touch = true;
	};

	void set_expression_mapping(uint64 mapping, std::string expression);
	[[nodiscard]] std::string get_expression_mapping(uint64 mapping) const;
	[[nodiscard]] std::unordered_map<uint64, std::string> get_expression_mappings() const;
	void set_runtime_expression_mapping(uint64 mapping, std::string expression,
		bool replaceProfileMapping);
	void clear_runtime_expression_mappings();
	void set_route_motion_controller(const std::shared_ptr<ControllerBase>& controller);
	void set_route_pointer_controller(const std::shared_ptr<ControllerBase>& controller);
	void clear_route_controllers();
	void set_motion_mapping_settings(const MotionMappingSettings& settings);
	[[nodiscard]] MotionMappingSettings get_motion_mapping_settings() const;
	void set_dsu_route_override(std::optional<DsuRouteOverride> route)
	{
		m_dsu_route_override = std::move(route);
	}
	[[nodiscard]] const std::optional<DsuRouteOverride>& get_dsu_route_override() const
	{
		return m_dsu_route_override;
	}
	void reset_expression_state();

	struct PointerCursor
	{
		glm::vec2 position{};
		bool touching = false;
	};
	// where a touch is or would land, also while nothing touches, so the screen can show a cursor
	[[nodiscard]] std::optional<PointerCursor> get_pointer_cursor() const;
#endif

	virtual uint32 get_emulated_button_flag(uint32 mapping) const = 0;

	bool operator==(const EmulatedController& o) const;
	bool operator!=(const EmulatedController& o) const;

	virtual size_t get_highest_mapping_id() const = 0;
	virtual bool is_axis_mapping(uint64 mapping) const = 0;

	virtual bool is_start_down() const = 0;
	virtual bool is_left_down() const = 0;
	virtual bool is_right_down() const = 0;
	virtual bool is_up_down() const = 0;
	virtual bool is_down_down() const = 0;
	virtual bool is_a_down() const = 0;
	virtual bool is_b_down() const = 0;
	virtual bool is_home_down() const = 0;

	bool was_home_button_down() { return std::exchange(m_homebutton_down, false); }

	virtual bool set_default_mapping(const std::shared_ptr<ControllerBase>& controller) { return false; }

protected:
	size_t m_player_index;
	std::string m_profile_name = "default";

	mutable std::shared_mutex m_mutex;
	std::vector<std::shared_ptr<ControllerBase>> m_controllers;

	float get_axis_value(uint64 mapping) const;
	bool m_rumble = false;

	struct Mapping
	{
		std::weak_ptr<ControllerBase> controller;
		uint64 button;
	};
	std::unordered_map<uint64, Mapping> m_mappings;
#if defined(CEMU_UWP)
	class MappingExpressionHolder;
	struct RuntimeExpressionMapping
	{
		std::string expression;
		bool replaceProfileMapping{};
	};
	std::string effective_expression_locked(uint64 mapping) const;
	void recompile_expression_locked(uint64 mapping);
	float evaluate_expression(uint64 mapping) const;
	bool has_expression_mapping(uint64 mapping) const;
	bool has_active_expression_mapping(uint64 mapping) const;
	void reset_motion_calibration_locked() const;
	void apply_motion_settings_locked(float gyroscope[3], float accelerometer[3],
		std::chrono::steady_clock::time_point now) const;
	glm::vec2 apply_pointer_settings_locked(glm::vec2 position) const;
	std::string make_control_reference(const ControllerBase& controller, uint64 button) const;
	std::unordered_map<uint64, std::string> m_expression_sources;
	std::unordered_map<uint64, RuntimeExpressionMapping> m_runtime_expression_sources;
	mutable std::unordered_map<uint64, std::shared_ptr<MappingExpressionHolder>> m_compiled_expressions;
	mutable std::mutex m_expression_mutex;
	mutable std::mutex m_expression_motion_mutex;
	mutable WiiUMotionHandler m_expression_motion_handler;
	mutable std::chrono::steady_clock::time_point m_expression_motion_timestamp{};
	mutable std::chrono::steady_clock::time_point m_gyro_calibration_start{};
	mutable std::chrono::steady_clock::time_point m_gyro_calibration_last_update{};
	mutable std::array<float, 3> m_gyro_calibration_bias{};
	mutable std::array<double, 3> m_gyro_calibration_sum{};
	mutable std::size_t m_gyro_calibration_samples{};
	mutable bool m_gyro_has_calibration{};
	MotionMappingSettings m_motion_mapping_settings{};
	mutable glm::vec2 m_expression_pointer_position{};
	mutable glm::vec2 m_expression_previous_pointer_position{};
	std::weak_ptr<ControllerBase> m_route_motion_controller;
	std::weak_ptr<ControllerBase> m_route_pointer_controller;
	std::optional<DsuRouteOverride> m_dsu_route_override;
#endif

	bool m_homebutton_down = false;
};

using EmulatedControllerPtr = std::shared_ptr<EmulatedController>;

template <>
struct fmt::formatter<EmulatedController::Type> : formatter<string_view> {
	template <typename FormatContext>
	auto format(EmulatedController::Type v, FormatContext& ctx) const {
		switch (v)
		{
		case EmulatedController::Type::VPAD: return formatter<string_view>::format("Wii U Gamepad", ctx);
		case EmulatedController::Type::Pro: return formatter<string_view>::format("Wii U Pro Controller", ctx);
		case EmulatedController::Type::Classic: return formatter<string_view>::format("Wii U Classic Controller Pro", ctx);
		case EmulatedController::Type::Wiimote: return formatter<string_view>::format("Wiimote", ctx);
		}
		throw std::invalid_argument(fmt::format("invalid emulated controller type with value {}", stdx::to_underlying(v)));
	}
};
