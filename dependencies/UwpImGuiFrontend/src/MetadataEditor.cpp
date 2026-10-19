#include "UwpImGuiFrontend/MetadataEditor.h"
#include "UwpImGuiFrontend/Text.h"

#include "UwpImGuiFrontend/NativeTextInput.h"

#include <imgui_internal.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <vector>

namespace UwpImGuiFrontend
{
namespace
{
enum class Row : int
{
	Title,
	Description,
	Rating,
	ReleaseDate,
	Developer,
	Publisher,
	Genre,
	Players,
	Search,
	Save,
	Cancel,
	Count,
};

constexpr std::array<ScreenScraperArtwork, 10> kArtworkOrder{
	ScreenScraperArtwork::Box2D,
	ScreenScraperArtwork::Box3D,
	ScreenScraperArtwork::MixRecalboxV1,
	ScreenScraperArtwork::MixRecalboxV2,
	ScreenScraperArtwork::FanArt,
	ScreenScraperArtwork::Screenshot,
	ScreenScraperArtwork::Logo,
	ScreenScraperArtwork::BackCover,
	ScreenScraperArtwork::PhysicalMedia,
	ScreenScraperArtwork::TitleScreen,
};

ImU32 Color(const ImVec4& color)
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

template <std::size_t Size>
void CopyBuffer(std::array<char, Size>& buffer, std::string_view value)
{
	const std::size_t length = std::min(value.size(), Size - 1);
	std::memcpy(buffer.data(), value.data(), length);
	buffer[length] = '\0';
}

void DrawText(ImDrawList* draw, const ShellLayoutContext& context, ImFont* font,
	float size, ImVec2 position, ImU32 color, std::string_view text)
{
	const ImVec2 pixelPosition = ToPx(context, position);
	if (font)
		draw->AddText(font, size * context.uiScale, pixelPosition, color,
			text.data(), text.data() + text.size());
	else
		draw->AddText(pixelPosition, color, text.data(), text.data() + text.size());
}

ImVec2 MeasureText(const ShellLayoutContext& context, ImFont* font, float size,
	std::string_view text, float wrapWidth = 0.0f)
{
	const float wrapPixels = wrapWidth > 0.0f ? wrapWidth * context.uiScale : 0.0f;
	const ImVec2 measured = font ? font->CalcTextSizeA(size * context.uiScale,
		FLT_MAX, wrapPixels, text.data(), text.data() + text.size()) :
		ImGui::CalcTextSize(text.data(), text.data() + text.size(), false, wrapPixels);
	return { measured.x / context.uiScale, measured.y / context.uiScale };
}

float DrawWrappedText(ImDrawList* draw, const ShellLayoutContext& context,
	ImFont* font, float size, ImVec2 position, float width, ImU32 color,
	std::string_view text)
{
	if (text.empty())
		return 0.0f;
	const ImVec2 measured = MeasureText(context, font, size, text, width);
	const ImVec2 pixelPosition = ToPx(context, position);
	const float wrapPixels = std::max(1.0f, width * context.uiScale);
	if (font)
		draw->AddText(font, size * context.uiScale, pixelPosition, color,
			text.data(), text.data() + text.size(), wrapPixels);
	else
		draw->AddText(ImGui::GetFont(), ImGui::GetFontSize(), pixelPosition, color,
			text.data(), text.data() + text.size(), wrapPixels);
	return measured.y;
}

std::string Ellipsize(const ShellLayoutContext& context, ImFont* font,
	float size, std::string_view text, float width)
{
	if (MeasureText(context, font, size, text).x <= width)
		return std::string(text);
	std::string result(text);
	while (!result.empty() &&
		MeasureText(context, font, size, result + "...").x > width)
		result.pop_back();
	return result + "...";
}

void DrawGlassPanel(ImDrawList* draw, const ShellLayoutContext& context,
	const UnitRect& rect, const ShellTheme& theme, float radius, float opacity)
{
	DrawShellPanelPx(draw, RectMinPx(context, rect), RectMaxPx(context, rect),
		theme, radius * context.uiScale, opacity);
}

void DrawTextureContained(ImDrawList* draw, const ShellLayoutContext& context,
	TextureHandle texture, const UnitRect& bounds, float radius)
{
	if (!texture || bounds.w <= 0.0f || bounds.h <= 0.0f)
		return;
	const float aspect = static_cast<float>(texture.width) /
		std::max(1.0f, static_cast<float>(texture.height));
	float height = bounds.h;
	float width = height * aspect;
	if (width > bounds.w)
	{
		width = bounds.w;
		height = width / std::max(0.001f, aspect);
	}
	const UnitRect image{ bounds.x + (bounds.w - width) * 0.5f,
		bounds.y + (bounds.h - height) * 0.5f, width, height };
	ImVec2 min = RectMinPx(context, image);
	ImVec2 max = RectMaxPx(context, image);
	NormalizeRoundedRectPx(min, max);
	draw->AddImageRounded(TextureReference(texture.id), min, max, { 0, 0 }, { 1, 1 },
		IM_COL32_WHITE, radius * context.uiScale, ImDrawFlags_RoundCornersAll);
}

void DrawStars(const ShellLayoutContext& context, ImDrawList* draw,
	const UnitRect& bounds, const ShellTheme& theme, int halfSteps)
{
	constexpr int pointCount = 10;
	const float radius = std::min(12.0f, bounds.h * 0.30f);
	const float gap = radius * 2.35f;
	const float width = gap * 4.0f + radius * 2.0f;
	for (int star = 0; star < 5; ++star)
	{
		const ImVec2 center = ToPx(context, bounds.x + (bounds.w - width) * 0.5f +
			radius + star * gap, bounds.y + bounds.h * 0.5f);
		std::array<ImVec2, pointCount> points{};
		for (int point = 0; point < pointCount; ++point)
		{
			const float angle = -1.5707963f + point * 0.6283185f;
			const float pointRadius = (point % 2 == 0 ? radius : radius * 0.44f) *
				context.uiScale;
			points[point] = { center.x + std::cos(angle) * pointRadius,
				center.y + std::sin(angle) * pointRadius };
		}
		const int fill = std::clamp(halfSteps - star * 2, 0, 2);
		draw->AddConvexPolyFilled(points.data(), pointCount,
			ColorWithAlpha(theme.textSecondary, 0.30f));
		if (fill != 0)
		{
			if (fill == 1)
			{
				draw->PushClipRect({ center.x - radius * context.uiScale,
					center.y - radius * context.uiScale },
					{ center.x, center.y + radius * context.uiScale }, true);
			}
			draw->AddConvexPolyFilled(points.data(), pointCount,
				Color(theme.textPrimary));
			if (fill == 1)
				draw->PopClipRect();
		}
	}
}

const char* RowLabel(int row)
{
	switch (static_cast<Row>(row))
	{
	case Row::Title: return Translate("Title");
	case Row::Description: return Translate("Description");
	case Row::Rating: return Translate("Rating");
	case Row::ReleaseDate: return Translate("Release date");
	case Row::Developer: return Translate("Developer");
	case Row::Publisher: return Translate("Publisher");
	case Row::Genre: return Translate("Genre");
	case Row::Players: return Translate("Players");
	case Row::Search: return Translate("Find ScreenScraper match");
	case Row::Save: return Translate("Save");
	case Row::Cancel: return Translate("Cancel");
	case Row::Count: break;
	}
	return "";
}

bool IsTextRow(int row)
{
	return row == static_cast<int>(Row::Title) ||
		row == static_cast<int>(Row::Description) ||
		row == static_cast<int>(Row::ReleaseDate) ||
		row == static_cast<int>(Row::Developer) ||
		row == static_cast<int>(Row::Publisher) ||
		row == static_cast<int>(Row::Genre) ||
		row == static_cast<int>(Row::Players);
}

std::size_t NonSpaceCount(std::string_view text)
{
	return static_cast<std::size_t>(std::count_if(text.begin(), text.end(),
		[](unsigned char value) { return !std::isspace(value); }));
}

void ClearRetainedTextNavigationFocus()
{
	if (!GImGui)
		return;
	ImGui::ClearActiveID();
	ImGuiContext& context = *GImGui;
	if (context.NavWindow)
		context.NavWindow->NavLastIds[context.NavLayer] = 0;
	context.NavId = 0;
	context.NavActivateId = 0;
	context.NavActivateDownId = 0;
	context.NavActivatePressedId = 0;
	context.NavNextActivateId = 0;
	context.NavIdIsAlive = false;
}
}

int MetadataRatingStars(std::string_view rating)
{
	return static_cast<int>(std::lround(MetadataRatingHalfSteps(rating) / 2.0));
}

int MetadataRatingHalfSteps(std::string_view rating)
{
	if (rating.empty())
		return 0;
	std::string value(rating);
	char* end = nullptr;
	const double parsed = std::strtod(value.c_str(), &end);
	if (end == value.c_str())
		return 0;
	const double normalized = parsed > 1.0 ? parsed / 5.0 : parsed;
	return std::clamp(static_cast<int>(std::lround(normalized * 10.0)), 0, 10);
}

std::string MetadataRatingValue(int stars)
{
	if (stars <= 0)
		return {};
	char value[16]{};
	std::snprintf(value, sizeof(value), "%.1f", std::clamp(stars, 0, 5) / 5.0);
	return value;
}

std::string MetadataRatingValueFromHalfSteps(int halfSteps)
{
	if (halfSteps <= 0)
		return {};
	char value[16]{};
	std::snprintf(value, sizeof(value), "%.1f",
		std::clamp(halfSteps, 0, 10) / 10.0);
	return value;
}

bool ScreenScraperMatchHasArtwork(const ScreenScraperMatch& match,
	ScreenScraperArtwork artwork)
{
	if (std::any_of(match.previews.begin(), match.previews.end(),
		[artwork](const auto& preview) { return preview.artwork == artwork; }))
	{
		return true;
	}
	const auto lower = [](std::string_view value) {
		std::string lowered(value);
		std::transform(lowered.begin(), lowered.end(), lowered.begin(),
			[](unsigned char character) { return static_cast<char>(std::tolower(character)); });
		return lowered;
	};
	const auto hasType = [&](std::string_view type) {
		const std::string wanted = lower(type);
		return std::any_of(match.media.begin(), match.media.end(), [&](const auto& media) {
			return lower(media.type) == wanted &&
				(media.parent.empty() || lower(media.parent) == "jeu");
		});
	};
	switch (artwork)
	{
	case ScreenScraperArtwork::Box2D: return hasType("box-2D");
	case ScreenScraperArtwork::Box3D: return hasType("box-3D");
	case ScreenScraperArtwork::MixRecalboxV1: return hasType("mixrbv1");
	case ScreenScraperArtwork::MixRecalboxV2: return hasType("mixrbv2");
	case ScreenScraperArtwork::FanArt: return hasType("fanart");
	case ScreenScraperArtwork::Screenshot: return hasType("ss");
	case ScreenScraperArtwork::Logo: return hasType("wheel-hd") || hasType("wheel");
	case ScreenScraperArtwork::BackCover: return hasType("box-2D-back");
	case ScreenScraperArtwork::PhysicalMedia: return hasType("support-2D");
	case ScreenScraperArtwork::TitleScreen: return hasType("sstitle");
	}
	return false;
}

std::optional<ScreenScraperArtwork> CycleScreenScraperMatchArtwork(
	const ScreenScraperMatch& match, ScreenScraperArtwork current, int delta)
{
	std::vector<ScreenScraperArtwork> available;
	for (const ScreenScraperArtwork artwork : kArtworkOrder)
	{
		if (ScreenScraperMatchHasArtwork(match, artwork))
			available.push_back(artwork);
	}
	if (available.empty() || delta == 0)
		return std::nullopt;
	const auto found = std::find(available.begin(), available.end(), current);
	int index = found == available.end() ? (delta > 0 ? -1 : 0) :
		static_cast<int>(std::distance(available.begin(), found));
	index = (index + delta % static_cast<int>(available.size()) +
		static_cast<int>(available.size())) % static_cast<int>(available.size());
	return available[static_cast<std::size_t>(index)];
}

class MetadataEditor::Impl
{
public:
	explicit Impl(IScreenScraperService& service) : service(service) {}

