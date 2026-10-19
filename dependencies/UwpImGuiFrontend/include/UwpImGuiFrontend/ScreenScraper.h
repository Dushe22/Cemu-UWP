#pragma once

#include "Types.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

namespace UwpImGuiFrontend
{
enum class ScreenScraperState : std::uint8_t
{
	Idle,
	Working,
	Finished,
	UpToDate,
	Error,
	QuotaPaused,
};

enum class ScreenScraperRegion : std::uint8_t
{
	Automatic,
	UnitedStates,
	Europe,
	Japan,
	World,
};

enum class ScreenScraperArtwork : std::uint8_t
{
	Box2D,
	Box3D,
	MixRecalboxV1,
	MixRecalboxV2,
	FanArt,
	Screenshot,
	Logo,
	BackCover,
	PhysicalMedia,
	TitleScreen,
};

enum class ScreenScraperBackground : std::uint8_t
{
	Off,
	FanArt,
	Screenshot,
	Video,
	MixRecalboxV1,
	MixRecalboxV2,
};

[[nodiscard]] std::string_view ScreenScraperArtworkLabel(
	ScreenScraperArtwork artwork) noexcept;
[[nodiscard]] std::string_view ScreenScraperArtworkFileName(
	ScreenScraperArtwork artwork) noexcept;
[[nodiscard]] MediaKind ScreenScraperArtworkMediaKind(
	ScreenScraperArtwork artwork) noexcept;

// shared media library layout: <root>/<system>/<type>/<ROM-relative stem>.<ext>
struct SharedMediaLayout
{
	std::filesystem::path rootDirectory;
	std::string systemName;
	std::filesystem::path relativeGamePath;
};

[[nodiscard]] SharedMediaLayout MakeSharedMediaLayout(
	std::filesystem::path rootDirectory, std::string systemName,
	const std::filesystem::path& contentPath,
	const std::filesystem::path& contentRoot = {});
[[nodiscard]] std::filesystem::path ResolveSharedMediaArtworkPath(
	const SharedMediaLayout& layout, ScreenScraperArtwork artwork,
	std::string_view extension = ".png");
[[nodiscard]] std::filesystem::path ResolveSharedMediaVideoPath(
	const SharedMediaLayout& layout, std::string_view extension = ".mp4");
[[nodiscard]] std::optional<std::filesystem::path> FindExistingSharedMediaArtworkPath(
	const SharedMediaLayout& layout, ScreenScraperArtwork artwork);
[[nodiscard]] std::optional<std::filesystem::path> FindExistingSharedMediaVideoPath(
	const SharedMediaLayout& layout);

// the API "note" is on a 0-20 scale
[[nodiscard]] std::string NormalizeScreenScraperApiRating(
	std::string_view value);

[[nodiscard]] std::string NormalizeScreenScraperText(std::string_view value);

[[nodiscard]] std::string ResolveScreenScraperRegionCode(
	ScreenScraperRegion preferredRegion, std::string_view automaticRegionHint = {});

struct ScreenScraperCredentials
{
	std::string user;
	std::string password;
};

struct ScreenScraperDeveloperIdentity
{
	std::string id;
	std::string password;
	std::string softwareName;
};

struct ScreenScraperOptions
{
	bool downloadMetadata = true;
	bool downloadBoxArt = true;
	bool downloadFanArt = true;
	bool downloadVideos = false;
	bool downloadScreenshots = true;
	bool downloadLogos = true;
	bool overwriteExisting = false;
	// when unset, platform media follows overwriteExisting
	std::optional<bool> overwritePlatformMedia;
	ScreenScraperRegion preferredRegion = ScreenScraperRegion::Automatic;
	// two letter code for the text fields; empty falls back to English
	std::string preferredLanguage;
};

struct ScreenScraperTarget
{
	ItemId id = 0;
	std::uint32_t systemId = 0;
	std::uint64_t matchedGameId = 0;
	std::string name;
	std::string region;
	std::filesystem::path contentPath;
	// per-item directory for metadata.json and media.json
	std::filesystem::path mediaDirectory;
	std::optional<SharedMediaLayout> sharedMedia;
};

struct ScreenScraperRequest
{
	ScreenScraperDeveloperIdentity developer;
	ScreenScraperCredentials account;
	ScreenScraperOptions options;
	std::filesystem::path platformMediaDirectory;
	std::filesystem::path progressFile;
	std::uint32_t platformSystemId = 0;
	std::vector<ScreenScraperTarget> targets;
};

struct ScreenScraperProgress
{
	ScreenScraperState state = ScreenScraperState::Idle;
	std::size_t completedTargets = 0;
	std::size_t totalTargets = 0;
	std::size_t downloadedFiles = 0;
	std::string status;
};

struct ScreenScraperMediaReference
{
	std::string type;
	std::string parent;
	std::string region;
	std::string format;
};

struct ScreenScraperArtworkPreview
{
	ScreenScraperArtwork artwork = ScreenScraperArtwork::Box2D;
	std::uint32_t maximumHeight = 0;
	std::filesystem::path path;
};

struct ScreenScraperMatch
{
	std::uint64_t gameId = 0;
	std::uint32_t systemId = 0;
	std::string title;
	std::string platform;
	std::string region;
	Metadata metadata;
	std::vector<ScreenScraperMediaReference> media;
	std::vector<ScreenScraperArtworkPreview> previews;
};

enum class ScreenScraperSearchState : std::uint8_t
{
	Idle,
	Searching,
	Ready,
	NoMatches,
	Error,
};

struct ScreenScraperSearchRequest
{
	ScreenScraperDeveloperIdentity developer;
	ScreenScraperCredentials account;
	ScreenScraperRegion preferredRegion = ScreenScraperRegion::Automatic;
	std::uint32_t systemId = 0;
	// used only when a match omits the platform label
	std::string platformName;
	// used only when preferredRegion is Automatic
	std::string regionHint;
	std::string preferredLanguage;
	std::string query;
	std::filesystem::path previewDirectory;
};

struct ScreenScraperSearchSnapshot
{
	std::uint64_t generation = 0;
	ScreenScraperSearchState state = ScreenScraperSearchState::Idle;
	std::string query;
	std::string status;
	std::vector<ScreenScraperMatch> matches;
};

struct GameMetadataRecord
{
	ItemId itemId = 0;
	std::uint64_t sourceGameId = 0;
	std::string title;
	Metadata metadata;
};

struct ScreenScraperMediaManifest
{
	std::uint64_t gameId = 0;
	std::vector<std::string> available;
	std::vector<std::string> saved;
};

class IScreenScraperService
{
public:
	virtual ~IScreenScraperService() = default;

