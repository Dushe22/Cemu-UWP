#pragma once

#include "ImGuiShell.h"
#include "NativeTextInput.h"
#include "Types.h"
#include "Widgets.h"

#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace UwpImGuiFrontend
{
enum class SplitPane
{
	Navigation,
	Content,
};

struct SplitViewTab
{
	std::string id;
	std::string label;
	bool enabled = true;
};

struct SplitViewState
{
	std::size_t selected = 0;
	SplitPane focusedPane = SplitPane::Navigation;
	std::optional<SplitPane> requestedFocus;

	void SetItemCount(std::size_t count) noexcept;
	void Select(std::size_t index, std::size_t count) noexcept;
	void MoveSelection(int delta, std::size_t count) noexcept;
	void MoveFocus(int delta) noexcept;
	void RequestFocus(SplitPane pane) noexcept;
};

struct SplitViewOptions
{
	float navigationWidth = 260.0f;
	float rowHeight = 58.0f;
	float gap = 12.0f;
	float paneRadius = 18.0f;
	float paneOpacity = 0.74f;
	// per-frame scroll delta
	float contentControllerScroll = 0.0f;
	ImGuiWindowFlags childFlags = ImGuiWindowFlags_NoSavedSettings;
	bool navFlattened = true;
};

using DrawSplitContent = std::function<void(std::size_t)>;

void DrawSplitView(const char* id, SplitViewState& state,
	std::span<const SplitViewTab> tabs, const DrawSplitContent& drawContent,
	ImVec2 size = ImVec2(0.0f, 0.0f), const SplitViewOptions& options = {});

struct QuickMenuRow
{
	std::string id;
	std::string label;
	std::string value;
	bool enabled = true;
};

struct QuickMenuChoice
{
	std::string id;
	std::string label;
	bool enabled = true;
};

struct QuickMenuModel
{
	std::string windowId = "UwpImGuiFrontendQuickMenu";
	std::string title = "Quick Menu";
	std::string footer;
	std::vector<QuickMenuRow> rows;
	std::size_t selectedRow = 0;
	bool choicePopupOpen = false;
	std::string choiceTitle;
	std::vector<QuickMenuChoice> choices;
	std::size_t selectedChoice = 0;
};

struct QuickMenuResult
{
	std::optional<std::size_t> activatedRow;
	std::optional<std::size_t> activatedChoice;
	bool closeRequested = false;
	bool choiceCancelled = false;
};

[[nodiscard]] QuickMenuResult DrawQuickMenu(const QuickMenuModel& model,
	const ShellTheme& theme, ImFont* font = nullptr);

struct ActionListEntry
{
	std::string id;
	std::string label;
	std::string detail;
	bool enabled = true;
	bool checkable = false;
	bool checked = false;
	bool separatorBefore = false;
};

struct ActionListState
{
	std::size_t selected = 0;
	float scroll = 0.0f;
	bool requestFocus = false;

	void SetItemCount(std::size_t count) noexcept;
	void Move(int delta, std::size_t count) noexcept;
};

struct ActionListOptions
{
	float rowHeight = 42.0f;
	float separatorHeight = 10.0f;
	float footerHeight = 54.0f;
	float fontSizeUnits = 17.0f;
	float detailFontSizeUnits = 12.0f;
	std::string defaultFooter;
	bool showDisabledReasonFooter = true;
};

[[nodiscard]] std::optional<std::size_t> DrawActionList(
	const ShellLayoutContext& context, ImDrawList* draw, const UnitRect& panel,
	float contentTopUnits, ActionListState& state,
	std::span<const ActionListEntry> entries, ImFont* font = nullptr,
	const ActionListOptions& options = {});

struct TitleOptionAction
{
	std::string id;
	std::string label;
	std::string detail;
	bool enabled = true;
	bool checkable = false;
	bool checked = false;
	bool separatorBefore = false;
};

struct TitleInformationField
{
	std::string label;
	std::string value;
};

[[nodiscard]] std::vector<TitleOptionAction> MakeDefaultTitleOptionActions(
	bool favorite = false);

struct TitleOptionsModel
{
	TextureHandle artwork;
	std::string title;
	std::string titleId;
	std::string subtitle;
	std::string artworkPlaceholder = "No artwork is available";
	Metadata metadata;
	std::vector<TitleInformationField> information;
	std::vector<TitleOptionAction> actions;
	float artworkHeight = 245.0f;
	float detailControllerScroll = 0.0f;
};

struct TitleOptionsState
{
	ActionListState actions;
};

struct TitleOptionsResult
{
	std::optional<std::string> activatedAction;
	bool closeRequested = false;
};

[[nodiscard]] TitleOptionsResult DrawTitleOptionsView(
	const ShellLayoutContext& context, ImDrawList* draw, const UnitRect& panel,
	TitleOptionsState& state, const TitleOptionsModel& model,
	ImFont* font = nullptr);

struct DialogAction
{
	std::string id;
	std::string label;
	bool enabled = true;
};

struct DialogModel
{
	std::string popupId;
	std::string title;
	std::string message;
	std::vector<DialogAction> actions;
	std::size_t selectedAction = 0;
	bool destructive = false;
};

struct TextEditorPanelState
{
	bool requestFocus = false;
};

struct TextEditorPanelOptions
{
	std::string label = "Text";
	std::string saveLabel = "Save";
	std::string cancelLabel = "Cancel";
	std::string footer;
	TextInputScope inputScope = TextInputScope::Text;
	float contentTop = 112.0f;
};

struct TextEditorPanelResult
{
	bool changed = false;
	bool save = false;
	bool cancel = false;
};

[[nodiscard]] TextEditorPanelResult DrawTextEditorPanel(
	const ShellLayoutContext& context, ImDrawList* draw, const UnitRect& panel,
	TextEditorPanelState& state, char* buffer, std::size_t bufferSize,
	ImFont* font = nullptr, const TextEditorPanelOptions& options = {});

struct OverlayConfirmationModel
{
	std::string prompt;
	std::vector<std::string> details;
	std::vector<DialogAction> actions;
	std::size_t selectedAction = 0;
	std::string footer;
};

struct OverlayConfirmationResult
{
	std::optional<std::size_t> activatedAction;
};

[[nodiscard]] OverlayConfirmationResult DrawOverlayConfirmation(
	const ShellLayoutContext& context, ImDrawList* draw, const UnitRect& panel,
	const OverlayConfirmationModel& model, ImFont* font = nullptr);

enum class DataTableColumnKind
{
	Artwork,
	PrimaryText,
	Text,
	Action,
};

struct DataTableColumn
{
	std::string id;
	std::string label;
	DataTableColumnKind kind = DataTableColumnKind::Text;
	float width = 1.0f;
	bool stretch = false;
	bool resizable = true;
};

struct DataTableCell
{
	std::string text;
	TextureHandle artwork;
	bool enabled = true;
};

struct DataTableRow
{
	ItemId id = 0;
	std::vector<DataTableCell> cells;
	bool enabled = true;
};

struct DataTableState
{
	std::size_t selected = 0;
	bool requestFocus = false;
	bool controllerNavigationActive = false;

	void SetItemCount(std::size_t count) noexcept;
	void Select(std::size_t index, std::size_t count) noexcept;
	void Move(int delta, std::size_t count) noexcept;
};

struct DataTableOptions
{
	ImFont* font = nullptr;
	float rowHeight = 72.0f;
	float artworkPadding = 7.0f;
	float artworkRadius = 9.0f;
	// when positive, artwork keeps this height and derives width from its aspect ratio
	float artworkHeight = 0.0f;
	float focusedArtworkScale = 1.0f;
	float actionHeight = 36.0f;
	float actionMaxWidth = 96.0f;
	ImVec2 size = ImVec2(0.0f, 0.0f);
	bool hideScrollbar = false;
	bool showHeaders = true;
	bool controllerNavigation = false;
	bool wrapControllerNavigation = false;
	float controllerScroll = 0.0f;
};

struct DataTableResult
{
	std::optional<std::size_t> activatedRow;
	std::optional<std::size_t> selectionChanged;
	bool rowFocused = false;
	bool navigateBeforeTable = false;
};

[[nodiscard]] DataTableResult DrawDataTable(const char* id,
	DataTableState& state, std::span<const DataTableColumn> columns,
	std::span<const DataTableRow> rows, const DataTableOptions& options = {});

struct SelectionDialogEntry
{
	std::string id;
	std::string label;
	std::string detail;
	bool enabled = true;
};

struct SelectionDialogModel
{
	std::string popupId;
	std::string title;
	std::vector<SelectionDialogEntry> entries;
	std::size_t selected = 0;
	std::string closeLabel = "Close";
	bool dismissRequested = false;
};

struct SelectionDialogResult
{
	std::optional<std::size_t> selected;
	bool closed = false;
};

[[nodiscard]] SelectionDialogResult DrawSelectionDialog(
	const SelectionDialogModel& model, const ShellLayoutContext& context,
	const ShellTheme& theme, ImVec2 size = ImVec2(520.0f, 0.0f));

enum class FormFieldKind
{
	Toggle,
	Choice,
	ReadOnly,
};

struct FormField
{
	std::string id;
	std::string label;
	std::string value;
	FormFieldKind kind = FormFieldKind::Choice;
	bool checked = false;
	bool enabled = true;
};

struct FormSection
{
	std::string id;
	std::string label;
	std::vector<FormField> fields;
};

struct FormEditorState
{
	std::size_t section = 0;
	std::size_t row = 0;
	float scroll = 0.0f;
	bool choiceOpen = false;
	std::size_t choiceSelection = 0;
	float choiceScroll = 0.0f;
};

struct FormEditorOptions
{
	std::string saveLabel = "Save";
	std::string cancelLabel = "Cancel";
	std::string footer;
	float tabsTop = 82.0f;
	float contentTop = 137.0f;
};

struct FormEditorResult
{
	std::optional<std::size_t> selectedSection;
	std::optional<std::size_t> activatedField;
	std::optional<std::size_t> selectedChoice;
	bool save = false;
	bool cancel = false;
};

[[nodiscard]] FormEditorResult DrawFormEditor(
	const ShellLayoutContext& context, ImDrawList* draw, const UnitRect& panel,
	FormEditorState& state, std::span<const FormSection> sections,
	std::span<const ChoiceItem> activeChoices = {}, ImFont* font = nullptr,
	const FormEditorOptions& options = {});

[[nodiscard]] std::optional<std::size_t> DrawConfirmationDialog(
	const DialogModel& model, ImVec2 size = ImVec2(560.0f, 0.0f));
void DrawProgressDialogBody(std::string_view message, float fraction,
	std::string_view detail = {});

[[nodiscard]] bool BeginThemedModal(const char* id,
	const ShellLayoutContext& context, const ShellTheme& theme,
	ImVec2 size = ImVec2(0.0f, 0.0f), bool* open = nullptr,
	ImGuiWindowFlags flags = ImGuiWindowFlags_AlwaysAutoResize |
		ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoTitleBar);
void EndThemedModal();

void DrawDimmedBackground(ImU8 alpha = 168);
[[nodiscard]] UnitRect CenteredPanel(const ShellLayoutContext& context,
	float widthUnits, float heightUnits, float marginUnits = 28.0f) noexcept;
void DrawOverlayPanel(ImDrawList* draw, const ShellLayoutContext& context,
	const UnitRect& panel, const ShellTheme& theme, float opacity = 0.98f);
void DrawOverlayHeader(ImDrawList* draw, const ShellLayoutContext& context,
	const UnitRect& panel, const ShellTheme& theme, std::string_view title,
	std::string_view subtitle = {}, std::string_view trailing = {},
	ImFont* font = nullptr);
}
