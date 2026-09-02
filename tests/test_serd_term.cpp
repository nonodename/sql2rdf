/**
 * Unit tests for the rdf::Term <-> SerdNode bridge (r2rml/SerdTerm.h).
 *
 * The load-bearing group here is "node metadata round-trips". Serd derives
 * n_chars and the SERD_HAS_QUOTE / SERD_HAS_NEWLINE flags from a node's bytes,
 * and those flags select """long""" quoting in Turtle. ConstantTermMap
 * currently copies them from the reader's node instead of recomputing them, so
 * when it switches to storing an rdf::Term they will start being recomputed at
 * this boundary. These tests pin that recomputation as byte-identical, at the
 * cheapest possible point - a golden output diff would tell you something
 * changed, but not which of the two computations was wrong.
 */

#include <catch2/catch.hpp>

#include "r2rml/SerdTerm.h"

#include <string>

using r2rml::kindForTermType;
using r2rml::kindOf;
using r2rml::SerdTermRef;
using r2rml::serdTypeOf;
using r2rml::termFromSerdNode;
using r2rml::TermType;
using rdf::Term;
using rdf::TermKind;

namespace {

SerdNode node(SerdType type, const char *text) {
	return serd_node_from_string(type, reinterpret_cast<const uint8_t *>(text));
}

std::string textOf(const SerdNode *n) {
	REQUIRE(n != nullptr);
	return std::string(reinterpret_cast<const char *>(n->buf), n->n_bytes);
}

const char *const XSD_INTEGER = "http://www.w3.org/2001/XMLSchema#integer";

// The awkward literal: an embedded quote, an embedded newline, and multi-byte
// UTF-8 (an em dash, an accented Latin character, and two CJK characters).
// Between them these set both node flags and make n_chars differ from n_bytes.
const char *const AWKWARD = "He said \"hi\"\nthen left \xE2\x80\x94 caf\xC3\xA9 \xE6\x97\xA5\xE6\x9C\xAC";

} // namespace

// ---------------------------------------------------------------------------
// Kind <-> SerdType
// ---------------------------------------------------------------------------

TEST_CASE("serdTypeOf and kindOf round-trip the three real term kinds", "[serdterm]") {
	REQUIRE(serdTypeOf(TermKind::Iri) == SERD_URI);
	REQUIRE(serdTypeOf(TermKind::BlankNode) == SERD_BLANK);
	REQUIRE(serdTypeOf(TermKind::Literal) == SERD_LITERAL);
	REQUIRE(serdTypeOf(TermKind::Unknown) == SERD_NOTHING);

	REQUIRE(kindOf(SERD_URI) == TermKind::Iri);
	REQUIRE(kindOf(SERD_BLANK) == TermKind::BlankNode);
	REQUIRE(kindOf(SERD_LITERAL) == TermKind::Literal);
	REQUIRE(kindOf(SERD_NOTHING) == TermKind::Unknown);
}

TEST_CASE("kindOf maps SERD_CURIE to Unknown rather than Iri", "[serdterm]") {
	// An unexpanded prefixed name is not something rdf::Term can represent.
	// Calling it an IRI would let "ex:Employee" reach a position that requires
	// an absolute IRI and be written out verbatim. Callers must expand first.
	REQUIRE(kindOf(SERD_CURIE) == TermKind::Unknown);
	const SerdNode curie = node(SERD_CURIE, "ex:Employee");
	REQUIRE(termFromSerdNode(&curie).isNull());
}

TEST_CASE("kindForTermType maps rr:termType to the kind it produces", "[serdterm]") {
	REQUIRE(kindForTermType(TermType::IRI) == TermKind::Iri);
	REQUIRE(kindForTermType(TermType::BlankNode) == TermKind::BlankNode);
	REQUIRE(kindForTermType(TermType::Literal) == TermKind::Literal);
}

// ---------------------------------------------------------------------------
// termFromSerdNode
// ---------------------------------------------------------------------------

