#include "UwpImGuiFrontend/DsuCompanion.h"
#include "UwpImGuiFrontend/ScreenScraper.h"

#include <boost/asio.hpp>
#include <fmt/format.h>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <thread>
#include <utility>

namespace fs = std::filesystem;

namespace UwpImGuiFrontend
{
namespace
{
using boost::asio::ip::tcp;
using uint8 = std::uint8_t;
using uint16 = std::uint16_t;
using uint32 = std::uint32_t;
using uint64 = std::uint64_t;
using sint32 = std::int32_t;

constexpr uint16 kDsuProtocolVersion = 1001;
constexpr size_t kDsuHeaderSize = 20;
constexpr size_t kStateStringSize = 64;
constexpr size_t kStatePayloadSize = 1 + (kStateStringSize * 4) + 4;
constexpr uint32 kDsuClientId = 0xCEE00001u;

constexpr uint32 kMsgKeyboard = 0x120000;
constexpr uint32 kMsgState = 0x140000;
constexpr uint32 kMsgStreamStart = 0x150000;
constexpr uint32 kMsgStreamStop = 0x150001;
constexpr uint32 kMsgStreamStatus = 0x150002;
constexpr uint32 kMsgStreamControl = 0x150003;
constexpr uint32 kMsgAuxStreamStart = 0x150100;
constexpr uint32 kMsgAuxStreamStop = 0x150101;
constexpr uint32 kMsgAppLaunch = 0x180004;
constexpr uint32 kMsgAppLaunchStatus = 0x180005;
constexpr uint32 kMsgAppSelection = 0x180006;

constexpr uint8 kStateFlagContentLoaded = 1 << 0;
constexpr uint32 kCapabilityState = 1u << 0;
constexpr uint32 kCapabilityMainStream = 1u << 4;
constexpr uint32 kCapabilityAuxStream = 1u << 5;
constexpr uint32 kCapabilityRemoteCommands = 1u << 6;
constexpr uint32 kCapabilityLaunchById = 1u << 9;
constexpr uint32 kCapabilityCatalogue = 1u << 13;
constexpr uint32 kCapabilityCatalogueMedia = 1u << 14;
constexpr uint32 kCapabilityAppSelection = 1u << 15;
constexpr uint32 kBaseCapabilities = kCapabilityState | kCapabilityRemoteCommands |
		kCapabilityLaunchById | kCapabilityCatalogue | kCapabilityCatalogueMedia | kCapabilityAppSelection;

constexpr auto kAnnounceInterval = std::chrono::seconds(1);
constexpr size_t kRequestLimit = 4096;
constexpr size_t kFileChunk = 64 * 1024;
constexpr size_t kStreamScreenCount = MaxCompanionScreenId + 1;
constexpr uint8 kAllAuxiliaryScreenMask = (1u << MaxCompanionScreenId) - 1u;

enum class MediaType : uint32
{
	Marquee = 0,
	Cover = 1,
	Box3D = 2,
	MixImage = 3,
	BackCover = 4,
	PhysicalMedia = 5,
	Screenshot = 6,
	FanArt = 7,
	TitleScreen = 8,
	Video = 9,
};

enum class LaunchStatus : uint32
{
	Accepted = 0,
	StaleRevision = 1,
	UnknownEntry = 2,
	PermissionDenied = 3,
	Busy = 4,
};

struct CatalogueEntry
{
	uint64 token = 0;
	uint32 version = 0;
	std::string name;
	std::string platform;
	std::string region;
	std::string format;
	fs::path path;
	fs::path metadataPath;
	Metadata metadata;
	std::vector<MediaAsset> media;
	uint32 mediaMask = 0;
};

struct CatalogueSnapshot
{
	uint32 revision = 1;
	std::vector<CatalogueEntry> entries;
	std::vector<MediaAsset> platformMedia;
	uint32 platformMediaMask = 0;
};

struct StreamCommand
{
	bool start = false;
	bool padView = false;
	uint8 screenId = 0;
	uint32 authorizationToken = 0;
	StreamRequest request;
};

std::mutex s_mutex;
std::condition_variable s_wake;
std::thread s_worker;
std::atomic<bool> s_running{ false };
std::string s_runningTitleName;
std::string s_status;
std::optional<ItemId> s_localLaunchItem;
std::optional<ItemId> s_pendingLaunchItem;
std::optional<uint64> s_launchRequestToken;
std::optional<uint64> s_pendingSelectionToken;
std::optional<uint64> s_selectedTitleToken;
std::atomic<uint32> s_catalogueRevision{ 0 };
std::atomic<uint32> s_mediaServedRevision{ 0 };
std::array<uint32, 11> s_keyboardWords{};
std::deque<StreamCommand> s_streamCommands;
std::array<std::optional<ActiveStream>, kStreamScreenCount> s_reportedStreams{};
uint32 s_nextAuthorizationToken = (uint32)std::chrono::steady_clock::now().time_since_epoch().count();
uint32 s_pendingAuthorizationToken = 0;
uint8 s_pendingStreamMask = 0;
uint8 s_pendingScreenMask = 0;
std::chrono::steady_clock::time_point s_authorizationDeadline{};

ICompanionHost* s_frontendHost = nullptr;
std::string s_applicationName;
std::string s_platformName;
std::string s_metadataLanguage;
std::mutex s_transportMutex;
IExtensionPacketTransport* s_transport = nullptr;
std::atomic<std::int64_t> s_lastClientActivityMilliseconds{ 0 };

std::int64_t MonotonicMilliseconds()
{
	return std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count();
}

uint8 AvailableScreenMask(const DsuEndpoint& endpoint)
{
	const uint8 auxiliaryMask = endpoint.secondaryStreamingSupported
		? endpoint.auxiliaryStreamingScreenMask & kAllAuxiliaryScreenMask : 0;
	return (endpoint.mainStreamingSupported ? 0x01 : 0x00) |
		(uint8)(auxiliaryMask << 1);
}

uint8 StreamControlMask(uint8 screenMask)
{
	return ((screenMask & 0x01) ? 0x01 : 0x00) |
		((screenMask & 0x1E) ? 0x02 : 0x00);
}

uint8 AvailableStreamMask(const DsuEndpoint& endpoint)
{
	return StreamControlMask(AvailableScreenMask(endpoint));
}

uint8 SupportedScreenMask(const DsuEndpoint& endpoint)
{
	return endpoint.streamingEnabled ? AvailableScreenMask(endpoint) : 0;
}

uint8 SupportedStreamMask(const DsuEndpoint& endpoint)
{
	return StreamControlMask(SupportedScreenMask(endpoint));
}

uint32 AdvertisedCapabilities(const DsuEndpoint& endpoint)
{
	const uint8 streamMask = SupportedStreamMask(endpoint);
	return kBaseCapabilities |
		((streamMask & 0x01) ? kCapabilityMainStream : 0) |
		((streamMask & 0x02) ? kCapabilityAuxStream : 0);
}

template<typename... Args>
void Log(LogLevel level, fmt::format_string<Args...> format, Args&&... args)
{
	if (s_frontendHost)
		s_frontendHost->Log(level, fmt::format(format, std::forward<Args>(args)...));
}

std::string PathToUtf8(const fs::path& path)
{
	const auto text = path.u8string();
	return { reinterpret_cast<const char*>(text.data()), text.size() };
}

uint16 ReadU16(const uint8* data)
{
	return (uint16)(data[0] | ((uint16)data[1] << 8));
}

uint32 ReadU32(const uint8* data)
{
	return (uint32)data[0] | ((uint32)data[1] << 8) | ((uint32)data[2] << 16) | ((uint32)data[3] << 24);
}

uint64 ReadU64(const uint8* data)
{
	return (uint64)ReadU32(data) | ((uint64)ReadU32(data + 4) << 32);
}

void WriteU16(uint8* data, uint16 value)
{
	data[0] = (uint8)value;
	data[1] = (uint8)(value >> 8);
}

void WriteU32(uint8* data, uint32 value)
{
	for (int i = 0; i < 4; ++i)
		data[i] = (uint8)(value >> (i * 8));
}

void WriteU64(uint8* data, uint64 value)
{
	WriteU32(data, (uint32)value);
	WriteU32(data + 4, (uint32)(value >> 32));
}

uint32 Crc32(const uint8* data, size_t size)
{
	static const std::array<uint32, 256> table = [] {
		std::array<uint32, 256> values{};
		for (uint32 i = 0; i < values.size(); ++i)
		{
			uint32 value = i;
			for (int bit = 0; bit < 8; ++bit)
				value = (value & 1) ? (0xEDB88320u ^ (value >> 1)) : (value >> 1);
			values[i] = value;
		}
		return values;
	}();

	uint32 crc = 0xFFFFFFFFu;
	for (size_t i = 0; i < size; ++i)
		crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
	return crc ^ 0xFFFFFFFFu;
}

void WriteFixedString(uint8* destination, size_t capacity, std::string_view value)
{
	std::memset(destination, 0, capacity);
	if (capacity == 0)
		return;
	const size_t count = std::min(value.size(), capacity - 1);
	std::memcpy(destination, value.data(), count);
}

std::vector<uint8> BuildDsuPacket(uint32 type, size_t payloadSize)
{
	std::vector<uint8> packet(kDsuHeaderSize + payloadSize, 0);
	std::memcpy(packet.data(), "DSUC", 4);
	WriteU16(packet.data() + 4, kDsuProtocolVersion);
	WriteU16(packet.data() + 6, (uint16)(payloadSize + 4));
	WriteU32(packet.data() + 12, kDsuClientId);
	WriteU32(packet.data() + 16, type);
	return packet;
}

void FinalizeDsuPacket(std::vector<uint8>& packet)
{
	WriteU32(packet.data() + 8, 0);
	WriteU32(packet.data() + 8, Crc32(packet.data(), packet.size()));
}

std::vector<uint8> BuildStatePacket(const std::string& titleName, uint32 capabilities)
{
	auto packet = BuildDsuPacket(kMsgState, kStatePayloadSize);
	uint8* payload = packet.data() + kDsuHeaderSize;
	payload[0] = titleName.empty() ? 0 : kStateFlagContentLoaded;
	WriteFixedString(payload + 1, kStateStringSize, titleName);
	WriteFixedString(payload + 65, kStateStringSize, s_platformName);
	WriteFixedString(payload + 129, kStateStringSize, s_applicationName);
	WriteFixedString(payload + 193, kStateStringSize, s_applicationName);
	WriteU32(payload + 257, capabilities);
	FinalizeDsuPacket(packet);
	return packet;
}

std::vector<uint8> BuildLaunchStatus(LaunchStatus status, uint32 revision, uint64 token)
{
	auto packet = BuildDsuPacket(kMsgAppLaunchStatus, 144);
	uint8* payload = packet.data() + kDsuHeaderSize;
	WriteU32(payload, (uint32)status);
	WriteU32(payload + 4, revision);
	WriteU64(payload + 8, token);

	std::string_view message = "Launch accepted";
	switch (status)
	{
	case LaunchStatus::StaleRevision:
		message = "Library changed; refresh required";
		break;
	case LaunchStatus::UnknownEntry:
		message = "Unknown library entry";
		break;
	case LaunchStatus::PermissionDenied:
		message = "Remote launching is disabled";
		break;
	case LaunchStatus::Busy:
		message = "Another launch is pending";
		break;
	case LaunchStatus::Accepted:
		break;
	}
	WriteFixedString(payload + 16, 128, message);
	FinalizeDsuPacket(packet);
	return packet;
}

std::vector<uint8> BuildAppSelection(uint32 revision, uint64 token)
{
	auto packet = BuildDsuPacket(kMsgAppSelection, 12);
	uint8* payload = packet.data() + kDsuHeaderSize;
	WriteU32(payload, revision);
	WriteU64(payload + 4, token);
	FinalizeDsuPacket(packet);
	return packet;
}

std::vector<uint8> BuildStreamControl(bool start, uint8 streamMask, uint32 token)
{
	auto packet = BuildDsuPacket(kMsgStreamControl, 6);
	uint8* payload = packet.data() + kDsuHeaderSize;
	payload[0] = start ? 1 : 0;
	payload[1] = streamMask & 0x03;
	WriteU32(payload + 2, token);
	FinalizeDsuPacket(packet);
	return packet;
}

std::vector<uint8> BuildStreamStatus(uint32 state, uint32 errorCode, uint32 streamType,
	uint8 screenId, std::string_view url, uint16 width = 0, uint16 height = 0,
	bool audioEnabled = false)
{
	auto packet = BuildDsuPacket(kMsgStreamStatus, 281);
	uint8* payload = packet.data() + kDsuHeaderSize;
	WriteU32(payload, state);
	WriteU32(payload + 4, errorCode);
	WriteU32(payload + 8, streamType);
	payload[12] = screenId;
	WriteFixedString(payload + 16, 256, url);
	WriteU16(payload + 272, width);
	WriteU16(payload + 274, height);
	float aspect = height ? (float)width / height : 0.0f;
	std::memcpy(payload + 276, &aspect, sizeof(aspect));
	payload[280] = audioEnabled ? 1 : 0;
	FinalizeDsuPacket(packet);
	return packet;
}

std::optional<StreamCommand> ParseStreamCommand(uint32 type, const uint8* payload, size_t payloadSize)
{
	StreamCommand command;
	command.start = type == kMsgStreamStart || type == kMsgAuxStreamStart;
	command.padView = type == kMsgAuxStreamStart || type == kMsgAuxStreamStop;
	command.screenId = command.padView ? 1 : 0;
	command.request.screenId = command.screenId;
	command.request.secondaryScreen = command.padView;
	if (!command.start)
	{
		if (command.padView && payloadSize >= 1)
			command.screenId = payload[0];
		command.request.screenId = command.screenId;
		return command;
	}

	const size_t base = command.padView ? 1 : 0;
	const size_t minimum = command.padView ? 270 : 269;
	if (payloadSize < minimum)
		return std::nullopt;
	command.screenId = command.padView ? payload[0] : 0;
	command.request.screenId = command.screenId;
	command.request.streamType = ReadU32(payload + base);
	command.request.url.assign(reinterpret_cast<const char*>(payload + base + 4),
		strnlen(reinterpret_cast<const char*>(payload + base + 4), 256));
	command.request.bitrateKbps = ReadU32(payload + base + 260);
	command.request.width = ReadU16(payload + base + 264);
	command.request.height = ReadU16(payload + base + 266);
	command.request.framesPerSecond = payload[base + 268];
	command.request.audioEnabled = payloadSize > base + 269 ? payload[base + 269] != 0 : true;
	if (payloadSize >= base + 274)
		command.authorizationToken = ReadU32(payload + base + 270);
	return command;
}

const rapidjson::Value* JsonMember(const rapidjson::Value& object, const char* name)
{
	if (!object.IsObject())
		return nullptr;
	const auto member = object.FindMember(name);
	return member == object.MemberEnd() ? nullptr : &member->value;
}

std::string JsonText(const rapidjson::Value& value)
{
	if (value.IsString())
		return std::string(value.GetString(), value.GetStringLength());
	if (const rapidjson::Value* text = JsonMember(value, "text"); text && text->IsString())
		return std::string(text->GetString(), text->GetStringLength());
	return {};
}

std::string JsonMemberText(const rapidjson::Value& object, const char* name)
{
	const rapidjson::Value* value = JsonMember(object, name);
	return value ? JsonText(*value) : std::string{};
}

std::string PreferredArrayText(const rapidjson::Value* values, const char* selector,
	const std::vector<std::string_view>& preferences)
{
	if (!values)
		return {};
	if (!values->IsArray())
		return JsonText(*values);

	for (const std::string_view preference : preferences)
	{
		for (const auto& value : values->GetArray())
		{
			const rapidjson::Value* selected = JsonMember(value, selector);
			if (!selected || !selected->IsString() ||
				std::string_view(selected->GetString(), selected->GetStringLength()) != preference)
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

std::string NamedMetadataValues(const rapidjson::Value& game, const char* memberName,
	const std::vector<std::string_view>& languages)
{
	const rapidjson::Value* groups = JsonMember(game, memberName);
	if (!groups || !groups->IsArray())
		return {};

	std::vector<std::string> names;
	for (const auto& group : groups->GetArray())
	{
		std::string name = PreferredArrayText(JsonMember(group, "noms"), "langue", languages);
		if (name.empty())
			name = JsonText(group);
		if (!name.empty() && std::find(names.begin(), names.end(), name) == names.end())
			names.emplace_back(std::move(name));
	}

	std::string joined;
	for (const std::string& name : names)
	{
		if (!joined.empty())
			joined += ", ";
		joined += name;
	}
	return joined;
}

std::string NormalizeReleaseDate(std::string date)
{
	if (date.size() >= 10 && date[4] == '-' && date[7] == '-')
		return date.substr(0, 4) + date.substr(5, 2) + date.substr(8, 2) + "T000000";
	return date;
}

std::string NormalizedRating(const std::string& value)
{
	return NormalizeScreenScraperApiRating(value);
}

std::string ContentRating(const rapidjson::Value& game, std::string_view region)
{
	const rapidjson::Value* classifications = JsonMember(game, "classifications");
	if (!classifications || !classifications->IsArray())
		return {};

	std::vector<std::string_view> preferences{ "ESRB" };
	if (region == "Europe")
		preferences.emplace_back("PEGI");
	else if (region == "Japan")
		preferences.emplace_back("CERO");
	else if (region == "Australia")
		preferences.emplace_back("ACB");
	preferences.insert(preferences.end(), { "PEGI", "CERO", "ACB", "USK" });

	for (const std::string_view preference : preferences)
	{
		for (const auto& classification : classifications->GetArray())
		{
			const rapidjson::Value* type = JsonMember(classification, "type");
			if (!type || !type->IsString() ||
				std::string_view(type->GetString(), type->GetStringLength()) != preference)
				continue;
			const std::string rating = JsonText(classification);
			if (!rating.empty())
				return fmt::format("{} {}", preference, rating);
		}
	}
	return {};
}

Metadata LoadCatalogueMetadata(const fs::path& metadataPath, std::string_view region,
	std::string_view language)
{
	constexpr std::streamoff kMaximumMetadataBytes = 4 * 1024 * 1024;
	std::ifstream input(metadataPath, std::ios::binary | std::ios::ate);
	if (!input)
		return {};
	const std::streamoff length = input.tellg();
	if (length <= 0 || length > kMaximumMetadataBytes)
		return {};

	std::string json((size_t)length, '\0');
	input.seekg(0, std::ios::beg);
	if (!input.read(json.data(), (std::streamsize)json.size()))
		return {};

	rapidjson::Document document;
	document.Parse(json.data(), json.size());
	if (document.HasParseError() || !document.IsObject())
		return {};

	const rapidjson::Value* response = JsonMember(document, "response");
	const rapidjson::Value* game = response ? JsonMember(*response, "jeu") : nullptr;
	if (!game)
		game = JsonMember(document, "jeu");
	if (!game || !game->IsObject())
		return {};

	std::vector<std::string_view> dateRegions;
	if (region == "USA")
		dateRegions.emplace_back("us");
	else if (region == "Europe")
		dateRegions.emplace_back("eu");
	else if (region == "Japan")
		dateRegions.emplace_back("jp");
	else if (region == "Australia")
		dateRegions.emplace_back("au");
	dateRegions.insert(dateRegions.end(), { "wor", "us", "eu", "jp" });

	std::vector<std::string_view> languages;
	if (!language.empty() && language != "en")
		languages.emplace_back(language);
	languages.insert(languages.end(), { "en", "ss", "wor" });

	Metadata metadata;
	metadata.description = PreferredArrayText(JsonMember(*game, "synopsis"), "langue", languages);
	metadata.genre = NamedMetadataValues(*game, "genres", languages);
	metadata.developer = JsonMemberText(*game, "developpeur");
	metadata.publisher = JsonMemberText(*game, "editeur");
	metadata.players = JsonMemberText(*game, "joueurs");
	metadata.releaseDate = NormalizeReleaseDate(PreferredArrayText(JsonMember(*game, "dates"), "region", dateRegions));
	metadata.rating = NormalizedRating(JsonMemberText(*game, "note"));
	metadata.contentRating = ContentRating(*game, region);
	metadata.modes = NamedMetadataValues(*game, "modes", languages);
	metadata.themes = NamedMetadataValues(*game, "themes", languages);
	return metadata;
}

std::optional<MediaKind> ToMediaKind(MediaType type)
{
	switch (type)
	{
	case MediaType::Marquee: return MediaKind::Logo;
	case MediaType::Cover: return MediaKind::Cover2D;
	case MediaType::Box3D: return MediaKind::Cover3D;
	case MediaType::MixImage: return MediaKind::MixRecalboxV2;
	case MediaType::BackCover: return MediaKind::BackCover;
	case MediaType::PhysicalMedia: return MediaKind::PhysicalMedia;
	case MediaType::Screenshot: return MediaKind::Screenshot;
	case MediaType::FanArt: return MediaKind::FanArt;
	case MediaType::TitleScreen: return MediaKind::TitleScreen;
	case MediaType::Video: return MediaKind::Video;
	default: return std::nullopt;
	}
}

bool IsRegularFile(const fs::path& path)
{
	if (path.empty())
		return false;
	std::error_code error;
	return fs::is_regular_file(path, error) && !error;
}

fs::path MediaPath(const std::vector<MediaAsset>& media, MediaType type)
{
	const auto kind = ToMediaKind(type);
	if (!kind)
		return {};
	const auto found = std::find_if(media.begin(), media.end(),
		[kind](const MediaAsset& asset) { return asset.kind == *kind; });
	return found == media.end() ? fs::path{} : found->path;
}

fs::path MediaPath(const CatalogueEntry& entry, MediaType type)
{
	return MediaPath(entry.media, type);
}

uint32 BuildMediaMask(const std::vector<MediaAsset>& media)
{
	uint32 mask = 0;
	for (uint32 type = 0; type <= (uint32)MediaType::Video; ++type)
	{
		if (IsRegularFile(MediaPath(media, (MediaType)type)))
			mask |= 1u << type;
	}
	return mask;
}

uint32 BuildMediaMask(const CatalogueEntry& entry)
{
	return BuildMediaMask(entry.media);
}

void HashBytes(uint32& hash, const void* data, size_t size)
{
	const auto* bytes = static_cast<const uint8*>(data);
	for (size_t i = 0; i < size; ++i)
	{
		hash ^= bytes[i];
		hash *= 16777619u;
	}
}

void HashText(uint32& hash, std::string_view text)
{
	const uint64 length = text.size();
	HashBytes(hash, &length, sizeof(length));
	HashBytes(hash, text.data(), text.size());
}

void HashPath(uint32& hash, const fs::path& path)
{
	HashText(hash, PathToUtf8(path));
	if (path.empty())
		return;

	std::error_code error;
	const bool regular = fs::is_regular_file(path, error) && !error;
	HashBytes(hash, &regular, sizeof(regular));
	if (!regular)
		return;

	const uintmax_t size = fs::file_size(path, error);
	if (!error)
		HashBytes(hash, &size, sizeof(size));
	error.clear();
	const auto modified = fs::last_write_time(path, error);
	if (!error)
	{
		const auto ticks = modified.time_since_epoch().count();
		HashBytes(hash, &ticks, sizeof(ticks));
	}
}

void HashMetadata(uint32& hash, const Metadata& metadata)
{
	HashText(hash, metadata.description);
	HashText(hash, metadata.genre);
	HashText(hash, metadata.developer);
	HashText(hash, metadata.publisher);
	HashText(hash, metadata.players);
	HashText(hash, metadata.releaseDate);
	HashText(hash, metadata.rating);
	HashText(hash, metadata.contentRating);
	HashText(hash, metadata.modes);
	HashText(hash, metadata.themes);
}

CatalogueSnapshot BuildCatalogueSnapshot()
{
	// every entry probes the shared media tree, so a burst of requests reuses one scan
	constexpr auto kSnapshotLifetime = std::chrono::seconds(5);
	static std::mutex cacheMutex;
	static CatalogueSnapshot cached;
	static std::chrono::steady_clock::time_point cachedAt{};
	{
		std::lock_guard lock(cacheMutex);
		if (cached.revision != 0 && std::chrono::steady_clock::now() - cachedAt < kSnapshotLifetime)
			return cached;
	}

	CatalogueSnapshot snapshot;
	if (!s_frontendHost)
		return snapshot;
	auto items = s_frontendHost->SnapshotCatalogue();
	snapshot.platformMedia = s_frontendHost->SnapshotPlatformMedia();
	snapshot.platformMediaMask = BuildMediaMask(snapshot.platformMedia);
	snapshot.entries.reserve(items.size());
	for (auto& item : items)
	{
		if (item.id == 0 || item.launchPath.empty())
			continue;

		CatalogueEntry entry;
		entry.token = item.id;
		entry.version = item.version;
		entry.path = std::move(item.launchPath);
		entry.name = std::move(item.name);
		if (entry.name.empty())
			entry.name = fmt::format("{:016x}", entry.token);
		entry.platform = std::move(item.platform);
		entry.region = std::move(item.region);
		entry.format = std::move(item.format);
		entry.metadataPath = std::move(item.metadataPath);
		entry.metadata = std::move(item.metadata);
		entry.media = std::move(item.media);
		entry.mediaMask = BuildMediaMask(entry);
		snapshot.entries.emplace_back(std::move(entry));
	}

	std::sort(snapshot.entries.begin(), snapshot.entries.end(), [](const auto& left, const auto& right) {
		const auto lowerAscii = [](std::string value) {
			std::transform(value.begin(), value.end(), value.begin(),
				[](unsigned char character) { return (char)std::tolower(character); });
			return value;
		};
		return lowerAscii(left.name) < lowerAscii(right.name);
	});

	uint32 revision = 2166136261u;
	for (const auto& entry : snapshot.entries)
	{
		HashBytes(revision, &entry.token, sizeof(entry.token));
		HashBytes(revision, &entry.version, sizeof(entry.version));
		HashText(revision, entry.name);
		HashText(revision, entry.platform);
		HashText(revision, entry.region);
		HashText(revision, entry.format);
		HashPath(revision, entry.path);
		HashPath(revision, entry.metadataPath);
		HashMetadata(revision, entry.metadata);
		HashBytes(revision, &entry.mediaMask, sizeof(entry.mediaMask));

		std::vector<const MediaAsset*> media;
		media.reserve(entry.media.size());
		for (const auto& asset : entry.media)
			media.emplace_back(&asset);
		std::sort(media.begin(), media.end(), [](const MediaAsset* left, const MediaAsset* right) {
			if (left->kind != right->kind)
				return left->kind < right->kind;
			return PathToUtf8(left->path) < PathToUtf8(right->path);
		});
		for (const MediaAsset* asset : media)
		{
			HashBytes(revision, &asset->kind, sizeof(asset->kind));
			HashPath(revision, asset->path);
		}
	}
	HashBytes(revision, &snapshot.platformMediaMask,
		sizeof(snapshot.platformMediaMask));
	for (const auto& asset : snapshot.platformMedia)
	{
		HashBytes(revision, &asset.kind, sizeof(asset.kind));
		HashPath(revision, asset.path);
	}
	snapshot.revision = revision == 0 ? 1 : revision;
	{
		std::lock_guard lock(cacheMutex);
		cached = snapshot;
		cachedAt = std::chrono::steady_clock::now();
	}
	return snapshot;
}

const CatalogueEntry* FindEntry(const CatalogueSnapshot& snapshot, uint32 revision, uint64 token)
{
	if (revision != snapshot.revision)
		return nullptr;
	const auto it = std::find_if(snapshot.entries.begin(), snapshot.entries.end(),
		[token](const auto& entry) { return entry.token == token; });
	return it == snapshot.entries.end() ? nullptr : &*it;
}

std::string BuildCatalogueJson(const CatalogueSnapshot& snapshot)
{
	rapidjson::StringBuffer buffer;
	rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
	const auto string = [&writer](std::string_view value) {
		writer.String(value.data(), (rapidjson::SizeType)value.size());
	};

	writer.StartObject();
	writer.Key("version");
	writer.Uint(1);
	writer.Key("application");
	string(s_applicationName);
	writer.Key("playlists");
	writer.StartArray();
	writer.StartObject();
	writer.Key("name");
	string(s_platformName);
	writer.Key("platform_media");
	writer.Uint(snapshot.platformMediaMask);
	writer.Key("platform_media_revision");
	writer.Uint(snapshot.revision);
	writer.Key("platform_media_token");
	writer.String("0000000000000000");
	writer.Key("count");
	writer.Uint64(snapshot.entries.size());
	writer.Key("entries");
	writer.StartArray();
	for (const auto& entry : snapshot.entries)
	{
		const std::string token = fmt::format("{:016X}", entry.token);
		Metadata metadata = entry.metadata;
		if (!entry.metadataPath.empty())
		{
			MergeMissingMetadata(metadata,
				LoadScreenScraperMetadata(entry.metadataPath, entry.region, s_metadataLanguage));
		}
		const auto metadataField = [&writer, &string](const char* key, const std::string& value) {
			if (!value.empty())
			{
				writer.Key(key);
				string(value);
			}
		};
		writer.StartObject();
		writer.Key("label"); string(entry.name);
		std::string scheme = s_applicationName;
		std::transform(scheme.begin(), scheme.end(), scheme.begin(), [](unsigned char value) {
			return std::isalnum(value) ? static_cast<char>(std::tolower(value)) : '-';
		});
		writer.Key("path"); string(fmt::format("{}://{}/{}", scheme, snapshot.revision, token));
		writer.Key("file"); string(PathToUtf8(entry.path.filename()));
		writer.Key("core"); string(s_applicationName);
		writer.Key("core_path"); writer.String("");
		writer.Key("db"); string(entry.platform.empty() ?
			std::string_view(s_platformName) : std::string_view(entry.platform));
		writer.Key("platform"); string(entry.platform.empty() ?
			std::string_view(s_platformName) : std::string_view(entry.platform));
		writer.Key("crc32"); writer.String("");
		writer.Key("img"); writer.String("");
		writer.Key("img_full"); writer.String("");
		writer.Key("img_short"); writer.String("");
		writer.Key("runtime"); writer.String("");
		writer.Key("last_played"); writer.String("");
		writer.Key("app"); string(s_applicationName);
		writer.Key("launch_revision"); writer.Uint(snapshot.revision);
		writer.Key("launch_token"); string(token);
		writer.Key("media"); writer.Uint(entry.mediaMask);
		metadataField("desc", metadata.description);
		metadataField("genre", metadata.genre);
		metadataField("dev", metadata.developer);
		metadataField("pub", metadata.publisher);
		metadataField("players", metadata.players);
		metadataField("released", metadata.releaseDate);
		metadataField("rating", metadata.rating);
		metadataField("content_rating", metadata.contentRating);
		metadataField("modes", metadata.modes);
		metadataField("themes", metadata.themes);
		writer.Key("region"); string(entry.region);
		writer.Key("format"); string(entry.format);
		writer.EndObject();
	}
	writer.EndArray();
	writer.EndObject();
	writer.EndArray();
	writer.EndObject();
	return std::string(buffer.GetString(), buffer.GetSize());
}

std::string UrlDecode(std::string_view value)
{
	std::string result;
	result.reserve(value.size());
	for (size_t index = 0; index < value.size(); ++index)
	{
		if (value[index] == '%' && index + 2 < value.size())
		{
			unsigned decoded = 0;
			const std::string hex(value.substr(index + 1, 2));
			const auto conversion = std::from_chars(hex.data(), hex.data() + hex.size(), decoded, 16);
			if (conversion.ec == std::errc{})
			{
				result.push_back((char)decoded);
				index += 2;
				continue;
			}
		}
		result.push_back(value[index] == '+' ? ' ' : value[index]);
	}
	return result;
}

std::string QueryValue(std::string_view query, std::string_view key)
{
	while (!query.empty())
	{
		const size_t separator = query.find('&');
		const std::string_view field = query.substr(0, separator);
		const size_t equals = field.find('=');
		if (equals != std::string_view::npos && field.substr(0, equals) == key)
			return UrlDecode(field.substr(equals + 1));
		if (separator == std::string_view::npos)
			break;
		query.remove_prefix(separator + 1);
	}
	return {};
}

std::optional<uint64> ParseHexToken(std::string_view value)
{
	uint64 token = 0;
	const auto result = std::from_chars(value.data(), value.data() + value.size(), token, 16);
	if (value.empty() || result.ec != std::errc{} || result.ptr != value.data() + value.size())
		return std::nullopt;
	return token;
}

struct HttpReference
{
	uint32 revision = 0;
	uint64 token = 0;
};

std::optional<HttpReference> ParseHttpReference(std::string_view query)
{
	uint32 revision = 0;
	const std::string revisionText = QueryValue(query, "revision");
	const auto revisionResult = std::from_chars(
		revisionText.data(), revisionText.data() + revisionText.size(), revision);
	const auto token = ParseHexToken(QueryValue(query, "token"));
	if (revisionText.empty() || revisionResult.ec != std::errc{} ||
		revisionResult.ptr != revisionText.data() + revisionText.size() || !token)
		return std::nullopt;
	return HttpReference{ revision, *token };
}

std::string ContentType(const fs::path& path)
{
	std::string extension = PathToUtf8(path.extension());
	std::transform(extension.begin(), extension.end(), extension.begin(),
		[](unsigned char value) { return (char)std::tolower(value); });
	if (extension == ".png") return "image/png";
	if (extension == ".jpg" || extension == ".jpeg") return "image/jpeg";
	if (extension == ".webp") return "image/webp";
	if (extension == ".bmp") return "image/bmp";
	if (extension == ".gif") return "image/gif";
	if (extension == ".mp4" || extension == ".m4v") return "video/mp4";
	if (extension == ".mkv") return "video/x-matroska";
	if (extension == ".webm") return "video/webm";
	if (extension == ".avi") return "video/x-msvideo";
	if (extension == ".mov") return "video/quicktime";
	if (extension == ".wmv") return "video/x-ms-wmv";
	if (extension == ".mpg") return "video/mpeg";
	if (extension == ".3gp") return "video/3gpp";
	return "application/octet-stream";
}

bool SendAll(tcp::socket& socket, const void* data, size_t size)
{
	boost::system::error_code error;
	boost::asio::write(socket, boost::asio::buffer(data, size), error);
	return !error;
}

void SendHeader(tcp::socket& socket, std::string_view status, std::string_view contentType, uintmax_t length)
{
	const std::string header = fmt::format(
		"HTTP/1.1 {}\r\nContent-Type: {}\r\nContent-Length: {}\r\n"
		"Access-Control-Allow-Origin: *\r\nCache-Control: no-cache\r\nConnection: close\r\n\r\n",
		status, contentType, length);
	SendAll(socket, header.data(), header.size());
}

void SendText(tcp::socket& socket, std::string_view status, std::string_view contentType, std::string_view body)
{
	SendHeader(socket, status, contentType, body.size());
	if (!body.empty())
		SendAll(socket, body.data(), body.size());
}

void SendFile(tcp::socket& socket, const fs::path& path)
{
	std::error_code error;
	const uintmax_t length = fs::file_size(path, error);
	if (error)
	{
		SendText(socket, "404 Not Found", "text/plain", "404 Not Found");
		return;
	}

	std::ifstream input(path, std::ios::binary);
	if (!input)
	{
		SendText(socket, "404 Not Found", "text/plain", "404 Not Found");
		return;
	}

	SendHeader(socket, "200 OK", ContentType(path), length);
	std::vector<char> chunk(kFileChunk);
	while (input)
	{
		input.read(chunk.data(), (std::streamsize)chunk.size());
		const std::streamsize count = input.gcount();
		if (count <= 0 || !SendAll(socket, chunk.data(), (size_t)count))
			break;
	}
}

class CatalogueServer
{
public:
	bool Start(uint16 port)
	{
		Stop();
		m_port = port;
		m_startupFinished = false;
		m_requested = true;
		m_worker = std::thread(&CatalogueServer::Worker, this);
		std::unique_lock lock(m_startupMutex);
		m_startupCondition.wait_for(lock, std::chrono::seconds(3), [this] { return m_startupFinished; });
		return m_listening.load();
	}

	void Stop()
	{
		m_requested = false;
		std::shared_ptr<tcp::socket> client;
		{
			std::lock_guard lock(m_clientMutex);
			client = m_client;
		}
		if (client)
		{
			boost::system::error_code ignored;
			client->cancel(ignored);
			client->shutdown(tcp::socket::shutdown_both, ignored);
			client->close(ignored);
		}
		if (m_worker.joinable())
			m_worker.join();
		{
			std::lock_guard lock(m_clientMutex);
			m_client.reset();
		}
		m_listening = false;
		m_acceptor.reset();
	}

	bool IsRunning() const { return m_listening.load(); }

private:
	void Worker()
	{
		boost::system::error_code error;
		m_acceptor = std::make_unique<tcp::acceptor>(m_context);
		m_acceptor->open(tcp::v4(), error);
		if (!error) m_acceptor->set_option(tcp::acceptor::reuse_address(true), error);
		if (!error) m_acceptor->bind(tcp::endpoint(tcp::v4(), m_port), error);
		if (!error) m_acceptor->listen(4, error);
		if (!error) m_acceptor->non_blocking(true, error);
		{
			std::lock_guard lock(m_startupMutex);
			m_startupFinished = true;
			m_listening = !error;
		}
		m_startupCondition.notify_all();
		if (error)
		{
			Log(LogLevel::Information, "Companion service: catalogue server could not listen on port {} ({})", m_port, error.message());
			m_requested = false;
			return;
		}

		Log(LogLevel::Debug, "Companion app: catalogue server listening on port {}", m_port);
		while (m_requested.load())
		{
			auto client = std::make_shared<tcp::socket>(m_context);
			m_acceptor->accept(*client, error);
			if (error == boost::asio::error::would_block || error == boost::asio::error::try_again)
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(20));
				continue;
			}
			if (error)
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(20));
				continue;
			}
			{
				std::lock_guard lock(m_clientMutex);
				if (!m_requested.load())
				{
					boost::system::error_code ignored;
					client->close(ignored);
					break;
				}
				m_client = client;
			}
			HandleClient(*client);
			{
				std::lock_guard lock(m_clientMutex);
				if (m_client == client)
					m_client.reset();
			}
		}

		boost::system::error_code ignored;
		m_acceptor->close(ignored);
		m_listening = false;
		Log(LogLevel::Debug, "Companion app: catalogue server stopped");
	}

