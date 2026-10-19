#include "UwpImGuiFrontend/Widgets.h"

#include "UwpImGuiFrontend/Text.h"

#include <imgui_internal.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace UwpImGuiFrontend
{
namespace
{
struct WidgetRuntime
{
	const ShellTheme* theme = &ShellThemes().front();
	WidgetInputState input;
	WidgetInputState previousInput;
	bool inputActive = false;
	bool imguiOwnsNavigation = false;
	bool popupOpenLastFrame = false;
	bool popupOpenAtFrameStart = false;
	bool backConsumedThisFrame = false;
	bool initialFocusPending = false;
};

WidgetRuntime s_runtime;

constexpr ImGuiButtonFlags NavigableInvisibleButtonFlags() noexcept
{
#if IMGUI_VERSION_NUM >= 19140
	// InvisibleButton became non-navigable by default in ImGui 1.91.4
	return ImGuiButtonFlags_EnableNav;
#else
	return ImGuiButtonFlags_None;
#endif
}

bool NavigableInvisibleButton(const char* id, ImVec2 size)
{
#if IMGUI_VERSION_NUM >= 19140
	// these controls draw their own focus treatment, hide the built-in nav cursor
	ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0, 0, 0, 0));
#endif
	const bool activated = ImGui::InvisibleButton(id, size,
		NavigableInvisibleButtonFlags());
#if IMGUI_VERSION_NUM >= 19140
	ImGui::PopStyleColor();
#endif
	return activated;
}

ImU32 Color(const ImVec4& color)
{
	return ImGui::ColorConvertFloat4ToU32(color);
}

ImVec4 WithAlpha(ImVec4 color, float alpha)
{
	color.w = alpha;
	return color;
}

ImVec2 ResolveControlSize(ImVec2 size, float defaultHeight = 46.0f)
{
	const ImVec2 available = ImGui::GetContentRegionAvail();
	if (size.x <= 0.0f)
		size.x = std::max(1.0f, available.x + size.x);
	if (size.y <= 0.0f)
		size.y = defaultHeight;
	return size;
}

bool RetainedActivatePressed(bool focused)
{
	if (!focused)
		return false;
	if (s_runtime.inputActive && s_runtime.input.accept &&
		!s_runtime.previousInput.accept)
		return true;
	return ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
		ImGui::IsKeyPressed(ImGuiKey_Space, false) ||
		ImGui::IsKeyPressed(ImGuiKey_GamepadFaceDown, false);
}

bool RetainedLeftPressed(bool focused)
{
	if (!focused)
		return false;
	if (s_runtime.inputActive && s_runtime.input.left &&
		!s_runtime.previousInput.left)
		return true;
	return ImGui::IsKeyPressed(ImGuiKey_LeftArrow) ||
		ImGui::IsKeyPressed(ImGuiKey_GamepadDpadLeft) ||
		ImGui::IsKeyPressed(ImGuiKey_GamepadLStickLeft);
}

bool RetainedRightPressed(bool focused)
{
	if (!focused)
		return false;
	if (s_runtime.inputActive && s_runtime.input.right &&
		!s_runtime.previousInput.right)
		return true;
	return ImGui::IsKeyPressed(ImGuiKey_RightArrow) ||
		ImGui::IsKeyPressed(ImGuiKey_GamepadDpadRight) ||
		ImGui::IsKeyPressed(ImGuiKey_GamepadLStickRight);
}

void CancelHorizontalNavigationMove(bool focused, bool leftPressed,
	bool rightPressed)
{
	if (!focused || !ImGui::GetCurrentContext() || !GImGui->NavMoveSubmitted)
		return;
	if ((leftPressed && GImGui->NavMoveDir == ImGuiDir_Left) ||
		(rightPressed && GImGui->NavMoveDir == ImGuiDir_Right))
	{
		ImGui::NavMoveRequestCancel();
	}
}

void EraseLastUtf8CodePoint(std::string& text)
{
	if (text.empty())
		return;
	size_t start = text.size() - 1;
	while (start > 0 && (static_cast<unsigned char>(text[start]) & 0xc0) == 0x80)
		--start;
	text.erase(start);
}
}

