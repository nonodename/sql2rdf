/**
 * Structural tests for FILTER pushdown in the optimizer (Optimizer.cpp's
 * pushFilters pass). Pushdown is semantics-preserving, so these assert that a
 * filter was re-parented onto the intended sub-relation (or, for boundaries,
 * that it was NOT); the DuckDB execution tests (tests/duckdb/) are the
 * correctness oracle. Uses the full translateQuery pipeline (fold -> optimize
 * -> render) against real fixtures, all mapped by example_emp_dept.ttl.
 *
 * "How many times was this predicate instantiated" is the quantity these tests
 * actually care about: >1 means the predicate was replicated across union arms
 * / join sides (pushed down), ==1 with an outer position means it stayed at a
 * boundary. It is counted via countFilterInstances(), which normalises by how
 * many times one instantiation mentions its constant literal - deliberately
 * *measured* rather than hard-coded, so these tests measure pushdown and not
 * the incidental SQL shape of a comparison. See literalsPerFilter().
 */

#include <catch2/catch.hpp>

#include <cstddef>
#include <string>

#ifndef SOURCE_R2RML_DIR
#define SOURCE_R2RML_DIR ""
#endif
#ifndef SOURCE_SPARQL2SQL_DIR
#define SOURCE_SPARQL2SQL_DIR ""
#endif

#include "r2rml/R2RMLMapping.h"
#include "r2rml/R2RMLParser.h"
#include "sparql-parser/Parser.h"
#include "sparql2sql/DuckDbDialect.h"
#include "sparql2sql/Translator.h"

using r2rml::R2RMLMapping;
using r2rml::R2RMLParser;
using sparql::Parser;
using sparql2sql::DuckDbDialect;
using sparql2sql::translateQuery;

namespace {

std::string translateFixture(const char *rqFile) {
	Parser parser;
	auto q = parser.parseFile(std::string(SOURCE_SPARQL2SQL_DIR) + rqFile);
	R2RMLParser mappingParser;
	R2RMLMapping mapping = mappingParser.parse(SOURCE_R2RML_DIR "example_emp_dept.ttl");
	DuckDbDialect dialect;
	return translateQuery(*q, mapping, dialect);
}

std::size_t countOccurrences(const std::string &haystack, const std::string &needle) {
	std::size_t count = 0;
	std::size_t pos = 0;
	while ((pos = haystack.find(needle, pos)) != std::string::npos) {
		++count;
		pos += needle.size();
	}
	return count;
}

// How many times one instantiation of a `?x = "<literal>"` predicate mentions
// that literal. NOT a constant: it depends on how the comparison is spelled.
// The numeric-aware form mentions each operand three times (twice in the CASE
// guard and result, once in the string fallback); a comparison the translator
// can type statically mentions it once. So it is measured from a fixture whose
// predicate is known to be instantiated exactly once - see the
// "folded into its WHERE" case below, which pins that independently.
std::size_t literalsPerFilter() {
	static const std::size_t perFilter = countOccurrences(translateFixture("emp_dept_filter_folded.rq"), "'NEW YORK'");
	return perFilter;
}

// How many times a filter predicate carrying `literal` was instantiated in
// `sql`. The divisibility check is the point: if a change makes one predicate's
// SQL shape differ from the calibration fixture's, this fails loudly instead of
// silently reporting a wrong instance count.
std::size_t countFilterInstances(const std::string &sql, const std::string &literal) {
	const std::size_t perFilter = literalsPerFilter();
	REQUIRE(perFilter > 0);
	const std::size_t total = countOccurrences(sql, literal);
	REQUIRE(total % perFilter == 0);
	return total / perFilter;
}

} // namespace

TEST_CASE("pushdown: FILTER over UNION is distributed into every arm", "[sparql2sql]") {
	std::string sql = translateFixture("emp_dept_filter_union.rq");
	// One instantiation per union arm (>1) rather than a single outer wrap.
	CHECK(countFilterInstances(sql, "'NEW YORK'") >= 2);
	CHECK(sql.find("UNION ALL BY NAME") != std::string::npos);
	// No top-level wrapping filter above the union: the union feeds the
	// projection directly.
	CHECK(sql.find(") AS ") != std::string::npos);
}

TEST_CASE("pushdown: FILTER over an inner join lands on the referenced side only", "[sparql2sql]") {
	std::string sql = translateFixture("emp_dept_filter_join.rq");
	REQUIRE(sql.find("INNER JOIN") != std::string::npos);
	REQUIRE(sql.find("'NEW YORK'") != std::string::npos);
	// The ?loc filter references only the location side, so it is pushed there
	// - it appears before the INNER JOIN keyword, not above the whole join.
	CHECK(sql.find("'NEW YORK'") < sql.find("INNER JOIN"));
	// It is applied exactly once (on one side), not duplicated onto the union.
	CHECK(countFilterInstances(sql, "'NEW YORK'") == 1);
}