	void HandleClient(tcp::socket& client)
	{
		std::array<char, kRequestLimit> request{};
		boost::system::error_code error;
		const size_t received = client.read_some(boost::asio::buffer(request), error);
		if (error || received == 0)
			return;

		const std::string_view text(request.data(), received);
		const size_t lineEnd = text.find("\r\n");
		const std::string_view line = text.substr(0, lineEnd);
		if (!line.starts_with("GET "))
		{
			SendText(client, "405 Method Not Allowed", "text/plain", "405 Method Not Allowed");
			return;
		}
		const size_t targetEnd = line.find(' ', 4);
		if (targetEnd == std::string_view::npos)
		{
			SendText(client, "400 Bad Request", "text/plain", "400 Bad Request");
			return;
		}

		const std::string_view target = line.substr(4, targetEnd - 4);
		const size_t question = target.find('?');
		const std::string_view path = target.substr(0, question);
		const std::string_view query = question == std::string_view::npos ? std::string_view{} : target.substr(question + 1);
		const CatalogueSnapshot snapshot = BuildCatalogueSnapshot();
		if (path == "/catalogue")
		{
			const uint32 previous = s_catalogueRevision.exchange(snapshot.revision,
				std::memory_order_relaxed);
			const std::string body = BuildCatalogueJson(snapshot);
			if (previous != snapshot.revision)
			{
				Log(LogLevel::Information,
					"Companion service: catalogue revision {} served, {} entries, platform media 0x{:x}",
					snapshot.revision, snapshot.entries.size(),
					BuildMediaMask(snapshot.platformMedia));
			}
			SendText(client, "200 OK", "application/json", body);
			return;
		}
		if (!path.starts_with("/media/"))
		{
			SendText(client, "404 Not Found", "text/plain", "404 Not Found");
			return;
		}

		uint32 type = 0;
		const std::string_view typeText = path.substr(7);
		const auto conversion = std::from_chars(typeText.data(), typeText.data() + typeText.size(), type);
		if (typeText.empty() || conversion.ec != std::errc{} ||
			conversion.ptr != typeText.data() + typeText.size() || type > (uint32)MediaType::Video)
		{
			SendText(client, "404 Not Found", "text/plain", "404 Not Found");
			return;
		}

		const auto reference = ParseHttpReference(query);
		fs::path mediaPath;
		if (reference)
		{
			if (reference->token == 0)
				mediaPath = MediaPath(snapshot.platformMedia, (MediaType)type);
			// the library can be rescanned between the catalogue and its media, match on the token
			else if (const CatalogueEntry* entry = FindEntry(snapshot, snapshot.revision, reference->token))
				mediaPath = MediaPath(*entry, (MediaType)type);
		}
		if (!IsRegularFile(mediaPath))
		{
			Log(LogLevel::Information, "Companion service: media {} unavailable for {}",
				type, std::string(query));
			SendText(client, "404 Not Found", "text/plain", "404 Not Found");
			return;
		}
		if (s_mediaServedRevision.exchange(snapshot.revision, std::memory_order_relaxed) !=
			snapshot.revision)
		{
			Log(LogLevel::Information, "Companion service: serving media for revision {}",
				snapshot.revision);
		}
		SendFile(client, mediaPath);
	}