	void Open(MetadataEditorItem nextItem, MetadataEditorOptions nextOptions,
		MetadataEditorCallbacks nextCallbacks)
	{
		item = std::move(nextItem);
		options = std::move(nextOptions);
		callbacks = std::move(nextCallbacks);
		view = MetadataEditorView::Editor;
		open = true;
		row = 0;
		lastRevealedRow = -1;
		requestTextFocus = -1;
		scroll = 0.0f;
		pendingMatch = false;
		pendingListArtwork.reset();
		pendingDetailArtwork.reset();
		CopyBuffer(title, NormalizeDisplayText(
			item.record.title.empty() ? item.defaultTitle : item.record.title));
		CopyBuffer(description, item.record.metadata.description);
		CopyBuffer(releaseDate, FormatMetadataReleaseDateUk(
			item.record.metadata.releaseDate));
		CopyBuffer(developer, item.record.metadata.developer);
		CopyBuffer(publisher, item.record.metadata.publisher);
		CopyBuffer(genre, item.record.metadata.genre);
		CopyBuffer(players, item.record.metadata.players);
		ratingHalfSteps = MetadataRatingHalfSteps(item.record.metadata.rating);
		ResetSearch();
	}

	MetadataEditorEvent HandleInput(const MetadataEditorInput& input)
	{
		if (!open)
			return MetadataEditorEvent::None;
		if (view == MetadataEditorView::Editor)
		{
			if (input.back && IsNativeTextInputActive())
			{
				DismissNativeTextInput();
				requestTextFocus = -1;
				return MetadataEditorEvent::None;
			}
			if (input.back)
				return Cancel();
			if (input.up)
				row = std::max(0, row - 1);
			if (input.down)
				row = std::min(static_cast<int>(Row::Count) - 1, row + 1);
			if (row == static_cast<int>(Row::Rating))
			{
				if (input.left)
					ratingHalfSteps = std::max(0, ratingHalfSteps - 1);
				if (input.right)
					ratingHalfSteps = std::min(10, ratingHalfSteps + 1);
			}
			if (std::abs(input.rightStickY) > 0.18f)
				scroll = std::max(0.0f, scroll - input.rightStickY * 420.0f * input.deltaSeconds);
			if (input.accept)
				return ActivateRow(row);
			return MetadataEditorEvent::None;
		}

		SyncSnapshotSelection();
		const auto snapshot = service.GetSearchSnapshot();
		const bool ready = snapshot.generation == generation &&
			snapshot.state == ScreenScraperSearchState::Ready && !snapshot.matches.empty();
		if (input.back && IsNativeTextInputActive())
		{
			DismissNativeTextInput();
			queryFocused = false;
			requestQueryFocus = false;
			return MetadataEditorEvent::None;
		}
		if (input.back)
		{
			service.CancelSearch();
			view = MetadataEditorView::Editor;
			return MetadataEditorEvent::None;
		}
		if (input.down && ready)
		{
			if (queryFocused)
				queryFocused = false;
			else
				selected = std::min(static_cast<int>(snapshot.matches.size()) - 1, selected + 1);
		}
		if (input.up)
		{
			if (!queryFocused && selected > 0)
				--selected;
			else
				queryFocused = true;
		}
		TrackSelectedMatch(snapshot);
		if (std::abs(input.rightStickY) > 0.18f)
			detailsScroll = std::max(0.0f, detailsScroll -
				input.rightStickY * 480.0f * input.deltaSeconds);
		if (input.leftShoulder)
			CycleArtwork(-1, snapshot);
		if (input.rightShoulder)
			CycleArtwork(1, snapshot);
		if (input.accept)
		{
			if (queryFocused)
				requestQueryFocus = true;
			else if (ready && selected >= 0 && selected < static_cast<int>(snapshot.matches.size()))
				ApplyMatch(snapshot.matches[selected]);
		}
		return MetadataEditorEvent::None;
	}

