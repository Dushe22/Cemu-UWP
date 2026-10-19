#include "UwpImGuiFrontend/Views.h"
#include "UwpImGuiFrontend/MetadataEditor.h"
#include "UwpImGuiFrontend/Text.h"

#include <imgui_internal.h>

#include <fmt/format.h>

#include <algorithm>
#include <cfloat>
#include <cmath>

namespace UwpImGuiFrontend
{
namespace
{
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
	// the frontend draws its own focus border, hide the built-in nav cursor
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

float EntryTop(std::span<const ActionListEntry> entries, std::size_t index,
	const ActionListOptions& options)
{
	float y = 0.0f;
	for (std::size_t i = 0; i < index && i < entries.size(); ++i)
	{
		if (entries[i].separatorBefore)
			y += options.separatorHeight;
		y += options.rowHeight;
	}
	if (index < entries.size() && entries[index].separatorBefore)
		y += options.separatorHeight;
	return y;
}

float ContentHeight(std::span<const ActionListEntry> entries,
	const ActionListOptions& options)
{
	return entries.empty() ? 0.0f :
		EntryTop(entries, entries.size() - 1, options) + options.rowHeight;
}

void DrawText(ImDrawList* draw, const ShellLayoutContext& context, ImFont* font,
	float sizeUnits, ImVec2 positionUnits, ImU32 color, std::string_view text)
{
	const ImVec2 position = ToPx(context, positionUnits);
	if (font)
	{
		draw->AddText(font, sizeUnits * context.uiScale, position, color,
			text.data(), text.data() + text.size());
	}
	else
	{
		draw->AddText(position, color, text.data(), text.data() + text.size());
	}
}

ImVec2 MeasureText(const ShellLayoutContext& context, ImFont* font,
	float sizeUnits, std::string_view text)
{
	if (font)
	{
		const ImVec2 measured = font->CalcTextSizeA(sizeUnits * context.uiScale,
			FLT_MAX, 0.0f, text.data(), text.data() + text.size());
		return { measured.x / context.uiScale, measured.y / context.uiScale };
	}
	const ImVec2 measured = ImGui::CalcTextSize(text.data(), text.data() + text.size());
	return { measured.x / context.uiScale, measured.y / context.uiScale };
}

bool HitTarget(const ShellLayoutContext& context, const UnitRect& rect, int id)
{
	ImGui::SetCursorScreenPos(RectMinPx(context, rect));
	ImGui::PushID(id);
	const bool activated = NavigableInvisibleButton("action-list-hit",
		ToPx(context, rect.w, rect.h));
	const bool retainedActivated = ImGui::IsItemFocused() &&
		IsWidgetAcceptPressed();
	ImGui::PopID();
	return activated || retainedActivated;
}

#if IMGUI_VERSION_NUM >= 19200
ImTextureRef TextureReference(std::uintptr_t id)
{
	return ImTextureRef(static_cast<ImTextureID>(id));
}
#else
ImTextureID TextureReference(std::uintptr_t id)
{
	return reinterpret_cast<ImTextureID>(id);
}
#endif
}

void SplitViewState::SetItemCount(std::size_t count) noexcept
{
	selected = count == 0 ? 0 : std::min(selected, count - 1);
}

void SplitViewState::Select(std::size_t index, std::size_t count) noexcept
{
	selected = count == 0 ? 0 : std::min(index, count - 1);
}

void SplitViewState::MoveSelection(int delta, std::size_t count) noexcept
{
	if (count == 0)
	{
		selected = 0;
		return;
	}
	const auto next = static_cast<std::ptrdiff_t>(selected) + delta;
	selected = static_cast<std::size_t>(std::clamp<std::ptrdiff_t>(next, 0,
		static_cast<std::ptrdiff_t>(count - 1)));
}

void SplitViewState::MoveFocus(int delta) noexcept
{
	if (delta < 0)
		RequestFocus(SplitPane::Navigation);
	else if (delta > 0)
		RequestFocus(SplitPane::Content);
}

void SplitViewState::RequestFocus(SplitPane pane) noexcept
{
	focusedPane = pane;
	requestedFocus = pane;
}

void DrawSplitView(const char* id, SplitViewState& state,
	std::span<const SplitViewTab> tabs, const DrawSplitContent& drawContent,
	ImVec2 size, const SplitViewOptions& options)
{
	state.SetItemCount(tabs.size());
	const ImVec2 available = ImGui::GetContentRegionAvail();
	if (size.x <= 0.0f)
		size.x = std::max(0.0f, available.x + size.x);
	if (size.y <= 0.0f)
		size.y = std::max(0.0f, available.y + size.y);
	const float navigationWidth = std::min(options.navigationWidth,
		std::max(0.0f, size.x - options.gap));
	const float contentWidth = std::max(0.0f, size.x - navigationWidth - options.gap);

	ImGui::PushID(id);
	(void)BeginShellPane("navigation", { navigationWidth, size.y }, options.childFlags,
		options.paneRadius, options.paneOpacity, { 22.0f, 20.0f },
		options.navFlattened);
	const bool focusNavigation = state.requestedFocus == SplitPane::Navigation;
	if (focusNavigation && GImGui->NavMoveSubmitted &&
		(GImGui->NavMoveDir == ImGuiDir_Up ||
			GImGui->NavMoveDir == ImGuiDir_Down))
	{
		// the host already moved the selection, do not let the same D-pad edge escape
		ImGui::NavMoveRequestCancel();
	}
	for (std::size_t index = 0; index < tabs.size(); ++index)
	{
		const auto& tab = tabs[index];
		if (focusNavigation && state.selected == index)
			ImGui::SetKeyboardFocusHere();
		if (!tab.enabled)
			ImGui::BeginDisabled();
		const bool activated = ShellSelectableWithId(tab.id.c_str(), tab.label.c_str(),
			state.selected == index, { 0.0f, options.rowHeight });
		const bool focused = ImGui::IsItemFocused();
		if (!tab.enabled)
			ImGui::EndDisabled();
		// a stale focused item must not overwrite the selection while focus is pending
		if ((activated || (focused && !focusNavigation)) && tab.enabled)
		{
			state.selected = index;
			state.focusedPane = SplitPane::Navigation;
		}
	}
	EndShellPane();
	ImGui::SameLine(0.0f, options.gap);
	(void)BeginShellPane("content", { contentWidth, size.y }, options.childFlags,
		options.paneRadius, options.paneOpacity, { 22.0f, 20.0f },
		options.navFlattened);
	if (std::abs(options.contentControllerScroll) > 0.001f)
	{
		ImGui::SetScrollY(std::clamp(ImGui::GetScrollY() +
			options.contentControllerScroll, 0.0f, ImGui::GetScrollMaxY()));
	}
	const bool focusContent = state.requestedFocus == SplitPane::Content;
	if (focusContent)
		ImGui::SetKeyboardFocusHere();
	if (drawContent && !tabs.empty())
		drawContent(state.selected);
	const bool contentFocused = !focusNavigation &&
		ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows);
	// row focus resolves at frame end, stale content focus must not claim the pane
	const bool contentClicked =
		ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) &&
		ImGui::IsMouseClicked(ImGuiMouseButton_Left);
	if (focusContent || contentClicked || contentFocused)
		state.focusedPane = SplitPane::Content;
	else if (focusNavigation)
		state.focusedPane = SplitPane::Navigation;
	EndShellPane();
	state.requestedFocus.reset();
	ImGui::PopID();
}

