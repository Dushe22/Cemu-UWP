#include "UwpImGuiFrontend/ShellRenderer.h"
#include "UwpImGuiFrontend/Text.h"
#include "UwpImGuiFrontend/Widgets.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <ctime>
#include <map>
#include <random>
#include <string_view>
#include <utility>

namespace UwpImGuiFrontend
{
namespace
{
ImU32 Color(ImVec4 color)
{
	return ImGui::ColorConvertFloat4ToU32(color);
}

#if IMGUI_VERSION_NUM >= 19200
ImTextureRef TextureReference(std::uintptr_t id)
{
	return ImTextureRef(static_cast<ImTextureID>(id));
}
#else
ImTextureID TextureReference(std::uintptr_t id)
{
	return reinterpret_cast<ImTextureID>(id);
}
#endif

ImVec2 TextSizeUnits(const ShellLayoutContext& context, float sizeUnits,
	std::string_view text)
{
	if (text.empty())
		return {};
	const ImVec2 pixels = ImGui::GetFont()->CalcTextSizeA(sizeUnits * context.uiScale,
		FLT_MAX, 0.0f, text.data(), text.data() + text.size());
	return { pixels.x / context.uiScale, pixels.y / context.uiScale };
}

void DrawText(ImDrawList* draw, const ShellLayoutContext& context, float sizeUnits,
	ImVec2 positionUnits, ImU32 color, std::string_view text)
{
	if (!draw || text.empty())
		return;
	draw->AddText(ImGui::GetFont(), sizeUnits * context.uiScale,
		ToPx(context, positionUnits), color, text.data(), text.data() + text.size());
}

std::string Ellipsize(const ShellLayoutContext& context, float sizeUnits,
	std::string text, float maxWidth)
{
	if (TextSizeUnits(context, sizeUnits, text).x <= maxWidth)
		return text;
	constexpr std::string_view suffix = "...";
	while (!text.empty() && TextSizeUnits(context, sizeUnits, text + std::string(suffix)).x > maxWidth)
	{
		std::size_t eraseAt = text.size() - 1;
		while (eraseAt > 0 && (static_cast<unsigned char>(text[eraseAt]) & 0xc0u) == 0x80u)
			--eraseAt;
		text.erase(eraseAt);
	}
	return text.empty() ? std::string(suffix) : text + std::string(suffix);
}

void DrawTexture(ImDrawList* draw, const ShellLayoutContext& context,
	TextureHandle texture, const UnitRect& rect, float radius, float alpha = 1.0f)
{
	if (!draw || !texture)
		return;
	ImVec2 min = RectMinPx(context, rect);
	ImVec2 max = RectMaxPx(context, rect);
	NormalizeRoundedRectPx(min, max);
	draw->AddImageRounded(TextureReference(texture.id), min, max,
		{ 0.0f, 0.0f }, { 1.0f, 1.0f },
		IM_COL32(255, 255, 255, static_cast<int>(std::clamp(alpha * 255.0f, 0.0f, 255.0f))),
		radius * context.uiScale, ImDrawFlags_RoundCornersAll);
}

void DrawTextureContained(ImDrawList* draw, const ShellLayoutContext& context,
	TextureHandle texture, const UnitRect& bounds, float radius, float alpha = 1.0f)
{
	if (!draw || !texture || texture.width == 0 || texture.height == 0)
		return;
	const float sourceAspect = static_cast<float>(texture.width) /
		static_cast<float>(texture.height);
	UnitRect destination = bounds;
	destination.w = std::min(bounds.w, bounds.h * sourceAspect);
	destination.h = std::min(bounds.h, destination.w / sourceAspect);
	if (destination.h < bounds.h && destination.w < bounds.w)
	{
		destination.h = bounds.h;
		destination.w = std::min(bounds.w, destination.h * sourceAspect);
	}
	destination.x = bounds.x + (bounds.w - destination.w) * 0.5f;
	destination.y = bounds.y + (bounds.h - destination.h) * 0.5f;
	DrawTexture(draw, context, texture, destination, radius, alpha);
}

void DrawTextureCover(ImDrawList* draw, TextureHandle texture, ImVec2 min, ImVec2 max,
	float alpha = 1.0f)
{
	if (!draw || !texture || max.x <= min.x || max.y <= min.y)
		return;
	const float destinationAspect = (max.x - min.x) / std::max(1.0f, max.y - min.y);
	const float sourceAspect = static_cast<float>(texture.width) /
		std::max(1.0f, static_cast<float>(texture.height));
	ImVec2 uv0{ 0.0f, 0.0f };
	ImVec2 uv1{ 1.0f, 1.0f };
	if (sourceAspect > destinationAspect)
	{
		const float crop = 1.0f - destinationAspect / sourceAspect;
		uv0.x = crop * 0.5f;
		uv1.x = 1.0f - crop * 0.5f;
	}
	else if (sourceAspect < destinationAspect)
	{
		const float crop = 1.0f - sourceAspect / destinationAspect;
		uv0.y = crop * 0.5f;
		uv1.y = 1.0f - crop * 0.5f;
	}
	draw->AddImage(TextureReference(texture.id), min, max, uv0, uv1,
		IM_COL32(255, 255, 255, static_cast<int>(std::clamp(alpha * 255.0f, 0.0f, 255.0f))));
}

std::pair<std::string, std::string> LocalClockText()
{
	const std::time_t now = std::time(nullptr);
	std::tm local{};
	localtime_s(&local, &now);
	std::array<char, 32> time{};
	std::array<char, 32> date{};
	std::strftime(time.data(), time.size(), "%H:%M", &local);
	std::strftime(date.data(), date.size(), "%d/%m/%Y", &local);
	return { time.data(), date.data() };
}

struct FloatingShape
{
	ImVec2 position{};
	float size = 20.0f;
	float speed = 10.0f;
	float phase = 0.0f;
	float wobble = 12.0f;
	float rotation = 0.0f;
	float rotationSpeed = 0.0f;
	int type = 0;
	float alpha = 0.08f;
};

const MediaAsset* FindPlatformMedia(
	const std::vector<MediaAsset>& media, MediaKind kind) noexcept
{
	const auto found = std::find_if(media.begin(), media.end(),
		[kind](const MediaAsset& asset) { return asset.kind == kind; });
	return found == media.end() ? nullptr : &*found;
}
}

class ShellRenderer::Impl
{
public:
	explicit Impl(IFrontendHost& host) : m_host(host) {}