	MetadataEditorEvent Draw(const ShellLayoutContext& context, ImDrawList* draw,
		const UnitRect& panel, const ShellTheme& theme, ImFont* font)
	{
		if (!open || !draw)
			return MetadataEditorEvent::None;
		const ImGuiID triggerOwner = ImGui::GetID("##metadata-editor-trigger-owner");
		ImGui::SetKeyOwner(ImGuiKey_GamepadL2, triggerOwner,
			ImGuiInputFlags_LockThisFrame);
		ImGui::SetKeyOwner(ImGuiKey_GamepadR2, triggerOwner,
			ImGuiInputFlags_LockThisFrame);
		return view == MetadataEditorView::Editor ?
			DrawEditor(context, draw, panel, theme, font) :
			DrawBrowser(context, draw, panel, theme, font);
	}

	void ResetSearch()
	{
		query = {};
		observedQuery.clear();
		submittedQuery.clear();
		changedAt = std::chrono::steady_clock::now();
		generation = 0;
		displayedGeneration = 0;
		selectedGameId = 0;
		selected = 0;
		listScroll = 0.0f;
		detailsScroll = 0.0f;
		queryFocused = true;
		requestQueryFocus = false;
		queryInputWasActive = false;
		temporaryArtwork.reset();
	}

	void BeginSearch()
	{
		ResetSearch();
		CopyBuffer(query, title.data());
		observedQuery = query.data();
		changedAt = std::chrono::steady_clock::now();
		requestQueryFocus = true;
		view = MetadataEditorView::MatchBrowser;
	}

