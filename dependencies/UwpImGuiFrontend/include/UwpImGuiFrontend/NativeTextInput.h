#pragma once

#include "Types.h"

#include <imgui.h>

#include <cstddef>
#include <functional>
#include <string_view>

namespace UwpImGuiFrontend
{
enum class TextEditingCommand
{
	Backspace,
	Delete,
	MoveLeft,
	MoveRight,
	MoveHome,
	MoveEnd,
	Submit,
};

enum class TextInputScope
{
	Automatic,
	Text,
	Password,
	Url,
	UserName,
};

struct NativeTextInputConfiguration
{
	std::string contextName = "UWP ImGui text input";
	std::function<void(bool)> setInputCapture;
	std::function<void(LogLevel, std::string_view)> log;
};

void InitializeNativeTextInput(NativeTextInputConfiguration configuration = {});
void ShutdownNativeTextInput();
void BeginNativeTextInputFrame();
void EndNativeTextInputFrame();

[[nodiscard]] bool NativeInputText(const char* label, char* buffer,
	std::size_t bufferSize, ImGuiInputTextFlags flags = 0,
	TextInputScope scope = TextInputScope::Automatic,
	bool allowExplicitActivation = true);
[[nodiscard]] bool NativeInputTextMultiline(const char* label, char* buffer,
	std::size_t bufferSize, ImVec2 size = {}, ImGuiInputTextFlags flags = 0,
	TextInputScope scope = TextInputScope::Automatic,
	bool allowExplicitActivation = true);
void RequestNativeTextInput(std::string_view text, std::size_t bufferSize,
	TextInputScope scope = TextInputScope::Text);
void DismissNativeTextInput();
void HideNativeTextInput();
void HandleNativeTextEditingCommand(TextEditingCommand command,
	bool extendSelection = false);
[[nodiscard]] bool IsNativeTextInputActive() noexcept;
}
