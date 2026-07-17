#include "sparql-parser/Parser.h"

namespace sparql {

using ast::AggregateExpr;
using ast::AggregateKind;
using ast::BinaryExpr;
using ast::BinaryOp;
using ast::BuiltInCallExpr;
using ast::BuiltinFunction;
using ast::ExistsExpr;
using ast::Expression;
using ast::FunctionCallExpr;
using ast::InExpr;
using ast::Iri;
using ast::IriExpr;
using ast::LiteralExpr;
using ast::RdfLiteral;
using ast::UnaryExpr;
using ast::UnaryOp;
using ast::Var;
using ast::VarExpr;

namespace {
const char *const XSD_INTEGER_EXPR = "http://www.w3.org/2001/XMLSchema#integer";
const char *const XSD_DECIMAL_EXPR = "http://www.w3.org/2001/XMLSchema#decimal";
const char *const XSD_DOUBLE_EXPR = "http://www.w3.org/2001/XMLSchema#double";

struct KeywordFn {
	const char *kw;
	BuiltinFunction fn;
};

const KeywordFn kOneArgBuiltins[] = {
    {"STR", BuiltinFunction::Str},
    {"LANG", BuiltinFunction::Lang},
    {"DATATYPE", BuiltinFunction::Datatype},
    {"IRI", BuiltinFunction::IriFn},
    {"URI", BuiltinFunction::UriFn},
    {"ABS", BuiltinFunction::Abs},
    {"CEIL", BuiltinFunction::Ceil},
    {"FLOOR", BuiltinFunction::Floor},
    {"ROUND", BuiltinFunction::Round},
    {"STRLEN", BuiltinFunction::Strlen},
    {"UCASE", BuiltinFunction::Ucase},
    {"LCASE", BuiltinFunction::Lcase},
    {"ENCODE_FOR_URI", BuiltinFunction::EncodeForUri},
    {"YEAR", BuiltinFunction::Year},
    {"MONTH", BuiltinFunction::Month},
    {"DAY", BuiltinFunction::Day},
    {"HOURS", BuiltinFunction::Hours},
    {"MINUTES", BuiltinFunction::Minutes},
    {"SECONDS", BuiltinFunction::Seconds},
    {"TIMEZONE", BuiltinFunction::Timezone},
    {"TZ", BuiltinFunction::Tz},
    {"MD5", BuiltinFunction::Md5},
    {"SHA1", BuiltinFunction::Sha1},
    {"SHA256", BuiltinFunction::Sha256},
    {"SHA384", BuiltinFunction::Sha384},
    {"SHA512", BuiltinFunction::Sha512},
    {"ISIRI", BuiltinFunction::IsIri},
    {"ISURI", BuiltinFunction::IsUri},
    {"ISBLANK", BuiltinFunction::IsBlank},
    {"ISLITERAL", BuiltinFunction::IsLiteral},
    {"ISNUMERIC", BuiltinFunction::IsNumeric},
};

const KeywordFn kTwoArgBuiltins[] = {
    {"LANGMATCHES", BuiltinFunction::LangMatches}, {"CONTAINS", BuiltinFunction::Contains},
    {"STRSTARTS", BuiltinFunction::Strstarts},     {"STRENDS", BuiltinFunction::Strends},
    {"STRBEFORE", BuiltinFunction::Strbefore},     {"STRAFTER", BuiltinFunction::Strafter},
    {"STRLANG", BuiltinFunction::Strlang},         {"STRDT", BuiltinFunction::Strdt},
    {"SAMETERM", BuiltinFunction::SameTerm},
};

const char *const kZeroArgKeywords[] = {"RAND", "NOW", "UUID", "STRUUID"};
const BuiltinFunction kZeroArgFns[] = {BuiltinFunction::Rand, BuiltinFunction::Now, BuiltinFunction::Uuid,
                                       BuiltinFunction::Struuid};

const char *const kExpressionStartKeywords[] = {
    "TRUE",        "FALSE",     "COUNT",
    "SUM",         "MIN",       "MAX",
    "AVG",         "SAMPLE",    "GROUP_CONCAT",
    "NOT",         "EXISTS",    "BOUND",
    "IF",          "COALESCE",  "CONCAT",
    "BNODE",       "RAND",      "NOW",
    "UUID",        "STRUUID",   "STR",
    "LANG",        "DATATYPE",  "IRI",
    "URI",         "ABS",       "CEIL",
    "FLOOR",       "ROUND",     "STRLEN",
    "UCASE",       "LCASE",     "ENCODE_FOR_URI",
    "YEAR",        "MONTH",     "DAY",
    "HOURS",       "MINUTES",   "SECONDS",
    "TIMEZONE",    "TZ",        "MD5",
    "SHA1",        "SHA256",    "SHA384",
    "SHA512",      "ISIRI",     "ISURI",
    "ISBLANK",     "ISLITERAL", "ISNUMERIC",
    "LANGMATCHES", "CONTAINS",  "STRSTARTS",
    "STRENDS",     "STRBEFORE", "STRAFTER",
    "STRLANG",     "STRDT",     "SAMETERM",
    "SUBSTR",      "REGEX",     "REPLACE",
};

} // namespace

std::unique_ptr<Expression> Parser::parseExpression() {
	return parseConditionalOrExpression();
}

std::unique_ptr<Expression> Parser::parseConditionalOrExpression() {
	std::unique_ptr<Expression> left = parseConditionalAndExpression();
	while (matchType(TokenType::OrOr)) {
		left.reset(new BinaryExpr(BinaryOp::Or, std::move(left), parseConditionalAndExpression()));
	}
	return left;
}

std::unique_ptr<Expression> Parser::parseConditionalAndExpression() {
	std::unique_ptr<Expression> left = parseValueLogical();
	while (matchType(TokenType::AndAnd)) {
		left.reset(new BinaryExpr(BinaryOp::And, std::move(left), parseValueLogical()));
	}
	return left;
}

std::unique_ptr<Expression> Parser::parseValueLogical() {
	return parseRelationalExpression();
}

std::unique_ptr<Expression> Parser::parseRelationalExpression() {
	std::unique_ptr<Expression> left = parseNumericExpression();
	if (matchType(TokenType::Equals)) {
		return std::unique_ptr<Expression>(new BinaryExpr(BinaryOp::Eq, std::move(left), parseNumericExpression()));
	}
	if (matchType(TokenType::NotEquals)) {
		return std::unique_ptr<Expression>(new BinaryExpr(BinaryOp::Ne, std::move(left), parseNumericExpression()));
	}
	if (matchType(TokenType::Less)) {
		return std::unique_ptr<Expression>(new BinaryExpr(BinaryOp::Lt, std::move(left), parseNumericExpression()));
	}
	if (matchType(TokenType::Greater)) {
		return std::unique_ptr<Expression>(new BinaryExpr(BinaryOp::Gt, std::move(left), parseNumericExpression()));
	}
	if (matchType(TokenType::LessEquals)) {
		return std::unique_ptr<Expression>(new BinaryExpr(BinaryOp::Le, std::move(left), parseNumericExpression()));
	}
	if (matchType(TokenType::GreaterEquals)) {
		return std::unique_ptr<Expression>(new BinaryExpr(BinaryOp::Ge, std::move(left), parseNumericExpression()));
	}
	if (checkKeyword("IN")) {
		advance();
		return std::unique_ptr<Expression>(new InExpr(std::move(left), parseExpressionList(), false));
	}
	if (checkKeyword("NOT")) {
		advance();
		expectKeyword("IN");
		return std::unique_ptr<Expression>(new InExpr(std::move(left), parseExpressionList(), true));
	}
	return left;
}

std::unique_ptr<Expression> Parser::parseNumericExpression() {
	return parseAdditiveExpression();
}

std::unique_ptr<Expression> Parser::parseAdditiveExpression() {
	std::unique_ptr<Expression> left = parseMultiplicativeExpression();
	for (;;) {
		if (matchType(TokenType::Plus)) {
			left.reset(new BinaryExpr(BinaryOp::Add, std::move(left), parseMultiplicativeExpression()));
			continue;
		}
		if (matchType(TokenType::Minus)) {
			left.reset(new BinaryExpr(BinaryOp::Sub, std::move(left), parseMultiplicativeExpression()));
			continue;
		}
		// A signed numeral glued directly (no whitespace) to the previous
		// token is lexed as one signed-literal token; per grammar note 6 it
		// still means "+"/"-" applied to the (unsigned) numeral, which may
		// itself continue with '*'/'/' (rule 116).
		bool numeric = check(TokenType::Integer) || check(TokenType::Decimal) || check(TokenType::Double);
		if (numeric && !current_.text.empty() && (current_.text[0] == '+' || current_.text[0] == '-')) {
			BinaryOp op = (current_.text[0] == '-') ? BinaryOp::Sub : BinaryOp::Add;
			TokenType t = current_.type;
			std::string unsignedLex = current_.text.substr(1);
			advance();
			std::unique_ptr<RdfLiteral> lit(new RdfLiteral(unsignedLex));
			const char *dt = XSD_DOUBLE_EXPR;
			if (t == TokenType::Integer) {
				dt = XSD_INTEGER_EXPR;
			} else if (t == TokenType::Decimal) {
				dt = XSD_DECIMAL_EXPR;
			}
			lit->datatype = makeIri(dt, "");
			std::unique_ptr<Expression> rhs(new LiteralExpr(std::move(lit)));
			rhs = continueMultiplicativeFrom(std::move(rhs));
			left.reset(new BinaryExpr(op, std::move(left), std::move(rhs)));
			continue;
		}
		break;
	}
	return left;
}

std::unique_ptr<Expression> Parser::parseMultiplicativeExpression() {
	return continueMultiplicativeFrom(parseUnaryExpression());
}

std::unique_ptr<Expression> Parser::continueMultiplicativeFrom(std::unique_ptr<Expression> left) {
	for (;;) {
		if (matchType(TokenType::Star)) {
			left.reset(new BinaryExpr(BinaryOp::Mul, std::move(left), parseUnaryExpression()));
		} else if (matchType(TokenType::Slash)) {
			left.reset(new BinaryExpr(BinaryOp::Div, std::move(left), parseUnaryExpression()));
		} else {
			break;
		}
	}
	return left;
}

std::unique_ptr<Expression> Parser::parseUnaryExpression() {
	if (matchType(TokenType::Bang)) {
		return std::unique_ptr<Expression>(new UnaryExpr(UnaryOp::Not, parsePrimaryExpression()));
	}
	if (matchType(TokenType::Plus)) {
		return std::unique_ptr<Expression>(new UnaryExpr(UnaryOp::Plus, parsePrimaryExpression()));
	}
	if (matchType(TokenType::Minus)) {
		return std::unique_ptr<Expression>(new UnaryExpr(UnaryOp::Minus, parsePrimaryExpression()));
	}
	return parsePrimaryExpression();
}

std::unique_ptr<Expression> Parser::parsePrimaryExpression() {
	if (matchType(TokenType::LParen)) {
		std::unique_ptr<Expression> e = parseExpression();
		expectType(TokenType::RParen, "')' closing a parenthesized expression");
		return e;
	}
	if (check(TokenType::Var1) || check(TokenType::Var2)) {
		return std::unique_ptr<Expression>(new VarExpr(parseVar()));
	}
	if (check(TokenType::StringLiteral)) {
		return std::unique_ptr<Expression>(new LiteralExpr(parseRdfLiteral()));
	}
	if (check(TokenType::Integer) || check(TokenType::Decimal) || check(TokenType::Double)) {
		return std::unique_ptr<Expression>(new LiteralExpr(parseNumericLiteral()));
	}
	if (checkKeyword("TRUE") || checkKeyword("FALSE")) {
		return std::unique_ptr<Expression>(new LiteralExpr(parseBooleanLiteral()));
	}
	if (check(TokenType::Iriref) || check(TokenType::PnameNs) || check(TokenType::PnameLn)) {
		return parseIriOrFunctionCall();
	}
	return parseBuiltInCallOrAggregateOrExists();
}

std::unique_ptr<Expression> Parser::parseIriOrFunctionCall() {
	std::unique_ptr<Iri> iri = parseIri();
	if (check(TokenType::LParen) || check(TokenType::Nil)) {
		bool distinct = false;
		std::vector<std::unique_ptr<Expression>> args = parseArgList(&distinct);
		return std::unique_ptr<Expression>(new FunctionCallExpr(std::move(iri), std::move(args), distinct));
	}
	return std::unique_ptr<Expression>(new IriExpr(std::move(iri)));
}

std::vector<std::unique_ptr<Expression>> Parser::parseArgList(bool *distinctOut) {
	std::vector<std::unique_ptr<Expression>> args;
	if (matchType(TokenType::Nil)) {
		if (distinctOut) {
			*distinctOut = false;
		}
		return args;
	}
	expectType(TokenType::LParen, "'(' to start an argument list");
	if (distinctOut) {
		*distinctOut = matchKeyword("DISTINCT");
	}
	if (!check(TokenType::RParen)) {
		args.push_back(parseExpression());
		while (matchType(TokenType::Comma)) {
			args.push_back(parseExpression());
		}
	}
	expectType(TokenType::RParen, "')' closing an argument list");
	return args;
}

std::vector<std::unique_ptr<Expression>> Parser::parseExpressionList() {
	std::vector<std::unique_ptr<Expression>> list;
	if (matchType(TokenType::Nil)) {
		return list;
	}
	expectType(TokenType::LParen, "'(' to start an expression list");
	list.push_back(parseExpression());
	while (matchType(TokenType::Comma)) {
		list.push_back(parseExpression());
	}
	expectType(TokenType::RParen, "')' closing an expression list");
	return list;
}

std::unique_ptr<Expression> Parser::parseBuiltinArgsN(BuiltinFunction fn, int minArgs, int maxArgs) {
	advance(); // consume the builtin's keyword
	expectType(TokenType::LParen, "'(' to start this function's argument list");
	std::vector<std::unique_ptr<Expression>> args;
	args.push_back(parseExpression());
	while (static_cast<int>(args.size()) < maxArgs && matchType(TokenType::Comma)) {
		args.push_back(parseExpression());
	}
	expectType(TokenType::RParen, "')' closing this function's argument list");
	if (static_cast<int>(args.size()) < minArgs) {
		error("too few arguments to function call");
	}
	return std::unique_ptr<Expression>(new BuiltInCallExpr(fn, std::move(args)));
}

std::unique_ptr<Expression> Parser::parseAggregate(AggregateKind kind) {
	advance(); // consume the aggregate's keyword
	expectType(TokenType::LParen, "'(' after aggregate name");
	std::unique_ptr<AggregateExpr> agg(new AggregateExpr());
	agg->aggKind = kind;
	agg->distinct = matchKeyword("DISTINCT");
	if (kind == AggregateKind::Count && matchType(TokenType::Star)) {
		agg->star = true;
	} else {
		agg->arg = parseExpression();
		if (kind == AggregateKind::GroupConcat && matchType(TokenType::Semicolon)) {
			expectKeyword("SEPARATOR");
			expectType(TokenType::Equals, "'=' after SEPARATOR");
			Token sep = expectType(TokenType::StringLiteral, "a string literal for SEPARATOR");
			agg->hasSeparator = true;
			agg->separator = sep.text;
		}
	}
	expectType(TokenType::RParen, "')' closing aggregate call");
	return std::unique_ptr<Expression>(agg.release());
}

std::unique_ptr<Expression> Parser::parseBuiltInCallOrAggregateOrExists() {
	if (checkKeyword("COUNT")) {
		return parseAggregate(AggregateKind::Count);
	}
	if (checkKeyword("SUM")) {
		return parseAggregate(AggregateKind::Sum);
	}
	if (checkKeyword("MIN")) {
		return parseAggregate(AggregateKind::Min);
	}
	if (checkKeyword("MAX")) {
		return parseAggregate(AggregateKind::Max);
	}
	if (checkKeyword("AVG")) {
		return parseAggregate(AggregateKind::Avg);
	}
	if (checkKeyword("SAMPLE")) {
		return parseAggregate(AggregateKind::Sample);
	}
	if (checkKeyword("GROUP_CONCAT")) {
		return parseAggregate(AggregateKind::GroupConcat);
	}

	if (checkKeyword("NOT")) {
		advance();
		expectKeyword("EXISTS");
		return std::unique_ptr<Expression>(new ExistsExpr(true, parseGroupGraphPattern()));
	}
	if (checkKeyword("EXISTS")) {
		advance();
		return std::unique_ptr<Expression>(new ExistsExpr(false, parseGroupGraphPattern()));
	}

	if (checkKeyword("BOUND")) {
		advance();
		expectType(TokenType::LParen, "'(' after BOUND");
		std::unique_ptr<Var> v = parseVar();
		expectType(TokenType::RParen, "')' closing BOUND(...)");
		std::vector<std::unique_ptr<Expression>> args;
		args.push_back(std::unique_ptr<Expression>(new VarExpr(std::move(v))));
		return std::unique_ptr<Expression>(new BuiltInCallExpr(BuiltinFunction::Bound, std::move(args)));
	}
	if (checkKeyword("IF")) {
		advance();
		expectType(TokenType::LParen, "'(' after IF");
		std::vector<std::unique_ptr<Expression>> args;
		args.push_back(parseExpression());
		expectType(TokenType::Comma, "',' in IF(...)");
		args.push_back(parseExpression());
		expectType(TokenType::Comma, "',' in IF(...)");
		args.push_back(parseExpression());
		expectType(TokenType::RParen, "')' closing IF(...)");
		return std::unique_ptr<Expression>(new BuiltInCallExpr(BuiltinFunction::If, std::move(args)));
	}
	if (checkKeyword("COALESCE")) {
		advance();
		return std::unique_ptr<Expression>(new BuiltInCallExpr(BuiltinFunction::Coalesce, parseExpressionList()));
	}
	if (checkKeyword("CONCAT")) {
		advance();
		return std::unique_ptr<Expression>(new BuiltInCallExpr(BuiltinFunction::Concat, parseExpressionList()));
	}
	if (checkKeyword("BNODE")) {
		advance();
		std::vector<std::unique_ptr<Expression>> args;
		if (!matchType(TokenType::Nil)) {
			expectType(TokenType::LParen, "'(' after BNODE");
			args.push_back(parseExpression());
			expectType(TokenType::RParen, "')' closing BNODE(...)");
		}
		return std::unique_ptr<Expression>(new BuiltInCallExpr(BuiltinFunction::Bnode, std::move(args)));
	}

	for (std::size_t i = 0; i < sizeof(kZeroArgKeywords) / sizeof(kZeroArgKeywords[0]); ++i) {
		if (checkKeyword(kZeroArgKeywords[i])) {
			advance();
			matchType(TokenType::Nil);
			return std::unique_ptr<Expression>(
			    new BuiltInCallExpr(kZeroArgFns[i], std::vector<std::unique_ptr<Expression>>()));
		}
	}
	for (const auto &e : kOneArgBuiltins) {
		if (checkKeyword(e.kw)) {
			return parseBuiltinArgsN(e.fn, 1, 1);
		}
	}
	for (const auto &e : kTwoArgBuiltins) {
		if (checkKeyword(e.kw)) {
			return parseBuiltinArgsN(e.fn, 2, 2);
		}
	}
	if (checkKeyword("SUBSTR")) {
		return parseBuiltinArgsN(BuiltinFunction::Substr, 2, 3);
	}
	if (checkKeyword("REGEX")) {
		return parseBuiltinArgsN(BuiltinFunction::Regex, 2, 3);
	}
	if (checkKeyword("REPLACE")) {
		return parseBuiltinArgsN(BuiltinFunction::Replace, 3, 4);
	}
	error("expected a built-in function call, aggregate, or EXISTS/NOT EXISTS");
	return nullptr;
}

bool Parser::startsExpression() const {
	if (check(TokenType::Var1) || check(TokenType::Var2) || check(TokenType::LParen) ||
	    check(TokenType::StringLiteral) || check(TokenType::Integer) || check(TokenType::Decimal) ||
	    check(TokenType::Double) || check(TokenType::Iriref) || check(TokenType::PnameNs) ||
	    check(TokenType::PnameLn) || check(TokenType::Bang) || check(TokenType::Plus) || check(TokenType::Minus)) {
		return true;
	}
	if (current_.type != TokenType::Keyword) {
		return false;
	}
	for (const char *kw : kExpressionStartKeywords) {
		if (current_.keyword == kw) {
			return true;
		}
	}
	return false;
}

} // namespace sparql
