#include "UwpImGuiFrontend/CredentialStore.h"

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Security.Credentials.h>

#include <cstdint>
#include <utility>

namespace UwpImGuiFrontend
{
namespace
{
class PasswordVaultCredentialStore final : public ICredentialStore
{
public:
	explicit PasswordVaultCredentialStore(std::wstring resourceName)
		: m_resourceName(std::move(resourceName))
	{
	}

	ScreenScraperCredentials Load() const override
	{
		try
		{
			winrt::Windows::Security::Credentials::PasswordVault vault;
			const auto matches = vault.FindAllByResource(m_resourceName);
			if (matches.Size() == 0)
				return {};
			auto credential = matches.GetAt(0);
			credential.RetrievePassword();
			return {
				winrt::to_string(credential.UserName()),
				winrt::to_string(credential.Password()),
			};
		}
		catch (const winrt::hresult_error&)
		{
			return {};
		}
	}

	bool Save(const ScreenScraperCredentials& credentials) override
	{
		if (credentials.user.empty() || credentials.password.empty())
			return false;
		try
		{
			Clear();
			winrt::Windows::Security::Credentials::PasswordVault vault;
			vault.Add(winrt::Windows::Security::Credentials::PasswordCredential(
				m_resourceName, winrt::to_hstring(credentials.user),
				winrt::to_hstring(credentials.password)));
			return true;
		}
		catch (const winrt::hresult_error&)
		{
			return false;
		}
	}

	bool Clear() override
	{
		try
		{
			winrt::Windows::Security::Credentials::PasswordVault vault;
			const auto matches = vault.FindAllByResource(m_resourceName);
			for (const auto& credential : matches)
				vault.Remove(credential);
			return true;
		}
		catch (const winrt::hresult_error& error)
		{
			constexpr std::int32_t elementNotFound = static_cast<std::int32_t>(0x80070490u);
			return error.code().value == elementNotFound;
		}
	}

private:
	std::wstring m_resourceName;
};
}

std::unique_ptr<ICredentialStore> CreatePasswordVaultCredentialStore(
	std::wstring resourceName)
{
	return std::make_unique<PasswordVaultCredentialStore>(std::move(resourceName));
}
}
