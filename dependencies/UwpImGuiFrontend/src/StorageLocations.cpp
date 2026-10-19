#include "UwpImGuiFrontend/StorageLocations.h"

#include "UwpImGuiFrontend/Text.h"
#include "UwpImGuiFrontend/Widgets.h"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <utility>
#include <vector>

namespace UwpImGuiFrontend
{
namespace
{
std::string PathToUtf8(const std::filesystem::path& path)
{
	const std::u8string value = path.generic_u8string();
	return { reinterpret_cast<const char*>(value.data()), value.size() };
}

std::string ComparablePath(const std::filesystem::path& path)
{
	std::string value = PathToUtf8(path.lexically_normal());
	std::replace(value.begin(), value.end(), '/', '\\');
	while (value.size() > 3 && value.back() == '\\')
		value.pop_back();
	std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
		return static_cast<char>(std::tolower(ch));
	});
	return value;
}
}

std::array<StorageLocationPreset, 2> MakeXboxStorageLocationPresets(
	std::filesystem::path developmentFiles,
	std::filesystem::path usbStorage)
{
	return {{
		{ "development-files", "Development files",
			std::move(developmentFiles) },
		{ "usb-storage", "USB storage", std::move(usbStorage) },
	}};
}

std::optional<std::size_t> FindStorageLocationPreset(
	const std::filesystem::path& path,
	std::span<const StorageLocationPreset> presets) noexcept
{
	if (path.empty())
		return std::nullopt;
	try
	{
		const std::string current = ComparablePath(path);
		for (std::size_t index = 0; index < presets.size(); ++index)
		{
			if (ComparablePath(presets[index].path) == current)
				return index;
		}
	}
	catch (...)
	{
	}
	return std::nullopt;
}

std::wstring MakeStorageLocationFutureAccessToken(std::string_view id)
{
	std::wstring token = L"uwp-imgui-folder-";
	if (id.empty())
		id = "storage-location";
	for (const unsigned char ch : id)
	{
		if (std::isalnum(ch) || ch == '-' || ch == '_')
			token.push_back(static_cast<wchar_t>(ch));
		else
			token.push_back(L'-');
	}
	return token;
}

StorageLocationSelectorResult DrawStorageLocationSelector(
	const StorageLocationSelectorModel& model, IPickerService& picker,
	const StorageLocationSelectorCallbacks& callbacks)
{
	StorageLocationSelectorResult result;
	std::vector<ChoiceItem> choices;
	choices.reserve(model.presets.size() + 2);

	const bool showPrompt = model.currentPath.empty() && !model.promptLabel.empty();
	if (showPrompt)
		choices.push_back({ "prompt", std::string(model.promptLabel), false });
	const std::size_t presetOffset = choices.size();
	for (const StorageLocationPreset& preset : model.presets)
		choices.push_back({ preset.id, preset.label, preset.enabled });
	const std::size_t customIndex = choices.size();
	choices.push_back({ "custom-folder", std::string(model.customLabel), true });

	std::size_t selectedIndex = customIndex;
	if (showPrompt)
	{
		selectedIndex = 0;
	}
	else if (const auto preset = FindStorageLocationPreset(
		model.currentPath, model.presets))
	{
		selectedIndex = presetOffset + *preset;
	}

	ImGui::PushID(model.id.data(), model.id.data() + model.id.size());
	const std::string label(model.label);
	const bool changed = ShellCombo(label.c_str(), &selectedIndex, choices);
	ImGui::PopID();
	if (changed)
	{
		result.selectionRequested = true;
		if (selectedIndex >= presetOffset && selectedIndex < customIndex)
		{
			const StorageLocationPreset& preset =
				model.presets[selectedIndex - presetOffset];
			if (callbacks.selected)
				callbacks.selected({ preset.id, preset.path, {}, false });
		}
		else if (selectedIndex == customIndex)
		{
			PickerRequest request = model.customPicker;
			if (model.persistCustomAccess && request.futureAccessToken.empty())
			{
				request.futureAccessToken =
					MakeStorageLocationFutureAccessToken(model.id);
			}
			PickerCallbacks pickerCallbacks;
			pickerCallbacks.completed = [callbacks](std::unique_ptr<PickedItem> item) {
				if (item && callbacks.selected)
				{
					callbacks.selected({ "custom-folder", std::move(item->path),
						std::move(item->futureAccessToken), true });
				}
			};
			pickerCallbacks.failed = callbacks.failed;
			pickerCallbacks.openStateChanged = callbacks.pickerOpenChanged;
			result.pickerStarted = picker.PickFolder(std::move(request),
				std::move(pickerCallbacks));
			if (!result.pickerStarted && callbacks.failed)
				callbacks.failed(Translate("A file or folder picker is already open"));
		}
	}

	if (model.showCurrentPath && !model.currentPath.empty())
	{
		const std::string path = PathToUtf8(model.currentPath);
		ImGui::TextWrapped("%.*s: %s",
			static_cast<int>(model.currentPathLabel.size()),
			model.currentPathLabel.data(), path.c_str());
	}
	return result;
}
}
