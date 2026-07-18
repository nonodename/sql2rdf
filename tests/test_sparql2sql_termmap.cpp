/**
 * Unit tests for the SPARQL-to-SQL translator's template parsing/inversion
 * module (TemplateUtil) and its general TermMap-to-SQL dispatcher
 * (TermMapSql). Pure string/R2RML logic - no SPARQL AST, no DuckDB.
 */

#include <catch2/catch_test_macros.hpp>
#include <serd/serd.h>

#include <memory>
#include <string>

#include "r2rml/ColumnTermMap.h"
#include "r2rml/ConstantTermMap.h"
#include "r2rml/TemplateTermMap.h"
#include "sparql-parser/ast/Term.h"
#include "sparql2sql/DuckDbDialect.h"
#include "sparql2sql/TemplateUtil.h"
#include "sparql2sql/TermMapSql.h"

using sparql2sql::buildProjectionSql;
using sparql2sql::DuckDbDialect;
using sparql2sql::InversionKind;
using sparql2sql::InversionResult;
using sparql2sql::invertTemplate;
using sparql2sql::invertTermMapAgainstBoundTerm;
using sparql2sql::parseTemplate;
using sparql2sql::percentDecode;
using sparql2sql::referencedColumns;
using sparql2sql::SqlExpr;
using sparql2sql::termMapToSqlExpr;

TEST_CASE("parseTemplate splits literal text and {column} placeholders") {
	auto segs = parseTemplate("http://data.example.com/employee/{EMPNO}");
	REQUIRE(segs.size() == 2);
	CHECK_FALSE(segs[0].isPlaceholder);
	CHECK(segs[0].text == "http://data.example.com/employee/");
	CHECK(segs[1].isPlaceholder);
	CHECK(segs[1].text == "EMPNO");
}

TEST_CASE("parseTemplate handles multiple placeholders separated by a literal delimiter") {
	auto segs = parseTemplate("http://ex.org/order/{CUSTID}/{ORDERID}");
	REQUIRE(segs.size() == 4);
	CHECK_FALSE(segs[0].isPlaceholder);
	CHECK(segs[0].text == "http://ex.org/order/");
	CHECK(segs[1].isPlaceholder);
	CHECK(segs[1].text == "CUSTID");
	CHECK_FALSE(segs[2].isPlaceholder);
	CHECK(segs[2].text == "/");
	CHECK(segs[3].isPlaceholder);
	CHECK(segs[3].text == "ORDERID");
}

TEST_CASE("parseTemplate handles adjacent placeholders with no delimiter") {
	auto segs = parseTemplate("{CUSTID}{ORDERID}");
	REQUIRE(segs.size() == 2);
	CHECK(segs[0].isPlaceholder);
	CHECK(segs[0].text == "CUSTID");
	CHECK(segs[1].isPlaceholder);
	CHECK(segs[1].text == "ORDERID");
}

TEST_CASE("parseTemplate mirrors TemplateTermMap.cpp's unmatched-brace truncation") {
	// Matches r2rml::TemplateTermMap::generateRDFTerm exactly: an unmatched
	// '{' truncates the rest of the template (despite that function's own
	// comment claiming "treat rest as literal" - the actual code `break`s).
	auto segs = parseTemplate("abc{unterminated and more text");
	REQUIRE(segs.size() == 1);
	CHECK_FALSE(segs[0].isPlaceholder);
	CHECK(segs[0].text == "abc");
}

TEST_CASE("referencedColumns returns distinct placeholder names in first-occurrence order") {
	auto segs = parseTemplate("{a}-{b}-{a}");
	auto cols = referencedColumns(segs);
	REQUIRE(cols.size() == 2);
	CHECK(cols[0] == "a");
	CHECK(cols[1] == "b");
}

TEST_CASE("buildProjectionSql reconstructs the template as a concatenation expression") {
	DuckDbDialect dialect;
	auto segs = parseTemplate("http://ex.org/e/{ID}");
	std::string sql = buildProjectionSql(segs, "t1", dialect);
	CHECK(sql.find("t1") != std::string::npos);
	CHECK(sql.find("\"ID\"") != std::string::npos);
	CHECK(sql.find("'http://ex.org/e/'") != std::string::npos);
}

