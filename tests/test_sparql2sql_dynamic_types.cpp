/**
 * Structural tests for the *runtime* RDF term dimension: the type-tag column
 * (mangleVarTag / TagSql.h) that lets the term-kind builtins, RDF term
 * equality, SPARQL value comparison and Section 15.1 ordering be evaluated per
 * row instead of resolved at translation time.
 *
 * Two halves, and the first is the more important one:
 *
 *   1. NOTHING materialises a tag unless it has to. A query whose types the
 *      R2RML mapping determines generates SQL with no `d_` column anywhere and
 *      no runtime dispatch - byte for byte what it generated before tags
 *      existed. That is what makes the rest of this suite (especially
 *      test_sparql2sql_terms.cpp and test_sparql2sql_typed_exprs.cpp) a
 *      regression harness for this feature rather than a casualty of it.
 *
 *   2. Where the mapping genuinely cannot decide - the multi-arm predicates
 *      ex:mixed / ex:desc / ex:mixedtag / ex:mixeddt in sparql2sql_terms.ttl -
 *      the tag appears and is consulted, instead of the query being refused.
 *
 * As everywhere in test_runner, these assert only on the *shape* of the
 * generated SQL. That dynamic typing produces the right ROWS is proved in
 * tests/duckdb/test_sparql2sql_duckdb.cpp, which is the only place that can
 * see them.
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
#include "sparql2sql/TagSql.h"
#include "sparql2sql/TermInfo.h"
#include "sparql2sql/TranslationError.h"
#include "sparql2sql/Translator.h"

using r2rml::R2RMLMapping;
using r2rml::R2RMLParser;
using sparql::Parser;
using sparql2sql::decodeTag;
using sparql2sql::DuckDbDialect;
using sparql2sql::encodeTag;
using sparql2sql::isFullyDetermined;
using sparql2sql::kRdfLangString;
using sparql2sql::meet;
using sparql2sql::RdfTermKind;
using sparql2sql::TermInfo;
using sparql2sql::translateQuery;
using sparql2sql::TranslationError;

namespace {

const char *const kPrefix = "PREFIX ex: <http://example.com/ns#>\n"
                            "PREFIX xsd: <http://www.w3.org/2001/XMLSchema#>\n"
                            "PREFIX rdf: <http://www.w3.org/1999/02/22-rdf-syntax-ns#>\n";

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

// Whether the generated SQL materialises a runtime tag column at all. The `d_`
// prefix is unique to mangleVarTag (mangleVar uses `v_`, the renderer's hidden
// native join keys use `k_`), so its absence is a reliable "no tags here".
bool hasAnyTagColumn(const std::string &sql) {
	return contains(sql, "\"d_");
}

TermInfo lit(const char *datatype = "", const char *lang = "") {
	TermInfo t;
	t.kind = RdfTermKind::Literal;
	t.datatypeIri = datatype;
	t.lang = lang;
	if (lang[0] != '\0') {
		t.maybeLangTagged = true;
	}
	return t;
}

} // namespace

// ---------------------------------------------------------------------------
// 1. Demand-driven materialisation
// ---------------------------------------------------------------------------

TEST_CASE("tags: a query whose types the mapping determines materialises none", "[sparql2sql][dynamic-types]") {
	// Every one of these inspects a term dimension, and every one is answered
	// statically, so not one of them should reach for a tag column.
	CHECK_FALSE(hasAnyTagColumn(translate("SELECT ?a WHERE { ?m ex:amount ?a . FILTER(?a > 50) }")));
	CHECK_FALSE(hasAnyTagColumn(translate("SELECT ?t WHERE { ?m ex:title ?t . FILTER(lang(?t) = \"en\") }")));
	CHECK_FALSE(hasAnyTagColumn(translate("SELECT ?a WHERE { ?m ex:amount ?a . FILTER(datatype(?a) = xsd:integer) }")));
	CHECK_FALSE(hasAnyTagColumn(translate("SELECT ?h WHERE { ?m ex:homepage ?h . FILTER(isIRI(?h)) }")));
	CHECK_FALSE(hasAnyTagColumn(translate("SELECT ?a WHERE { ?m ex:amount ?a } ORDER BY ?a")));
	CHECK_FALSE(hasAnyTagColumn(translate("SELECT DISTINCT ?a WHERE { ?m ex:amount ?a }")));
}

TEST_CASE("tags: an undeclared-datatype literal is a determined dimension, not an undetermined one",
          "[sparql2sql][dynamic-types]") {
	// ex:rawamount is a bare rr:column with no rr:datatype and no TypeCatalog:
	// the mapping genuinely cannot determine the datatype, which is itself a
	// definite fact ("L"). So there is nothing a per-row tag could add, and
	// `FILTER(?r > 50)` must keep the untyped numeric-aware comparison it has
	// always had rather than being routed through a dispatch that would only
	// land back on the same fallback.
	const std::string sql = translate("SELECT ?r WHERE { ?m ex:rawamount ?r . FILTER(?r > 50) }");
	CHECK_FALSE(hasAnyTagColumn(sql));
	CHECK(contains(sql, "TRY_CAST"));
}

TEST_CASE("tags: a query the mapping cannot decide materialises one", "[sparql2sql][dynamic-types]") {
	// Each of these routes the value through a BIND before inspecting it. That is
	// not incidental: without it, filter pushdown distributes the predicate into
	// each union arm, where the dimension IS statically known, and folds it - so
	// no tag is needed and none is emitted (see the test below). A FILTER on a
	// BIND's own output cannot be pushed below the BIND that defines it, which is
	// what leaves the dimension genuinely undecidable at translation time.
	CHECK(hasAnyTagColumn(translate("SELECT ?y WHERE { ?m ex:mixed ?x . BIND(?x AS ?y) FILTER(isLITERAL(?y)) }")));
	CHECK(hasAnyTagColumn(translate("SELECT ?y WHERE { ?m ex:desc ?d . BIND(?d AS ?y) FILTER(lang(?y) = \"fr\") }")));
	CHECK(hasAnyTagColumn(
	    translate("SELECT ?y WHERE { ?m ex:mixeddt ?v . BIND(?v AS ?y) FILTER(datatype(?y) = xsd:string) }")));
}

TEST_CASE("tags: a predicate filter pushdown can resolve per arm still needs none", "[sparql2sql][dynamic-types]") {
	// The pay-off of deciding expression tag demand *after* optimize(): pushdown
	// re-renders this predicate against each union arm, where ex:mixed is an IRI in
	// one and a literal in the other, folding it to FALSE and TRUE respectively.
	// Nothing is left to decide per row, so nothing is materialised.
	const std::string sql = translate("SELECT ?x WHERE { ?m ex:mixed ?x . FILTER(isLITERAL(?x)) }");
	CHECK_FALSE(hasAnyTagColumn(sql));
	CHECK(contains(sql, "(FALSE)"));
	CHECK(contains(sql, "(TRUE)"));
}

TEST_CASE("tags: only the variables that need one get one", "[sparql2sql][dynamic-types]") {
	// ?x's dimension is undetermined and inspected; ?a's is determined and also
	// inspected. Exactly one tag column should exist.
	const std::string sql = translate("SELECT ?y ?a WHERE { ?m ex:mixed ?x . ?m ex:amount ?a . BIND(?x AS ?y) "
	                                  "FILTER(isIRI(?y) && ?a > 1) }");
	CHECK(contains(sql, "\"d_y\""));
	CHECK_FALSE(contains(sql, "\"d_a\""));
}

// ---------------------------------------------------------------------------
// 2. The term-kind builtins, evaluated per row
// ---------------------------------------------------------------------------

TEST_CASE("isLITERAL over disagreeing arms tests the tag instead of refusing", "[sparql2sql][dynamic-types]") {
	// Before tags this threw: the arms' meet is Unknown, and the kind was only
	// ever resolvable at translation time.
	const std::string sql = translate("SELECT ?y WHERE { ?m ex:mixed ?x . BIND(?x AS ?y) FILTER(isLITERAL(?y)) }");
	CHECK(contains(sql, "NOT IN ('I', 'B')"));
}

TEST_CASE("isIRI over disagreeing arms compares the tag to the IRI tag", "[sparql2sql][dynamic-types]") {
	const std::string sql = translate("SELECT ?y WHERE { ?m ex:mixed ?x . BIND(?x AS ?y) FILTER(isIRI(?y)) }");
	CHECK(contains(sql, "= 'I')"));
}

TEST_CASE("LANG over arms with different rr:language reads the tag", "[sparql2sql][dynamic-types]") {
	// ex:desc is @en in one arm and @fr in the other. Previously an outright
	// refusal; now a per-row SUBSTR of the tag.
	const std::string sql = translate("SELECT ?y WHERE { ?m ex:desc ?d . BIND(?d AS ?y) FILTER(lang(?y) = \"fr\") }");
	CHECK(contains(sql, "SUBSTR("));
	CHECK(contains(sql, "'@'"));
}

TEST_CASE("LANG over a tagged arm meeting an untagged one still does not answer the empty string",
          "[sparql2sql][dynamic-types]") {
	// ex:mixedtag: one arm @en, the other declares nothing. The old code needed
	// TermInfo::maybeLangTagged to avoid folding "" here; the tag now answers
	// per row, and must distinguish the untagged arm's '' from the @en arm's
	// 'en' rather than claiming either for both.
	const std::string sql = translate("SELECT ?y WHERE { ?m ex:mixedtag ?g . BIND(?g AS ?y) FILTER(lang(?y) = \"\") }");
	CHECK(hasAnyTagColumn(sql));
	CHECK(contains(sql, "'@en'"));
	CHECK(contains(sql, "'L'"));
}

TEST_CASE("DATATYPE over arms with different rr:datatype reads the tag", "[sparql2sql][dynamic-types]") {
	const std::string sql =
	    translate("SELECT ?y WHERE { ?m ex:mixeddt ?v . BIND(?v AS ?y) FILTER(datatype(?y) = xsd:string) }");
	CHECK(hasAnyTagColumn(sql));
	// Both arms' constants are projected, one per arm, and the consumer strips
	// the 'D' prefix back off at read time.
	CHECK(contains(sql, "'Dhttp://www.w3.org/2001/XMLSchema#integer'"));
	CHECK(contains(sql, "'Dhttp://www.w3.org/2001/XMLSchema#string'"));
	CHECK(contains(sql, "SUBSTR("));
}

TEST_CASE("DATATYPE over an undeclared literal yields NULL rather than refusing", "[sparql2sql][dynamic-types]") {
	// ex:plain is a literal whose datatype the mapping does not determine. There
	// is no correct IRI to answer, so DATATYPE is a type error - SQL NULL, which
	// the FILTER drops. Refusing to translate the whole query was strictly worse.
	const std::string sql = translate("SELECT ?p WHERE { ?m ex:plain ?p . FILTER(datatype(?p) = xsd:string) }");
	CHECK(contains(sql, "ELSE NULL END"));
}

TEST_CASE("isNUMERIC folds to FALSE for a known non-numeric datatype", "[sparql2sql][dynamic-types]") {
	// The cast test alone would answer TRUE for the string "42"; the declared
	// datatype settles it without touching data.
	const std::string sql = translate("SELECT ?t WHERE { ?m ex:title ?t . FILTER(isNUMERIC(?t)) }");
	CHECK(contains(sql, "FALSE"));
}

TEST_CASE("isNUMERIC keeps the cast test when the datatype is undeclared", "[sparql2sql][dynamic-types]") {
	const std::string sql = translate("SELECT ?r WHERE { ?m ex:rawamount ?r . FILTER(isNUMERIC(?r)) }");
	CHECK(contains(sql, "TRY_CAST"));
	CHECK_FALSE(hasAnyTagColumn(sql));
}

// ---------------------------------------------------------------------------
// 3. RDF term equality and SPARQL value comparison
// ---------------------------------------------------------------------------

TEST_CASE("sameTerm compares the tag as well as the lexical form", "[sparql2sql][dynamic-types]") {
	// This is the correctness fix: string equality alone would match an IRI
	// against a literal whose text happens to equal it.
	const std::string sql = translate("SELECT ?p WHERE { ?m ex:mixed ?x . ?m ex:desc ?d . BIND(?x AS ?p) "
	                                  "BIND(?d AS ?q) FILTER(sameTerm(?p, ?q)) }");
	CHECK(contains(sql, "\"d_p\""));
	CHECK(contains(sql, "\"d_q\""));
}

TEST_CASE("sameTerm stays plain string equality when the mapping proves the dimensions agree",
          "[sparql2sql][dynamic-types]") {
	const std::string sql =
	    translate("SELECT ?a WHERE { ?m ex:amount ?a . ?n ex:amount ?b . FILTER(sameTerm(?a, ?b)) }");
	CHECK_FALSE(hasAnyTagColumn(sql));
}

TEST_CASE("equality over undetermined operands dispatches on both value spaces", "[sparql2sql][dynamic-types]") {
	const std::string sql = translate("SELECT ?p WHERE { ?m ex:mixeddt ?v . ?m ex:desc ?d . BIND(?v AS ?p) "
	                                  "BIND(?d AS ?q) FILTER(?p = ?q) }");
	CHECK(contains(sql, "\"d_p\""));
	CHECK(contains(sql, "\"d_q\""));
	// Two terms in different value spaces are not the same RDF term: a definite
	// FALSE, not a type error.
	CHECK(contains(sql, "THEN FALSE"));
}

TEST_CASE("an ordering comparison across value spaces is a type error, not a false", "[sparql2sql][dynamic-types]") {
	// Section 17.3's operator table defines < only within a value space.
	const std::string sql = translate("SELECT ?p WHERE { ?m ex:mixeddt ?v . ?m ex:desc ?d . BIND(?v AS ?p) "
	                                  "BIND(?d AS ?q) FILTER(?p < ?q) }");
	CHECK(contains(sql, "THEN NULL"));
}

TEST_CASE("a dynamic comparison still falls back to the untyped form for an undeterminable datatype",
          "[sparql2sql][dynamic-types]") {
	// ex:mixeddt's tag varies per row, and one of its possible values is a
	// datatype outside the four orderable value spaces. The dispatch must keep
	// the pre-tag numeric-aware comparison as its own last resort.
	const std::string sql = translate("SELECT ?y WHERE { ?m ex:mixeddt ?v . BIND(?v AS ?y) FILTER(?y > 50) }");
	CHECK(contains(sql, "TRY_CAST"));
	CHECK(contains(sql, "\"d_y\""));
}

// ---------------------------------------------------------------------------
// 4. Ordering, dedup and construction
// ---------------------------------------------------------------------------

TEST_CASE("ORDER BY over an undetermined dimension expands to the Section 15.1 layered key",
          "[sparql2sql][dynamic-types]") {
	const std::string sql = translate("SELECT ?x WHERE { ?m ex:mixed ?x } ORDER BY ?x");
	// Term-kind rank first (unbound < blank < IRI < literal), then value space,
	// then the value read as each orderable type, then the lexical form.
	CHECK(contains(sql, "THEN 1 WHEN"));
	CHECK(contains(sql, "TRY_CAST"));
	CHECK(contains(sql, "\"d_x\""));
}

TEST_CASE("ORDER BY over a determined dimension keeps its single typed key", "[sparql2sql][dynamic-types]") {
	const std::string sql = translate("SELECT ?a WHERE { ?m ex:amount ?a } ORDER BY ?a");
	CHECK(contains(sql, "ORDER BY TRY_CAST"));
	CHECK_FALSE(hasAnyTagColumn(sql));
}

TEST_CASE("DISTINCT includes the tag so differently-typed equal lexical forms stay distinct",
          "[sparql2sql][dynamic-types]") {
	// Without the tag in the dedup key, "1"^^xsd:integer and "1"^^xsd:string -
	// two different RDF terms, hence two different solutions - would collapse.
	const std::string sql = translate("SELECT DISTINCT ?v WHERE { ?m ex:mixeddt ?v }");
	CHECK(contains(sql, "DISTINCT"));
	CHECK(contains(sql, "\"d_v\""));
}

TEST_CASE("ORDER BY outside a sub-select sees an undetermined projected variable's tag need in time",
          "[sparql2sql][dynamic-types]") {
	// ?y's dimension is undetermined (ex:mixed) and it is projected bare out of
	// the sub-select, so the enclosing ORDER BY's demand has to reach the
	// sub-select's own SELECT list before that SQL text is frozen - otherwise
	// producedTag() has nothing to give the outer ORDER BY and translation
	// throws instead of building the Section 15.1 layered key.
	const std::string sql =
	    translate("SELECT ?y WHERE { { SELECT ?y WHERE { ?m ex:mixed ?x . BIND(?x AS ?y) } } } ORDER BY ?y");
	CHECK(contains(sql, "\"d_y\""));
}

TEST_CASE("DISTINCT outside a sub-select sees an undetermined projected variable's tag need in time",
          "[sparql2sql][dynamic-types]") {
	const std::string sql = translate("SELECT DISTINCT ?v WHERE { { SELECT ?v WHERE { ?m ex:mixeddt ?v } } }");
	CHECK(contains(sql, "\"d_v\""));
}

TEST_CASE("a determined variable projected out of a sub-select still orders by a single typed key",
          "[sparql2sql][dynamic-types]") {
	// Guards against the pre-fold pass over-marking: ?a's dimension is fully
	// determined (ex:amount), so wrapping it in a sub-select and ordering by it
	// outside must still produce the plain single-key ORDER BY, not the layered
	// Section 15.1 comparison. The pre-fold pass has no schema yet to check
	// isFullyDetermined against, so an inner, unreferenced "d_a" column may
	// still appear deep in the sub-select's own SQL - the documented
	// "over-marking costs one unused column" trade-off - but that must not
	// reach the outer projection or degrade its ORDER BY.
	const std::string sql = translate("SELECT ?a WHERE { { SELECT ?a WHERE { ?m ex:amount ?a } } } ORDER BY ?a");
	CHECK(contains(sql, "ORDER BY TRY_CAST"));
	CHECK_FALSE(contains(sql, "THEN 1 WHEN"));
}

TEST_CASE("STRDT accepts a non-constant datatype argument", "[sparql2sql][dynamic-types]") {
	// Previously refused outright: the dimension had to be a translation-time
	// constant. It is now simply a computed tag.
	const std::string sql = translate("SELECT ?m WHERE { ?m ex:plain ?p . ?m ex:homepage ?h . "
	                                  "FILTER(STRDT(?p, ?h) = \"x\") }");
	CHECK(contains(sql, "'D'"));
}

TEST_CASE("STRLANG accepts a non-constant language argument", "[sparql2sql][dynamic-types]") {
	const std::string sql = translate("SELECT ?m WHERE { ?m ex:plain ?p . ?m ex:title ?t . "
	                                  "FILTER(STRLANG(?p, ?t) = \"x\") }");
	CHECK(contains(sql, "'@'"));
}

TEST_CASE("IRI() and BNODE() are supported term constructors", "[sparql2sql][dynamic-types]") {
	// The lexical form of an IRI term IS its IRI string, so IRI() is a
	// pass-through whose only effect is the tag; BNODE() has to mint a label.
	CHECK_NOTHROW(translate("SELECT ?p WHERE { ?m ex:plain ?p . FILTER(isIRI(IRI(?p))) }"));
	CHECK(contains(translate("SELECT ?p WHERE { ?m ex:plain ?p . BIND(BNODE(?p) AS ?b) }"), "md5("));
	CHECK(contains(translate("SELECT ?p WHERE { ?m ex:plain ?p . BIND(BNODE() AS ?b) }"), "uuid()"));
}

TEST_CASE("a BIND whose dimension is undetermined carries its tag forward", "[sparql2sql][dynamic-types]") {
	const std::string sql = translate("SELECT ?y WHERE { ?m ex:mixed ?x . BIND(?x AS ?y) FILTER(isIRI(?y)) }");
	CHECK(contains(sql, "\"d_y\""));
}

// ---------------------------------------------------------------------------
// 5. The tag encoding itself
// ---------------------------------------------------------------------------

TEST_CASE("encodeTag/decodeTag round-trip every fully-determined annotation", "[sparql2sql][dynamic-types]") {
	TermInfo iri;
	iri.kind = RdfTermKind::Iri;
	TermInfo bnode;
	bnode.kind = RdfTermKind::BlankNode;

	const TermInfo samples[] = {iri, bnode, lit(), lit("http://www.w3.org/2001/XMLSchema#integer"),
	                            lit(kRdfLangString, "en")};
	for (const auto &t : samples) {
		REQUIRE(isFullyDetermined(t));
		const std::string tag = encodeTag(t);
		REQUIRE_FALSE(tag.empty());
		const TermInfo back = decodeTag(tag);
		CHECK(back.kind == t.kind);
		CHECK(back.datatypeIri == t.datatypeIri);
		CHECK(back.lang == t.lang);
	}
}

TEST_CASE("encodeTag distinguishes an undeterminable datatype from a degraded one", "[sparql2sql][dynamic-types]") {
	// The distinction TermInfo::degraded exists for. Both have kind == Literal
	// and an empty datatypeIri, but only the first has a single honest tag.
	const TermInfo undeclared = lit();
	CHECK(encodeTag(undeclared) == "L");

	const TermInfo disagreed =
	    meet(lit("http://www.w3.org/2001/XMLSchema#integer"), lit("http://www.w3.org/2001/XMLSchema#string"));
	CHECK(disagreed.kind == RdfTermKind::Literal);
	CHECK(disagreed.datatypeIri.empty());
	CHECK(disagreed.degraded);
	CHECK_FALSE(isFullyDetermined(disagreed));
	CHECK(encodeTag(disagreed).empty());
}

TEST_CASE("encodeTag refuses a language-tagged literal whose tag is not known", "[sparql2sql][dynamic-types]") {
	const TermInfo m = meet(lit(kRdfLangString, "en"), lit(kRdfLangString, "fr"));
	CHECK(m.datatypeIri == kRdfLangString);
	CHECK(m.lang.empty());
	CHECK_FALSE(isFullyDetermined(m));
	CHECK(encodeTag(m).empty());
}

TEST_CASE("decodeTag of an unrecognised or empty tag claims nothing", "[sparql2sql][dynamic-types]") {
	CHECK_FALSE(decodeTag("").kindKnown());
	CHECK_FALSE(decodeTag("?nonsense").kindKnown());
}

TEST_CASE("GROUP BY partitions by term, not just by lexical form", "[sparql2sql][dynamic-types]") {
	// Symmetric with the DISTINCT case: grouping on the text alone would merge
	// "9"^^xsd:integer and "9"^^xsd:string into one group.
	const std::string sql = translate("SELECT ?y (COUNT(*) AS ?c) WHERE { ?m ex:mixeddt ?v . BIND(?v AS ?y) } "
	                                  "GROUP BY ?y");
	CHECK(contains(sql, "GROUP BY"));
	CHECK(contains(sql, "\"d_y\""));
}

TEST_CASE("GROUP BY over a determined dimension keeps its single key", "[sparql2sql][dynamic-types]") {
	const std::string sql = translate("SELECT ?a (COUNT(*) AS ?c) WHERE { ?m ex:amount ?a } GROUP BY ?a");
	CHECK_FALSE(hasAnyTagColumn(sql));
}
