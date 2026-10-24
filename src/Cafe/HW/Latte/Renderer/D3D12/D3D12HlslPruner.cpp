#include "Cafe/HW/Latte/Renderer/D3D12/D3D12HlslPruner.h"

#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace D3D12HlslPruner
{
namespace
{
bool IsIdentStart(char c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

bool IsIdentChar(char c)
{
	return IsIdentStart(c) || (c >= '0' && c <= '9');
}

bool IsSpace(char c)
{
	return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

// marks characters that belong to comments, string literals or preprocessor lines
std::vector<bool> BuildNonCodeMask(std::string_view src)
{
	std::vector<bool> nonCode(src.size(), false);
	bool lineStart = true;
	for (size_t i = 0; i < src.size();)
	{
		const char c = src[i];
		if (c == '/' && i + 1 < src.size() && src[i + 1] == '/')
		{
			while (i < src.size() && src[i] != '\n')
				nonCode[i++] = true;
			continue;
		}
		if (c == '/' && i + 1 < src.size() && src[i + 1] == '*')
		{
			nonCode[i] = nonCode[i + 1] = true;
			i += 2;
			while (i < src.size() && !(src[i] == '*' && i + 1 < src.size() && src[i + 1] == '/'))
				nonCode[i++] = true;
			for (int k = 0; k < 2 && i < src.size(); k++)
				nonCode[i++] = true;
			continue;
		}
		if (c == '"')
		{
			nonCode[i++] = true;
			while (i < src.size() && src[i] != '"' && src[i] != '\n')
			{
				if (src[i] == '\\' && i + 1 < src.size())
					nonCode[i++] = true;
				nonCode[i++] = true;
			}
			if (i < src.size())
				nonCode[i++] = true;
			continue;
		}
		if (lineStart && c == '#')
		{
			// a directive runs to the end of the line, longer with backslash continuations
			while (i < src.size() && src[i] != '\n')
			{
				if (src[i] == '\\' && i + 1 < src.size() && (src[i + 1] == '\n' || src[i + 1] == '\r'))
				{
					nonCode[i++] = true;
					while (i < src.size() && (src[i] == '\r' || src[i] == '\n'))
						nonCode[i++] = true;
					continue;
				}
				nonCode[i++] = true;
			}
			continue;
		}
		if (c == '\n')
			lineStart = true;
		else if (!IsSpace(c))
			lineStart = false;
		i++;
	}
	return nonCode;
}

// Accepts "[qualifiers] type name(params) [: semantic]" and returns the position of name. Anything
// else (declarations with register(), attributes, initialisers, templates, struct/cbuffer) is rejected.
bool ParseFunctionHeader(std::string_view header, size_t& nameOffset, size_t& nameLength)
{
	size_t end = header.size();
	while (end > 0 && IsSpace(header[end - 1]))
		end--;
	// optional return semantic
	if (end > 0 && IsIdentChar(header[end - 1]))
	{
		size_t identBegin = end;
		while (identBegin > 0 && IsIdentChar(header[identBegin - 1]))
			identBegin--;
		size_t k = identBegin;
		while (k > 0 && IsSpace(header[k - 1]))
			k--;
		if (k == 0 || header[k - 1] != ':')
			return false;
		end = k - 1;
		while (end > 0 && IsSpace(header[end - 1]))
			end--;
	}
	if (end == 0 || header[end - 1] != ')')
		return false;
	// matching parenthesis
	int parenDepth = 0;
	size_t open = end;
	while (open > 0)
	{
		const char c = header[--open];
		if (c == ')')
			parenDepth++;
		else if (c == '(' && --parenDepth == 0)
			break;
		else if (c == '{' || c == '}' || c == ';')
			return false;
	}
	if (parenDepth != 0)
		return false;
	size_t nameEnd = open;
	while (nameEnd > 0 && IsSpace(header[nameEnd - 1]))
		nameEnd--;
	size_t nameBegin = nameEnd;
	while (nameBegin > 0 && IsIdentChar(header[nameBegin - 1]))
		nameBegin--;
	if (nameBegin == nameEnd || !IsIdentStart(header[nameBegin]))
		return false;
	// the return type and qualifiers: identifiers, template arguments and nothing else
	if (nameBegin == 0 || !(IsSpace(header[nameBegin - 1]) || header[nameBegin - 1] == '>'))
		return false;
	const std::string_view prefix = header.substr(0, nameBegin);
	size_t firstToken = 0;
	while (firstToken < prefix.size() && IsSpace(prefix[firstToken]))
		firstToken++;
	if (firstToken == prefix.size())
		return false;
	for (size_t k = firstToken; k < prefix.size(); k++)
	{
		const char c = prefix[k];
		if (!(IsIdentChar(c) || IsSpace(c) || c == '<' || c == '>' || c == ','))
			return false;
	}
	size_t firstWordEnd = firstToken;
	while (firstWordEnd < prefix.size() && IsIdentChar(prefix[firstWordEnd]))
		firstWordEnd++;
	const std::string_view firstWord = prefix.substr(firstToken, firstWordEnd - firstToken);
	if (firstWord == "struct" || firstWord == "cbuffer" || firstWord == "tbuffer" || firstWord == "typedef" ||
		firstWord == "template" || firstWord == "namespace" || firstWord == "return")
		return false;
	nameOffset = nameBegin;
	nameLength = nameEnd - nameBegin;
	return true;
}

struct FunctionSpan
{
	std::string_view name;
	size_t begin;
	size_t end;
};

// also records the identifiers that are followed by '(' (calls or function-like macro invocations)
void CollectIdentifiers(std::string_view text, std::vector<std::string_view>& out, std::unordered_set<std::string_view>& called)
{
	for (size_t i = 0; i < text.size();)
	{
		if (IsIdentStart(text[i]) && (i == 0 || !IsIdentChar(text[i - 1])))
		{
			size_t j = i + 1;
			while (j < text.size() && IsIdentChar(text[j]))
				j++;
			const std::string_view name = text.substr(i, j - i);
			out.emplace_back(name);
			size_t k = j;
			while (k < text.size() && IsSpace(text[k]))
				k++;
			if (k < text.size() && text[k] == '(')
				called.insert(name);
			i = j;
		}
		else
			i++;
	}
}

struct MacroDefinition
{
	std::string_view body;
	bool functionLike;
	bool expanded;
};
}

bool PruneUnusedFunctions(std::string_view src, std::string_view entryPoint, std::string& output)
{
	output.clear();
	const std::vector<bool> nonCode = BuildNonCodeMask(src);

	std::vector<FunctionSpan> functions;
	size_t statementStart = 0;
	int depth = 0;
	size_t i = 0;
	std::string header;
	while (i < src.size())
	{
		if (nonCode[i])
		{
			i++;
			continue;
		}
		const char c = src[i];
		if (depth == 0 && c == ';')
		{
			statementStart = i + 1;
		}
		else if (c == '{')
		{
			if (depth == 0)
			{
				header.clear();
				for (size_t k = statementStart; k < i; k++)
					header.push_back(nonCode[k] ? ' ' : src[k]);
				size_t nameInHeader = 0, nameLength = 0;
				if (ParseFunctionHeader(header, nameInHeader, nameLength))
				{
					// find the matching brace
					int bodyDepth = 0;
					size_t k = i;
					for (; k < src.size(); k++)
					{
						if (nonCode[k])
							continue;
						if (src[k] == '{')
							bodyDepth++;
						else if (src[k] == '}' && --bodyDepth == 0)
							break;
					}
					if (k >= src.size())
						return false; // unbalanced, leave the source alone
					// start at the first code character: preprocessor lines and comments in front of the
					// definition (an #if guarding it, say) must stay even when the function goes
					size_t begin = statementStart;
					while (begin < i && (nonCode[begin] || IsSpace(src[begin])))
						begin++;
					functions.push_back({ src.substr(statementStart + nameInHeader, nameLength), begin, k + 1 });
					i = k + 1;
					statementStart = i;
					continue;
				}
			}
			depth++;
		}
		else if (c == '}')
		{
			if (--depth < 0)
				return false;
			if (depth == 0)
				statementStart = i + 1;
		}
		i++;
	}
	if (depth != 0 || functions.empty())
		return false;

	// Everything outside function definitions is a root, as is the entry point, except directives: a
	// #define body only references what it names once the macro is used (a function-like macro only
	// when its name is followed by '('), and #if/#undef/... lines reference nothing.
	std::vector<std::string_view> pending;
	std::unordered_set<std::string_view> called;
	std::vector<MacroDefinition> macros;
	std::unordered_multimap<std::string_view, size_t> macrosByName;
	auto collectRoots = [&](std::string_view text)
	{
		size_t lineBegin = 0;
		while (lineBegin < text.size())
		{
			// logical line, including backslash continuations
			size_t lineEnd = lineBegin;
			while (lineEnd < text.size() && text[lineEnd] != '\n')
			{
				if (text[lineEnd] == '\\' && lineEnd + 1 < text.size() && (text[lineEnd + 1] == '\n' || text[lineEnd + 1] == '\r'))
				{
					lineEnd++;
					while (lineEnd < text.size() && (text[lineEnd] == '\r' || text[lineEnd] == '\n'))
						lineEnd++;
					continue;
				}
				lineEnd++;
			}
			const std::string_view line = text.substr(lineBegin, lineEnd - lineBegin);
			size_t k = 0;
			while (k < line.size() && (line[k] == ' ' || line[k] == '\t'))
				k++;
			if (k < line.size() && line[k] == '#')
			{
				k++;
				while (k < line.size() && (line[k] == ' ' || line[k] == '\t'))
					k++;
				if (line.substr(k, 6) == "define" && k + 6 < line.size() && !IsIdentChar(line[k + 6]))
				{
					k += 6;
					while (k < line.size() && (line[k] == ' ' || line[k] == '\t'))
						k++;
					size_t nameEnd = k;
					while (nameEnd < line.size() && IsIdentChar(line[nameEnd]))
						nameEnd++;
					if (nameEnd > k)
					{
						const bool functionLike = nameEnd < line.size() && line[nameEnd] == '(';
						macrosByName.emplace(line.substr(k, nameEnd - k), macros.size());
						macros.push_back({ line.substr(nameEnd), functionLike, false });
					}
				}
				lineBegin = lineEnd + 1;
				continue;
			}
			CollectIdentifiers(line, pending, called);
			lineBegin = lineEnd + 1;
		}
	};
	size_t cursor = 0;
	for (const auto& function : functions)
	{
		collectRoots(src.substr(cursor, function.begin - cursor));
		cursor = function.end;
	}
	collectRoots(src.substr(cursor));
	pending.emplace_back(entryPoint);
	called.insert(entryPoint);

	std::unordered_multimap<std::string_view, size_t> functionsByName;
	for (size_t f = 0; f < functions.size(); f++)
		functionsByName.emplace(functions[f].name, f);

	// a referenced name keeps every overload, so overload resolution never changes
	std::unordered_set<std::string_view> reachable;
	while (!pending.empty())
	{
		const std::string_view name = pending.back();
		pending.pop_back();
		// a used macro references whatever its definitions name; the call check is repeated each
		// time the name comes up, since the invocation may only be collected later
		auto [macroFirst, macroLast] = macrosByName.equal_range(name);
		for (auto it = macroFirst; it != macroLast; ++it)
		{
			MacroDefinition& macro = macros[it->second];
			if (macro.expanded || (macro.functionLike && !called.contains(name)))
				continue;
			macro.expanded = true;
			CollectIdentifiers(macro.body, pending, called);
		}
		if (!functionsByName.contains(name) || !reachable.insert(name).second)
			continue;
		auto [first, last] = functionsByName.equal_range(name);
		for (auto it = first; it != last; ++it)
		{
			const auto& function = functions[it->second];
			CollectIdentifiers(src.substr(function.begin, function.end - function.begin), pending, called);
		}
	}

	bool removedAny = false;
	output.reserve(src.size());
	cursor = 0;
	for (const auto& function : functions)
	{
		if (reachable.contains(function.name))
			continue;
		output.append(src.substr(cursor, function.begin - cursor));
		// keep the line count so DXC diagnostics still point at the emitted source
		for (size_t k = function.begin; k < function.end; k++)
		{
			if (src[k] == '\n')
				output.push_back('\n');
		}
		cursor = function.end;
		removedAny = true;
	}
	if (!removedAny)
	{
		output.clear();
		return false;
	}
	output.append(src.substr(cursor));
	return true;
}
}
