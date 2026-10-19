#include "UwpImGuiFrontend/ShellState.h"

#include <algorithm>
#include <cmath>

namespace UwpImGuiFrontend
{
LayoutContext MakeLayoutContext(Vec2 viewportPixels) noexcept
{
	LayoutContext context;
	context.viewportPixels.x = std::max(1.0f, viewportPixels.x);
	context.viewportPixels.y = std::max(1.0f, viewportPixels.y);
	context.scale = std::min(context.viewportPixels.x / 1280.0f, context.viewportPixels.y / 720.0f);
	if (!std::isfinite(context.scale) || context.scale <= 0.0f)
		context.scale = 1.0f;
	context.widthUnits = context.viewportPixels.x / context.scale;
	context.heightUnits = context.viewportPixels.y / context.scale;
	return context;
}

GridLayout MakeGridLayout(const LayoutContext& context, std::size_t itemCount) noexcept
{
	GridLayout layout;
	constexpr float topHudHeight = 90.0f;
	constexpr float bottomBandHeight = 90.0f;
	constexpr float minimumTileSize = 88.0f;
	constexpr float baseTileSize = 150.0f;

	const float sideMargin = std::clamp(context.widthUnits * 0.171875f, 112.0f, 220.0f);
	layout.band = { sideMargin, topHudHeight, std::max(1.0f, context.widthUnits - sideMargin * 2.0f),
		std::max(1.0f, context.heightUnits - topHudHeight - bottomBandHeight) };

	const float requiredWidth = layout.columns * baseTileSize + (layout.columns - 1) * layout.horizontalGap;
	const float requiredHeight = layout.rows * baseTileSize + (layout.rows - 1) * layout.verticalGap;
	const float safeHeight = std::max(1.0f, layout.band.height - 40.0f);
	const float fit = std::min(layout.band.width / requiredWidth, safeHeight / requiredHeight);
	if (fit < 1.0f)
	{
		layout.tileSize = std::max(minimumTileSize, baseTileSize * fit);
		layout.horizontalGap = std::max(8.0f, layout.horizontalGap * fit);
		layout.verticalGap = std::max(8.0f, layout.verticalGap * fit);
	}

	layout.itemsPerPage = layout.columns * layout.rows;
	const std::size_t displayedItems = std::max<std::size_t>(itemCount, 1);
	layout.pageCount = std::max<std::size_t>(1, (displayedItems + layout.itemsPerPage - 1) / layout.itemsPerPage);
	return layout;
}

void ShellState::SetItemCount(std::size_t itemCount) noexcept
{
	m_itemCount = itemCount;
	if (m_itemCount == 0)
		m_selectedIndex = 0;
	else
		m_selectedIndex = std::min(m_selectedIndex, m_itemCount - 1);
}

void ShellState::SetGridDimensions(std::size_t columns, std::size_t rows) noexcept
{
	m_columns = std::max<std::size_t>(columns, 1);
	m_rows = std::max<std::size_t>(rows, 1);
}

void ShellState::Move(Direction direction) noexcept
{
	if (m_itemCount == 0)
		return;

	const std::size_t pageSize = ItemsPerPage();
	const std::size_t pageStart = (m_selectedIndex / pageSize) * pageSize;
	const std::size_t localIndex = m_selectedIndex - pageStart;
	const std::size_t row = localIndex / m_columns;
	const std::size_t column = localIndex % m_columns;

	switch (direction)
	{
	case Direction::Left:
		if (column > 0)
			--m_selectedIndex;
		else if (pageStart > 0)
			m_selectedIndex = pageStart - 1;
		break;
	case Direction::Right:
		if (m_selectedIndex + 1 < m_itemCount)
			++m_selectedIndex;
		break;
	case Direction::Up:
		if (row > 0)
			m_selectedIndex -= m_columns;
		else if (pageStart >= pageSize)
			m_selectedIndex = std::min(pageStart - pageSize + column, m_itemCount - 1);
		break;
	case Direction::Down:
		if (m_selectedIndex + m_columns < m_itemCount)
			m_selectedIndex += m_columns;
		else
			m_selectedIndex = std::min(pageStart + (m_rows - 1) * m_columns + column, m_itemCount - 1);
		break;
	}
}

void ShellState::Select(std::size_t index) noexcept
{
	if (m_itemCount == 0)
		m_selectedIndex = 0;
	else
		m_selectedIndex = std::min(index, m_itemCount - 1);
}

std::size_t ShellState::SelectedPage() const noexcept
{
	return m_selectedIndex / ItemsPerPage();
}

std::size_t ShellState::PageCount() const noexcept
{
	return std::max<std::size_t>(1, (std::max<std::size_t>(m_itemCount, 1) + ItemsPerPage() - 1) / ItemsPerPage());
}
}
