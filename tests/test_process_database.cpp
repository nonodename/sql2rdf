/**
 * Integration tests for R2RMLMapping::processDatabase().
 *
 * Each test:
 *   1. Loads a real R2RML mapping from one of the spec example .ttl files.
 *   2. Supplies a MockSQLConnection pre-loaded with the spec input tables.
 *   3. Calls processDatabase() and captures the NTriples output.
 *   4. Asserts that the expected RDF triples (from the spec) appear in the
 *      output.
 *
 * All tests are expected to FAIL until processDatabase() is implemented –
 * the current implementation is a no-op stub.
 *
 * Input data is taken from the W3C R2RML specification (spec.pdf), section 2:
 *
 *   EMP  table:  EMPNO=7369, ENAME="SMITH", JOB="CLERK", DEPTNO=10
 *   DEPT table:  DEPTNO=10,  DNAME="APPSERVER", LOC="NEW YORK"
 *                STAFF=1 (computed by the SQL view)
 *   EMP2DEPT:    (7369,10), (7369,20), (7400,10)
 */

#include <catch2/catch_test_macros.hpp>
#include <serd/serd.h>

#include <map>
#include <memory>
#include <string>
#include <vector>

// Fallback for IDE tooling; CMake overrides via target_compile_definitions.
#ifndef SOURCE_R2RML_DIR
#define SOURCE_R2RML_DIR ""
#endif

#include "r2rml/R2RMLMapping.h"
#include "r2rml/R2RMLParser.h"
#include "r2rml/SQLConnection.h"
#include "r2rml/SQLResultSet.h"
#include "r2rml/SQLRow.h"
#include "r2rml/SQLValue.h"
#include "r2rml/TriplesMap.h"

using namespace r2rml;

// ---------------------------------------------------------------------------
// Mock SQL infrastructure
// ---------------------------------------------------------------------------

namespace {

// Iterates over a fixed vector of SQLRows.
class MockSQLResultSet : public SQLResultSet {
public:
    explicit MockSQLResultSet(std::vector<SQLRow> rows)
        : rows_(std::move(rows)) {}

    bool next() override {
        ++cursor_;
        return cursor_ < static_cast<int>(rows_.size());
    }

    SQLRow getCurrentRow() const override {
        return rows_[static_cast<size_t>(cursor_)];
    }

private:
    std::vector<SQLRow> rows_;
    int cursor_{-1};
};

// Returns pre-registered rows for any query whose text contains a registered
// key fragment.  When multiple keys match, the longest one wins (so more
// specific fragments take priority over shorter substrings).
class MockSQLConnection : public SQLConnection {
public:
    void addResult(std::string queryFragment, std::vector<SQLRow> rows) {
        results_.push_back({std::move(queryFragment), std::move(rows)});
    }

