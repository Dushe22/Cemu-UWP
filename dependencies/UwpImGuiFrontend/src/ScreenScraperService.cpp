#include "UwpImGuiFrontend/ScreenScraper.h"
#include "UwpImGuiFrontend/Text.h"

#include <fmt/format.h>

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Web.Http.Headers.h>
#include <winrt/Windows.Web.Http.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <fileapifromapp.h>
#endif

namespace fs = std::filesystem;

namespace UwpImGuiFrontend
{
namespace
{
using State = ScreenScraperState;
using Request = ScreenScraperRequest;
using TitleRequest = ScreenScraperTarget;
using uint8 = std::uint8_t;
using uint32 = std::uint32_t;
using uint64 = std::uint64_t;
using sint32 = std::int32_t;
using sint64 = std::int64_t;

constexpr std::string_view kApiRoot = "https://api.screenscraper.fr/api2/";
constexpr uint32 kMaxAttempts = 3;
constexpr uint32 kRetryWaitCapMs = 60000;
constexpr uint32 kDefaultRequestIntervalMs = 1000;
constexpr uint32 kMinRequestIntervalMs = 250;
constexpr uint32 kMaxRequestIntervalMs = 10000;
// reject truncated images and text error responses
constexpr uint64 kMinImageBytes = 512;
constexpr uint64 kMinVideoBytes = 4096;
constexpr uint32 kMaxConsecutiveSoftFailures = 10;

constexpr std::string_view kMediaFileNames[] = {
	"box2d.png", "box3d.png", "mixrbv1.png", "mixrbv2.png", "fanart.png",
	"screenshot.png", "logo.png", "box2d_back.png", "support2d.png", "video.mp4"
};

std::atomic<State> s_state{ State::Idle };
std::atomic<bool> s_finishedPending{ false };
std::atomic<bool> s_running{ false };
std::atomic<bool> s_stopRequested{ false };
std::mutex s_workerMutex;
std::thread s_worker;
std::mutex s_statusMutex;
std::string s_status;
ScreenScraperLog s_log;
std::atomic<std::size_t> s_completedTargets{ 0 };
std::atomic<std::size_t> s_totalTargets{ 0 };
std::atomic<std::size_t> s_downloadedFiles{ 0 };

struct SearchPreviewRequest
{
	std::uint64_t generation = 0;
	std::uint64_t gameId = 0;
	ScreenScraperArtwork artwork = ScreenScraperArtwork::Box2D;
	std::uint32_t maximumHeight = 0;
};

std::atomic<std::uint64_t> s_searchGeneration{ 0 };
std::atomic_bool s_searchShutdown{ false };
std::mutex s_searchMutex;
std::condition_variable s_searchWake;
std::thread s_searchWorker;
std::optional<ScreenScraperSearchRequest> s_pendingSearch;
std::optional<ScreenScraperSearchRequest> s_activeSearch;
std::deque<SearchPreviewRequest> s_searchPreviews;
std::unordered_set<std::string> s_pendingPreviewKeys;
std::unordered_set<std::string> s_unavailablePreviewKeys;
ScreenScraperSearchSnapshot s_searchSnapshot;

template<typename... Args>
void Log(LogLevel level, fmt::format_string<Args...> format, Args&&... args)
{
	if (s_log)
		s_log(level, fmt::format(format, std::forward<Args>(args)...));
}

std::string PathToUtf8(const fs::path& path)
{
	const auto text = path.u8string();
	return { reinterpret_cast<const char*>(text.data()), text.size() };
}

fs::path Utf8ToPath(std::string_view value)
{
	const auto* first = reinterpret_cast<const char8_t*>(value.data());
	return fs::path(std::u8string(first, first + value.size()));
}

std::optional<std::vector<uint8>> ReadFile(const fs::path& path)
{
	std::ifstream input(path, std::ios::binary | std::ios::ate);
	if (!input)
		return std::nullopt;
	const std::streamoff length = input.tellg();
	if (length < 0)
		return std::nullopt;
	std::vector<uint8> data(static_cast<std::size_t>(length));
	input.seekg(0, std::ios::beg);
	if (!data.empty() && !input.read(reinterpret_cast<char*>(data.data()), length))
		return std::nullopt;
	return data;
}

enum class RequestResult
{
	Ok,
	NotFound,
	SoftFail,
	QuotaBlocked,
	AuthFailed,
};

struct QuotaInfo
{
	bool valid = false;
	uint32 maxThreads = 0;
	uint32 maxRequestsPerMin = 0;
	uint32 maxRequestsPerDay = 0;
	uint32 requestsToday = 0;
	uint32 requestsKoToday = 0;
	uint32 maxRequestsKoPerDay = 0;
};

struct ScrapeContext
{
	winrt::Windows::Web::Http::HttpClient client{ nullptr };
	const Request* request = nullptr;
	std::chrono::steady_clock::time_point lastRequest{};
	bool hasLastRequest = false;
	uint32 minIntervalMs = kDefaultRequestIntervalMs;
	uint32 consecutiveSoftFailures = 0;
	uint32 requestsSinceQuota = 0;
	QuotaInfo quota;
	bool quotaLogged = false;
	bool aborted = false;
	RequestResult abortResult = RequestResult::Ok;
	std::string abortReason;
};

void SetStatus(State state, const std::string& status)
{
	{
		std::scoped_lock lock(s_statusMutex);
		s_status = status;
	}
	s_state.store(state);
	Log(LogLevel::Information, "ScreenScraper: {}", status);
}

std::string LowerAscii(std::string value)
{
	std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return (char)std::tolower(c); });
	return value;
}

std::string UrlEncode(std::string_view value)
{
	std::ostringstream encoded;
	encoded << std::uppercase << std::hex;
	for (unsigned char c : value)
	{
		const bool unreserved = std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~';
		if (unreserved)
			encoded << (char)c;
		else
			encoded << '%' << std::setw(2) << std::setfill('0') << (int)c;
	}
	return encoded.str();
}

void AppendParam(std::string& query, std::string_view key, std::string_view value)
{
	if (!query.empty())
		query += '&';
	query += key;
	query += '=';
	query += UrlEncode(value);
}

std::string BuildApiUrl(std::string_view endpoint, const Request& request, std::initializer_list<std::pair<std::string_view, std::string_view>> params)
{
	std::string query;
	AppendParam(query, "devid", request.developer.id);
	AppendParam(query, "devpassword", request.developer.password);
	AppendParam(query, "softname", request.developer.softwareName);
	if (!request.account.user.empty())
		AppendParam(query, "ssid", request.account.user);
	if (!request.account.password.empty())
		AppendParam(query, "sspassword", request.account.password);
	for (const auto& [key, value] : params)
		AppendParam(query, key, value);

	return fmt::format("{}{}?{}", kApiRoot, endpoint, query);
}

bool SleepInterruptible(uint32 milliseconds)
{
	constexpr uint32 sliceMs = 100;
	uint32 slept = 0;
	while (slept < milliseconds)
	{
		if (s_stopRequested.load())
			return false;
		const uint32 slice = std::min(sliceMs, milliseconds - slept);
		std::this_thread::sleep_for(std::chrono::milliseconds(slice));
		slept += slice;
	}
	return !s_stopRequested.load();
}

template<typename TAsyncOperation>
bool WaitForHttpOperation(TAsyncOperation operation)
{
	using winrt::Windows::Foundation::AsyncStatus;
	while (operation.Status() == AsyncStatus::Started)
	{
		if (s_stopRequested.load())
		{
			operation.Cancel();
			return false;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(25));
	}
	return !s_stopRequested.load();
}

void PaceRequest(ScrapeContext& ctx)
{
	if (ctx.hasLastRequest)
	{
		const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - ctx.lastRequest).count();
		if (elapsedMs < (sint64)ctx.minIntervalMs)
			SleepInterruptible((uint32)((sint64)ctx.minIntervalMs - elapsedMs));
	}
	ctx.lastRequest = std::chrono::steady_clock::now();
	ctx.hasLastRequest = true;
}

void AbortRun(ScrapeContext& ctx, RequestResult result, std::string reason)
{
	if (ctx.aborted)
		return;
	ctx.aborted = true;
	ctx.abortResult = result;
	ctx.abortReason = std::move(reason);
	Log(LogLevel::Information, "ScreenScraper: stopping the run ({})", ctx.abortReason);
}

uint32 ReadRetryAfterMs(const winrt::Windows::Web::Http::HttpResponseMessage& response)
{
	try
	{
		auto retryAfter = response.Headers().RetryAfter();
		if (!retryAfter)
			return 0;
		if (auto delta = retryAfter.Delta())
		{
			const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(delta.Value()).count();
			if (milliseconds > 0)
				return (uint32)std::min<int64_t>(milliseconds, kRetryWaitCapMs);
		}
	}
	catch (const winrt::hresult_error&)
	{
	}
	return 0;
}

RequestResult MapStatusCode(uint32 statusCode, bool& retryable, bool& escalateToQuota, std::string& reason)
{
	retryable = false;
	escalateToQuota = false;
	switch (statusCode)
	{
	case 400:
		reason = "malformed request";
		return RequestResult::SoftFail;
	case 401:
		reason = "server busy or API closed to non-members";
		retryable = true;
		return RequestResult::SoftFail;
	case 403:
		reason = "ScreenScraper rejected the developer credentials";
		return RequestResult::AuthFailed;
	case 404:
		reason = "no match";
		return RequestResult::NotFound;
	case 423:
		reason = "the ScreenScraper API is closed";
		return RequestResult::QuotaBlocked;
	case 426:
		reason = "this account or software is blacklisted by ScreenScraper";
		return RequestResult::AuthFailed;
	case 429:
		reason = "rate limited";
		retryable = true;
		escalateToQuota = true;
		return RequestResult::SoftFail;
	case 430:
		reason = "daily request quota exhausted";
		return RequestResult::QuotaBlocked;
	case 431:
		reason = "daily quota for unrecognised ROMs exhausted";
		return RequestResult::QuotaBlocked;
	default:
		break;
	}
	if (statusCode >= 500)
	{
		reason = fmt::format("server error HTTP {}", statusCode);
		retryable = true;
		return RequestResult::SoftFail;
	}
	reason = fmt::format("HTTP {}", statusCode);
	return RequestResult::SoftFail;
}

RequestResult Fetch(ScrapeContext& ctx, const std::string& url, std::vector<uint8>& buffer, const std::string& label)
{
	buffer.clear();
	uint32 backoffMs = 1000;
	for (uint32 attempt = 1; attempt <= kMaxAttempts; ++attempt)
	{
		if (s_stopRequested.load() || ctx.aborted)
			return RequestResult::SoftFail;

		PaceRequest(ctx);
		if (s_stopRequested.load() || ctx.aborted)
			return RequestResult::SoftFail;
		ctx.requestsSinceQuota++;

		uint32 statusCode = 0;
		uint32 retryAfterMs = 0;
		bool transportError = false;
		std::string reason;
		try
		{
			winrt::Windows::Foundation::Uri uri{ winrt::to_hstring(url) };
			auto responseOperation = ctx.client.GetAsync(uri);
			if (!WaitForHttpOperation(responseOperation))
				return RequestResult::SoftFail;
			auto response = responseOperation.GetResults();
			statusCode = (uint32)response.StatusCode();
			retryAfterMs = ReadRetryAfterMs(response);
			if (statusCode >= 200 && statusCode < 300)
			{
				auto bufferOperation = response.Content().ReadAsBufferAsync();
				if (!WaitForHttpOperation(bufferOperation))
					return RequestResult::SoftFail;
				auto responseBuffer = bufferOperation.GetResults();
				buffer.resize(responseBuffer.Length());
				if (!buffer.empty())
				{
					auto reader = winrt::Windows::Storage::Streams::DataReader::FromBuffer(responseBuffer);
					reader.ReadBytes(winrt::array_view<uint8_t>(buffer.data(), buffer.data() + buffer.size()));
				}
				ctx.consecutiveSoftFailures = 0;
				Log(LogLevel::Information, "ScreenScraper: {} ok ({} bytes)", label, buffer.size());
				return RequestResult::Ok;
			}
		}
		catch (const winrt::hresult_error& e)
		{
			transportError = true;
			reason = fmt::format("hr=0x{:08x}", (uint32)e.code().value);
		}
		catch (const std::exception&)
		{
			transportError = true;
			reason = "transport exception";
		}

		bool retryable = false;
		bool escalateToQuota = false;
		RequestResult result = RequestResult::SoftFail;
		if (transportError)
			retryable = true;
		else
			result = MapStatusCode(statusCode, retryable, escalateToQuota, reason);

		if (result == RequestResult::NotFound)
		{
			ctx.consecutiveSoftFailures = 0;
			Log(LogLevel::Information, "ScreenScraper: {} skipped (HTTP {}, {})", label, statusCode, reason);
			return RequestResult::NotFound;
		}
		if (result == RequestResult::QuotaBlocked || result == RequestResult::AuthFailed)
		{
			Log(LogLevel::Information, "ScreenScraper: {} failed (HTTP {}, {})", label, statusCode, reason);
			AbortRun(ctx, result, reason);
			return result;
		}

		const bool lastAttempt = (attempt == kMaxAttempts);
		if (!retryable || lastAttempt)
		{
			if (escalateToQuota)
			{
				Log(LogLevel::Information, "ScreenScraper: {} failed (HTTP {}, {} after {} attempts)", label, statusCode, reason, attempt);
				AbortRun(ctx, RequestResult::QuotaBlocked, reason);
				return RequestResult::QuotaBlocked;
			}
			ctx.consecutiveSoftFailures++;
			Log(LogLevel::Information, "ScreenScraper: {} failed (HTTP {}, {}) after {} attempt{}", label, statusCode, reason, attempt, attempt == 1 ? "" : "s");
			if (ctx.consecutiveSoftFailures >= kMaxConsecutiveSoftFailures)
				AbortRun(ctx, RequestResult::SoftFail, "ScreenScraper is unreachable or keeps refusing requests");
			return RequestResult::SoftFail;
		}

		const uint32 waitMs = std::min(std::max(retryAfterMs, backoffMs), kRetryWaitCapMs);
		Log(LogLevel::Information, "ScreenScraper: {} retry {}/{} in {} ms (HTTP {}, {})", label, attempt + 1, kMaxAttempts, waitMs, statusCode, reason);
		if (!SleepInterruptible(waitMs))
			return RequestResult::SoftFail;
		backoffMs *= 2;
	}
	return RequestResult::SoftFail;
}

