#include <catch2/catch_test_macros.hpp>

#include "r2rml/TripleStore.h"

using r2rml::TripleStore;
using rdf::Term;
using rdf::TermKind;

namespace {

// Thin aliases over rdf::Term's named constructors, kept so the test bodies
// below read the same as they did when TripleStore had its own ObjValue struct.
Term uri(const std::string &value) {
	return Term::iri(value);
}

// The label is stored BARE here, with no "_:". objKey() adds the prefix when it
// needs a subject-lookup key - see the round-trip assertions below.
Term blank(const std::string &value) {
	return Term::blankNode(value);
}

Term literal(const std::string &value, const std::string &datatype = "", const std::string &lang = "") {
	if (!lang.empty()) {
		return Term::langLiteral(value, lang);
	}
	return Term::typedLiteral(value, datatype);
}

} // namespace

TEST_CASE("TripleStore is empty until a triple is inserted", "[triplestore]") {
	TripleStore ts;
	REQUIRE(ts.empty());
	REQUIRE(ts.getObjects("s", "p") == nullptr);
	REQUIRE(ts.getFirstLiteral("s", "p").empty());
	REQUIRE(ts.getFirstUri("s", "p").empty());
	REQUIRE(ts.getFirstObjKey("s", "p").empty());
}

TEST_CASE("TripleStore getObjects returns all objects for a subject/predicate pair", "[triplestore]") {
	TripleStore ts;
	ts.insert("s", "p", uri("http://example.org/a"));
	ts.insert("s", "p", literal("hello"));

	const auto *objs = ts.getObjects("s", "p");
	REQUIRE(objs != nullptr);
	REQUIRE(objs->size() == 2);
	REQUIRE((*objs)[0].lexical() == "http://example.org/a");
	REQUIRE((*objs)[1].lexical() == "hello");

	REQUIRE(ts.getObjects("s", "other") == nullptr);
	REQUIRE(ts.getObjects("missing", "p") == nullptr);
}

TEST_CASE("TripleStore getFirstLiteral skips non-literal objects", "[triplestore]") {
	TripleStore ts;
	ts.insert("s", "p", uri("http://example.org/a"));
	ts.insert("s", "p", literal("first"));
	ts.insert("s", "p", literal("second"));

	REQUIRE(ts.getFirstLiteral("s", "p") == "first");
	REQUIRE(ts.getFirstUri("s", "q").empty());
}

TEST_CASE("TripleStore getFirstUri skips non-URI objects", "[triplestore]") {
	TripleStore ts;
	ts.insert("s", "p", literal("not a uri"));
	ts.insert("s", "p", uri("http://example.org/first"));
	ts.insert("s", "p", uri("http://example.org/second"));

	REQUIRE(ts.getFirstUri("s", "p") == "http://example.org/first");
	REQUIRE(ts.getFirstLiteral("s", "q").empty());
}

TEST_CASE("TripleStore::objKey maps object kinds to subject-lookup keys", "[triplestore]") {
	REQUIRE(TripleStore::objKey(uri("http://example.org/a")) == "http://example.org/a");
	REQUIRE(TripleStore::objKey(blank("b0")) == "_:b0");
	REQUIRE(TripleStore::objKey(literal("text")).empty());
}

TEST_CASE("TripleStore getFirstObjKey returns the key of the first object regardless of type", "[triplestore]") {
	TripleStore ts;
	ts.insert("s", "p", blank("b0"));
	ts.insert("s", "p", uri("http://example.org/a"));

	REQUIRE(ts.getFirstObjKey("s", "p") == "_:b0");
	REQUIRE(ts.getFirstObjKey("s", "missing").empty());

	TripleStore uriFirst;
	uriFirst.insert("s", "p", uri("http://example.org/a"));
	uriFirst.insert("s", "p", blank("b0"));
	REQUIRE(uriFirst.getFirstObjKey("s", "p") == "http://example.org/a");

	TripleStore literalFirst;
	literalFirst.insert("s", "p", literal("text"));
	REQUIRE(literalFirst.getFirstObjKey("s", "p").empty());
}

TEST_CASE("TripleStore supports iteration over subjects", "[triplestore]") {
	TripleStore ts;
	ts.insert("s1", "p", literal("a"));
	ts.insert("s2", "p", literal("b"));

	std::size_t count = 0;
	bool sawS1 = false;
	bool sawS2 = false;
	for (const auto &entry : ts) {
		++count;
		if (entry.first == "s1") {
			sawS1 = true;
		}
		if (entry.first == "s2") {
			sawS2 = true;
		}
	}
	REQUIRE(count == 2);
	REQUIRE(sawS1);
	REQUIRE(sawS2);
}

TEST_CASE("TripleStore getFirstOfKind returns the whole term, not just its text", "[triplestore]") {
	TripleStore ts;
	ts.insert("s", "p", uri("http://example.org/a"));
	ts.insert("s", "p", literal("5", "http://www.w3.org/2001/XMLSchema#integer"));
	ts.insert("s", "p", literal("chat", "", "fr"));

	const Term *first = ts.getFirstOfKind("s", "p", TermKind::Iri);
	REQUIRE(first != nullptr);
	REQUIRE(first->lexical() == "http://example.org/a");

	// getFirstLiteral flattens a literal to its lexical form, discarding the
	// datatype. That is fine for the single-valued R2RML properties it serves
	// (rr:template, rr:column, ...), but it is a real loss for anything that
	// cares about the type.
	REQUIRE(ts.getFirstLiteral("s", "p") == "5");

	// getFirstOfKind is the accessor that does not lose it.
	const Term *lit = ts.getFirstOfKind("s", "p", TermKind::Literal);
	REQUIRE(lit != nullptr);
	REQUIRE(lit->lexical() == "5");
	REQUIRE(lit->datatypeIri() == "http://www.w3.org/2001/XMLSchema#integer");

	REQUIRE(ts.getFirstOfKind("s", "p", TermKind::BlankNode) == nullptr);
	REQUIRE(ts.getFirstOfKind("s", "missing", TermKind::Iri) == nullptr);
}

TEST_CASE("TripleStore stores blank labels bare and adds the prefix in objKey", "[triplestore]") {
	// The "_:" prefix is this store's key-space encoding, not part of the term.
	// Feeding an already-prefixed label in here would make every subject lookup
	// silently miss - the mapping would still parse, with whole fragments of it
	// quietly absent - so the asymmetry is pinned explicitly.
	TripleStore ts;
	ts.insert("s", "p", blank("b0"));

	const Term *stored = ts.getFirstOfKind("s", "p", TermKind::BlankNode);
	REQUIRE(stored != nullptr);
	REQUIRE(stored->lexical() == "b0");

	REQUIRE(ts.getFirstObjKey("s", "p") == "_:b0");
	REQUIRE(TripleStore::objKey(*stored) == "_:b0");
}
