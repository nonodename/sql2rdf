#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "sparql-parser/Lexer.h"
#include "sparql-parser/ParseError.h"
#include "sparql-parser/Token.h"
#include "sparql-parser/ast/Expression.h"
#include "sparql-parser/ast/GraphPattern.h"
#include "sparql-parser/ast/Path.h"
#include "sparql-parser/ast/Query.h"
#include "sparql-parser/ast/Term.h"

namespace sparql {

/// Recursive-descent parser for the SPARQL 1.1 Query grammar (§19.8,
/// productions 1-2, 4-28, 52-173; SPARQL Update - productions 3, 29-51 -
/// is out of scope, see the project plan). One token of lookahead
/// (`current_`) is enough throughout, matching grammar note 4.
class Parser {
public:
	Parser() = default;

	/// Read and parse the SPARQL query in the file at `path`.
	/// @throws std::runtime_error if the file cannot be read.
	/// @throws ParseError on a grammar violation.
	std::unique_ptr<ast::Query> parseFile(const std::string &path);

	/// Parse `queryText` directly. `baseIri` seeds relative-IRI resolution
	/// before any BASE clause in the query itself.
	/// @throws ParseError on a grammar violation.
	std::unique_ptr<ast::Query> parseString(const std::string &queryText, const std::string &baseIri = "");

private:
	// ---- token-stream plumbing ----------------------------------------
	void initLexer(const std::string &text);
	const Token &cur() const {
		return current_;
	}
	void advance();
	bool check(TokenType type) const;
	bool checkKeyword(const char *kw) const;
	bool matchType(TokenType type);
	bool matchKeyword(const char *kw);
	Token expectType(TokenType type, const char *what);
	void expectKeyword(const char *kw);
	[[noreturn]] void error(const std::string &message) const;

	// ---- IRIs / prologue -----------------------------------------------
	std::string lookupPrefix(const std::string &prefix) const;
	std::string resolveIriValue(TokenType type, const std::string &lexicalForm) const;
	std::unique_ptr<ast::Iri> parseIri();
	std::unique_ptr<ast::Var> parseVar();
	std::unique_ptr<ast::Iri> makeIri(const std::string &absoluteValue, const std::string &lexicalForm);
	static std::unique_ptr<ast::Iri> cloneIri(const ast::Iri &iri);
	static std::unique_ptr<ast::Term> cloneTerm(const ast::Term &term);
	static std::unique_ptr<ast::PropertyPathExpr> clonePath(const ast::PropertyPathExpr &path);

	void parsePrologue(ast::Prologue &prologue);
	void parseBaseDecl(ast::Prologue &prologue);
	void parsePrefixDecl(ast::Prologue &prologue);

	// ---- top-level query forms -----------------------------------------
	std::unique_ptr<ast::Query> parseQuery();
	void parseSelectClause(ast::Query &query);
	void parseConstructQuery(ast::Query &query);
	void parseDescribeQuery(ast::Query &query);
	void parseAskQuery(ast::Query &query);
	void parseDatasetClauses(std::vector<ast::DatasetClause> &clauses);
	std::unique_ptr<ast::GroupGraphPattern> parseWhereClause();
	void parseSolutionModifier(ast::SolutionModifier &modifier);
	void parseGroupClause(ast::SolutionModifier &modifier);
	ast::GroupCondition parseGroupCondition();
	void parseHavingClause(ast::SolutionModifier &modifier);
	void parseOrderClause(ast::SolutionModifier &modifier);
	ast::OrderCondition parseOrderCondition();
	void parseLimitOffsetClauses(ast::SolutionModifier &modifier);
	std::unique_ptr<ast::InlineData> parseValuesClauseOptional();
	std::unique_ptr<ast::Query> parseSubSelect();

	std::vector<ast::TriplePattern> parseConstructTemplate();
	void parseConstructTriples(std::vector<ast::TriplePattern> &out);

	// ---- graph patterns (ParserGraphPatterns.cpp) ----------------------
	std::unique_ptr<ast::GroupGraphPattern> parseGroupGraphPattern();
	void parseGroupGraphPatternSub(ast::GroupGraphPattern &group);
	bool startsGraphPatternNotTriples() const;
	std::unique_ptr<ast::GroupElement> parseGraphPatternNotTriples();
	std::unique_ptr<ast::GroupElement> parseGroupOrUnionGraphPattern();
	std::unique_ptr<ast::GroupElement> parseOptionalGraphPattern();
	std::unique_ptr<ast::GroupElement> parseMinusGraphPattern();
	std::unique_ptr<ast::GroupElement> parseGraphGraphPattern();
	std::unique_ptr<ast::GroupElement> parseServiceGraphPattern();
	std::unique_ptr<ast::GroupElement> parseFilterElement();
	std::unique_ptr<ast::GroupElement> parseBindElement();
	std::unique_ptr<ast::GroupElement> parseInlineDataElement();
	std::unique_ptr<ast::InlineData> parseInlineDataOneVar();
	std::unique_ptr<ast::InlineData> parseInlineDataFull();