	std::pair<char*, std::size_t> BufferForRow(int selectedRow)
	{
		switch (static_cast<Row>(selectedRow))
		{
		case Row::Title: return { title.data(), title.size() };
		case Row::Description: return { description.data(), description.size() };
		case Row::ReleaseDate: return { releaseDate.data(), releaseDate.size() };
		case Row::Developer: return { developer.data(), developer.size() };
		case Row::Publisher: return { publisher.data(), publisher.size() };
		case Row::Genre: return { genre.data(), genre.size() };
		case Row::Players: return { players.data(), players.size() };
		default: return { nullptr, 0 };
		}
	}

	GameMetadataRecord Record() const
	{
		// copying item.record keeps fields this editor does not expose
		GameMetadataRecord record = item.record;
		record.itemId = item.record.itemId;
		record.sourceGameId = item.record.sourceGameId;
		record.title = title.data();
		record.metadata.description = description.data();
		record.metadata.releaseDate = NormalizeMetadataReleaseDate(releaseDate.data());
		record.metadata.developer = developer.data();
		record.metadata.publisher = publisher.data();
		record.metadata.genre = genre.data();
		record.metadata.players = players.data();
		record.metadata.rating = MetadataRatingValueFromHalfSteps(ratingHalfSteps);
		return record;
	}

	MetadataEditorEvent Save()
	{
		const GameMetadataRecord record = Record();
		if (callbacks.save && !callbacks.save(record, pendingMatch))
			return MetadataEditorEvent::None;
		open = false;
		service.CancelSearch();
		return MetadataEditorEvent::Saved;
	}

	MetadataEditorEvent Cancel()
	{
		open = false;
		service.CancelSearch();
		return MetadataEditorEvent::Cancelled;
	}

	MetadataEditorEvent ActivateRow(int selectedRow)
	{
		if (IsTextRow(selectedRow))
			requestTextFocus = selectedRow;
		else if (selectedRow == static_cast<int>(Row::Rating))
			ratingHalfSteps = ratingHalfSteps >= 10 ? 0 : ratingHalfSteps + 1;
		else if (selectedRow == static_cast<int>(Row::Search))
			BeginSearch();
		else if (selectedRow == static_cast<int>(Row::Save))
			return Save();
		else if (selectedRow == static_cast<int>(Row::Cancel))
			return Cancel();
		return MetadataEditorEvent::None;
	}

	void ApplyMatch(const ScreenScraperMatch& match)
	{
		item.record.metadata = match.metadata;
		CopyBuffer(title, match.title);
		CopyBuffer(description, match.metadata.description);
		CopyBuffer(releaseDate, FormatMetadataReleaseDateUk(
			match.metadata.releaseDate));
		CopyBuffer(developer, match.metadata.developer);
		CopyBuffer(publisher, match.metadata.publisher);
		CopyBuffer(genre, match.metadata.genre);
		CopyBuffer(players, match.metadata.players);
		ratingHalfSteps = MetadataRatingHalfSteps(match.metadata.rating);
		item.record.sourceGameId = match.gameId;
		const auto* list = FindPreview(match, options.listArtwork);
		const auto* detail = FindPreview(match, options.detailArtwork);
		pendingListArtwork = list ? std::optional(list->path) : std::nullopt;
		pendingDetailArtwork = detail ? std::optional(detail->path) : std::nullopt;
		pendingMatch = true;
		row = 0;
		scroll = 0.0f;
		service.CancelSearch();
		view = MetadataEditorView::Editor;
		queryInputWasActive = false;
	}

	void ServiceDebounce()
	{
		const std::string current = query.data();
		const auto now = std::chrono::steady_clock::now();
		if (current != observedQuery)
		{
			if (generation != 0)
				service.CancelSearch();
			generation = 0;
			displayedGeneration = 0;
			submittedQuery.clear();
			selected = 0;
			selectedGameId = 0;
			listScroll = 0.0f;
			detailsScroll = 0.0f;
			temporaryArtwork.reset();
			observedQuery = current;
			changedAt = now;
		}
		if (NonSpaceCount(current) < 2)
		{
			if (generation != 0)
				service.CancelSearch();
			generation = 0;
			displayedGeneration = 0;
			submittedQuery.clear();
			temporaryArtwork.reset();
			return;
		}
		if (current == submittedQuery || now - changedAt < std::chrono::milliseconds(200))
			return;
		submittedQuery = current;
		displayedGeneration = 0;
		selected = 0;
		selectedGameId = 0;
		listScroll = 0.0f;
		detailsScroll = 0.0f;
		temporaryArtwork.reset();
		auto request = options.searchRequest;
		request.query = current;
		generation = service.BeginSearch(std::move(request));
	}

	void SyncSnapshotSelection()
	{
		const auto snapshot = service.GetSearchSnapshot();
		if (snapshot.generation == generation && displayedGeneration != snapshot.generation)
		{
			displayedGeneration = snapshot.generation;
			selected = 0;
			selectedGameId = 0;
			listScroll = 0.0f;
			detailsScroll = 0.0f;
			temporaryArtwork.reset();
		}
	}

	void TrackSelectedMatch(const ScreenScraperSearchSnapshot& snapshot)
	{
		if (snapshot.generation != generation || selected < 0 ||
			selected >= static_cast<int>(snapshot.matches.size()))
			return;
		const std::uint64_t next = snapshot.matches[selected].gameId;
		if (next != selectedGameId)
		{
			selectedGameId = next;
			detailsScroll = 0.0f;
			temporaryArtwork.reset();
		}
	}

	void CycleArtwork(int delta, const ScreenScraperSearchSnapshot& snapshot)
	{
		if (snapshot.generation != generation || selected < 0 ||
			selected >= static_cast<int>(snapshot.matches.size()))
			return;
		const auto& match = snapshot.matches[selected];
		const auto next = CycleScreenScraperMatchArtwork(match,
			temporaryArtwork.value_or(options.listArtwork), delta);
		if (!next)
		{
			if (callbacks.notify)
				callbacks.notify(Translate("This ScreenScraper match has no supported artwork"));
			return;
		}
		temporaryArtwork = *next;
	}

