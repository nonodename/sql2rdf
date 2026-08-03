/**
 * Unit tests for translatePath: the desugaring of SPARQL property path
 * operators into the relational algebra (SPARQL 1.1 Section 18.1.7).
 *
 * These assert only on the *structure* of the generated SQL, which is all
 * test_runner can do without linking DuckDB. Execution correctness for every
 * operator is covered by tests/duckdb/test_sparql2sql_duckdb.cpp.
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
#include "sparql2sql/Translator.h"
#include "sparql2sql/TriplePatternTranslator.h"
#include "sparql2sql/ir/RelNode.h"
#include "sparql2sql/ir/SqlRenderer.h"

using r2rml::R2RMLMapping;
using r2rml::R2RMLParser;
using sparql::Parser;
using sparql::ast::BasicGraphPattern;
using sparql2sql::DuckDbDialect;
using sparql2sql::renderRelation;
using sparql2sql::TranslatedPattern;
using sparql2sql::translateQuery;
using sparql2sql::translateTriplePattern;
using sparql2sql::TranslationContext;
using sparql2sql::TranslationError;

namespace {

const sparql::ast::TriplePattern &firstTriple(const sparql::ast::Query &q) {
	const auto &el = *q.where->elements.at(0);
	return static_cast<const BasicGraphPattern &>(el).triples.at(0);
}

R2RMLMapping parseMapping(const std::string &ttlFile) {
	R2RMLParser mappingParser;
	R2RMLMapping mapping = mappingParser.parse(SOURCE_R2RML_DIR + ttlFile);
	REQUIRE(mapping.isValid());
	return mapping;
}

// Translate the first triple pattern of a fixture and render it back for
// structural (substring) inspection.
TranslatedPattern translateFirstTriple(const std::string &rqFile, const R2RMLMapping &mapping,
                                       const DuckDbDialect &dialect) {
	Parser parser;
	auto q = parser.parseFile(SOURCE_SPARQL2SQL_DIR + rqFile);
	TranslationContext ctx(mapping, dialect);
	return renderRelation(*translateTriplePattern(firstTriple(*q), ctx), ctx);
}

std::string translateWholeQuery(const std::string &rqFile, const R2RMLMapping &mapping, const DuckDbDialect &dialect) {
	Parser parser;
	auto q = parser.parseFile(SOURCE_SPARQL2SQL_DIR + rqFile);
	return translateQuery(*q, mapping, dialect);
}

bool contains(const std::string &haystack, const std::string &needle) {
	return haystack.find(needle) != std::string::npos;
}

// The outermost SELECT list: everything before the query-level " FROM (" that
// wraps the folded relation. Inner relations legitimately project internal
// variables (a sequence path joins on one); only the query's own projection
// has to be free of them.
std::string outerSelectList(const std::string &sql) {
	return sql.substr(0, sql.find(" FROM ("));
}

} // namespace

TEST_CASE("translatePath: a sequence path joins its two halves and binds only the query variables") {
	R2RMLMapping mapping = parseMapping("example_emp_dept.ttl");
	DuckDbDialect dialect;
	TranslatedPattern result = translateFirstTriple("emp_dept_path_seq.rq", mapping, dialect);

	// ?e ex:department/ex:name ?dn - the intermediate department node is a
	// fresh internal variable, joined but never surfaced as a query variable.
	CHECK(result.boundVars.count("e") == 1);
	CHECK(result.boundVars.count("dn") == 1);
	CHECK(contains(result.sql, "JOIN"));
	// Both halves are present: the EMP->DEPT link and the department name.
	CHECK(contains(result.sql, "\"EMP\""));
	CHECK(contains(result.sql, "\"DNAME\""));
}

TEST_CASE("translatePath: an inverse path is the plain path with its endpoints exchanged") {
	R2RMLMapping mapping = parseMapping("example_emp_dept.ttl");
	DuckDbDialect dialect;

	// ?d ^ex:department ?e must translate exactly as ?e ex:department ?d does,
	// modulo the alias counter (both fixtures translate in a fresh context).
	TranslatedPattern inverse = translateFirstTriple("emp_dept_path_inverse.rq", mapping, dialect);
	TranslatedPattern forward = translateFirstTriple("emp_dept_join.rq", mapping, dialect);

	CHECK(inverse.boundVars.count("d") == 1);
	CHECK(inverse.boundVars.count("e") == 1);
	CHECK(inverse.sql == forward.sql);
}

TEST_CASE("translatePath: an alternative path unions its two branches") {
	R2RMLMapping mapping = parseMapping("example_emp_dept.ttl");
	DuckDbDialect dialect;
	TranslatedPattern result = translateFirstTriple("emp_dept_path_alt.rq", mapping, dialect);

	CHECK(result.boundVars.count("d") == 1);
	CHECK(result.boundVars.count("v") == 1);
	CHECK(contains(result.sql, "UNION"));
	CHECK(contains(result.sql, "\"LOC\""));
	CHECK(contains(result.sql, "\"STAFF\""));
	// ex:name is in neither branch.
	CHECK_FALSE(contains(result.sql, "\"DNAME\""));
}

TEST_CASE("translatePath: a negated property set excludes named predicates but keeps the rest") {
	R2RMLMapping mapping = parseMapping("example_emp_dept.ttl");
	DuckDbDialect dialect;
	TranslatedPattern result = translateFirstTriple("emp_dept_path_nps.rq", mapping, dialect);

	// !(ex:name|ex:staff): the excluded predicates are constant predicate maps,
	// so their candidates are pruned outright rather than filtered at runtime.
	CHECK_FALSE(contains(result.sql, "\"DNAME\""));
	CHECK_FALSE(contains(result.sql, "\"STAFF\""));
	CHECK_FALSE(contains(result.sql, "\"ENAME\""));
	// Everything else survives: ex:location, and both rr:class rdf:type edges.
	CHECK(contains(result.sql, "\"LOC\""));
	CHECK(contains(result.sql, "http://example.com/ns#Department"));
	CHECK(contains(result.sql, "http://example.com/ns#Employee"));
}

TEST_CASE("translatePath: an all-inverse negated property set emits only the reversed arm") {
	R2RMLMapping mapping = parseMapping("example_emp_dept.ttl");
	DuckDbDialect dialect;
	TranslatedPattern result = translateFirstTriple("emp_dept_path_nps_inverse.rq", mapping, dialect);

	// !(^ex:department) matches reversed triples whose predicate is not
	// ex:department - so the referencing object map's child JOIN parent source
	// (the only way ex:department is produced) must not appear.
	CHECK(result.boundVars.count("s") == 1);
	CHECK(result.boundVars.count("o") == 1);
	CHECK(contains(result.sql, "\"DNAME\""));
	CHECK(contains(result.sql, "\"ENAME\""));
	CHECK_FALSE(contains(result.sql, "\"DEPTNO\" = "));
}

TEST_CASE("translatePath: a zero-or-one path unions the one-step path with the zero-length path") {
	R2RMLMapping mapping = parseMapping("example_emp_dept.ttl");
	DuckDbDialect dialect;
	TranslatedPattern result = translateFirstTriple("emp_dept_path_zero_or_one.rq", mapping, dialect);

	// <employee/7369> ex:department? ?d - one endpoint is bound, so the
	// zero-length arm is just that term, with no term-universe scan.
	CHECK(result.boundVars.count("d") == 1);
	CHECK(contains(result.sql, "UNION"));
	CHECK(contains(result.sql, "http://data.example.com/employee/7369"));
}

TEST_CASE("translatePath: a zero-length path with two unbound endpoints ranges over all graph terms") {
	R2RMLMapping mapping = parseMapping("sparql2sql_path_terms.ttl");
	DuckDbDialect dialect;
	TranslatedPattern result = translateFirstTriple("sparql2sql_path_terms.rq", mapping, dialect);

	CHECK(result.boundVars.count("x") == 1);
	CHECK(result.boundVars.count("y") == 1);
	CHECK(contains(result.sql, "UNION"));
	CHECK(contains(result.sql, "\"NODES\""));
	// The term universe binds both endpoints to the same term.
	CHECK(contains(result.sql, "\"v_x\""));
	CHECK(contains(result.sql, "\"v_y\""));
}

TEST_CASE("translatePath: SELECT * never projects a sequence path's intermediate variable") {
	R2RMLMapping mapping = parseMapping("example_emp_dept.ttl");
	DuckDbDialect dialect;
	std::string projection = outerSelectList(translateWholeQuery("emp_dept_path_star_select.rq", mapping, dialect));

	CHECK(contains(projection, "\"v_e\""));
	CHECK(contains(projection, "\"v_dn\""));
	// The minted intermediate variable is internal: joined on by the inner
	// relations, but never a query variable, so it must not be projected here.
	CHECK_FALSE(contains(projection, "v_%p"));
}

TEST_CASE("SELECT * never projects a blank-node position") {
	R2RMLMapping mapping = parseMapping("example_emp_dept.ttl");
	DuckDbDialect dialect;
	std::string projection = outerSelectList(translateWholeQuery("emp_dept_bnode_select_star.rq", mapping, dialect));

	// A blank node is scoped like a variable during translation but is not a
	// query variable, so `SELECT *` must not surface it. It shares the
	// internal-variable mechanism the sequence path's midpoint uses.
	CHECK(contains(projection, "\"v_e\""));
	CHECK_FALSE(contains(projection, "_bnode_"));
}

TEST_CASE("translatePath: the unsupported one-or-more operator throws and names itself") {
	R2RMLMapping mapping = parseMapping("example_emp_dept.ttl");
	DuckDbDialect dialect;
	Parser parser;
	auto q = parser.parseFile(SOURCE_SPARQL2SQL_DIR "unsupported_property_path.rq");
	TranslationContext ctx(mapping, dialect);
	CHECK_THROWS_AS(translateTriplePattern(firstTriple(*q), ctx), TranslationError);
}

TEST_CASE("translatePath: the unsupported zero-or-more operator throws and names itself") {
	R2RMLMapping mapping = parseMapping("example_emp_dept.ttl");
	DuckDbDialect dialect;
	Parser parser;
	auto q = parser.parseFile(SOURCE_SPARQL2SQL_DIR "unsupported_path_star.rq");
	TranslationContext ctx(mapping, dialect);
	CHECK_THROWS_AS(translateTriplePattern(firstTriple(*q), ctx), TranslationError);
}
