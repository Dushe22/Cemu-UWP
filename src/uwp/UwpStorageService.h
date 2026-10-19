#pragma once

#include <winrt/Windows.Storage.h>

namespace CemuUwp::StorageService
{
	void Initialize();
	fs::path LocalRoot();
	fs::path ConfigRoot();
	fs::path CacheRoot();
	fs::path InstalledContentRoot();
	fs::path MlcRoot();
	void RememberFile(std::wstring_view token, const winrt::Windows::Storage::StorageFile& file);
	void RememberFolder(std::wstring_view token, const winrt::Windows::Storage::StorageFolder& folder);
};
