/**
 * Tests for the SPARQL algebra fold over GroupGraphPattern::elements: the
 * combinators (innerJoin/leftOuterJoin/antiJoin/unionAll) tested directly with
 * hand-built IR relations (rendered to SQL for inspection), plus
 * integration-level tests driving fold() through parsed .rq/.ttl fixtures for
 * AND/OPTIONAL/UNION/MINUS/VALUES/SubSelect/GRAPH.
 */

#include <catch2/catch.hpp>

#include <initializer_list>
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
#include "sparql2sql/PatternFolder.h"
#include "sparql2sql/TranslationError.h"
#include "sparql2sql/Translator.h"
#include "sparql2sql/ir/RelNode.h"
#include "sparql2sql/ir/SqlRenderer.h"

using r2rml::R2RMLMapping;
using r2rml::R2RMLParser;
using sparql::Parser;
using sparql2sql::antiJoin;
using sparql2sql::ColumnInfo;
using sparql2sql::DuckDbDialect;
using sparql2sql::fold;
using sparql2sql::identityRelation;
using sparql2sql::innerJoin;
using sparql2sql::leftOuterJoin;
using sparql2sql::RawRelation;
using sparql2sql::RelNodePtr;
using sparql2sql::renderRelation;
using sparql2sql::TranslatedPattern;
using sparql2sql::translateQuery;
using sparql2sql::TranslationContext;
using sparql2sql::TranslationError;
using sparql2sql::unionAll;

namespace {

// A hand-built leaf relation carrying a placeholder SQL string and a schema
// (bound + optional variables). Enough to exercise the combinators' bound/
// optional bookkeeping and rendered join shape without a mapping.
RelNodePtr makeRel(const std::string &sql, std::initializer_list<std::string> bound,
                   std::initializer_list<std::string> optional = {}) {
	RelNodePtr node(new RawRelation());
	static_cast<RawRelation &>(*node).sql = sql;
	for (const auto &v : bound) {
		ColumnInfo c;
		c.var = v;
		c.nonNull = true;
		node->schema().push_back(c);
	}
	for (const auto &v : optional) {
		ColumnInfo c;
		c.var = v;
		c.nonNull = false;
		node->schema().push_back(c);
	}
	return node;
}

std::string renderedSql(const RelNodePtr &node, TranslationContext &ctx) {
	return renderRelation(*node, ctx).sql;
}

} // namespace

// --- Direct combinator unit tests (no mapping/parsing needed) ---

TEST_CASE("innerJoin: identity relation is elided on either side") {
	R2RMLMapping mapping;
	DuckDbDialect dialect;
	TranslationContext ctx(mapping, dialect);

	CHECK(renderedSql(innerJoin(identityRelation(ctx), makeRel("SELECT 1", {"x"}), ctx), ctx) == "SELECT 1");
	CHECK(renderedSql(innerJoin(makeRel("SELECT 1", {"x"}), identityRelation(ctx), ctx), ctx) == "SELECT 1");
}

TEST_CASE("innerJoin: a shared variable bound on either side stays bound in the result") {
	R2RMLMapping mapping;
	DuckDbDialect dialect;
	TranslationContext ctx(mapping, dialect);

	RelNodePtr node = innerJoin(makeRel("SELECT ...", {"x", "y"}), makeRel("SELECT ...", {}, {"x", "z"}), ctx);
	TranslatedPattern result = renderRelation(*node, ctx);
	CHECK(result.boundVars.count("x") == 1); // left guarantees it
	CHECK(result.boundVars.count("y") == 1);
	CHECK(result.optionalVars.count("z") == 1);
	CHECK(result.sql.find("INNER JOIN") != std::string::npos);
	// x is optional on the right, so the join must use the null-safe form.
	CHECK(result.sql.find("COALESCE") != std::string::npos);
	CHECK(result.sql.find("IS NULL") != std::string::npos);
}

TEST_CASE("innerJoin: no COALESCE/null-safety emitted when neither side is optional for the shared var") {
	R2RMLMapping mapping;
	DuckDbDialect dialect;
	TranslationContext ctx(mapping, dialect);

	std::string sql = renderedSql(innerJoin(makeRel("SELECT ...", {"x"}), makeRel("SELECT ...", {"x"}), ctx), ctx);
	CHECK(sql.find("COALESCE") == std::string::npos);
}

