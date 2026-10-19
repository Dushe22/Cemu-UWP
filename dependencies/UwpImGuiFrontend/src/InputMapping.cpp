#include "UwpImGuiFrontend/InputMapping.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <memory>
#include <stdexcept>
#include <utility>

namespace UwpImGuiFrontend
{
std::string EscapeInputControlReference(std::string_view reference)
{
	std::string result;
	result.reserve(reference.size() + 2);
	result.push_back('`');
	for (const char character : reference)
	{
		if (character == '`' || character == '\\')
			result.push_back('\\');
		result.push_back(character);
	}
	result.push_back('`');
	return result;
}

std::optional<std::string> RewriteInputControlDeviceIds(
	std::string_view expression, std::span<const InputDeviceIdAlias> aliases)
{
	std::string result;
	result.reserve(expression.size());
	for (std::size_t position = 0; position < expression.size();)
	{
		if (expression[position] != '`')
		{
			result.push_back(expression[position++]);
			continue;
		}

		++position;
		std::string reference;
		bool closed = false;
		while (position < expression.size())
		{
			const char character = expression[position++];
			if (character == '`')
			{
				closed = true;
				break;
			}
			if (character == '\\')
			{
				if (position == expression.size())
					return std::nullopt;
				reference.push_back(expression[position++]);
			}
			else
				reference.push_back(character);
		}
		if (!closed)
			return std::nullopt;

		for (const InputDeviceIdAlias& alias : aliases)
		{
			if (alias.previousId.empty() || alias.previousId == alias.currentId)
				continue;
			if (reference == alias.previousId)
			{
				reference = alias.currentId;
				break;
			}
			if (reference.size() > alias.previousId.size() &&
				reference.starts_with(alias.previousId) &&
				reference[alias.previousId.size()] == '/')
			{
				reference.replace(0, alias.previousId.size(), alias.currentId);
				break;
			}
		}
		result += EscapeInputControlReference(reference);
	}
	return result;
}

namespace
{
enum class TokenKind
{
	End, Number, Identifier, Control, LeftParen, RightParen, Comma, Question,
	Colon, Plus, Minus, Star, Slash, Percent, Bang, Tilde, Ampersand, Pipe,
	Caret, Less, LessEqual, Greater, GreaterEqual, EqualEqual, NotEqual,
	LogicalAnd, LogicalOr, Assign, PlusAssign, MinusAssign, StarAssign,
	SlashAssign,
};

struct Token
{
	TokenKind kind = TokenKind::End;
	std::string text;
	double number = 0.0;
};

class ParseFailure final : public std::runtime_error
{
public:
	using std::runtime_error::runtime_error;
};

class Lexer
{
public:
	Lexer(std::string_view source, const ExpressionLimits& limits)
		: m_source(source), m_limits(limits)
	{
		if (source.size() > limits.maximumLength)
			throw ParseFailure("expression is too long");
	}

	std::vector<Token> Run()
	{
		std::vector<Token> result;
		while (m_position < m_source.size())
		{
			if (std::isspace(static_cast<unsigned char>(m_source[m_position])))
			{
				++m_position;
				continue;
			}
			if (result.size() >= m_limits.maximumTokens)
				throw ParseFailure("expression has too many tokens");

			const char value = m_source[m_position];
			if (std::isdigit(static_cast<unsigned char>(value)) ||
				(value == '.' && m_position + 1 < m_source.size() &&
				 std::isdigit(static_cast<unsigned char>(m_source[m_position + 1]))))
			{
				result.emplace_back(ReadNumber());
				continue;
			}
			if (std::isalpha(static_cast<unsigned char>(value)) || value == '_' || value == '$')
			{
				result.emplace_back(ReadIdentifier());
				continue;
			}
			if (value == '`')
			{
				result.emplace_back(ReadControl());
				continue;
			}

			result.emplace_back(ReadOperator());
		}
		result.push_back({ TokenKind::End, {} });
		return result;
	}

private:
	Token ReadNumber()
	{
		const std::size_t start = m_position;
		while (m_position < m_source.size() &&
			(std::isdigit(static_cast<unsigned char>(m_source[m_position])) ||
			 m_source[m_position] == '.' || m_source[m_position] == 'e' ||
			 m_source[m_position] == 'E' ||
			 ((m_source[m_position] == '+' || m_source[m_position] == '-') &&
			  m_position > start && (m_source[m_position - 1] == 'e' || m_source[m_position - 1] == 'E'))))
			++m_position;
		const std::string text(m_source.substr(start, m_position - start));
		char* end = nullptr;
		const double number = std::strtod(text.c_str(), &end);
		if (!end || *end != '\0' || !std::isfinite(number))
			throw ParseFailure("invalid number");
		return { TokenKind::Number, text, number };
	}

