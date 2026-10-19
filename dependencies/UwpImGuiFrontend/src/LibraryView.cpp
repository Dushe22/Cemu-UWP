#include "UwpImGuiFrontend/LibraryView.h"

#include "UwpImGuiFrontend/Widgets.h"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace UwpImGuiFrontend
{
namespace
{
std::string ContentIdLabel(std::uint64_t contentId)
{
	if (contentId == 0)
		return {};
	std::array<char, 17> text{};
	std::snprintf(text.data(), text.size(), "%016llX",
		static_cast<unsigned long long>(contentId));
	return text.data();
}
}

LibraryViewResult LibraryView::Draw(const LibraryViewModel& model,
	const LibraryViewCallbacks& callbacks, ImVec2 size)
{
	LibraryViewResult result;
	const float controlsStartY = ImGui::GetCursorPosY();
	const bool hasGamesFolderSelector = model.gamesFolderSelector.has_value() &&
		model.picker != nullptr;
	bool toolbarFocused = false;
	if (model.showHeading)
		SectionTitle("Library");
	if (model.showAddContent)
	{
		if (m_focusToolbar)
			ImGui::SetKeyboardFocusHere();
		const char* label = model.addGamesFolderLabel.empty() ?
			"Add games folder" : model.addGamesFolderLabel.c_str();
		result.addContent = ShellButton(label, { 220.0f, 42.0f });
		toolbarFocused |= ImGui::IsItemFocused();
		if (result.addContent)
		{
			if (hasGamesFolderSelector)
				OpenAddGamesFolder();
			else if (callbacks.addContent)
				callbacks.addContent();
		}
	}
	if (model.showRefresh)
	{
		if (model.showAddContent)
			ImGui::SameLine();
		if (m_focusToolbar && !model.showAddContent)
			ImGui::SetKeyboardFocusHere();
		result.refresh = ShellButton("Refresh", { 150.0f, 42.0f });
		toolbarFocused |= ImGui::IsItemFocused();
		if (result.refresh && callbacks.refresh)
			callbacks.refresh();
	}
	m_focusToolbar = false;
	if (toolbarFocused && !m_addGamesFolderOpen &&
		(ImGui::IsKeyPressed(ImGuiKey_GamepadDpadDown) ||
		 ImGui::IsKeyPressed(ImGuiKey_GamepadLStickDown)))
	{
		m_table.requestFocus = true;
	}

	if (m_addGamesFolderOpen && !hasGamesFolderSelector)
		CloseAddGamesFolder();
	if (m_addGamesFolderOpen)
	{
		ImGui::Spacing();
		if (m_focusGamesFolderSelector)
		{
			ImGui::SetKeyboardFocusHere();
			m_focusGamesFolderSelector = false;
		}
		StorageLocationSelectorCallbacks selectorCallbacks = callbacks.addGamesFolder;
		const auto selected = selectorCallbacks.selected;
		selectorCallbacks.selected = [this, selected](StorageLocationSelection selection) {
			CloseAddGamesFolder();
			if (selected)
				selected(std::move(selection));
		};
		result.gamesFolderSelection = DrawStorageLocationSelector(
			*model.gamesFolderSelector, *model.picker, selectorCallbacks);
		if (ConsumeWidgetBackPressed())
			CloseAddGamesFolder();
		result.gamesFolderSelectorOpen = m_addGamesFolderOpen;
		// a preset callback may refresh the catalogue, model.items is stale past here
		if (result.gamesFolderSelection.selectionRequested)
			return result;
	}
	result.gamesFolderSelectorOpen = m_addGamesFolderOpen;

	if (model.items.empty())
	{
		ImGui::Spacing();
		ImGui::TextDisabled("%s", model.emptyLabel.c_str());
		m_table.SetItemCount(0);
		return result;
	}

	const std::array columns{
		DataTableColumn{ "artwork", "", DataTableColumnKind::Artwork, 82.0f },
		DataTableColumn{ "title", "Title", DataTableColumnKind::PrimaryText,
			2.6f, true },
		DataTableColumn{ "platform", "Platform", DataTableColumnKind::Text,
			1.0f, true },
		DataTableColumn{ "content-id", "Content ID", DataTableColumnKind::Text,
			168.0f },
		DataTableColumn{ "version", "Version", DataTableColumnKind::Text,
			84.0f },
		DataTableColumn{ "region", "Region", DataTableColumnKind::Text,
			90.0f },
		DataTableColumn{ "action", "Action", DataTableColumnKind::Action,
			104.0f },
	};
	std::vector<DataTableRow> rows;
	rows.reserve(model.items.size());
	for (const LibraryItem& item : model.items)
	{
		rows.push_back({ item.id, {
			{ {}, callbacks.artwork ? callbacks.artwork(item) : TextureHandle{} },
			{ item.name },
			{ item.platform },
			{ ContentIdLabel(item.contentId) },
			{ item.version == 0 ? std::string{} : std::to_string(item.version) },
			{ item.region },
			{ "Open" },
		} });
	}

	DataTableOptions options;
	options.size = size;
	if (options.size.y > 0.0f)
	{
		const float controlsHeight = std::max(0.0f,
			ImGui::GetCursorPosY() - controlsStartY);
		options.size.y = std::max(1.0f, options.size.y - controlsHeight);
	}
	options.hideScrollbar = true;
	options.showHeaders = false;
	options.controllerNavigation = true;
	options.artworkHeight = 58.0f;
	options.focusedArtworkScale = 1.20f;
	options.controllerScroll = model.controllerScroll;
	const DataTableResult tableResult = DrawDataTable("portable-library",
		m_table, columns, rows, options);
	if (tableResult.navigateBeforeTable &&
		(model.showAddContent || model.showRefresh))
	{
		m_focusToolbar = true;
	}
	if (tableResult.selectionChanged && *tableResult.selectionChanged < model.items.size())
	{
		const LibraryItem& item = model.items[*tableResult.selectionChanged];
		result.selectionChanged = item.id;
		if (callbacks.selectionChanged)
			callbacks.selectionChanged(item);
	}
	if (tableResult.activatedRow && *tableResult.activatedRow < model.items.size())
	{
		const LibraryItem& item = model.items[*tableResult.activatedRow];
		result.activated = item.id;
		if (callbacks.launch)
			callbacks.launch(item);
	}
	if (model.contextRequested && tableResult.rowFocused &&
		m_table.selected < model.items.size())
	{
		const LibraryItem& item = model.items[m_table.selected];
		result.contextItem = item.id;
		if (callbacks.openContextMenu)
			callbacks.openContextMenu(item);
	}
	return result;
}

void LibraryView::Select(std::size_t index, std::size_t count) noexcept
{
	m_table.Select(index, count);
	m_table.requestFocus = count != 0;
}

std::size_t LibraryView::SelectedIndex() const noexcept
{
	return m_table.selected;
}

std::optional<ItemId> LibraryView::SelectedItem(
	std::span<const LibraryItem> items) const noexcept
{
	if (items.empty() || m_table.selected >= items.size())
		return std::nullopt;
	return items[m_table.selected].id;
}

void LibraryView::OpenAddGamesFolder() noexcept
{
	m_addGamesFolderOpen = true;
	m_focusGamesFolderSelector = true;
}

void LibraryView::CloseAddGamesFolder() noexcept
{
	m_addGamesFolderOpen = false;
	m_focusGamesFolderSelector = false;
}

bool LibraryView::IsAddGamesFolderOpen() const noexcept
{
	return m_addGamesFolderOpen;
}
}
