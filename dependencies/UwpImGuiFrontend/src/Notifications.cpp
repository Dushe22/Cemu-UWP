#include "UwpImGuiFrontend/Notifications.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <utility>

namespace UwpImGuiFrontend
{
namespace
{
void EraseLastUtf8CodePoint(std::string& text)
{
	if (text.empty())
		return;
	std::size_t start = text.size() - 1;
	while (start > 0 && (static_cast<unsigned char>(text[start]) & 0xc0) == 0x80)
		--start;
	text.erase(start);
}

ImVec2 MeasureText(ImFont* font, float fontSizePx, std::string_view text)
{
	if (font)
		return font->CalcTextSizeA(fontSizePx, FLT_MAX, 0.0f,
			text.data(), text.data() + text.size());
	return ImGui::CalcTextSize(text.data(), text.data() + text.size());
}

std::string Ellipsize(ImFont* font, float fontSizePx, std::string text, float maxWidthPx)
{
	if (MeasureText(font, fontSizePx, text).x <= maxWidthPx)
		return text;
	constexpr std::string_view ellipsis = "...";
	while (!text.empty())
	{
		EraseLastUtf8CodePoint(text);
		std::string candidate = text;
		candidate += ellipsis;
		if (MeasureText(font, fontSizePx, candidate).x <= maxWidthPx)
			return candidate;
	}
	return std::string(ellipsis);
}
}

NotificationCenter::NotificationCenter(std::size_t capacity) noexcept
	: m_capacity(std::max<std::size_t>(1, capacity))
{
}

void NotificationCenter::Push(Notification notification)
{
	if (notification.message.empty())
		return;
	const auto now = std::chrono::steady_clock::now();
	const float duration = std::max(0.1f, notification.durationSeconds);
	std::lock_guard lock(m_mutex);
	TrimLocked(now);
	m_notifications.push_front({
		.notification = std::move(notification),
		.expiresAt = now + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
			std::chrono::duration<float>(duration)),
	});
	while (m_notifications.size() > m_capacity)
		m_notifications.pop_back();
}

void NotificationCenter::Push(std::string message, float durationSeconds)
{
	Push(Notification{ std::move(message), durationSeconds });
}

std::vector<Notification> NotificationCenter::Snapshot()
{
	std::lock_guard lock(m_mutex);
	TrimLocked(std::chrono::steady_clock::now());
	std::vector<Notification> result;
	result.reserve(m_notifications.size());
	for (const auto& entry : m_notifications)
		result.push_back(entry.notification);
	return result;
}

void NotificationCenter::Clear()
{
	std::lock_guard lock(m_mutex);
	m_notifications.clear();
}

void NotificationCenter::TrimLocked(std::chrono::steady_clock::time_point now)
{
	std::erase_if(m_notifications,
		[now](const TimedNotification& entry) { return entry.expiresAt <= now; });
}

void DrawNotificationCards(ImDrawList* draw, const ShellLayoutContext& context,
	const ShellTheme& theme, std::span<const Notification> notifications,
	ImFont* font, const NotificationDrawOptions& options)
{
	if (!draw || notifications.empty())
		return;
	float y = options.topUnits;
	const float fontSizePx = options.fontSizeUnits * context.uiScale;
	for (const auto& notification : notifications)
	{
		const UnitRect rect{
			context.layoutUnitsW - options.rightMarginUnits - options.widthUnits,
			y,
			options.widthUnits,
			options.heightUnits,
		};
		DrawShellPanelPx(draw, RectMinPx(context, rect), RectMaxPx(context, rect),
			theme, options.heightUnits * 0.5f * context.uiScale,
			options.panelOpacity);
		const float availableWidthPx = std::max(1.0f,
			(rect.w - 26.0f) * context.uiScale);
		const std::string fitted = Ellipsize(font, fontSizePx,
			notification.message, availableWidthPx);
		const ImVec2 position = ToPx(context, rect.x + 13.0f, rect.y + 7.0f);
		if (font)
			draw->AddText(font, fontSizePx, position,
				ImGui::ColorConvertFloat4ToU32(theme.textPrimary), fitted.c_str());
		else
			draw->AddText(position, ImGui::ColorConvertFloat4ToU32(theme.textPrimary),
				fitted.c_str());
		y += options.heightUnits + options.gapUnits;
	}
}
}
