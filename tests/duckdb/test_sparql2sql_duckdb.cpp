/**
 * End-to-end validation of the SPARQL-to-SQL translator: translate each
 * SPARQL fixture against its R2RML mapping, execute the resulting SQL
 * against a real, in-memory DuckDB database seeded with toy data, and
 * assert on the actual result rows.
 *
 * This is the primary correctness oracle for the whole feature - the
 * test_runner-based tests (test_sparql2sql_*.cpp) can only assert on the
 * structural shape of generated SQL text, since test_runner deliberately
 * never links DuckDB (see CLAUDE.md). This target is the one place in the
 * repo besides the CLI that requires a real DuckDB, kept as a separate,
 * gated CTest target for exactly that reason.
 */

#include <catch2/catch.hpp>

#include <algorithm>
#include <cctype>
#include <map>
#include <string>
#include <vector>

#ifndef SOURCE_R2RML_DIR
#define SOURCE_R2RML_DIR ""
#endif
#ifndef SOURCE_SPARQL2SQL_DIR
#define SOURCE_SPARQL2SQL_DIR ""
#endif

#include "DuckDBConnection.h"
#include "r2rml/R2RMLMapping.h"
#include "r2rml/R2RMLParser.h"
#include "r2rml/SQLResultSet.h"
#include "r2rml/SQLRow.h"
#include "r2rml/SQLValue.h"
#include "sparql-parser/Parser.h"
#include "sparql2sql/DuckDbDialect.h"
#include "sparql2sql/TermInfo.h"
#include "sparql2sql/TranslationError.h"
#include "sparql2sql/Translator.h"
#include "sparql2sql/TypeCatalog.h"
#include "sql2rdf/TypeCatalogLoader.h"

using r2rml::DuckDBConnection;
using r2rml::R2RMLMapping;
using r2rml::R2RMLParser;
using sparql::Parser;
using sparql2sql::DuckDbDialect;
using sparql2sql::translateQuery;

namespace {

// A sentinel distinguishable from any real column value, used to represent
// SQL NULL in the row maps built by collectRows() below.
const char *const kNull = "\x01NULL\x01";

using Row = std::map<std::string, std::string>;

std::unique_ptr<DuckDBConnection> makeSeededDatabase() {
	std::unique_ptr<DuckDBConnection> conn(new DuckDBConnection(":memory:"));
	// EMPNO is the declared key, so the catalog can prove a subject-keyed
	// candidate arm needs no DISTINCT; MGR backs example_emp_dept.ttl's ex:knows
	// edge, giving the +/* property-path closures a real relation to walk.
	conn->execute("CREATE TABLE EMP (EMPNO INTEGER PRIMARY KEY, ENAME VARCHAR, DEPTNO INTEGER, MGR INTEGER)");
	conn->execute("CREATE TABLE DEPT (DEPTNO INTEGER, DNAME VARCHAR, LOC VARCHAR)");
	// SMITH has a department; JONES does not (exercises OPTIONAL/MINUS).
	// SMITH reports to JONES, who reports to nobody - so ex:knows+ from SMITH
	// reaches exactly JONES, and a NULL MGR emits no triple at all.
	conn->execute("INSERT INTO EMP VALUES (7369, 'SMITH', 10, 7400), (7400, 'JONES', NULL, NULL)");
	// SALES has no employees (exercises UNION/MINUS asymmetry).
	conn->execute("INSERT INTO DEPT VALUES (10, 'APPSERVER', 'NEW YORK'), (20, 'SALES', 'BOSTON')");

	conn->execute("CREATE TABLE PEOPLE (ID INTEGER, NAME VARCHAR)");
	conn->execute("INSERT INTO PEOPLE VALUES (1, 'ALICE'), (2, 'BOB')");

	conn->execute("CREATE TABLE ORDERS (CUSTID INTEGER, ORDERID INTEGER)");
	conn->execute("INSERT INTO ORDERS VALUES (42, 99)");

	conn->execute("CREATE TABLE WIDGETS (ID INTEGER, NAME VARCHAR)");
	conn->execute("INSERT INTO WIDGETS VALUES (1, 'GADGET')");

	// LABEL holds RFC3986 reserved characters (space, '/', '?', '#') to
	// exercise percent-encode-on-projection / percent-decode-on-inversion for
	// rr:template.
	conn->execute("CREATE TABLE TAGS (ID INTEGER, LABEL VARCHAR)");
	conn->execute("INSERT INTO TAGS VALUES (1, 'a b/c?d#e')");

	// Cross-table integer join fixture (no declared R2RML FK): COMPANY.CAPIQ
	// and RELATIONS.PARENT are both BIGINT, unified by a shared SPARQL var.
	conn->execute("CREATE TABLE COMPANY (ID INTEGER, CAPIQ BIGINT, LEGALNAME VARCHAR)");
	conn->execute("INSERT INTO COMPANY VALUES (1, 100, 'ACME'), (2, 200, 'GLOBEX')");
	conn->execute("CREATE TABLE RELATIONS (PARENT BIGINT, CHILD BIGINT)");
	conn->execute("INSERT INTO RELATIONS VALUES (100, 200)");

	// Self-referencing chain (1 -> 2 -> 3 -> end) backing sparql2sql_path_terms.ttl,
	// whose deliberately tiny mapping makes the zero-length property path's
	// term universe small enough to assert row-exactly.
	conn->execute("CREATE TABLE NODES (ID INTEGER, NEXT INTEGER)");
	conn->execute("INSERT INTO NODES VALUES (1, 2), (2, 3), (3, NULL)");

	// A pure 3-cycle (1 -> 2 -> 3 -> 1, no dangling end) backing
	// sparql2sql_path_cycle.ttl - the E+/E* cycle-safety tests. Real graph
	// data has cycles, and a recursive CTE that only worked on the acyclic
	// NODES chain above wouldn't actually prove termination/dedup.
	conn->execute("CREATE TABLE CYCLE_NODES (ID INTEGER, NEXT INTEGER)");
	conn->execute("INSERT INTO CYCLE_NODES VALUES (1, 2), (2, 3), (3, 1)");

	// Typed-literal fixture backing sparql2sql_terms.ttl. AMOUNT's two values
	// are chosen so lexicographic and numeric ordering DISAGREE ('100' < '9' as
	// text, 9 < 100 as numbers) - without that, every typed comparison /
	// ORDER BY / MIN / MAX assertion below would pass vacuously. TITLE is a
	// VARCHAR that ex:badtyped deliberately mis-declares as xsd:integer.
	conn->execute("CREATE TABLE MEASURES (ID INTEGER, AMOUNT INTEGER, PRICE DOUBLE, WHEN_DT VARCHAR, "
	              "TITLE VARCHAR, HOMEPAGE VARCHAR, REF INTEGER)");
	conn->execute("INSERT INTO MEASURES VALUES "
	              "(1, 9,   2.50,  '2019-12-31T23:59:59', 'alpha', 'http://ex.org/a', 2), "
	              "(2, 100, 10.00, '2020-06-01T12:00:00', 'beta',  'http://ex.org/b', 1)");

	// Blank-node-subject fixture, and the literal-valued arm of the four
	// multi-arm predicates (ex:mixed / ex:desc / ex:mixedtag / ex:mixeddt).
	//
	// Two rows, not one, and both BODY values chosen deliberately:
	//   'note one' has no numeric reading, so an xsd:string row really is
	//              incomparable with an xsd:integer row from MEASURES.AMOUNT;
	//   '9'        is numerically equal to MEASURES.AMOUNT's 9 and lexically equal
	//              to its text, so a query that only compares lexical forms cannot
	//              tell "9"^^xsd:integer from "9"^^xsd:string. That collision is
	//              the whole point: it is what makes DISTINCT, sameTerm and RDF
	//              term equality observably wrong without a runtime type tag, and
	//              observably right with one.
	conn->execute("CREATE TABLE NOTES (NID INTEGER, BODY VARCHAR)");
	conn->execute("INSERT INTO NOTES VALUES (1, 'note one'), (2, '9')");

	// 11 'a' rows and 2 'b' rows: the group counts (11, 2) sort the wrong way
	// round lexicographically, which is what pins the ORDER-BY-over-a-COUNT
	// -alias fix.
	conn->execute("CREATE TABLE COUNTS (RID INTEGER, G VARCHAR)");
	conn->execute("INSERT INTO COUNTS VALUES (1,'a'),(2,'a'),(3,'a'),(4,'a'),(5,'a'),(6,'a'),(7,'a'),(8,'a'),"
	              "(9,'a'),(10,'a'),(11,'a'),(12,'b'),(13,'b')");

	// Two tables whose subject templates share the same literal shape
	// ("ex:data/PROD/{...}") but name their placeholder column differently -
	// PRODUCTS.PROD_CODE vs ITEMS.ITEM_PROD_CODE - backing
	// sparql2sql_template_join_diff_placeholder.ttl. Only PROD_CODE 7 has a
	// matching ITEMS row.
	conn->execute("CREATE TABLE PRODUCTS (PROD_CODE INTEGER, PNAME VARCHAR)");
	conn->execute("INSERT INTO PRODUCTS VALUES (7, 'WIDGET'), (8, 'GIZMO')");
	conn->execute("CREATE TABLE ITEMS (ITEM_PROD_CODE INTEGER, IDESC VARCHAR)");
	conn->execute("INSERT INTO ITEMS VALUES (7, 'blue widget')");

	return conn;
}

// Resolve the actual reported name of a result column, matching `target`
// case-insensitively (backends report result-column names in their own case).
std::string resolveColName(const std::vector<std::string> &names, const std::string &target) {
	for (const auto &n : names) {
		if (n.size() == target.size()) {
			bool eq = true;
			for (std::size_t i = 0; i < n.size(); ++i) {
				if (std::tolower(static_cast<unsigned char>(n[i])) !=
				    std::tolower(static_cast<unsigned char>(target[i]))) {
					eq = false;
					break;
				}
			}
			if (eq) {
				return n;
			}
		}
	}
	return std::string();
}

sparql2sql::TypeCatalog catalogOf(DuckDBConnection &conn) {
	sparql2sql::TypeCatalog cat;
	std::unique_ptr<r2rml::SQLResultSet> rs =
	    conn.execute("SELECT table_name, column_name, data_type FROM information_schema.columns");
	bool resolved = false;
	std::string tName, cName, dName;
	while (rs->next()) {
		const r2rml::SQLRow &row = rs->getCurrentRow();
		if (!resolved) {
			std::vector<std::string> names = row.columnNames();
			tName = resolveColName(names, "table_name");
			cName = resolveColName(names, "column_name");
			dName = resolveColName(names, "data_type");
			resolved = true;
		}
		if (tName.empty() || cName.empty() || dName.empty()) {
			continue;
		}
		std::unique_ptr<r2rml::SQLValue> t = row.getValue(tName);
		std::unique_ptr<r2rml::SQLValue> c = row.getValue(cName);
		std::unique_ptr<r2rml::SQLValue> d = row.getValue(dName);
		if (t->isNull() || c->isNull() || d->isNull()) {
			continue;
		}
		cat.columnTypes[t->asString()][c->asString()] = d->asString();
	}
	return cat;
}

std::vector<Row> collectRows(r2rml::SQLResultSet &rs) {
	std::vector<Row> rows;
	while (rs.next()) {
		const r2rml::SQLRow &row = rs.getCurrentRow();
		Row out;
		for (const auto &col : row.columnNames()) {
			std::unique_ptr<r2rml::SQLValue> v = row.getValue(col);
			out[col] = v->isNull() ? kNull : v->asString();
		}
		rows.push_back(out);
	}
	return rows;
}

std::vector<Row> translateAndRun(DuckDBConnection &conn, const std::string &rqFile, const std::string &ttlFile) {
	Parser parser;
	auto query = parser.parseFile(SOURCE_SPARQL2SQL_DIR + rqFile);
	R2RMLParser mappingParser;
	R2RMLMapping mapping = mappingParser.parse(SOURCE_R2RML_DIR + ttlFile);
	REQUIRE(mapping.isValid());
	DuckDbDialect dialect;
	std::string sql = translateQuery(*query, mapping, dialect);
	INFO("SQL: " << sql);
	std::unique_ptr<r2rml::SQLResultSet> rs = conn.execute(sql);
	return collectRows(*rs);
}

bool containsRow(const std::vector<Row> &rows, const Row &expected) {
	return std::find(rows.begin(), rows.end(), expected) != rows.end();
}

// One column's values in result order. containsRow() is order-insensitive by
// design, but the ORDER BY cases are *about* the order, so they need this.
std::vector<std::string> columnSeq(const std::vector<Row> &rows, const std::string &name) {
	std::vector<std::string> out;
	out.reserve(rows.size());
	for (const auto &r : rows) {
		std::map<std::string, std::string>::const_iterator it = r.find(name);
		out.push_back(it == r.end() ? std::string() : it->second);
	}
	return out;
}

} // namespace

