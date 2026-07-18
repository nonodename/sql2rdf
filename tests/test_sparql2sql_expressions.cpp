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
