#pragma once

#include "Pickers.h"

#include <array>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace UwpImGuiFrontend
{
struct StorageLocationPreset
{
	std::string id;
	std::string label;
	std::filesystem::path path;
	bool enabled = true;
};

struct StorageLocationSelectorModel
{
	std::string_view id = "storage-location";
	std::string_view label = "Storage location";
	std::filesystem::path currentPath;
	std::span<const StorageLocationPreset> presets;
	std::string_view customLabel = "Custom folder";
	// when set and currentPath is empty, the combo acts as an action selector
	std::string_view promptLabel;
	std::string_view currentPathLabel = "Path";
	PickerRequest customPicker;
	bool showCurrentPath = true;
	// derives a FutureAccessList token from id when customPicker has none
	bool persistCustomAccess = true;
};

struct StorageLocationSelection
{
	std::string id;
	std::filesystem::path path;
	std::wstring futureAccessToken;
	bool custom = false;
};

struct StorageLocationSelectorCallbacks
{
	std::function<void(StorageLocationSelection)> selected;
	std::function<void(std::string_view)> failed;
	std::function<void(bool)> pickerOpenChanged;
};

struct StorageLocationSelectorResult
{
	bool selectionRequested = false;
	bool pickerStarted = false;
};

[[nodiscard]] std::array<StorageLocationPreset, 2>
MakeXboxStorageLocationPresets(std::filesystem::path developmentFiles,
	std::filesystem::path usbStorage);

[[nodiscard]] std::optional<std::size_t> FindStorageLocationPreset(
	const std::filesystem::path& path,
	std::span<const StorageLocationPreset> presets) noexcept;

[[nodiscard]] std::wstring MakeStorageLocationFutureAccessToken(
	std::string_view id);

[[nodiscard]] StorageLocationSelectorResult DrawStorageLocationSelector(
	const StorageLocationSelectorModel& model, IPickerService& picker,
	const StorageLocationSelectorCallbacks& callbacks = {});
}