TEST_CASE("emp_dept_join.rq: only the employee with a non-null department joins") {
	auto conn = makeSeededDatabase();
	auto rows = translateAndRun(*conn, "emp_dept_join.rq", "example_emp_dept.ttl");
	REQUIRE(rows.size() == 1);
	CHECK(containsRow(
	    rows, {{"V_E", "http://data.example.com/employee/7369"}, {"V_D", "http://data.example.com/department/10"}}));
}

TEST_CASE("emp_dept_path_plus_view.rq: a hoisted view CTE and recursive closure CTEs execute in one statement") {
	auto conn = makeSeededDatabase();
	// The mapping's rr:sqlQuery view is hoisted into the same WITH clause as
	// ex:knows+'s closure CTEs. A closure body can reference the view CTE but
	// never the reverse, so the view has to be defined first - get that ordering
	// wrong and DuckDB rejects the statement outright rather than returning
	// wrong rows, which is what makes executing this worth doing.
	auto rows = translateAndRun(*conn, "emp_dept_path_plus_view.rq", "example_emp_dept.ttl");
	// ex:knows+ contributes exactly SMITH -> JONES; ex:location contributes both
	// departments; the patterns are disjoint, so the cross product is 1 x 2.
	REQUIRE(rows.size() == 2);
	CHECK(containsRow(rows, {{"V_S", "http://data.example.com/employee/7369"},
	                         {"V_O", "http://data.example.com/employee/7400"},
	                         {"V_LOC", "NEW YORK"}}));
	CHECK(containsRow(rows, {{"V_S", "http://data.example.com/employee/7369"},
	                         {"V_O", "http://data.example.com/employee/7400"},
	                         {"V_LOC", "BOSTON"}}));
}

TEST_CASE("emp_dept_simple_select.rq: employee and department names both match ex:name") {
	auto conn = makeSeededDatabase();
	auto rows = translateAndRun(*conn, "emp_dept_simple_select.rq", "example_emp_dept.ttl");
	REQUIRE(rows.size() == 4);
	CHECK(containsRow(rows, {{"V_E", "http://data.example.com/employee/7369"}, {"V_N", "SMITH"}}));
	CHECK(containsRow(rows, {{"V_E", "http://data.example.com/employee/7400"}, {"V_N", "JONES"}}));
	CHECK(containsRow(rows, {{"V_E", "http://data.example.com/department/10"}, {"V_N", "APPSERVER"}}));
	CHECK(containsRow(rows, {{"V_E", "http://data.example.com/department/20"}, {"V_N", "SALES"}}));
}

TEST_CASE("emp_dept_optional.rq: JONES (no department) still appears with ?d unbound") {
	auto conn = makeSeededDatabase();
	auto rows = translateAndRun(*conn, "emp_dept_optional.rq", "example_emp_dept.ttl");
	REQUIRE(rows.size() == 4);
	CHECK(containsRow(rows, {{"V_E", "http://data.example.com/employee/7369"},
	                         {"V_N", "SMITH"},
	                         {"V_D", "http://data.example.com/department/10"}}));
	CHECK(containsRow(rows, {{"V_E", "http://data.example.com/employee/7400"}, {"V_N", "JONES"}, {"V_D", kNull}}));
	CHECK(containsRow(rows, {{"V_E", "http://data.example.com/department/10"}, {"V_N", "APPSERVER"}, {"V_D", kNull}}));
	CHECK(containsRow(rows, {{"V_E", "http://data.example.com/department/20"}, {"V_N", "SALES"}, {"V_D", kNull}}));
}

TEST_CASE("emp_dept_xsd_cast.rq: xsd:integer() cast filters on the numeric STAFF column") {
	auto conn = makeSeededDatabase();
	auto rows = translateAndRun(*conn, "emp_dept_xsd_cast.rq", "example_emp_dept.ttl");
	REQUIRE(rows.size() == 1);
	CHECK(containsRow(rows, {{"V_D", "http://data.example.com/department/10"}, {"V_STAFF", "1"}}));
}

TEST_CASE("emp_dept_xsd_cast_extra.rq: xsd:boolean/dateTime/date/decimal/long casts on literals") {
	auto conn = makeSeededDatabase();
	auto rows = translateAndRun(*conn, "emp_dept_xsd_cast_extra.rq", "example_emp_dept.ttl");
	REQUIRE(rows.size() == 1);
	CHECK(containsRow(rows, {{"V_E", "http://data.example.com/employee/7369"},
	                         {"V_B", "true"},
	                         {"V_DT", "2020-05-15T10:30:45"},
	                         {"V_D", "2020-05-15"},
	                         {"V_DEC", "123456789012345678.123456789012345678"},
	                         {"V_I", "42"}}));
}

TEST_CASE("emp_dept_datetime_accessors.rq: YEAR/MONTH/DAY/HOURS/MINUTES/SECONDS extract from a literal timestamp") {
	auto conn = makeSeededDatabase();
	auto rows = translateAndRun(*conn, "emp_dept_datetime_accessors.rq", "example_emp_dept.ttl");
	REQUIRE(rows.size() == 1);
	CHECK(containsRow(rows, {{"V_E", "http://data.example.com/employee/7369"},
	                         {"V_Y", "2020"},
	                         {"V_MO", "5"},
	                         {"V_D", "15"},
	                         {"V_H", "10"},
	                         {"V_MI", "30"},
	                         {"V_S", "45"}}));
}

TEST_CASE("emp_dept_nondeterministic_functions.rq: NOW() is stable while RAND()/UUID()/STRUUID() vary per row") {
	auto conn = makeSeededDatabase();
	auto rows = translateAndRun(*conn, "emp_dept_nondeterministic_functions.rq", "example_emp_dept.ttl");
	REQUIRE(rows.size() == 1);
	const Row &row = rows.front();

	// NOW() must be the exact same value for both calls within this query.
	REQUIRE(row.count("V_N1") == 1);
	REQUIRE(row.count("V_N2") == 1);
	CHECK(row.at("V_N1") == row.at("V_N2"));
	CHECK(row.at("V_N1") != kNull);

	// RAND() must produce a value parseable as a double in [0, 1).
	REQUIRE(row.count("V_R") == 1);
	double r = std::stod(row.at("V_R"));
	CHECK(r >= 0.0);
	CHECK(r < 1.0);

	// UUID() must produce a fresh urn:uuid: IRI; STRUUID() the bare UUID string.
	REQUIRE(row.count("V_U") == 1);
	REQUIRE(row.count("V_SU") == 1);
	CHECK(row.at("V_U").substr(0, 9) == "urn:uuid:");
	CHECK(row.at("V_SU").substr(0, 9) != "urn:uuid:");
	CHECK(row.at("V_U") != "urn:uuid:" + row.at("V_SU"));
}

TEST_CASE("emp_dept_union.rq: bag union of ex:name and ex:location results") {
	auto conn = makeSeededDatabase();
	auto rows = translateAndRun(*conn, "emp_dept_union.rq", "example_emp_dept.ttl");
	// 4 ex:name rows (2 employees + 2 departments) + 2 ex:location rows
	// (departments only) = 6.
	CHECK(rows.size() == 6);
}

TEST_CASE("emp_dept_union_mismatched.rq: mismatched branch schemas pad the missing var with NULL") {
	auto conn = makeSeededDatabase();
	auto rows = translateAndRun(*conn, "emp_dept_union_mismatched.rq", "example_emp_dept.ttl");
	// 4 ex:name rows + 1 ex:department row (SMITH only) = 5.
	REQUIRE(rows.size() == 5);
	CHECK(containsRow(rows, {{"V_E", "http://data.example.com/employee/7369"},
	                         {"V_N", kNull},
	                         {"V_D", "http://data.example.com/department/10"}}));
}

TEST_CASE("emp_dept_minus.rq: MINUS removes the employee that has a department") {
	auto conn = makeSeededDatabase();
	auto rows = translateAndRun(*conn, "emp_dept_minus.rq", "example_emp_dept.ttl");
	REQUIRE(rows.size() == 3);
	CHECK_FALSE(containsRow(rows, {{"V_E", "http://data.example.com/employee/7369"}}));
	CHECK(containsRow(rows, {{"V_E", "http://data.example.com/employee/7400"}}));
}

TEST_CASE("emp_dept_minus_novars.rq: zero shared variables makes MINUS a spec-mandated no-op") {
	auto conn = makeSeededDatabase();
	auto rows = translateAndRun(*conn, "emp_dept_minus_novars.rq", "example_emp_dept.ttl");
	// Nothing removed: all 4 ex:name subjects survive.
	CHECK(rows.size() == 4);
}

TEST_CASE("emp_dept_subquery.rq: the subquery restricts to employees with a department") {
	auto conn = makeSeededDatabase();
	auto rows = translateAndRun(*conn, "emp_dept_subquery.rq", "example_emp_dept.ttl");
	REQUIRE(rows.size() == 1);
	CHECK(containsRow(rows, {{"V_E", "http://data.example.com/employee/7369"}, {"V_N", "SMITH"}}));
}

TEST_CASE("emp_dept_class.rq: every subject gets exactly one rdf:type") {
	auto conn = makeSeededDatabase();
	auto rows = translateAndRun(*conn, "emp_dept_class.rq", "example_emp_dept.ttl");
	REQUIRE(rows.size() == 4);
	CHECK(containsRow(rows,
	                  {{"V_E", "http://data.example.com/employee/7369"}, {"V_C", "http://example.com/ns#Employee"}}));
	CHECK(containsRow(rows,
	                  {{"V_E", "http://data.example.com/department/10"}, {"V_C", "http://example.com/ns#Department"}}));
}

TEST_CASE("emp_dept_aggregate.rq: COUNT per department") {
	auto conn = makeSeededDatabase();
	auto rows = translateAndRun(*conn, "emp_dept_aggregate.rq", "example_emp_dept.ttl");
	// Only DEPT 10 has an employee with a non-null department (SMITH).
	REQUIRE(rows.size() == 1);
	CHECK(containsRow(rows, {{"V_D", "http://data.example.com/department/10"}, {"V_CNT", "1"}}));
}

TEST_CASE("emp_dept_name_count.rq: COUNT(*) over a multi-candidate predicate doesn't double-count the "
          "candidate union's own dedup") {
	auto conn = makeSeededDatabase();
	auto rows = translateAndRun(*conn, "emp_dept_name_count.rq", "example_emp_dept.ttl");
	// Same 4 rows as emp_dept_simple_select.rq (2 employees + 2 departments):
	// each candidate SpjRelation's own DISTINCT and the candidate UNION's
	// dedup (UNION BY NAME, not ALL) must survive under an aggregate query,
	// since queryHasAggregate() suppresses the stripDistinct/UNION-ALL
	// optimization that only applies when the outer SELECT DISTINCT/ASK
	// already subsumes per-candidate dedup.
	REQUIRE(rows.size() == 1);
	CHECK(rows[0].at("V_CNT") == "4");
}

TEST_CASE("emp_dept_union_count.rq: COUNT(*) over an explicit bag UNION preserves duplicate solutions") {
	auto conn = makeSeededDatabase();
	auto rows = translateAndRun(*conn, "emp_dept_union_count.rq", "example_emp_dept.ttl");
	// Same 6 rows as emp_dept_union.rq: SPARQL UNION is bag-preserving (UNION
	// ALL BY NAME), so the two branches' rows are not deduped against each
	// other even though each branch's own SpjRelation is still DISTINCT.
	REQUIRE(rows.size() == 1);
	CHECK(rows[0].at("V_CNT") == "6");
}

TEST_CASE("emp_dept_ask.rq: ASK is true when the pattern matches at least once") {
	auto conn = makeSeededDatabase();
	auto rows = translateAndRun(*conn, "emp_dept_ask.rq", "example_emp_dept.ttl");
	REQUIRE(rows.size() == 1);
	CHECK(rows[0].at("ASK") == "true");
}

TEST_CASE("emp_dept_values.rq: VALUES restricts to the given ?n bindings") {
	auto conn = makeSeededDatabase();
	auto rows = translateAndRun(*conn, "emp_dept_values.rq", "example_emp_dept.ttl");
	REQUIRE(rows.size() == 2);
	CHECK(containsRow(rows, {{"V_E", "http://data.example.com/employee/7369"}, {"V_N", "SMITH"}}));
	CHECK(containsRow(rows, {{"V_E", "http://data.example.com/employee/7400"}, {"V_N", "JONES"}}));
}

TEST_CASE("sparql2sql_self_ref.rq: the self-join guard matches the single widget") {
	auto conn = makeSeededDatabase();
	auto rows = translateAndRun(*conn, "sparql2sql_self_ref.rq", "sparql2sql_self_ref.ttl");
	REQUIRE(rows.size() == 1);
	CHECK(containsRow(rows, {{"V_E", "http://ex.org/widget/1"}}));
}

TEST_CASE("sparql2sql_constant_pom.rq: both people have the constant ex:status") {
	auto conn = makeSeededDatabase();
	auto rows = translateAndRun(*conn, "sparql2sql_constant_pom.rq", "sparql2sql_constant_pom.ttl");
	REQUIRE(rows.size() == 2);
	CHECK(containsRow(rows, {{"V_P", "http://data.example.com/person/1"}, {"V_N", "ALICE"}}));
	CHECK(containsRow(rows, {{"V_P", "http://data.example.com/person/2"}, {"V_N", "BOB"}}));
}

