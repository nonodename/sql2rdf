/**
 * Tests for FILTER/BIND/EXISTS expression translation (ExpressionTranslator).
 * Uses ad hoc SPARQL query text (via Parser::parseString) against the
 * example_emp_dept.ttl mapping fixture, asserting on structural properties
 * of the generated SQL, plus negative tests for every deferred builtin.
 */

#include <catch2/catch_test_macros.hpp>

#include <string>

#ifndef SOURCE_R2RML_DIR
#define SOURCE_R2RML_DIR ""
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
using sparql2sql::TranslationError;

namespace {

const char *const kPrefix = "PREFIX ex: <http://example.com/ns#>\n";

std::string translate(const std::string &queryBody, R2RMLMapping &mapping) {
	Parser parser;
	auto q = parser.parseString(kPrefix + queryBody);
	DuckDbDialect dialect;
	return translateQuery(*q, mapping, dialect);
}

} // namespace

TEST_CASE("FILTER: a comparison against a variable and a literal") {
	R2RMLParser mappingParser;
	R2RMLMapping mapping = mappingParser.parse(SOURCE_R2RML_DIR "example_emp_dept.ttl");
	std::string sql = translate("SELECT ?e ?n WHERE { ?e ex:name ?n . FILTER(STRLEN(?n) > 5) }", mapping);
	CHECK(sql.find("LENGTH(") != std::string::npos);
	CHECK(sql.find("WHERE") != std::string::npos);
	CHECK(sql.find("TRY_CAST") != std::string::npos);
}

TEST_CASE("FILTER: bound() checks the qualified projected column") {
	R2RMLParser mappingParser;
	R2RMLMapping mapping = mappingParser.parse(SOURCE_R2RML_DIR "example_emp_dept.ttl");
	std::string sql = translate(
	    "SELECT ?e ?n ?d WHERE { ?e ex:name ?n . OPTIONAL { ?e ex:department ?d } FILTER(bound(?d)) }", mapping);
	CHECK(sql.find("\"v_d\" IS NOT NULL") != std::string::npos);
}

TEST_CASE("FILTER: bound() on an out-of-scope variable throws") {
	R2RMLParser mappingParser;
	R2RMLMapping mapping = mappingParser.parse(SOURCE_R2RML_DIR "example_emp_dept.ttl");
	CHECK_THROWS_AS(translate("SELECT ?e WHERE { ?e ex:name ?n . FILTER(bound(?nope)) }", mapping), TranslationError);
}

TEST_CASE("BIND: introduces a computed column via UCASE") {
	R2RMLParser mappingParser;
	R2RMLMapping mapping = mappingParser.parse(SOURCE_R2RML_DIR "example_emp_dept.ttl");
	std::string sql = translate("SELECT ?e ?upper WHERE { ?e ex:name ?n . BIND(UCASE(?n) AS ?upper) }", mapping);
	CHECK(sql.find("UPPER(") != std::string::npos);
	CHECK(sql.find("\"v_upper\"") != std::string::npos);
}

TEST_CASE("FILTER EXISTS: correlates on the shared variable, qualified on both sides") {
	R2RMLParser mappingParser;
	R2RMLMapping mapping = mappingParser.parse(SOURCE_R2RML_DIR "example_emp_dept.ttl");
	std::string sql = translate("SELECT ?e WHERE { ?e ex:name ?n . FILTER EXISTS { ?e ex:department ?d } }", mapping);
	CHECK(sql.find("EXISTS (") != std::string::npos);
	CHECK(sql.find("NOT EXISTS") == std::string::npos);
	// Correlation condition references v_e qualified on both sides (never
	// bare - see ExpressionTranslator.h's doc comment on why).
	CHECK(sql.find("\"v_e\" = ") != std::string::npos);
}

