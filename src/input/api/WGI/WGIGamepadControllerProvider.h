#pragma once

#if defined(CEMU_UWP)

#include "input/api/ControllerProvider.h"

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Gaming.Input.h>

class WGIGamepadControllerProvider : public ControllerProviderBase
{
public:
	inline static InputAPI::Type kAPIType = InputAPI::WGIGamepad;
	InputAPI::Type api() const override { return kAPIType; }

	std::vector<std::shared_ptr<ControllerBase>> get_controllers() override;

	winrt::Windows::Gaming::Input::Gamepad GetGamepad(uint32 index) const;
	winrt::Windows::Gaming::Input::Gamepad GetGamepad(
		std::string_view nonRoamableId, uint32 fallbackIndex) const;
	static std::string GetNonRoamableId(
		const winrt::Windows::Gaming::Input::Gamepad& gamepad);
};

#endif
