#include "sparql-parser/Lexer.h"

#include <cctype>
#include <cstdint>

#include "sparql-parser/ParseError.h"

namespace sparql {

namespace {

bool isHexDigit(char c) {
	return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

int hexValue(char c) {
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
	return 10 + (c - 'A');
}

void appendUtf8(std::string &out, unsigned long codepoint) {
	if (codepoint <= 0x7F) {
		out.push_back(static_cast<char>(codepoint));
	} else if (codepoint <= 0x7FF) {
		out.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
		out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
	} else if (codepoint <= 0xFFFF) {
		out.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
		out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
		out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
	} else {
		out.push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
		out.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
		out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
		out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
	}
}

// Approximation of PN_CHARS_BASE/PN_CHARS_U/PN_CHARS for identifier-like
// content: treat any non-ASCII (UTF-8 continuation/lead) byte as valid,
// rather than encoding the exact Unicode code point ranges from §19.8.
// This accepts a superset of the formally-allowed characters but rejects
// nothing a well-formed query would use.
bool isNameStartChar(char c) {
	return std::isalpha(static_cast<unsigned char>(c)) != 0 || c == '_' || static_cast<unsigned char>(c) >= 0x80;
}

bool isNameChar(char c) {
	return isNameStartChar(c) || std::isdigit(static_cast<unsigned char>(c)) != 0 || c == '-' ||
	       static_cast<unsigned char>(c) == 0xB7;
}

// VARNAME (rule 166) is narrower than PN_CHARS: no '-', so that e.g. "?n-1"
// lexes as variable "n" followed by the numeral "-1", not a variable
// literally named "n-1".
bool isVarNameChar(char c) {
	return isNameStartChar(c) || std::isdigit(static_cast<unsigned char>(c)) != 0 ||
	       static_cast<unsigned char>(c) == 0xB7;
}

} // namespace

std::string preprocessCodepointEscapes(const std::string &input) {
	std::string out;
	out.reserve(input.size());
	for (std::size_t i = 0; i < input.size();) {
		if (input[i] == '\\' && i + 1 < input.size() && (input[i + 1] == 'u' || input[i + 1] == 'U')) {
			std::size_t digits = (input[i + 1] == 'u') ? 4 : 8;
			if (i + 2 + digits <= input.size()) {
				bool allHex = true;
				for (std::size_t d = 0; d < digits; ++d) {
					if (!isHexDigit(input[i + 2 + d])) {
						allHex = false;
						break;
					}
				}
				if (allHex) {
					unsigned long codepoint = 0;
					for (std::size_t d = 0; d < digits; ++d) {
						codepoint = (codepoint << 4) | static_cast<unsigned long>(hexValue(input[i + 2 + d]));
					}
					appendUtf8(out, codepoint);
					i += 2 + digits;
					continue;
				}
			}
		}
		out.push_back(input[i]);
		++i;
	}
	return out;
}

Lexer::Lexer(std::string source) : source_(std::move(source)) {}

char Lexer::peek(std::size_t offset) const {
	std::size_t idx = pos_ + offset;
	if (idx >= source_.size()) return '\0';
	return source_[idx];
}

char Lexer::advance() {
	char c = source_[pos_++];
	if (c == '\n') {
		++line_;
		column_ = 1;
	} else {
		++column_;
	}
	return c;
}

bool Lexer::match(char expected) {
	if (peek() != expected) return false;
	advance();
	return true;
}

void Lexer::error(const std::string &message) {
	std::string near = source_.substr(pos_, 16);
	throw ParseError(message, line_, column_, near);
}

Token Lexer::makeToken(TokenType type, std::string text, std::size_t startLine, std::size_t startColumn) {
	Token t;
	t.type = type;
	t.text = std::move(text);
	t.line = startLine;
	t.column = startColumn;
	return t;
}

void Lexer::skipWhitespaceAndComments() {
	for (;;) {
		char c = peek();
		if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
			advance();
		} else if (c == '#') {
			while (peek() != '\0' && peek() != '\n' && peek() != '\r') advance();
		} else {
			break;
		}
	}
}

Token Lexer::next() {
	skipWhitespaceAndComments();
	std::size_t startLine = line_;
	std::size_t startColumn = column_;
	char c = peek();
	if (c == '\0') return makeToken(TokenType::Eof, "", startLine, startColumn);

	switch (c) {
	case '<':
		if (looksLikeIriref()) return lexIriref();
		advance();
		if (match('=')) return makeToken(TokenType::LessEquals, "<=", startLine, startColumn);
		return makeToken(TokenType::Less, "<", startLine, startColumn);
	case '?':
		// '?' is both the VAR1 prefix and the ZeroOrOnePath modifier (rule
		// 93). It's a variable only when immediately followed by a valid
		// name-start character; otherwise (whitespace, punctuation, EOF)
		// it's the path modifier.
		if (isNameStartChar(peek(1)) || std::isdigit(static_cast<unsigned char>(peek(1)))) return lexVar();
		advance();
		return makeToken(TokenType::Question, "?", startLine, startColumn);
	case '$':
		return lexVar();
	case '_':
		if (peek(1) == ':') return lexBlankNodeLabel();
		break;
	case '(': {
		advance();
		std::size_t save = pos_, saveLine = line_, saveCol = column_;
		skipWhitespaceAndComments();
		if (peek() == ')') {
			advance();
			return makeToken(TokenType::Nil, "()", startLine, startColumn);
		}
		pos_ = save;
		line_ = saveLine;
		column_ = saveCol;
		return makeToken(TokenType::LParen, "(", startLine, startColumn);
	}
	case ')':
		advance();
		return makeToken(TokenType::RParen, ")", startLine, startColumn);
	case '{':
		advance();
		return makeToken(TokenType::LBrace, "{", startLine, startColumn);
	case '}':
		advance();
		return makeToken(TokenType::RBrace, "}", startLine, startColumn);
	case '[': {
		advance();
		std::size_t save = pos_, saveLine = line_, saveCol = column_;
		skipWhitespaceAndComments();
		if (peek() == ']') {
			advance();
			return makeToken(TokenType::Anon, "[]", startLine, startColumn);
		}
		pos_ = save;
		line_ = saveLine;
		column_ = saveCol;
		return makeToken(TokenType::LBracket, "[", startLine, startColumn);
	}
	case ']':
		advance();
		return makeToken(TokenType::RBracket, "]", startLine, startColumn);
	case '.':
		if (!std::isdigit(static_cast<unsigned char>(peek(1)))) {
			advance();
			return makeToken(TokenType::Dot, ".", startLine, startColumn);
		}
		return lexNumberOrSign();
	case ',':
		advance();
		return makeToken(TokenType::Comma, ",", startLine, startColumn);
	case ';':
		advance();
		return makeToken(TokenType::Semicolon, ";", startLine, startColumn);
	case '|':
		advance();
		if (match('|')) return makeToken(TokenType::OrOr, "||", startLine, startColumn);
		return makeToken(TokenType::Pipe, "|", startLine, startColumn);
	case '/':
		advance();
		return makeToken(TokenType::Slash, "/", startLine, startColumn);
	case '^':
		advance();
		if (match('^')) return makeToken(TokenType::Caret, "^^", startLine, startColumn);
		return makeToken(TokenType::Caret, "^", startLine, startColumn);
	case '*':
		advance();
		return makeToken(TokenType::Star, "*", startLine, startColumn);
	case '+':
		if (std::isdigit(static_cast<unsigned char>(peek(1))) || peek(1) == '.') return lexNumberOrSign();
		advance();
		return makeToken(TokenType::Plus, "+", startLine, startColumn);
	case '-':
		if (std::isdigit(static_cast<unsigned char>(peek(1))) || peek(1) == '.') return lexNumberOrSign();
		advance();
		return makeToken(TokenType::Minus, "-", startLine, startColumn);
	case '=':
		advance();
		return makeToken(TokenType::Equals, "=", startLine, startColumn);
	case '!':
		advance();
		if (match('=')) return makeToken(TokenType::NotEquals, "!=", startLine, startColumn);
		return makeToken(TokenType::Bang, "!", startLine, startColumn);
	case '&':
		advance();
		if (match('&')) return makeToken(TokenType::AndAnd, "&&", startLine, startColumn);
		error("unexpected character '&'");
	case '@':
		return lexLangTagOrCaret();
	case '>':
		advance();
		if (match('=')) return makeToken(TokenType::GreaterEquals, ">=", startLine, startColumn);
		return makeToken(TokenType::Greater, ">", startLine, startColumn);
	case ':':
		return lexPrefixedNameOrKeyword();
	default:
		break;
	}

	if (c == '\'' || c == '"') return lexString();

	if (std::isdigit(static_cast<unsigned char>(c))) return lexNumberOrSign();

	if (isNameStartChar(c)) return lexPrefixedNameOrKeyword();

	error(std::string("unexpected character '") + c + "'");
}

bool Lexer::looksLikeIriref() const {
	for (std::size_t i = pos_ + 1; i < source_.size(); ++i) {
		char c = source_[i];
		if (c == '>') return true;
		if (c == '<' || c == '"' || c == '{' || c == '}' || c == '|' || c == '^' || c == '`' ||
		    static_cast<unsigned char>(c) <= 0x20) {
			return false;
		}
	}
	return false;
}

Token Lexer::lexIriref() {
	std::size_t startLine = line_, startColumn = column_;
	advance(); // consume '<'
	std::string value;
	for (;;) {
		char c = peek();
		if (c == '\0') error("unterminated IRIREF");
		if (c == '>') {
			advance();
			break;
		}
		if (c == '<' || c == '"' || c == '{' || c == '}' || c == '|' || c == '^' || c == '`' ||
		    static_cast<unsigned char>(c) <= 0x20) {
			error("invalid character in IRIREF");
		}
		value.push_back(advance());
	}
	return makeToken(TokenType::Iriref, value, startLine, startColumn);
}

std::string Lexer::lexPercentOrLocalEscape() {
	// Called with the current char being either '%' (PERCENT) or '\\' (PN_LOCAL_ESC).
	std::string out;
	if (peek() == '%') {
		out.push_back(advance());
		for (int i = 0; i < 2; ++i) {
			if (!isHexDigit(peek())) error("invalid %-escape in local name");
			out.push_back(advance());
		}
	} else if (peek() == '\\') {
		advance();
		char c = peek();
		static const std::string allowed = "_~.-!$&'()*+,;=/?#@%";
		if (allowed.find(c) == std::string::npos) error("invalid escape in local name");
		out.push_back(advance());
	}
	return out;
}

std::string Lexer::lexPnLocal() {
	std::string out;
	// First character: PN_CHARS_U | ':' | digit | PLX
	if (peek() == '%' || peek() == '\\') {
		out += lexPercentOrLocalEscape();
	} else if (isNameStartChar(peek()) || peek() == ':' || std::isdigit(static_cast<unsigned char>(peek()))) {
		out.push_back(advance());
	} else {
		return out;
	}
	for (;;) {
		char c = peek();
		if (c == '%' || c == '\\') {
			out += lexPercentOrLocalEscape();
		} else if (isNameChar(c) || c == ':' || c == '.') {
			out.push_back(advance());
		} else {
			break;
		}
	}
	// PN_LOCAL must not end with '.'; trim trailing dots back onto the stream.
	while (!out.empty() && out.back() == '.') {
		out.pop_back();
		--pos_;
		--column_;
	}
	return out;
}

Token Lexer::lexPrefixedNameOrKeyword() {
	std::size_t startLine = line_, startColumn = column_;
	std::string prefix;
	while (isNameChar(peek()) || (peek() == '.' && isNameChar(peek(1)))) {
		prefix.push_back(advance());
	}
	if (peek() == ':') {
		advance();
		std::string local = lexPnLocal();
		if (local.empty()) return makeToken(TokenType::PnameNs, prefix + ":", startLine, startColumn);
		return makeToken(TokenType::PnameLn, prefix + ":" + local, startLine, startColumn);
	}
	if (prefix == "a") return makeToken(TokenType::A, "a", startLine, startColumn);
	Token t = makeToken(TokenType::Keyword, prefix, startLine, startColumn);
	std::string upper;
	upper.reserve(prefix.size());
	for (char ch : prefix) upper.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(ch))));
	t.keyword = upper;
	return t;
}

