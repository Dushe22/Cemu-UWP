#pragma once

#include "ImGuiShell.h"
#include "ScreenScraper.h"
#include "Types.h"

#include <imgui.h>

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace UwpImGuiFrontend
{
enum class MetadataEditorView : std::uint8_t
{
	Editor,
	MatchBrowser,
};

enum class MetadataEditorEvent : std::uint8_t
{
	None,
	Saved,
	Cancelled,
};

struct MetadataEditorInput
{
	bool accept = false;
	bool back = false;
	bool up = false;
	bool down = false;
	bool left = false;
	bool right = false;
	bool leftShoulder = false;
	bool rightShoulder = false;
	float rightStickY = 0.0f;
	float deltaSeconds = 0.0f;
};

struct MetadataEditorItem
{
	GameMetadataRecord record;
	std::string defaultTitle;
	std::filesystem::path listArtwork;
	std::filesystem::path detailArtwork;
};

struct MetadataEditorOptions
{
	ScreenScraperArtwork listArtwork = ScreenScraperArtwork::Box2D;
	ScreenScraperArtwork detailArtwork = ScreenScraperArtwork::Box2D;
	ScreenScraperSearchRequest searchRequest;
};

struct MetadataEditorCallbacks
{
	std::function<TextureHandle(const std::filesystem::path&)> loadTexture;
	std::function<bool(const GameMetadataRecord&, bool selectedMatch)> save;
	std::function<void(std::string_view)> notify;
};

class MetadataEditor
{
public:
	explicit MetadataEditor(IScreenScraperService& service);
	~MetadataEditor();

	MetadataEditor(const MetadataEditor&) = delete;
	MetadataEditor& operator=(const MetadataEditor&) = delete;
	MetadataEditor(MetadataEditor&&) noexcept;
	MetadataEditor& operator=(MetadataEditor&&) noexcept;

	void Open(MetadataEditorItem item, MetadataEditorOptions options,
		MetadataEditorCallbacks callbacks);
	void Close();
	[[nodiscard]] bool IsOpen() const noexcept;
	[[nodiscard]] MetadataEditorView View() const noexcept;
	[[nodiscard]] std::string_view Heading() const noexcept;

	[[nodiscard]] MetadataEditorEvent HandleInput(const MetadataEditorInput& input);
	[[nodiscard]] MetadataEditorEvent Draw(const ShellLayoutContext& context,
		ImDrawList* draw, const UnitRect& panel, const ShellTheme& theme,
		ImFont* font = nullptr);

private:
	class Impl;
	std::unique_ptr<Impl> m_impl;
};

[[nodiscard]] int MetadataRatingStars(std::string_view rating);
[[nodiscard]] std::string MetadataRatingValue(int stars);
[[nodiscard]] int MetadataRatingHalfSteps(std::string_view rating);
[[nodiscard]] std::string MetadataRatingValueFromHalfSteps(int halfSteps);
[[nodiscard]] bool ScreenScraperMatchHasArtwork(const ScreenScraperMatch& match,
	ScreenScraperArtwork artwork);
[[nodiscard]] std::optional<ScreenScraperArtwork> CycleScreenScraperMatchArtwork(
	const ScreenScraperMatch& match, ScreenScraperArtwork current, int delta);
}