	Token ReadIdentifier()
	{
		const std::size_t start = m_position++;
		while (m_position < m_source.size())
		{
			const unsigned char value = static_cast<unsigned char>(m_source[m_position]);
			if (!std::isalnum(value) && value != '_' && value != '.' && value != '$')
				break;
			++m_position;
		}
		return { TokenKind::Identifier,
			std::string(m_source.substr(start, m_position - start)) };
	}

	Token ReadControl()
	{
		++m_position;
		std::string value;
		while (m_position < m_source.size())
		{
			const char current = m_source[m_position++];
			if (current == '`')
				return { TokenKind::Control, std::move(value) };
			if (current == '\\' && m_position < m_source.size())
				value.push_back(m_source[m_position++]);
			else
				value.push_back(current);
		}
		throw ParseFailure("unterminated control reference");
	}

	Token ReadOperator()
	{
		const char value = m_source[m_position++];
		auto match = [this](char next) {
			if (m_position < m_source.size() && m_source[m_position] == next)
			{
				++m_position;
				return true;
			}
			return false;
		};
		switch (value)
		{
		case '(': return { TokenKind::LeftParen, "(" };
		case ')': return { TokenKind::RightParen, ")" };
		case ',': return { TokenKind::Comma, "," };
		case '?': return { TokenKind::Question, "?" };
		case ':': return { TokenKind::Colon, ":" };
		case '+': return { match('=') ? TokenKind::PlusAssign : TokenKind::Plus, "+" };
		case '-': return { match('=') ? TokenKind::MinusAssign : TokenKind::Minus, "-" };
		case '*': return { match('=') ? TokenKind::StarAssign : TokenKind::Star, "*" };
		case '/': return { match('=') ? TokenKind::SlashAssign : TokenKind::Slash, "/" };
		case '%': return { TokenKind::Percent, "%" };
		case '~': return { TokenKind::Tilde, "~" };
		case '!': return { match('=') ? TokenKind::NotEqual : TokenKind::Bang, "!" };
		case '&': return { match('&') ? TokenKind::LogicalAnd : TokenKind::Ampersand, "&" };
		case '|': return { match('|') ? TokenKind::LogicalOr : TokenKind::Pipe, "|" };
		case '^': return { TokenKind::Caret, "^" };
		case '<': return { match('=') ? TokenKind::LessEqual : TokenKind::Less, "<" };
		case '>': return { match('=') ? TokenKind::GreaterEqual : TokenKind::Greater, ">" };
		case '=':
			return { match('=') ? TokenKind::EqualEqual : TokenKind::Assign, "=" };
		default: throw ParseFailure("unexpected character in expression");
		}
	}

