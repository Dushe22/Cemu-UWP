#pragma once

#include "StorageLocations.h"
#include "Types.h"
#include "Views.h"

#include <functional>
#include <optional>
#include <span>

namespace UwpImGuiFrontend
{
struct LibraryViewModel
{
	std::span<const LibraryItem> items;
	bool showAddContent = true;
	bool showRefresh = true;
	bool showHeading = false;
	bool contextRequested = false;
	float controllerScroll = 0.0f;
	std::string emptyLabel = "No content found.";
	std::string addGamesFolderLabel = "Add games folder";
	std::optional<StorageLocationSelectorModel> gamesFolderSelector;
	IPickerService* picker = nullptr;
};

struct LibraryViewCallbacks
{
	std::function<TextureHandle(const LibraryItem&)> artwork;
	std::function<void(const LibraryItem&)> launch;
	std::function<void(const LibraryItem&)> openContextMenu;
	std::function<void(const LibraryItem&)> selectionChanged;
	std::function<void()> addContent;
	std::function<void()> refresh;
	StorageLocationSelectorCallbacks addGamesFolder;
};

struct LibraryViewResult
{
	std::optional<ItemId> activated;
	std::optional<ItemId> contextItem;
	std::optional<ItemId> selectionChanged;
	bool addContent = false;
	bool refresh = false;
	bool gamesFolderSelectorOpen = false;
	StorageLocationSelectorResult gamesFolderSelection;
};

class LibraryView
{
public:
	[[nodiscard]] LibraryViewResult Draw(const LibraryViewModel& model,
		const LibraryViewCallbacks& callbacks,
		ImVec2 size = ImVec2(0.0f, 0.0f));

	void Select(std::size_t index, std::size_t count) noexcept;
	[[nodiscard]] std::size_t SelectedIndex() const noexcept;
	[[nodiscard]] std::optional<ItemId> SelectedItem(
		std::span<const LibraryItem> items) const noexcept;
	void OpenAddGamesFolder() noexcept;
	void CloseAddGamesFolder() noexcept;
	[[nodiscard]] bool IsAddGamesFolderOpen() const noexcept;

private:
	DataTableState m_table;
	bool m_addGamesFolderOpen = false;
	bool m_focusGamesFolderSelector = false;
	bool m_focusToolbar = false;
};
}