QuickMenuResult DrawQuickMenu(const QuickMenuModel& model,
	const ShellTheme& theme, ImFont* font)
{
	QuickMenuResult result;
	const ImGuiViewport* viewport = ImGui::GetMainViewport();
	if (!viewport)
		return result;
	if (ConsumeWidgetBackPressed())
	{
		result.choiceCancelled = model.choicePopupOpen;
		result.closeRequested = !model.choicePopupOpen;
	}
	SetWidgetTheme(theme);
	ImGui::GetBackgroundDrawList()->AddRectFilled(viewport->Pos,
		{ viewport->Pos.x + viewport->Size.x, viewport->Pos.y + viewport->Size.y },
		IM_COL32(0, 0, 0, 145));
	ImGui::SetNextWindowPos(viewport->WorkPos);
	ImGui::SetNextWindowSize(viewport->WorkSize);
	constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoMove |
		ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
		ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings |
		ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar |
		ImGuiWindowFlags_NoScrollWithMouse;
	if (!ImGui::Begin(model.windowId.c_str(), nullptr, flags))
	{
		ImGui::End();
		return result;
	}

	constexpr float rowHeight = 54.0f;
	constexpr float rowGap = 8.0f;
	constexpr float rowTop = 84.0f;
	constexpr float panelBottomPadding = 28.0f;
	constexpr float panelFooterPadding = 64.0f;
	const float rowsBottom = model.rows.empty() ? rowTop :
		rowTop + static_cast<float>(model.rows.size()) * (rowHeight + rowGap) - rowGap;
	const float contentHeight = rowsBottom +
		(model.footer.empty() ? panelBottomPadding : panelFooterPadding);
	const float maxPanelWidth = std::max(320.0f, viewport->WorkSize.x - 96.0f);
	const float maxPanelHeight = std::max(360.0f, viewport->WorkSize.y - 96.0f);
	const ImVec2 panelSize{
		std::min(maxPanelWidth, std::max(560.0f, viewport->WorkSize.x * 0.42f)),
		std::min(maxPanelHeight, std::max({ 450.0f,
			viewport->WorkSize.y * 0.52f, contentHeight })),
	};
	const ImVec2 panelPosition{
		viewport->WorkPos.x + (viewport->WorkSize.x - panelSize.x) * 0.5f,
		viewport->WorkPos.y + (viewport->WorkSize.y - panelSize.y) * 0.5f,
	};
	ImDrawList* draw = ImGui::GetWindowDrawList();
	DrawShellPanelPx(draw, panelPosition,
		{ panelPosition.x + panelSize.x, panelPosition.y + panelSize.y },
		theme, 26.0f, 0.86f);
	if (font)
		ImGui::PushFont(font);
	ImGui::SetCursorScreenPos({ panelPosition.x + 34.0f, panelPosition.y + 28.0f });
	ImGui::TextUnformatted(model.title.c_str());

	const float rowWidth = panelSize.x - 64.0f;
	const float rowY = panelPosition.y + rowTop;
	for (std::size_t index = 0; index < model.rows.size(); ++index)
	{
		const auto& row = model.rows[index];
		const bool modelSelected = model.selectedRow == index &&
			!model.choicePopupOpen;
		const ImVec2 min{ panelPosition.x + 32.0f,
			rowY + static_cast<float>(index) * (rowHeight + rowGap) };
		const ImVec2 max{ min.x + rowWidth, min.y + rowHeight };
		ImGui::SetCursorScreenPos(min);
		ImGui::PushID(static_cast<int>(index));
		if (!row.enabled || model.choicePopupOpen)
			ImGui::BeginDisabled();
		if (modelSelected)
			(void)ApplyPendingWidgetFocus();
		const bool activated = NavigableInvisibleButton("quick-row",
			{ rowWidth, rowHeight });
		const bool focused = ImGui::IsItemFocused();
		const bool hovered = ImGui::IsItemHovered() || ImGui::IsItemFocused();
		if (!row.enabled || model.choicePopupOpen)
			ImGui::EndDisabled();
		ImGui::PopID();
		const bool selected = modelSelected || focused;
		DrawShellPanelPx(draw, min, max, theme, 14.0f,
			selected ? 0.82f : (hovered ? 0.62f : 0.44f));
		DrawReplicaFocus(draw, min, max, 15.0f, focused, hovered);
		const ImU32 labelColor = row.enabled ?
			(selected ? Color(theme.textPrimary) : Color(theme.textSecondary)) :
			ColorWithAlpha(theme.textSecondary, 0.42f);
		draw->AddText({ min.x + 18.0f,
			min.y + (rowHeight - ImGui::GetTextLineHeight()) * 0.5f },
			labelColor, row.label.c_str());
		if (!row.value.empty())
		{
			const ImVec2 valueSize = ImGui::CalcTextSize(row.value.c_str());
			draw->AddText({ max.x - valueSize.x - 42.0f,
				min.y + (rowHeight - valueSize.y) * 0.5f },
				Color(theme.textPrimary), row.value.c_str());
			const ImVec2 center{ max.x - 22.0f, min.y + rowHeight * 0.5f };
			draw->AddTriangleFilled({ center.x - 5.0f, center.y - 2.0f },
				{ center.x + 5.0f, center.y - 2.0f },
				{ center.x, center.y + 4.0f },
				ColorWithAlpha(theme.textPrimary, 0.76f));
		}
		if (activated && row.enabled && !model.choicePopupOpen)
			result.activatedRow = index;
	}

	if (model.choicePopupOpen && !model.choices.empty())
	{
		const float popupWidth = std::min(380.0f,
			std::max(330.0f, panelSize.x * 0.48f));
		const float popupHeight = 64.0f + model.choices.size() * 44.0f;
		ImVec2 popupPosition{ panelPosition.x + panelSize.x + 16.0f,
			panelPosition.y + rowHeight + rowGap + 84.0f };
		if (popupPosition.x + popupWidth >
			viewport->WorkPos.x + viewport->WorkSize.x - 24.0f)
		{
			popupPosition.x = panelPosition.x + panelSize.x - popupWidth - 32.0f;
		}
		if (popupPosition.y + popupHeight >
			viewport->WorkPos.y + viewport->WorkSize.y - 24.0f)
		{
			popupPosition.y = viewport->WorkPos.y + viewport->WorkSize.y -
				popupHeight - 24.0f;
		}
		popupPosition.x = std::max(viewport->WorkPos.x + 24.0f, popupPosition.x);
		popupPosition.y = std::max(viewport->WorkPos.y + 24.0f, popupPosition.y);
		DrawShellPanelPx(draw, popupPosition,
			{ popupPosition.x + popupWidth, popupPosition.y + popupHeight },
			theme, 22.0f, 0.93f);
		draw->AddText({ popupPosition.x + 24.0f, popupPosition.y + 22.0f },
			Color(theme.textPrimary), model.choiceTitle.c_str());
		for (std::size_t index = 0; index < model.choices.size(); ++index)
		{
			const auto& choice = model.choices[index];
			const ImVec2 min{ popupPosition.x + 18.0f,
				popupPosition.y + 58.0f + static_cast<float>(index) * 44.0f };
			const ImVec2 max{ popupPosition.x + popupWidth - 18.0f, min.y + 38.0f };
			ImGui::SetCursorScreenPos(min);
			ImGui::PushID(1000 + static_cast<int>(index));
			if (!choice.enabled)
				ImGui::BeginDisabled();
			if (model.selectedChoice == index)
				(void)ApplyPendingWidgetFocus();
			const bool activated = NavigableInvisibleButton("choice-row",
				{ max.x - min.x, max.y - min.y });
			const bool focused = ImGui::IsItemFocused();
			const bool hovered = ImGui::IsItemHovered();
			if (!choice.enabled)
				ImGui::EndDisabled();
			ImGui::PopID();
			const bool selected = model.selectedChoice == index || focused;
			if (selected || hovered)
				DrawShellPanelPx(draw, min, max, theme, 12.0f,
					selected ? 0.78f : 0.50f);
			DrawReplicaFocus(draw, min, max, 13.0f, focused, hovered);
			draw->AddText({ min.x + 14.0f,
				min.y + (38.0f - ImGui::GetTextLineHeight()) * 0.5f },
				choice.enabled ? (selected ? Color(theme.textPrimary) :
					Color(theme.textSecondary)) : ColorWithAlpha(theme.textSecondary, 0.42f),
				choice.label.c_str());
			if (activated && choice.enabled)
				result.activatedChoice = index;
		}
	}

	if (!model.footer.empty())
	{
		ImGui::SetCursorScreenPos({ panelPosition.x + 34.0f,
			panelPosition.y + panelSize.y - 46.0f });
		ImGui::TextDisabled("%s", model.footer.c_str());
	}
	if (font)
		ImGui::PopFont();
	ImGui::End();
	return result;
}

void ActionListState::SetItemCount(std::size_t count) noexcept
{
	selected = count == 0 ? 0 : std::min(selected, count - 1);
	if (count == 0)
		scroll = 0.0f;
}

void ActionListState::Move(int delta, std::size_t count) noexcept
{
	if (count == 0)
	{
		selected = 0;
		return;
	}
	const auto next = static_cast<std::ptrdiff_t>(selected) + delta;
	selected = static_cast<std::size_t>(std::clamp<std::ptrdiff_t>(next, 0,
		static_cast<std::ptrdiff_t>(count - 1)));
}

