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

TEST_CASE("emp_dept_xsd_cast.rq: xsd:integer() cast filters on the numeric STAFF column") {
	auto conn = makeSeededDatabase();
	auto rows = translateAndRun(*conn, "emp_dept_xsd_cast.rq", "example_emp_dept.ttl");
	REQUIRE(rows.size() == 1);
	CHECK(containsRow(rows, {{"V_D", "http://data.example.com/department/10"}, {"V_STAFF", "1"}}));
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
	// ex:location and ex:department.
	REQUIRE(rows.size() == 7);
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
	// 2 ex:staff, 2 rdf:type + 2 ex:name for employees.
	REQUIRE(rows.size() == 12);
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