	[[nodiscard]] virtual ScreenScraperProgress GetProgress() const = 0;
	virtual bool Start(ScreenScraperRequest request) = 0;
	[[nodiscard]] virtual bool ConsumeFinished() = 0;
	virtual void RequestStop() = 0;
	[[nodiscard]] virtual bool HasPendingScan(const std::filesystem::path& progressFile) const = 0;
	virtual bool ResumePendingScan(ScreenScraperRequest defaults) = 0;
	[[nodiscard]] virtual std::optional<std::vector<std::string>> GetAvailableMediaTypes(
		const std::filesystem::path& mediaDirectory) const = 0;

	// a new search invalidates the previous generation and its pending previews
	virtual std::uint64_t BeginSearch(ScreenScraperSearchRequest request) = 0;
	[[nodiscard]] virtual ScreenScraperSearchSnapshot GetSearchSnapshot() const = 0;
	virtual void CancelSearch() = 0;
	virtual void RequestSearchPreview(std::uint64_t generation,
		std::uint64_t gameId, ScreenScraperArtwork artwork,
		std::uint32_t maximumHeight) = 0;
};

using ScreenScraperLog = std::function<void(LogLevel, std::string_view)>;

[[nodiscard]] Metadata LoadScreenScraperMetadata(
	const std::filesystem::path& metadataPath,
	std::string_view region = {}, std::string_view language = {});
void MergeMissingMetadata(Metadata& target, const Metadata& source);

[[nodiscard]] std::optional<GameMetadataRecord> LoadGameMetadataRecord(
	const std::filesystem::path& path);
[[nodiscard]] bool SaveGameMetadataRecordAtomic(
	const std::filesystem::path& path, const GameMetadataRecord& record);

[[nodiscard]] std::optional<ScreenScraperMediaManifest>
	LoadScreenScraperMediaManifest(const std::filesystem::path& path);

[[nodiscard]] const ScreenScraperArtworkPreview* FindPreview(
	const ScreenScraperMatch& match, ScreenScraperArtwork artwork) noexcept;

[[nodiscard]] std::unique_ptr<IScreenScraperService> CreateScreenScraperService(
	ScreenScraperLog log = {});
}
