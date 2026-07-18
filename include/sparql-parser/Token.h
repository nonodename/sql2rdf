#pragma once

#include <cstddef>
#include <string>

namespace sparql {

enum class TokenType {
	Iriref,
	PnameNs,
	PnameLn,
	BlankNodeLabel,
	Var1,
	Var2,
	Integer,
	Decimal,
	Double,
	StringLiteral,
	LangTag,
	LParen,
	RParen,
	LBrace,
	RBrace,
	LBracket,
	RBracket,
	Dot,
	Comma,
	Semicolon,
	Pipe,
	Slash,
	Caret,
	Question,
	Star,
	Plus,
	Minus,
	Equals,
	NotEquals,
	Less,
	Greater,
	LessEquals,
	GreaterEquals,
	Bang,
	AndAnd,
	OrOr,
	Nil,
	Anon,
	A,
	Keyword,
	Eof
};

/// A single lexical token. `text` holds the decoded lexeme (escape
/// sequences already resolved for strings/blank node labels/etc.);
/// `keyword` holds the upper-cased spelling when `type == Keyword`, so the
/// parser can compare case-insensitively without re-normalizing each time.
struct Token {
	TokenType type = TokenType::Eof;
	std::string text;
	std::string keyword;
	std::size_t line = 1;
	std::size_t column = 1;
};

} // namespace sparql