void SubmitImGuiNavigationInput(const FrameInput& input,
	bool gamepadConnected) noexcept
{
	if (!ImGui::GetCurrentContext())
		return;
	ImGuiIO& io = ImGui::GetIO();
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
	if (gamepadConnected)
		io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
	else
		io.BackendFlags &= ~ImGuiBackendFlags_HasGamepad;

	io.AddKeyEvent(ImGuiKey_GamepadFaceDown, input.accept);
	io.AddKeyEvent(ImGuiKey_GamepadFaceRight, input.back);
	// GamepadFaceLeft triggers ImGui's window-switching overlay, keep it neutral
	io.AddKeyEvent(ImGuiKey_GamepadFaceLeft, false);
	io.AddKeyEvent(ImGuiKey_GamepadFaceUp, input.alternate);
	io.AddKeyEvent(ImGuiKey_GamepadStart, input.menu);
	io.AddKeyEvent(ImGuiKey_GamepadBack, input.view);
	io.AddKeyEvent(ImGuiKey_GamepadL1, input.leftShoulder);
	io.AddKeyEvent(ImGuiKey_GamepadR1, input.rightShoulder);

	const auto magnitude = [](bool digital, float analog) {
		if (digital)
			return 1.0f;
		constexpr float deadzone = 0.35f;
		analog = std::clamp(analog, 0.0f, 1.0f);
		return analog <= deadzone ? 0.0f :
			(analog - deadzone) / (1.0f - deadzone);
	};
	const float stickLeft = magnitude(false, -input.leftStickX);
	const float stickRight = magnitude(false, input.leftStickX);
	const float stickUp = magnitude(false, input.leftStickY);
	const float stickDown = magnitude(false, -input.leftStickY);
	io.AddKeyAnalogEvent(ImGuiKey_GamepadDpadLeft, input.left,
		input.left ? 1.0f : 0.0f);
	io.AddKeyAnalogEvent(ImGuiKey_GamepadDpadRight, input.right,
		input.right ? 1.0f : 0.0f);
	io.AddKeyAnalogEvent(ImGuiKey_GamepadDpadUp, input.up,
		input.up ? 1.0f : 0.0f);
	io.AddKeyAnalogEvent(ImGuiKey_GamepadDpadDown, input.down,
		input.down ? 1.0f : 0.0f);
	io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickLeft, stickLeft > 0.0f, stickLeft);
	io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickRight, stickRight > 0.0f, stickRight);
	io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickUp, stickUp > 0.0f, stickUp);
	io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickDown, stickDown > 0.0f, stickDown);

	const float rightLeft = magnitude(false, -input.rightStickX);
	const float rightRight = magnitude(false, input.rightStickX);
	const float rightUp = magnitude(false, input.rightStickY);
	const float rightDown = magnitude(false, -input.rightStickY);
	io.AddKeyAnalogEvent(ImGuiKey_GamepadRStickLeft, rightLeft > 0.0f, rightLeft);
	io.AddKeyAnalogEvent(ImGuiKey_GamepadRStickRight, rightRight > 0.0f, rightRight);
	io.AddKeyAnalogEvent(ImGuiKey_GamepadRStickUp, rightUp > 0.0f, rightUp);
	io.AddKeyAnalogEvent(ImGuiKey_GamepadRStickDown, rightDown > 0.0f, rightDown);
}

void BeginWidgetFrame(const ShellTheme& theme, const WidgetInputState& input,
	const WidgetInputState& previousInput, bool imguiOwnsNavigation) noexcept
{
	const bool navigationOwnershipEntered = imguiOwnsNavigation &&
		!s_runtime.imguiOwnsNavigation;
	s_runtime.theme = &theme;
	s_runtime.input = input;
	s_runtime.previousInput = previousInput;
	s_runtime.inputActive = true;
	s_runtime.imguiOwnsNavigation = imguiOwnsNavigation;
	s_runtime.popupOpenAtFrameStart = s_runtime.popupOpenLastFrame;
	s_runtime.backConsumedThisFrame = false;
	if (ImGui::GetCurrentContext())
	{
		ApplyShellStyle();
		if (imguiOwnsNavigation && !s_runtime.popupOpenAtFrameStart &&
			(navigationOwnershipEntered || GImGui->NavId == 0))
		{
			s_runtime.initialFocusPending = true;
		}
	}
	if (!imguiOwnsNavigation)
		s_runtime.initialFocusPending = false;
}

void EndWidgetFrame() noexcept
{
	s_runtime.popupOpenLastFrame = ImGui::GetCurrentContext() &&
		ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopup);
	s_runtime.inputActive = false;
}

bool IsWidgetInputCaptured() noexcept
{
	return s_runtime.inputActive && s_runtime.imguiOwnsNavigation;
}

bool IsWidgetBackNavigationConsumed() noexcept
{
	return s_runtime.backConsumedThisFrame ||
		(s_runtime.inputActive && s_runtime.popupOpenAtFrameStart &&
			s_runtime.input.back && !s_runtime.previousInput.back);
}

bool IsWidgetAcceptPressed() noexcept
{
	if (!s_runtime.inputActive)
		return false;
	return s_runtime.input.accept && !s_runtime.previousInput.accept;
}

bool IsWidgetUpPressed() noexcept
{
	return s_runtime.inputActive && s_runtime.input.up &&
		!s_runtime.previousInput.up;
}

bool IsWidgetDownPressed() noexcept
{
	return s_runtime.inputActive && s_runtime.input.down &&
		!s_runtime.previousInput.down;
}

bool ConsumeWidgetBackPressed() noexcept
{
	if (!s_runtime.inputActive || s_runtime.backConsumedThisFrame ||
		!s_runtime.input.back || s_runtime.previousInput.back)
	{
		return false;
	}
	s_runtime.backConsumedThisFrame = true;
	return true;
}

