/**
 * Unit tests for the static RDF term dimension's lattice (TermInfo.h).
 *
 * TermInfo annotates each column with what the R2RML mapping statically says
 * about the term it produces - IRI / blank node / literal, plus a literal's
 * datatype and language. `meet` is how that annotation survives every algebra
 * node fed by more than one input (a UNION's arms, a triple pattern's several
 * candidate term maps, an OPTIONAL join's COALESCEd column).
 *
 * The lattice's whole safety argument is that `Unknown` reproduces the
 * translator's pre-annotation behaviour exactly. That makes an over-eager
 * degrade to Unknown *invisible* to every other test in the suite - it just
 * looks like the old behaviour. So these tests assert the positive direction
 * (agreement is preserved) at least as hard as the negative one.
 */

#include <catch2/catch.hpp>

#include <memory>
#include <string>
#include <vector>

#include "sparql-parser/Parser.h"
#include "sparql-parser/ast/Expression.h"
#include "sparql-parser/ast/Query.h"
#include "sparql2sql/TermInference.h"
#include "sparql2sql/TermInfo.h"
#include "sparql2sql/TranslatedPattern.h"
#include "sparql2sql/TypeCatalog.h"

using sparql2sql::inferExprTermInfo;
using sparql2sql::isXsdCastIri;
using sparql2sql::isXsdIntegralIri;
using sparql2sql::isXsdNumericIri;
using sparql2sql::isXsdTemporalIri;
using sparql2sql::kRdfLangString;
using sparql2sql::meet;
using sparql2sql::naturalXsdDatatype;
using sparql2sql::RdfTermKind;
using sparql2sql::TermInfo;
using sparql2sql::TranslatedPattern;
namespace xsd = sparql2sql::xsd;

namespace {

TermInfo lit(const char *datatypeIri = "", const char *lang = "") {
	TermInfo t;
	t.kind = RdfTermKind::Literal;
	t.datatypeIri = datatypeIri;
	t.lang = lang;
	return t;
}

TermInfo iri() {
	TermInfo t;
	t.kind = RdfTermKind::Iri;
	return t;
}

TermInfo bnode() {
	TermInfo t;
	t.kind = RdfTermKind::BlankNode;
	return t;
}

bool same(const TermInfo &a, const TermInfo &b) {
	return a.kind == b.kind && a.datatypeIri == b.datatypeIri && a.lang == b.lang;
}

// A spread of the lattice wide enough to exercise every degradation path.
std::vector<TermInfo> sampleLattice() {
	return {TermInfo(),
	        iri(),
	        bnode(),
	        lit(),
	        lit(xsd::kInteger),
	        lit(xsd::kString),
	        lit(xsd::kDateTime),
	        lit(kRdfLangString, "en"),
	        lit(kRdfLangString, "fr")};
}

} // namespace

TEST_CASE("TermInfo: a default-constructed annotation is Unknown and claims nothing", "[sparql2sql][ir]") {
	TermInfo t;
	CHECK(t.kind == RdfTermKind::Unknown);
	CHECK_FALSE(t.kindKnown());
	CHECK_FALSE(t.isKnownLiteral());
	CHECK_FALSE(t.isNumeric());
	CHECK_FALSE(t.isIntegral());
	CHECK_FALSE(t.isTemporal());
	CHECK_FALSE(t.isStringy());
}

TEST_CASE("meet: identical annotations are their own meet", "[sparql2sql][ir]") {
	for (const auto &t : sampleLattice()) {
		CHECK(same(meet(t, t), t));
	}
}

TEST_CASE("meet: differing term kinds collapse to Unknown", "[sparql2sql][ir]") {
	CHECK(meet(iri(), lit(xsd::kString)).kind == RdfTermKind::Unknown);
	CHECK(meet(iri(), bnode()).kind == RdfTermKind::Unknown);
	CHECK(meet(bnode(), lit()).kind == RdfTermKind::Unknown);
}

