/**
 * Tests for the active-graph scope machinery (GraphConstraint,
 * TranslationContext::ActiveGraphGuard, and the FilterNode/BindNode capture).
 *
 * This is plumbing only: nothing yet *reads* the active graph to change the SQL
 * it generates, so these assert on the mechanism directly rather than on
 * generated SQL. That is the point at this stage - the whole step is required to
 * be SQL-identical, so a SQL-level assertion could not distinguish working
 * plumbing from no plumbing at all.
 *
 * The load-bearing case is the capture on FilterNode/BindNode. Their `predicate`
 * / `expr` are borrowed AST pointers translated lazily at *render* time, long
 * after fold() returned and every guard was destroyed. An EXISTS inside one
 * folds a graph pattern of its own at that point, so without the capture it
 * would silently be matched against the default graph even when written inside
 * a GRAPH block.
 */

#include <catch2/catch_test_macros.hpp>

#include <string>

#ifndef SOURCE_R2RML_DIR
#define SOURCE_R2RML_DIR ""
#endif
#ifndef SOURCE_SPARQL2SQL_DIR
#define SOURCE_SPARQL2SQL_DIR ""
#endif

#include "r2rml/R2RMLMapping.h"
#include "r2rml/R2RMLParser.h"
#include "sparql-parser/Parser.h"
#include "sparql2sql/DuckDbDialect.h"
#include "sparql2sql/GraphConstraint.h"
#include "sparql2sql/PatternFolder.h"
#include "sparql2sql/TranslatedPattern.h"
#include "sparql2sql/Translator.h"
#include "sparql2sql/ir/Optimizer.h"
#include "sparql2sql/ir/RelNode.h"

using r2rml::R2RMLMapping;
using r2rml::R2RMLParser;
using sparql::Parser;
using sparql2sql::boundGraph;
using sparql2sql::DuckDbDialect;
using sparql2sql::fold;
using sparql2sql::GraphConstraint;
using sparql2sql::RelKind;
using sparql2sql::RelNode;
using sparql2sql::RelNodePtr;
using sparql2sql::TranslationContext;
using sparql2sql::variableGraph;

namespace {

R2RMLMapping empDept() {
	R2RMLParser mappingParser;
	R2RMLMapping mapping = mappingParser.parse(SOURCE_R2RML_DIR "example_emp_dept.ttl");
	REQUIRE(mapping.isValid());
	return mapping;
}

// A mapping that really does declare <http://example.com/graph/g1>. Needed by
// any test that folds under a BoundIri constraint: candidate enumeration now
// prunes a graph the mapping cannot produce, so folding `GRAPH <g1>` against a
// graph-less mapping correctly yields an empty relation - and then there is no
// FilterNode left to inspect.
R2RMLMapping graphMapping() {
	R2RMLParser mappingParser;
	R2RMLMapping mapping = mappingParser.parse(SOURCE_R2RML_DIR "sparql2sql_graphs.ttl");
	REQUIRE(mapping.isValid());
	return mapping;
}

// Collect every FilterNode's captured constraint in the tree, so a test does
// not depend on where pushdown chose to re-parent the filter.
void collectFilterGraphs(const RelNode &node, std::vector<GraphConstraint> &out) {
	switch (node.kind()) {
	case RelKind::Filter: {
		const auto &f = static_cast<const sparql2sql::FilterNode &>(node);
		out.push_back(f.activeGraph);
		collectFilterGraphs(*f.child, out);
		break;
	}
	case RelKind::Bind:
		collectFilterGraphs(*static_cast<const sparql2sql::BindNode &>(node).child, out);
		break;
	case RelKind::Join: {
		const auto &j = static_cast<const sparql2sql::JoinNode &>(node);
		collectFilterGraphs(*j.left, out);
		collectFilterGraphs(*j.right, out);
		break;
	}
	case RelKind::AntiJoin: {
		const auto &a = static_cast<const sparql2sql::AntiJoinNode &>(node);
		collectFilterGraphs(*a.left, out);
		collectFilterGraphs(*a.right, out);
		break;
	}
	case RelKind::UnionByName:
		for (const auto &arm : static_cast<const sparql2sql::UnionByNameNode &>(node).arms) {
			collectFilterGraphs(*arm, out);
		}
		break;
	case RelKind::TransitiveClosure:
		collectFilterGraphs(*static_cast<const sparql2sql::TransitiveClosureNode &>(node).step, out);
		break;
	case RelKind::Spj:
	case RelKind::Raw:
	case RelKind::SingleRow:
	case RelKind::Empty:
		break;
	}
}

RelNodePtr foldFixture(const char *rqFile, TranslationContext &ctx, std::unique_ptr<sparql::ast::Query> &queryOut) {
	Parser parser;
	queryOut = parser.parseFile(std::string(SOURCE_SPARQL2SQL_DIR) + rqFile);
	return fold(*queryOut->where, ctx);
}

} // namespace