TEST_CASE("FILTER NOT EXISTS: negates the EXISTS clause") {
	R2RMLParser mappingParser;
	R2RMLMapping mapping = mappingParser.parse(SOURCE_R2RML_DIR "example_emp_dept.ttl");
	std::string sql =
	    translate("SELECT ?e WHERE { ?e ex:name ?n . FILTER NOT EXISTS { ?e ex:department ?d } }", mapping);
	CHECK(sql.find("NOT EXISTS (") != std::string::npos);
}

TEST_CASE("FILTER: string builtins map to the expected DuckDB functions") {
	R2RMLParser mappingParser;
	R2RMLMapping mapping = mappingParser.parse(SOURCE_R2RML_DIR "example_emp_dept.ttl");
	CHECK(translate("SELECT ?e ?n WHERE { ?e ex:name ?n . FILTER(CONTAINS(?n, \"MI\")) }", mapping).find("CONTAINS(") !=
	      std::string::npos);
	CHECK(translate("SELECT ?e ?n WHERE { ?e ex:name ?n . FILTER(STRSTARTS(?n, \"S\")) }", mapping)
	          .find("STARTS_WITH(") != std::string::npos);
	CHECK(translate("SELECT ?e ?n WHERE { ?e ex:name ?n . FILTER(STRENDS(?n, \"H\")) }", mapping).find("ENDS_WITH(") !=
	      std::string::npos);
	CHECK(translate("SELECT ?e ?n WHERE { ?e ex:name ?n . FILTER(REGEX(?n, \"^S\")) }", mapping)
	          .find("regexp_matches(") != std::string::npos);
}

TEST_CASE("FILTER: hash builtins available in DuckDB translate directly") {
	R2RMLParser mappingParser;
	R2RMLMapping mapping = mappingParser.parse(SOURCE_R2RML_DIR "example_emp_dept.ttl");
	CHECK(translate("SELECT ?e WHERE { ?e ex:name ?n . FILTER(MD5(?n) = \"x\") }", mapping).find("md5(") !=
	      std::string::npos);
	CHECK(translate("SELECT ?e WHERE { ?e ex:name ?n . FILTER(SHA1(?n) = \"x\") }", mapping).find("sha1(") !=
	      std::string::npos);
	CHECK(translate("SELECT ?e WHERE { ?e ex:name ?n . FILTER(SHA256(?n) = \"x\") }", mapping).find("sha256(") !=
	      std::string::npos);
}

TEST_CASE("FILTER: deferred builtins throw a named TranslationError") {
	R2RMLParser mappingParser;
	R2RMLMapping mapping = mappingParser.parse(SOURCE_R2RML_DIR "example_emp_dept.ttl");

	CHECK_THROWS_AS(translate("SELECT ?e WHERE { ?e ex:name ?n . FILTER(isIRI(?e)) }", mapping), TranslationError);
	CHECK_THROWS_AS(translate("SELECT ?e WHERE { ?e ex:name ?n . FILTER(isBLANK(?e)) }", mapping), TranslationError);
	CHECK_THROWS_AS(translate("SELECT ?e WHERE { ?e ex:name ?n . FILTER(isLITERAL(?n)) }", mapping), TranslationError);
	CHECK_THROWS_AS(translate("SELECT ?e WHERE { ?e ex:name ?n . FILTER(lang(?n) = \"\") }", mapping),
	                TranslationError);
	CHECK_THROWS_AS(translate("SELECT ?e WHERE { ?e ex:name ?n . FILTER(datatype(?n) = ex:foo) }", mapping),
	                TranslationError);
	CHECK_THROWS_AS(translate("SELECT ?e WHERE { ?e ex:name ?n . FILTER(SHA384(?n) = \"x\") }", mapping),
	                TranslationError);
	CHECK_THROWS_AS(translate("SELECT ?e WHERE { ?e ex:name ?n . FILTER(SHA512(?n) = \"x\") }", mapping),
	                TranslationError);
	CHECK_THROWS_AS(translate("SELECT ?e WHERE { ?e ex:name ?n . FILTER(YEAR(?n) = 2020) }", mapping),
	                TranslationError);
	CHECK_THROWS_AS(translate("SELECT ?e WHERE { ?e ex:name ?n . FILTER(ENCODE_FOR_URI(?n) = \"\") }", mapping),
	                TranslationError);
	CHECK_THROWS_AS(translate("SELECT ?e WHERE { ?e ex:name ?n . FILTER(RAND() > 0.5) }", mapping), TranslationError);
}