	boost::asio::io_context m_context;
	std::unique_ptr<tcp::acceptor> m_acceptor;
	std::thread m_worker;
	std::atomic<bool> m_requested{ false };
	std::atomic<bool> m_listening{ false };
	uint16 m_port = 0;
	std::mutex m_clientMutex;
	std::shared_ptr<tcp::socket> m_client;
	std::mutex m_startupMutex;
	std::condition_variable m_startupCondition;
	bool m_startupFinished = false;
};

void SetStatus(std::string status)
{
	std::lock_guard lock(s_mutex);
	s_status = std::move(status);
}

void QueuePacket(std::vector<uint8> packet)
{
	std::lock_guard lock(s_transportMutex);
	if (s_transport)
		s_transport->SendExtensionPacket(std::move(packet), false);
}

void QueueSelectionPacket(std::vector<uint8> packet)
{
	std::lock_guard lock(s_transportMutex);
	if (s_transport)
		s_transport->SendExtensionPacket(std::move(packet), true);
}

bool KeyDownLocked(uint32 key)
{
	const size_t word = key / 32;
	const uint32 bit = key % 32;
	return word < s_keyboardWords.size() && (s_keyboardWords[word] & (1u << bit)) != 0;
}

void QueueStreamStatus(uint32 state, uint32 errorCode, uint32 streamType, uint8 screenId,
	std::string_view url = {}, uint16 width = 0, uint16 height = 0, bool audioEnabled = false)
{
	QueuePacket(BuildStreamStatus(state, errorCode, streamType, screenId, url,
		width, height, audioEnabled));
}

uint8 ActiveScreenId(const ActiveStream& active)
{
	return active.screenId != 0 ? active.screenId : (active.secondaryScreen ? 1 : 0);
}

ActiveStream NormalizeActiveStream(ActiveStream active, uint8 screenId)
{
	active.screenId = screenId;
	active.secondaryScreen = screenId != 0;
	return active;
}

std::optional<ActiveStream> GetActiveScreen(uint8 screenId)
{
	auto active = s_frontendHost
		? s_frontendHost->GetActiveScreenStream(screenId) : std::nullopt;
	if (active && ActiveScreenId(*active) == screenId)
		return NormalizeActiveStream(std::move(*active), screenId);
	active.reset();
	if (!active)
	{
		std::lock_guard lock(s_mutex);
		if (screenId < s_reportedStreams.size())
			active = s_reportedStreams[screenId];
	}
	return active;
}

void StopActiveScreen(uint8 screenId, bool sendStatus, bool stopIfInactive = true)
{
	const auto active = GetActiveScreen(screenId);
	if (s_frontendHost && (active || stopIfInactive))
		s_frontendHost->StopScreenStream(screenId);
	{
		std::lock_guard lock(s_mutex);
		if (screenId < s_reportedStreams.size())
			s_reportedStreams[screenId].reset();
	}
	if (sendStatus)
		QueueStreamStatus(0, 0, active ? active->streamType : 0, screenId);
}

void StopAllActiveStreams(bool sendStatus)
{
	for (uint8 screenId = 0; screenId <= MaxCompanionScreenId; ++screenId)
	{
		if (GetActiveScreen(screenId))
			StopActiveScreen(screenId, sendStatus, false);
	}
}

void ProcessStreamCommand(const StreamCommand& command)
{
	const DsuEndpoint endpoint = s_frontendHost ? s_frontendHost->GetDsuEndpoint() : DsuEndpoint{};
	const uint8 supportedScreenMask = SupportedScreenMask(endpoint);
	const uint8 requestedStreamBit = command.padView ? 0x02 : 0x01;
	const bool validScreenId = command.padView
		? command.screenId >= 1 && command.screenId <= MaxCompanionScreenId
		: command.screenId == 0;
	if (!command.start)
	{
		if (!validScreenId)
		{
			QueueStreamStatus(2, 3, 0, command.screenId);
			return;
		}
		// stopping an inactive screen must not tear down another screen's stream
		StopActiveScreen(command.screenId, true, false);
		return;
	}

	StreamRequest request = command.request;
	request.authorizationToken = command.authorizationToken;
	if (request.url.empty())
		request.url = endpoint.streamingUrl;
	const auto reportError = [&](uint32 code) {
		QueueStreamStatus(2, code, request.streamType, command.screenId,
			request.url, 0, 0, request.audioEnabled);
	};
	if (!validScreenId)
	{
		reportError(3);
		return;
	}
	const uint8 requestedScreenBit = (uint8)(1u << command.screenId);
	if ((supportedScreenMask & requestedScreenBit) == 0)
	{
		reportError(6);
		return;
	}
	if ((request.streamType != 3 && request.streamType != 4) ||
		!request.url.starts_with("udp://") ||
		((request.width == 0) != (request.height == 0)) ||
		request.width > 4096 || request.height > 4096 ||
		request.framesPerSecond == 0 || request.framesPerSecond > 60)
	{
		reportError(1);
		return;
	}

	bool titleRunning = false;
	bool authorized = command.authorizationToken == 0;
	{
		std::lock_guard lock(s_mutex);
		titleRunning = !s_runningTitleName.empty();
		if (s_pendingAuthorizationToken != 0 &&
			std::chrono::steady_clock::now() > s_authorizationDeadline)
		{
			s_pendingAuthorizationToken = 0;
			s_pendingStreamMask = 0;
			s_pendingScreenMask = 0;
		}
		if (command.authorizationToken != 0)
		{
			authorized = command.authorizationToken == s_pendingAuthorizationToken &&
				(s_pendingStreamMask & requestedStreamBit) != 0 &&
				(s_pendingScreenMask & requestedScreenBit) != 0;
		}
	}
	if (!titleRunning)
	{
		reportError(2);
		return;
	}
	if (!endpoint.streamingEnabled || !authorized)
	{
		reportError(6);
		return;
	}

	if (!endpoint.independentScreenStreamsSupported)
	{
		for (uint8 screenId = 0; screenId <= MaxCompanionScreenId; ++screenId)
		{
			if (GetActiveScreen(screenId))
				StopActiveScreen(screenId, screenId != command.screenId, false);
		}
	}
	else if (GetActiveScreen(command.screenId))
		StopActiveScreen(command.screenId, false, false);

	if (!s_frontendHost || !s_frontendHost->StartScreenStream(request))
	{
		const std::string error = s_frontendHost
			? s_frontendHost->TakeScreenStreamFailure(command.screenId).value_or(
				"stream encoder rejected the request")
			: "stream host is unavailable";
		Log(LogLevel::Error, "Companion service: stream start failed: {}", error);
		const bool runtimeFailure = error.find("runtime") != std::string::npos ||
			error.find("encoder is unavailable") != std::string::npos;
		reportError(runtimeFailure ? 5 : 4);
		return;
	}
	auto activeInfo = s_frontendHost->GetActiveScreenStream(command.screenId);
	if (!activeInfo)
	{
		s_frontendHost->StopScreenStream(command.screenId);
		reportError(4);
		return;
	}
	*activeInfo = NormalizeActiveStream(std::move(*activeInfo), command.screenId);

	{
		std::lock_guard lock(s_mutex);
		s_reportedStreams[command.screenId] = *activeInfo;
		if (command.authorizationToken != 0 && command.authorizationToken == s_pendingAuthorizationToken)
		{
			s_pendingScreenMask &= ~requestedScreenBit;
			if (s_pendingScreenMask == 0)
			{
				s_pendingStreamMask = 0;
				s_pendingAuthorizationToken = 0;
			}
		}
	}
	QueueStreamStatus(1, 0, activeInfo->streamType, command.screenId,
		activeInfo->url, static_cast<uint16>(activeInfo->width),
		static_cast<uint16>(activeInfo->height), activeInfo->audioEnabled);
}

void WorkerMain()
{
	CatalogueServer catalogueServer;
	uint16 cataloguePort = 0;
	bool lastRunning = false;
	std::string lastTitleName;
	uint32 lastCapabilities = 0;

	while (s_running.load())
	{
		std::deque<StreamCommand> commands;
		{
			std::lock_guard lock(s_mutex);
			commands = std::exchange(s_streamCommands, {});
		}
		for (const auto& command : commands)
			ProcessStreamCommand(command);
		for (uint8 screenId = 0; screenId <= MaxCompanionScreenId; ++screenId)
		{
			const auto failedInfo = GetActiveScreen(screenId);
			if (!failedInfo || !s_frontendHost)
				continue;
			const auto failure = s_frontendHost->TakeScreenStreamFailure(screenId);
			if (!failure)
				continue;
			Log(LogLevel::Error, "Companion service: screen {} encoder worker failed: {}",
				screenId, *failure);
			StopActiveScreen(screenId, false, false);
			QueueStreamStatus(2, 4, failedInfo->streamType, screenId);
		}

		const DsuEndpoint endpoint = s_frontendHost ? s_frontendHost->GetDsuEndpoint() : DsuEndpoint{};
		const uint32 capabilities = AdvertisedCapabilities(endpoint);
		const bool enabled = endpoint.enabled;
		const sint32 configuredPort = endpoint.port;
		const uint16 desiredCataloguePort = (uint16)std::clamp(configuredPort + 1, 1, 65535);

		if (!enabled)
		{
			if (catalogueServer.IsRunning())
				catalogueServer.Stop();
			cataloguePort = 0;
			SetStatus("disabled");
		}
		else
		{
			if (!catalogueServer.IsRunning() || cataloguePort != desiredCataloguePort)
			{
				catalogueServer.Stop();
				cataloguePort = catalogueServer.Start(desiredCataloguePort) ? desiredCataloguePort : 0;
			}

			std::string titleName;
			{
				std::lock_guard lock(s_mutex);
				titleName = s_runningTitleName;
			}
			QueuePacket(BuildStatePacket(titleName, capabilities));
			{
				// the lock must span QueueSelectionPacket so the token cannot change
				std::lock_guard lock(s_mutex);
				if (s_selectedTitleToken)
					QueueSelectionPacket(BuildAppSelection(s_catalogueRevision.load(std::memory_order_relaxed), *s_selectedTitleToken));
			}
			const bool running = !titleName.empty();
			if (running != lastRunning || titleName != lastTitleName || capabilities != lastCapabilities)
			{
				Log(LogLevel::Debug, "Companion app: state running={} title='{}' capabilities=0x{:08x}",
					running ? 1 : 0, titleName, capabilities);
				lastRunning = running;
				lastTitleName = titleName;
				lastCapabilities = capabilities;
			}
			SetStatus(cataloguePort != 0
				? fmt::format("companion ready; catalogue on port {}", cataloguePort)
				: "companion ready; catalogue server unavailable");
		}

		std::unique_lock lock(s_mutex);
		s_wake.wait_for(lock, kAnnounceInterval, [] {
			return !s_running.load() || !s_streamCommands.empty();
		});
	}
	StopAllActiveStreams(false);
	catalogueServer.Stop();
}
} // namespace

