/**
 * Tests for the YARRRML → R2RML translator and parser (yarrrml::YARRRMLParser)
 * plus the small production extensions to r2rml::R2RMLParser it relies on
 * (parseString, rr:termType, rr:language, literal rr:constant).
 *
 * Mirrors the depth and style of tests/test_r2rml.cpp (object-model
 * assertions on parsed mappings) and tests/test_process_database.cpp
 * (end-to-end NTriples assertions via a MockSQLConnection).
 *
 * Fixtures live in tests/sourceYARRRML/ and are YARRRML equivalents of the
 * W3C R2RML specification examples used by tests/sourceR2RML/.
 */

#include <catch2/catch_test_macros.hpp>

#include <serd/serd.h>

#include <algorithm>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

// Fallback for IDE tooling; CMake overrides via target_compile_definitions.
#ifndef SOURCE_YARRRML_DIR
#define SOURCE_YARRRML_DIR ""
#endif

#include "r2rml/BaseTableOrView.h"
#include "r2rml/ColumnTermMap.h"
#include "r2rml/ConstantTermMap.h"
#include "r2rml/JoinCondition.h"
#include "r2rml/PredicateObjectMap.h"
#include "r2rml/R2RMLMapping.h"
#include "r2rml/R2RMLParser.h"
#include "r2rml/R2RMLView.h"
#include "r2rml/ReferencingObjectMap.h"
#include "r2rml/SQLConnection.h"
#include "r2rml/SQLResultSet.h"
#include "r2rml/SQLRow.h"
#include "r2rml/SQLValue.h"
#include "r2rml/StringSQLValue.h"
#include "r2rml/SubjectMap.h"
#include "r2rml/TemplateTermMap.h"
#include "r2rml/TermMap.h"
#include "r2rml/TriplesMap.h"
#include "yarrrml/YARRRMLParser.h"
#include "MockSQL.h"

using r2rml::BaseTableOrView;
using r2rml::ColumnTermMap;
using r2rml::ConstantTermMap;
using r2rml::PredicateObjectMap;
using r2rml::R2RMLMapping;
using r2rml::R2RMLParser;
using r2rml::R2RMLView;
using r2rml::ReferencingObjectMap;
using r2rml::StringSQLValue;
using r2rml::TemplateTermMap;
using r2rml::TermType;
using r2rml::TriplesMap;
using r2rml::testing::makeRow;
using r2rml::testing::MockSQLConnection;
using yarrrml::YARRRMLParser;

// ---------------------------------------------------------------------------
// Helpers (kept local to this file; do not reuse helpers from other test
// files since we must not modify them).
// ---------------------------------------------------------------------------

namespace {

std::string nodeUri(const SerdNode &n) {
	return std::string(reinterpret_cast<const char *>(n.buf), n.n_bytes);
}

TriplesMap *findById(R2RMLMapping &m, const std::string &fragment) {
	for (auto &tm : m.triplesMaps) {
		if (tm->id.find(fragment) != std::string::npos) {
			return tm.get();
		}
	}
	return nullptr;
}

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

} // namespace

// ---------------------------------------------------------------------------
// R2RMLParser::parseString
// ---------------------------------------------------------------------------

TEST_CASE("R2RMLParser::parseString - example1 content from a string with explicit base") {
	std::string turtle = "@prefix rr: <http://www.w3.org/ns/r2rml#>.\n"
	                     "@prefix ex: <http://example.com/ns#>.\n"
	                     "<#TriplesMap1>\n"
	                     "    rr:logicalTable [ rr:tableName \"EMP\" ];\n"
	                     "    rr:subjectMap [\n"
	                     "        rr:template \"http://data.example.com/employee/{EMPNO}\";\n"
	                     "        rr:class ex:Employee;\n"
	                     "    ];\n"
	                     "    rr:predicateObjectMap [\n"
	                     "        rr:predicate ex:name;\n"
	                     "        rr:objectMap [ rr:column \"ENAME\" ];\n"
	                     "    ].";

	R2RMLParser parser;
	R2RMLMapping mapping = parser.parseString(turtle, "http://example.com/base/mapping.ttl");

	REQUIRE(mapping.triplesMaps.size() == 1);
	TriplesMap *tm = findById(mapping, "TriplesMap1");
	REQUIRE(tm != nullptr);
	REQUIRE(tm->id == "http://example.com/base/mapping.ttl#TriplesMap1");

	auto *table = dynamic_cast<BaseTableOrView *>(tm->logicalTable.get());
	REQUIRE(table != nullptr);
	REQUIRE(table->tableName == "EMP");

	REQUIRE(tm->subjectMap != nullptr);
	REQUIRE(tm->subjectMap->classIRIs.size() == 1);
	REQUIRE(tm->subjectMap->classIRIs[0] == "http://example.com/ns#Employee");

	REQUIRE(tm->predicateObjectMaps.size() == 1);
	auto *obj = dynamic_cast<ColumnTermMap *>(tm->predicateObjectMaps[0]->objectMaps[0].get());
	REQUIRE(obj != nullptr);
	REQUIRE(obj->columnName == "ENAME");
}

