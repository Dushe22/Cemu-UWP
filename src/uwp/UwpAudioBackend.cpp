#include "UwpAudioBackend.h"

#include "audio/IAudioAPI.h"
#include "config/CemuConfig.h"

namespace CemuUwp::AudioBackend
{
void ConfigureDefaultDevice()
{
	GetConfig().audio_api = IAudioAPI::XAudio2;
	if (GetConfig().tv_device.empty())
		GetConfig().tv_device = L"default";
	if (GetConfig().pad_device.empty())
		GetConfig().pad_device = L"default";
}
}