std::vector<TitleOptionAction> MakeDefaultTitleOptionActions(bool favorite)
{
	return {
		{ "start", "Start" },
		{ "favorite", "Favorite", {}, true, true, favorite, true },
		{ "metadata", "Edit Metadata" },
		{ "controller-mapping", "Controller Mapping" },
	};
}

TitleOptionsResult DrawTitleOptionsView(const ShellLayoutContext& context,
	ImDrawList* draw, const UnitRect& panel, TitleOptionsState& state,
	const TitleOptionsModel& model, ImFont* font)
{
	TitleOptionsResult result;
	if (!draw)
		return result;
	if (ConsumeWidgetBackPressed())
		result.closeRequested = true;
	const auto& theme = CurrentWidgetTheme();
	DrawOverlayPanel(draw, context, panel, theme);
	DrawOverlayHeader(draw, context, panel, theme, model.title,
		model.subtitle, model.titleId, font);

	std::vector<ActionListEntry> actions;
	actions.reserve(model.actions.size());
	for (const TitleOptionAction& action : model.actions)
	{
		actions.push_back({ action.id, action.label, action.detail,
			action.enabled, action.checkable, action.checked,
			action.separatorBefore });
	}
	const float actionsWidth = std::min(410.0f, panel.w * 0.38f);
	const UnitRect actionPane{ panel.x + 10.0f, panel.y + 82.0f,
		actionsWidth - 16.0f, panel.h - 94.0f };
	DrawShellPanelPx(draw, RectMinPx(context, actionPane),
		RectMaxPx(context, actionPane), theme, 16.0f * context.uiScale, 0.62f);
	if (const auto activated = DrawActionList(context, draw, actionPane,
		6.0f, state.actions, actions, font,
		{ .footerHeight = 0.0f, .showDisabledReasonFooter = false }))
	{
		if (*activated < model.actions.size() && model.actions[*activated].enabled)
			result.activatedAction = model.actions[*activated].id;
	}

	const float detailX = panel.x + actionsWidth + 8.0f;
	const float detailWidth = std::max(1.0f, panel.right() - detailX - 18.0f);
	const float detailHeight = std::max(1.0f, panel.h - 104.0f);
	ImGui::SetCursorScreenPos(ToPx(context, detailX, panel.y + 82.0f));
	(void)BeginShellPane("title-information",
		ToPx(context, detailWidth, detailHeight),
		ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar,
		16.0f * context.uiScale, 0.62f, ToPx(context, 20.0f, 18.0f), true);
	if (std::abs(model.detailControllerScroll) > 0.001f)
	{
		ImGui::SetScrollY(std::clamp(ImGui::GetScrollY() +
			model.detailControllerScroll * context.uiScale,
			0.0f, ImGui::GetScrollMaxY()));
	}
	if (font)
		ImGui::PushFont(font);
	const float previewHeight = std::max(120.0f,
		model.artworkHeight * context.uiScale);
	const float previewWidth = std::max(1.0f, ImGui::GetContentRegionAvail().x);
	const ImVec2 previewMin = ImGui::GetCursorScreenPos();
	const ImVec2 previewMax{ previewMin.x + previewWidth,
		previewMin.y + previewHeight };
	DrawShellPanelPx(draw, previewMin, previewMax, theme,
		12.0f * context.uiScale, 0.42f);
	if (model.artwork)
	{
		const float aspect = model.artwork.height == 0 ? 1.0f :
			static_cast<float>(model.artwork.width) /
			static_cast<float>(model.artwork.height);
		const float inset = 8.0f * context.uiScale;
		const float availableWidth = std::max(1.0f, previewWidth - inset * 2.0f);
		const float availableHeight = std::max(1.0f, previewHeight - inset * 2.0f);
		float imageWidth = availableWidth;
		float imageHeight = imageWidth / std::max(0.001f, aspect);
		if (imageHeight > availableHeight)
		{
			imageHeight = availableHeight;
			imageWidth = imageHeight * aspect;
		}
		const ImVec2 imageMin{
			previewMin.x + (previewWidth - imageWidth) * 0.5f,
			previewMin.y + (previewHeight - imageHeight) * 0.5f,
		};
		draw->AddImageRounded(TextureReference(model.artwork.id), imageMin,
			{ imageMin.x + imageWidth, imageMin.y + imageHeight },
			{ 0.0f, 0.0f }, { 1.0f, 1.0f }, IM_COL32_WHITE,
			10.0f * context.uiScale);
	}
	else
	{
		const std::string_view placeholder = model.artworkPlaceholder.empty() ?
			std::string_view{ Translate("No artwork is available") } :
			std::string_view{ model.artworkPlaceholder };
		const ImVec2 textSize = ImGui::CalcTextSize(placeholder.data(),
			placeholder.data() + placeholder.size());
		draw->AddText({ previewMin.x + (previewWidth - textSize.x) * 0.5f,
			previewMin.y + (previewHeight - textSize.y) * 0.5f },
			Color(theme.textSecondary), placeholder.data(),
			placeholder.data() + placeholder.size());
	}
	ImGui::Dummy({ previewWidth, previewHeight });

	ImGui::Spacing();
	auto field = [](std::string_view label, std::string_view value) {
		if (value.empty())
			return false;
		ImGui::TextDisabled("%.*s", static_cast<int>(label.size()), label.data());
		ImGui::SameLine();
		ImGui::TextWrapped("%.*s", static_cast<int>(value.size()), value.data());
		return true;
	};
	bool hasInformation = false;
	for (const TitleInformationField& item : model.information)
		hasInformation |= field(item.label, item.value);
	if (!model.metadata.rating.empty())
	{
		const float rating = MetadataRatingHalfSteps(model.metadata.rating) * 0.5f;
		hasInformation |= field(Translate("Rating"), fmt::format("{:.1f} / 5", rating));
	}
	hasInformation |= field(Translate("Release date"),
		FormatMetadataReleaseDateUk(model.metadata.releaseDate));
	hasInformation |= field(Translate("Developer"), model.metadata.developer);
	hasInformation |= field(Translate("Publisher"), model.metadata.publisher);
	hasInformation |= field(Translate("Genre"), model.metadata.genre);
	hasInformation |= field(Translate("Players"), model.metadata.players);
	hasInformation |= field(Translate("Content rating"), model.metadata.contentRating);
	hasInformation |= field(Translate("Modes"), model.metadata.modes);
	hasInformation |= field(Translate("Themes"), model.metadata.themes);
	if (!model.metadata.description.empty())
	{
		ImGui::Spacing();
		ImGui::TextWrapped("%s", model.metadata.description.c_str());
		hasInformation = true;
	}
	if (!hasInformation)
		ImGui::TextDisabled("%s", Translate("No metadata is available."));
	if (font)
		ImGui::PopFont();
	EndShellPane();
	return result;
}