TEST_CASE("R2RMLParser::parseString - invalid turtle is tolerated like parse()") {
	std::string turtle = "@prefix rr: <http://www.w3.org/ns/r2rml#>.\n"
	                     "<#TriplesMap1> rr:logicalTable [ rr:tableName \"EMP .\n";
	R2RMLParser parser;
	R2RMLMapping mapping = parser.parseString(turtle, "http://example.com/base#");
	// Serd degrades gracefully; we only assert this does not crash/throw.
	(void)mapping;
	SUCCEED("parseString tolerates malformed Turtle without throwing");
}

// ---------------------------------------------------------------------------
// R2RML parser extensions: rr:termType, rr:language, literal rr:constant
// ---------------------------------------------------------------------------

TEST_CASE("R2RML extension - explicit rr:termType rr:IRI on column object map wins over the Literal default") {
	std::string turtle = "@prefix rr: <http://www.w3.org/ns/r2rml#>.\n"
	                     "@prefix ex: <http://example.com/ns#>.\n"
	                     "<#TM1>\n"
	                     "    rr:logicalTable [ rr:tableName \"EMP\" ];\n"
	                     "    rr:subjectMap [ rr:template \"http://x/{EMPNO}\" ];\n"
	                     "    rr:predicateObjectMap [\n"
	                     "        rr:predicate ex:homepage;\n"
	                     "        rr:objectMap [ rr:column \"HOMEPAGE\"; rr:termType rr:IRI ];\n"
	                     "    ].";
	R2RMLParser parser;
	R2RMLMapping mapping = parser.parseString(turtle, "http://example.com/base#");

	TriplesMap *tm = findById(mapping, "TM1");
	REQUIRE(tm != nullptr);
	REQUIRE(tm->predicateObjectMaps.size() == 1);
	auto *col = dynamic_cast<ColumnTermMap *>(tm->predicateObjectMaps[0]->objectMaps[0].get());
	REQUIRE(col != nullptr);
	REQUIRE(col->termType == TermType::IRI);
}

TEST_CASE("R2RML extension - column object map without rr:termType still defaults to Literal") {
	std::string turtle = "@prefix rr: <http://www.w3.org/ns/r2rml#>.\n"
	                     "@prefix ex: <http://example.com/ns#>.\n"
	                     "<#TM1>\n"
	                     "    rr:logicalTable [ rr:tableName \"EMP\" ];\n"
	                     "    rr:subjectMap [ rr:template \"http://x/{EMPNO}\" ];\n"
	                     "    rr:predicateObjectMap [\n"
	                     "        rr:predicate ex:name;\n"
	                     "        rr:objectMap [ rr:column \"ENAME\" ];\n"
	                     "    ].";
	R2RMLParser parser;
	R2RMLMapping mapping = parser.parseString(turtle, "http://example.com/base#");
	TriplesMap *tm = findById(mapping, "TM1");
	REQUIRE(tm != nullptr);
	auto *col = dynamic_cast<ColumnTermMap *>(tm->predicateObjectMaps[0]->objectMaps[0].get());
	REQUIRE(col != nullptr);
	REQUIRE(col->termType == TermType::Literal);
}

TEST_CASE("R2RML extension - rr:language sets languageTag on a column object map") {
	std::string turtle = "@prefix rr: <http://www.w3.org/ns/r2rml#>.\n"
	                     "@prefix ex: <http://example.com/ns#>.\n"
	                     "<#TM1>\n"
	                     "    rr:logicalTable [ rr:tableName \"EMP\" ];\n"
	                     "    rr:subjectMap [ rr:template \"http://x/{EMPNO}\" ];\n"
	                     "    rr:predicateObjectMap [\n"
	                     "        rr:predicate ex:name;\n"
	                     "        rr:objectMap [ rr:column \"ENAME\"; rr:language \"en\" ];\n"
	                     "    ].";
	R2RMLParser parser;
	R2RMLMapping mapping = parser.parseString(turtle, "http://example.com/base#");
	TriplesMap *tm = findById(mapping, "TM1");
	REQUIRE(tm != nullptr);
	auto *col = dynamic_cast<ColumnTermMap *>(tm->predicateObjectMaps[0]->objectMaps[0].get());
	REQUIRE(col != nullptr);
	REQUIRE(col->languageTag != nullptr);
	REQUIRE(*col->languageTag == "en");
}

