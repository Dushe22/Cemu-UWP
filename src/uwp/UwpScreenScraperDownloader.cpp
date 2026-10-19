#include "UwpScreenScraperDownloader.h"

#include "Cemu/Logging/CemuLogging.h"
#include "UwpBuildConfig.h"
#include "UwpImGuiFrontend/ScreenScraper.h"
#include "config/ActiveSettings.h"
#include "config/CemuConfig.h"

#include <algorithm>
#include <memory>
#include <utility>

namespace CemuUwp::ScreenScraperDownloader
{
namespace
{
constexpr std::uint32_t kWiiUSystemId = 18;

UwpImGuiFrontend::IScreenScraperService& SharedService()
{
	static auto service = UwpImGuiFrontend::CreateScreenScraperService(
		[](UwpImGuiFrontend::LogLevel level, std::string_view message) {
			if (level == UwpImGuiFrontend::LogLevel::Debug)
				cemuLog_logDebug(LogType::Force, "{}", message);
			else
				cemuLog_log(LogType::Force, "{}", message);
		});
	return *service;
}

UwpImGuiFrontend::ScreenScraperDeveloperIdentity DeveloperIdentity()
{
	return {
		.id = CEMU_UWP_SCREENSCRAPER_DEV_ID,
		.password = CEMU_UWP_SCREENSCRAPER_DEV_PASSWORD,
		.softwareName = "Cemu-UWP",
	};
}

std::filesystem::path ProgressFile()
{
	return ActiveSettings::GetUserDataPath("titleImages/scan_progress.json");
}

std::filesystem::path MediaDirectory(std::uint64_t titleId)
{
	return ActiveSettings::GetUserDataPath("titleImages/{:016x}", titleId);
}

std::filesystem::path SearchPreviewDirectory()
{
	return ActiveSettings::GetUserDataPath("titleImages/.search");
}

bool IsPathWithin(const std::filesystem::path& path,
	const std::filesystem::path& root)
{
	const auto relative = path.lexically_normal().lexically_relative(
		root.lexically_normal());
	if (relative.empty() || relative.is_absolute() || relative.has_root_path())
		return false;
	for (const auto& component : relative)
	{
		if (component == "..")
			return false;
	}
	return true;
}

std::filesystem::path FindContentRoot(const std::filesystem::path& contentPath)
{
	std::filesystem::path best;
	for (const std::string& configured : GetConfig().game_paths)
	{
		const std::filesystem::path candidate = _utf8ToPath(configured);
		if (!candidate.empty() && IsPathWithin(contentPath, candidate) &&
			candidate.native().size() > best.native().size())
		{
			best = candidate;
		}
	}
	return best;
}

UwpImGuiFrontend::ScreenScraperRequest AdaptRequest(Request request)
{
	UwpImGuiFrontend::ScreenScraperRequest adapted;
	adapted.developer = DeveloperIdentity();
	adapted.account = { std::move(request.user), std::move(request.password) };
	adapted.options.downloadMetadata = request.downloadMetadata;
	adapted.options.downloadBoxArt = request.downloadBoxArt;
	adapted.options.downloadFanArt = request.downloadFanart;
	adapted.options.downloadVideos = request.downloadVideos;
	adapted.options.downloadScreenshots = request.downloadScreenshots;
	adapted.options.downloadLogos = request.downloadLogos;
	adapted.options.overwriteExisting = request.overwriteExisting;
	adapted.options.preferredRegion = static_cast<UwpImGuiFrontend::ScreenScraperRegion>(
		std::clamp<sint32>(request.preferredRegion, 0, 4));
	adapted.options.preferredLanguage = request.metadataLanguage;
	adapted.platformSystemId = kWiiUSystemId;
	adapted.platformMediaDirectory = ActiveSettings::GetUserDataPath("platformImages/wiiu");
	adapted.progressFile = ProgressFile();
	adapted.targets.reserve(request.titles.size());
	for (auto& title : request.titles)
	{
		auto mediaLayout = MakeTitleMediaLayout(title.path);
		adapted.targets.push_back({
			.id = title.titleId,
			.systemId = kWiiUSystemId,
			.matchedGameId = title.matchedGameId,
			.name = std::move(title.name),
			.region = std::move(title.region),
			.contentPath = std::move(title.path),
			.mediaDirectory = MediaDirectory(title.titleId),
			.sharedMedia = std::move(mediaLayout),
		});
	}
	return adapted;
}

State AdaptState(UwpImGuiFrontend::ScreenScraperState state)
{
	switch (state)
	{
	case UwpImGuiFrontend::ScreenScraperState::Working: return State::Working;
	case UwpImGuiFrontend::ScreenScraperState::Finished: return State::Finished;
	case UwpImGuiFrontend::ScreenScraperState::UpToDate: return State::UpToDate;
	case UwpImGuiFrontend::ScreenScraperState::Error: return State::Error;
	case UwpImGuiFrontend::ScreenScraperState::QuotaPaused: return State::QuotaPaused;
	case UwpImGuiFrontend::ScreenScraperState::Idle:
	default: return State::Idle;
	}
}

UwpImGuiFrontend::ScreenScraperSearchRequest AdaptSearchRequest(
	std::string user, std::string password, sint32 preferredRegion,
	std::string regionHint, std::string metadataLanguage)
{
	UwpImGuiFrontend::ScreenScraperSearchRequest request;
	request.developer = DeveloperIdentity();
	request.account = { std::move(user), std::move(password) };
	request.preferredRegion = static_cast<UwpImGuiFrontend::ScreenScraperRegion>(
		std::clamp<sint32>(preferredRegion, 0, 4));
	request.systemId = kWiiUSystemId;
	request.platformName = "Nintendo Wii U";
	request.regionHint = std::move(regionHint);
	request.preferredLanguage = std::move(metadataLanguage);
	request.previewDirectory = SearchPreviewDirectory();
	return request;
}
}

UwpImGuiFrontend::IScreenScraperService& GetService()
{
	return SharedService();
}

std::filesystem::path GetConfiguredMediaRoot()
{
	const std::string& configured =
		GetConfig().uwp_screenscraper.media_directory.GetValue();
	if (!configured.empty())
		return _utf8ToPath(configured);
	return L"E:\\ES-DE\\downloaded_media";
}

UwpImGuiFrontend::SharedMediaLayout MakeTitleMediaLayout(
	const std::filesystem::path& contentPath)
{
	return UwpImGuiFrontend::MakeSharedMediaLayout(GetConfiguredMediaRoot(),
		"wiiu", contentPath, FindContentRoot(contentPath));
}

std::optional<std::filesystem::path> FindTitleArtwork(
	const std::filesystem::path& contentPath,
	UwpImGuiFrontend::ScreenScraperArtwork artwork)
{
	return UwpImGuiFrontend::FindExistingSharedMediaArtworkPath(
		MakeTitleMediaLayout(contentPath), artwork);
}

std::optional<std::filesystem::path> FindTitleVideo(
	const std::filesystem::path& contentPath)
{
	return UwpImGuiFrontend::FindExistingSharedMediaVideoPath(
		MakeTitleMediaLayout(contentPath));
}

UwpImGuiFrontend::ScreenScraperSearchRequest MakeSearchRequest(
	std::string user, std::string password, sint32 preferredRegion,
	std::string regionHint, std::string metadataLanguage)
{
	return AdaptSearchRequest(std::move(user), std::move(password),
		preferredRegion, std::move(regionHint), std::move(metadataLanguage));
}

State GetState()
{
	return AdaptState(SharedService().GetProgress().state);
}

std::string GetStatus()
{
	return SharedService().GetProgress().status;
}

bool Start(Request request)
{
	return SharedService().Start(AdaptRequest(std::move(request)));
}

bool ConsumeFinished()
{
	return SharedService().ConsumeFinished();
}

void RequestStop()
{
	SharedService().RequestStop();
}

bool HasPendingScan()
{
	return SharedService().HasPendingScan(ProgressFile());
}

bool ResumePendingScan(Request request)
{
	return SharedService().ResumePendingScan(AdaptRequest(std::move(request)));
}

std::optional<std::vector<std::string>> GetAvailableMediaTypes(uint64 titleId)
{
	return SharedService().GetAvailableMediaTypes(MediaDirectory(titleId));
}

}