	TextureHandle Texture(const std::filesystem::path& path)
	{
		return path.empty() || !callbacks.loadTexture ? TextureHandle{} :
			callbacks.loadTexture(path);
	}

	TextureHandle SearchTexture(const ScreenScraperSearchSnapshot& snapshot,
		const ScreenScraperMatch& match, ScreenScraperArtwork artwork,
		std::uint32_t maximumHeight)
	{
		if (!ScreenScraperMatchHasArtwork(match, artwork))
			return {};
		const auto* preview = FindPreview(match, artwork);
		if (!preview || preview->maximumHeight < maximumHeight)
			service.RequestSearchPreview(snapshot.generation, match.gameId, artwork,
				maximumHeight);
		return preview ? Texture(preview->path) : TextureHandle{};
	}

	float EditorRowHeight(int selectedRow, float rowsWidth,
		const ShellLayoutContext& context, ImFont* font) const
	{
		if (selectedRow != static_cast<int>(Row::Description))
			return 58.0f;
		const float textWidth = std::max(80.0f, rowsWidth - 40.0f);
		const float textHeight = MeasureText(context, font, 15.0f,
			description.data(), textWidth).y;
		return std::max(116.0f, textHeight + 64.0f);
	}

	void DrawArtworkPreview(const ShellLayoutContext& context, ImDrawList* draw,
		const UnitRect& bounds, const ShellTheme& theme, ImFont* font,
		const std::filesystem::path& path)
	{
		DrawGlassPanel(draw, context, bounds, theme, 14.0f, 0.72f);
		const UnitRect image{ bounds.x + 10.0f, bounds.y + 10.0f,
			bounds.w - 20.0f, bounds.h - 20.0f };
		if (const TextureHandle texture = Texture(path))
			DrawTextureContained(draw, context, texture, image, 10.0f);
		else
		{
			const std::string_view unavailable = Translate("No selected artwork");
			const ImVec2 size = MeasureText(context, font, 13.0f, unavailable);
			DrawText(draw, context, font, 13.0f,
				{ image.x + (image.w - size.x) * 0.5f,
				  image.y + (image.h - size.y) * 0.5f },
				ColorWithAlpha(theme.textSecondary, 0.70f), unavailable);
		}
	}