TEST_CASE("invertTemplate: NeverMatches when the literal prefix doesn't fit") {
	auto segs = parseTemplate("http://ex.org/e/{ID}");
	auto outcome = invertTemplate(segs, "http://other.org/e/7");
	CHECK(outcome.kind == InversionKind::NeverMatches);
}

TEST_CASE("invertTemplate: PerColumnMatch for a single unambiguous placeholder") {
	auto segs = parseTemplate("http://ex.org/e/{ID}");
	auto outcome = invertTemplate(segs, "http://ex.org/e/7369");
	REQUIRE(outcome.kind == InversionKind::PerColumnMatch);
	REQUIRE(outcome.columnValues.size() == 1);
	CHECK(outcome.columnValues[0].first == "ID");
	CHECK(outcome.columnValues[0].second == "7369");
}

TEST_CASE("invertTemplate: PerColumnMatch splits multiple delimited placeholders") {
	auto segs = parseTemplate("http://ex.org/order/{CUSTID}/{ORDERID}");
	auto outcome = invertTemplate(segs, "http://ex.org/order/42/99");
	REQUIRE(outcome.kind == InversionKind::PerColumnMatch);
	REQUIRE(outcome.columnValues.size() == 2);
	CHECK(outcome.columnValues[0].first == "CUSTID");
	CHECK(outcome.columnValues[0].second == "42");
	CHECK(outcome.columnValues[1].first == "ORDERID");
	CHECK(outcome.columnValues[1].second == "99");
}

TEST_CASE("invertTemplate: WholeTemplateMatch for adjacent placeholders with no delimiter") {
	auto segs = parseTemplate("{CUSTID}{ORDERID}");
	auto outcome = invertTemplate(segs, "4299");
	CHECK(outcome.kind == InversionKind::WholeTemplateMatch);
	CHECK(outcome.columnValues.empty());
}

TEST_CASE("invertTemplate: adjacent-placeholder template still prunes on a bad literal anchor") {
	auto segs = parseTemplate("pre-{A}{B}-post");
	auto outcome = invertTemplate(segs, "WRONG-4299-post");
	CHECK(outcome.kind == InversionKind::NeverMatches);
}

TEST_CASE("invertTemplate: a placeholder that is the final segment absorbs the whole remainder") {
	// No trailing literal to delimit {ID}, so it legitimately captures
	// everything left, including what looks like "extra" path segments.
	auto segs = parseTemplate("http://ex.org/e/{ID}");
	auto outcome = invertTemplate(segs, "http://ex.org/e/7369/extra");
	REQUIRE(outcome.kind == InversionKind::PerColumnMatch);
	REQUIRE(outcome.columnValues.size() == 1);
	CHECK(outcome.columnValues[0].second == "7369/extra");
}

TEST_CASE("invertTemplate: trailing literal segment that isn't fully consumed is NeverMatches") {
	auto segs = parseTemplate("abc{X}def");
	auto outcome = invertTemplate(segs, "abcVALUEdefEXTRA");
	CHECK(outcome.kind == InversionKind::NeverMatches);
}

TEST_CASE("percentDecode is the inverse of RFC3986 percent-encoding") {
	CHECK(percentDecode("hello%20world") == "hello world");
	CHECK(percentDecode("100%25") == "100%");
	CHECK(percentDecode("no-encoding-here") == "no-encoding-here");
	// Trailing malformed escape left as-is rather than decoded.
	CHECK(percentDecode("bad%2") == "bad%2");
}

TEST_CASE("termMapToSqlExpr: ColumnTermMap produces a cast column reference") {
	DuckDbDialect dialect;
	r2rml::ColumnTermMap col("EMPNO");
	SqlExpr e = termMapToSqlExpr(col, "t1", dialect);
	CHECK(e.expr == "CAST(t1.\"EMPNO\" AS VARCHAR)");
	REQUIRE(e.requiredNonNullColumns.size() == 1);
	// Alias-qualified, not a bare column name: a candidate's FROM clause can
	// join multiple tables, and a bare "IS NOT NULL" guard would be
	// ambiguous (or silently wrong) if both sides share a column name.
	CHECK(e.requiredNonNullColumns[0] == "t1.\"EMPNO\"");
}

