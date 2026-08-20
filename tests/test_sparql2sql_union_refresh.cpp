/**
 * Integration-level tests for pruneUnionSide's schema refresh (Optimizer.cpp):
 * when a union is narrowed by join-based pruning but MORE THAN ONE arm
 * survives, the surviving arms' term annotations must be re-meeted rather
 * than left at the stale, degraded meet computed over the original (larger)
 * arm set. Uses sparql2sql_union_refresh.ttl, where ex:val is ambiguous over
 * three triples maps (TableA/TableB xsd:integer, TableC xsd:string) and
 * ex:marker is ambiguous over only TableA/TableB, with subject templates
 * disjoint from TableC's.
 */

#include <catch2/catch_test_macros.hpp>

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
	R2RMLMapping mapping = mappingParser.parse(SOURCE_R2RML_DIR "sparql2sql_union_refresh.ttl");
	DuckDbDialect dialect;
	return translateQuery(*q, mapping, dialect);
}

bool contains(const std::string &haystack, const std::string &needle) {
	return haystack.find(needle) != std::string::npos;
}

bool hasAnyTagColumn(const std::string &sql) {
	return contains(sql, "\"d_");
}

} // namespace

TEST_CASE("translateQuery: ex:val alone is genuinely ambiguous across all three arms") {
	// Baseline: without the ex:marker join, TableC's xsd:string arm is still
	// live, so ex:val's meet is degraded (integer vs integer vs string) and
	// ORDER BY ?v must carry the full runtime dispatch.
	std::string sql = translateFixture("union_refresh_val_alone.rq");
	CHECK(contains(sql, "\"TABLE_C\""));
	CHECK(hasAnyTagColumn(sql));
}

TEST_CASE("translateQuery: joining ex:val with ex:marker prunes TableC but leaves two arms") {
	// ex:marker only exists on TableA/TableB, whose subject templates
	// (".../a/{ID}", ".../b/{ID}") are disjoint from TableC's (".../c/{ID}"),
	// so pruneUnionSide drops exactly the TableC arm - two arms survive, not
	// one, so unwrapSingletonUnion never fires.
	std::string sql = translateFixture("union_refresh_val_marker_order.rq");
	CHECK_FALSE(contains(sql, "\"TABLE_C\""));
	CHECK(contains(sql, "\"TABLE_A\""));
	CHECK(contains(sql, "\"TABLE_B\""));

	// TableA and TableB agree on ex:val's datatype (xsd:integer), so the
	// refreshed two-arm meet is fully determined even though the original
	// three-arm meet was not: ORDER BY ?v should fold to a single typed key
	// with no runtime type-tag column and no cross-type CASE ladder.
	CHECK_FALSE(hasAnyTagColumn(sql));
	CHECK(contains(sql, "ORDER BY"));
	CHECK_FALSE(contains(sql, "WHEN 1 THEN"));
}
