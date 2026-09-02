#include <catch2/catch.hpp>

#include <sstream>
#include <string>

#ifndef SOURCE_SPARQL_DIR
#define SOURCE_SPARQL_DIR ""
#endif

#include "sparql-parser/Parser.h"
#include "sparql-parser/PrettyPrinter.h"

using sparql::Parser;

namespace {
std::string printToString(const sparql::ast::Query &q) {
	std::ostringstream oss;
	sparql::print(oss, q);
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
	sparql::print(a, *q);
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
	// Inverse ('^'), Sequence with an inverse operand, and a negated property
	// set with both forward and inverse members all live on other triples in
	// the same fixture.
	REQUIRE(out.find("rdf:type/rdfs:subClassOf*") != std::string::npos);
	REQUIRE(out.find("^foaf:knows") != std::string::npos);
	REQUIRE(out.find("!(rdf:type|^rdf:type)") != std::string::npos);
	REQUIRE(out.find("(?x != ?y)") != std::string::npos);
}

TEST_CASE("print() renders an Alternative path wrapped in a ZeroOrOne path") {
	Parser parser;
	auto q = parser.parseString("SELECT * WHERE { ?x (<urn:p>|<urn:q>)? ?y }");
	std::string out = printToString(*q);
	REQUIRE(out.find("<urn:p>|<urn:q>?") != std::string::npos);
}

TEST_CASE("print() renders CONSTRUCT and DESCRIBE query forms") {
	Parser parser;
	auto construct = parser.parseFile(SOURCE_SPARQL_DIR "construct_template.rq");
	std::string constructOut = printToString(*construct);
	REQUIRE(constructOut.find("Query [CONSTRUCT]") != std::string::npos);
	REQUIRE(constructOut.find("ConstructTemplate") != std::string::npos);
	// The template's second/third triples share a blank-node subject.
	REQUIRE(constructOut.find("TriplePattern ?x vcard:N _:") != std::string::npos);

	auto describe = parser.parseFile(SOURCE_SPARQL_DIR "describe_query.rq");
	std::string describeOut = printToString(*describe);
	REQUIRE(describeOut.find("Query [DESCRIBE]") != std::string::npos);
	REQUIRE(describeOut.find("Describe") != std::string::npos);
	REQUIRE(describeOut.find("example.org") != std::string::npos);
}

TEST_CASE("print() renders SELECT * and an inline VALUES pattern element") {
	Parser parser;
	auto q = parser.parseString("SELECT * WHERE { VALUES (?x ?y) { (1 2) } }");
	std::string out = printToString(*q);
	REQUIRE(out.find("*\n") != std::string::npos);
	REQUIRE(out.find("Values (?x ?y) 1 row(s)") != std::string::npos);
}

TEST_CASE("print() renders GRAPH, SERVICE SILENT, BIND, and a top-level VALUES clause") {
	Parser parser;
	auto graph = parser.parseFile(SOURCE_SPARQL_DIR "dataset_graph.rq");
	std::string graphOut = printToString(*graph);
	REQUIRE(graphOut.find("FromNamed") != std::string::npos);
	REQUIRE(graphOut.find("Graph ?src") != std::string::npos);

	auto service = parser.parseString("SELECT * WHERE { SERVICE SILENT <urn:ep> { ?s ?p ?o } }");
	std::string serviceOut = printToString(*service);
	REQUIRE(serviceOut.find("Service SILENT <urn:ep>") != std::string::npos);

	auto bindValues = parser.parseFile(SOURCE_SPARQL_DIR "bind_values.rq");
	std::string bindOut = printToString(*bindValues);
	REQUIRE(bindOut.find("Bind ?price = (?p * (\"1\"") != std::string::npos);
	REQUIRE(bindOut.find("Filter (?price < \"20\"") != std::string::npos);
	REQUIRE(bindOut.find("Values (?x ?title) 2 row(s)") != std::string::npos);
}

TEST_CASE("print() renders EXISTS/NOT EXISTS filters with their nested pattern") {
	Parser parser;
	auto q = parser.parseString(
	    "SELECT * WHERE { ?s ?p ?o . FILTER EXISTS { ?s ?p2 ?o2 } FILTER NOT EXISTS { ?s ?p3 ?o3 } }");
	std::string out = printToString(*q);
	REQUIRE(out.find("Filter EXISTS {...}") != std::string::npos);
	REQUIRE(out.find("Filter NOT EXISTS {...}") != std::string::npos);
}

TEST_CASE("print() renders every remaining BinaryOp symbol and unary +/- signs") {
	Parser parser;
	auto q = parser.parseString("SELECT * WHERE { ?a ?p ?n FILTER("
	                            "(?n = 1) && (?n != 2) && (?n < 3) && (?n <= 4) && (?n > 5) && (?n >= 6) || "
	                            "(?n + 1 = 7) || (?n - 1 = 8) || (?n * 2 = 9) || (?n / 2 = 10) || "
	                            "(+?n = 100) || (-?n = 101)"
	                            ") }");
	std::string out = printToString(*q);
	// Numeric literals carry an xsd:integer datatype (see "Numeric literal
	// sugar" in test_sparql_expressions.cpp), so termStr appends a "^^<...>"
	// suffix after the quoted lexical form - match on the prefix only.
	REQUIRE(out.find("(?n = \"1\"") != std::string::npos);
	REQUIRE(out.find("(?n != \"2\"") != std::string::npos);
	REQUIRE(out.find("(?n < \"3\"") != std::string::npos);
	REQUIRE(out.find("(?n <= \"4\"") != std::string::npos);
	REQUIRE(out.find("(?n > \"5\"") != std::string::npos);
	REQUIRE(out.find("(?n >= \"6\"") != std::string::npos);
	REQUIRE(out.find("(?n + \"1\"") != std::string::npos);
	REQUIRE(out.find("(?n - \"1\"") != std::string::npos);
	REQUIRE(out.find("(?n * \"2\"") != std::string::npos);
	REQUIRE(out.find("(?n / \"2\"") != std::string::npos);
	REQUIRE(out.find("(+?n = \"100\"") != std::string::npos);
	REQUIRE(out.find("(-?n = \"101\"") != std::string::npos);
	REQUIRE(out.find("&&") != std::string::npos);
	REQUIRE(out.find("||") != std::string::npos);
}

TEST_CASE("print() renders IN and NOT IN expressions") {
	Parser parser;
	auto in = parser.parseString("SELECT * WHERE { ?a ?p ?n FILTER(?n IN (1, 2, 3)) }");
	REQUIRE(printToString(*in).find("?n IN (\"1\"") != std::string::npos);

	auto notIn = parser.parseString("SELECT * WHERE { ?a ?p ?n FILTER(?n NOT IN (1, 2)) }");
	REQUIRE(printToString(*notIn).find("?n NOT IN (\"1\"") != std::string::npos);
}

TEST_CASE("print() renders a non-builtin FunctionCall and a bare IriRef expression") {
	Parser parser;
	auto q = parser.parseString("SELECT * WHERE { ?x ?p ?o . "
	                            "FILTER(<http://example.org/fn#even>(?o)) "
	                            "FILTER(?o = <urn:x>) }");
	std::string out = printToString(*q);
	REQUIRE(out.find("<http://example.org/fn#even>(?o)") != std::string::npos);
	REQUIRE(out.find("(?o = <urn:x>)") != std::string::npos);
}

TEST_CASE("print() renders every AggregateKind, DISTINCT, and a GROUP_CONCAT separator") {
	Parser parser;
	auto q = parser.parseString("SELECT (AVG(?n) AS ?vAvg) (MAX(?n) AS ?vMax) (SAMPLE(?n) AS ?vSample) "
	                            "(GROUP_CONCAT(DISTINCT ?n; SEPARATOR=\",\") AS ?vGroupConcat) "
	                            "WHERE { ?x ?p ?n }");
	std::string out = printToString(*q);
	REQUIRE(out.find("AVG(?n)") != std::string::npos);
	REQUIRE(out.find("MAX(?n)") != std::string::npos);
	REQUIRE(out.find("SAMPLE(?n)") != std::string::npos);
	REQUIRE(out.find("GROUP_CONCAT(DISTINCT ?n; SEPARATOR=\",\")") != std::string::npos);
}

TEST_CASE("print() renders a GROUP BY (expr AS var) alias condition") {
	Parser parser;
	auto q = parser.parseString("SELECT ?sum WHERE { ?s <urn:a> ?oa . ?s <urn:b> ?ob } GROUP BY (?oa + ?ob AS ?sum)");
	std::string out = printToString(*q);
	REQUIRE(out.find("(?oa + ?ob) AS ?sum") != std::string::npos);
}

TEST_CASE("print() renders a language-tagged literal and a datatype-tagged literal") {
	Parser parser;
	auto q = parser.parseString("SELECT * WHERE { ?x <urn:p> \"hello\"@en . "
	                            "?x <urn:q> \"5\"^^<http://www.w3.org/2001/XMLSchema#integer> }");
	std::string out = printToString(*q);
	REQUIRE(out.find("\"hello\"@en") != std::string::npos);
	REQUIRE(out.find("\"5\"^^<http://www.w3.org/2001/XMLSchema#integer>") != std::string::npos);
}

TEST_CASE("print() renders builtin functions across numeric/string/hash/date/term-construction categories") {
	Parser parser;
	auto q = parser.parseString(
	    "SELECT "
	    "(LANG(?s) AS ?vLang) (DATATYPE(?s) AS ?vDatatype) (IRI(?s) AS ?vIri) (URI(?s) AS ?vUri) "
	    "(ABS(?n) AS ?vAbs) (CEIL(?n) AS ?vCeil) (FLOOR(?n) AS ?vFloor) (ROUND(?n) AS ?vRound) "
	    "(UCASE(?s) AS ?vUcase) (LCASE(?s) AS ?vLcase) (ENCODE_FOR_URI(?s) AS ?vEnc) "
	    "(YEAR(?d) AS ?vYear) (MONTH(?d) AS ?vMonth) (DAY(?d) AS ?vDay) (HOURS(?d) AS ?vHours) "
	    "(MINUTES(?d) AS ?vMinutes) (SECONDS(?d) AS ?vSeconds) (TIMEZONE(?d) AS ?vTz) (TZ(?d) AS ?vTzShort) "
	    "(MD5(?s) AS ?vMd5) (SHA1(?s) AS ?vSha1) (SHA256(?s) AS ?vSha256) (SHA384(?s) AS ?vSha384) "
	    "(SHA512(?s) AS ?vSha512) "
	    "(ISURI(?s) AS ?vIsUri) (ISLITERAL(?s) AS ?vIsLiteral) (ISNUMERIC(?n) AS ?vIsNumeric) "
	    "(LANGMATCHES(?s, ?s2) AS ?vLangMatches) (STRSTARTS(?s, ?s2) AS ?vStrStarts) "
	    "(STRENDS(?s, ?s2) AS ?vStrEnds) (STRBEFORE(?s, ?s2) AS ?vStrBefore) (STRAFTER(?s, ?s2) AS ?vStrAfter) "
	    "(STRLANG(?s, ?s2) AS ?vStrLang) (STRDT(?s, ?s2) AS ?vStrDt) (SAMETERM(?s, ?s2) AS ?vSameTerm) "
	    "(RAND() AS ?vRand) (NOW() AS ?vNow) (UUID() AS ?vUuid) (STRUUID() AS ?vStruuid) "
	    "(IF(?n = 1, \"a\", \"b\") AS ?vIf) (COALESCE(?s, ?s2) AS ?vCoalesce) (CONCAT(?s, ?s2) AS ?vConcat) "
	    "(BNODE() AS ?vBnode0) (BNODE(?s) AS ?vBnode1) "
	    "(SUBSTR(?s, 1, 3) AS ?vSubstr) (REPLACE(?s, \"a\", \"b\") AS ?vReplace) "
	    "WHERE { ?x ?p1 ?n . ?x ?p2 ?s . ?x ?p3 ?s2 . ?x ?p4 ?d }");
	std::string out = printToString(*q);
	REQUIRE(out.find("LANG(?s)") != std::string::npos);
	REQUIRE(out.find("DATATYPE(?s)") != std::string::npos);
	REQUIRE(out.find("IRI(?s)") != std::string::npos);
	REQUIRE(out.find("URI(?s)") != std::string::npos);
	REQUIRE(out.find("ABS(?n)") != std::string::npos);
	REQUIRE(out.find("CEIL(?n)") != std::string::npos);
	REQUIRE(out.find("FLOOR(?n)") != std::string::npos);
	REQUIRE(out.find("ROUND(?n)") != std::string::npos);
	REQUIRE(out.find("UCASE(?s)") != std::string::npos);
	REQUIRE(out.find("LCASE(?s)") != std::string::npos);
	REQUIRE(out.find("ENCODE_FOR_URI(?s)") != std::string::npos);
	REQUIRE(out.find("YEAR(?d)") != std::string::npos);
	REQUIRE(out.find("MONTH(?d)") != std::string::npos);
	REQUIRE(out.find("DAY(?d)") != std::string::npos);
	REQUIRE(out.find("HOURS(?d)") != std::string::npos);
	REQUIRE(out.find("MINUTES(?d)") != std::string::npos);
	REQUIRE(out.find("SECONDS(?d)") != std::string::npos);
	REQUIRE(out.find("TIMEZONE(?d)") != std::string::npos);
	REQUIRE(out.find("TZ(?d)") != std::string::npos);
	REQUIRE(out.find("MD5(?s)") != std::string::npos);
	REQUIRE(out.find("SHA1(?s)") != std::string::npos);
	REQUIRE(out.find("SHA256(?s)") != std::string::npos);
	REQUIRE(out.find("SHA384(?s)") != std::string::npos);
	REQUIRE(out.find("SHA512(?s)") != std::string::npos);
	REQUIRE(out.find("isURI(?s)") != std::string::npos);
	REQUIRE(out.find("isLITERAL(?s)") != std::string::npos);
	REQUIRE(out.find("isNUMERIC(?n)") != std::string::npos);
	REQUIRE(out.find("LANGMATCHES(?s, ?s2)") != std::string::npos);
	REQUIRE(out.find("STRSTARTS(?s, ?s2)") != std::string::npos);
	REQUIRE(out.find("STRENDS(?s, ?s2)") != std::string::npos);
	REQUIRE(out.find("STRBEFORE(?s, ?s2)") != std::string::npos);
	REQUIRE(out.find("STRAFTER(?s, ?s2)") != std::string::npos);
	REQUIRE(out.find("STRLANG(?s, ?s2)") != std::string::npos);
	REQUIRE(out.find("STRDT(?s, ?s2)") != std::string::npos);
	REQUIRE(out.find("sameTerm(?s, ?s2)") != std::string::npos);
	REQUIRE(out.find("RAND()") != std::string::npos);
	REQUIRE(out.find("NOW()") != std::string::npos);
	REQUIRE(out.find("UUID()") != std::string::npos);
	REQUIRE(out.find("STRUUID()") != std::string::npos);
	REQUIRE(out.find("IF(") != std::string::npos);
	REQUIRE(out.find("COALESCE(?s, ?s2)") != std::string::npos);
	REQUIRE(out.find("CONCAT(?s, ?s2)") != std::string::npos);
	REQUIRE(out.find("BNODE()") != std::string::npos);
	REQUIRE(out.find("BNODE(?s)") != std::string::npos);
	REQUIRE(out.find("SUBSTR(?s, \"1\"") != std::string::npos);
	REQUIRE(out.find("REPLACE(?s, \"a\", \"b\")") != std::string::npos);
}
