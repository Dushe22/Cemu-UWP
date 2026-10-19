#include "UwpCompanionService.h"

#include "Cafe/TitleList/TitleId.h"
#include "Cafe/TitleList/TitleInfo.h"
#include "Cafe/TitleList/TitleList.h"
#include "Cemu/Logging/CemuLogging.h"
#include "UwpImGuiFrontend/DsuCompanion.h"
#include "UwpImGuiFrontend/ScreenScraper.h"
#include "config/ActiveSettings.h"
#include "config/CemuConfig.h"
#include "input/api/DSU/DSUControllerProvider.h"
#include "uwp/UwpMediaStreamer.h"
#include "uwp/UwpScreenScraperDownloader.h"
#include "util/helpers/helpers.h"

#include <algorithm>
#include <cctype>
#include <fmt/format.h>
#include <atomic>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

namespace CemuUwp::CompanionService
{
namespace
{
bool IsLaunchableTitle(TitleIdParser::TITLE_TYPE type)
{
	return type == TitleIdParser::TITLE_TYPE::BASE_TITLE ||
		type == TitleIdParser::TITLE_TYPE::BASE_TITLE_DEMO ||
		type == TitleIdParser::TITLE_TYPE::HOMEBREW;
}

std::string RegionName(CafeConsoleRegion region)
{
	switch (region)
	{
	case CafeConsoleRegion::JPN: return "Japan";
	case CafeConsoleRegion::USA: return "USA";
	case CafeConsoleRegion::EUR: return "Europe";
	case CafeConsoleRegion::AUS_DEPR: return "Australia";
	case CafeConsoleRegion::CHN: return "China";
	case CafeConsoleRegion::KOR: return "Korea";
	case CafeConsoleRegion::TWN: return "Taiwan";
	default: return "Mixed";
	}
}

std::string FormatName(const fs::path& path, TitleInfo::TitleDataFormat format)
{
	std::string extension = _pathToUtf8(path.extension());
	std::transform(extension.begin(), extension.end(), extension.begin(),
		[](unsigned char value) { return static_cast<char>(std::toupper(value)); });
	if (!extension.empty())
		return extension.substr(1);
	switch (format)
	{
	case TitleInfo::TitleDataFormat::HOST_FS: return "Extracted";
	case TitleInfo::TitleDataFormat::WUD: return "WUD/WUX";
	case TitleInfo::TitleDataFormat::WIIU_ARCHIVE: return "WUA";
	case TitleInfo::TitleDataFormat::NUS: return "NUS";
	case TitleInfo::TitleDataFormat::WUHB: return "WUHB";
	default: return "Wii U";
	}
}

bool IsRegularFile(const fs::path& path)
{
	std::error_code error;
	return fs::is_regular_file(path, error) && !error;
}

void AddMedia(std::vector<UwpImGuiFrontend::MediaAsset>& media,
	UwpImGuiFrontend::MediaKind kind, const fs::path& path)
{
	if (IsRegularFile(path))
		media.push_back({ kind, path });
}

void AddMedia(UwpImGuiFrontend::LibraryItem& item,
	UwpImGuiFrontend::MediaKind kind, const fs::path& path)
{
	AddMedia(item.media, kind, path);
}

// downloaded artwork lives in the shared media tree, the per-title folder is the older layout
void AddTitleArtwork(UwpImGuiFrontend::LibraryItem& item, UwpImGuiFrontend::MediaKind kind,
	UwpImGuiFrontend::ScreenScraperArtwork artwork, const fs::path& contentPath,
	const fs::path& legacyPath)
{
	if (const auto shared = CemuUwp::ScreenScraperDownloader::FindTitleArtwork(contentPath, artwork))
	{
		AddMedia(item, kind, *shared);
		return;
	}
	AddMedia(item, kind, legacyPath);
}

class CemuCompanionHost final : public UwpImGuiFrontend::ICompanionHost
{
public:
	std::vector<UwpImGuiFrontend::LibraryItem> SnapshotCatalogue() override
	{
		std::vector<UwpImGuiFrontend::LibraryItem> catalogue;
		auto titles = CafeTitleList::AcquireInternalList();
		catalogue.reserve(titles.size());
		for (TitleInfo* title : titles)
		{
			if (!title || !title->IsValid() || title->IsSystemDataTitle())
				continue;
			const uint64 titleId = title->GetAppTitleId();
			if (!IsLaunchableTitle(TitleIdParser(titleId).GetType()))
				continue;

			UwpImGuiFrontend::LibraryItem item;
			item.id = titleId != 0 ? titleId : title->GetUID();
			item.contentId = titleId;
			item.version = title->GetAppTitleVersion();
			item.name = title->GetMetaTitleName();
			if (item.name.empty())
				item.name = fmt::format("{:016x}", titleId);
			item.platform = "Nintendo - Wii U";
			item.region = RegionName(title->GetMetaRegion());
			item.format = FormatName(title->GetPath(), title->GetFormat());
			item.launchPath = title->GetPath();

			const fs::path mediaRoot = ActiveSettings::GetUserDataPath("titleImages/{:016x}", titleId);
			item.metadataPath = mediaRoot / "metadata.json";
			if (const auto metadata = UwpImGuiFrontend::LoadGameMetadataRecord(
				mediaRoot / "frontend_metadata.json"); metadata &&
				(metadata->itemId == 0 || metadata->itemId == titleId))
			{
				if (!metadata->title.empty())
					item.name = metadata->title;
				item.metadata = metadata->metadata;
				// An explicit manual record is authoritative, including empty fields.
				item.metadataPath.clear();
			}
			using Artwork = UwpImGuiFrontend::ScreenScraperArtwork;
			using Kind = UwpImGuiFrontend::MediaKind;
			const fs::path& contentPath = item.launchPath;
			AddTitleArtwork(item, Kind::Logo, Artwork::Logo, contentPath, mediaRoot / "logo.png");
			AddTitleArtwork(item, Kind::Cover2D, Artwork::Box2D, contentPath, mediaRoot / "box2d.png");
			AddTitleArtwork(item, Kind::Cover3D, Artwork::Box3D, contentPath, mediaRoot / "box3d.png");
			AddTitleArtwork(item, Kind::MixRecalboxV1, Artwork::MixRecalboxV1, contentPath, mediaRoot / "mixrbv1.png");
			AddTitleArtwork(item, Kind::MixRecalboxV2, Artwork::MixRecalboxV2, contentPath, mediaRoot / "mixrbv2.png");
			AddTitleArtwork(item, Kind::Screenshot, Artwork::Screenshot, contentPath, mediaRoot / "screenshot.png");
			AddTitleArtwork(item, Kind::FanArt, Artwork::FanArt, contentPath, mediaRoot / "fanart.png");
			AddTitleArtwork(item, Kind::BackCover, Artwork::BackCover, contentPath, mediaRoot / "box2d_back.png");
			AddTitleArtwork(item, Kind::PhysicalMedia, Artwork::PhysicalMedia, contentPath, mediaRoot / "support2d.png");
			AddTitleArtwork(item, Kind::TitleScreen, Artwork::TitleScreen, contentPath, mediaRoot / "titlescreen.png");
			if (const auto video = CemuUwp::ScreenScraperDownloader::FindTitleVideo(contentPath))
				AddMedia(item, Kind::Video, *video);
			else
				AddMedia(item, Kind::Video, mediaRoot / "video.mp4");
			catalogue.emplace_back(std::move(item));
		}
		CafeTitleList::ReleaseInternalList();
		return catalogue;
	}