TEST_CASE("R2RML extension - rr:constant accepts a literal object") {
	std::string turtle = "@prefix rr: <http://www.w3.org/ns/r2rml#>.\n"
	                     "@prefix ex: <http://example.com/ns#>.\n"
	                     "<#TM1>\n"
	                     "    rr:logicalTable [ rr:tableName \"EMP\" ];\n"
	                     "    rr:subjectMap [ rr:template \"http://x/{EMPNO}\" ];\n"
	                     "    rr:predicateObjectMap [\n"
	                     "        rr:predicate ex:status;\n"
	                     "        rr:objectMap [ rr:constant \"active\" ];\n"
	                     "    ].";
	R2RMLParser parser;
	R2RMLMapping mapping = parser.parseString(turtle, "http://example.com/base#");
	TriplesMap *tm = findById(mapping, "TM1");
	REQUIRE(tm != nullptr);
	auto *cst = dynamic_cast<ConstantTermMap *>(tm->predicateObjectMaps[0]->objectMaps[0].get());
	REQUIRE(cst != nullptr);
	REQUIRE(cst->termType == TermType::Literal);
	REQUIRE(cst->constantValue.type == SERD_LITERAL);
	REQUIRE(nodeUri(cst->constantValue) == "active");
}

TEST_CASE("R2RML extension - literal rr:constant end-to-end produces a plain literal") {
	std::string turtle = "@prefix rr: <http://www.w3.org/ns/r2rml#>.\n"
	                     "@prefix ex: <http://example.com/ns#>.\n"
	                     "<#TM1>\n"
	                     "    rr:logicalTable [ rr:tableName \"EMP\" ];\n"
	                     "    rr:subjectMap [ rr:template \"http://x/{EMPNO}\" ];\n"
	                     "    rr:predicateObjectMap [\n"
	                     "        rr:predicate ex:status;\n"
	                     "        rr:objectMap [ rr:constant \"active\" ];\n"
	                     "    ].";
	R2RMLParser parser;
	R2RMLMapping mapping = parser.parseString(turtle, "http://example.com/base#");

	MockSQLConnection conn;
	conn.addResult("EMP", {makeRow({{"EMPNO", StringSQLValue(std::string("1"))}})});
	std::string out = runProcessDatabase(mapping, conn);
	REQUIRE(out.find("\"active\"") != std::string::npos);
}

TEST_CASE("R2RML extension - rr:language end-to-end produces a language-tagged literal") {
	std::string turtle = "@prefix rr: <http://www.w3.org/ns/r2rml#>.\n"
	                     "@prefix ex: <http://example.com/ns#>.\n"
	                     "<#TM1>\n"
	                     "    rr:logicalTable [ rr:tableName \"EMP\" ];\n"
	                     "    rr:subjectMap [ rr:template \"http://x/{EMPNO}\" ];\n"
	                     "    rr:predicateObjectMap [\n"
	                     "        rr:predicate ex:name;\n"
	                     "        rr:objectMap [ rr:column \"ENAME\"; rr:language \"en\" ];\n"
	                     "    ].";
	R2RMLParser parser;
	R2RMLMapping mapping = parser.parseString(turtle, "http://example.com/base#");

	MockSQLConnection conn;
	conn.addResult("EMP", {makeRow({{"EMPNO", StringSQLValue(std::string("7369"))},
	                                {"ENAME", StringSQLValue(std::string("SMITH"))}})});
	std::string out = runProcessDatabase(mapping, conn);
	REQUIRE(out.find("\"SMITH\"@en") != std::string::npos);
}

// ---------------------------------------------------------------------------
// YARRRML translation edge cases - object-model assertions
//
// These used to assert on the intermediate Turtle text produced by the
// (now removed) translateToTurtle(); since YARRRMLParser emits statements
// directly, they assert on the parsed object model instead. This is in some
// ways a stronger test: it proves literal values reach the model unchanged,
// with no serialise/re-parse round trip in between to mask an escaping bug.
// ---------------------------------------------------------------------------

TEST_CASE("YARRRML translation - literal value with embedded quotes/backslashes reaches the model verbatim") {
	YARRRMLParser parser;
	R2RMLMapping mapping = parser.parse(SOURCE_YARRRML_DIR "quoted_literal_value.yml");

	TriplesMap *tm = findById(mapping, "employee");
	REQUIRE(tm != nullptr);
	REQUIRE(tm->predicateObjectMaps.size() == 1);
	auto *cst = dynamic_cast<ConstantTermMap *>(tm->predicateObjectMaps[0]->objectMaps[0].get());
	REQUIRE(cst != nullptr);
	REQUIRE(nodeUri(cst->constantValue) == "the \"boss\" said \\hi\\");
}

