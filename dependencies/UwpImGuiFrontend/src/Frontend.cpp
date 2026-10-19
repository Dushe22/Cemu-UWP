#include "UwpImGuiFrontend/Frontend.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace UwpImGuiFrontend
{
FrontendConfiguration MakeDefaultFrontendConfiguration()
{
	FrontendConfiguration configuration;
	configuration.logo = "icons/app.png";
	configuration.placeholderArtwork = "icons/content.png";
	configuration.leftRail = {
		{ "home", "Home", "icons/home.png", HostCommand::OpenHome },
		{ "library", "Library", "icons/library.png", HostCommand::OpenTitleManager },
		{ "settings", "Settings", "icons/settings.png", HostCommand::OpenSettings },
	};
	configuration.rightRail = {
		{ "controllers", "Controllers", "icons/controllers.png", HostCommand::OpenControllers },
		{ "graphics", "Graphics", "icons/graphics.png", HostCommand::OpenGraphics },
		{ "power", "Power", "icons/power.png", HostCommand::ExitApplication },
	};
	configuration.actionHints = {
		{ "A", "Select" },
		{ "B", "Back" },
		{ "X", "Options" },
		{ "Y", "Theme" },
		{ "Menu", "Settings" },
		{ "View", "Library" },
	};
	return configuration;
}

Frontend::Frontend(IFrontendHost& host, IScreenScraperService& screenScraper,
	ICompanionService& companion, FrontendConfiguration configuration)
	: m_host(host), m_screenScraper(screenScraper), m_companion(companion),
	  m_configuration(std::move(configuration))
{
}

void Frontend::Initialize()
{
	if (m_initialized)
		return;
	m_initialized = true;
	m_companion.Initialize(m_host, m_configuration.applicationName,
		m_configuration.platformName);
	RefreshCatalogue();
}

void Frontend::Shutdown()
{
	if (!m_initialized)
		return;
	m_companion.Shutdown();
	m_screenScraper.RequestStop();
	m_catalogue.clear();
	m_notifications.Clear();
	m_pendingLaunch.reset();
	m_initialized = false;
}

void Frontend::RefreshCatalogue()
{
	m_catalogue = m_host.SnapshotCatalogue();
	m_shell.SetItemCount(m_catalogue.size());
	if (!m_catalogue.empty())
		m_companion.UpdateSelection(m_catalogue[m_shell.SelectedIndex()].id);
}

void Frontend::HandleInput(const FrameInput& input)
{
	if (m_pendingLaunch)
	{
		m_previousInput = input;
		return;
	}
	const NavigationState remoteNavigation = m_companion.GetNavigationState();
	constexpr float analogDeadzone = 0.55f;
	bool up = input.up || input.leftStickY >= analogDeadzone || remoteNavigation.up;
	bool down = input.down || input.leftStickY <= -analogDeadzone || remoteNavigation.down;
	bool left = input.left || input.leftStickX <= -analogDeadzone || remoteNavigation.left;
	bool right = input.right || input.leftStickX >= analogDeadzone || remoteNavigation.right;
	if (up && down)
		up = down = false;
	if (left && right)
		left = right = false;
	const float deltaSeconds = std::clamp(input.deltaSeconds, 0.0f, 0.1f);

	if (DirectionPressed(up, m_previousInput.up, m_upHeldSeconds, deltaSeconds))
		Move(Direction::Up);
	if (DirectionPressed(down, m_previousInput.down, m_downHeldSeconds, deltaSeconds))
		Move(Direction::Down);
	if (DirectionPressed(left, m_previousInput.left, m_leftHeldSeconds, deltaSeconds))
		Move(Direction::Left);
	if (DirectionPressed(right, m_previousInput.right, m_rightHeldSeconds, deltaSeconds))
		Move(Direction::Right);

	const auto pressed = [](bool current, bool previous) { return current && !previous; };
	if (pressed(input.accept, m_previousInput.accept))
		ActivateFocused();
	if (pressed(input.context, m_previousInput.context))
		OpenContextMenu();
	if (pressed(input.back, m_previousInput.back))
		m_host.Execute(HostCommand::OpenHome);
	if (pressed(input.menu, m_previousInput.menu))
		m_host.Execute(HostCommand::OpenSettings);
	if (pressed(input.view, m_previousInput.view))
		m_host.Execute(HostCommand::OpenTitleManager);
	if (pressed(input.alternate, m_previousInput.alternate))
		m_host.Execute(HostCommand::CycleTheme);
	if (pressed(input.leftShoulder, m_previousInput.leftShoulder) && m_shell.SelectedPage() > 0)
		Select(m_shell.SelectedIndex() - std::min(m_shell.SelectedIndex(), m_shell.ItemsPerPage()));
	if (pressed(input.rightShoulder, m_previousInput.rightShoulder) &&
		m_shell.SelectedPage() + 1 < m_shell.PageCount())
	{
		Select(std::min(m_shell.SelectedIndex() + m_shell.ItemsPerPage(),
			m_catalogue.size() - 1));
	}

	if (const auto remoteSelection = m_companion.TakePendingSelection())
	{
		const auto found = std::find_if(m_catalogue.begin(), m_catalogue.end(),
			[remoteSelection](const LibraryItem& item) { return item.id == *remoteSelection; });
		if (found != m_catalogue.end())
			Select(static_cast<std::size_t>(std::distance(m_catalogue.begin(), found)));
	}
	if (const auto remoteLaunch = m_companion.TakePendingLaunch())
	{
		const auto found = std::find_if(m_catalogue.begin(), m_catalogue.end(),
			[remoteLaunch](const LibraryItem& item) { return item.id == *remoteLaunch; });
		const bool launched = found != m_catalogue.end() && m_host.RequestLaunch(found->id);
		m_companion.CompletePendingLaunch(launched);
		if (!launched)
			PushNotification({ "Unable to launch the requested item", 4.0f });
	}
	m_companion.UpdateRunningContent(m_host.GetRunningContent());

	m_previousInput = input;
	m_previousInput.up = up;
	m_previousInput.down = down;
	m_previousInput.left = left;
	m_previousInput.right = right;
}

bool Frontend::DirectionPressed(bool current, bool previous, float& heldSeconds,
	float deltaSeconds) noexcept
{
	constexpr float initialDelay = 0.30f;
	constexpr float repeatInterval = 0.095f;
	if (!current)
	{
		heldSeconds = 0.0f;
		return false;
	}
	if (!previous)
	{
		heldSeconds = 0.0f;
		return true;
	}

	const float previousHeld = heldSeconds;
	heldSeconds += deltaSeconds;
	if (heldSeconds < initialDelay)
		return false;
	const int previousRepeat = previousHeld < initialDelay ? -1 :
		static_cast<int>(std::floor((previousHeld - initialDelay) / repeatInterval));
	const int currentRepeat = static_cast<int>(std::floor((heldSeconds - initialDelay) / repeatInterval));
	return currentRepeat > previousRepeat;
}

void Frontend::Move(Direction direction)
{
	switch (m_focusArea)
	{
	case ShellFocusArea::LeftRail:
		if (direction == Direction::Up && m_leftRailIndex > 0)
			--m_leftRailIndex;
		else if (direction == Direction::Down && m_leftRailIndex + 1 < m_configuration.leftRail.size())
			++m_leftRailIndex;
		else if (direction == Direction::Right)
			m_focusArea = ShellFocusArea::Catalogue;
		return;
	case ShellFocusArea::RightRail:
		if (direction == Direction::Up && m_rightRailIndex > 0)
			--m_rightRailIndex;
		else if (direction == Direction::Down && m_rightRailIndex + 1 < m_configuration.rightRail.size())
			++m_rightRailIndex;
		else if (direction == Direction::Left)
			m_focusArea = ShellFocusArea::Catalogue;
		return;
	case ShellFocusArea::Catalogue:
		break;
	}

	const std::size_t columns = m_shell.Columns();
	const std::size_t column = m_shell.SelectedIndex() % columns;
	if (direction == Direction::Left && column == 0 && !m_configuration.leftRail.empty())
	{
		m_focusArea = ShellFocusArea::LeftRail;
		return;
	}
	if (direction == Direction::Right &&
		(column + 1 >= columns || m_shell.SelectedIndex() + 1 >= std::max<std::size_t>(m_catalogue.size(), 1)) &&
		!m_configuration.rightRail.empty())
	{
		m_focusArea = ShellFocusArea::RightRail;
		return;
	}
	m_shell.Move(direction);
	if (!m_catalogue.empty())
		m_companion.UpdateSelection(m_catalogue[m_shell.SelectedIndex()].id);
}

void Frontend::ActivateFocused()
{
	switch (m_focusArea)
	{
	case ShellFocusArea::LeftRail:
		ActivateAction(ShellFocusArea::LeftRail, m_leftRailIndex);
		break;
	case ShellFocusArea::RightRail:
		ActivateAction(ShellFocusArea::RightRail, m_rightRailIndex);
		break;
	case ShellFocusArea::Catalogue:
		if (m_catalogue.empty())
			m_host.Execute(HostCommand::AddContent);
		else
			ActivateSelection();
		break;
	}
}

void Frontend::ActivateSelection()
{
	if (m_catalogue.empty() || m_shell.SelectedIndex() >= m_catalogue.size())
		return;
	const auto& item = m_catalogue[m_shell.SelectedIndex()];
	if (!m_companion.TryBeginLocalLaunch(item.id))
	{
		PushNotification({ "A remote launch is already pending", 4.0f });
		return;
	}
	if (m_configuration.animateLaunches)
	{
		m_pendingLaunch = item.id;
		return;
	}
	if (!m_host.RequestLaunch(item.id))
	{
		m_companion.CancelLocalLaunch(item.id);
		PushNotification({ "Unable to launch " + item.name, 4.0f });
	}
}

bool Frontend::CommitPendingLaunch()
{
	if (!m_pendingLaunch)
		return false;
	const ItemId itemId = *m_pendingLaunch;
	m_pendingLaunch.reset();
	const auto found = std::find_if(m_catalogue.begin(), m_catalogue.end(),
		[itemId](const LibraryItem& item) { return item.id == itemId; });
	if (found == m_catalogue.end())
	{
		m_companion.CancelLocalLaunch(itemId);
		PushNotification({ "The selected item is no longer available", 4.0f });
		return false;
	}
	if (m_host.RequestLaunch(itemId))
		return true;
	m_companion.CancelLocalLaunch(itemId);
	PushNotification({ "Unable to launch " + found->name, 4.0f });
	return false;
}

void Frontend::OpenContextMenu()
{
	if (m_focusArea != ShellFocusArea::Catalogue || m_catalogue.empty() ||
		m_shell.SelectedIndex() >= m_catalogue.size())
		return;
	m_host.OpenContextMenu(m_catalogue[m_shell.SelectedIndex()].id);
}

void Frontend::Select(std::size_t index) noexcept
{
	m_focusArea = ShellFocusArea::Catalogue;
	m_shell.Select(index);
	if (!m_catalogue.empty() && m_shell.SelectedIndex() < m_catalogue.size())
		m_companion.UpdateSelection(m_catalogue[m_shell.SelectedIndex()].id);
}

void Frontend::ActivateAction(ShellFocusArea area, std::size_t index)
{
	const std::vector<ShellAction>* actions = nullptr;
	if (area == ShellFocusArea::LeftRail)
		actions = &m_configuration.leftRail;
	else if (area == ShellFocusArea::RightRail)
		actions = &m_configuration.rightRail;
	if (!actions || index >= actions->size())
		return;
	m_host.Execute((*actions)[index].command);
}

void Frontend::SetFocus(ShellFocusArea area, std::size_t index) noexcept
{
	m_focusArea = area;
	if (area == ShellFocusArea::LeftRail)
		m_leftRailIndex = m_configuration.leftRail.empty() ? 0 :
			std::min(index, m_configuration.leftRail.size() - 1);
	else if (area == ShellFocusArea::RightRail)
		m_rightRailIndex = m_configuration.rightRail.empty() ? 0 :
			std::min(index, m_configuration.rightRail.size() - 1);
}

void Frontend::PushNotification(Notification notification)
{
	if (!notification.message.empty())
		m_notifications.Push(std::move(notification));
}
}