	~Impl()
	{
		ClearTextures();
	}

	void ClearTextures()
	{
		m_host.StopVideoPlayback();
		m_launch = {};
		for (const auto& [path, texture] : m_textures)
		{
			if (texture)
				m_host.ReleaseTexture(texture);
		}
		m_textures.clear();
	}

	TextureHandle Texture(const std::filesystem::path& path, bool applicationAsset = false)
	{
		if (path.empty())
			return {};
		const std::filesystem::path resolved = applicationAsset && path.is_relative() ?
			m_host.ResourceRoot() / path : path;
		const auto existing = m_textures.find(resolved);
		if (existing != m_textures.end())
			return existing->second;
		return m_textures.emplace(resolved, m_host.LoadTexture(resolved)).first->second;
	}

	TextureHandle PreferredArtwork(const LibraryItem& item,
		const ShellPresentation& presentation)
	{
		if (const MediaAsset* media = FindMedia(item, presentation.listArtwork))
			return Texture(media->path);
		return {};
	}

	void DrawBackground(ImDrawList* draw, const ShellLayoutContext& context,
		Frontend& frontend, const ShellPresentation& presentation, const ShellTheme& theme,
		float deltaSeconds)
	{
		TextureHandle media;
		const LibraryItem* selected = nullptr;
		if (presentation.backgroundItem)
		{
			const auto found = std::find_if(frontend.Catalogue().begin(),
				frontend.Catalogue().end(), [&presentation](const LibraryItem& item) {
					return item.id == *presentation.backgroundItem;
				});
			if (found != frontend.Catalogue().end())
				selected = &*found;
		}
		else if (!frontend.Catalogue().empty() &&
			frontend.State().SelectedIndex() < frontend.Catalogue().size())
		{
			selected = &frontend.Catalogue()[frontend.State().SelectedIndex()];
		}

		const bool useThemeDynamicBackground =
			!presentation.overrideThemeDynamicBackground;
		const bool videoBackground = presentation.videoBackground ||
			(useThemeDynamicBackground && !presentation.backgroundArtwork &&
				theme.mediaMode == ShellThemeMediaMode::DynamicVideo);
		const std::optional<MediaKind> backgroundArtwork = presentation.backgroundArtwork ?
			presentation.backgroundArtwork :
			(useThemeDynamicBackground &&
				theme.mediaMode == ShellThemeMediaMode::DynamicBackground ?
				std::optional<MediaKind>(MediaKind::FanArt) : std::nullopt);
		if (videoBackground)
		{
			const MediaAsset* video = selected ? FindMedia(*selected, MediaKind::Video) : nullptr;
			if (!video && presentation.allowPlatformBackgroundFallback)
				video = FindPlatformMedia(presentation.platformMedia, MediaKind::Video);
			if (video)
				media = m_host.AcquireVideoFrame(video->path);
			if (!video)
				m_host.StopVideoPlayback();
		}
		else
		{
			m_host.StopVideoPlayback();
		}

		if (!media && backgroundArtwork)
		{
			if (selected)
			{
				if (const MediaAsset* artwork = FindMedia(*selected, *backgroundArtwork))
					media = Texture(artwork->path);
			}
			if (!media && presentation.allowPlatformBackgroundFallback)
			{
				if (const MediaAsset* artwork = FindPlatformMedia(
					presentation.platformMedia, *backgroundArtwork))
				{
					media = Texture(artwork->path);
				}
			}
		}

		const ImVec2 viewportMin = context.originPx;
		const ImVec2 viewportMax{ context.originPx.x + context.viewportPx.x,
			context.originPx.y + context.viewportPx.y };
		if (media)
		{
			DrawTextureCover(draw, media, viewportMin, viewportMax);
			draw->AddRectFilled(viewportMin, viewportMax,
				IM_COL32(0, 0, 0, videoBackground ? 112 : 96));
			draw->AddRectFilledMultiColor(viewportMin, viewportMax,
				IM_COL32(0, 0, 0, 92), IM_COL32(0, 0, 0, 92),
				IM_COL32(0, 0, 0, 176), IM_COL32(0, 0, 0, 176));
			return;
		}

		draw->AddRectFilledMultiColor(viewportMin, viewportMax,
			Color(theme.backgroundAccent), Color(theme.backgroundAccent),
			Color(theme.background), Color(theme.background));
		EnsureShapes(context);
		for (FloatingShape& shape : m_shapes)
		{
			shape.position.y -= shape.speed * deltaSeconds;
			shape.position.x += std::sin(static_cast<float>(ImGui::GetTime()) * 0.7f + shape.phase) *
				shape.wobble * deltaSeconds;
			shape.rotation += shape.rotationSpeed * deltaSeconds;
			if (shape.position.y + shape.size < -20.0f)
			{
				shape.position.y = context.layoutUnitsH + shape.size + 20.0f;
				shape.position.x = std::fmod(shape.position.x + 319.0f, context.layoutUnitsW);
			}
			ImVec4 color = theme.shapeColor;
			color.w = shape.alpha;
			DrawShape(draw, context, shape, Color(color));
		}
	}