bool LooksLikeApiTextError(const std::vector<uint8>& buffer)
{
	if (buffer.empty() || buffer.size() > 4096)
		return false;

	std::string text(reinterpret_cast<const char*>(buffer.data()), buffer.size());
	std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return (char)std::toupper(c); });
	return text.find("NOMEDIA") != std::string::npos ||
		   text.find("CRCOK") != std::string::npos ||
		   text.find("MD5OK") != std::string::npos ||
		   text.find("SHA1OK") != std::string::npos ||
		   text.find("ERREUR") != std::string::npos ||
		   text.find("ERROR") != std::string::npos;
}

bool LooksLikePng(const std::vector<uint8>& buffer)
{
	static constexpr uint8 pngSig[] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
	return buffer.size() > sizeof(pngSig) && std::equal(std::begin(pngSig), std::end(pngSig), buffer.begin());
}

bool LooksLikeJpeg(const std::vector<uint8>& buffer)
{
	return buffer.size() > 3 && buffer[0] == 0xFF && buffer[1] == 0xD8 && buffer[2] == 0xFF;
}

bool LooksLikeWebp(const std::vector<uint8>& buffer)
{
	return buffer.size() > 12 &&
		   buffer[0] == 'R' && buffer[1] == 'I' && buffer[2] == 'F' && buffer[3] == 'F' &&
		   buffer[8] == 'W' && buffer[9] == 'E' && buffer[10] == 'B' && buffer[11] == 'P';
}

// ScreenScraper ignores outputformat for some media, fanart always arrives as jpeg
std::string_view ImageExtension(const std::vector<uint8>& buffer)
{
	if (LooksLikePng(buffer))
		return ".png";
	if (LooksLikeJpeg(buffer))
		return ".jpg";
	if (LooksLikeWebp(buffer))
		return ".webp";
	return {};
}

bool LooksLikeMp4(const std::vector<uint8>& buffer)
{
	return buffer.size() > 12 &&
		   buffer[4] == 'f' && buffer[5] == 't' && buffer[6] == 'y' && buffer[7] == 'p';
}

bool WriteFileAtomic(const fs::path& path, const void* data, size_t size)
{
	std::error_code ec;
	fs::create_directories(path.parent_path(), ec);

	fs::path tempPath = path;
	tempPath += ".tmp";
	fs::remove(tempPath, ec);

	{
		std::ofstream file(tempPath, std::ios::binary | std::ios::trunc);
		if (!file)
		{
			Log(LogLevel::Information, "ScreenScraper: cannot create {}", PathToUtf8(tempPath));
			return false;
		}
		if (size != 0)
			file.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
		file.flush();
		if (!file)
		{
			Log(LogLevel::Information, "ScreenScraper: failed to write {} ({} bytes)", PathToUtf8(tempPath), size);
			file.close();
			fs::remove(tempPath, ec);
			return false;
		}
	}

#if defined(_WIN32)
	const bool targetExists = fs::exists(path, ec) && !ec;
	const BOOL replaced = targetExists ?
		ReplaceFileFromAppW(path.c_str(), tempPath.c_str(), nullptr,
			REPLACEFILE_IGNORE_MERGE_ERRORS, nullptr, nullptr) :
		MoveFileFromAppW(tempPath.c_str(), path.c_str());
	if (!replaced)
	{
		const DWORD lastError = GetLastError();
		Log(LogLevel::Information, "ScreenScraper: failed to replace {} (Win32 error {})",
			PathToUtf8(path), lastError);
		std::error_code removeEc;
		fs::remove(tempPath, removeEc);
		return false;
	}
	return true;
#else
	ec.clear();
	fs::rename(tempPath, path, ec);
	if (!ec)
		return true;
	Log(LogLevel::Information, "ScreenScraper: failed to replace {} ({})",
		PathToUtf8(path), ec.message());
	std::error_code removeEc;
	fs::remove(tempPath, removeEc);
	return false;
#endif
}

bool WriteFileAtomic(const fs::path& path, const std::vector<uint8>& buffer)
{
	return WriteFileAtomic(path, buffer.data(), buffer.size());
}

bool WriteTextFileAtomic(const fs::path& path, const std::string& text)
{
	return WriteFileAtomic(path, text.data(), text.size());
}

fs::path ManifestPath(const fs::path& mediaDirectory)
{
	return mediaDirectory / "media.json";
}

bool FileLooksComplete(const fs::path& path, uint64 minimumSize)
{
	std::error_code ec;
	if (!fs::exists(path, ec) || fs::is_directory(path, ec))
		return false;
	const auto size = fs::file_size(path, ec);
	if (ec)
		return false;
	return size >= minimumSize;
}

bool ShouldSkipExisting(const fs::path& path, bool overwriteExisting, uint64 minimumSize)
{
	if (overwriteExisting)
		return false;
	return FileLooksComplete(path, minimumSize);
}

std::string CurrentTimestamp()
{
	const std::time_t now = std::time(nullptr);
	std::tm utc{};
	if (gmtime_s(&utc, &now) != 0)
		return fmt::format("{}", (uint64)now);
	char buffer[32]{};
	if (std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &utc) == 0)
		return fmt::format("{}", (uint64)now);
	return buffer;
}

std::string CleanSearchName(std::string value)
{
	for (char& c : value)
	{
		if (c == '_' || c == '.')
			c = ' ';
	}
	std::string cleaned;
	cleaned.reserve(value.size());
	int parenDepth = 0;
	int bracketDepth = 0;
	for (char c : value)
	{
		if (c == '(')
		{
			parenDepth++;
			continue;
		}
		if (c == ')' && parenDepth > 0)
		{
			parenDepth--;
			continue;
		}
		if (c == '[')
		{
			bracketDepth++;
			continue;
		}
		if (c == ']' && bracketDepth > 0)
		{
			bracketDepth--;
			continue;
		}
		if (parenDepth == 0 && bracketDepth == 0)
			cleaned.push_back(c);
	}
	std::string collapsed;
	collapsed.reserve(cleaned.size());
	for (char c : cleaned)
	{
		if (c == ' ' && !collapsed.empty() && collapsed.back() == ' ')
			continue;
		collapsed.push_back(c);
	}
	while (!collapsed.empty() && std::isspace((unsigned char)collapsed.front()))
		collapsed.erase(collapsed.begin());
	while (!collapsed.empty() && std::isspace((unsigned char)collapsed.back()))
		collapsed.pop_back();
	return collapsed.empty() ? value : collapsed;
}

void AddUnique(std::vector<std::string>& values, std::string value)
{
	if (value.empty())
		return;
	if (std::find(values.begin(), values.end(), value) == values.end())
		values.emplace_back(std::move(value));
}

std::vector<std::string> RomNameCandidates(const TitleRequest& title)
{
	std::vector<std::string> candidates;
	const std::string extension = title.contentPath.has_extension() ? PathToUtf8(title.contentPath.extension()) : ".wua";
	AddUnique(candidates, title.name + extension);
	if (!title.contentPath.empty())
		AddUnique(candidates, PathToUtf8(title.contentPath.filename()));
	AddUnique(candidates, title.name);
	AddUnique(candidates, CleanSearchName(title.name) + extension);
	if (!title.contentPath.empty())
		AddUnique(candidates, CleanSearchName(PathToUtf8(title.contentPath.stem())) + extension);
	return candidates;
}

std::vector<std::string> SearchNameCandidates(const TitleRequest& title)
{
	std::vector<std::string> candidates;
	AddUnique(candidates, title.name);
	AddUnique(candidates, CleanSearchName(title.name));
	if (!title.contentPath.empty())
	{
		AddUnique(candidates, PathToUtf8(title.contentPath.stem()));
		AddUnique(candidates, CleanSearchName(PathToUtf8(title.contentPath.stem())));
	}
	return candidates;
}

std::optional<std::string> StringFromJsonValue(const rapidjson::Value& value)
{
	if (value.IsString())
		return NormalizeScreenScraperText(
			std::string_view(value.GetString(), value.GetStringLength()));
	if (value.IsUint64())
		return fmt::format("{}", value.GetUint64());
	if (value.IsInt64())
		return fmt::format("{}", value.GetInt64());
	if (value.IsUint())
		return fmt::format("{}", value.GetUint());
	if (value.IsInt())
		return fmt::format("{}", value.GetInt());
	return std::nullopt;
}

std::string StringMember(const rapidjson::Value& object, const char* key)
{
	if (!object.IsObject())
		return {};
	auto it = object.FindMember(key);
	if (it == object.MemberEnd())
		return {};
	if (auto value = StringFromJsonValue(it->value))
		return *value;
	return {};
}

std::optional<uint64> ParseUint64(std::string_view text)
{
	uint64 value = 0;
	bool any = false;
	for (char c : text)
	{
		if (c < '0' || c > '9')
		{
			if (any)
				break;
			if (std::isspace((unsigned char)c))
				continue;
			return std::nullopt;
		}
		value = value * 10 + (uint64)(c - '0');
		any = true;
	}
	return any ? std::optional<uint64>(value) : std::nullopt;
}

uint32 UintMember(const rapidjson::Value& object, const char* key)
{
	const std::string text = StringMember(object, key);
	if (text.empty())
		return 0;
	if (auto value = ParseUint64(text))
		return (uint32)std::min<uint64>(*value, 0xFFFFFFFFull);
	return 0;
}

struct MediaEntry
{
	std::string type;
	std::string parent;
	std::string region;
	std::string format;
	std::string url;
};

struct GameInfo
{
	std::string gameId;
	std::string json;
	std::vector<MediaEntry> medias;
};

const rapidjson::Value* FindGameValue(const rapidjson::Document& doc)
{
	if (!doc.IsObject())
		return nullptr;
	auto responseIt = doc.FindMember("response");
	if (responseIt == doc.MemberEnd() || !responseIt->value.IsObject())
		return nullptr;

	const rapidjson::Value& response = responseIt->value;
	if (auto jeuIt = response.FindMember("jeu"); jeuIt != response.MemberEnd())
	{
		if (jeuIt->value.IsObject())
			return &jeuIt->value;
		if (jeuIt->value.IsArray() && !jeuIt->value.Empty())
			return &jeuIt->value.GetArray()[0];
	}
	if (auto jeuxIt = response.FindMember("jeux"); jeuxIt != response.MemberEnd())
	{
		if (jeuxIt->value.IsArray() && !jeuxIt->value.Empty())
			return &jeuxIt->value.GetArray()[0];
		if (jeuxIt->value.IsObject())
			return &jeuxIt->value;
	}
	return nullptr;
}