bool ApplyPendingWidgetFocus() noexcept
{
	if (!s_runtime.initialFocusPending || !s_runtime.imguiOwnsNavigation ||
		!ImGui::GetCurrentContext() || !GImGui->CurrentWindow ||
		(GImGui->CurrentItemFlags & ImGuiItemFlags_Disabled) != 0)
	{
		return false;
	}
	ImGui::SetKeyboardFocusHere();
#if IMGUI_VERSION_NUM >= 19140
	// SetKeyboardFocusHere() leaves the nav cursor hidden, it must show immediately
	ImGui::SetNavCursorVisible(true);
#else
	GImGui->NavDisableHighlight = false;
#endif
	s_runtime.initialFocusPending = false;
	return true;
}

void SetWidgetTheme(const ShellTheme& theme) noexcept
{
	s_runtime.theme = &theme;
}

const ShellTheme& CurrentWidgetTheme() noexcept
{
	return s_runtime.theme ? *s_runtime.theme : ShellThemes().front();
}

void ApplyShellStyle()
{
	const auto& theme = CurrentWidgetTheme();
	ImGuiStyle& style = ImGui::GetStyle();
	style.WindowRounding = 0.0f;
	style.ChildRounding = 18.0f;
	style.FrameRounding = 10.0f;
	style.PopupRounding = 14.0f;
	// no scrollbar gutter, panes still scroll via navigation, wheel and SetScrollY
	style.ScrollbarSize = 0.0f;
	style.ScrollbarRounding = 10.0f;
	style.GrabRounding = 10.0f;
	style.TabRounding = 10.0f;
	style.AntiAliasedLines = true;
	style.AntiAliasedLinesUseTex = true;
	style.AntiAliasedFill = true;
	style.WindowBorderSize = 0.0f;
	style.ChildBorderSize = 0.0f;
	style.FrameBorderSize = 0.0f;
	style.ItemSpacing = ImVec2(12.0f, 10.0f);
	style.ItemInnerSpacing = ImVec2(8.0f, 6.0f);
	style.FramePadding = ImVec2(14.0f, 10.0f);
	style.WindowPadding = ImVec2(0.0f, 0.0f);
	style.Colors[ImGuiCol_Text] = theme.textPrimary;
	style.Colors[ImGuiCol_TextDisabled] = theme.textSecondary;
	style.Colors[ImGuiCol_WindowBg] = ImVec4(0, 0, 0, 0);
	style.Colors[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
	style.Colors[ImGuiCol_FrameBg] = WithAlpha(theme.panelBase, theme.dark ? 0.55f : 0.70f);
	style.Colors[ImGuiCol_FrameBgHovered] = WithAlpha(theme.cursorNormal, 0.22f);
	style.Colors[ImGuiCol_FrameBgActive] = WithAlpha(theme.cursorNormal, 0.35f);
	style.Colors[ImGuiCol_Button] = WithAlpha(theme.panelBase, theme.dark ? 0.55f : 0.70f);
	style.Colors[ImGuiCol_ButtonHovered] = WithAlpha(theme.cursorNormal, 0.30f);
	style.Colors[ImGuiCol_ButtonActive] = WithAlpha(theme.cursorNormal, 0.44f);
	style.Colors[ImGuiCol_Header] = WithAlpha(theme.cursorNormal, 0.20f);
	style.Colors[ImGuiCol_HeaderHovered] = WithAlpha(theme.cursorNormal, 0.30f);
	style.Colors[ImGuiCol_HeaderActive] = WithAlpha(theme.cursorNormal, 0.42f);
	style.Colors[ImGuiCol_CheckMark] = theme.pageIndicatorActive;
	style.Colors[ImGuiCol_SliderGrab] = theme.cursorNormal;
	style.Colors[ImGuiCol_SliderGrabActive] = theme.pageIndicatorActive;
	style.Colors[ImGuiCol_NavHighlight] = WithAlpha(theme.cursorNormal, 0.62f);
	style.Colors[ImGuiCol_NavWindowingHighlight] = WithAlpha(theme.cursorNormal, 0.24f);
	style.Colors[ImGuiCol_NavWindowingDimBg] = ImVec4(0, 0, 0, 0);
}

void PushShellPopupStyle()
{
	const auto& theme = CurrentWidgetTheme();
	ImVec4 background = theme.panelBase;
	background.w = theme.dark ? 0.97f : 0.94f;
	ImVec4 border = theme.panelBorder;
	border.w = std::max(border.w, theme.dark ? 0.34f : 0.18f);
	ImGui::PushStyleColor(ImGuiCol_PopupBg, background);
	ImGui::PushStyleColor(ImGuiCol_Border, border);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f, 10.0f));
	ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 14.0f);
	ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 1.0f);
}

void PopShellPopupStyle()
{
	ImGui::PopStyleVar(3);
	ImGui::PopStyleColor(2);
}

bool BeginShellPopup(const char* id, ImGuiWindowFlags flags)
{
	PushShellPopupStyle();
	if (ImGui::BeginPopup(id, flags))
	{
		if (ConsumeWidgetBackPressed())
			ImGui::CloseCurrentPopup();
		return true;
	}
	PopShellPopupStyle();
	return false;
}

