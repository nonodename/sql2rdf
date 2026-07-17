#include <catch2/catch_test_macros.hpp>

#include <string>

#ifndef SOURCE_SPARQL_DIR
#define SOURCE_SPARQL_DIR ""
#endif

#include "sparql-parser/Parser.h"

using namespace sparql;
using namespace sparql::ast;

namespace {
const BasicGraphPattern &firstBgp(const GroupGraphPattern &g) {
	return static_cast<const BasicGraphPattern &>(*g.elements[0]);
}
} // namespace

TEST_CASE("Predicate-object lists and object lists share a subject/predicate (spec 4.2.1/4.2.2)") {
	Parser parser;
	auto q = parser.parseString("PREFIX : <http://ex/>\n"
	                             "SELECT * WHERE { ?x :name ?n ; :nick \"A\", \"B\" }");
	const auto &bgp = firstBgp(*q->where);
	REQUIRE(bgp.triples.size() == 3);
	REQUIRE(bgp.triples[1].predicate->kind() == PathKind::Predicate);
	REQUIRE(static_cast<const RdfLiteral &>(*bgp.triples[1].object).lexicalForm == "A");
	REQUIRE(static_cast<const RdfLiteral &>(*bgp.triples[2].object).lexicalForm == "B");
}

TEST_CASE("'a' is sugar for rdf:type (spec 4.2.4)") {
	Parser parser;
	auto q = parser.parseString("SELECT * WHERE { ?x a <urn:Class1> }");
	const auto &bgp = firstBgp(*q->where);
	const auto &pred = static_cast<const PredicatePath &>(*bgp.triples[0].predicate);
	REQUIRE(pred.iri->value == "http://www.w3.org/1999/02/22-rdf-syntax-ns#type");
}

TEST_CASE("RDF collections desugar into an rdf:first/rdf:rest/rdf:nil chain (spec 4.2.3)") {
	Parser parser;
	auto q = parser.parseFile(SOURCE_SPARQL_DIR "collection_and_blank_node_property_list.rq");
	const auto &bgp = firstBgp(*q->where);
	// (1 ?x 3 4) generates 4 rdf:first triples, 4 rdf:rest triples (the
	// last pointing at rdf:nil), plus the ":hasMember" link triple = 9.
	int firstCount = 0, restCount = 0;
	for (const auto &tp : bgp.triples) {
		const auto &pred = static_cast<const PredicatePath &>(*tp.predicate);
		if (pred.iri->value == "http://www.w3.org/1999/02/22-rdf-syntax-ns#first") ++firstCount;
		if (pred.iri->value == "http://www.w3.org/1999/02/22-rdf-syntax-ns#rest") ++restCount;
	}
	REQUIRE(firstCount == 4);
	REQUIRE(restCount == 4);
}

TEST_CASE("An empty collection '()' is the IRI rdf:nil, not a blank node chain") {
	Parser parser;
	auto q = parser.parseString("SELECT * WHERE { ?x <urn:p> () }");
	const auto &bgp = firstBgp(*q->where);
	REQUIRE(bgp.triples.size() == 1);
	const auto &obj = static_cast<const Iri &>(*bgp.triples[0].object);
	REQUIRE(obj.value == "http://www.w3.org/1999/02/22-rdf-syntax-ns#nil");
}

TEST_CASE("Blank node property lists '[ ... ]' generate a fresh blank node subject (spec 4.1.4)") {
	Parser parser;
	auto q = parser.parseFile(SOURCE_SPARQL_DIR "collection_and_blank_node_property_list.rq");
	const auto &bgp = firstBgp(*q->where);
	int pCount = 0, qCount = 0, linksCount = 0;
	for (const auto &tp : bgp.triples) {
		const auto &pred = static_cast<const PredicatePath &>(*tp.predicate);
		if (pred.iri->value == "http://example.org/ns#p") ++pCount;
		if (pred.iri->value == "http://example.org/ns#q") ++qCount;
		if (pred.iri->value == "http://example.org/ns#links") ++linksCount;
	}
	REQUIRE(pCount == 1);
	REQUIRE(qCount == 1);
	REQUIRE(linksCount == 1);
}

TEST_CASE("Anonymous blank node '[]' is distinct from a plain blank node label") {
	Parser parser;
	auto q = parser.parseString("SELECT * WHERE { [] <urn:p> ?o }");
	const auto &bgp = firstBgp(*q->where);
	const auto &subj = static_cast<const BlankNode &>(*bgp.triples[0].subject);
	REQUIRE(subj.anonymous);
}

TEST_CASE("OPTIONAL wraps its inner pattern (spec 6.1)") {
	Parser parser;
	auto q = parser.parseFile(SOURCE_SPARQL_DIR "optional_pattern.rq");
	REQUIRE(q->where->elements.size() == 2);
	REQUIRE(q->where->elements[0]->kind() == ElementKind::BasicGraphPattern);
	REQUIRE(q->where->elements[1]->kind() == ElementKind::OptionalGraphPattern);
	const auto &opt = static_cast<const OptionalGraphPattern &>(*q->where->elements[1]);
	REQUIRE(firstBgp(*opt.pattern).triples.size() == 1);
}