	std::string_view m_source;
	const ExpressionLimits& m_limits;
	std::size_t m_position = 0;
};

enum class NodeKind { Number, Reference, Unary, Binary, Conditional, Call, Assignment };

struct Node
{
	NodeKind kind = NodeKind::Number;
	TokenKind operation = TokenKind::End;
	double number = 0.0;
	std::string text;
	std::vector<std::unique_ptr<Node>> children;
	std::size_t stateId = std::numeric_limits<std::size_t>::max();
};

bool IsStatefulFunction(std::string_view name)
{
	return name == "timer" || name == "toggle" || name == "smooth" ||
		name == "hold" || name == "tap" || name == "relative" || name == "pulse";
}

bool IsValidFunction(std::string_view name, std::size_t argumentCount)
{
	if (name == "if") return argumentCount == 3;
	if (name == "not" || name == "abs" || name == "sin" || name == "cos" ||
		name == "tan" || name == "asin" || name == "acos" || name == "atan" ||
		name == "sqrt" || name == "timer" || name == "toggle")
		return argumentCount == 1;
	if (name == "atan2" || name == "pow" || name == "min" || name == "max" ||
		name == "hold")
		return argumentCount == 2;
	if (name == "clamp") return argumentCount == 3;
	if (name == "deadzone") return argumentCount == 2 || argumentCount == 3;
	if (name == "smooth" || name == "tap" || name == "relative" || name == "pulse")
		return argumentCount == 1 || argumentCount == 2;
	return false;
}

class Parser
{
public:
	Parser(std::vector<Token> tokens, const ExpressionLimits& limits)
		: m_tokens(std::move(tokens)), m_limits(limits) {}

	std::unique_ptr<Node> Run()
	{
		auto root = ParseAssignment(0);
		if (Current().kind != TokenKind::End)
			throw ParseFailure("unexpected token after expression");
		return root;
	}

	std::size_t StatefulNodeCount() const noexcept { return m_statefulNodes; }

private:
	const Token& Current() const { return m_tokens[m_position]; }
	bool Match(TokenKind kind)
	{
		if (Current().kind != kind)
			return false;
		++m_position;
		return true;
	}
	const Token& Consume(TokenKind kind, const char* error)
	{
		if (Current().kind != kind)
			throw ParseFailure(error);
		return m_tokens[m_position++];
	}
	void CheckDepth(std::size_t depth) const
	{
		if (depth > m_limits.maximumNesting)
			throw ParseFailure("expression nesting is too deep");
	}

	std::unique_ptr<Node> MakeBinary(TokenKind operation,
		std::unique_ptr<Node> left, std::unique_ptr<Node> right)
	{
		auto result = std::make_unique<Node>();
		result->kind = NodeKind::Binary;
		result->operation = operation;
		result->children.push_back(std::move(left));
		result->children.push_back(std::move(right));
		return result;
	}

	std::unique_ptr<Node> ParseAssignment(std::size_t depth)
	{
		CheckDepth(depth);
		auto left = ParseConditional(depth + 1);
		const TokenKind operation = Current().kind;
		if (operation != TokenKind::Assign && operation != TokenKind::PlusAssign &&
			operation != TokenKind::MinusAssign && operation != TokenKind::StarAssign &&
			operation != TokenKind::SlashAssign)
			return left;
		++m_position;
		if (left->kind != NodeKind::Reference || left->text.empty() || left->text[0] != '$')
			throw ParseFailure("assignment target must be a $variable");
		auto result = std::make_unique<Node>();
		result->kind = NodeKind::Assignment;
		result->operation = operation;
		result->text = left->text;
		result->children.push_back(ParseAssignment(depth + 1));
		return result;
	}

	std::unique_ptr<Node> ParseConditional(std::size_t depth)
	{
		auto condition = ParseLogicalOr(depth + 1);
		if (!Match(TokenKind::Question))
			return condition;
		auto result = std::make_unique<Node>();
		result->kind = NodeKind::Conditional;
		result->children.push_back(std::move(condition));
		result->children.push_back(ParseAssignment(depth + 1));
		Consume(TokenKind::Colon, "expected ':' in conditional expression");
		result->children.push_back(ParseConditional(depth + 1));
		return result;
	}

#define PARSE_BINARY(Name, Next, ...) \
	std::unique_ptr<Node> Name(std::size_t depth) \
	{ \
		auto left = Next(depth + 1); \
		for (;;) \
		{ \
			const TokenKind operation = Current().kind; \
			if (!( __VA_ARGS__ )) return left; \
			++m_position; \
			left = MakeBinary(operation, std::move(left), Next(depth + 1)); \
		} \
	}