void EndShellPopup()
{
	ImGui::EndPopup();
	PopShellPopupStyle();
}

bool BeginShellModal(const char* id, bool* open, ImGuiWindowFlags flags)
{
	PushShellPopupStyle();
	if (ImGui::BeginPopupModal(id, open, flags))
		return true;
	PopShellPopupStyle();
	return false;
}

void EndShellModal()
{
	ImGui::EndPopup();
	PopShellPopupStyle();
}

ImVec2 ResolveChildSize(ImVec2 requested)
{
	const ImVec2 available = ImGui::GetContentRegionAvail();
	if (requested.x <= 0.0f)
		requested.x = std::max(0.0f, available.x + requested.x);
	if (requested.y <= 0.0f)
		requested.y = std::max(0.0f, available.y + requested.y);
	return requested;
}

bool BeginShellPane(const char* id, ImVec2 size, ImGuiWindowFlags flags,
	float radius, float opacity, ImVec2 padding, bool navFlattened)
{
	const ImVec2 resolvedSize = ResolveChildSize(size);
	const ImVec2 position = ImGui::GetCursorScreenPos();
	DrawShellPanelPx(ImGui::GetWindowDrawList(), position,
		{ position.x + resolvedSize.x, position.y + resolvedSize.y },
		CurrentWidgetTheme(), radius, opacity);
	ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
	ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0.0f);
	ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 0.0f);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, padding);
#if IMGUI_VERSION_NUM >= 19090
	const ImGuiChildFlags childFlags = navFlattened ?
		ImGuiChildFlags_NavFlattened : ImGuiChildFlags_None;
	return ImGui::BeginChild(id, resolvedSize, childFlags,
		flags | ImGuiWindowFlags_NoBackground);
#else
	if (navFlattened)
		flags |= ImGuiWindowFlags_NavFlattened;
	return ImGui::BeginChild(id, resolvedSize, false,
		flags | ImGuiWindowFlags_NoBackground);
#endif
}

void EndShellPane()
{
	ImGui::EndChild();
	ImGui::PopStyleVar(3);
	ImGui::PopStyleColor();
}

void DrawReplicaFocus(ImDrawList* draw, ImVec2 min, ImVec2 max, float radius,
	bool focused, bool hovered)
{
	if (!focused && !hovered)
		return;
	const auto& theme = CurrentWidgetTheme();
	// AddRoundedRectStrokePx insets its stroke, keeping the ring inside the pane
	AddRoundedRectFilledPx(draw, min, max,
		ColorWithAlpha(theme.cursorNormal, focused ? 0.12f : 0.05f), radius);
	AddRoundedRectStrokePx(draw, min, max,
		ColorWithAlpha(theme.cursorNormal, focused ? 0.90f : 0.45f),
		radius, focused ? 2.4f : 1.5f);
}

void DrawClippedText(ImDrawList* draw, ImVec2 min, ImVec2 max, ImU32 color,
	const char* text, bool center)
{
	if (!draw || !text || text[0] == '\0' || max.x <= min.x || max.y <= min.y)
		return;
	std::string fitted(text);
	const float maxWidth = std::max(1.0f, max.x - min.x);
	if (ImGui::CalcTextSize(fitted.c_str()).x > maxWidth)
	{
		constexpr const char* ellipsis = "...";
		while (!fitted.empty() &&
			ImGui::CalcTextSize((fitted + ellipsis).c_str()).x > maxWidth)
		{
			EraseLastUtf8CodePoint(fitted);
		}
		fitted += ellipsis;
	}
	const ImVec2 textSize = ImGui::CalcTextSize(fitted.c_str());
	ImVec2 position{ min.x,
		min.y + std::max(0.0f, (max.y - min.y - textSize.y) * 0.5f) };
	if (center)
		position.x += std::max(0.0f, (max.x - min.x - textSize.x) * 0.5f);
	draw->PushClipRect(min, max, true);
	draw->AddText(position, color, fitted.c_str());
	draw->PopClipRect();
}

bool ShellButton(const char* label, ImVec2 size)
{
	const auto& theme = CurrentWidgetTheme();
	const char* visibleEnd = std::strstr(label, "##");
	const std::string visibleLabel = visibleEnd ?
		std::string(label, static_cast<std::size_t>(visibleEnd - label)) :
		std::string(label);
	size = ResolveControlSize(size);
	const ImVec2 position = ImGui::GetCursorScreenPos();
	(void)ApplyPendingWidgetFocus();
	const bool activated = NavigableInvisibleButton(label, size);
	const bool hovered = ImGui::IsItemHovered();
	const bool focused = ImGui::IsItemFocused();
	const bool pressed = activated || RetainedActivatePressed(focused);
	ImDrawList* draw = ImGui::GetWindowDrawList();
	draw->PushClipRect({ position.x - 8.0f, position.y - 8.0f },
		{ position.x + size.x + 8.0f, position.y + size.y + 8.0f }, true);
	DrawShellPanelPx(draw, position, { position.x + size.x, position.y + size.y },
		theme, 14.0f, pressed ? 0.96f : (hovered || focused ? 0.88f : 0.72f));
	DrawReplicaFocus(draw, position, { position.x + size.x, position.y + size.y },
		15.0f, focused, hovered);
	DrawClippedText(draw, { position.x + 14.0f, position.y },
		{ position.x + size.x - 14.0f, position.y + size.y },
		Color(theme.textPrimary), visibleLabel.c_str(), true);
	draw->PopClipRect();
	return pressed;
}

