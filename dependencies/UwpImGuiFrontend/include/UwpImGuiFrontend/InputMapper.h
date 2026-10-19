#pragma once

#include "Input.h"
#include "InputMapping.h"
#include "NativeTextInput.h"
#include "Views.h"

#include <array>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace UwpImGuiFrontend
{
enum class InputMapperVisualPage : std::uint8_t
{
	Controller,
	MotionPointer,
	InputSources,
	ProfilesAdvanced,
};

struct InputMapperResult
{
	bool saved = false;
	bool cancelled = false;
	std::string error;
};

class InputMapperView
{
public:
	void Open(IInputMappingHost& host, std::optional<std::uint64_t> titleId,
		std::uint8_t player = 0);
	void Close() noexcept;
	[[nodiscard]] bool IsOpen() const noexcept { return m_open; }
	[[nodiscard]] bool IsCapturing() const noexcept;
	[[nodiscard]] std::uint8_t Player() const noexcept { return m_player; }

	[[nodiscard]] InputMapperResult Draw(const ShellLayoutContext& context,
		ImDrawList* draw, const FrameInput& input, ImFont* font = nullptr);

private:
	enum class CapturePhase : std::uint8_t { None, WaitingForNeutral, Collecting };

	void LoadPlayer(std::uint8_t player);
	void BeginCapture(bool append);
	void UpdateCapture(double nowSeconds);
	void AcceptCapture();
	[[nodiscard]] std::vector<InputCaptureSample> FilterCaptureSamples(
		std::vector<InputCaptureSample> samples) const;
	void OpenExpressionEditor();
	void OpenProfileNameEditor();
	void ValidateDraft();
	void RebuildControls(bool preserveMappings);
	void SelectControllerType(std::string_view typeId);
	void EnsureDeviceSettings(std::string_view deviceId);
	[[nodiscard]] InputDeviceSettingsDraft* SelectedDeviceSettings();
	[[nodiscard]] const InputDeviceSettingsDraft* SelectedDeviceSettings() const;
	[[nodiscard]] std::optional<std::size_t> SelectedMappingIndex() const;
	[[nodiscard]] std::vector<std::size_t> VisibleMappingIndices() const;
	[[nodiscard]] std::string BuildCapturedExpression() const;

	IInputMappingHost* m_host = nullptr;
	std::optional<std::uint64_t> m_titleId;
	ControllerProfileDraft m_draft;
	std::vector<InputDeviceDescriptor> m_devices;
	std::vector<DsuServerModel> m_dsuServers;
	std::vector<EmulatedControllerDescriptor> m_controllerTypes;
	std::vector<EmulatedControlDescriptor> m_controls;
	std::vector<std::string> m_profiles;
	std::array<std::optional<ControllerProfileDraft>, 8> m_playerDrafts;
	std::array<std::unordered_map<std::string, ControllerProfileDraft>, 8>
		m_controllerDrafts;
	std::uint8_t m_player = 0;
	InputMapperVisualPage m_visualPage = InputMapperVisualPage::Controller;
	std::string m_visualFocusId;
	std::optional<std::size_t> m_selectedMappingIndex;
	bool m_open = false;
	bool m_expressionEditorOpen = false;
	bool m_expressionTextEditorOpen = false;
	bool m_profileNameEditorOpen = false;
	bool m_profileDeleteConfirmation = false;
	bool m_captureAppend = false;
	bool m_captureFromAllDevices = false;
	bool m_waitForAlternateInputs = false;
	bool m_iterativeMapping = false;
	bool m_captureInputReleaseBarrier = false;
	CapturePhase m_capturePhase = CapturePhase::None;
	double m_captureStarted = 0.0;
	double m_neutralSince = 0.0;
	double m_chordStarted = 0.0;
	std::vector<InputCaptureSample> m_baseline;
	std::vector<InputCaptureSample> m_captured;
	std::array<char, 4097> m_expressionBuffer{};
	TextEditorPanelState m_expressionEditorState;
	ActionListState m_expressionRows;
	std::string m_expressionDeviceId;
	std::array<char, 129> m_profileNameBuffer{};
	TextEditorPanelState m_profileNameEditorState;
	std::string m_settingsDeviceId;
	std::string m_status;
	bool m_playerLoaded = false;
};
}