	bool startsVerb(bool pathMode) const;
	bool startsTriplesNode() const;
	bool startsTriple() const;
	void parseTriplesBlock(ast::BasicGraphPattern &bgp);
	void parseTriplesSameSubject(std::vector<ast::TriplePattern> &out, bool pathMode);
	void parsePropertyListNotEmpty(const ast::Term &subject, std::vector<ast::TriplePattern> &out, bool pathMode);
	void parseObjectList(const ast::Term &subject, const ast::PropertyPathExpr &predicate,
	                     std::vector<ast::TriplePattern> &out, bool pathMode);
	std::unique_ptr<ast::Term> parseDataBlockValue();
	std::unique_ptr<ast::PropertyPathExpr> parseVerbSimple();
	/// Dispatches to a bare-variable predicate, parsePath() (path mode), or
	/// parseVerbSimple() (non-path mode) - a variable predicate is valid in
	/// both PropertyListNotEmpty (rule 78's Verb) and PropertyListPathNotEmpty
	/// (rule 83's VerbSimple alternative), so it's checked first either way.
	std::unique_ptr<ast::PropertyPathExpr> parsePredicate(bool pathMode);
	std::unique_ptr<ast::Term> parseVarOrTerm();
	std::unique_ptr<ast::Term> parseGraphTerm();
	std::unique_ptr<ast::Term> parseGraphNode(std::vector<ast::TriplePattern> &out, bool pathMode);
	std::unique_ptr<ast::Term> parseTriplesNode(std::vector<ast::TriplePattern> &out, bool pathMode);
	std::unique_ptr<ast::Term> parseCollection(std::vector<ast::TriplePattern> &out, bool pathMode);
	std::unique_ptr<ast::Term> parseBlankNodePropertyList(std::vector<ast::TriplePattern> &out, bool pathMode);
	std::unique_ptr<ast::RdfLiteral> parseRdfLiteral();
	std::unique_ptr<ast::RdfLiteral> parseNumericLiteral();
	std::unique_ptr<ast::RdfLiteral> parseBooleanLiteral();
	std::string freshBlankNodeLabel();

	// ---- property paths (ParserPaths.cpp) ------------------------------
	std::unique_ptr<ast::PropertyPathExpr> parsePath();
	std::unique_ptr<ast::PropertyPathExpr> parsePathAlternative();
	std::unique_ptr<ast::PropertyPathExpr> parsePathSequence();
	std::unique_ptr<ast::PropertyPathExpr> parsePathEltOrInverse();
	std::unique_ptr<ast::PropertyPathExpr> parsePathElt();
	std::unique_ptr<ast::PropertyPathExpr> parsePathPrimary();
	std::unique_ptr<ast::PropertyPathExpr> parsePathNegatedPropertySet();

	// ---- expressions (ParserExpressions.cpp) ---------------------------
	std::unique_ptr<ast::Expression> parseExpression();
	std::unique_ptr<ast::Expression> parseConditionalOrExpression();
	std::unique_ptr<ast::Expression> parseConditionalAndExpression();
	std::unique_ptr<ast::Expression> parseValueLogical();
	std::unique_ptr<ast::Expression> parseRelationalExpression();
	std::unique_ptr<ast::Expression> parseNumericExpression();
	std::unique_ptr<ast::Expression> parseAdditiveExpression();
	std::unique_ptr<ast::Expression> parseMultiplicativeExpression();
	std::unique_ptr<ast::Expression> continueMultiplicativeFrom(std::unique_ptr<ast::Expression> left);
	std::unique_ptr<ast::Expression> parseUnaryExpression();
	std::unique_ptr<ast::Expression> parsePrimaryExpression();
	std::unique_ptr<ast::Expression> parseBuiltInCallOrAggregateOrExists();
	std::unique_ptr<ast::Expression> parseAggregate(ast::AggregateKind kind);
	std::unique_ptr<ast::Expression> parseBuiltinArgsN(ast::BuiltinFunction fn, int minArgs, int maxArgs);
	std::unique_ptr<ast::Expression> parseIriOrFunctionCall();
	std::vector<std::unique_ptr<ast::Expression>> parseExpressionList();
	std::vector<std::unique_ptr<ast::Expression>> parseArgList(bool *distinctOut);
	/// True if the current token can start a primary expression (used to
	/// decide whether a `+`/HAVING/GROUP BY/ORDER BY repetition continues).
	bool startsExpression() const;

	std::string source_;
	std::unique_ptr<Lexer> lexer_;
	Token current_;
	ast::Prologue *prologue_ = nullptr; // points at the Prologue of the query currently being built
	uint64_t blankNodeCounter_ = 0;
};

} // namespace sparql
