/**
 * Tests for FROM / FROM NAMED dataset clauses (ActiveDataset, applied inside
 * graphBranchesFor).
 *
 * The whole point of the dataset is which graphs a pattern may match, so these
 * assert on that: which candidate survives, or that none does. Two SPARQL 1.1
 * Section 13.2 rules make a query legitimately return nothing, and both read
 * like a bug unless they are pinned:
 *
 *   - FROM *replaces* the default graph, so with `FROM <g>` a triple with no
 *     graph map at all becomes invisible.
 *   - FROM NAMED alone leaves the default graph EMPTY, so a pattern outside any
 *     GRAPH block matches nothing.
 *
 * Mapping under test is sparql2sql_graphs.ttl; see its header for the shapes.
 */

#include <catch2/catch_test_macros.hpp>

#include <string>

#ifndef SOURCE_R2RML_DIR
#define SOURCE_R2RML_DIR ""
#endif

#include "r2rml/R2RMLMapping.h"
#include "r2rml/R2RMLParser.h"
#include "sparql-parser/Parser.h"
#include "sparql2sql/DuckDbDialect.h"
#include "sparql2sql/Translator.h"

using r2rml::R2RMLMapping;
using r2rml::R2RMLParser;
using sparql::Parser;
using sparql2sql::DuckDbDialect;
using sparql2sql::translateQuery;

namespace {

const char *const kG1 = "http://example.com/graph/g1";
const char *const kDept10 = "http://example.com/graph/dept/10";

std::string translate(const char *ttlFile, const std::string &queryBody) {
	R2RMLParser mappingParser;
	R2RMLMapping mapping = mappingParser.parse(std::string(SOURCE_R2RML_DIR) + ttlFile);
	REQUIRE(mapping.isValid());
	Parser parser;
	auto q = parser.parseString("PREFIX ex: <http://example.com/ns#>\n" + queryBody);
	DuckDbDialect dialect;
	return translateQuery(*q, mapping, dialect);
}

std::string graphsQuery(const std::string &queryBody) {
	return translate("sparql2sql_graphs.ttl", queryBody);
}

bool has(const std::string &haystack, const std::string &needle) {
	return haystack.find(needle) != std::string::npos;
}

bool empty(const std::string &sql) {
	return has(sql, "WHERE FALSE");
}

} // namespace

TEST_CASE("dataset: no clauses leaves the dataset unrestricted", "[sparql2sql]") {
	// The pre-existing behaviour, and the shape every existing query takes.
	std::string sql = graphsQuery("SELECT ?d ?n WHERE { ?d ex:name ?n . }");
	CHECK_FALSE(empty(sql));
	CHECK(has(sql, "\"DNAME\"")); // DeptMap's ex:name: no graph map -> default graph
	CHECK_FALSE(has(sql, "\"ENAME\""));
}

TEST_CASE("dataset: FROM merges the named graph into the default graph", "[sparql2sql]") {
	std::string sql = graphsQuery(std::string("SELECT ?d ?l FROM <") + kG1 + "> WHERE { ?d ex:location ?l . }");
	// ex:location is declared only in g1 and is invisible without a dataset
	// clause; FROM makes it the default graph, so now it matches.
	CHECK_FALSE(empty(sql));
	CHECK(has(sql, "\"LOC\""));
}

TEST_CASE("dataset: FROM replaces the default graph, so ungraphed triples become invisible", "[sparql2sql]") {
	// The rule that surprises people. ex:name is mapped twice: EmpMap's is in g1
	// (ENAME), DeptMap's has no graph at all (DNAME). Under FROM <g1> only the
	// former is in the dataset's default graph - the ungraphed one drops out
	// even though it was the *only* match without the clause.
	std::string sql = graphsQuery(std::string("SELECT ?d ?n FROM <") + kG1 + "> WHERE { ?d ex:name ?n . }");
	CHECK_FALSE(empty(sql));
	CHECK(has(sql, "\"ENAME\""));
	CHECK_FALSE(has(sql, "\"DNAME\""));
}

TEST_CASE("dataset: FROM NAMED alone leaves the default graph empty", "[sparql2sql]") {
	// No FROM clause means no default-graph content, so a pattern outside a
	// GRAPH block matches nothing - even though the graph named here does
	// contain the triples the pattern asks for (see the next case).
	std::string sql = graphsQuery(std::string("SELECT ?d ?l FROM NAMED <") + kG1 + "> WHERE { ?d ex:location ?l . }");
	CHECK(empty(sql));
}

