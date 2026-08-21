/**
 * Structural tests for the DDL constraint facts a TypeCatalog can carry, and
 * the two rewrites they unlock.
 *
 * The catalog deliberately holds only *schema* facts, never data statistics:
 * NOT NULL and PRIMARY KEY / UNIQUE come from DDL, so a rewrite proved against
 * them stays valid as rows come and go. (Cardinality is left to the target
 * engine, which has better numbers than an information_schema sweep could get.)
 *
 * Two consumers:
 *   - a column declared NOT NULL needs no "IS NOT NULL" guard, since R2RML's
 *     "null column => drop this term" rule can never fire for it;
 *   - a candidate arm whose projected columns contain a whole declared key
 *     needs no DISTINCT, since no two source rows can produce the same output
 *     tuple. This one matters because that DISTINCT is otherwise only removed
 *     when the *enclosing* query dedups.
 *
 * Every test here has a no-catalog counterpart: the catalog is optional, and a
 * missing or partial one must reproduce the old SQL exactly. That is the
 * regression that matters most - the rewrites are opt-in by construction.
 *
 * Whether a real backend's information_schema fills the catalog correctly is
 * settled in tests/duckdb/.
 */

#include <catch2/catch_test_macros.hpp>

#include <set>
#include <string>

#ifndef SOURCE_R2RML_DIR
#define SOURCE_R2RML_DIR ""
#endif

#include "r2rml/R2RMLMapping.h"
#include "r2rml/R2RMLParser.h"
#include "sparql-parser/Parser.h"
#include "sparql2sql/DuckDbDialect.h"
#include "sparql2sql/Translator.h"
#include "sparql2sql/TypeCatalog.h"

using r2rml::R2RMLMapping;
using r2rml::R2RMLParser;
using sparql::Parser;
using sparql2sql::DuckDbDialect;
using sparql2sql::translateQuery;
using sparql2sql::TypeCatalog;

namespace {

const char *const kPrefix = "PREFIX ex: <http://example.com/ns#>\n"
                            "PREFIX rdf: <http://www.w3.org/1999/02/22-rdf-syntax-ns#>\n";

const R2RMLMapping &empDeptMapping() {
	static R2RMLParser parser;
	static R2RMLMapping mapping = parser.parse(SOURCE_R2RML_DIR "example_emp_dept.ttl");
	return mapping;
}

std::string translate(const std::string &queryBody, const TypeCatalog *catalog = nullptr) {
	Parser parser;
	auto q = parser.parseString(kPrefix + queryBody);
	DuckDbDialect dialect;
	return translateQuery(*q, empDeptMapping(), dialect, catalog);
}

bool contains(const std::string &haystack, const std::string &needle) {
	return haystack.find(needle) != std::string::npos;
}

// Types only - what the catalog held before constraint facts existed. Used as
// the control: any behaviour difference below must come from the constraints,
// not from suddenly having types.
TypeCatalog typesOnlyCatalog() {
	TypeCatalog catalog;
	catalog.columnTypes["EMP"]["EMPNO"] = "INTEGER";
	catalog.columnTypes["EMP"]["ENAME"] = "VARCHAR";
	catalog.columnTypes["EMP"]["DEPTNO"] = "INTEGER";
	catalog.columnTypes["EMP"]["MGR"] = "INTEGER";
	return catalog;
}

} // namespace

// --- TypeCatalog::isNotNull ---------------------------------------------

TEST_CASE("TypeCatalog::isNotNull: reports declared columns, case-insensitively", "[sparql2sql]") {
	TypeCatalog catalog;
	catalog.notNullColumns["EMP"].insert("ENAME");

	CHECK(catalog.isNotNull("EMP", "ENAME"));
	// SQL identifiers are case-insensitive, and a backend folds unquoted names
	// to its own case while the mapping declares its own spelling.
	CHECK(catalog.isNotNull("emp", "ename"));
	CHECK(catalog.isNotNull("EMP", "EName"));
}

