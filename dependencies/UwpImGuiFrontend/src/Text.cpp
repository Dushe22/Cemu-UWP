#include "UwpImGuiFrontend/Text.h"

#include <imgui.h>

#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>

namespace UwpImGuiFrontend
{
namespace
{
void AppendUtf8(std::string& output, std::uint32_t codePoint)
{
	if (codePoint <= 0x7f)
		output.push_back(static_cast<char>(codePoint));
	else if (codePoint <= 0x7ff)
	{
		output.push_back(static_cast<char>(0xc0 | (codePoint >> 6)));
		output.push_back(static_cast<char>(0x80 | (codePoint & 0x3f)));
	}
	else if (codePoint <= 0xffff)
	{
		output.push_back(static_cast<char>(0xe0 | (codePoint >> 12)));
		output.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3f)));
		output.push_back(static_cast<char>(0x80 | (codePoint & 0x3f)));
	}
	else
	{
		output.push_back(static_cast<char>(0xf0 | (codePoint >> 18)));
		output.push_back(static_cast<char>(0x80 | ((codePoint >> 12) & 0x3f)));
		output.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3f)));
		output.push_back(static_cast<char>(0x80 | (codePoint & 0x3f)));
	}
}

std::uint32_t Windows1252CodePoint(unsigned char value)
{
	static constexpr std::array<std::uint16_t, 32> extensions{
		0x20ac, 0x0081, 0x201a, 0x0192, 0x201e, 0x2026, 0x2020, 0x2021,
		0x02c6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008d, 0x017d, 0x008f,
		0x0090, 0x2018, 0x2019, 0x201c, 0x201d, 0x2022, 0x2013, 0x2014,
		0x02dc, 0x2122, 0x0161, 0x203a, 0x0153, 0x009d, 0x017e, 0x0178,
	};
	return value >= 0x80 && value <= 0x9f ? extensions[value - 0x80] : value;
}

std::size_t ValidUtf8SequenceLength(std::string_view value, std::size_t offset)
{
	const auto first = static_cast<unsigned char>(value[offset]);
	std::size_t length = 0;
	std::uint32_t minimum = 0;
	std::uint32_t codePoint = 0;
	if (first >= 0xc2 && first <= 0xdf)
	{
		length = 2;
		minimum = 0x80;
		codePoint = first & 0x1f;
	}
	else if (first >= 0xe0 && first <= 0xef)
	{
		length = 3;
		minimum = 0x800;
		codePoint = first & 0x0f;
	}
	else if (first >= 0xf0 && first <= 0xf4)
	{
		length = 4;
		minimum = 0x10000;
		codePoint = first & 0x07;
	}
	else
		return 0;
	if (offset + length > value.size())
		return 0;
	for (std::size_t index = 1; index < length; ++index)
	{
		const auto continuation = static_cast<unsigned char>(value[offset + index]);
		if ((continuation & 0xc0) != 0x80)
			return 0;
		codePoint = (codePoint << 6) | (continuation & 0x3f);
	}
	if (codePoint < minimum || codePoint > 0x10ffff ||
		(codePoint >= 0xd800 && codePoint <= 0xdfff))
		return 0;
	return length;
}

bool DecodeEntity(std::string_view entity, std::uint32_t& codePoint)
{
	if (entity == "amp") codePoint = '&';
	else if (entity == "quot") codePoint = '"';
	else if (entity == "apos") codePoint = '\'';
	else if (entity == "lt") codePoint = '<';
	else if (entity == "gt") codePoint = '>';
	else if (entity == "nbsp") codePoint = 0x00a0;
	else if (entity == "copy") codePoint = 0x00a9;
	else if (entity == "reg") codePoint = 0x00ae;
	else if (entity == "trade") codePoint = 0x2122;
	else if (entity == "ndash") codePoint = 0x2013;
	else if (entity == "mdash") codePoint = 0x2014;
	else if (entity == "lsquo") codePoint = 0x2018;
	else if (entity == "rsquo") codePoint = 0x2019;
	else if (entity == "ldquo") codePoint = 0x201c;
	else if (entity == "rdquo") codePoint = 0x201d;
	else if (entity == "bull") codePoint = 0x2022;
	else if (entity == "hellip") codePoint = 0x2026;
	else if (entity == "euro") codePoint = 0x20ac;
	else if (entity == "pound") codePoint = 0x00a3;
	else if (entity == "yen") codePoint = 0x00a5;
	else if (entity == "deg") codePoint = 0x00b0;
	else if (entity.size() > 1 && entity.front() == '#')
	{
		const bool hexadecimal = entity.size() > 2 &&
			(entity[1] == 'x' || entity[1] == 'X');
		const std::string_view digits = entity.substr(hexadecimal ? 2 : 1);
		const auto result = std::from_chars(digits.data(), digits.data() + digits.size(),
			codePoint, hexadecimal ? 16 : 10);
		return !digits.empty() && result.ec == std::errc{} &&
			result.ptr == digits.data() + digits.size();
	}
	else
		return false;
	return true;
}

TextTranslator s_translator = nullptr;
}

void SetTextTranslator(TextTranslator translator)
{
	s_translator = translator;
}

const char* Translate(const char* text)
{
	if (!s_translator || !text)
		return text;
	const char* translated = s_translator(text);
	return translated ? translated : text;
}