std::optional<std::string> ExtractGameId(const rapidjson::Value& game)
{
	if (!game.IsObject())
		return std::nullopt;
	for (const char* key : { "id", "idjeu", "gameid" })
	{
		auto it = game.FindMember(key);
		if (it != game.MemberEnd())
		{
			if (auto id = StringFromJsonValue(it->value))
				return id;
		}
	}
	return std::nullopt;
}

std::vector<MediaEntry> ExtractMedias(const rapidjson::Value& game)
{
	std::vector<MediaEntry> medias;
	if (!game.IsObject())
		return medias;
	auto it = game.FindMember("medias");
	if (it == game.MemberEnd() || !it->value.IsArray())
		return medias;
	for (const auto& value : it->value.GetArray())
	{
		if (!value.IsObject())
			continue;
		MediaEntry entry;
		entry.type = StringMember(value, "type");
		entry.parent = StringMember(value, "parent");
		entry.region = StringMember(value, "region");
		entry.format = StringMember(value, "format");
		entry.url = StringMember(value, "url");
		if (entry.type.empty())
			continue;
		medias.emplace_back(std::move(entry));
	}
	return medias;
}

QuotaInfo ExtractQuota(const rapidjson::Document& doc)
{
	QuotaInfo quota;
	if (!doc.IsObject())
		return quota;
	auto responseIt = doc.FindMember("response");
	if (responseIt == doc.MemberEnd() || !responseIt->value.IsObject())
		return quota;
	auto userIt = responseIt->value.FindMember("ssuser");
	if (userIt == responseIt->value.MemberEnd() || !userIt->value.IsObject())
		return quota;

	const rapidjson::Value& user = userIt->value;
	quota.maxThreads = UintMember(user, "maxthreads");
	quota.maxRequestsPerMin = UintMember(user, "maxrequestspermin");
	quota.maxRequestsPerDay = UintMember(user, "maxrequestsperday");
	quota.requestsToday = UintMember(user, "requeststoday");
	quota.requestsKoToday = UintMember(user, "requestskotoday");
	quota.maxRequestsKoPerDay = UintMember(user, "maxrequestskoperday");
	quota.valid = quota.maxRequestsPerDay != 0 || quota.maxRequestsPerMin != 0 || quota.requestsToday != 0;
	return quota;
}

void ApplyQuota(ScrapeContext& ctx, const QuotaInfo& quota)
{
	if (!quota.valid)
		return;
	ctx.quota = quota;
	ctx.requestsSinceQuota = 0;

	const uint32 previousInterval = ctx.minIntervalMs;
	if (quota.maxRequestsPerMin > 0)
		ctx.minIntervalMs = std::clamp(60000u / quota.maxRequestsPerMin + 50u, kMinRequestIntervalMs, kMaxRequestIntervalMs);
	else
		ctx.minIntervalMs = kDefaultRequestIntervalMs;

	if (!ctx.quotaLogged || previousInterval != ctx.minIntervalMs)
	{
		Log(LogLevel::Information, "ScreenScraper: quota {}/{} requests today, {}/{} failed lookups, {} req/min, {} threads (pacing {} ms)",
					quota.requestsToday, quota.maxRequestsPerDay, quota.requestsKoToday, quota.maxRequestsKoPerDay,
					quota.maxRequestsPerMin, quota.maxThreads, ctx.minIntervalMs);
		ctx.quotaLogged = true;
	}

	if (quota.maxRequestsPerDay > 0 && quota.requestsToday >= quota.maxRequestsPerDay)
		AbortRun(ctx, RequestResult::QuotaBlocked, fmt::format("daily quota reached ({}/{} requests)", quota.requestsToday, quota.maxRequestsPerDay));
	else if (quota.maxRequestsKoPerDay > 0 && quota.requestsKoToday >= quota.maxRequestsKoPerDay)
		AbortRun(ctx, RequestResult::QuotaBlocked, fmt::format("daily quota for unrecognised ROMs reached ({}/{})", quota.requestsKoToday, quota.maxRequestsKoPerDay));
}

std::string QuotaSuffix(const ScrapeContext& ctx)
{
	if (!ctx.quota.valid || ctx.quota.maxRequestsPerDay == 0)
		return {};
	return fmt::format(" - {}/{} API requests today", ctx.quota.requestsToday + ctx.requestsSinceQuota, ctx.quota.maxRequestsPerDay);
}

std::string RegionCode(const TitleRequest& title, ScreenScraperRegion preferredRegion)
{
	return ResolveScreenScraperRegionCode(preferredRegion, title.region);
}

enum class TitleLookup
{
	Found,
	NotFound,
	Failed,
};

bool ParseGameResponse(const std::vector<uint8>& response, GameInfo& info, QuotaInfo& quota)
{
	if (response.empty())
		return false;
	std::string json(reinterpret_cast<const char*>(response.data()), response.size());
	rapidjson::Document doc;
	doc.Parse<rapidjson::kParseStopWhenDoneFlag>(json.data(), json.size());
	if (doc.HasParseError())
		return false;

	quota = ExtractQuota(doc);
	const rapidjson::Value* game = FindGameValue(doc);
	if (!game)
		return false;
	auto gameId = ExtractGameId(*game);
	if (!gameId)
		return false;

	info.gameId = std::move(*gameId);
	info.medias = ExtractMedias(*game);
	info.json = std::move(json);
	return true;
}

TitleLookup QueryGameInfoByRomName(ScrapeContext& ctx, const TitleRequest& title, GameInfo& info)
{
	const Request& request = *ctx.request;
	std::vector<uint8> response;
	bool sawNotFound = false;
	bool sawFailure = false;
	for (const auto& romName : RomNameCandidates(title))
	{
		if (s_stopRequested.load() || ctx.aborted)
			return TitleLookup::Failed;

		const std::string systemId = fmt::format("{}", title.systemId);
		const std::string url = BuildApiUrl("jeuInfos.php", request, {
			{ "output", "json" },
			{ "systemeid", systemId },
			{ "romtype", "rom" },
			{ "romnom", romName },
		});
		const auto result = Fetch(ctx, url, response, fmt::format("info '{}'", romName));
		if (result == RequestResult::QuotaBlocked || result == RequestResult::AuthFailed)
			return TitleLookup::Failed;
		if (result == RequestResult::NotFound)
		{
			sawNotFound = true;
			continue;
		}
		if (result != RequestResult::Ok)
		{
			sawFailure = true;
			continue;
		}
		if (LooksLikeApiTextError(response))
		{
			Log(LogLevel::Information, "ScreenScraper: info '{}' skipped (API returned a text error)", romName);
			sawNotFound = true;
			continue;
		}

		QuotaInfo quota;
		GameInfo parsed;
		const bool ok = ParseGameResponse(response, parsed, quota);
		ApplyQuota(ctx, quota);
		if (ctx.aborted)
			return TitleLookup::Failed;
		if (!ok)
		{
			Log(LogLevel::Information, "ScreenScraper: info '{}' skipped (invalid or incomplete JSON response)", romName);
			sawFailure = true;
			continue;
		}
		info = std::move(parsed);
		return TitleLookup::Found;
	}
	return sawNotFound && !sawFailure ?
		TitleLookup::NotFound : TitleLookup::Failed;
}

std::optional<std::string> QueryGameIdBySearch(ScrapeContext& ctx,
	const TitleRequest& title, bool& sawNotFound, bool& sawFailure)
{
	const Request& request = *ctx.request;
	std::vector<uint8> response;
	for (const auto& name : SearchNameCandidates(title))
	{
		if (s_stopRequested.load() || ctx.aborted)
			return std::nullopt;

		const std::string systemId = fmt::format("{}", title.systemId);
		const std::string url = BuildApiUrl("jeuRecherche.php", request, {
			{ "output", "json" },
			{ "systemeid", systemId },
			{ "recherche", name },
		});
		const auto result = Fetch(ctx, url, response, fmt::format("search '{}'", name));
		if (result == RequestResult::QuotaBlocked || result == RequestResult::AuthFailed)
			return std::nullopt;
		if (result == RequestResult::NotFound)
		{
			sawNotFound = true;
			continue;
		}
		if (result != RequestResult::Ok)
		{
			sawFailure = true;
			continue;
		}
		if (LooksLikeApiTextError(response))
		{
			Log(LogLevel::Information, "ScreenScraper: search '{}' skipped (API returned a text error)", name);
			sawNotFound = true;
			continue;
		}

		std::string json(reinterpret_cast<const char*>(response.data()), response.size());
		rapidjson::Document doc;
		doc.Parse<rapidjson::kParseStopWhenDoneFlag>(json.data(), json.size());
		if (doc.HasParseError())
		{
			Log(LogLevel::Information, "ScreenScraper: search '{}' skipped (malformed JSON response)", name);
			sawFailure = true;
			continue;
		}
		ApplyQuota(ctx, ExtractQuota(doc));
		if (ctx.aborted)
			return std::nullopt;
		const rapidjson::Value* game = FindGameValue(doc);
		if (!game)
		{
			sawNotFound = true;
			continue;
		}
		if (auto id = ExtractGameId(*game))
			return id;
		sawFailure = true;
	}
	return std::nullopt;
}

// search results require a game-info request to enumerate media
TitleLookup QueryGameInfoById(ScrapeContext& ctx, const std::string& gameId,
	std::uint32_t targetSystemId, GameInfo& info)
{
	const Request& request = *ctx.request;
	const std::string systemId = fmt::format("{}", targetSystemId);
	const std::string url = BuildApiUrl("jeuInfos.php", request, {
		{ "output", "json" },
		{ "systemeid", systemId },
		{ "gameid", gameId },
	});
	std::vector<uint8> response;
	const auto result = Fetch(ctx, url, response, fmt::format("info gameid {}", gameId));
	if (result == RequestResult::QuotaBlocked || result == RequestResult::AuthFailed)
		return TitleLookup::Failed;
	if (result == RequestResult::NotFound)
		return TitleLookup::NotFound;
	if (result != RequestResult::Ok)
		return TitleLookup::Failed;
	if (LooksLikeApiTextError(response))
	{
		Log(LogLevel::Information, "ScreenScraper: info gameid {} skipped (API returned a text error)", gameId);
		return TitleLookup::NotFound;
	}

	QuotaInfo quota;
	GameInfo parsed;
	const bool ok = ParseGameResponse(response, parsed, quota);
	ApplyQuota(ctx, quota);
	if (ctx.aborted)
		return TitleLookup::Failed;
	if (!ok)
	{
		Log(LogLevel::Information, "ScreenScraper: info gameid {} skipped (invalid or incomplete JSON response)", gameId);
		return TitleLookup::Failed;
	}
	info = std::move(parsed);
	return TitleLookup::Found;
}

TitleLookup QueryGame(ScrapeContext& ctx, const TitleRequest& title, GameInfo& info)
{
	if (title.matchedGameId != 0)
		return QueryGameInfoById(ctx, fmt::format("{}", title.matchedGameId), title.systemId, info);

	const auto byRom = QueryGameInfoByRomName(ctx, title, info);
	if (byRom == TitleLookup::Found)
		return byRom;
	if (ctx.aborted || s_stopRequested.load())
		return TitleLookup::Failed;

	bool sawNotFound = (byRom == TitleLookup::NotFound);
	bool sawFailure = (byRom == TitleLookup::Failed);
	auto gameId = QueryGameIdBySearch(ctx, title, sawNotFound, sawFailure);
	if (ctx.aborted || s_stopRequested.load())
		return TitleLookup::Failed;
	if (!gameId)
		return sawNotFound && !sawFailure ?
			TitleLookup::NotFound : TitleLookup::Failed;

	return QueryGameInfoById(ctx, *gameId, title.systemId, info);
}

struct MediaGroup
{
	std::string fileName;
	std::vector<std::string> types; // preference order, first available one wins
	bool video = false;
};

std::optional<ScreenScraperArtwork> ArtworkForLogicalFile(
	std::string_view fileName)
{
	if (fileName == "box2d.png") return ScreenScraperArtwork::Box2D;
	if (fileName == "box3d.png") return ScreenScraperArtwork::Box3D;
	if (fileName == "mixrbv1.png") return ScreenScraperArtwork::MixRecalboxV1;
	if (fileName == "mixrbv2.png") return ScreenScraperArtwork::MixRecalboxV2;
	if (fileName == "fanart.png") return ScreenScraperArtwork::FanArt;
	if (fileName == "screenshot.png") return ScreenScraperArtwork::Screenshot;
	if (fileName == "logo.png") return ScreenScraperArtwork::Logo;
	if (fileName == "box2d_back.png") return ScreenScraperArtwork::BackCover;
	if (fileName == "support2d.png") return ScreenScraperArtwork::PhysicalMedia;
	if (fileName == "titlescreen.png") return ScreenScraperArtwork::TitleScreen;
	return std::nullopt;
}