TEST_CASE("sparql2sql_multi_placeholder.rq: multi-placeholder template inversion round-trips") {
	auto conn = makeSeededDatabase();
	auto rows = translateAndRun(*conn, "sparql2sql_multi_placeholder.rq", "sparql2sql_multi_placeholder.ttl");
	REQUIRE(rows.size() == 1);
	CHECK(containsRow(rows, {{"V_O", "http://ex.org/order/42/99"}}));
}

TEST_CASE("sparql2sql_percent_encode.rq: subject template projection percent-encodes reserved characters") {
	auto conn = makeSeededDatabase();
	auto rows = translateAndRun(*conn, "sparql2sql_percent_encode.rq", "sparql2sql_percent_encode.ttl");
	REQUIRE(rows.size() == 1);
	// LABEL = "a b/c?d#e": space/'/'/? '#' must each be percent-encoded in the
	// constructed subject IRI, while the plain rr:column object stays raw.
	CHECK(containsRow(rows, {{"V_T", "http://ex.org/tag/a%20b%2Fc%3Fd%23e"}, {"V_L", "a b/c?d#e"}}));
}

TEST_CASE("sparql2sql_percent_encode_filter.rq: a percent-encoded bound IRI inverts back to the raw column value") {
	auto conn = makeSeededDatabase();
	auto rows = translateAndRun(*conn, "sparql2sql_percent_encode_filter.rq", "sparql2sql_percent_encode.ttl");
	REQUIRE(rows.size() == 1);
	CHECK(containsRow(rows, {{"V_L", "a b/c?d#e"}}));
}

TEST_CASE("sparql2sql_native_join.rq: a type catalog enables a native integer join, same rows") {
	auto conn = makeSeededDatabase();
	Parser parser;
	auto query = parser.parseFile(SOURCE_SPARQL2SQL_DIR "sparql2sql_native_join.rq");
	R2RMLParser mappingParser;
	R2RMLMapping mapping = mappingParser.parse(SOURCE_R2RML_DIR "sparql2sql_native_join.ttl");
	REQUIRE(mapping.isValid());
	DuckDbDialect dialect;

	const Row expected = {{"V_CO", "http://ex.org/co/1"}, {"V_REL", "http://ex.org/rel/100-200"}, {"V_CHILD", "200"}};

	// Without a catalog: the ?p join is a VARCHAR-cast comparison.
	std::string plainSql = translateQuery(*query, mapping, dialect);
	INFO("plain SQL: " << plainSql);
	CHECK(plainSql.find("AS VARCHAR) = CAST(") != std::string::npos);
	{
		std::unique_ptr<r2rml::SQLResultSet> rs = conn->execute(plainSql);
		auto rows = collectRows(*rs);
		REQUIRE(rows.size() == 1);
		CHECK(containsRow(rows, expected));
	}

	// The catalog read from the live database must know the BIGINT join
	// columns, case-insensitively (the mapping declares COMPANY/CAPIQ, DuckDB
	// reports them lower-cased). This REQUIRE localizes any failure: if it
	// trips, the information_schema read/lookup is at fault, not translation.
	sparql2sql::TypeCatalog cat = catalogOf(*conn);
	INFO("catalog table count: " << cat.columnTypes.size());
	{
		std::string dump;
		for (const auto &t : cat.columnTypes) {
			dump += "[" + t.first + ": ";
			for (const auto &c : t.second) {
				dump += c.first + "=" + c.second + " ";
			}
			dump += "] ";
		}
		INFO("catalog dump: " << dump);
	}
	INFO("typeOf(COMPANY,CAPIQ)='" << cat.typeOf("COMPANY", "CAPIQ") << "' typeOf(RELATIONS,PARENT)='"
	                               << cat.typeOf("RELATIONS", "PARENT") << "'");
	REQUIRE(cat.comparable("COMPANY", "CAPIQ", "RELATIONS", "PARENT"));

	// With a catalog: the join is emitted on the native BIGINT columns.
	std::string nativeSql = translateQuery(*query, mapping, dialect, &cat);
	INFO("native SQL: " << nativeSql);
	CHECK(nativeSql.find("\"CAPIQ\" = ") != std::string::npos);
	CHECK(nativeSql.find("\"PARENT\"") != std::string::npos);
	{
		std::unique_ptr<r2rml::SQLResultSet> rs = conn->execute(nativeSql);
		auto rows = collectRows(*rs);
		REQUIRE(rows.size() == 1);
		CHECK(containsRow(rows, expected));
	}
}

TEST_CASE("sparql2sql_template_join.rq: two same-template subjects join on the native key, same rows") {
	auto conn = makeSeededDatabase();
	Parser parser;
	auto query = parser.parseFile(SOURCE_SPARQL2SQL_DIR "sparql2sql_template_join.rq");
	R2RMLParser mappingParser;
	R2RMLMapping mapping = mappingParser.parse(SOURCE_R2RML_DIR "sparql2sql_template_join.ttl");
	REQUIRE(mapping.isValid());
	DuckDbDialect dialect;

	// Only PEOPLE.ID 1 has a matching WIDGETS row.
	const Row expected = {{"V_T", "http://ex.org/thing/1"}, {"V_PN", "ALICE"}, {"V_WN", "GADGET"}};

	// Without a catalog: the subjects are compared as constructed IRI strings.
	std::string plainSql = translateQuery(*query, mapping, dialect);
	INFO("plain SQL: " << plainSql);
	CHECK(plainSql.find("'http://ex.org/thing/' || ") != std::string::npos);
	{
		std::unique_ptr<r2rml::SQLResultSet> rs = conn->execute(plainSql);
		auto rows = collectRows(*rs);
		REQUIRE(rows.size() == 1);
		CHECK(containsRow(rows, expected));
	}

	sparql2sql::TypeCatalog cat = catalogOf(*conn);
	REQUIRE(cat.comparable("PEOPLE", "ID", "WIDGETS", "ID"));

	// With a catalog: the join is emitted on the placeholder columns, and the
	// result set is unchanged.
	std::string nativeSql = translateQuery(*query, mapping, dialect, &cat);
	INFO("native SQL: " << nativeSql);
	CHECK(nativeSql.find("\"ID\" = ") != std::string::npos);
	{
		std::unique_ptr<r2rml::SQLResultSet> rs = conn->execute(nativeSql);
		auto rows = collectRows(*rs);
		REQUIRE(rows.size() == 1);
		CHECK(containsRow(rows, expected));
	}
}

TEST_CASE("sparql2sql_template_join_diff_placeholder.rq: same-shape templates with differently-named "
          "placeholders join on their native columns, same rows") {
	auto conn = makeSeededDatabase();
	Parser parser;
	auto query = parser.parseFile(SOURCE_SPARQL2SQL_DIR "sparql2sql_template_join_diff_placeholder.rq");
	R2RMLParser mappingParser;
	R2RMLMapping mapping = mappingParser.parse(SOURCE_R2RML_DIR "sparql2sql_template_join_diff_placeholder.ttl");
	REQUIRE(mapping.isValid());
	DuckDbDialect dialect;

	// Only PRODUCTS.PROD_CODE 7 has a matching ITEMS row.
	const Row expected = {{"V_T", "http://ex.org/data/PROD/7"}, {"V_PN", "WIDGET"}, {"V_D", "blue widget"}};

	// Without a catalog: the subjects are compared as constructed IRI strings.
	std::string plainSql = translateQuery(*query, mapping, dialect);
	INFO("plain SQL: " << plainSql);
	CHECK(plainSql.find("'http://ex.org/data/PROD/' || ") != std::string::npos);
	{
		std::unique_ptr<r2rml::SQLResultSet> rs = conn->execute(plainSql);
		auto rows = collectRows(*rs);
		REQUIRE(rows.size() == 1);
		CHECK(containsRow(rows, expected));
	}

	sparql2sql::TypeCatalog cat = catalogOf(*conn);
	REQUIRE(cat.comparable("PRODUCTS", "PROD_CODE", "ITEMS", "ITEM_PROD_CODE"));

	// With a catalog: despite the differently-named placeholder columns, the
	// join is emitted on the native columns instead of the constructed IRI
	// text, and the result set is unchanged.
	std::string nativeSql = translateQuery(*query, mapping, dialect, &cat);
	INFO("native SQL: " << nativeSql);
	CHECK(nativeSql.find("\"PROD_CODE\" = ") != std::string::npos);
	CHECK(nativeSql.find("\"ITEM_PROD_CODE\"") != std::string::npos);
	{
		std::unique_ptr<r2rml::SQLResultSet> rs = conn->execute(nativeSql);
		auto rows = collectRows(*rs);
		REQUIRE(rows.size() == 1);
		CHECK(containsRow(rows, expected));
	}
}

// --- FILTER pushdown: these queries must return identical results whether or
// not the optimizer re-parents the filter. They are the correctness oracle for
// the pushFilters pass (semantics preservation across every push target and
// boundary). ---

TEST_CASE("emp_dept_filter_folded.rq: a FILTER folded into the SPJ block selects the same row") {
	auto conn = makeSeededDatabase();
	auto rows = translateAndRun(*conn, "emp_dept_filter_folded.rq", "example_emp_dept.ttl");
	REQUIRE(rows.size() == 1);
	CHECK(containsRow(rows, {{"V_D", "http://data.example.com/department/10"}, {"V_LOC", "NEW YORK"}, {"V_S", "1"}}));
}

TEST_CASE("emp_dept_filter_keeps_mergeable.rq: folding across a merged block preserves the join result") {
	auto conn = makeSeededDatabase();
	auto rows = translateAndRun(*conn, "emp_dept_filter_keeps_mergeable.rq", "example_emp_dept.ttl");
	// Only SMITH has a department, and only department 10 is in NEW YORK.
	REQUIRE(rows.size() == 1);
	CHECK(containsRow(rows, {{"V_E", "http://data.example.com/employee/7369"}, {"V_LOC", "NEW YORK"}}));
}

TEST_CASE("emp_dept_filter_union.rq: FILTER distributed into union arms keeps only the matching row") {
	auto conn = makeSeededDatabase();
	auto rows = translateAndRun(*conn, "emp_dept_filter_union.rq", "example_emp_dept.ttl");
	REQUIRE(rows.size() == 1);
	CHECK(containsRow(rows, {{"V_D", "http://data.example.com/department/10"}, {"V_LOC", "NEW YORK"}}));
}

TEST_CASE("emp_dept_filter_join.rq: FILTER pushed onto the join side, union side still joins") {
	auto conn = makeSeededDatabase();
	auto rows = translateAndRun(*conn, "emp_dept_filter_join.rq", "example_emp_dept.ttl");
	// ?loc filtered to NEW YORK selects department 10, which inner-joins the
	// UNION arms: its name (APPSERVER) and its staff count (1).
	REQUIRE(rows.size() == 2);
	CHECK(containsRow(rows,
	                  {{"V_D", "http://data.example.com/department/10"}, {"V_LOC", "NEW YORK"}, {"V_X", "APPSERVER"}}));
	CHECK(containsRow(rows, {{"V_D", "http://data.example.com/department/10"}, {"V_LOC", "NEW YORK"}, {"V_X", "1"}}));
}

TEST_CASE("emp_dept_filter_optional.rq: FILTER on the optional var applies after the outer join") {
	auto conn = makeSeededDatabase();
	auto rows = translateAndRun(*conn, "emp_dept_filter_optional.rq", "example_emp_dept.ttl");
	// Only department 10 has LOC = NEW YORK; employees (loc unbound) and the
	// other department are dropped by the filter above the left outer join.
	REQUIRE(rows.size() == 1);
	CHECK(containsRow(rows,
	                  {{"V_E", "http://data.example.com/department/10"}, {"V_N", "APPSERVER"}, {"V_LOC", "NEW YORK"}}));
}

TEST_CASE("emp_dept_filter_optional_join.rq: FILTER on a var that's nullable via OPTIONAL on one join side but "
          "required on the other is not dropped early") {
	auto conn = makeSeededDatabase();
	auto rows = translateAndRun(*conn, "emp_dept_filter_optional_join.rq", "example_emp_dept.ttl");
	// ?loc is nullable coming out of the OPTIONAL (no Employee has ex:location),
	// but the required `?d2 ex:location ?loc` pattern re-binds it non-null via a
	// nullSafe join key. SMITH and JONES both have OPTIONAL-unbound ?loc, so
	// they're compatible with (and must join against) every ?d2 solution,
	// including department 10's NEW YORK; department 10 itself also directly
	// binds ?loc = NEW YORK. All three must survive the filter.
	REQUIRE(rows.size() == 3);
	CHECK(containsRow(rows, {{"V_E", "http://data.example.com/employee/7369"},
	                         {"V_N", "SMITH"},
	                         {"V_D2", "http://data.example.com/department/10"},
	                         {"V_LOC", "NEW YORK"}}));
	CHECK(containsRow(rows, {{"V_E", "http://data.example.com/employee/7400"},
	                         {"V_N", "JONES"},
	                         {"V_D2", "http://data.example.com/department/10"},
	                         {"V_LOC", "NEW YORK"}}));
	CHECK(containsRow(rows, {{"V_E", "http://data.example.com/department/10"},
	                         {"V_N", "APPSERVER"},
	                         {"V_D2", "http://data.example.com/department/10"},
	                         {"V_LOC", "NEW YORK"}}));
}