TEST_CASE("FILTER: a non-builtin function call throws a named TranslationError") {
	R2RMLParser mappingParser;
	R2RMLMapping mapping = mappingParser.parse(SOURCE_R2RML_DIR "example_emp_dept.ttl");
	CHECK_THROWS_AS(translate("SELECT ?e WHERE { ?e ex:name ?n . FILTER(ex:customFunc(?n)) }", mapping),
	                TranslationError);
}

TEST_CASE("FILTER: xsd:integer/decimal/double/float/string casts translate without throwing") {
	R2RMLParser mappingParser;
	R2RMLMapping mapping = mappingParser.parse(SOURCE_R2RML_DIR "example_emp_dept.ttl");
	std::string sql = translate("PREFIX xsd: <http://www.w3.org/2001/XMLSchema#>\n"
	                            "SELECT ?e ?n WHERE { ?e ex:name ?n . "
	                            "FILTER(xsd:integer(?n) > 5 || xsd:decimal(?n) > 5.0 || xsd:double(?n) > 5.0 || "
	                            "xsd:float(?n) > 5.0 || xsd:string(?n) != \"\") }",
	                            mapping);
	CHECK(sql.find("TRY_CAST") != std::string::npos);
	CHECK(sql.find("AS BIGINT)") != std::string::npos);
}

TEST_CASE("FILTER: xsd:integer() comparison produces a numeric-aware comparison") {
	R2RMLParser mappingParser;
	R2RMLMapping mapping = mappingParser.parse(SOURCE_R2RML_DIR "example_emp_dept.ttl");
	std::string sql = translate("PREFIX xsd: <http://www.w3.org/2001/XMLSchema#>\n"
	                            "SELECT ?e ?n WHERE { ?e ex:name ?n . FILTER(xsd:integer(?n) > 5) }",
	                            mapping);
	CHECK(sql.find("CASE WHEN") != std::string::npos);
	CHECK(sql.find("IS NOT NULL AND") != std::string::npos);
}

TEST_CASE("BIND: xsd:integer introduces a computed column") {
	R2RMLParser mappingParser;
	R2RMLMapping mapping = mappingParser.parse(SOURCE_R2RML_DIR "example_emp_dept.ttl");
	std::string sql = translate("PREFIX xsd: <http://www.w3.org/2001/XMLSchema#>\n"
	                            "SELECT ?e ?asint WHERE { ?e ex:name ?n . BIND(xsd:integer(?n) AS ?asint) }",
	                            mapping);
	CHECK(sql.find("\"v_asint\"") != std::string::npos);
	CHECK(sql.find("AS BIGINT)") != std::string::npos);
}

TEST_CASE("FILTER: xsd cast with wrong arity throws a named TranslationError") {
	R2RMLParser mappingParser;
	R2RMLMapping mapping = mappingParser.parse(SOURCE_R2RML_DIR "example_emp_dept.ttl");
	CHECK_THROWS_AS(translate("PREFIX xsd: <http://www.w3.org/2001/XMLSchema#>\n"
	                          "SELECT ?e WHERE { ?e ex:name ?n . FILTER(xsd:integer() = \"\") }",
	                          mapping),
	                TranslationError);
	CHECK_THROWS_AS(translate("PREFIX xsd: <http://www.w3.org/2001/XMLSchema#>\n"
	                          "SELECT ?e WHERE { ?e ex:name ?n . FILTER(xsd:integer(?n, ?n) > 0) }",
	                          mapping),
	                TranslationError);
}