	std::vector<UwpImGuiFrontend::MediaAsset> SnapshotPlatformMedia() override
	{
		std::vector<UwpImGuiFrontend::MediaAsset> media;
		const fs::path root = ActiveSettings::GetUserDataPath("platformImages/wiiu");
		for (const char* name : { "fanart", "background" })
		{
			for (const char* extension : { ".png", ".jpg", ".jpeg", ".webp" })
			{
				if (!media.empty())
					break;
				AddMedia(media, UwpImGuiFrontend::MediaKind::FanArt,
					root / (std::string(name) + extension));
			}
		}
		AddMedia(media, UwpImGuiFrontend::MediaKind::Video, root / "video.mp4");
		return media;
	}

	UwpImGuiFrontend::RunningContent GetRunningContent() const override
	{
		std::lock_guard lock(m_mutex);
		return m_runningContent;
	}

	UwpImGuiFrontend::DsuEndpoint GetDsuEndpoint() const override
	{
		const auto& config = GetConfig().dsu_client;
		return {
			.enabled = config.enabled.GetValue(),
			.streamingEnabled = config.streaming_enabled.GetValue(),
			.host = config.host.GetValue(),
			.port = config.port.GetValue(),
		};
	}

	bool StartStream(const UwpImGuiFrontend::StreamRequest& request) override
	{
		MediaStreamer::Request adapted;
		adapted.padView = request.secondaryScreen;
		adapted.streamType = request.streamType;
		adapted.url = request.url;
		adapted.bitrateKbps = request.bitrateKbps;
		adapted.width = static_cast<uint16>(request.width);
		adapted.height = static_cast<uint16>(request.height);
		adapted.fps = static_cast<uint8>(request.framesPerSecond);
		adapted.audioEnabled = request.audioEnabled;

		MediaStreamer::ActiveInfo activeInfo;
		std::string error;
		const bool started = MediaStreamer::Start(adapted, activeInfo, error);
		if (!started)
		{
			std::lock_guard lock(m_mutex);
			m_streamFailure = std::move(error);
		}
		return started;
	}

