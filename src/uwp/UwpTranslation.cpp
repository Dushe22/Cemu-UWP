#include "UwpTranslation.h"

#include "Cemu/Logging/CemuLogging.h"
#include "Common/FileStream.h"
#include "config/ActiveSettings.h"
#include "imgui/imgui_extension.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <unordered_map>

namespace fs = std::filesystem;

namespace
{
constexpr uint32 kMoMagic = 0x950412de;
constexpr uint32 kMoMagicSwapped = 0xde120495;

std::mutex s_mutex;
std::unordered_map<std::string, std::string> s_catalog;
std::string s_language;
uint32_t s_generation = 0;
std::vector<CemuUwp::Translation::Language> s_languages;

uint32 ReadU32(const std::vector<uint8>& data, size_t offset, bool swap)
{
	if (offset + sizeof(uint32) > data.size())
		return 0;
	uint32 value;
	std::memcpy(&value, data.data() + offset, sizeof(value));
	return swap ? _swapEndianU32(value) : value;
}

// gettext binary catalog: header, then parallel original and translation tables
bool ParseCatalog(const std::vector<uint8>& data,
	std::unordered_map<std::string, std::string>& catalog)
{
	if (data.size() < 28)
		return false;
	uint32 magic;
	std::memcpy(&magic, data.data(), sizeof(magic));
	if (magic != kMoMagic && magic != kMoMagicSwapped)
		return false;
	const bool swap = magic == kMoMagicSwapped;

	const uint32 count = ReadU32(data, 8, swap);
	const uint32 originalTable = ReadU32(data, 12, swap);
	const uint32 translationTable = ReadU32(data, 16, swap);
	for (uint32 i = 0; i < count; ++i)
	{
		const uint32 originalLength = ReadU32(data, originalTable + i * 8, swap);
		const uint32 originalOffset = ReadU32(data, originalTable + i * 8 + 4, swap);
		const uint32 translationLength = ReadU32(data, translationTable + i * 8, swap);
		const uint32 translationOffset = ReadU32(data, translationTable + i * 8 + 4, swap);
		if ((uint64)originalOffset + originalLength > data.size() ||
			(uint64)translationOffset + translationLength > data.size())
			return false;
		if (originalLength == 0 || translationLength == 0)
			continue;
		catalog.emplace(
			std::string(reinterpret_cast<const char*>(data.data() + originalOffset), originalLength),
			std::string(reinterpret_cast<const char*>(data.data() + translationOffset), translationLength));
	}
	return true;
}

// tab separated "source<TAB>translation" lines for strings that only exist here
void ParseOverlay(const std::string& text,
	std::unordered_map<std::string, std::string>& catalog)
{
	size_t start = 0;
	while (start < text.size())
	{
		size_t end = text.find(0x0A, start);
		if (end == std::string::npos)
			end = text.size();
		std::string_view line(text.data() + start, end - start);
		start = end + 1;
		if (!line.empty() && line.back() == 0x0D)
			line.remove_suffix(1);
		if (line.empty() || line.front() == '#')
			continue;
		const size_t separator = line.find(0x09);
		if (separator == std::string_view::npos || separator == 0)
			continue;
		std::string source(line.substr(0, separator));
		std::string translation(line.substr(separator + 1));
		if (!translation.empty())
			catalog[std::move(source)] = std::move(translation);
	}
}

// no packaged font provides glyphs for these scripts
constexpr std::array kUnrenderableLanguages{ "ar", "he" };

// names use the script the interface font always carries so the list stays readable
const char* LanguageName(std::string_view code)
{
	if (code == "ca") return "Català";
	if (code == "de") return "Deutsch";
	if (code == "es") return "Español";
	if (code == "fr") return "Français";
	if (code == "hu") return "Magyar";
	if (code == "it") return "Italiano";
	if (code == "ja") return "Japanese";
	if (code == "ko") return "Korean";
	if (code == "nb") return "Norsk";
	if (code == "nl") return "Nederlands";
	if (code == "pl") return "Polski";
	if (code == "pt") return "Português";
	if (code == "ru") return "Русский";
	if (code == "sv") return "Svenska";
	if (code == "tr") return "Türkçe";
	if (code == "uk") return "Українська";
	if (code == "zh") return "Chinese";
	return nullptr;
}

fs::path ScriptFontPath(std::string_view code)
{
	std::string_view font;
	if (code == "ja")
		font = "CafeStd.ttf";
	else if (code == "ko")
		font = "CafeKr.ttf";
	else if (code == "zh")
		font = "CafeCn.ttf";
	if (font.empty())
		return {};
	return ActiveSettings::GetDataPath("resources/sharedFonts/{}", font);
}

void AppendCodepoints(std::string_view text, std::vector<uint32>& codepoints)
{
	for (size_t index = 0; index < text.size();)
	{
		const uint8 lead = (uint8)text[index];
		uint32 codepoint = lead;
		size_t length = 1;
		if (lead >= 0xF0)
		{
			codepoint = lead & 0x07;
			length = 4;
		}
		else if (lead >= 0xE0)
		{
			codepoint = lead & 0x0F;
			length = 3;
		}
		else if (lead >= 0xC0)
		{
			codepoint = lead & 0x1F;
			length = 2;
		}
		if (index + length > text.size())
			break;
		for (size_t part = 1; part < length; ++part)
			codepoint = (codepoint << 6) | ((uint8)text[index + part] & 0x3F);
		index += length;
		codepoints.push_back(codepoint);
	}
}
}

