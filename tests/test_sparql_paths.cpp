#include <catch2/catch_test_macros.hpp>

#include <string>

#ifndef SOURCE_SPARQL_DIR
#define SOURCE_SPARQL_DIR ""
#endif

#include "sparql-parser/Parser.h"

using sparql::ParseError;
using sparql::Parser;
using sparql::ast::AlternativePath;
using sparql::ast::BasicGraphPattern;
using sparql::ast::InversePath;
using sparql::ast::NegatedPropertySet;
using sparql::ast::OneOrMorePath;
using sparql::ast::PathKind;
using sparql::ast::PredicatePath;
using sparql::ast::PropertyPathExpr;
using sparql::ast::Query;
using sparql::ast::SequencePath;
using sparql::ast::VariablePath;
using sparql::ast::ZeroOrMorePath;

namespace {
const PropertyPathExpr &predicateOf(const Query &q, std::size_t elementIndex = 0, std::size_t tripleIndex = 0) {
	const auto &bgp = static_cast<const BasicGraphPattern &>(*q.where->elements[elementIndex]);
	return *bgp.triples[tripleIndex].predicate;
}
} // namespace

TEST_CASE("A bare IRI predicate is a one-step PredicatePath") {
	Parser parser;
	auto q = parser.parseString("SELECT * WHERE { ?s <urn:p> ?o }");
	REQUIRE(predicateOf(*q).kind() == PathKind::Predicate);
}

TEST_CASE("A bare variable predicate is a VariablePath, in both path and non-path contexts") {
	Parser parser;
	auto q = parser.parseString("SELECT * WHERE { ?s ?p ?o }");
	REQUIRE(predicateOf(*q).kind() == PathKind::Variable);

	auto tmpl = parser.parseString("CONSTRUCT { ?s ?p ?o } WHERE { ?s ?p ?o }");
	const auto &constructPred = *tmpl->constructTemplate[0].predicate;
	REQUIRE(constructPred.kind() == PathKind::Variable);
}

TEST_CASE("Inverse path '^p'") {
	Parser parser;
	auto q = parser.parseString("SELECT * WHERE { ?s ^<urn:p> ?o }");
	REQUIRE(predicateOf(*q).kind() == PathKind::Inverse);
}

TEST_CASE("Sequence path 'p1/p2' is left-associative") {
	Parser parser;
	auto q = parser.parseString("PREFIX : <http://ex/>\nSELECT * WHERE { ?s :a/:b/:c ?o }");
	const auto &outer = static_cast<const SequencePath &>(predicateOf(*q));
	REQUIRE(outer.right->kind() == PathKind::Predicate);
	const auto &inner = static_cast<const SequencePath &>(*outer.left);
	REQUIRE(static_cast<const PredicatePath &>(*inner.left).iri->value == "http://ex/a");
}

TEST_CASE("Alternative path 'p1|p2'") {
	Parser parser;
	auto q = parser.parseString("PREFIX : <http://ex/>\nSELECT * WHERE { ?s :a|:b ?o }");
	REQUIRE(predicateOf(*q).kind() == PathKind::Alternative);
}

TEST_CASE("Sequence binds tighter than alternative: 'a/b|c' is (a/b)|c") {
	Parser parser;
	auto q = parser.parseString("PREFIX : <http://ex/>\nSELECT * WHERE { ?s :a/:b|:c ?o }");
	const auto &alt = static_cast<const AlternativePath &>(predicateOf(*q));
	REQUIRE(alt.left->kind() == PathKind::Sequence);
	REQUIRE(alt.right->kind() == PathKind::Predicate);
}

TEST_CASE("Postfix modifiers '*' '+' '?' wrap the preceding path element") {
	Parser parser;
	auto star = parser.parseString("SELECT * WHERE { ?s <urn:p>* ?o }");
	REQUIRE(predicateOf(*star).kind() == PathKind::ZeroOrMore);
	auto plus = parser.parseString("SELECT * WHERE { ?s <urn:p>+ ?o }");
	REQUIRE(predicateOf(*plus).kind() == PathKind::OneOrMore);
	auto opt = parser.parseString("SELECT * WHERE { ?s <urn:p>? ?o }");
	REQUIRE(predicateOf(*opt).kind() == PathKind::ZeroOrOne);
}

TEST_CASE("Grouping with parens controls precedence: '(a|b)+' modifies the whole alternative") {
	Parser parser;
	auto q = parser.parseString("PREFIX : <http://ex/>\nSELECT * WHERE { ?s (:a|:b)+ ?o }");
	const auto &plus = static_cast<const OneOrMorePath &>(predicateOf(*q));
	REQUIRE(plus.child->kind() == PathKind::Alternative);
}

