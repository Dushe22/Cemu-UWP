#pragma once

#include "Common/precompiled.h"
#include "UwpImGuiFrontend/ScreenScraper.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace CemuUwp::ScreenScraperDownloader
{
	UwpImGuiFrontend::IScreenScraperService& GetService();
	UwpImGuiFrontend::ScreenScraperSearchRequest MakeSearchRequest(
		std::string user, std::string password, sint32 preferredRegion,
		std::string regionHint = {}, std::string metadataLanguage = {});
	std::filesystem::path GetConfiguredMediaRoot();
	UwpImGuiFrontend::SharedMediaLayout MakeTitleMediaLayout(
		const std::filesystem::path& contentPath);
	std::optional<std::filesystem::path> FindTitleArtwork(
		const std::filesystem::path& contentPath,
		UwpImGuiFrontend::ScreenScraperArtwork artwork);
	std::optional<std::filesystem::path> FindTitleVideo(
		const std::filesystem::path& contentPath);

	enum class State
	{
		Idle,
		Working,
		Finished,
		UpToDate,
		Error,
		QuotaPaused,
	};

	struct TitleRequest
	{
		uint64 titleId = 0;
		uint64 matchedGameId = 0;
		std::string name;
		std::string region;
		std::filesystem::path path;
	};

	struct Request
	{
		std::string user;
		std::string password;
		bool downloadMetadata = true;
		bool downloadBoxArt = true;
		bool downloadFanart = true;
		bool downloadVideos = false;
		bool downloadScreenshots = true;
		bool downloadLogos = true;
		bool overwriteExisting = false;
		sint32 preferredRegion = 0;
		std::string metadataLanguage;
		std::vector<TitleRequest> titles;
	};

	State GetState();
	std::string GetStatus();

	bool Start(Request request);

	bool ConsumeFinished();

	void RequestStop();

	bool HasPendingScan();

	bool ResumePendingScan(Request request);

	std::optional<std::vector<std::string>> GetAvailableMediaTypes(uint64 titleId);

}
