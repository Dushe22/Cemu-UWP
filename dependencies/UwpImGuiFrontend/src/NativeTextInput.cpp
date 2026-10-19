#include "UwpImGuiFrontend/NativeTextInput.h"
#include "UwpImGuiFrontend/Widgets.h"

#include <imgui_internal.h>

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.Text.Core.h>
#include <winrt/Windows.UI.ViewManagement.Core.h>
#include <winrt/Windows.UI.ViewManagement.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <climits>
#include <cmath>
#include <cstring>
#include <fmt/format.h>
#include <string>
#include <utility>

namespace UwpImGuiFrontend
{
namespace
{
namespace TextCore = winrt::Windows::UI::Text::Core;
namespace ViewCore = winrt::Windows::UI::ViewManagement::Core;
namespace ViewManagement = winrt::Windows::UI::ViewManagement;

struct NativeTextState
{
	NativeTextInputConfiguration configuration;
	bool initialized = false;
	bool keyboardShown = false;
	bool keyboardFailureLogged = false;
	bool contextFailureLogged = false;
	std::atomic_bool coreViewVisible{ false };
	bool contextFocused = false;
	bool currentSessionSawKeyboard = false;
	bool editAuthorized = false;
	bool itemFocusedThisFrame = false;
	int blurFrames = 0;
	ImGuiID itemId = 0;
	bool textDirty = false;
	bool selectionDirty = false;
	bool submitRequested = false;
	std::size_t bufferSize = 0;
	ImGuiID dismissedItemId = 0;
	ImGuiID deactivateItemId = 0;
	ImGuiID caretItemId = 0;
	std::int32_t caretPosition = -1;
	double caretBlinkStart = 0.0;
	TextCore::CoreTextEditContext editContext{ nullptr };
	ViewManagement::InputPane inputPane{ nullptr };
	ViewCore::CoreInputView coreInputView{ nullptr };
	winrt::event_token coreInputViewOcclusionsChangedToken{};
	std::wstring text;
	TextCore::CoreTextRange selection{ 0, 0 };
	std::int32_t selectionAnchor = -1;
	ImVec2 inputScreenPosition{};
	float inputLineHeight = 24.0f;
	std::uint64_t contextGeneration = 0;
	ImGuiID contextItemId = 0;
};

NativeTextState s_state;

void Log(LogLevel level, std::string message)
{
	if (s_state.configuration.log)
		s_state.configuration.log(level, message);
}

void SetInputCapture(bool active)
{
	if (s_state.configuration.setInputCapture)
		s_state.configuration.setInputCapture(active);
}

std::int32_t ClampWidePosition(std::wstring_view text, std::int32_t position)
{
	const std::int32_t length = static_cast<std::int32_t>(
		std::min<std::size_t>(text.size(), INT32_MAX));
	position = std::clamp(position, 0, length);
	if (position > 0 && position < length)
	{
		const wchar_t before = text[static_cast<std::size_t>(position) - 1];
		const wchar_t after = text[static_cast<std::size_t>(position)];
		if (before >= 0xd800 && before <= 0xdbff &&
			after >= 0xdc00 && after <= 0xdfff)
		{
			--position;
		}
	}
	return position;
}

bool IsWideBoundary(std::wstring_view text, std::int32_t position)
{
	if (position < 0 || static_cast<std::size_t>(position) > text.size())
		return false;
	if (position == 0 || static_cast<std::size_t>(position) == text.size())
		return true;
	const wchar_t before = text[static_cast<std::size_t>(position) - 1];
	const wchar_t after = text[static_cast<std::size_t>(position)];
	return !(before >= 0xd800 && before <= 0xdbff &&
		after >= 0xdc00 && after <= 0xdfff);
}

bool IsValidRange(std::wstring_view text, TextCore::CoreTextRange range)
{
	return range.StartCaretPosition >= 0 &&
		range.StartCaretPosition <= range.EndCaretPosition &&
		static_cast<std::size_t>(range.EndCaretPosition) <= text.size() &&
		IsWideBoundary(text, range.StartCaretPosition) &&
		IsWideBoundary(text, range.EndCaretPosition);
}

TextCore::CoreTextRange ClampSelection(std::wstring_view text,
	TextCore::CoreTextRange selection)
{
	const std::int32_t start = ClampWidePosition(text,
		selection.StartCaretPosition);
	const std::int32_t end = ClampWidePosition(text,
		selection.EndCaretPosition);
	selection.StartCaretPosition = std::min(start, end);
	selection.EndCaretPosition = std::max(start, end);
	return selection;
}

struct TextDelta
{
	std::int32_t start = 0;
	std::int32_t oldEnd = 0;
	std::int32_t newEnd = 0;