TEST_CASE("TypeCatalog::isNotNull: unknown table or column is not an assertion", "[sparql2sql]") {
	TypeCatalog catalog;
	catalog.notNullColumns["EMP"].insert("ENAME");

	// Absence means "not known to be non-nullable", never "known nullable" -
	// the conservative direction, which only ever keeps a guard.
	CHECK_FALSE(catalog.isNotNull("EMP", "DEPTNO"));
	CHECK_FALSE(catalog.isNotNull("DEPT", "ENAME"));
	CHECK_FALSE(TypeCatalog().isNotNull("EMP", "ENAME"));
}

// --- TypeCatalog::coversUniqueKey ---------------------------------------

TEST_CASE("TypeCatalog::coversUniqueKey: a projection containing a whole key covers it", "[sparql2sql]") {
	TypeCatalog catalog;
	std::set<std::string> key;
	key.insert("EMPNO");
	catalog.uniqueKeys["EMP"].push_back(key);

	std::set<std::string> exact;
	exact.insert("EMPNO");
	CHECK(catalog.coversUniqueKey("EMP", exact));

	// A superset still determines the row.
	std::set<std::string> superset;
	superset.insert("EMPNO");
	superset.insert("ENAME");
	CHECK(catalog.coversUniqueKey("EMP", superset));

	// Case-insensitive on both the table and the columns.
	std::set<std::string> lower;
	lower.insert("empno");
	CHECK(catalog.coversUniqueKey("emp", lower));
}

TEST_CASE("TypeCatalog::coversUniqueKey: a composite key needs every column", "[sparql2sql]") {
	TypeCatalog catalog;
	std::set<std::string> key;
	key.insert("EMPNO");
	key.insert("DEPTNO");
	catalog.uniqueKeys["EMP"].push_back(key);

	std::set<std::string> partial;
	partial.insert("EMPNO");
	CHECK_FALSE(catalog.coversUniqueKey("EMP", partial));

	std::set<std::string> whole;
	whole.insert("EMPNO");
	whole.insert("DEPTNO");
	CHECK(catalog.coversUniqueKey("EMP", whole));
}

TEST_CASE("TypeCatalog::coversUniqueKey: declines on an unknown table or an empty key", "[sparql2sql]") {
	TypeCatalog catalog;
	std::set<std::string> cols;
	cols.insert("EMPNO");
	CHECK_FALSE(catalog.coversUniqueKey("EMP", cols));

	// An empty constraint column list must not vacuously cover every
	// projection - that would strip DISTINCT from arms it cannot prove.
	catalog.uniqueKeys["EMP"].emplace_back();
	CHECK_FALSE(catalog.coversUniqueKey("EMP", cols));
	CHECK_FALSE(catalog.coversUniqueKey("EMP", std::set<std::string>()));
}

// --- NOT NULL guard suppression -----------------------------------------

TEST_CASE("constraints: a NOT NULL column needs no IS NOT NULL guard", "[sparql2sql]") {
	const std::string query = "SELECT ?n WHERE { ?e ex:name ?n }";

	// Control: with types alone the guard is emitted, exactly as before.
	TypeCatalog types = typesOnlyCatalog();
	CHECK(contains(translate(query, &types), "\"ENAME\" IS NOT NULL"));

	// Declaring ENAME NOT NULL makes that conjunct unconditionally true.
	TypeCatalog withConstraint = typesOnlyCatalog();
	withConstraint.notNullColumns["EMP"].insert("ENAME");
	CHECK_FALSE(contains(translate(query, &withConstraint), "\"ENAME\" IS NOT NULL"));
}

TEST_CASE("constraints: a nullable column keeps its guard", "[sparql2sql]") {
	// MGR is nullable (an employee at the top of the chain reports to nobody),
	// so R2RML's null-column rule really can fire and the guard is load-bearing.
	TypeCatalog catalog = typesOnlyCatalog();
	catalog.notNullColumns["EMP"].insert("ENAME");
	CHECK(contains(translate("SELECT ?m WHERE { ?e ex:knows ?m }", &catalog), "\"MGR\" IS NOT NULL"));
}

