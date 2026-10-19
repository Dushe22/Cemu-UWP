#pragma once

#if defined(CEMU_UWP)

#include "UwpImGuiFrontend/InputMapping.h"

#include "input/emulated/EmulatedController.h"

#include <array>
#include <memory>
#include <optional>
#include <unordered_map>

class ControllerBase;

namespace CemuUwp
{
class InputMappingHost final : public UwpImGuiFrontend::IInputMappingHost
{
public:
	std::vector<UwpImGuiFrontend::InputDeviceDescriptor> EnumerateInputDevices() override;
	void RefreshInputDevices() override;
	std::vector<UwpImGuiFrontend::DsuServerModel> EnumerateDsuServers() override;
	std::vector<UwpImGuiFrontend::EmulatedControllerDescriptor>
		EnumerateEmulatedControllerTypes(std::uint8_t player) override;
	UwpImGuiFrontend::TextureHandle GetControllerPreviewTexture(
		std::string_view controllerTypeId) override;
	std::vector<UwpImGuiFrontend::EmulatedControlDescriptor> EnumerateEmulatedControls(
		std::uint8_t player, std::string_view controllerTypeId) override;
	std::vector<UwpImGuiFrontend::InputSettingDraft> CreateControllerSettings(
		std::uint8_t player, std::string_view controllerTypeId) override;
	UwpImGuiFrontend::InputDeviceSettingsDraft CreateInputDeviceSettings(
		std::string_view deviceId) override;
	std::vector<std::string> EnumerateProfileNames(std::uint8_t player) override;
	UwpImGuiFrontend::ControllerProfileDraft LoadProfileDraft(std::uint8_t player,
		std::optional<std::uint64_t> titleId) override;
	UwpImGuiFrontend::ControllerProfileDraft LoadNamedProfileDraft(std::uint8_t player,
		std::string_view profileName,
		std::optional<std::uint64_t> titleId) override;
	UwpImGuiFrontend::ControllerProfileDraft CreateDefaultProfile(std::uint8_t player,
		std::optional<std::uint64_t> titleId, std::string_view controllerTypeId = {},
		std::string_view preferredDeviceId = {}) override;
	UwpImGuiFrontend::MappingSaveResult SaveProfileDraft(
		const UwpImGuiFrontend::ControllerProfileDraft& draft) override;
	UwpImGuiFrontend::MappingSaveResult SaveProfileDrafts(
		std::span<const UwpImGuiFrontend::ControllerProfileDraft> drafts) override;
	UwpImGuiFrontend::MappingSaveResult DeleteNamedProfile(
		std::string_view profileName) override;
	UwpImGuiFrontend::MappingSaveResult ConnectInputDevice(
		std::string_view deviceId) override;
	UwpImGuiFrontend::MappingSaveResult CalibrateInputDevice(
		std::string_view deviceId) override;
	std::vector<UwpImGuiFrontend::InputCaptureSample> ReadLiveInputs() override;
	void TestRumble(std::string_view deviceId, float strength,
		std::chrono::milliseconds duration) override;
	void ResetMappingRuntime() override;

private:
	UwpImGuiFrontend::MappingSaveResult ValidateProfileDrafts(
		std::span<const UwpImGuiFrontend::ControllerProfileDraft> drafts);
	UwpImGuiFrontend::MappingSaveResult SaveValidatedProfileDraft(
		const UwpImGuiFrontend::ControllerProfileDraft& draft,
		bool activateRuntime, bool updateGlobalRoute);
	void ApplyDefaultMappings(UwpImGuiFrontend::ControllerProfileDraft& draft,
		EmulatedController::Type type, std::string_view preferredDeviceId);

	std::array<EmulatedController::Type, 8> m_types{
		EmulatedController::VPAD, EmulatedController::Pro, EmulatedController::Pro,
		EmulatedController::Pro, EmulatedController::Pro, EmulatedController::Pro,
		EmulatedController::Pro, EmulatedController::Pro,
	};
	std::unordered_map<std::string, std::shared_ptr<ControllerBase>> m_devices;
};
}

#endif