TEST_CASE("YARRRML translation - \\$( escape produces a literal $( in the model") {
	YARRRMLParser parser;
	R2RMLMapping mapping = parser.parse(SOURCE_YARRRML_DIR "dollar_escape.yml");

	TriplesMap *tm = findById(mapping, "employee");
	REQUIRE(tm != nullptr);
	auto *cst = dynamic_cast<ConstantTermMap *>(tm->predicateObjectMaps[0]->objectMaps[0].get());
	REQUIRE(cst != nullptr);
	REQUIRE(nodeUri(cst->constantValue) == "total = $(x) + 1");
}

TEST_CASE("YARRRML translation - multi-line SQL query with embedded quotes reaches the model verbatim") {
	YARRRMLParser parser;
	R2RMLMapping mapping = parser.parse(SOURCE_YARRRML_DIR "sql_query_escaping.yml");

	TriplesMap *tm = findById(mapping, "department");
	REQUIRE(tm != nullptr);
	auto *view = dynamic_cast<R2RMLView *>(tm->logicalTable.get());
	REQUIRE(view != nullptr);
	REQUIRE(view->sqlQuery.find("DEPTNO, DNAME\nFROM DEPT") != std::string::npos);
	REQUIRE(view->sqlQuery.find("BOSS \"TOP\"") != std::string::npos);
}

TEST_CASE("YARRRML translation - CURIE prefix directly abutting a column ref (no separator text)") {
	YARRRMLParser parser;
	R2RMLMapping mapping = parser.parse(SOURCE_YARRRML_DIR "curie_abuts_column.yml");

	TriplesMap *tm = findById(mapping, "employee");
	REQUIRE(tm != nullptr);
	REQUIRE(tm->predicateObjectMaps.size() == 1);
	auto *obj = dynamic_cast<TemplateTermMap *>(tm->predicateObjectMaps[0]->objectMaps[0].get());
	REQUIRE(obj != nullptr);
	REQUIRE(obj->templateString == "http://example.com/ns#{MGR}");
}

// ---------------------------------------------------------------------------
// YARRRMLParser::parse - object model assertions (spec examples 1, 2, 4, 5)
// ---------------------------------------------------------------------------

TEST_CASE("YARRRML Example 1 - EMP table with template subject and column object") {
	YARRRMLParser parser;
	R2RMLMapping mapping = parser.parse(SOURCE_YARRRML_DIR "example1.yml");

	REQUIRE(mapping.triplesMaps.size() == 1);
	TriplesMap *tm = findById(mapping, "employee");
	REQUIRE(tm != nullptr);

	auto *table = dynamic_cast<BaseTableOrView *>(tm->logicalTable.get());
	REQUIRE(table != nullptr);
	REQUIRE(table->tableName == "EMP");

	REQUIRE(tm->subjectMap != nullptr);
	REQUIRE(tm->subjectMap->classIRIs.size() == 1);
	REQUIRE(tm->subjectMap->classIRIs[0] == "http://example.com/ns#Employee");

	REQUIRE(tm->predicateObjectMaps.size() == 1);
	PredicateObjectMap &pom = *tm->predicateObjectMaps[0];
	auto *pred = dynamic_cast<ConstantTermMap *>(pom.predicateMaps[0].get());
	REQUIRE(pred != nullptr);
	REQUIRE(nodeUri(pred->constantValue) == "http://example.com/ns#name");

	auto *obj = dynamic_cast<ColumnTermMap *>(pom.objectMaps[0].get());
	REQUIRE(obj != nullptr);
	REQUIRE(obj->columnName == "ENAME");

	REQUIRE(mapping.isValid());
}

TEST_CASE("YARRRML Example 2 - department SQL view with three predicate-object maps") {
	YARRRMLParser parser;
	R2RMLMapping mapping = parser.parse(SOURCE_YARRRML_DIR "example2.yml");

	REQUIRE(mapping.triplesMaps.size() == 1);
	TriplesMap *tm = findById(mapping, "department");
	REQUIRE(tm != nullptr);

	auto *view = dynamic_cast<R2RMLView *>(tm->logicalTable.get());
	REQUIRE(view != nullptr);
	REQUIRE(view->sqlQuery.find("SELECT DEPTNO") != std::string::npos);

	REQUIRE(tm->subjectMap->classIRIs.size() == 1);
	REQUIRE(tm->subjectMap->classIRIs[0] == "http://example.com/ns#Department");

	REQUIRE(tm->predicateObjectMaps.size() == 3);
	std::vector<std::string> colNames;
	for (auto &pom : tm->predicateObjectMaps) {
		auto *col = dynamic_cast<ColumnTermMap *>(pom->objectMaps[0].get());
		REQUIRE(col != nullptr);
		colNames.push_back(col->columnName);
	}
	REQUIRE(std::find(colNames.begin(), colNames.end(), "DNAME") != colNames.end());
	REQUIRE(std::find(colNames.begin(), colNames.end(), "LOC") != colNames.end());
	REQUIRE(std::find(colNames.begin(), colNames.end(), "STAFF") != colNames.end());

	REQUIRE(mapping.isValid());
}

