#pragma once

#include "CredentialStore.h"
#include "DsuCompanion.h"
#include "InputMapping.h"
#include "StorageLocations.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace UwpImGuiFrontend
{
struct ScreenScraperPanelModel
{
	bool enabled = false;
	ScreenScraperOptions options;
	ScreenScraperArtwork listArtwork = ScreenScraperArtwork::Box3D;
	ScreenScraperArtwork detailArtwork = ScreenScraperArtwork::Box3D;
	ScreenScraperBackground dynamicBackground = ScreenScraperBackground::Off;
	// presentation-only state, not persisted
	bool developerCredentialsConfigured = true;
	std::size_t artworkTitleCount = 0;
	std::size_t missingListArtworkCount = 0;
	std::string titleMediaPath;
	std::string platformMediaPath;
	std::filesystem::path progressFile;
	std::filesystem::path mediaDirectory;
	std::vector<StorageLocationPreset> mediaDirectoryPresets;
	PickerRequest mediaDirectoryPicker;
};

struct ScreenScraperPanelCallbacks
{
	std::function<ScreenScraperRequest(ScreenScraperCredentials, ScreenScraperOptions)> makeRequest;
	std::function<void(const ScreenScraperPanelModel&)> persist;
	std::function<void(std::string_view)> notify;
	std::function<void(StorageLocationSelection)> mediaDirectorySelected;
	std::function<void(bool)> pickerOpenChanged;
};

class ScreenScraperPanel
{
public:
	void Draw(ScreenScraperPanelModel& model, IScreenScraperService& service,
		ICredentialStore& credentials, const ScreenScraperPanelCallbacks& callbacks,
		IPickerService* picker = nullptr);

private:
	void EnsureCredentialsLoaded(ICredentialStore& credentials);
	ScreenScraperCredentials CurrentCredentials() const;

	std::array<char, 128> m_user{};
	std::array<char, 128> m_password{};
	bool m_credentialsLoaded = false;
};

struct CompanionPanelModel
{
	std::vector<DsuServerModel> servers;
	bool streamingEnabled = false;
	std::string streamingUrl;
};

[[nodiscard]] DsuEndpoint MakeCompanionEndpoint(
	const CompanionPanelModel& model) noexcept;

using PersistCompanionPanel = std::function<void(const CompanionPanelModel&)>;

class CompanionPanel
{
public:
	void Draw(CompanionPanelModel& model, const PersistCompanionPanel& persist);

private:
	std::array<char, 512> m_streamingUrl{};
	std::vector<std::array<char, 128>> m_serverNames;
	std::vector<std::array<char, 256>> m_serverHosts;
	bool m_loaded = false;
};
}