TEST_CASE("FILTER: xsd:boolean is out of scope and still throws") {
	R2RMLParser mappingParser;
	R2RMLMapping mapping = mappingParser.parse(SOURCE_R2RML_DIR "example_emp_dept.ttl");
	CHECK_THROWS_AS(translate("SELECT ?e WHERE { ?e ex:name ?n . "
	                          "FILTER(<http://www.w3.org/2001/XMLSchema#boolean>(?n) = \"true\") }",
	                          mapping),
	                TranslationError);
}

TEST_CASE("FILTER: remaining comparison operators (!=, <=, >=) and Or/And") {
	R2RMLParser mappingParser;
	R2RMLMapping mapping = mappingParser.parse(SOURCE_R2RML_DIR "example_emp_dept.ttl");
	std::string sql = translate(
	    "SELECT ?e ?n WHERE { ?e ex:name ?n . FILTER(?n != \"X\" || (?n <= \"Y\" && ?n >= \"Z\")) }", mapping);
	CHECK(sql.find("<>") != std::string::npos);
	CHECK(sql.find("<=") != std::string::npos);
	CHECK(sql.find(">=") != std::string::npos);
	CHECK(sql.find(" OR ") != std::string::npos);
	CHECK(sql.find(" AND ") != std::string::npos);
}

TEST_CASE("FILTER: arithmetic operators (+, -, *, /) translate via CAST/tryCastToDouble") {
	R2RMLParser mappingParser;
	R2RMLMapping mapping = mappingParser.parse(SOURCE_R2RML_DIR "example_emp_dept.ttl");
	std::string sql = translate("SELECT ?e ?n WHERE { ?e ex:name ?n . "
	                            "FILTER((?n + \"1\") != \"2\" || (?n - \"1\") != \"2\" || "
	                            "(?n * \"2\") != \"2\" || (?n / \"2\") != \"2\") }",
	                            mapping);
	CHECK(sql.find(" + ") != std::string::npos);
	CHECK(sql.find(" - ") != std::string::npos);
	CHECK(sql.find(" * ") != std::string::npos);
	CHECK(sql.find(" / ") != std::string::npos);
}

TEST_CASE("FILTER: unary NOT, +, and - translate to their SQL forms") {
	R2RMLParser mappingParser;
	R2RMLMapping mapping = mappingParser.parse(SOURCE_R2RML_DIR "example_emp_dept.ttl");
	std::string sql = translate(
	    "SELECT ?e ?n WHERE { ?e ex:name ?n . FILTER(!(?n = \"X\") || (+?n) != \"1\" || (-?n) != \"1\") }", mapping);
	CHECK(sql.find("NOT (") != std::string::npos);
	CHECK(sql.find("CAST((+") != std::string::npos);
	CHECK(sql.find("CAST((-") != std::string::npos);
}

TEST_CASE("FILTER: IN and NOT IN translate to SQL IN/NOT IN lists") {
	R2RMLParser mappingParser;
	R2RMLMapping mapping = mappingParser.parse(SOURCE_R2RML_DIR "example_emp_dept.ttl");
	std::string sql =
	    translate("SELECT ?e ?n WHERE { ?e ex:name ?n . FILTER(?n IN (\"A\", \"B\") || ?n NOT IN (\"C\")) }", mapping);
	CHECK(sql.find(" IN (") != std::string::npos);
	CHECK(sql.find(" NOT IN (") != std::string::npos);
}

TEST_CASE("FILTER: a bare IriRef operand translates to its lexical IRI value") {
	R2RMLParser mappingParser;
	R2RMLMapping mapping = mappingParser.parse(SOURCE_R2RML_DIR "example_emp_dept.ttl");
	std::string sql = translate("SELECT ?e ?n WHERE { ?e ex:name ?n . FILTER(?n != ex:foo) }", mapping);
	CHECK(sql.find("example.com/ns#foo") != std::string::npos);
}

