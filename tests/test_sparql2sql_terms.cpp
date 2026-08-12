/**
 * Structural tests for the consumers of the static RDF term dimension: the
 * term-kind builtins (isIRI/isBLANK/isLITERAL/LANG/DATATYPE/langMatches),
 * sameTerm's tightening, and STRDT/STRLANG's re-tagging.
 *
 * All against sparql2sql_terms.ttl, whose term maps are chosen so each
 * predicate pins one point of the lattice (see the fixture's header comment).
 * Execution correctness lives in tests/duckdb/; these assert only that the
 * right constant was folded in, which is all test_runner can see.
 *
 * Two properties are worth stating up front, because they are what the
 * negative cases here are really defending:
 *   - Unknown means "behave exactly as before term tracking", so a regression
 *     that loses an annotation shows up ONLY in the positive cases.
 *   - Whether a builtin throws is optimizer-dependent: filter pushdown can
 *     re-render a predicate against a single union arm where the kind IS known.
 *     The genuinely-Unknown case below is therefore built so that no pushdown
 *     can rescue it.
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
#include "sparql2sql/TypeCatalog.h"

using r2rml::R2RMLMapping;
using r2rml::R2RMLParser;
using sparql::Parser;
using sparql2sql::DuckDbDialect;
using sparql2sql::translateQuery;
using sparql2sql::TranslationError;
using sparql2sql::TypeCatalog;

namespace {

const char *const kPrefix = "PREFIX ex: <http://example.com/ns#>\n"
                            "PREFIX xsd: <http://www.w3.org/2001/XMLSchema#>\n";

std::string translate(const std::string &queryBody, const TypeCatalog *catalog = nullptr) {
	static R2RMLParser mappingParser;
	static R2RMLMapping mapping = mappingParser.parse(SOURCE_R2RML_DIR "sparql2sql_terms.ttl");
	Parser parser;
	auto q = parser.parseString(kPrefix + queryBody);
	DuckDbDialect dialect;
	return translateQuery(*q, mapping, dialect, catalog);
}

bool contains(const std::string &haystack, const std::string &needle) {
	return haystack.find(needle) != std::string::npos;
}

} // namespace

// --- isIRI / isBLANK / isLITERAL ---

TEST_CASE("isIRI: a template-generated subject folds to a constant TRUE", "[sparql2sql]") {
	CHECK(contains(translate("SELECT ?m WHERE { ?m ex:amount ?a . FILTER(isIRI(?m)) }"), "TRUE"));
}

TEST_CASE("isIRI: a known-literal object folds to FALSE", "[sparql2sql]") {
	CHECK(contains(translate("SELECT ?m WHERE { ?m ex:amount ?a . FILTER(isIRI(?a)) }"), "FALSE"));
}

TEST_CASE("isBLANK: an rr:BlankNode subject folds TRUE and a template IRI subject FALSE", "[sparql2sql]") {
	CHECK(contains(translate("SELECT ?n WHERE { ?n ex:body ?b . FILTER(isBLANK(?n)) }"), "TRUE"));
	CHECK(contains(translate("SELECT ?m WHERE { ?m ex:amount ?a . FILTER(isBLANK(?m)) }"), "FALSE"));
}

TEST_CASE("isLITERAL: a bare rr:column object folds TRUE even with an unknown datatype", "[sparql2sql]") {
	// The kind and the datatype are tracked independently: knowing it is a
	// literal does not require knowing which one.
	CHECK(contains(translate("SELECT ?m WHERE { ?m ex:plain ?p . FILTER(isLITERAL(?p)) }"), "TRUE"));
}

TEST_CASE("isIRI: a nullable known-IRI variable is NULL-guarded, not folded to a bare TRUE", "[sparql2sql]") {
	// SPARQL calls isIRI(unbound) an error; SQL NULL is this translator's
	// spelling of both, and a NULL in a FILTER drops the row - which is what
	// treat-an-error-as-false means.
	const std::string sql =
	    translate("SELECT ?m ?h WHERE { ?m ex:amount ?a . OPTIONAL { ?m ex:homepage ?h } FILTER(isIRI(?h)) }");
	CHECK(contains(sql, "IS NULL THEN NULL"));
}

TEST_CASE("term builtins: a genuinely unknown term kind still throws", "[sparql2sql]") {
	// COALESCE of an IRI-valued and a literal-valued variable meets to Unknown,
	// and a FILTER on a BIND's own output variable cannot be pushed below the
	// BIND that defines it - so this throw does not depend on optimizer choices.
	const std::string q = "SELECT ?x WHERE { ?m ex:homepage ?h . ?m ex:plain ?p . BIND(COALESCE(?h, ?p) AS ?x) ";
	CHECK_THROWS_AS(translate(q + "FILTER(isIRI(?x)) }"), TranslationError);
	CHECK_THROWS_AS(translate(q + "FILTER(isBLANK(?x)) }"), TranslationError);
	CHECK_THROWS_AS(translate(q + "FILTER(isLITERAL(?x)) }"), TranslationError);
	CHECK_THROWS_AS(translate(q + "FILTER(lang(?x) = \"\") }"), TranslationError);
	CHECK_THROWS_AS(translate(q + "FILTER(datatype(?x) = xsd:string) }"), TranslationError);
}

TEST_CASE("term builtins: disagreeing candidate arms are resolved per arm by pushdown", "[sparql2sql]") {
	// ex:mixed is an IRI from <#Measure> and a literal from <#Note>, so the
	// meet at the union is Unknown. But filter pushdown distributes the
	// predicate into each arm, where the kind IS known - so instead of throwing,
	// the IRI arm is folded away to FALSE and the literal arm to TRUE.
	//
	// This is why "does this builtin throw" is optimizer-dependent, and why the
	// negative case above is built to be pushdown-proof.
	const std::string sql = translate("SELECT ?x WHERE { ?s ex:mixed ?x . FILTER(isLITERAL(?x)) }");
	CHECK(contains(sql, "(FALSE)"));
	CHECK(contains(sql, "(TRUE)"));
	CHECK(contains(sql, "UNION"));
}

// --- LANG / DATATYPE ---

TEST_CASE("LANG: an rr:language literal folds to the static tag", "[sparql2sql]") {
	CHECK(contains(translate("SELECT ?t WHERE { ?m ex:title ?t . FILTER(lang(?t) = \"en\") }"), "'en'"));
}

TEST_CASE("LANG: a literal with no rr:language folds to the empty string", "[sparql2sql]") {
	CHECK(contains(translate("SELECT ?p WHERE { ?m ex:plain ?p . FILTER(lang(?p) = \"\") }"), "''"));
}

TEST_CASE("LANG: an IRI-valued argument is a static type error", "[sparql2sql]") {
	CHECK_THROWS_AS(translate("SELECT ?m WHERE { ?m ex:amount ?a . FILTER(lang(?m) = \"\") }"), TranslationError);
}

TEST_CASE("LANG: candidate arms tagged with different rr:language values still throw", "[sparql2sql]") {
	// ex:desc is a literal in both <#Measure> ("en") and <#Note> ("fr"), so the
	// meet at the union knows the kind AND the datatype (rdf:langString in both
	// arms) but not the specific tag - meet() degrades disagreeing langs to "".
	// Folding that empty string in would silently answer lang(?d) = "" for a
	// row that is actually @en or @fr, so this must throw instead - same
	// rationale as DATATYPE() refusing when only the kind is known. The BIND
	// makes this pushdown-proof, mirroring the Unknown-kind case above.
	const std::string q = "SELECT ?x WHERE { ?m ex:desc ?d . BIND(?d AS ?x) ";
	CHECK_THROWS_AS(translate(q + "FILTER(lang(?x) = \"\") }"), TranslationError);
}

TEST_CASE("LANG: disagreeing rr:language candidate arms are resolved per arm by pushdown", "[sparql2sql]") {
	// Without the BIND indirection above, filter pushdown distributes the
	// FILTER into each union arm, where the specific language IS known - so
	// instead of throwing, each arm folds to its own static tag.
	const std::string sql = translate("SELECT ?d WHERE { ?m ex:desc ?d . FILTER(lang(?d) = \"en\") }");
	CHECK(contains(sql, "'en'"));
	CHECK(contains(sql, "'fr'"));
	CHECK(contains(sql, "UNION"));
}

TEST_CASE("LANG: an rr:language arm meeting an untagged arm must not fold to the empty string", "[sparql2sql]") {
	// ex:mixedtag is @en in <#Measure> and a bare, untagged rr:column in <#Note>.
	// Their datatypeIri disagrees ("rdf:langString" vs unknown) as well as their
	// lang, so meet() degrades BOTH to "" - indistinguishable, by field value
	// alone, from ex:plain's "no rr:language anywhere" case.
	//
	// Bug reproduced here: the LANG() guard only fires when the merged
	// datatypeIri still reads rdf:langString, so this mixed case slips past it
	// and folds lang(?x) to '' unconditionally. That silently discards the
	// "en" tag on every row from the <#Measure> arm: FILTER(lang(?x) = "en")
	// always evaluates false (the row that really is @en never matches), which
	// is exactly the reported "@en tags are silently dropped" bug. The BIND
	// makes this pushdown-proof, mirroring the ex:desc LANG throw test above.
	const std::string q = "SELECT ?x WHERE { ?m ex:mixedtag ?d . BIND(?d AS ?x) ";
	CHECK_THROWS_AS(translate(q + "FILTER(lang(?x) = \"en\") }"), TranslationError);
}

TEST_CASE("DATATYPE: a declared rr:datatype folds to that IRI", "[sparql2sql]") {
	CHECK(contains(translate("SELECT ?a WHERE { ?m ex:amount ?a . FILTER(datatype(?a) = xsd:integer) }"),
	               "'http://www.w3.org/2001/XMLSchema#integer'"));
	CHECK(contains(translate("SELECT ?p WHERE { ?m ex:price ?p . FILTER(datatype(?p) = xsd:decimal) }"),
	               "'http://www.w3.org/2001/XMLSchema#decimal'"));
}

TEST_CASE("DATATYPE: an rr:language literal folds to rdf:langString", "[sparql2sql]") {
	CHECK(contains(translate("SELECT ?t WHERE { ?m ex:title ?t . FILTER(datatype(?t) = xsd:string) }"),
	               "'http://www.w3.org/1999/02/22-rdf-syntax-ns#langString'"));
}

TEST_CASE("DATATYPE: a literal with no declared datatype and no catalog still throws", "[sparql2sql]") {
	// Guessing xsd:string here would contradict R2RML's own natural mapping,
	// which derives a datatype from the column's SQL type. This is the case
	// that keeps a meaningful negative alive after the feature lands.
	CHECK_THROWS_AS(translate("SELECT ?p WHERE { ?m ex:plain ?p . FILTER(datatype(?p) = xsd:string) }"),
	                TranslationError);
}

TEST_CASE("DATATYPE: a TypeCatalog supplies the Section 10.2 natural-mapping datatype", "[sparql2sql]") {
	// The one consumer-side test of the catalog inference path: ex:rawamount is
	// a bare rr:column over MEASURES.AMOUNT, so with a catalog its datatype is
	// derivable even though the mapping declares none.
	TypeCatalog catalog;
	catalog.columnTypes["MEASURES"]["AMOUNT"] = "INTEGER";
	CHECK(contains(translate("SELECT ?r WHERE { ?m ex:rawamount ?r . FILTER(datatype(?r) = xsd:integer) }", &catalog),
	               "'http://www.w3.org/2001/XMLSchema#integer'"));
	// ...and without one it still refuses.
	CHECK_THROWS_AS(translate("SELECT ?r WHERE { ?m ex:rawamount ?r . FILTER(datatype(?r) = xsd:integer) }"),
	                TranslationError);
}

// --- langMatches ---

TEST_CASE("langMatches: a constant range emits a case-insensitive prefix test", "[sparql2sql]") {
	const std::string sql = translate("SELECT ?t WHERE { ?m ex:title ?t . FILTER(langMatches(lang(?t), \"en\")) }");
	CHECK(contains(sql, "LOWER("));
	CHECK(contains(sql, "STARTS_WITH("));
	CHECK(contains(sql, "'en-'"));
}

TEST_CASE("langMatches: the wildcard range only tests for a non-empty tag", "[sparql2sql]") {
	const std::string sql = translate("SELECT ?t WHERE { ?m ex:title ?t . FILTER(langMatches(lang(?t), \"*\")) }");
	CHECK(contains(sql, "<> ''"));
	CHECK_FALSE(contains(sql, "STARTS_WITH("));
}

TEST_CASE("langMatches: a non-constant range throws", "[sparql2sql]") {
	CHECK_THROWS_AS(
	    translate("SELECT ?t WHERE { ?m ex:title ?t . ?m ex:plain ?p . FILTER(langMatches(lang(?t), ?p)) }"),
	    TranslationError);
}

// --- sameTerm ---

TEST_CASE("sameTerm: two variables of different known kinds fold to FALSE", "[sparql2sql]") {
	const std::string sql =
	    translate("SELECT ?m WHERE { ?m ex:homepage ?h . ?m ex:plain ?p . FILTER(sameTerm(?h, ?p)) }");
	CHECK(contains(sql, "FALSE"));
}

TEST_CASE("sameTerm: two literals of different declared datatypes fold to FALSE", "[sparql2sql]") {
	const std::string sql =
	    translate("SELECT ?m WHERE { ?m ex:amount ?a . ?m ex:price ?p . FILTER(sameTerm(?a, ?p)) }");
	CHECK(contains(sql, "FALSE"));
}

TEST_CASE("sameTerm: two terms of the same known kind keep the string equality", "[sparql2sql]") {
	const std::string sql =
	    translate("SELECT ?m WHERE { ?m ex:plain ?p . ?m ex:rawamount ?r . FILTER(sameTerm(?p, ?r)) }");
	CHECK_FALSE(contains(sql, "FALSE"));
	CHECK(contains(sql, " = "));
}

TEST_CASE("sameTerm: an unknown language tag from a union meet does not fold to FALSE", "[sparql2sql]") {
	// ex:desc is @en in one union arm and @fr in the other, so meet() degrades
	// the language to "" (unknown) while kind and datatype (rdf:langString)
	// stay known - the same case the LANG throw test above exercises. An
	// unknown lang is not "known to differ" from ex:title's known @en tag, so
	// this must fall through to a real string comparison instead of folding to
	// FALSE, which would silently drop rows where ?x is actually @en and equal
	// to ?t. The BIND makes this pushdown-proof, mirroring the LANG test.
	const std::string q = "SELECT ?x WHERE { ?m ex:desc ?d . ?m ex:title ?t . BIND(?d AS ?x) ";
	const std::string sql = translate(q + "FILTER(sameTerm(?x, ?t)) }");
	CHECK_FALSE(contains(sql, "FALSE"));
	CHECK(contains(sql, " = "));
}

TEST_CASE("sameTerm: a nullable known-differing-kind operand is NULL-guarded, not folded to a bare FALSE",
          "[sparql2sql]") {
	// Mirrors the isIRI nullable case above: sameTerm(?h, ?p) is statically
	// FALSE when bound (IRI vs Literal never match), but ?h can be unbound via
	// the OPTIONAL, and SPARQL's sameTerm(unbound, x) must not evaluate to a
	// bare true/false - it has to fall through to NULL so the FILTER drops the
	// row, exactly like the untyped comparison path already does.
	const std::string sql =
	    translate("SELECT ?m ?h WHERE { ?m ex:plain ?p . OPTIONAL { ?m ex:homepage ?h } FILTER(sameTerm(?h, ?p)) }");
	CHECK(contains(sql, "IS NULL THEN NULL"));
}

TEST_CASE("comparison: a nullable known-differing-kind operand is NULL-guarded, not folded to a bare FALSE",
          "[sparql2sql]") {
	// Same gap as above, but for `=`/`<>`'s own kind-mismatch fold in
	// comparison(): folding straight to a bare boolean would make
	// `FILTER(?h = ?p)` keep rows where ?h is unbound, diverging from
	// SPARQL's treat-an-error-as-false semantics.
	const std::string sql =
	    translate("SELECT ?m ?h WHERE { ?m ex:plain ?p . OPTIONAL { ?m ex:homepage ?h } FILTER(?h = ?p) }");
	CHECK(contains(sql, "IS NULL THEN NULL"));
}

// --- STRDT / STRLANG ---

TEST_CASE("STRDT: a constant datatype IRI translates as a lexical pass-through", "[sparql2sql]") {
	// STRDT emits no SQL of its own - the physical representation already IS the
	// lexical form. Its whole effect is on the expression's *annotation*; the
	// downstream consequence (a statically typed comparison) is asserted in
	// test_sparql2sql_typed_exprs.cpp.
	const std::string sql = translate("SELECT ?m WHERE { ?m ex:rawamount ?r . FILTER(STRDT(?r, xsd:integer) > 50) }");
	CHECK(contains(sql, "\"AMOUNT\""));
	CHECK(contains(sql, "50"));
}

TEST_CASE("STRDT: a non-constant datatype argument throws", "[sparql2sql]") {
	CHECK_THROWS_AS(translate("SELECT ?m WHERE { ?m ex:plain ?p . ?m ex:homepage ?h . FILTER(STRDT(?p, ?h) = \"x\") }"),
	                TranslationError);
}

TEST_CASE("STRLANG: a constant tag types the result so LANG() folds to it", "[sparql2sql]") {
	const std::string sql =
	    translate("SELECT ?m WHERE { ?m ex:plain ?p . BIND(STRLANG(?p, \"fr\") AS ?l) FILTER(lang(?l) = \"fr\") }");
	CHECK(contains(sql, "'fr'"));
}

TEST_CASE("STRLANG: a non-constant language argument throws", "[sparql2sql]") {
	CHECK_THROWS_AS(translate("SELECT ?m WHERE { ?m ex:plain ?p . ?m ex:title ?t . FILTER(STRLANG(?p, ?t) = \"x\") }"),
	                TranslationError);
}
