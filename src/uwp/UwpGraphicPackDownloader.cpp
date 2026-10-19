#include "UwpGraphicPackDownloader.h"

#include "Common/FileStream.h"
#include "Cemu/Logging/CemuLogging.h"
#include "config/ActiveSettings.h"

#include <rapidjson/document.h>
#include <zip.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Web.Http.h>
#include <winrt/Windows.Web.Http.Headers.h>

#include <atomic>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

// AppContainer downloader for community graphic packs.
namespace CemuUwp::GraphicPackDownloader
{
namespace
{
std::atomic<State> s_state{ State::Idle };
std::atomic<bool> s_finishedPending{ false };
std::mutex s_statusMutex;
std::string s_status;

void SetStatus(State state, const std::string& status)
{
	{
		std::scoped_lock lock(s_statusMutex);
		s_status = status;
	}
	s_state.store(state);
	cemuLog_log(LogType::Force, "Graphic pack download: {}", status);
}

// WinRT HttpClient supplies AppContainer networking and system TLS handling.
bool DownloadToBuffer(const char* url, std::vector<uint8>& buffer, const char* stageLabel)
{
	buffer.clear();
	try
	{
		winrt::Windows::Web::Http::HttpClient client;
		client.DefaultRequestHeaders().UserAgent().TryParseAdd(L"Cemu-UWP");
		const std::string urlUtf8(url);
		const std::wstring urlWide(urlUtf8.begin(), urlUtf8.end());
		winrt::Windows::Foundation::Uri uri{ winrt::hstring(urlWide) };
		auto responseBuffer = client.GetBufferAsync(uri).get();
		buffer.resize(responseBuffer.Length());
		if (!buffer.empty())
		{
			auto reader = winrt::Windows::Storage::Streams::DataReader::FromBuffer(responseBuffer);
			reader.ReadBytes(winrt::array_view<uint8_t>(buffer.data(), buffer.data() + buffer.size()));
		}
		cemuLog_log(LogType::Force, "Graphic pack download: {} ok ({} bytes)", stageLabel, buffer.size());
		return true;
	}
	catch (const winrt::hresult_error& e)
	{
		cemuLog_log(LogType::Force, "Graphic pack download: {} failed (hr=0x{:08x} {})", stageLabel, (uint32)e.code().value, winrt::to_string(e.message()));
		return false;
	}
	catch (const std::exception& e)
	{
		cemuLog_log(LogType::Force, "Graphic pack download: {} failed ({})", stageLabel, e.what());
		return false;
	}
}

bool VersionMatchesDownloaded(const std::string& nameVersion)
{
	const auto path = ActiveSettings::GetGraphicPacksPath("downloadedGraphicPacks/version.txt");
	std::unique_ptr<FileStream> file(FileStream::openFile2(path));
	std::string versionInFile;
	if (file && file->readLine(versionInFile))
	{
		if (versionInFile.size() == nameVersion.size() &&
			std::equal(versionInFile.begin(), versionInFile.end(), nameVersion.begin(),
					   [](char a, char b) { return std::tolower((unsigned char)a) == std::tolower((unsigned char)b); }))
			return true;
	}
	return false;
}

void WriteDownloadedVersion(const std::string& nameVersion)
{
	const auto path = ActiveSettings::GetGraphicPacksPath("downloadedGraphicPacks/version.txt");
	std::unique_ptr<FileStream> file(FileStream::createFile2(path));
	if (file)
		file->writeString(nameVersion.c_str());
}

void DeleteDownloadedGraphicPacks()
{
	const auto path = ActiveSettings::GetGraphicPacksPath("downloadedGraphicPacks");
	std::error_code ec;
	if (!fs::exists(path, ec))
		return;
	for (auto& entry : fs::directory_iterator(path, ec))
		fs::remove_all(entry.path(), ec);
}

bool ExtractZip(std::vector<uint8>& zipData)
{
	zip_error_t error;
	zip_error_init(&error);
	zip_source_t* src = zip_source_buffer_create(zipData.data(), zipData.size(), 0, &error);
	if (!src)
	{
		zip_error_fini(&error);
		return false;
	}
	zip_t* za = zip_open_from_source(src, 0, &error);
	if (!za)
	{
		zip_source_free(src);
		zip_error_fini(&error);
		return false;
	}

	DeleteDownloadedGraphicPacks();
	std::error_code ec;
	fs::create_directories(ActiveSettings::GetGraphicPacksPath("downloadedGraphicPacks"), ec);

	const sint64 numEntries = zip_get_num_entries(za, 0);
	std::vector<uint8> fileBuffer;
	for (sint64 i = 0; i < numEntries; i++)
	{
		if ((i & 63) == 0)
			SetStatus(State::Working, fmt::format("Extracting... {}/{}", i, numEntries));
		zip_stat_t sb{};
		if (zip_stat_index(za, i, 0, &sb) != 0 || !sb.name)
			continue;
		if (std::strstr(sb.name, "../") || std::strstr(sb.name, "..\\"))
			continue;
		const size_t nameLen = std::strlen(sb.name);
		if (nameLen == 0)
			continue;
		const auto path = ActiveSettings::GetGraphicPacksPath("downloadedGraphicPacks/{}", sb.name);
		if (sb.name[nameLen - 1] == '/')
		{
			fs::create_directories(path, ec);
			continue;
		}
		if (sb.size == 0 || sb.size > (1024ull * 1024ull * 128ull))
			continue;
		zip_file_t* zipFile = zip_fopen_index(za, i, 0);
		if (!zipFile)
			continue;
		fileBuffer.resize(sb.size);
		if (zip_fread(zipFile, fileBuffer.data(), sb.size) == (sint64)sb.size)
		{
			fs::create_directories(path.parent_path(), ec);
			std::unique_ptr<FileStream> out(FileStream::createFile2(path));
			if (out)
				out->writeData(fileBuffer.data(), sb.size);
		}
		zip_fclose(zipFile);
	}
	zip_close(za);
	return true;
}

void WorkerThread()
{
	SetStatus(State::Working, "Checking for latest release...");
	std::vector<uint8> apiResponse;
	if (!DownloadToBuffer("https://api.github.com/repos/cemu-project/cemu_graphic_packs/releases/latest", apiResponse, "release query"))
	{
		SetStatus(State::Error, "Could not reach github.com - check the network connection");
		return;
	}

	rapidjson::Document doc;
	doc.Parse(reinterpret_cast<const char*>(apiResponse.data()), apiResponse.size());
	if (doc.HasParseError() || !doc.IsObject() || !doc.HasMember("name") || !doc["name"].IsString() ||
		!doc.HasMember("assets") || !doc["assets"].IsArray() || doc["assets"].GetArray().Empty() ||
		!doc["assets"].GetArray()[0].IsObject() || !doc["assets"].GetArray()[0].HasMember("browser_download_url") ||
		!doc["assets"].GetArray()[0]["browser_download_url"].IsString())
	{
		SetStatus(State::Error, "Unexpected response from github.com");
		return;
	}
	const std::string releaseName = doc["name"].GetString();
	const std::string downloadUrl = doc["assets"].GetArray()[0]["browser_download_url"].GetString();

	if (VersionMatchesDownloaded(releaseName))
	{
		SetStatus(State::UpToDate, fmt::format("Already up to date ({})", releaseName));
		return;
	}

	SetStatus(State::Working, fmt::format("Downloading {}...", releaseName));
	std::vector<uint8> zipData;
	if (!DownloadToBuffer(downloadUrl.c_str(), zipData, "pack archive download"))
	{
		SetStatus(State::Error, "Download failed - check the network connection");
		return;
	}

	SetStatus(State::Working, "Extracting...");
	if (!ExtractZip(zipData))
	{
		SetStatus(State::Error, "Extraction failed");
		return;
	}
	WriteDownloadedVersion(releaseName);
	SetStatus(State::Finished, fmt::format("Installed {}", releaseName));
	s_finishedPending.store(true);
}
} // anonymous namespace

State GetState()
{
	return s_state.load();
}

std::string GetStatus()
{
	std::scoped_lock lock(s_statusMutex);
	return s_status;
}

bool Start()
{
	if (s_state.load() == State::Working)
		return false;
	SetStatus(State::Working, "Starting...");
	std::thread(WorkerThread).detach();
	return true;
}

bool ConsumeFinished()
{
	return s_finishedPending.exchange(false);
}
}
