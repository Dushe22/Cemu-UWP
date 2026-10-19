#pragma once

#include <imgui.h>

#include <array>
#include <cstddef>

namespace UwpImGuiFrontend
{
struct UnitRect
{
	float x = 0.0f;
	float y = 0.0f;
	float w = 0.0f;
	float h = 0.0f;

	[[nodiscard]] float right() const noexcept { return x + w; }
	[[nodiscard]] float bottom() const noexcept { return y + h; }
	[[nodiscard]] UnitRect Expanded(float amount) const noexcept;
	[[nodiscard]] UnitRect Shrunk(float amount) const noexcept;
	[[nodiscard]] bool Contains(ImVec2 point) const noexcept;
};

struct ShellLayoutContext
{
	ImVec2 originPx{};
	ImVec2 viewportPx{};
	float uiScale = 1.0f;
	float layoutUnitsW = 1280.0f;
	float layoutUnitsH = 720.0f;
};

enum class ShellThemeMediaMode
{
	None,
	DynamicBackground,
	DynamicVideo,
	DynamicScreenshot,
	DynamicMixRecalboxV1,
	DynamicMixRecalboxV2,
};

struct ShellTheme
{
	const char* name = "";
	bool dark = false;
	ShellThemeMediaMode mediaMode = ShellThemeMediaMode::None;
	ImVec4 background{};
	ImVec4 backgroundAccent{};
	ImVec4 shapeColor{};
	ImVec4 panelBase{};
	ImVec4 panelBorder{};
	ImVec4 panelHighlight{};
	ImVec4 textPrimary{};
	ImVec4 textSecondary{};
	ImVec4 cursorNormal{};
	ImVec4 cursorGlow{};
	ImVec4 pageIndicator{};
	ImVec4 pageIndicatorActive{};
};

using ShellThemeCollection = std::array<ShellTheme, 13>;

[[nodiscard]] const ShellThemeCollection& ShellThemes() noexcept;
[[nodiscard]] ShellLayoutContext MakeShellLayout(const ImVec2& viewportPx,
	ImVec2 originPx = {}) noexcept;
[[nodiscard]] ImVec2 ToPx(const ShellLayoutContext& context, float x, float y) noexcept;
[[nodiscard]] ImVec2 ToPx(const ShellLayoutContext& context, ImVec2 point) noexcept;
[[nodiscard]] UnitRect UnitFromPx(const ShellLayoutContext& context, ImVec2 point) noexcept;
[[nodiscard]] ImVec2 UnitPointFromPx(const ShellLayoutContext& context, ImVec2 point) noexcept;
[[nodiscard]] ImVec2 RectMinPx(const ShellLayoutContext& context, const UnitRect& rect) noexcept;
[[nodiscard]] ImVec2 RectMaxPx(const ShellLayoutContext& context, const UnitRect& rect) noexcept;
[[nodiscard]] ImU32 ColorWithAlpha(ImVec4 color, float alphaScale) noexcept;
[[nodiscard]] ImVec2 PixelFloor(ImVec2 point) noexcept;
[[nodiscard]] ImVec2 PixelCeil(ImVec2 point) noexcept;

void NormalizeRoundedRectPx(ImVec2& min, ImVec2& max) noexcept;
void AddRoundedRectStrokePx(ImDrawList* draw, ImVec2 min, ImVec2 max, ImU32 color,
	float rounding, float thickness, ImDrawFlags flags = ImDrawFlags_RoundCornersAll);
void AddRoundedRectFilledPx(ImDrawList* draw, ImVec2 min, ImVec2 max, ImU32 color,
	float rounding, ImDrawFlags flags = ImDrawFlags_RoundCornersAll);
void DrawShellPanelPx(ImDrawList* draw, ImVec2 min, ImVec2 max, const ShellTheme& theme,
	float radiusPx, float opacity);

enum class ShellEasing
{
	OutCubic,
	OutExpo,
	OutBack,
};

[[nodiscard]] float Ease(ShellEasing easing, float t) noexcept;

struct AnimatedFloat
{
	float value = 0.0f;
	float from = 0.0f;
	float target = 0.0f;
	float duration = 0.0f;
	float elapsed = 0.0f;
	ShellEasing easing = ShellEasing::OutCubic;

	void SetImmediate(float value) noexcept;
	void Set(float value, float seconds, ShellEasing easing = ShellEasing::OutCubic) noexcept;
	void Update(float deltaSeconds) noexcept;
};

struct CursorAnimation
{
	bool initialized = false;
	float time = 0.0f;
	AnimatedFloat x;
	AnimatedFloat y;
	AnimatedFloat w;
	AnimatedFloat h;
	AnimatedFloat radius;

	[[nodiscard]] float AdaptiveDuration(const UnitRect& target, float targetRadius,
		float baseDuration) const noexcept;
	void MoveTo(const UnitRect& target, float targetRadius, float baseDuration) noexcept;
	void Update(float deltaSeconds) noexcept;
	void Draw(const ShellLayoutContext& context, ImDrawList* draw,
		const ShellTheme& theme) const;
	// the parts of Draw, for a cursor whose glow lies under what it surrounds
	void DrawGlow(const ShellLayoutContext& context, ImDrawList* draw,
		const ShellTheme& theme) const;
	void DrawBorder(const ShellLayoutContext& context, ImDrawList* draw,
		const ShellTheme& theme) const;
};
}