fs::path CanonicalMediaPath(const TitleRequest& title,
	std::string_view fileName)
{
	if (title.sharedMedia)
	{
		if (fileName == "video.mp4")
			return ResolveSharedMediaVideoPath(*title.sharedMedia);
		if (const auto artwork = ArtworkForLogicalFile(fileName))
			return ResolveSharedMediaArtworkPath(*title.sharedMedia, *artwork);
	}
	return title.mediaDirectory / fs::path(fileName);
}

std::optional<fs::path> ExistingMediaPath(const TitleRequest& title,
	std::string_view fileName)
{
	if (title.sharedMedia)
	{
		if (fileName == "video.mp4")
			return FindExistingSharedMediaVideoPath(*title.sharedMedia);
		if (const auto artwork = ArtworkForLogicalFile(fileName))
			return FindExistingSharedMediaArtworkPath(*title.sharedMedia, *artwork);
	}

	const fs::path path = CanonicalMediaPath(title, fileName);
	std::error_code error;
	if (!path.empty() && fs::exists(path, error) &&
		!fs::is_directory(path, error))
	{
		return path;
	}
	return std::nullopt;
}

std::vector<MediaGroup> BuildMediaGroups(const Request& request)
{
	std::vector<MediaGroup> groups;
	if (request.options.downloadBoxArt)
	{
		groups.push_back({ "box2d.png", { "box-2D" }, false });
		groups.push_back({ "box3d.png", { "box-3D" }, false });
		groups.push_back({ "mixrbv1.png", { "mixrbv1" }, false });
		groups.push_back({ "mixrbv2.png", { "mixrbv2" }, false });
		groups.push_back({ "box2d_back.png", { "box-2D-back" }, false });
		groups.push_back({ "support2d.png", { "support-2D" }, false });
	}
	if (request.options.downloadScreenshots)
	{
		groups.push_back({ "screenshot.png", { "ss" }, false });
		groups.push_back({ "titlescreen.png", { "sstitle" }, false });
	}
	if (request.options.downloadFanArt)
		groups.push_back({ "fanart.png", { "fanart" }, false });
	if (request.options.downloadLogos)
		groups.push_back({ "logo.png", { "wheel-hd", "wheel" }, false });
	if (request.options.downloadVideos)
		groups.push_back({ "video.mp4", { "video" }, true });
	return groups;
}

std::string JoinTypes(const std::vector<std::string>& types)
{
	std::string joined;
	for (const std::string& type : types)
	{
		if (!joined.empty())
			joined += '/';
		joined += type;
	}
	return joined;
}

struct MediaChoice
{
	std::string type;
	std::string region;
};

std::optional<MediaChoice> ChooseMedia(const std::vector<MediaEntry>& medias, const std::vector<std::string>& types, const std::string& preferredRegion)
{
	std::vector<std::string> regionOrder;
	AddUnique(regionOrder, preferredRegion);
	AddUnique(regionOrder, "wor");
	AddUnique(regionOrder, "us");
	AddUnique(regionOrder, "eu");
	AddUnique(regionOrder, "jp");

	for (const std::string& type : types)
	{
		const std::string wanted = LowerAscii(type);
		std::vector<const MediaEntry*> matches;
		for (const MediaEntry& entry : medias)
		{
			if (LowerAscii(entry.type) != wanted)
				continue;
			if (!entry.parent.empty() && LowerAscii(entry.parent) != "jeu")
				continue;
			matches.push_back(&entry);
		}
		if (matches.empty())
			continue;
		for (const std::string& region : regionOrder)
		{
			for (const MediaEntry* entry : matches)
			{
				if (LowerAscii(entry->region) == region)
					return MediaChoice{ entry->type, entry->region };
			}
		}
		return MediaChoice{ matches.front()->type, matches.front()->region };
	}
	return std::nullopt;
}

RequestResult FetchMediaFile(ScrapeContext& ctx, const std::string& gameId,
	std::uint32_t targetSystemId, const MediaChoice& choice, const fs::path& target, bool video)
{
	const Request& request = *ctx.request;
	const std::string systemId = fmt::format("{}", targetSystemId);
	const std::string mediaParam = (!video && !choice.region.empty()) ? fmt::format("{}({})", choice.type, choice.region) : choice.type;
	const std::string endpoint = video ? "mediaVideoJeu.php" : "mediaJeu.php";
	const std::string url = video
		? BuildApiUrl(endpoint, request, {
			  { "systemeid", systemId },
			  { "jeuid", gameId },
			  { "media", mediaParam },
		  })
		: BuildApiUrl(endpoint, request, {
			  { "systemeid", systemId },
			  { "jeuid", gameId },
			  { "media", mediaParam },
			  { "outputformat", "png" },
		  });

	std::vector<uint8> data;
	const std::string label = fmt::format("media {} (game {})", mediaParam, gameId);
	const auto result = Fetch(ctx, url, data, label);
	if (result != RequestResult::Ok)
		return result;

	if (LooksLikeApiTextError(data))
	{
		Log(LogLevel::Information, "ScreenScraper: {} skipped (API returned a text error)", label);
		return RequestResult::NotFound;
	}
	const std::string_view imageExtension = video ? std::string_view{} : ImageExtension(data);
	if (video ? !LooksLikeMp4(data) : imageExtension.empty())
	{
		Log(LogLevel::Information, "ScreenScraper: {} skipped ({}, {} bytes)", label,
			video ? "not-mp4" : "not an image", data.size());
		return RequestResult::SoftFail;
	}
	if (data.size() < (video ? kMinVideoBytes : kMinImageBytes))
	{
		Log(LogLevel::Information, "ScreenScraper: {} skipped (truncated, {} bytes)", label, data.size());
		return RequestResult::SoftFail;
	}
	fs::path destination = target;
	if (!imageExtension.empty() && PathToUtf8(target.extension()) != imageExtension)
		destination.replace_extension(imageExtension);
	if (!WriteFileAtomic(destination, data))
		return RequestResult::SoftFail;
	return RequestResult::Ok;
}

RequestResult FetchPlatformMedia(ScrapeContext& ctx, const std::string& media,
	const fs::path& target, bool video, bool overwriteExisting)
{
	const Request& request = *ctx.request;
	if (ShouldSkipExisting(target, overwriteExisting,
		video ? kMinVideoBytes : kMinImageBytes))
	{
		Log(LogLevel::Information, "ScreenScraper: platform {} skipped (already present)", media);
		return RequestResult::NotFound;
	}

	const std::string systemId = fmt::format("{}", request.platformSystemId);
	const std::string endpoint = video ? "mediaVideoSysteme.php" : "mediaSysteme.php";
	const std::string url = video
		? BuildApiUrl(endpoint, request, {
			  { "systemeid", systemId },
			  { "media", media },
		  })
		: BuildApiUrl(endpoint, request, {
			  { "systemeid", systemId },
			  { "media", media },
			  { "outputformat", "png" },
		  });

	std::vector<uint8> data;
	const std::string label = fmt::format("platform {}", media);
	const auto result = Fetch(ctx, url, data, label);
	if (result != RequestResult::Ok)
		return result;
	if (LooksLikeApiTextError(data))
	{
		Log(LogLevel::Information, "ScreenScraper: {} skipped (API returned a text error)", label);
		return RequestResult::NotFound;
	}
	const std::string_view imageExtension = video ? std::string_view{} : ImageExtension(data);
	if (video ? !LooksLikeMp4(data) : imageExtension.empty())
	{
		Log(LogLevel::Information, "ScreenScraper: {} skipped ({}, {} bytes)", label,
			video ? "not-mp4" : "not an image", data.size());
		return RequestResult::SoftFail;
	}
	fs::path destination = target;
	if (!imageExtension.empty() && PathToUtf8(target.extension()) != imageExtension)
		destination.replace_extension(imageExtension);
	if (!WriteFileAtomic(destination, data))
		return RequestResult::SoftFail;
	return RequestResult::Ok;
}

std::vector<std::string> SavedMediaFileNames(const TitleRequest& title)
{
	std::vector<std::string> saved;
	for (std::string_view fileName : kMediaFileNames)
	{
		const bool video = fileName.ends_with(".mp4");
		const auto existing = ExistingMediaPath(title, fileName);
		if (existing && FileLooksComplete(*existing,
			video ? kMinVideoBytes : kMinImageBytes))
		{
			saved.emplace_back(fileName);
		}
	}
	return saved;
}

bool ContainsSavedFile(const std::vector<std::string>& saved,
	std::string_view fileName)
{
	return std::find(saved.begin(), saved.end(), fileName) != saved.end();
}

void RemoveSavedFile(std::vector<std::string>& saved,
	std::string_view fileName)
{
	std::erase(saved, fileName);
}

// the media manifest distinguishes missing downloads from unavailable media
bool WriteMediaManifest(const TitleRequest& title, const std::string& gameId,
	const std::vector<MediaEntry>& medias, const std::vector<std::string>& saved)
{
	rapidjson::StringBuffer buffer;
	rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
	writer.StartObject();
	writer.Key("gameId");
	writer.Uint64(ParseUint64(gameId).value_or(0));
	writer.Key("available");
	writer.StartArray();
	std::vector<std::string> types;
	for (const MediaEntry& entry : medias)
		AddUnique(types, entry.type);
	for (const std::string& type : types)
		writer.String(type.data(), (rapidjson::SizeType)type.size());
	writer.EndArray();
	writer.Key("saved");
	writer.StartArray();
	for (const std::string& fileName : saved)
		writer.String(fileName.data(), (rapidjson::SizeType)fileName.size());
	writer.EndArray();
	writer.Key("scrapedAt");
	const std::string timestamp = CurrentTimestamp();
	writer.String(timestamp.data(), (rapidjson::SizeType)timestamp.size());
	writer.EndObject();

	return WriteTextFileAtomic(ManifestPath(title.mediaDirectory), std::string(buffer.GetString(), buffer.GetSize()));
}

struct TitleDownloadResult
{
	int downloaded = 0;
	bool complete = false;
};

bool MetadataMatchesGame(const fs::path& path, std::uint64_t gameId)
{
	if (gameId == 0)
		return false;
	std::error_code error;
	const auto size = fs::file_size(path, error);
	if (error || size == 0 || size > 4 * 1024 * 1024)
		return false;
	const auto data = ReadFile(path);
	if (!data)
		return false;
	rapidjson::Document document;
	document.Parse<rapidjson::kParseStopWhenDoneFlag>(
		reinterpret_cast<const char*>(data->data()), data->size());
	if (document.HasParseError())
		return false;
	const rapidjson::Value* game = FindGameValue(document);
	if (!game)
		return false;
	const auto source = ExtractGameId(*game);
	return source && ParseUint64(*source).value_or(0) == gameId;
}