	void EnsureShapes(const ShellLayoutContext& context)
	{
		const int targetCount = std::clamp(static_cast<int>(std::round(30.0f *
			std::sqrt((context.layoutUnitsW * context.layoutUnitsH) / (1280.0f * 720.0f)))),
			30, 56);
		if (static_cast<int>(m_shapes.size()) == targetCount &&
			std::abs(m_shapeArea.x - context.layoutUnitsW) < 8.0f &&
			std::abs(m_shapeArea.y - context.layoutUnitsH) < 8.0f)
		{
			return;
		}

		m_shapeArea = { context.layoutUnitsW, context.layoutUnitsH };
		m_shapes.clear();
		std::mt19937 random(0x55495046u);
		std::uniform_real_distribution<float> unit(0.0f, 1.0f);
		for (int index = 0; index < targetCount; ++index)
		{
			FloatingShape shape;
			shape.position = { unit(random) * context.layoutUnitsW,
				unit(random) * context.layoutUnitsH };
			shape.size = 14.0f + unit(random) * 40.0f;
			shape.speed = 6.0f + unit(random) * 22.0f;
			shape.phase = unit(random) * 6.2831853f;
			shape.wobble = 6.0f + unit(random) * 16.0f;
			shape.rotation = unit(random) * 6.2831853f;
			shape.rotationSpeed = (unit(random) > 0.5f ? 1.0f : -1.0f) *
				(0.18f + unit(random) * 0.32f);
			shape.type = index % 5;
			shape.alpha = 0.05f + unit(random) * 0.11f;
			m_shapes.push_back(shape);
		}
	}

	void DrawShape(ImDrawList* draw, const ShellLayoutContext& context,
		const FloatingShape& shape, ImU32 color)
	{
		const ImVec2 center = ToPx(context, shape.position);
		const float radius = shape.size * context.uiScale;
		if (shape.type == 0)
		{
			draw->AddCircleFilled(center, radius, color, 24);
			return;
		}
		const int pointCount = shape.type == 1 ? 3 : (shape.type == 4 ? 6 : 4);
		std::array<ImVec2, 6> points{};
		for (int index = 0; index < pointCount; ++index)
		{
			const float angle = shape.rotation + static_cast<float>(index) *
				(6.2831853f / static_cast<float>(pointCount));
			points[index] = { center.x + std::cos(angle) * radius,
				center.y + std::sin(angle) * radius };
		}
		draw->AddConvexPolyFilled(points.data(), pointCount, color);
	}