TEST_CASE("Negated property set with a single forward IRI: '!iri' is short for '!(iri)'") {
	Parser parser;
	auto q = parser.parseString("SELECT * WHERE { ?s !<urn:p> ?o }");
	const auto &n = static_cast<const NegatedPropertySet &>(predicateOf(*q));
	REQUIRE(n.forward.size() == 1);
	REQUIRE(n.inverse.empty());
}

TEST_CASE("Negated property set mixes forward and inverse members") {
	Parser parser;
	auto q = parser.parseFile(SOURCE_SPARQL_DIR "property_paths.rq");
	// The FILTER breaks the basic graph pattern in two; the second BGP
	// (after the filter) holds the !(rdf:type|^rdf:type) triple.
	const auto &bgp = static_cast<const BasicGraphPattern &>(*q->where->elements[2]);
	const auto &n = static_cast<const NegatedPropertySet &>(*bgp.triples.back().predicate);
	REQUIRE(n.forward.size() == 1);
	REQUIRE(n.inverse.size() == 1);
}

TEST_CASE("Property path spec examples all parse: knows+/name, type/subClassOf*, knows/^knows") {
	Parser parser;
	auto q = parser.parseFile(SOURCE_SPARQL_DIR "property_paths.rq");
	const auto &bgp = static_cast<const BasicGraphPattern &>(*q->where->elements[0]);
	REQUIRE(bgp.triples.size() == 4);
	REQUIRE(bgp.triples[1].predicate->kind() == PathKind::Sequence);
	REQUIRE(bgp.triples[2].predicate->kind() == PathKind::Sequence);
	REQUIRE(bgp.triples[3].predicate->kind() == PathKind::Sequence);
}

TEST_CASE("An unterminated negated property set is a ParseError") {
	Parser parser;
	REQUIRE_THROWS_AS(parser.parseFile(SOURCE_SPARQL_DIR "invalid_incomplete_property_path.rq"), ParseError);
}

TEST_CASE("Negated property set 'a' shorthand, ungrouped: '!a' means '!(rdf:type)'") {
	Parser parser;
	auto q = parser.parseString("SELECT * WHERE { ?s !a ?o }");
	const auto &n = static_cast<const NegatedPropertySet &>(predicateOf(*q));
	REQUIRE(n.forward.size() == 1);
	REQUIRE(n.inverse.empty());
	REQUIRE(n.forward[0]->value == "http://www.w3.org/1999/02/22-rdf-syntax-ns#type");
}

TEST_CASE("Negated property set 'a' shorthand inside a grouped set, both forward and inverse: '!(a|^a)'") {
	Parser parser;
	auto q = parser.parseString("SELECT * WHERE { ?s !(a|^a) ?o }");
	const auto &n = static_cast<const NegatedPropertySet &>(predicateOf(*q));
	REQUIRE(n.forward.size() == 1);
	REQUIRE(n.inverse.size() == 1);
	REQUIRE(n.forward[0]->value == "http://www.w3.org/1999/02/22-rdf-syntax-ns#type");
	REQUIRE(n.inverse[0]->value == "http://www.w3.org/1999/02/22-rdf-syntax-ns#type");
}

TEST_CASE("Chained compound path mixing sequence, alternative, inverse and repetition: 'a/knows+|^p*/q'") {
	Parser parser;
	auto q = parser.parseString("PREFIX ex: <http://ex/>\n"
	                            "PREFIX foaf: <http://xmlns.com/foaf/0.1/>\n"
	                            "SELECT * WHERE { ?s a/foaf:knows+|^ex:p*/ex:q ?o }");
	const auto &alt = static_cast<const AlternativePath &>(predicateOf(*q));
	REQUIRE(alt.kind() == PathKind::Alternative);

	// Left arm: sequence 'a/foaf:knows+'
	const auto &leftSeq = static_cast<const SequencePath &>(*alt.left);
	REQUIRE(leftSeq.left->kind() == PathKind::Predicate);
	REQUIRE(static_cast<const PredicatePath &>(*leftSeq.left).iri->value ==
	        "http://www.w3.org/1999/02/22-rdf-syntax-ns#type");
	const auto &leftPlus = static_cast<const OneOrMorePath &>(*leftSeq.right);
	REQUIRE(leftPlus.child->kind() == PathKind::Predicate);
	REQUIRE(static_cast<const PredicatePath &>(*leftPlus.child).iri->value == "http://xmlns.com/foaf/0.1/knows");

	// Right arm: sequence '^ex:p*/ex:q'
	const auto &rightSeq = static_cast<const SequencePath &>(*alt.right);
	const auto &rightInv = static_cast<const InversePath &>(*rightSeq.left);
	const auto &rightStar = static_cast<const ZeroOrMorePath &>(*rightInv.child);
	REQUIRE(static_cast<const PredicatePath &>(*rightStar.child).iri->value == "http://ex/p");
	REQUIRE(rightSeq.right->kind() == PathKind::Predicate);
	REQUIRE(static_cast<const PredicatePath &>(*rightSeq.right).iri->value == "http://ex/q");
}
