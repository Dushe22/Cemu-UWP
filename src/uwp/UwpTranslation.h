#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace CemuUwp::Translation
{
	struct Language
	{
		std::string code;
		std::string name;
	};

	// languages with a catalog in the packaged resources, "" first for English
	const std::vector<Language>& GetAvailableLanguages();
	// loads the catalog and installs the translation callback; "" restores English
	bool SetLanguage(const std::string& code);
	std::string GetLanguage();
	std::string Translate(std::string_view text);
	// bumped on every language change so cached lookups can be dropped
	uint32_t GetGeneration();
}
