/**
 * Integration-level tests for two dedup-elimination optimizations in
 * Optimizer.cpp:
 *
 *  - relaxUnionDedupIfDisjoint: a candidate union's own cross-arm dedup
 *    (UNION BY NAME) is unnecessary, and downgrades to UNION ALL BY NAME,
 *    when every pair of arms is provably disjoint on some rr:template
 *    column - but must stay a dedup when the arms are *not* provably
 *    disjoint (sparql2sql_dedup_union.ttl's AMap/BMap candidates share the
 *    exact same subject template).
 *  - stripAntiJoinRightDedup: a MINUS's subtracted side is only ever probed
 *    with NOT EXISTS, so its own dedup (both per-arm DISTINCT and the
 *    union's cross-arm dedup) is always redundant - even when the enclosing
 *    query is not itself DISTINCT and the arms are not template-disjoint.
 */

#include <catch2/catch.hpp>

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

std::string translateFixture(const char *ttlFile, const char *rqFile) {
	Parser parser;
	auto q = parser.parseFile(std::string(SOURCE_SPARQL2SQL_DIR) + rqFile);
	R2RMLParser mappingParser;
	R2RMLMapping mapping = mappingParser.parse(std::string(SOURCE_R2RML_DIR) + ttlFile);
	DuckDbDialect dialect;
	return translateQuery(*q, mapping, dialect);
}

} // namespace

TEST_CASE("translateQuery: a candidate union whose arms are not template-disjoint keeps its cross-arm dedup") {
	// AMap/BMap both use "http://ex.org/thing/{ID}" for ex:val, so the two
	// candidates can agree on a row - the union must stay a dedup (not ALL),
	// and each arm keeps its own per-candidate DISTINCT.
	std::string sql = translateFixture("sparql2sql_dedup_union.ttl", "dedup_union_ambiguous.rq");
	CHECK(sql.find("UNION BY NAME") != std::string::npos);
	CHECK(sql.find("UNION ALL BY NAME") == std::string::npos);
	CHECK(sql.find("SELECT DISTINCT") != std::string::npos);
}

TEST_CASE("translateQuery: MINUS's subtracted side drops all dedup even though the outer query isn't DISTINCT") {
	std::string sql = translateFixture("sparql2sql_dedup_union.ttl", "dedup_union_minus.rq");
	std::size_t notExists = sql.find("NOT EXISTS");
	REQUIRE(notExists != std::string::npos);

	// Before NOT EXISTS: the kept ("?s ex:val ?v") side's candidates are the
	// same non-disjoint AMap/BMap union, so it still dedups normally.
	std::string before = sql.substr(0, notExists);
	CHECK(before.find("UNION BY NAME") != std::string::npos);
	CHECK(before.find("UNION ALL BY NAME") == std::string::npos);
	CHECK(before.find("SELECT DISTINCT") != std::string::npos);

	// From NOT EXISTS onward: the subtracted side is a pure existence probe,
	// so every dedup - the union's own and each arm's DISTINCT - is gone.
	std::string after = sql.substr(notExists);
	CHECK(after.find("UNION ALL BY NAME") != std::string::npos);
	CHECK(after.find("UNION BY NAME") == std::string::npos);
	CHECK(after.find("DISTINCT") == std::string::npos);
}
