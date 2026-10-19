#include "UwpImGuiFrontend/ScreenScraper.h"
#include "UwpImGuiFrontend/Text.h"

#include <fmt/format.h>

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <fstream>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <fileapifromapp.h>
#endif

namespace fs = std::filesystem;

namespace UwpImGuiFrontend
{
namespace
{
std::string JsonString(const rapidjson::Value& object, const char* name)
{
	if (!object.IsObject())
		return {};
	const auto found = object.FindMember(name);
	return found != object.MemberEnd() && found->value.IsString() ?
		std::string(found->value.GetString(), found->value.GetStringLength()) :
		std::string{};
}

bool WriteTextAtomic(const fs::path& path, std::string_view text)
{
	std::error_code error;
	fs::create_directories(path.parent_path(), error);
	fs::path temporary = path;
	temporary += ".tmp";
	fs::remove(temporary, error);
	{
		std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
		if (!output)
			return false;
		output.write(text.data(), static_cast<std::streamsize>(text.size()));
		output.flush();
		if (!output)
		{
			output.close();
			fs::remove(temporary, error);
			return false;
		}
	}

#if defined(_WIN32)
	const bool targetExists = fs::exists(path, error) && !error;
	const BOOL replaced = targetExists ?
		ReplaceFileFromAppW(path.c_str(), temporary.c_str(), nullptr,
			REPLACEFILE_IGNORE_MERGE_ERRORS, nullptr, nullptr) :
		MoveFileFromAppW(temporary.c_str(), path.c_str());
	if (replaced)
		return true;
	std::error_code removeError;
	fs::remove(temporary, removeError);
	return false;
#else
	error.clear();
	fs::rename(temporary, path, error);
	if (!error)
		return true;
	std::error_code removeError;
	fs::remove(temporary, removeError);
	return false;
#endif
}

bool IsSafeRelativePath(const fs::path& path)
{
	if (path.empty() || path.is_absolute() || path.has_root_path())
		return false;
	for (const fs::path& component : path)
	{
		if (component == "..")
			return false;
	}
	return true;
}

bool IsSafePathComponent(std::string_view value)
{
	return !value.empty() && value != "." && value != ".." &&
		value.find_first_of("/\\:") == std::string_view::npos;
}

bool IsSafeExtension(std::string_view extension)
{
	return extension.size() >= 2 && extension.size() <= 16 &&
		extension.front() == '.' &&
		std::all_of(extension.begin() + 1, extension.end(), [](char value) {
			return std::isalnum(static_cast<unsigned char>(value)) != 0;
		});
}

std::string_view SharedMediaArtworkDirectory(ScreenScraperArtwork artwork) noexcept
{
	switch (artwork)
	{
	case ScreenScraperArtwork::Box2D: return "covers";
	case ScreenScraperArtwork::Box3D: return "3dboxes";
	case ScreenScraperArtwork::MixRecalboxV1: return "miximages-v1";
	case ScreenScraperArtwork::MixRecalboxV2: return "miximages";
	case ScreenScraperArtwork::FanArt: return "fanart";
	case ScreenScraperArtwork::Screenshot: return "screenshots";
	case ScreenScraperArtwork::Logo: return "marquees";
	case ScreenScraperArtwork::BackCover: return "backcovers";
	case ScreenScraperArtwork::PhysicalMedia: return "physicalmedia";
	case ScreenScraperArtwork::TitleScreen: return "titlescreens";
	}
	return "covers";
}

fs::path ResolveSharedMediaPath(const SharedMediaLayout& layout,
	std::string_view mediaDirectory, std::string_view extension)
{
	if (layout.rootDirectory.empty() ||
		!IsSafePathComponent(layout.systemName) ||
		!IsSafePathComponent(mediaDirectory) ||
		!IsSafeExtension(extension) ||
		!IsSafeRelativePath(layout.relativeGamePath))
	{
		return {};
	}

	fs::path relative = layout.relativeGamePath;
	fs::path fileName = relative.filename();
	fileName.replace_extension(fs::path(extension));
	return layout.rootDirectory / fs::path(layout.systemName) /
		fs::path(mediaDirectory) / relative.parent_path() / fileName;
}

template<std::size_t Size>
std::optional<fs::path> FindExistingSharedMediaPath(
	const SharedMediaLayout& layout, std::string_view mediaDirectory,
	const std::array<std::string_view, Size>& extensions)
{
	for (const std::string_view extension : extensions)
	{
		const fs::path candidate = ResolveSharedMediaPath(
			layout, mediaDirectory, extension);
		std::error_code error;
		if (!candidate.empty() && fs::exists(candidate, error) &&
			!fs::is_directory(candidate, error))
		{
			return candidate;
		}
	}
	return std::nullopt;
}
}

std::string NormalizeScreenScraperText(std::string_view value)
{
	return NormalizeDisplayText(value);
}

std::string_view ScreenScraperArtworkLabel(ScreenScraperArtwork artwork) noexcept
{
	switch (artwork)
	{
	case ScreenScraperArtwork::Box2D: return "2D box";
	case ScreenScraperArtwork::Box3D: return "3D box";
	case ScreenScraperArtwork::MixRecalboxV1: return "Recalbox Mix V1";
	case ScreenScraperArtwork::MixRecalboxV2: return "Recalbox Mix V2";
	case ScreenScraperArtwork::FanArt: return "Fan art";
	case ScreenScraperArtwork::Screenshot: return "Screenshot";
	case ScreenScraperArtwork::Logo: return "Game logo";
	case ScreenScraperArtwork::BackCover: return "2D box back";
	case ScreenScraperArtwork::PhysicalMedia: return "2D cartridge";
	case ScreenScraperArtwork::TitleScreen: return "Title screen";
	}
	return "2D box";
}

std::string_view ScreenScraperArtworkFileName(ScreenScraperArtwork artwork) noexcept
{
	switch (artwork)
	{
	case ScreenScraperArtwork::Box2D: return "box2d.png";
	case ScreenScraperArtwork::Box3D: return "box3d.png";
	case ScreenScraperArtwork::MixRecalboxV1: return "mixrbv1.png";
	case ScreenScraperArtwork::MixRecalboxV2: return "mixrbv2.png";
	case ScreenScraperArtwork::FanArt: return "fanart.png";
	case ScreenScraperArtwork::Screenshot: return "screenshot.png";
	case ScreenScraperArtwork::Logo: return "logo.png";
	case ScreenScraperArtwork::BackCover: return "box2d_back.png";
	case ScreenScraperArtwork::PhysicalMedia: return "support2d.png";
	case ScreenScraperArtwork::TitleScreen: return "titlescreen.png";
	}
	return "box2d.png";
}

MediaKind ScreenScraperArtworkMediaKind(ScreenScraperArtwork artwork) noexcept
{
	switch (artwork)
	{
	case ScreenScraperArtwork::Box2D: return MediaKind::Cover2D;
	case ScreenScraperArtwork::Box3D: return MediaKind::Cover3D;
	case ScreenScraperArtwork::MixRecalboxV1: return MediaKind::MixRecalboxV1;
	case ScreenScraperArtwork::MixRecalboxV2: return MediaKind::MixRecalboxV2;
	case ScreenScraperArtwork::FanArt: return MediaKind::FanArt;
	case ScreenScraperArtwork::Screenshot: return MediaKind::Screenshot;
	case ScreenScraperArtwork::Logo: return MediaKind::Logo;
	case ScreenScraperArtwork::BackCover: return MediaKind::BackCover;
	case ScreenScraperArtwork::PhysicalMedia: return MediaKind::PhysicalMedia;
	case ScreenScraperArtwork::TitleScreen: return MediaKind::TitleScreen;
	}
	return MediaKind::Cover2D;
}

SharedMediaLayout MakeSharedMediaLayout(fs::path rootDirectory,
	std::string systemName, const fs::path& contentPath,
	const fs::path& contentRoot)
{
	SharedMediaLayout layout{
		.rootDirectory = std::move(rootDirectory),
		.systemName = std::move(systemName),
	};

	if (!contentRoot.empty())
	{
		const fs::path relative = contentPath.lexically_normal().lexically_relative(
			contentRoot.lexically_normal());
		if (IsSafeRelativePath(relative))
			layout.relativeGamePath = relative;
	}
	if (layout.relativeGamePath.empty())
		layout.relativeGamePath = contentPath.filename();
	return layout;
}

fs::path ResolveSharedMediaArtworkPath(const SharedMediaLayout& layout,
	ScreenScraperArtwork artwork, std::string_view extension)
{
	return ResolveSharedMediaPath(layout, SharedMediaArtworkDirectory(artwork), extension);
}

fs::path ResolveSharedMediaVideoPath(const SharedMediaLayout& layout,
	std::string_view extension)
{
	return ResolveSharedMediaPath(layout, "videos", extension);
}

std::optional<fs::path> FindExistingSharedMediaArtworkPath(
	const SharedMediaLayout& layout, ScreenScraperArtwork artwork)
{
	static constexpr std::array<std::string_view, 6> extensions{
		".png", ".jpg", ".jpeg", ".webp", ".bmp", ".gif"
	};
	return FindExistingSharedMediaPath(layout, SharedMediaArtworkDirectory(artwork),
		extensions);
}

std::optional<fs::path> FindExistingSharedMediaVideoPath(
	const SharedMediaLayout& layout)
{
	static constexpr std::array<std::string_view, 9> extensions{
		".mp4", ".mkv", ".avi", ".mov", ".wmv", ".m4v", ".webm", ".mpg", ".3gp"
	};
	return FindExistingSharedMediaPath(layout, "videos", extensions);
}

std::string NormalizeScreenScraperApiRating(std::string_view value)
{
	if (value.empty())
		return {};
	const std::string text(value);
	char* end = nullptr;
	const double score = std::strtod(text.c_str(), &end);
	if (end == text.c_str())
		return {};
	while (*end != '\0' && std::isspace(static_cast<unsigned char>(*end)))
		++end;
	if (*end != '\0')
		return {};
	return fmt::format("{:.3g}", std::clamp(score / 20.0, 0.0, 1.0));
}

std::string ResolveScreenScraperRegionCode(
	ScreenScraperRegion preferredRegion, std::string_view automaticRegionHint)
{
	switch (preferredRegion)
	{
	case ScreenScraperRegion::UnitedStates: return "us";
	case ScreenScraperRegion::Europe: return "eu";
	case ScreenScraperRegion::Japan: return "jp";
	case ScreenScraperRegion::World: return "wor";
	case ScreenScraperRegion::Automatic:
	default: break;
	}

	std::string region(automaticRegionHint);
	std::transform(region.begin(), region.end(), region.begin(),
		[](unsigned char character) {
			return static_cast<char>(std::tolower(character));
		});
	if (region.find("usa") != std::string::npos ||
		region.find("america") != std::string::npos ||
		region == "us")
	{
		return "us";
	}
	if (region.find("europe") != std::string::npos ||
		region == "eur" || region == "eu")
	{
		return "eu";
	}
	if (region.find("japan") != std::string::npos ||
		region == "jpn" || region == "jp")
	{
		return "jp";
	}
	return "wor";
}

std::optional<GameMetadataRecord> LoadGameMetadataRecord(const fs::path& path)
{
	constexpr std::streamoff kMaximumBytes = 1024 * 1024;
	std::ifstream input(path, std::ios::binary | std::ios::ate);
	if (!input)
		return std::nullopt;
	const std::streamoff length = input.tellg();
	if (length <= 0 || length > kMaximumBytes)
		return std::nullopt;
	std::string json(static_cast<std::size_t>(length), '\0');
	input.seekg(0, std::ios::beg);
	if (!input.read(json.data(), static_cast<std::streamsize>(json.size())))
		return std::nullopt;

	rapidjson::Document document;
	document.Parse(json.data(), json.size());
	if (document.HasParseError() || !document.IsObject())
		return std::nullopt;

	GameMetadataRecord record;
	if (const auto id = document.FindMember("itemId"); id != document.MemberEnd() && id->value.IsUint64())
		record.itemId = id->value.GetUint64();
	if (const auto source = document.FindMember("sourceGameId"); source != document.MemberEnd() && source->value.IsUint64())
		record.sourceGameId = source->value.GetUint64();
	record.title = NormalizeScreenScraperText(JsonString(document, "title"));
	const auto metadata = document.FindMember("metadata");
	if (metadata != document.MemberEnd() && metadata->value.IsObject())
	{
		record.metadata.description = NormalizeScreenScraperText(JsonString(metadata->value, "description"));
		record.metadata.rating = JsonString(metadata->value, "rating");
		record.metadata.releaseDate = NormalizeMetadataReleaseDate(
			JsonString(metadata->value, "releaseDate"));
		record.metadata.developer = NormalizeScreenScraperText(JsonString(metadata->value, "developer"));
		record.metadata.publisher = NormalizeScreenScraperText(JsonString(metadata->value, "publisher"));
		record.metadata.genre = NormalizeScreenScraperText(JsonString(metadata->value, "genre"));
		record.metadata.players = NormalizeScreenScraperText(JsonString(metadata->value, "players"));
		record.metadata.contentRating = NormalizeScreenScraperText(JsonString(metadata->value, "contentRating"));
		record.metadata.modes = NormalizeScreenScraperText(JsonString(metadata->value, "modes"));
		record.metadata.themes = NormalizeScreenScraperText(JsonString(metadata->value, "themes"));
	}
	return record;
}

bool SaveGameMetadataRecordAtomic(const fs::path& path,
	const GameMetadataRecord& record)
{
	rapidjson::StringBuffer buffer;
	rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
	const auto string = [&writer](std::string_view value) {
		writer.String(value.data(), static_cast<rapidjson::SizeType>(value.size()));
	};
	writer.StartObject();
	writer.Key("version");
	writer.Uint(1);
	writer.Key("itemId");
	writer.Uint64(record.itemId);
	writer.Key("sourceGameId");
	writer.Uint64(record.sourceGameId);
	writer.Key("title");
	string(record.title);
	writer.Key("metadata");
	writer.StartObject();
	writer.Key("description"); string(record.metadata.description);
	writer.Key("rating"); string(record.metadata.rating);
	writer.Key("releaseDate"); string(record.metadata.releaseDate);
	writer.Key("developer"); string(record.metadata.developer);
	writer.Key("publisher"); string(record.metadata.publisher);
	writer.Key("genre"); string(record.metadata.genre);
	writer.Key("players"); string(record.metadata.players);
	writer.Key("contentRating"); string(record.metadata.contentRating);
	writer.Key("modes"); string(record.metadata.modes);
	writer.Key("themes"); string(record.metadata.themes);
	writer.EndObject();
	writer.EndObject();
	return WriteTextAtomic(path,
		std::string_view(buffer.GetString(), buffer.GetSize()));
}

std::optional<ScreenScraperMediaManifest> LoadScreenScraperMediaManifest(
	const fs::path& path)
{
	constexpr std::streamoff kMaximumBytes = 1024 * 1024;
	std::ifstream input(path, std::ios::binary | std::ios::ate);
	if (!input)
		return std::nullopt;
	const std::streamoff length = input.tellg();
	if (length <= 0 || length > kMaximumBytes)
		return std::nullopt;
	std::string json(static_cast<std::size_t>(length), '\0');
	input.seekg(0, std::ios::beg);
	if (!input.read(json.data(), static_cast<std::streamsize>(json.size())))
		return std::nullopt;

	rapidjson::Document document;
	document.Parse(json.data(), json.size());
	if (document.HasParseError() || !document.IsObject())
		return std::nullopt;

	ScreenScraperMediaManifest manifest;
	if (const auto game = document.FindMember("gameId");
		game != document.MemberEnd() && game->value.IsUint64())
	{
		manifest.gameId = game->value.GetUint64();
	}
	const auto strings = [&document](const char* name) {
		std::vector<std::string> values;
		const auto member = document.FindMember(name);
		if (member == document.MemberEnd() || !member->value.IsArray())
			return values;
		for (const auto& value : member->value.GetArray())
		{
			if (value.IsString())
				values.emplace_back(value.GetString(), value.GetStringLength());
		}
		return values;
	};
	manifest.available = strings("available");
	manifest.saved = strings("saved");
	return manifest;
}

const ScreenScraperArtworkPreview* FindPreview(const ScreenScraperMatch& match,
	ScreenScraperArtwork artwork) noexcept
{
	const ScreenScraperArtworkPreview* best = nullptr;
	for (const auto& preview : match.previews)
	{
		if (preview.artwork == artwork &&
			(!best || preview.maximumHeight > best->maximumHeight))
		{
			best = &preview;
		}
	}
	return best;
}
}