TEST_CASE("emp_dept_filter_exists.rq: EXISTS filter (kept above the union) keeps only rows with a staff triple") {
	auto conn = makeSeededDatabase();
	auto rows = translateAndRun(*conn, "emp_dept_filter_exists.rq", "example_emp_dept.ttl");
	// Both departments have a staff count (EXISTS true); employees have none.
	REQUIRE(rows.size() == 4);
	CHECK(containsRow(rows, {{"V_D", "http://data.example.com/department/10"}, {"V_LOC", "NEW YORK"}}));
	CHECK(containsRow(rows, {{"V_D", "http://data.example.com/department/20"}, {"V_LOC", "BOSTON"}}));
	CHECK(containsRow(rows, {{"V_D", "http://data.example.com/department/10"}, {"V_LOC", "APPSERVER"}}));
	CHECK(containsRow(rows, {{"V_D", "http://data.example.com/department/20"}, {"V_LOC", "SALES"}}));
}

TEST_CASE("emp_dept_filter_minus.rq: FILTER pushed onto the MINUS left side preserves anti-join semantics") {
	auto conn = makeSeededDatabase();
	auto rows = translateAndRun(*conn, "emp_dept_filter_minus.rq", "example_emp_dept.ttl");
	// Left side filtered to SMITH (employee 7369); SMITH has no ex:location
	// triple so MINUS keeps it.
	REQUIRE(rows.size() == 1);
	CHECK(containsRow(rows, {{"V_E", "http://data.example.com/employee/7369"}, {"V_N", "SMITH"}}));
}

// --- Property paths -------------------------------------------------------
//
// Every operator below is desugared into the relational algebra by
// PropertyPathTranslator; these cases are what actually pins down that the
// desugaring is semantically right, since test_runner can only inspect the
// shape of the generated SQL.

TEST_CASE("emp_dept_path_seq.rq: a sequence path walks employee -> department -> name") {
	auto conn = makeSeededDatabase();
	auto rows = translateAndRun(*conn, "emp_dept_path_seq.rq", "example_emp_dept.ttl");
	// Only SMITH has a department; JONES's DEPTNO is NULL so the first hop
	// drops it entirely.
	REQUIRE(rows.size() == 1);
	CHECK(containsRow(rows, {{"V_E", "http://data.example.com/employee/7369"}, {"V_DN", "APPSERVER"}}));
}

TEST_CASE("emp_dept_path_inverse.rq: an inverse path reverses subject and object") {
	auto conn = makeSeededDatabase();
	auto rows = translateAndRun(*conn, "emp_dept_path_inverse.rq", "example_emp_dept.ttl");
	REQUIRE(rows.size() == 1);
	CHECK(containsRow(
	    rows, {{"V_D", "http://data.example.com/department/10"}, {"V_E", "http://data.example.com/employee/7369"}}));
}

TEST_CASE("emp_dept_path_alt.rq: an alternative path returns both branches' objects") {
	auto conn = makeSeededDatabase();
	auto rows = translateAndRun(*conn, "emp_dept_path_alt.rq", "example_emp_dept.ttl");
	// ex:location and ex:staff, for both departments. STAFF is the view's
	// correlated employee count: 1 for APPSERVER (SMITH), 0 for SALES.
	REQUIRE(rows.size() == 4);
	CHECK(containsRow(rows, {{"V_D", "http://data.example.com/department/10"}, {"V_V", "NEW YORK"}}));
	CHECK(containsRow(rows, {{"V_D", "http://data.example.com/department/20"}, {"V_V", "BOSTON"}}));
	CHECK(containsRow(rows, {{"V_D", "http://data.example.com/department/10"}, {"V_V", "1"}}));
	CHECK(containsRow(rows, {{"V_D", "http://data.example.com/department/20"}, {"V_V", "0"}}));
}

TEST_CASE("emp_dept_path_nested.rq: a sequence of a forward and an inverse path finds departmental peers") {
	auto conn = makeSeededDatabase();
	auto rows = translateAndRun(*conn, "emp_dept_path_nested.rq", "example_emp_dept.ttl");
	// SMITH is the only employee in department 10, so the only peer SMITH has
	// is SMITH. (SPARQL does not exclude the reflexive pair here.)
	REQUIRE(rows.size() == 1);
	CHECK(containsRow(
	    rows, {{"V_E", "http://data.example.com/employee/7369"}, {"V_PEER", "http://data.example.com/employee/7369"}}));
}

TEST_CASE("emp_dept_path_nps.rq: a negated property set keeps every predicate it does not name") {
	auto conn = makeSeededDatabase();
	auto rows = translateAndRun(*conn, "emp_dept_path_nps.rq", "example_emp_dept.ttl");
	// !(ex:name|ex:staff) leaves rdf:type (from both rr:class assertions),
	// ex:location, ex:department, and SMITH's one ex:knows edge (JONES has a
	// NULL MGR, so contributes none).
	REQUIRE(rows.size() == 8);
	CHECK(containsRow(rows, {{"V_D", "http://data.example.com/department/10"}, {"V_V", "NEW YORK"}}));
	CHECK(containsRow(rows, {{"V_D", "http://data.example.com/department/20"}, {"V_V", "BOSTON"}}));
	CHECK(containsRow(rows,
	                  {{"V_D", "http://data.example.com/department/10"}, {"V_V", "http://example.com/ns#Department"}}));
	CHECK(containsRow(rows,
	                  {{"V_D", "http://data.example.com/employee/7369"}, {"V_V", "http://example.com/ns#Employee"}}));
	CHECK(containsRow(
	    rows, {{"V_D", "http://data.example.com/employee/7369"}, {"V_V", "http://data.example.com/department/10"}}));
	// The two excluded predicates contribute nothing.
	CHECK_FALSE(containsRow(rows, {{"V_D", "http://data.example.com/department/10"}, {"V_V", "APPSERVER"}}));
	CHECK_FALSE(containsRow(rows, {{"V_D", "http://data.example.com/department/10"}, {"V_V", "1"}}));
}

TEST_CASE("emp_dept_path_nps_inverse.rq: an all-inverse negated property set only matches reversed triples") {
	auto conn = makeSeededDatabase();
	auto rows = translateAndRun(*conn, "emp_dept_path_nps_inverse.rq", "example_emp_dept.ttl");
	// !(^ex:department) reverses every triple whose predicate is not
	// ex:department: 2 rdf:type + 2 ex:name for departments, 2 ex:location,
	// 2 ex:staff, 2 rdf:type + 2 ex:name for employees, plus SMITH's one
	// ex:knows edge (JONES has a NULL MGR, so contributes none).
	REQUIRE(rows.size() == 13);
	CHECK(containsRow(rows, {{"V_S", "APPSERVER"}, {"V_O", "http://data.example.com/department/10"}}));
	CHECK(containsRow(rows, {{"V_S", "SMITH"}, {"V_O", "http://data.example.com/employee/7369"}}));
	CHECK(containsRow(rows,
	                  {{"V_S", "http://example.com/ns#Employee"}, {"V_O", "http://data.example.com/employee/7369"}}));
	// The one excluded predicate: department/10 must never appear as a subject
	// of the reversed ex:department triple.
	CHECK_FALSE(containsRow(
	    rows, {{"V_S", "http://data.example.com/department/10"}, {"V_O", "http://data.example.com/employee/7369"}}));
}

TEST_CASE("emp_dept_path_zero_or_one.rq: a zero-or-one path matches both the zero-length and one-step routes") {
	auto conn = makeSeededDatabase();
	auto rows = translateAndRun(*conn, "emp_dept_path_zero_or_one.rq", "example_emp_dept.ttl");
	// <employee/7369> ex:department? ?d binds ?d to employee/7369 itself (zero
	// length) and to department/10 (one step); each then supplies its ex:name.
	REQUIRE(rows.size() == 2);
	CHECK(containsRow(rows, {{"V_N", "SMITH"}}));
	CHECK(containsRow(rows, {{"V_N", "APPSERVER"}}));
}

TEST_CASE("sparql2sql_path_terms.rq: a zero-length path with two unbound endpoints ranges over all graph terms") {
	auto conn = makeSeededDatabase();
	auto rows = translateAndRun(*conn, "sparql2sql_path_terms.rq", "sparql2sql_path_terms.ttl");
	// The term universe is the three node subjects (NODES has no rr:class, and
	// the referencing object map's objects are those same subjects), giving
	// three reflexive pairs; ex:next then adds the two real edges. Node 3's
	// NEXT is NULL, so it has no outgoing edge.
	REQUIRE(rows.size() == 5);
	CHECK(containsRow(rows, {{"V_X", "http://ex.org/node/1"}, {"V_Y", "http://ex.org/node/1"}}));
	CHECK(containsRow(rows, {{"V_X", "http://ex.org/node/2"}, {"V_Y", "http://ex.org/node/2"}}));
	CHECK(containsRow(rows, {{"V_X", "http://ex.org/node/3"}, {"V_Y", "http://ex.org/node/3"}}));
	CHECK(containsRow(rows, {{"V_X", "http://ex.org/node/1"}, {"V_Y", "http://ex.org/node/2"}}));
	CHECK(containsRow(rows, {{"V_X", "http://ex.org/node/2"}, {"V_Y", "http://ex.org/node/3"}}));
}

TEST_CASE("sparql2sql_path_terms_star.rq: a zero-or-more path over an acyclic chain adds genuinely new reflexive "
          "pairs") {
	// NODES is the acyclic 1 -> 2 -> 3 -> NULL chain (unlike the 3-cycle
	// below), so E+ alone never contains a reflexive pair here - this is the
	// test that actually demonstrates "E* includes zero-length" contributing
	// pairs the E+ closure doesn't already have. E+ over the chain gives
	// (1,2), (1,3), (2,3); the zero-length arm adds the three reflexive pairs.
	auto conn = makeSeededDatabase();
	auto rows = translateAndRun(*conn, "sparql2sql_path_terms_star.rq", "sparql2sql_path_terms.ttl");
	REQUIRE(rows.size() == 6);
	CHECK(containsRow(rows, {{"V_X", "http://ex.org/node/1"}, {"V_Y", "http://ex.org/node/1"}}));
	CHECK(containsRow(rows, {{"V_X", "http://ex.org/node/2"}, {"V_Y", "http://ex.org/node/2"}}));
	CHECK(containsRow(rows, {{"V_X", "http://ex.org/node/3"}, {"V_Y", "http://ex.org/node/3"}}));
	CHECK(containsRow(rows, {{"V_X", "http://ex.org/node/1"}, {"V_Y", "http://ex.org/node/2"}}));
	CHECK(containsRow(rows, {{"V_X", "http://ex.org/node/1"}, {"V_Y", "http://ex.org/node/3"}}));
	CHECK(containsRow(rows, {{"V_X", "http://ex.org/node/2"}, {"V_Y", "http://ex.org/node/3"}}));
}

TEST_CASE("sparql2sql_path_terms_plus_bound_subject.rq: a bound-subject one-or-more path over an acyclic chain "
          "excludes the starting node itself") {
	// Regression test: the unary reachable-set seed must start from the
	// anchor's one-hop successors, not the anchor itself. Node 1's chain
	// (1 -> 2 -> 3 -> NULL) never loops back, so node 1 must not appear in its
	// own E+ reachable set even though it's the seed.
	auto conn = makeSeededDatabase();
	auto rows = translateAndRun(*conn, "sparql2sql_path_terms_plus_bound_subject.rq", "sparql2sql_path_terms.ttl");
	REQUIRE(rows.size() == 2);
	CHECK(containsRow(rows, {{"V_Y", "http://ex.org/node/2"}}));
	CHECK(containsRow(rows, {{"V_Y", "http://ex.org/node/3"}}));
	CHECK_FALSE(containsRow(rows, {{"V_Y", "http://ex.org/node/1"}}));
}

TEST_CASE("sparql2sql_path_terms_plus_bound_object.rq: a bound-object one-or-more path over an acyclic chain "
          "excludes the ending node itself") {
	// Same regression, walked backward: node 3 is the anchor and must not
	// appear in its own reachable-from set.
	auto conn = makeSeededDatabase();
	auto rows = translateAndRun(*conn, "sparql2sql_path_terms_plus_bound_object.rq", "sparql2sql_path_terms.ttl");
	REQUIRE(rows.size() == 2);
	CHECK(containsRow(rows, {{"V_X", "http://ex.org/node/1"}}));
	CHECK(containsRow(rows, {{"V_X", "http://ex.org/node/2"}}));
	CHECK_FALSE(containsRow(rows, {{"V_X", "http://ex.org/node/3"}}));
}