	MetadataEditorEvent DrawEditor(const ShellLayoutContext& context,
		ImDrawList* draw, const UnitRect& panel, const ShellTheme& theme, ImFont* font)
	{
		const float contentTop = panel.y + 88.0f;
		const float contentBottom = panel.bottom() - 18.0f;
		const float previewWidth = std::clamp(panel.w * 0.25f, 190.0f, 250.0f);
		const UnitRect listPreview{ panel.x + 18.0f, contentTop, previewWidth, 172.0f };
		const UnitRect detailPreview{ panel.x + 18.0f, listPreview.bottom() + 14.0f,
			previewWidth, std::max(150.0f, contentBottom - listPreview.bottom() - 14.0f) };
		DrawArtworkPreview(context, draw, listPreview, theme, font, pendingMatch ?
				pendingListArtwork.value_or(std::filesystem::path{}) : item.listArtwork);
		DrawArtworkPreview(context, draw, detailPreview, theme, font, pendingMatch ?
				pendingDetailArtwork.value_or(std::filesystem::path{}) : item.detailArtwork);

		const float rowsX = listPreview.right() + 16.0f;
		const float rowsWidth = panel.right() - rowsX - 18.0f;
		const float visibleHeight = std::max(120.0f, contentBottom - contentTop);
		std::array<float, static_cast<std::size_t>(Row::Count)> offsets{};
		float totalHeight = 0.0f;
		for (int index = 0; index < static_cast<int>(Row::Count); ++index)
		{
			offsets[index] = totalHeight;
			totalHeight += EditorRowHeight(index, rowsWidth, context, font) + 8.0f;
		}
		totalHeight = std::max(0.0f, totalHeight - 8.0f);
		if (lastRevealedRow != row)
		{
			const float selectedTop = offsets[row];
			const float selectedBottom = selectedTop +
				EditorRowHeight(row, rowsWidth, context, font);
			if (selectedTop < scroll)
				scroll = selectedTop;
			else if (selectedBottom > scroll + visibleHeight)
				scroll = selectedBottom - visibleHeight;
			lastRevealedRow = row;
		}
		scroll = std::clamp(scroll, 0.0f, std::max(0.0f, totalHeight - visibleHeight));

		const ImVec2 clipMin = ToPx(context, rowsX, contentTop);
		const ImVec2 clipMax = ToPx(context, rowsX + rowsWidth, contentBottom);
		draw->PushClipRect(clipMin, clipMax, true);
		ImGui::PushClipRect(clipMin, clipMax, true);
		const ImVec2 pointer = UnitPointFromPx(context, ImGui::GetIO().MousePos);
		MetadataEditorEvent event = MetadataEditorEvent::None;
		for (int index = 0; index < static_cast<int>(Row::Count); ++index)
		{
			const float height = EditorRowHeight(index, rowsWidth, context, font);
			const UnitRect rowRect{ rowsX, contentTop + offsets[index] - scroll,
				rowsWidth, height };
			if (rowRect.bottom() < contentTop || rowRect.y > contentBottom)
				continue;
			const bool focused = index == row;
			AddRoundedRectFilledPx(draw, RectMinPx(context, rowRect), RectMaxPx(context, rowRect),
				focused ? ColorWithAlpha(theme.cursorNormal, 0.22f) :
					ColorWithAlpha(theme.panelBase, 0.55f), 10.0f * context.uiScale);
			if (focused)
				AddRoundedRectStrokePx(draw, RectMinPx(context, rowRect), RectMaxPx(context, rowRect),
					ColorWithAlpha(theme.cursorNormal, 0.92f), 10.0f * context.uiScale,
					2.0f * context.uiScale);
			if (index >= static_cast<int>(Row::Search))
			{
				const std::string_view label = RowLabel(index);
				const ImVec2 size = MeasureText(context, font, 17.0f, label);
				DrawText(draw, context, font, 17.0f,
					{ rowRect.x + (rowRect.w - size.x) * 0.5f,
					  rowRect.y + (rowRect.h - size.y) * 0.5f },
					Color(theme.textPrimary), label);
			}
			else
			{
				DrawText(draw, context, font, 13.0f,
					{ rowRect.x + 12.0f, rowRect.y + 7.0f },
					Color(theme.textSecondary), RowLabel(index));
				if (index == static_cast<int>(Row::Rating))
					DrawStars(context, draw, { rowRect.x + 10.0f, rowRect.y + 17.0f,
						rowRect.w - 20.0f, rowRect.h - 19.0f }, theme,
						ratingHalfSteps);
				else
				{
					auto [buffer, bufferSize] = BufferForRow(index);
					const UnitRect inputRect{ rowRect.x + 10.0f, rowRect.y + 23.0f,
						rowRect.w - 20.0f, std::max(26.0f, rowRect.h - 29.0f) };
					const bool requestFocus = requestTextFocus == index;
					ImGui::SetCursorScreenPos(RectMinPx(context, inputRect));
					ImGui::SetNextItemWidth(inputRect.w * context.uiScale);
					if (requestFocus)
						ImGui::SetKeyboardFocusHere();
					ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 7.0f * context.uiScale);
					ImGui::PushStyleColor(ImGuiCol_FrameBg, ColorWithAlpha(theme.panelBase, 0.30f));
					ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ColorWithAlpha(theme.panelHighlight, 0.42f));
					ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ColorWithAlpha(theme.panelHighlight, 0.52f));
					const std::string widget = "##metadata-field-" + std::to_string(index);
					if (index == static_cast<int>(Row::Description))
					{
						ImGuiInputTextFlags descriptionFlags = 0;
#if IMGUI_VERSION_NUM >= 19200
						descriptionFlags |= ImGuiInputTextFlags_WordWrap;
#else
						descriptionFlags |= ImGuiInputTextFlags_NoHorizontalScroll;
#endif
						(void)NativeInputTextMultiline(widget.c_str(), buffer,
							bufferSize, { inputRect.w * context.uiScale,
								inputRect.h * context.uiScale }, descriptionFlags,
							TextInputScope::Text, false);
					}
					else
					{
						(void)NativeInputText(widget.c_str(), buffer, bufferSize,
							0, TextInputScope::Text, false);
					}
					if (requestFocus)
					{
						RequestNativeTextInput(buffer, bufferSize, TextInputScope::Text);
						requestTextFocus = -1;
					}
					ImGui::PopStyleColor(3);
					ImGui::PopStyleVar();
				}
			}
			if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && rowRect.Contains(pointer))
			{
				row = index;
				event = ActivateRow(index);
				if (event != MetadataEditorEvent::None || view != MetadataEditorView::Editor)
					break;
			}
		}
		ImGui::PopClipRect();
		draw->PopClipRect();
		return event;
	}

	void DrawPlaceholder(const ShellLayoutContext& context, ImDrawList* draw,
		const UnitRect& bounds, const ShellTheme& theme)
	{
		AddRoundedRectFilledPx(draw, RectMinPx(context, bounds), RectMaxPx(context, bounds),
			ColorWithAlpha(theme.textSecondary, 0.08f), 10.0f * context.uiScale);
	}

	MetadataEditorEvent DrawBrowser(const ShellLayoutContext& context,
		ImDrawList* draw, const UnitRect& panel, const ShellTheme& theme, ImFont* font)
	{
		const UnitRect queryRect{ panel.x + 20.0f, panel.y + 84.0f,
			panel.w - 40.0f, 54.0f };
		AddRoundedRectFilledPx(draw, RectMinPx(context, queryRect), RectMaxPx(context, queryRect),
			ColorWithAlpha(queryFocused ? theme.cursorNormal : theme.panelBase,
				queryFocused ? 0.20f : 0.58f), 11.0f * context.uiScale);
		if (queryFocused)
			AddRoundedRectStrokePx(draw, RectMinPx(context, queryRect), RectMaxPx(context, queryRect),
				ColorWithAlpha(theme.cursorNormal, 0.92f), 11.0f * context.uiScale,
				2.0f * context.uiScale);
		ImGui::SetCursorScreenPos(ToPx(context, queryRect.x + 12.0f, queryRect.y + 9.0f));
		ImGui::SetNextItemWidth((queryRect.w - 24.0f) * context.uiScale);
		if (requestQueryFocus)
			ImGui::SetKeyboardFocusHere();
		ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.0f * context.uiScale);
		ImGui::PushStyleColor(ImGuiCol_FrameBg, ColorWithAlpha(theme.panelBase, 0.20f));
		ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ColorWithAlpha(theme.panelHighlight, 0.35f));
		ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ColorWithAlpha(theme.panelHighlight, 0.45f));
		(void)NativeInputText("##screenscraper-match-query", query.data(), query.size(),
			0, TextInputScope::Text, false);
		if (requestQueryFocus)
		{
			RequestNativeTextInput(query.data(), query.size(), TextInputScope::Text);
			requestQueryFocus = false;
		}
		const bool queryInputActive = IsNativeTextInputActive();
		if (queryInputWasActive && !queryInputActive)
		{
			queryFocused = false;
			requestQueryFocus = false;
			ClearRetainedTextNavigationFocus();
		}
		else if (!queryFocused && ImGui::IsItemFocused())
			ClearRetainedTextNavigationFocus();
		queryInputWasActive = queryInputActive;
		ImGui::PopStyleColor(3);
		ImGui::PopStyleVar();
		if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
			queryRect.Contains(UnitPointFromPx(context, ImGui::GetIO().MousePos)))
			queryFocused = true;

		ServiceDebounce();
		SyncSnapshotSelection();
		auto snapshot = service.GetSearchSnapshot();
		const bool current = generation != 0 && snapshot.generation == generation;
		if (!current)
			snapshot = {};
		if (snapshot.state == ScreenScraperSearchState::Ready && !snapshot.matches.empty())
			selected = std::clamp(selected, 0, static_cast<int>(snapshot.matches.size()) - 1);
		else
			selected = 0;

		const float contentTop = queryRect.bottom() + 14.0f;
		const float contentBottom = panel.bottom() - 18.0f;
		const float contentHeight = std::max(100.0f, contentBottom - contentTop);
		const float listWidth = std::clamp(panel.w * 0.42f, 380.0f, 540.0f);
		const UnitRect listRect{ panel.x + 20.0f, contentTop, listWidth, contentHeight };
		const UnitRect detailsRect{ listRect.right() + 14.0f, contentTop,
			panel.right() - 34.0f - listRect.right(), contentHeight };
		DrawGlassPanel(draw, context, listRect, theme, 15.0f, 0.72f);
		DrawGlassPanel(draw, context, detailsRect, theme, 15.0f, 0.72f);
		if (snapshot.state != ScreenScraperSearchState::Ready || snapshot.matches.empty())
		{
			std::string status = NonSpaceCount(query.data()) < 2 ?
				Translate("Enter at least two characters") : (current ? snapshot.status : Translate("Waiting to search..."));
			if (status.empty())
				status = Translate("Searching ScreenScraper...");
			const ImVec2 size = MeasureText(context, font, 16.0f, status);
			DrawText(draw, context, font, 16.0f,
				{ listRect.x + (listRect.w - size.x) * 0.5f,
				  listRect.y + (listRect.h - size.y) * 0.5f },
				Color(theme.textSecondary), status);
			return MetadataEditorEvent::None;
		}

		constexpr float rowHeight = 92.0f;
		constexpr float padding = 10.0f;
		const float visibleHeight = listRect.h - padding * 2.0f;
		const float selectedTop = selected * rowHeight;
		if (selectedTop < listScroll)
			listScroll = selectedTop;
		else if (selectedTop + rowHeight > listScroll + visibleHeight)
			listScroll = selectedTop + rowHeight - visibleHeight;
		listScroll = std::clamp(listScroll, 0.0f,
			std::max(0.0f, snapshot.matches.size() * rowHeight - visibleHeight));
		const ImVec2 pointer = UnitPointFromPx(context, ImGui::GetIO().MousePos);
		draw->PushClipRect(ToPx(context, listRect.x + padding, listRect.y + padding),
			ToPx(context, listRect.right() - padding, listRect.bottom() - padding), true);
		for (int index = 0; index < static_cast<int>(snapshot.matches.size()); ++index)
		{
			const UnitRect entry{ listRect.x + padding,
				listRect.y + padding + index * rowHeight - listScroll,
				listRect.w - padding * 2.0f, rowHeight - 6.0f };
			if (entry.bottom() < listRect.y + padding || entry.y > listRect.bottom() - padding)
				continue;
			const bool focused = !queryFocused && index == selected;
			AddRoundedRectFilledPx(draw, RectMinPx(context, entry), RectMaxPx(context, entry),
				focused ? ColorWithAlpha(theme.cursorNormal, 0.22f) :
					ColorWithAlpha(theme.panelBase, 0.46f), 10.0f * context.uiScale);
			if (focused)
				AddRoundedRectStrokePx(draw, RectMinPx(context, entry), RectMaxPx(context, entry),
					ColorWithAlpha(theme.cursorNormal, 0.94f), 10.0f * context.uiScale,
					2.0f * context.uiScale);
			const auto& match = snapshot.matches[index];
			const auto artwork = index == selected && temporaryArtwork ?
				*temporaryArtwork : options.listArtwork;
			const float scale = focused ? 1.20f : 1.0f;
			const UnitRect artworkBounds{ entry.x + 8.0f,
				entry.y + (entry.h - 66.0f * scale) * 0.5f, 94.0f * scale, 66.0f * scale };
			if (const TextureHandle texture = SearchTexture(snapshot, match, artwork, 128))
				DrawTextureContained(draw, context, texture, artworkBounds, 8.0f);
			else
				DrawPlaceholder(context, draw, artworkBounds, theme);
			const float textX = entry.x + 112.0f;
			DrawText(draw, context, font, 16.0f, { textX, entry.y + 14.0f },
				Color(theme.textPrimary), Ellipsize(context, font, 16.0f, match.title,
					entry.right() - textX - 8.0f));
			DrawText(draw, context, font, 13.0f, { textX, entry.y + 40.0f },
				Color(theme.textSecondary), Ellipsize(context, font, 13.0f, match.platform,
					entry.right() - textX - 8.0f));
			const std::string releaseDateText = FormatMetadataReleaseDateUk(
				match.metadata.releaseDate);
			DrawText(draw, context, font, 12.0f, { textX, entry.y + 61.0f },
				ColorWithAlpha(theme.textSecondary, 0.82f), releaseDateText);
			if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && entry.Contains(pointer))
			{
				selected = index;
				queryFocused = false;
				selectedGameId = match.gameId;
				detailsScroll = 0.0f;
				temporaryArtwork.reset();
			}
		}
		draw->PopClipRect();

		const auto& match = snapshot.matches[selected];
		const auto artwork = temporaryArtwork.value_or(options.detailArtwork);
		const UnitRect artworkRect{ detailsRect.x + 14.0f, detailsRect.y + 14.0f,
			detailsRect.w - 28.0f, std::clamp(detailsRect.h * 0.38f, 170.0f, 270.0f) };
		if (const TextureHandle texture = SearchTexture(snapshot, match, artwork, 480))
			DrawTextureContained(draw, context, texture, artworkRect, 11.0f);
		else
			DrawPlaceholder(context, draw, artworkRect, theme);
		const std::string preview = std::string(Translate("Preview:")) + " " + std::string(ScreenScraperArtworkLabel(artwork));
		DrawText(draw, context, font, 12.0f,
			{ artworkRect.x + 4.0f, artworkRect.bottom() - 17.0f },
			ColorWithAlpha(theme.textSecondary, 0.86f), preview);

		const UnitRect useButton{ detailsRect.x + 14.0f, detailsRect.bottom() - 58.0f,
			detailsRect.w - 28.0f, 44.0f };
		AddRoundedRectFilledPx(draw, RectMinPx(context, useButton), RectMaxPx(context, useButton),
			ColorWithAlpha(theme.cursorNormal, 0.28f), 10.0f * context.uiScale);
		AddRoundedRectStrokePx(draw, RectMinPx(context, useButton), RectMaxPx(context, useButton),
			ColorWithAlpha(theme.cursorNormal, 0.88f), 10.0f * context.uiScale,
			1.5f * context.uiScale);
		constexpr std::string_view useLabel = "Use This Entry";
		const ImVec2 useSize = MeasureText(context, font, 16.0f, useLabel);
		DrawText(draw, context, font, 16.0f,
			{ useButton.x + (useButton.w - useSize.x) * 0.5f,
			  useButton.y + (useButton.h - useSize.y) * 0.5f },
			Color(theme.textPrimary), useLabel);
		if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && useButton.Contains(pointer))
		{
			ApplyMatch(match);
			return MetadataEditorEvent::None;
		}

		const float textTop = artworkRect.bottom() + 10.0f;
		const float textBottom = useButton.y - 8.0f;
		const float textWidth = detailsRect.w - 36.0f;
		draw->PushClipRect(ToPx(context, detailsRect.x + 12.0f, textTop),
			ToPx(context, detailsRect.right() - 12.0f, textBottom), true);
		float y = textTop - detailsScroll;
		const float x = detailsRect.x + 18.0f;
		y += DrawWrappedText(draw, context, font, 20.0f, { x, y }, textWidth,
			Color(theme.textPrimary), match.title) + 8.0f;
		DrawStars(context, draw, { x, y, 150.0f, 32.0f }, theme,
			MetadataRatingHalfSteps(match.metadata.rating));
		y += 38.0f;
		if (!match.metadata.description.empty())
			y += DrawWrappedText(draw, context, font, 14.0f, { x, y }, textWidth,
				Color(theme.textSecondary), match.metadata.description) + 12.0f;
		const auto field = [&](std::string_view label, std::string_view value) {
			if (value.empty())
				return;
			const std::string line = std::string(label) + ": " + std::string(value);
			y += DrawWrappedText(draw, context, font, 13.0f, { x, y }, textWidth,
				Color(theme.textSecondary), line) + 7.0f;
		};
		field("Publisher", match.metadata.publisher);
		field("Developer", match.metadata.developer);
		field("Genre", match.metadata.genre);
		field("Release date", FormatMetadataReleaseDateUk(match.metadata.releaseDate));
		field("Players", match.metadata.players);
		field("Content rating", match.metadata.contentRating);
		field("Modes", match.metadata.modes);
		field("Themes", match.metadata.themes);
		field("Platform", match.platform);
		draw->PopClipRect();
		const float detailsHeight = y - (textTop - detailsScroll);
		detailsScroll = std::clamp(detailsScroll, 0.0f,
			std::max(0.0f, detailsHeight - (textBottom - textTop)));
		return MetadataEditorEvent::None;
	}

	IScreenScraperService& service;
	MetadataEditorItem item;
	MetadataEditorOptions options;
	MetadataEditorCallbacks callbacks;
	bool open = false;
	MetadataEditorView view = MetadataEditorView::Editor;
	std::array<char, 512> title{};
	std::array<char, 8192> description{};
	std::array<char, 64> releaseDate{};
	std::array<char, 256> developer{};
	std::array<char, 256> publisher{};
	std::array<char, 256> genre{};
	std::array<char, 64> players{};
	int ratingHalfSteps = 0;
	int row = 0;
	int lastRevealedRow = -1;
	int requestTextFocus = -1;
	float scroll = 0.0f;
	bool pendingMatch = false;
	std::optional<std::filesystem::path> pendingListArtwork;
	std::optional<std::filesystem::path> pendingDetailArtwork;
	std::array<char, 512> query{};
	std::string observedQuery;
	std::string submittedQuery;
	std::chrono::steady_clock::time_point changedAt{};
	std::uint64_t generation = 0;
	std::uint64_t displayedGeneration = 0;
	std::uint64_t selectedGameId = 0;
	int selected = 0;
	float listScroll = 0.0f;
	float detailsScroll = 0.0f;
	bool queryFocused = true;
	bool requestQueryFocus = false;
	bool queryInputWasActive = false;
	std::optional<ScreenScraperArtwork> temporaryArtwork;
};

