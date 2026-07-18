#pragma once

#include <string>

#include "sparql-parser/Token.h"

namespace sparql {

/// Hand-written tokenizer for the SPARQL 1.1 Query grammar terminals
/// (§19.8, productions 139-173), plus punctuation. Codepoint escape
/// sequences (\uXXXX / \UXXXXXXXX, §19.2) are expected to already have been
/// resolved by `preprocessCodepointEscapes` before the source text reaches
/// the lexer, matching the spec's own two-pass description.
class Lexer {
public:
	explicit Lexer(std::string source);

	/// Return the next token, or an Eof token once the input is exhausted.
	/// Throws ParseError on malformed input (unterminated string/IRIREF,
	/// invalid escape, stray character, ...).
	Token next();

private:
	char peek(std::size_t offset = 0) const;
	char advance();
	bool match(char expected);
	void skipWhitespaceAndComments();
	/// True if the characters after the current '<' form a valid IRIREF
	/// (reach a '>' before any excluded character); used to disambiguate
	/// IRIREF from the '<'/'<=' relational operators, which share a prefix.
	bool looksLikeIriref() const;
	[[noreturn]] void error(const std::string &message);

	Token makeToken(TokenType type, std::string text, std::size_t startLine, std::size_t startColumn);

	Token lexIriref();
	Token lexPrefixedNameOrKeyword();
	Token lexVar();
	Token lexBlankNodeLabel();
	Token lexNumberOrSign();
	Token lexNumber(bool negative, bool consumedSign);
	Token lexString();
	Token lexLangTagOrCaret();
	std::string lexPercentOrLocalEscape();
	std::string lexPnLocal();

	std::string source_;
	std::size_t pos_ = 0;
	std::size_t line_ = 1;
	std::size_t column_ = 1;
};

/// Resolve \uXXXX / \UXXXXXXXX codepoint escapes anywhere in `input` into
/// their UTF-8 byte sequence, per SPARQL §19.2. Applied once to the whole
/// query text before lexing.
std::string preprocessCodepointEscapes(const std::string &input);

} // namespace sparql