TEST_CASE("sparql2sql_path_terms_plus_bound_both_self.rq: ASK is false for a node reaching itself with no cycle") {
	// Both endpoints bound to the same node in an acyclic graph: E+ requires
	// >=1 hop, and node 1 never loops back to itself, so this must be false -
	// not vacuously true from a zero-hop seed.
	auto conn = makeSeededDatabase();
	auto rows = translateAndRun(*conn, "sparql2sql_path_terms_plus_bound_both_self.rq", "sparql2sql_path_terms.ttl");
	REQUIRE(rows.size() == 1);
	CHECK(rows[0].at("ASK") == "false");
}

TEST_CASE("sparql2sql_path_cycle_plus.rq: a one-or-more path over a 3-cycle terminates with the full pairs closure") {
	// A hang here (rather than a failed assertion) would be the primary
	// cycle-safety failure mode - the recursive CTE's UNION (not UNION ALL)
	// must both terminate and dedup. Every node reaches every node in a
	// 3-cycle via >=1 hop, so the closure is the complete 3x3 = 9 pairs.
	auto conn = makeSeededDatabase();
	auto rows = translateAndRun(*conn, "sparql2sql_path_cycle_plus.rq", "sparql2sql_path_cycle.ttl");
	REQUIRE(rows.size() == 9);
	for (int i = 1; i <= 3; ++i) {
		for (int j = 1; j <= 3; ++j) {
			CHECK(containsRow(rows, {{"V_X", "http://ex.org/cycle/" + std::to_string(i)},
			                         {"V_Y", "http://ex.org/cycle/" + std::to_string(j)}}));
		}
	}
}

// ---------------------------------------------------------------------------
// Strict RDF-dataset semantics, on real rows. sparql2sql_graphs.ttl declares
// named graphs; a query with no GRAPH block must see ONLY the triples whose
// applicable graph set is empty or names rr:defaultGraph.
//
// The GRAPH-block side of this cannot be executed yet - fold() does not accept
// GraphGraphPattern until the next step - so these cover the default-graph half,
// which is exactly where the deliberate behaviour change lives.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// FROM / FROM NAMED on real rows. Both cases below return a *different* answer
// than the same pattern without the clause, so neither can pass by the dataset
// filter being inert.
// ---------------------------------------------------------------------------

TEST_CASE("dataset_from_g1_name.rq: FROM replaces the default graph, flipping which ex:name matches") {
	auto conn = makeSeededDatabase();
	auto rows = translateAndRun(*conn, "dataset_from_g1_name.rq", "sparql2sql_graphs.ttl");
	// Without the FROM clause this query returns the two DEPARTMENTS
	// (graphs_default_name.rq asserts exactly that). With it, the ungraphed
	// DeptMap triples leave the dataset and the g1 EmpMap ones enter.
	REQUIRE(rows.size() == 2);
	CHECK(containsRow(rows, {{"V_D", "http://data.example.com/employee/7369"}, {"V_N", "SMITH"}}));
	CHECK(containsRow(rows, {{"V_D", "http://data.example.com/employee/7400"}, {"V_N", "JONES"}}));
}

TEST_CASE("dataset_from_named_only.rq: FROM NAMED alone leaves the default graph empty") {
	auto conn = makeSeededDatabase();
	auto rows = translateAndRun(*conn, "dataset_from_named_only.rq", "sparql2sql_graphs.ttl");
	CHECK(rows.empty());
}

TEST_CASE("dataset_from_named_graph.rq: the same FROM NAMED graph is reachable inside a GRAPH block") {
	// The control for the case above: identical dataset clause, identical
	// triples, but named by a GRAPH block - so the emptiness there is the
	// default-graph rule and not the graph simply being unreachable.
	auto conn = makeSeededDatabase();
	auto rows = translateAndRun(*conn, "dataset_from_named_graph.rq", "sparql2sql_graphs.ttl");
	REQUIRE(rows.size() == 2);
	CHECK(containsRow(rows, {{"V_D", "http://data.example.com/department/10"}, {"V_L", "NEW YORK"}}));
	CHECK(containsRow(rows, {{"V_D", "http://data.example.com/department/20"}, {"V_L", "BOSTON"}}));
}

// ---------------------------------------------------------------------------
// The closure invariant column, on real rows. sparql2sql_graph_path.ttl puts
// each edge of the 1->2->3 chain in a DIFFERENT named graph, so a path
// evaluated within one graph can only be one hop - and a closure that failed to
// hold the graph constant would visibly return the two-hop pair.
// ---------------------------------------------------------------------------

TEST_CASE("graph_path_var_plus.rq: a path inside GRAPH ?g cannot hop between graphs") {
	auto conn = makeSeededDatabase();
	auto rows = translateAndRun(*conn, "graph_path_var_plus.rq", "sparql2sql_graph_path.ttl");
	// Exactly the two one-hop pairs, each in its own graph.
	REQUIRE(rows.size() == 2);
	CHECK(containsRow(
	    rows, {{"V_G", "http://ex.org/g/1"}, {"V_S", "http://ex.org/node/1"}, {"V_O", "http://ex.org/node/2"}}));
	CHECK(containsRow(
	    rows, {{"V_G", "http://ex.org/g/2"}, {"V_S", "http://ex.org/node/2"}, {"V_O", "http://ex.org/node/3"}}));
	// The row count above already excludes it, but say it outright: reaching
	// node/3 from node/1 needs two hops in two different graphs.
	for (const auto &row : rows) {
		auto s = row.find("V_S");
		auto o = row.find("V_O");
		bool crossGraph = s != row.end() && o != row.end() && s->second == "http://ex.org/node/1" &&
		                  o->second == "http://ex.org/node/3";
		CHECK_FALSE(crossGraph);
	}
}

TEST_CASE("graph_path_var_zero_or_one.rq: an unbound zero-length path enumerates each graph's own nodes") {
	// nodes(graph), per graph. sparql2sql_graph_path.ttl puts edge 1->2 in
	// <g/1> and edge 2->3 in <g/2>, so:
	//   <g/1> contains node/1 and node/2  -> pairs (1,2) plus (1,1) and (2,2)
	//   <g/2> contains node/2 and node/3  -> pairs (2,3) plus (2,2) and (3,3)
	// The reflexive pairs are per-graph: (3,3) must NOT appear under <g/1>,
	// which is what distinguishes a graph-aware term universe from the
	// mapping-wide one.
	auto conn = makeSeededDatabase();
	auto rows = translateAndRun(*conn, "graph_path_var_zero_or_one.rq", "sparql2sql_graph_path.ttl");
	REQUIRE(rows.size() == 6);

	const std::string g1 = "http://ex.org/g/1";
	const std::string g2 = "http://ex.org/g/2";
	auto node = [](int i) {
		return "http://ex.org/node/" + std::to_string(i);
	};

	CHECK(containsRow(rows, {{"V_G", g1}, {"V_S", node(1)}, {"V_O", node(2)}}));
	CHECK(containsRow(rows, {{"V_G", g1}, {"V_S", node(1)}, {"V_O", node(1)}}));
	CHECK(containsRow(rows, {{"V_G", g1}, {"V_S", node(2)}, {"V_O", node(2)}}));
	CHECK(containsRow(rows, {{"V_G", g2}, {"V_S", node(2)}, {"V_O", node(3)}}));
	CHECK(containsRow(rows, {{"V_G", g2}, {"V_S", node(2)}, {"V_O", node(2)}}));
	CHECK(containsRow(rows, {{"V_G", g2}, {"V_S", node(3)}, {"V_O", node(3)}}));

	// node/3 is not a node of <g/1>, and node/1 is not a node of <g/2>.
	CHECK_FALSE(containsRow(rows, {{"V_G", g1}, {"V_S", node(3)}, {"V_O", node(3)}}));
	CHECK_FALSE(containsRow(rows, {{"V_G", g2}, {"V_S", node(1)}, {"V_O", node(1)}}));
}

TEST_CASE("graph_path_var_plus_bound_both.rq: both endpoints bound under GRAPH ?g binds the connecting graph") {
	// Mode::BothBound is no longer a pure existence check: the answer is which
	// graphs connect the endpoints, so the graph is projected.
	auto conn = makeSeededDatabase();
	auto rows = translateAndRun(*conn, "graph_path_var_plus_bound_both.rq", "sparql2sql_graph_path.ttl");
	REQUIRE(rows.size() == 1);
	CHECK(containsRow(rows, {{"V_G", "http://ex.org/g/1"}}));
}

TEST_CASE("graph_path_var_plus_cross_graph_absent.rq: the cross-graph two-hop pair has no answer") {
	// The negative half of the pair above, asked directly. Without the invariant
	// this returns a graph; with it, nothing.
	auto conn = makeSeededDatabase();
	auto rows = translateAndRun(*conn, "graph_path_var_plus_cross_graph_absent.rq", "sparql2sql_graph_path.ttl");
	CHECK(rows.empty());
}

TEST_CASE("graph_iri_location.rq: naming the graph finds the triples the default graph cannot see") {
	// The mirror of graphs_default_location.rq below, which finds nothing. Both
	// directions are asserted so neither can pass by the filter being inert.
	auto conn = makeSeededDatabase();
	auto rows = translateAndRun(*conn, "graph_iri_location.rq", "sparql2sql_graphs.ttl");
	REQUIRE(rows.size() == 2);
	CHECK(containsRow(rows, {{"V_D", "http://data.example.com/department/10"}, {"V_L", "NEW YORK"}}));
	CHECK(containsRow(rows, {{"V_D", "http://data.example.com/department/20"}, {"V_L", "BOSTON"}}));
}

TEST_CASE("graph_var_staff.rq: GRAPH ?g binds the graph name alongside the pattern's own variables") {
	auto conn = makeSeededDatabase();
	auto rows = translateAndRun(*conn, "graph_var_staff.rq", "sparql2sql_graphs.ttl");
	// ex:staff's set is {<graph/g1>, rr:defaultGraph}; only the named member is
	// a GRAPH solution, so each department appears exactly once, with ?g bound.
	REQUIRE(rows.size() == 2);
	CHECK(containsRow(
	    rows,
	    {{"V_G", "http://example.com/graph/g1"}, {"V_D", "http://data.example.com/department/10"}, {"V_S", "10"}}));
	CHECK(containsRow(
	    rows,
	    {{"V_G", "http://example.com/graph/g1"}, {"V_D", "http://data.example.com/department/20"}, {"V_S", "20"}}));
}

TEST_CASE("graph_var_dept.rq: a two-member graph set yields one solution per graph") {
	// The reason enumeration emits one branch per graph rather than one branch
	// with a disjunction: SMITH's ex:dept triple is in BOTH <graph/g1> and
	// <graph/dept/10>, so it must appear twice under GRAPH ?g with different ?g.
	// (JONES has a NULL DEPTNO, so the template yields no triple at all.)
	auto conn = makeSeededDatabase();
	auto rows = translateAndRun(*conn, "graph_var_dept.rq", "sparql2sql_graphs.ttl");
	REQUIRE(rows.size() == 2);
	CHECK(
	    containsRow(rows, {{"V_G", "http://example.com/graph/g1"}, {"V_E", "http://data.example.com/employee/7369"}}));
	CHECK(containsRow(rows,
	                  {{"V_G", "http://example.com/graph/dept/10"}, {"V_E", "http://data.example.com/employee/7369"}}));
}

TEST_CASE("graph_iri_path_plus.rq: an arbitrary-length path inside a NAMED graph executes") {
	// A bound graph IRI needs no invariant column through the closure - its
	// condition is inside the step relation. The graph-VARIABLE form is refused
	// instead (see the structural refusal tests); this is the supported half.
	auto conn = makeSeededDatabase();
	auto rows = translateAndRun(*conn, "graph_iri_path_plus.rq", "sparql2sql_graphs.ttl");
	// ex:dept in <graph/g1> is employee/7369 -> department/10, one hop, no
	// further edge out of a department, so the closure is that single pair.
	REQUIRE(rows.size() == 1);
	CHECK(containsRow(
	    rows, {{"V_S", "http://data.example.com/employee/7369"}, {"V_O", "http://data.example.com/department/10"}}));
}

TEST_CASE("graphs_default_name.rq: a predicate mapped into a named graph is invisible in the default graph") {
	auto conn = makeSeededDatabase();
	auto rows = translateAndRun(*conn, "graphs_default_name.rq", "sparql2sql_graphs.ttl");
	// DeptMap's ex:name only. EmpMap's is in <graph/g1> via its subject map, so
	// SMITH and JONES must be absent - that is the strict-semantics change.
	REQUIRE(rows.size() == 2);
	CHECK(containsRow(rows, {{"V_D", "http://data.example.com/department/10"}, {"V_N", "APPSERVER"}}));
	CHECK(containsRow(rows, {{"V_D", "http://data.example.com/department/20"}, {"V_N", "SALES"}}));
}

TEST_CASE("graphs_default_location.rq: a wholly named-graph predicate yields no rows in the default graph") {
	auto conn = makeSeededDatabase();
	auto rows = translateAndRun(*conn, "graphs_default_location.rq", "sparql2sql_graphs.ttl");
	CHECK(rows.empty());
}