TEST_CASE("leftOuterJoin: only the left side's own guarantee keeps a shared var definitely bound") {
	R2RMLMapping mapping;
	DuckDbDialect dialect;
	TranslationContext ctx(mapping, dialect);

	// x AND d unconditionally bound on the right.
	RelNodePtr node = leftOuterJoin(makeRel("SELECT ...", {"x"}), makeRel("SELECT ...", {"x", "d"}), ctx);
	TranslatedPattern result = renderRelation(*node, ctx);
	CHECK(result.sql.find("LEFT OUTER JOIN") != std::string::npos);
	CHECK(result.boundVars.count("x") == 1); // left's own guarantee
	// d is unique to the right side; an unmatched left row would NULL it, so it
	// must be optional in the result regardless of the right side's own
	// (pre-join) bound-ness.
	CHECK(result.optionalVars.count("d") == 1);
	CHECK(result.boundVars.count("d") == 0);
}

TEST_CASE("antiJoin: zero shared variables is a spec-mandated no-op (no SQL change at all)") {
	R2RMLMapping mapping;
	DuckDbDialect dialect;
	TranslationContext ctx(mapping, dialect);

	RelNodePtr result = antiJoin(makeRel("SELECT LEFT_MARKER", {"x"}), makeRel("SELECT RIGHT_MARKER", {"y"}), ctx);
	std::string sql = renderedSql(result, ctx);
	CHECK(sql == "SELECT LEFT_MARKER");
	CHECK(sql.find("NOT EXISTS") == std::string::npos);
}

TEST_CASE("antiJoin: a shared variable produces a NOT EXISTS anti-join") {
	R2RMLMapping mapping;
	DuckDbDialect dialect;
	TranslationContext ctx(mapping, dialect);

	RelNodePtr node = antiJoin(makeRel("SELECT ...", {"x"}), makeRel("SELECT ...", {"x"}), ctx);
	TranslatedPattern result = renderRelation(*node, ctx);
	CHECK(result.sql.find("NOT EXISTS") != std::string::npos);
	CHECK(result.boundVars.count("x") == 1);
}

TEST_CASE("unionAll: a variable bound in every branch stays bound; others become optional") {
	R2RMLMapping mapping;
	DuckDbDialect dialect;
	TranslationContext ctx(mapping, dialect);

	std::vector<RelNodePtr> branches;
	branches.push_back(makeRel("SELECT ...", {"e", "n"}));
	branches.push_back(makeRel("SELECT ...", {"e", "d"}));

	RelNodePtr node = unionAll(std::move(branches), ctx);
	TranslatedPattern result = renderRelation(*node, ctx);
	CHECK(result.boundVars.count("e") == 1);
	CHECK(result.optionalVars.count("n") == 1);
	CHECK(result.optionalVars.count("d") == 1);
	CHECK(result.sql.find("UNION ALL BY NAME") != std::string::npos);
}

TEST_CASE("unionAll: a single branch is returned unchanged, with no UNION emitted") {
	R2RMLMapping mapping;
	DuckDbDialect dialect;
	TranslationContext ctx(mapping, dialect);

	std::vector<RelNodePtr> branches;
	branches.push_back(makeRel("SELECT ONLY_BRANCH", {"e"}));
	std::string sql = renderedSql(unionAll(std::move(branches), ctx), ctx);
	CHECK(sql == "SELECT ONLY_BRANCH");
}

TEST_CASE("leftOuterJoin: OPTIONAL with nothing preceding still joins against the one-row identity") {
	R2RMLMapping mapping;
	DuckDbDialect dialect;
	TranslationContext ctx(mapping, dialect);

	std::string identitySql = renderedSql(identityRelation(ctx), ctx);
	RelNodePtr node = leftOuterJoin(identityRelation(ctx), makeRel("SELECT ...", {"x"}), ctx);
	TranslatedPattern result = renderRelation(*node, ctx);
	CHECK(result.sql.find("LEFT OUTER JOIN") != std::string::npos);
	CHECK(result.sql != identitySql);
	CHECK(result.optionalVars.count("x") == 1);
}

// --- Integration-level tests through the fold, using real fixtures ---

