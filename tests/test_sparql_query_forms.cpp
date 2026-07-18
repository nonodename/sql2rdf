#include <catch2/catch_test_macros.hpp>

#include <string>

// Fallback for IDE tooling; CMake overrides this via target_compile_definitions.
#ifndef SOURCE_SPARQL_DIR
#define SOURCE_SPARQL_DIR ""
#endif

#include "sparql-parser/Parser.h"

using sparql::ParseError;
using sparql::Parser;
using sparql::ast::BasicGraphPattern;
using sparql::ast::DatasetClauseKind;
using sparql::ast::ElementKind;
using sparql::ast::GraphGraphPattern;
using sparql::ast::Iri;
using sparql::ast::QueryForm;
using sparql::ast::SelectItem;
using sparql::ast::TermKind;
using sparql::ast::UnionGraphPattern;

TEST_CASE("Simple SELECT with a single triple pattern (spec 2.1)") {
	Parser parser;
	auto q = parser.parseFile(SOURCE_SPARQL_DIR "simple_select.rq");
	REQUIRE(q->form == QueryForm::Select);
	REQUIRE(q->prologue.prefixes.size() == 1);
	REQUIRE_FALSE(q->selectStar);
	REQUIRE(q->selectItems.size() == 1);
	REQUIRE(q->selectItems[0].var->name == "title");
	REQUIRE(q->where != nullptr);
	REQUIRE(q->where->elements.size() == 1);
	const auto &bgp = static_cast<const BasicGraphPattern &>(*q->where->elements[0]);
	REQUIRE(bgp.triples.size() == 1);
	REQUIRE(bgp.triples[0].subject->kind() == TermKind::Iri);
}

TEST_CASE("SELECT * projects all in-scope variables via selectStar") {
	Parser parser;
	auto q = parser.parseString("SELECT * WHERE { ?s ?p ?o }");
	REQUIRE(q->selectStar);
	REQUIRE(q->selectItems.empty());
}

TEST_CASE("SELECT DISTINCT and SELECT REDUCED are mutually exclusive flags") {
	Parser parser;
	auto distinctQ = parser.parseString("SELECT DISTINCT ?x WHERE { ?x ?p ?o }");
	REQUIRE(distinctQ->distinct);
	REQUIRE_FALSE(distinctQ->reduced);

	auto reducedQ = parser.parseString("SELECT REDUCED ?x WHERE { ?x ?p ?o }");
	REQUIRE(reducedQ->reduced);
	REQUIRE_FALSE(reducedQ->distinct);
}

TEST_CASE("SELECT expression AS variable is projected as a SelectItem with an expr") {
	Parser parser;
	auto q = parser.parseString("SELECT ?x (?x + 1 AS ?y) WHERE { ?x ?p ?o }");
	REQUIRE(q->selectItems.size() == 2);
	REQUIRE(q->selectItems[0].expr == nullptr);
	REQUIRE(q->selectItems[1].expr != nullptr);
	REQUIRE(q->selectItems[1].var->name == "y");
}

TEST_CASE("CONSTRUCT template triples are independent of the WHERE pattern's triples (spec 16.2.1)") {
	Parser parser;
	auto q = parser.parseFile(SOURCE_SPARQL_DIR "construct_template.rq");
	REQUIRE(q->form == QueryForm::Construct);
	REQUIRE(q->hasConstructTemplate);
	REQUIRE(q->constructTemplate.size() == 3);
	REQUIRE(q->where != nullptr);
	// The WHERE clause here is two UNIONs, not a plain BasicGraphPattern.
	REQUIRE(q->where->elements.size() == 2);
	REQUIRE(q->where->elements[0]->kind() == ElementKind::UnionGraphPattern);
}

TEST_CASE("CONSTRUCT WHERE short form uses the same triples for template and pattern") {
	Parser parser;
	auto q = parser.parseFile(SOURCE_SPARQL_DIR "construct_where_short.rq");
	REQUIRE(q->form == QueryForm::Construct);
	REQUIRE(q->constructTemplate.size() == 1);
	const auto &bgp = static_cast<const BasicGraphPattern &>(*q->where->elements[0]);
	REQUIRE(bgp.triples.size() == 1);
}

TEST_CASE("DESCRIBE with explicit variables and an IRI target (spec 16.4.2)") {
	Parser parser;
	auto q = parser.parseFile(SOURCE_SPARQL_DIR "describe_query.rq");
	REQUIRE(q->form == QueryForm::Describe);
	REQUIRE_FALSE(q->describeStar);
	REQUIRE(q->describeTargets.size() == 3);
	REQUIRE(q->describeTargets[0]->kind() == TermKind::Var);
	REQUIRE(q->describeTargets[1]->kind() == TermKind::Var);
	REQUIRE(q->describeTargets[2]->kind() == TermKind::Iri);
	REQUIRE(q->where != nullptr);
}

TEST_CASE("DESCRIBE * has no WHERE clause and no explicit targets") {
	Parser parser;
	auto q = parser.parseString("DESCRIBE *");
	REQUIRE(q->describeStar);
	REQUIRE(q->describeTargets.empty());
	REQUIRE(q->where == nullptr);
}

TEST_CASE("ASK returns a query with a WHERE clause but no projection") {
	Parser parser;
	auto q = parser.parseFile(SOURCE_SPARQL_DIR "ask_query.rq");
	REQUIRE(q->form == QueryForm::Ask);
	REQUIRE(q->where != nullptr);
}

TEST_CASE("FROM and FROM NAMED populate datasetClauses; GRAPH ?var wraps the pattern") {
	Parser parser;
	auto q = parser.parseFile(SOURCE_SPARQL_DIR "dataset_graph.rq");
	REQUIRE(q->datasetClauses.size() == 2);
	REQUIRE(q->datasetClauses[0].kind == DatasetClauseKind::Named);
	REQUIRE(q->datasetClauses[1].kind == DatasetClauseKind::Named);
	REQUIRE(q->where->elements[0]->kind() == ElementKind::GraphGraphPattern);
	const auto &g = static_cast<const GraphGraphPattern &>(*q->where->elements[0]);
	REQUIRE(g.graphNameOrVar->kind() == TermKind::Var);
}

TEST_CASE("A trailing PREFIX redeclaration overrides the earlier binding") {
	Parser parser;
	auto q = parser.parseString("PREFIX : <http://a/>\nPREFIX : <http://b/>\nSELECT * WHERE { ?s : ?o }");
	REQUIRE(q->prologue.prefixes.size() == 1);
	REQUIRE(q->prologue.prefixes[0].iri == "http://b/");
}

TEST_CASE("Unknown prefix use raises a ParseError") {
	Parser parser;
	REQUIRE_THROWS_AS(parser.parseString("SELECT * WHERE { ?s undeclared:p ?o }"), ParseError);
}