bool ShellSelectableWithId(const char* id, const char* label, bool selected,
	ImVec2 size)
{
	const auto& theme = CurrentWidgetTheme();
	size = ResolveControlSize(size);
	const ImVec2 position = ImGui::GetCursorScreenPos();
	ImGui::PushID(id);
	(void)ApplyPendingWidgetFocus();
	const bool activated = NavigableInvisibleButton("select", size);
	const bool hovered = ImGui::IsItemHovered();
	const bool focused = ImGui::IsItemFocused();
	const bool pressed = activated || RetainedActivatePressed(focused);
	ImGui::PopID();
	// the fill follows the cursor; in popups the current value is marked by its dot only,
	// otherwise a second fill a few rows away reads as a stray highlight
	const ImGuiWindow* window = ImGui::GetCurrentWindow();
	const bool inPopup = window && window->RootWindow &&
		(window->RootWindow->Flags & ImGuiWindowFlags_Popup) != 0;
	const bool cursorHere = focused || hovered;
	ImDrawList* draw = ImGui::GetWindowDrawList();
	draw->PushClipRect({ position.x - 8.0f, position.y - 8.0f },
		{ position.x + size.x + 8.0f, position.y + size.y + 8.0f }, true);
	if (cursorHere || (selected && !inPopup))
	{
		DrawShellPanelPx(draw, position,
			{ position.x + size.x, position.y + size.y }, theme, 14.0f,
			cursorHere ? 0.82f : 0.50f);
	}
	DrawReplicaFocus(draw, position,
		{ position.x + size.x, position.y + size.y }, 14.0f,
		focused, hovered);
	DrawClippedText(draw, { position.x + 16.0f, position.y },
		{ position.x + size.x - (selected ? 40.0f : 12.0f), position.y + size.y },
		selected ? Color(theme.textPrimary) : Color(theme.textSecondary), label);
	if (selected)
	{
		draw->AddCircleFilled({ position.x + size.x - 24.0f,
			position.y + size.y * 0.5f }, 5.0f,
			Color(theme.pageIndicatorActive), 18);
	}
	draw->PopClipRect();
	return pressed;
}

bool ShellSelectable(const char* label, bool selected, ImVec2 size)
{
	return ShellSelectableWithId(label, label, selected, size);
}

bool ShellCheckbox(const char* label, bool* value)
{
	if (!value)
		return false;
	const auto& theme = CurrentWidgetTheme();
	const ImVec2 size = ResolveControlSize(ImVec2(0.0f, 48.0f));
	const ImVec2 position = ImGui::GetCursorScreenPos();
	ImGui::PushID(label);
	(void)ApplyPendingWidgetFocus();
	const bool activated = NavigableInvisibleButton("check", size);
	const bool hovered = ImGui::IsItemHovered();
	const bool focused = ImGui::IsItemFocused();
	const bool pressed = activated || RetainedActivatePressed(focused);
	ImGui::PopID();
	if (pressed)
		*value = !*value;

	ImDrawList* draw = ImGui::GetWindowDrawList();
	draw->PushClipRect({ position.x - 8.0f, position.y - 8.0f },
		{ position.x + size.x + 8.0f, position.y + size.y + 8.0f }, true);
	DrawShellPanelPx(draw, position, { position.x + size.x, position.y + size.y },
		theme, 14.0f, hovered || focused ? 0.70f : 0.48f);
	DrawReplicaFocus(draw, position, { position.x + size.x, position.y + size.y },
		14.0f, focused, hovered);
	DrawClippedText(draw, { position.x + 16.0f, position.y },
		{ position.x + size.x - 76.0f, position.y + size.y },
		Color(theme.textPrimary), label);
	const ImVec2 trackMin{ position.x + size.x - 64.0f,
		position.y + (size.y - 24.0f) * 0.5f };
	const ImVec2 trackMax{ trackMin.x + 48.0f, trackMin.y + 24.0f };
	draw->AddRectFilled(trackMin, trackMax,
		ColorWithAlpha(*value ? theme.cursorNormal : theme.textSecondary,
			*value ? 0.85f : 0.28f), 12.0f, ImDrawFlags_RoundCornersAll);
	draw->AddCircleFilled({ *value ? trackMax.x - 12.0f : trackMin.x + 12.0f,
		(trackMin.y + trackMax.y) * 0.5f }, 9.0f, Color(theme.textPrimary), 24);
	draw->PopClipRect();
	return pressed;
}