	PARSE_BINARY(ParseLogicalOr, ParseLogicalXor,
		operation == TokenKind::LogicalOr || operation == TokenKind::Pipe)
	PARSE_BINARY(ParseLogicalXor, ParseLogicalAnd, operation == TokenKind::Caret)
	PARSE_BINARY(ParseLogicalAnd, ParseEquality,
		operation == TokenKind::LogicalAnd || operation == TokenKind::Ampersand)
	PARSE_BINARY(ParseEquality, ParseComparison,
		operation == TokenKind::EqualEqual || operation == TokenKind::NotEqual)
	PARSE_BINARY(ParseComparison, ParseAdditive,
		operation == TokenKind::Less || operation == TokenKind::LessEqual ||
		operation == TokenKind::Greater || operation == TokenKind::GreaterEqual)
	PARSE_BINARY(ParseAdditive, ParseMultiplicative,
		operation == TokenKind::Plus || operation == TokenKind::Minus)
	PARSE_BINARY(ParseMultiplicative, ParseUnary,
		operation == TokenKind::Star || operation == TokenKind::Slash ||
		operation == TokenKind::Percent)
#undef PARSE_BINARY

	std::unique_ptr<Node> ParseUnary(std::size_t depth)
	{
		CheckDepth(depth);
		const TokenKind operation = Current().kind;
		if (operation == TokenKind::Plus || operation == TokenKind::Minus ||
			operation == TokenKind::Bang || operation == TokenKind::Tilde)
		{
			++m_position;
			auto result = std::make_unique<Node>();
			result->kind = NodeKind::Unary;
			result->operation = operation;
			result->children.push_back(ParseUnary(depth + 1));
			return result;
		}
		return ParsePrimary(depth + 1);
	}

	std::unique_ptr<Node> ParsePrimary(std::size_t depth)
	{
		CheckDepth(depth);
		if (Current().kind == TokenKind::Number)
		{
			auto result = std::make_unique<Node>();
			result->kind = NodeKind::Number;
			result->number = m_tokens[m_position++].number;
			return result;
		}
		if (Current().kind == TokenKind::Control)
		{
			auto result = std::make_unique<Node>();
			result->kind = NodeKind::Reference;
			result->text = m_tokens[m_position++].text;
			return result;
		}
		if (Current().kind == TokenKind::Identifier)
		{
			const std::string name = m_tokens[m_position++].text;
			if (!Match(TokenKind::LeftParen))
			{
				auto result = std::make_unique<Node>();
				result->kind = NodeKind::Reference;
				result->text = name;
				return result;
			}
			auto result = std::make_unique<Node>();
			result->kind = NodeKind::Call;
			result->text = name;
			if (!Match(TokenKind::RightParen))
			{
				do result->children.push_back(ParseAssignment(depth + 1));
				while (Match(TokenKind::Comma));
				Consume(TokenKind::RightParen, "expected ')' after function arguments");
			}
			if (!IsValidFunction(name, result->children.size()))
				throw ParseFailure("unknown function or invalid argument count: " + name);
			if (IsStatefulFunction(name))
			{
				if (m_statefulNodes >= m_limits.maximumStatefulNodes)
					throw ParseFailure("expression has too much evaluation state");
				result->stateId = m_statefulNodes++;
			}
			return result;
		}
		if (Match(TokenKind::LeftParen))
		{
			auto result = ParseAssignment(depth + 1);
			Consume(TokenKind::RightParen, "expected ')' after expression");
			return result;
		}
		throw ParseFailure("expected a value or control reference");
	}