TextEditorPanelResult DrawTextEditorPanel(const ShellLayoutContext& context,
	ImDrawList* draw, const UnitRect& panel, TextEditorPanelState& state,
	char* buffer, std::size_t bufferSize, ImFont* font,
	const TextEditorPanelOptions& options)
{
	TextEditorPanelResult result;
	if (!draw || !buffer || bufferSize == 0)
		return result;
	if (!IsNativeTextInputActive() && ConsumeWidgetBackPressed())
		result.cancel = true;
	const auto& theme = CurrentWidgetTheme();
	DrawText(draw, context, font, 17.0f,
		{ panel.x + 34.0f, panel.y + options.contentTop },
		Color(theme.textSecondary), options.label);
	ImGui::SetCursorScreenPos(ToPx(context,
		panel.x + 34.0f, panel.y + options.contentTop + 31.0f));
	ImGui::SetNextItemWidth((panel.w - 68.0f) * context.uiScale);
	if (state.requestFocus)
		ImGui::SetKeyboardFocusHere();
	result.changed = NativeInputText("##overlay-text-editor", buffer,
		bufferSize, 0, options.inputScope);
	if (state.requestFocus)
	{
		RequestNativeTextInput(buffer, bufferSize, options.inputScope);
		state.requestFocus = false;
	}

	const float buttonY = panel.bottom() - 94.0f;
	const float gap = 14.0f;
	const float buttonWidth = std::min(150.0f,
		std::max(90.0f, (panel.w - 68.0f - gap) * 0.5f));
	const std::array buttons{
		UnitRect{ panel.x + 34.0f, buttonY, buttonWidth, 46.0f },
		UnitRect{ panel.x + 34.0f + buttonWidth + gap, buttonY,
			buttonWidth, 46.0f },
	};
	const std::array<std::string_view, 2> labels{
		options.saveLabel, options.cancelLabel
	};
	for (std::size_t index = 0; index < buttons.size(); ++index)
	{
		if (index == 0 && !IsNativeTextInputActive())
			(void)ApplyPendingWidgetFocus();
		const bool clicked = HitTarget(context, buttons[index],
			24000 + static_cast<int>(index));
		const bool focused = ImGui::IsItemFocused();
		const bool hovered = ImGui::IsItemHovered() || focused;
		DrawShellPanelPx(draw, RectMinPx(context, buttons[index]),
			RectMaxPx(context, buttons[index]), theme,
			12.0f * context.uiScale, hovered ? 0.78f :
			(index == 0 ? 0.72f : 0.52f));
		DrawReplicaFocus(draw, RectMinPx(context, buttons[index]),
			RectMaxPx(context, buttons[index]), 12.0f * context.uiScale,
			focused, hovered);
		const ImVec2 textSize = MeasureText(context, font, 17.0f, labels[index]);
		DrawText(draw, context, font, 17.0f,
			{ buttons[index].x + (buttons[index].w - textSize.x) * 0.5f,
			  buttons[index].y + 13.0f },
			hovered || index == 0 ? Color(theme.textPrimary) :
				Color(theme.textSecondary), labels[index]);
		if (clicked)
		{
			result.save = index == 0;
			result.cancel = index == 1;
		}
	}
	if (!options.footer.empty())
	{
		DrawText(draw, context, font, 14.0f,
			{ panel.x + 34.0f, panel.bottom() - 34.0f },
			Color(theme.textSecondary), options.footer);
	}
	return result;
}

OverlayConfirmationResult DrawOverlayConfirmation(
	const ShellLayoutContext& context, ImDrawList* draw, const UnitRect& panel,
	const OverlayConfirmationModel& model, ImFont* font)
{
	OverlayConfirmationResult result;
	if (!draw || model.actions.empty())
		return result;
	if (ConsumeWidgetBackPressed())
	{
		std::size_t cancelIndex = model.actions.size();
		for (std::size_t index = 0; index < model.actions.size(); ++index)
		{
			if (model.actions[index].enabled && model.actions[index].id == "cancel")
			{
				cancelIndex = index;
				break;
			}
		}
		if (cancelIndex == model.actions.size())
		{
			for (std::size_t index = model.actions.size(); index-- > 0; )
			{
				if (model.actions[index].enabled)
				{
					cancelIndex = index;
					break;
				}
			}
		}
		if (cancelIndex < model.actions.size())
			result.activatedAction = cancelIndex;
	}
	const auto& theme = CurrentWidgetTheme();
	DrawText(draw, context, font, 20.0f,
		{ panel.x + 34.0f, panel.y + 116.0f },
		Color(theme.textPrimary), model.prompt);
	float detailY = panel.y + 151.0f;
	for (const std::string& detail : model.details)
	{
		DrawText(draw, context, font, 14.0f,
			{ panel.x + 34.0f, detailY },
			Color(theme.textSecondary), detail);
		detailY += 27.0f;
	}

	constexpr float gap = 16.0f;
	const float availableWidth = panel.w - 68.0f;
	const float buttonWidth = std::min(176.0f,
		std::max(1.0f, (availableWidth - gap *
			static_cast<float>(model.actions.size() - 1)) /
			static_cast<float>(model.actions.size())));
	const float buttonY = panel.bottom() - 112.0f;
	std::size_t defaultIndex = std::min(model.selectedAction,
		model.actions.size() - 1);
	if (!model.actions[defaultIndex].enabled)
	{
		const auto enabled = std::find_if(model.actions.begin(), model.actions.end(),
			[](const DialogAction& action) { return action.enabled; });
		if (enabled != model.actions.end())
			defaultIndex = static_cast<std::size_t>(enabled - model.actions.begin());
	}
	for (std::size_t index = 0; index < model.actions.size(); ++index)
	{
		const DialogAction& action = model.actions[index];
		const UnitRect button{ panel.x + 34.0f +
			static_cast<float>(index) * (buttonWidth + gap),
			buttonY, buttonWidth, 50.0f };
		const bool modelSelected = model.selectedAction == index;
		if (defaultIndex == index && action.enabled)
			(void)ApplyPendingWidgetFocus();
		const bool clicked = action.enabled && HitTarget(context, button,
			25000 + static_cast<int>(index));
		const bool focused = action.enabled && ImGui::IsItemFocused();
		const bool hovered = action.enabled &&
			(ImGui::IsItemHovered() || focused);
		const bool selected = modelSelected || focused;
		DrawShellPanelPx(draw, RectMinPx(context, button), RectMaxPx(context, button),
			theme, 12.0f * context.uiScale,
			action.enabled ? (selected ? 0.78f : 0.48f) : 0.28f);
		DrawReplicaFocus(draw, RectMinPx(context, button), RectMaxPx(context, button),
			13.0f * context.uiScale, focused, hovered);
		const ImVec2 textSize = MeasureText(context, font, 17.0f, action.label);
		DrawText(draw, context, font, 17.0f,
			{ button.x + (button.w - textSize.x) * 0.5f,
			  button.y + 14.0f },
			action.enabled ? (selected ? Color(theme.textPrimary) :
				Color(theme.textSecondary)) :
				ColorWithAlpha(theme.textSecondary, 0.42f), action.label);
		if (clicked)
			result.activatedAction = index;
	}
	if (!model.footer.empty())
	{
		DrawText(draw, context, font, 14.0f,
			{ panel.x + 34.0f, panel.bottom() - 38.0f },
			Color(theme.textSecondary), model.footer);
	}
	return result;
}

void DataTableState::SetItemCount(std::size_t count) noexcept
{
	selected = count == 0 ? 0 : std::min(selected, count - 1);
	if (count == 0)
		controllerNavigationActive = false;
}

void DataTableState::Select(std::size_t index, std::size_t count) noexcept
{
	selected = count == 0 ? 0 : std::min(index, count - 1);
}

void DataTableState::Move(int delta, std::size_t count) noexcept
{
	if (count == 0)
	{
		selected = 0;
		return;
	}
	const auto next = static_cast<std::ptrdiff_t>(selected) + delta;
	selected = static_cast<std::size_t>(std::clamp<std::ptrdiff_t>(next, 0,
		static_cast<std::ptrdiff_t>(count - 1)));
	requestFocus = true;
	controllerNavigationActive = true;
}

