#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace UwpQuickMenuBinding
{
using Mask = std::uint16_t;
inline constexpr Mask A = 1u << 0, B = 1u << 1, X = 1u << 2, Y = 1u << 3;
inline constexpr Mask Up = 1u << 4, Down = 1u << 5, Left = 1u << 6, Right = 1u << 7;
inline constexpr Mask LB = 1u << 8, RB = 1u << 9, Menu = 1u << 10, View = 1u << 11;
inline constexpr Mask AllButtons = (1u << 12) - 1;
inline constexpr Mask DefaultMask = Menu | View;
inline constexpr char DefaultBinding[] = "Gamepad:Menu+View";

constexpr bool IsCombo(Mask mask) noexcept
{
	return (mask & ~AllButtons) == 0 && mask != 0 && (mask & (mask - 1)) != 0;
}

inline std::string CompactLower(std::string_view value)
{
	std::string result;
	result.reserve(value.size());
	for (char c : value)
	{
		if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v')
			continue;
		result += c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c;
	}
	return result;
}

inline Mask TokenMask(std::string_view token) noexcept
{
	if (token == "a") return A;
	if (token == "b" || token == "back") return B;
	if (token == "x") return X;
	if (token == "y") return Y;
	if (token == "up" || token == "dpadup") return Up;
	if (token == "down" || token == "dpaddown") return Down;
	if (token == "left" || token == "dpadleft") return Left;
	if (token == "right" || token == "dpadright") return Right;
	if (token == "lb" || token == "l1" || token == "leftshoulder") return LB;
	if (token == "rb" || token == "r1" || token == "rightshoulder") return RB;
	if (token == "menu" || token == "start") return Menu;
	if (token == "view" || token == "select") return View;
	return 0;
}

// aliases like Menu+Start map to one button and do not form a combo
inline Mask Parse(std::string_view binding)
{
	const std::string normalized = CompactLower(binding);
	std::string_view combo = normalized;
	const auto colon = combo.find(':');
	if (colon != std::string_view::npos)
	{
		if (combo.substr(0, colon) != "gamepad")
			return 0;
		combo.remove_prefix(colon + 1);
	}
	Mask mask = 0;
	while (true)
	{
		const auto plus = combo.find('+');
		const Mask button = TokenMask(combo.substr(0, plus));
		if (button == 0 || (mask & button) != 0)
			return 0;
		mask |= button;
		if (plus == std::string_view::npos)
			break;
		combo.remove_prefix(plus + 1);
	}
	return IsCombo(mask) ? mask : 0;
}

inline Mask EffectiveMask(std::string_view binding)
{
	const Mask mask = Parse(binding);
	return mask != 0 ? mask : DefaultMask;
}

inline std::string Normalize(std::string_view binding)
{
	const Mask mask = EffectiveMask(binding);
	constexpr std::array names{ "A", "B", "X", "Y", "DPadUp", "DPadDown",
		"DPadLeft", "DPadRight", "LB", "RB", "Menu", "View" };
	std::string result = "Gamepad:";
	bool first = true;
	for (std::size_t i = 0; i < names.size(); ++i)
	{
		if ((mask & (1u << i)) == 0)
			continue;
		if (!first)
			result += '+';
		result += names[i];
		first = false;
	}
	return result;
}

constexpr Mask Buttons(bool a, bool b, bool x, bool y, bool up, bool down,
	bool left, bool right, bool lb, bool rb, bool menu, bool view) noexcept
{
	return (a ? A : 0) | (b ? B : 0) | (x ? X : 0) | (y ? Y : 0) |
		(up ? Up : 0) | (down ? Down : 0) | (left ? Left : 0) | (right ? Right : 0) |
		(lb ? LB : 0) | (rb ? RB : 0) | (menu ? Menu : 0) | (view ? View : 0);
}

struct Snapshot
{
	Mask binding = DefaultMask;
	bool down = false;

	// per controller; Menu on one pad and View on another must not combine
	constexpr void ObserveGamepad(Mask buttons) noexcept
	{
		down |= IsCombo(binding) && (buttons & binding) == binding;
	}
};

constexpr bool Pressed(const Snapshot& now, const Snapshot& previous) noexcept
{
	// rebinding while buttons are held is not a new press
	return IsCombo(now.binding) && now.binding == previous.binding && now.down && !previous.down;
}
}