TEST_CASE("dataset: FROM NAMED makes a graph nameable by GRAPH", "[sparql2sql]") {
	std::string sql = graphsQuery(std::string("SELECT ?d ?l FROM NAMED <") + kG1 + "> WHERE { GRAPH <" + kG1 +
	                              "> { ?d ex:location ?l . } }");
	CHECK_FALSE(empty(sql));
	CHECK(has(sql, "\"LOC\""));
}

TEST_CASE("dataset: FROM alone leaves no named graphs, so every GRAPH block is empty", "[sparql2sql]") {
	// The mirror of the case above: naming a graph with FROM puts it in the
	// default graph but does NOT make it nameable.
	std::string sql = graphsQuery(std::string("SELECT ?d ?l FROM <") + kG1 + "> WHERE { GRAPH <" + kG1 +
	                              "> { ?d ex:location ?l . } }");
	CHECK(empty(sql));
}

TEST_CASE("dataset: GRAPH over an IRI the dataset does not name matches nothing", "[sparql2sql]") {
	std::string sql = graphsQuery(std::string("SELECT ?d ?l FROM NAMED <http://elsewhere.example/x> WHERE { GRAPH <") +
	                              kG1 + "> { ?d ex:location ?l . } }");
	CHECK(empty(sql));
}

TEST_CASE("dataset: a FROM graph the mapping cannot produce is not an error", "[sparql2sql]") {
	// Consistent with translateAtomicPattern's "always succeeds" contract: an
	// unmatchable dataset yields zero rows, not a refusal.
	std::string sql = graphsQuery("SELECT ?d ?l FROM <http://elsewhere.example/x> WHERE { ?d ex:location ?l . }");
	CHECK(empty(sql));
}

TEST_CASE("dataset: several FROM graphs are OR'd across the graph map's inversions", "[sparql2sql]") {
	// ex:dept's graph set is {<graph/g1>, <graph/dept/{DEPTNO}>}. Listing both
	// the constant and one template instance must keep both arms: the constant
	// one unconditionally, the template one under its inverted DEPTNO condition.
	std::string sql =
	    graphsQuery(std::string("SELECT ?e ?d FROM <") + kG1 + "> FROM <" + kDept10 + "> WHERE { ?e ex:dept ?d . }");
	CHECK_FALSE(empty(sql));
	CHECK(has(sql, "UNION")); // both arms survived
	CHECK(has(sql, "'10'"));  // the template arm inverted onto DEPTNO
}

TEST_CASE("dataset: FROM NAMED restricts which graphs GRAPH ?g can bind", "[sparql2sql]") {
	// Without a dataset clause this binds ?g to both g1 and dept/10. Naming only
	// dept/10 must drop the g1 arm and keep the template one, inverted.
	std::string sql =
	    graphsQuery(std::string("SELECT ?g ?e FROM NAMED <") + kDept10 + "> WHERE { GRAPH ?g { ?e ex:dept ?d . } }");
	CHECK_FALSE(empty(sql));
	CHECK(has(sql, "\"v_g\""));
	CHECK(has(sql, "'10'"));
	// The constant-g1 arm is gone: g1 is not nameable in this dataset.
	CHECK_FALSE(has(sql, std::string("'") + kG1 + "'"));
}

TEST_CASE("dataset: a repeated FROM graph is deduplicated", "[sparql2sql]") {
	std::string once = graphsQuery(std::string("SELECT ?d ?l FROM <") + kG1 + "> WHERE { ?d ex:location ?l . }");
	std::string twice =
	    graphsQuery(std::string("SELECT ?d ?l FROM <") + kG1 + "> FROM <" + kG1 + "> WHERE { ?d ex:location ?l . }");
	// Listing the same graph twice is one graph, so it must not duplicate the
	// candidate or double the OR list.
	CHECK(once == twice);
}

TEST_CASE("dataset: a mapping with no graph maps has an empty default graph under FROM", "[sparql2sql]") {
	// FROM replaces the default graph, and example_emp_dept.ttl declares no
	// graphs at all, so there is nothing for FROM to select.
	std::string sql =
	    translate("example_emp_dept.ttl", std::string("SELECT ?e ?n FROM <") + kG1 + "> WHERE { ?e ex:name ?n . }");
	CHECK(empty(sql));
}