DataTableResult DrawDataTable(const char* id, DataTableState& state,
	std::span<const DataTableColumn> columns, std::span<const DataTableRow> rows,
	const DataTableOptions& options)
{
	DataTableResult result;
	if (!id || columns.empty())
		return result;
	ImFont* tableFont = ResolveFrontendTextFont(options.font);
	if (tableFont)
		ImGui::PushFont(tableFont);
	state.SetItemCount(rows.size());
	ImVec2 tableSize = options.size;
	if (tableSize.y <= 0.0f)
		tableSize.y = std::max(1.0f, ImGui::GetContentRegionAvail().y + tableSize.y);
	constexpr ImGuiTableFlags flags = ImGuiTableFlags_RowBg |
		ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_Resizable |
		ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY;
#if IMGUI_VERSION_NUM >= 19090
	GImGui->NextWindowData.HasFlags |= ImGuiNextWindowDataFlags_HasChildFlags;
	GImGui->NextWindowData.ChildFlags |= ImGuiChildFlags_NavFlattened;
#endif
	if (!ImGui::BeginTable(id, static_cast<int>(columns.size()), flags, tableSize))
	{
		if (tableFont)
			ImGui::PopFont();
		return result;
	}
	if (ImGuiTable* table = ImGui::GetCurrentTable(); table && table->InnerWindow)
	{
		if (options.hideScrollbar)
			table->InnerWindow->Flags |= ImGuiWindowFlags_NoScrollbar;
		if (std::abs(options.controllerScroll) > 0.001f)
		{
			const float next = std::clamp(table->InnerWindow->Scroll.y +
				options.controllerScroll, 0.0f, table->InnerWindow->ScrollMax.y);
			ImGui::SetScrollY(table->InnerWindow, next);
		}
	}
#if IMGUI_VERSION_NUM < 19090
	if (ImGuiTable* table = ImGui::GetCurrentTable(); table &&
		table->InnerWindow != table->OuterWindow)
	{
		table->InnerWindow->Flags |= ImGuiWindowFlags_NavFlattened;
		table->InnerWindow->RootWindowForNav =
			table->OuterWindow->RootWindowForNav;
	}
#endif

	for (const DataTableColumn& column : columns)
	{
		ImGuiTableColumnFlags columnFlags = column.stretch ?
			ImGuiTableColumnFlags_WidthStretch : ImGuiTableColumnFlags_WidthFixed;
		if (!column.resizable)
			columnFlags |= ImGuiTableColumnFlags_NoResize;
		ImGui::TableSetupColumn(column.label.c_str(), columnFlags, column.width,
			static_cast<ImGuiID>(std::hash<std::string>{}(column.id)));
	}
	ImGui::TableSetupScrollFreeze(0, options.showHeaders ? 1 : 0);
	if (options.showHeaders)
		ImGui::TableHeadersRow();
	const bool focusRequested = state.requestFocus && !rows.empty();
	const std::size_t requestedIndex = state.selected;
	if (focusRequested && options.controllerNavigation)
		state.controllerNavigationActive = true;
	if (focusRequested)
	{
		ImGui::SetScrollY(std::max(0.0f,
			static_cast<float>(requestedIndex) * options.rowHeight - options.rowHeight));
	}

	ImGuiListClipper clipper;
	clipper.Begin(static_cast<int>(rows.size()), options.rowHeight);
	const auto& theme = CurrentWidgetTheme();
	std::optional<std::size_t> focusedIndex;
	std::optional<std::size_t> hoveredIndex;
	while (clipper.Step())
	{
		for (int rowIndex = clipper.DisplayStart; rowIndex < clipper.DisplayEnd; ++rowIndex)
		{
			const DataTableRow& row = rows[static_cast<std::size_t>(rowIndex)];
			ImGui::TableNextRow(ImGuiTableRowFlags_None, options.rowHeight);
			ImGui::TableSetColumnIndex(0);
			const float rowTop = ImGui::GetCursorScreenPos().y;
			if (requestedIndex == static_cast<std::size_t>(rowIndex))
			{
				if (focusRequested)
					ImGui::SetKeyboardFocusHere();
				else
					(void)ApplyPendingWidgetFocus();
			}
			ImGui::PushID(static_cast<int>(row.id ^ (row.id >> 32)));
			if (!row.enabled)
				ImGui::BeginDisabled();
			const bool activated = ImGui::Selectable("##data-row",
				state.selected == static_cast<std::size_t>(rowIndex),
				ImGuiSelectableFlags_SpanAllColumns |
#if IMGUI_VERSION_NUM >= 18970
				ImGuiSelectableFlags_AllowOverlap,
#else
				ImGuiSelectableFlags_AllowItemOverlap,
#endif
				{ 0.0f, options.rowHeight });
			const bool focused = ImGui::IsItemFocused();
			const bool hovered = ImGui::IsItemHovered();
			result.rowFocused |= focused;
			if (focused)
				focusedIndex = static_cast<std::size_t>(rowIndex);
			if (hovered)
				hoveredIndex = static_cast<std::size_t>(rowIndex);
			if (!row.enabled)
				ImGui::EndDisabled();
			ImGui::PopID();
			const bool selected = state.selected == static_cast<std::size_t>(rowIndex);
			if (selected || focused || hovered)
			{
				ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
					ColorWithAlpha(theme.cursorNormal,
						focused ? 0.30f : (hovered ? 0.24f : 0.16f)));
			}
			if (activated && row.enabled)
				result.activatedRow = static_cast<std::size_t>(rowIndex);

			ImGuiTable* table = ImGui::GetCurrentTable();
			if (!table)
				continue;
			ImDrawList* draw = ImGui::GetWindowDrawList();
			const auto cellRect = [table, rowTop, &options](int column) {
				const ImGuiTableColumn& tableColumn = table->Columns[column];
				return ImRect({ tableColumn.WorkMinX, rowTop },
					{ tableColumn.WorkMaxX, rowTop + options.rowHeight });
			};
			const std::size_t cellCount = std::min(columns.size(), row.cells.size());
			for (std::size_t cellIndex = 0; cellIndex < cellCount; ++cellIndex)
			{
				ImGui::TableSetColumnIndex(static_cast<int>(cellIndex));
				const DataTableColumn& column = columns[cellIndex];
				const DataTableCell& cell = row.cells[cellIndex];
				const ImRect rect = cellRect(static_cast<int>(cellIndex));
				if (column.kind == DataTableColumnKind::Artwork)
				{
					if (!rect.Overlaps(table->InnerClipRect))
						continue;
					const float maximumWidth = std::max(1.0f,
						rect.GetWidth() - options.artworkPadding * 2.0f);
					const float maximumHeight = std::max(1.0f,
						options.rowHeight - options.artworkPadding * 2.0f);
					const float aspect = cell.artwork && cell.artwork.width > 0 &&
						cell.artwork.height > 0 ?
						static_cast<float>(cell.artwork.width) /
							static_cast<float>(cell.artwork.height) : 1.0f;
					float width = 0.0f;
					float height = 0.0f;
					if (options.artworkHeight > 0.0f)
					{
						height = std::min(options.artworkHeight, maximumHeight);
						width = height * aspect;
						if (width > maximumWidth)
						{
							height *= maximumWidth / width;
							width = maximumWidth;
						}
					}
					else
					{
						const float available = std::min(maximumWidth, maximumHeight);
						if (aspect > 1.0f)
						{
							width = available;
							height = available / aspect;
						}
						else
						{
							height = available;
							width = available * aspect;
						}
					}
					const float focusScale = focused ?
						std::max(1.0f, options.focusedArtworkScale) : 1.0f;
					width *= focusScale;
					height *= focusScale;
					const ImVec2 min{ rect.GetCenter().x - width * 0.5f,
						rect.GetCenter().y - height * 0.5f };
					const ImVec2 max{ min.x + width, min.y + height };
					draw->PushClipRect(rect.Min, rect.Max, true);
					if (cell.artwork)
					{
						draw->AddImageRounded(TextureReference(cell.artwork.id), min, max,
							{ 0.0f, 0.0f }, { 1.0f, 1.0f }, IM_COL32_WHITE,
							options.artworkRadius, ImDrawFlags_RoundCornersAll);
					}
					else
					{
						draw->AddRectFilled(min, max,
							ColorWithAlpha(theme.textSecondary, 0.10f),
							options.artworkRadius);
					}
					draw->PopClipRect();
				}
				else if (column.kind == DataTableColumnKind::Action)
				{
					const float actionWidth = std::max(44.0f,
						std::min(options.actionMaxWidth, rect.GetWidth() - 8.0f));
					const ImVec2 min{ rect.GetCenter().x - actionWidth * 0.5f,
						rowTop + (options.rowHeight - options.actionHeight) * 0.5f };
					const ImVec2 max{ min.x + actionWidth, min.y + options.actionHeight };
					draw->PushClipRect(rect.Min, rect.Max, true);
					DrawShellPanelPx(draw, min, max, theme, 10.0f,
						cell.enabled && row.enabled ? (selected ? 0.88f : 0.66f) : 0.30f);
					DrawClippedText(draw, { min.x + 8.0f, min.y },
						{ max.x - 8.0f, max.y },
						cell.enabled && row.enabled ? Color(theme.textPrimary) :
							ColorWithAlpha(theme.textSecondary, 0.55f),
						cell.text.c_str(), true);
					draw->PopClipRect();
				}
				else
				{
					const ImU32 color = !cell.enabled ?
						ColorWithAlpha(theme.textSecondary, 0.48f) :
						(column.kind == DataTableColumnKind::PrimaryText ?
							Color(theme.textPrimary) : Color(theme.textSecondary));
					DrawClippedText(draw,
						{ rect.Min.x + 7.0f, rect.Min.y },
						{ rect.Max.x - 7.0f, rect.Max.y },
						color, cell.text.c_str());
				}
			}
		}
	}
	std::optional<std::size_t> interactionIndex;
	if (!focusRequested)
		interactionIndex = focusedIndex ? focusedIndex : hoveredIndex;
	if (interactionIndex && state.selected != *interactionIndex)
	{
		state.selected = *interactionIndex;
		result.selectionChanged = state.selected;
	}
	state.requestFocus = false;
	ImGui::EndTable();
	if (options.controllerNavigation && state.controllerNavigationActive &&
		!focusRequested && !rows.empty())
	{
		const bool up = ImGui::IsKeyPressed(ImGuiKey_GamepadDpadUp) ||
			ImGui::IsKeyPressed(ImGuiKey_GamepadLStickUp);
		const bool down = ImGui::IsKeyPressed(ImGuiKey_GamepadDpadDown) ||
			ImGui::IsKeyPressed(ImGuiKey_GamepadLStickDown);
		if (up != down)
		{
			std::size_t next = state.selected;
			if (down && state.selected + 1 < rows.size())
				next = state.selected + 1;
			else if (down && options.wrapControllerNavigation)
				next = 0;
			else if (up && state.selected > 0)
				next = state.selected - 1;
			else if (up && options.wrapControllerNavigation)
				next = rows.size() - 1;
			else if (up)
			{
				result.navigateBeforeTable = true;
				state.controllerNavigationActive = false;
			}
			if (next != state.selected)
			{
				state.selected = next;
				state.requestFocus = true;
				result.selectionChanged = next;
			}
		}
	}
	if (tableFont)
		ImGui::PopFont();
	return result;
}

