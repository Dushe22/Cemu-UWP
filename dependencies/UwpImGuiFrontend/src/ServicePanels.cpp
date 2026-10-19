#include "UwpImGuiFrontend/ServicePanels.h"
#include "UwpImGuiFrontend/NativeTextInput.h"
#include "UwpImGuiFrontend/Widgets.h"

#include <imgui.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace UwpImGuiFrontend
{
namespace
{
template<std::size_t Size>
void CopyBuffer(std::array<char, Size>& destination, std::string_view source)
{
	destination.fill('\0');
	const std::size_t length = std::min(source.size(), destination.size() - 1);
	std::memcpy(destination.data(), source.data(), length);
}

void Notify(const ScreenScraperPanelCallbacks& callbacks, std::string_view message)
{
	if (callbacks.notify)
		callbacks.notify(message);
}
}

void ScreenScraperPanel::EnsureCredentialsLoaded(ICredentialStore& credentials)
{
	if (m_credentialsLoaded)
		return;
	const ScreenScraperCredentials stored = credentials.Load();
	CopyBuffer(m_user, stored.user);
	CopyBuffer(m_password, stored.password);
	m_credentialsLoaded = true;
}

ScreenScraperCredentials ScreenScraperPanel::CurrentCredentials() const
{
	return { m_user.data(), m_password.data() };
}

void ScreenScraperPanel::Draw(ScreenScraperPanelModel& model,
	IScreenScraperService& service, ICredentialStore& credentials,
	const ScreenScraperPanelCallbacks& callbacks, IPickerService* picker)
{
	EnsureCredentialsLoaded(credentials);
	SectionTitle("ScreenScraper");
	bool changed = ShellCheckbox("Enable ScreenScraper", &model.enabled);
	(void)NativeInputText("Username", m_user.data(), m_user.size(), 0,
		TextInputScope::UserName);
	(void)NativeInputText("Password", m_password.data(), m_password.size(),
		ImGuiInputTextFlags_Password, TextInputScope::Password);
	if (!model.developerCredentialsConfigured)
	{
		ImGui::TextColored(ImVec4(0.90f, 0.28f, 0.18f, 1.0f),
			"Developer API credentials are missing from this build.");
	}
	if (ShellButton("Save account credentials", { 240.0f, 42.0f }))
	{
		Notify(callbacks, credentials.Save(CurrentCredentials()) ?
			"ScreenScraper account saved" : "Failed to save ScreenScraper account");
	}
	ImGui::SameLine();
	if (ShellButton("Clear account credentials", { 250.0f, 42.0f }))
	{
		if (credentials.Clear())
		{
			CopyBuffer(m_user, {});
			CopyBuffer(m_password, {});
			Notify(callbacks, "ScreenScraper account cleared");
		}
		else
		{
			Notify(callbacks, "Failed to clear ScreenScraper account");
		}
	}

	SectionTitle("Downloads");
	changed |= ShellCheckbox("Download metadata", &model.options.downloadMetadata);
	changed |= ShellCheckbox("Download box art", &model.options.downloadBoxArt);
	changed |= ShellCheckbox("Download fanart backgrounds", &model.options.downloadFanArt);
	changed |= ShellCheckbox("Download videos", &model.options.downloadVideos);
	changed |= ShellCheckbox("Download screenshots", &model.options.downloadScreenshots);
	changed |= ShellCheckbox("Download logos", &model.options.downloadLogos);
	changed |= ShellCheckbox("Overwrite existing media", &model.options.overwriteExisting);

	const ScreenScraperProgress progress = service.GetProgress();
	const bool running = progress.state == ScreenScraperState::Working;
	const bool hasPendingScan = !running && callbacks.makeRequest &&
		!model.progressFile.empty() && service.HasPendingScan(model.progressFile);
	if (ShellButton(running ? "Stop download" : "Download library assets",
		{ 280.0f, 42.0f }))
	{
		if (running)
		{
			service.RequestStop();
		}
		else if (!model.enabled)
		{
			Notify(callbacks, "Enable ScreenScraper first");
		}
		else if (!model.developerCredentialsConfigured)
		{
			Notify(callbacks, "Developer API credentials are missing from this build");
		}
		else if (!callbacks.makeRequest)
		{
			Notify(callbacks, "ScreenScraper request adapter is unavailable");
		}
		else
		{
			ScreenScraperRequest request = callbacks.makeRequest(
				CurrentCredentials(), model.options);
			if (request.account.user.empty() || request.account.password.empty())
			{
				Notify(callbacks, "Sign in to ScreenScraper first");
			}
			else if (request.targets.empty())
			{
				Notify(callbacks, "No titles to scrape");
			}
			else
			{
				(void)credentials.Save(request.account);
				if (!service.Start(std::move(request)))
					Notify(callbacks, "ScreenScraper download is already running");
			}
		}
	}
	if (!running && hasPendingScan)
	{
		ImGui::SameLine();
		if (ShellButton("Resume scan", { 180.0f, 42.0f }))
		{
			ScreenScraperRequest request = callbacks.makeRequest(
				CurrentCredentials(), model.options);
			if (!service.ResumePendingScan(std::move(request)))
				Notify(callbacks, "Nothing left to resume");
		}
	}
	if (progress.state != ScreenScraperState::Idle)
		ImGui::TextWrapped("Status: %s", progress.status.c_str());
	if (!running && hasPendingScan)
	{
		ImGui::TextWrapped("An unfinished scan is saved; it resumes "
			"automatically on the next launch.");
	}

	if (!model.mediaDirectoryPresets.empty())
	{
		SectionTitle("Storage");
		if (picker)
		{
			StorageLocationSelectorModel storage;
			storage.id = "screenscraper-media-directory";
			storage.label = "Downloaded media location";
			storage.currentPath = model.mediaDirectory;
			storage.presets = model.mediaDirectoryPresets;
			storage.currentPathLabel = "Media path";
			storage.customPicker = model.mediaDirectoryPicker;
			(void)DrawStorageLocationSelector(storage, *picker, {
				.selected = callbacks.mediaDirectorySelected,
				.failed = [&callbacks](std::string_view error) {
					Notify(callbacks, error);
				},
				.pickerOpenChanged = callbacks.pickerOpenChanged,
			});
		}
		else
		{
			ImGui::TextDisabled("A folder picker is required to change this location.");
		}
	}

	SectionTitle("Artwork");
	static constexpr std::array<ScreenScraperArtwork, 10> artwork{{
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
	}};
	std::vector<ChoiceItem> artworkChoices;
	artworkChoices.reserve(artwork.size());
	for (ScreenScraperArtwork kind : artwork)
	{
		artworkChoices.push_back({ std::to_string(static_cast<int>(kind)),
			std::string(ScreenScraperArtworkLabel(kind)), true });
	}
	std::size_t listArtwork = static_cast<std::size_t>(model.listArtwork);
	if (ShellCombo("Game-list artwork", &listArtwork, artworkChoices))
	{
		model.listArtwork = artwork[std::min(listArtwork, artwork.size() - 1)];
		changed = true;
	}
	std::size_t detailArtwork = static_cast<std::size_t>(model.detailArtwork);
	if (ShellCombo("Large information artwork", &detailArtwork, artworkChoices))
	{
		model.detailArtwork = artwork[std::min(detailArtwork, artwork.size() - 1)];
		changed = true;
	}
	if (model.artworkTitleCount != 0 && model.missingListArtworkCount != 0)
	{
		ImGui::TextWrapped("%zu of %zu titles have no game-list artwork of this "
			"type yet.", model.missingListArtworkCount, model.artworkTitleCount);
	}

	static constexpr std::array<std::pair<ScreenScraperBackground, const char*>, 6> backgrounds{{
		{ ScreenScraperBackground::Off, "Off" },
		{ ScreenScraperBackground::FanArt, "Fan art" },
		{ ScreenScraperBackground::Screenshot, "Screenshot" },
		{ ScreenScraperBackground::Video, "Video" },
		{ ScreenScraperBackground::MixRecalboxV1, "Recalbox Mix V1" },
		{ ScreenScraperBackground::MixRecalboxV2, "Recalbox Mix V2" },
	}};
	std::size_t background = static_cast<std::size_t>(model.dynamicBackground);
	std::vector<ChoiceItem> backgroundChoices;
	for (const auto& [kind, label] : backgrounds)
		backgroundChoices.push_back({ std::to_string(static_cast<int>(kind)), label, true });
	if (ShellCombo("Dynamic background", &background, backgroundChoices))
	{
		model.dynamicBackground = backgrounds[std::min(background, backgrounds.size() - 1)].first;
		changed = true;
	}
	if (model.dynamicBackground == ScreenScraperBackground::Video)
		ImGui::TextWrapped("Video backgrounds use more storage and bandwidth.");

	static constexpr std::array<std::pair<ScreenScraperRegion, const char*>, 5> regions{{
		{ ScreenScraperRegion::Automatic, "Auto" },
		{ ScreenScraperRegion::UnitedStates, "USA" },
		{ ScreenScraperRegion::Europe, "Europe" },
		{ ScreenScraperRegion::Japan, "Japan" },
		{ ScreenScraperRegion::World, "World" },
	}};
	std::size_t region = static_cast<std::size_t>(model.options.preferredRegion);
	std::vector<ChoiceItem> regionChoices;
	regionChoices.reserve(regions.size());
	for (const auto& [kind, label] : regions)
		regionChoices.push_back({ std::to_string(static_cast<int>(kind)), label, true });
	if (ShellCombo("Preferred region", &region, regionChoices))
	{
		model.options.preferredRegion =
			regions[std::min(region, regions.size() - 1)].first;
		changed = true;
	}

	if (!model.titleMediaPath.empty())
		ImGui::TextWrapped("Title media: %s", model.titleMediaPath.c_str());
	if (!model.platformMediaPath.empty())
		ImGui::TextWrapped("Platform media: %s", model.platformMediaPath.c_str());

	if (changed && callbacks.persist)
		callbacks.persist(model);
}

DsuEndpoint MakeCompanionEndpoint(const CompanionPanelModel& model) noexcept
{
	DsuEndpoint endpoint;
	endpoint.streamingEnabled = model.streamingEnabled;
	endpoint.streamingUrl = model.streamingUrl;
	const auto primary = std::find_if(model.servers.begin(), model.servers.end(),
		[](const DsuServerModel& server) { return server.enabled; });
	if (primary != model.servers.end())
	{
		endpoint.enabled = true;
		endpoint.host = primary->host;
		endpoint.port = primary->port;
	}
	return endpoint;
}

void CompanionPanel::Draw(CompanionPanelModel& model,
	const PersistCompanionPanel& persist)
{
	if (model.servers.empty())
	{
		model.servers.push_back({
			.id = "default", .name = "DSU Server",
		});
	}
	if (!m_loaded)
	{
		CopyBuffer(m_streamingUrl, model.streamingUrl);
		m_loaded = true;
	}
	if (m_serverHosts.size() != model.servers.size())
	{
		m_serverHosts.resize(model.servers.size());
		m_serverNames.resize(model.servers.size());
		for (std::size_t index = 0; index < model.servers.size(); ++index)
		{
			CopyBuffer(m_serverHosts[index], model.servers[index].host);
			CopyBuffer(m_serverNames[index], model.servers[index].name);
		}
	}

	bool changed = false;
	SectionTitle("DSU servers");
	for (std::size_t index = 0; index < model.servers.size(); ++index)
	{
		auto& server = model.servers[index];
		ImGui::PushID(static_cast<int>(index));
		if (server.id.empty())
			server.id = "dsu-" + std::to_string(index + 1);
		changed |= ShellCheckbox("Enabled", &server.enabled);
		if (NativeInputText("Name", m_serverNames[index].data(),
			m_serverNames[index].size(), 0, TextInputScope::Text))
		{
			server.name = m_serverNames[index].data();
			changed = true;
		}
		if (NativeInputText("Host", m_serverHosts[index].data(),
			m_serverHosts[index].size(), 0, TextInputScope::Text))
		{
			server.host = m_serverHosts[index].data();
			changed = true;
		}
		int port = server.port;
		if (ShellIntegerStepper("Port", &port, 1, 65535))
		{
			server.port = static_cast<std::uint16_t>(std::clamp(port, 1, 65535));
			changed = true;
		}
		ImGui::PopID();
	}
	if (ShellButton("Add another DSU server"))
	{
		const std::size_t number = model.servers.size() + 1;
		model.servers.push_back({ .id = "dsu-" + std::to_string(number),
			.name = "DSU Server " + std::to_string(number) });
		m_serverHosts.emplace_back();
		m_serverNames.emplace_back();
		CopyBuffer(m_serverHosts.back(), model.servers.back().host);
		CopyBuffer(m_serverNames.back(), model.servers.back().name);
		changed = true;
	}

	SectionTitle("Streaming");
	changed |= ShellCheckbox("Enable streaming", &model.streamingEnabled);
	if (NativeInputText("Streaming URL", m_streamingUrl.data(),
		m_streamingUrl.size(), 0, TextInputScope::Url))
	{
		model.streamingUrl = m_streamingUrl.data();
		changed = true;
	}
	if (changed && persist)
		persist(model);
}
}