TEST_CASE("YARRRML Example 4 - EMP2DEPT table with template subject and template objects") {
	YARRRMLParser parser;
	R2RMLMapping mapping = parser.parse(SOURCE_YARRRML_DIR "example4.yml");

	REQUIRE(mapping.triplesMaps.size() == 1);
	TriplesMap *tm = findById(mapping, "emp2dept");
	REQUIRE(tm != nullptr);

	auto *table = dynamic_cast<BaseTableOrView *>(tm->logicalTable.get());
	REQUIRE(table != nullptr);
	REQUIRE(table->tableName == "EMP2DEPT");

	REQUIRE(tm->predicateObjectMaps.size() == 2);
	std::map<std::string, std::string> predToTemplate;
	for (auto &pom : tm->predicateObjectMaps) {
		auto *pred = dynamic_cast<ConstantTermMap *>(pom->predicateMaps[0].get());
		REQUIRE(pred != nullptr);
		auto *obj = dynamic_cast<TemplateTermMap *>(pom->objectMaps[0].get());
		REQUIRE(obj != nullptr);
		predToTemplate[nodeUri(pred->constantValue)] = obj->templateString;
	}
	REQUIRE(predToTemplate["http://example.com/ns#employee"] == "http://data.example.com/employee/{EMPNO}");
	REQUIRE(predToTemplate["http://example.com/ns#department"] == "http://data.example.com/department/{DEPTNO}");

	REQUIRE(mapping.isValid());
}

TEST_CASE("YARRRML CURIE-prefixed templates - subject and object templates expand the prefix") {
	YARRRMLParser parser;
	R2RMLMapping mapping = parser.parse(SOURCE_YARRRML_DIR "prefixed_templates.yml");

	REQUIRE(mapping.triplesMaps.size() == 1);
	TriplesMap *tm = findById(mapping, "employee");
	REQUIRE(tm != nullptr);

	REQUIRE(tm->subjectMap != nullptr);

	REQUIRE(tm->predicateObjectMaps.size() == 1);
	auto *obj = dynamic_cast<TemplateTermMap *>(tm->predicateObjectMaps[0]->objectMaps[0].get());
	REQUIRE(obj != nullptr);
	REQUIRE(obj->templateString == "http://example.com/ns#employee/{MGR}");

	REQUIRE(mapping.isValid());
}

TEST_CASE("YARRRML Example 5 - CASE SQL view with role template object") {
	YARRRMLParser parser;
	R2RMLMapping mapping = parser.parse(SOURCE_YARRRML_DIR "example5.yml");

	REQUIRE(mapping.triplesMaps.size() == 1);
	TriplesMap *tm = findById(mapping, "employee");
	REQUIRE(tm != nullptr);

	auto *view = dynamic_cast<R2RMLView *>(tm->logicalTable.get());
	REQUIRE(view != nullptr);
	REQUIRE(view->sqlQuery.find("SELECT EMP.*") != std::string::npos);

	auto *obj = dynamic_cast<TemplateTermMap *>(tm->predicateObjectMaps[0]->objectMaps[0].get());
	REQUIRE(obj != nullptr);
	REQUIRE(obj->templateString == "http://data.example.com/roles/{ROLE}");

	REQUIRE(mapping.isValid());
}