TitleDownloadResult DownloadTitleAssets(ScrapeContext& ctx,
	const TitleRequest& title)
{
	const Request& request = *ctx.request;
	GameInfo info;
	const auto lookup = QueryGame(ctx, title, info);
	if (lookup != TitleLookup::Found)
	{
		if (ctx.aborted || s_stopRequested.load())
			return {};
		if (lookup == TitleLookup::NotFound)
		{
			Log(LogLevel::Information, "ScreenScraper: no match for {} (ScreenScraper does not know this ROM)", title.name);
			return {
				.downloaded = 0,
				.complete = WriteMediaManifest(title, {}, {},
					SavedMediaFileNames(title)),
			};
		}
		Log(LogLevel::Information, "ScreenScraper: lookup for {} failed, leaving it for the next run", title.name);
		return {};
	}

	const fs::path& root = title.mediaDirectory;
	int downloaded = 0;
	bool complete = true;
	const std::uint64_t sourceGameId = ParseUint64(info.gameId).value_or(0);
	const auto previousManifest = LoadScreenScraperMediaManifest(
		ManifestPath(root));
	const bool sameSource = previousManifest && sourceGameId != 0 &&
		previousManifest->gameId == sourceGameId;
	const std::vector<std::string> completeFiles = SavedMediaFileNames(title);
	std::vector<std::string> validSaved;
	if (sameSource)
	{
		for (const std::string& fileName : previousManifest->saved)
		{
			const bool video = fileName.ends_with(".mp4");
			const auto existing = ExistingMediaPath(title, fileName);
			if (ContainsSavedFile(completeFiles, fileName) && existing &&
				FileLooksComplete(*existing,
					video ? kMinVideoBytes : kMinImageBytes))
			{
				AddUnique(validSaved, fileName);
			}
		}
	}

	if (request.options.downloadMetadata && !info.json.empty())
	{
		const fs::path metadataPath = root / "metadata.json";
		if (request.options.overwriteExisting ||
			!MetadataMatchesGame(metadataPath, sourceGameId))
		{
			if (WriteTextFileAtomic(metadataPath, info.json))
				downloaded++;
			else
				complete = false;
		}
	}

	if (info.medias.empty())
		Log(LogLevel::Information, "ScreenScraper: {} (game {}) has no media listed by ScreenScraper", title.name, info.gameId);

	const std::string region = RegionCode(title, request.options.preferredRegion);
	for (const MediaGroup& group : BuildMediaGroups(request))
	{
		if (s_stopRequested.load() || ctx.aborted)
			break;

		const fs::path target = CanonicalMediaPath(title, group.fileName);
		const uint64 minimumSize = group.video ? kMinVideoBytes : kMinImageBytes;
		const auto existingPath = ExistingMediaPath(title, group.fileName);
		const bool validExisting = existingPath &&
			FileLooksComplete(*existingPath, minimumSize) &&
			(title.sharedMedia || ContainsSavedFile(validSaved, group.fileName));
		if (!request.options.overwriteExisting && validExisting)
		{
			AddUnique(validSaved, group.fileName);
			Log(LogLevel::Information, "ScreenScraper: {} skipped for {} (already present)", group.fileName, title.name);
			continue;
		}

		const auto choice = ChooseMedia(info.medias, group.types, region);
		if (!choice)
		{
			RemoveSavedFile(validSaved, group.fileName);
			Log(LogLevel::Information, "ScreenScraper: {} skipped for {} (type {} not offered for this game)",
						group.fileName, title.name, JoinTypes(group.types));
			continue;
		}

		const auto result = FetchMediaFile(ctx, info.gameId, title.systemId, *choice, target, group.video);
		if (result == RequestResult::Ok)
		{
			if (existingPath && *existingPath != target)
			{
				std::error_code removeError;
				fs::remove(*existingPath, removeError);
			}
			downloaded++;
			AddUnique(validSaved, group.fileName);
		}
		else if (!sameSource)
		{
			RemoveSavedFile(validSaved, group.fileName);
		}
		if (result != RequestResult::Ok && result != RequestResult::NotFound)
			complete = false;
	}

	if (ctx.aborted || s_stopRequested.load())
		complete = false;
	if (!WriteMediaManifest(title, info.gameId, info.medias, validSaved))
		complete = false;
	return { downloaded, complete };
}

bool ReadBoolMember(const rapidjson::Value& object, const char* key, bool fallback)
{
	if (!object.IsObject())
		return fallback;
	auto it = object.FindMember(key);
	if (it == object.MemberEnd())
		return fallback;
	if (it->value.IsBool())
		return it->value.GetBool();
	if (it->value.IsInt())
		return it->value.GetInt() != 0;
	return fallback;
}

std::optional<uint64> ReadUint64Value(const rapidjson::Value& value)
{
	if (value.IsUint64())
		return value.GetUint64();
	if (value.IsInt64() && value.GetInt64() >= 0)
		return (uint64)value.GetInt64();
	if (value.IsString())
		return ParseUint64(std::string_view(value.GetString(), value.GetStringLength()));
	return std::nullopt;
}

std::string BuildProgressJson(const Request& request, const std::vector<uint64>& completed)
{
	rapidjson::StringBuffer buffer;
	rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
	const auto writeString = [&writer](const std::string& value) {
		writer.String(value.data(), (rapidjson::SizeType)value.size());
	};

	writer.StartObject();
	writer.Key("version");
	writer.Int(3);
	writer.Key("updatedAt");
	writeString(CurrentTimestamp());
	writer.Key("platformSystemId");
	writer.Uint(request.platformSystemId);
	writer.Key("platformMediaDirectory");
	writeString(PathToUtf8(request.platformMediaDirectory));
	writer.Key("flags");
	writer.StartObject();
	writer.Key("downloadMetadata");
	writer.Bool(request.options.downloadMetadata);
	writer.Key("downloadBoxArt");
	writer.Bool(request.options.downloadBoxArt);
	writer.Key("downloadFanart");
	writer.Bool(request.options.downloadFanArt);
	writer.Key("downloadVideos");
	writer.Bool(request.options.downloadVideos);
	writer.Key("downloadScreenshots");
	writer.Bool(request.options.downloadScreenshots);
	writer.Key("downloadLogos");
	writer.Bool(request.options.downloadLogos);
	writer.Key("overwriteExisting");
	writer.Bool(request.options.overwriteExisting);
	if (request.options.overwritePlatformMedia.has_value())
	{
		writer.Key("overwritePlatformMedia");
		writer.Bool(*request.options.overwritePlatformMedia);
	}
	writer.Key("preferredRegion");
	writer.Int(static_cast<int>(request.options.preferredRegion));
	writer.Key("preferredLanguage");
	writeString(request.options.preferredLanguage);
	writer.EndObject();
	writer.Key("titles");
	writer.StartArray();
	for (const TitleRequest& title : request.targets)
	{
		writer.StartObject();
		writer.Key("titleId");
		writer.Uint64(title.id);
		writer.Key("systemId");
		writer.Uint(title.systemId);
		writer.Key("matchedGameId");
		writer.Uint64(title.matchedGameId);
		writer.Key("name");
		writeString(title.name);
		writer.Key("region");
		writeString(title.region);
		writer.Key("path");
		writeString(PathToUtf8(title.contentPath));
		writer.Key("mediaDirectory");
		writeString(PathToUtf8(title.mediaDirectory));
		writer.EndObject();
	}
	writer.EndArray();
	writer.Key("completed");
	writer.StartArray();
	for (uint64 titleId : completed)
		writer.Uint64(titleId);
	writer.EndArray();
	writer.EndObject();

	return std::string(buffer.GetString(), buffer.GetSize());
}

void SaveProgress(const Request& request, const std::vector<uint64>& completed)
{
	if (!request.progressFile.empty())
		WriteTextFileAtomic(request.progressFile, BuildProgressJson(request, completed));
}

void DeleteProgress(const fs::path& progressFile)
{
	std::error_code ec;
	fs::remove(progressFile, ec);
}

bool LoadProgress(const fs::path& progressFile, Request& request, std::unordered_set<uint64>& completed)
{
	auto data = ReadFile(progressFile);
	if (!data || data->empty())
		return false;
	request.progressFile = progressFile;

	rapidjson::Document doc;
	doc.Parse<rapidjson::kParseStopWhenDoneFlag>(reinterpret_cast<const char*>(data->data()), data->size());
	if (doc.HasParseError() || !doc.IsObject())
	{
		Log(LogLevel::Information, "ScreenScraper: scan_progress.json is malformed, ignoring it");
		return false;
	}

	if (auto systemIt = doc.FindMember("platformSystemId"); systemIt != doc.MemberEnd() && systemIt->value.IsUint())
		request.platformSystemId = systemIt->value.GetUint();
	const std::string platformMediaDirectory = StringMember(doc, "platformMediaDirectory");
	if (!platformMediaDirectory.empty())
		request.platformMediaDirectory = Utf8ToPath(platformMediaDirectory);

	if (auto flagsIt = doc.FindMember("flags"); flagsIt != doc.MemberEnd() && flagsIt->value.IsObject())
	{
		const rapidjson::Value& flags = flagsIt->value;
		request.options.downloadMetadata = ReadBoolMember(flags, "downloadMetadata", request.options.downloadMetadata);
		request.options.downloadBoxArt = ReadBoolMember(flags, "downloadBoxArt", request.options.downloadBoxArt);
		request.options.downloadFanArt = ReadBoolMember(flags, "downloadFanart", request.options.downloadFanArt);
		request.options.downloadVideos = ReadBoolMember(flags, "downloadVideos", request.options.downloadVideos);
		request.options.downloadScreenshots = ReadBoolMember(flags, "downloadScreenshots", request.options.downloadScreenshots);
		request.options.downloadLogos = ReadBoolMember(flags, "downloadLogos", request.options.downloadLogos);
		request.options.overwriteExisting = ReadBoolMember(flags, "overwriteExisting", request.options.overwriteExisting);
		if (flags.HasMember("overwritePlatformMedia"))
		{
			request.options.overwritePlatformMedia = ReadBoolMember(flags,
				"overwritePlatformMedia", request.options.overwriteExisting);
		}
		if (auto regionIt = flags.FindMember("preferredRegion"); regionIt != flags.MemberEnd() && regionIt->value.IsInt())
			request.options.preferredRegion = static_cast<ScreenScraperRegion>(std::clamp(regionIt->value.GetInt(), 0, 4));
		request.options.preferredLanguage = StringMember(flags, "preferredLanguage");
	}

	if (auto titlesIt = doc.FindMember("titles"); titlesIt != doc.MemberEnd() && titlesIt->value.IsArray())
	{
		for (const auto& value : titlesIt->value.GetArray())
		{
			if (!value.IsObject())
				continue;
			auto idIt = value.FindMember("titleId");
			if (idIt == value.MemberEnd())
				continue;
			auto titleId = ReadUint64Value(idIt->value);
			if (!titleId)
				continue;
			TitleRequest title;
			title.id = *titleId;
			if (auto systemIt = value.FindMember("systemId"); systemIt != value.MemberEnd() && systemIt->value.IsUint())
				title.systemId = systemIt->value.GetUint();
			if (auto gameIt = value.FindMember("matchedGameId"); gameIt != value.MemberEnd())
				title.matchedGameId = ReadUint64Value(gameIt->value).value_or(0);
			title.name = StringMember(value, "name");
			title.region = StringMember(value, "region");
			const std::string path = StringMember(value, "path");
			if (!path.empty())
				title.contentPath = Utf8ToPath(path);
			const std::string mediaDirectory = StringMember(value, "mediaDirectory");
			if (!mediaDirectory.empty())
				title.mediaDirectory = Utf8ToPath(mediaDirectory);
			request.targets.emplace_back(std::move(title));
		}
	}

	if (auto completedIt = doc.FindMember("completed"); completedIt != doc.MemberEnd() && completedIt->value.IsArray())
	{
		for (const auto& value : completedIt->value.GetArray())
		{
			if (auto titleId = ReadUint64Value(value))
				completed.insert(*titleId);
		}
	}

	return !request.targets.empty();
}