TEST_CASE("meet: a collapsed kind drops the datatype and language too", "[sparql2sql][ir]") {
	// Without this, a consumer could read a datatype off a term whose kind it
	// has already been told not to trust.
	TermInfo m = meet(lit(kRdfLangString, "en"), iri());
	CHECK(m.kind == RdfTermKind::Unknown);
	CHECK(m.datatypeIri.empty());
	CHECK(m.lang.empty());
}

TEST_CASE("meet: same kind with differing datatypes keeps the kind and drops the datatype", "[sparql2sql][ir]") {
	TermInfo m = meet(lit(xsd::kInteger), lit(xsd::kString));
	CHECK(m.kind == RdfTermKind::Literal);
	CHECK(m.datatypeIri.empty());
	// Still a known literal, so isLITERAL() may fold - but DATATYPE() must not.
	CHECK(m.isKnownLiteral());
	CHECK_FALSE(m.isNumeric());
}

TEST_CASE("meet: same datatype with differing language tags drops only the language", "[sparql2sql][ir]") {
	TermInfo m = meet(lit(kRdfLangString, "en"), lit(kRdfLangString, "fr"));
	CHECK(m.kind == RdfTermKind::Literal);
	CHECK(m.datatypeIri == kRdfLangString);
	CHECK(m.lang.empty());
}

TEST_CASE("meet: an Unknown operand absorbs everything", "[sparql2sql][ir]") {
	for (const auto &t : sampleLattice()) {
		CHECK(meet(t, TermInfo()).kind == RdfTermKind::Unknown);
		CHECK(meet(TermInfo(), t).kind == RdfTermKind::Unknown);
	}
}

TEST_CASE("meet: a known annotation met with a bare known literal keeps only the kind", "[sparql2sql][ir]") {
	// The multi-candidate case that matters most in practice: one term map
	// declares rr:datatype, a second maps the same predicate without one.
	TermInfo m = meet(lit(xsd::kInteger), lit());
	CHECK(m.kind == RdfTermKind::Literal);
	CHECK(m.datatypeIri.empty());
}

TEST_CASE("meet: commutative, associative and idempotent over the lattice", "[sparql2sql][ir]") {
	const std::vector<TermInfo> sample = sampleLattice();
	for (const auto &a : sample) {
		for (const auto &b : sample) {
			CHECK(same(meet(a, b), meet(b, a)));
			CHECK(same(meet(a, meet(a, b)), meet(a, b)));
			for (const auto &c : sample) {
				CHECK(same(meet(meet(a, b), c), meet(a, meet(b, c))));
			}
		}
	}
}

TEST_CASE("TermInfo: numeric, integral, temporal and stringy classification", "[sparql2sql][ir]") {
	CHECK(lit(xsd::kInteger).isIntegral());
	CHECK(lit(xsd::kLong).isIntegral());
	CHECK(lit(xsd::kByte).isIntegral());
	CHECK_FALSE(lit(xsd::kDouble).isIntegral());
	CHECK_FALSE(lit(xsd::kDecimal).isIntegral());

	CHECK(lit(xsd::kInteger).isNumeric());
	CHECK(lit(xsd::kDouble).isNumeric());
	CHECK(lit(xsd::kDecimal).isNumeric());
	CHECK(lit(xsd::kFloat).isNumeric());
	CHECK_FALSE(lit(xsd::kString).isNumeric());
	CHECK_FALSE(lit(xsd::kBoolean).isNumeric());

	CHECK(lit(xsd::kDate).isTemporal());
	CHECK(lit(xsd::kDateTime).isTemporal());
	CHECK_FALSE(lit(xsd::kTime).isTemporal());

	CHECK(lit(xsd::kString).isStringy());
	CHECK(lit(kRdfLangString, "en").isStringy());
	CHECK_FALSE(lit().isStringy());
}

TEST_CASE("TermInfo: a datatype on a non-literal kind never classifies", "[sparql2sql][ir]") {
	// meet/setters keep this from arising, but the predicates must be
	// self-defending: an IRI is never numeric no matter what else is set.
	TermInfo t = iri();
	t.datatypeIri = xsd::kInteger;
	CHECK_FALSE(t.isNumeric());
	CHECK_FALSE(t.isIntegral());
	CHECK_FALSE(t.isStringy());
}