TEST_CASE("FILTER EXISTS: correlates on multiple shared variables joined with AND") {
	R2RMLParser mappingParser;
	R2RMLMapping mapping = mappingParser.parse(SOURCE_R2RML_DIR "example_emp_dept.ttl");
	std::string sql = translate("SELECT ?e WHERE { ?e ex:name ?n . ?e ex:department ?d . "
	                            "FILTER EXISTS { ?e ex:department ?d } }",
	                            mapping);
	// Both correlated variables are bound on both sides, so each is a plain
	// equality (see below) and the two are ANDed together.
	CHECK(sql.find("\" AND ") != std::string::npos);
	CHECK(sql.find("\"v_d\" = ") != std::string::npos);
	CHECK(sql.find("\"v_e\" = ") != std::string::npos);
}

TEST_CASE("FILTER EXISTS: a correlation on variables bound on both sides is a plain equi-join") {
	R2RMLParser mappingParser;
	R2RMLMapping mapping = mappingParser.parse(SOURCE_R2RML_DIR "example_emp_dept.ttl");
	std::string sql = translate("SELECT ?e WHERE { ?e ex:name ?n . FILTER EXISTS { ?e ex:department ?d } }", mapping);
	// No null-tolerant disjunction: ?e cannot be unbound on either side, so the
	// dead IS NULL disjuncts are omitted - which is what keeps the correlated
	// subquery decorrelatable into a hash semi-join rather than a nested loop.
	std::size_t exists = sql.find("EXISTS (SELECT 1 FROM");
	REQUIRE(exists != std::string::npos);
	CHECK(sql.find("IS NULL", exists) == std::string::npos);
}

TEST_CASE("FILTER EXISTS: an OPTIONAL-lineage correlation keeps its null-tolerant disjunct") {
	R2RMLParser mappingParser;
	R2RMLMapping mapping = mappingParser.parse(SOURCE_R2RML_DIR "example_emp_dept.ttl");
	std::string sql = translate("SELECT ?e WHERE { ?e ex:name ?n . OPTIONAL { ?e ex:department ?d } "
	                            "FILTER EXISTS { ?d ex:location ?l } }",
	                            mapping);
	// ?d may be unbound in the outer solution, so it is not in that solution's
	// domain and must match any inner row.
	std::size_t exists = sql.find("EXISTS (SELECT 1 FROM");
	REQUIRE(exists != std::string::npos);
	CHECK(sql.find("IS NULL", exists) != std::string::npos);
}

TEST_CASE("FILTER: REGEX with a literal flags argument, and a non-literal flags argument throws") {
	R2RMLParser mappingParser;
	R2RMLMapping mapping = mappingParser.parse(SOURCE_R2RML_DIR "example_emp_dept.ttl");
	CHECK_NOTHROW(translate("SELECT ?e ?n WHERE { ?e ex:name ?n . FILTER(REGEX(?n, \"^S\", \"i\")) }", mapping));
	CHECK_THROWS_AS(translate("SELECT ?e ?n WHERE { ?e ex:name ?n . FILTER(REGEX(?n, \"^S\", ?n)) }", mapping),
	                TranslationError);
}

TEST_CASE("FILTER: SUBSTR with 2 and 3 arguments") {
	R2RMLParser mappingParser;
	R2RMLMapping mapping = mappingParser.parse(SOURCE_R2RML_DIR "example_emp_dept.ttl");
	CHECK(translate("SELECT ?e ?n WHERE { ?e ex:name ?n . FILTER(SUBSTR(?n, 1) != \"\") }", mapping).find("SUBSTR(") !=
	      std::string::npos);
	CHECK(
	    translate("SELECT ?e ?n WHERE { ?e ex:name ?n . FILTER(SUBSTR(?n, 1, 3) != \"\") }", mapping).find("SUBSTR(") !=
	    std::string::npos);
}

