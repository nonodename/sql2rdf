/**
 * Structural tests for hoisting rr:sqlQuery logical tables into WITH-clause
 * CTEs.
 *
 * A view's SQL used to be inlined as a derived table at every use site, and
 * one view commonly backs many: each predicate-object map of its TriplesMap,
 * both sides of a referencing object map, and every arm of a variable-predicate
 * expansion. That duplicated the text across the whole statement and left the
 * engine no syntactic signal the occurrences were one relation, so it could not
 * consider evaluating the view once. Each distinct view is now defined once in
 * a top-level WITH clause and referenced by name.
 *
 * These tests assert that shape. That the hoisted form still returns the same
 * rows is settled in tests/duckdb/.
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

const char *const kPrefix = "PREFIX ex: <http://example.com/ns#>\n"
                            "PREFIX rdf: <http://www.w3.org/1999/02/22-rdf-syntax-ns#>\n";

std::string translate(const std::string &ttlFile, const std::string &queryBody) {
	R2RMLParser mappingParser;
	R2RMLMapping mapping = mappingParser.parse(SOURCE_R2RML_DIR + ttlFile);
	REQUIRE(mapping.isValid());
	Parser parser;
	auto q = parser.parseString(kPrefix + queryBody);
	DuckDbDialect dialect;
	return translateQuery(*q, mapping, dialect);
}

std::size_t countOccurrences(const std::string &haystack, const std::string &needle) {
	std::size_t count = 0;
	for (std::size_t at = haystack.find(needle); at != std::string::npos; at = haystack.find(needle, at + 1)) {
		++count;
	}
	return count;
}

bool contains(const std::string &haystack, const std::string &needle) {
	return haystack.find(needle) != std::string::npos;
}

// The body of <#DeptTableView> in example_emp_dept.ttl. Its correlated
// subquery makes it the kind of view that is expensive to evaluate twice.
const char *const kDeptViewTail = "FROM DEPT";

} // namespace

TEST_CASE("view CTE: an rr:sqlQuery view is defined once in a WITH clause, not inlined per use site", "[sparql2sql]") {
	// ex:name comes off both TriplesMap1 (rr:tableName "EMP") and TriplesMap2
	// (the view), so the statement has a view-backed arm.
	std::string sql = translate("example_emp_dept.ttl", "SELECT ?e WHERE { ?e ex:name ?n . }");

	REQUIRE(sql.compare(0, 5, "WITH ") == 0);
	CHECK(countOccurrences(sql, kDeptViewTail) == 1);
	// Referenced by name where the derived table used to sit.
	CHECK(contains(sql, "cte1 AS t"));
	// No closure CTE here, so the RECURSIVE flag is not needed - which keeps the
	// view body out of a scope where a real table named "cte1" would be shadowed.
	CHECK_FALSE(contains(sql, "WITH RECURSIVE"));
}

TEST_CASE("view CTE: two predicates off one view share a single definition", "[sparql2sql]") {
	// ex:location and ex:staff are both predicate-object maps of TriplesMap2,
	// so before hoisting this inlined <#DeptTableView> twice.
	std::string sql =
	    translate("example_emp_dept.ttl", "SELECT ?d ?l ?s WHERE { ?d ex:location ?l . ?d ex:staff ?s . }");

	CHECK(countOccurrences(sql, kDeptViewTail) == 1);
	CHECK(countOccurrences(sql, "cte1 AS t") >= 1);
}

TEST_CASE("view CTE: a base table is not hoisted", "[sparql2sql]") {
	// example1.ttl is all rr:tableName: nothing to hoist, so no WITH clause at
	// all rather than an empty one.
	std::string sql = translate("example1.ttl", "SELECT ?s ?o WHERE { ?s ?p ?o . }");

	CHECK(sql.compare(0, 6, "SELECT") == 0);
	CHECK_FALSE(contains(sql, "WITH"));
}

TEST_CASE("view CTE: two TriplesMaps declaring the identical rr:sqlQuery share one CTE", "[sparql2sql]") {
	// sparql2sql_view_types.ttl's <#DeptLabel> and <#DeptCode> read the same
	// query text; ?p being a variable pulls in both, plus the EMP table arm.
	std::string sql = translate("sparql2sql_view_types.ttl", "SELECT ?s ?p ?o WHERE { ?s ?p ?o . }");

	REQUIRE(sql.compare(0, 5, "WITH ") == 0);
	// One definition covering both TriplesMaps, keyed on the query text.
	CHECK(countOccurrences(sql, "SELECT DEPTNO, DNAME FROM DEPT") == 1);
	// The other, genuinely different view still gets its own.
	CHECK(countOccurrences(sql, "LENGTH(ENAME) AS NAMELEN") == 1);
}

TEST_CASE("view CTE: a variable predicate expands to many arms over a single view definition", "[sparql2sql]") {
	// The allTermsRelation path - one arm per predicate-object map per
	// TriplesMap - is what made inlining worst: the arm count grows with the
	// mapping while the view text is the same every time.
	std::string sql = translate("example_emp_dept.ttl", "SELECT ?s ?o WHERE { ?s ?p ?o . }");

	REQUIRE(sql.compare(0, 5, "WITH ") == 0);
	CHECK(countOccurrences(sql, kDeptViewTail) == 1);
	// Many arms, all pointing at that one definition.
	CHECK(countOccurrences(sql, "cte1 AS t") > 1);
}

TEST_CASE("view CTE: a referencing object map joining a view to a base table hoists only the view", "[sparql2sql]") {
	// emp_dept_join.rq's predicate is a ReferencingObjectMap: the child (EMP,
	// a base table) is joined to the parent (the view) inside one FROM element.
	std::string sql = translate("example_emp_dept.ttl", "SELECT ?e ?d WHERE { ?e ex:department ?d . }");

	CHECK(countOccurrences(sql, kDeptViewTail) == 1);
	CHECK(contains(sql, "\"EMP\" AS t"));
	CHECK(contains(sql, "JOIN cte1 AS t"));
}

TEST_CASE("view CTE: a query mixing a closure and a view emits both kinds, views first", "[sparql2sql]") {
	// ex:knows+ registers closure CTEs while *rendering*, i.e. after the view
	// CTEs were registered during translation; ex:location contributes the view.
	// Both kinds land in the one WITH clause.
	std::string sql =
	    translate("example_emp_dept.ttl", "SELECT ?s ?o ?l WHERE { ?s ex:knows+ ?o . ?x ex:location ?l . }");

	REQUIRE(sql.compare(0, 14, "WITH RECURSIVE") == 0);
	REQUIRE(contains(sql, kDeptViewTail));
	REQUIRE(contains(sql, "\"cte_from\""));
	// A closure body may reference a view CTE, while a view body is raw mapping
	// SQL that can never reference a CTE - so views must be defined first.
	CHECK(sql.find(kDeptViewTail) < sql.find("\"cte_from\""));
}

TEST_CASE("view CTE: a view whose only referencing arm was pruned is not emitted", "[sparql2sql]") {
	// A view CTE is registered while the IR is built, so branch pruning can
	// afterwards drop the arm that referenced it. ex:knows resolves to EMP
	// alone, so the DEPT view is minted for a candidate that does not survive.
	std::string sql = translate("example_emp_dept.ttl", "SELECT ?s ?o WHERE { ?s ex:knows+ ?o . }");

	REQUIRE(sql.compare(0, 14, "WITH RECURSIVE") == 0);
	CHECK_FALSE(contains(sql, kDeptViewTail));
	// And nothing is left referring to the dropped definition.
	CHECK_FALSE(contains(sql, "cte1"));
}
