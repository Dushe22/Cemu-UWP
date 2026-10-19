#pragma once

#include "Common/precompiled.h"

#include <filesystem>
#include <vector>

namespace CemuUwp::VideoPlayer
{
	bool Open(const std::filesystem::path& path);
	void Close();
	bool IsOpen();
	std::filesystem::path CurrentPath();
	bool AcquireFrame(std::vector<uint8>& bgra, int& width, int& height);
	void Shutdown();
}