TEST_CASE("GraphConstraint: a value-initialised constraint is the default graph") {
	GraphConstraint g;
	CHECK(g.kind == GraphConstraint::Kind::Default);
	CHECK(g.isDefault());
	// This default is what makes the whole feature SQL-neutral for queries that
	// never mention GRAPH: every construction site gets it for free.
}

TEST_CASE("ActiveGraphGuard: saves and restores, and a nested guard replaces rather than composes") {
	R2RMLMapping mapping = empDept();
	DuckDbDialect dialect;
	TranslationContext ctx(mapping, dialect);

	REQUIRE(ctx.activeGraph().isDefault());
	{
		TranslationContext::ActiveGraphGuard outer(ctx, boundGraph("http://example.com/g1"));
		REQUIRE(ctx.activeGraph().kind == GraphConstraint::Kind::BoundIri);
		CHECK(ctx.activeGraph().iri == "http://example.com/g1");
		{
			// SPARQL 1.1 Section 13.3: the inner GRAPH's graph *becomes* the
			// active graph outright - it does not intersect with the outer one.
			TranslationContext::ActiveGraphGuard inner(ctx, variableGraph("g"));
			REQUIRE(ctx.activeGraph().kind == GraphConstraint::Kind::Variable);
			CHECK(ctx.activeGraph().varName == "g");
		}
		// ...and the enclosing one must come back, which is why this is
		// save/restore rather than SubqueryDepthGuard's increment/decrement.
		REQUIRE(ctx.activeGraph().kind == GraphConstraint::Kind::BoundIri);
		CHECK(ctx.activeGraph().iri == "http://example.com/g1");
	}
	CHECK(ctx.activeGraph().isDefault());
}

TEST_CASE("fold: a FILTER captures the active graph, because its predicate is translated after the guard is gone") {
	R2RMLMapping mapping = graphMapping();
	DuckDbDialect dialect;
	TranslationContext ctx(mapping, dialect);

	std::unique_ptr<sparql::ast::Query> query;
	RelNodePtr root;
	{
		TranslationContext::ActiveGraphGuard guard(ctx, boundGraph("http://example.com/graph/g1"));
		root = foldFixture("emp_dept_filter_exists.rq", ctx, query);
	}
	// Guard is now destroyed - exactly the situation renderFilter runs in.
	REQUIRE(ctx.activeGraph().isDefault());

	std::vector<GraphConstraint> graphs;
	collectFilterGraphs(*root, graphs);
	REQUIRE(graphs.size() == 1);
	CHECK(graphs[0].kind == GraphConstraint::Kind::BoundIri);
	CHECK(graphs[0].iri == "http://example.com/graph/g1");
}

TEST_CASE("fold: a BIND captures the active graph too") {
	R2RMLMapping mapping = empDept();
	DuckDbDialect dialect;
	TranslationContext ctx(mapping, dialect);

	std::unique_ptr<sparql::ast::Query> query;
	RelNodePtr root;
	{
		TranslationContext::ActiveGraphGuard guard(ctx, variableGraph("g"));
		root = foldFixture("emp_dept_optional_bind.rq", ctx, query);
	}

	// Walk down to the BindNode; a BIND's expression may equally contain an
	// EXISTS, so it needs the same treatment as FILTER.
	const RelNode *n = root.get();
	while (n->kind() != RelKind::Bind) {
		REQUIRE(n->kind() == RelKind::Filter);
		n = static_cast<const sparql2sql::FilterNode &>(*n).child.get();
	}
	const auto &b = static_cast<const sparql2sql::BindNode &>(*n);
	CHECK(b.activeGraph.kind == GraphConstraint::Kind::Variable);
	CHECK(b.activeGraph.varName == "g");
}

