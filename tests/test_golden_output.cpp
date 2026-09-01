/**
 * GOLDEN OUTPUT TESTS for R2RMLMapping::processDatabase().
 *
 * Why this file exists, separately from test_process_database.cpp:
 *
 * test_process_database.cpp asserts with substring probes
 * (`out.find(...) != npos`).  That style is readable, but it is blind to
 * exactly the failure modes a refactor of the term/serialisation path
 * introduces.  It cannot see:
 *
 *   - EXTRA triples that should not have been emitted,
 *   - MISSING triples, when another probe happens to match the same text,
 *   - duplicate triples, or a change in emission COUNT,
 *   - literal ESCAPING and quoting drift,
 *   - a triple silently moving between the default graph and a named graph.
 *
 * Every test here therefore serialises the whole graph as NQuads, splits it
 * into lines, SORTS them (so the assertion does not depend on emission order,
 * which is not part of the contract) and compares for EXACT equality against a
 * checked-in expected block.  Nothing can slip through unseen.
 *
 * The expectations below record CURRENT behaviour, including two places where
 * current behaviour is known to be wrong (see golden_typed_constant.ttl).
 * A deliberate fix should show up here as a reviewable diff; an accidental
 * change should show up as a failure.
 */

#include <catch2/catch_test_macros.hpp>
#include <serd/serd.h>

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

// Fallback for IDE tooling; CMake overrides via target_compile_definitions.
#ifndef SOURCE_R2RML_DIR
#define SOURCE_R2RML_DIR ""
#endif

#include "r2rml/R2RMLMapping.h"
#include "r2rml/R2RMLParser.h"
#include "r2rml/StringSQLValue.h"
#include "MockSQL.h"

using r2rml::R2RMLMapping;
using r2rml::R2RMLParser;
using r2rml::StringSQLValue;
using r2rml::testing::makeRow;
using r2rml::testing::MockSQLConnection;

