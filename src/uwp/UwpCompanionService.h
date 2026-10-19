#pragma once

#include "Common/precompiled.h"

#include <optional>
#include <string>
#include <vector>

class DSUControllerProvider;

namespace CemuUwp::CompanionService
{
	struct NavigationState
	{
		bool up = false;
		bool down = false;
		bool left = false;
		bool right = false;
	};

	void Initialize();
	void Shutdown();
	void SetMetadataLanguage(const std::string& language);

	void AttachDsuProvider(DSUControllerProvider* provider);
	void DetachDsuProvider(DSUControllerProvider* provider);

	bool HandleExtensionPacket(const uint8* data, size_t size, std::vector<std::vector<uint8>>& responses);

	void SetRunningTitle(const std::string& titleName);
	void SetSelectedTitleToken(uint64 token);
	std::optional<fs::path> TakePendingLaunchPath();
	std::optional<uint64> TakePendingSelectionToken();
	void CompletePendingLaunch(bool succeeded);
	NavigationState GetNavigationState();

	bool IsEnabled();
	std::string GetStatus();

	bool IsStreamActive();
	// encoder setup blocks for seconds, so both run on a worker thread
	bool IsStreamBusy();
	void RequestStreamStart();
	void RequestStreamStop();
	std::optional<std::string> TakeStreamMessage();
}
