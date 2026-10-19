#pragma once

#include "Views.h"

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace UwpImGuiFrontend
{
struct GraphicsViewPage
{
	std::string id;
	std::string label;
	bool enabled = true;
	std::function<void()> draw;
};

struct GraphicsViewModel
{
	std::vector<GraphicsViewPage> pages;
	std::string emptyLabel = "No graphics options are provided by this application.";
	float contentControllerScroll = 0.0f;
};

struct GraphicsViewResult
{
	bool closeRequested = false;
};

class GraphicsView
{
public:
	[[nodiscard]] GraphicsViewResult Draw(const GraphicsViewModel& model,
		ImVec2 size = ImVec2(0.0f, 0.0f),
		const SplitViewOptions& options = {});
	void SelectPage(std::string_view id, const GraphicsViewModel& model);

	[[nodiscard]] SplitViewState& State() noexcept { return m_state; }
	[[nodiscard]] const SplitViewState& State() const noexcept { return m_state; }
	[[nodiscard]] std::string_view SelectedPageId() const noexcept
	{
		return m_selectedPageId;
	}

private:
	SplitViewState m_state;
	std::string m_selectedPageId;
};
}