	void DrawTopHud(ImDrawList* draw, const ShellLayoutContext& context,
		const FrontendConfiguration& configuration, const ShellPresentation& presentation,
		const ShellTheme& theme)
	{
		const UnitRect clockRect{ 24.0f, 14.0f, 150.0f, 62.0f };
		DrawShellPanelPx(draw, RectMinPx(context, clockRect), RectMaxPx(context, clockRect),
			theme, 20.0f * context.uiScale, 0.92f);
		const auto [time, date] = LocalClockText();
		const ImVec2 timeSize = TextSizeUnits(context, 24.0f, time);
		const ImVec2 dateSize = TextSizeUnits(context, 12.6f, date);
		DrawText(draw, context, 24.0f,
			{ clockRect.x + (clockRect.w - timeSize.x) * 0.5f, clockRect.y + 8.0f },
			Color(theme.textPrimary), time);
		DrawText(draw, context, 12.6f,
			{ clockRect.x + (clockRect.w - dateSize.x) * 0.5f, clockRect.y + 39.0f },
			Color(theme.textSecondary), date);

		const UnitRect logoRect{ context.layoutUnitsW * 0.5f - 32.0f, 13.0f, 64.0f, 64.0f };
		DrawShellPanelPx(draw, RectMinPx(context, logoRect), RectMaxPx(context, logoRect),
			theme, 32.0f * context.uiScale, 0.86f);
		DrawTexture(draw, context, Texture(configuration.logo, true), logoRect.Shrunk(8.0f), 24.0f);

		const UnitRect statusRect{ context.layoutUnitsW - 174.0f, 14.0f, 150.0f, 62.0f };
		DrawShellPanelPx(draw, RectMinPx(context, statusRect), RectMaxPx(context, statusRect),
			theme, 20.0f * context.uiScale, 0.92f);
		constexpr float statusFontSize = 13.5f;
		constexpr float statusHorizontalPadding = 10.0f;
		const float statusMaxWidth = std::max(0.0f,
			statusRect.w - statusHorizontalPadding * 2.0f);
		const std::string status = Ellipsize(context, statusFontSize,
			presentation.controllerStatus.empty() ? "Controller" : presentation.controllerStatus,
			statusMaxWidth);
		const ImVec2 statusSize = TextSizeUnits(context, statusFontSize, status);
		const float statusMinX = statusRect.x + statusHorizontalPadding;
		const float statusMaxX = std::max(statusMinX,
			statusRect.right() - statusHorizontalPadding - statusSize.x);
		const float statusX = std::clamp(
			statusRect.x + (statusRect.w - statusSize.x) * 0.5f,
			statusMinX, statusMaxX);
		const auto drawStatus = [&](float y) {
			const UnitRect clipRect = statusRect.Shrunk(4.0f);
			draw->PushClipRect(RectMinPx(context, clipRect), RectMaxPx(context, clipRect), true);
			DrawText(draw, context, statusFontSize, { statusX, y },
				Color(theme.textSecondary), status);
			draw->PopClipRect();
		};
		if (presentation.showControllerBattery)
		{
			const UnitRect body{ statusRect.x + 45.0f, statusRect.y + 12.0f,
				46.0f, 22.0f };
			AddRoundedRectStrokePx(draw, RectMinPx(context, body),
				RectMaxPx(context, body), Color(theme.textPrimary),
				9.0f * context.uiScale, 2.0f * context.uiScale);
			const UnitRect nub{ body.right() + 2.0f, body.y + 6.0f, 4.0f, 10.0f };
			AddRoundedRectFilledPx(draw, RectMinPx(context, nub),
				RectMaxPx(context, nub), Color(theme.textPrimary),
				2.0f * context.uiScale);
			const float fillWidth = (body.w - 8.0f) * std::clamp(
				presentation.controllerBatteryLevel, 0.0f, 1.0f);
			if (fillWidth > 0.0f)
			{
				const UnitRect fill{ body.x + 4.0f, body.y + 4.0f,
					fillWidth, body.h - 8.0f };
				AddRoundedRectFilledPx(draw, RectMinPx(context, fill),
					RectMaxPx(context, fill), Color(theme.pageIndicatorActive),
					5.0f * context.uiScale);
			}
			drawStatus(statusRect.y + 40.0f);
		}
		else
		{
			drawStatus(statusRect.y + (statusRect.h - statusSize.y) * 0.5f);
		}
	}

	UnitRect RailRect(const ShellLayoutContext& context, bool left,
		std::size_t index, std::size_t count) const
	{
		constexpr float buttonSize = 70.0f;
		constexpr float gap = 16.0f;
		const float totalHeight = static_cast<float>(count) * buttonSize +
			static_cast<float>(count > 0 ? count - 1 : 0) * gap;
		return {
			left ? 14.0f : context.layoutUnitsW - 14.0f - buttonSize,
			(context.layoutUnitsH - totalHeight) * 0.5f + static_cast<float>(index) * (buttonSize + gap),
			buttonSize,
			buttonSize,
		};
	}

	void DrawRail(ImDrawList* draw, const ShellLayoutContext& context, Frontend& frontend,
		const ShellTheme& theme, bool left)
	{
		const auto& actions = left ? frontend.Configuration().leftRail :
			frontend.Configuration().rightRail;
		for (std::size_t index = 0; index < actions.size(); ++index)
		{
			const UnitRect rect = RailRect(context, left, index, actions.size());
			const bool focused = frontend.FocusArea() ==
				(left ? ShellFocusArea::LeftRail : ShellFocusArea::RightRail) &&
				(left ? frontend.LeftRailIndex() : frontend.RightRailIndex()) == index;
			DrawShellPanelPx(draw, RectMinPx(context, rect), RectMaxPx(context, rect), theme,
				16.0f * context.uiScale, focused ? 1.0f : 0.88f);
			DrawTexture(draw, context, Texture(actions[index].icon, true), rect.Shrunk(6.0f),
				12.0f, focused ? 1.0f : 0.92f);
			if (focused)
			{
				const ImVec2 labelSize = TextSizeUnits(context, 13.0f, actions[index].label);
				DrawText(draw, context, 13.0f,
					{ rect.x + (rect.w - labelSize.x) * 0.5f, rect.bottom() + 5.0f },
					Color(theme.textSecondary), actions[index].label);
			}
			if (!IsWidgetInputCaptured() &&
				ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
				rect.Contains(UnitPointFromPx(context, ImGui::GetIO().MousePos)))
			{
				const ShellFocusArea area = left ? ShellFocusArea::LeftRail :
					ShellFocusArea::RightRail;
				frontend.SetFocus(area, index);
				frontend.ActivateAction(area, index);
			}
		}
	}

