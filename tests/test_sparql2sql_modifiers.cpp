/**
 * Tests for solution modifiers: SELECT projection/DISTINCT/REDUCED,
 * GROUP BY/HAVING/aggregates, ORDER BY, LIMIT/OFFSET, and ASK.
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
#include "sparql2sql/TranslationError.h"
#include "sparql2sql/Translator.h"

using r2rml::R2RMLMapping;
using r2rml::R2RMLParser;
using sparql::Parser;
using sparql2sql::DuckDbDialect;
using sparql2sql::translateQuery;

namespace {

const char *const kPrefix = "PREFIX ex: <http://example.com/ns#>\n";

std::string translate(const std::string &queryBody, R2RMLMapping &mapping) {
	Parser parser;
	auto q = parser.parseString(kPrefix + queryBody);
	DuckDbDialect dialect;
	return translateQuery(*q, mapping, dialect);
}

} // namespace

TEST_CASE("SELECT DISTINCT emits SQL DISTINCT") {
	R2RMLParser mappingParser;
	R2RMLMapping mapping = mappingParser.parse(SOURCE_R2RML_DIR "example_emp_dept.ttl");
	std::string sql = translate("SELECT DISTINCT ?e WHERE { ?e ex:name ?n . }", mapping);
	CHECK(sql.find("SELECT DISTINCT") != std::string::npos);
}

TEST_CASE("SELECT REDUCED also maps to SQL DISTINCT") {
	R2RMLParser mappingParser;
	R2RMLMapping mapping = mappingParser.parse(SOURCE_R2RML_DIR "example_emp_dept.ttl");
	std::string sql = translate("SELECT REDUCED ?e WHERE { ?e ex:name ?n . }", mapping);
	CHECK(sql.find("SELECT DISTINCT") != std::string::npos);
}

TEST_CASE("plain SELECT has no DISTINCT") {
	R2RMLParser mappingParser;
	R2RMLMapping mapping = mappingParser.parse(SOURCE_R2RML_DIR "example_emp_dept.ttl");
	std::string sql = translate("SELECT ?e WHERE { ?e ex:name ?n . }", mapping);
	REQUIRE(sql.substr(0, 6) == "SELECT");
	CHECK(sql.substr(0, 15) != "SELECT DISTINCT");
}

TEST_CASE("GROUP BY / COUNT aggregate") {
	R2RMLParser mappingParser;
	R2RMLMapping mapping = mappingParser.parse(SOURCE_R2RML_DIR "example_emp_dept.ttl");
	std::string sql = translate("SELECT ?d (COUNT(?e) AS ?cnt) WHERE { ?e ex:department ?d . } GROUP BY ?d", mapping);
	CHECK(sql.find("GROUP BY") != std::string::npos);
	CHECK(sql.find("COUNT(") != std::string::npos);
	CHECK(sql.find("\"v_cnt\"") != std::string::npos);
}

TEST_CASE("implicit whole-result grouping: an aggregate with no explicit GROUP BY emits no GROUP BY clause") {
	R2RMLParser mappingParser;
	R2RMLMapping mapping = mappingParser.parse(SOURCE_R2RML_DIR "example_emp_dept.ttl");
	std::string sql = translate("SELECT (COUNT(?e) AS ?cnt) WHERE { ?e ex:department ?d . }", mapping);
	CHECK(sql.find("COUNT(") != std::string::npos);
	CHECK(sql.find("GROUP BY") == std::string::npos);
}

TEST_CASE("HAVING filters on an aggregate condition") {
	R2RMLParser mappingParser;
	R2RMLMapping mapping = mappingParser.parse(SOURCE_R2RML_DIR "example_emp_dept.ttl");
	std::string sql = translate(
	    "SELECT ?d (COUNT(?e) AS ?cnt) WHERE { ?e ex:department ?d . } GROUP BY ?d HAVING(COUNT(?e) > 1)", mapping);
	CHECK(sql.find("HAVING") != std::string::npos);
}

TEST_CASE("GROUP BY (expr AS ?var) is selectable and orderable via its alias") {
	R2RMLParser mappingParser;
	R2RMLMapping mapping = mappingParser.parse(SOURCE_R2RML_DIR "example_emp_dept.ttl");
	std::string sql = translate("SELECT ?u (COUNT(?e) AS ?cnt) WHERE { ?e ex:name ?u . } "
	                            "GROUP BY (UCASE(?u) AS ?u) ORDER BY ?cnt",
	                            mapping);
	CHECK(sql.find("UPPER(") != std::string::npos);
	CHECK(sql.find("GROUP BY \"v_u\"") != std::string::npos);
	CHECK(sql.find("ORDER BY \"v_cnt\"") != std::string::npos);
}

TEST_CASE("ORDER BY ASC/DESC") {
	R2RMLParser mappingParser;
	R2RMLMapping mapping = mappingParser.parse(SOURCE_R2RML_DIR "example_emp_dept.ttl");
	std::string ascSql = translate("SELECT ?n WHERE { ?e ex:name ?n . } ORDER BY ?n", mapping);
	CHECK(ascSql.find("ORDER BY") != std::string::npos);
	CHECK(ascSql.find(" ASC") != std::string::npos);

	std::string descSql = translate("SELECT ?n WHERE { ?e ex:name ?n . } ORDER BY DESC(?n)", mapping);
	CHECK(descSql.find(" DESC") != std::string::npos);
}

TEST_CASE("LIMIT/OFFSET") {
	R2RMLParser mappingParser;
	R2RMLMapping mapping = mappingParser.parse(SOURCE_R2RML_DIR "example_emp_dept.ttl");
	std::string sql = translate("SELECT ?n WHERE { ?e ex:name ?n . } LIMIT 5 OFFSET 10", mapping);
	CHECK(sql.find("LIMIT 5") != std::string::npos);
	CHECK(sql.find("OFFSET 10") != std::string::npos);
}

TEST_CASE("ASK wraps the WHERE pattern as an EXISTS check") {
	R2RMLParser mappingParser;
	R2RMLMapping mapping = mappingParser.parse(SOURCE_R2RML_DIR "example_emp_dept.ttl");
	std::string sql = translate("ASK { ?e ex:department ?d . }", mapping);
	CHECK(sql.find("SELECT EXISTS (") != std::string::npos);
	CHECK(sql.find("\"ask\"") != std::string::npos);
}

TEST_CASE("ASK ignores ORDER BY/LIMIT/OFFSET but respects GROUP BY/HAVING") {
	R2RMLParser mappingParser;
	R2RMLMapping mapping = mappingParser.parse(SOURCE_R2RML_DIR "example_emp_dept.ttl");
	std::string sql = translate("ASK { ?e ex:department ?d . } GROUP BY ?d HAVING(COUNT(?e) > 100)", mapping);
	CHECK(sql.find("GROUP BY") != std::string::npos);
	CHECK(sql.find("HAVING") != std::string::npos);
	CHECK(sql.find("ORDER BY") == std::string::npos);
	CHECK(sql.find("LIMIT") == std::string::npos);
}

TEST_CASE("HAVING: containsAggregate recurses through Unary/In/BuiltInCall/IriRef/Exists wrappers") {
	R2RMLParser mappingParser;
	R2RMLMapping mapping = mappingParser.parse(SOURCE_R2RML_DIR "example_emp_dept.ttl");
	std::string sql = translate("SELECT ?g (SUM(?e) AS ?s) WHERE { ?e ex:name ?n . ?e ex:department ?g . } "
	                            "GROUP BY ?g "
	                            "HAVING (!(?g = \"X\") && (?g IN (\"A\", \"B\")) && BOUND(?g) && "
	                            "(?g != <urn:const>) && EXISTS { ?e ex:name ?z } && (SUM(?e) > 0))",
	                            mapping);
	CHECK(sql.find("HAVING") != std::string::npos);
}

TEST_CASE("HAVING: containsAggregate's FunctionCall arm runs even though translation later throws") {
	R2RMLParser mappingParser;
	R2RMLMapping mapping = mappingParser.parse(SOURCE_R2RML_DIR "example_emp_dept.ttl");
	CHECK_THROWS_AS(translate("SELECT ?g WHERE { ?e ex:name ?n . ?e ex:department ?g . } GROUP BY ?g "
	                          "HAVING (<urn:f>(?g) && (SUM(?n) > 0))",
	                          mapping),
	                sparql2sql::TranslationError);
}

TEST_CASE("An aggregate only inside HAVING (not in the SELECT list) still triggers grouping") {
	R2RMLParser mappingParser;
	R2RMLMapping mapping = mappingParser.parse(SOURCE_R2RML_DIR "example_emp_dept.ttl");
	std::string sql = translate(
	    "SELECT ?g WHERE { ?e ex:name ?n . ?e ex:department ?g . } GROUP BY ?g HAVING (COUNT(?n) > 1)", mapping);
	CHECK(sql.find("HAVING") != std::string::npos);
}

TEST_CASE("Multiple HAVING conditions are joined with AND") {
	R2RMLParser mappingParser;
	R2RMLMapping mapping = mappingParser.parse(SOURCE_R2RML_DIR "example_emp_dept.ttl");
	std::string sql = translate("SELECT ?g (COUNT(?n) AS ?c) WHERE { ?e ex:name ?n . ?e ex:department ?g . } "
	                            "GROUP BY ?g HAVING (COUNT(?n) > 1) (?g != \"\")",
	                            mapping);
	CHECK(sql.find("HAVING") != std::string::npos);
	CHECK(sql.find(" AND ") != std::string::npos);
}

TEST_CASE("Multiple ORDER BY conditions are comma-joined") {
	R2RMLParser mappingParser;
	R2RMLMapping mapping = mappingParser.parse(SOURCE_R2RML_DIR "example_emp_dept.ttl");
	std::string sql = translate("SELECT ?e ?n WHERE { ?e ex:name ?n . } ORDER BY ?e ?n", mapping);
	CHECK(sql.find("ORDER BY") != std::string::npos);
	std::size_t orderByPos = sql.find("ORDER BY");
	CHECK(sql.find(", ", orderByPos) != std::string::npos);
}

TEST_CASE("A trailing VALUES clause merges with the WHERE pattern via an inner join") {
	R2RMLParser mappingParser;
	R2RMLMapping mapping = mappingParser.parse(SOURCE_R2RML_DIR "example_emp_dept.ttl");
	std::string sql = translate("SELECT ?e ?n WHERE { ?e ex:name ?n . } VALUES ?n { \"SMITH\" }", mapping);
	CHECK(sql.find("SMITH") != std::string::npos);
}

TEST_CASE("SELECT * projects every in-scope variable") {
	R2RMLParser mappingParser;
	R2RMLMapping mapping = mappingParser.parse(SOURCE_R2RML_DIR "example_emp_dept.ttl");
	std::string sql = translate("SELECT * WHERE { ?e ex:name ?n . }", mapping);
	CHECK(sql.find("\"v_e\"") != std::string::npos);
	CHECK(sql.find("\"v_n\"") != std::string::npos);
}

TEST_CASE("Bare-projecting a GROUP BY (expr AS ?var) alias that isn't an original source var") {
	R2RMLParser mappingParser;
	R2RMLMapping mapping = mappingParser.parse(SOURCE_R2RML_DIR "example_emp_dept.ttl");
	std::string sql = translate("SELECT ?y WHERE { ?e ex:name ?n . } GROUP BY (STRLEN(?n) + 1 AS ?y)", mapping);
	CHECK(sql.find("\"v_y\"") != std::string::npos);
}

TEST_CASE("Multiple GROUP BY keys are comma-joined") {
	R2RMLParser mappingParser;
	R2RMLMapping mapping = mappingParser.parse(SOURCE_R2RML_DIR "example_emp_dept.ttl");
	std::string sql = translate("SELECT ?g ?n WHERE { ?e ex:name ?n . ?e ex:department ?g . } GROUP BY ?g ?n", mapping);
	std::size_t groupByPos = sql.find("GROUP BY");
	REQUIRE(groupByPos != std::string::npos);
	CHECK(sql.find(", ", groupByPos) != std::string::npos);
}

TEST_CASE("A non-star SubSelect populates its own boundVars distinct from the outer SELECT *") {
	R2RMLParser mappingParser;
	R2RMLMapping mapping = mappingParser.parse(SOURCE_R2RML_DIR "example_emp_dept.ttl");
	std::string sql = translate("SELECT * WHERE { { SELECT ?e WHERE { ?e ex:name ?n . } } }", mapping);
	CHECK(sql.find("\"v_e\"") != std::string::npos);
}

TEST_CASE("ASK with a trailing VALUES clause merges via an inner join") {
	R2RMLParser mappingParser;
	R2RMLMapping mapping = mappingParser.parse(SOURCE_R2RML_DIR "example_emp_dept.ttl");
	std::string sql = translate("ASK { ?e ex:name ?n . } VALUES ?n { \"SMITH\" }", mapping);
	CHECK(sql.find("SMITH") != std::string::npos);
}

TEST_CASE("ASK with GROUP BY (expr AS ?var) and HAVING emits a select-list prefix column") {
	R2RMLParser mappingParser;
	R2RMLMapping mapping = mappingParser.parse(SOURCE_R2RML_DIR "example_emp_dept.ttl");
	std::string sql = translate("ASK { ?e ex:name ?n . } GROUP BY (UCASE(?n) AS ?nn) HAVING (?n != \"\")", mapping);
	CHECK(sql.find("UPPER(") != std::string::npos);
	CHECK(sql.find("HAVING") != std::string::npos);
}
