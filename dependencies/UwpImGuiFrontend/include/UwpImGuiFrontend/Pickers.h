#pragma once

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace UwpImGuiFrontend
{
enum class PickerStartLocation
{
	Computer,
	Documents,
	Pictures,
	Videos,
	Music,
	Desktop,
	Downloads,
};

struct PickerRequest
{
	PickerStartLocation startLocation = PickerStartLocation::Computer;
	std::vector<std::wstring> fileTypes;
	std::wstring futureAccessToken;
};

struct PickedItem
{
	std::filesystem::path path;
	std::wstring name;
	std::wstring futureAccessToken;
	bool folder = false;
};

struct PickerCallbacks
{
	std::function<void(std::unique_ptr<PickedItem>)> completed;
	std::function<void(std::string_view)> failed;
	std::function<void(bool)> openStateChanged;
};

class IPickerService
{
public:
	virtual ~IPickerService() = default;
	[[nodiscard]] virtual bool PickFile(PickerRequest request,
		PickerCallbacks callbacks) = 0;
	[[nodiscard]] virtual bool PickFolder(PickerRequest request,
		PickerCallbacks callbacks) = 0;
	[[nodiscard]] virtual bool IsOpen() const noexcept = 0;
	virtual void CancelCallbacks() noexcept = 0;
};

[[nodiscard]] std::unique_ptr<IPickerService> CreatePickerService();
}