	UnitRect GridRect(const ShellLayoutContext& context, std::size_t localIndex) const
	{
		const LayoutContext generic = MakeLayoutContext({ context.viewportPx.x, context.viewportPx.y });
		const GridLayout grid = MakeGridLayout(generic, 1);
		const float gridWidth = static_cast<float>(grid.columns) * grid.tileSize +
			static_cast<float>(grid.columns - 1) * grid.horizontalGap;
		const float gridHeight = static_cast<float>(grid.rows) * grid.tileSize +
			static_cast<float>(grid.rows - 1) * grid.verticalGap;
		const float originX = (context.layoutUnitsW - gridWidth) * 0.5f;
		const float originY = grid.band.y + (grid.band.height - gridHeight) * 0.5f;
		const std::size_t column = localIndex % grid.columns;
		const std::size_t row = localIndex / grid.columns;
		return {
			originX + static_cast<float>(column) * (grid.tileSize + grid.horizontalGap),
			originY + static_cast<float>(row) * (grid.tileSize + grid.verticalGap),
			grid.tileSize,
			grid.tileSize,
		};
	}

	void BeginPendingLaunch(const ShellLayoutContext& context, Frontend& frontend,
		const ShellPresentation& presentation)
	{
		if (m_launch.active)
			return;
		const auto pending = frontend.PendingLaunchItem();
		if (!pending)
			return;
		const auto found = std::find_if(frontend.Catalogue().begin(),
			frontend.Catalogue().end(), [pending](const LibraryItem& item) {
				return item.id == *pending;
			});
		if (found == frontend.Catalogue().end())
		{
			(void)frontend.CommitPendingLaunch();
			return;
		}
		const std::size_t index = static_cast<std::size_t>(
			std::distance(frontend.Catalogue().begin(), found));
		m_launch.active = true;
		m_launch.committed = false;
		m_launch.timer = 0.0f;
		m_launch.from = GridRect(context,
			index % frontend.State().ItemsPerPage());
		m_launch.tileSide = std::min(m_launch.from.w, m_launch.from.h);
		m_launch.aspect = 1.0f;
		m_launch.texture = PreferredArtwork(*found, presentation);
		if (m_launch.texture && m_launch.texture.width > 0 && m_launch.texture.height > 0)
		{
			// start from the artwork as drawn on the tile so the zoom keeps its shape
			m_launch.aspect = static_cast<float>(m_launch.texture.width) /
				static_cast<float>(m_launch.texture.height);
			const UnitRect bounds = m_launch.from.Shrunk(8.0f);
			float width = bounds.h * m_launch.aspect;
			float height = bounds.h;
			if (width > bounds.w)
			{
				width = bounds.w;
				height = width / m_launch.aspect;
			}
			m_launch.from = { bounds.x + (bounds.w - width) * 0.5f,
				bounds.y + (bounds.h - height) * 0.5f, width, height };
		}
	}

	void DrawLaunchAnimation(ImDrawList* draw,
		const ShellLayoutContext& context, Frontend& frontend,
		const ShellTheme& theme, float deltaSeconds)
	{
		if (!m_launch.active)
			return;
		m_launch.timer += deltaSeconds;
		constexpr float zoomDuration = 0.45f;
		constexpr float holdDuration = 0.35f;
		constexpr float fadeDuration = 0.25f;
		constexpr float blackDuration = 0.30f;
		// fixed width, height follows the artwork's aspect ratio
		float targetW = m_launch.tileSide * 1.8f;
		float targetH = targetW / std::max(0.001f, m_launch.aspect);
		const float maxTargetH = context.layoutUnitsH * 0.9f;
		if (targetH > maxTargetH)
		{
			targetH = maxTargetH;
			targetW = targetH * m_launch.aspect;
		}
		const UnitRect target{
			(context.layoutUnitsW - targetW) * 0.5f,
			(context.layoutUnitsH - targetH) * 0.5f,
			targetW,
			targetH,
		};
		const float zoom = Ease(ShellEasing::OutCubic,
			std::clamp(m_launch.timer / zoomDuration, 0.0f, 1.0f));
		const UnitRect rect{
			m_launch.from.x + (target.x - m_launch.from.x) * zoom,
			m_launch.from.y + (target.y - m_launch.from.y) * zoom,
			m_launch.from.w + (target.w - m_launch.from.w) * zoom,
			m_launch.from.h + (target.h - m_launch.from.h) * zoom,
		};
		const float fadeStart = zoomDuration + holdDuration;
		const float fade = std::clamp(
			(m_launch.timer - fadeStart) / fadeDuration, 0.0f, 1.0f);
		const ImVec2 viewportMax{
			context.originPx.x + context.viewportPx.x,
			context.originPx.y + context.viewportPx.y,
		};
		draw->AddRectFilled(context.originPx, viewportMax,
			IM_COL32(0, 0, 0, static_cast<int>(128.0f * zoom)));
		if (m_launch.texture)
			DrawTexture(draw, context, m_launch.texture, rect, 24.0f, 1.0f - fade);
		else
		{
			DrawShellPanelPx(draw, RectMinPx(context, rect), RectMaxPx(context, rect),
				theme, 24.0f * context.uiScale, 1.0f - fade);
		}
		if (!m_launch.committed && m_launch.timer >= fadeStart)
		{
			m_launch.committed = true;
			(void)frontend.CommitPendingLaunch();
		}
		const float black = std::clamp(
			(m_launch.timer - fadeStart - fadeDuration) / blackDuration,
			0.0f, 1.0f);
		if (black > 0.0f)
		{
			draw->AddRectFilled(context.originPx, viewportMax,
				IM_COL32(0, 0, 0, static_cast<int>(255.0f * black)));
		}
		if (m_launch.timer > fadeStart + fadeDuration + blackDuration + 0.15f)
			m_launch = {};
	}

