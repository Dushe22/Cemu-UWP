#pragma once

#include "Input.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace UwpImGuiFrontend
{
inline constexpr std::uint8_t MaxCompanionScreenId = 4;

struct DsuEndpoint
{
	bool enabled = false;
	bool streamingEnabled = false;
	bool mainStreamingSupported = true;
	bool secondaryStreamingSupported = true;
	std::string host = "127.0.0.1";
	std::uint16_t port = 26760;
	std::string streamingUrl = "udp://127.0.0.1:5000";
	// bits 0-3 map to auxiliary screen IDs 1-4
	std::uint8_t auxiliaryStreamingScreenMask = 0x01;
	// requires the exact-screen methods on ICompanionHost
	bool independentScreenStreamsSupported = false;
};

struct StreamRequest
{
	bool secondaryScreen = false;
	std::uint32_t streamType = 4;
	std::uint32_t authorizationToken = 0;
	std::string url;
	std::uint32_t width = 0;
	std::uint32_t height = 0;
	std::uint32_t bitrateKbps = 0;
	std::uint32_t framesPerSecond = 0;
	bool audioEnabled = true;
	// 0 is the main screen, 1-4 auxiliary; secondaryScreen mirrors screenId != 0
	std::uint8_t screenId = 0;
};

struct ActiveStream
{
	bool secondaryScreen = false;
	std::uint32_t streamType = 4;
	std::string url;
	std::uint32_t width = 0;
	std::uint32_t height = 0;
	std::uint32_t framesPerSecond = 0;
	bool audioEnabled = false;
	std::uint8_t screenId = 0;
};

struct RunningContent
{
	ItemId id = 0;
	std::string name;
	bool running = false;
};

struct CompanionStatus
{
	bool running = false;
	bool transportAttached = false;
	// true only after recent traffic from the remote peer
	bool clientConnected = false;
	std::string message;
};

class IExtensionPacketTransport
{
public:
	virtual ~IExtensionPacketTransport() = default;
	virtual void RequestControllerData(std::uint8_t slot) = 0;
	virtual void SendExtensionPacket(std::vector<std::uint8_t> packet, bool priority) = 0;
};

class ICompanionHost
{
public:
	virtual ~ICompanionHost() = default;

	[[nodiscard]] virtual std::vector<LibraryItem> SnapshotCatalogue() = 0;
	[[nodiscard]] virtual std::vector<MediaAsset> SnapshotPlatformMedia() { return {}; }
	[[nodiscard]] virtual RunningContent GetRunningContent() const = 0;
	[[nodiscard]] virtual DsuEndpoint GetDsuEndpoint() const = 0;
	// single-encoder contract, the exact-screen defaults below map onto it
	virtual bool StartStream(const StreamRequest& request) = 0;
	virtual void StopStream(bool secondaryScreen) = 0;
	[[nodiscard]] virtual std::optional<ActiveStream> GetActiveStream() const = 0;
	[[nodiscard]] virtual std::optional<std::string> TakeStreamFailure() = 0;

	// exact-screen contract, used when independentScreenStreamsSupported is set
	virtual bool StartScreenStream(const StreamRequest& request)
	{
		return StartStream(request);
	}
	virtual void StopScreenStream(std::uint8_t screenId)
	{
		StopStream(screenId != 0);
	}
	[[nodiscard]] virtual std::optional<ActiveStream> GetActiveScreenStream(
		std::uint8_t screenId) const
	{
		auto active = GetActiveStream();
		if (!active)
			return std::nullopt;
		const std::uint8_t activeScreenId = active->screenId != 0
			? active->screenId : (active->secondaryScreen ? 1 : 0);
		return activeScreenId == screenId ? active : std::nullopt;
	}
	[[nodiscard]] virtual std::optional<std::string> TakeScreenStreamFailure(
		std::uint8_t)
	{
		return TakeStreamFailure();
	}
	virtual void Log(LogLevel level, std::string_view message) = 0;
};

class ICompanionService
{
public:
	virtual ~ICompanionService() = default;

	virtual void Initialize(ICompanionHost& host,
		std::string applicationName, std::string platformName) = 0;
	virtual void Shutdown() = 0;
	virtual void SetMetadataLanguage(std::string language) = 0;
	virtual void AttachTransport(IExtensionPacketTransport* transport) = 0;
	virtual void DetachTransport(IExtensionPacketTransport* transport) = 0;
	virtual void UpdateRunningContent(RunningContent content) = 0;
	virtual void NotifyStreamChanged(std::uint8_t screenId) = 0;
	virtual void UpdateSelection(ItemId item) = 0;
	// false means a remote launch already owns the slot, service that instead
	[[nodiscard]] virtual bool TryBeginLocalLaunch(ItemId) { return true; }
	virtual void CancelLocalLaunch(ItemId) {}
	[[nodiscard]] virtual std::optional<ItemId> TakePendingLaunch() = 0;
	[[nodiscard]] virtual std::optional<ItemId> TakePendingSelection() = 0;
	virtual void CompletePendingLaunch(bool succeeded) = 0;
	[[nodiscard]] virtual NavigationState GetNavigationState() const = 0;
	[[nodiscard]] virtual CompanionStatus GetStatus() const = 0;

	// returns true when the extension consumed an unrecognized DSU datagram
	virtual bool HandleExtensionPacket(
		std::span<const std::uint8_t> request,
		std::vector<std::vector<std::uint8_t>>& responses) = 0;
};

[[nodiscard]] std::unique_ptr<ICompanionService> CreateCompanionService();
}
