#pragma once
#include "Cafe/OS/RPL/COSModule.h"

namespace nn 
{
	namespace erreula
	{
		void render(bool mainWindow);
#if defined(CEMU_UWP)
		bool hasInputHook();
#endif

		COSModule* GetModule();
	}
}