	void StopStream(bool) override { MediaStreamer::Stop(); }

	std::optional<UwpImGuiFrontend::ActiveStream> GetActiveStream() const override
	{
		const auto active = MediaStreamer::GetActiveInfo();
		if (!active)
			return std::nullopt;
		return UwpImGuiFrontend::ActiveStream{
			.secondaryScreen = active->padView,
			.streamType = active->streamType,
			.url = active->url,
			.width = active->width,
			.height = active->height,
			.framesPerSecond = active->fps,
			.audioEnabled = active->audioEnabled,
		};
	}

	std::optional<std::string> TakeStreamFailure() override
	{
		if (auto failure = MediaStreamer::TakeFailure())
			return failure;
		std::lock_guard lock(m_mutex);
		return std::exchange(m_streamFailure, std::nullopt);
	}

	void Log(UwpImGuiFrontend::LogLevel level, std::string_view message) override
	{
		if (level == UwpImGuiFrontend::LogLevel::Debug)
			cemuLog_logDebug(LogType::Force, "{}", message);
		else
			cemuLog_log(LogType::Force, "{}", message);
	}

	void SetRunningContent(std::string name)
	{
		std::lock_guard lock(m_mutex);
		const bool running = !name.empty();
		m_runningContent = {
			.name = std::move(name),
			.running = running,
		};
	}

private:
	mutable std::mutex m_mutex;
	UwpImGuiFrontend::RunningContent m_runningContent;
	std::optional<std::string> m_streamFailure;
};

class ProviderTransport final : public UwpImGuiFrontend::IExtensionPacketTransport
{
public:
	void Attach(DSUControllerProvider* provider)
	{
		std::lock_guard lock(m_mutex);
		m_provider = provider;
	}

	bool Detach(DSUControllerProvider* provider)
	{
		std::lock_guard lock(m_mutex);
		if (m_provider != provider)
			return false;
		m_provider = nullptr;
		return true;
	}

	void RequestControllerData(std::uint8_t slot) override
	{
		std::lock_guard lock(m_mutex);
		if (m_provider)
			m_provider->request_pad_data(slot);
	}