TEST_CASE("FILTER: STRBEFORE and STRAFTER translate via strpos/SUBSTR") {
	R2RMLParser mappingParser;
	R2RMLMapping mapping = mappingParser.parse(SOURCE_R2RML_DIR "example_emp_dept.ttl");
	CHECK(translate("SELECT ?e ?n WHERE { ?e ex:name ?n . FILTER(STRBEFORE(?n, \"S\") != \"\") }", mapping)
	          .find("strpos(") != std::string::npos);
	CHECK(translate("SELECT ?e ?n WHERE { ?e ex:name ?n . FILTER(STRAFTER(?n, \"S\") != \"\") }", mapping)
	          .find("strpos(") != std::string::npos);
}

TEST_CASE("FILTER: REPLACE with and without a flags argument") {
	R2RMLParser mappingParser;
	R2RMLMapping mapping = mappingParser.parse(SOURCE_R2RML_DIR "example_emp_dept.ttl");
	CHECK(translate("SELECT ?e ?n WHERE { ?e ex:name ?n . FILTER(REPLACE(?n, \"S\", \"X\") != \"\") }", mapping)
	          .find("regexp_replace(") != std::string::npos);
	CHECK(translate("SELECT ?e ?n WHERE { ?e ex:name ?n . FILTER(REPLACE(?n, \"S\", \"X\", \"i\") != \"\") }", mapping)
	          .find("regexp_replace(") != std::string::npos);
}

TEST_CASE("FILTER: numeric builtins ABS/CEIL/FLOOR/ROUND") {
	R2RMLParser mappingParser;
	R2RMLMapping mapping = mappingParser.parse(SOURCE_R2RML_DIR "example_emp_dept.ttl");
	std::string sql = translate("SELECT ?e ?n WHERE { ?e ex:name ?n . "
	                            "FILTER(ABS(?n) != \"0\" || CEIL(?n) != \"0\" || FLOOR(?n) != \"0\" || "
	                            "ROUND(?n) != \"0\") }",
	                            mapping);
	CHECK(sql.find("ABS(") != std::string::npos);
	CHECK(sql.find("CEIL(") != std::string::npos);
	CHECK(sql.find("FLOOR(") != std::string::npos);
	CHECK(sql.find("ROUND(") != std::string::npos);
}

TEST_CASE("BIND: COALESCE, IF, SAMETERM, and ISNUMERIC") {
	R2RMLParser mappingParser;
	R2RMLMapping mapping = mappingParser.parse(SOURCE_R2RML_DIR "example_emp_dept.ttl");
	std::string sql = translate("SELECT ?e ?c ?i ?st ?isnum WHERE { ?e ex:name ?n . "
	                            "BIND(COALESCE(?n, \"x\") AS ?c) "
	                            "BIND(IF(?n = \"A\", \"y\", \"n\") AS ?i) "
	                            "BIND(SAMETERM(?n, ?n) AS ?st) "
	                            "BIND(ISNUMERIC(?n) AS ?isnum) }",
	                            mapping);
	CHECK(sql.find("COALESCE(") != std::string::npos);
	CHECK(sql.find("CASE WHEN") != std::string::npos);
}

TEST_CASE("Aggregates: SUM/AVG/MIN/MAX/SAMPLE/GROUP_CONCAT with DISTINCT") {
	R2RMLParser mappingParser;
	R2RMLMapping mapping = mappingParser.parse(SOURCE_R2RML_DIR "example_emp_dept.ttl");
	std::string sql = translate("SELECT (SUM(?n) AS ?s) (AVG(?n) AS ?a) (MIN(?n) AS ?mn) (MAX(?n) AS ?mx) "
	                            "(SAMPLE(?n) AS ?sm) (GROUP_CONCAT(DISTINCT ?n; SEPARATOR=\",\") AS ?gc) "
	                            "WHERE { ?e ex:name ?n }",
	                            mapping);
	CHECK(sql.find("SUM(") != std::string::npos);
	CHECK(sql.find("AVG(") != std::string::npos);
	CHECK(sql.find("MIN(") != std::string::npos);
	CHECK(sql.find("MAX(") != std::string::npos);
	CHECK(sql.find("any_value(") != std::string::npos);
	CHECK(sql.find("string_agg(DISTINCT") != std::string::npos);
}