TEST_CASE("fold: AND joins consecutive triples on shared variables") {
	Parser parser;
	auto q = parser.parseFile(SOURCE_SPARQL2SQL_DIR "sparql2sql_constant_pom.rq");
	R2RMLParser mappingParser;
	R2RMLMapping mapping = mappingParser.parse(SOURCE_R2RML_DIR "sparql2sql_constant_pom.ttl");
	REQUIRE(mapping.isValid());

	DuckDbDialect dialect;
	TranslationContext ctx(mapping, dialect);
	TranslatedPattern result = renderRelation(*fold(*q->where, ctx), ctx);

	CHECK(result.boundVars.count("p") == 1);
	CHECK(result.boundVars.count("n") == 1);
	CHECK(result.sql.find("INNER JOIN") != std::string::npos);
}

TEST_CASE("fold: OPTIONAL produces a left outer join with the department var optional") {
	Parser parser;
	auto q = parser.parseFile(SOURCE_SPARQL2SQL_DIR "emp_dept_optional.rq");
	R2RMLParser mappingParser;
	R2RMLMapping mapping = mappingParser.parse(SOURCE_R2RML_DIR "example_emp_dept.ttl");
	REQUIRE(mapping.isValid());

	DuckDbDialect dialect;
	TranslationContext ctx(mapping, dialect);
	TranslatedPattern result = renderRelation(*fold(*q->where, ctx), ctx);

	CHECK(result.sql.find("LEFT OUTER JOIN") != std::string::npos);
	CHECK(result.boundVars.count("e") == 1);
	CHECK(result.boundVars.count("n") == 1);
	CHECK(result.optionalVars.count("d") == 1);
}

TEST_CASE("fold: UNION with mismatched branch schemas leaves the unshared vars optional") {
	Parser parser;
	auto q = parser.parseFile(SOURCE_SPARQL2SQL_DIR "emp_dept_union_mismatched.rq");
	R2RMLParser mappingParser;
	R2RMLMapping mapping = mappingParser.parse(SOURCE_R2RML_DIR "example_emp_dept.ttl");
	REQUIRE(mapping.isValid());

	DuckDbDialect dialect;
	TranslationContext ctx(mapping, dialect);
	TranslatedPattern result = renderRelation(*fold(*q->where, ctx), ctx);

	CHECK(result.sql.find("UNION ALL BY NAME") != std::string::npos);
	CHECK(result.boundVars.count("e") == 1);
	CHECK(result.optionalVars.count("n") == 1);
	CHECK(result.optionalVars.count("d") == 1);
}

TEST_CASE("fold: MINUS with shared variables emits NOT EXISTS") {
	Parser parser;
	auto q = parser.parseFile(SOURCE_SPARQL2SQL_DIR "emp_dept_minus.rq");
	R2RMLParser mappingParser;
	R2RMLMapping mapping = mappingParser.parse(SOURCE_R2RML_DIR "example_emp_dept.ttl");
	REQUIRE(mapping.isValid());

	DuckDbDialect dialect;
	TranslationContext ctx(mapping, dialect);
	TranslatedPattern result = renderRelation(*fold(*q->where, ctx), ctx);
	CHECK(result.sql.find("NOT EXISTS") != std::string::npos);
}

TEST_CASE("fold: MINUS with zero shared variables is a no-op, no NOT EXISTS emitted") {
	Parser parser;
	auto q = parser.parseFile(SOURCE_SPARQL2SQL_DIR "emp_dept_minus_novars.rq");
	R2RMLParser mappingParser;
	R2RMLMapping mapping = mappingParser.parse(SOURCE_R2RML_DIR "example_emp_dept.ttl");
	REQUIRE(mapping.isValid());

	DuckDbDialect dialect;
	TranslationContext ctx(mapping, dialect);
	TranslatedPattern result = renderRelation(*fold(*q->where, ctx), ctx);
	CHECK(result.sql.find("NOT EXISTS") == std::string::npos);
}

TEST_CASE("fold: VALUES joins the inline data table on the shared variable") {
	Parser parser;
	auto q = parser.parseFile(SOURCE_SPARQL2SQL_DIR "emp_dept_values.rq");
	R2RMLParser mappingParser;
	R2RMLMapping mapping = mappingParser.parse(SOURCE_R2RML_DIR "example_emp_dept.ttl");
	REQUIRE(mapping.isValid());

	DuckDbDialect dialect;
	TranslationContext ctx(mapping, dialect);
	TranslatedPattern result = renderRelation(*fold(*q->where, ctx), ctx);
	CHECK(result.boundVars.count("e") == 1);
	CHECK(result.boundVars.count("n") == 1);
	CHECK(result.sql.find("'SMITH'") != std::string::npos);
	CHECK(result.sql.find("'JONES'") != std::string::npos);
}

