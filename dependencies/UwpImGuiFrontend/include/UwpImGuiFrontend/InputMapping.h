#pragma once

#include "Types.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace UwpImGuiFrontend
{
enum class InputControlKind : std::uint8_t
{
	Button,
	Axis,
	Trigger,
	Accelerometer,
	Gyroscope,
	Pointer,
	Touch,
};

struct InputControlDescriptor
{
	std::string id;
	std::string label;
	InputControlKind kind = InputControlKind::Button;
	float minimum = 0.0f;
	float maximum = 1.0f;
	bool bidirectional = false;
};

struct InputDeviceDescriptor
{
	std::string id;
	std::string label;
	std::string api;
	bool connected = true;
	bool supportsRumble = false;
	bool supportsMotion = false;
	bool supportsPointer = false;
	bool supportsConnection = false;
	bool supportsCalibration = false;
	std::vector<InputControlDescriptor> controls;
};

struct InputDeviceIdAlias
{
	std::string previousId;
	std::string currentId;
};

[[nodiscard]] std::string EscapeInputControlReference(std::string_view reference);
[[nodiscard]] std::optional<std::string> RewriteInputControlDeviceIds(
	std::string_view expression,
	std::span<const InputDeviceIdAlias> aliases);

[[nodiscard]] std::string FormatInputExpressionForDisplay(
	std::string_view expression,
	std::span<const InputDeviceDescriptor> devices,
	std::string_view defaultDeviceId = {},
	std::string_view fallbackControlLabel = {});

struct EmulatedControlDescriptor
{
	std::string id;
	std::string label;
	std::string group;
	InputControlKind kind = InputControlKind::Button;
};

struct ExpressionLimits
{
	std::size_t maximumLength = 4096;
	std::size_t maximumTokens = 512;
	std::size_t maximumNesting = 64;
	std::size_t maximumStatefulNodes = 128;
};

class IInputValueSource
{
public:
	virtual ~IInputValueSource() = default;
	// references are opaque to the expression engine (`A` or `device/A`)
	[[nodiscard]] virtual float ReadInput(std::string_view reference) const = 0;
};

class MappingExpression
{
public:
	MappingExpression();
	explicit MappingExpression(std::string source,
		ExpressionLimits limits = {});
	MappingExpression(MappingExpression&&) noexcept;
	MappingExpression& operator=(MappingExpression&&) noexcept;
	MappingExpression(const MappingExpression&) = delete;
	MappingExpression& operator=(const MappingExpression&) = delete;
	~MappingExpression();

	[[nodiscard]] bool Compile(std::string source,
		ExpressionLimits limits = {});
	[[nodiscard]] float Evaluate(const IInputValueSource& values,
		double nowSeconds);
	void Reset() noexcept;

	[[nodiscard]] const std::string& Source() const noexcept;
	[[nodiscard]] const std::string& Error() const noexcept;
	[[nodiscard]] bool IsValid() const noexcept;

private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};

struct ControllerMappingDraft
{
	std::string emulatedControlId;
	std::string expression;
	std::string validationError;
	std::string validationWarning;
};

enum class InputSettingKind : std::uint8_t
{
	Toggle,
	Number,
	Choice,
};

struct InputSettingChoice
{
	std::string value;
	std::string label;
};

struct InputSettingDraft
{
	std::string id;
	std::string label;
	std::string group;
	std::string description;
	InputSettingKind kind = InputSettingKind::Toggle;
	bool enabled = true;
	bool boolValue = false;
	float numberValue = 0.0f;
	float minimum = 0.0f;
	float maximum = 1.0f;
	float step = 0.05f;
	std::string suffix;
	std::string choiceValue;
	std::vector<InputSettingChoice> choices;
};

struct InputDeviceSettingsDraft
{
	std::string deviceId;
	std::vector<InputSettingDraft> settings;
};

enum class ControllerPreviewLayout : std::uint8_t
{
	Generic,
	ScreenController,
	TopDualStick,
	BottomDualStick,
	Remote,
};

struct EmulatedControllerDescriptor
{
	std::string id;
	std::string label;
	ControllerPreviewLayout previewLayout = ControllerPreviewLayout::Generic;
	// clockwise from the top button: top, right, bottom, left
	std::array<std::string, 4> faceButtonLabels{};
};

struct DsuServerModel
{
	std::string id;
	std::string name;
	std::string host = "127.0.0.1";
	std::uint16_t port = 26760;
	bool enabled = false;
};

enum class DsuRouteMode : std::uint8_t
{
	FullController,
	Merge,
};

struct DsuPlayerRoute
{
	std::uint8_t player = 0;
	std::string serverId;
	std::uint8_t remoteSlot = 0;
	DsuRouteMode mode = DsuRouteMode::FullController;
	bool gamepad = true;
	bool motion = true;
	bool touch = true;
};

struct ControllerProfileDraft
{
	std::string name;
	std::string sourceProfileName;
	std::uint8_t player = 0;
	std::optional<std::uint64_t> titleId;
	std::string emulatedControllerTypeId;
	std::vector<InputSettingDraft> controllerSettings;
	std::string defaultDeviceId;
	std::vector<std::string> attachedDeviceIds;
	std::vector<InputDeviceSettingsDraft> deviceSettings;
	std::vector<ControllerMappingDraft> mappings;
	std::optional<DsuPlayerRoute> dsuRoute;
	bool dirty = false;
};

struct InputCaptureSample
{
	std::string deviceId;
	std::string controlId;
	float value = 0.0f;
	InputControlKind kind = InputControlKind::Button;
};

struct MappingSaveResult
{
	bool succeeded = false;
	std::string error;
};

class IInputMappingHost
{
public:
	virtual ~IInputMappingHost() = default;
	[[nodiscard]] virtual std::vector<InputDeviceDescriptor> EnumerateInputDevices() = 0;
	virtual void RefreshInputDevices() {}
	[[nodiscard]] virtual std::vector<DsuServerModel> EnumerateDsuServers()
	{
		return {};
	}
	[[nodiscard]] virtual std::vector<EmulatedControllerDescriptor>
		EnumerateEmulatedControllerTypes(std::uint8_t player) = 0;
	[[nodiscard]] virtual TextureHandle GetControllerPreviewTexture(
		std::string_view controllerTypeId)
	{
		return {};
	}
	[[nodiscard]] virtual std::vector<EmulatedControlDescriptor> EnumerateEmulatedControls(
		std::uint8_t player, std::string_view controllerTypeId) = 0;
	[[nodiscard]] virtual std::vector<InputSettingDraft> CreateControllerSettings(
		std::uint8_t player, std::string_view controllerTypeId) = 0;
	[[nodiscard]] virtual InputDeviceSettingsDraft CreateInputDeviceSettings(
		std::string_view deviceId) = 0;
	[[nodiscard]] virtual std::vector<std::string> EnumerateProfileNames(
		std::uint8_t player) = 0;
	[[nodiscard]] virtual ControllerProfileDraft LoadProfileDraft(
		std::uint8_t player, std::optional<std::uint64_t> titleId) = 0;
	[[nodiscard]] virtual ControllerProfileDraft LoadNamedProfileDraft(
		std::uint8_t player, std::string_view profileName,
		std::optional<std::uint64_t> titleId) = 0;
	[[nodiscard]] virtual ControllerProfileDraft CreateDefaultProfile(
		std::uint8_t player, std::optional<std::uint64_t> titleId,
		std::string_view controllerTypeId = {},
		std::string_view preferredDeviceId = {}) = 0;
	[[nodiscard]] virtual MappingSaveResult SaveProfileDraft(
		const ControllerProfileDraft& draft) = 0;
	[[nodiscard]] virtual MappingSaveResult SaveProfileDrafts(
		std::span<const ControllerProfileDraft> drafts)
	{
		for (const ControllerProfileDraft& draft : drafts)
		{
			const MappingSaveResult result = SaveProfileDraft(draft);
			if (!result.succeeded)
				return result;
		}
		return { true, {} };
	}
	[[nodiscard]] virtual MappingSaveResult DeleteNamedProfile(
		std::string_view profileName)
	{
		return { false, "Deleting profiles is not supported by this host" };
	}
	[[nodiscard]] virtual MappingSaveResult ConnectInputDevice(std::string_view)
	{
		return { false, "Connecting devices is not supported by this host" };
	}
	[[nodiscard]] virtual MappingSaveResult CalibrateInputDevice(std::string_view)
	{
		return { false, "Calibrating devices is not supported by this host" };
	}
	[[nodiscard]] virtual std::vector<InputCaptureSample> ReadLiveInputs() = 0;
	virtual void TestRumble(std::string_view deviceId, float strength,
		std::chrono::milliseconds duration) = 0;
	virtual void ResetMappingRuntime() = 0;
};
}
