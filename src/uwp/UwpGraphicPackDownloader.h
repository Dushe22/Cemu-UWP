#pragma once

#include <string>

namespace CemuUwp::GraphicPackDownloader
{
	enum class State
	{
		Idle,
		Working,
		Finished,
		UpToDate,
		Error,
	};

	State GetState();
	std::string GetStatus();
	bool Start();
	bool ConsumeFinished();
}
