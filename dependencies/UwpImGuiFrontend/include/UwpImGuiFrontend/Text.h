#pragma once

#include <string>
#include <string_view>

struct ImFont;

namespace UwpImGuiFrontend
{
// returns a pointer that stays valid until the host changes language
using TextTranslator = const char* (*)(const char*);

void SetTextTranslator(TextTranslator translator);
[[nodiscard]] const char* Translate(const char* text);

// decodes character references and repairs Windows-1252 bytes
[[nodiscard]] std::string NormalizeDisplayText(std::string_view value);

// normalizes ISO, YYYYMMDD and unambiguous day-first dates to YYYY-MM-DD
[[nodiscard]] std::string NormalizeMetadataReleaseDate(std::string_view value);

[[nodiscard]] std::string FormatMetadataReleaseDateUk(std::string_view value);

[[nodiscard]] ImFont* ResolveFrontendTextFont(ImFont* preferred = nullptr);
}
