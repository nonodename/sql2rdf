/**
 * Unit tests for translatePath: the desugaring of SPARQL property path
 * operators into the relational algebra (SPARQL 1.1 Section 18.1.7).
 *
 * These assert only on the *structure* of the generated SQL, which is all
 * test_runner can do without linking DuckDB. Execution correctness for every
 * operator is covered by tests/duckdb/test_sparql2sql_duckdb.cpp.
 */

#include <catch2/catch.hpp>

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

TEST_CASE("translatePath: a negated property set with both forward and inverse IRIs emits both arms, each "
          "excluding only its own list") {
	R2RMLMapping mapping = parseMapping("example_emp_dept.ttl");
	DuckDbDialect dialect;
	TranslatedPattern result = translateFirstTriple("emp_dept_path_nps_mixed.rq", mapping, dialect);

	// !(ex:name|^ex:department): forwardIris = {ex:name}, inverseIris =
	// {ex:department}, both non-empty, so translateNegatedPropertySet's guard
	// (`!forwardIris.empty() || inverseIris.empty()`) takes its true branch via
	// the first operand *and* the second arm's `!inverseIris.empty()` guard is
	// also true - both arms get emitted in the same call, unlike the
	// forward-only and inverse-only fixtures above.
	CHECK(result.boundVars.count("d") == 1);
	CHECK(result.boundVars.count("v") == 1);
	CHECK(contains(result.sql, "UNION"));
	// The forward arm excludes only ex:name, so ex:department's referencing
	// object join (fixed-IRI predicate, not in forwardIris) survives forward.
	CHECK(contains(result.sql, "\"DEPTNO\" = "));
	CHECK(contains(result.sql, "\"LOC\""));
	CHECK(contains(result.sql, "\"STAFF\""));
	// The inverse arm excludes only ex:department (reversed), so ex:name's
	// candidates (fixed IRI, not in inverseIris) survive in the reverse arm.
	CHECK(contains(result.sql, "\"DNAME\""));
	CHECK(contains(result.sql, "\"ENAME\""));
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

TEST_CASE("translatePath: a zero-or-one path with two bound, unequal endpoints degenerates to just the child path") {
	R2RMLMapping mapping = parseMapping("example_emp_dept.ttl");
	DuckDbDialect dialect;
	TranslatedPattern result = translateFirstTriple("emp_dept_path_zero_or_one_bound_both.rq", mapping, dialect);

	// <employee/7369> ex:department? <department/99> - both endpoints bound and
	// unequal, so zeroLengthPath returns an EmptyNode with no schema: the
	// `zero->kind() == RelKind::Empty && zero->schema().empty()` short-circuit
	// in translateZeroOrOne fires and the union collapses to just the one-step
	// child path, with no zero-length arm and no UNION at all.
	CHECK(result.boundVars.empty());
	CHECK_FALSE(contains(result.sql, "UNION"));
	CHECK(contains(result.sql, "\"DEPTNO\" = "));
	// Bound IRIs are inverted back to their raw template values for the
	// filter, not compared against the full IRI string.
	CHECK(contains(result.sql, "'7369'"));
	CHECK(contains(result.sql, "'99'"));
}

TEST_CASE("translatePath: a zero-or-more path with two bound, unequal endpoints degenerates to just the "
          "one-or-more closure") {
	// Like the one-or-more/both-bound test below, the recursive CTE's WITH
	// RECURSIVE prefix is only emitted by translateQuery's top-level call
	// sites, so this must go through translateWholeQuery rather than
	// translateFirstTriple/renderRelation.
	R2RMLMapping mapping = parseMapping("example_emp_dept.ttl");
	DuckDbDialect dialect;
	std::string sql = translateWholeQuery("emp_dept_path_zero_or_more_bound_both.rq", mapping, dialect);

	// <employee/7369> ex:knows* <employee/9999> - both endpoints bound and
	// unequal, so zeroLengthPath's EmptyNode/empty-schema short-circuit fires
	// in translateZeroOrMore: the result is exactly translateOneOrMore's
	// BothBound EXISTS closure, with no zero-length arm unioned in and no
	// UNION at all (unlike the two-unbound-endpoints ZeroOrMore case above,
	// which always unions the closure with the zero-length arm).
	// A single UNION remains (the recursive step's own base/step union inside
	// the CTE), but no outer UNION combines the closure with a zero-length arm.
	CHECK(sql.find("UNION") == sql.rfind("UNION"));
	CHECK(contains(sql, "WITH RECURSIVE"));
	CHECK(contains(sql, "EXISTS"));
	CHECK(contains(sql, "http://data.example.com/employee/7369"));
	CHECK(contains(sql, "http://data.example.com/employee/9999"));
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

TEST_CASE("translatePath: a one-or-more path with both endpoints unbound is a full pairs closure") {
	// unsupported_property_path.rq: ?s ex:knows+ ?o - the pre-recursive-CTE
	// codebase rejected this; both endpoints are variables, so this is the
	// expensive full (from, to) pairs closure case. The WITH RECURSIVE prefix
	// is only emitted by translateQuery's two top-level call sites, so this
	// (unlike the other translatePath tests in this file) must go through the
	// whole-query path rather than translateFirstTriple/renderRelation.
	R2RMLMapping mapping = parseMapping("example_emp_dept.ttl");
	DuckDbDialect dialect;
	std::string sql = translateWholeQuery("unsupported_property_path.rq", mapping, dialect);

	CHECK(contains(sql, "WITH RECURSIVE"));
	CHECK(contains(sql, "UNION"));
	CHECK_FALSE(contains(sql, "UNION ALL"));
	CHECK(contains(outerSelectList(sql), "\"v_s\""));
	CHECK(contains(outerSelectList(sql), "\"v_o\""));
}

TEST_CASE("translatePath: a zero-or-more path unions the one-or-more closure with the zero-length path") {
	// unsupported_path_star.rq: ?s ex:knows* ?o.
	R2RMLMapping mapping = parseMapping("example_emp_dept.ttl");
	DuckDbDialect dialect;
	std::string sql = translateWholeQuery("unsupported_path_star.rq", mapping, dialect);

	CHECK(contains(sql, "WITH RECURSIVE"));
	// The outer relation unions the E+ closure with the zero-length arm.
	CHECK(contains(sql, "UNION"));
	CHECK(contains(outerSelectList(sql), "\"v_s\""));
	CHECK(contains(outerSelectList(sql), "\"v_o\""));
}

TEST_CASE("translatePath: a one-or-more path with a bound subject seeds a unary reachable-set forward") {
	R2RMLMapping mapping = parseMapping("example_emp_dept.ttl");
	DuckDbDialect dialect;
	std::string sql = translateWholeQuery("emp_dept_path_plus_bound_subject.rq", mapping, dialect);

	// <employee/7369> ex:knows+ ?m - seeded forward from the bound subject: a
	// unary "cte_node" reachable-set, not a two-column pairs CTE.
	CHECK(contains(sql, "WITH RECURSIVE"));
	CHECK(contains(sql, "\"cte_node\""));
	CHECK_FALSE(contains(sql, "\"cte_from\""));
	CHECK(contains(sql, "http://data.example.com/employee/7369"));
	CHECK(contains(outerSelectList(sql), "\"v_m\""));
}

TEST_CASE("translatePath: a one-or-more path with a bound object seeds a unary reachable-set backward") {
	R2RMLMapping mapping = parseMapping("example_emp_dept.ttl");
	DuckDbDialect dialect;
	std::string sql = translateWholeQuery("emp_dept_path_plus_bound_object.rq", mapping, dialect);

	// ?m ex:knows+ <employee/7839> - seeded backward from the bound object:
	// still a unary reachable-set, walked in reverse.
	CHECK(contains(sql, "WITH RECURSIVE"));
	CHECK(contains(sql, "\"cte_node\""));
	CHECK_FALSE(contains(sql, "\"cte_from\""));
	CHECK(contains(sql, "http://data.example.com/employee/7839"));
	CHECK(contains(outerSelectList(sql), "\"v_m\""));
}

TEST_CASE("translatePath: a one-or-more path with both endpoints bound is an EXISTS membership test") {
	R2RMLMapping mapping = parseMapping("example_emp_dept.ttl");
	DuckDbDialect dialect;
	TranslatedPattern result = translateFirstTriple("emp_dept_path_plus_bound_both.rq", mapping, dialect);

	// Both bound and unequal: no columns to project, just closure membership.
	CHECK(result.boundVars.empty());
	CHECK(contains(result.sql, "EXISTS"));
	CHECK_FALSE(contains(result.sql, "\"cte_from\""));
}

TEST_CASE("translatePath: a one-or-more path with the same variable on both ends filters the closure diagonal") {
	R2RMLMapping mapping = parseMapping("example_emp_dept.ttl");
	DuckDbDialect dialect;
	std::string sql = translateWholeQuery("emp_dept_path_plus_same_var.rq", mapping, dialect);

	// ?m ex:knows+ ?m - only the diagonal of the pairs closure satisfies the
	// pattern, so exactly one variable is projected.
	CHECK(contains(sql, "WITH RECURSIVE"));
	CHECK(contains(sql, "\"cte_from\" = "));
	CHECK(contains(outerSelectList(sql), "\"v_m\""));
}

TEST_CASE("translatePath: the closure's diagonal is driven by sameEndpointVar, not by projected width") {
	R2RMLMapping mapping = parseMapping("example_emp_dept.ttl");
	DuckDbDialect dialect;
	TranslationContext ctx(mapping, dialect);

	Parser parser;
	auto sameVar = parser.parseFile(SOURCE_SPARQL2SQL_DIR "emp_dept_path_plus_same_var.rq");
	sparql2sql::RelNodePtr sameNode = translateTriplePattern(firstTriple(*sameVar), ctx);
	REQUIRE(sameNode->kind() == sparql2sql::RelKind::TransitiveClosure);
	const auto &sameTc = static_cast<const sparql2sql::TransitiveClosureNode &>(*sameNode);
	CHECK(sameTc.mode == sparql2sql::TransitiveClosureNode::Mode::BothVars);
	CHECK(sameTc.sameEndpointVar);

	// The distinct-endpoint sibling must leave the flag clear. Asserting the
	// flag directly - rather than only the rendered SQL - is the point: the
	// renderer used to infer this from schema().size(), which is correct only
	// while width is pinned by mode. A future carried column (e.g. a named-graph
	// invariant) would break that inference silently, so the producer's
	// intent is what gets pinned here.
	// path_plus_view.rq's first triple is `?s ex:knows+ ?o` - both endpoints
	// variables, but distinct ones.
	auto twoVar = parser.parseFile(SOURCE_SPARQL2SQL_DIR "emp_dept_path_plus_view.rq");
	sparql2sql::RelNodePtr twoNode = translateTriplePattern(firstTriple(*twoVar), ctx);
	REQUIRE(twoNode->kind() == sparql2sql::RelKind::TransitiveClosure);
	const auto &twoTc = static_cast<const sparql2sql::TransitiveClosureNode &>(*twoNode);
	CHECK(twoTc.mode == sparql2sql::TransitiveClosureNode::Mode::BothVars);
	CHECK_FALSE(twoTc.sameEndpointVar);
}