Metadata LoadScreenScraperMetadata(const fs::path& metadataPath, std::string_view region,
	std::string_view language)
{
	return LoadCatalogueMetadata(metadataPath, region, language);
}

void SetMetadataLanguage(std::string language)
{
	std::lock_guard lock(s_mutex);
	s_metadataLanguage = std::move(language);
}

void MergeMissingMetadata(Metadata& target, const Metadata& source)
{
	const auto fill = [](std::string& destination, const std::string& value) {
		if (destination.empty())
			destination = value;
	};
	fill(target.description, source.description);
	fill(target.genre, source.genre);
	fill(target.developer, source.developer);
	fill(target.publisher, source.publisher);
	fill(target.players, source.players);
	fill(target.releaseDate, source.releaseDate);
	fill(target.rating, source.rating);
	fill(target.contentRating, source.contentRating);
	fill(target.modes, source.modes);
	fill(target.themes, source.themes);
}

void Initialize(ICompanionHost& host, std::string applicationName, std::string platformName)
{
	s_frontendHost = &host;
	s_lastClientActivityMilliseconds.store(0, std::memory_order_relaxed);
	s_applicationName = applicationName.empty() ? "Application" : std::move(applicationName);
	s_platformName = platformName.empty() ? "Library" : std::move(platformName);
	if (s_running.exchange(true))
		return;
	s_worker = std::thread(WorkerMain);
}