bool ShellCombo(const char* label, std::size_t* selected,
	std::span<const ChoiceItem> choices, ImVec2 popupItemSize)
{
	if (!selected || choices.empty())
		return false;
	*selected = std::min(*selected, choices.size() - 1);
	const std::string buttonText = std::string(label) + ": " +
		choices[*selected].label + "###choice-button";
	bool changed = false;
	ImGui::PushID(label);
	if (ShellButton(buttonText.c_str()))
		ImGui::OpenPopup("choices");
	if (const ImGuiViewport* viewport = ImGui::GetMainViewport())
	{
		const float width = std::max(1.0f, popupItemSize.x + 24.0f);
		const float maximumHeight = std::max(96.0f, viewport->WorkSize.y - 48.0f);
		ImGui::SetNextWindowSizeConstraints({ width, 0.0f },
			{ width, maximumHeight });
	}
	if (BeginShellPopup("choices"))
	{
		std::size_t defaultIndex = *selected;
		if (!choices[defaultIndex].enabled)
		{
			const auto enabled = std::find_if(choices.begin(), choices.end(),
				[](const ChoiceItem& choice) { return choice.enabled; });
			if (enabled != choices.end())
				defaultIndex = static_cast<std::size_t>(enabled - choices.begin());
		}
		for (std::size_t index = 0; index < choices.size(); ++index)
		{
			const ChoiceItem& choice = choices[index];
			if (!choice.enabled)
				ImGui::BeginDisabled();
			if (ShellSelectableWithId(choice.id.c_str(), choice.label.c_str(),
				*selected == index, popupItemSize) && choice.enabled)
			{
				*selected = index;
				changed = true;
				ImGui::CloseCurrentPopup();
			}
			if (!choice.enabled)
				ImGui::EndDisabled();
			if (defaultIndex == index && choice.enabled)
				ImGui::SetItemDefaultFocus();
		}
		EndShellPopup();
	}
	ImGui::PopID();
	return changed;
}

static bool ShellSliderFloatWithStep(const char* label, float* value,
	float minValue, float maxValue, const char* format, float navigationStep)
{
	if (!value)
		return false;
	const auto& theme = CurrentWidgetTheme();
	const ImVec2 size = ResolveControlSize(ImVec2(0.0f, 58.0f), 58.0f);
	const ImVec2 position = ImGui::GetCursorScreenPos();
	ImGui::PushID(label);
	(void)ApplyPendingWidgetFocus();
	NavigableInvisibleButton("slider", size);
	const bool hovered = ImGui::IsItemHovered();
	const bool focused = ImGui::IsItemFocused();
	const bool active = ImGui::IsItemActive();
	bool changed = false;
	if (active && ImGui::IsMouseDown(ImGuiMouseButton_Left))
	{
		const float t = std::clamp((ImGui::GetIO().MousePos.x - (position.x + 18.0f)) /
			std::max(1.0f, size.x - 36.0f), 0.0f, 1.0f);
		const float next = minValue + (maxValue - minValue) * t;
		if (std::abs(*value - next) > 0.0001f)
		{
			*value = next;
			changed = true;
		}
	}
	const bool leftPressed = RetainedLeftPressed(focused);
	const bool rightPressed = RetainedRightPressed(focused);
	CancelHorizontalNavigationMove(focused, leftPressed, rightPressed);
	if (leftPressed || rightPressed)
	{
		const float step = navigationStep > 0.0f ? navigationStep :
			std::max(0.0001f, maxValue - minValue) / 40.0f;
		const float next = std::clamp(*value + (rightPressed ? step : -step),
			minValue, maxValue);
		if (std::abs(*value - next) > 0.0001f)
		{
			*value = next;
			changed = true;
		}
	}
	ImGui::PopID();

	ImDrawList* draw = ImGui::GetWindowDrawList();
	draw->PushClipRect({ position.x - 8.0f, position.y - 8.0f },
		{ position.x + size.x + 8.0f, position.y + size.y + 8.0f }, true);
	DrawShellPanelPx(draw, position, { position.x + size.x, position.y + size.y },
		theme, 14.0f, hovered || focused ? 0.70f : 0.48f);
	DrawReplicaFocus(draw, position, { position.x + size.x, position.y + size.y },
		14.0f, focused, hovered);
	char valueText[64]{};
	std::snprintf(valueText, sizeof(valueText), format ? format : "%.2f", *value);
	DrawClippedText(draw, { position.x + 16.0f, position.y + 2.0f },
		{ position.x + size.x * 0.62f, position.y + 28.0f },
		Color(theme.textPrimary), label);
	const ImVec2 valueSize = ImGui::CalcTextSize(valueText);
	draw->AddText({ position.x + size.x - valueSize.x - 16.0f,
		position.y + std::max(2.0f, (28.0f - valueSize.y) * 0.5f) },
		Color(theme.textSecondary), valueText);
	const float trackY = position.y + size.y - 15.0f;
	const float trackMin = position.x + 18.0f;
	const float trackMax = position.x + size.x - 18.0f;
	const float t = std::clamp((*value - minValue) /
		std::max(0.0001f, maxValue - minValue), 0.0f, 1.0f);
	draw->AddRectFilled({ trackMin, trackY - 3.0f }, { trackMax, trackY + 3.0f },
		ColorWithAlpha(theme.textSecondary, 0.28f), 3.0f,
		ImDrawFlags_RoundCornersAll);
	draw->AddRectFilled({ trackMin, trackY - 3.0f },
		{ trackMin + (trackMax - trackMin) * t, trackY + 3.0f },
		Color(theme.cursorNormal), 3.0f, ImDrawFlags_RoundCornersAll);
	draw->AddCircleFilled({ trackMin + (trackMax - trackMin) * t, trackY },
		8.0f, Color(theme.pageIndicatorActive), 24);
	draw->PopClipRect();
	return changed;
}