	void DrawCatalogue(ImDrawList* draw, const ShellLayoutContext& context,
		Frontend& frontend, const ShellPresentation& presentation, const ShellTheme& theme,
		float deltaSeconds)
	{
		const std::size_t page = frontend.State().SelectedPage();
		if (page != m_lastPage)
		{
			m_lastPage = page;
			m_pageAppearTime = 0.0f;
		}
		m_pageAppearTime += deltaSeconds;

		const std::size_t pageSize = frontend.State().ItemsPerPage();
		const std::size_t first = page * pageSize;
		const std::size_t visibleCount = frontend.Catalogue().empty() ? 1 :
			std::min(pageSize, frontend.Catalogue().size() - std::min(first, frontend.Catalogue().size()));
		for (std::size_t localIndex = 0; localIndex < visibleCount; ++localIndex)
		{
			const std::size_t globalIndex = first + localIndex;
			const bool hasItem = globalIndex < frontend.Catalogue().size();
			const bool focused = frontend.FocusArea() == ShellFocusArea::Catalogue &&
				frontend.State().SelectedIndex() == globalIndex;
			float& focusAmount = m_focusAmounts[globalIndex];
			focusAmount += ((focused ? 1.0f : 0.0f) - focusAmount) *
				std::clamp(deltaSeconds * (focused ? 10.0f : 8.0f), 0.0f, 1.0f);

			const std::size_t column = localIndex % frontend.State().Columns();
			const std::size_t row = localIndex / frontend.State().Columns();
			const float delay = static_cast<float>(column + row) /
				static_cast<float>((frontend.State().Columns() - 1) + (frontend.State().Rows() - 1)) * 0.40f;
			const float appear = Ease(ShellEasing::OutExpo,
				std::clamp((m_pageAppearTime - delay) / 0.40f, 0.0f, 1.0f));
			if (appear <= 0.01f)
				continue;

			const UnitRect base = GridRect(context, localIndex);
			const float focusScale = std::clamp(
				Ease(ShellEasing::OutBack, focusAmount), 0.0f, 1.0f);
			const float scale = appear * (1.0f + 0.20f * focusScale);
			UnitRect rect = base;
			rect.w *= scale;
			rect.h *= scale;
			rect.x += (base.w - rect.w) * 0.5f;
			rect.y += (base.h - rect.h) * 0.5f;
			DrawShellPanelPx(draw, RectMinPx(context, rect), RectMaxPx(context, rect), theme,
				16.0f * context.uiScale, appear);

			TextureHandle artwork;
			if (hasItem)
				artwork = PreferredArtwork(frontend.Catalogue()[globalIndex], presentation);
			if (!artwork && !hasItem)
				artwork = Texture(frontend.Configuration().placeholderArtwork, true);
			if (artwork)
				DrawTextureContained(draw, context, artwork, rect.Shrunk(hasItem ? 8.0f : 18.0f),
					13.0f, hasItem ? appear : appear * 0.42f);
			else if (!hasItem)
			{
				const ImVec2 plusSize = TextSizeUnits(context, 42.0f, "+");
				DrawText(draw, context, 42.0f,
					{ rect.x + (rect.w - plusSize.x) * 0.5f,
					  rect.y + (rect.h - plusSize.y) * 0.5f },
					ColorWithAlpha(theme.textSecondary, 0.55f * appear), "+");
			}

			const bool hovered = !IsWidgetInputCaptured() &&
				base.Contains(UnitPointFromPx(context, ImGui::GetIO().MousePos));
			if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && hasItem)
			{
				frontend.Select(globalIndex);
				frontend.OpenContextMenu();
			}
			else if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
			{
				frontend.Select(globalIndex);
				if (hasItem)
					frontend.ActivateSelection();
				else
					m_host.Execute(HostCommand::AddContent);
			}
		}

		DrawTitlePill(draw, context, frontend, theme, deltaSeconds);
		DrawPageIndicator(draw, context, frontend.State().SelectedPage(),
			frontend.State().PageCount(), theme);
	}