TEST_CASE("optimize: filter pushdown carries the captured graph onto every replacement FilterNode") {
	// The regression that matters. pushFilters destroys the original FilterNode
	// and pushConjuncts/wrapFilters build fresh ones from bare Expression*, so
	// without threading the constraint through, the capture would be silently
	// dropped by the optimizer even though fold() set it correctly.
	//
	// An EXISTS conjunct is never pushed into a join side, distributed into
	// union arms, or folded into an Spj (pushConjuncts' containsExists guards),
	// so it always survives as a FilterNode - which is precisely the node whose
	// deferred translation needs the graph.
	R2RMLMapping mapping = graphMapping();
	DuckDbDialect dialect;
	TranslationContext ctx(mapping, dialect);

	std::unique_ptr<sparql::ast::Query> query;
	RelNodePtr root;
	{
		TranslationContext::ActiveGraphGuard guard(ctx, boundGraph("http://example.com/graph/g1"));
		root = foldFixture("emp_dept_filter_exists.rq", ctx, query);
	}

	sparql2sql::OptimizerOptions opts;
	opts.ctx = &ctx;
	opts.catalog = ctx.catalog();
	root = sparql2sql::optimize(std::move(root), opts);

	std::vector<GraphConstraint> graphs;
	collectFilterGraphs(*root, graphs);
	REQUIRE_FALSE(graphs.empty());
	for (const auto &g : graphs) {
		CHECK(g.kind == GraphConstraint::Kind::BoundIri);
		CHECK(g.iri == "http://example.com/graph/g1");
	}
}

// ---------------------------------------------------------------------------
// End-to-end proof of the deferred-expression capture. Until GRAPH blocks
// folded, the capture could only be asserted structurally (above); these are
// the observable consequences.
// ---------------------------------------------------------------------------

namespace {

std::string translateWith(const char *ttlFile, const std::string &queryText) {
	R2RMLParser mappingParser;
	R2RMLMapping mapping = mappingParser.parse(std::string(SOURCE_R2RML_DIR) + ttlFile);
	REQUIRE(mapping.isValid());
	Parser parser;
	auto q = parser.parseString("PREFIX ex: <http://example.com/ns#>\n" + queryText);
	DuckDbDialect dialect;
	return sparql2sql::translateQuery(*q, mapping, dialect);
}

bool has(const std::string &haystack, const std::string &needle) {
	return haystack.find(needle) != std::string::npos;
}

} // namespace

TEST_CASE("GRAPH: an EXISTS inside a GRAPH block is evaluated against that graph") {
	// The hazard the FilterNode capture exists for. The EXISTS body folds at
	// *render* time, after every ActiveGraphGuard is gone, so without the
	// capture it would resolve ex:name against the default graph.
	//
	// In sparql2sql_graphs.ttl, ex:name is mapped twice: EmpMap's is in
	// <graph/g1> (ENAME), DeptMap's is default-graph only (DNAME). Which table
	// the EXISTS body reads is therefore a direct readout of which graph it was
	// folded against.
	std::string sql =
	    translateWith("sparql2sql_graphs.ttl", "SELECT ?d WHERE { GRAPH <http://example.com/graph/g1> {\n"
	                                           "  ?d ex:location ?l . FILTER(EXISTS { ?d ex:name ?n . }) } }");
	CHECK(has(sql, "EXISTS"));
	CHECK(has(sql, "\"ENAME\""));       // g1's ex:name - correct
	CHECK_FALSE(has(sql, "\"DNAME\"")); // default graph's - would mean the capture was lost
}

TEST_CASE("GRAPH: a sub-select inside a GRAPH block inherits the active graph") {
	// A sub-select folds inside fold(), so the guard is still live and
	// inheritance is automatic - which is also the correct semantics.
	std::string sql = translateWith("sparql2sql_graphs.ttl", "SELECT ?n WHERE { GRAPH <http://example.com/graph/g1> {\n"
	                                                         "  { SELECT ?n WHERE { ?e ex:name ?n . } } } }");
	CHECK(has(sql, "\"ENAME\""));
	CHECK_FALSE(has(sql, "\"DNAME\""));
}

TEST_CASE("GRAPH ?g: the graph name is projected as an ordinary query variable") {
	// ?g must reach the SELECT list; a constant graph map folds it to a literal.
	std::string sql = translateWith("sparql2sql_graphs.ttl", "SELECT ?g ?d WHERE { GRAPH ?g { ?d ex:staff ?s . } }");
	CHECK(has(sql, "\"v_g\""));
	CHECK(has(sql, "'http://example.com/graph/g1'"));
}
