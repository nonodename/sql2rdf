#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "sparql-parser/Lexer.h"
#include "sparql-parser/ParseError.h"

using sparql::Lexer;
using sparql::Token;
using sparql::TokenType;

namespace {
std::vector<Token> tokenize(const std::string &text) {
	Lexer lexer(sparql::preprocessCodepointEscapes(text));
	std::vector<Token> tokens;
	for (;;) {
		Token t = lexer.next();
		tokens.push_back(t);
		if (t.type == TokenType::Eof) {
			break;
		}
	}
	return tokens;
}
} // namespace

TEST_CASE("Lexer tokenizes an IRIREF") {
	auto tokens = tokenize("<http://example.org/book1>");
	REQUIRE(tokens.size() == 2);
	REQUIRE(tokens[0].type == TokenType::Iriref);
	REQUIRE(tokens[0].text == "http://example.org/book1");
}

TEST_CASE("Lexer tokenizes prefixed names (PNAME_NS and PNAME_LN)") {
	auto tokens = tokenize("foaf: foaf:name");
	REQUIRE(tokens[0].type == TokenType::PnameNs);
	REQUIRE(tokens[0].text == "foaf:");
	REQUIRE(tokens[1].type == TokenType::PnameLn);
	REQUIRE(tokens[1].text == "foaf:name");
}

TEST_CASE("Lexer tokenizes a blank node label") {
	auto tokens = tokenize("_:b1");
	REQUIRE(tokens[0].type == TokenType::BlankNodeLabel);
	REQUIRE(tokens[0].text == "b1");
}

TEST_CASE("Lexer tokenizes VAR1 and VAR2 as the same variable form") {
	auto tokens = tokenize("?x $y");
	REQUIRE(tokens[0].type == TokenType::Var1);
	REQUIRE(tokens[0].text == "x");
	REQUIRE(tokens[1].type == TokenType::Var2);
	REQUIRE(tokens[1].text == "y");
}

TEST_CASE("Lexer distinguishes unsigned integer, decimal, and double literals") {
	auto tokens = tokenize("42 1.300 1.0e6 .5e2");
	REQUIRE(tokens[0].type == TokenType::Integer);
	REQUIRE(tokens[0].text == "42");
	REQUIRE(tokens[1].type == TokenType::Decimal);
	REQUIRE(tokens[1].text == "1.300");
	REQUIRE(tokens[2].type == TokenType::Double);
	REQUIRE(tokens[2].text == "1.0e6");
	REQUIRE(tokens[3].type == TokenType::Double);
	REQUIRE(tokens[3].text == ".5e2");
}

TEST_CASE("Lexer glues an adjacent sign onto a numeral (no whitespace)") {
	auto tokens = tokenize("+1 -1 - 1");
	REQUIRE(tokens[0].type == TokenType::Integer);
	REQUIRE(tokens[0].text == "+1");
	REQUIRE(tokens[1].type == TokenType::Integer);
	REQUIRE(tokens[1].text == "-1");
	// A sign separated from the numeral by whitespace is its own token.
	REQUIRE(tokens[2].type == TokenType::Minus);
	REQUIRE(tokens[3].type == TokenType::Integer);
	REQUIRE(tokens[3].text == "1");
}

TEST_CASE("Lexer supports all four string literal quoting forms and escapes") {
	auto tokens = tokenize("'a\\tb' \"c\\nd\" '''e''' \"\"\"f\"\"\"");
	REQUIRE(tokens[0].type == TokenType::StringLiteral);
	REQUIRE(tokens[0].text == "a\tb");
	REQUIRE(tokens[1].text == "c\nd");
	REQUIRE(tokens[2].text == "e");
	REQUIRE(tokens[3].text == "f");
}

TEST_CASE("Lexer allows embedded quotes and newlines inside triple-quoted strings") {
	auto tokens = tokenize("\"\"\"line1\nline2 \"quoted\"\"\"\"");
	REQUIRE(tokens[0].type == TokenType::StringLiteral);
	REQUIRE(tokens[0].text == "line1\nline2 \"quoted\"");
}