bool ShellSliderFloat(const char* label, float* value, float minValue,
	float maxValue, const char* format)
{
	return ShellSliderFloatWithStep(label, value, minValue, maxValue, format, 0.0f);
}

bool ShellSliderInt(const char* label, int* value, int minValue, int maxValue,
	const char* format)
{
	if (!value)
		return false;
	float current = static_cast<float>(*value);
	std::string floatFormat = format ? format : "%d";
	const std::size_t conversion = floatFormat.find("%d");
	if (conversion == std::string::npos)
		floatFormat = "%.0f";
	else
		floatFormat.replace(conversion, 2, "%.0f");
	const bool changed = ShellSliderFloatWithStep(label, &current,
		static_cast<float>(minValue), static_cast<float>(maxValue),
		floatFormat.c_str(), 1.0f);
	if (changed)
		*value = std::clamp(static_cast<int>(std::round(current)), minValue, maxValue);
	return changed;
}

bool ShellIntegerStepper(const char* label, int* value, int minValue,
	int maxValue, int step, const char* format)
{
	if (!value || minValue > maxValue)
		return false;
	step = std::max(1, step);
	*value = std::clamp(*value, minValue, maxValue);
	const auto& theme = CurrentWidgetTheme();
	const ImVec2 size = ResolveControlSize({ 0.0f, 48.0f });
	const ImVec2 position = ImGui::GetCursorScreenPos();
	ImGui::PushID(label);
	(void)ApplyPendingWidgetFocus();
	NavigableInvisibleButton("stepper", size);
	const bool hovered = ImGui::IsItemHovered();
	const bool focused = ImGui::IsItemFocused();
	const bool active = ImGui::IsItemActivated() || RetainedActivatePressed(focused);
	const bool left = RetainedLeftPressed(focused);
	const bool right = RetainedRightPressed(focused);
	CancelHorizontalNavigationMove(focused, left, right);
	bool changed = false;
	int direction = right || active ? 1 : (left ? -1 : 0);
	if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
		direction = ImGui::GetIO().MousePos.x < position.x + size.x * 0.5f ? -1 : 1;
	if (direction != 0)
	{
		const int next = std::clamp(*value + direction * step, minValue, maxValue);
		changed = next != *value;
		*value = next;
	}
	ImGui::PopID();

	char valueText[64]{};
	std::snprintf(valueText, sizeof(valueText), format ? format : "%d", *value);
	ImDrawList* draw = ImGui::GetWindowDrawList();
	draw->PushClipRect({ position.x - 8.0f, position.y - 8.0f },
		{ position.x + size.x + 8.0f, position.y + size.y + 8.0f }, true);
	DrawShellPanelPx(draw, position, { position.x + size.x, position.y + size.y },
		theme, 14.0f, hovered || focused ? 0.70f : 0.48f);
	DrawReplicaFocus(draw, position, { position.x + size.x, position.y + size.y },
		14.0f, focused, hovered);
	DrawClippedText(draw, { position.x + 16.0f, position.y },
		{ position.x + size.x - 132.0f, position.y + size.y },
		Color(theme.textPrimary), label);
	const ImVec2 valueSize = ImGui::CalcTextSize(valueText);
	const float valueCenter = position.x + size.x - 64.0f;
	draw->AddText({ valueCenter - valueSize.x * 0.5f,
		position.y + (size.y - valueSize.y) * 0.5f },
		Color(theme.textPrimary), valueText);
	draw->AddText({ position.x + size.x - 116.0f,
		position.y + (size.y - ImGui::GetTextLineHeight()) * 0.5f },
		Color(theme.textSecondary), "<");
	draw->AddText({ position.x + size.x - 22.0f,
		position.y + (size.y - ImGui::GetTextLineHeight()) * 0.5f },
		Color(theme.textSecondary), ">");
	draw->PopClipRect();
	return changed;
}

