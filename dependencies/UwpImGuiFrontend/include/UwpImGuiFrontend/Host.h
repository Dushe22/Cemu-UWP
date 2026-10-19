#pragma once

#include "DsuCompanion.h"
#include "ScreenScraper.h"

namespace UwpImGuiFrontend
{
enum class HostCommand : std::uint8_t
{
	AddContent,
	OpenHome,
	OpenSettings,
	OpenControllers,
	OpenGraphics,
	OpenTitleManager,
	CycleTheme,
	RestartContent,
	StopContent,
	ExitApplication,
};

class IFrontendHost : public ICompanionHost
{
public:
	~IFrontendHost() override = default;

	[[nodiscard]] virtual std::filesystem::path ResourceRoot() const = 0;
	[[nodiscard]] virtual std::filesystem::path StateRoot() const = 0;
	[[nodiscard]] virtual TextureHandle LoadTexture(const std::filesystem::path& path) = 0;
	virtual void ReleaseTexture(TextureHandle texture) = 0;
	[[nodiscard]] virtual TextureHandle AcquireVideoFrame(const std::filesystem::path&) { return {}; }
	virtual void StopVideoPlayback() {}
	virtual bool RequestLaunch(ItemId item) = 0;
	virtual void OpenContextMenu(ItemId item) = 0;
	virtual void Execute(HostCommand command) = 0;
	virtual void DrawSettingsPage(std::string_view pageId) = 0;
	virtual void OpenUri(std::string_view uri) = 0;
	virtual void PersistFrontendState(std::string_view data) = 0;
	[[nodiscard]] virtual std::string LoadFrontendState() const = 0;
};
}
