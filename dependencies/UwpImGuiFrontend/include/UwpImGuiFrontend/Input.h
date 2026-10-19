#pragma once

#include "Types.h"

namespace UwpImGuiFrontend
{
enum class Direction : std::uint8_t
{
	Up,
	Down,
	Left,
	Right,
};

struct FrameInput
{
	float deltaSeconds = 1.0f / 60.0f;
	bool up = false;
	bool down = false;
	bool left = false;
	bool right = false;
	bool accept = false;
	bool back = false;
	bool context = false;
	bool alternate = false;
	bool menu = false;
	bool view = false;
	bool leftShoulder = false;
	bool rightShoulder = false;
	// held until input is released so a fresh capture cannot leak into navigation
	bool controllerInputHeld = false;
	float leftStickX = 0.0f;
	float leftStickY = 0.0f;
	float rightStickX = 0.0f;
	float rightStickY = 0.0f;
	std::optional<Vec2> pointer;
	bool pointerPressed = false;
};

struct NavigationState
{
	bool up = false;
	bool down = false;
	bool left = false;
	bool right = false;
};
}