    std::unique_ptr<SQLResultSet> execute(const std::string& query) override {
        const std::vector<SQLRow>* best = nullptr;
        size_t bestLen = 0;
        for (const auto& kv : results_) {
            if (query.find(kv.first) != std::string::npos &&
                    kv.first.size() > bestLen) {
                bestLen = kv.first.size();
                best = &kv.second;
            }
        }
        if (best) {
            return std::unique_ptr<SQLResultSet>(new MockSQLResultSet(*best));
        }
        return std::unique_ptr<SQLResultSet>(
            new MockSQLResultSet(std::vector<SQLRow>{}));
    }

private:
    std::vector<std::pair<std::string, std::vector<SQLRow>>> results_;
};

// ---------------------------------------------------------------------------
// Helper utilities
// ---------------------------------------------------------------------------

// Run processDatabase and capture the NTriples serialisation as a string.
std::string runProcessDatabase(R2RMLMapping& mapping, MockSQLConnection& conn) {
    SerdChunk chunk{NULL, 0};
    SerdEnv*    env    = serd_env_new(NULL);
    SerdWriter* writer = serd_writer_new(
        SERD_NTRIPLES, (SerdStyle)0, env, NULL, serd_chunk_sink, &chunk);

    mapping.processDatabase(conn, *writer);

    serd_writer_finish(writer);
    uint8_t* raw = serd_chunk_sink_finish(&chunk);
    std::string result;
    if (raw) {
        result = std::string(reinterpret_cast<const char*>(raw));
        serd_free(raw);
    }
    serd_writer_free(writer);
    serd_env_free(env);
    return result;
}

// Build an SQLRow from an initializer-list of {column, value} pairs.
SQLRow makeRow(std::initializer_list<std::pair<const std::string, SQLValue>> cols) {
    return SQLRow(std::map<std::string, SQLValue>(cols));
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Spec §2.3 – Example 1: simple EMP table mapping
//
// Mapping:  example1.ttl
// Input:    EMP row EMPNO=7369, ENAME="SMITH", JOB="CLERK", DEPTNO=10
//
// Expected RDF:
//   <http://data.example.com/employee/7369>
//       rdf:type ex:Employee ;
//       ex:name  "SMITH" .
// ---------------------------------------------------------------------------
TEST_CASE("processDatabase Example1 - EMP table produces rdf:type and name triples") {
    R2RMLParser  parser;
    R2RMLMapping mapping = parser.parse(SOURCE_R2RML_DIR "example1.ttl");
    REQUIRE(mapping.isValid());

    MockSQLConnection conn;
    // Key "EMP" matches SELECT * FROM EMP (or the quoted variant "EMP").
    // It is shorter than "EMP2DEPT" so it won't win over that longer key if
    // both are registered in the same connection.
    conn.addResult("EMP", {
        makeRow({
            {"EMPNO",  SQLValue(std::string("7369"))},
            {"ENAME",  SQLValue(std::string("SMITH"))},
            {"JOB",    SQLValue(std::string("CLERK"))},
            {"DEPTNO", SQLValue(std::string("10"))}
        })
    });

    std::string out = runProcessDatabase(mapping, conn);

    // Subject URI must appear
    REQUIRE(out.find("<http://data.example.com/employee/7369>") != std::string::npos);

    // rdf:type ex:Employee
    REQUIRE(out.find("<http://www.w3.org/1999/02/22-rdf-syntax-ns#type>") != std::string::npos);
    REQUIRE(out.find("<http://example.com/ns#Employee>") != std::string::npos);

    // ex:name "SMITH"
    REQUIRE(out.find("<http://example.com/ns#name>") != std::string::npos);
    REQUIRE(out.find("\"SMITH\"") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Spec §2.4 – Example 2: DEPT SQL view mapping
//
// Mapping:  example2.ttl
// Input:    DEPT view row DEPTNO=10, DNAME="APPSERVER", LOC="NEW YORK", STAFF=1
//
// Expected RDF:
//   <http://data.example.com/department/10>
//       rdf:type    ex:Department ;
//       ex:name     "APPSERVER" ;
//       ex:location "NEW YORK" ;
//       ex:staff    "1" .
// ---------------------------------------------------------------------------
TEST_CASE("processDatabase Example2 - DEPT SQL view produces Department triples") {
    R2RMLParser  parser;
    R2RMLMapping mapping = parser.parse(SOURCE_R2RML_DIR "example2.ttl");
    REQUIRE(mapping.isValid());

    MockSQLConnection conn;
    // The logical table uses rr:sqlQuery; "DNAME" is a unique fragment of
    // that SQL text and does not appear in EMP or EMP2DEPT table queries.
    conn.addResult("DNAME", {
        makeRow({
            {"DEPTNO", SQLValue(std::string("10"))},
            {"DNAME",  SQLValue(std::string("APPSERVER"))},
            {"LOC",    SQLValue(std::string("NEW YORK"))},
            {"STAFF",  SQLValue(1)}
        })
    });

    std::string out = runProcessDatabase(mapping, conn);

    // Subject URI
    REQUIRE(out.find("<http://data.example.com/department/10>") != std::string::npos);

    // rdf:type ex:Department
    REQUIRE(out.find("<http://www.w3.org/1999/02/22-rdf-syntax-ns#type>") != std::string::npos);
    REQUIRE(out.find("<http://example.com/ns#Department>") != std::string::npos);

    // ex:name "APPSERVER"
    REQUIRE(out.find("<http://example.com/ns#name>") != std::string::npos);
    REQUIRE(out.find("\"APPSERVER\"") != std::string::npos);

    // ex:location "NEW YORK"
    REQUIRE(out.find("<http://example.com/ns#location>") != std::string::npos);
    REQUIRE(out.find("\"NEW YORK\"") != std::string::npos);

    // ex:staff "1" (STAFF is an integer column; value serialised as string)
    REQUIRE(out.find("<http://example.com/ns#staff>") != std::string::npos);
    REQUIRE(out.find("\"1\"") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Spec §2.5 – Linking EMP to DEPT via a referencing object map (join)
//
// Mapping:  example_emp_dept.ttl  (complete combined mapping)
// Input:    EMP row  EMPNO=7369, ENAME="SMITH", DEPTNO=10
//           DEPT view row  DEPTNO=10, DNAME="APPSERVER", LOC="NEW YORK", STAFF=1
//
// Expected RDF (selected triples):
//   <http://data.example.com/employee/7369>
//       rdf:type       ex:Employee ;
//       ex:name        "SMITH" ;
//       ex:department  <http://data.example.com/department/10> .
//   <http://data.example.com/department/10>
//       rdf:type    ex:Department ;
//       ex:name     "APPSERVER" .
// ---------------------------------------------------------------------------
TEST_CASE("processDatabase emp+dept join - employee links to department IRI") {
    R2RMLParser  parser;
    R2RMLMapping mapping = parser.parse(SOURCE_R2RML_DIR "example_emp_dept.ttl");
    REQUIRE(mapping.isValid());

    MockSQLConnection conn;
    // "EMP" (3 chars) – matched for the EMP base table query.
    // "DNAME" (5 chars) – matched for the DEPT SQL view query; wins over "EMP"
    // when the DEPT view SQL is executed (the view text also contains "EMP"
    // in a subquery, but "DNAME" is longer and therefore selected).
    conn.addResult("EMP", {
        makeRow({
            {"EMPNO",  SQLValue(std::string("7369"))},
            {"ENAME",  SQLValue(std::string("SMITH"))},
            {"JOB",    SQLValue(std::string("CLERK"))},
            {"DEPTNO", SQLValue(std::string("10"))}
        })
    });
    conn.addResult("DNAME", {
        makeRow({
            {"DEPTNO", SQLValue(std::string("10"))},
            {"DNAME",  SQLValue(std::string("APPSERVER"))},
            {"LOC",    SQLValue(std::string("NEW YORK"))},
            {"STAFF",  SQLValue(1)}
        })
    });

    std::string out = runProcessDatabase(mapping, conn);

    // Employee subject and class
    REQUIRE(out.find("<http://data.example.com/employee/7369>") != std::string::npos);
    REQUIRE(out.find("<http://example.com/ns#Employee>") != std::string::npos);

    // Department subject and class
    REQUIRE(out.find("<http://data.example.com/department/10>") != std::string::npos);
    REQUIRE(out.find("<http://example.com/ns#Department>") != std::string::npos);

    // Join result: employee ex:department department
    REQUIRE(out.find("<http://example.com/ns#department>") != std::string::npos);

    // The object of ex:department must be the department IRI (not a literal)
    const std::string deptLink =
        "<http://data.example.com/employee/7369> "
        "<http://example.com/ns#department> "
        "<http://data.example.com/department/10>";
    REQUIRE(out.find(deptLink) != std::string::npos);
}

// ---------------------------------------------------------------------------
// Spec §2.6 – Example 4: many-to-many EMP2DEPT junction table
//
// Mapping:  example4.ttl
// Input:    EMP2DEPT rows (7369,10), (7369,20), (7400,10)
//
// Expected RDF:
//   <http://data.example.com/employee=7369/department=10>
//       ex:employee   <http://data.example.com/employee/7369> ;
//       ex:department <http://data.example.com/department/10> .
//   <http://data.example.com/employee=7369/department=20>
//       ex:employee   <http://data.example.com/employee/7369> ;
//       ex:department <http://data.example.com/department/20> .
//   <http://data.example.com/employee=7400/department=10>
//       ex:employee   <http://data.example.com/employee/7400> ;
//       ex:department <http://data.example.com/department/10> .
// ---------------------------------------------------------------------------
TEST_CASE("processDatabase Example4 - EMP2DEPT many-to-many produces link triples") {
    R2RMLParser  parser;
    R2RMLMapping mapping = parser.parse(SOURCE_R2RML_DIR "example4.ttl");
    REQUIRE(mapping.isValid());

    MockSQLConnection conn;
    // "EMP2DEPT" (8 chars) is longer than "EMP" (3 chars), so it wins when
    // the EMP2DEPT query is executed even though "EMP" is a substring.
    conn.addResult("EMP2DEPT", {
        makeRow({
            {"EMPNO",  SQLValue(std::string("7369"))},
            {"DEPTNO", SQLValue(std::string("10"))}
        }),
        makeRow({
            {"EMPNO",  SQLValue(std::string("7369"))},
            {"DEPTNO", SQLValue(std::string("20"))}
        }),
        makeRow({
            {"EMPNO",  SQLValue(std::string("7400"))},
            {"DEPTNO", SQLValue(std::string("10"))}
        })
    });

    std::string out = runProcessDatabase(mapping, conn);

    // --- Junction row (7369, 10) ---
    REQUIRE(out.find("<http://data.example.com/employee=7369/department=10>")
            != std::string::npos);
    REQUIRE(out.find("<http://example.com/ns#employee>")   != std::string::npos);
    REQUIRE(out.find("<http://data.example.com/employee/7369>") != std::string::npos);
    REQUIRE(out.find("<http://example.com/ns#department>") != std::string::npos);
    REQUIRE(out.find("<http://data.example.com/department/10>") != std::string::npos);

    // --- Junction row (7369, 20) ---
    REQUIRE(out.find("<http://data.example.com/employee=7369/department=20>")
            != std::string::npos);
    REQUIRE(out.find("<http://data.example.com/department/20>") != std::string::npos);

    // --- Junction row (7400, 10) ---
    REQUIRE(out.find("<http://data.example.com/employee=7400/department=10>")
            != std::string::npos);
    REQUIRE(out.find("<http://data.example.com/employee/7400>") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Spec §2.7 – Example 5: translating JOB codes to role IRIs via a CASE view
//
// Mapping:  example5.ttl
// Input:    CASE view row EMPNO=7369, JOB="CLERK", ROLE="general-office"
//           (the mock pre-computes the CASE result that the SQL engine would
//            normally compute)
//
// Expected RDF:
//   <http://data.example.com/employee/7369>
//       ex:role <http://data.example.com/roles/general-office> .
// ---------------------------------------------------------------------------
TEST_CASE("processDatabase Example5 - CASE view maps JOB code to role IRI") {
    R2RMLParser  parser;
    R2RMLMapping mapping = parser.parse(SOURCE_R2RML_DIR "example5.ttl");
    REQUIRE(mapping.isValid());

    MockSQLConnection conn;
    // "ROLE" is the computed column name in the CASE SQL view; it appears in
    // the rr:sqlQuery text of example5.ttl and is unique to that query.
    conn.addResult("ROLE", {
        makeRow({
            {"EMPNO", SQLValue(std::string("7369"))},
            {"ENAME", SQLValue(std::string("SMITH"))},
            {"JOB",   SQLValue(std::string("CLERK"))},
            {"ROLE",  SQLValue(std::string("general-office"))}
        })
    });

    std::string out = runProcessDatabase(mapping, conn);

    // Employee subject
    REQUIRE(out.find("<http://data.example.com/employee/7369>") != std::string::npos);

    // ex:role <http://data.example.com/roles/general-office>
    REQUIRE(out.find("<http://example.com/ns#role>") != std::string::npos);
    REQUIRE(out.find("<http://data.example.com/roles/general-office>")
            != std::string::npos);
}