SelectionDialogResult DrawSelectionDialog(const SelectionDialogModel& model,
	const ShellLayoutContext& context, const ShellTheme& theme, ImVec2 size)
{
	SelectionDialogResult result;
	if (model.popupId.empty())
		return result;
	SetWidgetTheme(theme);
	if (!BeginThemedModal(model.popupId.c_str(), context, theme, size))
		return result;
	if (ConsumeWidgetBackPressed())
	{
		ImGui::CloseCurrentPopup();
		result.closed = true;
		EndThemedModal();
		return result;
	}
	if (model.dismissRequested)
	{
		ImGui::CloseCurrentPopup();
		result.closed = true;
		EndThemedModal();
		return result;
	}
	if (!model.title.empty())
		SectionTitle(model.title.c_str());
	std::size_t defaultIndex = model.entries.empty() ? 0 :
		std::min(model.selected, model.entries.size() - 1);
	if (!model.entries.empty() && !model.entries[defaultIndex].enabled)
	{
		const auto enabled = std::find_if(model.entries.begin(), model.entries.end(),
			[](const SelectionDialogEntry& entry) { return entry.enabled; });
		if (enabled != model.entries.end())
			defaultIndex = static_cast<std::size_t>(enabled - model.entries.begin());
	}
	for (std::size_t index = 0; index < model.entries.size(); ++index)
	{
		const SelectionDialogEntry& entry = model.entries[index];
		if (!entry.enabled)
			ImGui::BeginDisabled();
		const std::string label = entry.detail.empty() ? entry.label :
			entry.label + "    " + entry.detail;
		if (ShellSelectableWithId(entry.id.c_str(), label.c_str(),
			model.selected == index, { 0.0f, 54.0f }) && entry.enabled)
		{
			result.selected = index;
			ImGui::CloseCurrentPopup();
		}
		if (!entry.enabled)
			ImGui::EndDisabled();
		if (defaultIndex == index && entry.enabled)
			ImGui::SetItemDefaultFocus();
	}
	if (!model.closeLabel.empty() &&
		ShellButton(model.closeLabel.c_str(), { 0.0f, 48.0f }))
	{
		result.closed = true;
		ImGui::CloseCurrentPopup();
	}
	EndThemedModal();
	return result;
}

FormEditorResult DrawFormEditor(const ShellLayoutContext& context,
	ImDrawList* draw, const UnitRect& panel, FormEditorState& state,
	std::span<const FormSection> sections,
	std::span<const ChoiceItem> activeChoices, ImFont* font,
	const FormEditorOptions& options)
{
	FormEditorResult result;
	if (!draw || sections.empty())
		return result;
	if (ConsumeWidgetBackPressed())
	{
		if (state.choiceOpen)
			state.choiceOpen = false;
		else
			result.cancel = true;
	}
	state.section = std::min(state.section, sections.size() - 1);
	const FormSection& section = sections[state.section];
	state.row = section.fields.empty() ? 0 :
		std::min(state.row, section.fields.size() - 1);
	const auto& theme = CurrentWidgetTheme();
	const float tabGap = 8.0f;
	const float tabWidth = (panel.w - 44.0f -
		tabGap * static_cast<float>(sections.size() - 1)) /
		static_cast<float>(sections.size());
	for (std::size_t index = 0; index < sections.size(); ++index)
	{
		const UnitRect tab{
			panel.x + 22.0f + static_cast<float>(index) * (tabWidth + tabGap),
			panel.y + options.tabsTop, tabWidth, 42.0f
		};
		const bool clicked = !state.choiceOpen && HitTarget(context, tab,
			23000 + static_cast<int>(index));
		const bool focused = !state.choiceOpen && ImGui::IsItemFocused();
		const bool hovered = !state.choiceOpen &&
			(ImGui::IsItemHovered() || focused);
		const bool selected = state.section == index || focused;
		if (selected || hovered)
		{
			DrawShellPanelPx(draw, RectMinPx(context, tab), RectMaxPx(context, tab),
				theme, 10.0f * context.uiScale, selected ? 0.78f : 0.48f);
		}
		DrawReplicaFocus(draw, RectMinPx(context, tab), RectMaxPx(context, tab),
			10.0f * context.uiScale, focused, hovered);
		const ImVec2 textSize = MeasureText(context, font, 15.0f,
			sections[index].label);
		DrawText(draw, context, font, 15.0f,
			{ tab.x + std::max(8.0f, (tab.w - textSize.x) * 0.5f), tab.y + 11.0f },
			selected ? Color(theme.textPrimary) : Color(theme.textSecondary),
			sections[index].label);
		if (selected)
		{
			const UnitRect marker{ tab.x + 12.0f, tab.bottom() - 3.0f,
				tab.w - 24.0f, 3.0f };
			AddRoundedRectFilledPx(draw, RectMinPx(context, marker),
				RectMaxPx(context, marker), Color(theme.pageIndicatorActive),
				1.5f * context.uiScale);
		}
		if (clicked || focused)
		{
			const bool changedSection = state.section != index;
			state.section = index;
			state.row = std::min(state.row,
				sections[index].fields.empty() ? 0 : sections[index].fields.size() - 1);
			if (changedSection)
			{
				state.scroll = 0.0f;
				result.selectedSection = index;
			}
		}
	}

	std::vector<ActionListEntry> entries;
	entries.reserve(section.fields.size());
	for (const FormField& field : section.fields)
	{
		entries.push_back({
			.id = field.id,
			.label = field.label,
			.detail = field.kind == FormFieldKind::Toggle ? std::string{} : field.value,
			.enabled = field.enabled && field.kind != FormFieldKind::ReadOnly,
			.checkable = field.kind == FormFieldKind::Toggle,
			.checked = field.checked,
		});
	}
	ActionListState listState{ state.row, state.scroll };
	const ActionListOptions listOptions{
		.rowHeight = 50.0f,
		.separatorHeight = 8.0f,
		.footerHeight = 74.0f,
		.fontSizeUnits = 16.0f,
		.detailFontSizeUnits = 15.0f,
		.defaultFooter = options.footer,
	};
	if (state.choiceOpen)
		ImGui::BeginDisabled();
	const auto activatedField = DrawActionList(context, draw, panel,
		options.contentTop, listState, entries, font, listOptions);
	if (state.choiceOpen)
		ImGui::EndDisabled();
	bool openedChoiceThisFrame = false;
	if (activatedField)
	{
		state.row = *activatedField;
		result.activatedField = *activatedField;
		if (section.fields[*activatedField].kind == FormFieldKind::Choice &&
			!activeChoices.empty())
		{
			state.choiceOpen = true;
			openedChoiceThisFrame = true;
		}
	}
	state.row = listState.selected;
	state.scroll = listState.scroll;

	const float gap = 12.0f;
	const float buttonWidth = (panel.w - 56.0f) * 0.5f;
	const float buttonY = panel.bottom() - 55.0f;
	const std::array buttons{
		UnitRect{ panel.x + 22.0f, buttonY, buttonWidth, 40.0f },
		UnitRect{ panel.x + 22.0f + buttonWidth + gap, buttonY, buttonWidth, 40.0f },
	};
	const std::array<std::string_view, 2> labels{ options.saveLabel, options.cancelLabel };
	for (std::size_t index = 0; index < buttons.size(); ++index)
	{
		const bool clicked = !state.choiceOpen && HitTarget(context, buttons[index],
			23200 + static_cast<int>(index));
		const bool focused = !state.choiceOpen && ImGui::IsItemFocused();
		const bool hovered = !state.choiceOpen &&
			(ImGui::IsItemHovered() || focused);
		DrawShellPanelPx(draw, RectMinPx(context, buttons[index]),
			RectMaxPx(context, buttons[index]), theme, 10.0f * context.uiScale,
			hovered ? 0.74f : 0.50f);
		DrawReplicaFocus(draw, RectMinPx(context, buttons[index]),
			RectMaxPx(context, buttons[index]), 10.0f * context.uiScale,
			focused, hovered);
		const ImVec2 textSize = MeasureText(context, font, 15.0f, labels[index]);
		DrawText(draw, context, font, 15.0f,
			{ buttons[index].x + (buttons[index].w - textSize.x) * 0.5f,
			  buttons[index].y + 11.0f },
			hovered ? Color(theme.textPrimary) : Color(theme.textSecondary),
			labels[index]);
		if (clicked)
		{
			result.save = index == 0;
			result.cancel = index == 1;
		}
	}

	if (state.choiceOpen && !activeChoices.empty())
	{
		state.choiceSelection = std::min(state.choiceSelection,
			activeChoices.size() - 1);
		const float visibleRows = static_cast<float>(
			std::min<std::size_t>(activeChoices.size(), 8));
		const UnitRect choicePanel = CenteredPanel(context,
			std::min(520.0f, panel.w - 56.0f), 114.0f + visibleRows * 44.0f);
		DrawOverlayPanel(draw, context, choicePanel, theme, 1.0f);
		const std::string heading = section.fields.empty() ? std::string{} :
			section.fields[state.row].label;
		DrawText(draw, context, font, 20.0f,
			{ choicePanel.x + 24.0f, choicePanel.y + 20.0f },
			Color(theme.textPrimary), heading);
		std::vector<ActionListEntry> choices;
		choices.reserve(activeChoices.size());
		for (std::size_t index = 0; index < activeChoices.size(); ++index)
		{
			choices.push_back({
				.id = activeChoices[index].id,
				.label = activeChoices[index].label,
				.enabled = activeChoices[index].enabled,
				.checkable = true,
				.checked = state.choiceSelection == index,
			});
		}
		ActionListState choiceState{ state.choiceSelection, state.choiceScroll };
		choiceState.requestFocus = openedChoiceThisFrame;
		if (const auto activated = DrawActionList(context, draw, choicePanel,
			57.0f, choiceState, choices, font,
			{ .rowHeight = 44.0f, .footerHeight = 42.0f,
			  .fontSizeUnits = 16.0f }))
		{
			state.choiceSelection = *activated;
			state.choiceOpen = false;
			result.selectedChoice = *activated;
		}
		state.choiceSelection = choiceState.selected;
		state.choiceScroll = choiceState.scroll;
	}
	return result;
}