TEST_CASE("pushdown: FILTER on an OPTIONAL var is NOT pushed below the left outer join", "[sparql2sql]") {
	std::string sql = translateFixture("emp_dept_filter_optional.rq");
	REQUIRE(sql.find("LEFT OUTER JOIN") != std::string::npos);
	REQUIRE(sql.find("'NEW YORK'") != std::string::npos);
	// The filter stays above the outer join (boundary): it appears after the
	// LEFT OUTER JOIN, never pushed down onto the optional side.
	CHECK(sql.find("LEFT OUTER JOIN") < sql.find("'NEW YORK'"));
	CHECK(countFilterInstances(sql, "'NEW YORK'") == 1);
}

TEST_CASE("pushdown: FILTER on a var nullable via OPTIONAL on one join side but required on the other is not "
          "pushed onto the nullable side",
          "[sparql2sql]") {
	std::string sql = translateFixture("emp_dept_filter_optional_join.rq");
	REQUIRE(sql.find("LEFT OUTER JOIN") != std::string::npos);
	REQUIRE(sql.find("INNER JOIN") != std::string::npos);
	REQUIRE(sql.find("'NEW YORK'") != std::string::npos);
	// ?loc is a nullSafe key of the outer INNER join (nullable coming out of
	// the OPTIONAL, required on the ?d2 side). The filter must not land on the
	// OPTIONAL side - it is folded into the required (?d2) side instead, which
	// renders after both join keywords.
	CHECK(sql.find("LEFT OUTER JOIN") < sql.find("INNER JOIN"));
	CHECK(sql.find("INNER JOIN") < sql.find("'NEW YORK'"));
	// Folded once (into the ?d2 block's WHERE), not duplicated, not dropped.
	CHECK(countFilterInstances(sql, "'NEW YORK'") == 1);
	CHECK(sql.find("SELECT * FROM (") == std::string::npos);
}

TEST_CASE("pushdown: an EXISTS-bearing FILTER is not distributed into union arms", "[sparql2sql]") {
	std::string sql = translateFixture("emp_dept_filter_exists.rq");
	REQUIRE(sql.find("EXISTS (") != std::string::npos);
	CHECK(sql.find("NOT EXISTS") == std::string::npos);
	// EXISTS conjuncts are a hard boundary: kept once, above the union.
	CHECK(countOccurrences(sql, "EXISTS (") == 1);
	CHECK(sql.find("UNION ALL BY NAME") < sql.find("EXISTS ("));
}

TEST_CASE("pushdown: a FILTER reaching an SPJ block is folded into its WHERE", "[sparql2sql]") {
	std::string sql = translateFixture("emp_dept_filter_folded.rq");
	REQUIRE(sql.find("'NEW YORK'") != std::string::npos);
	// A FilterNode always renders as `SELECT * FROM (<child>) AS tN WHERE ...`,
	// so its absence means the predicate went straight into the block's own
	// WHERE list - one fewer derived table, and the predicate now sits next to
	// the base scan rather than above a subquery.
	CHECK(sql.find("SELECT * FROM (") == std::string::npos);
	// Folded predicates reference the block's own column expressions, not a
	// wrapper alias's projected v_ columns.
	//
	// This exact spelling holds because ?loc is a bare rr:column with no
	// rr:datatype and no TypeCatalog is supplied, so the translator cannot type
	// the comparison and falls back to the lexical form. Supplying a catalog
	// (as the CLI does) would legitimately change it. This case is also
	// literalsPerFilter()'s calibration fixture - exactly one instantiation.
	CHECK(sql.find("(CAST(t6.\"LOC\" AS VARCHAR)) = 'NEW YORK'") != std::string::npos);
	CHECK(countFilterInstances(sql, "'NEW YORK'") == 1);
}

TEST_CASE("pushdown: folding a FILTER keeps its block mergeable by flattening", "[sparql2sql]") {
	std::string sql = translateFixture("emp_dept_filter_keeps_mergeable.rq");
	REQUIRE(sql.find("'NEW YORK'") != std::string::npos);
	// This is the point of folding rather than re-parenting a FilterNode: the
	// filtered pattern still fuses with its inner-join partner into a single
	// multi-source block. Left as a FilterNode it would be a flattening
	// boundary, splitting this into two derived tables joined by a subquery.
	CHECK(sql.find("SELECT * FROM (") == std::string::npos);
	CHECK(sql.find("INNER JOIN") == std::string::npos);
	CHECK(countOccurrences(sql, "SELECT DISTINCT") == 1);
}

TEST_CASE("pushdown: FILTER over MINUS is pushed onto the anti-join left side", "[sparql2sql]") {
	std::string sql = translateFixture("emp_dept_filter_minus.rq");
	REQUIRE(sql.find("NOT EXISTS") != std::string::npos);
	REQUIRE(sql.find("'SMITH'") != std::string::npos);
	// The ?n filter references only the kept (left) side of MINUS, so it is
	// pushed below the anti-join - before the NOT EXISTS, not above it. The
	// left side is itself a UNION (ex:name maps two tables), so the filter is
	// then distributed into both of its arms.
	CHECK(sql.find("'SMITH'") < sql.find("NOT EXISTS"));
	CHECK(countFilterInstances(sql, "'SMITH'") >= 2);
}