	void SendExtensionPacket(std::vector<std::uint8_t> packet, bool priority) override
	{
		std::lock_guard lock(m_mutex);
		if (!m_provider)
			return;
		if (priority)
			m_provider->send_priority_extension_packet(std::move(packet));
		else
			m_provider->send_extension_packet(std::move(packet));
	}

private:
	std::mutex m_mutex;
	DSUControllerProvider* m_provider = nullptr;
};

struct Runtime
{
	CemuCompanionHost host;
	ProviderTransport transport;
	std::unique_ptr<UwpImGuiFrontend::ICompanionService> service =
		UwpImGuiFrontend::CreateCompanionService();
};

Runtime& GetRuntime()
{
	static Runtime runtime;
	return runtime;
}

std::atomic_bool s_streamBusy{ false };
std::mutex s_streamMessageMutex;
std::optional<std::string> s_streamMessage;

void SetStreamMessage(std::string message)
{
	cemuLog_log(LogType::Force, "Companion stream: {}", message);
	std::lock_guard lock(s_streamMessageMutex);
	s_streamMessage = std::move(message);
}

std::string StreamTargetUrl()
{
	const auto& dsu = GetConfig().dsu_client;
	std::string url = dsu.streaming_url.GetValue();
	if (!url.empty())
		return url;
	const std::string host = dsu.host.GetValue();
	return fmt::format("udp://{}:5000", host.empty() ? "127.0.0.1" : host);
}
}

void Initialize()
{
	auto& runtime = GetRuntime();
	runtime.service->Initialize(runtime.host, "Cemu", "Nintendo - Wii U");
}

void Shutdown()
{
	GetRuntime().service->Shutdown();
}

void SetMetadataLanguage(const std::string& language)
{
	GetRuntime().service->SetMetadataLanguage(language);
}

void AttachDsuProvider(DSUControllerProvider* provider)
{
	if (!provider)
		return;
	auto& runtime = GetRuntime();
	runtime.transport.Attach(provider);
	runtime.service->AttachTransport(&runtime.transport);
}

void DetachDsuProvider(DSUControllerProvider* provider)
{
	auto& runtime = GetRuntime();
	if (runtime.transport.Detach(provider))
		runtime.service->DetachTransport(&runtime.transport);
}

bool HandleExtensionPacket(const uint8* data, size_t size, std::vector<std::vector<uint8>>& responses)
{
	return GetRuntime().service->HandleExtensionPacket(
		std::span<const std::uint8_t>(data, size), responses);
}

void SetRunningTitle(const std::string& titleName)
{
	auto& runtime = GetRuntime();
	runtime.host.SetRunningContent(titleName);
	runtime.service->UpdateRunningContent({
		.name = titleName,
		.running = !titleName.empty(),
	});
}

void SetSelectedTitleToken(uint64 token)
{
	GetRuntime().service->UpdateSelection(token);
}

std::optional<fs::path> TakePendingLaunchPath()
{
	const auto pending = GetRuntime().service->TakePendingLaunch();
	if (!pending)
		return std::nullopt;
	for (const auto& item : GetRuntime().host.SnapshotCatalogue())
	{
		if (item.id == *pending)
			return item.launchPath;
	}
	GetRuntime().service->CompletePendingLaunch(false);
	return std::nullopt;
}

std::optional<uint64> TakePendingSelectionToken()
{
	return GetRuntime().service->TakePendingSelection();
}

void CompletePendingLaunch(bool succeeded)
{
	GetRuntime().service->CompletePendingLaunch(succeeded);
}

NavigationState GetNavigationState()
{
	const auto state = GetRuntime().service->GetNavigationState();
	return { state.up, state.down, state.left, state.right };
}

bool IsEnabled()
{
	return GetConfig().dsu_client.enabled.GetValue();
}

std::string GetStatus()
{
	return GetRuntime().service->GetStatus().message;
}

bool IsStreamActive()
{
	return MediaStreamer::IsActive();
}

bool IsStreamBusy()
{
	return s_streamBusy.load(std::memory_order_acquire);
}

std::optional<std::string> TakeStreamMessage()
{
	std::lock_guard lock(s_streamMessageMutex);
	return std::exchange(s_streamMessage, std::nullopt);
}

void RequestStreamStart()
{
	if (s_streamBusy.exchange(true, std::memory_order_acq_rel))
		return;
	std::thread([] {
		MediaStreamer::Request request;
		request.url = StreamTargetUrl();
		MediaStreamer::ActiveInfo activeInfo;
		std::string error;
		const bool started = MediaStreamer::Start(request, activeInfo, error);
		if (started)
			GetRuntime().service->NotifyStreamChanged(0);
		SetStreamMessage(started ? fmt::format("Streaming to {}", request.url) :
			(error.empty() ? "Stream could not be started" : error));
		s_streamBusy.store(false, std::memory_order_release);
	}).detach();
}

void RequestStreamStop()
{
	if (s_streamBusy.exchange(true, std::memory_order_acq_rel))
		return;
	std::thread([] {
		MediaStreamer::Stop();
		GetRuntime().service->NotifyStreamChanged(0);
		SetStreamMessage("Stream stopped");
		s_streamBusy.store(false, std::memory_order_release);
	}).detach();
}
}