TEST_CASE("termFromSerdNode deep-copies IRIs and blank nodes", "[serdterm]") {
	const SerdNode iriNode = node(SERD_URI, "http://example.org/a");
	const Term i = termFromSerdNode(&iriNode);
	REQUIRE(i.isIri());
	REQUIRE(i.lexical() == "http://example.org/a");

	// Serd's blank labels are bare; the Term keeps them bare. TripleCollector
	// adds the "_:" prefix and the merge-scope tag itself.
	const SerdNode blankNode = node(SERD_BLANK, "b0");
	const Term b = termFromSerdNode(&blankNode);
	REQUIRE(b.isBlankNode());
	REQUIRE(b.lexical() == "b0");
}

TEST_CASE("termFromSerdNode attaches datatype and language to literals", "[serdterm]") {
	const SerdNode dt = node(SERD_URI, XSD_INTEGER);
	const SerdNode lang = node(SERD_LITERAL, "fr");

	const SerdNode textLit = node(SERD_LITERAL, "text");
	const SerdNode fiveLit = node(SERD_LITERAL, "5");
	const SerdNode chatLit = node(SERD_LITERAL, "chat");

	REQUIRE(termFromSerdNode(&textLit) == Term::literal("text"));
	REQUIRE(termFromSerdNode(&fiveLit, &dt) == Term::typedLiteral("5", XSD_INTEGER));
	REQUIRE(termFromSerdNode(&chatLit, nullptr, &lang) == Term::langLiteral("chat", "fr"));

	// Both supplied: language wins, per RDF 1.1 and Term's invariant.
	const Term both = termFromSerdNode(&chatLit, &dt, &lang);
	REQUIRE(both.lang() == "fr");
	REQUIRE(both.datatypeIri().empty());

	// A datatype or language on a NON-literal is meaningless and ignored.
	const SerdNode iriNode = node(SERD_URI, "http://example.org/a");
	const Term iri = termFromSerdNode(&iriNode, &dt, &lang);
	REQUIRE(iri.isIri());
	REQUIRE(iri.datatypeIri().empty());
	REQUIRE(iri.lang().empty());
}

TEST_CASE("termFromSerdNode yields the absent term for null and empty nodes", "[serdterm]") {
	REQUIRE(termFromSerdNode(nullptr).isNull());
	SerdNode nothing = SERD_NODE_NULL;
	REQUIRE(termFromSerdNode(&nothing).isNull());
}

TEST_CASE("termFromSerdNode preserves an empty literal as a real term", "[serdterm]") {
	// An empty literal is a well-formed RDF term and must NOT collapse to the
	// absent term - "" and "no term at all" are different things.
	const SerdNode emptyLit = node(SERD_LITERAL, "");
	const Term empty = termFromSerdNode(&emptyLit);
	REQUIRE_FALSE(empty.isNull());
	REQUIRE(empty.isLiteral());
	REQUIRE(empty.lexical().empty());
}

// ---------------------------------------------------------------------------
// SerdTermRef
// ---------------------------------------------------------------------------

TEST_CASE("SerdTermRef exposes the value node and nothing else for non-literals", "[serdterm]") {
	const SerdTermRef iri(Term::iri("http://example.org/a"));
	REQUIRE(iri.present());
	REQUIRE(iri.value()->type == SERD_URI);
	REQUIRE(textOf(iri.value()) == "http://example.org/a");
	REQUIRE(iri.datatype() == nullptr);
	REQUIRE(iri.lang() == nullptr);

	const SerdTermRef blank(Term::blankNode("b0"));
	REQUIRE(blank.value()->type == SERD_BLANK);
	REQUIRE(textOf(blank.value()) == "b0");
}

TEST_CASE("SerdTermRef splits a literal into value plus at most one of dt/lang", "[serdterm]") {
	const SerdTermRef plain(Term::literal("text"));
	REQUIRE(plain.datatype() == nullptr);
	REQUIRE(plain.lang() == nullptr);

	const SerdTermRef typed(Term::typedLiteral("5", XSD_INTEGER));
	REQUIRE(typed.lang() == nullptr);
	REQUIRE(typed.datatype()->type == SERD_URI);
	REQUIRE(textOf(typed.datatype()) == XSD_INTEGER);

	const SerdTermRef tagged(Term::langLiteral("chat", "fr"));
	REQUIRE(tagged.datatype() == nullptr);
	// Serd carries a language tag in a SERD_LITERAL-typed node - a quirk of its
	// statement signature rather than a claim that the tag is itself a literal.
	REQUIRE(tagged.lang()->type == SERD_LITERAL);
	REQUIRE(textOf(tagged.lang()) == "fr");
}