void Shutdown()
{
	if (!s_running.exchange(false))
		return;
	s_wake.notify_all();
	if (s_worker.joinable())
		s_worker.join();
	{
		std::lock_guard lock(s_mutex);
		s_runningTitleName.clear();
		s_status.clear();
		s_localLaunchItem.reset();
		s_pendingLaunchItem.reset();
		s_launchRequestToken.reset();
		s_pendingSelectionToken.reset();
		s_selectedTitleToken.reset();
		s_catalogueRevision.store(0, std::memory_order_relaxed);
		s_keyboardWords.fill(0);
		s_streamCommands.clear();
		for (auto& stream : s_reportedStreams)
			stream.reset();
		s_pendingAuthorizationToken = 0;
		s_pendingStreamMask = 0;
		s_pendingScreenMask = 0;
	}
	{
		std::lock_guard lock(s_transportMutex);
		s_transport = nullptr;
	}
	s_lastClientActivityMilliseconds.store(0, std::memory_order_relaxed);
	s_frontendHost = nullptr;
}

void AttachTransport(IExtensionPacketTransport* transport)
{
	if (!transport)
		return;
	{
		std::lock_guard lock(s_mutex);
		s_keyboardWords.fill(0);
	}
	{
		std::lock_guard lock(s_transportMutex);
		s_transport = transport;
	}
	transport->RequestControllerData(0);
	s_wake.notify_all();
}