TEST_CASE("UNION with two branches produces a UnionGraphPattern with 2 branches (spec 7)") {
	Parser parser;
	auto q = parser.parseFile(SOURCE_SPARQL_DIR "union_pattern.rq");
	REQUIRE(q->where->elements[0]->kind() == ElementKind::UnionGraphPattern);
	const auto &u = static_cast<const UnionGraphPattern &>(*q->where->elements[0]);
	REQUIRE(u.branches.size() == 2);
}

TEST_CASE("Chained UNION (three alternatives) accumulates into one UnionGraphPattern") {
	Parser parser;
	auto q = parser.parseString("PREFIX : <http://ex/>\n"
	                             "SELECT * WHERE { { ?s :p1 ?v } UNION { ?s :p2 ?v } UNION { ?s :p3 ?v } }");
	const auto &u = static_cast<const UnionGraphPattern &>(*q->where->elements[0]);
	REQUIRE(u.branches.size() == 3);
}

TEST_CASE("A single-branch group (no UNION) collapses to a plain GroupGraphPattern element") {
	Parser parser;
	auto q = parser.parseString("SELECT * WHERE { { ?s ?p ?o } }");
	REQUIRE(q->where->elements[0]->kind() == ElementKind::GroupGraphPattern);
}

TEST_CASE("MINUS wraps its inner pattern (spec 8.2)") {
	Parser parser;
	auto q = parser.parseFile(SOURCE_SPARQL_DIR "minus_pattern.rq");
	REQUIRE(q->distinct);
	bool sawMinus = false;
	for (const auto &el : q->where->elements) {
		if (el->kind() == ElementKind::MinusGraphPattern) sawMinus = true;
	}
	REQUIRE(sawMinus);
}

TEST_CASE("FILTER EXISTS / NOT EXISTS carry their nested GroupGraphPattern (spec 8.1)") {
	Parser parser;
	auto q = parser.parseString("PREFIX : <http://ex/>\n"
	                             "SELECT ?p WHERE { ?p a :Person . FILTER NOT EXISTS { ?p :name ?n } }");
	const auto &filter = static_cast<const Filter &>(*q->where->elements[1]);
	REQUIRE(filter.constraint->kind() == ExprKind::Exists);
	const auto &exists = static_cast<const ExistsExpr &>(*filter.constraint);
	REQUIRE(exists.negated);
	REQUIRE(exists.pattern != nullptr);
}

TEST_CASE("BIND introduces a fresh variable bound to an expression (spec 10.1)") {
	Parser parser;
	auto q = parser.parseFile(SOURCE_SPARQL_DIR "bind_values.rq");
	bool sawBind = false;
	for (const auto &el : q->where->elements) {
		if (el->kind() == ElementKind::Bind) {
			sawBind = true;
			REQUIRE(static_cast<const Bind &>(*el).var->name == "price");
		}
	}
	REQUIRE(sawBind);
	REQUIRE(q->valuesClause != nullptr);
	REQUIRE(q->valuesClause->vars.size() == 2);
	REQUIRE(q->valuesClause->rows.size() == 2);
	// UNDEF in the first row's first column.
	REQUIRE(q->valuesClause->rows[0][0] == nullptr);
}

TEST_CASE("VALUES with a single variable uses the InlineDataOneVar short form (spec 10.2.1)") {
	Parser parser;
	auto q = parser.parseString("SELECT * WHERE { VALUES ?z { \"abc\" \"def\" } }");
	const auto &values = static_cast<const InlineData &>(*q->where->elements[0]);
	REQUIRE(values.vars.size() == 1);
	REQUIRE(values.rows.size() == 2);
	REQUIRE(values.rows[0].size() == 1);
}

TEST_CASE("Subqueries embed a nested Query tagged with Select form (spec 12)") {
	Parser parser;
	auto q = parser.parseFile(SOURCE_SPARQL_DIR "subquery.rq");
	bool sawSubSelect = false;
	for (const auto &el : q->where->elements) {
		if (el->kind() == ElementKind::GroupGraphPattern) {
			const auto &inner = static_cast<const GroupGraphPattern &>(*el);
			if (!inner.elements.empty() && inner.elements[0]->kind() == ElementKind::SubSelect) {
				sawSubSelect = true;
				const auto &sub = static_cast<const SubSelectElement &>(*inner.elements[0]);
				REQUIRE(sub.query->form == QueryForm::Select);
				REQUIRE(sub.query->solutionModifier.groupBy.size() == 1);
			}
		}
	}
	REQUIRE(sawSubSelect);
}

TEST_CASE("GRAPH with an explicit IRI sets graphNameOrVar to an Iri") {
	Parser parser;
	auto q = parser.parseString("SELECT * WHERE { GRAPH <urn:g> { ?s ?p ?o } }");
	const auto &g = static_cast<const GraphGraphPattern &>(*q->where->elements[0]);
	REQUIRE(g.graphNameOrVar->kind() == TermKind::Iri);
}

TEST_CASE("SERVICE SILENT sets the silent flag and keeps the endpoint term") {
	Parser parser;
	auto q = parser.parseString("SELECT * WHERE { SERVICE SILENT <urn:ep> { ?s ?p ?o } }");
	const auto &s = static_cast<const ServiceGraphPattern &>(*q->where->elements[0]);
	REQUIRE(s.silent);
	REQUIRE(s.endpoint->kind() == TermKind::Iri);
}
