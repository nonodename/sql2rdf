/**
 * Tests for the SPARQL algebra fold over GroupGraphPattern::elements: the
 * combinators (innerJoin/leftOuterJoin/antiJoin/unionAll) tested directly
 * with hand-built relations, plus integration-level tests driving fold()
 * through parsed .rq/.ttl fixtures for AND/OPTIONAL/UNION/MINUS/VALUES/
 * SubSelect/GRAPH.
 */

#include <catch2/catch_test_macros.hpp>

#include <string>

#ifndef SOURCE_SPARQL2SQL_DIR
#define SOURCE_SPARQL2SQL_DIR ""
#endif
#ifndef SOURCE_R2RML_DIR
#define SOURCE_R2RML_DIR ""
#endif

#include "r2rml/R2RMLMapping.h"
#include "r2rml/R2RMLParser.h"
#include "sparql-parser/Parser.h"
#include "sparql2sql/DuckDbDialect.h"
#include "sparql2sql/PatternFolder.h"
#include "sparql2sql/TranslationError.h"
#include "sparql2sql/Translator.h"

using r2rml::R2RMLMapping;
using r2rml::R2RMLParser;
using sparql::Parser;
using sparql2sql::antiJoin;
using sparql2sql::DuckDbDialect;
using sparql2sql::fold;
using sparql2sql::identityRelation;
using sparql2sql::innerJoin;
using sparql2sql::leftOuterJoin;
using sparql2sql::TranslatedPattern;
using sparql2sql::translateQuery;
using sparql2sql::TranslationContext;
using sparql2sql::TranslationError;
using sparql2sql::unionAll;

namespace {

TranslatedPattern makePattern(const std::string &sql, std::initializer_list<std::string> bound,
                              std::initializer_list<std::string> optional = {}) {
	TranslatedPattern p;
	p.sql = sql;
	for (const auto &v : bound) {
		p.boundVars.insert(v);
	}
	for (const auto &v : optional) {
		p.optionalVars.insert(v);
	}
	return p;
}

} // namespace

// --- Direct combinator unit tests (no mapping/parsing needed) ---

TEST_CASE("innerJoin: identity relation is elided on either side") {
	R2RMLMapping mapping;
	DuckDbDialect dialect;
	TranslationContext ctx(mapping, dialect);

	TranslatedPattern p = makePattern("SELECT 1", {"x"});
	TranslatedPattern id = identityRelation(ctx);

	CHECK(innerJoin(id, p, ctx).sql == p.sql);
	CHECK(innerJoin(p, id, ctx).sql == p.sql);
}

TEST_CASE("innerJoin: a shared variable bound on either side stays bound in the result") {
	R2RMLMapping mapping;
	DuckDbDialect dialect;
	TranslationContext ctx(mapping, dialect);

	TranslatedPattern left = makePattern("SELECT ...", {"x", "y"});
	TranslatedPattern right = makePattern("SELECT ...", {}, {"x", "z"}); // x optional on the right

	TranslatedPattern result = innerJoin(left, right, ctx);
	CHECK(result.boundVars.count("x") == 1); // left guarantees it
	CHECK(result.boundVars.count("y") == 1);
	CHECK(result.optionalVars.count("z") == 1);
	CHECK(result.sql.find("INNER JOIN") != std::string::npos);
	// x is optional on the right, so the join must use the null-safe form.
	CHECK(result.sql.find("COALESCE") != std::string::npos);
	CHECK(result.sql.find("IS NULL") != std::string::npos);
}

TEST_CASE("innerJoin: no COALESCE/null-safety emitted when neither side is optional for the shared var") {
	R2RMLMapping mapping;
	DuckDbDialect dialect;
	TranslationContext ctx(mapping, dialect);

	TranslatedPattern left = makePattern("SELECT ...", {"x"});
	TranslatedPattern right = makePattern("SELECT ...", {"x"});

	TranslatedPattern result = innerJoin(left, right, ctx);
	CHECK(result.sql.find("COALESCE") == std::string::npos);
}

TEST_CASE("leftOuterJoin: only the left side's own guarantee keeps a shared var definitely bound") {
	R2RMLMapping mapping;
	DuckDbDialect dialect;
	TranslationContext ctx(mapping, dialect);

	TranslatedPattern left = makePattern("SELECT ...", {"x"});
	TranslatedPattern right = makePattern("SELECT ...", {"x", "d"}); // x AND d unconditionally bound on the right

	TranslatedPattern result = leftOuterJoin(left, right, ctx);
	CHECK(result.sql.find("LEFT OUTER JOIN") != std::string::npos);
	CHECK(result.boundVars.count("x") == 1); // left's own guarantee
	// d is unique to the right side; an unmatched left row would NULL it,
	// so it must be optional in the result regardless of the right side's
	// own (pre-join) bound-ness.
	CHECK(result.optionalVars.count("d") == 1);
	CHECK(result.boundVars.count("d") == 0);
}