void RunScrape(Request& request)
{
	if (request.developer.id.empty() || request.developer.password.empty() || request.developer.softwareName.empty())
	{
		SetStatus(State::Error, "ScreenScraper developer identity is incomplete");
		return;
	}
	if (request.account.user.empty() || request.account.password.empty())
	{
		SetStatus(State::Error, "Sign in to ScreenScraper before downloading media");
		return;
	}
	if (request.targets.empty())
	{
		SetStatus(State::Error, "No library items are available to scrape");
		return;
	}
	if (request.platformSystemId == 0 || request.platformMediaDirectory.empty())
	{
		SetStatus(State::Error, "ScreenScraper platform storage is not configured");
		return;
	}
	for (const auto& target : request.targets)
	{
		if (target.systemId == 0 || target.mediaDirectory.empty())
		{
			SetStatus(State::Error, fmt::format("ScreenScraper target '{}' is missing its system or media directory", target.name));
			return;
		}
		if (target.sharedMedia &&
			(ResolveSharedMediaArtworkPath(*target.sharedMedia,
				ScreenScraperArtwork::Box2D).empty() ||
				ResolveSharedMediaVideoPath(*target.sharedMedia).empty()))
		{
			SetStatus(State::Error, fmt::format(
				"ScreenScraper target '{}' has an invalid shared media location",
				target.name));
			return;
		}
	}

	ScrapeContext ctx;
	ctx.request = &request;
	ctx.client = winrt::Windows::Web::Http::HttpClient();
	ctx.client.DefaultRequestHeaders().UserAgent().TryParseAdd(winrt::to_hstring(request.developer.softwareName));
	s_completedTargets.store(0);
	s_totalTargets.store(request.targets.size());
	s_downloadedFiles.store(0);

	// single-title scrapes must not overwrite a pending library scan's progress
	const bool persistProgress = request.targets.size() > 1;
	std::vector<uint64> completed;
	if (persistProgress)
		SaveProgress(request, completed);

	SetStatus(State::Working, fmt::format("Downloading media for {} title{}", request.targets.size(), request.targets.size() == 1 ? "" : "s"));

	int downloaded = 0;
	const bool overwritePlatformMedia =
		request.options.overwritePlatformMedia.value_or(
			request.options.overwriteExisting);
	if (request.options.downloadFanArt)
	{
		downloaded += FetchPlatformMedia(ctx, "fanart",
			request.platformMediaDirectory / "fanart.png", false,
			overwritePlatformMedia) == RequestResult::Ok ? 1 : 0;
		if (!ctx.aborted && !s_stopRequested.load())
			downloaded += FetchPlatformMedia(ctx, "background",
				request.platformMediaDirectory / "background.png", false,
				overwritePlatformMedia) == RequestResult::Ok ? 1 : 0;
	}
	if (request.options.downloadVideos &&
		!ctx.aborted && !s_stopRequested.load())
		downloaded += FetchPlatformMedia(ctx, "video",
			request.platformMediaDirectory / "video.mp4", true,
			overwritePlatformMedia) == RequestResult::Ok ? 1 : 0;
	s_downloadedFiles.store(static_cast<std::size_t>(downloaded));

	for (size_t i = 0; i < request.targets.size(); ++i)
	{
		if (s_stopRequested.load() || ctx.aborted)
			break;

		const auto& title = request.targets[i];
		SetStatus(State::Working, fmt::format("Scraping {} ({}/{}){}", title.name, i + 1, request.targets.size(), QuotaSuffix(ctx)));
		const TitleDownloadResult result = DownloadTitleAssets(ctx, title);
		downloaded += result.downloaded;
		s_downloadedFiles.store(static_cast<std::size_t>(downloaded));
		if (result.complete)
			completed.push_back(title.id);
		s_completedTargets.store(completed.size());
		if (persistProgress)
			SaveProgress(request, completed);
		if (ctx.aborted || s_stopRequested.load())
			break;
	}

	const size_t remaining = request.targets.size() - std::min(completed.size(), request.targets.size());
	if (ctx.aborted && ctx.abortResult == RequestResult::QuotaBlocked)
	{
		SetStatus(State::QuotaPaused, fmt::format("ScreenScraper quota reached ({}). {} of {} titles done, {} file{} saved{}",
												  ctx.abortReason, completed.size(), request.targets.size(), downloaded, downloaded == 1 ? "" : "s",
												  remaining > 0 ? " - the scan will resume where it stopped" : ""));
	}
	else if (ctx.aborted && ctx.abortResult == RequestResult::AuthFailed)
	{
		SetStatus(State::Error, fmt::format("ScreenScraper refused the account ({}). {} file{} saved", ctx.abortReason, downloaded, downloaded == 1 ? "" : "s"));
	}
	else if (ctx.aborted)
	{
		SetStatus(State::Error, fmt::format("ScreenScraper scan stopped ({}). {} of {} titles done, {} file{} saved",
											ctx.abortReason, completed.size(), request.targets.size(), downloaded, downloaded == 1 ? "" : "s"));
	}
	else if (s_stopRequested.load())
	{
		SetStatus(State::Finished, fmt::format("ScreenScraper scan cancelled after {} of {} titles, {} file{} saved",
											   completed.size(), request.targets.size(), downloaded, downloaded == 1 ? "" : "s"));
	}
	else if (remaining > 0)
	{
		SetStatus(State::Error, fmt::format(
			"ScreenScraper could not finish {} of {} title{}. {} file{} saved; the unfinished title{} will be retried",
			remaining, request.targets.size(), request.targets.size() == 1 ? "" : "s",
			downloaded, downloaded == 1 ? "" : "s", remaining == 1 ? "" : "s"));
	}
	else
	{
		if (persistProgress)
			DeleteProgress(request.progressFile);
		if (downloaded == 0)
			SetStatus(State::UpToDate, "No new ScreenScraper media downloaded");
		else
			SetStatus(State::Finished, fmt::format("Downloaded {} ScreenScraper file{}", downloaded, downloaded == 1 ? "" : "s"));
	}
}

void WorkerThread(Request request)
{
	try
	{
		winrt::init_apartment(winrt::apartment_type::multi_threaded);
	}
	catch (const winrt::hresult_error&)
	{
	}

	try
	{
		RunScrape(request);
	}
	catch (const std::exception& e)
	{
		SetStatus(State::Error, fmt::format("ScreenScraper scan aborted ({})", e.what()));
	}
	catch (...)
	{
		SetStatus(State::Error, "ScreenScraper scan aborted (unknown error)");
	}

	s_stopRequested.store(false);
	// clear running before finishedPending so the consumer can start the next scan
	s_running.store(false);
	s_finishedPending.store(true);
}
} // anonymous namespace

bool Start(Request request)
{
	bool expected = false;
	if (!s_running.compare_exchange_strong(expected, true))
		return false;

	s_stopRequested.store(false);
	s_finishedPending.store(false);
	SetStatus(State::Working, "Starting ScreenScraper download...");
	try
	{
		std::scoped_lock lock(s_workerMutex);
		if (s_worker.joinable())
			s_worker.join();
		s_worker = std::thread(WorkerThread, std::move(request));
	}
	catch (const std::exception& e)
	{
		s_running.store(false);
		SetStatus(State::Error, fmt::format("Failed to start the ScreenScraper scan ({})", e.what()));
		return false;
	}
	return true;
}

bool ConsumeFinished()
{
	return s_finishedPending.exchange(false);
}

void RequestStop()
{
	if (s_running.load())
		s_stopRequested.store(true);
}

bool HasPendingScan(const fs::path& progressFile)
{
	Request request;
	std::unordered_set<uint64> completed;
	if (LoadProgress(progressFile, request, completed))
	{
		for (const TitleRequest& title : request.targets)
		{
			if (!completed.contains(title.id))
				return true;
		}
	}
	return false;
}

bool ResumePendingScan(Request defaults)
{
	if (s_running.load())
		return false;

	// the progress file only holds completed ids, the request comes from defaults
	Request savedRequest;
	std::unordered_set<uint64> completed;
	if (!LoadProgress(defaults.progressFile, savedRequest, completed))
		return false;

	std::vector<TitleRequest> pending;
	pending.reserve(defaults.targets.size());
	for (TitleRequest& title : defaults.targets)
	{
		if (!completed.contains(title.id))
			pending.emplace_back(std::move(title));
	}
	if (pending.empty())
	{
		DeleteProgress(defaults.progressFile);
		return false;
	}

	defaults.targets = std::move(pending);
	Log(LogLevel::Information, "ScreenScraper: resuming a pending scan with {} title(s) left", defaults.targets.size());
	return Start(std::move(defaults));
}

std::optional<std::vector<std::string>> GetAvailableMediaTypes(const fs::path& mediaDirectory)
{
	auto data = ReadFile(ManifestPath(mediaDirectory));
	if (!data || data->empty())
		return std::nullopt;

	rapidjson::Document doc;
	doc.Parse<rapidjson::kParseStopWhenDoneFlag>(reinterpret_cast<const char*>(data->data()), data->size());
	if (doc.HasParseError() || !doc.IsObject())
		return std::nullopt;

	auto it = doc.FindMember("available");
	if (it == doc.MemberEnd() || !it->value.IsArray())
		return std::nullopt;

	std::vector<std::string> types;
	for (const auto& value : it->value.GetArray())
	{
		if (value.IsString())
			types.emplace_back(value.GetString(), value.GetStringLength());
	}
	return types;
}

const rapidjson::Value* JsonMember(const rapidjson::Value& object, const char* name)
{
	if (!object.IsObject())
		return nullptr;
	const auto found = object.FindMember(name);
	return found == object.MemberEnd() ? nullptr : &found->value;
}

std::string JsonText(const rapidjson::Value& value)
{
	if (auto text = StringFromJsonValue(value))
		return *text;
	if (!value.IsObject())
		return {};
	for (const char* name : { "text", "nom", "name", "value" })
	{
		if (const auto* member = JsonMember(value, name))
		{
			if (auto text = StringFromJsonValue(*member))
				return *text;
		}
	}
	return {};
}

std::string PreferredText(const rapidjson::Value* values, const char* selector,
	std::initializer_list<std::string_view> preferences)
{
	if (!values)
		return {};
	if (values->IsObject())
	{
		for (std::string_view preference : preferences)
		{
			for (std::string_view prefix : { std::string_view("nom_"),
				std::string_view("synopsis_"), std::string_view("date_") })
			{
				const std::string key = std::string(prefix) + std::string(preference);
				if (const auto* member = JsonMember(*values, key.c_str()))
				{
					if (std::string text = JsonText(*member); !text.empty())
						return text;
				}
			}
		}
		return JsonText(*values);
	}
	if (!values->IsArray())
		return JsonText(*values);

	for (std::string_view preference : preferences)
	{
		for (const auto& value : values->GetArray())
		{
			const auto* selected = JsonMember(value, selector);
			if (!selected || LowerAscii(JsonText(*selected)) != LowerAscii(std::string(preference)))
				continue;
			if (std::string text = JsonText(value); !text.empty())
				return text;
		}
	}
	for (const auto& value : values->GetArray())
	{
		if (std::string text = JsonText(value); !text.empty())
			return text;
	}
	return {};
}

std::string JoinedNames(const rapidjson::Value& game, const char* memberName,
	std::string_view language)
{
	const auto* groups = JsonMember(game, memberName);
	if (!groups)
		return {};
	if (!groups->IsArray())
		return JsonText(*groups);

	std::vector<std::string> names;
	for (const auto& group : groups->GetArray())
	{
		std::string name = PreferredText(JsonMember(group, "noms"), "langue",
			{ language, "en", "ss", "wor" });
		if (name.empty())
			name = JsonText(group);
		AddUnique(names, std::move(name));
	}
	std::string result;
	for (const std::string& name : names)
	{
		if (!result.empty())
			result += ", ";
		result += name;
	}
	return result;
}

std::string SearchRating(const rapidjson::Value& game)
{
	const auto* rating = JsonMember(game, "note");
	const std::string value = rating ? JsonText(*rating) : std::string{};
	return NormalizeScreenScraperApiRating(value);
}

std::string SearchContentRating(const rapidjson::Value& game,
	std::string_view region)
{
	const auto* classifications = JsonMember(game, "classifications");
	if (!classifications || !classifications->IsArray())
		return {};

	std::vector<std::string_view> preferences;
	if (region == "eu")
		preferences.emplace_back("PEGI");
	else if (region == "jp")
		preferences.emplace_back("CERO");
	else
		preferences.emplace_back("ESRB");
	for (const std::string_view fallback :
		{ std::string_view("ESRB"), std::string_view("PEGI"),
		  std::string_view("CERO"), std::string_view("ACB"),
		  std::string_view("USK") })
	{
		if (std::find(preferences.begin(), preferences.end(), fallback) ==
			preferences.end())
		{
			preferences.push_back(fallback);
		}
	}

	for (const std::string_view preference : preferences)
	{
		for (const auto& classification : classifications->GetArray())
		{
			const auto* type = JsonMember(classification, "type");
			if (!type || LowerAscii(JsonText(*type)) !=
				LowerAscii(std::string(preference)))
			{
				continue;
			}
			const std::string rating = JsonText(classification);
			if (!rating.empty())
				return std::string(preference) + " " + rating;
		}
	}
	return {};
}

Metadata SearchMetadata(const rapidjson::Value& game, std::string_view region,
	std::string_view language)
{
	Metadata metadata;
	metadata.description = PreferredText(JsonMember(game, "synopsis"), "langue",
		{ language, "en", "ss", "wor" });
	metadata.genre = JoinedNames(game, "genres", language);
	metadata.developer = StringMember(game, "developpeur");
	metadata.publisher = StringMember(game, "editeur");
	metadata.players = StringMember(game, "joueurs");
	metadata.releaseDate = PreferredText(JsonMember(game, "dates"), "region",
		{ region, "wor", "us", "eu", "jp" });
	metadata.releaseDate = NormalizeMetadataReleaseDate(metadata.releaseDate);
	metadata.rating = SearchRating(game);
	metadata.contentRating = SearchContentRating(game, region);
	metadata.modes = JoinedNames(game, "modes", language);
	metadata.themes = JoinedNames(game, "themes", language);
	return metadata;
}

