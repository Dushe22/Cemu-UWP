#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace UwpImGuiFrontend
{
using ItemId = std::uint64_t;

struct Vec2
{
	float x = 0.0f;
	float y = 0.0f;
};

struct Rect
{
	float x = 0.0f;
	float y = 0.0f;
	float width = 0.0f;
	float height = 0.0f;

	[[nodiscard]] float Right() const noexcept { return x + width; }
	[[nodiscard]] float Bottom() const noexcept { return y + height; }
	[[nodiscard]] bool Contains(Vec2 point) const noexcept
	{
		return point.x >= x && point.x <= Right() && point.y >= y && point.y <= Bottom();
	}
};

enum class MediaKind : std::uint8_t
{
	Cover2D,
	Cover3D,
	MixRecalboxV2,
	Screenshot,
	TitleScreen,
	FanArt,
	Logo,
	Video,
	BackCover,
	PhysicalMedia,
	MixRecalboxV1,
};

struct MediaAsset
{
	MediaKind kind = MediaKind::Cover2D;
	std::filesystem::path path;
};

struct Metadata
{
	std::string description;
	std::string genre;
	std::string developer;
	std::string publisher;
	std::string players;
	std::string releaseDate;
	std::string rating;
	std::string contentRating;
	std::string modes;
	std::string themes;
};

struct LibraryItem
{
	ItemId id = 0;
	std::uint64_t contentId = 0;
	std::uint32_t version = 0;
	std::string name;
	std::string platform;
	std::string region;
	std::string format;
	std::filesystem::path launchPath;
	std::filesystem::path metadataPath;
	Metadata metadata;
	std::vector<MediaAsset> media;
};

[[nodiscard]] const MediaAsset* FindMedia(const LibraryItem& item, MediaKind kind) noexcept;

enum class ThemeMediaMode : std::uint8_t
{
	None,
	DynamicBackground,
	DynamicVideo,
};

enum class ScreenLayoutMode : std::uint8_t
{
	PrimaryOnly,
	SecondaryOnly,
	SideBySide,
	Stacked,
	PrimaryWithSecondary,
	SecondaryWithPrimary,
};

enum class LogLevel : std::uint8_t
{
	Debug,
	Information,
	Warning,
	Error,
};

struct TextureHandle
{
	std::uintptr_t id = 0;
	std::uint32_t width = 0;
	std::uint32_t height = 0;

	[[nodiscard]] explicit operator bool() const noexcept { return id != 0; }
};

struct Notification
{
	std::string message;
	float durationSeconds = 3.0f;
};
}
