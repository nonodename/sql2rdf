/**
 * Unit tests for translateTriplePattern: the "alpha/beta inversion" that
 * turns a single SPARQL triple pattern into a SQL relation by enumerating
 * every candidate R2RML TriplesMap/PredicateObjectMap/rr:class source that
 * could produce a matching triple.
 */

#include <catch2/catch_test_macros.hpp>

#include <string>

#ifndef SOURCE_SPARQL2SQL_DIR
#define SOURCE_SPARQL2SQL_DIR ""
#endif
#ifndef SOURCE_R2RML_DIR
#define SOURCE_R2RML_DIR ""
#endif

#include "r2rml/R2RMLMapping.h"
#include "r2rml/R2RMLParser.h"
#include "sparql-parser/Parser.h"
#include "sparql2sql/DuckDbDialect.h"
#include "sparql2sql/TranslatedPattern.h"
#include "sparql2sql/TranslationError.h"
#include "sparql2sql/TriplePatternTranslator.h"

using r2rml::R2RMLMapping;
using r2rml::R2RMLParser;
using sparql::Parser;
using sparql::ast::BasicGraphPattern;
using sparql2sql::DuckDbDialect;
using sparql2sql::TranslatedPattern;
using sparql2sql::translateTriplePattern;
using sparql2sql::TranslationContext;
using sparql2sql::TranslationError;

namespace {

const sparql::ast::TriplePattern &nthTriple(const sparql::ast::Query &q, std::size_t elementIndex,
                                            std::size_t tripleIndex = 0) {
	const auto &el = *q.where->elements.at(elementIndex);
	const auto &bgp = static_cast<const BasicGraphPattern &>(el);
	return bgp.triples.at(tripleIndex);
}

} // namespace

TEST_CASE("translateTriplePattern: multiple candidates combine via UNION BY NAME") {
	Parser parser;
	auto q = parser.parseFile(SOURCE_SPARQL2SQL_DIR "emp_dept_simple_select.rq");
	R2RMLParser mappingParser;
	R2RMLMapping mapping = mappingParser.parse(SOURCE_R2RML_DIR "example_emp_dept.ttl");
	REQUIRE(mapping.isValid());

	DuckDbDialect dialect;
	TranslationContext ctx(mapping, dialect);
	TranslatedPattern result = translateTriplePattern(nthTriple(*q, 0), ctx);

	CHECK(result.boundVars.count("e") == 1);
	CHECK(result.boundVars.count("n") == 1);
	CHECK(result.optionalVars.empty());
	// Both TriplesMap1 (EMP.ENAME) and TriplesMap2 (DEPT.DNAME) have an
	// ex:name predicate-object map, so this must be a UNION of >=2 candidates.
	CHECK(result.sql.find("UNION BY NAME") != std::string::npos);
	CHECK(result.sql.find("\"EMP\"") != std::string::npos);
	CHECK(result.sql.find("\"ENAME\"") != std::string::npos);
	CHECK(result.sql.find("\"DNAME\"") != std::string::npos);
}

TEST_CASE("translateTriplePattern: a ReferencingObjectMap POM generates a real SQL JOIN") {
	Parser parser;
	auto q = parser.parseFile(SOURCE_SPARQL2SQL_DIR "emp_dept_join.rq");
	R2RMLParser mappingParser;
	R2RMLMapping mapping = mappingParser.parse(SOURCE_R2RML_DIR "example_emp_dept.ttl");
	REQUIRE(mapping.isValid());

	DuckDbDialect dialect;
	TranslationContext ctx(mapping, dialect);
	TranslatedPattern result = translateTriplePattern(nthTriple(*q, 0), ctx);

	CHECK(result.boundVars.count("e") == 1);
	CHECK(result.boundVars.count("d") == 1);
	CHECK(result.optionalVars.empty());
	CHECK(result.sql.find("JOIN") != std::string::npos);
	CHECK(result.sql.find("\"DEPTNO\"") != std::string::npos);
	CHECK(result.sql.find("\"EMP\"") != std::string::npos);
	// The parent (DEPT) side's logical table is an R2RMLView; its embedded
	// query text must appear (trailing ';' stripped) as a wrapped subquery.
	CHECK(result.sql.find("FROM DEPT") != std::string::npos);
}

TEST_CASE("translateTriplePattern: rr:class candidates for a constant rdf:type predicate") {
	Parser parser;
	auto q = parser.parseFile(SOURCE_SPARQL2SQL_DIR "sparql2sql_multi_placeholder.rq");
	R2RMLParser mappingParser;
	R2RMLMapping mapping = mappingParser.parse(SOURCE_R2RML_DIR "sparql2sql_multi_placeholder.ttl");
	REQUIRE(mapping.isValid());

	DuckDbDialect dialect;
	TranslationContext ctx(mapping, dialect);
	TranslatedPattern result = translateTriplePattern(nthTriple(*q, 0), ctx);

	CHECK(result.boundVars.count("o") == 1);
	CHECK(result.sql.find("\"ORDERS\"") != std::string::npos);
	CHECK(result.sql.find("\"CUSTID\"") != std::string::npos);
	CHECK(result.sql.find("\"ORDERID\"") != std::string::npos);
}