Token Lexer::lexVar() {
	std::size_t startLine = line_, startColumn = column_;
	TokenType type = (peek() == '?') ? TokenType::Var1 : TokenType::Var2;
	advance();
	std::string name;
	if (!isNameStartChar(peek()) && !std::isdigit(static_cast<unsigned char>(peek()))) {
		error("expected variable name after '?'/'$'");
	}
	name.push_back(advance());
	while (isVarNameChar(peek())) name.push_back(advance());
	return makeToken(type, name, startLine, startColumn);
}

Token Lexer::lexBlankNodeLabel() {
	std::size_t startLine = line_, startColumn = column_;
	advance(); // '_'
	advance(); // ':'
	std::string label;
	if (!isNameStartChar(peek()) && !std::isdigit(static_cast<unsigned char>(peek()))) {
		error("expected blank node label after '_:'");
	}
	label.push_back(advance());
	while (isNameChar(peek()) || (peek() == '.' && isNameChar(peek(1)))) label.push_back(advance());
	return makeToken(TokenType::BlankNodeLabel, label, startLine, startColumn);
}

Token Lexer::lexNumberOrSign() {
	bool negative = false;
	bool consumedSign = false;
	if (peek() == '+' || peek() == '-') {
		negative = (peek() == '-');
		advance();
		consumedSign = true;
	}
	return lexNumber(negative, consumedSign);
}

