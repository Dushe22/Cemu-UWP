#pragma once

#include "Notifications.h"
#include "ShellState.h"

#include <filesystem>
#include <string>
#include <vector>

namespace UwpImGuiFrontend
{
enum class ShellFocusArea : std::uint8_t
{
	Catalogue,
	LeftRail,
	RightRail,
};

struct ShellAction
{
	std::string id;
	std::string label;
	std::filesystem::path icon;
	HostCommand command = HostCommand::OpenHome;
};

struct ActionHint
{
	std::string button;
	std::string label;
};

struct FrontendConfiguration
{
	std::string applicationName = "Application";
	std::string platformName = "Library";
	std::string emptyCatalogueLabel = "Add games folder";
	std::filesystem::path logo;
	std::filesystem::path placeholderArtwork;
	std::vector<ShellAction> leftRail;
	std::vector<ShellAction> rightRail;
	std::vector<ActionHint> actionHints;
	bool animateLaunches = true;
};

[[nodiscard]] FrontendConfiguration MakeDefaultFrontendConfiguration();

class Frontend
{
public:
	Frontend(IFrontendHost& host, IScreenScraperService& screenScraper, ICompanionService& companion,
		FrontendConfiguration configuration = MakeDefaultFrontendConfiguration());

	void Initialize();
	void Shutdown();
	void RefreshCatalogue();
	void HandleInput(const FrameInput& input);
	void ActivateSelection();
	void OpenContextMenu();
	void Select(std::size_t index) noexcept;
	void ActivateAction(ShellFocusArea area, std::size_t index);
	void PushNotification(Notification notification);
	[[nodiscard]] std::optional<ItemId> PendingLaunchItem() const noexcept
	{
		return m_pendingLaunch;
	}
	[[nodiscard]] bool CommitPendingLaunch();

	[[nodiscard]] const std::vector<LibraryItem>& Catalogue() const noexcept { return m_catalogue; }
	[[nodiscard]] std::vector<Notification> Notifications() { return m_notifications.Snapshot(); }
	[[nodiscard]] ShellState& State() noexcept { return m_shell; }
	[[nodiscard]] const ShellState& State() const noexcept { return m_shell; }
	[[nodiscard]] IFrontendHost& Host() noexcept { return m_host; }
	[[nodiscard]] const FrontendConfiguration& Configuration() const noexcept { return m_configuration; }
	void SetAnimateLaunches(bool enabled) noexcept { m_configuration.animateLaunches = enabled; }
	[[nodiscard]] bool AnimateLaunches() const noexcept { return m_configuration.animateLaunches; }
	[[nodiscard]] ShellFocusArea FocusArea() const noexcept { return m_focusArea; }
	[[nodiscard]] std::size_t LeftRailIndex() const noexcept { return m_leftRailIndex; }
	[[nodiscard]] std::size_t RightRailIndex() const noexcept { return m_rightRailIndex; }
	void SetFocus(ShellFocusArea area, std::size_t index = 0) noexcept;

private:
	void Move(Direction direction);
	void ActivateFocused();
	[[nodiscard]] bool DirectionPressed(bool current, bool previous, float& heldSeconds,
		float deltaSeconds) noexcept;

	IFrontendHost& m_host;
	IScreenScraperService& m_screenScraper;
	ICompanionService& m_companion;
	FrontendConfiguration m_configuration;
	std::vector<LibraryItem> m_catalogue;
	NotificationCenter m_notifications;
	ShellState m_shell;
	FrameInput m_previousInput{};
	ShellFocusArea m_focusArea = ShellFocusArea::Catalogue;
	std::size_t m_leftRailIndex = 0;
	std::size_t m_rightRailIndex = 0;
	float m_upHeldSeconds = 0.0f;
	float m_downHeldSeconds = 0.0f;
	float m_leftHeldSeconds = 0.0f;
	float m_rightHeldSeconds = 0.0f;
	std::optional<ItemId> m_pendingLaunch;
	bool m_initialized = false;
};
}