	void DrawTitlePill(ImDrawList* draw, const ShellLayoutContext& context,
		const Frontend& frontend, const ShellTheme& theme, float deltaSeconds)
	{
		std::string title = frontend.Configuration().emptyCatalogueLabel;
		if (frontend.FocusArea() == ShellFocusArea::LeftRail &&
			frontend.LeftRailIndex() < frontend.Configuration().leftRail.size())
		{
			title = frontend.Configuration().leftRail[frontend.LeftRailIndex()].label;
		}
		else if (frontend.FocusArea() == ShellFocusArea::RightRail &&
			frontend.RightRailIndex() < frontend.Configuration().rightRail.size())
		{
			title = frontend.Configuration().rightRail[frontend.RightRailIndex()].label;
		}
		else if (!frontend.Catalogue().empty() &&
			frontend.State().SelectedIndex() < frontend.Catalogue().size())
		{
			title = frontend.Catalogue()[frontend.State().SelectedIndex()].name;
		}
		const bool changed = title != m_title;
		if (changed)
		{
			m_title = title;
			m_titleReveal.SetImmediate(0.32f);
			m_titleReveal.Set(1.0f, 0.18f);
		}
		constexpr float padding = 48.0f;
		const float maximumWidth = std::max(54.0f, context.layoutUnitsW - 64.0f);
		const float naturalWidth = std::ceil(TextSizeUnits(context, 24.0f, title).x) + padding;
		const float targetWidth = std::min(maximumWidth, naturalWidth);
		const float targetX = (context.layoutUnitsW - targetWidth) * 0.5f;
		if (m_titleWidth.value <= 0.5f)
		{
			m_titleWidth.SetImmediate(std::min(targetWidth, 54.0f));
			m_titleX.SetImmediate((context.layoutUnitsW - m_titleWidth.value) * 0.5f);
		}
		if (changed || std::abs(m_titleWidth.target - targetWidth) >= 0.01f ||
			std::abs(m_titleX.target - targetX) >= 0.01f)
		{
			m_titleWidth.Set(targetWidth, 0.22f);
			m_titleX.Set(targetX, 0.22f);
		}
		m_titleWidth.Update(deltaSeconds);
		m_titleX.Update(deltaSeconds);
		m_titleReveal.Update(deltaSeconds);
		const UnitRect rect{ m_titleX.value, context.layoutUnitsH - 90.0f,
			m_titleWidth.value, 42.0f };
		DrawShellPanelPx(draw, RectMinPx(context, rect), RectMaxPx(context, rect), theme,
			21.0f * context.uiScale, 0.92f);
		const std::string fitted = Ellipsize(context, 24.0f, title,
			std::max(4.0f, rect.w - padding));
		const ImVec2 textSize = TextSizeUnits(context, 24.0f, fitted);
		draw->PushClipRect(RectMinPx(context, rect), RectMaxPx(context, rect), true);
		DrawText(draw, context, 24.0f,
			{ rect.x + (rect.w - textSize.x) * 0.5f,
			  rect.y + (rect.h - textSize.y) * 0.5f +
				  (1.0f - m_titleReveal.value) * 3.0f },
			ColorWithAlpha(theme.textPrimary, m_titleReveal.value), fitted);
		draw->PopClipRect();
	}

	void DrawPageIndicator(ImDrawList* draw, const ShellLayoutContext& context,
		std::size_t currentPage, std::size_t pageCount, const ShellTheme& theme)
	{
		if (pageCount <= 1)
			return;
		constexpr float dotRadius = 4.0f;
		constexpr float gap = 14.0f;
		const float dotsWidth = static_cast<float>(pageCount) * dotRadius * 2.0f +
			static_cast<float>(pageCount - 1) * gap;
		const UnitRect panel{ (context.layoutUnitsW - (dotsWidth + 36.0f)) * 0.5f,
			context.layoutUnitsH - 35.0f, dotsWidth + 36.0f, 28.0f };
		DrawShellPanelPx(draw, RectMinPx(context, panel), RectMaxPx(context, panel), theme,
			14.0f * context.uiScale, 0.70f);
		for (std::size_t index = 0; index < pageCount; ++index)
		{
			const ImVec2 center = ToPx(context,
				panel.x + 18.0f + static_cast<float>(index) * (dotRadius * 2.0f + gap) + dotRadius,
				panel.y + panel.h * 0.5f);
			draw->AddCircleFilled(center, dotRadius * context.uiScale,
				index == currentPage ? Color(theme.pageIndicatorActive) : Color(theme.pageIndicator), 18);
		}
	}

	void DrawActionHints(ImDrawList* draw, const ShellLayoutContext& context,
		const FrontendConfiguration& configuration, const ShellTheme& theme)
	{
		if (configuration.actionHints.empty())
			return;
		constexpr float rowHeight = 22.0f;
		constexpr float horizontalPadding = 10.0f;
		constexpr float verticalPadding = 8.0f;
		constexpr float gap = 3.0f;
		float contentWidth = 0.0f;
		for (const ActionHint& hint : configuration.actionHints)
			contentWidth = std::max(contentWidth,
				TextSizeUnits(context, 13.0f, hint.button + "  " + hint.label).x);
		const float panelWidth = std::clamp(contentWidth + horizontalPadding * 2.0f,
			104.0f, 230.0f);
		const float panelHeight = verticalPadding * 2.0f +
			static_cast<float>(configuration.actionHints.size()) * rowHeight +
			static_cast<float>(configuration.actionHints.size() - 1) * gap;
		const UnitRect panel{ context.layoutUnitsW - 18.0f - panelWidth,
			context.layoutUnitsH - 18.0f - panelHeight, panelWidth, panelHeight };
		DrawShellPanelPx(draw, RectMinPx(context, panel), RectMaxPx(context, panel), theme,
			16.0f * context.uiScale, 0.72f);
		float y = panel.y + verticalPadding;
		for (const ActionHint& hint : configuration.actionHints)
		{
			DrawText(draw, context, 13.0f, { panel.x + horizontalPadding, y + 2.0f },
				Color(theme.textPrimary), hint.button);
			DrawText(draw, context, 12.0f, { panel.x + horizontalPadding + 42.0f, y + 3.0f },
				Color(theme.textSecondary), hint.label);
			y += rowHeight + gap;
		}
	}