std::vector<const rapidjson::Value*> SearchGameValues(const rapidjson::Document& document)
{
	std::vector<const rapidjson::Value*> games;
	const auto* response = JsonMember(document, "response");
	if (!response)
		return games;
	const rapidjson::Value* values = JsonMember(*response, "jeux");
	if (!values)
		values = JsonMember(*response, "jeu");
	if (!values)
		return games;
	if (values->IsArray())
	{
		for (const auto& game : values->GetArray())
		{
			if (game.IsObject())
				games.push_back(&game);
		}
	}
	else if (values->IsObject())
	{
		games.push_back(values);
	}
	return games;
}

std::vector<ScreenScraperMatch> ParseSearchMatches(const std::vector<uint8>& response,
	const ScreenScraperSearchRequest& request)
{
	if (response.empty())
		return {};
	rapidjson::Document document;
	document.Parse<rapidjson::kParseStopWhenDoneFlag>(
		reinterpret_cast<const char*>(response.data()), response.size());
	if (document.HasParseError() || !document.IsObject())
		return {};

	const std::string region = ResolveScreenScraperRegionCode(
		request.preferredRegion, request.regionHint);
	const std::string language = request.preferredLanguage.empty() ?
		std::string("en") : request.preferredLanguage;
	std::vector<ScreenScraperMatch> matches;
	for (const rapidjson::Value* game : SearchGameValues(document))
	{
		const auto id = ExtractGameId(*game);
		if (!id)
			continue;
		const auto numericId = ParseUint64(*id);
		if (!numericId)
			continue;

		ScreenScraperMatch match;
		match.gameId = *numericId;
		match.systemId = request.systemId;
		if (const auto* system = JsonMember(*game, "systeme"))
		{
			match.platform = JsonText(*system);
			if (const auto* systemId = JsonMember(*system, "id"))
				match.systemId = static_cast<std::uint32_t>(
					ParseUint64(JsonText(*systemId)).value_or(match.systemId));
		}
		if (match.platform.empty())
			match.platform = request.platformName;
		match.title = PreferredText(JsonMember(*game, "noms"), "region",
			{ region, "wor", "ss", "us", "eu", "jp" });
		if (match.title.empty())
			match.title = StringMember(*game, "nom");
		match.region = region;
		match.metadata = SearchMetadata(*game, region, language);
		for (const MediaEntry& entry : ExtractMedias(*game))
		{
			match.media.push_back({ entry.type, entry.parent, entry.region, entry.format });
		}
		matches.emplace_back(std::move(match));
		if (matches.size() == 30)
			break;
	}
	return matches;
}

std::vector<std::string> ArtworkApiTypes(ScreenScraperArtwork artwork)
{
	switch (artwork)
	{
	case ScreenScraperArtwork::Box2D: return { "box-2D" };
	case ScreenScraperArtwork::Box3D: return { "box-3D" };
	case ScreenScraperArtwork::MixRecalboxV1: return { "mixrbv1" };
	case ScreenScraperArtwork::MixRecalboxV2: return { "mixrbv2" };
	case ScreenScraperArtwork::FanArt: return { "fanart" };
	case ScreenScraperArtwork::Screenshot: return { "ss" };
	case ScreenScraperArtwork::Logo: return { "wheel-hd", "wheel" };
	case ScreenScraperArtwork::BackCover: return { "box-2D-back" };
	case ScreenScraperArtwork::PhysicalMedia: return { "support-2D" };
	case ScreenScraperArtwork::TitleScreen: return { "sstitle" };
	}
	return {};
}

std::optional<MediaChoice> ChooseSearchMedia(const ScreenScraperMatch& match,
	ScreenScraperArtwork artwork, std::string_view preferredRegion)
{
	std::vector<MediaEntry> media;
	media.reserve(match.media.size());
	for (const auto& entry : match.media)
		media.push_back({ entry.type, entry.parent, entry.region, entry.format, {} });
	return ChooseMedia(media, ArtworkApiTypes(artwork), std::string(preferredRegion));
}

bool SearchCancelled(std::uint64_t generation)
{
	return s_searchShutdown.load() || generation != s_searchGeneration.load();
}

bool WaitForSearchOperation(const auto& operation, std::uint64_t generation)
{
	using winrt::Windows::Foundation::AsyncStatus;
	while (operation.Status() == AsyncStatus::Started)
	{
		if (SearchCancelled(generation))
		{
			operation.Cancel();
			return false;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
	}
	return !SearchCancelled(generation);
}

bool WaitForSearchDelay(std::uint32_t milliseconds,
	std::uint64_t generation)
{
	const auto deadline = std::chrono::steady_clock::now() +
		std::chrono::milliseconds(milliseconds);
	while (std::chrono::steady_clock::now() < deadline)
	{
		if (SearchCancelled(generation))
			return false;
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
	}
	return !SearchCancelled(generation);
}

enum class SearchFetchResult
{
	Ok,
	Cancelled,
	NotFound,
	TransientFailure,
	PermanentFailure,
};

SearchFetchResult FetchSearchData(const std::string& url,
	const std::string& userAgent,
	std::uint64_t generation, std::vector<uint8>& output, std::string& error)
{
	static std::chrono::steady_clock::time_point lastRequest{};
	output.clear();
	error.clear();
	std::uint32_t backoffMs = kDefaultRequestIntervalMs;
	for (std::uint32_t attempt = 1; attempt <= kMaxAttempts; ++attempt)
	{
		if (SearchCancelled(generation))
			return SearchFetchResult::Cancelled;
		const auto now = std::chrono::steady_clock::now();
		const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
			now - lastRequest).count();
		if (lastRequest.time_since_epoch().count() != 0 &&
			elapsed < kDefaultRequestIntervalMs &&
			!WaitForSearchDelay(static_cast<std::uint32_t>(
				kDefaultRequestIntervalMs - elapsed), generation))
		{
			return SearchFetchResult::Cancelled;
		}
		lastRequest = std::chrono::steady_clock::now();

		std::uint32_t status = 0;
		std::uint32_t retryAfterMs = 0;
		bool transportFailure = false;
		try
		{
			winrt::Windows::Web::Http::HttpClient client;
			client.DefaultRequestHeaders().UserAgent().TryParseAdd(
				winrt::to_hstring(userAgent));
			auto responseOperation = client.GetAsync(
				winrt::Windows::Foundation::Uri(winrt::to_hstring(url)));
			if (!WaitForSearchOperation(responseOperation, generation))
				return SearchFetchResult::Cancelled;
			const auto response = responseOperation.GetResults();
			status = static_cast<std::uint32_t>(response.StatusCode());
			retryAfterMs = ReadRetryAfterMs(response);
			if (status >= 200 && status < 300)
			{
				auto contentOperation = response.Content().ReadAsBufferAsync();
				if (!WaitForSearchOperation(contentOperation, generation))
					return SearchFetchResult::Cancelled;
				const auto buffer = contentOperation.GetResults();
				output.resize(buffer.Length());
				if (!output.empty())
				{
					auto reader = winrt::Windows::Storage::Streams::DataReader::FromBuffer(buffer);
					reader.ReadBytes(winrt::array_view<uint8_t>(
						output.data(), output.data() + output.size()));
				}
				return SearchFetchResult::Ok;
			}
		}
		catch (const winrt::hresult_error& exception)
		{
			if (SearchCancelled(generation))
				return SearchFetchResult::Cancelled;
			transportFailure = true;
			error = fmt::format("ScreenScraper connection failed (0x{:08x})",
				static_cast<std::uint32_t>(exception.code().value));
		}
		catch (const std::exception&)
		{
			if (SearchCancelled(generation))
				return SearchFetchResult::Cancelled;
			transportFailure = true;
			error = "ScreenScraper connection failed";
		}

		if (status == 404)
			return SearchFetchResult::NotFound;
		const bool retryable = transportFailure || status == 401 ||
			status == 429 || status >= 500;
		if (!transportFailure)
		{
			error = status == 403 ?
				"ScreenScraper rejected the developer credentials" :
				status == 426 ? "ScreenScraper rejected this software" :
				status == 423 ? "ScreenScraper API is unavailable" :
				status == 429 ? "ScreenScraper rate limit reached" :
				status == 430 || status == 431 ?
					"ScreenScraper daily quota reached" :
					fmt::format("ScreenScraper request failed (HTTP {})", status);
		}
		if (!retryable)
			return SearchFetchResult::PermanentFailure;
		if (attempt == kMaxAttempts)
			return SearchFetchResult::TransientFailure;

		const std::uint32_t waitMs = std::min(
			std::max(retryAfterMs, backoffMs), kRetryWaitCapMs);
		if (!WaitForSearchDelay(waitMs, generation))
			return SearchFetchResult::Cancelled;
		backoffMs = std::min(backoffMs * 2, kRetryWaitCapMs);
	}
	return SearchFetchResult::TransientFailure;
}

std::string PreviewKey(const SearchPreviewRequest& request)
{
	return fmt::format("{}:{}:{}:{}", request.generation, request.gameId,
		static_cast<int>(request.artwork), request.maximumHeight);
}

std::string_view SearchPreviewSessionName()
{
	static const std::string name = fmt::format("session-{:x}",
		static_cast<std::uint64_t>(std::chrono::system_clock::now()
			.time_since_epoch().count()));
	return name;
}

fs::path SearchPreviewCacheRoot(const ScreenScraperSearchRequest& request)
{
	return request.previewDirectory / "uwp-imgui-preview-cache" /
		SearchPreviewSessionName();
}

void PrepareSearchPreviewCache(const ScreenScraperSearchRequest& request,
	std::uint64_t generation)
{
	if (request.previewDirectory.empty())
		return;
	// older generations must survive, a staged editor match may still preview them
	const fs::path ownedRoot = request.previewDirectory /
		"uwp-imgui-preview-cache";
	const fs::path sessionRoot = SearchPreviewCacheRoot(request);
	std::error_code error;
	fs::create_directories(ownedRoot, error);
	for (fs::directory_iterator entry(ownedRoot, error), end;
		!error && entry != end; entry.increment(error))
	{
		const std::string name = PathToUtf8(entry->path().filename());
		if (name.starts_with("session-") && entry->path() != sessionRoot)
		{
			std::error_code removeError;
			fs::remove_all(entry->path(), removeError);
		}
	}
	error.clear();
	fs::create_directories(sessionRoot, error);
	constexpr std::uint64_t kRetainedGenerations = 8;
	for (fs::directory_iterator entry(sessionRoot, error), end;
		!error && entry != end; entry.increment(error))
	{
		const std::string name = PathToUtf8(entry->path().filename());
		constexpr std::string_view prefix = "generation-";
		if (!name.starts_with(prefix))
			continue;
		const auto oldGeneration = ParseUint64(
			std::string_view(name).substr(prefix.size()));
		if (oldGeneration && *oldGeneration + kRetainedGenerations < generation)
		{
			std::error_code removeError;
			fs::remove_all(entry->path(), removeError);
		}
	}
}

void PublishSearchError(std::uint64_t generation, std::string status)
{
	std::scoped_lock lock(s_searchMutex);
	if (generation != s_searchGeneration.load())
		return;
	s_searchSnapshot.state = ScreenScraperSearchState::Error;
	s_searchSnapshot.status = std::move(status);
}