TEST_CASE("YARRRML emp+dept join - referencing object map with resolved parent and join condition") {
	YARRRMLParser parser;
	R2RMLMapping mapping = parser.parse(SOURCE_YARRRML_DIR "example_emp_dept.yml");

	REQUIRE(mapping.triplesMaps.size() == 2);
	TriplesMap *dept = findById(mapping, "department");
	TriplesMap *emp = findById(mapping, "employee");
	REQUIRE(dept != nullptr);
	REQUIRE(emp != nullptr);

	// Find the predicate-object map holding the join (ex:department).
	PredicateObjectMap *joinPom = nullptr;
	for (auto &pom : emp->predicateObjectMaps) {
		auto *pred = dynamic_cast<ConstantTermMap *>(pom->predicateMaps[0].get());
		if (pred && nodeUri(pred->constantValue) == "http://example.com/ns#department") {
			joinPom = pom.get();
		}
	}
	REQUIRE(joinPom != nullptr);
	REQUIRE(joinPom->objectMaps.size() == 1);

	auto *rom = dynamic_cast<ReferencingObjectMap *>(joinPom->objectMaps[0].get());
	REQUIRE(rom != nullptr);
	REQUIRE(rom->parentTriplesMap == dept);
	REQUIRE(rom->joinConditions.size() == 1);
	REQUIRE(rom->joinConditions[0].childColumn == "DEPTNO");
	REQUIRE(rom->joinConditions[0].parentColumn == "DEPTNO");

	REQUIRE(mapping.isValid());
}

// ---------------------------------------------------------------------------
// End-to-end processDatabase tests
// ---------------------------------------------------------------------------

TEST_CASE("YARRRML processDatabase Example1 - EMP table produces rdf:type and name triples") {
	YARRRMLParser parser;
	R2RMLMapping mapping = parser.parse(SOURCE_YARRRML_DIR "example1.yml");
	REQUIRE(mapping.isValid());

	MockSQLConnection conn;
	conn.addResult("EMP", {makeRow({{"EMPNO", StringSQLValue(std::string("7369"))},
	                                {"ENAME", StringSQLValue(std::string("SMITH"))},
	                                {"JOB", StringSQLValue(std::string("CLERK"))},
	                                {"DEPTNO", StringSQLValue(std::string("10"))}})});

	std::string out = runProcessDatabase(mapping, conn);

	REQUIRE(out.find("<http://data.example.com/employee/7369>") != std::string::npos);
	REQUIRE(out.find("<http://www.w3.org/1999/02/22-rdf-syntax-ns#type>") != std::string::npos);
	REQUIRE(out.find("<http://example.com/ns#Employee>") != std::string::npos);
	REQUIRE(out.find("<http://example.com/ns#name>") != std::string::npos);
	REQUIRE(out.find("\"SMITH\"") != std::string::npos);
}

TEST_CASE("YARRRML processDatabase - CURIE-prefixed subject/object templates expand to full IRIs") {
	YARRRMLParser parser;
	R2RMLMapping mapping = parser.parse(SOURCE_YARRRML_DIR "prefixed_templates.yml");
	REQUIRE(mapping.isValid());

	MockSQLConnection conn;
	conn.addResult("EMP", {makeRow({{"EMPNO", StringSQLValue(std::string("7369"))},
	                                {"MGR", StringSQLValue(std::string("7902"))}})});

	std::string out = runProcessDatabase(mapping, conn);

	REQUIRE(out.find("<http://example.com/ns#employee/7369>") != std::string::npos);
	const std::string managerLink = "<http://example.com/ns#employee/7369> "
	                                "<http://example.com/ns#manager> "
	                                "<http://example.com/ns#employee/7902>";
	REQUIRE(out.find(managerLink) != std::string::npos);
}

TEST_CASE("YARRRML processDatabase emp+dept join - employee links to department IRI") {
	YARRRMLParser parser;
	R2RMLMapping mapping = parser.parse(SOURCE_YARRRML_DIR "example_emp_dept.yml");
	REQUIRE(mapping.isValid());

	MockSQLConnection conn;
	conn.addResult("EMP", {makeRow({{"EMPNO", StringSQLValue(std::string("7369"))},
	                                {"ENAME", StringSQLValue(std::string("SMITH"))},
	                                {"JOB", StringSQLValue(std::string("CLERK"))},
	                                {"DEPTNO", StringSQLValue(std::string("10"))}})});
	conn.addResult("DNAME", {makeRow({{"DEPTNO", StringSQLValue(std::string("10"))},
	                                  {"DNAME", StringSQLValue(std::string("APPSERVER"))},
	                                  {"LOC", StringSQLValue(std::string("NEW YORK"))},
	                                  {"STAFF", StringSQLValue(1)}})});

	std::string out = runProcessDatabase(mapping, conn);

	REQUIRE(out.find("<http://data.example.com/employee/7369>") != std::string::npos);
	REQUIRE(out.find("<http://example.com/ns#Employee>") != std::string::npos);
	REQUIRE(out.find("<http://data.example.com/department/10>") != std::string::npos);
	REQUIRE(out.find("<http://example.com/ns#Department>") != std::string::npos);

	const std::string deptLink = "<http://data.example.com/employee/7369> "
	                             "<http://example.com/ns#department> "
	                             "<http://data.example.com/department/10>";
	REQUIRE(out.find(deptLink) != std::string::npos);
}