void DetachTransport(IExtensionPacketTransport* transport)
{
	bool detached = false;
	{
		std::lock_guard lock(s_transportMutex);
		if (s_transport == transport)
		{
			s_transport = nullptr;
			s_lastClientActivityMilliseconds.store(0, std::memory_order_relaxed);
			detached = true;
		}
	}
	if (detached)
	{
		std::lock_guard lock(s_mutex);
		s_keyboardWords.fill(0);
	}
}

bool HandleExtensionPacket(std::span<const uint8> request, std::vector<std::vector<uint8>>& responses)
{
	const uint8* data = request.data();
	const size_t size = request.size();
	if (!data || size < kDsuHeaderSize || std::memcmp(data, "DSUS", 4) != 0)
		return false;
	const uint16 packetLength = ReadU16(data + 6);
	if (packetLength < 4)
		return false;
	const size_t expectedSize = kDsuHeaderSize + packetLength - 4;
	if (expectedSize > size)
		return false;

	const uint32 type = ReadU32(data + 16);
	if (type < kMsgKeyboard)
		return false;

	std::vector<uint8> crcBytes(data, data + expectedSize);
	const uint32 expectedCrc = ReadU32(crcBytes.data() + 8);
	WriteU32(crcBytes.data() + 8, 0);
	if (Crc32(crcBytes.data(), crcBytes.size()) != expectedCrc)
		return true;
	s_lastClientActivityMilliseconds.store(MonotonicMilliseconds(), std::memory_order_relaxed);

	const uint8* payload = data + kDsuHeaderSize;
	const size_t payloadSize = expectedSize - kDsuHeaderSize;
	if (type == kMsgKeyboard && payloadSize >= 51)
	{
		std::lock_guard lock(s_mutex);
		for (size_t word = 0; word < s_keyboardWords.size(); ++word)
			s_keyboardWords[word] = ReadU32(payload + 7 + word * 4);
		return true;
	}

	if (type == kMsgAppLaunch && payloadSize >= 12)
	{
		const uint32 revision = ReadU32(payload);
		const uint64 token = ReadU64(payload + 4);
		LaunchStatus status = LaunchStatus::UnknownEntry;
		const CatalogueSnapshot snapshot = BuildCatalogueSnapshot();
		if (revision != snapshot.revision)
		{
			status = LaunchStatus::StaleRevision;
		}
		else if (const CatalogueEntry* entry = FindEntry(snapshot, revision, token))
		{
			std::lock_guard lock(s_mutex);
			if (s_launchRequestToken)
			{
				status = *s_launchRequestToken == token ? LaunchStatus::Accepted : LaunchStatus::Busy;
			}
			else if (s_localLaunchItem || s_pendingLaunchItem ||
				!s_runningTitleName.empty())
				status = LaunchStatus::Busy;
			else
			{
				s_pendingLaunchItem = entry->token;
				s_launchRequestToken = token;
				status = LaunchStatus::Accepted;
			}
		}
		responses.emplace_back(BuildLaunchStatus(status, revision, token));
		Log(LogLevel::Debug, "Companion app: launch request revision={} token={:016x} status={}",
			revision, token, (uint32)status);
		return true;
	}

	if (type == kMsgAppSelection && payloadSize >= 12)
	{
		const uint64 token = ReadU64(payload + 4);
		if (token != 0)
		{
			std::lock_guard lock(s_mutex);
			s_pendingSelectionToken = token;
		}
		return true;
	}

	if (type == kMsgStreamStart || type == kMsgStreamStop ||
		type == kMsgAuxStreamStart || type == kMsgAuxStreamStop)
	{
		const auto command = ParseStreamCommand(type, payload, payloadSize);
		if (command)
		{
			std::lock_guard lock(s_mutex);
			s_streamCommands.emplace_back(*command);
			s_wake.notify_all();
		}
		return true;
	}

	return true;
}