void RunInteractiveSearch(ScreenScraperSearchRequest request,
	std::uint64_t generation)
{
	if (request.developer.id.empty() || request.developer.password.empty() ||
		request.developer.softwareName.empty())
	{
		PublishSearchError(generation, "ScreenScraper developer identity is incomplete");
		return;
	}
	if (request.account.user.empty() || request.account.password.empty())
	{
		PublishSearchError(generation, "Sign in to ScreenScraper before searching");
		return;
	}
	if (request.systemId == 0)
	{
		PublishSearchError(generation, "ScreenScraper platform is not configured");
		return;
	}
	PrepareSearchPreviewCache(request, generation);
	if (SearchCancelled(generation))
		return;

	Request apiRequest;
	apiRequest.developer = request.developer;
	apiRequest.account = request.account;
	const std::string systemId = fmt::format("{}", request.systemId);
	const std::string url = BuildApiUrl("jeuRecherche.php", apiRequest, {
		{ "output", "json" },
		{ "systemeid", systemId },
		{ "recherche", request.query },
	});
	std::vector<uint8> response;
	std::string error;
	const SearchFetchResult fetchResult = FetchSearchData(url,
		request.developer.softwareName, generation, response, error);
	if (fetchResult != SearchFetchResult::Ok)
	{
		if (fetchResult == SearchFetchResult::NotFound)
		{
			std::scoped_lock lock(s_searchMutex);
			if (generation == s_searchGeneration.load())
			{
				s_activeSearch = std::move(request);
				s_searchSnapshot.matches.clear();
				s_searchSnapshot.state = ScreenScraperSearchState::NoMatches;
				s_searchSnapshot.status = "No ScreenScraper matches found";
			}
		}
		else if (fetchResult != SearchFetchResult::Cancelled &&
			!SearchCancelled(generation))
		{
			PublishSearchError(generation, error.empty() ? "ScreenScraper search was cancelled" : error);
		}
		return;
	}
	if (LooksLikeApiTextError(response))
	{
		PublishSearchError(generation, "ScreenScraper returned an invalid search response");
		return;
	}
	auto matches = ParseSearchMatches(response, request);
	if (SearchCancelled(generation))
		return;
	std::size_t matchCount = 0;
	{
		std::scoped_lock lock(s_searchMutex);
		if (generation != s_searchGeneration.load())
			return;
		s_activeSearch = std::move(request);
		s_searchSnapshot.matches = std::move(matches);
		s_searchSnapshot.state = s_searchSnapshot.matches.empty() ?
			ScreenScraperSearchState::NoMatches : ScreenScraperSearchState::Ready;
		s_searchSnapshot.status = s_searchSnapshot.matches.empty() ?
			"No ScreenScraper matches found" :
			fmt::format("{} match{}", s_searchSnapshot.matches.size(),
					s_searchSnapshot.matches.size() == 1 ? "" : "es");
		matchCount = s_searchSnapshot.matches.size();
	}
	Log(LogLevel::Information, "ScreenScraper: interactive search returned {} match(es)",
		matchCount);
}

void CompletePreviewRequest(const SearchPreviewRequest& preview, bool unavailable)
{
	const std::string key = PreviewKey(preview);
	std::scoped_lock lock(s_searchMutex);
	s_pendingPreviewKeys.erase(key);
	if (unavailable && preview.generation == s_searchGeneration.load())
		s_unavailablePreviewKeys.insert(key);
}

void RunSearchPreview(const SearchPreviewRequest& preview)
{
	ScreenScraperSearchRequest request;
	ScreenScraperMatch match;
	bool requestIsInvalid = false;
	{
		std::scoped_lock lock(s_searchMutex);
		if (preview.generation != s_searchGeneration.load() || !s_activeSearch)
		{
			requestIsInvalid = true;
		}
		else
		{
			request = *s_activeSearch;
			const auto found = std::find_if(s_searchSnapshot.matches.begin(),
				s_searchSnapshot.matches.end(), [&preview](const ScreenScraperMatch& candidate) {
					return candidate.gameId == preview.gameId;
				});
			if (found == s_searchSnapshot.matches.end())
				requestIsInvalid = true;
			else
				match = *found;
		}
	}
	if (requestIsInvalid)
	{
		CompletePreviewRequest(preview, false);
		return;
	}
	if (request.previewDirectory.empty())
	{
		CompletePreviewRequest(preview, true);
		return;
	}

	const std::string region = ResolveScreenScraperRegionCode(
		request.preferredRegion, request.regionHint);
	const auto media = ChooseSearchMedia(match, preview.artwork, region);
	if (!media)
	{
		CompletePreviewRequest(preview, true);
		return;
	}

	const std::uint32_t maximumHeight = std::clamp(preview.maximumHeight, 64u, 720u);
	const fs::path target = SearchPreviewCacheRoot(request) /
		fmt::format("generation-{}", preview.generation) /
		fmt::format("{}", preview.gameId) /
		fmt::format("{}-{}", maximumHeight,
			ScreenScraperArtworkFileName(preview.artwork));
	if (!FileLooksComplete(target, kMinImageBytes))
	{
		Request apiRequest;
		apiRequest.developer = request.developer;
		apiRequest.account = request.account;
		const std::string systemId = fmt::format("{}", request.systemId);
		const std::string gameId = fmt::format("{}", preview.gameId);
		const std::string height = fmt::format("{}", maximumHeight);
		const std::string mediaParam = media->region.empty() ? media->type :
			fmt::format("{}({})", media->type, media->region);
		const std::string url = BuildApiUrl("mediaJeu.php", apiRequest, {
			{ "systemeid", systemId },
			{ "jeuid", gameId },
			{ "media", mediaParam },
			{ "maxheight", height },
			{ "outputformat", "png" },
		});
		std::vector<uint8> data;
		std::string error;
		const SearchFetchResult fetchResult = FetchSearchData(url,
			request.developer.softwareName, preview.generation, data, error);
		if (fetchResult != SearchFetchResult::Ok)
		{
			CompletePreviewRequest(preview,
				fetchResult == SearchFetchResult::NotFound ||
				fetchResult == SearchFetchResult::PermanentFailure);
			return;
		}
		if (!LooksLikePng(data) || data.size() < kMinImageBytes)
		{
			CompletePreviewRequest(preview, true);
			return;
		}
		if (!WriteFileAtomic(target, data))
		{
			CompletePreviewRequest(preview, false);
			return;
		}
	}

	{
		std::scoped_lock lock(s_searchMutex);
		if (preview.generation == s_searchGeneration.load())
		{
			const auto found = std::find_if(s_searchSnapshot.matches.begin(),
				s_searchSnapshot.matches.end(), [&preview](const ScreenScraperMatch& candidate) {
					return candidate.gameId == preview.gameId;
				});
			if (found != s_searchSnapshot.matches.end())
			{
				std::erase_if(found->previews, [&preview](const ScreenScraperArtworkPreview& existing) {
					return existing.artwork == preview.artwork &&
						existing.maximumHeight <= preview.maximumHeight;
				});
				found->previews.push_back({ preview.artwork, maximumHeight, target });
			}
		}
		s_pendingPreviewKeys.erase(PreviewKey(preview));
	}
}

void InteractiveSearchWorker()
{
	try
	{
		winrt::init_apartment(winrt::apartment_type::multi_threaded);
	}
	catch (const winrt::hresult_error&)
	{
	}

	while (!s_searchShutdown.load())
	{
		std::optional<ScreenScraperSearchRequest> search;
		std::optional<SearchPreviewRequest> preview;
		std::uint64_t generation = 0;
		{
			std::unique_lock lock(s_searchMutex);
			s_searchWake.wait(lock, [] {
				return s_searchShutdown.load() || s_pendingSearch.has_value() ||
					!s_searchPreviews.empty();
			});
			if (s_searchShutdown.load())
				break;
			if (s_pendingSearch)
			{
				search = std::move(s_pendingSearch);
				s_pendingSearch.reset();
				generation = s_searchGeneration.load();
			}
			else if (!s_searchPreviews.empty())
			{
				preview = s_searchPreviews.front();
				s_searchPreviews.pop_front();
			}
		}
		if (search)
			RunInteractiveSearch(std::move(*search), generation);
		else if (preview)
			RunSearchPreview(*preview);
	}
}

void StartInteractiveSearchWorker()
{
	std::scoped_lock lock(s_searchMutex);
	if (s_searchWorker.joinable())
		return;
	s_searchShutdown.store(false);
	s_searchWorker = std::thread(InteractiveSearchWorker);
}

void StopInteractiveSearchWorker()
{
	s_searchShutdown.store(true);
	s_searchGeneration.fetch_add(1);
	s_searchWake.notify_all();
	if (s_searchWorker.joinable())
		s_searchWorker.join();
	std::scoped_lock lock(s_searchMutex);
	s_pendingSearch.reset();
	s_activeSearch.reset();
	s_searchPreviews.clear();
	s_pendingPreviewKeys.clear();
	s_unavailablePreviewKeys.clear();
	s_searchSnapshot = {};
}

std::uint64_t BeginInteractiveSearch(ScreenScraperSearchRequest request)
{
	const std::uint64_t generation = s_searchGeneration.fetch_add(1) + 1;
	{
		std::scoped_lock lock(s_searchMutex);
		s_pendingSearch = std::move(request);
		s_activeSearch.reset();
		s_searchPreviews.clear();
		s_pendingPreviewKeys.clear();
		s_unavailablePreviewKeys.clear();
		s_searchSnapshot = {
			.generation = generation,
			.state = ScreenScraperSearchState::Searching,
			.query = s_pendingSearch->query,
			.status = "Searching ScreenScraper...",
		};
	}
	s_searchWake.notify_one();
	return generation;
}

ScreenScraperSearchSnapshot SearchSnapshot()
{
	std::scoped_lock lock(s_searchMutex);
	return s_searchSnapshot;
}

void CancelInteractiveSearch()
{
	s_searchGeneration.fetch_add(1);
	std::scoped_lock lock(s_searchMutex);
	s_pendingSearch.reset();
	s_activeSearch.reset();
	s_searchPreviews.clear();
	s_pendingPreviewKeys.clear();
	s_unavailablePreviewKeys.clear();
	s_searchSnapshot = {};
}

void QueueSearchPreview(std::uint64_t generation, std::uint64_t gameId,
	ScreenScraperArtwork artwork, std::uint32_t maximumHeight)
{
	SearchPreviewRequest request{ generation, gameId, artwork,
		std::clamp(maximumHeight, 64u, 720u) };
	const std::string key = PreviewKey(request);
	{
		std::scoped_lock lock(s_searchMutex);
		if (generation != s_searchGeneration.load() ||
			s_searchSnapshot.state != ScreenScraperSearchState::Ready ||
			s_pendingPreviewKeys.contains(key) || s_unavailablePreviewKeys.contains(key))
		{
			return;
		}
		const auto match = std::find_if(s_searchSnapshot.matches.begin(),
			s_searchSnapshot.matches.end(), [gameId](const ScreenScraperMatch& candidate) {
				return candidate.gameId == gameId;
			});
		if (match == s_searchSnapshot.matches.end())
			return;
		const auto existing = std::find_if(match->previews.begin(), match->previews.end(),
			[artwork, maximumHeight](const ScreenScraperArtworkPreview& preview) {
				return preview.artwork == artwork && preview.maximumHeight >= maximumHeight;
			});
		if (existing != match->previews.end())
			return;
		s_pendingPreviewKeys.insert(key);
		s_searchPreviews.push_back(request);
	}
	s_searchWake.notify_one();
}

class ScreenScraperService final : public IScreenScraperService
{
public:
	explicit ScreenScraperService(ScreenScraperLog log)
	{
		s_log = std::move(log);
		StartInteractiveSearchWorker();
	}

	~ScreenScraperService() override
	{
		RequestStop();
		std::scoped_lock lock(s_workerMutex);
		if (s_worker.joinable())
			s_worker.join();
		StopInteractiveSearchWorker();
	}

	ScreenScraperProgress GetProgress() const override
	{
		ScreenScraperProgress progress;
		progress.state = s_state.load();
		progress.completedTargets = s_completedTargets.load();
		progress.totalTargets = s_totalTargets.load();
		progress.downloadedFiles = s_downloadedFiles.load();
		std::scoped_lock lock(s_statusMutex);
		progress.status = s_status;
		return progress;
	}

	bool Start(ScreenScraperRequest request) override { return UwpImGuiFrontend::Start(std::move(request)); }
	bool ConsumeFinished() override { return UwpImGuiFrontend::ConsumeFinished(); }
	void RequestStop() override { UwpImGuiFrontend::RequestStop(); }
	bool HasPendingScan(const fs::path& progressFile) const override
	{
		return UwpImGuiFrontend::HasPendingScan(progressFile);
	}
	bool ResumePendingScan(ScreenScraperRequest defaults) override
	{
		return UwpImGuiFrontend::ResumePendingScan(std::move(defaults));
	}
	std::optional<std::vector<std::string>> GetAvailableMediaTypes(
		const fs::path& mediaDirectory) const override
	{
		return UwpImGuiFrontend::GetAvailableMediaTypes(mediaDirectory);
	}
	std::uint64_t BeginSearch(ScreenScraperSearchRequest request) override
	{
		return BeginInteractiveSearch(std::move(request));
	}
	ScreenScraperSearchSnapshot GetSearchSnapshot() const override
	{
		return SearchSnapshot();
	}
	void CancelSearch() override { CancelInteractiveSearch(); }
	void RequestSearchPreview(std::uint64_t generation, std::uint64_t gameId,
		ScreenScraperArtwork artwork, std::uint32_t maximumHeight) override
	{
		QueueSearchPreview(generation, gameId, artwork, maximumHeight);
	}
};

std::unique_ptr<IScreenScraperService> CreateScreenScraperService(ScreenScraperLog log)
{
	return std::make_unique<ScreenScraperService>(std::move(log));
}
}