TEST_CASE("YARRRML processDatabase - datatype shortcut produces xsd:integer typed literal") {
	YARRRMLParser parser;
	R2RMLMapping mapping = parser.parse(SOURCE_YARRRML_DIR "datatype_language.yml");
	REQUIRE(mapping.isValid());

	MockSQLConnection conn;
	conn.addResult("MEASUREMENTS", {makeRow({{"ID", StringSQLValue(std::string("1"))},
	                                         {"COUNT", StringSQLValue(std::string("42"))},
	                                         {"NAME", StringSQLValue(std::string("SMITH"))}})});

	std::string out = runProcessDatabase(mapping, conn);
	REQUIRE(out.find("\"42\"^^<http://www.w3.org/2001/XMLSchema#integer>") != std::string::npos);
}

TEST_CASE("YARRRML processDatabase - language shortcut produces @en tagged literal") {
	YARRRMLParser parser;
	R2RMLMapping mapping = parser.parse(SOURCE_YARRRML_DIR "datatype_language.yml");
	REQUIRE(mapping.isValid());

	MockSQLConnection conn;
	conn.addResult("MEASUREMENTS", {makeRow({{"ID", StringSQLValue(std::string("1"))},
	                                         {"COUNT", StringSQLValue(std::string("42"))},
	                                         {"NAME", StringSQLValue(std::string("SMITH"))}})});

	std::string out = runProcessDatabase(mapping, conn);
	REQUIRE(out.find("\"SMITH\"@en") != std::string::npos);
}

TEST_CASE("YARRRML processDatabase - ~iri suffix produces an IRI object") {
	YARRRMLParser parser;
	R2RMLMapping mapping = parser.parse(SOURCE_YARRRML_DIR "iri_suffix.yml");
	REQUIRE(mapping.isValid());

	TriplesMap *tm = findById(mapping, "employee");
	REQUIRE(tm != nullptr);
	auto *col = dynamic_cast<ColumnTermMap *>(tm->predicateObjectMaps[0]->objectMaps[0].get());
	REQUIRE(col != nullptr);
	REQUIRE(col->columnName == "HOMEPAGE");
	REQUIRE(col->termType == TermType::IRI);

	MockSQLConnection conn;
	conn.addResult("EMP", {makeRow({{"EMPNO", StringSQLValue(std::string("7369"))},
	                                {"HOMEPAGE", StringSQLValue(std::string("http://example.com/~smith"))}})});
	std::string out = runProcessDatabase(mapping, conn);
	REQUIRE(out.find("<http://example.com/~smith>") != std::string::npos);
}

