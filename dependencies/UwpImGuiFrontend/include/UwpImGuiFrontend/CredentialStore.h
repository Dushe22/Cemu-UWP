#pragma once

#include "ScreenScraper.h"

#include <memory>
#include <string>

namespace UwpImGuiFrontend
{
class ICredentialStore
{
public:
	virtual ~ICredentialStore() = default;
	[[nodiscard]] virtual ScreenScraperCredentials Load() const = 0;
	virtual bool Save(const ScreenScraperCredentials& credentials) = 0;
	virtual bool Clear() = 0;
};

[[nodiscard]] std::unique_ptr<ICredentialStore> CreatePasswordVaultCredentialStore(
	std::wstring resourceName);
}
