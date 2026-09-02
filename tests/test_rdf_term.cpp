/**
 * Unit tests for rdf::Term - the project's owning RDF term value type.
 *
 * These are deliberately exhaustive about the REPRESENTATION INVARIANTS
 * documented on the class, because everything downstream relies on them:
 * the serd bridge's datatype/language branch is only total because a Term can
 * never carry both, and the per-row generation path is only allocation-free
 * because clear() preserves capacity.
 */

#include <catch2/catch.hpp>

#include "rdf/Term.h"

#include <sstream>
#include <string>

using rdf::Term;
using rdf::TermKind;

namespace {

std::string printed(const Term &t) {
	std::ostringstream os;
	os << t;
	return os.str();
}

const char *const XSD_INTEGER = "http://www.w3.org/2001/XMLSchema#integer";

} // namespace

TEST_CASE("rdf::Term default-constructs to the absent term", "[rdfterm]") {
	Term t;
	REQUIRE(t.isNull());
	REQUIRE(t.kind() == TermKind::Unknown);
	REQUIRE_FALSE(static_cast<bool>(t));
	REQUIRE(t.lexical().empty());
	REQUIRE(t.datatypeIri().empty());
	REQUIRE(t.lang().empty());
	REQUIRE(printed(t) == "(null)");
}

TEST_CASE("rdf::Term iri and blankNode carry a bare lexical form", "[rdfterm]") {
	const Term i = Term::iri("http://example.org/a");
	REQUIRE(i.isIri());
	REQUIRE(static_cast<bool>(i));
	// No angle brackets in the stored value - they belong to the syntax, not
	// the term.
	REQUIRE(i.lexical() == "http://example.org/a");
	REQUIRE(i.datatypeIri().empty());
	REQUIRE(i.lang().empty());
	REQUIRE(printed(i) == "<http://example.org/a>");

	const Term b = Term::blankNode("b0");
	REQUIRE(b.isBlankNode());
	// No "_:" prefix in the stored label. TripleStore::objKey adds it when it
	// needs a subject-lookup key; feeding a prefixed label in here would make
	// those lookups silently miss.
	REQUIRE(b.lexical() == "b0");
	REQUIRE(printed(b) == "_:b0");
}

// NB: these three are separate TEST_CASEs rather than SECTIONs of one, because
// Catch2's SECTION expands to an if-with-initialiser - a C++17 extension, and
// this project is C++11 with clang-tidy's WarningsAsErrors: '*'. No test in the
// suite uses SECTION.
TEST_CASE("rdf::Term plain literal has neither datatype nor language", "[rdfterm]") {
	const Term l = Term::literal("text");
	REQUIRE(l.isLiteral());
	REQUIRE(l.lexical() == "text");
	REQUIRE(l.datatypeIri().empty());
	REQUIRE(l.lang().empty());
	REQUIRE_FALSE(l.hasLang());
	// The STORED datatype is empty ("no datatype stated"), so it round-trips
	// as plain rather than being serialised as typed...
	REQUIRE(l.datatypeIri().empty());
	// ...but the EFFECTIVE datatype is xsd:string: RDF 1.1 Concepts 3.3 treats
	// a simple literal as sugar for an explicit xsd:string.
	REQUIRE(l.effectiveDatatypeIri() == rdf::XSD_STRING);
	REQUIRE(printed(l) == "\"text\"");
}

TEST_CASE("rdf::Term typed literal has a datatype and no language", "[rdfterm]") {
	const Term l = Term::typedLiteral("5", XSD_INTEGER);
	REQUIRE(l.datatypeIri() == XSD_INTEGER);
	REQUIRE(l.lang().empty());
	REQUIRE(l.effectiveDatatypeIri() == XSD_INTEGER);
	REQUIRE(printed(l) == "\"5\"^^<http://www.w3.org/2001/XMLSchema#integer>");
}

TEST_CASE("rdf::Term language-tagged literal has an implicit rdf:langString", "[rdfterm]") {
	const Term l = Term::langLiteral("chat", "fr");
	REQUIRE(l.lang() == "fr");
	REQUIRE(l.hasLang());
	// Stored datatype is empty...
	REQUIRE(l.datatypeIri().empty());
	// ...but the abstract datatype is rdf:langString. Serialisers want the
	// former, SPARQL's DATATYPE() wants the latter.
	REQUIRE(l.effectiveDatatypeIri() == rdf::RDF_LANG_STRING);
	REQUIRE(printed(l) == "\"chat\"@fr");
}

TEST_CASE("rdf::Term degrades ill-formed literal combinations to a plain literal", "[rdfterm]") {
	// An empty datatype is not a datatype.
	REQUIRE(Term::typedLiteral("x", "") == Term::literal("x"));
	// Nor is an empty language tag a language tag.
	REQUIRE(Term::langLiteral("x", "") == Term::literal("x"));

	// rdf:langString WITHOUT a tag is not a well-formed RDF term, so storing it
	// as a datatype would produce something no parser accepts back. It degrades
	// rather than round-tripping a lie.
	const Term degraded = Term::typedLiteral("x", rdf::RDF_LANG_STRING);
	REQUIRE(degraded.datatypeIri().empty());
	REQUIRE(degraded.lang().empty());
	REQUIRE(degraded == Term::literal("x"));

	// But WITH a tag it is fine, and reported as the effective datatype.
	REQUIRE(Term::langLiteral("x", "en").effectiveDatatypeIri() == rdf::RDF_LANG_STRING);
}