namespace CemuUwp::Translation
{
const std::vector<Language>& GetAvailableLanguages()
{
	std::scoped_lock lock(s_mutex);
	if (!s_languages.empty())
		return s_languages;

	s_languages.push_back({ "", "English" });
	std::error_code ec;
	const fs::path root = ActiveSettings::GetDataPath("resources");
	for (const auto& entry : fs::directory_iterator(root, ec))
	{
		if (!entry.is_directory(ec))
			continue;
		const std::string code = _pathToUtf8(entry.path().filename());
		if (!fs::exists(entry.path() / "cemu.mo", ec))
			continue;
		if (std::find(kUnrenderableLanguages.begin(), kUnrenderableLanguages.end(), code) !=
			kUnrenderableLanguages.end())
			continue;
		const char* name = LanguageName(code);
		s_languages.push_back({ code, name ? name : code });
	}
	std::sort(s_languages.begin() + 1, s_languages.end(),
		[](const Language& a, const Language& b) { return a.code < b.code; });
	return s_languages;
}

bool SetLanguage(const std::string& code)
{
	std::unordered_map<std::string, std::string> catalog;
	if (!code.empty())
	{
		const auto path = ActiveSettings::GetDataPath("resources/{}/cemu.mo", code);
		std::unique_ptr<FileStream> file(FileStream::openFile2(path));
		if (!file)
		{
			cemuLog_log(LogType::Force, "UWP interface language {} has no catalog", code);
			return false;
		}
		std::vector<uint8> data;
		file->extract(data);
		if (!ParseCatalog(data, catalog))
		{
			cemuLog_log(LogType::Force, "UWP interface language {} catalog is invalid", code);
			return false;
		}
		const auto overlayPath = ActiveSettings::GetDataPath("resources/{}/cemu_uwp.txt", code);
		if (std::unique_ptr<FileStream> overlay{ FileStream::openFile2(overlayPath) })
		{
			std::vector<uint8> overlayData;
			overlay->extract(overlayData);
			ParseOverlay(std::string(reinterpret_cast<const char*>(overlayData.data()),
				overlayData.size()), catalog);
		}
	}

	std::vector<uint32> codepoints;
	for (const auto& [source, translation] : catalog)
		AppendCodepoints(translation, codepoints);
	ImGui_SetTextGlyphs(codepoints, ScriptFontPath(code));

	{
		std::scoped_lock lock(s_mutex);
		s_catalog = std::move(catalog);
		s_language = code;
		++s_generation;
	}
	SetTranslationCallback([](std::string_view text) { return Translate(text); });
	return true;
}

uint32_t GetGeneration()
{
	std::scoped_lock lock(s_mutex);
	return s_generation;
}

std::string GetLanguage()
{
	std::scoped_lock lock(s_mutex);
	return s_language;
}

std::string Translate(std::string_view text)
{
	std::scoped_lock lock(s_mutex);
	if (s_catalog.empty())
		return std::string(text);
	const auto match = s_catalog.find(std::string(text));
	return match == s_catalog.end() ? std::string(text) : match->second;
}
}