TEST_CASE("antiJoin: zero shared variables is a spec-mandated no-op (no SQL change at all)") {
	R2RMLMapping mapping;
	DuckDbDialect dialect;
	TranslationContext ctx(mapping, dialect);

	TranslatedPattern left = makePattern("SELECT LEFT_MARKER", {"x"});
	TranslatedPattern right = makePattern("SELECT RIGHT_MARKER", {"y"});

	TranslatedPattern result = antiJoin(left, right, ctx);
	CHECK(result.sql == left.sql);
	CHECK(result.sql.find("NOT EXISTS") == std::string::npos);
}

TEST_CASE("antiJoin: a shared variable produces a NOT EXISTS anti-join") {
	R2RMLMapping mapping;
	DuckDbDialect dialect;
	TranslationContext ctx(mapping, dialect);

	TranslatedPattern left = makePattern("SELECT ...", {"x"});
	TranslatedPattern right = makePattern("SELECT ...", {"x"});

	TranslatedPattern result = antiJoin(left, right, ctx);
	CHECK(result.sql.find("NOT EXISTS") != std::string::npos);
	CHECK(result.boundVars == left.boundVars);
}

TEST_CASE("unionAll: a variable bound in every branch stays bound; others become optional") {
	R2RMLMapping mapping;
	DuckDbDialect dialect;
	TranslationContext ctx(mapping, dialect);

	TranslatedPattern b1 = makePattern("SELECT ...", {"e", "n"});
	TranslatedPattern b2 = makePattern("SELECT ...", {"e", "d"});

	TranslatedPattern result = unionAll({b1, b2}, ctx);
	CHECK(result.boundVars.count("e") == 1);
	CHECK(result.optionalVars.count("n") == 1);
	CHECK(result.optionalVars.count("d") == 1);
	CHECK(result.sql.find("UNION ALL BY NAME") != std::string::npos);
}

// --- Integration-level tests through the fold, using real fixtures ---

TEST_CASE("fold: AND joins consecutive triples on shared variables") {
	Parser parser;
	auto q = parser.parseFile(SOURCE_SPARQL2SQL_DIR "sparql2sql_constant_pom.rq");
	R2RMLParser mappingParser;
	R2RMLMapping mapping = mappingParser.parse(SOURCE_R2RML_DIR "sparql2sql_constant_pom.ttl");
	REQUIRE(mapping.isValid());

	DuckDbDialect dialect;
	TranslationContext ctx(mapping, dialect);
	TranslatedPattern result = fold(*q->where, ctx);

	CHECK(result.boundVars.count("p") == 1);
	CHECK(result.boundVars.count("n") == 1);
	CHECK(result.sql.find("INNER JOIN") != std::string::npos);
}

TEST_CASE("fold: OPTIONAL produces a left outer join with the department var optional") {
	Parser parser;
	auto q = parser.parseFile(SOURCE_SPARQL2SQL_DIR "emp_dept_optional.rq");
	R2RMLParser mappingParser;
	R2RMLMapping mapping = mappingParser.parse(SOURCE_R2RML_DIR "example_emp_dept.ttl");
	REQUIRE(mapping.isValid());

	DuckDbDialect dialect;
	TranslationContext ctx(mapping, dialect);
	TranslatedPattern result = fold(*q->where, ctx);

	CHECK(result.sql.find("LEFT OUTER JOIN") != std::string::npos);
	CHECK(result.boundVars.count("e") == 1);
	CHECK(result.boundVars.count("n") == 1);
	CHECK(result.optionalVars.count("d") == 1);
}

TEST_CASE("fold: UNION with mismatched branch schemas leaves the unshared vars optional") {
	Parser parser;
	auto q = parser.parseFile(SOURCE_SPARQL2SQL_DIR "emp_dept_union_mismatched.rq");
	R2RMLParser mappingParser;
	R2RMLMapping mapping = mappingParser.parse(SOURCE_R2RML_DIR "example_emp_dept.ttl");
	REQUIRE(mapping.isValid());

	DuckDbDialect dialect;
	TranslationContext ctx(mapping, dialect);
	TranslatedPattern result = fold(*q->where, ctx);

	CHECK(result.sql.find("UNION ALL BY NAME") != std::string::npos);
	CHECK(result.boundVars.count("e") == 1);
	CHECK(result.optionalVars.count("n") == 1);
	CHECK(result.optionalVars.count("d") == 1);
}