TEST_CASE("graphs_default_staff.rq: rr:defaultGraph keeps a named-graph predicate visible in the default graph") {
	auto conn = makeSeededDatabase();
	auto rows = translateAndRun(*conn, "graphs_default_staff.rq", "sparql2sql_graphs.ttl");
	// ex:staff declares <graph/g1> AND rr:defaultGraph. Contrast with
	// ex:location above, which declares only the named graph - so this is not
	// passing merely because the graph filter is inert.
	REQUIRE(rows.size() == 2);
	CHECK(containsRow(rows, {{"V_D", "http://data.example.com/department/10"}, {"V_S", "10"}}));
	CHECK(containsRow(rows, {{"V_D", "http://data.example.com/department/20"}, {"V_S", "20"}}));
}

TEST_CASE("graphs_default_class.rq: rr:class triples follow the subject map's graphs, not the POM's") {
	auto conn = makeSeededDatabase();
	auto rows = translateAndRun(*conn, "graphs_default_class.rq", "sparql2sql_graphs.ttl");
	// DeptMap has no subject-level graph, so its rdf:type is in the default
	// graph; EmpMap's subject map has rr:graph, so its rdf:type is not.
	REQUIRE(rows.size() == 2);
	CHECK(containsRow(rows,
	                  {{"V_S", "http://data.example.com/department/10"}, {"V_C", "http://example.com/ns#Department"}}));
	CHECK(containsRow(rows,
	                  {{"V_S", "http://data.example.com/department/20"}, {"V_C", "http://example.com/ns#Department"}}));
}

TEST_CASE("sparql2sql_path_cycle_plus_same_var.rq: the same variable on both ends projects the closure diagonal") {
	// Guards renderTransitiveClosure's diagonal projection, which is selected by
	// TransitiveClosureNode::sameEndpointVar. That decision used to be inferred
	// from schema().size() == 1, which held only because the node's projected
	// width is fully determined by its mode - so adding any column to a closure
	// (e.g. a carried named-graph invariant) would silently emit the
	// two-endpoint form here, dropping the diagonal WHERE and returning the
	// wrong rows. This pins the correct answer so that can't happen unnoticed.
	//
	// The sibling test above shows the *pairs* closure over this same 3-cycle is
	// all 9 combinations; the diagonal of that is exactly the 3 nodes.
	auto conn = makeSeededDatabase();
	auto rows = translateAndRun(*conn, "sparql2sql_path_cycle_plus_same_var.rq", "sparql2sql_path_cycle.ttl");
	REQUIRE(rows.size() == 3);
	for (int i = 1; i <= 3; ++i) {
		CHECK(containsRow(rows, {{"V_X", "http://ex.org/cycle/" + std::to_string(i)}}));
	}
}

TEST_CASE("sparql2sql_path_cycle_star.rq: a zero-or-more path over a 3-cycle dedups against the already-reflexive "
          "E+ closure") {
	// E* = E+ union zero-length, but in a full cycle E+ already contains every
	// reflexive (x, x) pair (each node reaches itself by going all the way
	// around), so the union with the zero-length arm's own reflexive pairs
	// contributes nothing new: still exactly the same 9 pairs as the plain E+
	// case, proving the union's dedup (not the recursive term's) removes the
	// overlap correctly rather than double-counting it.
	auto conn = makeSeededDatabase();
	auto rows = translateAndRun(*conn, "sparql2sql_path_cycle_star.rq", "sparql2sql_path_cycle.ttl");
	REQUIRE(rows.size() == 9);
	for (int i = 1; i <= 3; ++i) {
		CHECK(containsRow(rows, {{"V_X", "http://ex.org/cycle/" + std::to_string(i)},
		                         {"V_Y", "http://ex.org/cycle/" + std::to_string(i)}}));
	}
}

TEST_CASE("sparql2sql_path_cycle_plus_forward.rq: a bound-subject one-or-more path over a 3-cycle reaches every "
          "node") {
	// Directional seeding from the bound subject: a unary reachable-set, not
	// a pairs closure. All three nodes are reachable from node 1 in a 3-cycle.
	auto conn = makeSeededDatabase();
	auto rows = translateAndRun(*conn, "sparql2sql_path_cycle_plus_forward.rq", "sparql2sql_path_cycle.ttl");
	REQUIRE(rows.size() == 3);
	CHECK(containsRow(rows, {{"V_Y", "http://ex.org/cycle/1"}}));
	CHECK(containsRow(rows, {{"V_Y", "http://ex.org/cycle/2"}}));
	CHECK(containsRow(rows, {{"V_Y", "http://ex.org/cycle/3"}}));
}

TEST_CASE("sparql2sql_path_cycle_plus_backward.rq: a bound-object one-or-more path over a 3-cycle is reached from "
          "every node") {
	// Directional seeding from the bound object, walking the edge relation
	// backward - still a unary reachable-set.
	auto conn = makeSeededDatabase();
	auto rows = translateAndRun(*conn, "sparql2sql_path_cycle_plus_backward.rq", "sparql2sql_path_cycle.ttl");
	REQUIRE(rows.size() == 3);
	CHECK(containsRow(rows, {{"V_X", "http://ex.org/cycle/1"}}));
	CHECK(containsRow(rows, {{"V_X", "http://ex.org/cycle/2"}}));
	CHECK(containsRow(rows, {{"V_X", "http://ex.org/cycle/3"}}));
}

TEST_CASE("sparql2sql_path_cycle_plus_bound_both.rq: ASK is true for a node reaching itself around a cycle") {
	// Both endpoints bound to the same node: E+ requires >=1 hop, and node 1
	// does reach itself after going all the way around the 3-cycle.
	auto conn = makeSeededDatabase();
	auto rows = translateAndRun(*conn, "sparql2sql_path_cycle_plus_bound_both.rq", "sparql2sql_path_cycle.ttl");
	REQUIRE(rows.size() == 1);
	CHECK(rows[0].at("ASK") == "true");
}

TEST_CASE("emp_dept_path_star_select.rq: SELECT * over a sequence path omits the intermediate variable") {
	auto conn = makeSeededDatabase();
	auto rows = translateAndRun(*conn, "emp_dept_path_star_select.rq", "example_emp_dept.ttl");
	REQUIRE(rows.size() == 1);
	// Exactly the two query variables - the minted midpoint is not a column.
	REQUIRE(rows[0].size() == 2);
	CHECK(containsRow(rows, {{"V_E", "http://data.example.com/employee/7369"}, {"V_DN", "APPSERVER"}}));
}

TEST_CASE("emp_dept_bnode_select_star.rq: SELECT * omits a blank-node position") {
	auto conn = makeSeededDatabase();
	auto rows = translateAndRun(*conn, "emp_dept_bnode_select_star.rq", "example_emp_dept.ttl");
	REQUIRE(rows.size() == 4);
	// ?e only: the blank node constrains the pattern but is not a variable.
	REQUIRE(rows[0].size() == 1);
	CHECK(containsRow(rows, {{"V_E", "http://data.example.com/employee/7369"}}));
	CHECK(containsRow(rows, {{"V_E", "http://data.example.com/department/10"}}));
}

// --- Static RDF term dimension: term-kind builtins and typed operators ---
//
// A lattice error produces *plausible* SQL, so this is the only place it can be
// caught. Every folded builtin and every typed operator therefore gets a row
// assertion, not just the interesting ones. The MEASURES data is chosen so
// lexicographic and numeric answers differ, which is what stops these passing
// vacuously if the typing silently stops working.

TEST_CASE("sparql2sql_term_kinds.rq: term-kind builtins fold to per-term-map constants") {
	auto conn = makeSeededDatabase();
	auto rows = translateAndRun(*conn, "sparql2sql_term_kinds.rq", "sparql2sql_terms.ttl");
	REQUIRE(rows.size() == 2);
	// ?m is a template IRI and ?t an rr:language literal, in every candidate.
	CHECK(containsRow(rows, {{"V_M", "http://ex.org/m/1"}, {"V_II", "true"}, {"V_IB", "false"}, {"V_IL", "true"}}));
	CHECK(containsRow(rows, {{"V_M", "http://ex.org/m/2"}, {"V_II", "true"}, {"V_IB", "false"}, {"V_IL", "true"}}));
}

TEST_CASE("sparql2sql_bnode_kind.rq: an rr:BlankNode subject satisfies isBLANK") {
	auto conn = makeSeededDatabase();
	auto rows = translateAndRun(*conn, "sparql2sql_bnode_kind.rq", "sparql2sql_terms.ttl");
	REQUIRE(rows.size() == 2);
	CHECK(containsRow(rows, {{"V_B", "note one"}}));
	CHECK(containsRow(rows, {{"V_B", "9"}}));
}

TEST_CASE("sparql2sql_term_lang.rq: langMatches against a static rr:language tag") {
	auto conn = makeSeededDatabase();
	auto rows = translateAndRun(*conn, "sparql2sql_term_lang.rq", "sparql2sql_terms.ttl");
	// Both titles are tagged @en by the mapping, so both survive.
	REQUIRE(rows.size() == 2);
	CHECK(containsRow(rows, {{"V_T", "alpha"}}));
	CHECK(containsRow(rows, {{"V_T", "beta"}}));
}

TEST_CASE("sparql2sql_term_datatype.rq: DATATYPE folds to the declared rr:datatype IRI") {
	auto conn = makeSeededDatabase();
	auto rows = translateAndRun(*conn, "sparql2sql_term_datatype.rq", "sparql2sql_terms.ttl");
	REQUIRE(rows.size() == 2);
	CHECK(containsRow(rows, {{"V_M", "http://ex.org/m/1"}, {"V_DT", "http://www.w3.org/2001/XMLSchema#integer"}}));
	CHECK(containsRow(rows, {{"V_M", "http://ex.org/m/2"}, {"V_DT", "http://www.w3.org/2001/XMLSchema#integer"}}));
}

TEST_CASE("sparql2sql_typed_filter.rq: a declared xsd:integer filters numerically, not lexicographically") {
	auto conn = makeSeededDatabase();
	auto rows = translateAndRun(*conn, "sparql2sql_typed_filter.rq", "sparql2sql_terms.ttl");
	// ?a > 50 over {9, 100}: numerically only 100 qualifies. Lexicographically
	// '9' > '50' and '100' < '50', so an untyped comparison would return the
	// other row entirely - this assertion fails loudly if the typing regresses.
	REQUIRE(rows.size() == 1);
	CHECK(containsRow(rows, {{"V_M", "http://ex.org/m/2"}}));
}

TEST_CASE("sparql2sql_typed_order.rq: ORDER BY on a declared xsd:integer sorts numerically") {
	auto conn = makeSeededDatabase();
	auto rows = translateAndRun(*conn, "sparql2sql_typed_order.rq", "sparql2sql_terms.ttl");
	REQUIRE(rows.size() == 2);
	// Lexicographically this would be {"100", "9"}.
	CHECK(columnSeq(rows, "V_A") == std::vector<std::string>({"9", "100"}));
}

TEST_CASE("sparql2sql_typed_minmax.rq: MIN/MAX on a declared xsd:integer aggregate numerically") {
	auto conn = makeSeededDatabase();
	auto rows = translateAndRun(*conn, "sparql2sql_typed_minmax.rq", "sparql2sql_terms.ttl");
	REQUIRE(rows.size() == 1);
	// Lexicographically MIN would be "100" and MAX "9".
	CHECK(containsRow(rows, {{"V_MN", "9"}, {"V_MX", "100"}}));
}

TEST_CASE("sparql2sql_typed_arith.rq: integral arithmetic keeps an integral lexical form") {
	auto conn = makeSeededDatabase();
	auto rows = translateAndRun(*conn, "sparql2sql_typed_arith.rq", "sparql2sql_terms.ttl");
	REQUIRE(rows.size() == 2);
	// Not "10.0"/"101.0", which is what the DOUBLE round-trip used to produce.
	CHECK(containsRow(rows, {{"V_S", "10"}}));
	CHECK(containsRow(rows, {{"V_S", "101"}}));
}

TEST_CASE("sparql2sql_typed_datetime.rq: a declared xsd:dateTime compares as a timestamp") {
	auto conn = makeSeededDatabase();
	auto rows = translateAndRun(*conn, "sparql2sql_typed_datetime.rq", "sparql2sql_terms.ttl");
	REQUIRE(rows.size() == 1);
	CHECK(containsRow(rows, {{"V_M", "http://ex.org/m/1"}}));
}

TEST_CASE("sparql2sql_strdt.rq: STRDT types an otherwise-untyped column for comparison") {
	auto conn = makeSeededDatabase();
	auto rows = translateAndRun(*conn, "sparql2sql_strdt.rq", "sparql2sql_terms.ttl");
	// ex:rawamount declares no rr:datatype, so STRDT is the only thing making
	// this numeric. Same {9, 100} vs 50 discrimination as the typed filter.
	REQUIRE(rows.size() == 1);
	CHECK(containsRow(rows, {{"V_M", "http://ex.org/m/2"}}));
}

TEST_CASE("sparql2sql_term_disagree.rq: a term-kind filter over disagreeing arms resolves per arm") {
	auto conn = makeSeededDatabase();
	auto rows = translateAndRun(*conn, "sparql2sql_term_disagree.rq", "sparql2sql_terms.ttl");
	// ex:mixed is an IRI from <#Measure> and a literal from <#Note>. Pushdown
	// folds the IRI arm away entirely, leaving only the NOTES rows - so this is
	// still resolved statically, per arm, and materialises no runtime tag at all.
	REQUIRE(rows.size() == 2);
	CHECK(containsRow(rows, {{"V_X", "note one"}}));
	CHECK(containsRow(rows, {{"V_X", "9"}}));
}

