#include "input/api/WGI/WGIGamepadControllerProvider.h"

#if defined(CEMU_UWP)

#include "input/api/WGI/WGIGamepadController.h"

std::vector<std::shared_ptr<ControllerBase>> WGIGamepadControllerProvider::get_controllers()
{
	std::vector<std::shared_ptr<ControllerBase>> result;
	const auto gamepads = winrt::Windows::Gaming::Input::Gamepad::Gamepads();
	result.reserve(gamepads.Size());
	for (uint32 i = 0; i < gamepads.Size(); ++i)
		result.emplace_back(std::make_shared<WGIGamepadController>(i));
	return result;
}

winrt::Windows::Gaming::Input::Gamepad WGIGamepadControllerProvider::GetGamepad(uint32 index) const
{
	const auto gamepads = winrt::Windows::Gaming::Input::Gamepad::Gamepads();
	if (index >= gamepads.Size())
		return nullptr;
	return gamepads.GetAt(index);
}

winrt::Windows::Gaming::Input::Gamepad WGIGamepadControllerProvider::GetGamepad(
	std::string_view nonRoamableId, uint32 fallbackIndex) const
{
	if (!nonRoamableId.empty())
	{
		const auto gamepads = winrt::Windows::Gaming::Input::Gamepad::Gamepads();
		for (const auto& gamepad : gamepads)
		{
			if (GetNonRoamableId(gamepad) == nonRoamableId)
				return gamepad;
		}
		// the saved identity can stop matching (BOTW's profile lost all input after the pad came back with another
		// id); with a single pad connected there is no other player it could belong to
		if (gamepads.Size() == 1)
			return gamepads.GetAt(0);
		return nullptr;
	}
	return GetGamepad(fallbackIndex);
}

std::string WGIGamepadControllerProvider::GetNonRoamableId(
	const winrt::Windows::Gaming::Input::Gamepad& gamepad)
{
	if (!gamepad)
		return {};
	const auto raw = winrt::Windows::Gaming::Input::RawGameController::
		FromGameController(gamepad);
	return raw ? winrt::to_string(raw.NonRoamableId()) : std::string{};
}

#endif