void NotifyStreamChanged(uint8 screenId)
{
	if (screenId > MaxCompanionScreenId)
		return;
	auto active = s_frontendHost ? s_frontendHost->GetActiveScreenStream(screenId) : std::nullopt;
	if (active)
		active = NormalizeActiveStream(std::move(*active), screenId);
	{
		std::lock_guard lock(s_mutex);
		if (active)
			s_reportedStreams[screenId] = *active;
		else
			s_reportedStreams[screenId].reset();
	}
	if (active)
		QueueStreamStatus(1, 0, active->streamType, screenId, active->url,
			static_cast<uint16>(active->width), static_cast<uint16>(active->height),
			active->audioEnabled);
	else
		QueueStreamStatus(0, 0, 0, screenId);
}

void UpdateRunningContent(RunningContent content)
{
	std::string titleName = content.running ? std::move(content.name) : std::string{};
	std::optional<std::vector<uint8>> control;
	const DsuEndpoint endpoint = s_frontendHost ? s_frontendHost->GetDsuEndpoint() : DsuEndpoint{};
	const uint8 supportedScreenMask = SupportedScreenMask(endpoint);
	const uint8 availableScreenMask = AvailableScreenMask(endpoint);
	const uint8 supportedStreamMask = SupportedStreamMask(endpoint);
	const uint8 availableStreamMask = AvailableStreamMask(endpoint);
	{
		std::lock_guard lock(s_mutex);
		if (content.running)
			s_localLaunchItem.reset();
		if (s_runningTitleName == titleName)
			return;
		const bool wasRunning = !s_runningTitleName.empty();
		const bool nowRunning = !titleName.empty();
		s_runningTitleName = titleName;
		if (!wasRunning && nowRunning && supportedStreamMask != 0)
		{
			if (++s_nextAuthorizationToken == 0)
				++s_nextAuthorizationToken;
			s_pendingAuthorizationToken = s_nextAuthorizationToken;
			s_pendingStreamMask = supportedStreamMask;
			s_pendingScreenMask = supportedScreenMask;
			s_authorizationDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
			control = BuildStreamControl(true, s_pendingStreamMask, s_pendingAuthorizationToken);
		}
		else if (wasRunning && !nowRunning)
		{
			s_launchRequestToken.reset();
			if (++s_nextAuthorizationToken == 0)
				++s_nextAuthorizationToken;
			if (availableStreamMask != 0)
				control = BuildStreamControl(false, availableStreamMask, s_nextAuthorizationToken);
			s_pendingAuthorizationToken = 0;
			s_pendingStreamMask = 0;
			s_pendingScreenMask = 0;
			for (uint8 screenId = 0; screenId <= MaxCompanionScreenId; ++screenId)
			{
				if ((availableScreenMask & (1u << screenId)) == 0)
					continue;
				s_streamCommands.push_back(StreamCommand{
					.start = false,
					.padView = screenId != 0,
					.screenId = screenId,
				});
			}
		}
	}
	if (control)
		QueuePacket(std::move(*control));
	s_wake.notify_all();
}

