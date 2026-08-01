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

#include <catch2/catch_test_macros.hpp>

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
#include "sparql2sql/Translator.h"
#include "sparql2sql/TypeCatalog.h"

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
	conn->execute("CREATE TABLE EMP (EMPNO INTEGER, ENAME VARCHAR, DEPTNO INTEGER)");
	conn->execute("CREATE TABLE DEPT (DEPTNO INTEGER, DNAME VARCHAR, LOC VARCHAR)");
	// SMITH has a department; JONES does not (exercises OPTIONAL/MINUS).
	conn->execute("INSERT INTO EMP VALUES (7369, 'SMITH', 10), (7400, 'JONES', NULL)");
	// SALES has no employees (exercises UNION/MINUS asymmetry).
	conn->execute("INSERT INTO DEPT VALUES (10, 'APPSERVER', 'NEW YORK'), (20, 'SALES', 'BOSTON')");

	conn->execute("CREATE TABLE PEOPLE (ID INTEGER, NAME VARCHAR)");
	conn->execute("INSERT INTO PEOPLE VALUES (1, 'ALICE'), (2, 'BOB')");

	conn->execute("CREATE TABLE ORDERS (CUSTID INTEGER, ORDERID INTEGER)");
	conn->execute("INSERT INTO ORDERS VALUES (42, 99)");

	conn->execute("CREATE TABLE WIDGETS (ID INTEGER, NAME VARCHAR)");
	conn->execute("INSERT INTO WIDGETS VALUES (1, 'GADGET')");

	// Cross-table integer join fixture (no declared R2RML FK): COMPANY.CAPIQ
	// and RELATIONS.PARENT are both BIGINT, unified by a shared SPARQL var.
	conn->execute("CREATE TABLE COMPANY (ID INTEGER, CAPIQ BIGINT, LEGALNAME VARCHAR)");
	conn->execute("INSERT INTO COMPANY VALUES (1, 100, 'ACME'), (2, 200, 'GLOBEX')");
	conn->execute("CREATE TABLE RELATIONS (PARENT BIGINT, CHILD BIGINT)");
	conn->execute("INSERT INTO RELATIONS VALUES (100, 200)");

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

} // namespace

TEST_CASE("emp_dept_join.rq: only the employee with a non-null department joins") {
	auto conn = makeSeededDatabase();
	auto rows = translateAndRun(*conn, "emp_dept_join.rq", "example_emp_dept.ttl");
	REQUIRE(rows.size() == 1);
	CHECK(containsRow(
	    rows, {{"V_E", "http://data.example.com/employee/7369"}, {"V_D", "http://data.example.com/department/10"}}));
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