std::string NormalizeDisplayText(std::string_view value)
{
	std::string output;
	output.reserve(value.size());
	for (std::size_t offset = 0; offset < value.size();)
	{
		const auto byte = static_cast<unsigned char>(value[offset]);
		if (byte == '&')
		{
			const std::size_t terminator = value.find(';', offset + 1);
			if (terminator != std::string_view::npos && terminator - offset <= 12)
			{
				std::uint32_t codePoint = 0;
				if (DecodeEntity(value.substr(offset + 1, terminator - offset - 1), codePoint) &&
					codePoint != 0 && codePoint <= 0x10ffff &&
					!(codePoint >= 0xd800 && codePoint <= 0xdfff))
				{
					AppendUtf8(output, codePoint);
					offset = terminator + 1;
					continue;
				}
			}
		}
		if (byte < 0x80)
		{
			output.push_back(static_cast<char>(byte));
			++offset;
			continue;
		}
		if (const std::size_t length = ValidUtf8SequenceLength(value, offset))
		{
			output.append(value.substr(offset, length));
			offset += length;
			continue;
		}
		AppendUtf8(output, Windows1252CodePoint(byte));
		++offset;
	}
	return output;
}

std::string NormalizeMetadataReleaseDate(std::string_view value)
{
	while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
		value.remove_prefix(1);
	while (!value.empty() && (value.back() == ' ' || value.back() == '\t'))
		value.remove_suffix(1);
	if (value.empty())
		return {};

	int year = 0;
	unsigned month = 0;
	unsigned day = 0;
	const auto digits = [](std::string_view text) {
		for (const char character : text)
		{
			if (character < '0' || character > '9')
				return false;
		}
		return !text.empty();
	};
	const auto number = [](std::string_view text, int& result) {
		const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
		return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
	};

	if (value.size() >= 10 && digits(value.substr(0, 4)) &&
		(value[4] == '-' || value[4] == '/') &&
		(value[7] == '-' || value[7] == '/') && digits(value.substr(5, 2)) &&
		digits(value.substr(8, 2)))
	{
		int parsedMonth = 0;
		int parsedDay = 0;
		if (!number(value.substr(0, 4), year) ||
			!number(value.substr(5, 2), parsedMonth) ||
			!number(value.substr(8, 2), parsedDay))
			return std::string(value);
		month = static_cast<unsigned>(parsedMonth);
		day = static_cast<unsigned>(parsedDay);
	}
	else if (value.size() >= 8 && digits(value.substr(0, 8)))
	{
		int parsedMonth = 0;
		int parsedDay = 0;
		if (!number(value.substr(0, 4), year) ||
			!number(value.substr(4, 2), parsedMonth) ||
			!number(value.substr(6, 2), parsedDay))
			return std::string(value);
		month = static_cast<unsigned>(parsedMonth);
		day = static_cast<unsigned>(parsedDay);
	}
	else if (value.size() == 10 && digits(value.substr(0, 2)) &&
		(value[2] == '/' || value[2] == '-') && digits(value.substr(3, 2)) &&
		(value[5] == '/' || value[5] == '-') && digits(value.substr(6, 4)))
	{
		int parsedDay = 0;
		int parsedMonth = 0;
		if (!number(value.substr(0, 2), parsedDay) ||
			!number(value.substr(3, 2), parsedMonth) ||
			!number(value.substr(6, 4), year))
			return std::string(value);
		month = static_cast<unsigned>(parsedMonth);
		day = static_cast<unsigned>(parsedDay);
	}
	else
		return std::string(value);

	const std::chrono::year_month_day date{ std::chrono::year(year),
		std::chrono::month(month), std::chrono::day(day) };
	if (!date.ok())
		return std::string(value);
	char result[11]{};
	std::snprintf(result, sizeof(result), "%04d-%02u-%02u", year, month, day);
	return result;
}

std::string FormatMetadataReleaseDateUk(std::string_view value)
{
	const std::string normalized = NormalizeMetadataReleaseDate(value);
	if (normalized.size() == 10 && normalized[4] == '-' && normalized[7] == '-')
		return normalized.substr(8, 2) + "/" + normalized.substr(5, 2) + "/" +
			normalized.substr(0, 4);
	return normalized;
}

bool FontContainsGlyph(ImFont* font, ImWchar glyph)
{
	if (!font)
		return false;
#if IMGUI_VERSION_NUM >= 19200
	return font->IsGlyphInFont(glyph);
#else
	return font->FindGlyphNoFallback(glyph) != nullptr;
#endif
}

float FontReferenceSize(ImFont* font)
{
	if (!font)
		return 0.0f;
#if IMGUI_VERSION_NUM >= 19200
	return font->LegacySize;
#else
	return font->FontSize;
#endif
}

ImFont* ResolveFrontendTextFont(ImFont* preferred)
{
	static constexpr std::array<ImWchar, 9> representativeGlyphs{
		0x00a9, 0x00ae, 0x2013, 0x2019, 0x2022, 0x2026, 0x20ac, 0x2122, 0,
	};
	const auto score = [](ImFont* font) {
		int result = 0;
		if (!font)
			return result;
		for (const ImWchar glyph : representativeGlyphs)
		{
			if (glyph != 0 && FontContainsGlyph(font, glyph))
				++result;
		}
		return result;
	};

	ImFont* best = preferred;
	int bestScore = score(preferred);
	float bestSizeDistance = 0.0f;
	if (!ImGui::GetCurrentContext() || !ImGui::GetIO().Fonts)
		return best;
	for (ImFont* candidate : ImGui::GetIO().Fonts->Fonts)
	{
		const float sizeDistance = preferred ?
			std::abs(FontReferenceSize(candidate) -
				FontReferenceSize(preferred)) : 0.0f;
		if (preferred && sizeDistance > 1.0f)
			continue;
		const int candidateScore = score(candidate);
		if (candidateScore > bestScore ||
			(candidateScore == bestScore && sizeDistance < bestSizeDistance))
		{
			best = candidate;
			bestScore = candidateScore;
			bestSizeDistance = sizeDistance;
		}
	}
	return best;
}
}
