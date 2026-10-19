#pragma once

#include "ImGuiShell.h"
#include "Input.h"

#include <imgui.h>

#include <cstddef>
#include <span>
#include <string>
#include <string_view>

namespace UwpImGuiFrontend
{
struct WidgetInputState
{
	bool accept = false;
	bool back = false;
	bool up = false;
	bool down = false;
	bool left = false;
	bool right = false;
	bool leftShoulder = false;
	bool rightShoulder = false;
};

struct ChoiceItem
{
	std::string id;
	std::string label;
	bool enabled = true;
};

// call before NewFrame(); context/X is skipped, ImGui reserves it for window switching
void SubmitImGuiNavigationInput(const FrameInput& input,
	bool gamepadConnected = true) noexcept;

void BeginWidgetFrame(const ShellTheme& theme, const WidgetInputState& input,
	const WidgetInputState& previousInput, bool imguiOwnsNavigation = false) noexcept;
void EndWidgetFrame() noexcept;
[[nodiscard]] bool IsWidgetInputCaptured() noexcept;
[[nodiscard]] bool IsWidgetBackNavigationConsumed() noexcept;
[[nodiscard]] bool IsWidgetAcceptPressed() noexcept;
[[nodiscard]] bool IsWidgetUpPressed() noexcept;
[[nodiscard]] bool IsWidgetDownPressed() noexcept;
[[nodiscard]] bool ConsumeWidgetBackPressed() noexcept;
[[nodiscard]] bool ApplyPendingWidgetFocus() noexcept;
void SetWidgetTheme(const ShellTheme& theme) noexcept;
[[nodiscard]] const ShellTheme& CurrentWidgetTheme() noexcept;

void ApplyShellStyle();
void PushShellPopupStyle();
void PopShellPopupStyle();
[[nodiscard]] bool BeginShellPopup(const char* id, ImGuiWindowFlags flags = 0);
void EndShellPopup();
[[nodiscard]] bool BeginShellModal(const char* id, bool* open = nullptr,
	ImGuiWindowFlags flags = ImGuiWindowFlags_AlwaysAutoResize);
void EndShellModal();

[[nodiscard]] ImVec2 ResolveChildSize(ImVec2 requested);
[[nodiscard]] bool BeginShellPane(const char* id, ImVec2 size,
	ImGuiWindowFlags flags = 0, float radius = 18.0f, float opacity = 0.74f,
	ImVec2 padding = ImVec2(22.0f, 20.0f), bool navFlattened = false);
void EndShellPane();

void DrawReplicaFocus(ImDrawList* draw, ImVec2 min, ImVec2 max, float radius,
	bool focused, bool hovered);
void DrawClippedText(ImDrawList* draw, ImVec2 min, ImVec2 max, ImU32 color,
	const char* text, bool center = false);

[[nodiscard]] bool ShellButton(const char* label,
	ImVec2 size = ImVec2(0.0f, 46.0f));
[[nodiscard]] bool ShellSelectableWithId(const char* id, const char* label,
	bool selected, ImVec2 size = ImVec2(0.0f, 46.0f));
[[nodiscard]] bool ShellSelectable(const char* label, bool selected,
	ImVec2 size = ImVec2(0.0f, 46.0f));
[[nodiscard]] bool ShellCheckbox(const char* label, bool* value);
[[nodiscard]] bool ShellCombo(const char* label, std::size_t* selected,
	std::span<const ChoiceItem> choices,
	ImVec2 popupItemSize = ImVec2(320.0f, 42.0f));
[[nodiscard]] bool ShellSliderFloat(const char* label, float* value,
	float minValue, float maxValue, const char* format = "%.2f");
[[nodiscard]] bool ShellSliderInt(const char* label, int* value,
	int minValue, int maxValue, const char* format = "%d");
[[nodiscard]] bool ShellIntegerStepper(const char* label, int* value,
	int minValue, int maxValue, int step = 1, const char* format = "%d");
[[nodiscard]] bool ShellColorEdit(const char* label, ImVec4* color,
	bool editAlpha = true);
void ShellProgressBar(float fraction, std::string_view overlay = {},
	ImVec2 size = ImVec2(-1.0f, 18.0f));
void SectionTitle(const char* title);

void PushGameplayDialogStyle();
void PopGameplayDialogStyle();
}