MetadataEditor::MetadataEditor(IScreenScraperService& service)
	: m_impl(std::make_unique<Impl>(service))
{
}

MetadataEditor::~MetadataEditor() = default;
MetadataEditor::MetadataEditor(MetadataEditor&&) noexcept = default;
MetadataEditor& MetadataEditor::operator=(MetadataEditor&&) noexcept = default;

void MetadataEditor::Open(MetadataEditorItem item, MetadataEditorOptions options,
	MetadataEditorCallbacks callbacks)
{
	m_impl->Open(std::move(item), std::move(options), std::move(callbacks));
}

void MetadataEditor::Close()
{
	m_impl->Cancel();
}

bool MetadataEditor::IsOpen() const noexcept
{
	return m_impl->open;
}

MetadataEditorView MetadataEditor::View() const noexcept
{
	return m_impl->view;
}

std::string_view MetadataEditor::Heading() const noexcept
{
	return m_impl->view == MetadataEditorView::MatchBrowser ?
		"ScreenScraper Match Browser" : "Edit Metadata";
}

MetadataEditorEvent MetadataEditor::HandleInput(const MetadataEditorInput& input)
{
	return m_impl->HandleInput(input);
}

MetadataEditorEvent MetadataEditor::Draw(const ShellLayoutContext& context,
	ImDrawList* draw, const UnitRect& panel, const ShellTheme& theme, ImFont* font)
{
	return m_impl->Draw(context, draw, panel, theme,
		ResolveFrontendTextFont(font));
}
}