TEST_CASE("rdf::Term assign* mutators never leave a stale datatype or language", "[rdfterm]") {
	// This is the invariant the serd bridge depends on: it branches on
	// "has language, else has datatype" and that is only total if the two can
	// never both be set. Every transition between flavours must scrub the
	// field it is not using.
	Term t = Term::langLiteral("chat", "fr");

	t.assignTypedLiteral("5", XSD_INTEGER);
	REQUIRE(t.lang().empty());
	REQUIRE(t.datatypeIri() == XSD_INTEGER);

	t.assignLangLiteral("chat", "fr");
	REQUIRE(t.datatypeIri().empty());
	REQUIRE(t.lang() == "fr");

	t.assignLiteral("plain");
	REQUIRE(t.datatypeIri().empty());
	REQUIRE(t.lang().empty());

	// Non-literals must not carry either, whatever the term held before.
	t.assignLangLiteral("chat", "fr");
	t.assignIri("http://example.org/a");
	REQUIRE(t.isIri());
	REQUIRE(t.datatypeIri().empty());
	REQUIRE(t.lang().empty());

	t.assignTypedLiteral("5", XSD_INTEGER);
	t.assignBlankNode("b1");
	REQUIRE(t.isBlankNode());
	REQUIRE(t.datatypeIri().empty());
	REQUIRE(t.lang().empty());
}

TEST_CASE("rdf::Term clear resets the term but preserves string capacity", "[rdfterm]") {
	Term t = Term::typedLiteral(std::string(512, 'x'), XSD_INTEGER);
	const std::string::size_type capacityBefore = t.lexical().capacity();
	REQUIRE(capacityBefore >= 512);

	t.clear();

	REQUIRE(t.isNull());
	REQUIRE(t.lexical().empty());
	REQUIRE(t.datatypeIri().empty());
	REQUIRE(t.lang().empty());
	// The whole point of clear() over assignment: a Term hoisted out of a
	// per-row loop stops allocating after the first few rows. If this ever
	// regresses, the generation hot loop silently starts heap-allocating per
	// term per row.
	REQUIRE(t.lexical().capacity() == capacityBefore);
}

TEST_CASE("rdf::Term mutableLexical supports piecewise construction", "[rdfterm]") {
	// TemplateTermMap builds its value by appending literal chunks and expanded
	// column values, which is why this exists. Contract: clear(), append,
	// setKind().
	Term t = Term::langLiteral("stale", "fr");
	t.clear();
	t.mutableLexical() += "http://example.org/";
	t.mutableLexical() += "employee/";
	t.mutableLexical() += "7369";
	t.setKind(TermKind::Iri);

	REQUIRE(t.isIri());
	REQUIRE(t.lexical() == "http://example.org/employee/7369");
	// clear() scrubbed the language, so the rebuilt IRI cannot inherit it.
	REQUIRE(t.lang().empty());
	REQUIRE(t.datatypeIri().empty());
}

TEST_CASE("rdf::Term equality compares the abstract term", "[rdfterm]") {
	REQUIRE(Term() == Term());
	REQUIRE(Term::iri("http://example.org/a") == Term::iri("http://example.org/a"));
	REQUIRE(Term::iri("http://example.org/a") != Term::iri("http://example.org/b"));

	// Same lexical form, different kind: not equal. This is the distinction
	// TermMapSql currently loses when it flattens a constant to bare text.
	REQUIRE(Term::iri("x") != Term::blankNode("x"));
	REQUIRE(Term::iri("x") != Term::literal("x"));
	REQUIRE(Term::blankNode("x") != Term::literal("x"));

	// Literals differ by datatype and by language.
	REQUIRE(Term::literal("5") != Term::typedLiteral("5", XSD_INTEGER));
	REQUIRE(Term::langLiteral("chat", "fr") != Term::langLiteral("chat", "en"));
	REQUIRE(Term::langLiteral("chat", "fr") != Term::literal("chat"));

	// A plain literal and an explicit xsd:string-typed literal with the same
	// lexical form are the SAME abstract term (RDF 1.1 Concepts 3.3), even
	// though their STORED datatype differs (one empty, one explicit).
	REQUIRE(Term::literal("hello") == Term::typedLiteral("hello", rdf::XSD_STRING));
	REQUIRE(Term::literal("hello").datatypeIri() != Term::typedLiteral("hello", rdf::XSD_STRING).datatypeIri());

	// Language tags compare case-INsensitively (BCP 47 / RDF 1.1 Concepts 3.3):
	// differently-cased equivalent tags are the same term...
	REQUIRE(Term::langLiteral("chat", "fr") == Term::langLiteral("chat", "FR"));
	REQUIRE(Term::langLiteral("chat", "fr") == Term::langLiteral("chat", "Fr"));
	// ...but the stored spelling is untouched by the comparison.
	REQUIRE(Term::langLiteral("chat", "FR").lang() == "FR");
}

TEST_CASE("rdf::Term copies are independent", "[rdfterm]") {
	// Term owns its bytes; that is the whole reason it exists, given that a
	// SerdNode only borrows. A copy must not alias the original's buffers.
	Term original = Term::typedLiteral("5", XSD_INTEGER);
	Term copy = original;
	REQUIRE(copy == original);

	original.assignLangLiteral("chat", "fr");
	REQUIRE(copy.lexical() == "5");
	REQUIRE(copy.datatypeIri() == XSD_INTEGER);
	REQUIRE(copy != original);
}

TEST_CASE("rdf::termKindName is total over the enum", "[rdfterm]") {
	REQUIRE(std::string(rdf::termKindName(TermKind::Unknown)) == "Unknown");
	REQUIRE(std::string(rdf::termKindName(TermKind::Iri)) == "Iri");
	REQUIRE(std::string(rdf::termKindName(TermKind::BlankNode)) == "BlankNode");
	REQUIRE(std::string(rdf::termKindName(TermKind::Literal)) == "Literal");
}