TEST_CASE("SerdTermRef reports the absent term as not present", "[serdterm]") {
	const SerdTermRef none((Term()));
	REQUIRE_FALSE(none.present());
	REQUIRE(none.value() == nullptr);
	REQUIRE(none.datatype() == nullptr);
	REQUIRE(none.lang() == nullptr);
}

TEST_CASE("SerdTermRef keeps its nodes valid after the source term dies", "[serdterm]") {
	// The reason this type exists: a SerdNode borrows its buffer, so building
	// one over a caller's temporary is a dangling pointer waiting to happen.
	// SerdTermRef copies the term into itself, so it does not care what the
	// caller does next.
	SerdTermRef *ref = nullptr;
	{
		Term temporary = Term::typedLiteral("5", XSD_INTEGER);
		ref = new SerdTermRef(temporary);
		temporary.assignLangLiteral("clobbered", "zz");
		temporary.clear();
	}
	REQUIRE(textOf(ref->value()) == "5");
	REQUIRE(textOf(ref->datatype()) == XSD_INTEGER);
	delete ref;
}

// ---------------------------------------------------------------------------
// Node metadata - the reason this file exists
// ---------------------------------------------------------------------------

TEST_CASE("SerdTermRef recomputes n_bytes, n_chars and flags identically", "[serdterm]") {
	// Serd derives n_chars and SERD_HAS_QUOTE / SERD_HAS_NEWLINE from the bytes
	// rather than accepting them from the caller. ConstantTermMap is currently
	// the one place that copies them from the reader's node; once it stores an
	// rdf::Term they get recomputed here instead. Assert the recomputation is
	// byte-for-byte identical to what serd itself would produce.
	const SerdNode direct = node(SERD_LITERAL, AWKWARD);
	const SerdTermRef viaTerm(Term::literal(AWKWARD));

	REQUIRE(viaTerm.value()->n_bytes == direct.n_bytes);
	REQUIRE(viaTerm.value()->n_chars == direct.n_chars);
	REQUIRE(viaTerm.value()->flags == direct.flags);
	REQUIRE(viaTerm.value()->type == direct.type);
	REQUIRE(textOf(viaTerm.value()) == AWKWARD);

	// Sanity-check that the fixture actually exercises what it claims to: both
	// flags set, and multi-byte UTF-8 making n_chars differ from n_bytes.
	REQUIRE((direct.flags & SERD_HAS_QUOTE) != 0);
	REQUIRE((direct.flags & SERD_HAS_NEWLINE) != 0);
	REQUIRE(direct.n_chars < direct.n_bytes);
}

TEST_CASE("a full round-trip through Term preserves node metadata", "[serdterm]") {
	// SerdNode -> Term -> SerdNode, which is the path a parsed rr:constant
	// takes once ConstantTermMap stores a Term.
	const SerdNode original = node(SERD_LITERAL, AWKWARD);
	const SerdTermRef roundTripped(termFromSerdNode(&original));

	REQUIRE(roundTripped.value()->n_bytes == original.n_bytes);
	REQUIRE(roundTripped.value()->n_chars == original.n_chars);
	REQUIRE(roundTripped.value()->flags == original.flags);
	REQUIRE(textOf(roundTripped.value()) == AWKWARD);
}

TEST_CASE("plain ASCII terms carry no flags and equal char and byte counts", "[serdterm]") {
	// The control for the case above: nothing awkward, so nothing set.
	const SerdNode direct = node(SERD_URI, "http://example.org/a");
	const SerdTermRef viaTerm(Term::iri("http://example.org/a"));

	REQUIRE(viaTerm.value()->flags == 0);
	REQUIRE(viaTerm.value()->flags == direct.flags);
	REQUIRE(viaTerm.value()->n_chars == viaTerm.value()->n_bytes);
	REQUIRE(viaTerm.value()->n_chars == direct.n_chars);
}
