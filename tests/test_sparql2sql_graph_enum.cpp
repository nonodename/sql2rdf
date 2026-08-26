/**
 * Tests for named-graph candidate enumeration (graphBranchesFor +
 * tryAddCandidate's graph position in TriplePatternTranslator.cpp).
 *
 * The active graph is driven directly through ActiveGraphGuard rather than by a
 * GRAPH block, because fold() does not yet accept GraphGraphPattern - that is
 * the next step. What is under test here is the enumeration: which candidates
 * survive a given active graph, what guards they carry, and whether the graph
 * name is projected.
 *
 * Mapping under test is sparql2sql_graphs.ttl; see that file's header for what
 * each triples map is there to exercise.
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
#include "sparql2sql/TranslatedPattern.h"
#include "sparql2sql/Translator.h"
#include "sparql2sql/TranslationError.h"
#include "sparql2sql/TriplePatternTranslator.h"
#include "sparql2sql/ir/SqlRenderer.h"

using r2rml::R2RMLMapping;
using r2rml::R2RMLParser;
using sparql::Parser;
using sparql2sql::boundGraph;
using sparql2sql::DuckDbDialect;
using sparql2sql::GraphConstraint;
using sparql2sql::renderRelation;
using sparql2sql::TranslatedPattern;
using sparql2sql::translateTriplePattern;
using sparql2sql::TranslationContext;
using sparql2sql::variableGraph;

namespace {

const char *const kG1 = "http://example.com/graph/g1";
const char *const kPrefix = "PREFIX ex: <http://example.com/ns#>\n";

R2RMLMapping parseMapping(const char *ttlFile) {
	R2RMLParser mappingParser;
	R2RMLMapping mapping = mappingParser.parse(std::string(SOURCE_R2RML_DIR) + ttlFile);
	REQUIRE(mapping.isValid());
	return mapping;
}

bool contains(const std::string &haystack, const std::string &needle) {
	return haystack.find(needle) != std::string::npos;
}

// Translate `SELECT * WHERE { <pattern> }`'s single triple pattern under the
// given active graph, and render it for structural inspection.
TranslatedPattern translateUnderGraph(const R2RMLMapping &mapping, const std::string &pattern,
                                      const GraphConstraint &graph) {
	Parser parser;
	auto q = parser.parseString(std::string(kPrefix) + "SELECT * WHERE { " + pattern + " }");
	const auto &bgp = static_cast<const sparql::ast::BasicGraphPattern &>(*q->where->elements.at(0));
	DuckDbDialect dialect;
	TranslationContext ctx(mapping, dialect);
	TranslationContext::ActiveGraphGuard guard(ctx, graph);
	return renderRelation(*translateTriplePattern(bgp.triples.at(0), ctx), ctx);
}

// An always-empty relation is how the translator says "this pattern provably
// matches nothing given this mapping" - it never errors for that.
bool isEmptyRelation(const TranslatedPattern &tp) {
	return contains(tp.sql, "WHERE FALSE");
}

} // namespace

TEST_CASE("graph enum: a mapping with no graph maps is unaffected by the default active graph", "[sparql2sql]") {
	R2RMLMapping mapping = parseMapping("example_emp_dept.ttl");
	TranslatedPattern tp = translateUnderGraph(mapping, "?e ex:name ?n .", GraphConstraint());

	// No graph condition, no graph column - byte-identical to pre-feature
	// output. This is the invariant that keeps existing users unaffected.
	CHECK_FALSE(contains(tp.sql, "defaultGraph"));
	CHECK_FALSE(contains(tp.sql, "v_g"));
	CHECK(contains(tp.sql, "\"ENAME\""));
	CHECK_FALSE(isEmptyRelation(tp));
}

TEST_CASE("graph enum: a mapping with no graph maps has no named graphs to match", "[sparql2sql]") {
	// Strict RDF-dataset semantics: GRAPH <anything> over a mapping that
	// declares no graph at all matches nothing. Not an error - a real engine
	// answers zero rows.
	R2RMLMapping mapping = parseMapping("example_emp_dept.ttl");
	TranslatedPattern tp = translateUnderGraph(mapping, "?e ex:name ?n .", boundGraph(kG1));
	CHECK(isEmptyRelation(tp));
}

TEST_CASE("graph enum: named-graph triples are invisible in the default graph", "[sparql2sql]") {
	// ex:location is declared only in <graph/g1>, so a query with no GRAPH block
	// must not see it. This is the deliberate behaviour change of strict
	// semantics.
	R2RMLMapping mapping = parseMapping("sparql2sql_graphs.ttl");
	TranslatedPattern tp = translateUnderGraph(mapping, "?d ex:location ?l .", GraphConstraint());
	CHECK(isEmptyRelation(tp));
}

TEST_CASE("graph enum: an ungraphed predicate-object map stays in the default graph", "[sparql2sql]") {
	// DeptMap's ex:name has no graph map, and DeptMap's subject map has none
	// either, so its graph set is empty -> default graph, with no condition and
	// no graph column.
	R2RMLMapping mapping = parseMapping("sparql2sql_graphs.ttl");
	TranslatedPattern tp = translateUnderGraph(mapping, "?d ex:name ?n .", GraphConstraint());
	REQUIRE_FALSE(isEmptyRelation(tp));
	CHECK(contains(tp.sql, "\"DNAME\""));
	// EmpMap's ex:name is in <graph/g1> (from its subject map), so it must NOT
	// contribute a candidate here.
	CHECK_FALSE(contains(tp.sql, "\"ENAME\""));
	CHECK_FALSE(contains(tp.sql, "defaultGraph"));
}

TEST_CASE("graph enum: GRAPH <iri> keeps only the candidates that graph can produce", "[sparql2sql]") {
	R2RMLMapping mapping = parseMapping("sparql2sql_graphs.ttl");
	TranslatedPattern tp = translateUnderGraph(mapping, "?s ex:name ?n .", boundGraph(kG1));
	REQUIRE_FALSE(isEmptyRelation(tp));
	// EmpMap's ex:name is in g1 via its subject map's constant rr:graph...
	CHECK(contains(tp.sql, "\"ENAME\""));
	// ...while DeptMap's ex:name is default-graph only, so it is pruned. A
	// constant graph map needs no runtime condition: the pruning is static.
	CHECK_FALSE(contains(tp.sql, "\"DNAME\""));
}

TEST_CASE("graph enum: rr:defaultGraph is a member of the graph set, not a suppressor", "[sparql2sql]") {
	// ex:staff declares both <graph/g1> and rr:defaultGraph, so it is reachable
	// from BOTH - matching r2rml::forEachGraphNode, which emits a quad and a
	// default-graph statement for such a triple.
	R2RMLMapping mapping = parseMapping("sparql2sql_graphs.ttl");

	TranslatedPattern viaDefault = translateUnderGraph(mapping, "?d ex:staff ?s .", GraphConstraint());
	CHECK_FALSE(isEmptyRelation(viaDefault));

	TranslatedPattern viaNamed = translateUnderGraph(mapping, "?d ex:staff ?s .", boundGraph(kG1));
	CHECK_FALSE(isEmptyRelation(viaNamed));
}

TEST_CASE("graph enum: rr:class takes only the SUBJECT map's graphs", "[sparql2sql]") {
	// The counterintuitive one, and it follows the export path exactly
	// (TriplesMap::generateTriples passes an empty pom-graph list for rr:class).
	//
	//   EmpMap  has a subject-level rr:graph  -> its rdf:type is in <graph/g1>
	//   DeptMap has none                      -> its rdf:type is in the default graph
	R2RMLMapping mapping = parseMapping("sparql2sql_graphs.ttl");

	TranslatedPattern inDefault = translateUnderGraph(mapping, "?s a ?c .", GraphConstraint());
	REQUIRE_FALSE(isEmptyRelation(inDefault));
	CHECK(contains(inDefault.sql, "ns#Department"));
	CHECK_FALSE(contains(inDefault.sql, "ns#Employee"));

	TranslatedPattern inG1 = translateUnderGraph(mapping, "?s a ?c .", boundGraph(kG1));
	REQUIRE_FALSE(isEmptyRelation(inG1));
	CHECK(contains(inG1.sql, "ns#Employee"));
	CHECK_FALSE(contains(inG1.sql, "ns#Department"));
}

TEST_CASE("graph enum: a bound graph IRI inverts against a template graph map", "[sparql2sql]") {
	// EmpMap's ex:dept has rr:graphMap [ rr:template ".../dept/{DEPTNO}" ], so
	// GRAPH <.../dept/10> must invert to a condition on DEPTNO rather than
	// comparing constructed IRI text.
	R2RMLMapping mapping = parseMapping("sparql2sql_graphs.ttl");
	TranslatedPattern tp =
	    translateUnderGraph(mapping, "?e ex:dept ?d .", boundGraph("http://example.com/graph/dept/10"));
	REQUIRE_FALSE(isEmptyRelation(tp));
	CHECK(contains(tp.sql, "\"DEPTNO\""));
	CHECK(contains(tp.sql, "'10'"));

	// A graph the template provably cannot produce prunes the candidate.
	TranslatedPattern miss = translateUnderGraph(mapping, "?e ex:dept ?d .", boundGraph("http://elsewhere.example/g"));
	CHECK(isEmptyRelation(miss));
}

TEST_CASE("graph enum: GRAPH ?g projects the graph name as a bound variable", "[sparql2sql]") {
	R2RMLMapping mapping = parseMapping("sparql2sql_graphs.ttl");
	TranslatedPattern tp = translateUnderGraph(mapping, "?d ex:location ?l .", variableGraph("g"));
	REQUIRE_FALSE(isEmptyRelation(tp));

	// ?g is bound (a graph name is always present when the triple is in a named
	// graph) and carries the graph IRI.
	CHECK(tp.boundVars.count("g") == 1);
	CHECK(tp.optionalVars.count("g") == 0);
	CHECK(contains(tp.sql, "\"v_g\""));
	CHECK(contains(tp.sql, kG1));

	// A graph name is always an IRI, whatever the term map declared.
	CHECK(tp.termInfoOf("g").kind == sparql2sql::RdfTermKind::Iri);
}

TEST_CASE("graph enum: GRAPH ?g over a template graph map excludes a dynamic rr:defaultGraph", "[sparql2sql]") {
	// A per-row graph name that happens to equal rr:defaultGraph denotes the
	// default graph, so it is not a named-graph solution. Here the template
	// provably cannot produce that IRI, so the guard is resolved statically
	// rather than emitted - no always-true comparison in the output.
	R2RMLMapping mapping = parseMapping("sparql2sql_graphs.ttl");
	TranslatedPattern tp = translateUnderGraph(mapping, "?e ex:dept ?d .", variableGraph("g"));
	REQUIRE_FALSE(isEmptyRelation(tp));
	CHECK(contains(tp.sql, "\"v_g\""));
	CHECK(contains(tp.sql, "graph/dept/"));
}

TEST_CASE("graph enum: GRAPH ?g unified with the subject position equates the two", "[sparql2sql]") {
	// The graph is the 4th position of tryAddCandidate's self-join guard, so
	// reusing a subject/object variable as the graph name must force the two
	// source expressions equal rather than project the variable twice.
	R2RMLMapping mapping = parseMapping("sparql2sql_graphs.ttl");
	TranslatedPattern tp = translateUnderGraph(mapping, "?g ex:location ?l .", variableGraph("g"));

	// Projected exactly once despite occupying two positions - that is the
	// guard's job, and it is what would break if the graph entry were merged
	// into the loop incorrectly.
	REQUIRE(tp.allVars().count("g") == 1);

	// DeptMap's subject is .../department/{DEPTNO} while the graph is the
	// constant <graph/g1>, so the guard emits an equality between the two source
	// expressions. It does NOT attempt to prove them disjoint statically (the
	// guard has no such pass, unlike bound-term inversion), so the relation is
	// real SQL carrying an unsatisfiable condition rather than an EmptyNode.
	CHECK_FALSE(isEmptyRelation(tp));
	CHECK(contains(tp.sql, kG1));
	CHECK(contains(tp.sql, "department/"));
	CHECK(contains(tp.sql, " = "));
}

// ---------------------------------------------------------------------------
// Refusals. A graph VARIABLE combined with a path operator that is not anchored
// on a matched triple cannot bind the graph name, so it is refused rather than
// answered wrongly. Both were reachable the moment GRAPH blocks started
// folding, and the `+`/`*` form previously emitted SQL referencing a column the
// closure had dropped - invalid, not merely wrong.
// ---------------------------------------------------------------------------

namespace {

std::string translateFull(const char *ttlFile, const char *rqFile) {
	R2RMLMapping mapping = parseMapping(ttlFile);
	Parser parser;
	auto q = parser.parseFile(std::string(SOURCE_SPARQL2SQL_DIR) + rqFile);
	DuckDbDialect dialect;
	return sparql2sql::translateQuery(*q, mapping, dialect);
}

} // namespace

TEST_CASE("graph refusal: GRAPH ?g with an arbitrary-length path throws", "[sparql2sql]") {
	CHECK_THROWS_AS(translateFull("sparql2sql_graphs.ttl", "unsupported_graph_var_path_plus.rq"),
	                sparql2sql::TranslationError);
}

TEST_CASE("graph refusal: GRAPH ?g with a zero-length path throws", "[sparql2sql]") {
	CHECK_THROWS_AS(translateFull("sparql2sql_graphs.ttl", "unsupported_graph_var_path_zero.rq"),
	                sparql2sql::TranslationError);
}

TEST_CASE("graph refusal: the same path inside a NAMED graph is supported", "[sparql2sql]") {
	// The contrast that makes the two refusals above precise rather than a
	// blanket "no paths inside GRAPH": a bound IRI carries its condition inside
	// the step relation, so it needs no invariant column.
	std::string sql = translateFull("sparql2sql_graphs.ttl", "graph_iri_path_plus.rq");
	CHECK(contains(sql, "WITH RECURSIVE"));
	CHECK(contains(sql, "cte_from"));
	// And crucially it does not reference a graph column the closure dropped.
	CHECK_FALSE(contains(sql, "v_g"));
}