TEST_CASE("fold: MINUS with shared variables emits NOT EXISTS") {
	Parser parser;
	auto q = parser.parseFile(SOURCE_SPARQL2SQL_DIR "emp_dept_minus.rq");
	R2RMLParser mappingParser;
	R2RMLMapping mapping = mappingParser.parse(SOURCE_R2RML_DIR "example_emp_dept.ttl");
	REQUIRE(mapping.isValid());

	DuckDbDialect dialect;
	TranslationContext ctx(mapping, dialect);
	TranslatedPattern result = fold(*q->where, ctx);
	CHECK(result.sql.find("NOT EXISTS") != std::string::npos);
}

TEST_CASE("fold: MINUS with zero shared variables is a no-op, no NOT EXISTS emitted") {
	Parser parser;
	auto q = parser.parseFile(SOURCE_SPARQL2SQL_DIR "emp_dept_minus_novars.rq");
	R2RMLParser mappingParser;
	R2RMLMapping mapping = mappingParser.parse(SOURCE_R2RML_DIR "example_emp_dept.ttl");
	REQUIRE(mapping.isValid());

	DuckDbDialect dialect;
	TranslationContext ctx(mapping, dialect);
	TranslatedPattern result = fold(*q->where, ctx);
	CHECK(result.sql.find("NOT EXISTS") == std::string::npos);
}

TEST_CASE("fold: VALUES joins the inline data table on the shared variable") {
	Parser parser;
	auto q = parser.parseFile(SOURCE_SPARQL2SQL_DIR "emp_dept_values.rq");
	R2RMLParser mappingParser;
	R2RMLMapping mapping = mappingParser.parse(SOURCE_R2RML_DIR "example_emp_dept.ttl");
	REQUIRE(mapping.isValid());

	DuckDbDialect dialect;
	TranslationContext ctx(mapping, dialect);
	TranslatedPattern result = fold(*q->where, ctx);
	CHECK(result.boundVars.count("e") == 1);
	CHECK(result.boundVars.count("n") == 1);
	CHECK(result.sql.find("'SMITH'") != std::string::npos);
	CHECK(result.sql.find("'JONES'") != std::string::npos);
}

TEST_CASE("fold: a subquery element is joined in with its variables treated as optional") {
	Parser parser;
	auto q = parser.parseFile(SOURCE_SPARQL2SQL_DIR "emp_dept_subquery.rq");
	R2RMLParser mappingParser;
	R2RMLMapping mapping = mappingParser.parse(SOURCE_R2RML_DIR "example_emp_dept.ttl");
	REQUIRE(mapping.isValid());

	DuckDbDialect dialect;
	TranslationContext ctx(mapping, dialect);
	TranslatedPattern result = fold(*q->where, ctx);
	// ?e is independently guaranteed by the outer ex:name triple pattern,
	// so it stays bound in spite of the subquery's own conservative
	// optional-marking.
	CHECK(result.boundVars.count("e") == 1);
	CHECK(result.sql.find("INNER JOIN") != std::string::npos);
}

TEST_CASE("fold: GRAPH throws a clear TranslationError (no named-graph support)") {
	Parser parser;
	auto q = parser.parseFile(SOURCE_SPARQL2SQL_DIR "unsupported_graph.rq");
	R2RMLParser mappingParser;
	R2RMLMapping mapping = mappingParser.parse(SOURCE_R2RML_DIR "example_emp_dept.ttl");
	REQUIRE(mapping.isValid());

	DuckDbDialect dialect;
	TranslationContext ctx(mapping, dialect);
	CHECK_THROWS_AS(fold(*q->where, ctx), TranslationError);
}

TEST_CASE("translateQuery: CONSTRUCT is rejected with a clear TranslationError") {
	Parser parser;
	auto q = parser.parseFile(SOURCE_SPARQL2SQL_DIR "unsupported_construct.rq");
	R2RMLParser mappingParser;
	R2RMLMapping mapping = mappingParser.parse(SOURCE_R2RML_DIR "example_emp_dept.ttl");
	REQUIRE(mapping.isValid());

	DuckDbDialect dialect;
	CHECK_THROWS_AS(translateQuery(*q, mapping, dialect), TranslationError);
}