TEST_CASE("translateTriplePattern: variable predicate matches both rr:class and predicate-object candidates") {
	Parser parser;
	auto q = parser.parseFile(SOURCE_SPARQL2SQL_DIR "emp_dept_class.rq");
	R2RMLParser mappingParser;
	R2RMLMapping mapping = mappingParser.parse(SOURCE_R2RML_DIR "example_emp_dept.ttl");
	REQUIRE(mapping.isValid());

	DuckDbDialect dialect;
	TranslationContext ctx(mapping, dialect);
	TranslatedPattern result = translateTriplePattern(nthTriple(*q, 0), ctx);

	CHECK(result.boundVars.count("e") == 1);
	CHECK(result.boundVars.count("c") == 1);
	// Two TriplesMaps each with exactly one rr:class -> two branches. Since
	// the predicate ("a") is a bound constant equal to rdf:type, the match
	// is resolved statically (no runtime predicate check needed, so
	// rdf:type's IRI text need not appear in the SQL); ?c's value - the
	// class IRI itself - is what gets projected as a literal per branch.
	CHECK(result.sql.find("UNION BY NAME") != std::string::npos);
	CHECK(result.sql.find("http://example.com/ns#Employee") != std::string::npos);
	CHECK(result.sql.find("http://example.com/ns#Department") != std::string::npos);
}

TEST_CASE("translateTriplePattern: self-join guard for a variable repeated across positions") {
	Parser parser;
	auto q = parser.parseFile(SOURCE_SPARQL2SQL_DIR "sparql2sql_self_ref.rq");
	R2RMLParser mappingParser;
	R2RMLMapping mapping = mappingParser.parse(SOURCE_R2RML_DIR "sparql2sql_self_ref.ttl");
	REQUIRE(mapping.isValid());

	DuckDbDialect dialect;
	TranslationContext ctx(mapping, dialect);
	TranslatedPattern result = translateTriplePattern(nthTriple(*q, 0), ctx);

	// Only ONE output column for ?e, despite it appearing as both subject
	// and object of the triple pattern.
	REQUIRE(result.boundVars.size() == 1);
	CHECK(result.boundVars.count("e") == 1);
	CHECK(result.sql.find(" = ") != std::string::npos);
}

TEST_CASE("translateTriplePattern: bound predicate and bound object (rr:predicate/rr:object shortcuts)") {
	Parser parser;
	auto q = parser.parseFile(SOURCE_SPARQL2SQL_DIR "sparql2sql_constant_pom.rq");
	R2RMLParser mappingParser;
	R2RMLMapping mapping = mappingParser.parse(SOURCE_R2RML_DIR "sparql2sql_constant_pom.ttl");
	REQUIRE(mapping.isValid());

	DuckDbDialect dialect;
	TranslationContext ctx(mapping, dialect);

	TranslatedPattern result = translateTriplePattern(nthTriple(*q, 0, 0), ctx);
	REQUIRE(result.boundVars.size() == 1);
	CHECK(result.boundVars.count("p") == 1);
	CHECK(result.sql.find("\"PEOPLE\"") != std::string::npos);
}

TEST_CASE("translateTriplePattern: bound predicate via the general rr:predicateMap[rr:constant] form") {
	Parser parser;
	auto q = parser.parseFile(SOURCE_SPARQL2SQL_DIR "sparql2sql_constant_pom.rq");
	R2RMLParser mappingParser;
	R2RMLMapping mapping = mappingParser.parse(SOURCE_R2RML_DIR "sparql2sql_constant_pom.ttl");
	REQUIRE(mapping.isValid());

	DuckDbDialect dialect;
	TranslationContext ctx(mapping, dialect);

	// Both triples sit in the same BasicGraphPattern (no combinator between
	// them), so the second triple is triples[1] of element 0.
	TranslatedPattern result = translateTriplePattern(nthTriple(*q, 0, 1), ctx);
	CHECK(result.boundVars.count("p") == 1);
	CHECK(result.boundVars.count("n") == 1);
	CHECK(result.sql.find("\"NAME\"") != std::string::npos);
}

TEST_CASE("translateTriplePattern: a pattern matching zero candidates is a valid empty relation") {
	Parser parser;
	auto q = parser.parseString("PREFIX ex: <http://example.com/ns#>\n"
	                            "SELECT ?x ?y WHERE { ?x ex:doesNotExist ?y . }");
	R2RMLParser mappingParser;
	R2RMLMapping mapping = mappingParser.parse(SOURCE_R2RML_DIR "example_emp_dept.ttl");
	REQUIRE(mapping.isValid());

	DuckDbDialect dialect;
	TranslationContext ctx(mapping, dialect);
	TranslatedPattern result = translateTriplePattern(nthTriple(*q, 0), ctx);

	CHECK(result.boundVars.count("x") == 1);
	CHECK(result.boundVars.count("y") == 1);
	CHECK(result.sql.find("WHERE FALSE") != std::string::npos);
}

TEST_CASE("translateTriplePattern: unsupported property paths throw TranslationError") {
	Parser parser;
	auto q = parser.parseFile(SOURCE_SPARQL2SQL_DIR "unsupported_property_path.rq");
	R2RMLParser mappingParser;
	R2RMLMapping mapping = mappingParser.parse(SOURCE_R2RML_DIR "example_emp_dept.ttl");
	REQUIRE(mapping.isValid());

	DuckDbDialect dialect;
	TranslationContext ctx(mapping, dialect);
	CHECK_THROWS_AS(translateTriplePattern(nthTriple(*q, 0), ctx), TranslationError);
}
