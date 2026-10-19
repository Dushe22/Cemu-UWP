#include "UwpImGuiFrontend/GraphicsView.h"

#include "UwpImGuiFrontend/Widgets.h"

#include <imgui.h>

#include <algorithm>

namespace UwpImGuiFrontend
{
GraphicsViewResult GraphicsView::Draw(const GraphicsViewModel& model,
	ImVec2 size, const SplitViewOptions& requestedOptions)
{
	GraphicsViewResult result;
	std::vector<SplitViewTab> tabs;
	std::vector<const GraphicsViewPage*> pages;
	tabs.reserve(std::max<std::size_t>(1, model.pages.size()));
	pages.reserve(model.pages.size());
	for (const GraphicsViewPage& page : model.pages)
	{
		if (!page.id.empty())
		{
			tabs.push_back({ page.id, page.label, page.enabled });
			pages.push_back(&page);
		}
	}
	const bool empty = tabs.empty();
	if (empty)
		tabs.push_back({ "graphics", "Graphics", true });

	if (!m_selectedPageId.empty())
	{
		const auto selected = std::ranges::find_if(tabs, [this](const auto& tab) {
			return tab.id == m_selectedPageId && tab.enabled;
		});
		if (selected != tabs.end())
			m_state.Select(static_cast<std::size_t>(selected - tabs.begin()), tabs.size());
	}
	m_state.SetItemCount(tabs.size());
	if (!tabs[m_state.selected].enabled)
	{
		const auto enabled = std::ranges::find_if(tabs,
			[](const auto& tab) { return tab.enabled; });
		if (enabled != tabs.end())
			m_state.Select(static_cast<std::size_t>(enabled - tabs.begin()), tabs.size());
	}
	m_selectedPageId = tabs[m_state.selected].id;

	if (ConsumeWidgetBackPressed())
	{
		if (m_state.focusedPane == SplitPane::Content)
			m_state.RequestFocus(SplitPane::Navigation);
		else
			result.closeRequested = true;
	}

	SplitViewOptions options = requestedOptions;
	options.contentControllerScroll = model.contentControllerScroll;
	DrawSplitView("reusable-graphics", m_state, tabs,
		[&](std::size_t index) {
			if (empty)
			{
				ImGui::TextWrapped("%s", model.emptyLabel.c_str());
				return;
			}
			if (index < pages.size() && pages[index]->draw)
				pages[index]->draw();
		}, size, options);
	return result;
}

void GraphicsView::SelectPage(std::string_view id,
	const GraphicsViewModel& model)
{
	const auto selected = std::ranges::find_if(model.pages,
		[id](const GraphicsViewPage& page) {
			return page.id == id && page.enabled;
		});
	if (selected == model.pages.end())
		return;
	m_state.Select(static_cast<std::size_t>(selected - model.pages.begin()),
		model.pages.size());
	m_state.RequestFocus(SplitPane::Navigation);
	m_selectedPageId.assign(id);
}
}
