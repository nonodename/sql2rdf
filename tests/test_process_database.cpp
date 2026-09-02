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

#include <catch2/catch.hpp>
#include <serd/serd.h>

#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

// Fallback for IDE tooling; CMake overrides via target_compile_definitions.
#ifndef SOURCE_R2RML_DIR
#define SOURCE_R2RML_DIR ""
#endif

#include "r2rml/ColumnTermMap.h"
#include "r2rml/PredicateObjectMap.h"
#include "r2rml/R2RMLMapping.h"
#include "r2rml/R2RMLParser.h"
#include "r2rml/SQLConnection.h"
#include "r2rml/SubjectMap.h"
#include "r2rml/SQLResultSet.h"
#include "r2rml/SQLRow.h"
#include "r2rml/SQLValue.h"
#include "r2rml/StringSQLValue.h"
#include "r2rml/TermMap.h"
#include "r2rml/TriplesMap.h"
#include "MockSQL.h"

using r2rml::R2RMLMapping;
using r2rml::R2RMLParser;
using r2rml::SQLValue;
using r2rml::StringSQLValue;
using r2rml::testing::makeRow;
using r2rml::testing::MockSQLConnection;

// ---------------------------------------------------------------------------
// Helper utilities
// ---------------------------------------------------------------------------

namespace {

// Run processDatabase and capture the NTriples serialisation as a string.
std::string runProcessDatabase(R2RMLMapping &mapping, MockSQLConnection &conn) {
	SerdChunk chunk {nullptr, 0};
	SerdEnv *env = serd_env_new(nullptr);
	SerdWriter *writer = serd_writer_new(SERD_NTRIPLES, (SerdStyle)0, env, nullptr, serd_chunk_sink, &chunk);

	mapping.processDatabase(conn, *writer);

	serd_writer_finish(writer);
	uint8_t *raw = serd_chunk_sink_finish(&chunk);
	std::string result;
	if (raw) {
		result = std::string(reinterpret_cast<const char *>(raw));
		serd_free(raw);
	}
	serd_writer_free(writer);
	serd_env_free(env);
	return result;
}

// Same as runProcessDatabase(), but serialises as NQuads so a triple's graph
// component (rr:graph/rr:graphMap) is visible in the output text.
std::string runProcessDatabaseNQuads(R2RMLMapping &mapping, MockSQLConnection &conn) {
	SerdChunk chunk {nullptr, 0};
	SerdEnv *env = serd_env_new(nullptr);
	SerdWriter *writer = serd_writer_new(SERD_NQUADS, (SerdStyle)0, env, nullptr, serd_chunk_sink, &chunk);

	mapping.processDatabase(conn, *writer);

	serd_writer_finish(writer);
	uint8_t *raw = serd_chunk_sink_finish(&chunk);
	std::string result;
	if (raw) {
		result = std::string(reinterpret_cast<const char *>(raw));
		serd_free(raw);
	}
	serd_writer_free(writer);
	serd_env_free(env);
	return result;
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
	R2RMLParser parser;
	R2RMLMapping mapping = parser.parse(SOURCE_R2RML_DIR "example1.ttl");
	REQUIRE(mapping.isValid());

	MockSQLConnection conn;
	// Key "EMP" matches SELECT * FROM EMP (or the quoted variant "EMP").
	// It is shorter than "EMP2DEPT" so it won't win over that longer key if
	// both are registered in the same connection.
	conn.addResult("EMP", {makeRow({{"EMPNO", StringSQLValue(std::string("7369"))},
	                                {"ENAME", StringSQLValue(std::string("SMITH"))},
	                                {"JOB", StringSQLValue(std::string("CLERK"))},
	                                {"DEPTNO", StringSQLValue(std::string("10"))}})});

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
	R2RMLParser parser;
	R2RMLMapping mapping = parser.parse(SOURCE_R2RML_DIR "example2.ttl");
	REQUIRE(mapping.isValid());

	MockSQLConnection conn;
	// The logical table uses rr:sqlQuery; "DNAME" is a unique fragment of
	// that SQL text and does not appear in EMP or EMP2DEPT table queries.
	conn.addResult("DNAME", {makeRow({{"DEPTNO", StringSQLValue(std::string("10"))},
	                                  {"DNAME", StringSQLValue(std::string("APPSERVER"))},
	                                  {"LOC", StringSQLValue(std::string("NEW YORK"))},
	                                  {"STAFF", StringSQLValue(1)}})});

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
	R2RMLParser parser;
	R2RMLMapping mapping = parser.parse(SOURCE_R2RML_DIR "example_emp_dept.ttl");
	REQUIRE(mapping.isValid());

	MockSQLConnection conn;
	// "EMP" (3 chars) – matched for the EMP base table query.
	// "DNAME" (5 chars) – matched for the DEPT SQL view query; wins over "EMP"
	// when the DEPT view SQL is executed (the view text also contains "EMP"
	// in a subquery, but "DNAME" is longer and therefore selected).
	conn.addResult("EMP", {makeRow({{"EMPNO", StringSQLValue(std::string("7369"))},
	                                {"ENAME", StringSQLValue(std::string("SMITH"))},
	                                {"JOB", StringSQLValue(std::string("CLERK"))},
	                                {"DEPTNO", StringSQLValue(std::string("10"))}})});
	conn.addResult("DNAME", {makeRow({{"DEPTNO", StringSQLValue(std::string("10"))},
	                                  {"DNAME", StringSQLValue(std::string("APPSERVER"))},
	                                  {"LOC", StringSQLValue(std::string("NEW YORK"))},
	                                  {"STAFF", StringSQLValue(1)}})});

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
	const std::string deptLink = "<http://data.example.com/employee/7369> "
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
	R2RMLParser parser;
	R2RMLMapping mapping = parser.parse(SOURCE_R2RML_DIR "example4.ttl");
	REQUIRE(mapping.isValid());

	MockSQLConnection conn;
	// "EMP2DEPT" (8 chars) is longer than "EMP" (3 chars), so it wins when
	// the EMP2DEPT query is executed even though "EMP" is a substring.
	conn.addResult(
	    "EMP2DEPT",
	    {makeRow({{"EMPNO", StringSQLValue(std::string("7369"))}, {"DEPTNO", StringSQLValue(std::string("10"))}}),
	     makeRow({{"EMPNO", StringSQLValue(std::string("7369"))}, {"DEPTNO", StringSQLValue(std::string("20"))}}),
	     makeRow({{"EMPNO", StringSQLValue(std::string("7400"))}, {"DEPTNO", StringSQLValue(std::string("10"))}})});

	std::string out = runProcessDatabase(mapping, conn);

	// --- Junction row (7369, 10) ---
	REQUIRE(out.find("<http://data.example.com/employee=7369/department=10>") != std::string::npos);
	REQUIRE(out.find("<http://example.com/ns#employee>") != std::string::npos);
	REQUIRE(out.find("<http://data.example.com/employee/7369>") != std::string::npos);
	REQUIRE(out.find("<http://example.com/ns#department>") != std::string::npos);
	REQUIRE(out.find("<http://data.example.com/department/10>") != std::string::npos);

	// --- Junction row (7369, 20) ---
	REQUIRE(out.find("<http://data.example.com/employee=7369/department=20>") != std::string::npos);
	REQUIRE(out.find("<http://data.example.com/department/20>") != std::string::npos);

	// --- Junction row (7400, 10) ---
	REQUIRE(out.find("<http://data.example.com/employee=7400/department=10>") != std::string::npos);
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
	R2RMLParser parser;
	R2RMLMapping mapping = parser.parse(SOURCE_R2RML_DIR "example5.ttl");
	REQUIRE(mapping.isValid());

	MockSQLConnection conn;
	// "ROLE" is the computed column name in the CASE SQL view; it appears in
	// the rr:sqlQuery text of example5.ttl and is unique to that query.
	conn.addResult("ROLE", {makeRow({{"EMPNO", StringSQLValue(std::string("7369"))},
	                                 {"ENAME", StringSQLValue(std::string("SMITH"))},
	                                 {"JOB", StringSQLValue(std::string("CLERK"))},
	                                 {"ROLE", StringSQLValue(std::string("general-office"))}})});

	std::string out = runProcessDatabase(mapping, conn);

	// Employee subject
	REQUIRE(out.find("<http://data.example.com/employee/7369>") != std::string::npos);

	// ex:role <http://data.example.com/roles/general-office>
	REQUIRE(out.find("<http://example.com/ns#role>") != std::string::npos);
	REQUIRE(out.find("<http://data.example.com/roles/general-office>") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Datatyped IRIs – value-inferred XSD datatypes
//
// Mapping:  typed_columns.ttl  (no rr:datatype annotations)
// Input:    MEASUREMENTS row with integer, double, boolean, and string columns
//
// Expected RDF (selected fragments):
//   ex:count  "42"^^<xsd:integer>
//   ex:ratio  "1.500000"^^<xsd:double>
//   ex:active "true"^^<xsd:boolean>
//   ex:label  "hello"               (plain literal, no datatype)
// ---------------------------------------------------------------------------
TEST_CASE("processDatabase typed columns - value types produce XSD datatype annotations") {
	R2RMLParser parser;
	R2RMLMapping mapping = parser.parse(SOURCE_R2RML_DIR "typed_columns.ttl");
	REQUIRE(mapping.isValid());

	MockSQLConnection conn;
	conn.addResult("MEASUREMENTS", {makeRow({{"ID", StringSQLValue(std::string("1"))},
	                                         {"COUNT", StringSQLValue(42)},
	                                         {"RATIO", StringSQLValue(1.5)},
	                                         {"ACTIVE", StringSQLValue(true)},
	                                         {"LABEL", StringSQLValue(std::string("hello"))}})});

	std::string out = runProcessDatabase(mapping, conn);

	// Subject URI
	REQUIRE(out.find("<http://data.example.com/measurement/1>") != std::string::npos);

	// Integer column: value annotated with xsd:integer
	REQUIRE(out.find("\"42\"^^<http://www.w3.org/2001/XMLSchema#integer>") != std::string::npos);

	// Double column: value annotated with xsd:double (std::to_string uses 6 decimal places)
	REQUIRE(out.find("\"1.500000\"^^<http://www.w3.org/2001/XMLSchema#double>") != std::string::npos);

	// Boolean column: value annotated with xsd:boolean
	REQUIRE(out.find("\"true\"^^<http://www.w3.org/2001/XMLSchema#boolean>") != std::string::npos);

	// String column: plain literal with no datatype annotation
	REQUIRE(out.find("\"hello\"") != std::string::npos);
	REQUIRE(out.find("\"hello\"^^") == std::string::npos);
}

// ---------------------------------------------------------------------------
// Datatyped IRIs – static rr:datatype overrides the inferred type
//
// Mapping:  typed_columns_with_static_datatype.ttl
//           COUNT column has rr:datatype xsd:string
// Input:    MEASUREMENTS row with COUNT=42 (Integer SQL value)
//
// Expected RDF:
//   ex:count "42"^^<xsd:string>   (static annotation wins over xsd:integer)
// ---------------------------------------------------------------------------
TEST_CASE("processDatabase typed columns - static rr:datatype overrides inferred type") {
	R2RMLParser parser;
	R2RMLMapping mapping = parser.parse(SOURCE_R2RML_DIR "typed_columns_with_static_datatype.ttl");
	REQUIRE(mapping.isValid());

	MockSQLConnection conn;
	conn.addResult("MEASUREMENTS",
	               {makeRow({{"ID", StringSQLValue(std::string("1"))}, {"COUNT", StringSQLValue(42)}})});

	std::string out = runProcessDatabase(mapping, conn);

	// The static rr:datatype xsd:string must win over the inferred xsd:integer.
	REQUIRE(out.find("\"42\"^^<http://www.w3.org/2001/XMLSchema#string>") != std::string::npos);
	REQUIRE(out.find("\"42\"^^<http://www.w3.org/2001/XMLSchema#integer>") == std::string::npos);
}

// ---------------------------------------------------------------------------
// Regression: an unresolvable/invalid IRI must abort processDatabase() on
// the first bad row, not repeat the same write failure once per row.
//
// Mapping:  invalid_template_subject.ttl
//           rr:template "not-a-valid-iri-{ID}" has no scheme, so every
//           generated subject is a relative reference; writing it as
//           N-Triples with no writer base configured always fails
//           (TriplesMap::generateTriples / PredicateObjectMap::processRow
//           now check serd_writer_write_statement's status and throw
//           std::runtime_error on the first failure).
// Input:    BADSUBJECT table with 3 rows.
//
// Expected: processDatabase() throws std::runtime_error, and nothing from
// rows 2 or 3 (which would have appeared had it looped through the whole
// table repeating the error) is ever written.
// ---------------------------------------------------------------------------
TEST_CASE("processDatabase throws on the first row with an unresolvable IRI instead of looping over the table") {
	R2RMLParser parser;
	R2RMLMapping mapping = parser.parse(SOURCE_R2RML_DIR "invalid_template_subject.ttl");
	REQUIRE(mapping.isValid());

	MockSQLConnection conn;
	conn.addResult("BADSUBJECT",
	               {makeRow({{"ID", StringSQLValue(std::string("1"))}, {"V", StringSQLValue(std::string("x"))}}),
	                makeRow({{"ID", StringSQLValue(std::string("2"))}, {"V", StringSQLValue(std::string("y"))}}),
	                makeRow({{"ID", StringSQLValue(std::string("3"))}, {"V", StringSQLValue(std::string("z"))}})});

	SerdChunk chunk {nullptr, 0};
	SerdEnv *env = serd_env_new(nullptr);
	SerdWriter *writer = serd_writer_new(SERD_NTRIPLES, (SerdStyle)0, env, nullptr, serd_chunk_sink, &chunk);

	bool threw = false;
	try {
		mapping.processDatabase(conn, *writer);
	} catch (const std::runtime_error &) {
		threw = true;
	}

	serd_writer_finish(writer);
	uint8_t *raw = serd_chunk_sink_finish(&chunk);
	std::string out;
	if (raw) {
		out = std::string(reinterpret_cast<const char *>(raw));
		serd_free(raw);
	}
	serd_writer_free(writer);
	serd_env_free(env);

	REQUIRE(threw);
	// Rows 2 and 3 must never have been reached.
	REQUIRE(out.find("\"y\"") == std::string::npos);
	REQUIRE(out.find("\"z\"") == std::string::npos);
}

// ---------------------------------------------------------------------------
// Spec 7.4 - rr:termType on a *subject* map.
//
// Mapping: subject_blanknode.ttl
//
// Until this was fixed, buildSubjectMap never read rr:termType at all (unlike
// buildTermMap, which does it for predicate/object maps), so a declared
// rr:BlankNode subject was silently parsed - and emitted - as an IRI. That is
// also what made a blank-node term kind unreachable for the SPARQL-to-SQL
// translator, which reads TermMap::termType directly.
// ---------------------------------------------------------------------------
TEST_CASE("subject map: rr:termType rr:BlankNode is parsed rather than silently ignored") {
	R2RMLParser parser;
	R2RMLMapping mapping = parser.parse(SOURCE_R2RML_DIR "subject_blanknode.ttl");
	REQUIRE(mapping.isValid());

	int checked = 0;
	for (const auto &tm : mapping.triplesMaps) {
		REQUIRE(tm->subjectMap != nullptr);
		const r2rml::TermMap *value = tm->subjectMap->valueTermMap();
		REQUIRE(value != nullptr);
		// Both blank-node maps (templated and column-valued) must have kept the
		// declared term type; the rr:Literal one must have been coerced to IRI.
		if (dynamic_cast<const r2rml::ColumnTermMap *>(value) != nullptr) {
			CHECK(value->termType == r2rml::TermType::BlankNode);
			++checked;
		}
	}
	CHECK(checked == 1);

	// Exactly one of the three subject maps declares a valid non-default term
	// type per kind; count the BlankNodes across the whole mapping.
	int blankNodes = 0;
	int iris = 0;
	for (const auto &tm : mapping.triplesMaps) {
		switch (tm->subjectMap->valueTermMap()->termType) {
		case r2rml::TermType::BlankNode:
			++blankNodes;
			break;
		case r2rml::TermType::IRI:
			++iris;
			break;
		case r2rml::TermType::Literal:
			FAIL("a subject map must never be left as rr:Literal");
			break;
		}
	}
	CHECK(blankNodes == 2);
	CHECK(iris == 1);
}

TEST_CASE("subject map: rr:termType rr:Literal is rejected as a parse error and falls back to rr:IRI") {
	R2RMLParser parser;
	R2RMLMapping mapping = parser.parse(SOURCE_R2RML_DIR "subject_blanknode.ttl");

	bool reported = false;
	for (const auto &err : mapping.parseErrors) {
		if (err.find("rr:Literal is not allowed on a subject map") != std::string::npos) {
			reported = true;
		}
	}
	CHECK(reported);
}

TEST_CASE("processDatabase: a rr:BlankNode subject map emits a blank node, not an IRI") {
	R2RMLParser parser;
	R2RMLMapping mapping = parser.parse(SOURCE_R2RML_DIR "subject_blanknode.ttl");
	REQUIRE(mapping.isValid());

	MockSQLConnection conn;
	conn.addResult("NOTES", {makeRow({{"NID", StringSQLValue(std::string("1"))},
	                                  {"BODY", StringSQLValue(std::string("note one"))}})});
	conn.addResult("LABELS", {makeRow({{"LABEL", StringSQLValue(std::string("lbl7"))},
	                                   {"BODY", StringSQLValue(std::string("labelled"))}})});

	std::string out = runProcessDatabase(mapping, conn);

	// The templated blank-node subject map.
	CHECK(out.find("_:note1") != std::string::npos);
	// The column-valued one.
	CHECK(out.find("_:lbl7") != std::string::npos);
	// And they must not also have been emitted as IRIs - the pre-fix behaviour.
	CHECK(out.find("<note1>") == std::string::npos);
	CHECK(out.find("<lbl7>") == std::string::npos);
	// The rr:Literal subject map, coerced to IRI, emits an IRI subject rather
	// than a literal in the subject position (which is not even representable).
	CHECK(out.find("<http://data.example.com/note/1>") != std::string::npos);
}

TEST_CASE("processDatabase: rr:column/rr:template object maps with explicit rr:termType rr:IRI emit IRIs") {
	R2RMLParser parser;
	R2RMLMapping mapping = parser.parse(SOURCE_R2RML_DIR "termtype_iri_object.ttl");
	REQUIRE(mapping.isValid());

	MockSQLConnection conn;
	conn.addResult("FROG", {makeRow({{"ID", StringSQLValue(std::string("F0009"))},
	                                 {"TARGET", StringSQLValue(std::string("frog:F0009/HTT0000001"))}})});

	std::string out = runProcessDatabase(mapping, conn);
	INFO(out);
	CHECK(out.find("<frog:F0009/HTT0000001>") != std::string::npos);
	CHECK(out.find("\"frog:F0009/HTT0000001\"") == std::string::npos);
	CHECK(out.find("<frog:F0009/HTT0000001> .") != std::string::npos);
}

// Regression test for a real-world defect report: a few bytes of corrupted/
// mis-encoded whitespace sat right before "rr:termType rr:IRI" on an object
// map. Serd's Turtle tokenizer can't parse the resulting garbled token and
// drops the rr:termType triple while silently recovering - and until now,
// R2RMLParser's Serd error sink discarded every syntax error unconditionally
// (see the old cbError()), so the mapping reported isValid()==true with zero
// parseErrors while the object map silently fell back to the rr:column
// object-map default of rr:Literal instead of the rr:IRI the author wrote.
// The fix is to feed Serd's error callback into the same error-collection
// path used for semantic errors, so a syntax error near a term-type
// assertion is surfaced instead of silently swallowed.
TEST_CASE("R2RML parser reports a Turtle syntax error instead of silently dropping rr:termType") {
	R2RMLParser parser;
	R2RMLMapping mapping = parser.parse(SOURCE_R2RML_DIR "object_termtype_corrupted_whitespace.ttl");

	bool reportedSyntaxError = false;
	for (const auto &err : mapping.parseErrors) {
		if (err.find("Turtle syntax error") != std::string::npos) {
			reportedSyntaxError = true;
		}
	}
	CHECK(reportedSyntaxError);
}

// ---------------------------------------------------------------------------
// Regression test: rr:graph on a subject map and rr:graphMap on a
// predicate-object map must actually route generated triples into named
// graphs (quads), not the default graph. Previously TriplesMap::
// generateTriples/PredicateObjectMap::processRow always wrote statements
// with a null graph, silently discarding rr:graph/rr:graphMap entirely.
//
// Mapping (subject_named_graph.ttl):
//   - subjectMap has rr:graph <http://example.com/graph/employees>
//   - the ex:name predicate-object map has no graph of its own, so it
//     inherits only the subject map's graph
//   - the ex:job predicate-object map additionally has its own
//     rr:graphMap (template), so per R2RML §12 its triple is written into
//     the UNION of both graphs
// ---------------------------------------------------------------------------
TEST_CASE("processDatabase honours rr:graph and rr:graphMap by emitting named-graph quads") {
	R2RMLParser parser;
	R2RMLMapping mapping = parser.parse(SOURCE_R2RML_DIR "subject_named_graph.ttl");
	REQUIRE(mapping.isValid());

	MockSQLConnection conn;
	conn.addResult("EMP", {makeRow({{"EMPNO", StringSQLValue(std::string("7369"))},
	                                {"ENAME", StringSQLValue(std::string("SMITH"))},
	                                {"JOB", StringSQLValue(std::string("CLERK"))},
	                                {"DEPTNO", StringSQLValue(std::string("10"))}})});

	std::string out = runProcessDatabaseNQuads(mapping, conn);
	INFO(out);

	// rdf:type triple: subject map's graph applies.
	CHECK(out.find("<http://example.com/ns#Employee> <http://example.com/graph/employees> .") != std::string::npos);

	// ex:name triple: only the subject map's graph applies (the POM has none
	// of its own).
	CHECK(out.find("\"SMITH\" <http://example.com/graph/employees> .") != std::string::npos);

	// ex:job triple: written into BOTH the subject map's graph and the POM's
	// own rr:graphMap-derived graph.
	CHECK(out.find("\"CLERK\" <http://example.com/graph/employees> .") != std::string::npos);
	CHECK(out.find("\"CLERK\" <http://example.com/graph/jobs/CLERK> .") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Regression test: rr:defaultGraph is a *member* of R2RML §12's target graph
// set, not a self-suppressing no-op. forEachGraphNode previously emitted the
// default-graph statement only when NO named graph resolved, so a graph set
// like {rr:defaultGraph, ex:g1} silently lost its default-graph copy.
//
// Mapping (default_graph_union.ttl): the ex:name POM declares both
// rr:defaultGraph and a named graph; the ex:job POM declares only the named
// graph, as the control.
// ---------------------------------------------------------------------------
TEST_CASE("processDatabase treats rr:defaultGraph as a member of the target graph set") {
	R2RMLParser parser;
	R2RMLMapping mapping = parser.parse(SOURCE_R2RML_DIR "default_graph_union.ttl");
	REQUIRE(mapping.isValid());

	MockSQLConnection conn;
	conn.addResult("EMP", {makeRow({{"EMPNO", StringSQLValue(std::string("7369"))},
	                                {"ENAME", StringSQLValue(std::string("SMITH"))},
	                                {"JOB", StringSQLValue(std::string("CLERK"))}})});

	std::string out = runProcessDatabaseNQuads(mapping, conn);
	INFO(out);

	// ex:name: BOTH the named-graph quad and the default-graph triple. In
	// NQuads a default-graph statement has no fourth term, so the object is
	// followed directly by " .".
	CHECK(out.find("\"SMITH\" <http://example.com/graph/g1> .") != std::string::npos);
	CHECK(out.find("\"SMITH\" .") != std::string::npos);

	// ex:job: named graph only. The control - if the default-graph copy were
	// emitted unconditionally rather than only for an explicit
	// rr:defaultGraph, this would also appear bare.
	CHECK(out.find("\"CLERK\" <http://example.com/graph/g1> .") != std::string::npos);
	CHECK(out.find("\"CLERK\" .") == std::string::npos);

	// The rr:class triple: the subject map declares no graph, so default only.
	CHECK(out.find("<http://example.com/ns#Employee> .") != std::string::npos);
}