bool ShellColorEdit(const char* label, ImVec4* color, bool editAlpha)
{
	if (!color)
		return false;
	bool changed = false;
	ImGui::PushID(label);
	const ImVec2 buttonPosition = ImGui::GetCursorScreenPos();
	if (ShellButton((std::string(label) + "##color-button").c_str()))
		ImGui::OpenPopup("color-editor");
	const ImVec2 buttonMax = ImGui::GetItemRectMax();
	ImGui::GetWindowDrawList()->AddRectFilled(
		{ buttonMax.x - 54.0f, buttonPosition.y + 10.0f },
		{ buttonMax.x - 18.0f, buttonMax.y - 10.0f }, Color(*color), 8.0f);
	if (BeginShellPopup("color-editor"))
	{
		const bool appearing = ImGui::IsWindowAppearing();
		if (appearing)
			ImGui::SetKeyboardFocusHere();
		changed |= ShellSliderFloat("Red", &color->x, 0.0f, 1.0f, "%.2f");
		changed |= ShellSliderFloat("Green", &color->y, 0.0f, 1.0f, "%.2f");
		changed |= ShellSliderFloat("Blue", &color->z, 0.0f, 1.0f, "%.2f");
		if (editAlpha)
			changed |= ShellSliderFloat("Alpha", &color->w, 0.0f, 1.0f, "%.2f");
		if (ShellButton(Translate("Done"), { 180.0f, 42.0f }))
			ImGui::CloseCurrentPopup();
		EndShellPopup();
	}
	ImGui::PopID();
	return changed;
}

void ShellProgressBar(float fraction, std::string_view overlay, ImVec2 size)
{
	size = ResolveControlSize(size, 18.0f);
	const ImVec2 position = ImGui::GetCursorScreenPos();
	const auto& theme = CurrentWidgetTheme();
	fraction = std::clamp(fraction, 0.0f, 1.0f);
	ImDrawList* draw = ImGui::GetWindowDrawList();
	draw->AddRectFilled(position, { position.x + size.x, position.y + size.y },
		ColorWithAlpha(theme.textSecondary, 0.22f), size.y * 0.5f);
	if (fraction > 0.0f)
	{
		draw->AddRectFilled(position,
			{ position.x + size.x * fraction, position.y + size.y },
			Color(theme.cursorNormal), size.y * 0.5f);
	}
	if (!overlay.empty())
	{
		DrawClippedText(draw, position,
			{ position.x + size.x, position.y + size.y },
			Color(theme.textPrimary), std::string(overlay).c_str(), true);
	}
	ImGui::Dummy(size);
}

void SectionTitle(const char* title)
{
	const auto& theme = CurrentWidgetTheme();
	ImGui::Dummy(ImVec2(1.0f, 6.0f));
	const ImVec2 position = ImGui::GetCursorScreenPos();
	const ImVec2 available = ImGui::GetContentRegionAvail();
	ImDrawList* draw = ImGui::GetWindowDrawList();
	draw->AddText(position, Color(theme.textPrimary), title);
	const float y = position.y + ImGui::GetTextLineHeight() + 8.0f;
	draw->AddRectFilled({ position.x, y }, { position.x + available.x, y + 2.0f },
		ColorWithAlpha(theme.cursorNormal, 0.22f), 1.0f);
	ImGui::Dummy(ImVec2(std::max(1.0f, available.x),
		ImGui::GetTextLineHeight() + 14.0f));
}

void PushGameplayDialogStyle()
{
	const auto& theme = CurrentWidgetTheme();
	ImVec4 background = theme.panelBase;
	background.w = theme.dark ? 0.97f : 0.95f;
	ImVec4 border = theme.panelBorder;
	border.w = std::max(border.w, theme.dark ? 0.40f : 0.22f);
	ImGui::PushStyleColor(ImGuiCol_Text, theme.textPrimary);
	ImGui::PushStyleColor(ImGuiCol_TextDisabled, theme.textSecondary);
	ImGui::PushStyleColor(ImGuiCol_WindowBg, background);
	ImGui::PushStyleColor(ImGuiCol_Border, border);
	ImGui::PushStyleColor(ImGuiCol_FrameBg,
		WithAlpha(theme.panelBase, theme.dark ? 0.72f : 0.82f));
	ImGui::PushStyleColor(ImGuiCol_FrameBgHovered,
		WithAlpha(theme.cursorNormal, 0.28f));
	ImGui::PushStyleColor(ImGuiCol_FrameBgActive,
		WithAlpha(theme.cursorNormal, 0.40f));
	ImGui::PushStyleColor(ImGuiCol_Button,
		WithAlpha(theme.panelBase, theme.dark ? 0.76f : 0.86f));
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
		WithAlpha(theme.cursorNormal, 0.34f));
	ImGui::PushStyleColor(ImGuiCol_ButtonActive,
		WithAlpha(theme.cursorNormal, 0.48f));
	ImGui::PushStyleColor(ImGuiCol_NavHighlight,
		WithAlpha(theme.cursorNormal, 0.72f));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(22.0f, 18.0f));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 16.0f);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(13.0f, 9.0f));
	ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 10.0f);
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(10.0f, 9.0f));
	ImGui::PushStyleVar(ImGuiStyleVar_ItemInnerSpacing, ImVec2(8.0f, 6.0f));
}

void PopGameplayDialogStyle()
{
	ImGui::PopStyleVar(7);
	ImGui::PopStyleColor(11);
}
}
