#include <catch2/catch_test_macros.hpp>

#include "r2rml/TripleStore.h"

using r2rml::ObjType;
using r2rml::ObjValue;
using r2rml::TripleStore;

namespace {

ObjValue uri(const std::string &value) {
	ObjValue o;
	o.type = ObjType::URI;
	o.value = value;
	return o;
}

ObjValue blank(const std::string &value) {
	ObjValue o;
	o.type = ObjType::Blank;
	o.value = value;
	return o;
}

ObjValue literal(const std::string &value, const std::string &datatype = "", const std::string &lang = "") {
	ObjValue o;
	o.type = ObjType::Literal;
	o.value = value;
	o.datatype = datatype;
	o.lang = lang;
	return o;
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
	REQUIRE((*objs)[0].value == "http://example.org/a");
	REQUIRE((*objs)[1].value == "hello");

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
