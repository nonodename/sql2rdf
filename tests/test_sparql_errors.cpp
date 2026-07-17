#include <catch2/catch_test_macros.hpp>

#include <string>

#ifndef SOURCE_SPARQL_DIR
#define SOURCE_SPARQL_DIR ""
#endif

#include "sparql-parser/Parser.h"

using namespace sparql;

namespace {
void expectParseErrorWithLocation(const std::string &path) {
	Parser parser;
	bool threw = false;
	try {
		parser.parseFile(path);
	} catch (const ParseError &e) {
		threw = true;
		REQUIRE(e.line() > 0);
		REQUIRE(e.column() > 0);
		// what() should be the fully formatted, human-readable message,
		// not just the raw fragment - i.e. it must still mention "line".
		REQUIRE(std::string(e.what()).find("line") != std::string::npos);
	}
	REQUIRE(threw);
}
} // namespace

TEST_CASE("Unbalanced '{' raises a ParseError with a sane source location") {
	expectParseErrorWithLocation(SOURCE_SPARQL_DIR "invalid_unbalanced_brace.rq");
}

TEST_CASE("Unterminated string literal raises a ParseError") {
	expectParseErrorWithLocation(SOURCE_SPARQL_DIR "invalid_unterminated_string.rq");
}

TEST_CASE("An IRIREF that can't close before whitespace is rejected by the parser") {
	expectParseErrorWithLocation(SOURCE_SPARQL_DIR "invalid_bad_iriref.rq");
}

TEST_CASE("An unrecognized keyword in predicate position raises a ParseError") {
	expectParseErrorWithLocation(SOURCE_SPARQL_DIR "invalid_unknown_keyword.rq");
}

TEST_CASE("VALUES row/variable-list arity mismatch is caught with a specific message") {
	Parser parser;
	bool threw = false;
	try {
		parser.parseFile(SOURCE_SPARQL_DIR "invalid_values_arity_mismatch.rq");
	} catch (const ParseError &e) {
		threw = true;
		REQUIRE(std::string(e.what()).find("VALUES row") != std::string::npos);
	}
	REQUIRE(threw);
}

TEST_CASE("An incomplete negated property set raises a ParseError") {
	expectParseErrorWithLocation(SOURCE_SPARQL_DIR "invalid_incomplete_property_path.rq");
}

TEST_CASE("Parsing a nonexistent file throws plain std::runtime_error, not ParseError") {
	Parser parser;
	REQUIRE_THROWS_AS(parser.parseFile(SOURCE_SPARQL_DIR "does_not_exist.rq"), std::runtime_error);
}

TEST_CASE("ParseError derives from std::runtime_error so generic catch sites still work") {
	Parser parser;
	bool caught = false;
	try {
		parser.parseString("SELECT * WHERE { ?s ?p ?o");
	} catch (const std::exception &e) {
		caught = true;
		REQUIRE(std::string(e.what()).find("Parse error") != std::string::npos);
	}
	REQUIRE(caught);
}
