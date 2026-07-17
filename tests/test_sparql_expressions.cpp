#include <catch2/catch_test_macros.hpp>

#include <string>

#ifndef SOURCE_SPARQL_DIR
#define SOURCE_SPARQL_DIR ""
#endif

#include "sparql-parser/Parser.h"

using namespace sparql;
using namespace sparql::ast;

namespace {
const Expression &filterExprOf(const Query &q, std::size_t elementIndex = 1) {
	return *static_cast<const Filter &>(*q.where->elements[elementIndex]).constraint;
}
} // namespace

TEST_CASE("Operator precedence: && binds tighter than ||") {
	Parser parser;
	// a || b && c  ==  a || (b && c)
	auto q = parser.parseString("SELECT * WHERE { ?a ?p ?x FILTER(?a = 1 || ?a = 2 && ?a = 3) }");
	const auto &orExpr = static_cast<const BinaryExpr &>(filterExprOf(*q));
	REQUIRE(orExpr.op == BinaryOp::Or);
	REQUIRE(orExpr.right->kind() == ExprKind::Binary);
	REQUIRE(static_cast<const BinaryExpr &>(*orExpr.right).op == BinaryOp::And);
}

TEST_CASE("Operator precedence: relational binds tighter than &&, and is non-chaining") {
	Parser parser;
	auto q = parser.parseString("SELECT * WHERE { ?a ?p ?x FILTER(?a < 5 && ?a > 1) }");
	const auto &andExpr = static_cast<const BinaryExpr &>(filterExprOf(*q));
	REQUIRE(andExpr.op == BinaryOp::And);
	REQUIRE(static_cast<const BinaryExpr &>(*andExpr.left).op == BinaryOp::Lt);
	REQUIRE(static_cast<const BinaryExpr &>(*andExpr.right).op == BinaryOp::Gt);
}

TEST_CASE("Operator precedence: * binds tighter than +/-") {
	Parser parser;
	auto q = parser.parseString("SELECT * WHERE { ?a ?p ?x FILTER(?a = 1 + 2 * 3) }");
	const auto &eq = static_cast<const BinaryExpr &>(filterExprOf(*q));
	const auto &add = static_cast<const BinaryExpr &>(*eq.right);
	REQUIRE(add.op == BinaryOp::Add);
	REQUIRE(add.right->kind() == ExprKind::Binary);
	REQUIRE(static_cast<const BinaryExpr &>(*add.right).op == BinaryOp::Mul);
}

TEST_CASE("Unary '!' negates a parenthesized expression") {
	Parser parser;
	auto q = parser.parseString("SELECT * WHERE { ?a ?p ?x FILTER(!(?a = 1)) }");
	const auto &notExpr = static_cast<const UnaryExpr &>(filterExprOf(*q));
	REQUIRE(notExpr.op == UnaryOp::Not);
}

TEST_CASE("A signed numeral glued to the previous token is still a subtraction (grammar note 6)") {
	Parser parser;
	auto q = parser.parseString("SELECT * WHERE { ?a ?p ?n FILTER(?n-1*2>0) }");
	const auto &gt = static_cast<const BinaryExpr &>(filterExprOf(*q));
	REQUIRE(gt.op == BinaryOp::Gt);
	const auto &sub = static_cast<const BinaryExpr &>(*gt.left);
	REQUIRE(sub.op == BinaryOp::Sub);
	REQUIRE(sub.left->kind() == ExprKind::VarRef);
	// The "1*2" continues to multiply, per rule 116's trailing repetition.
	REQUIRE(sub.right->kind() == ExprKind::Binary);
	REQUIRE(static_cast<const BinaryExpr &>(*sub.right).op == BinaryOp::Mul);
}

TEST_CASE("IN and NOT IN build InExpr nodes with the right negated flag") {
	Parser parser;
	auto in = parser.parseString("SELECT * WHERE { ?a ?p ?n FILTER(?n IN (1, 2, 3)) }");
	const auto &inExpr = static_cast<const InExpr &>(filterExprOf(*in));
	REQUIRE_FALSE(inExpr.negated);
	REQUIRE(inExpr.list.size() == 3);

	auto notIn = parser.parseString("SELECT * WHERE { ?a ?p ?n FILTER(?n NOT IN (1, 2)) }");
	const auto &notInExpr = static_cast<const InExpr &>(filterExprOf(*notIn));
	REQUIRE(notInExpr.negated);
	REQUIRE(notInExpr.list.size() == 2);
}

TEST_CASE("BOUND(?var) is represented with the variable wrapped as its sole argument") {
	Parser parser;
	auto q = parser.parseString("SELECT * WHERE { ?a ?p ?x OPTIONAL { ?a ?q ?y } FILTER(bound(?y)) }");
	const auto &call = static_cast<const BuiltInCallExpr &>(filterExprOf(*q, 2));
	REQUIRE(call.fn == BuiltinFunction::Bound);
	REQUIRE(call.args.size() == 1);
	REQUIRE(call.args[0]->kind() == ExprKind::VarRef);
}

TEST_CASE("IF takes exactly three expressions") {
	Parser parser;
	auto q = parser.parseString("SELECT (IF(?x = 1, \"one\", \"other\") AS ?label) WHERE { ?x ?p ?o }");
	const auto &call = static_cast<const BuiltInCallExpr &>(*q->selectItems[0].expr);
	REQUIRE(call.fn == BuiltinFunction::If);
	REQUIRE(call.args.size() == 3);
}