TEST_CASE("sparql2sql_badtyped.rq: a mis-declared rr:datatype drops rows rather than raising") {
	auto conn = makeSeededDatabase();
	// ex:badtyped declares xsd:integer over the VARCHAR TITLE column ('alpha',
	// 'beta'). This pins the decision to keep TRY_CAST rather than CAST in every
	// typed branch: a declared datatype the data contradicts must bound the
	// damage to "fewer rows", never "the whole query errors out".
	CHECK_NOTHROW(translateAndRun(*conn, "sparql2sql_badtyped.rq", "sparql2sql_terms.ttl"));
	auto rows = translateAndRun(*conn, "sparql2sql_badtyped.rq", "sparql2sql_terms.ttl");
	CHECK(rows.empty());
}

TEST_CASE("sparql2sql_count_order.rq: ORDER BY over a COUNT alias sorts numerically") {
	auto conn = makeSeededDatabase();
	auto rows = translateAndRun(*conn, "sparql2sql_count_order.rq", "sparql2sql_count_order.ttl");
	REQUIRE(rows.size() == 2);
	// 11 'a' rows vs 2 'b' rows, ordered DESC by count. Lexicographically "11"
	// sorts below "2", so the pre-fix bare-alias ORDER BY inverted this.
	CHECK(columnSeq(rows, "V_G") == std::vector<std::string>({"a", "b"}));
	CHECK(columnSeq(rows, "V_C") == std::vector<std::string>({"11", "2"}));
}

TEST_CASE("sparql2sql_view_types.rq: describing rr:sqlQuery views makes DATATYPE() answerable") {
	auto conn = makeSeededDatabase();
	Parser parser;
	R2RMLParser mappingParser;
	R2RMLMapping mapping = mappingParser.parse(SOURCE_R2RML_DIR "sparql2sql_view_types.ttl");
	REQUIRE(mapping.isValid());
	DuckDbDialect dialect;

	// The originally reported failure: a catalog read only from information_schema
	// types EMP.ENAME but not the view's DNAME, and one untyped candidate arm
	// degrades the meet, so DATATYPE() had no single static answer and refused.
	//
	// It no longer refuses. The two arms carry different runtime type tags, so
	// DATATYPE() is decided per row: the typed EMP arm reports xsd:string and the
	// still-untyped view arms report a type error (SQL NULL, hence an unbound ?dt),
	// which is a strictly better outcome than failing the whole query.
	auto query = parser.parseFile(SOURCE_SPARQL2SQL_DIR "sparql2sql_view_types.rq");
	sparql2sql::TypeCatalog baseOnly = catalogOf(*conn);
	REQUIRE_FALSE(baseOnly.typeOf("EMP", "ENAME").empty());
	REQUIRE(baseOnly.typeOf("view:SELECT DEPTNO, DNAME FROM DEPT", "DNAME").empty());
	{
		std::string partialSql = translateQuery(*query, mapping, dialect, &baseOnly);
		INFO("SQL (base-only catalog): " << partialSql);
		std::unique_ptr<r2rml::SQLResultSet> partialRs = conn->execute(partialSql);
		auto partialRows = collectRows(*partialRs);
		REQUIRE(partialRows.size() == 4);
		CHECK(containsRow(partialRows, {{"V_S", "http://data.example.com/emp/7369"},
		                                {"V_L", "SMITH"},
		                                {"V_DT", "http://www.w3.org/2001/XMLSchema#string"}}));
		CHECK(containsRow(partialRows,
		                  {{"V_S", "http://data.example.com/dept/10"}, {"V_L", "APPSERVER"}, {"V_DT", kNull}}));
	}

	// The fix: loadTypeCatalog also describes each rr:sqlQuery view and files
	// its result columns under the view's identity.
	sparql2sql::TypeCatalog cat;
	sql2rdf::loadTypeCatalog(*conn, &mapping, cat);
	INFO("typeOf(dept view, DNAME)='" << cat.typeOf("view:SELECT DEPTNO, DNAME FROM DEPT", "DNAME") << "'");
	REQUIRE_FALSE(cat.typeOf("view:SELECT DEPTNO, DNAME FROM DEPT", "DNAME").empty());
	// A computed column exists in no information_schema at all, so describing
	// the query is the only way to learn it is an integer.
	REQUIRE(sparql2sql::naturalXsdDatatype(cat.typeOf("view:SELECT EMPNO, LENGTH(ENAME) AS NAMELEN FROM EMP",
	                                                  "NAMELEN")) == std::string(sparql2sql::xsd::kInteger));

	std::string sql = translateQuery(*query, mapping, dialect, &cat);
	INFO("SQL: " << sql);
	std::unique_ptr<r2rml::SQLResultSet> rs = conn->execute(sql);
	auto rows = collectRows(*rs);
	const char *const kXsdString = "http://www.w3.org/2001/XMLSchema#string";
	REQUIRE(rows.size() == 4);
	// Both the base-table arm and the view arm report a datatype now.
	CHECK(containsRow(rows, {{"V_S", "http://data.example.com/emp/7369"}, {"V_L", "SMITH"}, {"V_DT", kXsdString}}));
	CHECK(containsRow(rows, {{"V_S", "http://data.example.com/dept/10"}, {"V_L", "APPSERVER"}, {"V_DT", kXsdString}}));
	CHECK(containsRow(rows, {{"V_S", "http://data.example.com/dept/20"}, {"V_L", "SALES"}, {"V_DT", kXsdString}}));
}

TEST_CASE("sparql2sql_view_types_computed.rq: a computed view column's datatype comes from DESCRIBE") {
	auto conn = makeSeededDatabase();
	Parser parser;
	auto query = parser.parseFile(SOURCE_SPARQL2SQL_DIR "sparql2sql_view_types_computed.rq");
	R2RMLParser mappingParser;
	R2RMLMapping mapping = mappingParser.parse(SOURCE_R2RML_DIR "sparql2sql_view_types.ttl");
	REQUIRE(mapping.isValid());
	DuckDbDialect dialect;

	sparql2sql::TypeCatalog cat;
	sql2rdf::loadTypeCatalog(*conn, &mapping, cat);
	std::string sql = translateQuery(*query, mapping, dialect, &cat);
	INFO("SQL: " << sql);
	std::unique_ptr<r2rml::SQLResultSet> rs = conn->execute(sql);
	auto rows = collectRows(*rs);
	const char *const kXsdInteger = "http://www.w3.org/2001/XMLSchema#integer";
	REQUIRE(rows.size() == 2);
	CHECK(containsRow(rows, {{"V_S", "http://data.example.com/emp/7369"}, {"V_N", "5"}, {"V_DT", kXsdInteger}}));
	CHECK(containsRow(rows, {{"V_S", "http://data.example.com/emp/7400"}, {"V_N", "5"}, {"V_DT", kXsdInteger}}));
}

// ---------------------------------------------------------------------------
// Dynamic RDF term dimension: the runtime type tag, proved on real rows.
//
// The static cases above cover everything the R2RML mapping determines. These
// cover what it cannot: the four multi-arm predicates in sparql2sql_terms.ttl,
// whose two triples maps produce terms of DIFFERENT dimensions over different
// tables, so a single query really does return rows whose term kind / datatype /
// language differ from row to row.
//
// Every query here routes the value through a BIND before inspecting it. That is
// load-bearing, not stylistic: without it, filter pushdown distributes the
// predicate into each union arm where the dimension IS statically known and folds
// it, so nothing would be decided at run time and these tests would prove
// nothing. (That per-arm resolution is itself asserted by
// sparql2sql_term_disagree.rq above.)
//
// NOTES.BODY = '9' and MEASURES.AMOUNT = 9 are the crux: same lexical form,
// different datatype. Any assertion below that distinguishes them is one a
// lexical-form-only representation gets wrong.
// ---------------------------------------------------------------------------

TEST_CASE("dyn_term_kinds.rq: isIRI/isLITERAL differ per row over a kind-disagreeing predicate") {
	auto conn = makeSeededDatabase();
	auto rows = translateAndRun(*conn, "dyn_term_kinds.rq", "sparql2sql_terms.ttl");
	REQUIRE(rows.size() == 4);
	// The <#Measure> arm builds an IRI from rr:template "http://ex.org/m/{REF}"...
	CHECK(containsRow(rows, {{"V_Y", "http://ex.org/m/2"}, {"V_II", "true"}, {"V_IL", "false"}}));
	CHECK(containsRow(rows, {{"V_Y", "http://ex.org/m/1"}, {"V_II", "true"}, {"V_IL", "false"}}));
	// ...and the <#Note> arm a plain literal from NOTES.BODY. Same query, same
	// column, opposite answers.
	CHECK(containsRow(rows, {{"V_Y", "note one"}, {"V_II", "false"}, {"V_IL", "true"}}));
	CHECK(containsRow(rows, {{"V_Y", "9"}, {"V_II", "false"}, {"V_IL", "true"}}));
}

TEST_CASE("dyn_datatype.rq: DATATYPE reports each row's own declared datatype") {
	auto conn = makeSeededDatabase();
	auto rows = translateAndRun(*conn, "dyn_datatype.rq", "sparql2sql_terms.ttl");
	const char *const kInt = "http://www.w3.org/2001/XMLSchema#integer";
	const char *const kStr = "http://www.w3.org/2001/XMLSchema#string";
	REQUIRE(rows.size() == 4);
	CHECK(containsRow(rows, {{"V_Y", "9"}, {"V_DT", kInt}}));   // MEASURES.AMOUNT
	CHECK(containsRow(rows, {{"V_Y", "100"}, {"V_DT", kInt}})); // MEASURES.AMOUNT
	CHECK(containsRow(rows, {{"V_Y", "note one"}, {"V_DT", kStr}}));
	// The collision: identical lexical form, different datatype. Two rows.
	CHECK(containsRow(rows, {{"V_Y", "9"}, {"V_DT", kStr}}));
}

TEST_CASE("dyn_lang.rq: LANG reports each row's own tag, and '' only where there really is none") {
	auto conn = makeSeededDatabase();
	auto rows = translateAndRun(*conn, "dyn_lang.rq", "sparql2sql_terms.ttl");
	REQUIRE(rows.size() == 4);
	// ex:mixedtag is rr:language "en" over MEASURES.TITLE...
	CHECK(containsRow(rows, {{"V_Y", "alpha"}, {"V_LG", "en"}}));
	CHECK(containsRow(rows, {{"V_Y", "beta"}, {"V_LG", "en"}}));
	// ...and untagged over NOTES.BODY. This is the regression the old
	// TermInfo::maybeLangTagged refusal was protecting: folding '' for every row
	// would have silently dropped the two @en tags above.
	CHECK(containsRow(rows, {{"V_Y", "note one"}, {"V_LG", ""}}));
	CHECK(containsRow(rows, {{"V_Y", "9"}, {"V_LG", ""}}));
}

TEST_CASE("dyn_sameterm.rq: sameTerm rejects a lexical match across datatypes") {
	auto conn = makeSeededDatabase();
	auto rows = translateAndRun(*conn, "dyn_sameterm.rq", "sparql2sql_terms.ttl");
	// One row pairs "9"^^xsd:string (NOTES.BODY) with "9"^^xsd:integer
	// (MEASURES.AMOUNT). Plain string equality - what this translator did before
	// the tag existed - answers true; RDF term equality must answer false.
	bool sawTheCollision = false;
	for (const auto &row : rows) {
		auto p = row.find("V_P");
		auto q = row.find("V_Q");
		auto same = row.find("V_SAME");
		REQUIRE(p != row.end());
		REQUIRE(q != row.end());
		REQUIRE(same != row.end());
		if (p->second == "9" && q->second == "9") {
			// Either both are the xsd:integer arm (genuinely the same term) or this
			// is the cross-datatype pair. Only the former may be true.
			sawTheCollision = sawTheCollision || same->second == "false";
		}
		// Different lexical forms are never the same term, whatever their tags.
		if (p->second != q->second) {
			CHECK(same->second == "false");
		}
	}
	CHECK(sawTheCollision);
}

TEST_CASE("dyn_compare_incomparable.rq: an ordering comparison across value spaces drops the row") {
	auto conn = makeSeededDatabase();
	auto rows = translateAndRun(*conn, "dyn_compare_incomparable.rq", "sparql2sql_terms.ttl");
	// Section 17.3 defines < only within a value space, so an xsd:string ?p
	// against an xsd:integer ?q is a type error and the row is dropped. Only the
	// xsd:integer rows of ex:mixeddt can survive, and only where 9 < 100.
	for (const auto &row : rows) {
		auto p = row.find("V_P");
		auto q = row.find("V_Q");
		REQUIRE(p != row.end());
		REQUIRE(q != row.end());
		INFO("surviving pair: " << p->second << " < " << q->second);
		// Every surviving pair must be numerically ordered - which also rules out
		// the lexicographic answer ('100' < '9' as text).
		CHECK(std::stod(p->second) < std::stod(q->second));
	}
	CHECK(containsRow(rows, {{"V_P", "9"}, {"V_Q", "100"}}));
	// 'note one' has no numeric reading and is an xsd:string besides, so it can
	// never appear.
	CHECK_FALSE(containsRow(rows, {{"V_P", "note one"}, {"V_Q", "9"}}));
	CHECK_FALSE(containsRow(rows, {{"V_P", "note one"}, {"V_Q", "100"}}));
}

