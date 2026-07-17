#include <catch2/catch_test_macros.hpp>

#include <string>

#ifndef SOURCE_SPARQL_DIR
#define SOURCE_SPARQL_DIR ""
#endif

#include "sparql-parser/Parser.h"

using namespace sparql;
using namespace sparql::ast;

TEST_CASE("GROUP BY with a plain variable and HAVING over an aggregate (spec 11.1)") {
	Parser parser;
	auto q = parser.parseFile(SOURCE_SPARQL_DIR "aggregate_group_having.rq");
	REQUIRE(q->solutionModifier.groupBy.size() == 1);
	REQUIRE(q->solutionModifier.groupBy[0].expr->kind() == ExprKind::VarRef);
	REQUIRE(q->solutionModifier.groupBy[0].asVar == nullptr);
	REQUIRE(q->solutionModifier.having.size() == 1);
	REQUIRE(q->selectItems[0].expr->kind() == ExprKind::Aggregate);
}

TEST_CASE("GROUP BY (expr AS var) captures the alias") {
	Parser parser;
	auto q = parser.parseString("SELECT ?z WHERE { ?a <urn:x> ?x ; <urn:y> ?y } GROUP BY (?x + ?y AS ?z)");
	REQUIRE(q->solutionModifier.groupBy.size() == 1);
	REQUIRE(q->solutionModifier.groupBy[0].asVar->name == "z");
	REQUIRE(q->solutionModifier.groupBy[0].expr->kind() == ExprKind::Binary);
}

TEST_CASE("Multiple GROUP BY conditions are all captured") {
	Parser parser;
	auto q = parser.parseString("SELECT * WHERE { ?x <urn:p> ?y ; <urn:q> ?z } GROUP BY ?y ?z");
	REQUIRE(q->solutionModifier.groupBy.size() == 2);
}

TEST_CASE("ORDER BY ASC()/DESC() and a bare variable (implicit ASC)") {
	Parser parser;
	auto q = parser.parseString("SELECT ?name WHERE { ?x <urn:name> ?name ; <urn:emp> ?emp } "
	                            "ORDER BY ?name DESC(?emp)");
	REQUIRE(q->solutionModifier.orderBy.size() == 2);
	REQUIRE(q->solutionModifier.orderBy[0].direction == OrderDirection::Asc);
	REQUIRE(q->solutionModifier.orderBy[1].direction == OrderDirection::Desc);
}

TEST_CASE("DISTINCT + ORDER BY DESC + LIMIT + OFFSET all populate SolutionModifier") {
	Parser parser;
	auto q = parser.parseFile(SOURCE_SPARQL_DIR "solution_modifiers.rq");
	REQUIRE(q->distinct);
	REQUIRE(q->solutionModifier.orderBy.size() == 1);
	REQUIRE(q->solutionModifier.orderBy[0].direction == OrderDirection::Desc);
	REQUIRE(q->solutionModifier.hasLimit);
	REQUIRE(q->solutionModifier.limit == 5);
	REQUIRE(q->solutionModifier.hasOffset);
	REQUIRE(q->solutionModifier.offset == 10);
}

TEST_CASE("LIMIT and OFFSET may appear in either order") {
	Parser parser;
	auto limitFirst = parser.parseString("SELECT * WHERE { ?s ?p ?o } LIMIT 1 OFFSET 2");
	REQUIRE(limitFirst->solutionModifier.limit == 1);
	REQUIRE(limitFirst->solutionModifier.offset == 2);

	auto offsetFirst = parser.parseString("SELECT * WHERE { ?s ?p ?o } OFFSET 2 LIMIT 1");
	REQUIRE(offsetFirst->solutionModifier.limit == 1);
	REQUIRE(offsetFirst->solutionModifier.offset == 2);
}

TEST_CASE("A trailing VALUES clause on the outer query is distinct from an inner VALUES element") {
	Parser parser;
	auto q = parser.parseString("SELECT ?book WHERE { ?book <urn:title> ?title } VALUES ?book { <urn:b1> <urn:b2> }");
	REQUIRE(q->where->elements.size() == 1);
	REQUIRE(q->valuesClause != nullptr);
	REQUIRE(q->valuesClause->rows.size() == 2);
}