TEST_CASE("XSD IRI predicates: the twelve constructor-cast IRIs are exactly the castable set", "[sparql2sql][ir]") {
	const std::vector<const char *> casts = {xsd::kInteger, xsd::kLong,    xsd::kInt,      xsd::kShort,
	                                         xsd::kByte,    xsd::kDecimal, xsd::kDouble,   xsd::kFloat,
	                                         xsd::kString,  xsd::kBoolean, xsd::kDateTime, xsd::kDate};
	for (const auto *iriText : casts) {
		CHECK(isXsdCastIri(iriText));
	}
	// xsd:time and rdf:langString are recognised datatypes but not casts.
	CHECK_FALSE(isXsdCastIri(xsd::kTime));
	CHECK_FALSE(isXsdCastIri(kRdfLangString));
	CHECK_FALSE(isXsdCastIri("http://example.com/ns#notADatatype"));
	CHECK_FALSE(isXsdCastIri(""));
}

TEST_CASE("XSD IRI predicates: numeric implies neither temporal nor an empty IRI", "[sparql2sql][ir]") {
	CHECK(isXsdNumericIri(xsd::kDecimal));
	CHECK(isXsdIntegralIri(xsd::kShort));
	CHECK(isXsdTemporalIri(xsd::kDate));
	CHECK_FALSE(isXsdNumericIri(xsd::kDate));
	CHECK_FALSE(isXsdTemporalIri(xsd::kInteger));
	CHECK_FALSE(isXsdNumericIri(""));
	CHECK_FALSE(isXsdIntegralIri(""));
	CHECK_FALSE(isXsdTemporalIri(""));
}

// --- R2RML Section 10.2 natural mapping (TypeCatalog.h) ---

TEST_CASE("naturalXsdDatatype: the Section 10.2 type families", "[sparql2sql][ir]") {
	CHECK(naturalXsdDatatype("VARCHAR") == xsd::kString);
	CHECK(naturalXsdDatatype("TEXT") == xsd::kString);
	CHECK(naturalXsdDatatype("INTEGER") == xsd::kInteger);
	CHECK(naturalXsdDatatype("BIGINT") == xsd::kInteger);
	CHECK(naturalXsdDatatype("SMALLINT") == xsd::kInteger);
	CHECK(naturalXsdDatatype("DECIMAL") == xsd::kDecimal);
	CHECK(naturalXsdDatatype("NUMERIC") == xsd::kDecimal);
	CHECK(naturalXsdDatatype("DOUBLE") == xsd::kDouble);
	CHECK(naturalXsdDatatype("BOOLEAN") == xsd::kBoolean);
	CHECK(naturalXsdDatatype("DATE") == xsd::kDate);
	CHECK(naturalXsdDatatype("TIMESTAMP") == xsd::kDateTime);
	CHECK(naturalXsdDatatype("TIME") == xsd::kTime);
	CHECK(naturalXsdDatatype("BLOB") == xsd::kHexBinary);
}

TEST_CASE("naturalXsdDatatype: FLOAT and REAL map to xsd:double, per Section 10.2", "[sparql2sql][ir]") {
	// Deliberate, not an oversight: the spec's table maps SQL's single-precision
	// types to xsd:double. Pinned so it doesn't get "corrected" to xsd:float.
	CHECK(naturalXsdDatatype("FLOAT") == xsd::kDouble);
	CHECK(naturalXsdDatatype("REAL") == xsd::kDouble);
}

TEST_CASE("naturalXsdDatatype: lookup is case-insensitive and ignores a precision suffix", "[sparql2sql][ir]") {
	CHECK(naturalXsdDatatype("varchar") == xsd::kString);
	CHECK(naturalXsdDatatype("VarChar(255)") == xsd::kString);
	CHECK(naturalXsdDatatype("decimal(18,2)") == xsd::kDecimal);
	CHECK(naturalXsdDatatype("NUMERIC(10)") == xsd::kDecimal);
}