Token Lexer::lexNumber(bool negative, bool consumedSign) {
	std::size_t startLine = line_, startColumn = column_;
	std::string text = negative ? "-" : (consumedSign ? "+" : "");
	bool sawDigitsBeforeDot = false;
	while (std::isdigit(static_cast<unsigned char>(peek()))) {
		text.push_back(advance());
		sawDigitsBeforeDot = true;
	}
	bool isDecimal = false;
	if (peek() == '.' && std::isdigit(static_cast<unsigned char>(peek(1)))) {
		isDecimal = true;
		text.push_back(advance());
		while (std::isdigit(static_cast<unsigned char>(peek()))) text.push_back(advance());
	} else if (peek() == '.' && !sawDigitsBeforeDot) {
		error("invalid numeric literal");
	}
	bool isDouble = false;
	if (peek() == 'e' || peek() == 'E') {
		isDouble = true;
		text.push_back(advance());
		if (peek() == '+' || peek() == '-') text.push_back(advance());
		if (!std::isdigit(static_cast<unsigned char>(peek()))) error("invalid exponent in numeric literal");
		while (std::isdigit(static_cast<unsigned char>(peek()))) text.push_back(advance());
	}
	TokenType type = isDouble ? TokenType::Double : (isDecimal ? TokenType::Decimal : TokenType::Integer);
	return makeToken(type, text, startLine, startColumn);
}