	[[nodiscard]] bool HasChanges() const noexcept
	{
		return start != oldEnd || start != newEnd;
	}
};

TextDelta FindMinimalTextDelta(std::wstring_view oldText,
	std::wstring_view newText)
{
	std::size_t start = 0;
	const std::size_t sharedLength = std::min(oldText.size(), newText.size());
	while (start < sharedLength && oldText[start] == newText[start])
		++start;
	while (start > 0 && (!IsWideBoundary(oldText, static_cast<std::int32_t>(start)) ||
		!IsWideBoundary(newText, static_cast<std::int32_t>(start))))
	{
		--start;
	}
	std::size_t oldEnd = oldText.size();
	std::size_t newEnd = newText.size();
	while (oldEnd > start && newEnd > start &&
		oldText[oldEnd - 1] == newText[newEnd - 1])
	{
		--oldEnd;
		--newEnd;
	}
	while ((!IsWideBoundary(oldText, static_cast<std::int32_t>(oldEnd)) ||
		!IsWideBoundary(newText, static_cast<std::int32_t>(newEnd))) &&
		oldEnd < oldText.size() && newEnd < newText.size())
	{
		++oldEnd;
		++newEnd;
	}
	return {
		static_cast<std::int32_t>(std::min<std::size_t>(start, INT32_MAX)),
		static_cast<std::int32_t>(std::min<std::size_t>(oldEnd, INT32_MAX)),
		static_cast<std::int32_t>(std::min<std::size_t>(newEnd, INT32_MAX)),
	};
}

std::wstring Utf8ToWide(std::string_view text)
{
	const winrt::hstring wide = winrt::to_hstring(std::string(text));
	return std::wstring(wide.c_str(), wide.size());
}

std::string WideToUtf8(std::wstring_view text)
{
	return winrt::to_string(winrt::hstring(text));
}

std::int32_t Utf8OffsetToWidePosition(std::string_view text,
	std::int32_t byteOffset)
{
	std::size_t bytes = std::min<std::size_t>(
		static_cast<std::size_t>(std::max(byteOffset, 0)), text.size());
	while (bytes > 0 && bytes < text.size() &&
		(static_cast<unsigned char>(text[bytes]) & 0xc0) == 0x80)
	{
		--bytes;
	}
	return static_cast<std::int32_t>(std::min<std::size_t>(
		Utf8ToWide(text.substr(0, bytes)).size(), INT32_MAX));
}

int WidePositionToUtf8Offset(std::wstring_view text, std::int32_t position)
{
	position = ClampWidePosition(text, position);
	return static_cast<int>(std::min<std::size_t>(
		WideToUtf8(text.substr(0, static_cast<std::size_t>(position))).size(),
		INT_MAX));
}

std::string TruncateUtf8(std::string text, std::size_t bufferSize)
{
	if (bufferSize == 0)
		return {};
	if (text.size() < bufferSize)
		return text;
	std::size_t bytes = bufferSize - 1;
	while (bytes > 0 &&
		(static_cast<unsigned char>(text[bytes]) & 0xc0) == 0x80)
	{
		--bytes;
	}
	text.resize(bytes);
	return text;
}

bool CopyContextToBuffer(char* buffer, std::size_t bufferSize)
{
	if (!buffer || bufferSize == 0)
		return false;
	const std::string text = TruncateUtf8(WideToUtf8(s_state.text), bufferSize);
	if (std::strncmp(buffer, text.c_str(), bufferSize) == 0)
		return false;
	std::strncpy(buffer, text.c_str(), bufferSize - 1);
	buffer[bufferSize - 1] = '\0';
	return true;
}

void LogFailure(const winrt::hresult_error& error, std::string_view action)
{
	if (s_state.contextFailureLogged)
		return;
	Log(LogLevel::Error, fmt::format(
		"Native text input {} failed: 0x{:08x} {}", action,
		static_cast<std::uint32_t>(error.code().value),
		winrt::to_string(error.message())));
	s_state.contextFailureLogged = true;
}

void SetSnapshot(std::string_view text, TextCore::CoreTextRange selection)
{
	if (s_state.textDirty || s_state.selectionDirty)
		return;
	const std::wstring nextText = Utf8ToWide(text);
	selection = ClampSelection(nextText, selection);
	const bool textChanged = s_state.text != nextText;
	const bool selectionChanged =
		s_state.selection.StartCaretPosition != selection.StartCaretPosition ||
		s_state.selection.EndCaretPosition != selection.EndCaretPosition;
	if (!textChanged && !selectionChanged)
		return;
	const TextDelta delta = FindMinimalTextDelta(s_state.text, nextText);
	s_state.text = nextText;
	s_state.selection = selection;
	s_state.selectionAnchor = -1;
	if (!s_state.editContext || !s_state.contextFocused)
		return;
	try
	{
		if (textChanged)
		{
			s_state.editContext.NotifyTextChanged({ delta.start, delta.oldEnd },
				delta.newEnd - delta.start, s_state.selection);
		}
		else
		{
			s_state.editContext.NotifySelectionChanged(s_state.selection);
		}
	}
	catch (const winrt::hresult_error& error)
	{
		LogFailure(error, "synchronization");
	}
}

std::int32_t PreviousPosition(std::wstring_view text, std::int32_t position)
{
	position = ClampWidePosition(text, position);
	if (position <= 0)
		return 0;
	--position;
	if (position > 0)
	{
		const wchar_t current = text[static_cast<std::size_t>(position)];
		const wchar_t previous = text[static_cast<std::size_t>(position) - 1];
		if (current >= 0xdc00 && current <= 0xdfff &&
			previous >= 0xd800 && previous <= 0xdbff)
		{
			--position;
		}
	}
	return position;
}

std::int32_t NextPosition(std::wstring_view text, std::int32_t position)
{
	const std::int32_t length = static_cast<std::int32_t>(
		std::min<std::size_t>(text.size(), INT32_MAX));
	position = ClampWidePosition(text, position);
	if (position >= length)
		return length;
	if (position + 1 < length)
	{
		const wchar_t current = text[static_cast<std::size_t>(position)];
		const wchar_t next = text[static_cast<std::size_t>(position) + 1];
		if (current >= 0xd800 && current <= 0xdbff &&
			next >= 0xdc00 && next <= 0xdfff)
		{
			return position + 2;
		}
	}
	return position + 1;
}

TextCore::CoreTextRange MoveSelection(TextCore::CoreTextRange selection,
	bool towardStart, bool moveToBoundary, bool extendSelection)
{
	selection = ClampSelection(s_state.text, selection);
	const std::int32_t textLength = static_cast<std::int32_t>(
		std::min<std::size_t>(s_state.text.size(), INT32_MAX));
	const bool hasSelection =
		selection.StartCaretPosition != selection.EndCaretPosition;
	if (!extendSelection)
	{
		s_state.selectionAnchor = -1;
		std::int32_t caret = selection.EndCaretPosition;
		if (moveToBoundary)
			caret = towardStart ? 0 : textLength;
		else if (hasSelection)
			caret = towardStart ? selection.StartCaretPosition :
				selection.EndCaretPosition;
		else
			caret = towardStart ? PreviousPosition(s_state.text, caret) :
				NextPosition(s_state.text, caret);
		return { caret, caret };
	}

	std::int32_t activeCaret = selection.EndCaretPosition;
	if (s_state.selectionAnchor < 0 || s_state.selectionAnchor > textLength ||
		(hasSelection && s_state.selectionAnchor != selection.StartCaretPosition &&
			s_state.selectionAnchor != selection.EndCaretPosition))
	{
		if (hasSelection)
		{
			s_state.selectionAnchor = towardStart ? selection.EndCaretPosition :
				selection.StartCaretPosition;
			activeCaret = towardStart ? selection.StartCaretPosition :
				selection.EndCaretPosition;
		}
		else
		{
			s_state.selectionAnchor = selection.StartCaretPosition;
			activeCaret = selection.StartCaretPosition;
		}
	}
	else if (hasSelection)
	{
		activeCaret = s_state.selectionAnchor == selection.StartCaretPosition ?
			selection.EndCaretPosition : selection.StartCaretPosition;
	}
	if (moveToBoundary)
		activeCaret = towardStart ? 0 : textLength;
	else
		activeCaret = towardStart ? PreviousPosition(s_state.text, activeCaret) :
			NextPosition(s_state.text, activeCaret);
	return { std::min(s_state.selectionAnchor, activeCaret),
		std::max(s_state.selectionAnchor, activeCaret) };
}

void ReleasePaneHandlers()
{
	try
	{
		if (s_state.coreInputView &&
			s_state.coreInputViewOcclusionsChangedToken.value != 0)
		{
			s_state.coreInputView.OcclusionsChanged(
				s_state.coreInputViewOcclusionsChangedToken);
		}
	}
	catch (const winrt::hresult_error&)
	{
	}
	s_state.coreInputViewOcclusionsChangedToken = {};
	s_state.inputPane = nullptr;
	s_state.coreInputView = nullptr;
	s_state.coreViewVisible.store(false, std::memory_order_release);
	s_state.currentSessionSawKeyboard = false;
}

void EnsurePaneHandlers()
{
	if (s_state.inputPane && s_state.coreInputView &&
		s_state.coreInputViewOcclusionsChangedToken.value != 0)
	{
		return;
	}
	try
	{
		if (!s_state.inputPane)
			s_state.inputPane = ViewManagement::InputPane::GetForCurrentView();
		if (!s_state.coreInputView)
			s_state.coreInputView = ViewCore::CoreInputView::GetForCurrentView();
		if (s_state.coreInputViewOcclusionsChangedToken.value == 0)
		{
			s_state.coreInputViewOcclusionsChangedToken =
				s_state.coreInputView.OcclusionsChanged(
					[](auto&&, const ViewCore::CoreInputViewOcclusionsChangedEventArgs& args) {
						const bool visible = args.Occlusions().Size() != 0;
						s_state.coreViewVisible.store(visible,
							std::memory_order_release);
					});
		}
	}
	catch (const winrt::hresult_error& error)
	{
		LogFailure(error, "pane event registration");
	}
}

bool EnsureEditContext()
{
	if (s_state.editContext)
	{
		EnsurePaneHandlers();
		return true;
	}
	try
	{
		const auto manager = TextCore::CoreTextServicesManager::GetForCurrentView();
		const std::uint64_t generation = ++s_state.contextGeneration;
		const ImGuiID ownerItemId = s_state.itemId;
		s_state.editContext = manager.CreateEditContext();
		s_state.contextItemId = ownerItemId;
		s_state.editContext.Name(winrt::to_hstring(s_state.configuration.contextName));
		s_state.editContext.InputScope(TextCore::CoreTextInputScope::Text);
		s_state.editContext.InputPaneDisplayPolicy(
			TextCore::CoreTextInputPaneDisplayPolicy::Manual);
		s_state.editContext.TextRequested(
			[generation, ownerItemId](auto&&,
				const TextCore::CoreTextTextRequestedEventArgs& args) {
				if (generation != s_state.contextGeneration ||
					ownerItemId != s_state.contextItemId)
					return;
				const auto request = args.Request();
				const auto range = request.Range();
				const std::int32_t length = static_cast<std::int32_t>(
					std::min<std::size_t>(s_state.text.size(), INT32_MAX));
				const std::int32_t start = std::clamp(range.StartCaretPosition, 0, length);
				const std::int32_t end = std::clamp(range.EndCaretPosition, start, length);
				request.Text(winrt::hstring(s_state.text.substr(
					static_cast<std::size_t>(start),
					static_cast<std::size_t>(end - start))));
			});
		s_state.editContext.SelectionRequested(
			[generation, ownerItemId](auto&&,
				const TextCore::CoreTextSelectionRequestedEventArgs& args) {
				if (generation != s_state.contextGeneration ||
					ownerItemId != s_state.contextItemId)
					return;
				args.Request().Selection(s_state.selection);
			});
		s_state.editContext.LayoutRequested(
			[generation, ownerItemId](auto&&,
				const TextCore::CoreTextLayoutRequestedEventArgs& args) {
				if (generation != s_state.contextGeneration ||
					ownerItemId != s_state.contextItemId)
					return;
				const winrt::Windows::Foundation::Rect rect{
					s_state.inputScreenPosition.x, s_state.inputScreenPosition.y,
					1.0f, std::max(1.0f, s_state.inputLineHeight) };
				const auto bounds = args.Request().LayoutBounds();
				bounds.TextBounds(rect);
				bounds.ControlBounds(rect);
			});
		s_state.editContext.TextUpdating(
			[generation, ownerItemId](auto&&,
				const TextCore::CoreTextTextUpdatingEventArgs& args) {
				const auto range = args.Range();
				if (generation != s_state.contextGeneration ||
					ownerItemId != s_state.contextItemId ||
					s_state.itemId != ownerItemId || s_state.bufferSize == 0 ||
					!IsValidRange(s_state.text, range))
				{
					args.Result(TextCore::CoreTextTextUpdatingResult::Failed);
					return;
				}
				std::wstring updated = s_state.text;
				updated.replace(static_cast<std::size_t>(range.StartCaretPosition),
					static_cast<std::size_t>(range.EndCaretPosition -
						range.StartCaretPosition), std::wstring(args.Text()));
				const auto selection = args.NewSelection();
				if (!IsValidRange(updated, selection) ||
					WideToUtf8(updated).size() >= s_state.bufferSize)
				{
					args.Result(TextCore::CoreTextTextUpdatingResult::Failed);
					return;
				}
				s_state.text = std::move(updated);
				s_state.selection = selection;
				s_state.selectionAnchor = -1;
				s_state.textDirty = true;
				s_state.selectionDirty = true;
				args.Result(TextCore::CoreTextTextUpdatingResult::Succeeded);
			});
		s_state.editContext.SelectionUpdating(
			[generation, ownerItemId](auto&&,
				const TextCore::CoreTextSelectionUpdatingEventArgs& args) {
				const auto selection = args.Selection();
				if (generation != s_state.contextGeneration ||
					ownerItemId != s_state.contextItemId ||
					s_state.itemId != ownerItemId ||
					!IsValidRange(s_state.text, selection))
				{
					args.Result(TextCore::CoreTextSelectionUpdatingResult::Failed);
					return;
				}
				s_state.selection = selection;
				s_state.selectionAnchor = -1;
				s_state.selectionDirty = true;
				args.Result(TextCore::CoreTextSelectionUpdatingResult::Succeeded);
			});
		s_state.editContext.CompositionStarted([](auto&&, auto&&) {});
		s_state.editContext.CompositionCompleted([](auto&&, auto&&) {});
		s_state.editContext.FocusRemoved(
			[generation, ownerItemId](auto&&, auto&&) {
			if (generation != s_state.contextGeneration ||
				ownerItemId != s_state.contextItemId)
				return;
			s_state.dismissedItemId = ownerItemId;
			s_state.deactivateItemId = ownerItemId;
			s_state.contextFocused = false;
			s_state.editAuthorized = false;
			s_state.selectionAnchor = -1;
			s_state.keyboardShown = false;
			s_state.currentSessionSawKeyboard = false;
			s_state.editContext = nullptr;
			s_state.contextItemId = 0;
			++s_state.contextGeneration;
			SetInputCapture(false);
		});
		EnsurePaneHandlers();
		s_state.contextFailureLogged = false;
		return true;
	}
	catch (const winrt::hresult_error& error)
	{
		LogFailure(error, "context creation");
		return false;
	}
}

TextCore::CoreTextInputScope ResolveScope(TextInputScope scope,
	const char* label, ImGuiInputTextFlags flags)
{
	if (scope == TextInputScope::Password ||
		(flags & ImGuiInputTextFlags_Password) != 0)
		return TextCore::CoreTextInputScope::Password;
	if (scope == TextInputScope::Url)
		return TextCore::CoreTextInputScope::Url;
	if (scope == TextInputScope::UserName)
		return TextCore::CoreTextInputScope::UserName;
	if (scope == TextInputScope::Text)
		return TextCore::CoreTextInputScope::Text;
	std::string lower = label ? label : "";
	std::transform(lower.begin(), lower.end(), lower.begin(),
		[](unsigned char value) { return static_cast<char>(std::tolower(value)); });
	if (lower.find("url") != std::string::npos ||
		lower.find("host") != std::string::npos)
		return TextCore::CoreTextInputScope::Url;
	if (lower.find("user") != std::string::npos)
		return TextCore::CoreTextInputScope::UserName;
	return TextCore::CoreTextInputScope::Text;
}

bool FocusEditContext(TextCore::CoreTextInputScope scope)
{
	if (!EnsureEditContext())
		return false;
	try
	{
		s_state.editContext.InputScope(scope);
		if (!s_state.contextFocused)
		{
			s_state.editContext.NotifyFocusEnter();
			s_state.contextFocused = true;
			SetInputCapture(true);
		}
		s_state.editContext.NotifyLayoutChanged();
		s_state.contextFailureLogged = false;
		return true;
	}
	catch (const winrt::hresult_error& error)
	{
		LogFailure(error, "focus");
		return false;
	}
}

void ReleaseFocus()
{
	const auto context = s_state.editContext;
	const bool wasFocused = s_state.contextFocused;
	// invalidate callbacks first, NotifyFocusLeave can complete asynchronously
	++s_state.contextGeneration;
	s_state.editContext = nullptr;
	s_state.contextItemId = 0;
	s_state.contextFocused = false;
	s_state.currentSessionSawKeyboard = false;
	s_state.editAuthorized = false;
	s_state.selectionAnchor = -1;
	SetInputCapture(false);
	if (context && wasFocused)
	{
		try
		{
			context.NotifyFocusLeave();
		}
		catch (const winrt::hresult_error&)
		{
		}
	}
}

bool TryShowKeyboard()
{
	EnsurePaneHandlers();
	s_state.currentSessionSawKeyboard = false;
	s_state.coreViewVisible.store(false, std::memory_order_release);
	try
	{
		const auto view = s_state.coreInputView ? s_state.coreInputView :
			ViewCore::CoreInputView::GetForCurrentView();
		if (view.TryShow(ViewCore::CoreInputViewKind::Keyboard))
			return true;
	}
	catch (const winrt::hresult_error&)
	{
	}
	try
	{
		return ViewManagement::InputPane::GetForCurrentView().TryShow();
	}
	catch (const winrt::hresult_error&)
	{
		return false;
	}
}

void TryHideKeyboard()
{
	try
	{
		const auto view = s_state.coreInputView ? s_state.coreInputView :
			ViewCore::CoreInputView::GetForCurrentView();
		(void)view.TryHide();
	}
	catch (const winrt::hresult_error&)
	{
	}
	try
	{
		const auto pane = s_state.inputPane ? s_state.inputPane :
			ViewManagement::InputPane::GetForCurrentView();
		(void)pane.TryHide();
	}
	catch (const winrt::hresult_error&)
	{
	}
}

void DismissActiveTextInput()
{
	const ImGuiID itemId = s_state.itemId;
	if (itemId == 0)
		return;
	s_state.dismissedItemId = itemId;
	s_state.deactivateItemId = itemId;
	ReleaseFocus();
	TryHideKeyboard();
	s_state.keyboardShown = false;
	s_state.editAuthorized = false;
	s_state.submitRequested = false;
	s_state.blurFrames = 0;
	s_state.caretItemId = 0;
	s_state.caretPosition = -1;
	if (ImGui::GetCurrentContext() && GImGui->ActiveId == itemId)
		ImGui::ClearActiveID();
}

void RequestKeyboard(TextCore::CoreTextInputScope scope)
{
	if (s_state.contextFocused && s_state.contextItemId != s_state.itemId)
		ReleaseFocus();
	s_state.editAuthorized = true;
	s_state.dismissedItemId = 0;
	s_state.submitRequested = false;
	if (!FocusEditContext(scope))
		return;
	if (TryShowKeyboard())
	{
		s_state.keyboardShown = true;
		s_state.keyboardFailureLogged = false;
		s_state.blurFrames = 0;
		return;
	}
	if (!s_state.keyboardFailureLogged)
	{
		Log(LogLevel::Warning, "Native text input show request was rejected");
		s_state.keyboardFailureLogged = true;
	}
}

void ProcessPaneVisibility()
{
	if (!s_state.keyboardShown || !s_state.contextFocused ||
		s_state.itemId == 0)
		return;

	bool visible = s_state.coreViewVisible.load(std::memory_order_acquire);
	if (s_state.inputPane)
	{
		try
		{
			visible = visible || s_state.inputPane.Visible();
		}
		catch (const winrt::hresult_error&)
		{
		}
	}
	if (visible)
	{
		s_state.currentSessionSawKeyboard = true;
		return;
	}
	// TryShow is asynchronous, ignore a stale hidden event from the previous field
	if (!s_state.currentSessionSawKeyboard)
		return;
	DismissActiveTextInput();
}

#if IMGUI_VERSION_NUM >= 19110
void PlatformImeData(ImGuiContext*, ImGuiViewport*, ImGuiPlatformImeData* data)
#else
void PlatformImeData(ImGuiViewport*, ImGuiPlatformImeData* data)
#endif
{
	if (!data)
		return;
	s_state.inputScreenPosition = data->InputPos;
	s_state.inputLineHeight = std::max(1.0f, data->InputLineHeight);
	if (s_state.editContext && s_state.contextFocused)
	{
		try
		{
			s_state.editContext.NotifyLayoutChanged();
		}
		catch (const winrt::hresult_error&)
		{
		}
	}
	if (s_state.editAuthorized && data->WantVisible &&
		(s_state.dismissedItemId == 0 ||
		s_state.dismissedItemId != s_state.itemId))
	{
		if (s_state.contextFocused)
			s_state.blurFrames = 0;
		else
			RequestKeyboard(TextCore::CoreTextInputScope::Text);
	}
	else if (s_state.itemFocusedThisFrame)
		s_state.blurFrames = 0;
	else
		HideNativeTextInput();
}

struct InputCallbackState
{
	ImGuiID itemId = 0;
	bool invoked = false;
	bool appliedText = false;
	bool appliedContext = false;
	int cursor = 0;
	int selectionStart = 0;
	int selectionEnd = 0;
};

int InputTextCallback(ImGuiInputTextCallbackData* data)
{
	if (!data || data->EventFlag != ImGuiInputTextFlags_CallbackAlways)
		return 0;
	auto* callback = static_cast<InputCallbackState*>(data->UserData);
	if (!callback)
		return 0;
	callback->invoked = true;
	if (callback->itemId != 0 && callback->itemId == s_state.itemId &&
		(s_state.textDirty || s_state.selectionDirty))
	{
		if (s_state.textDirty)
		{
			const std::wstring current = Utf8ToWide(
				std::string_view(data->Buf, static_cast<std::size_t>(data->BufTextLen)));
			const TextDelta delta = FindMinimalTextDelta(current, s_state.text);
			if (delta.HasChanges())
			{
				const int deleteStart = WidePositionToUtf8Offset(current, delta.start);
				const int deleteEnd = WidePositionToUtf8Offset(current, delta.oldEnd);
				const std::string replacement = WideToUtf8(s_state.text.substr(
					static_cast<std::size_t>(delta.start),
					static_cast<std::size_t>(delta.newEnd - delta.start)));
				if (deleteEnd > deleteStart)
					data->DeleteChars(deleteStart, deleteEnd - deleteStart);
				if (!replacement.empty())
					data->InsertChars(deleteStart, replacement.c_str(),
						replacement.c_str() + replacement.size());
				callback->appliedText = true;
			}
		}
		const auto selection = ClampSelection(s_state.text, s_state.selection);
		data->SelectionStart = std::clamp(WidePositionToUtf8Offset(
			s_state.text, selection.StartCaretPosition), 0, data->BufTextLen);
		data->SelectionEnd = std::clamp(WidePositionToUtf8Offset(
			s_state.text, selection.EndCaretPosition), 0, data->BufTextLen);
		data->CursorPos = data->SelectionEnd;
		callback->appliedContext = true;
	}
	callback->cursor = data->CursorPos;
	callback->selectionStart = data->SelectionStart;
	callback->selectionEnd = data->SelectionEnd;
	return 0;
}

std::int32_t ActiveCaretPosition()
{
	const auto selection = ClampSelection(s_state.text, s_state.selection);
	if (selection.StartCaretPosition == selection.EndCaretPosition)
		return selection.EndCaretPosition;
	if (s_state.selectionAnchor == selection.EndCaretPosition)
		return selection.StartCaretPosition;
	return selection.EndCaretPosition;
}

std::string TextPrefixForCaret(std::int32_t caretPosition, bool password)
{
	caretPosition = ClampWidePosition(s_state.text, caretPosition);
	const std::wstring_view prefix(s_state.text.data(),
		static_cast<std::size_t>(caretPosition));
	if (!password)
		return WideToUtf8(prefix);
	std::string masked;
	masked.reserve(prefix.size());
	for (std::size_t index = 0; index < prefix.size(); ++index)
	{
		masked.push_back('*');
		if (prefix[index] >= 0xd800 && prefix[index] <= 0xdbff &&
			index + 1 < prefix.size() && prefix[index + 1] >= 0xdc00 &&
			prefix[index + 1] <= 0xdfff)
		{
			++index;
		}
	}
	return masked;
}

void DrawCaret(ImGuiID itemId, ImGuiInputTextFlags flags,
	const ImRect& frameRect)
{
	if (!s_state.contextFocused || itemId == 0 || itemId != s_state.itemId)
		return;
	const std::int32_t caretPosition = ActiveCaretPosition();
	const double now = ImGui::GetTime();
	if (s_state.caretItemId != itemId || s_state.caretPosition != caretPosition)
	{
		s_state.caretItemId = itemId;
		s_state.caretPosition = caretPosition;
		s_state.caretBlinkStart = now;
	}
	if (std::fmod(std::max(0.0, now - s_state.caretBlinkStart), 1.2) >= 0.8)
		return;
	const ImGuiStyle& style = ImGui::GetStyle();
	const std::string prefix = TextPrefixForCaret(caretPosition,
		(flags & ImGuiInputTextFlags_Password) != 0);
	float scrollX = 0.0f;
	if (const ImGuiInputTextState* inputState = ImGui::GetInputTextState(itemId))
	{
#if IMGUI_VERSION_NUM >= 19200
		scrollX = inputState->Scroll.x;
#else
		scrollX = inputState->ScrollX;
#endif
	}
	const float innerMinX = frameRect.Min.x + style.FramePadding.x;
	const float innerMaxX = std::max(innerMinX,
		frameRect.Max.x - style.FramePadding.x);
	const float caretX = std::clamp(innerMinX +
		ImGui::CalcTextSize(prefix.c_str()).x - scrollX, innerMinX,
		std::max(innerMinX, innerMaxX - 1.0f));
	const float caretTop = frameRect.Min.y + style.FramePadding.y;
	const float caretBottom = std::min(frameRect.Max.y - style.FramePadding.y,
		caretTop + ImGui::GetFontSize());
	if (caretBottom <= caretTop)
		return;
	ImDrawList* draw = ImGui::GetWindowDrawList();
	draw->PushClipRect(frameRect.Min, frameRect.Max, true);
	draw->AddLine({ std::floor(caretX) + 0.5f, std::floor(caretTop) },
		{ std::floor(caretX) + 0.5f, std::floor(caretBottom) },
		ImGui::GetColorU32(ImGuiCol_Text), 2.0f);
	draw->PopClipRect();
}

void RestoreNavigationFocus(ImGuiID itemId)
{
	ImGuiWindow* window = ImGui::GetCurrentWindow();
	if (!window || itemId == 0)
		return;
	ImGui::ClearActiveID();
	ImGui::FocusWindow(window);
	ImGui::SetFocusID(itemId, window);
	ImGuiContext& context = *GImGui;
	context.NavActivateId = 0;
	context.NavActivateDownId = 0;
	context.NavActivatePressedId = 0;
	context.NavActivateFlags = ImGuiActivateFlags_None;
	if (context.NavNextActivateId == itemId)
	{
		context.NavNextActivateId = 0;
		context.NavNextActivateFlags = ImGuiActivateFlags_None;
	}
	context.NavIdIsAlive = true;
#if IMGUI_VERSION_NUM >= 19140
	context.NavCursorVisible = true;
	context.NavHighlightItemUnderNav = true;
#else
	context.NavDisableHighlight = false;
	context.NavDisableMouseHover = true;
#endif
}
}

void InitializeNativeTextInput(NativeTextInputConfiguration configuration)
{
	s_state.configuration = std::move(configuration);
	s_state.initialized = true;
	if (ImGui::GetCurrentContext())
	{
#if IMGUI_VERSION_NUM >= 19110
		ImGui::GetPlatformIO().Platform_SetImeDataFn = PlatformImeData;
#else
		ImGui::GetIO().SetPlatformImeDataFn = PlatformImeData;
#endif
	}
}

void ShutdownNativeTextInput()
{
	HideNativeTextInput();
	ReleasePaneHandlers();
	s_state.editContext = nullptr;
	s_state.text.clear();
	s_state.selection = { 0, 0 };
	s_state.caretItemId = 0;
	s_state.caretPosition = -1;
	s_state.caretBlinkStart = 0.0;
	SetInputCapture(false);
	if (ImGui::GetCurrentContext())
	{
#if IMGUI_VERSION_NUM >= 19110
		if (ImGui::GetPlatformIO().Platform_SetImeDataFn == PlatformImeData)
			ImGui::GetPlatformIO().Platform_SetImeDataFn = nullptr;
#else
		if (ImGui::GetIO().SetPlatformImeDataFn == PlatformImeData)
		ImGui::GetIO().SetPlatformImeDataFn = nullptr;
#endif
	}
	s_state.initialized = false;
}

void BeginNativeTextInputFrame()
{
	ProcessPaneVisibility();
	s_state.itemFocusedThisFrame = false;
}

void EndNativeTextInputFrame()
{
	if (!s_state.keyboardShown && !s_state.contextFocused)
		return;
	if (s_state.itemFocusedThisFrame || ImGui::GetIO().WantTextInput)
	{
		s_state.blurFrames = 0;
		return;
	}
	if (++s_state.blurFrames >= 20)
		HideNativeTextInput();
}

bool NativeInputTextImpl(const char* label, char* buffer,
	std::size_t bufferSize, ImVec2 multilineSize, ImGuiInputTextFlags flags,
	TextInputScope scope, bool multiline, bool allowExplicitActivation)
{
	const ImGuiID itemId = ImGui::GetID(label);
	InputCallbackState callback{ .itemId = itemId };
	(void)ApplyPendingWidgetFocus();
	bool changed = multiline ?
		ImGui::InputTextMultiline(label, buffer, bufferSize, multilineSize,
			flags | ImGuiInputTextFlags_CallbackAlways, InputTextCallback, &callback) :
		ImGui::InputText(label, buffer, bufferSize,
			flags | ImGuiInputTextFlags_CallbackAlways, InputTextCallback, &callback);
	const ImRect frameRect = GImGui->LastItemData.ID == itemId ?
		GImGui->LastItemData.NavRect :
		ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
	if (callback.appliedContext)
	{
		s_state.textDirty = false;
		s_state.selectionDirty = false;
		changed |= callback.appliedText;
	}
	else if ((s_state.textDirty || s_state.selectionDirty) && itemId != 0 &&
		itemId == s_state.itemId && !ImGui::IsItemActive())
	{
		if (s_state.textDirty)
			changed |= CopyContextToBuffer(buffer, bufferSize);
		s_state.textDirty = false;
		s_state.selectionDirty = false;
	}

	if (ImGui::IsItemActive() || ImGui::IsItemFocused())
	{
		const ImGuiID previousItemId = s_state.itemId;
		const bool itemChanged = previousItemId != 0 && previousItemId != itemId;
		if (itemChanged)
		{
			s_state.textDirty = false;
			s_state.selectionDirty = false;
		}
		const bool contextOwnsSelection =
			(s_state.contextFocused && previousItemId == itemId) ||
			s_state.deactivateItemId == itemId;
		s_state.itemId = itemId;
		s_state.bufferSize = bufferSize;
		s_state.itemFocusedThisFrame = true;
		const ImVec2 itemMin = ImGui::GetItemRectMin();
		const ImVec2 itemMax = ImGui::GetItemRectMax();
		s_state.inputScreenPosition = itemMin;
		s_state.inputLineHeight = multiline ? ImGui::GetTextLineHeight() :
			std::max(1.0f, itemMax.y - itemMin.y);
		if (!callback.appliedContext && callback.invoked && !contextOwnsSelection)
		{
			const std::string_view text = buffer ? std::string_view(buffer) :
				std::string_view{};
			SetSnapshot(text, {
				Utf8OffsetToWidePosition(text, callback.selectionStart),
				Utf8OffsetToWidePosition(text, callback.selectionEnd),
			});
		}
	}
	const bool explicitRequest = allowExplicitActivation && ImGui::IsItemFocused() &&
		(ImGui::IsKeyPressed(ImGuiKey_GamepadFaceDown, false) ||
			ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
			ImGui::IsKeyPressed(ImGuiKey_Space, false) ||
			IsWidgetAcceptPressed());
	const bool activated = ImGui::IsItemActivated();
	const bool pointerActivation = activated &&
		ImGui::IsMouseClicked(ImGuiMouseButton_Left);
	if (explicitRequest || pointerActivation)
	{
		const std::string_view text = buffer ? std::string_view(buffer) :
			std::string_view{};
		const std::int32_t end = static_cast<std::int32_t>(
			std::min<std::size_t>(Utf8ToWide(text).size(), INT32_MAX));
		SetSnapshot(text, { end, end });
	}
	// controller focus alone must not summon the on-screen keyboard
	if (explicitRequest || pointerActivation)
		RequestKeyboard(ResolveScope(scope, label, flags));
	if (s_state.submitRequested && itemId == s_state.itemId)
		DismissActiveTextInput();
	// native focus suppresses ImGui's own caret on the single-line CoreText path
	if (!multiline)
		DrawCaret(itemId, flags, frameRect);
	if (s_state.deactivateItemId == itemId)
	{
		RestoreNavigationFocus(itemId);
		s_state.deactivateItemId = 0;
	}
	return changed;
}

bool NativeInputText(const char* label, char* buffer, std::size_t bufferSize,
	ImGuiInputTextFlags flags, TextInputScope scope, bool allowExplicitActivation)
{
	return NativeInputTextImpl(label, buffer, bufferSize, {}, flags, scope,
		false, allowExplicitActivation);
}

bool NativeInputTextMultiline(const char* label, char* buffer,
	std::size_t bufferSize, ImVec2 size, ImGuiInputTextFlags flags,
	TextInputScope scope, bool allowExplicitActivation)
{
	return NativeInputTextImpl(label, buffer, bufferSize, size, flags, scope,
		true, allowExplicitActivation);
}

void RequestNativeTextInput(std::string_view text, std::size_t bufferSize,
	TextInputScope scope)
{
	if (s_state.itemId == 0)
		return;
	s_state.bufferSize = bufferSize;
	const std::int32_t end = static_cast<std::int32_t>(
		std::min<std::size_t>(Utf8ToWide(text).size(), INT32_MAX));
	SetSnapshot(text, { end, end });
	RequestKeyboard(ResolveScope(scope, nullptr, 0));
}

void DismissNativeTextInput()
{
	DismissActiveTextInput();
}

void HideNativeTextInput()
{
	const ImGuiID itemId = s_state.itemId;
	ReleaseFocus();
	TryHideKeyboard();
	if (ImGui::GetCurrentContext() && itemId != 0 && GImGui->ActiveId == itemId)
		ImGui::ClearActiveID();
	s_state.keyboardShown = false;
	s_state.editAuthorized = false;
	s_state.blurFrames = 0;
	s_state.textDirty = false;
	s_state.selectionDirty = false;
	s_state.submitRequested = false;
	s_state.itemId = 0;
	s_state.bufferSize = 0;
	s_state.dismissedItemId = 0;
	s_state.deactivateItemId = 0;
	s_state.caretItemId = 0;
	s_state.caretPosition = -1;
	s_state.selectionAnchor = -1;
}

void HandleNativeTextEditingCommand(TextEditingCommand command,
	bool extendSelection)
{
	if (!s_state.contextFocused || s_state.itemId == 0 || s_state.bufferSize == 0)
		return;
	auto selection = ClampSelection(s_state.text, s_state.selection);
	const auto originalSelection = selection;
	TextCore::CoreTextRange modifiedRange = selection;
	bool textChanged = false;
	switch (command)
	{
	case TextEditingCommand::Backspace:
		if (modifiedRange.StartCaretPosition == modifiedRange.EndCaretPosition)
			modifiedRange.StartCaretPosition = PreviousPosition(s_state.text,
				modifiedRange.StartCaretPosition);
		textChanged = modifiedRange.StartCaretPosition !=
			modifiedRange.EndCaretPosition;
		break;
	case TextEditingCommand::Delete:
		if (modifiedRange.StartCaretPosition == modifiedRange.EndCaretPosition)
			modifiedRange.EndCaretPosition = NextPosition(s_state.text,
				modifiedRange.EndCaretPosition);
		textChanged = modifiedRange.StartCaretPosition !=
			modifiedRange.EndCaretPosition;
		break;
	case TextEditingCommand::MoveLeft:
		selection = MoveSelection(selection, true, false, extendSelection);
		break;
	case TextEditingCommand::MoveRight:
		selection = MoveSelection(selection, false, false, extendSelection);
		break;
	case TextEditingCommand::MoveHome:
		selection = MoveSelection(selection, true, true, extendSelection);
		break;
	case TextEditingCommand::MoveEnd:
		selection = MoveSelection(selection, false, true, extendSelection);
		break;
	case TextEditingCommand::Submit:
		s_state.submitRequested = true;
		return;
	}
	try
	{
		if (textChanged)
		{
			s_state.text.erase(static_cast<std::size_t>(modifiedRange.StartCaretPosition),
				static_cast<std::size_t>(modifiedRange.EndCaretPosition -
					modifiedRange.StartCaretPosition));
			selection = { modifiedRange.StartCaretPosition,
				modifiedRange.StartCaretPosition };
			s_state.selection = selection;
			s_state.selectionAnchor = -1;
			s_state.textDirty = true;
			s_state.selectionDirty = true;
			s_state.editContext.NotifyTextChanged(modifiedRange, 0, selection);
		}
		else if (selection.StartCaretPosition != originalSelection.StartCaretPosition ||
			selection.EndCaretPosition != originalSelection.EndCaretPosition)
		{
			s_state.selection = selection;
			s_state.selectionDirty = true;
			s_state.editContext.NotifySelectionChanged(selection);
		}
	}
	catch (const winrt::hresult_error& error)
	{
		LogFailure(error, "editing command");
	}
}

bool IsNativeTextInputActive() noexcept
{
	return s_state.contextFocused;
}
}