TEST_CASE("dyn_distinct_types.rq: DISTINCT keeps two terms with one lexical form but two datatypes") {
	auto conn = makeSeededDatabase();
	auto rows = translateAndRun(*conn, "dyn_distinct_types.rq", "sparql2sql_terms.ttl");
	// Four distinct RDF terms: 9 and 100 as xsd:integer, 'note one' and 9 as
	// xsd:string. Deduplicating on the lexical form alone would give three.
	CHECK(rows.size() == 4);
	int nines = 0;
	for (const auto &row : rows) {
		auto y = row.find("V_Y");
		REQUIRE(y != row.end());
		nines += (y->second == "9") ? 1 : 0;
	}
	CHECK(nines == 2);
}

TEST_CASE("dyn_order_mixed.rq: ORDER BY sorts blank nodes before IRIs before literals") {
	auto conn = makeSeededDatabase();
	auto rows = translateAndRun(*conn, "dyn_order_mixed.rq", "sparql2sql_terms.ttl");
	REQUIRE(rows.size() == 4);
	// Section 15.1's cross-kind order. ex:mixed yields two IRIs (from the
	// rr:template arm) and two literals (from NOTES.BODY), so every IRI must come
	// before every literal regardless of lexical text - which it would not, sorted
	// as plain VARCHAR ('9' > 'http://...' is false, so '9' would lead).
	const std::vector<std::string> order = columnSeq(rows, "V_Y");
	INFO("order: " << order[0] << ", " << order[1] << ", " << order[2] << ", " << order[3]);
	CHECK(order[0].compare(0, 7, "http://") == 0);
	CHECK(order[1].compare(0, 7, "http://") == 0);
	CHECK(order[2].compare(0, 7, "http://") != 0);
	CHECK(order[3].compare(0, 7, "http://") != 0);
}

TEST_CASE("dyn_strdt_var.rq: STRDT with a per-row datatype IRI") {
	auto conn = makeSeededDatabase();
	auto rows = translateAndRun(*conn, "dyn_strdt_var.rq", "sparql2sql_terms.ttl");
	// The datatype comes from MEASURES.HOMEPAGE, a column - so it differs per row
	// and could not have been resolved at translation time.
	REQUIRE(rows.size() == 2);
	CHECK(containsRow(rows, {{"V_P", "alpha"}, {"V_DT", "http://ex.org/a"}}));
	CHECK(containsRow(rows, {{"V_P", "beta"}, {"V_DT", "http://ex.org/b"}}));
}

TEST_CASE("dyn_minmax_mixed.rq: MIN reports the winning row's own datatype") {
	auto conn = makeSeededDatabase();
	auto rows = translateAndRun(*conn, "dyn_minmax_mixed.rq", "sparql2sql_terms.ttl");
	REQUIRE(rows.size() == 1);
	// The lexical MIN over {'9', '100', 'note one', '9'} is '100'. Its datatype
	// must be the one that row actually carries (xsd:integer, from
	// MEASURES.AMOUNT) rather than either arm's constant picked at translation
	// time - which is why the tag is aggregated with arg_min over the same key as
	// the value, not by an independent MIN.
	auto lo = rows[0].find("V_LO");
	auto dt = rows[0].find("V_LODT");
	REQUIRE(lo != rows[0].end());
	REQUIRE(dt != rows[0].end());
	CHECK(lo->second == "100");
	CHECK(dt->second == "http://www.w3.org/2001/XMLSchema#integer");
}

TEST_CASE("dyn_iri_construct.rq: IRI() re-tags a literal as an IRI term") {
	auto conn = makeSeededDatabase();
	auto rows = translateAndRun(*conn, "dyn_iri_construct.rq", "sparql2sql_terms.ttl");
	REQUIRE(rows.size() == 2);
	// The lexical form is untouched - the argument still reads as a literal - but
	// the constructed term is an IRI.
	CHECK(containsRow(rows, {{"V_H", "alpha"}, {"V_II", "true"}, {"V_IL", "true"}}));
	CHECK(containsRow(rows, {{"V_H", "beta"}, {"V_II", "true"}, {"V_IL", "true"}}));
}

TEST_CASE("dyn_group_types.rq: GROUP BY keeps two terms with one lexical form but two datatypes apart") {
	auto conn = makeSeededDatabase();
	auto rows = translateAndRun(*conn, "dyn_group_types.rq", "sparql2sql_terms.ttl");
	// Four terms, four groups of one each. Grouping on the lexical form alone
	// would give three groups, one of them a count of 2.
	REQUIRE(rows.size() == 4);
	for (const auto &row : rows) {
		auto c = row.find("V_C");
		REQUIRE(c != row.end());
		CHECK(c->second == "1");
	}
}

// --- DDL constraint facts: loaded, and safe to act on -------------------

namespace {

// A dedicated database for the constraint tests: KEYED.ID is a PRIMARY KEY and
// LABEL repeats, which is what makes key-proven DISTINCT elimination observable
// in result rows rather than only in SQL text.
std::unique_ptr<DuckDBConnection> makeKeyedDatabase() {
	std::unique_ptr<DuckDBConnection> conn(new DuckDBConnection(":memory:"));
	conn->execute("CREATE TABLE KEYED (ID INTEGER PRIMARY KEY, LABEL VARCHAR NOT NULL)");
	conn->execute("INSERT INTO KEYED VALUES (1, 'DUP'), (2, 'DUP'), (3, 'UNIQUE')");
	// No key, and two FULLY identical rows: R2RML forward generation yields a set
	// of triples, so these two rows denote one triple and the arm's DISTINCT is
	// load-bearing. The rewrite must decline here.
	conn->execute("CREATE TABLE UNKEYED (ID INTEGER, LABEL VARCHAR)");
	conn->execute("INSERT INTO UNKEYED VALUES (1, 'SAME'), (1, 'SAME')");
	return conn;
}

// containsRow is order-insensitive by design, but comparing two whole result
// sets needs a canonical order: nothing constrains the row order of either
// query, and the rewrite is not expected to preserve it.
std::vector<Row> sorted(std::vector<Row> rows) {
	std::sort(rows.begin(), rows.end());
	return rows;
}

std::vector<Row> runWithCatalog(DuckDBConnection &conn, const std::string &queryText, const R2RMLMapping &mapping,
                                const sparql2sql::TypeCatalog *catalog) {
	Parser parser;
	auto query = parser.parseString(queryText);
	DuckDbDialect dialect;
	std::string sql = translateQuery(*query, mapping, dialect, catalog);
	INFO("SQL: " << sql);
	std::unique_ptr<r2rml::SQLResultSet> rs = conn.execute(sql);
	return collectRows(*rs);
}

} // namespace

TEST_CASE("loadTypeCatalog: reads NOT NULL and PRIMARY KEY from a real information_schema") {
	auto conn = makeKeyedDatabase();
	sparql2sql::TypeCatalog cat;
	sql2rdf::loadTypeCatalog(*conn, nullptr, cat);

	// This is where the "does DuckDB actually expose these views" question is
	// settled empirically rather than by assumption.
	CHECK(cat.isNotNull("KEYED", "ID"));
	CHECK(cat.isNotNull("KEYED", "LABEL"));

	std::set<std::string> justId;
	justId.insert("ID");
	CHECK(cat.coversUniqueKey("KEYED", justId));

	// LABEL alone is not a declared key, however non-null it is.
	std::set<std::string> justLabel;
	justLabel.insert("LABEL");
	CHECK_FALSE(cat.coversUniqueKey("KEYED", justLabel));
}

TEST_CASE("constraints: dropping a key-proven DISTINCT does not change the result rows") {
	auto conn = makeKeyedDatabase();
	R2RMLParser mappingParser;
	R2RMLMapping mapping = mappingParser.parse(SOURCE_R2RML_DIR "sparql2sql_keyed.ttl");
	REQUIRE(mapping.isValid());

	const char *const kQuery = "PREFIX ex: <http://example.com/ns#>\n"
	                           "SELECT ?s ?l WHERE { ?s ex:label ?l }";

	sparql2sql::TypeCatalog cat;
	sql2rdf::loadTypeCatalog(*conn, &mapping, cat);

	// The projection contains the whole key (ID, via the subject template), so
	// the arm's DISTINCT is provably redundant and gets dropped.
	auto withCatalog = runWithCatalog(*conn, kQuery, mapping, &cat);
	// Same query with no catalog at all: the DISTINCT stays.
	auto withoutCatalog = runWithCatalog(*conn, kQuery, mapping, nullptr);

	// The point of the whole rewrite: identical rows either way. Three subjects,
	// two of which share a label - the duplicate labels must NOT collapse, since
	// they belong to different subjects.
	REQUIRE(withCatalog.size() == 3);
	REQUIRE(withoutCatalog.size() == 3);
	CHECK(sorted(withCatalog) == sorted(withoutCatalog));
	CHECK(containsRow(withCatalog, {{"V_S", "http://data.example.com/keyed/1"}, {"V_L", "DUP"}}));
	CHECK(containsRow(withCatalog, {{"V_S", "http://data.example.com/keyed/2"}, {"V_L", "DUP"}}));
	CHECK(containsRow(withCatalog, {{"V_S", "http://data.example.com/keyed/3"}, {"V_L", "UNIQUE"}}));

	// And the rewrite really did fire - otherwise the equality above would be
	// vacuous.
	Parser parser;
	auto query = parser.parseString(kQuery);
	DuckDbDialect dialect;
	CHECK(translateQuery(*query, mapping, dialect, &cat).find("SELECT DISTINCT") == std::string::npos);
	CHECK(translateQuery(*query, mapping, dialect, nullptr).find("SELECT DISTINCT") != std::string::npos);
}

TEST_CASE("constraints: an unkeyed table keeps its DISTINCT, so duplicate rows stay one triple") {
	auto conn = makeKeyedDatabase();
	R2RMLParser mappingParser;
	R2RMLMapping mapping = mappingParser.parse(SOURCE_R2RML_DIR "sparql2sql_unkeyed.ttl");
	REQUIRE(mapping.isValid());

	sparql2sql::TypeCatalog cat;
	sql2rdf::loadTypeCatalog(*conn, &mapping, cat);
	// The catalog is fully populated - UNKEYED just has no key to find, so the
	// rewrite must decline rather than fire on a table it cannot prove.
	std::set<std::string> both;
	both.insert("ID");
	both.insert("LABEL");
	REQUIRE_FALSE(cat.coversUniqueKey("UNKEYED", both));

	const char *const kQuery = "PREFIX ex: <http://example.com/ns#>\n"
	                           "SELECT ?s ?l WHERE { ?s ex:label ?l }";
	Parser parser;
	auto query = parser.parseString(kQuery);
	DuckDbDialect dialect;
	CHECK(translateQuery(*query, mapping, dialect, &cat).find("SELECT DISTINCT") != std::string::npos);

	// Two fully identical rows denote ONE triple. If the rewrite ever over-fired
	// here, this would return 2 - this is the test that catches it.
	auto rows = runWithCatalog(*conn, kQuery, mapping, &cat);
	REQUIRE(rows.size() == 1);
	CHECK(containsRow(rows, {{"V_S", "http://data.example.com/unkeyed/1"}, {"V_L", "SAME"}}));
}

TEST_CASE("constraints: a NOT NULL column's guard is dropped without changing results") {
	auto conn = makeKeyedDatabase();
	R2RMLParser mappingParser;
	R2RMLMapping mapping = mappingParser.parse(SOURCE_R2RML_DIR "sparql2sql_keyed.ttl");
	REQUIRE(mapping.isValid());

	const char *const kQuery = "PREFIX ex: <http://example.com/ns#>\n"
	                           "SELECT ?s ?l WHERE { ?s ex:label ?l }";

	sparql2sql::TypeCatalog cat;
	sql2rdf::loadTypeCatalog(*conn, &mapping, cat);
	Parser parser;
	auto query = parser.parseString(kQuery);
	DuckDbDialect dialect;
	std::string sql = translateQuery(*query, mapping, dialect, &cat);
	INFO("SQL: " << sql);
	// KEYED.LABEL is declared NOT NULL, so the guard R2RML's null-column rule
	// would otherwise require is unconditionally true and never emitted.
	CHECK(sql.find("\"LABEL\" IS NOT NULL") == std::string::npos);
	// Control: without the catalog it is emitted.
	CHECK(translateQuery(*query, mapping, dialect, nullptr).find("\"LABEL\" IS NOT NULL") != std::string::npos);

	std::unique_ptr<r2rml::SQLResultSet> rs = conn->execute(sql);
	auto rows = collectRows(*rs);
	CHECK(rows.size() == 3);
}