Token Lexer::lexString() {
	std::size_t startLine = line_, startColumn = column_;
	char quote = peek();
	bool longForm = (peek(1) == quote && peek(2) == quote);
	if (longForm) {
		advance();
		advance();
		advance();
	} else {
		advance();
	}
	std::string value;
	for (;;) {
		if (peek() == '\0') error("unterminated string literal");
		if (longForm) {
			// A run of exactly 3 quote characters closes the string. A
			// longer run (peek(3) is also the quote char) means the actual
			// delimiter is the *last* 3 of the run, and this quote is
			// content (e.g. a lexical form ending in a quote right before
			// the closing delimiter, as in `"""she said "hi""""`).
			if (peek() == quote && peek(1) == quote && peek(2) == quote && peek(3) != quote) {
				advance();
				advance();
				advance();
				break;
			}
		} else {
			if (peek() == quote) {
				advance();
				break;
			}
			if (peek() == '\n' || peek() == '\r') error("unterminated string literal");
		}
		if (peek() == '\\') {
			advance();
			char e = advance();
			switch (e) {
			case 't':
				value.push_back('\t');
				break;
			case 'n':
				value.push_back('\n');
				break;
			case 'r':
				value.push_back('\r');
				break;
			case 'b':
				value.push_back('\b');
				break;
			case 'f':
				value.push_back('\f');
				break;
			case '"':
				value.push_back('"');
				break;
			case '\'':
				value.push_back('\'');
				break;
			case '\\':
				value.push_back('\\');
				break;
			default:
				error("invalid escape sequence in string literal");
			}
		} else {
			value.push_back(advance());
		}
	}
	return makeToken(TokenType::StringLiteral, value, startLine, startColumn);
}

Token Lexer::lexLangTagOrCaret() {
	std::size_t startLine = line_, startColumn = column_;
	advance(); // '@'
	std::string tag;
	if (!std::isalpha(static_cast<unsigned char>(peek()))) error("expected language tag after '@'");
	while (std::isalpha(static_cast<unsigned char>(peek()))) tag.push_back(advance());
	while (peek() == '-') {
		tag.push_back(advance());
		if (!std::isalnum(static_cast<unsigned char>(peek()))) error("invalid language tag");
		while (std::isalnum(static_cast<unsigned char>(peek()))) tag.push_back(advance());
	}
	return makeToken(TokenType::LangTag, tag, startLine, startColumn);
}

} // namespace sparql
