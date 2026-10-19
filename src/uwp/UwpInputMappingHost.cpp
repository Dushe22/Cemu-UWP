#include "UwpInputMappingHost.h"

#include "UwpImGuiFrontend/Text.h"

#if defined(CEMU_UWP)

#include "Cafe/CafeSystem.h"
#include "Cafe/GameProfile/GameProfile.h"
#include "config/ActiveSettings.h"
#include "input/ControllerFactory.h"
#include "input/InputManager.h"
#include "input/api/Controller.h"
#if HAS_DSU
#include "input/api/DSU/DSUController.h"
#endif
#include "input/api/WGI/WGIGamepadController.h"
#include "input/emulated/ClassicController.h"
#include "input/emulated/ProController.h"
#include "input/emulated/VPADController.h"
#include "input/emulated/WiimoteController.h"
#include "uwp/UwpFrontend.h"
#include "util/helpers/helpers.h"

#include <pugixml.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <thread>
#include <tuple>

namespace CemuUwp
{
namespace
{
using namespace UwpImGuiFrontend;

struct SupplementalControl
{
	std::uint64_t id;
	std::string_view label;
	std::string_view group;
	InputControlKind kind;
};

constexpr std::array kSupplementalControls{
	SupplementalControl{ EmulatedController::kExpressionAccelX,
		"Accelerometer X axis", "Motion", InputControlKind::Accelerometer },
	SupplementalControl{ EmulatedController::kExpressionAccelY,
		"Accelerometer Y axis", "Motion", InputControlKind::Accelerometer },
	SupplementalControl{ EmulatedController::kExpressionAccelZ,
		"Accelerometer Z axis", "Motion", InputControlKind::Accelerometer },
	SupplementalControl{ EmulatedController::kExpressionGyroX,
		"Gyroscope pitch (X)", "Motion", InputControlKind::Gyroscope },
	SupplementalControl{ EmulatedController::kExpressionGyroY,
		"Gyroscope yaw (Y)", "Motion", InputControlKind::Gyroscope },
	SupplementalControl{ EmulatedController::kExpressionGyroZ,
		"Gyroscope roll (Z)", "Motion", InputControlKind::Gyroscope },
	SupplementalControl{ EmulatedController::kExpressionPointerX,
		"Pointer X position", "Pointer", InputControlKind::Pointer },
	SupplementalControl{ EmulatedController::kExpressionPointerY,
		"Pointer Y position", "Pointer", InputControlKind::Pointer },
	SupplementalControl{ EmulatedController::kExpressionPointerPress,
		"Pointer pressed", "Pointer", InputControlKind::Touch },
};

bool SupportsMotionMapping(EmulatedController::Type type)
{
	return type == EmulatedController::VPAD || type == EmulatedController::Wiimote;
}

bool SupportsPointerMapping(EmulatedController::Type type)
{
	return type == EmulatedController::VPAD || type == EmulatedController::Wiimote;
}

bool SupportsSupplementalControl(EmulatedController::Type type,
	const SupplementalControl& control)
{
	if (control.kind == InputControlKind::Accelerometer ||
		control.kind == InputControlKind::Gyroscope)
		return SupportsMotionMapping(type);
	return SupportsPointerMapping(type);
}

std::optional<DsuPlayerRoute> ConfiguredDsuRoute(std::uint8_t player)
{
	const auto& routes = GetConfig().dsu_client.player_routes;
	const auto route = std::ranges::find_if(routes,
		[player](const UwpDsuPlayerRouteEntry& value) { return value.player == player; });
	if (route == routes.end())
		return std::nullopt;
	return DsuPlayerRoute{ route->player, route->server_id, route->remote_slot,
		route->merge ? DsuRouteMode::Merge : DsuRouteMode::FullController,
		route->gamepad, route->motion, route->touch };
}

struct ProfileDsuRoute
{
	bool present = false;
	std::optional<DsuPlayerRoute> route;
};

ProfileDsuRoute ParseProfileDsuRoute(const pugi::xml_node& root,
	std::uint8_t player)
{
	const auto node = root.child("uwp_dsu_route");
	if (!node)
		return {};
	ProfileDsuRoute result{ .present = true };
	if (!node.attribute("enabled").as_bool(false))
		return result;
	const unsigned remoteSlot = node.attribute("slot").as_uint(0);
	const std::string serverId = node.attribute("server").as_string();
	if (serverId.empty() || remoteSlot >= 16)
		return result;
	result.route = DsuPlayerRoute{
		.player = player,
		.serverId = serverId,
		.remoteSlot = static_cast<std::uint8_t>(remoteSlot),
		.mode = node.attribute("merge").as_bool(false) ?
			DsuRouteMode::Merge : DsuRouteMode::FullController,
		.gamepad = node.attribute("gamepad").as_bool(true),
		.motion = node.attribute("motion").as_bool(true),
		.touch = node.attribute("touch").as_bool(true),
	};
	return result;
}

void SetProfileDsuRoute(pugi::xml_node root,
	const std::optional<DsuPlayerRoute>& route)
{
	root.remove_child("uwp_dsu_route");
	auto node = root.append_child("uwp_dsu_route");
	node.append_attribute("enabled") = route.has_value();
	if (!route)
		return;
	node.append_attribute("server") = route->serverId.c_str();
	node.append_attribute("slot") = route->remoteSlot;
	node.append_attribute("merge") = route->mode == DsuRouteMode::Merge;
	node.append_attribute("gamepad") = route->gamepad;
	node.append_attribute("motion") = route->motion;
	node.append_attribute("touch") = route->touch;
}

std::string DeviceId(const ControllerBase& controller)
{
	return fmt::format("{}:{}", controller.api_name(), controller.uuid());
}

std::pair<std::string, std::shared_ptr<ControllerBase>> PreferredInputDevice(
	const std::unordered_map<std::string, std::shared_ptr<ControllerBase>>& devices)
{
	auto rank = [](const auto& entry) {
		const auto& [id, controller] = entry;
		const bool connected = controller && controller->is_connected();
		const int apiRank = controller && controller->api() == InputAPI::WGIGamepad ? 0 :
			(controller && controller->api() == InputAPI::DSUClient ? 1 : 2);
		return std::tuple{ connected ? 0 : 1, apiRank, id };
	};
	const auto selected = std::ranges::min_element(devices, {}, rank);
	if (selected == devices.end())
		return {};
	return { selected->first, selected->second };
}

InputControlKind PhysicalControlKind(std::uint64_t control)
{
	if (control == kButtonZL || control == kButtonZR ||
		(control >= kTriggerXP && control <= kTriggerYN))
		return InputControlKind::Trigger;
	if (control >= kButtonAxisStart)
		return InputControlKind::Axis;
	return InputControlKind::Button;
}

bool IsMappablePhysicalControl(const ControllerBase& controller, std::uint64_t control)
{
	const bool commonAxis = (control >= kAxisXP && control <= kRotationYN) ||
		control == kTriggerXP || control == kTriggerYP;
	if (controller.api() == InputAPI::WGIGamepad)
		return control <= kButton9 || (control >= kButton12 && control <= kButton15) ||
			commonAxis;
#if HAS_DSU
	if (controller.api() == InputAPI::DSUClient)
		return control <= kButton17 || commonAxis;
#endif
	return true;
}

std::string PhysicalControlName(const ControllerBase& controller,
	std::uint64_t control)
{
	return controller.get_button_name(control);
}

std::string EmulatedControlGroup(EmulatedController::Type type,
	std::uint64_t mapping);

std::string EmulatedControlName(EmulatedController::Type type, std::uint64_t mapping)
{
	if (type == EmulatedController::VPAD)
	{
		const auto id = static_cast<VPADController::ButtonId>(mapping);
		if (id == VPADController::kButtonId_Mic)
			return "Microphone";
		if (id == VPADController::kButtonId_Screen)
			return "Screen";
	}

	std::string label;
	switch (type)
	{
	case EmulatedController::VPAD:
		label = VPADController::get_button_name(
			static_cast<VPADController::ButtonId>(mapping));
		break;
	case EmulatedController::Pro:
		label = ProController::get_button_name(
			static_cast<ProController::ButtonId>(mapping));
		break;
	case EmulatedController::Classic:
		label = ClassicController::get_button_name(
			static_cast<ClassicController::ButtonId>(mapping));
		break;
	case EmulatedController::Wiimote:
		label = WiimoteController::get_button_name(
			static_cast<WiimoteController::ButtonId>(mapping));
		break;
	default:
		return fmt::format("Control {}", mapping);
	}

	if (label == "home")
		return "Home";
	const std::string group = EmulatedControlGroup(type, mapping);
	if (label == "click")
		return group + " Click";
	if (label == "up")
		return group + " Up";
	if (label == "down")
		return group + " Down";
	if (label == "left")
		return group + " Left";
	if (label == "right")
		return group + " Right";
	return label.empty() ? fmt::format("Control {}", mapping) : label;
}

InputControlKind EmulatedControlKind(const EmulatedController& controller,
	std::uint64_t mapping)
{
	return controller.is_axis_mapping(mapping) ? InputControlKind::Axis :
		InputControlKind::Button;
}

fs::path ProfilePath(std::string_view name, std::uint8_t player)
{
	const std::string filename = name.empty() ? fmt::format("controller{}", player) :
		std::string(name);
	return ActiveSettings::GetConfigPath("controllerProfiles/{}.xml", filename);
}

std::string TitleProfileName(std::uint64_t titleId, std::uint8_t player)
{
	return fmt::format("title-{:016x}-p{}", titleId, player + 1);
}

bool LoadXml(const fs::path& path, pugi::xml_document& document)
{
	return document.load_file(_pathToUtf8(path).c_str());
}

bool WriteXmlAtomic(const pugi::xml_document& document, const fs::path& target,
	std::string& error)
{
	std::error_code ec;
	fs::create_directories(target.parent_path(), ec);
	if (ec)
	{
		error = fmt::format("Unable to create controller-profile folder: {}", ec.message());
		return false;
	}
	fs::path temporary = target;
	temporary += ".tmp";
	fs::path backup = target;
	backup += ".bak";
	fs::remove(temporary, ec);
	ec.clear();
	if (!document.save_file(_pathToUtf8(temporary).c_str(), "\t",
		pugi::format_default, pugi::encoding_utf8))
	{
		error = "Unable to write the temporary controller profile";
		return false;
	}
	pugi::xml_document validation;
	if (!LoadXml(temporary, validation) || !validation.child("emulated_controller"))
	{
		fs::remove(temporary, ec);
		error = "The staged controller profile did not validate";
		return false;
	}

	fs::remove(backup, ec);
	ec.clear();
	if (fs::exists(target, ec))
	{
		fs::rename(target, backup, ec);
		if (ec)
		{
			fs::remove(temporary, ec);
			error = fmt::format("Unable to stage the previous controller profile: {}", ec.message());
			return false;
		}
	}
	fs::rename(temporary, target, ec);
	if (ec)
	{
		std::error_code restoreError;
		if (fs::exists(backup, restoreError))
			fs::rename(backup, target, restoreError);
		error = fmt::format("Unable to replace the controller profile: {}", ec.message());
		return false;
	}
	fs::remove(backup, ec);
	return true;
}

struct FileTransactionBackup
{
	fs::path target;
	fs::path backup;
	bool existed{};
};

bool PrepareFileBackup(const fs::path& target, FileTransactionBackup& result,
	std::string& error, std::string_view suffix = ".uwp-save-backup")
{
	result.target = target;
	result.backup = target;
	result.backup += suffix;
	std::error_code ec;
	fs::remove(result.backup, ec);
	ec.clear();
	result.existed = fs::exists(target, ec);
	if (ec)
	{
		error = fmt::format("Unable to inspect the previous file: {}", ec.message());
		return false;
	}
	if (!result.existed)
		return true;
	fs::copy_file(target, result.backup, fs::copy_options::overwrite_existing, ec);
	if (ec)
	{
		error = fmt::format("Unable to preserve the previous file: {}", ec.message());
		return false;
	}
	return true;
}

void RestoreFileBackup(const FileTransactionBackup& backup)
{
	std::error_code ec;
	fs::remove(backup.target, ec);
	if (backup.existed)
	{
		ec.clear();
		fs::rename(backup.backup, backup.target, ec);
		if (ec)
		{
			ec.clear();
			fs::copy_file(backup.backup, backup.target,
				fs::copy_options::overwrite_existing, ec);
		}
	}
	fs::remove(backup.backup, ec);
}

void DiscardFileBackup(const FileTransactionBackup& backup)
{
	std::error_code ec;
	fs::remove(backup.backup, ec);
}

std::optional<std::string> AssignedProfile(std::uint64_t titleId, std::uint8_t player)
{
	GameProfile profile;
	profile.Reset();
	profile.Load(titleId);
	return profile.GetControllerProfile()[player];
}

std::optional<DsuPlayerRoute> EffectiveTitleDsuRoute(std::uint64_t titleId,
	std::uint8_t player)
{
	if (const auto profileName = AssignedProfile(titleId, player);
		profileName && !profileName->empty())
	{
		pugi::xml_document document;
		if (LoadXml(ProfilePath(*profileName, player), document))
		{
			const ProfileDsuRoute profileRoute = ParseProfileDsuRoute(
				document.child("emulated_controller"), player);
			if (profileRoute.present)
				return profileRoute.route;
		}
	}
	return ConfiguredDsuRoute(player);
}

EmulatedController::Type ParseType(const pugi::xml_node& root,
	EmulatedController::Type fallback)
{
	try
	{
		if (const auto type = root.child("type"))
			return EmulatedController::type_from_string(type.child_value());
	}
	catch (...)
	{
	}
	return fallback;
}

std::optional<EmulatedController::Type> ParseTypeId(std::string_view typeId)
{
	try
	{
		return EmulatedController::type_from_string(typeId);
	}
	catch (...)
	{
		return std::nullopt;
	}
}

std::string Translated(const std::string& text)
{
	return UwpImGuiFrontend::Translate(text.c_str());
}

InputSettingDraft ToggleSetting(std::string id, std::string label,
	std::string group, bool value, bool enabled = true,
	std::string description = {})
{
	return { .id = std::move(id), .label = Translated(label),
		.group = Translated(group), .description = Translated(description),
		.kind = InputSettingKind::Toggle, .enabled = enabled,
		.boolValue = value };
}

InputSettingDraft NumberSetting(std::string id, std::string label,
	std::string group, float value, float minimum, float maximum, float step,
	std::string suffix = {}, bool enabled = true)
{
	return { .id = std::move(id), .label = Translated(label),
		.group = Translated(group), .kind = InputSettingKind::Number,
		.enabled = enabled, .numberValue = std::clamp(value, minimum, maximum), .minimum = minimum,
		.maximum = maximum, .step = step, .suffix = std::move(suffix) };
}

InputSettingDraft ChoiceSetting(std::string id, std::string label,
	std::string group, std::string value, std::vector<InputSettingChoice> choices)
{
	for (InputSettingChoice& choice : choices)
		choice.label = Translated(choice.label);
	return { .id = std::move(id), .label = Translated(label),
		.group = Translated(group), .kind = InputSettingKind::Choice,
		.choiceValue = std::move(value), .choices = std::move(choices) };
}

const InputSettingDraft* FindSetting(const std::vector<InputSettingDraft>& settings,
	std::string_view id)
{
	const auto found = std::ranges::find_if(settings,
		[id](const InputSettingDraft& setting) { return setting.id == id; });
	return found == settings.end() ? nullptr : &*found;
}

void SetChildText(pugi::xml_node parent, const char* name, const char* value)
{
	auto child = parent.child(name);
	if (!child)
		child = parent.append_child(name);
	child.text().set(value);
}

template <typename T>
void SetChildValue(pugi::xml_node parent, const char* name, T value)
{
	auto child = parent.child(name);
	if (!child)
		child = parent.append_child(name);
	child.text().set(value);
}

std::string EmulatedControlGroup(EmulatedController::Type type, std::uint64_t mapping)
{
	switch (type)
	{
	case EmulatedController::VPAD:
	{
		const auto id = static_cast<VPADController::ButtonId>(mapping);
		if (id >= VPADController::kButtonId_Up && id <= VPADController::kButtonId_Right)
			return "D-Pad";
		if (id == VPADController::kButtonId_StickL ||
			(id >= VPADController::kButtonId_StickL_Up &&
				id <= VPADController::kButtonId_StickL_Right))
			return "Left Stick";
		if (id == VPADController::kButtonId_StickR ||
			(id >= VPADController::kButtonId_StickR_Up &&
				id <= VPADController::kButtonId_StickR_Right))
			return "Right Stick";
		if (id == VPADController::kButtonId_ZL || id == VPADController::kButtonId_ZR)
			return "Triggers";
		if (id == VPADController::kButtonId_Mic || id == VPADController::kButtonId_Screen)
			return "GamePad Actions";
		return "Buttons";
	}
	case EmulatedController::Pro:
	{
		const auto id = static_cast<ProController::ButtonId>(mapping);
		if (id >= ProController::kButtonId_Up && id <= ProController::kButtonId_Right)
			return "D-Pad";
		if (id == ProController::kButtonId_StickL ||
			(id >= ProController::kButtonId_StickL_Up && id <= ProController::kButtonId_StickL_Right))
			return "Left Stick";
		if (id == ProController::kButtonId_StickR ||
			(id >= ProController::kButtonId_StickR_Up && id <= ProController::kButtonId_StickR_Right))
			return "Right Stick";
		if (id == ProController::kButtonId_ZL || id == ProController::kButtonId_ZR)
			return "Triggers";
		return "Buttons";
	}
	case EmulatedController::Classic:
	{
		const auto id = static_cast<ClassicController::ButtonId>(mapping);
		if (id >= ClassicController::kButtonId_Up && id <= ClassicController::kButtonId_Right)
			return "D-Pad";
		if (id >= ClassicController::kButtonId_StickL_Up && id <= ClassicController::kButtonId_StickL_Right)
			return "Left Stick";
		if (id >= ClassicController::kButtonId_StickR_Up && id <= ClassicController::kButtonId_StickR_Right)
			return "Right Stick";
		if (id == ClassicController::kButtonId_ZL || id == ClassicController::kButtonId_ZR)
			return "Triggers";
		return "Buttons";
	}
	case EmulatedController::Wiimote:
	{
		const auto id = static_cast<WiimoteController::ButtonId>(mapping);
		if (id >= WiimoteController::kButtonId_Up && id <= WiimoteController::kButtonId_Right)
			return "Wii Remote D-Pad";
		if (id == WiimoteController::kButtonId_Nunchuck_C ||
			id == WiimoteController::kButtonId_Nunchuck_Z)
			return "Nunchuk Buttons";
		if (id >= WiimoteController::kButtonId_Nunchuck_Up &&
			id <= WiimoteController::kButtonId_Nunchuck_Right)
			return "Nunchuk Stick";
		return "Wii Remote Buttons";
	}
	default:
		return "Buttons";
	}
}

std::vector<InputSettingDraft> ControllerSettingsForType(
	EmulatedController::Type type, const pugi::xml_node& root = {})
{
	std::vector<InputSettingDraft> settings;
	EmulatedController::MotionMappingSettings motionSettings;
	if (const auto motion = root.child("uwp_motion"))
	{
		motionSettings.accelerometer_sensitivity =
			motion.child("accelerometer_sensitivity").text().as_float(
				motionSettings.accelerometer_sensitivity);
		motionSettings.gyroscope_sensitivity =
			motion.child("gyroscope_sensitivity").text().as_float(
				motionSettings.gyroscope_sensitivity);
		motionSettings.gyroscope_deadzone_degrees =
			motion.child("gyroscope_deadzone_degrees").text().as_float(
				motionSettings.gyroscope_deadzone_degrees);
		motionSettings.gyroscope_calibration_period =
			motion.child("gyroscope_calibration_period").text().as_float(
				motionSettings.gyroscope_calibration_period);
		motionSettings.pointer_horizontal_sensitivity =
			motion.child("pointer_horizontal_sensitivity").text().as_float(
				motionSettings.pointer_horizontal_sensitivity);
		motionSettings.pointer_vertical_sensitivity =
			motion.child("pointer_vertical_sensitivity").text().as_float(
				motionSettings.pointer_vertical_sensitivity);
		motionSettings.invert_pointer_x = motion.child("invert_pointer_x")
			.text().as_bool(motionSettings.invert_pointer_x);
		motionSettings.invert_pointer_y = motion.child("invert_pointer_y")
			.text().as_bool(motionSettings.invert_pointer_y);
	}
	if (type == EmulatedController::VPAD)
	{
		settings.push_back(ToggleSetting("toggle_display", "Toggle GamePad display",
			"GamePad", root.child("toggle_display").text().as_bool(false), true,
			"Treat the mapped Screen control as a toggle instead of a hold action"));
	}
	else if (type == EmulatedController::Wiimote)
	{
		const auto deviceType = root.child("device_type");
		settings.push_back(ChoiceSetting("device_type", "Wii Remote extension",
			"Wii Remote", deviceType ? deviceType.child_value() :
				std::to_string(kWAPDevCore),
			{ { std::to_string(kWAPDevCore), "None" },
				{ std::to_string(kWAPDevFreestyle), "Nunchuk" },
				{ std::to_string(kWAPDevMPLS), "MotionPlus" },
				{ std::to_string(kWAPDevMPLSFreeStyle), "MotionPlus + Nunchuk" } }));
	}
	if (SupportsMotionMapping(type))
	{
		settings.push_back(NumberSetting("accelerometer_sensitivity",
			"Accelerometer sensitivity", "Accelerometer",
			motionSettings.accelerometer_sensitivity * 100.0f,
			25.0f, 300.0f, 5.0f, "%"));
		settings.push_back(NumberSetting("gyroscope_sensitivity",
			"Gyroscope sensitivity", "Gyroscope",
			motionSettings.gyroscope_sensitivity * 100.0f,
			25.0f, 300.0f, 5.0f, "%"));
		settings.push_back(NumberSetting("gyroscope_deadzone",
			"Gyroscope dead zone", "Gyroscope",
			motionSettings.gyroscope_deadzone_degrees,
			0.0f, 180.0f, 1.0f, " deg/s"));
		settings.push_back(NumberSetting("gyroscope_calibration_period",
			"Stable-input calibration period", "Gyroscope",
			motionSettings.gyroscope_calibration_period,
			0.0f, 30.0f, 1.0f, " s"));
	}
	if (SupportsPointerMapping(type))
	{
		settings.push_back(NumberSetting("pointer_sensitivity_x",
			"Horizontal pointer sensitivity", "Pointer",
			motionSettings.pointer_horizontal_sensitivity * 100.0f,
			25.0f, 300.0f, 5.0f, "%"));
		settings.push_back(NumberSetting("pointer_sensitivity_y",
			"Vertical pointer sensitivity", "Pointer",
			motionSettings.pointer_vertical_sensitivity * 100.0f,
			25.0f, 300.0f, 5.0f, "%"));
		settings.push_back(ToggleSetting("pointer_invert_x", "Invert pointer X",
			"Pointer", motionSettings.invert_pointer_x));
		settings.push_back(ToggleSetting("pointer_invert_y", "Invert pointer Y",
			"Pointer", motionSettings.invert_pointer_y));
	}
	return settings;
}

std::vector<InputSettingDraft> DeviceSettingsForNode(const pugi::xml_node& node,
	const std::shared_ptr<ControllerBase>& controller)
{
	const auto defaults = controller ? controller->get_settings() : ControllerBase::Settings{};
	const auto axisValue = [&](const char* axis, const char* value, float fallback) {
		const auto child = node.child(axis).child(value);
		return child ? child.text().as_float(fallback) : fallback;
	};
	const bool supportsMotion = controller ? controller->has_motion() : !!node.child("motion");
	const bool supportsRumble = controller ? controller->has_rumble() : !!node.child("rumble");
	const bool supportsAxes = controller ? controller->has_axis() :
		!!node.child("axis") || !!node.child("rotation") || !!node.child("trigger");
	const std::string api = node.child_value("api");
	const bool supportsPointer = api == InputAPI::to_string(InputAPI::WGIGamepad) ||
		!!node.child("uwp_virtual_position");
	std::vector<InputSettingDraft> settings;
	settings.push_back(ToggleSetting("motion", "Use motion", "General",
		node.child("motion").text().as_bool(defaults.motion), supportsMotion));
	settings.push_back(NumberSetting("rumble", "Rumble strength", "General",
		node.child("rumble").text().as_float(defaults.rumble) * 100.0f,
		0.0f, 100.0f, 5.0f, "%", supportsRumble));
	settings.push_back(NumberSetting("axis_deadzone", "Left stick deadzone", "Left Stick",
		axisValue("axis", "deadzone", defaults.axis.deadzone) * 100.0f,
		0.0f, 100.0f, 1.0f, "%", supportsAxes));
	settings.push_back(NumberSetting("axis_range", "Left stick range", "Left Stick",
		axisValue("axis", "range", defaults.axis.range) * 100.0f,
		50.0f, 200.0f, 5.0f, "%", supportsAxes));
	settings.push_back(NumberSetting("rotation_deadzone", "Right stick deadzone", "Right Stick",
		axisValue("rotation", "deadzone", defaults.rotation.deadzone) * 100.0f,
		0.0f, 100.0f, 1.0f, "%", supportsAxes));
	settings.push_back(NumberSetting("rotation_range", "Right stick range", "Right Stick",
		axisValue("rotation", "range", defaults.rotation.range) * 100.0f,
		50.0f, 200.0f, 5.0f, "%", supportsAxes));
	settings.push_back(NumberSetting("trigger_deadzone", "Trigger deadzone", "Triggers",
		axisValue("trigger", "deadzone", defaults.trigger.deadzone) * 100.0f,
		0.0f, 100.0f, 1.0f, "%", supportsAxes));
	settings.push_back(NumberSetting("trigger_range", "Trigger range", "Triggers",
		axisValue("trigger", "range", defaults.trigger.range) * 100.0f,
		50.0f, 200.0f, 5.0f, "%", supportsAxes));
	if (supportsPointer)
	{
		const auto pointer = node.child("uwp_virtual_position");
		settings.push_back(ToggleSetting("pointer_enabled", "Virtual touch / IR pointer",
			"Pointer", pointer.child("enabled").text().as_bool(true)));
		settings.push_back(NumberSetting("pointer_speed", "Pointer speed", "Pointer",
			pointer.child("speed").text().as_float(0.65f), 0.15f, 2.5f, 0.05f));
		settings.push_back(NumberSetting("pointer_deadzone", "Pointer stick dead zone",
			"Pointer", pointer.child("deadzone").text().as_float(0.18f) * 100.0f,
			0.0f, 95.0f, 1.0f, "%"));
		settings.push_back(ChoiceSetting("pointer_stick", "Pointer stick", "Pointer",
			std::to_string(pointer.child("stick").text().as_int(0)),
			{ { "0", "Right stick" }, { "1", "Left stick" } }));
		settings.push_back(ChoiceSetting("pointer_activation", "Pointer activation", "Pointer",
			std::to_string(pointer.child("activation").text().as_int(0)),
			{ { "0", "Hold stick button" }, { "1", "Always on" } }));
	}
	return settings;
}

void ApplyControllerSettings(pugi::xml_node root, EmulatedController::Type type,
	const std::vector<InputSettingDraft>& settings)
{
	if (type != EmulatedController::VPAD)
		root.remove_child("toggle_display");
	if (type != EmulatedController::Wiimote)
		root.remove_child("device_type");
	if (type == EmulatedController::VPAD)
	{
		if (const auto* setting = FindSetting(settings, "toggle_display"))
			SetChildValue(root, "toggle_display", setting->boolValue);
	}
	else if (type == EmulatedController::Wiimote)
	{
		if (const auto* setting = FindSetting(settings, "device_type"))
			SetChildText(root, "device_type", setting->choiceValue.c_str());
	}
	root.remove_child("uwp_motion");
	if (SupportsMotionMapping(type) || SupportsPointerMapping(type))
	{
		auto motion = root.append_child("uwp_motion");
		auto setNumber = [&](const char* nodeName, std::string_view settingId,
			float scale = 1.0f, float fallback = 0.0f) {
			const auto* setting = FindSetting(settings, settingId);
			SetChildValue(motion, nodeName,
				setting ? setting->numberValue * scale : fallback);
		};
		auto setToggle = [&](const char* nodeName, std::string_view settingId) {
			const auto* setting = FindSetting(settings, settingId);
			SetChildValue(motion, nodeName, setting && setting->boolValue);
		};
		setNumber("accelerometer_sensitivity", "accelerometer_sensitivity",
			0.01f, 1.0f);
		setNumber("gyroscope_sensitivity", "gyroscope_sensitivity", 0.01f, 1.0f);
		setNumber("gyroscope_deadzone_degrees", "gyroscope_deadzone", 1.0f, 0.0f);
		setNumber("gyroscope_calibration_period", "gyroscope_calibration_period",
			1.0f, 0.0f);
		setNumber("pointer_horizontal_sensitivity", "pointer_sensitivity_x",
			0.01f, 1.0f);
		setNumber("pointer_vertical_sensitivity", "pointer_sensitivity_y",
			0.01f, 1.0f);
		setToggle("invert_pointer_x", "pointer_invert_x");
		setToggle("invert_pointer_y", "pointer_invert_y");
	}
}

void ApplyDeviceSettings(pugi::xml_node node,
	const std::vector<InputSettingDraft>& settings)
{
	if (const auto* setting = FindSetting(settings, "motion"))
		SetChildValue(node, "motion", setting->boolValue);
	if (const auto* setting = FindSetting(settings, "rumble"))
		SetChildValue(node, "rumble", setting->numberValue / 100.0f);
	for (const auto [name, deadzoneId, rangeId] : std::array{
		std::tuple{ "axis", "axis_deadzone", "axis_range" },
		std::tuple{ "rotation", "rotation_deadzone", "rotation_range" },
		std::tuple{ "trigger", "trigger_deadzone", "trigger_range" } })
	{
		auto axis = node.child(name);
		if (!axis)
			axis = node.append_child(name);
		if (const auto* setting = FindSetting(settings, deadzoneId))
			SetChildValue(axis, "deadzone", setting->numberValue / 100.0f);
		if (const auto* setting = FindSetting(settings, rangeId))
			SetChildValue(axis, "range", setting->numberValue / 100.0f);
	}
	if (FindSetting(settings, "pointer_enabled") || FindSetting(settings, "pointer_speed") ||
		FindSetting(settings, "pointer_deadzone") || FindSetting(settings, "pointer_stick") ||
		FindSetting(settings, "pointer_activation"))
	{
		auto pointer = node.child("uwp_virtual_position");
		if (!pointer)
			pointer = node.append_child("uwp_virtual_position");
		if (const auto* setting = FindSetting(settings, "pointer_enabled"))
			SetChildValue(pointer, "enabled", setting->boolValue);
		if (const auto* setting = FindSetting(settings, "pointer_speed"))
			SetChildValue(pointer, "speed", setting->numberValue);
		if (const auto* setting = FindSetting(settings, "pointer_deadzone"))
			SetChildValue(pointer, "deadzone", setting->numberValue / 100.0f);
		if (const auto* setting = FindSetting(settings, "pointer_stick"))
			SetChildValue(pointer, "stick", setting->choiceValue.c_str());
		if (const auto* setting = FindSetting(settings, "pointer_activation"))
			SetChildValue(pointer, "activation", setting->choiceValue.c_str());
	}
}

std::string ControllerNodeId(const pugi::xml_node& node);

std::unordered_map<std::uint64_t, std::string> ParseExpressions(
	const pugi::xml_node& root)
{
	std::unordered_map<std::uint64_t, std::string> result;
	if (const auto expressions = root.child("uwp_expressions"))
	{
		for (const auto& selected : expressions.select_nodes("entry"))
		{
			const auto node = selected.node();
			const auto mapping = node.attribute("mapping");
			const auto expression = node.attribute("expression");
			if (mapping && expression)
				result[mapping.as_ullong()] = expression.value();
		}
	}
	for (const auto& selected : root.select_nodes("controller"))
	{
		const auto controller = selected.node();
		const std::string deviceId = ControllerNodeId(controller);
		if (deviceId.empty())
			continue;
		for (const auto& mappingSelection : controller.select_nodes("mappings/entry"))
		{
			const auto mappingNode = mappingSelection.node();
			std::uint64_t mapping = 0;
			std::uint64_t button = 0;
			const auto mappingText = std::string_view(mappingNode.child_value("mapping"));
			const auto buttonText = std::string_view(mappingNode.child_value("button"));
			const auto mappingResult = std::from_chars(mappingText.data(),
				mappingText.data() + mappingText.size(), mapping);
			const auto buttonResult = std::from_chars(buttonText.data(),
				buttonText.data() + buttonText.size(), button);
			if (mappingResult.ec == std::errc{} && buttonResult.ec == std::errc{} &&
				!result.contains(mapping))
			{
				result[mapping] = EscapeInputControlReference(
					fmt::format("{}/{}", deviceId, button));
			}
		}
	}
	return result;
}

std::vector<std::string> ParseAttachedDevices(const pugi::xml_node& root)
{
	std::vector<std::string> result;
	for (const auto& selected : root.select_nodes("controller"))
	{
		const std::string deviceId = ControllerNodeId(selected.node());
		if (!deviceId.empty())
			result.push_back(deviceId);
	}
	return result;
}

void AddControllerNode(pugi::xml_node root, ControllerBase& controller)
{
	auto node = root.append_child("controller");
	node.append_child("api").text().set(std::string(controller.api_name()).c_str());
	node.append_child("uuid").text().set(controller.uuid().c_str());
	node.append_child("display_name").text().set(controller.display_name().c_str());
	const auto settings = controller.get_settings();
	node.append_child("motion").text().set(settings.motion);
	node.append_child("rumble").text().set(settings.rumble);
	for (const auto [name, value] : std::array{
		std::pair{ "axis", settings.axis }, std::pair{ "rotation", settings.rotation },
		std::pair{ "trigger", settings.trigger } })
	{
		auto axis = node.append_child(name);
		axis.append_child("deadzone").text().set(value.deadzone);
		axis.append_child("range").text().set(value.range);
	}
	controller.save(node);
	node.append_child("mappings");
}

std::string ControllerNodeId(const pugi::xml_node& node)
{
	const std::string api = node.child_value("api");
	const std::string uuid = node.child_value("uuid");
	if (api.empty() || uuid.empty())
		return {};
#if HAS_DSU
	if (api == InputAPI::to_string(InputAPI::DSUClient))
	{
		const std::size_t separator = uuid.find_last_of(':');
		const std::string slot = separator == std::string::npos ?
			uuid : uuid.substr(separator + 1);
		std::string endpoint = node.child_value("endpoint_id");
		if (endpoint.empty())
		{
			const std::string host = node.child_value("ip");
			const std::string port = node.child_value("port");
			if (!host.empty() && !port.empty())
				endpoint = fmt::format("{}:{}", host, port);
		}
		if (!endpoint.empty())
			return fmt::format("{}:{}:{}", api, endpoint, slot);
	}
#endif
	return fmt::format("{}:{}", api, uuid);
}

struct SimpleControlReference
{
	std::string deviceId;
	std::uint64_t control{};
};

std::optional<SimpleControlReference> ParseSimpleControlReference(
	std::string_view expression, std::string_view defaultDeviceId)
{
	if (expression.size() < 2 || expression.front() != '`' ||
		expression.back() != '`')
		return std::nullopt;

	std::string reference;
	reference.reserve(expression.size() - 2);
	bool escaped = false;
	for (std::size_t index = 1; index + 1 < expression.size(); ++index)
	{
		const char character = expression[index];
		if (escaped)
		{
			reference.push_back(character);
			escaped = false;
			continue;
		}
		if (character == '\\')
		{
			escaped = true;
			continue;
		}
		if (character == '`')
			return std::nullopt;
		reference.push_back(character);
	}
	if (escaped || reference.empty())
		return std::nullopt;

	std::string deviceId(defaultDeviceId);
	std::string_view controlText(reference);
	if (const std::size_t separator = reference.find_last_of('/');
		separator != std::string::npos)
	{
		deviceId = reference.substr(0, separator);
		controlText = std::string_view(reference).substr(separator + 1);
	}
	if (deviceId.empty() || controlText.empty())
		return std::nullopt;

	std::uint64_t control = 0;
	const auto parsed = std::from_chars(controlText.data(),
		controlText.data() + controlText.size(), control);
	if (parsed.ec != std::errc{} || parsed.ptr != controlText.data() + controlText.size())
		return std::nullopt;
	return SimpleControlReference{ std::move(deviceId), control };
}

void SyncLegacyMappings(pugi::xml_node root, const ControllerProfileDraft& draft,
	std::uint64_t highestMappingId)
{
	auto removeMapping = [root](std::uint64_t mapping) {
		for (pugi::xml_node controller : root.children("controller"))
		{
			auto mappings = controller.child("mappings");
			for (auto entry = mappings.child("entry"); entry;)
			{
				const auto next = entry.next_sibling("entry");
				std::uint64_t storedMapping = 0;
				const std::string_view text(entry.child_value("mapping"));
				const auto parsed = std::from_chars(text.data(),
					text.data() + text.size(), storedMapping);
				if (parsed.ec == std::errc{} &&
					parsed.ptr == text.data() + text.size() && storedMapping == mapping)
				{
					mappings.remove_child(entry);
				}
				entry = next;
			}
		}
	};

	for (const ControllerMappingDraft& mapping : draft.mappings)
	{
		std::uint64_t mappingId = 0;
		const auto parsed = std::from_chars(mapping.emulatedControlId.data(),
			mapping.emulatedControlId.data() + mapping.emulatedControlId.size(), mappingId);
		if (parsed.ec != std::errc{} ||
			parsed.ptr != mapping.emulatedControlId.data() + mapping.emulatedControlId.size() ||
			mappingId == 0 || mappingId >= highestMappingId)
		{
			continue;
		}

		if (mapping.expression.empty())
		{
			removeMapping(mappingId);
			continue;
		}
		const auto reference = ParseSimpleControlReference(mapping.expression,
			draft.defaultDeviceId);
		if (!reference)
			continue;

		removeMapping(mappingId);
		for (pugi::xml_node controller : root.children("controller"))
		{
			if (ControllerNodeId(controller) != reference->deviceId)
				continue;
			auto mappings = controller.child("mappings");
			if (!mappings)
				mappings = controller.append_child("mappings");
			auto entry = mappings.append_child("entry");
			entry.append_child("mapping").text().set(mappingId);
			entry.append_child("button").text().set(reference->control);
			break;
		}
	}
}
}

std::vector<InputDeviceDescriptor> InputMappingHost::EnumerateInputDevices()
{
	m_devices.clear();
	std::vector<InputDeviceDescriptor> result;
	std::set<std::string> seen;
	auto addController = [this, &result, &seen](
		const std::shared_ptr<ControllerBase>& controller) {
		if (!controller)
			return;
		const std::string id = DeviceId(*controller);
		if (!seen.insert(id).second)
			return;
		m_devices[id] = controller;
		const bool supportsVirtualPointer =
			controller->api() == InputAPI::WGIGamepad;
		const bool supportsDsuTouch =
			controller->api() == InputAPI::DSUClient;
		InputDeviceDescriptor device{ .id = id,
			.label = controller->display_name(), .api = std::string(controller->api_name()),
			.connected = controller->is_connected(),
			.supportsRumble = controller->has_rumble(),
			.supportsMotion = controller->has_motion(),
			.supportsPointer = controller->has_position() ||
				supportsVirtualPointer || supportsDsuTouch,
			.supportsConnection = true,
			.supportsCalibration = controller->has_axis() ||
				controller->has_motion() };
		device.controls.reserve(kButtonMAX + 9);
		for (std::uint64_t control = 0; control < kButtonMAX; ++control)
		{
			if (!IsMappablePhysicalControl(*controller, control))
				continue;
			const InputControlKind kind = controller->api() == InputAPI::DSUClient &&
				control == kButton16 ? InputControlKind::Touch :
				PhysicalControlKind(control);
			device.controls.push_back({ std::to_string(control),
				PhysicalControlName(*controller, control), kind,
				control >= kButtonAxisStart ? -1.0f : 0.0f, 1.0f,
				control >= kButtonAxisStart });
		}
		if (controller->has_motion())
		{
			for (const char* axis : { "Accel X", "Accel Y", "Accel Z" })
				device.controls.push_back({ axis, axis, InputControlKind::Accelerometer, -1, 1, true });
			for (const char* axis : { "Gyro X", "Gyro Y", "Gyro Z" })
				device.controls.push_back({ axis, axis, InputControlKind::Gyroscope, -1, 1, true });
		}
		if (controller->has_position() || supportsVirtualPointer)
		{
			device.controls.push_back({ "Pointer X", "Pointer X", InputControlKind::Pointer, 0, 1 });
			device.controls.push_back({ "Pointer Y", "Pointer Y", InputControlKind::Pointer, 0, 1 });
			device.controls.push_back({ "Pointer Press", "Pointer Press", InputControlKind::Touch, 0, 1 });
		}
#if HAS_DSU
		if (controller->api() == InputAPI::DSUClient)
		{
			for (const char* touch : { "Touch 1", "Touch 2" })
			{
				device.controls.push_back({ fmt::format("{} X", touch),
					fmt::format("{} X", touch), InputControlKind::Pointer, 0, 1 });
				device.controls.push_back({ fmt::format("{} Y", touch),
					fmt::format("{} Y", touch), InputControlKind::Pointer, 0, 1 });
				device.controls.push_back({ fmt::format("{} Press", touch),
					fmt::format("{} Press", touch), InputControlKind::Touch, 0, 1 });
			}
		}
#endif
		result.push_back(std::move(device));
	};
	for (const auto& providers : InputManager::instance().get_api_providers())
	{
		for (const auto& provider : providers)
		{
			if (!provider)
				continue;
			for (const auto& controller : provider->get_controllers())
				addController(controller);
		}
	}
	for (std::size_t player = 0; player < InputManager::kMaxController; ++player)
	{
		if (const auto emulated = InputManager::instance().get_controller(player))
		{
			for (const auto& controller : emulated->get_controllers())
				addController(controller);
		}
	}
	std::ranges::sort(result, [](const InputDeviceDescriptor& left,
		const InputDeviceDescriptor& right) {
		auto rank = [](const InputDeviceDescriptor& device) {
			const int apiRank = device.api == InputAPI::to_string(InputAPI::WGIGamepad) ? 0 :
				(device.api == InputAPI::to_string(InputAPI::DSUClient) ? 1 : 2);
			return std::tuple{ device.connected ? 0 : 1, apiRank, device.label, device.id };
		};
		return rank(left) < rank(right);
	});
	return result;
}

void InputMappingHost::RefreshInputDevices()
{
	m_devices.clear();
#if HAS_DSU
	InputManager::instance().refresh_uwp_dsu_provider();
#endif
}

std::vector<DsuServerModel> InputMappingHost::EnumerateDsuServers()
{
	std::vector<DsuServerModel> result;
	const auto& config = GetConfig().dsu_client;
	for (const auto& server : config.servers)
	{
		result.push_back({ server.id, server.name, server.host, server.port,
			server.enabled });
	}
	if (result.empty())
	{
		result.push_back({ "default", "DSU Server", config.host.GetValue(),
			config.port.GetValue(), config.enabled.GetValue() });
	}
	return result;
}

std::vector<EmulatedControllerDescriptor>
InputMappingHost::EnumerateEmulatedControllerTypes(std::uint8_t player)
{
	player = std::min<std::uint8_t>(player, 7);
	std::size_t vpadCount = 0;
	std::size_t wpadCount = 0;
	EmulatedController::Type selectedType = m_types[player];
	for (std::size_t index = 0; index < InputManager::kMaxController; ++index)
	{
		const auto controller = InputManager::instance().get_controller(index);
		if (index == player)
		{
			if (controller)
				selectedType = controller->type();
			continue;
		}
		if (!controller)
			continue;
		if (controller->type() == EmulatedController::VPAD)
			++vpadCount;
		else
			++wpadCount;
	}

	std::vector<EmulatedControllerDescriptor> result;
	if (vpadCount < InputManager::kMaxVPADControllers ||
		selectedType == EmulatedController::VPAD)
	{
		result.push_back({ std::string(EmulatedController::type_to_string(
			EmulatedController::VPAD)), "Wii U GamePad",
			ControllerPreviewLayout::ScreenController, { "X", "A", "B", "Y" } });
	}
	if (wpadCount < InputManager::kMaxWPADControllers ||
		selectedType != EmulatedController::VPAD)
	{
		result.push_back({ std::string(EmulatedController::type_to_string(
			EmulatedController::Pro)), "Wii U Pro Controller",
			ControllerPreviewLayout::TopDualStick, { "X", "A", "B", "Y" } });
		result.push_back({ std::string(EmulatedController::type_to_string(
			EmulatedController::Classic)), "Wii U Classic Controller",
			ControllerPreviewLayout::BottomDualStick, { "X", "A", "B", "Y" } });
		result.push_back({ std::string(EmulatedController::type_to_string(
			EmulatedController::Wiimote)), "Wii Remote",
			ControllerPreviewLayout::Remote });
	}
	return result;
}

TextureHandle InputMappingHost::GetControllerPreviewTexture(
	std::string_view controllerTypeId)
{
	const auto type = ParseTypeId(controllerTypeId);
	if (!type)
		return {};
	switch (*type)
	{
	case EmulatedController::VPAD:
		return Frontend::GetControllerPreviewTexture("wii-u-gamepad");
	case EmulatedController::Pro:
	case EmulatedController::Classic:
		return Frontend::GetControllerPreviewTexture("wii-u-pro-controller");
	case EmulatedController::Wiimote:
		return Frontend::GetControllerPreviewTexture("wii-remote");
	default:
		return {};
	}
}

std::vector<EmulatedControlDescriptor> InputMappingHost::EnumerateEmulatedControls(
	std::uint8_t player, std::string_view controllerTypeId)
{
	player = std::min<std::uint8_t>(player, 7);
	const auto parsedType = ParseTypeId(controllerTypeId);
	const EmulatedController::Type type = parsedType.value_or(m_types[player]);
	auto controller = ControllerFactory::CreateEmulatedController(player, type);
	std::vector<EmulatedControlDescriptor> result;
	for (std::uint64_t mapping = 1; mapping < controller->get_highest_mapping_id(); ++mapping)
	{
		result.push_back({ std::to_string(mapping),
			EmulatedControlName(type, mapping), EmulatedControlGroup(type, mapping),
			EmulatedControlKind(*controller, mapping) });
	}
	for (const auto& control : kSupplementalControls)
	{
		if (!SupportsSupplementalControl(type, control))
			continue;
		result.push_back({ std::to_string(control.id), std::string(control.label),
			std::string(control.group), control.kind });
	}
	return result;
}

std::vector<InputSettingDraft> InputMappingHost::CreateControllerSettings(
	std::uint8_t, std::string_view controllerTypeId)
{
	const auto parsedType = ParseTypeId(controllerTypeId);
	return ControllerSettingsForType(parsedType.value_or(EmulatedController::VPAD));
}

InputDeviceSettingsDraft InputMappingHost::CreateInputDeviceSettings(
	std::string_view deviceId)
{
	InputDeviceSettingsDraft result{ .deviceId = std::string(deviceId) };
	const auto found = m_devices.find(result.deviceId);
	if (found == m_devices.end() || !found->second)
		return result;
	pugi::xml_document document;
	auto root = document.append_child("controllers");
	AddControllerNode(root, *found->second);
	result.settings = DeviceSettingsForNode(root.child("controller"), found->second);
	return result;
}

std::vector<std::string> InputMappingHost::EnumerateProfileNames(std::uint8_t)
{
	return InputManager::get_profiles();
}

ControllerProfileDraft InputMappingHost::LoadProfileDraft(std::uint8_t player,
	std::optional<std::uint64_t> titleId)
{
	player = std::min<std::uint8_t>(player, 7);
	std::string sourceName;
	if (titleId)
		sourceName = AssignedProfile(*titleId, player).value_or(std::string{});
	else if (const auto controller = InputManager::instance().get_controller(player);
		controller && controller->has_profile_name())
		sourceName = controller->get_profile_name();
	return LoadNamedProfileDraft(player, sourceName, titleId);
}

ControllerProfileDraft InputMappingHost::LoadNamedProfileDraft(std::uint8_t player,
	std::string_view profileName, std::optional<std::uint64_t> titleId)
{
	player = std::min<std::uint8_t>(player, 7);
	const auto liveController = InputManager::instance().get_controller(player);
	const EmulatedController::Type fallbackType = liveController ?
		liveController->type() : m_types[player];
	auto attachPreferredDevice = [this](ControllerProfileDraft& draft) {
		if (!draft.attachedDeviceIds.empty())
			return;
		const auto [deviceId, device] = PreferredInputDevice(m_devices);
		if (deviceId.empty() || !device)
			return;
		draft.defaultDeviceId = deviceId;
		draft.attachedDeviceIds.push_back(deviceId);
		draft.deviceSettings.push_back(CreateInputDeviceSettings(deviceId));
	};
	std::string sourceName(profileName);
	fs::path source = ProfilePath(sourceName, player);
	if (!fs::exists(source))
	{
		sourceName.clear();
		source = ProfilePath({}, player);
	}

	pugi::xml_document document;
	if (!LoadXml(source, document))
	{
		ControllerProfileDraft empty;
		empty.player = player;
		empty.titleId = titleId;
		empty.name = titleId ? TitleProfileName(*titleId, player) :
			fmt::format("controller{}", player);
		empty.sourceProfileName = sourceName;
		empty.emulatedControllerTypeId = std::string(
			EmulatedController::type_to_string(fallbackType));
		empty.controllerSettings = ControllerSettingsForType(fallbackType);
		empty.dsuRoute = ConfiguredDsuRoute(player);
		auto controller = ControllerFactory::CreateEmulatedController(player, fallbackType);
		for (std::uint64_t mapping = 1; mapping < controller->get_highest_mapping_id(); ++mapping)
			empty.mappings.push_back({ std::to_string(mapping), {} });
		for (const auto& control : kSupplementalControls)
		{
			if (!SupportsSupplementalControl(fallbackType, control))
				continue;
			empty.mappings.push_back({ std::to_string(control.id), {} });
		}
		attachPreferredDevice(empty);
		ApplyDefaultMappings(empty, fallbackType, empty.defaultDeviceId);
		m_types[player] = fallbackType;
		return empty;
	}
	const auto root = document.child("emulated_controller");
	const EmulatedController::Type loadedType = ParseType(root, fallbackType);
	m_types[player] = loadedType;
	auto controller = ControllerFactory::CreateEmulatedController(player, loadedType);
	auto expressions = ParseExpressions(root);
	std::vector<InputDeviceIdAlias> legacyDeviceAliases;
	auto addDeviceAlias = [&legacyDeviceAliases](std::string previousId,
		std::string currentId) {
		if (previousId.empty() || currentId.empty() || previousId == currentId ||
			std::ranges::any_of(legacyDeviceAliases,
				[&previousId](const InputDeviceIdAlias& alias) {
					return alias.previousId == previousId;
				}))
		{
			return;
		}
		legacyDeviceAliases.push_back(
			{ std::move(previousId), std::move(currentId) });
	};
	for (const auto& [stableId, device] : m_devices)
	{
		const auto wgi = std::dynamic_pointer_cast<WGIGamepadController>(device);
		if (!wgi || wgi->enumeration_index() == std::numeric_limits<uint32>::max())
			continue;
		addDeviceAlias(fmt::format("{}:{}", device->api_name(),
			wgi->enumeration_index()), stableId);
	}
	for (const auto& selected : root.select_nodes("controller"))
	{
		const auto node = selected.node();
		if (std::string_view(node.child_value("api")) !=
			InputAPI::to_string(InputAPI::WGIGamepad))
		{
			continue;
		}
		const std::string persistedId = ControllerNodeId(node);
		const std::string_view uuid = node.child_value("uuid");
		constexpr std::string_view legacyStablePrefix = "stable:";
		if (!uuid.starts_with(legacyStablePrefix))
			continue;
		const std::string_view rawIdentity = uuid.substr(legacyStablePrefix.size());
		const auto live = std::ranges::find_if(m_devices,
			[rawIdentity](const auto& entry) {
				const auto wgi = std::dynamic_pointer_cast<WGIGamepadController>(
					entry.second);
				return wgi && wgi->non_roamable_id() == rawIdentity;
			});
		if (live != m_devices.end())
			addDeviceAlias(persistedId, live->first);
	}
	auto normalizeDeviceId = [&legacyDeviceAliases](std::string id) {
		const auto alias = std::ranges::find_if(legacyDeviceAliases,
			[&id](const InputDeviceIdAlias& value) {
				return value.previousId == id;
			});
		return alias == legacyDeviceAliases.end() ? id : alias->currentId;
	};
	std::set<std::uint64_t> recoverableExpressions;
	for (auto& [mapping, expression] : expressions)
	{
		if (expression.empty())
			continue;
		if (auto rewritten = RewriteInputControlDeviceIds(
			expression, legacyDeviceAliases))
		{
			expression = std::move(*rewritten);
			continue;
		}
		for (const InputDeviceIdAlias& alias : legacyDeviceAliases)
		{
			const std::string escaped = EscapeInputControlReference(alias.previousId);
			if (expression.find(alias.previousId) != std::string::npos ||
				(escaped.size() > 1 && expression.find(
					escaped.substr(0, escaped.size() - 1)) != std::string::npos))
			{
				recoverableExpressions.insert(mapping);
				break;
			}
		}
	}

	ControllerProfileDraft draft;
	draft.player = player;
	draft.titleId = titleId;
	draft.name = titleId ? TitleProfileName(*titleId, player) :
		(sourceName.empty() ? fmt::format("controller{}", player) : sourceName);
	draft.sourceProfileName = sourceName.empty() ?
		fmt::format("controller{}", player) : sourceName;
	draft.emulatedControllerTypeId = std::string(
		EmulatedController::type_to_string(loadedType));
	draft.controllerSettings = ControllerSettingsForType(loadedType, root);
	const ProfileDsuRoute profileRoute = ParseProfileDsuRoute(root, player);
	draft.dsuRoute = titleId && profileRoute.present ? profileRoute.route :
		ConfiguredDsuRoute(player);
	draft.attachedDeviceIds = ParseAttachedDevices(root);
	for (std::string& deviceId : draft.attachedDeviceIds)
		deviceId = normalizeDeviceId(std::move(deviceId));
	std::set<std::string> attachedDeviceIds;
	std::erase_if(draft.attachedDeviceIds,
		[&attachedDeviceIds](const std::string& deviceId) {
			return !attachedDeviceIds.insert(deviceId).second;
		});
	if (!draft.attachedDeviceIds.empty())
		draft.defaultDeviceId = draft.attachedDeviceIds.front();
	for (const auto& selected : root.select_nodes("controller"))
	{
		const auto node = selected.node();
		const std::string deviceId = normalizeDeviceId(ControllerNodeId(node));
		if (deviceId.empty())
			continue;
		const auto device = m_devices.find(deviceId);
		draft.deviceSettings.push_back({ deviceId,
			DeviceSettingsForNode(node, device == m_devices.end() ? nullptr : device->second) });
	}
	for (std::uint64_t mapping = 1; mapping < controller->get_highest_mapping_id(); ++mapping)
	{
		const auto found = expressions.find(mapping);
		draft.mappings.push_back({ std::to_string(mapping),
			found == expressions.end() ? std::string{} : found->second });
	}
	for (const auto& control : kSupplementalControls)
	{
		if (!SupportsSupplementalControl(loadedType, control))
			continue;
		const auto found = expressions.find(control.id);
			draft.mappings.push_back({ std::to_string(control.id),
			found == expressions.end() ? std::string{} : found->second });
	}
	attachPreferredDevice(draft);
	if (!recoverableExpressions.empty())
	{
		ControllerProfileDraft defaults = draft;
		ApplyDefaultMappings(defaults, loadedType, draft.defaultDeviceId);
		for (auto& mapping : draft.mappings)
		{
			std::uint64_t mappingId = 0;
			const auto parsed = std::from_chars(mapping.emulatedControlId.data(),
				mapping.emulatedControlId.data() + mapping.emulatedControlId.size(),
				mappingId);
			if (parsed.ec != std::errc{} ||
				!recoverableExpressions.contains(mappingId))
			{
				continue;
			}
			const auto replacement = std::ranges::find_if(defaults.mappings,
				[&mapping](const ControllerMappingDraft& candidate) {
					return candidate.emulatedControlId == mapping.emulatedControlId;
				});
			mapping.expression = replacement == defaults.mappings.end() ?
				std::string{} : replacement->expression;
		}
	}
	const auto expressionNode = root.child("uwp_expressions");
	if (!expressionNode || expressionNode.attribute("supplemental_defaults").as_uint() < 1)
	{
		ControllerProfileDraft defaults = draft;
		ApplyDefaultMappings(defaults, loadedType, draft.defaultDeviceId);
		for (const auto& control : kSupplementalControls)
		{
			if (!SupportsSupplementalControl(loadedType, control))
				continue;
			const std::string controlId = std::to_string(control.id);
			const auto current = std::ranges::find_if(draft.mappings,
				[&controlId](const ControllerMappingDraft& mapping) {
					return mapping.emulatedControlId == controlId;
				});
			const auto recommended = std::ranges::find_if(defaults.mappings,
				[&controlId](const ControllerMappingDraft& mapping) {
					return mapping.emulatedControlId == controlId;
				});
			if (current != draft.mappings.end() && current->expression.empty() &&
				recommended != defaults.mappings.end())
			{
				current->expression = recommended->expression;
			}
		}
		for (const std::string& attached : defaults.attachedDeviceIds)
		{
			if (std::ranges::find(draft.attachedDeviceIds, attached) ==
				draft.attachedDeviceIds.end())
			{
				draft.attachedDeviceIds.push_back(attached);
			}
		}
		for (const auto& settings : defaults.deviceSettings)
		{
			if (!std::ranges::any_of(draft.deviceSettings,
				[&settings](const InputDeviceSettingsDraft& current) {
					return current.deviceId == settings.deviceId;
				}))
			{
				draft.deviceSettings.push_back(settings);
			}
		}
	}
	return draft;
}

void InputMappingHost::ApplyDefaultMappings(ControllerProfileDraft& draft,
	EmulatedController::Type type, std::string_view preferredDeviceId)
{
	draft.emulatedControllerTypeId = std::string(
		EmulatedController::type_to_string(type));
	draft.controllerSettings = ControllerSettingsForType(type);
	auto emulated = ControllerFactory::CreateEmulatedController(draft.player, type);
	draft.mappings.clear();
	for (std::uint64_t mapping = 1; mapping < emulated->get_highest_mapping_id(); ++mapping)
		draft.mappings.push_back({ std::to_string(mapping), {} });
	for (const auto& control : kSupplementalControls)
	{
		if (!SupportsSupplementalControl(type, control))
			continue;
		draft.mappings.push_back({ std::to_string(control.id), {} });
	}

	std::string deviceId;
	std::shared_ptr<ControllerBase> device;
	auto selectDevice = [this, &deviceId, &device](std::string_view id) {
		if (id.empty())
			return;
		const auto found = m_devices.find(std::string(id));
		if (found == m_devices.end() || !found->second)
			return;
		deviceId = found->first;
		device = found->second;
	};
	selectDevice(preferredDeviceId);
	if (!device)
		selectDevice(draft.defaultDeviceId);
	if (!device && !m_devices.empty())
	{
		auto preferred = PreferredInputDevice(m_devices);
		deviceId = std::move(preferred.first);
		device = std::move(preferred.second);
	}

	auto attachDevice = [this, &draft](const std::string& id, bool makeDefault) {
		if (id.empty())
			return;
		if (makeDefault)
			draft.defaultDeviceId = id;
		if (std::ranges::find(draft.attachedDeviceIds, id) ==
			draft.attachedDeviceIds.end())
		{
			if (makeDefault)
				draft.attachedDeviceIds.insert(draft.attachedDeviceIds.begin(), id);
			else
				draft.attachedDeviceIds.push_back(id);
		}
		if (!std::ranges::any_of(draft.deviceSettings,
			[&id](const InputDeviceSettingsDraft& settings) {
				return settings.deviceId == id;
			}))
		{
			draft.deviceSettings.push_back(CreateInputDeviceSettings(id));
		}
	};

	std::string dsuRouteDeviceId;
	std::shared_ptr<ControllerBase> dsuRouteDevice;
#if HAS_DSU
	if (draft.dsuRoute)
	{
		const std::string expectedUuid = fmt::format("{}:{}",
			draft.dsuRoute->serverId, draft.dsuRoute->remoteSlot);
		const auto found = std::ranges::find_if(m_devices,
			[&expectedUuid](const auto& entry) {
				return entry.second && entry.second->api() == InputAPI::DSUClient &&
					entry.second->uuid() == expectedUuid;
			});
		if (found != m_devices.end())
		{
			dsuRouteDeviceId = found->first;
			dsuRouteDevice = found->second;
		}
	}
#endif
	if (!device && dsuRouteDevice)
	{
		deviceId = dsuRouteDeviceId;
		device = dsuRouteDevice;
	}
	if (!device || deviceId.empty())
		return;

	attachDevice(deviceId, true);
	if (dsuRouteDevice)
		attachDevice(dsuRouteDeviceId, false);

	std::shared_ptr<ControllerBase> mappingDevice = device;
	if (draft.dsuRoute && draft.dsuRoute->gamepad &&
		draft.dsuRoute->mode == DsuRouteMode::FullController && dsuRouteDevice)
	{
		mappingDevice = dsuRouteDevice;
	}
	emulated->add_controller(mappingDevice);
	if (emulated->set_default_mapping(mappingDevice))
	{
		for (auto& mapping : draft.mappings)
		{
			std::uint64_t id = 0;
			const auto parsed = std::from_chars(mapping.emulatedControlId.data(),
				mapping.emulatedControlId.data() + mapping.emulatedControlId.size(), id);
			if (parsed.ec == std::errc{})
				mapping.expression = emulated->get_expression_mapping(id);
		}
	}

	auto setExpression = [&draft](std::uint64_t mappingId,
		std::string_view sourceDeviceId, std::string_view controlId) {
		const std::string id = std::to_string(mappingId);
		const auto mapping = std::ranges::find_if(draft.mappings,
			[&id](const ControllerMappingDraft& value) {
				return value.emulatedControlId == id;
			});
		if (mapping != draft.mappings.end())
			mapping->expression = EscapeInputControlReference(
				std::string(sourceDeviceId) + "/" + std::string(controlId));
	};

	auto findAttachedDevice = [this, &draft](const auto& predicate)
		-> std::pair<std::string, std::shared_ptr<ControllerBase>> {
		for (const std::string& attachedId : draft.attachedDeviceIds)
		{
			const auto found = m_devices.find(attachedId);
			if (found != m_devices.end() && found->second && predicate(*found->second))
				return { found->first, found->second };
		}
		return {};
	};

	std::string motionDeviceId = deviceId;
	std::shared_ptr<ControllerBase> motionDevice = device;
	if (draft.dsuRoute && draft.dsuRoute->motion && dsuRouteDevice)
	{
		motionDeviceId = dsuRouteDeviceId;
		motionDevice = dsuRouteDevice;
	}
	else if (!motionDevice || !motionDevice->has_motion())
	{
		auto attachedMotion = findAttachedDevice(
			[](ControllerBase& candidate) { return candidate.has_motion(); });
		motionDeviceId = std::move(attachedMotion.first);
		motionDevice = std::move(attachedMotion.second);
	}
	if (SupportsMotionMapping(type) && motionDevice && motionDevice->has_motion())
	{
		for (const auto [mappingId, controlId] : std::array{
			std::pair{ EmulatedController::kExpressionAccelX, "Accel X" },
			std::pair{ EmulatedController::kExpressionAccelY, "Accel Y" },
			std::pair{ EmulatedController::kExpressionAccelZ, "Accel Z" },
			std::pair{ EmulatedController::kExpressionGyroX, "Gyro X" },
			std::pair{ EmulatedController::kExpressionGyroY, "Gyro Y" },
			std::pair{ EmulatedController::kExpressionGyroZ, "Gyro Z" },
		})
			setExpression(mappingId, motionDeviceId, controlId);
		const auto settings = std::ranges::find_if(draft.deviceSettings,
			[&motionDeviceId](const InputDeviceSettingsDraft& value) {
				return value.deviceId == motionDeviceId;
			});
		if (settings != draft.deviceSettings.end())
		{
			const auto motion = std::ranges::find_if(settings->settings,
				[](const InputSettingDraft& value) { return value.id == "motion"; });
			if (motion != settings->settings.end() && motion->enabled)
				motion->boolValue = true;
		}
	}
	if (SupportsPointerMapping(type))
	{
		std::string pointerDeviceId = deviceId;
		std::shared_ptr<ControllerBase> pointerDevice = device;
#if HAS_DSU
		if (draft.dsuRoute && draft.dsuRoute->touch && dsuRouteDevice)
		{
			pointerDeviceId = dsuRouteDeviceId;
			pointerDevice = dsuRouteDevice;
		}
		else if (!pointerDevice ||
			(pointerDevice->api() != InputAPI::WGIGamepad &&
				pointerDevice->api() != InputAPI::DSUClient &&
				!pointerDevice->has_position()))
		{
			auto attachedPointer = findAttachedDevice([](ControllerBase& candidate) {
				return candidate.api() == InputAPI::WGIGamepad ||
					candidate.api() == InputAPI::DSUClient || candidate.has_position();
			});
			pointerDeviceId = std::move(attachedPointer.first);
			pointerDevice = std::move(attachedPointer.second);
		}
		if (pointerDevice && pointerDevice->api() == InputAPI::DSUClient)
		{
			setExpression(EmulatedController::kExpressionPointerX,
				pointerDeviceId, "Touch 1 X");
			setExpression(EmulatedController::kExpressionPointerY,
				pointerDeviceId, "Touch 1 Y");
			setExpression(EmulatedController::kExpressionPointerPress,
				pointerDeviceId, "Touch 1 Press");
		}
		else
#endif
		if (pointerDevice && (pointerDevice->api() == InputAPI::WGIGamepad ||
			pointerDevice->has_position()))
		{
			setExpression(EmulatedController::kExpressionPointerX,
				pointerDeviceId, "Pointer X");
			setExpression(EmulatedController::kExpressionPointerY,
				pointerDeviceId, "Pointer Y");
			if (pointerDevice->api() == InputAPI::WGIGamepad)
				setExpression(EmulatedController::kExpressionPointerPress,
					pointerDeviceId, "Pointer Press");
		}
	}
}

ControllerProfileDraft InputMappingHost::CreateDefaultProfile(std::uint8_t player,
	std::optional<std::uint64_t> titleId, std::string_view controllerTypeId,
	std::string_view preferredDeviceId)
{
	player = std::min<std::uint8_t>(player, 7);
	auto draft = LoadProfileDraft(player, titleId);
	const EmulatedController::Type type = controllerTypeId.empty() ?
		ParseTypeId(draft.emulatedControllerTypeId).value_or(m_types[player]) :
		ParseTypeId(controllerTypeId).value_or(m_types[player]);
	ApplyDefaultMappings(draft, type, preferredDeviceId);
	draft.dirty = false;
	return draft;
}

MappingSaveResult InputMappingHost::ValidateProfileDrafts(
	std::span<const ControllerProfileDraft> drafts)
{
	MappingSaveResult result;
	if (drafts.empty())
	{
		result.succeeded = true;
		return result;
	}
	const std::optional<std::uint64_t> titleId = drafts.front().titleId;
	std::set<std::uint8_t> players;
	std::set<fs::path> targets;
	const auto servers = EnumerateDsuServers();
	for (const ControllerProfileDraft& draft : drafts)
	{
		if (draft.player >= 8)
		{
			result.error = "Invalid player number";
			return result;
		}
		if (draft.titleId != titleId)
		{
			result.error = "Controller drafts from different profile scopes cannot be saved together";
			return result;
		}
		if (!players.insert(draft.player).second)
		{
			result.error = fmt::format("Player {} appears more than once in the save batch",
				draft.player + 1);
			return result;
		}
		const auto parsedType = ParseTypeId(draft.emulatedControllerTypeId);
		if (!parsedType)
		{
			result.error = "The selected emulated controller type is invalid";
			return result;
		}
		const std::string defaultProfile = fmt::format("controller{}", draft.player);
		if (draft.name.empty() || (draft.name != defaultProfile &&
			!InputManager::is_valid_profilename(draft.name)))
		{
			result.error = "The controller profile name is invalid or reserved";
			return result;
		}
		if (draft.titleId && draft.name != TitleProfileName(*draft.titleId, draft.player))
		{
			result.error = "The per-game controller profile name is invalid";
			return result;
		}
		const fs::path target = ProfilePath(draft.name, draft.player).lexically_normal();
		if (!targets.insert(target).second)
		{
			result.error = "Two players cannot save different controller data to the same named profile";
			return result;
		}
		auto validateSettings = [&result](const std::vector<InputSettingDraft>& settings) {
			for (const auto& setting : settings)
			{
				if (setting.kind == InputSettingKind::Number &&
					(!std::isfinite(setting.numberValue) || setting.numberValue < setting.minimum ||
						setting.numberValue > setting.maximum))
				{
					result.error = setting.label + " is outside its supported range";
					return false;
				}
				if (setting.kind == InputSettingKind::Choice &&
					!std::ranges::any_of(setting.choices, [&setting](const InputSettingChoice& choice) {
						return choice.value == setting.choiceValue;
					}))
				{
					result.error = setting.label + " has an invalid selection";
					return false;
				}
			}
			return true;
		};
		if (!validateSettings(draft.controllerSettings))
			return result;
		for (const auto& device : draft.deviceSettings)
		{
			if (!validateSettings(device.settings))
				return result;
		}
		std::set<std::string> attachedDevices;
		for (const std::string& deviceId : draft.attachedDeviceIds)
		{
			if (deviceId.empty() || !attachedDevices.insert(deviceId).second)
			{
				result.error = "Attached input devices must have unique non-empty identifiers";
				return result;
			}
		}
		if (!draft.defaultDeviceId.empty() &&
			!attachedDevices.contains(draft.defaultDeviceId))
		{
			result.error = "The default input device must also be attached to the player";
			return result;
		}
		if (draft.dsuRoute)
		{
			const DsuPlayerRoute& route = *draft.dsuRoute;
			if (route.player != draft.player)
			{
				result.error = "The DSU route belongs to a different player";
				return result;
			}
			if (route.remoteSlot >= 16)
			{
				result.error = "The DSU remote slot must be between 0 and 15";
				return result;
			}
			const auto server = std::ranges::find_if(servers,
				[&route](const DsuServerModel& candidate) {
					return candidate.id == route.serverId;
				});
			if (server == servers.end())
			{
				result.error = "The selected DSU server no longer exists";
				return result;
			}
			if (!server->enabled)
			{
				result.error = "Enable the selected DSU server before assigning it";
				return result;
			}
			if (server->host.empty())
			{
				result.error = "The selected DSU server has no host address";
				return result;
			}
			if (!route.gamepad && !route.motion && !route.touch)
			{
				result.error = "Enable gamepad, motion, or touch for the DSU route";
				return result;
			}
			if (route.mode == DsuRouteMode::FullController && !route.gamepad)
			{
				result.error = "Full controller mode requires DSU buttons and sticks; use Merge for motion or touch only";
				return result;
			}
		}
		std::set<std::string> mappingIds;
		const auto controls = EnumerateEmulatedControls(draft.player,
			draft.emulatedControllerTypeId);
		for (const auto& mapping : draft.mappings)
		{
			if (!mappingIds.insert(mapping.emulatedControlId).second ||
				!std::ranges::any_of(controls, [&mapping](const EmulatedControlDescriptor& control) {
					return control.id == mapping.emulatedControlId;
				}))
			{
				result.error = "The controller profile contains an unknown or duplicate emulated control";
				return result;
			}
			if (!mapping.expression.empty())
			{
				MappingExpression expression(mapping.expression);
				if (!expression.IsValid())
				{
					result.error = fmt::format("{}: {}", mapping.emulatedControlId,
						expression.Error());
					return result;
				}
			}
		}
	}

	std::array<std::optional<EmulatedController::Type>, 8> finalTypes{};
	std::array<std::set<std::string>, 8> dsuClaims{};
	const std::string dsuPrefix = std::string(InputAPI::to_string(InputAPI::DSUClient)) + ":";
	auto addDeviceClaim = [&dsuPrefix](std::set<std::string>& claims,
		std::string_view deviceId) {
		if (deviceId.starts_with(dsuPrefix))
			claims.emplace(deviceId.substr(dsuPrefix.size()));
	};
	auto addRouteClaim = [](std::set<std::string>& claims,
		const std::optional<DsuPlayerRoute>& route) {
		if (route)
			claims.emplace(fmt::format("{}:{}", route->serverId, route->remoteSlot));
	};
	for (std::uint8_t player = 0; player < 8; ++player)
	{
		if (const auto current = InputManager::instance().get_controller(player))
		{
			finalTypes[player] = current->type();
			for (const auto& device : current->get_controllers())
			{
				if (device && device->api() == InputAPI::DSUClient)
					dsuClaims[player].emplace(device->uuid());
			}
		}
		if (titleId)
		{
			if (const auto profileName = AssignedProfile(*titleId, player);
				profileName && !profileName->empty())
			{
				pugi::xml_document document;
				if (LoadXml(ProfilePath(*profileName, player), document))
				{
					const auto root = document.child("emulated_controller");
					finalTypes[player] = ParseType(root,
						finalTypes[player].value_or(m_types[player]));
					dsuClaims[player].clear();
					for (const std::string& deviceId : ParseAttachedDevices(root))
						addDeviceClaim(dsuClaims[player], deviceId);
				}
			}
			addRouteClaim(dsuClaims[player], EffectiveTitleDsuRoute(*titleId, player));
		}
		else
			addRouteClaim(dsuClaims[player], ConfiguredDsuRoute(player));
	}
	for (const ControllerProfileDraft& draft : drafts)
	{
		finalTypes[draft.player] = *ParseTypeId(draft.emulatedControllerTypeId);
		dsuClaims[draft.player].clear();
		for (const std::string& deviceId : draft.attachedDeviceIds)
			addDeviceClaim(dsuClaims[draft.player], deviceId);
		addRouteClaim(dsuClaims[draft.player], draft.dsuRoute);
	}
	const std::size_t vpadCount = std::ranges::count(finalTypes,
		std::optional{ EmulatedController::VPAD });
	const std::size_t wpadCount = std::ranges::count_if(finalTypes,
		[](const auto& type) { return type && *type != EmulatedController::VPAD; });
	if (vpadCount > InputManager::kMaxVPADControllers ||
		wpadCount > InputManager::kMaxWPADControllers)
	{
		result.error = fmt::format(
			"Cemu supports at most {} Wii U GamePads and {} Wii/Wii U controllers",
			InputManager::kMaxVPADControllers, InputManager::kMaxWPADControllers);
		return result;
	}
	std::map<std::string, std::uint8_t> claimedDsuDevices;
	for (std::uint8_t player = 0; player < 8; ++player)
	{
		for (const std::string& claim : dsuClaims[player])
		{
			const auto [existing, inserted] = claimedDsuDevices.emplace(claim, player);
			if (!inserted && existing->second != player)
			{
				result.error = fmt::format(
					"Players {} and {} cannot use the same DSU endpoint and slot",
					existing->second + 1, player + 1);
				return result;
			}
		}
	}
	result.succeeded = true;
	return result;
}

MappingSaveResult InputMappingHost::SaveProfileDraft(const ControllerProfileDraft& draft)
{
	const MappingSaveResult validation = ValidateProfileDrafts(
		std::span<const ControllerProfileDraft>(&draft, 1));
	if (!validation.succeeded)
		return validation;
	return SaveValidatedProfileDraft(draft, true, true);
}

MappingSaveResult InputMappingHost::SaveValidatedProfileDraft(
	const ControllerProfileDraft& draft, bool activateRuntime,
	bool updateGlobalRoute)
{
	MappingSaveResult result;
	const EmulatedController::Type draftType = *ParseTypeId(
		draft.emulatedControllerTypeId);

	const std::string previousAssignedName = draft.titleId ?
		AssignedProfile(*draft.titleId, draft.player).value_or(std::string{}) :
		std::string{};
	EmulatedControllerPtr previousGlobalController;
	if (!draft.titleId)
		previousGlobalController = InputManager::instance().get_controller(draft.player);
	std::string sourceName = draft.sourceProfileName;
	if (sourceName.empty() && draft.titleId)
		sourceName = previousAssignedName;
	fs::path source = ProfilePath(sourceName, draft.player);
	if (!fs::exists(source))
		source = ProfilePath({}, draft.player);
	pugi::xml_document document;
	pugi::xml_node root;
	if (LoadXml(source, document))
		root = document.child("emulated_controller");
	else
	{
		root = document.append_child("emulated_controller");
		root.append_child("type");
	}
	if (!root)
	{
		result.error = "The source controller profile is invalid";
		return result;
	}
	SetChildText(root, "type",
		std::string(EmulatedController::type_to_string(draftType)).c_str());
	if (auto profile = root.child("profile"))
		profile.text().set(draft.name.c_str());
	else
		root.append_child("profile").text().set(draft.name.c_str());
	ApplyControllerSettings(root, draftType, draft.controllerSettings);

	pugi::xml_document preservedControllers;
	auto preservedRoot = preservedControllers.append_child("controllers");
	for (const auto& selected : root.select_nodes("controller"))
		preservedRoot.append_copy(selected.node());
	while (const auto controller = root.child("controller"))
		root.remove_child(controller);

	std::vector<std::string> attachedDevices;
	attachedDevices.reserve(draft.attachedDeviceIds.size());
	if (!draft.defaultDeviceId.empty() &&
		std::ranges::find(draft.attachedDeviceIds, draft.defaultDeviceId) !=
			draft.attachedDeviceIds.end())
		attachedDevices.push_back(draft.defaultDeviceId);
	for (const std::string& deviceId : draft.attachedDeviceIds)
	{
		if (std::ranges::find(attachedDevices, deviceId) == attachedDevices.end())
			attachedDevices.push_back(deviceId);
	}
	for (const std::string& deviceId : attachedDevices)
	{
		pugi::xml_node preserved;
		for (const pugi::xml_node& node : preservedRoot.children("controller"))
		{
			if (ControllerNodeId(node) == deviceId)
			{
				preserved = node;
				break;
			}
		}
		if (preserved)
		{
			root.append_copy(preserved);
		}
		else
		{
			const auto device = m_devices.find(deviceId);
			if (device != m_devices.end())
				AddControllerNode(root, *device->second);
		}
		const auto settings = std::ranges::find_if(draft.deviceSettings,
			[&deviceId](const InputDeviceSettingsDraft& value) {
				return value.deviceId == deviceId;
			});
		if (settings == draft.deviceSettings.end())
			continue;
		for (auto node = root.last_child(); node; node = node.previous_sibling())
		{
			if (std::string_view(node.name()) == "controller" &&
				ControllerNodeId(node) == deviceId)
			{
				ApplyDeviceSettings(node, settings->settings);
				break;
			}
		}
	}
	auto mappingController = ControllerFactory::CreateEmulatedController(
		draft.player, draftType);
	SyncLegacyMappings(root, draft, mappingController->get_highest_mapping_id());
	if (auto expressions = root.child("uwp_expressions"))
		root.remove_child(expressions);
	auto expressions = root.append_child("uwp_expressions");
	expressions.append_attribute("supplemental_defaults") = 1;
	for (const auto& mapping : draft.mappings)
	{
		auto entry = expressions.append_child("entry");
		entry.append_attribute("mapping") = mapping.emulatedControlId.c_str();
		entry.append_attribute("expression") = mapping.expression.c_str();
	}
	if (draft.titleId)
		SetProfileDsuRoute(root, draft.dsuRoute);
	else
		root.remove_child("uwp_dsu_route");

	const fs::path target = ProfilePath(draft.name, draft.player);
	std::string writeError;
	FileTransactionBackup controllerBackup;
	if (!PrepareFileBackup(target, controllerBackup, writeError))
	{
		result.error = std::move(writeError);
		return result;
	}
	if (!WriteXmlAtomic(document, target, writeError))
	{
		DiscardFileBackup(controllerBackup);
		result.error = std::move(writeError);
		return result;
	}

	std::optional<FileTransactionBackup> globalProfileBackup;
	const fs::path globalProfilePath = ProfilePath({}, draft.player);
	const bool writeGlobalProfile = !draft.titleId &&
		target.lexically_normal() != globalProfilePath.lexically_normal();
	if (writeGlobalProfile)
	{
		globalProfileBackup.emplace();
		if (!PrepareFileBackup(globalProfilePath, *globalProfileBackup, writeError))
		{
			RestoreFileBackup(controllerBackup);
			result.error = std::move(writeError);
			return result;
		}
		if (!WriteXmlAtomic(document, globalProfilePath, writeError))
		{
			RestoreFileBackup(controllerBackup);
			RestoreFileBackup(*globalProfileBackup);
			result.error = std::move(writeError);
			return result;
		}
	}

	std::optional<FileTransactionBackup> gameProfileBackup;
	auto rollback = [&]() {
		RestoreFileBackup(controllerBackup);
		if (globalProfileBackup)
			RestoreFileBackup(*globalProfileBackup);
		if (gameProfileBackup)
			RestoreFileBackup(*gameProfileBackup);
	};
	auto restorePreviousGlobalProfile = [&]() {
		if (draft.titleId)
			return;
		if (previousGlobalController)
			InputManager::instance().set_controller(previousGlobalController);
		else
			InputManager::instance().delete_controller(draft.player, false);
	};
	if (draft.titleId)
	{
		const fs::path gameProfilePath = ActiveSettings::GetConfigPath(
			"gameProfiles/{:016x}.ini", *draft.titleId);
		gameProfileBackup.emplace();
		if (!PrepareFileBackup(gameProfilePath, *gameProfileBackup, writeError))
		{
			rollback();
			result.error = std::move(writeError);
			return result;
		}
		GameProfile profile;
		profile.Reset();
		profile.Load(*draft.titleId);
		profile.SetControllerProfile(draft.player, draft.name);
		profile.Save(*draft.titleId);
		GameProfile validation;
		validation.Reset();
		if (!validation.Load(*draft.titleId) ||
			validation.GetControllerProfile()[draft.player] != draft.name)
		{
			rollback();
			result.error = "The title profile assignment could not be validated";
			return result;
		}
	}

	if (activateRuntime && draft.titleId)
	{
		if (CafeSystem::IsTitleRunning() &&
			CafeSystem::GetForegroundTitleId() == *draft.titleId &&
			!InputManager::instance().activate_uwp_title_profile(
				draft.player, draft.name))
		{
			rollback();
			InputManager::instance().load(draft.player, previousAssignedName);
#if HAS_DSU
			InputManager::instance().apply_uwp_dsu_routes();
#endif
			result.error = "The saved profile could not be activated";
			return result;
		}
	}
	else if (activateRuntime && !InputManager::instance().load(draft.player))
	{
		rollback();
		restorePreviousGlobalProfile();
		result.error = "The saved global controller profile could not be activated";
		return result;
	}

	if (!draft.titleId && updateGlobalRoute)
	{
		const auto previousRoutes = GetConfig().dsu_client.player_routes;
		auto& routes = GetConfig().dsu_client.player_routes;
		std::erase_if(routes, [&draft](const UwpDsuPlayerRouteEntry& route) {
			return route.player == draft.player;
		});
		if (draft.dsuRoute)
		{
			const DsuPlayerRoute& route = *draft.dsuRoute;
			routes.push_back({ route.player, route.serverId, route.remoteSlot,
				route.mode == DsuRouteMode::Merge, route.gamepad, route.motion,
				route.touch });
		}
		if (!GetConfigHandle().Save())
		{
			routes = previousRoutes;
			GetConfigHandle().Save();
			rollback();
			restorePreviousGlobalProfile();
#if HAS_DSU
			InputManager::instance().refresh_uwp_dsu_provider();
#endif
			result.error = "The DSU player route could not be saved";
			return result;
		}
#if HAS_DSU
		InputManager::instance().refresh_uwp_dsu_provider();
#endif
	}
	DiscardFileBackup(controllerBackup);
	if (globalProfileBackup)
		DiscardFileBackup(*globalProfileBackup);
	if (gameProfileBackup)
		DiscardFileBackup(*gameProfileBackup);
	m_types[draft.player] = draftType;
	result.succeeded = true;
	return result;
}

MappingSaveResult InputMappingHost::SaveProfileDrafts(
	std::span<const ControllerProfileDraft> drafts)
{
	const MappingSaveResult validation = ValidateProfileDrafts(drafts);
	if (!validation.succeeded)
		return validation;
	if (drafts.empty())
		return { true, {} };

	std::set<fs::path> protectedPaths;
	for (const ControllerProfileDraft& draft : drafts)
	{
		protectedPaths.emplace(ProfilePath(draft.name, draft.player).lexically_normal());
		if (draft.titleId)
		{
			protectedPaths.emplace(ActiveSettings::GetConfigPath(
				"gameProfiles/{:016x}.ini", *draft.titleId).lexically_normal());
		}
		else
			protectedPaths.emplace(ProfilePath({}, draft.player).lexically_normal());
	}
	const bool globalProfiles = !drafts.front().titleId.has_value();
	if (globalProfiles)
		protectedPaths.emplace(ActiveSettings::GetConfigPath("settings.xml").lexically_normal());

	std::vector<FileTransactionBackup> backups;
	backups.reserve(protectedPaths.size());
	std::string error;
	for (const fs::path& path : protectedPaths)
	{
		FileTransactionBackup backup;
		if (!PrepareFileBackup(path, backup, error, ".uwp-batch-backup"))
		{
			for (const FileTransactionBackup& prepared : backups)
				DiscardFileBackup(prepared);
			return { false, std::move(error) };
		}
		backups.push_back(std::move(backup));
	}

	std::array<EmulatedControllerPtr, 8> previousControllers{};
	std::array<bool, 8> previousGameProfileState{};
	for (std::size_t player = 0; player < previousControllers.size(); ++player)
	{
		previousControllers[player] = InputManager::instance().get_controller(player);
		previousGameProfileState[player] = InputManager::instance().is_gameprofile_set(player);
	}
	const auto previousTypes = m_types;
	const auto previousRoutes = GetConfig().dsu_client.player_routes;
	auto rollback = [&](std::string message) {
		for (auto backup = backups.rbegin(); backup != backups.rend(); ++backup)
			RestoreFileBackup(*backup);
		GetConfig().dsu_client.player_routes = previousRoutes;
		m_types = previousTypes;
		for (std::size_t player = 0; player < previousControllers.size(); ++player)
			InputManager::instance().delete_controller(player, false);
		for (std::size_t player = 0; player < previousControllers.size(); ++player)
		{
			if (previousControllers[player])
				InputManager::instance().set_controller(previousControllers[player]);
			InputManager::instance().set_uwp_gameprofile_state(player,
				previousGameProfileState[player]);
		}
#if HAS_DSU
		InputManager::instance().apply_uwp_dsu_routes();
#endif
		return MappingSaveResult{ false, std::move(message) };
	};

	for (const ControllerProfileDraft& draft : drafts)
	{
		const MappingSaveResult saved = SaveValidatedProfileDraft(draft, false, false);
		if (!saved.succeeded)
			return rollback(fmt::format("Player {}: {}", draft.player + 1, saved.error));
	}

	if (globalProfiles)
	{
		auto& routes = GetConfig().dsu_client.player_routes;
		for (const ControllerProfileDraft& draft : drafts)
		{
			std::erase_if(routes, [&draft](const UwpDsuPlayerRouteEntry& route) {
				return route.player == draft.player;
			});
			if (draft.dsuRoute)
			{
				const DsuPlayerRoute& route = *draft.dsuRoute;
				routes.push_back({ route.player, route.serverId, route.remoteSlot,
					route.mode == DsuRouteMode::Merge, route.gamepad, route.motion,
					route.touch });
			}
		}
		if (!GetConfigHandle().Save())
			return rollback("The DSU player routes could not be saved");
	}

	const bool activateTitleProfiles = drafts.front().titleId &&
		CafeSystem::IsTitleRunning() &&
		CafeSystem::GetForegroundTitleId() == *drafts.front().titleId;
	if (globalProfiles || activateTitleProfiles)
	{
		for (const ControllerProfileDraft& draft : drafts)
		{
			const bool activated = globalProfiles ?
				InputManager::instance().load(draft.player) :
				InputManager::instance().activate_uwp_title_profile(
					draft.player, draft.name);
			if (!activated)
				return rollback(fmt::format(
					"Player {} controller profile could not be activated",
					draft.player + 1));
		}
	}
#if HAS_DSU
	if (globalProfiles)
		InputManager::instance().refresh_uwp_dsu_provider();
	else if (activateTitleProfiles)
		InputManager::instance().apply_uwp_dsu_routes();
#endif
	for (const FileTransactionBackup& backup : backups)
		DiscardFileBackup(backup);
	return { true, {} };
}

MappingSaveResult InputMappingHost::DeleteNamedProfile(std::string_view profileName)
{
	const std::string name(profileName);
	if (!InputManager::is_valid_profilename(name))
		return { false, "The profile name is invalid or reserved" };
	std::error_code ec;
	bool removed = false;
	for (const char* extension : { ".xml", ".txt" })
	{
		ec.clear();
		fs::path path = ActiveSettings::GetConfigPath("controllerProfiles/{}", name);
		path.replace_extension(extension);
		if (fs::remove(path, ec))
			removed = true;
		if (ec)
			return { false, fmt::format("Unable to delete the controller profile: {}", ec.message()) };
	}
	return removed ? MappingSaveResult{ true, {} } :
		MappingSaveResult{ false, "The controller profile does not exist" };
}

MappingSaveResult InputMappingHost::ConnectInputDevice(std::string_view deviceId)
{
	const auto found = m_devices.find(std::string(deviceId));
	if (found == m_devices.end() || !found->second)
		return { false, "The selected input device is unavailable" };
	return found->second->connect() ? MappingSaveResult{ true, {} } :
		MappingSaveResult{ false, "The selected input device did not connect" };
}

MappingSaveResult InputMappingHost::CalibrateInputDevice(std::string_view deviceId)
{
	const auto found = m_devices.find(std::string(deviceId));
	if (found == m_devices.end() || !found->second)
		return { false, "The selected input device is unavailable" };
	if (!found->second->has_axis() && !found->second->has_motion())
		return { false, "The selected input device has no calibratable axes or motion" };
	if (found->second->has_axis())
		found->second->calibrate();
	ResetMappingRuntime();
	return { true, {} };
}

std::vector<InputCaptureSample> InputMappingHost::ReadLiveInputs()
{
	Frontend::ScopedInputMappingRead mappingRead;
	std::vector<InputCaptureSample> result;
	for (const auto& [deviceId, controller] : m_devices)
	{
		if (!controller || !controller->is_connected())
			continue;
		controller->update_state();
		for (std::uint64_t control = 0; control < kButtonMAX; ++control)
		{
			if (!IsMappablePhysicalControl(*controller, control))
				continue;
			result.push_back({ deviceId, std::to_string(control),
				controller->get_axis_value(control), PhysicalControlKind(control) });
		}
		if (controller->has_motion())
		{
			float accelerometer[3]{};
			float gyroscope[3]{};
			auto motion = controller->get_motion_sample();
			motion.getAccelerometer(accelerometer);
			motion.getGyrometer(gyroscope);
			for (int axis = 0; axis < 3; ++axis)
			{
				result.push_back({ deviceId, fmt::format("Accel {}", "XYZ"[axis]),
					accelerometer[axis], InputControlKind::Accelerometer });
				result.push_back({ deviceId, fmt::format("Gyro {}", "XYZ"[axis]),
					gyroscope[axis], InputControlKind::Gyroscope });
			}
		}
		const bool supportsVirtualPointer =
			controller->api() == InputAPI::WGIGamepad;
		if (controller->has_position() || supportsVirtualPointer)
		{
			const auto position = controller->get_position();
			result.push_back({ deviceId, "Pointer X", position.x, InputControlKind::Pointer });
			result.push_back({ deviceId, "Pointer Y", position.y, InputControlKind::Pointer });
			result.push_back({ deviceId, "Pointer Press",
				controller->has_position() ? 1.0f : 0.0f, InputControlKind::Touch });
		}
#if HAS_DSU
		if (const auto dsu = std::dynamic_pointer_cast<DSUController>(controller))
		{
			for (std::uint32_t touchIndex = 0; touchIndex < 2; ++touchIndex)
			{
				glm::vec2 position{};
				const bool active = dsu->get_touch_position(touchIndex, position);
				const std::string prefix = fmt::format("Touch {}", touchIndex + 1);
				result.push_back({ deviceId, prefix + " X", active ? position.x : 0.0f,
					InputControlKind::Pointer });
				result.push_back({ deviceId, prefix + " Y", active ? position.y : 0.0f,
					InputControlKind::Pointer });
				result.push_back({ deviceId, prefix + " Press", active ? 1.0f : 0.0f,
					InputControlKind::Touch });
			}
		}
#endif
	}
	return result;
}

void InputMappingHost::TestRumble(std::string_view deviceId, float strength,
	std::chrono::milliseconds duration)
{
	const auto found = m_devices.find(std::string(deviceId));
	if (found == m_devices.end() || !found->second || !found->second->has_rumble())
		return;
	const std::weak_ptr<ControllerBase> weak = found->second;
	auto settings = found->second->get_settings();
	const float previous = settings.rumble;
	settings.rumble = std::clamp(strength, 0.0f, 1.0f);
	found->second->set_settings(settings);
	found->second->start_rumble();
	std::thread([weak, duration, previous] {
		std::this_thread::sleep_for(duration);
		if (const auto controller = weak.lock())
		{
			controller->stop_rumble();
			auto restored = controller->get_settings();
			restored.rumble = previous;
			controller->set_settings(restored);
		}
	}).detach();
}

void InputMappingHost::ResetMappingRuntime()
{
	InputManager::instance().reset_uwp_expression_state();
}
}

#endif