TEST_CASE("Lexer tokenizes a language tag") {
	auto tokens = tokenize("\"chat\"@en-GB");
	REQUIRE(tokens[0].type == TokenType::StringLiteral);
	REQUIRE(tokens[1].type == TokenType::LangTag);
	REQUIRE(tokens[1].text == "en-GB");
}

TEST_CASE("Lexer keywords are recognized case-insensitively but 'a' is case-sensitive") {
	auto tokens = tokenize("SeLeCt select a A");
	REQUIRE(tokens[0].type == TokenType::Keyword);
	REQUIRE(tokens[0].keyword == "SELECT");
	REQUIRE(tokens[1].type == TokenType::Keyword);
	REQUIRE(tokens[1].keyword == "SELECT");
	REQUIRE(tokens[2].type == TokenType::A);
	// A capital 'A' is not the special rdf:type keyword - it lexes as a
	// generic (here meaningless) keyword-shaped token, left for the parser
	// to reject.
	REQUIRE(tokens[3].type == TokenType::Keyword);
}

TEST_CASE("Lexer recognizes NIL '( )' and ANON '[ ]' even with whitespace between the brackets") {
	// Grammar: NIL ::= '(' WS* ')' and ANON ::= '[' WS* ']' - whitespace
	// between the brackets is explicitly permitted, so "( )" is NIL (not
	// LParen+RParen) and "[ ]" is ANON (not LBracket+RBracket).
	auto tokens = tokenize("() ( ) [] [ ]");
	REQUIRE(tokens[0].type == TokenType::Nil);
	REQUIRE(tokens[1].type == TokenType::Nil);
	REQUIRE(tokens[2].type == TokenType::Anon);
	REQUIRE(tokens[3].type == TokenType::Anon);
}

TEST_CASE("Lexer distinguishes non-empty brackets/parens from NIL/ANON") {
	auto tokens = tokenize("(1) [a]");
	REQUIRE(tokens[0].type == TokenType::LParen);
	REQUIRE(tokens[1].type == TokenType::Integer);
	REQUIRE(tokens[2].type == TokenType::RParen);
	REQUIRE(tokens[3].type == TokenType::LBracket);
	REQUIRE(tokens[4].type == TokenType::A);
	REQUIRE(tokens[5].type == TokenType::RBracket);
}

TEST_CASE("Lexer resolves \\u and \\U codepoint escapes before tokenizing") {
	// A is 'A'; this must be resolved even inside what becomes an
	// IRIREF, per SPARQL's own two-pass description (spec section 19.2).
	auto tokens = tokenize("<urn:\\u0041\\U00000042>");
	REQUIRE(tokens[0].type == TokenType::Iriref);
	REQUIRE(tokens[0].text == "urn:AB");
}

TEST_CASE("Lexer comments run to end of line and are skipped") {
	auto tokens = tokenize("?x # a comment with < and { in it\n?y");
	REQUIRE(tokens[0].type == TokenType::Var1);
	REQUIRE(tokens[0].text == "x");
	REQUIRE((tokens[1].type == TokenType::Var2 || tokens[1].type == TokenType::Var1));
	REQUIRE(tokens[1].text == "y");
}

TEST_CASE("Lexer disambiguates '<' as a relational operator from an IRIREF") {
	auto tokens = tokenize("?price < 20");
	REQUIRE(tokens[0].type == TokenType::Var1);
	REQUIRE(tokens[1].type == TokenType::Less);
	REQUIRE(tokens[2].type == TokenType::Integer);
}

TEST_CASE("Lexer throws on an unterminated string literal") {
	REQUIRE_THROWS_AS(tokenize("\"abc"), sparql::ParseError);
}

TEST_CASE("Lexer falls back to '<' as an operator when no valid IRIREF follows") {
	// "<urn:has space>" can't be a valid IRIREF (it contains a space before
	// any '>'), so '<' is tokenized as the relational operator instead of
	// failing outright at the lexer level - the resulting token stream is
	// simply ungrammatical, which the parser rejects (see
	// test_sparql_errors.cpp's "invalid_bad_iriref" case).
	auto tokens = tokenize("<urn:has space>");
	REQUIRE(tokens[0].type == TokenType::Less);
}