namespace {

// Serialise a mapping's output in the given syntax, with no abbreviation.
std::string serialise(const std::string &fixture, MockSQLConnection &conn, SerdSyntax syntax) {
	R2RMLParser parser;
	R2RMLMapping mapping = parser.parse(fixture);
	REQUIRE(mapping.isValid());

	SerdChunk chunk {nullptr, 0};
	SerdEnv *env = serd_env_new(nullptr);
	SerdWriter *writer = serd_writer_new(syntax, (SerdStyle)0, env, nullptr, serd_chunk_sink, &chunk);

	mapping.processDatabase(conn, *writer);

	serd_writer_finish(writer);
	uint8_t *raw = serd_chunk_sink_finish(&chunk);
	std::string text;
	if (raw) {
		text = std::string(reinterpret_cast<const char *>(raw));
		serd_free(raw);
	}
	serd_writer_free(writer);
	serd_env_free(env);
	return text;
}

// Serialise as NQuads, split into lines, sort, and rejoin.
//
// NQuads rather than NTriples so that a triple's graph component is visible:
// the graph fixtures below depend on being able to see a quad move into or out
// of the default graph.  Sorted so the comparison is order-independent -
// emission order is not part of the contract, but the exact set of quads is.
std::string goldenNQuads(const std::string &fixture, MockSQLConnection &conn) {
	const std::string text = serialise(fixture, conn, SERD_NQUADS);

	std::vector<std::string> lines;
	std::string::size_type start = 0;
	while (start < text.size()) {
		const std::string::size_type nl = text.find('\n', start);
		const std::string::size_type end = (nl == std::string::npos) ? text.size() : nl;
		// A literal may itself contain a raw newline only if the writer failed
		// to escape it; keeping empty lines out is enough for the comparison,
		// and an unescaped newline would show up as a garbled line pair - which
		// is precisely the drift golden_escapes.ttl is here to catch.
		if (end > start) {
			lines.push_back(text.substr(start, end - start));
		}
		if (nl == std::string::npos) {
			break;
		}
		start = nl + 1;
	}
	std::sort(lines.begin(), lines.end());

	std::string joined;
	for (std::size_t i = 0; i < lines.size(); ++i) {
		if (i) {
			joined += "\n";
		}
		joined += lines[i];
	}
	return joined;
}

// Serialise as TURTLE, verbatim and unsorted.
//
// This exists for exactly one reason: SerdNodeFlags (SERD_HAS_QUOTE /
// SERD_HAS_NEWLINE) only ever change the OUTPUT in Turtle, where they select
// """long""" quoting.  NQuads escapes every literal the same way regardless, so
// the NQuads goldens above cannot see flag drift at all.  ConstantTermMap is
// currently the one place that copies those flags from the reader's node rather
// than recomputing them, so this is the only assertion in the suite that would
// notice if that stopped being true.
std::string goldenTurtle(const std::string &fixture, MockSQLConnection &conn) {
	return serialise(fixture, conn, SERD_TURTLE);
}

// Golden comparison with a transcription escape hatch.
//
// Set GOLDEN_DUMP=1 to print the ACTUAL output of every golden case, verbatim
// and unwrapped, between markers.  Catch2's assertion output hard-wraps long
// lines, which makes copying an expectation out of a failure message a reliable
// way to introduce a typo.  This is a transcription aid only - it never changes
// what is asserted, and there is deliberately no mode that rewrites the
// expectations in this file automatically: a golden that updates itself is a
// golden that cannot fail.
void checkGolden(const char *name, const std::string &actual, const std::string &expected) {
	if (std::getenv("GOLDEN_DUMP") != nullptr) {
		std::cout << "\n===BEGIN " << name << "===\n" << actual << "\n===END " << name << "===\n";
	}
	CHECK(actual == expected);
}

// The EMP row from the W3C R2RML spec Section 2, shared by several fixtures.
MockSQLConnection specEmpConnection() {
	MockSQLConnection conn;
	conn.addResult("EMP", {makeRow({{"EMPNO", StringSQLValue(std::string("7369"))},
	                                {"ENAME", StringSQLValue(std::string("SMITH"))},
	                                {"JOB", StringSQLValue(std::string("CLERK"))},
	                                {"DEPTNO", StringSQLValue(std::string("10"))}})});
	return conn;
}

// Two rows for the golden_* fixtures: one carrying the awkward text, one plain,
// so a per-row bug cannot hide behind a single-row expectation.
MockSQLConnection goldenTableConnection() {
	MockSQLConnection conn;
	conn.addResult("GOLDEN",
	               {makeRow({{"ID", StringSQLValue(std::string("1"))},
	                         {"TEXT", StringSQLValue(std::string("He said \"hi\"\nthen left \xE2\x80\x94 caf\xC3\xA9 "
	                                                             "\xE6\x97\xA5\xE6\x9C\xAC"))},
	                         {"NUM", StringSQLValue(42)}}),
	                makeRow({{"ID", StringSQLValue(std::string("2"))},
	                         {"TEXT", StringSQLValue(std::string("plain"))},
	                         {"NUM", StringSQLValue(7)}})});
	return conn;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Example 1 - the simplest mapping: template subject, rr:class, one column.
// ---------------------------------------------------------------------------
TEST_CASE("golden example1 - full graph is exactly the type and name triples") {
	MockSQLConnection conn = specEmpConnection();
	const std::string expected = R"GOLD(<http://data.example.com/employee/7369> <http://example.com/ns#name> "SMITH" .
<http://data.example.com/employee/7369> <http://www.w3.org/1999/02/22-rdf-syntax-ns#type> <http://example.com/ns#Employee> .)GOLD";
	checkGolden("example1", goldenNQuads(SOURCE_R2RML_DIR "example1.ttl", conn), expected);
}

// ---------------------------------------------------------------------------
// Example 4 - many-to-many junction table, three rows, two template objects.
// Catches per-row duplication and row-loop off-by-ones.
// ---------------------------------------------------------------------------
TEST_CASE("golden example4 - junction table emits exactly two links per row") {
	MockSQLConnection conn;
	conn.addResult(
	    "EMP2DEPT",
	    {makeRow({{"EMPNO", StringSQLValue(std::string("7369"))}, {"DEPTNO", StringSQLValue(std::string("10"))}}),
	     makeRow({{"EMPNO", StringSQLValue(std::string("7369"))}, {"DEPTNO", StringSQLValue(std::string("20"))}}),
	     makeRow({{"EMPNO", StringSQLValue(std::string("7400"))}, {"DEPTNO", StringSQLValue(std::string("10"))}})});
	const std::string expected =
	    R"GOLD(<http://data.example.com/employee=7369/department=10> <http://example.com/ns#department> <http://data.example.com/department/10> .
<http://data.example.com/employee=7369/department=10> <http://example.com/ns#employee> <http://data.example.com/employee/7369> .
<http://data.example.com/employee=7369/department=20> <http://example.com/ns#department> <http://data.example.com/department/20> .
<http://data.example.com/employee=7369/department=20> <http://example.com/ns#employee> <http://data.example.com/employee/7369> .
<http://data.example.com/employee=7400/department=10> <http://example.com/ns#department> <http://data.example.com/department/10> .
<http://data.example.com/employee=7400/department=10> <http://example.com/ns#employee> <http://data.example.com/employee/7400> .)GOLD";
	checkGolden("example4", goldenNQuads(SOURCE_R2RML_DIR "example4.ttl", conn), expected);
}

// ---------------------------------------------------------------------------
// Example 5 - rr:sqlQuery logical table (R2RMLView rather than BaseTableOrView).
// ---------------------------------------------------------------------------
TEST_CASE("golden example5 - sqlQuery view maps JOB codes to role IRIs") {
	MockSQLConnection conn;
	conn.addResult("ROLE", {makeRow({{"EMPNO", StringSQLValue(std::string("7369"))},
	                                 {"ROLE", StringSQLValue(std::string("general-office"))}})});
	const std::string expected =
	    R"GOLD(<http://data.example.com/employee/7369> <http://example.com/ns#role> <http://data.example.com/roles/general-office> .)GOLD";
	checkGolden("example5", goldenNQuads(SOURCE_R2RML_DIR "example5.ttl", conn), expected);
}

// ---------------------------------------------------------------------------
// example_emp_dept - the referencing object map (join) path, including the
// two-row generateRDFTerm variant, plus a NULL column (MGR) that must produce
// no triple at all.
// ---------------------------------------------------------------------------
TEST_CASE("golden emp+dept - join, natural datatypes and a null column") {
	MockSQLConnection conn;
	conn.addResult("EMP", {makeRow({{"EMPNO", StringSQLValue(std::string("7369"))},
	                                {"ENAME", StringSQLValue(std::string("SMITH"))},
	                                {"JOB", StringSQLValue(std::string("CLERK"))},
	                                {"MGR", StringSQLValue(std::string("7566"))},
	                                {"DEPTNO", StringSQLValue(std::string("10"))}}),
	                       // MGR is NULL here: per R2RML's null-column rule this
	                       // row must emit NO ex:knows triple. A substring probe
	                       // cannot see that absence; an exact comparison can.
	                       makeRow({{"EMPNO", StringSQLValue(std::string("7566"))},
	                                {"ENAME", StringSQLValue(std::string("JONES"))},
	                                {"JOB", StringSQLValue(std::string("MANAGER"))},
	                                {"MGR", StringSQLValue()},
	                                {"DEPTNO", StringSQLValue(std::string("10"))}})});
	conn.addResult("DNAME", {makeRow({{"DEPTNO", StringSQLValue(std::string("10"))},
	                                  {"DNAME", StringSQLValue(std::string("APPSERVER"))},
	                                  {"LOC", StringSQLValue(std::string("NEW YORK"))},
	                                  {"STAFF", StringSQLValue(2)}})});
	const std::string expected =
	    R"GOLD(<http://data.example.com/department/10> <http://example.com/ns#location> "NEW YORK" .
<http://data.example.com/department/10> <http://example.com/ns#name> "APPSERVER" .
<http://data.example.com/department/10> <http://example.com/ns#staff> "2"^^<http://www.w3.org/2001/XMLSchema#integer> .
<http://data.example.com/department/10> <http://www.w3.org/1999/02/22-rdf-syntax-ns#type> <http://example.com/ns#Department> .
<http://data.example.com/employee/7369> <http://example.com/ns#department> <http://data.example.com/department/10> .
<http://data.example.com/employee/7369> <http://example.com/ns#knows> <http://data.example.com/employee/7566> .
<http://data.example.com/employee/7369> <http://example.com/ns#name> "SMITH" .
<http://data.example.com/employee/7369> <http://www.w3.org/1999/02/22-rdf-syntax-ns#type> <http://example.com/ns#Employee> .
<http://data.example.com/employee/7566> <http://example.com/ns#department> <http://data.example.com/department/10> .
<http://data.example.com/employee/7566> <http://example.com/ns#name> "JONES" .
<http://data.example.com/employee/7566> <http://www.w3.org/1999/02/22-rdf-syntax-ns#type> <http://example.com/ns#Employee> .)GOLD";
	checkGolden("emp_dept", goldenNQuads(SOURCE_R2RML_DIR "example_emp_dept.ttl", conn), expected);
}

// ---------------------------------------------------------------------------
// typed_columns - the natural-datatype path (SQLValue::datatypeIRI()) for
// integer, double, boolean and string columns.
// ---------------------------------------------------------------------------
TEST_CASE("golden typed columns - SQL value types produce XSD datatypes") {
	MockSQLConnection conn;
	conn.addResult("MEASUREMENTS", {makeRow({{"ID", StringSQLValue(std::string("1"))},
	                                         {"COUNT", StringSQLValue(42)},
	                                         {"RATIO", StringSQLValue(1.5)},
	                                         {"ACTIVE", StringSQLValue(true)},
	                                         {"LABEL", StringSQLValue(std::string("hello"))}})});
	const std::string expected =
	    R"GOLD(<http://data.example.com/measurement/1> <http://example.com/ns#active> "true"^^<http://www.w3.org/2001/XMLSchema#boolean> .
<http://data.example.com/measurement/1> <http://example.com/ns#count> "42"^^<http://www.w3.org/2001/XMLSchema#integer> .
<http://data.example.com/measurement/1> <http://example.com/ns#label> "hello" .
<http://data.example.com/measurement/1> <http://example.com/ns#ratio> "1.500000"^^<http://www.w3.org/2001/XMLSchema#double> .)GOLD";
	checkGolden("typed_columns", goldenNQuads(SOURCE_R2RML_DIR "typed_columns.ttl", conn), expected);
}

// ---------------------------------------------------------------------------
// sparql2sql_graphs - every shape forEachGraphNode has to classify: a
// subject-level constant graph (which the rr:class triple inherits), a POM-level
// template graph (union with the subject graph), an ungraphed POM, and
// rr:defaultGraph as a member of a graph set alongside a named graph.
//
// This is the fixture that makes a graph-set regression visible: get the
// wantsDefault/emittedNamed logic wrong and quads silently become triples, or
// get emitted twice.
// ---------------------------------------------------------------------------
TEST_CASE("golden named graphs - every graph-set shape lands in the right graph") {
	MockSQLConnection conn;
	conn.addResult("EMP", {makeRow({{"EMPNO", StringSQLValue(std::string("7369"))},
	                                {"ENAME", StringSQLValue(std::string("SMITH"))},
	                                {"DEPTNO", StringSQLValue(std::string("10"))}})});
	conn.addResult("DEPT", {makeRow({{"DEPTNO", StringSQLValue(std::string("10"))},
	                                 {"DNAME", StringSQLValue(std::string("APPSERVER"))},
	                                 {"LOC", StringSQLValue(std::string("NEW YORK"))}})});
	const std::string expected =
	    R"GOLD(<http://data.example.com/department/10> <http://example.com/ns#location> "NEW YORK" <http://example.com/graph/g1> .
<http://data.example.com/department/10> <http://example.com/ns#name> "APPSERVER" .
<http://data.example.com/department/10> <http://example.com/ns#staff> "10" .
<http://data.example.com/department/10> <http://example.com/ns#staff> "10" <http://example.com/graph/g1> .
<http://data.example.com/department/10> <http://www.w3.org/1999/02/22-rdf-syntax-ns#type> <http://example.com/ns#Department> .
<http://data.example.com/employee/7369> <http://example.com/ns#dept> <http://data.example.com/department/10> <http://example.com/graph/dept/10> .
<http://data.example.com/employee/7369> <http://example.com/ns#dept> <http://data.example.com/department/10> <http://example.com/graph/g1> .
<http://data.example.com/employee/7369> <http://example.com/ns#name> "SMITH" <http://example.com/graph/g1> .
<http://data.example.com/employee/7369> <http://www.w3.org/1999/02/22-rdf-syntax-ns#type> <http://example.com/ns#Employee> <http://example.com/graph/g1> .)GOLD";
	checkGolden("graphs", goldenNQuads(SOURCE_R2RML_DIR "sparql2sql_graphs.ttl", conn), expected);
}

// ---------------------------------------------------------------------------
// default_graph_union - rr:defaultGraph as a set MEMBER: ex:name must appear
// both as a quad in ex:g1 and as a triple in the default graph, while ex:job
// (named graph only) must NOT reach the default graph.
// ---------------------------------------------------------------------------
TEST_CASE("golden default graph union - rr:defaultGraph is a set member") {
	MockSQLConnection conn = specEmpConnection();
	const std::string expected =
	    R"GOLD(<http://data.example.com/employee/7369> <http://example.com/ns#job> "CLERK" <http://example.com/graph/g1> .
<http://data.example.com/employee/7369> <http://example.com/ns#name> "SMITH" .
<http://data.example.com/employee/7369> <http://example.com/ns#name> "SMITH" <http://example.com/graph/g1> .
<http://data.example.com/employee/7369> <http://www.w3.org/1999/02/22-rdf-syntax-ns#type> <http://example.com/ns#Employee> .)GOLD";
	checkGolden("default_graph_union", goldenNQuads(SOURCE_R2RML_DIR "default_graph_union.ttl", conn), expected);
}

// ---------------------------------------------------------------------------
// golden_escapes - literal escaping and multi-byte UTF-8, through both the
// constant path (which copies SerdNodeFlags/n_chars from the reader's node
// today) and the column path (which recomputes them). See the fixture comment.
// ---------------------------------------------------------------------------
TEST_CASE("golden escapes - quotes, newlines and UTF-8 survive both term paths") {
	MockSQLConnection conn = goldenTableConnection();
	const std::string expected =
	    R"GOLD(<http://data.example.com/golden/1> <http://example.com/ns#columnEscaped> "He said \"hi\"\nthen left — café 日本" .
<http://data.example.com/golden/1> <http://example.com/ns#constEscaped> "He said \"hi\"\nthen left — café 日本" .
<http://data.example.com/golden/2> <http://example.com/ns#columnEscaped> "plain" .
<http://data.example.com/golden/2> <http://example.com/ns#constEscaped> "He said \"hi\"\nthen left — café 日本" .)GOLD";
	checkGolden("escapes", goldenNQuads(SOURCE_R2RML_DIR "golden_escapes.ttl", conn), expected);
}

// The same fixture in TURTLE. This is the only golden that can see SerdNodeFlags
// drift, because """long""" quoting is a Turtle-only decision - see goldenTurtle.
TEST_CASE("golden escapes in turtle - node flags select long-quote form") {
	MockSQLConnection conn = goldenTableConnection();
	const std::string expected = R"GOLD(<http://data.example.com/golden/1>
	<http://example.com/ns#constEscaped> """He said "hi"
then left — café 日本""" ;
	<http://example.com/ns#columnEscaped> """He said "hi"
then left — café 日本""" .

<http://data.example.com/golden/2>
	<http://example.com/ns#constEscaped> """He said "hi"
then left — café 日本""" ;
	<http://example.com/ns#columnEscaped> "plain" .
)GOLD";
	checkGolden("escapes_turtle", goldenTurtle(SOURCE_R2RML_DIR "golden_escapes.ttl", conn), expected);
}

// ---------------------------------------------------------------------------
// golden_language - the rr:language arm of processRow and its mutually
// exclusive rr:datatype sibling.
// ---------------------------------------------------------------------------
TEST_CASE("golden language tags - rr:language and rr:datatype stay exclusive") {
	MockSQLConnection conn = goldenTableConnection();
	const std::string expected =
	    R"GOLD(<http://data.example.com/golden/1> <http://example.com/ns#label> "He said \"hi\"\nthen left — café 日本"@en .
<http://data.example.com/golden/1> <http://example.com/ns#labelFr> "He said \"hi\"\nthen left — café 日本"@fr-CA .
<http://data.example.com/golden/1> <http://example.com/ns#plain> "He said \"hi\"\nthen left — café 日本" .
<http://data.example.com/golden/1> <http://example.com/ns#typed> "42"^^<http://www.w3.org/2001/XMLSchema#integer> .
<http://data.example.com/golden/2> <http://example.com/ns#label> "plain"@en .
<http://data.example.com/golden/2> <http://example.com/ns#labelFr> "plain"@fr-CA .
<http://data.example.com/golden/2> <http://example.com/ns#plain> "plain" .
<http://data.example.com/golden/2> <http://example.com/ns#typed> "7"^^<http://www.w3.org/2001/XMLSchema#integer> .)GOLD";
	checkGolden("language", goldenNQuads(SOURCE_R2RML_DIR "golden_language.ttl", conn), expected);
}

// ---------------------------------------------------------------------------
// golden_typed_constant - pins the CURRENT, KNOWN-BUGGY output: an rr:constant
// loses its datatype and its language tag, because R2RMLParser::buildTermMap
// reads only the ObjValue's value field. The expectations below are therefore
// deliberately WRONG-as-specified and RIGHT-as-implemented; the follow-up fix
// should change them in a visible diff.
// ---------------------------------------------------------------------------
TEST_CASE("golden typed constants - records the current datatype/lang loss") {
	MockSQLConnection conn = goldenTableConnection();
	const std::string expected =
	    R"GOLD(<http://data.example.com/golden/1> <http://example.com/ns#iriConst> <http://example.com/ns#Thing> .
<http://data.example.com/golden/1> <http://example.com/ns#langConst> "chat" .
<http://data.example.com/golden/1> <http://example.com/ns#plainConst> "plain" .
<http://data.example.com/golden/1> <http://example.com/ns#typedConst> "5" .
<http://data.example.com/golden/2> <http://example.com/ns#iriConst> <http://example.com/ns#Thing> .
<http://data.example.com/golden/2> <http://example.com/ns#langConst> "chat" .
<http://data.example.com/golden/2> <http://example.com/ns#plainConst> "plain" .
<http://data.example.com/golden/2> <http://example.com/ns#typedConst> "5" .)GOLD";
	checkGolden("typed_constant", goldenNQuads(SOURCE_R2RML_DIR "golden_typed_constant.ttl", conn), expected);
}