TEST_CASE("naturalXsdDatatype: multi-word type names resolve", "[sparql2sql][ir]") {
	// normalizeType truncates at '(' and trims only *trailing* spaces, so these
	// reach the table with their interior spaces intact and must match verbatim.
	CHECK(naturalXsdDatatype("DOUBLE PRECISION") == xsd::kDouble);
	CHECK(naturalXsdDatatype("CHARACTER VARYING") == xsd::kString);
	CHECK(naturalXsdDatatype("character varying(64)") == xsd::kString);
	CHECK(naturalXsdDatatype("TIMESTAMP WITHOUT TIME ZONE") == xsd::kDateTime);
}

TEST_CASE("naturalXsdDatatype: an unmapped or unknown type infers nothing", "[sparql2sql][ir]") {
	// Empty means "cannot infer", which every consumer must treat as unknown -
	// never as a default of xsd:string.
	CHECK(naturalXsdDatatype("").empty());
	CHECK(naturalXsdDatatype("INTERVAL").empty());
	CHECK(naturalXsdDatatype("UUID").empty());
	CHECK(naturalXsdDatatype("STRUCT").empty());
	CHECK(naturalXsdDatatype("SOMETHING_INVENTED").empty());
	// Section 10.2 would give xsd:dateTime, but only with a rendered UTC offset
	// that this translator's lexical form cannot carry - so it stays unmapped.
	CHECK(naturalXsdDatatype("TIMESTAMP WITH TIME ZONE").empty());
}

// --- inferExprTermInfo (TermInference.h) ---
//
// The expression-side counterpart of the mapping-side annotation. It never
// throws and never guesses: everything it cannot analyse is Unknown. Its
// contract is SQL truth - the datatype of the lexical form the translator
// actually emits - so these cases pin the places where that diverges from what
// SPARQL's abstract semantics would say.

namespace {

// Parse a bare expression by wrapping it in a FILTER and pulling it back out.
const sparql::ast::Expression &parseExpr(const std::string &text,
                                         std::vector<std::unique_ptr<sparql::ast::Query>> &keepAlive) {
	sparql::Parser parser;
	keepAlive.push_back(parser.parseString("PREFIX xsd: <http://www.w3.org/2001/XMLSchema#>\n"
	                                       "SELECT * WHERE { ?s ?p ?o . FILTER(" +
	                                       text + ") }"));
	const auto &el = *keepAlive.back()->where->elements.at(1);
	return *static_cast<const sparql::ast::Filter &>(el).constraint;
}

// A scope binding ?i as xsd:integer, ?d as xsd:double, ?s as xsd:string,
// ?t as xsd:dateTime, ?u as a datatype-less literal, and ?iri as an IRI.
TranslatedPattern typedScope() {
	TranslatedPattern scope;
	scope.boundVars = {"i", "d", "s", "t", "u", "iri"};
	scope.termInfo["i"] = lit(xsd::kInteger);
	scope.termInfo["d"] = lit(xsd::kDouble);
	scope.termInfo["s"] = lit(xsd::kString);
	scope.termInfo["t"] = lit(xsd::kDateTime);
	scope.termInfo["u"] = lit();
	scope.termInfo["iri"] = iri();
	return scope;
}

TermInfo inferOf(const std::string &text) {
	std::vector<std::unique_ptr<sparql::ast::Query>> keepAlive;
	return inferExprTermInfo(parseExpr(text, keepAlive), typedScope());
}

} // namespace

TEST_CASE("inferExprTermInfo: a variable's annotation comes from the scope", "[sparql2sql][ir]") {
	CHECK(inferOf("?i > 0").kind == RdfTermKind::Literal); // the comparison itself
	std::vector<std::unique_ptr<sparql::ast::Query>> keep;
	CHECK(inferExprTermInfo(parseExpr("?i", keep), typedScope()).datatypeIri == xsd::kInteger);
	CHECK(inferExprTermInfo(parseExpr("?iri", keep), typedScope()).kind == RdfTermKind::Iri);
}