std::optional<std::size_t> DrawActionList(const ShellLayoutContext& context,
	ImDrawList* draw, const UnitRect& panel, float contentTopUnits,
	ActionListState& state, std::span<const ActionListEntry> entries,
	ImFont* font, const ActionListOptions& options)
{
	if (!draw || entries.empty())
		return std::nullopt;
	state.SetItemCount(entries.size());
	// one index drives both the fill and the focus ring: a navigation move updates the
	// selection, and a selection changed by the host moves ImGui focus onto it
	if (const ImGuiContext* imgui = ImGui::GetCurrentContext(); imgui && imgui->NavId != 0)
	{
		for (std::size_t index = 0; index < entries.size(); ++index)
		{
			ImGui::PushID(20000 + static_cast<int>(index));
			const ImGuiID rowId = ImGui::GetID("action-list-hit");
			ImGui::PopID();
			if (rowId != imgui->NavId)
				continue;
			if (index != state.selected)
			{
				if (imgui->NavJustMovedToId == imgui->NavId)
					state.selected = index;
				else
					state.requestFocus = true;
			}
			break;
		}
	}
	const auto& theme = CurrentWidgetTheme();
	const float contentHeight = std::max(120.0f,
		panel.h - contentTopUnits - options.footerHeight);
	const float totalHeight = ContentHeight(entries, options);
	const float maxScroll = std::max(0.0f, totalHeight - contentHeight);
	const float selectedTop = EntryTop(entries, state.selected, options);
	if (selectedTop < state.scroll)
		state.scroll = selectedTop;
	else if (selectedTop + options.rowHeight > state.scroll + contentHeight)
		state.scroll = selectedTop + options.rowHeight - contentHeight;
	state.scroll = std::clamp(state.scroll, 0.0f, maxScroll);

	const UnitRect contentRect{ panel.x + 18.0f, panel.y + contentTopUnits,
		panel.w - 36.0f, contentHeight };
	const ImVec2 contentMin = RectMinPx(context, contentRect);
	const ImVec2 contentMax = RectMaxPx(context, contentRect);
	if (ImGui::IsMouseHoveringRect(contentMin, contentMax, false) &&
		ImGui::GetIO().MouseWheel != 0.0f)
	{
		state.scroll = std::clamp(state.scroll - ImGui::GetIO().MouseWheel *
			options.rowHeight * 2.0f, 0.0f, maxScroll);
	}

	std::optional<std::size_t> activated;
	draw->PushClipRect(contentMin, contentMax, true);
	for (std::size_t index = 0; index < entries.size(); ++index)
	{
		const auto& entry = entries[index];
		const float itemTop = EntryTop(entries, index, options);
		UnitRect row{ contentRect.x + 3.0f,
			contentRect.y + itemTop - state.scroll,
			contentRect.w - 8.0f, options.rowHeight - 3.0f };
		if (row.bottom() < contentRect.y || row.y > contentRect.bottom())
			continue;
		if (entry.separatorBefore)
		{
			const ImVec2 start = ToPx(context, row.x + 8.0f,
				row.y - options.separatorHeight * 0.5f);
			const ImVec2 end = ToPx(context, row.right() - 8.0f,
				row.y - options.separatorHeight * 0.5f);
			draw->AddLine(start, end, ColorWithAlpha(theme.textSecondary, 0.18f),
				std::max(1.0f, context.uiScale));
		}
		const float hitTop = std::max(row.y, contentRect.y);
		const float hitBottom = std::min(row.bottom(), contentRect.bottom());
		const UnitRect hit{ row.x, hitTop, row.w,
			std::max(0.0f, hitBottom - hitTop) };
		bool focused = false;
		bool hovered = false;
		if (hit.h > 0.0f)
		{
			if (state.requestFocus && state.selected == index)
				ImGui::SetKeyboardFocusHere();
			if (HitTarget(context, hit, 20000 + static_cast<int>(index)))
			{
				state.selected = index;
				if (entry.enabled)
					activated = index;
			}
			focused = ImGui::IsItemFocused();
			hovered = ImGui::IsItemHovered() || focused;
		}
		const bool selected = state.selected == index;
		if (selected || hovered)
		{
			DrawShellPanelPx(draw, RectMinPx(context, row), RectMaxPx(context, row),
				theme, 10.0f * context.uiScale, selected ? 0.74f : 0.48f);
		}
		DrawReplicaFocus(draw, RectMinPx(context, row), RectMaxPx(context, row),
			11.0f * context.uiScale, focused, hovered);
		const ImU32 labelColor = entry.enabled ?
			(selected ? Color(theme.textPrimary) : Color(theme.textSecondary)) :
			ColorWithAlpha(theme.textSecondary, selected ? 0.66f : 0.42f);
		float labelX = row.x + 15.0f;
		if (entry.checkable)
		{
			const ImVec2 center = ToPx(context, row.x + 18.0f,
				row.y + row.h * 0.5f);
			draw->AddCircle(center, 6.5f * context.uiScale,
				ColorWithAlpha(theme.textSecondary, 0.70f), 20,
				std::max(1.0f, 1.5f * context.uiScale));
			if (entry.checked)
			{
				draw->AddCircleFilled(center, 3.5f * context.uiScale,
					Color(theme.pageIndicatorActive), 16);
			}
			labelX = row.x + 34.0f;
		}
		DrawText(draw, context, font, options.fontSizeUnits,
			{ labelX, row.y + 10.0f }, labelColor, entry.label);
		if (!entry.detail.empty())
		{
			const ImVec2 detailSize = MeasureText(context, font,
				options.detailFontSizeUnits, entry.detail);
			DrawText(draw, context, font, options.detailFontSizeUnits,
				{ row.right() - detailSize.x - 14.0f, row.y + 12.0f },
				ColorWithAlpha(theme.textSecondary, selected ? 0.66f : 0.40f),
				entry.detail);
		}
	}
	state.requestFocus = false;
	draw->PopClipRect();

	const std::string& footer = options.showDisabledReasonFooter &&
		!entries[state.selected].enabled && !entries[state.selected].detail.empty() ?
		entries[state.selected].detail : options.defaultFooter;
	if (!footer.empty())
	{
		DrawText(draw, context, font, 14.0f,
			{ panel.x + 24.0f, panel.bottom() - 34.0f },
			Color(theme.textSecondary), footer);
	}
	return activated;
}

