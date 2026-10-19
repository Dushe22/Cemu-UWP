#pragma once

#include "Host.h"

#include <cstddef>

namespace UwpImGuiFrontend
{
struct LayoutContext
{
	Vec2 viewportPixels{ 1280.0f, 720.0f };
	float scale = 1.0f;
	float widthUnits = 1280.0f;
	float heightUnits = 720.0f;
};

struct GridLayout
{
	Rect band;
	float tileSize = 150.0f;
	float horizontalGap = 20.0f;
	float verticalGap = 16.0f;
	std::size_t columns = 5;
	std::size_t rows = 3;
	std::size_t itemsPerPage = 15;
	std::size_t pageCount = 1;
};

[[nodiscard]] LayoutContext MakeLayoutContext(Vec2 viewportPixels) noexcept;
[[nodiscard]] GridLayout MakeGridLayout(const LayoutContext& context, std::size_t itemCount) noexcept;

class ShellState
{
public:
	void SetItemCount(std::size_t itemCount) noexcept;
	void SetGridDimensions(std::size_t columns, std::size_t rows) noexcept;
	void Move(Direction direction) noexcept;
	void Select(std::size_t index) noexcept;

	[[nodiscard]] std::size_t ItemCount() const noexcept { return m_itemCount; }
	[[nodiscard]] std::size_t SelectedIndex() const noexcept { return m_selectedIndex; }
	[[nodiscard]] std::size_t SelectedPage() const noexcept;
	[[nodiscard]] std::size_t PageCount() const noexcept;
	[[nodiscard]] std::size_t ItemsPerPage() const noexcept { return m_columns * m_rows; }
	[[nodiscard]] std::size_t Columns() const noexcept { return m_columns; }
	[[nodiscard]] std::size_t Rows() const noexcept { return m_rows; }

private:
	std::size_t m_itemCount = 0;
	std::size_t m_selectedIndex = 0;
	std::size_t m_columns = 5;
	std::size_t m_rows = 3;
};
}