TEST_CASE("inferExprTermInfo: an out-of-scope variable is Unknown rather than an error", "[sparql2sql][ir]") {
	// Unlike translateExpression, which rejects it. Inference must stay total so
	// callers never have to guard it.
	CHECK_NOTHROW(inferOf("?nosuchvar"));
	CHECK(inferOf("?nosuchvar").kind == RdfTermKind::Unknown);
}

TEST_CASE("inferExprTermInfo: comparisons and boolean connectives are xsd:boolean", "[sparql2sql][ir]") {
	CHECK(inferOf("?i > 5").datatypeIri == xsd::kBoolean);
	CHECK(inferOf("?s = \"x\"").datatypeIri == xsd::kBoolean);
	CHECK(inferOf("?i > 1 && ?i < 9").datatypeIri == xsd::kBoolean);
	CHECK(inferOf("!(?i > 1)").datatypeIri == xsd::kBoolean);
	CHECK(inferOf("?i IN (1, 2)").datatypeIri == xsd::kBoolean);
}

TEST_CASE("inferExprTermInfo: arithmetic stays integral only when both operands are", "[sparql2sql][ir]") {
	CHECK(inferOf("?i + 1").datatypeIri == xsd::kInteger);
	CHECK(inferOf("?i * ?i").datatypeIri == xsd::kInteger);
	CHECK(inferOf("?i + ?d").datatypeIri == xsd::kDouble);
	CHECK(inferOf("?d - 1.5").datatypeIri == xsd::kDouble);
	// Division always goes through DOUBLE in the emission, so it is reported as
	// DOUBLE even for two integers - SQL truth, not SPARQL's xsd:decimal.
	CHECK(inferOf("?i / ?i").datatypeIri == xsd::kDouble);
	// An operand of unknown datatype leaves the result unknown: the emission
	// TRY_CASTs it, and a non-castable value yields NULL, not a number.
	CHECK(inferOf("?u + 1").kind == RdfTermKind::Unknown);
	CHECK(inferOf("?s + 1").kind == RdfTermKind::Unknown);
}

TEST_CASE("inferExprTermInfo: an XSD constructor cast types its result", "[sparql2sql][ir]") {
	// The highest-value rule: it lets a query type an otherwise-untyped column
	// at the point of use.
	CHECK(inferOf("xsd:integer(?u) > 5").datatypeIri == xsd::kBoolean);
	std::vector<std::unique_ptr<sparql::ast::Query>> keep;
	CHECK(inferExprTermInfo(parseExpr("xsd:integer(?u)", keep), typedScope()).datatypeIri == xsd::kInteger);
	CHECK(inferExprTermInfo(parseExpr("xsd:dateTime(?u)", keep), typedScope()).datatypeIri == xsd::kDateTime);
	CHECK(inferExprTermInfo(parseExpr("xsd:decimal(?u)", keep), typedScope()).isNumeric());
}

TEST_CASE("inferExprTermInfo: string, integer and boolean builtins", "[sparql2sql][ir]") {
	std::vector<std::unique_ptr<sparql::ast::Query>> keep;
	const TranslatedPattern scope = typedScope();
	CHECK(inferExprTermInfo(parseExpr("STR(?i)", keep), scope).datatypeIri == xsd::kString);
	CHECK(inferExprTermInfo(parseExpr("UCASE(?s)", keep), scope).datatypeIri == xsd::kString);
	CHECK(inferExprTermInfo(parseExpr("STRLEN(?s)", keep), scope).datatypeIri == xsd::kInteger);
	CHECK(inferExprTermInfo(parseExpr("YEAR(?t)", keep), scope).datatypeIri == xsd::kInteger);
	CHECK(inferExprTermInfo(parseExpr("CONTAINS(?s, \"x\")", keep), scope).datatypeIri == xsd::kBoolean);
	CHECK(inferExprTermInfo(parseExpr("NOW()", keep), scope).datatypeIri == xsd::kDateTime);
	CHECK(inferExprTermInfo(parseExpr("ABS(?i)", keep), scope).datatypeIri == xsd::kInteger);
	CHECK(inferExprTermInfo(parseExpr("ABS(?d)", keep), scope).datatypeIri == xsd::kDouble);
}