TEST_CASE("fold: a subquery element's guaranteed-bound variables join with a plain equality, not a null-safe one") {
	Parser parser;
	auto q = parser.parseFile(SOURCE_SPARQL2SQL_DIR "emp_dept_subquery.rq");
	R2RMLParser mappingParser;
	R2RMLMapping mapping = mappingParser.parse(SOURCE_R2RML_DIR "example_emp_dept.ttl");
	REQUIRE(mapping.isValid());

	DuckDbDialect dialect;
	TranslationContext ctx(mapping, dialect);
	TranslatedPattern result = renderRelation(*fold(*q->where, ctx), ctx);
	// ?e is a plain triple pattern inside the subquery (no OPTIONAL/UNION), so
	// translateQueryPattern reports it as boundVars, not optionalVars; the outer
	// join must reuse that instead of over-approximating it as nullable, which
	// would otherwise force a non-sargable "OR ... IS NULL" join predicate.
	CHECK(result.boundVars.count("e") == 1);
	CHECK(result.sql.find("INNER JOIN") != std::string::npos);
	CHECK(result.sql.find("IS NULL") == std::string::npos);
	CHECK(result.sql.find("COALESCE") == std::string::npos);
}

TEST_CASE("fold: a GRAPH block folds its pattern against that graph and inner-joins the result") {
	// Was a refusal test until named-graph support landed. graph_var.rq
	// is `GRAPH ?g { ?e ex:name ?n }`, so folding it must bind ?g alongside the
	// pattern's own variables.
	Parser parser;
	auto q = parser.parseFile(SOURCE_SPARQL2SQL_DIR "graph_var.rq");
	R2RMLParser mappingParser;
	R2RMLMapping mapping = mappingParser.parse(SOURCE_R2RML_DIR "sparql2sql_graphs.ttl");
	REQUIRE(mapping.isValid());

	DuckDbDialect dialect;
	TranslationContext ctx(mapping, dialect);
	TranslatedPattern result = renderRelation(*fold(*q->where, ctx), ctx);

	// ?g is a real query variable, not an internal one: it is bound and
	// projected like any other.
	CHECK(result.boundVars.count("g") == 1);
	CHECK(result.boundVars.count("e") == 1);
	CHECK(result.boundVars.count("n") == 1);
	CHECK(result.sql.find("\"v_g\"") != std::string::npos);
	// EmpMap's ex:name is the one in <graph/g1>; DeptMap's is default-graph only.
	CHECK(result.sql.find("http://example.com/graph/g1") != std::string::npos);
	CHECK(result.sql.find("\"ENAME\"") != std::string::npos);
	CHECK(result.sql.find("\"DNAME\"") == std::string::npos);
}

TEST_CASE("fold: the active graph is restored after a GRAPH block ends") {
	// A GRAPH block must not leak its graph onto a sibling pattern outside it.
	// Here ?d ex:name ?n sits after the block and so is default-graph only,
	// which for this mapping means DeptMap (DNAME) and not EmpMap (ENAME).
	Parser parser;
	auto q = parser.parseString("PREFIX ex: <http://example.com/ns#>\n"
	                            "SELECT * WHERE {\n"
	                            "  GRAPH <http://example.com/graph/g1> { ?x ex:location ?l . }\n"
	                            "  ?d ex:name ?n .\n"
	                            "}");
	R2RMLParser mappingParser;
	R2RMLMapping mapping = mappingParser.parse(SOURCE_R2RML_DIR "sparql2sql_graphs.ttl");
	REQUIRE(mapping.isValid());

	DuckDbDialect dialect;
	TranslationContext ctx(mapping, dialect);
	TranslatedPattern result = renderRelation(*fold(*q->where, ctx), ctx);

	CHECK(result.sql.find("\"LOC\"") != std::string::npos);   // inside the block
	CHECK(result.sql.find("\"DNAME\"") != std::string::npos); // outside it, default graph
	CHECK(result.sql.find("\"ENAME\"") == std::string::npos); // would mean the graph leaked
}

