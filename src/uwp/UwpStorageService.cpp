#include "UwpStorageService.h"

#include "Cemu/ncrypto/ncrypto.h"
#include "util/helpers/helpers.h"

#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Storage.AccessCache.h>

#include <array>
#include <fstream>
#include <string_view>

namespace
{
fs::path s_localRoot;
fs::path s_installedRoot;

fs::path PathFromHString(const winrt::hstring& value)
{
	return fs::path(std::wstring(value.c_str()));
}

void WriteDefaultLanguageFiles(const fs::path& langDir)
{
	const auto languageFile = langDir / "language.txt";
	if (!fs::exists(languageFile))
	{
		std::ofstream file(languageFile, std::ios::trunc);
		constexpr std::array languages{ "ja", "en", "fr", "de", "it", "es", "zh", "ko", "nl", "pt", "ru", "zh" };
		for (const char* lang : languages)
			file << '"' << lang << "\"," << '\n';
	}

	const auto countryFile = langDir / "country.txt";
	if (!fs::exists(countryFile))
	{
		std::ofstream file(countryFile, std::ios::trunc);
		for (sint32 i = 0; i < (sint32)NCrypto::GetCountryCount(); i++)
		{
			const char* countryCode = NCrypto::GetCountryAsString(i);
			if (std::string_view(countryCode) == "NN")
				file << "NULL," << '\n';
			else
				file << '"' << countryCode << "\"," << '\n';
		}
	}
}

void EnsureDefaultMlcFiles(const fs::path& mlc)
{
	const std::array directories{
		mlc,
		mlc / "sys",
		mlc / "usr",
		mlc / "usr/title/00050000",
		mlc / "usr/title/0005000c",
		mlc / "usr/title/0005000e",
		mlc / "usr/save/00050010/1004a000/user/common/db",
		mlc / "usr/save/00050010/1004a100/user/common/db",
		mlc / "usr/save/00050010/1004a200/user/common/db",
		mlc / "sys/title/0005001b/1005c000/content",
	};

	std::error_code ec;
	for (const auto& path : directories)
		fs::create_directories(path, ec);

	WriteDefaultLanguageFiles(mlc / "sys/title/0005001b/1005c000/content");
}
}

namespace CemuUwp::StorageService
{
void Initialize()
{
	s_localRoot = PathFromHString(winrt::Windows::Storage::ApplicationData::Current().LocalFolder().Path());
	s_installedRoot = PathFromHString(winrt::Windows::ApplicationModel::Package::Current().InstalledLocation().Path());

	std::error_code ec;
	for (const auto& path : { ConfigRoot(), CacheRoot(), MlcRoot(), s_localRoot / "shaderCache", s_localRoot / "controllerProfiles", s_localRoot / "logs" })
		fs::create_directories(path, ec);

	EnsureDefaultMlcFiles(MlcRoot());
}

fs::path LocalRoot()
{
	return s_localRoot;
}

fs::path ConfigRoot()
{
	return s_localRoot / "config";
}

fs::path CacheRoot()
{
	return s_localRoot / "cache";
}

fs::path InstalledContentRoot()
{
	return s_installedRoot;
}

fs::path MlcRoot()
{
	return s_localRoot / "mlc01";
}

void RememberFile(std::wstring_view token, const winrt::Windows::Storage::StorageFile& file)
{
	if (file)
		winrt::Windows::Storage::AccessCache::StorageApplicationPermissions::FutureAccessList().AddOrReplace(winrt::hstring(token), file);
}

void RememberFolder(std::wstring_view token, const winrt::Windows::Storage::StorageFolder& folder)
{
	if (folder)
		winrt::Windows::Storage::AccessCache::StorageApplicationPermissions::FutureAccessList().AddOrReplace(winrt::hstring(token), folder);
}
}