TEST_CASE("inferExprTermInfo: UUID() is an IRI and STRUUID() a string", "[sparql2sql][ir]") {
	// UUID() emits "urn:uuid:..." - annotating it a string would be wrong even
	// though both are VARCHAR columns.
	std::vector<std::unique_ptr<sparql::ast::Query>> keep;
	const TranslatedPattern scope = typedScope();
	CHECK(inferExprTermInfo(parseExpr("UUID()", keep), scope).kind == RdfTermKind::Iri);
	CHECK(inferExprTermInfo(parseExpr("STRUUID()", keep), scope).datatypeIri == xsd::kString);
	CHECK(inferExprTermInfo(parseExpr("DATATYPE(?i)", keep), scope).kind == RdfTermKind::Iri);
}

TEST_CASE("inferExprTermInfo: COALESCE and IF meet their alternatives", "[sparql2sql][ir]") {
	std::vector<std::unique_ptr<sparql::ast::Query>> keep;
	const TranslatedPattern scope = typedScope();
	CHECK(inferExprTermInfo(parseExpr("COALESCE(?i, ?i)", keep), scope).datatypeIri == xsd::kInteger);
	CHECK(inferExprTermInfo(parseExpr("COALESCE(?i, ?s)", keep), scope).datatypeIri.empty());
	CHECK(inferExprTermInfo(parseExpr("COALESCE(?i, ?iri)", keep), scope).kind == RdfTermKind::Unknown);
	// IF's condition is not one of the alternatives.
	CHECK(inferExprTermInfo(parseExpr("IF(?s = \"x\", ?i, ?i)", keep), scope).datatypeIri == xsd::kInteger);
	CHECK(inferExprTermInfo(parseExpr("IF(?s = \"x\", ?i, ?iri)", keep), scope).kind == RdfTermKind::Unknown);
}

TEST_CASE("inferExprTermInfo: STRDT and STRLANG re-tag only with a constant argument", "[sparql2sql][ir]") {
	std::vector<std::unique_ptr<sparql::ast::Query>> keep;
	const TranslatedPattern scope = typedScope();
	CHECK(inferExprTermInfo(parseExpr("STRDT(?u, xsd:integer)", keep), scope).datatypeIri == xsd::kInteger);
	CHECK(inferExprTermInfo(parseExpr("STRLANG(?u, \"en\")", keep), scope).lang == "en");
	CHECK(inferExprTermInfo(parseExpr("STRLANG(?u, \"en\")", keep), scope).datatypeIri == kRdfLangString);
	// A per-row datatype cannot be tracked statically.
	CHECK(inferExprTermInfo(parseExpr("STRDT(?u, ?iri)", keep), scope).kind == RdfTermKind::Unknown);
	CHECK(inferExprTermInfo(parseExpr("STRLANG(?u, ?s)", keep), scope).kind == RdfTermKind::Unknown);
}

TEST_CASE("inferExprTermInfo: a literal carries its own datatype or language", "[sparql2sql][ir]") {
	std::vector<std::unique_ptr<sparql::ast::Query>> keep;
	const TranslatedPattern scope = typedScope();
	CHECK(inferExprTermInfo(parseExpr("5", keep), scope).datatypeIri == xsd::kInteger);
	CHECK(inferExprTermInfo(parseExpr("\"x\"", keep), scope).datatypeIri == xsd::kString);
	CHECK(inferExprTermInfo(parseExpr("\"x\"@en", keep), scope).lang == "en");
	CHECK(inferExprTermInfo(parseExpr("<http://ex.org/a>", keep), scope).kind == RdfTermKind::Iri);
}

TEST_CASE("inferExprTermInfo: the deferred timezone builtins stay Unknown", "[sparql2sql][ir]") {
	std::vector<std::unique_ptr<sparql::ast::Query>> keep;
	const TranslatedPattern scope = typedScope();
	CHECK(inferExprTermInfo(parseExpr("TIMEZONE(?t)", keep), scope).kind == RdfTermKind::Unknown);
	CHECK(inferExprTermInfo(parseExpr("TZ(?t)", keep), scope).kind == RdfTermKind::Unknown);
}