TEST_CASE("constraints: no catalog means every guard is kept", "[sparql2sql]") {
	// The regression that matters most: the rewrites are opt-in, so a consumer
	// that supplies no catalog must get byte-identical SQL to before.
	std::string sql = translate("SELECT ?n WHERE { ?e ex:name ?n }");
	CHECK(contains(sql, "\"ENAME\" IS NOT NULL"));
}

// --- Key-proven DISTINCT elimination ------------------------------------

TEST_CASE("constraints: a key-derived subject makes a candidate arm's DISTINCT redundant", "[sparql2sql]") {
	// The subject is rr:template ".../employee/{EMPNO}" over EMP, and EMPNO is
	// the declared key, so the arm's rows are already distinct. Note the query
	// itself does NOT say DISTINCT - that is the whole point, since stripDistinct
	// only fires when the enclosing query dedups.
	const std::string query = "SELECT ?e WHERE { ?e rdf:type ex:Employee }";

	TypeCatalog types = typesOnlyCatalog();
	CHECK(contains(translate(query, &types), "SELECT DISTINCT"));

	TypeCatalog withKey = typesOnlyCatalog();
	std::set<std::string> key;
	key.insert("EMPNO");
	withKey.uniqueKeys["EMP"].push_back(key);
	CHECK_FALSE(contains(translate(query, &withKey), "SELECT DISTINCT"));
}

TEST_CASE("constraints: a non-key projection keeps its DISTINCT", "[sparql2sql]") {
	// ?n projects ENAME alone, which no declared key covers: two employees may
	// share a name, and R2RML forward generation would emit that literal once,
	// so the dedup is load-bearing.
	TypeCatalog catalog = typesOnlyCatalog();
	std::set<std::string> key;
	key.insert("EMPNO");
	catalog.uniqueKeys["EMP"].push_back(key);
	CHECK(contains(translate("SELECT ?n WHERE { ?e ex:name ?n }", &catalog), "SELECT DISTINCT"));
}

TEST_CASE("constraints: no catalog means every candidate DISTINCT is kept", "[sparql2sql]") {
	CHECK(contains(translate("SELECT ?e WHERE { ?e rdf:type ex:Employee }"), "SELECT DISTINCT"));
}

// --- Empty-relation propagation (needs no catalog) ----------------------

TEST_CASE("propagateEmpty: a union drops an arm no mapping can satisfy", "[sparql2sql]") {
	// ex:nosuch appears in no triples map, so its pattern has zero candidates
	// and translates to an EmptyNode. The surviving union has one arm, which
	// unwraps entirely - so no UNION is emitted at all.
	//
	// ex:location deliberately, not ex:name: ex:name is mapped by BOTH triples
	// maps, so its own candidate union would supply a UNION of its own and mask
	// what this test is checking.
	std::string sql = translate("SELECT ?v WHERE { { ?d ex:location ?v } UNION { ?d ex:nosuch ?v } }");
	CHECK_FALSE(contains(sql, "UNION"));
	CHECK(contains(sql, "\"LOC\""));
}

TEST_CASE("propagateEmpty: a MINUS with an unsatisfiable right side vanishes", "[sparql2sql]") {
	// Nothing can be subtracted, so the result is the left operand alone - no
	// NOT EXISTS wrapper.
	std::string sql = translate("SELECT ?n WHERE { ?e ex:name ?n MINUS { ?e ex:nosuch ?x } }");
	CHECK_FALSE(contains(sql, "NOT EXISTS"));
	CHECK(contains(sql, "\"ENAME\""));
}

TEST_CASE("propagateEmpty: an inner join with an unsatisfiable side is empty", "[sparql2sql]") {
	// A conjunction one operand can never satisfy has no solutions at all, so
	// the whole block collapses to the empty relation rather than joining
	// against a subquery that provably returns nothing.
	std::string sql = translate("SELECT ?n WHERE { ?e ex:name ?n . ?e ex:nosuch ?x }");
	CHECK(contains(sql, "WHERE FALSE"));
	CHECK_FALSE(contains(sql, "\"EMP\""));
}