TEST_CASE("fold: a nested GRAPH block replaces the enclosing graph rather than composing with it") {
	// SPARQL 1.1 Section 13.3. The inner block names a graph the mapping can
	// produce, so it must match even though the outer graph differs - an
	// intersecting (rather than replacing) implementation would find nothing.
	Parser parser;
	auto q = parser.parseString("PREFIX ex: <http://example.com/ns#>\n"
	                            "SELECT * WHERE {\n"
	                            "  GRAPH <http://example.com/graph/dept/10> {\n"
	                            "    GRAPH <http://example.com/graph/g1> { ?d ex:location ?l . }\n"
	                            "  }\n"
	                            "}");
	R2RMLParser mappingParser;
	R2RMLMapping mapping = mappingParser.parse(SOURCE_R2RML_DIR "sparql2sql_graphs.ttl");
	REQUIRE(mapping.isValid());

	DuckDbDialect dialect;
	TranslationContext ctx(mapping, dialect);
	TranslatedPattern result = renderRelation(*fold(*q->where, ctx), ctx);

	CHECK(result.sql.find("\"LOC\"") != std::string::npos);
	CHECK(result.sql.find("WHERE FALSE") == std::string::npos);
}

TEST_CASE("fold: SERVICE throws a clear TranslationError (no federated query support)") {
	Parser parser;
	auto q = parser.parseFile(SOURCE_SPARQL2SQL_DIR "unsupported_service.rq");
	R2RMLParser mappingParser;
	R2RMLMapping mapping = mappingParser.parse(SOURCE_R2RML_DIR "example_emp_dept.ttl");
	REQUIRE(mapping.isValid());

	DuckDbDialect dialect;
	TranslationContext ctx(mapping, dialect);
	CHECK_THROWS_AS(fold(*q->where, ctx), TranslationError);
}

TEST_CASE("fold: VALUES with zero rows produces a WHERE FALSE table") {
	Parser parser;
	auto q = parser.parseFile(SOURCE_SPARQL2SQL_DIR "emp_dept_values_empty.rq");
	R2RMLParser mappingParser;
	R2RMLMapping mapping = mappingParser.parse(SOURCE_R2RML_DIR "example_emp_dept.ttl");
	REQUIRE(mapping.isValid());

	DuckDbDialect dialect;
	TranslationContext ctx(mapping, dialect);
	TranslatedPattern result = renderRelation(*fold(*q->where, ctx), ctx);
	CHECK(result.sql.find("WHERE FALSE") != std::string::npos);
}

TEST_CASE("fold: BIND over a variable made optional by a preceding OPTIONAL is itself optional") {
	Parser parser;
	auto q = parser.parseFile(SOURCE_SPARQL2SQL_DIR "emp_dept_optional_bind.rq");
	R2RMLParser mappingParser;
	R2RMLMapping mapping = mappingParser.parse(SOURCE_R2RML_DIR "example_emp_dept.ttl");
	REQUIRE(mapping.isValid());

	DuckDbDialect dialect;
	TranslationContext ctx(mapping, dialect);
	TranslatedPattern result = renderRelation(*fold(*q->where, ctx), ctx);
	CHECK(result.optionalVars.count("d") == 1);
	CHECK(result.optionalVars.count("dd") == 1);
}

TEST_CASE("translateQuery: CONSTRUCT is rejected with a clear TranslationError") {
	Parser parser;
	auto q = parser.parseFile(SOURCE_SPARQL2SQL_DIR "unsupported_construct.rq");
	R2RMLParser mappingParser;
	R2RMLMapping mapping = mappingParser.parse(SOURCE_R2RML_DIR "example_emp_dept.ttl");
	REQUIRE(mapping.isValid());

	DuckDbDialect dialect;
	CHECK_THROWS_AS(translateQuery(*q, mapping, dialect), TranslationError);
}

TEST_CASE("translateQuery: FROM over a graph the mapping cannot produce yields an empty relation, not an error") {
	// Was a refusal test until dataset support landed. dataset_from.rq names
	// <http://example.com/graph1>, which example_emp_dept.ttl (no graph maps at
	// all) cannot produce - so the query is translatable and simply matches
	// nothing, exactly as a real engine would answer it.
	Parser parser;
	auto q = parser.parseFile(SOURCE_SPARQL2SQL_DIR "dataset_from.rq");
	R2RMLParser mappingParser;
	R2RMLMapping mapping = mappingParser.parse(SOURCE_R2RML_DIR "example_emp_dept.ttl");
	REQUIRE(mapping.isValid());

	DuckDbDialect dialect;
	std::string sql = translateQuery(*q, mapping, dialect);
	CHECK(sql.find("WHERE FALSE") != std::string::npos);
}