	std::vector<Token> m_tokens;
	const ExpressionLimits& m_limits;
	std::size_t m_position = 0;
	std::size_t m_statefulNodes = 0;
};

struct FunctionState
{
	double value = 0.0;
	double started = 0.0;
	double until = 0.0;
	double previous = 0.0;
	bool active = false;
	bool initialized = false;
};

struct EvaluationContext
{
	const IInputValueSource& values;
	double now = 0.0;
	double delta = 0.0;
	std::unordered_map<std::string, double>& variables;
	std::vector<FunctionState>& state;
};

double Sanitize(double value)
{
	return std::isfinite(value) ? value : 0.0;
}

bool Digital(double value)
{
	return value >= 0.5;
}

double EvaluateNode(const Node& node, EvaluationContext& context);

std::vector<double> EvaluateArguments(const Node& node, EvaluationContext& context)
{
	std::vector<double> result;
	result.reserve(node.children.size());
	for (const auto& child : node.children)
		result.push_back(EvaluateNode(*child, context));
	return result;
}

double EvaluateCall(const Node& node, EvaluationContext& context)
{
	const std::vector<double> args = EvaluateArguments(node, context);
	auto require = [&args](std::size_t minimum, std::size_t maximum) {
		return args.size() >= minimum && args.size() <= maximum;
	};
	auto state = [&]() -> FunctionState& { return context.state.at(node.stateId); };
	const std::string& name = node.text;
	if (name == "if" && require(3, 3)) return Digital(args[0]) ? args[1] : args[2];
	if (name == "not" && require(1, 1)) return Digital(args[0]) ? 0.0 : 1.0;
	if (name == "abs" && require(1, 1)) return std::abs(args[0]);
	if (name == "sin" && require(1, 1)) return std::sin(args[0]);
	if (name == "cos" && require(1, 1)) return std::cos(args[0]);
	if (name == "tan" && require(1, 1)) return std::tan(args[0]);
	if (name == "asin" && require(1, 1)) return std::asin(std::clamp(args[0], -1.0, 1.0));
	if (name == "acos" && require(1, 1)) return std::acos(std::clamp(args[0], -1.0, 1.0));
	if (name == "atan" && require(1, 1)) return std::atan(args[0]);
	if (name == "atan2" && require(2, 2)) return std::atan2(args[0], args[1]);
	if (name == "sqrt" && require(1, 1)) return args[0] < 0.0 ? 0.0 : std::sqrt(args[0]);
	if (name == "pow" && require(2, 2)) return std::pow(args[0], args[1]);
	if (name == "min" && require(2, 2)) return std::min(args[0], args[1]);
	if (name == "max" && require(2, 2)) return std::max(args[0], args[1]);
	if (name == "clamp" && require(3, 3)) return std::clamp(args[0], std::min(args[1], args[2]), std::max(args[1], args[2]));
	if (name == "deadzone" && require(2, 3))
	{
		const double threshold = std::clamp(std::abs(args[1]), 0.0, 0.999999);
		const double maximum = args.size() == 3 ? std::max(threshold, std::abs(args[2])) : 1.0;
		const double magnitude = std::abs(args[0]);
		if (magnitude <= threshold) return 0.0;
		return std::copysign(std::clamp((magnitude - threshold) / (maximum - threshold), 0.0, 1.0), args[0]);
	}
	if (name == "timer" && require(1, 1))
	{
		auto& current = state();
		if (Digital(args[0]))
		{
			if (!current.active) current.started = context.now;
			current.active = true;
			return std::max(0.0, context.now - current.started);
		}
		current.active = false;
		return 0.0;
	}
	if (name == "toggle" && require(1, 1))
	{
		auto& current = state();
		const bool pressed = Digital(args[0]);
		if (pressed && !current.active) current.value = current.value == 0.0 ? 1.0 : 0.0;
		current.active = pressed;
		return current.value;
	}
	if (name == "smooth" && require(1, 2))
	{
		auto& current = state();
		const double speed = args.size() == 2 ? std::max(0.0, args[1]) : 12.0;
		if (!current.initialized) { current.value = args[0]; current.initialized = true; }
		const double amount = speed <= 0.0 ? 1.0 : 1.0 - std::exp(-speed * context.delta);
		current.value += (args[0] - current.value) * amount;
		return current.value;
	}
	if (name == "hold" && require(2, 2))
	{
		auto& current = state();
		if (Digital(args[0]))
		{
			if (!current.active) current.started = context.now;
			current.active = true;
			return context.now - current.started >= std::max(0.0, args[1]) ? args[0] : 0.0;
		}
		current.active = false;
		return 0.0;
	}
	if (name == "tap" && require(1, 2))
	{
		auto& current = state();
		const double maximum = args.size() == 2 ? std::max(0.0, args[1]) : 0.25;
		const bool pressed = Digital(args[0]);
		double output = 0.0;
		if (pressed && !current.active) current.started = context.now;
		if (!pressed && current.active && context.now - current.started <= maximum) output = 1.0;
		current.active = pressed;
		return output;
	}
	if (name == "relative" && require(1, 2))
	{
		auto& current = state();
		current.value += args[0] * context.delta * (args.size() == 2 ? args[1] : 1.0);
		return current.value;
	}
	if (name == "pulse" && require(1, 2))
	{
		auto& current = state();
		const bool pressed = Digital(args[0]);
		if (pressed && !current.active)
			current.until = context.now + (args.size() == 2 ? std::max(0.0, args[1]) : 0.1);
		current.active = pressed;
		return context.now < current.until ? 1.0 : 0.0;
	}
	return 0.0;
}

double EvaluateNode(const Node& node, EvaluationContext& context)
{
	double result = 0.0;
	switch (node.kind)
	{
	case NodeKind::Number: result = node.number; break;
	case NodeKind::Reference:
		if (!node.text.empty() && node.text[0] == '$')
		{
			const auto it = context.variables.find(node.text);
			result = it == context.variables.end() ? 0.0 : it->second;
		}
		else result = context.values.ReadInput(node.text);
		break;
	case NodeKind::Unary:
	{
		const double value = EvaluateNode(*node.children[0], context);
		switch (node.operation)
		{
		case TokenKind::Plus: result = value; break;
		case TokenKind::Minus: result = -value; break;
		case TokenKind::Bang:
		case TokenKind::Tilde: result = Digital(value) ? 0.0 : 1.0; break;
		default: break;
		}
		break;
	}
	case NodeKind::Binary:
	{
		const double left = EvaluateNode(*node.children[0], context);
		if (node.operation == TokenKind::LogicalAnd && !Digital(left)) return 0.0;
		if (node.operation == TokenKind::LogicalOr && Digital(left)) return 1.0;
		const double right = EvaluateNode(*node.children[1], context);
		switch (node.operation)
		{
		case TokenKind::Plus: result = left + right; break;
		case TokenKind::Minus: result = left - right; break;
		case TokenKind::Star: result = left * right; break;
		case TokenKind::Slash: result = right == 0.0 ? 0.0 : left / right; break;
		case TokenKind::Percent: result = right == 0.0 ? 0.0 : std::fmod(left, right); break;
		case TokenKind::Less: result = left < right; break;
		case TokenKind::LessEqual: result = left <= right; break;
		case TokenKind::Greater: result = left > right; break;
		case TokenKind::GreaterEqual: result = left >= right; break;
		case TokenKind::EqualEqual: result = left == right; break;
		case TokenKind::NotEqual: result = left != right; break;
		case TokenKind::Ampersand: result = std::min(left, right); break;
		case TokenKind::Pipe:
			result = std::abs(left) >= std::abs(right) ? left : right;
			break;
		case TokenKind::Caret: result = Digital(left) != Digital(right); break;
		case TokenKind::LogicalAnd: result = Digital(left) && Digital(right); break;
		case TokenKind::LogicalOr: result = Digital(left) || Digital(right); break;
		default: break;
		}
		break;
	}
	case NodeKind::Conditional:
		result = Digital(EvaluateNode(*node.children[0], context)) ?
			EvaluateNode(*node.children[1], context) : EvaluateNode(*node.children[2], context);
		break;
	case NodeKind::Call: result = EvaluateCall(node, context); break;
	case NodeKind::Assignment:
	{
		double& variable = context.variables[node.text];
		const double value = EvaluateNode(*node.children[0], context);
		switch (node.operation)
		{
		case TokenKind::Assign: variable = value; break;
		case TokenKind::PlusAssign: variable += value; break;
		case TokenKind::MinusAssign: variable -= value; break;
		case TokenKind::StarAssign: variable *= value; break;
		case TokenKind::SlashAssign: variable = value == 0.0 ? 0.0 : variable / value; break;
		default: break;
		}
		result = variable;
		break;
	}
	}
	return Sanitize(result);
}
}

struct MappingExpression::Impl
{
	std::string source;
	std::string error;
	std::unique_ptr<Node> root;
	std::vector<FunctionState> state;
	std::unordered_map<std::string, double> variables;
	double previousTime = 0.0;
	bool hasPreviousTime = false;
};

MappingExpression::MappingExpression() : m_impl(std::make_unique<Impl>()) {}

MappingExpression::MappingExpression(std::string source, ExpressionLimits limits)
	: MappingExpression()
{
	(void)Compile(std::move(source), limits);
}

MappingExpression::MappingExpression(MappingExpression&&) noexcept = default;
MappingExpression& MappingExpression::operator=(MappingExpression&&) noexcept = default;
MappingExpression::~MappingExpression() = default;

bool MappingExpression::Compile(std::string source, ExpressionLimits limits)
{
	m_impl = std::make_unique<Impl>();
	m_impl->source = std::move(source);
	if (m_impl->source.empty())
	{
		m_impl->error = "expression is empty";
		return false;
	}
	try
	{
		Parser parser(Lexer(m_impl->source, limits).Run(), limits);
		m_impl->root = parser.Run();
		m_impl->state.resize(parser.StatefulNodeCount());
		return true;
	}
	catch (const std::exception& error)
	{
		m_impl->root.reset();
		m_impl->error = error.what();
		return false;
	}
}

float MappingExpression::Evaluate(const IInputValueSource& values, double nowSeconds)
{
	if (!m_impl->root || !std::isfinite(nowSeconds))
		return 0.0f;
	const double delta = m_impl->hasPreviousTime ?
		std::clamp(nowSeconds - m_impl->previousTime, 0.0, 0.25) : 0.0;
	m_impl->previousTime = nowSeconds;
	m_impl->hasPreviousTime = true;
	EvaluationContext context{ values, nowSeconds, delta, m_impl->variables, m_impl->state };
	return static_cast<float>(Sanitize(EvaluateNode(*m_impl->root, context)));
}

void MappingExpression::Reset() noexcept
{
	m_impl->state.assign(m_impl->state.size(), {});
	m_impl->variables.clear();
	m_impl->previousTime = 0.0;
	m_impl->hasPreviousTime = false;
}

const std::string& MappingExpression::Source() const noexcept { return m_impl->source; }
const std::string& MappingExpression::Error() const noexcept { return m_impl->error; }
bool MappingExpression::IsValid() const noexcept { return m_impl->root != nullptr; }

std::string FormatInputExpressionForDisplay(std::string_view expression,
	std::span<const InputDeviceDescriptor> devices,
	std::string_view defaultDeviceId,
	std::string_view fallbackControlLabel)
{
	auto normalizedIdentifier = [](std::string_view value) {
		std::string normalized;
		normalized.reserve(value.size());
		for (const char character : value)
		{
			if (std::isalnum(static_cast<unsigned char>(character)))
				normalized.push_back(static_cast<char>(
					std::tolower(static_cast<unsigned char>(character))));
		}
		return normalized;
	};
	auto deviceFamily = [&normalizedIdentifier](std::string_view value) {
		const std::size_t separator = value.find(':');
		return normalizedIdentifier(value.substr(0, separator));
	};
	auto sameDeviceFamily = [&normalizedIdentifier, &deviceFamily](
		std::string_view referencedDeviceId, const InputDeviceDescriptor& candidate) {
		const std::string referenceFamily = deviceFamily(referencedDeviceId);
		const std::string idFamily = deviceFamily(candidate.id);
		const std::string apiFamily = normalizedIdentifier(candidate.api);
		auto equivalent = [](std::string_view left, std::string_view right) {
			if (left.empty() || right.empty())
				return false;
			if (left == right)
				return true;
			return left.size() >= 3 && right.size() >= 3 &&
				(left.starts_with(right) || right.starts_with(left));
		};
		return equivalent(referenceFamily, idFamily) ||
			equivalent(referenceFamily, apiFamily);
	};
	auto findDevice = [&devices](std::string_view id) -> const InputDeviceDescriptor* {
		const auto found = std::ranges::find_if(devices,
			[id](const InputDeviceDescriptor& device) { return device.id == id; });
		return found == devices.end() ? nullptr : &*found;
	};
	auto resolveReference = [&](std::string_view reference, bool& resolved) {
		const InputDeviceDescriptor* device = findDevice(defaultDeviceId);
		std::string_view controlId = reference;
		std::size_t matchedDeviceLength = 0;
		for (const auto& candidate : devices)
		{
			if (candidate.id.size() <= matchedDeviceLength ||
				reference.size() <= candidate.id.size() ||
				reference[candidate.id.size()] != '/' ||
				!reference.starts_with(candidate.id))
				continue;
			device = &candidate;
			controlId = reference.substr(candidate.id.size() + 1);
			matchedDeviceLength = candidate.id.size();
		}
		std::string_view referencedDeviceId;
		if (matchedDeviceLength == 0)
		{
			const std::size_t separator = reference.rfind('/');
			if (separator != std::string_view::npos)
			{
				device = nullptr;
				referencedDeviceId = reference.substr(0, separator);
				controlId = reference.substr(separator + 1);
			}
		}
		if (!device && !referencedDeviceId.empty())
		{
			const auto compatible = std::ranges::find_if(devices,
				[&](const InputDeviceDescriptor& candidate) {
					if (!sameDeviceFamily(referencedDeviceId, candidate))
						return false;
					return std::ranges::any_of(candidate.controls,
						[controlId](const InputControlDescriptor& descriptor) {
							return descriptor.id == controlId;
						});
				});
			if (compatible != devices.end())
				device = &*compatible;
		}
		if (device)
		{
			const auto control = std::ranges::find_if(device->controls,
				[controlId](const InputControlDescriptor& descriptor) {
					return descriptor.id == controlId;
				});
			if (control != device->controls.end() && !control->label.empty())
			{
				resolved = true;
				return control->label;
			}
		}
		resolved = false;
		return std::string(controlId);
	};

	std::string result;
	result.reserve(expression.size());
	std::size_t referenceCount = 0;
	std::size_t nonWhitespaceOutsideReferences = 0;
	bool soleReferenceResolved = false;
	for (std::size_t position = 0; position < expression.size();)
	{
		if (expression[position] != '`')
		{
			const char character = expression[position++];
			result.push_back(character);
			if (!std::isspace(static_cast<unsigned char>(character)))
				++nonWhitespaceOutsideReferences;
			continue;
		}

		const std::size_t referenceStart = position++;
		std::string reference;
		bool terminated = false;
		while (position < expression.size())
		{
			const char character = expression[position++];
			if (character == '`')
			{
				terminated = true;
				break;
			}
			if (character == '\\' && position < expression.size())
				reference.push_back(expression[position++]);
			else
				reference.push_back(character);
		}
		if (!terminated)
		{
			result.append(expression.substr(referenceStart));
			break;
		}
		bool resolved = false;
		result += resolveReference(reference, resolved);
		++referenceCount;
		soleReferenceResolved = resolved;
	}

	if (referenceCount == 1 && nonWhitespaceOutsideReferences == 0 &&
		!soleReferenceResolved && !fallbackControlLabel.empty())
		return std::string(fallbackControlLabel);
	return result;
}
}