std::optional<std::size_t> DrawConfirmationDialog(const DialogModel& model,
	ImVec2 size)
{
	if (model.popupId.empty())
		return std::nullopt;
	ImGui::SetNextWindowSize(size, ImGuiCond_Appearing);
	if (!BeginShellModal(model.popupId.c_str(), nullptr,
		ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings))
	{
		return std::nullopt;
	}
	if (ConsumeWidgetBackPressed())
	{
		ImGui::CloseCurrentPopup();
		EndShellModal();
		return std::nullopt;
	}
	if (!model.title.empty())
		SectionTitle(model.title.c_str());
	if (!model.message.empty())
		ImGui::TextWrapped("%s", model.message.c_str());
	std::optional<std::size_t> result;
	std::size_t defaultIndex = model.actions.empty() ? 0 :
		std::min(model.selectedAction, model.actions.size() - 1);
	if (!model.actions.empty() && !model.actions[defaultIndex].enabled)
	{
		const auto enabled = std::find_if(model.actions.begin(), model.actions.end(),
			[](const DialogAction& action) { return action.enabled; });
		if (enabled != model.actions.end())
			defaultIndex = static_cast<std::size_t>(enabled - model.actions.begin());
	}
	for (std::size_t index = 0; index < model.actions.size(); ++index)
	{
		const auto& action = model.actions[index];
		if (!action.enabled)
			ImGui::BeginDisabled();
		if (ShellButton(action.label.c_str(), { 180.0f, 48.0f }))
			result = index;
		if (!action.enabled)
			ImGui::EndDisabled();
		if (defaultIndex == index && action.enabled)
			ImGui::SetItemDefaultFocus();
		if (index + 1 < model.actions.size())
			ImGui::SameLine();
	}
	EndShellModal();
	return result;
}

void DrawProgressDialogBody(std::string_view message, float fraction,
	std::string_view detail)
{
	if (!message.empty())
		ImGui::TextWrapped("%.*s", static_cast<int>(message.size()), message.data());
	ShellProgressBar(fraction);
	if (!detail.empty())
		ImGui::TextDisabled("%.*s", static_cast<int>(detail.size()), detail.data());
}

bool BeginThemedModal(const char* id, const ShellLayoutContext& context,
	const ShellTheme& theme, ImVec2 size, bool* open, ImGuiWindowFlags flags)
{
	ImVec4 popupBackground = theme.panelBase;
	popupBackground.w = theme.dark ? 0.96f : 0.92f;
	ImVec4 border = theme.panelBorder;
	border.w = std::max(border.w, theme.dark ? 0.34f : 0.18f);
	ImVec4 frameBackground = theme.textSecondary;
	frameBackground.w = theme.dark ? 0.22f : 0.14f;
	ImVec4 separator = theme.cursorNormal;
	separator.w = 0.34f;
	ImVec4 dim = theme.background;
	dim.w = theme.dark ? 0.74f : 0.50f;
	ImGui::PushStyleColor(ImGuiCol_WindowBg, popupBackground);
	ImGui::PushStyleColor(ImGuiCol_PopupBg, popupBackground);
	ImGui::PushStyleColor(ImGuiCol_Border, border);
	ImGui::PushStyleColor(ImGuiCol_Text, theme.textPrimary);
	ImGui::PushStyleColor(ImGuiCol_TextDisabled, theme.textSecondary);
	ImGui::PushStyleColor(ImGuiCol_FrameBg, frameBackground);
	ImGui::PushStyleColor(ImGuiCol_PlotHistogram, theme.cursorNormal);
	ImGui::PushStyleColor(ImGuiCol_Separator, separator);
	ImGui::PushStyleColor(ImGuiCol_ModalWindowDimBg, dim);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 20.0f * context.uiScale);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize,
		std::max(1.0f, context.uiScale));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
		ToPx(context, 26.0f, 22.0f));
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
		ToPx(context, 10.0f, 10.0f));
	ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 12.0f * context.uiScale);
	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
		ToPx(context, 10.0f, 8.0f));
	if (const ImGuiViewport* viewport = ImGui::GetMainViewport())
	{
		ImGui::SetNextWindowPos({
			viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
			viewport->WorkPos.y + viewport->WorkSize.y * 0.5f,
		}, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
	}
	if (size.x > 0.0f || size.y > 0.0f)
		ImGui::SetNextWindowSize(size, ImGuiCond_Always);
	if (ImGui::BeginPopupModal(id, open, flags))
		return true;
	ImGui::PopStyleVar(6);
	ImGui::PopStyleColor(9);
	return false;
}

void EndThemedModal()
{
	ImGui::EndPopup();
	ImGui::PopStyleVar(6);
	ImGui::PopStyleColor(9);
}

void DrawDimmedBackground(ImU8 alpha)
{
	const ImGuiViewport* viewport = ImGui::GetMainViewport();
	if (!viewport)
		return;
	ImGui::GetWindowDrawList()->AddRectFilled(viewport->WorkPos,
		{ viewport->WorkPos.x + viewport->WorkSize.x,
			viewport->WorkPos.y + viewport->WorkSize.y }, IM_COL32(0, 0, 0, alpha));
}

UnitRect CenteredPanel(const ShellLayoutContext& context, float widthUnits,
	float heightUnits, float marginUnits) noexcept
{
	widthUnits = std::min(widthUnits,
		std::max(1.0f, context.layoutUnitsW - marginUnits * 2.0f));
	heightUnits = std::min(heightUnits,
		std::max(1.0f, context.layoutUnitsH - marginUnits * 2.0f));
	return {
		(context.layoutUnitsW - widthUnits) * 0.5f,
		(context.layoutUnitsH - heightUnits) * 0.5f,
		widthUnits,
		heightUnits,
	};
}

void DrawOverlayPanel(ImDrawList* draw, const ShellLayoutContext& context,
	const UnitRect& panel, const ShellTheme& theme, float opacity)
{
	if (!draw)
		return;
	DrawShellPanelPx(draw, RectMinPx(context, panel), RectMaxPx(context, panel),
		theme, 24.0f * context.uiScale, opacity);
}

void DrawOverlayHeader(ImDrawList* draw, const ShellLayoutContext& context,
	const UnitRect& panel, const ShellTheme& theme, std::string_view title,
	std::string_view subtitle, std::string_view trailing, ImFont* font)
{
	if (!draw)
		return;
	font = ResolveFrontendTextFont(font);
	DrawText(draw, context, font, 24.0f,
		{ panel.x + 24.0f, panel.y + 19.0f }, Color(theme.textPrimary), title);
	if (!subtitle.empty())
	{
		DrawText(draw, context, font, 15.0f,
			{ panel.x + 24.0f, panel.y + 52.0f },
			Color(theme.textSecondary), subtitle);
	}
	if (!trailing.empty())
	{
		const ImVec2 size = MeasureText(context, font, 14.0f, trailing);
		DrawText(draw, context, font, 14.0f,
			{ panel.right() - size.x - 24.0f, panel.y + 24.0f },
			Color(theme.textSecondary), trailing);
	}
}
}
