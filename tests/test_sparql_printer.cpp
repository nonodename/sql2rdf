#include <catch2/catch_test_macros.hpp>

#include <sstream>
#include <string>

#ifndef SOURCE_SPARQL_DIR
#define SOURCE_SPARQL_DIR ""
#endif

#include "sparql-parser/Parser.h"
#include "sparql-parser/PrettyPrinter.h"

using namespace sparql;

namespace {
std::string printToString(const ast::Query &q) {
	std::ostringstream oss;
	print(oss, q);
	return oss.str();
}
} // namespace

TEST_CASE("print() marks the query form and projected variables") {
	Parser parser;
	auto q = parser.parseFile(SOURCE_SPARQL_DIR "simple_select.rq");
	std::string out = printToString(*q);
	REQUIRE(out.find("Query [SELECT]") != std::string::npos);
	REQUIRE(out.find("?title") != std::string::npos);
	REQUIRE(out.find("TriplePattern") != std::string::npos);
	REQUIRE(out.find("dc:title") != std::string::npos);
}

TEST_CASE("operator<< delegates to print() and produces identical output") {
	Parser parser;
	auto q = parser.parseFile(SOURCE_SPARQL_DIR "ask_query.rq");
	std::ostringstream a, b;
	print(a, *q);
	b << *q;
	REQUIRE(a.str() == b.str());
	REQUIRE(a.str().find("Query [ASK]") != std::string::npos);
}

TEST_CASE("print() renders OPTIONAL, UNION, and MINUS as distinctly labeled nodes") {
	Parser parser;
	auto opt = parser.parseFile(SOURCE_SPARQL_DIR "optional_pattern.rq");
	REQUIRE(printToString(*opt).find("Optional") != std::string::npos);

	auto un = parser.parseFile(SOURCE_SPARQL_DIR "union_pattern.rq");
	REQUIRE(printToString(*un).find("Union") != std::string::npos);

	auto minus = parser.parseFile(SOURCE_SPARQL_DIR "minus_pattern.rq");
	REQUIRE(printToString(*minus).find("Minus") != std::string::npos);
}

TEST_CASE("print() shows aggregate expressions and GROUP BY/HAVING sections") {
	Parser parser;
	auto q = parser.parseFile(SOURCE_SPARQL_DIR "aggregate_group_having.rq");
	std::string out = printToString(*q);
	REQUIRE(out.find("SUM(?lprice)") != std::string::npos);
	REQUIRE(out.find("GroupBy") != std::string::npos);
	REQUIRE(out.find("Having") != std::string::npos);
}

TEST_CASE("print() renders a nested SubSelect under its enclosing GroupGraphPattern") {
	Parser parser;
	auto q = parser.parseFile(SOURCE_SPARQL_DIR "subquery.rq");
	std::string out = printToString(*q);
	REQUIRE(out.find("SubSelect") != std::string::npos);
	REQUIRE(out.find("MIN(?name)") != std::string::npos);
}

TEST_CASE("print() renders property paths compactly on the owning TriplePattern line") {
	Parser parser;
	auto q = parser.parseFile(SOURCE_SPARQL_DIR "property_paths.rq");
	std::string out = printToString(*q);
	REQUIRE(out.find("foaf:knows+/foaf:name") != std::string::npos);
}