	void DrawCursor(ImDrawList* draw, const ShellLayoutContext& context,
		const Frontend& frontend, const ShellTheme& theme, float deltaSeconds)
	{
		UnitRect target;
		switch (frontend.FocusArea())
		{
		case ShellFocusArea::LeftRail:
			target = RailRect(context, true, frontend.LeftRailIndex(),
				frontend.Configuration().leftRail.size());
			break;
		case ShellFocusArea::RightRail:
			target = RailRect(context, false, frontend.RightRailIndex(),
				frontend.Configuration().rightRail.size());
			break;
		case ShellFocusArea::Catalogue:
		default:
			target = GridRect(context,
				frontend.State().SelectedIndex() % frontend.State().ItemsPerPage());
			break;
		}
		m_cursor.MoveTo(target, 17.0f, 0.09f);
		m_cursor.Update(deltaSeconds);
		m_cursor.Draw(context, draw, theme);
	}

	void Draw(Frontend& frontend, const ShellPresentation& presentation,
		float deltaSeconds)
	{
		const ImGuiViewport* viewport = ImGui::GetMainViewport();
		if (!viewport)
			return;
		const ShellLayoutContext context = MakeShellLayout(viewport->WorkSize, viewport->WorkPos);
		const auto& themes = ShellThemes();
		const ShellTheme& theme = presentation.customTheme ? *presentation.customTheme :
			themes[std::clamp(presentation.themeIndex, 0,
				static_cast<int>(themes.size()) - 1)];

		ImGui::SetNextWindowPos(viewport->WorkPos);
		ImGui::SetNextWindowSize(viewport->WorkSize);
		constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration |
			ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
			ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoBackground;
		ImFont* frontendFont = ResolveFrontendTextFont(presentation.font);
		if (frontendFont)
			ImGui::PushFont(frontendFont);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, { 0.0f, 0.0f });
		ImGui::Begin("##uwp-imgui-frontend", nullptr, flags);
		ImDrawList* draw = ImGui::GetWindowDrawList();
		const float dt = std::clamp(deltaSeconds, 0.0f, 1.0f / 15.0f);
		DrawBackground(draw, context, frontend, presentation, theme, dt);
		DrawTopHud(draw, context, frontend.Configuration(), presentation, theme);
		DrawRail(draw, context, frontend, theme, true);
		DrawRail(draw, context, frontend, theme, false);
		DrawCatalogue(draw, context, frontend, presentation, theme, dt);
		DrawCursor(draw, context, frontend, theme, dt);
		if (presentation.showActionHints)
			DrawActionHints(draw, context, frontend.Configuration(), theme);
		const auto notifications = frontend.Notifications();
		DrawNotificationCards(ImGui::GetForegroundDrawList(), context,
			theme, notifications);
		BeginPendingLaunch(context, frontend, presentation);
		DrawLaunchAnimation(draw, context, frontend, theme, dt);
		ImGui::End();
		ImGui::PopStyleVar();
		if (frontendFont)
			ImGui::PopFont();
	}

private:
	IFrontendHost& m_host;
	std::map<std::filesystem::path, TextureHandle> m_textures;
	std::vector<FloatingShape> m_shapes;
	ImVec2 m_shapeArea{};
	std::map<std::size_t, float> m_focusAmounts;
	std::size_t m_lastPage = static_cast<std::size_t>(-1);
	float m_pageAppearTime = 0.0f;
	std::string m_title;
	AnimatedFloat m_titleX;
	AnimatedFloat m_titleWidth;
	AnimatedFloat m_titleReveal;
	CursorAnimation m_cursor;
	struct LaunchAnimation
	{
		bool active = false;
		bool committed = false;
		float timer = 0.0f;
		UnitRect from;
		float tileSide = 0.0f;
		// artwork width / height
		float aspect = 1.0f;
		TextureHandle texture;
	} m_launch;
};

ShellRenderer::ShellRenderer(IFrontendHost& host)
	: m_impl(std::make_unique<Impl>(host))
{
}

ShellRenderer::~ShellRenderer() = default;
ShellRenderer::ShellRenderer(ShellRenderer&&) noexcept = default;
ShellRenderer& ShellRenderer::operator=(ShellRenderer&&) noexcept = default;

void ShellRenderer::Draw(Frontend& frontend, const ShellPresentation& presentation,
	float deltaSeconds)
{
	m_impl->Draw(frontend, presentation, deltaSeconds);
}

void ShellRenderer::ClearTextures()
{
	m_impl->ClearTextures();
}
}
