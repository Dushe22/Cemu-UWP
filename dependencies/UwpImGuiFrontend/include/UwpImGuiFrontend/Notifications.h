#pragma once

#include "ImGuiShell.h"
#include "Types.h"

#include <chrono>
#include <cstddef>
#include <deque>
#include <mutex>
#include <span>
#include <string>
#include <vector>

namespace UwpImGuiFrontend
{
class NotificationCenter
{
public:
	explicit NotificationCenter(std::size_t capacity = 6) noexcept;

	void Push(Notification notification);
	void Push(std::string message, float durationSeconds = 5.0f);
	[[nodiscard]] std::vector<Notification> Snapshot();
	void Clear();

private:
	struct TimedNotification
	{
		Notification notification;
		std::chrono::steady_clock::time_point expiresAt;
	};

	void TrimLocked(std::chrono::steady_clock::time_point now);

	std::mutex m_mutex;
	std::deque<TimedNotification> m_notifications;
	std::size_t m_capacity = 6;
};

struct NotificationDrawOptions
{
	float rightMarginUnits = 30.0f;
	float topUnits = 92.0f;
	float widthUnits = 300.0f;
	float heightUnits = 30.0f;
	float gapUnits = 6.0f;
	float fontSizeUnits = 13.0f;
	float panelOpacity = 0.78f;
};

void DrawNotificationCards(ImDrawList* draw, const ShellLayoutContext& context,
	const ShellTheme& theme, std::span<const Notification> notifications,
	ImFont* font = nullptr, const NotificationDrawOptions& options = {});
}
