/**
 * Structural tests for the two "final flattening" moves that remove pure
 * rename/reproject derived-table layers from generated SQL:
 *
 *  - Optimizer.cpp's foldBindIntoSpj (part of the `flatten` pass): a BIND
 *    whose child has flattened down to a single SpjRelation is folded into
 *    that block's own SELECT list instead of wrapping it in a
 *    "SELECT *, (expr) AS v FROM (<spj>) AS alias" layer.
 *  - Translator.cpp's elideProjection: a query-level (or `{ SELECT ... }`
 *    subquery) projection that does nothing but rename the optimized tree's
 *    own columns back to themselves is spliced through directly instead of
 *    wrapped.
 *
 * Uses the full translateQuery pipeline (fold -> optimize -> render) against
 * real fixtures, mirroring test_sparql2sql_filter_pushdown.cpp's style.
 */

#include <catch2/catch_test_macros.hpp>

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

std::string translateFixture(const char *rqFile, const char *mappingFile) {
	Parser parser;
	auto q = parser.parseFile(std::string(SOURCE_SPARQL2SQL_DIR) + rqFile);
	R2RMLParser mappingParser;
	R2RMLMapping mapping = mappingParser.parse(std::string(SOURCE_R2RML_DIR) + mappingFile);
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

} // namespace

TEST_CASE("flatten: a BIND over a single-candidate predicate is folded into the SPJ block", "[sparql2sql]") {
	std::string sql = translateFixture("sparql2sql_typed_arith.rq", "sparql2sql_terms.ttl");
	// No "SELECT *, ... FROM (" wrapper: the BIND-computed column sits in the
	// same SELECT list as the base scan's own columns.
	CHECK(sql.find("SELECT *") == std::string::npos);
	CHECK(sql.find("FROM \"MEASURES\"") != std::string::npos);
}

TEST_CASE("flatten: a chain of two BINDs over the same SPJ block folds into one layer with no wrapper",
          "[sparql2sql]") {
	std::string sql = translateFixture("sparql2sql_bind_chain.rq", "sparql2sql_terms.ttl");
	// Both BIND columns (?b, ?c) end up projected directly off "MEASURES" with
	// no intervening derived table at all - not even the outermost
	// query-level projection, since ?m ?a ?b ?c is exactly the block's own
	// column order.
	CHECK(sql.find("SELECT *") == std::string::npos);
	CHECK(countOccurrences(sql, "FROM \"MEASURES\"") == 1);
	// No derived-table layer at all - not even the outer query-level wrap.
	CHECK(sql.find("FROM (") == std::string::npos);
}

TEST_CASE("flatten: a BIND over an OPTIONAL join is not folded (its child isn't a flattened SPJ block)",
          "[sparql2sql]") {
	std::string sql = translateFixture("emp_dept_optional_bind.rq", "example_emp_dept.ttl");
	REQUIRE(sql.find("LEFT OUTER JOIN") != std::string::npos);
	// The BIND still wraps its join child in a passthrough "SELECT *" layer -
	// correctly conservative, since the join's projection isn't an SpjRelation
	// this pass knows how to rewrite in place.
	CHECK(sql.find("SELECT *,") != std::string::npos);
}

TEST_CASE("flatten: eliding the outer identity projection preserves the query's declared column order",
          "[sparql2sql]") {
	std::string sql = translateFixture("sparql2sql_bind_chain.rq", "sparql2sql_terms.ttl");
	// ?m ?a ?b ?c, in that order - not just present, but in the order the
	// SELECT clause asked for, even though the wrapper that would normally
	// guarantee that order was elided.
	std::size_t posM = sql.find("AS \"v_m\"");
	std::size_t posA = sql.find("AS \"v_a\"");
	std::size_t posB = sql.find("AS \"v_b\"");
	std::size_t posC = sql.find("AS \"v_c\"");
	REQUIRE(posM != std::string::npos);
	REQUIRE(posA != std::string::npos);
	REQUIRE(posB != std::string::npos);
	REQUIRE(posC != std::string::npos);
	CHECK(posM < posA);
	CHECK(posA < posB);
	CHECK(posB < posC);
}

TEST_CASE("flatten: the outer identity projection is kept when SELECT reorders the block's own columns",
          "[sparql2sql]") {
	// ?a ?m instead of the block's own ?m, ?a order: not an identity
	// permutation, so eliding the wrapper would silently return the columns
	// in the wrong order. The wrapper must stay to actually perform the
	// reorder.
	std::string sql = translateFixture("sparql2sql_bind_chain_reordered.rq", "sparql2sql_terms.ttl");
	REQUIRE(sql.find("FROM (") != std::string::npos);
	std::size_t posA = sql.find("AS \"v_a\"");
	std::size_t posM = sql.find("AS \"v_m\"");
	REQUIRE(posA != std::string::npos);
	REQUIRE(posM != std::string::npos);
	// The outermost (first) AS "v_a"/"v_m" pair reflects the query's own
	// requested order, ?a before ?m.
	CHECK(posA < posM);
}