TEST_CASE("YARRRML processDatabase - constant literal object produces a plain literal") {
	YARRRMLParser parser;
	R2RMLMapping mapping = parser.parse(SOURCE_YARRRML_DIR "constant_literal.yml");
	REQUIRE(mapping.isValid());

	TriplesMap *tm = findById(mapping, "employee");
	REQUIRE(tm != nullptr);
	auto *cst = dynamic_cast<ConstantTermMap *>(tm->predicateObjectMaps[0]->objectMaps[0].get());
	REQUIRE(cst != nullptr);
	REQUIRE(cst->termType == TermType::Literal);

	MockSQLConnection conn;
	conn.addResult("EMP", {makeRow({{"EMPNO", StringSQLValue(std::string("7369"))}})});
	std::string out = runProcessDatabase(mapping, conn);
	REQUIRE(out.find("\"active\"") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Aliases: s/subject/subjects, po/predicateobjects, p/o
// ---------------------------------------------------------------------------

TEST_CASE("YARRRML aliases - source/subject/predicateobjects/predicate/object singular forms") {
	YARRRMLParser parser;
	R2RMLMapping mapping = parser.parse(SOURCE_YARRRML_DIR "aliases.yml");
	REQUIRE(mapping.triplesMaps.size() == 1);

	TriplesMap *tm = findById(mapping, "m1");
	REQUIRE(tm != nullptr);
	auto *table = dynamic_cast<BaseTableOrView *>(tm->logicalTable.get());
	REQUIRE(table != nullptr);
	REQUIRE(table->tableName == "EMP");

	REQUIRE(tm->subjectMap != nullptr);
	REQUIRE(tm->predicateObjectMaps.size() == 1);
	auto *obj = dynamic_cast<ColumnTermMap *>(tm->predicateObjectMaps[0]->objectMaps[0].get());
	REQUIRE(obj != nullptr);
	REQUIRE(obj->columnName == "ENAME");

	REQUIRE(mapping.isValid());
}

// ---------------------------------------------------------------------------
// Top-level named sources
// ---------------------------------------------------------------------------

TEST_CASE("YARRRML named sources - table and query referenced by name") {
	YARRRMLParser parser;
	R2RMLMapping mapping = parser.parse(SOURCE_YARRRML_DIR "named_sources.yml");
	REQUIRE(mapping.triplesMaps.size() == 2);

	TriplesMap *emp = findById(mapping, "employee");
	TriplesMap *dept = findById(mapping, "department");
	REQUIRE(emp != nullptr);
	REQUIRE(dept != nullptr);

	auto *empTable = dynamic_cast<BaseTableOrView *>(emp->logicalTable.get());
	REQUIRE(empTable != nullptr);
	REQUIRE(empTable->tableName == "EMP");

	auto *deptView = dynamic_cast<R2RMLView *>(dept->logicalTable.get());
	REQUIRE(deptView != nullptr);
	REQUIRE(deptView->sqlQuery.find("SELECT DEPTNO") != std::string::npos);

	REQUIRE(mapping.isValid());
}

// ---------------------------------------------------------------------------
// Error handling: fatal vs. non-fatal
// ---------------------------------------------------------------------------

TEST_CASE("YARRRML parse - invalid YAML syntax throws std::runtime_error") {
	YARRRMLParser parser;
	REQUIRE_THROWS_AS(parser.parse(SOURCE_YARRRML_DIR "invalid_yaml.yml"), std::runtime_error);
}

TEST_CASE("YARRRML parse - missing 'mappings' key throws std::runtime_error") {
	YARRRMLParser parser;
	REQUIRE_THROWS_AS(parser.parse(SOURCE_YARRRML_DIR "missing_mappings.yml"), std::runtime_error);
}

TEST_CASE("YARRRML parse - graphs key produces a non-fatal warning (lenient mode)") {
	YARRRMLParser parser;
	R2RMLMapping mapping = parser.parse(SOURCE_YARRRML_DIR "graphs_warning.yml", true);
	bool found = false;
	for (const auto &e : mapping.parseErrors) {
		if (e.find("graphs not supported") != std::string::npos) {
			found = true;
		}
	}
	REQUIRE(found);
}

TEST_CASE("YARRRML parse - graphs key throws in strict mode") {
	YARRRMLParser parser;
	REQUIRE_THROWS_AS(parser.parse(SOURCE_YARRRML_DIR "graphs_warning.yml", false), std::runtime_error);
}

TEST_CASE("YARRRML parse - multiple sources produces a non-fatal warning (lenient mode)") {
	YARRRMLParser parser;
	R2RMLMapping mapping = parser.parse(SOURCE_YARRRML_DIR "multiple_sources_warning.yml", true);
	bool found = false;
	for (const auto &e : mapping.parseErrors) {
		if (e.find("multiple sources") != std::string::npos) {
			found = true;
		}
	}
	REQUIRE(found);
}

TEST_CASE("YARRRML parse - multiple sources throws in strict mode") {
	YARRRMLParser parser;
	REQUIRE_THROWS_AS(parser.parse(SOURCE_YARRRML_DIR "multiple_sources_warning.yml", false), std::runtime_error);
}

TEST_CASE("YARRRML parse - unknown join condition function is skipped with a warning (lenient mode)") {
	YARRRMLParser parser;
	R2RMLMapping mapping = parser.parse(SOURCE_YARRRML_DIR "unknown_condition_function.yml", true);

	bool found = false;
	for (const auto &e : mapping.parseErrors) {
		if (e.find("unsupported join condition function") != std::string::npos) {
			found = true;
		}
	}
	REQUIRE(found);

	TriplesMap *emp = findById(mapping, "employee");
	REQUIRE(emp != nullptr);
	REQUIRE(emp->predicateObjectMaps.size() == 1);
	auto *rom = dynamic_cast<ReferencingObjectMap *>(emp->predicateObjectMaps[0]->objectMaps[0].get());
	REQUIRE(rom != nullptr);
	// The unsupported condition was skipped, so no join condition was added.
	REQUIRE(rom->joinConditions.empty());
	// The parent reference itself was still resolved.
	REQUIRE(rom->parentTriplesMap != nullptr);
}

TEST_CASE("YARRRML parse - unknown join condition function throws in strict mode") {
	YARRRMLParser parser;
	REQUIRE_THROWS_AS(parser.parse(SOURCE_YARRRML_DIR "unknown_condition_function.yml", false), std::runtime_error);
}

TEST_CASE("YARRRML parse - no errors on a fully valid mapping") {
	YARRRMLParser parser;
	R2RMLMapping mapping = parser.parse(SOURCE_YARRRML_DIR "example1.yml");
	REQUIRE(mapping.parseErrors.empty());
}
