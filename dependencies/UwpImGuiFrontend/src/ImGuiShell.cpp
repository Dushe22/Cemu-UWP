#include "UwpImGuiFrontend/ImGuiShell.h"

#include <algorithm>
#include <cmath>

namespace UwpImGuiFrontend
{
UnitRect UnitRect::Expanded(float amount) const noexcept
{
	return { x - amount, y - amount, w + amount * 2.0f, h + amount * 2.0f };
}

UnitRect UnitRect::Shrunk(float amount) const noexcept
{
	return { x + amount, y + amount, std::max(0.0f, w - amount * 2.0f),
		std::max(0.0f, h - amount * 2.0f) };
}

bool UnitRect::Contains(ImVec2 point) const noexcept
{
	return point.x >= x && point.x <= right() && point.y >= y && point.y <= bottom();
}

const ShellThemeCollection& ShellThemes() noexcept
{
	static const ShellThemeCollection themes{{
		{
			"Default Light",
			false,
			ShellThemeMediaMode::None,
			{ 0.92f, 0.93f, 0.96f, 1.0f },
			{ 0.85f, 0.88f, 0.95f, 1.0f },
			{ 0.55f, 0.58f, 0.80f, 0.10f },
			{ 1.00f, 1.00f, 1.00f, 0.55f },
			{ 0.00f, 0.00f, 0.00f, 0.06f },
			{ 1.00f, 1.00f, 1.00f, 0.55f },
			{ 0.10f, 0.10f, 0.15f, 1.00f },
			{ 0.30f, 0.30f, 0.40f, 0.85f },
			{ 0.00f, 0.48f, 1.00f, 1.00f },
			{ 0.00f, 0.48f, 1.00f, 0.15f },
			{ 0.30f, 0.30f, 0.40f, 0.30f },
			{ 0.15f, 1.00f, 0.447f, 1.00f },
		},
		{
			"Default Dark",
			true,
			ShellThemeMediaMode::None,
			{ 0.04f, 0.04f, 0.08f, 1.0f },
			{ 0.08f, 0.06f, 0.16f, 1.0f },
			{ 0.22f, 0.18f, 0.38f, 0.10f },
			{ 0.16f, 0.16f, 0.20f, 0.38f },
			{ 0.55f, 0.55f, 0.60f, 0.12f },
			{ 1.00f, 1.00f, 1.00f, 0.10f },
			{ 1.00f, 1.00f, 1.00f, 1.00f },
			{ 0.70f, 0.72f, 0.80f, 0.80f },
			{ 0.35f, 0.55f, 1.00f, 1.00f },
			{ 0.35f, 0.55f, 1.00f, 0.12f },
			{ 1.00f, 1.00f, 1.00f, 0.25f },
			{ 0.15f, 1.00f, 0.447f, 1.00f },
		},
		{
			"Ocean Blue",
			false,
			ShellThemeMediaMode::None,
			{ 0.90f, 0.97f, 1.00f, 1.0f },
			{ 0.70f, 0.88f, 1.00f, 1.0f },
			{ 0.36f, 0.58f, 0.82f, 0.10f },
			{ 1.00f, 1.00f, 1.00f, 0.58f },
			{ 0.02f, 0.13f, 0.24f, 0.08f },
			{ 1.00f, 1.00f, 1.00f, 0.55f },
			{ 0.06f, 0.12f, 0.18f, 1.00f },
			{ 0.20f, 0.36f, 0.48f, 0.85f },
			{ 0.00f, 0.50f, 1.00f, 1.00f },
			{ 0.00f, 0.50f, 1.00f, 0.15f },
			{ 0.20f, 0.36f, 0.48f, 0.30f },
			{ 0.05f, 0.85f, 0.72f, 1.00f },
		},
		{
			"Mint",
			false,
			ShellThemeMediaMode::None,
			{ 0.93f, 0.98f, 0.95f, 1.0f },
			{ 0.78f, 0.94f, 0.88f, 1.0f },
			{ 0.28f, 0.62f, 0.50f, 0.10f },
			{ 1.00f, 1.00f, 1.00f, 0.58f },
			{ 0.02f, 0.18f, 0.13f, 0.08f },
			{ 1.00f, 1.00f, 1.00f, 0.55f },
			{ 0.08f, 0.15f, 0.13f, 1.00f },
			{ 0.24f, 0.40f, 0.35f, 0.86f },
			{ 0.00f, 0.68f, 0.48f, 1.00f },
			{ 0.00f, 0.68f, 0.48f, 0.15f },
			{ 0.24f, 0.40f, 0.35f, 0.30f },
			{ 0.00f, 0.52f, 1.00f, 1.00f },
		},
		{
			"Coral",
			false,
			ShellThemeMediaMode::None,
			{ 1.00f, 0.94f, 0.91f, 1.0f },
			{ 1.00f, 0.83f, 0.82f, 1.0f },
			{ 0.78f, 0.42f, 0.40f, 0.10f },
			{ 1.00f, 1.00f, 1.00f, 0.58f },
			{ 0.24f, 0.05f, 0.06f, 0.08f },
			{ 1.00f, 1.00f, 1.00f, 0.55f },
			{ 0.18f, 0.09f, 0.10f, 1.00f },
			{ 0.48f, 0.27f, 0.28f, 0.86f },
			{ 1.00f, 0.33f, 0.37f, 1.00f },
			{ 1.00f, 0.33f, 0.37f, 0.15f },
			{ 0.48f, 0.27f, 0.28f, 0.30f },
			{ 1.00f, 0.74f, 0.22f, 1.00f },
		},
		{
			"Violet",
			true,
			ShellThemeMediaMode::None,
			{ 0.07f, 0.06f, 0.11f, 1.0f },
			{ 0.13f, 0.09f, 0.22f, 1.0f },
			{ 0.34f, 0.26f, 0.54f, 0.11f },
			{ 0.18f, 0.15f, 0.25f, 0.44f },
			{ 0.68f, 0.60f, 0.78f, 0.13f },
			{ 1.00f, 1.00f, 1.00f, 0.11f },
			{ 1.00f, 1.00f, 1.00f, 1.00f },
			{ 0.76f, 0.72f, 0.86f, 0.82f },
			{ 0.63f, 0.45f, 1.00f, 1.00f },
			{ 0.63f, 0.45f, 1.00f, 0.13f },
			{ 1.00f, 1.00f, 1.00f, 0.25f },
			{ 0.13f, 0.93f, 0.84f, 1.00f },
		},
		{
			"Mono",
			false,
			ShellThemeMediaMode::None,
			{ 0.92f, 0.93f, 0.94f, 1.0f },
			{ 0.76f, 0.78f, 0.82f, 1.0f },
			{ 0.42f, 0.44f, 0.48f, 0.10f },
			{ 1.00f, 1.00f, 1.00f, 0.58f },
			{ 0.00f, 0.00f, 0.00f, 0.07f },
			{ 1.00f, 1.00f, 1.00f, 0.55f },
			{ 0.08f, 0.09f, 0.11f, 1.00f },
			{ 0.32f, 0.34f, 0.38f, 0.86f },
			{ 0.12f, 0.44f, 0.86f, 1.00f },
			{ 0.12f, 0.44f, 0.86f, 0.15f },
			{ 0.32f, 0.34f, 0.38f, 0.30f },
			{ 0.18f, 0.72f, 0.62f, 1.00f },
		},
		{
			"Sunset",
			true,
			ShellThemeMediaMode::None,
			{ 0.07f, 0.06f, 0.08f, 1.0f },
			{ 0.15f, 0.08f, 0.11f, 1.0f },
			{ 0.50f, 0.24f, 0.20f, 0.11f },
			{ 0.20f, 0.14f, 0.16f, 0.44f },
			{ 0.86f, 0.62f, 0.52f, 0.13f },
			{ 1.00f, 1.00f, 1.00f, 0.11f },
			{ 1.00f, 0.98f, 0.94f, 1.00f },
			{ 0.86f, 0.75f, 0.72f, 0.84f },
			{ 1.00f, 0.44f, 0.25f, 1.00f },
			{ 1.00f, 0.44f, 0.25f, 0.13f },
			{ 1.00f, 0.98f, 0.94f, 0.25f },
			{ 1.00f, 0.76f, 0.20f, 1.00f },
		},
		{
			"Dynamic Backgrounds",
			true,
			ShellThemeMediaMode::DynamicBackground,
			{ 0.04f, 0.04f, 0.08f, 1.0f },
			{ 0.08f, 0.08f, 0.12f, 1.0f },
			{ 0.70f, 0.78f, 1.00f, 0.07f },
			{ 0.11f, 0.12f, 0.16f, 0.52f },
			{ 0.80f, 0.86f, 1.00f, 0.16f },
			{ 1.00f, 1.00f, 1.00f, 0.12f },
			{ 1.00f, 1.00f, 1.00f, 1.00f },
			{ 0.78f, 0.82f, 0.90f, 0.84f },
			{ 0.28f, 0.66f, 1.00f, 1.00f },
			{ 0.28f, 0.66f, 1.00f, 0.14f },
			{ 1.00f, 1.00f, 1.00f, 0.27f },
			{ 0.18f, 1.00f, 0.62f, 1.00f },
		},
		{
			"Dynamic Videos",
			true,
			ShellThemeMediaMode::DynamicVideo,
			{ 0.03f, 0.03f, 0.05f, 1.0f },
			{ 0.06f, 0.06f, 0.09f, 1.0f },
			{ 0.55f, 0.72f, 1.00f, 0.06f },
			{ 0.10f, 0.11f, 0.15f, 0.54f },
			{ 0.76f, 0.84f, 1.00f, 0.17f },
			{ 1.00f, 1.00f, 1.00f, 0.12f },
			{ 1.00f, 1.00f, 1.00f, 1.00f },
			{ 0.78f, 0.84f, 0.92f, 0.86f },
			{ 0.36f, 0.72f, 1.00f, 1.00f },
			{ 0.36f, 0.72f, 1.00f, 0.14f },
			{ 1.00f, 1.00f, 1.00f, 0.28f },
			{ 0.38f, 1.00f, 0.78f, 1.00f },
		},
		{
			"Dynamic Screenshots",
			true,
			ShellThemeMediaMode::DynamicScreenshot,
			{ 0.04f, 0.05f, 0.06f, 1.0f },
			{ 0.07f, 0.09f, 0.11f, 1.0f },
			{ 0.62f, 0.80f, 0.92f, 0.07f },
			{ 0.10f, 0.13f, 0.15f, 0.53f },
			{ 0.74f, 0.88f, 0.98f, 0.17f },
			{ 1.00f, 1.00f, 1.00f, 0.12f },
			{ 1.00f, 1.00f, 1.00f, 1.00f },
			{ 0.78f, 0.86f, 0.92f, 0.85f },
			{ 0.26f, 0.84f, 0.92f, 1.00f },
			{ 0.26f, 0.84f, 0.92f, 0.14f },
			{ 1.00f, 1.00f, 1.00f, 0.27f },
			{ 0.36f, 1.00f, 0.86f, 1.00f },
		},
		{
			"Dynamic Recalbox V1",
			true,
			ShellThemeMediaMode::DynamicMixRecalboxV1,
			{ 0.05f, 0.04f, 0.07f, 1.0f },
			{ 0.09f, 0.07f, 0.12f, 1.0f },
			{ 0.78f, 0.68f, 1.00f, 0.07f },
			{ 0.12f, 0.10f, 0.16f, 0.53f },
			{ 0.86f, 0.78f, 1.00f, 0.17f },
			{ 1.00f, 1.00f, 1.00f, 0.12f },
			{ 1.00f, 1.00f, 1.00f, 1.00f },
			{ 0.84f, 0.80f, 0.92f, 0.85f },
			{ 0.66f, 0.50f, 1.00f, 1.00f },
			{ 0.66f, 0.50f, 1.00f, 0.14f },
			{ 1.00f, 1.00f, 1.00f, 0.27f },
			{ 0.82f, 0.62f, 1.00f, 1.00f },
		},
		{
			"Dynamic Recalbox V2",
			true,
			ShellThemeMediaMode::DynamicMixRecalboxV2,
			{ 0.06f, 0.04f, 0.05f, 1.0f },
			{ 0.11f, 0.07f, 0.09f, 1.0f },
			{ 1.00f, 0.74f, 0.70f, 0.07f },
			{ 0.15f, 0.10f, 0.12f, 0.53f },
			{ 1.00f, 0.82f, 0.78f, 0.17f },
			{ 1.00f, 1.00f, 1.00f, 0.12f },
			{ 1.00f, 1.00f, 1.00f, 1.00f },
			{ 0.92f, 0.82f, 0.80f, 0.85f },
			{ 1.00f, 0.52f, 0.42f, 1.00f },
			{ 1.00f, 0.52f, 0.42f, 0.14f },
			{ 1.00f, 1.00f, 1.00f, 0.27f },
			{ 1.00f, 0.74f, 0.36f, 1.00f },
		},
	}};
	return themes;
}

ShellLayoutContext MakeShellLayout(const ImVec2& viewportPx, ImVec2 originPx) noexcept
{
	ShellLayoutContext context{};
	context.originPx = originPx;
	context.viewportPx = { std::max(1.0f, viewportPx.x), std::max(1.0f, viewportPx.y) };
	context.uiScale = std::min(context.viewportPx.x / 1280.0f, context.viewportPx.y / 720.0f);
	if (context.uiScale <= 0.0f || !std::isfinite(context.uiScale))
		context.uiScale = 1.0f;
	context.layoutUnitsW = context.viewportPx.x / context.uiScale;
	context.layoutUnitsH = context.viewportPx.y / context.uiScale;
	return context;
}

ImVec2 ToPx(const ShellLayoutContext& context, float x, float y) noexcept
{
	return { context.originPx.x + x * context.uiScale,
		context.originPx.y + y * context.uiScale };
}

ImVec2 ToPx(const ShellLayoutContext& context, ImVec2 point) noexcept
{
	return ToPx(context, point.x, point.y);
}

UnitRect UnitFromPx(const ShellLayoutContext& context, ImVec2 point) noexcept
{
	return { (point.x - context.originPx.x) / context.uiScale,
		(point.y - context.originPx.y) / context.uiScale, 0.0f, 0.0f };
}

ImVec2 UnitPointFromPx(const ShellLayoutContext& context, ImVec2 point) noexcept
{
	return { (point.x - context.originPx.x) / context.uiScale,
		(point.y - context.originPx.y) / context.uiScale };
}

ImVec2 RectMinPx(const ShellLayoutContext& context, const UnitRect& rect) noexcept
{
	return ToPx(context, rect.x, rect.y);
}

ImVec2 RectMaxPx(const ShellLayoutContext& context, const UnitRect& rect) noexcept
{
	return ToPx(context, rect.right(), rect.bottom());
}

ImU32 ColorWithAlpha(ImVec4 color, float alphaScale) noexcept
{
	color.w *= alphaScale;
	return ImGui::ColorConvertFloat4ToU32(color);
}

ImVec2 PixelFloor(ImVec2 point) noexcept
{
	return { std::floor(point.x), std::floor(point.y) };
}

ImVec2 PixelCeil(ImVec2 point) noexcept
{
	return { std::ceil(point.x), std::ceil(point.y) };
}

void NormalizeRoundedRectPx(ImVec2& min, ImVec2& max) noexcept
{
	min = PixelFloor(min);
	max = PixelCeil(max);
}

void AddRoundedRectStrokePx(ImDrawList* draw, ImVec2 min, ImVec2 max, ImU32 color,
	float rounding, float thickness, ImDrawFlags flags)
{
	if (!draw || max.x <= min.x || max.y <= min.y || thickness <= 0.0f)
		return;
	NormalizeRoundedRectPx(min, max);
	const float inset = thickness * 0.5f;
	min.x += inset;
	min.y += inset;
	max.x -= inset;
	max.y -= inset;
	if (max.x <= min.x || max.y <= min.y)
		return;
	draw->AddRect(min, max, color, std::max(0.0f, rounding - inset), flags, thickness);
}

void AddRoundedRectFilledPx(ImDrawList* draw, ImVec2 min, ImVec2 max, ImU32 color,
	float rounding, ImDrawFlags flags)
{
	if (!draw || max.x <= min.x || max.y <= min.y)
		return;
	NormalizeRoundedRectPx(min, max);
	draw->AddRectFilled(min, max, color, std::max(0.0f, rounding), flags);
}

void DrawShellPanelPx(ImDrawList* draw, ImVec2 min, ImVec2 max, const ShellTheme& theme,
	float radiusPx, float opacity)
{
	if (!draw || max.x <= min.x || max.y <= min.y)
		return;
	NormalizeRoundedRectPx(min, max);
	const float rounding = std::max(0.0f, radiusPx);
	const float stroke = std::max(1.0f, radiusPx / 18.0f);
	const ImU32 base = ColorWithAlpha(theme.panelBase, opacity);
	AddRoundedRectFilledPx(draw, { min.x - 1.0f, min.y - 1.0f },
		{ max.x + 1.0f, max.y + 1.0f }, base, rounding + 1.0f);
	AddRoundedRectFilledPx(draw, min, max, base, rounding);
	AddRoundedRectStrokePx(draw, min, max,
		ColorWithAlpha(theme.panelBorder, opacity), rounding, stroke);

	const float inset = std::min(2.0f,
		std::min(max.x - min.x, max.y - min.y) * 0.12f);
	const ImVec2 innerMin{ min.x + inset, min.y + inset };
	const ImVec2 innerMax{ max.x - inset, max.y - inset };
	if (innerMax.x <= innerMin.x || innerMax.y <= innerMin.y)
		return;

	const float innerRadius = std::max(0.0f, rounding - inset);
	AddRoundedRectFilledPx(draw, innerMin, innerMax,
		ColorWithAlpha(theme.panelHighlight, 0.055f * opacity), innerRadius);
	const float sheenHeight = std::max(0.0f, (innerMax.y - innerMin.y) * 0.34f);
	if (sheenHeight > 1.0f)
	{
		AddRoundedRectFilledPx(draw, innerMin, { innerMax.x, innerMin.y + sheenHeight },
			ColorWithAlpha(theme.panelHighlight, 0.060f * opacity), innerRadius,
			ImDrawFlags_RoundCornersTop);
	}
}

float Ease(ShellEasing easing, float t) noexcept
{
	t = std::clamp(t, 0.0f, 1.0f);
	switch (easing)
	{
	case ShellEasing::OutExpo:
		return t >= 1.0f ? 1.0f : 1.0f - std::pow(2.0f, -10.0f * t);
	case ShellEasing::OutBack:
	{
		constexpr float c1 = 1.70158f;
		constexpr float c3 = c1 + 1.0f;
		const float u = t - 1.0f;
		return 1.0f + c3 * u * u * u + c1 * u * u;
	}
	case ShellEasing::OutCubic:
	default:
		const float u = 1.0f - t;
		return 1.0f - u * u * u;
	}
}

void AnimatedFloat::SetImmediate(float newValue) noexcept
{
	value = from = target = newValue;
	duration = elapsed = 0.0f;
}

void AnimatedFloat::Set(float newValue, float seconds, ShellEasing newEasing) noexcept
{
	if (std::abs(target - newValue) < 0.01f && duration > 0.0f)
		return;
	from = value;
	target = newValue;
	duration = std::max(seconds, 0.001f);
	elapsed = 0.0f;
	easing = newEasing;
}

void AnimatedFloat::Update(float deltaSeconds) noexcept
{
	if (duration <= 0.0f)
		return;
	elapsed += deltaSeconds;
	const float t = std::clamp(elapsed / duration, 0.0f, 1.0f);
	value = from + (target - from) * Ease(easing, t);
	if (t >= 1.0f)
	{
		value = from = target;
		duration = 0.0f;
	}
}

float CursorAnimation::AdaptiveDuration(const UnitRect& target, float targetRadius,
	float baseDuration) const noexcept
{
	const float sourceWidth = std::max(1.0f, w.value);
	const float sourceHeight = std::max(1.0f, h.value);
	const float sourceCx = x.value + sourceWidth * 0.5f;
	const float sourceCy = y.value + sourceHeight * 0.5f;
	const float targetCx = target.x + target.w * 0.5f;
	const float targetCy = target.y + target.h * 0.5f;
	const float dx = targetCx - sourceCx;
	const float dy = targetCy - sourceCy;
	const float distance = std::sqrt(dx * dx + dy * dy);
	const float sourceDiag = std::sqrt(sourceWidth * sourceWidth + sourceHeight * sourceHeight);
	const float targetWidth = std::max(1.0f, target.w);
	const float targetHeight = std::max(1.0f, target.h);
	const float targetDiag = std::sqrt(targetWidth * targetWidth + targetHeight * targetHeight);
	const float sizeDelta = std::abs(targetDiag - sourceDiag) / std::max(1.0f, sourceDiag);
	const float sourceAspect = sourceWidth / sourceHeight;
	const float targetAspect = targetWidth / targetHeight;
	const float aspectDelta = std::abs(targetAspect - sourceAspect) /
		std::max(0.35f, sourceAspect);
	const float radiusDelta = std::abs(targetRadius - radius.value) /
		std::max(4.0f, std::max(std::abs(radius.value), std::abs(targetRadius)));
	const float distanceFactor = std::clamp(distance / 320.0f, 0.0f, 2.4f);
	const float deformationFactor = std::clamp(sizeDelta + aspectDelta + radiusDelta, 0.0f, 1.8f);
	return std::clamp(baseDuration *
		(1.0f + distanceFactor * 0.50f + deformationFactor * 0.35f),
		baseDuration * 0.95f, baseDuration * 2.8f);
}

void CursorAnimation::MoveTo(const UnitRect& target, float targetRadius,
	float baseDuration) noexcept
{
	if (!initialized)
	{
		x.SetImmediate(target.x);
		y.SetImmediate(target.y);
		w.SetImmediate(target.w);
		h.SetImmediate(target.h);
		radius.SetImmediate(targetRadius);
		initialized = true;
		return;
	}
	const float adaptiveDuration = AdaptiveDuration(target, targetRadius, baseDuration);
	x.Set(target.x, adaptiveDuration);
	y.Set(target.y, adaptiveDuration);
	w.Set(target.w, adaptiveDuration);
	h.Set(target.h, adaptiveDuration);
	radius.Set(targetRadius, adaptiveDuration);
}

void CursorAnimation::Update(float deltaSeconds) noexcept
{
	time += deltaSeconds;
	x.Update(deltaSeconds);
	y.Update(deltaSeconds);
	w.Update(deltaSeconds);
	h.Update(deltaSeconds);
	radius.Update(deltaSeconds);
}

void CursorAnimation::Draw(const ShellLayoutContext& context, ImDrawList* draw,
	const ShellTheme& theme) const
{
	DrawGlow(context, draw, theme);
	DrawBorder(context, draw, theme);
}

void CursorAnimation::DrawGlow(const ShellLayoutContext& context, ImDrawList* draw,
	const ShellTheme& theme) const
{
	if (!initialized || w.value < 1.0f || h.value < 1.0f)
		return;

	const UnitRect rect{ x.value, y.value, w.value, h.value };
	constexpr std::array<float, 5> bloomExpand{ 12.0f, 9.0f, 6.0f, 4.0f, 2.0f };
	constexpr std::array<float, 5> bloomAlpha{ 0.024f, 0.042f, 0.066f, 0.096f, 0.130f };
	for (std::size_t i = 0; i < bloomExpand.size(); ++i)
	{
		const float expand = bloomExpand[i];
		const UnitRect glow = rect.Expanded(expand);
		AddRoundedRectFilledPx(draw, RectMinPx(context, glow), RectMaxPx(context, glow),
			ColorWithAlpha(theme.cursorNormal, bloomAlpha[i]),
			(radius.value + expand + 2.0f) * context.uiScale);
	}
}

void CursorAnimation::DrawBorder(const ShellLayoutContext& context, ImDrawList* draw,
	const ShellTheme& theme) const
{
	if (!initialized || w.value < 1.0f || h.value < 1.0f)
		return;

	const UnitRect rect{ x.value, y.value, w.value, h.value };
	AddRoundedRectStrokePx(draw, RectMinPx(context, rect), RectMaxPx(context, rect),
		ImGui::ColorConvertFloat4ToU32(theme.cursorNormal), radius.value * context.uiScale,
		3.0f * context.uiScale);
}
}
