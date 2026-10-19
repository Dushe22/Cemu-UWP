#pragma once

#include "Common/precompiled.h"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace CemuUwp::MediaStreamer
{
	struct Request
	{
		bool padView = false;
		uint32 streamType = 4;
		std::string url;
		uint32 bitrateKbps = 4000;
		uint16 width = 0;
		uint16 height = 0;
		uint8 fps = 30;
		bool audioEnabled = true;
	};

	struct ActiveInfo
	{
		bool padView = false;
		uint32 streamType = 4;
		std::string url;
		uint16 width = 0;
		uint16 height = 0;
		uint8 fps = 30;
		bool audioEnabled = false;
	};

	bool Start(const Request& request, ActiveInfo& activeInfo, std::string& error);
	void Stop();
	bool IsActive();
	std::optional<ActiveInfo> GetActiveInfo();
	std::optional<std::string> TakeFailure();

	// Reserves the current encoder interval for one TV or GamePad readback.
	bool RequestVideoCapture(bool padView);
	std::vector<uint8> AcquireVideoFrameBuffer(std::size_t size);
	void SubmitVideoFrame(bool padView, uint32 width, uint32 height, uint32 rowBytes,
		uint32 dxgiFormat, std::vector<uint8> pixels);

	void SubmitAudioSamples(const sint16* samples, uint32 frames, uint32 channels, uint32 sampleRate);
}