TEST_CASE("termMapToSqlExpr: TemplateTermMap produces a concatenation and required columns") {
	DuckDbDialect dialect;
	r2rml::TemplateTermMap tmpl("http://data.example.com/employee/{EMPNO}");
	SqlExpr e = termMapToSqlExpr(tmpl, "t1", dialect);
	CHECK(e.expr.find("t1.\"EMPNO\"") != std::string::npos);
	REQUIRE(e.requiredNonNullColumns.size() == 1);
	CHECK(e.requiredNonNullColumns[0] == "t1.\"EMPNO\"");
}

TEST_CASE("termMapToSqlExpr: ConstantTermMap produces a string literal with no required columns") {
	DuckDbDialect dialect;
	std::string uriText = "http://example.com/ns#Employee";
	SerdNode node = serd_node_from_string(SERD_URI, reinterpret_cast<const uint8_t *>(uriText.c_str()));
	r2rml::ConstantTermMap constant(node);
	SqlExpr e = termMapToSqlExpr(constant, "t1", dialect);
	CHECK(e.expr == "'http://example.com/ns#Employee'");
	CHECK(e.requiredNonNullColumns.empty());
}

TEST_CASE("invertTermMapAgainstBoundTerm: ConstantTermMap matches only its own value") {
	DuckDbDialect dialect;
	std::string uriText = "http://example.com/ns#Active";
	SerdNode node = serd_node_from_string(SERD_URI, reinterpret_cast<const uint8_t *>(uriText.c_str()));
	r2rml::ConstantTermMap constant(node);

	sparql::ast::Iri matching("http://example.com/ns#Active", "ex:Active");
	InversionResult matchResult = invertTermMapAgainstBoundTerm(constant, matching, "t1", dialect);
	CHECK(matchResult.possible);
	CHECK(matchResult.whereConditions.empty());

	sparql::ast::Iri mismatching("http://example.com/ns#Inactive", "ex:Inactive");
	InversionResult mismatchResult = invertTermMapAgainstBoundTerm(constant, mismatching, "t1", dialect);
	CHECK_FALSE(mismatchResult.possible);
}

TEST_CASE("invertTermMapAgainstBoundTerm: ColumnTermMap always possible, emits an equality condition") {
	DuckDbDialect dialect;
	r2rml::ColumnTermMap col("ENAME");
	sparql::ast::RdfLiteral lit("SMITH");
	InversionResult result = invertTermMapAgainstBoundTerm(col, lit, "t1", dialect);
	CHECK(result.possible);
	REQUIRE(result.whereConditions.size() == 1);
	CHECK(result.whereConditions[0] == "CAST(t1.\"ENAME\" AS VARCHAR) = 'SMITH'");
}

TEST_CASE("invertTermMapAgainstBoundTerm: TemplateTermMap prunes on mismatch and constrains on match") {
	DuckDbDialect dialect;
	r2rml::TemplateTermMap tmpl("http://data.example.com/employee/{EMPNO}");

	sparql::ast::Iri boundMatching("http://data.example.com/employee/7369", "");
	InversionResult matchResult = invertTermMapAgainstBoundTerm(tmpl, boundMatching, "t1", dialect);
	CHECK(matchResult.possible);
	REQUIRE(matchResult.whereConditions.size() == 1);
	CHECK(matchResult.whereConditions[0] == "CAST(t1.\"EMPNO\" AS VARCHAR) = '7369'");

	sparql::ast::Iri boundMismatching("http://other.example.com/employee/7369", "");
	InversionResult mismatchResult = invertTermMapAgainstBoundTerm(tmpl, boundMismatching, "t1", dialect);
	CHECK_FALSE(mismatchResult.possible);
}