TEST_CASE("REGEX/SUBSTR/REPLACE accept their optional trailing argument") {
	Parser parser;
	auto regexNoFlags = parser.parseString("SELECT * WHERE { ?x ?p ?s FILTER(REGEX(?s, \"^a\")) }");
	REQUIRE(static_cast<const BuiltInCallExpr &>(filterExprOf(*regexNoFlags)).args.size() == 2);

	auto regexFlags = parser.parseString("SELECT * WHERE { ?x ?p ?s FILTER(REGEX(?s, \"^a\", \"i\")) }");
	REQUIRE(static_cast<const BuiltInCallExpr &>(filterExprOf(*regexFlags)).args.size() == 3);

	auto substr3 = parser.parseString("SELECT (SUBSTR(\"foobar\", 4, 1) AS ?s) WHERE { ?x ?p ?o }");
	REQUIRE(static_cast<const BuiltInCallExpr &>(*substr3->selectItems[0].expr).fn == BuiltinFunction::Substr);

	auto replace4 = parser.parseString("SELECT (REPLACE(\"abab\", \"B.\", \"Z\", \"i\") AS ?s) WHERE { ?x ?p ?o }");
	REQUIRE(static_cast<const BuiltInCallExpr &>(*replace4->selectItems[0].expr).args.size() == 4);
}

TEST_CASE("Aggregates: COUNT(*), COUNT(DISTINCT expr), and GROUP_CONCAT with SEPARATOR") {
	Parser parser;
	auto countStar = parser.parseString("SELECT (COUNT(*) AS ?c) WHERE { ?x ?p ?o }");
	const auto &c = static_cast<const AggregateExpr &>(*countStar->selectItems[0].expr);
	REQUIRE(c.aggKind == AggregateKind::Count);
	REQUIRE(c.star);
	REQUIRE(c.arg == nullptr);

	auto countDistinct = parser.parseString("SELECT (COUNT(DISTINCT ?x) AS ?c) WHERE { ?x ?p ?o }");
	const auto &cd = static_cast<const AggregateExpr &>(*countDistinct->selectItems[0].expr);
	REQUIRE(cd.distinct);
	REQUIRE_FALSE(cd.star);
	REQUIRE(cd.arg != nullptr);

	auto gc = parser.parseString("SELECT (GROUP_CONCAT(?n; SEPARATOR=\",\") AS ?names) WHERE { ?x ?p ?n }");
	const auto &gcExpr = static_cast<const AggregateExpr &>(*gc->selectItems[0].expr);
	REQUIRE(gcExpr.aggKind == AggregateKind::GroupConcat);
	REQUIRE(gcExpr.hasSeparator);
	REQUIRE(gcExpr.separator == ",");
}

TEST_CASE("Numeric literal sugar assigns the correct xsd datatype (spec 4.1.2)") {
	Parser parser;
	auto q = parser.parseString("SELECT * WHERE { ?x <urn:p> 1 . ?x <urn:q> 1.3 . ?x <urn:r> 1.0e6 }");
	const auto &bgp = static_cast<const BasicGraphPattern &>(*q->where->elements[0]);
	REQUIRE(static_cast<const RdfLiteral &>(*bgp.triples[0].object).datatype->value ==
	        "http://www.w3.org/2001/XMLSchema#integer");
	REQUIRE(static_cast<const RdfLiteral &>(*bgp.triples[1].object).datatype->value ==
	        "http://www.w3.org/2001/XMLSchema#decimal");
	REQUIRE(static_cast<const RdfLiteral &>(*bgp.triples[2].object).datatype->value ==
	        "http://www.w3.org/2001/XMLSchema#double");
}

TEST_CASE("Boolean literal sugar assigns xsd:boolean") {
	Parser parser;
	auto q = parser.parseString("SELECT * WHERE { ?x <urn:p> true . ?x <urn:q> false }");
	const auto &bgp = static_cast<const BasicGraphPattern &>(*q->where->elements[0]);
	REQUIRE(static_cast<const RdfLiteral &>(*bgp.triples[0].object).lexicalForm == "true");
	REQUIRE(static_cast<const RdfLiteral &>(*bgp.triples[0].object).datatype->value ==
	        "http://www.w3.org/2001/XMLSchema#boolean");
}

TEST_CASE("A function call named by IRI is a FunctionCallExpr, distinct from BuiltInCallExpr") {
	Parser parser;
	auto q = parser.parseString("SELECT * WHERE { ?x ?p ?o FILTER(<http://example.org/fn#even>(?o)) }");
	REQUIRE(filterExprOf(*q).kind() == ExprKind::FunctionCall);
}

TEST_CASE("A bare IRI (no ArgList) is an IriExpr, not a function call") {
	Parser parser;
	auto q = parser.parseString("SELECT * WHERE { ?x ?p ?o FILTER(?o = <urn:x>) }");
	const auto &eq = static_cast<const BinaryExpr &>(filterExprOf(*q));
	REQUIRE(eq.right->kind() == ExprKind::IriRef);
}

TEST_CASE("A malformed expression (mismatched parens) raises a ParseError") {
	Parser parser;
	REQUIRE_THROWS_AS(parser.parseString("SELECT * WHERE { ?x ?p ?o FILTER(?o = 1 }"), ParseError);
}