bool TryBeginLocalLaunch(ItemId item)
{
	std::lock_guard lock(s_mutex);
	if (item == 0)
		return false;
	if (s_localLaunchItem)
		return *s_localLaunchItem == item;
	if (s_pendingLaunchItem || s_launchRequestToken ||
		!s_runningTitleName.empty())
	{
		return false;
	}
	s_localLaunchItem = item;
	return true;
}

void CancelLocalLaunch(ItemId item)
{
	std::lock_guard lock(s_mutex);
	if (s_localLaunchItem == item)
		s_localLaunchItem.reset();
}

void UpdateSelection(uint64 token)
{
	if (token == 0)
		return;
	std::lock_guard lock(s_mutex);
	if (s_selectedTitleToken == token)
		return;
	s_selectedTitleToken = token;
	QueueSelectionPacket(BuildAppSelection(s_catalogueRevision.load(std::memory_order_relaxed), token));
}

std::optional<ItemId> TakePendingLaunch()
{
	std::lock_guard lock(s_mutex);
	return std::exchange(s_pendingLaunchItem, std::nullopt);
}

std::optional<ItemId> TakePendingSelection()
{
	std::lock_guard lock(s_mutex);
	return std::exchange(s_pendingSelectionToken, std::nullopt);
}

void CompletePendingLaunch(bool succeeded)
{
	if (succeeded)
		return;

	std::lock_guard lock(s_mutex);
	s_pendingLaunchItem.reset();
	s_launchRequestToken.reset();
}

NavigationState GetNavigationState()
{
	std::lock_guard lock(s_mutex);
	// 273-276 are SDL arrow key codes
	return {
		.up = KeyDownLocked(273),
		.down = KeyDownLocked(274),
		.left = KeyDownLocked(276),
		.right = KeyDownLocked(275),
	};
}

std::string GetCompanionStatusMessage()
{
	std::lock_guard lock(s_mutex);
	return s_status;
}

class CompanionService final : public ICompanionService
{
public:
	~CompanionService() override { Shutdown(); }

	void Initialize(ICompanionHost& host, std::string applicationName, std::string platformName) override
	{
		UwpImGuiFrontend::Initialize(host, std::move(applicationName), std::move(platformName));
	}
	void Shutdown() override { UwpImGuiFrontend::Shutdown(); }
	void SetMetadataLanguage(std::string language) override
	{
		UwpImGuiFrontend::SetMetadataLanguage(std::move(language));
	}
	void AttachTransport(IExtensionPacketTransport* transport) override
	{
		UwpImGuiFrontend::AttachTransport(transport);
	}
	void DetachTransport(IExtensionPacketTransport* transport) override
	{
		UwpImGuiFrontend::DetachTransport(transport);
	}
	void UpdateRunningContent(RunningContent content) override
	{
		UwpImGuiFrontend::UpdateRunningContent(std::move(content));
	}
	void NotifyStreamChanged(std::uint8_t screenId) override
	{
		UwpImGuiFrontend::NotifyStreamChanged(screenId);
	}
	void UpdateSelection(ItemId item) override { UwpImGuiFrontend::UpdateSelection(item); }
	bool TryBeginLocalLaunch(ItemId item) override
	{
		return UwpImGuiFrontend::TryBeginLocalLaunch(item);
	}
	void CancelLocalLaunch(ItemId item) override
	{
		UwpImGuiFrontend::CancelLocalLaunch(item);
	}
	std::optional<ItemId> TakePendingLaunch() override { return UwpImGuiFrontend::TakePendingLaunch(); }
	std::optional<ItemId> TakePendingSelection() override { return UwpImGuiFrontend::TakePendingSelection(); }
	void CompletePendingLaunch(bool succeeded) override
	{
		UwpImGuiFrontend::CompletePendingLaunch(succeeded);
	}
	NavigationState GetNavigationState() const override
	{
		return UwpImGuiFrontend::GetNavigationState();
	}
	CompanionStatus GetStatus() const override
	{
		CompanionStatus status;
		status.running = s_running.load();
		{
			std::lock_guard lock(s_transportMutex);
			status.transportAttached = s_transport != nullptr;
		}
		const std::int64_t lastActivity = s_lastClientActivityMilliseconds.load(std::memory_order_relaxed);
		status.clientConnected = status.transportAttached && lastActivity != 0 &&
			MonotonicMilliseconds() - lastActivity <= 3000;
		status.message = UwpImGuiFrontend::GetCompanionStatusMessage();
		return status;
	}
	bool HandleExtensionPacket(std::span<const std::uint8_t> request,
		std::vector<std::vector<std::uint8_t>>& responses) override
	{
		return UwpImGuiFrontend::HandleExtensionPacket(request, responses);
	}
};

std::unique_ptr<ICompanionService> CreateCompanionService()
{
	return std::make_unique<CompanionService>();
}
}
