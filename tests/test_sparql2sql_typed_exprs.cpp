/**
 * Structural tests for the *typed* consumers of the static RDF term dimension:
 * comparison, arithmetic, ORDER BY and MIN/MAX specialised by whatever the
 * R2RML mapping proves about the operands' datatypes.
 *
 * Against sparql2sql_terms.ttl, whose predicates pin one datatype each. Every
 * positive case is paired with an unknown-datatype sibling asserting the
 * pre-existing untyped form survives verbatim: "Unknown behaves exactly as
 * before" is the whole safety argument for this feature, and it is the one
 * property no other test in the suite can notice breaking.
 *
 * Whether the *rows* are right is settled by tests/duckdb/, not here.
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

namespace {

const char *const kPrefix = "PREFIX ex: <http://example.com/ns#>\n"
                            "PREFIX xsd: <http://www.w3.org/2001/XMLSchema#>\n";

std::string translate(const std::string &queryBody) {
	static R2RMLParser mappingParser;
	static R2RMLMapping mapping = mappingParser.parse(SOURCE_R2RML_DIR "sparql2sql_terms.ttl");
	Parser parser;
	auto q = parser.parseString(kPrefix + queryBody);
	DuckDbDialect dialect;
	return translateQuery(*q, mapping, dialect);
}

bool contains(const std::string &haystack, const std::string &needle) {
	return haystack.find(needle) != std::string::npos;
}

} // namespace

// --- Comparison ---

TEST_CASE("comparison: two known-numeric operands drop the string fallback", "[sparql2sql]") {
	const std::string sql = translate("SELECT ?m WHERE { ?m ex:amount ?a . FILTER(?a > 50) }");
	CHECK_FALSE(contains(sql, "CASE WHEN"));
	CHECK(contains(sql, "TRY_CAST"));
	CHECK(contains(sql, "AS DOUBLE"));
}

TEST_CASE("comparison: an unknown-datatype operand keeps the numeric-aware fallback", "[sparql2sql]") {
	// The no-regression anchor. ex:plain is a bare rr:column and no TypeCatalog
	// is supplied, so nothing is known and the old shape must survive.
	const std::string sql = translate("SELECT ?m WHERE { ?m ex:plain ?p . FILTER(?p > 50) }");
	CHECK(contains(sql, "CASE WHEN"));
	CHECK(contains(sql, "IS NOT NULL AND"));
}

TEST_CASE("comparison: two known xsd:string operands emit a bare VARCHAR comparison", "[sparql2sql]") {
	// STR() yields xsd:string and so does a plain string literal, so a VARCHAR
	// comparison is already exactly right - no cast and no fallback wrapper.
	const std::string sql = translate("SELECT ?m WHERE { ?m ex:plain ?p . FILTER(STR(?p) = \"alpha\") }");
	CHECK_FALSE(contains(sql, "CASE WHEN"));
	CHECK_FALSE(contains(sql, "TRY_CAST"));
}

TEST_CASE("comparison: a language-tagged literal is not the same type as a plain string", "[sparql2sql]") {
	// rdf:langString and xsd:string are different datatypes, so this does NOT
	// take the bare-VARCHAR path - it falls through to the untyped form rather
	// than asserting an equivalence RDF does not grant.
	const std::string sql = translate("SELECT ?m WHERE { ?m ex:title ?t . FILTER(?t = \"alpha\") }");
	CHECK(contains(sql, "CASE WHEN"));
}

TEST_CASE("comparison: two xsd:dateTime operands compare as timestamps", "[sparql2sql]") {
	// More than a size win: lexicographic dateTime comparison is wrong across
	// UTC offsets and non-canonical forms.
	const std::string sql =
	    translate("SELECT ?m WHERE { ?m ex:when ?w . FILTER(?w < \"2020-01-01T00:00:00\"^^xsd:dateTime) }");
	CHECK(contains(sql, "AS TIMESTAMP"));
	CHECK_FALSE(contains(sql, "CASE WHEN"));
}

TEST_CASE("comparison: an IRI-kind operand equated with a literal-kind one folds", "[sparql2sql]") {
	CHECK(contains(translate("SELECT ?m WHERE { ?m ex:homepage ?h . ?m ex:plain ?p . FILTER(?h = ?p) }"), "FALSE"));
	CHECK(contains(translate("SELECT ?m WHERE { ?m ex:homepage ?h . ?m ex:plain ?p . FILTER(?h != ?p) }"), "TRUE"));
}

TEST_CASE("comparison: an IRI ordered against a literal is NOT folded", "[sparql2sql]") {
	// Guards the =/<>-only restriction: a cross-kind ordering comparison is a
	// genuine SPARQL type error, so answering FALSE would be wrong, not merely
	// imprecise. It must fall through to the untyped form.
	const std::string sql = translate("SELECT ?m WHERE { ?m ex:homepage ?h . ?m ex:plain ?p . FILTER(?h < ?p) }");
	CHECK(contains(sql, "CASE WHEN"));
}

TEST_CASE("comparison: STRDT re-tags an untyped column enough to type the comparison", "[sparql2sql]") {
	// The escape hatch for mappings that declare no rr:datatype.
	const std::string sql = translate("SELECT ?m WHERE { ?m ex:rawamount ?r . FILTER(STRDT(?r, xsd:integer) > 50) }");
	CHECK_FALSE(contains(sql, "CASE WHEN"));
	CHECK(contains(sql, "AS DOUBLE"));
}

// --- Arithmetic ---

TEST_CASE("arithmetic: two xsd:integer operands stay integral", "[sparql2sql]") {
	// Without this, ?a + 1 renders "10.0" - the DOUBLE round-trip is visible in
	// the output text, not just the intermediate.
	const std::string sql = translate("SELECT ?s WHERE { ?m ex:amount ?a . BIND(?a + 1 AS ?s) }");
	CHECK(contains(sql, "AS BIGINT"));
	CHECK_FALSE(contains(sql, "AS DOUBLE"));
}

TEST_CASE("arithmetic: division always goes through DOUBLE even for integral operands", "[sparql2sql]") {
	const std::string sql = translate("SELECT ?s WHERE { ?m ex:amount ?a . BIND(?a / 2 AS ?s) }");
	CHECK(contains(sql, "AS DOUBLE"));
}

TEST_CASE("arithmetic: an unknown-datatype operand keeps the DOUBLE path", "[sparql2sql]") {
	const std::string sql = translate("SELECT ?s WHERE { ?m ex:plain ?p . BIND(?p + 1 AS ?s) }");
	CHECK(contains(sql, "AS DOUBLE"));
	CHECK_FALSE(contains(sql, "AS BIGINT"));
}

TEST_CASE("arithmetic: a decimal operand is numeric but not integral", "[sparql2sql]") {
	const std::string sql = translate("SELECT ?s WHERE { ?m ex:price ?p . BIND(?p + 1 AS ?s) }");
	CHECK(contains(sql, "AS DOUBLE"));
	CHECK_FALSE(contains(sql, "AS BIGINT"));
}

// --- ORDER BY ---

TEST_CASE("ORDER BY: a known-integral key sorts as BIGINT", "[sparql2sql]") {
	const std::string sql = translate("SELECT ?a WHERE { ?m ex:amount ?a } ORDER BY ?a");
	CHECK(contains(sql, "ORDER BY TRY_CAST("));
	CHECK(contains(sql, "AS BIGINT"));
}

TEST_CASE("ORDER BY: a known-dateTime key sorts as a timestamp", "[sparql2sql]") {
	const std::string sql = translate("SELECT ?w WHERE { ?m ex:when ?w } ORDER BY ?w");
	CHECK(contains(sql, "ORDER BY TRY_CAST("));
	CHECK(contains(sql, "AS TIMESTAMP"));
}

TEST_CASE("ORDER BY: an unknown-datatype key keeps lexicographic ordering", "[sparql2sql]") {
	const std::string sql = translate("SELECT ?p WHERE { ?m ex:plain ?p } ORDER BY ?p");
	CHECK_FALSE(contains(sql, "ORDER BY TRY_CAST("));
}

TEST_CASE("ORDER BY: an IRI-valued key is not cast", "[sparql2sql]") {
	const std::string sql = translate("SELECT ?m WHERE { ?m ex:amount ?a } ORDER BY ?m");
	CHECK_FALSE(contains(sql, "ORDER BY TRY_CAST("));
}

// --- Aggregates ---

TEST_CASE("MIN/MAX: a known-integral argument aggregates numerically", "[sparql2sql]") {
	const std::string sql = translate("SELECT (MIN(?a) AS ?mn) (MAX(?a) AS ?mx) WHERE { ?m ex:amount ?a }");
	CHECK(contains(sql, "MIN(TRY_CAST("));
	CHECK(contains(sql, "MAX(TRY_CAST("));
	CHECK(contains(sql, "AS BIGINT"));
}

TEST_CASE("MIN/MAX: an unknown-datatype argument stays lexicographic", "[sparql2sql]") {
	const std::string sql = translate("SELECT (MIN(?p) AS ?mn) WHERE { ?m ex:plain ?p }");
	CHECK(contains(sql, "MIN("));
	CHECK_FALSE(contains(sql, "MIN(TRY_CAST("));
}

TEST_CASE("MIN/MAX: a dateTime argument is canonicalised back to XSD's 'T' form", "[sparql2sql]") {
	const std::string sql = translate("SELECT (MIN(?w) AS ?mn) WHERE { ?m ex:when ?w }");
	CHECK(contains(sql, "AS TIMESTAMP"));
	CHECK(contains(sql, "' ', 'T'"));
}

TEST_CASE("SUM: an integral argument keeps an integral lexical form", "[sparql2sql]") {
	const std::string sql = translate("SELECT (SUM(?a) AS ?s) WHERE { ?m ex:amount ?a }");
	CHECK(contains(sql, "SUM(TRY_CAST("));
	CHECK(contains(sql, "AS BIGINT"));
}

TEST_CASE("AVG and COUNT are unaffected by the argument's datatype", "[sparql2sql]") {
	// AVG is always DOUBLE (an average of integers is not an integer), and
	// COUNT never looks at its argument's type at all.
	CHECK(contains(translate("SELECT (AVG(?a) AS ?s) WHERE { ?m ex:amount ?a }"), "AS DOUBLE"));
	const std::string countSql = translate("SELECT (COUNT(?a) AS ?c) WHERE { ?m ex:amount ?a }");
	CHECK(contains(countSql, "COUNT("));
	CHECK_FALSE(contains(countSql, "COUNT(TRY_CAST("));
}
