#include <catch2/catch_test_macros.hpp>

#include <serd/serd.h>

#include <algorithm>
#include <map>
#include <string>

// Fallback for IDE tooling; CMake overrides this via target_compile_definitions.
#ifndef SOURCE_R2RML_DIR
#define SOURCE_R2RML_DIR ""
#endif

#include "r2rml/R2RMLMapping.h"
#include "r2rml/JoinCondition.h"
#include "r2rml/TemplateTermMap.h"
#include "r2rml/ObjectMap.h"
#include "r2rml/R2RMLParser.h"
#include "r2rml/ColumnTermMap.h"
#include "r2rml/SubjectMap.h"
#include "r2rml/SQLValue.h"
#include "r2rml/SQLConnection.h"
#include "r2rml/PredicateObjectMap.h"
#include "r2rml/BaseTableOrView.h"
#include "r2rml/TermMap.h"
#include "r2rml/SQLResultSet.h"
#include "r2rml/R2RMLView.h"
#include "r2rml/ReferencingObjectMap.h"
#include "r2rml/PredicateMap.h"
#include "r2rml/LogicalTable.h"
#include "r2rml/ConstantTermMap.h"
#include "r2rml/SQLRow.h"
#include "r2rml/GraphMap.h"
#include "r2rml/TriplesMap.h"
#include "MockSQL.h"

using namespace r2rml;
using namespace r2rml::testing;

TEST_CASE("SQLValue basics") {
	SQLValue def;
	REQUIRE(def.isNull());

	SQLValue s(std::string("hello"));
	REQUIRE(s.type() == SQLValue::Type::String);
	REQUIRE(s.asString() == "hello");

	SQLValue i(42);
	REQUIRE(i.type() == SQLValue::Type::Integer);

	SQLValue d(3.14);
	REQUIRE(d.type() == SQLValue::Type::Double);

	SQLValue b(true);
	REQUIRE(b.type() == SQLValue::Type::Boolean);
}

TEST_CASE("JoinCondition stores columns") {
	JoinCondition jc("child_col", "parent_col");
	REQUIRE(jc.childColumn == "child_col");
	REQUIRE(jc.parentColumn == "parent_col");
}

TEST_CASE("R2RMLMapping defaults") {
	R2RMLMapping m;
	REQUIRE(m.serdEnvironment == nullptr);
	REQUIRE(m.triplesMaps.empty());
}

TEST_CASE("ConstantTermMap returns given node") {
	const uint8_t uri[] = "http://example/";
	SerdNode node = serd_node_from_string(SERD_URI, uri);
	ConstantTermMap c(node);
	SerdEnv *env1 = serd_env_new(NULL);
	SerdNode out = c.generateRDFTerm(SQLRow(), *env1);
	// Use serd_node_equals to compare nodes
	REQUIRE(serd_node_equals(&node, &out));
	serd_env_free(env1);
}

TEST_CASE("Column/Template term maps return default null node") {
	ColumnTermMap ct("col");
	TemplateTermMap tt("{col}");
	SerdEnv *env2 = serd_env_new(NULL);

	SerdNode cn = ct.generateRDFTerm(SQLRow(), *env2);
	SerdNode tn = tt.generateRDFTerm(SQLRow(), *env2);

	REQUIRE(serd_node_equals(&cn, &SERD_NODE_NULL));
	REQUIRE(serd_node_equals(&tn, &SERD_NODE_NULL));
	serd_env_free(env2);
}

TEST_CASE("BaseTableOrView and R2RMLView defaults") {
	BaseTableOrView b("mytable");
	REQUIRE(b.tableName == "mytable");
	MockSQLConnection conn;
	auto rows_b = b.getRows(conn);
	// getRows now delegates to the connection; an empty MockSQLConnection
	// returns a non-null but empty result set (no rows).
	REQUIRE(rows_b != nullptr);

	R2RMLView v("SELECT 1");
	REQUIRE(v.sqlQuery == "SELECT 1");
	auto rows_v = v.getRows(conn);
	REQUIRE(rows_v != nullptr);
}
// ReferencingObjectMap and other TermMap-derived classes that do not
// implement the `generateRDFTerm(const SQLRow&, const SerdEnv&)` signature
// remain abstract and are not instantiated here.

TEST_CASE("PredicateObjectMap and TriplesMap defaults") {
	PredicateObjectMap pom;
	REQUIRE(pom.predicateMaps.empty());
	REQUIRE(pom.objectMaps.empty());
	REQUIRE(pom.graphMaps.empty());

	TriplesMap tm;
	REQUIRE(tm.id.empty());
	REQUIRE(tm.logicalTable == nullptr);
	REQUIRE(tm.subjectMap == nullptr);
	REQUIRE(tm.predicateObjectMaps.empty());
}

TEST_CASE("SQLRow default behaviour") {
	SQLRow r;
	SQLValue v = r.getValue("nope");
	REQUIRE(v.isNull());
	REQUIRE(r.isNull("nope") == true);
}

TEST_CASE("Parser and Mapping API presence") {
	R2RMLParser p;
	R2RMLMapping m = p.parse("nonexistent.ttl");
	REQUIRE(m.triplesMaps.empty());
}

TEST_CASE("Concrete TermMap derivatives construct/destruct") {
	const uint8_t uri[] = "urn:const";
	SerdNode node = serd_node_from_string(SERD_URI, uri);
	ConstantTermMap c(node);
	ColumnTermMap col("col");
	TemplateTermMap tt("{col}");
	(void)c;
	(void)col;
	(void)tt;
	SUCCEED("constructed concrete term map types");
}

TEST_CASE("TermMap isValid methods") {
	// ConstantTermMap valid/invalid
	ConstantTermMap cnull; // default constructed, constantValue is null
	REQUIRE_FALSE(cnull.isValid());
	const uint8_t uri[] = "urn:const";
	SerdNode node = serd_node_from_string(SERD_URI, uri);
	ConstantTermMap cvalid(node);
	REQUIRE(cvalid.isValid());

	// ColumnTermMap valid/invalid
	ColumnTermMap colvalid("col");
	ColumnTermMap colinvalid("");
	REQUIRE(colvalid.isValid());
	REQUIRE_FALSE(colinvalid.isValid());

	// TemplateTermMap valid/invalid
	TemplateTermMap ttvalid("{col}");
	TemplateTermMap ttinvalid("");
	REQUIRE(ttvalid.isValid());
	REQUIRE_FALSE(ttinvalid.isValid());
}

// ---------------------------------------------------------------------------
// Helpers for parsed-mapping tests
// ---------------------------------------------------------------------------

static std::string nodeUri(const SerdNode &n) {
	return std::string(reinterpret_cast<const char *>(n.buf), n.n_bytes);
}

static TriplesMap *findById(R2RMLMapping &m, const std::string &fragment) {
	for (auto &tm : m.triplesMaps) {
		if (tm->id.find(fragment) != std::string::npos)
			return tm.get();
	}
	return nullptr;
}

// ---------------------------------------------------------------------------
// Negative tests – invalid Turtle (Serd parse errors)
// ---------------------------------------------------------------------------

TEST_CASE("Invalid Turtle - unclosed string literal: Serd error propagates as null logicalTable") {
	// Serd reports "line end in short string" to stderr and partially recovers:
	// the rr:logicalTable triple is emitted (so TriplesMap1 is identified) but
	// the rr:tableName value inside the blank node is malformed.  The blank node
	// ends up with no recognised predicates, so buildLogicalTable() returns
	// nullptr – the Serd error visibly degrades the mapping.
	R2RMLParser parser;
	R2RMLMapping mapping = parser.parse(SOURCE_R2RML_DIR "invalid_turtle_unclosed_literal.ttl");

	REQUIRE(mapping.triplesMaps.size() == 1);
	TriplesMap *tm = findById(mapping, "TriplesMap1");
	REQUIRE(tm != nullptr);
	REQUIRE(tm->logicalTable == nullptr);
}

TEST_CASE("Invalid Turtle - undeclared prefix produces empty mapping") {
	// The rr: prefix is used but never declared with @prefix.  Serd reports an
	// error; even if it attempts recovery, expandNode() cannot resolve CURIEs
	// to the full R2RML namespace so no resource is identified as a TriplesMap.
	R2RMLParser parser;
	R2RMLMapping mapping = parser.parse(SOURCE_R2RML_DIR "invalid_turtle_undeclared_prefix.ttl");
	REQUIRE(mapping.triplesMaps.empty());
}

// ---------------------------------------------------------------------------
// Negative tests – valid RDF, but not valid R2RML
// ---------------------------------------------------------------------------

TEST_CASE("Valid RDF with no R2RML predicates produces empty mapping") {
	// The file is syntactically correct Turtle describing an OWL ontology, but
	// contains no rr:* predicates.  No resource qualifies as a TriplesMap.
	R2RMLParser parser;
	R2RMLMapping mapping = parser.parse(SOURCE_R2RML_DIR "valid_rdf_no_r2rml_predicates.ttl");
	REQUIRE(mapping.triplesMaps.empty());
}

TEST_CASE("Valid R2RML with unrecognised logical table predicate yields null logicalTable") {
	// The logicalTable blank node uses rr:table (wrong) instead of rr:tableName.
	// buildLogicalTable() finds neither rr:tableName nor rr:sqlQuery and must
	// return nullptr.  The TriplesMap itself is still created.
	R2RMLParser parser;
	R2RMLMapping mapping = parser.parse(SOURCE_R2RML_DIR "valid_r2rml_wrong_logical_table_pred.ttl");

	REQUIRE(mapping.triplesMaps.size() == 1);
	TriplesMap *tm = findById(mapping, "TriplesMap1");
	REQUIRE(tm != nullptr);
	REQUIRE(tm->logicalTable == nullptr);

	// The subject map (rr:template) and POM were otherwise correct.
	REQUIRE(tm->subjectMap != nullptr);
	REQUIRE(tm->predicateObjectMaps.size() == 1);
}

TEST_CASE("Valid R2RML with unresolved parentTriplesMap yields null parent pointer") {
	// rr:parentTriplesMap references <#GhostTriplesMap> which does not appear
	// anywhere in the file.  After phase-3 reference resolution the
	// ReferencingObjectMap must have parentTriplesMap == nullptr.
	R2RMLParser parser;
	R2RMLMapping mapping = parser.parse(SOURCE_R2RML_DIR "valid_r2rml_unresolved_parent.ttl");

	REQUIRE(mapping.triplesMaps.size() == 1);
	TriplesMap *tm = findById(mapping, "TriplesMap1");
	REQUIRE(tm != nullptr);

	REQUIRE(tm->predicateObjectMaps.size() == 1);
	PredicateObjectMap &pom = *tm->predicateObjectMaps[0];
	REQUIRE(pom.objectMaps.size() == 1);

	auto *rom = dynamic_cast<ReferencingObjectMap *>(pom.objectMaps[0].get());
	REQUIRE(rom != nullptr);
	REQUIRE(rom->parentTriplesMap == nullptr);

	// The join condition was still parsed correctly.
	REQUIRE(rom->joinConditions.size() == 1);
	REQUIRE(rom->joinConditions[0].childColumn == "DEPTNO");
	REQUIRE(rom->joinConditions[0].parentColumn == "DEPTNO");
}

TEST_CASE("Valid R2RML with unrecognised objectMap predicate produces empty objectMaps") {
	// The objectMap blank node has rr:unknownProperty which buildTermMap()
	// does not recognise.  It returns nullptr; the parser warns and the POM's
	// objectMaps vector must remain empty.
	R2RMLParser parser;
	R2RMLMapping mapping = parser.parse(SOURCE_R2RML_DIR "valid_r2rml_empty_object_map.ttl");

	REQUIRE(mapping.triplesMaps.size() == 1);
	TriplesMap *tm = findById(mapping, "TriplesMap1");
	REQUIRE(tm != nullptr);

	REQUIRE(tm->predicateObjectMaps.size() == 1);
	PredicateObjectMap &pom = *tm->predicateObjectMaps[0];

	// The predicate (rr:predicate shortcut) was valid.
	REQUIRE(pom.predicateMaps.size() == 1);
	// The object map had no recognised property, so nothing was added.
	REQUIRE(pom.objectMaps.empty());
}

TEST_CASE("Valid R2RML with literal rr:predicate value produces empty predicateMaps") {
	// rr:predicate must have a URI as its object.  The file supplies a plain
	// string literal instead.  The parser only promotes URI-typed objects, so
	// the literal is silently ignored and predicateMaps must be empty.
	R2RMLParser parser;
	R2RMLMapping mapping = parser.parse(SOURCE_R2RML_DIR "valid_r2rml_literal_predicate.ttl");

	REQUIRE(mapping.triplesMaps.size() == 1);
	TriplesMap *tm = findById(mapping, "TriplesMap1");
	REQUIRE(tm != nullptr);

	REQUIRE(tm->predicateObjectMaps.size() == 1);
	PredicateObjectMap &pom = *tm->predicateObjectMaps[0];

	// The literal value must have been dropped.
	REQUIRE(pom.predicateMaps.empty());
	// The column object map was valid.
	REQUIRE(pom.objectMaps.size() == 1);
	auto *obj = dynamic_cast<ColumnTermMap *>(pom.objectMaps[0].get());
	REQUIRE(obj != nullptr);
	REQUIRE(obj->columnName == "ENAME");
}

// ---------------------------------------------------------------------------
// Example 1 – basic table mapping with template subject and column object
// ---------------------------------------------------------------------------

TEST_CASE("Example 1 - EMP table with template subject and column object") {
	R2RMLParser parser;
	R2RMLMapping mapping = parser.parse(SOURCE_R2RML_DIR "example1.ttl");

	REQUIRE(mapping.triplesMaps.size() == 1);

	TriplesMap *tm = findById(mapping, "TriplesMap1");
	REQUIRE(tm != nullptr);

	// Logical table: rr:tableName "EMP"
	auto *table = dynamic_cast<BaseTableOrView *>(tm->logicalTable.get());
	REQUIRE(table != nullptr);
	REQUIRE(table->tableName == "EMP");

	// Subject map carries rr:class ex:Employee
	REQUIRE(tm->subjectMap != nullptr);
	REQUIRE(tm->subjectMap->classIRIs.size() == 1);
	REQUIRE(tm->subjectMap->classIRIs[0] == "http://example.com/ns#Employee");

	// One predicate-object map: ex:name -> rr:column "ENAME"
	REQUIRE(tm->predicateObjectMaps.size() == 1);
	PredicateObjectMap &pom = *tm->predicateObjectMaps[0];
	REQUIRE(pom.predicateMaps.size() == 1);
	REQUIRE(pom.objectMaps.size() == 1);

	auto *pred = dynamic_cast<ConstantTermMap *>(pom.predicateMaps[0].get());
	REQUIRE(pred != nullptr);
	REQUIRE(nodeUri(pred->constantValue) == "http://example.com/ns#name");

	auto *obj = dynamic_cast<ColumnTermMap *>(pom.objectMaps[0].get());
	REQUIRE(obj != nullptr);
	REQUIRE(obj->columnName == "ENAME");

	REQUIRE(table->isValid());
	REQUIRE(pom.isValid());
	REQUIRE(tm->isValid());
	REQUIRE(mapping.isValid());
}

// ---------------------------------------------------------------------------
// Example 2 – SQL view with three predicate-object maps
// ---------------------------------------------------------------------------

TEST_CASE("Example 2 - department SQL view with three predicate-object maps") {
	R2RMLParser parser;
	R2RMLMapping mapping = parser.parse(SOURCE_R2RML_DIR "example2.ttl");

	REQUIRE(mapping.triplesMaps.size() == 1);

	TriplesMap *tm = findById(mapping, "TriplesMap2");
	REQUIRE(tm != nullptr);

	// Logical table: R2RMLView whose query selects DEPTNO
	auto *view = dynamic_cast<R2RMLView *>(tm->logicalTable.get());
	REQUIRE(view != nullptr);
	REQUIRE(view->sqlQuery.find("SELECT DEPTNO") != std::string::npos);

	// Subject map carries rr:class ex:Department
	REQUIRE(tm->subjectMap != nullptr);
	REQUIRE(tm->subjectMap->classIRIs.size() == 1);
	REQUIRE(tm->subjectMap->classIRIs[0] == "http://example.com/ns#Department");

	// Three predicate-object maps, each with one ColumnTermMap object
	REQUIRE(tm->predicateObjectMaps.size() == 3);

	std::vector<std::string> colNames;
	for (auto &pom : tm->predicateObjectMaps) {
		REQUIRE(pom->objectMaps.size() == 1);
		auto *col = dynamic_cast<ColumnTermMap *>(pom->objectMaps[0].get());
		REQUIRE(col != nullptr);
		colNames.push_back(col->columnName);
	}

	REQUIRE(std::find(colNames.begin(), colNames.end(), "DNAME") != colNames.end());
	REQUIRE(std::find(colNames.begin(), colNames.end(), "LOC") != colNames.end());
	REQUIRE(std::find(colNames.begin(), colNames.end(), "STAFF") != colNames.end());

	REQUIRE(view->isValid());
	REQUIRE(tm->isValid());
	REQUIRE(mapping.isValid());
}

// ---------------------------------------------------------------------------
// Example 3 – referencing object map with join condition
// ---------------------------------------------------------------------------

TEST_CASE("Example 3 - referencing object map joining employee to department") {
	R2RMLParser parser;
	R2RMLMapping mapping = parser.parse(SOURCE_R2RML_DIR "example3.ttl");

	REQUIRE(mapping.triplesMaps.size() == 2);

	// TriplesMap2: the department view (same structure as example 2)
	TriplesMap *tm2 = findById(mapping, "TriplesMap2");
	REQUIRE(tm2 != nullptr);
	REQUIRE(dynamic_cast<R2RMLView *>(tm2->logicalTable.get()) != nullptr);

	// TriplesMap1: no logical table or subject map of its own;
	// one POM whose sole object is a ReferencingObjectMap back to TriplesMap2
	TriplesMap *tm1 = findById(mapping, "TriplesMap1");
	REQUIRE(tm1 != nullptr);
	REQUIRE(tm1->logicalTable == nullptr);
	REQUIRE(tm1->subjectMap == nullptr);

	REQUIRE(tm1->predicateObjectMaps.size() == 1);
	PredicateObjectMap &pom = *tm1->predicateObjectMaps[0];
	REQUIRE(pom.predicateMaps.size() == 1);
	REQUIRE(pom.objectMaps.size() == 1);

	auto *rom = dynamic_cast<ReferencingObjectMap *>(pom.objectMaps[0].get());
	REQUIRE(rom != nullptr);
	REQUIRE(rom->parentTriplesMap == tm2);
	REQUIRE(rom->joinConditions.size() == 1);
	REQUIRE(rom->joinConditions[0].childColumn == "DEPTNO");
	REQUIRE(rom->joinConditions[0].parentColumn == "DEPTNO");

	// tm2 is fully specified; tm1 lacks logicalTable and subjectMap
	REQUIRE(tm2->isValid());
	REQUIRE(rom->isValid());
	REQUIRE(pom.isValid());
	REQUIRE_FALSE(tm1->isValid());
	REQUIRE_FALSE(mapping.isValid());
}

// ---------------------------------------------------------------------------
// Example 4 – template subjects and objects from a join table
// ---------------------------------------------------------------------------

TEST_CASE("Example 4 - EMP2DEPT table with template subject and template objects") {
	R2RMLParser parser;
	R2RMLMapping mapping = parser.parse(SOURCE_R2RML_DIR "example4.ttl");

	REQUIRE(mapping.triplesMaps.size() == 1);

	TriplesMap *tm = findById(mapping, "TriplesMap3");
	REQUIRE(tm != nullptr);

	// Logical table: rr:tableName "EMP2DEPT"
	auto *table = dynamic_cast<BaseTableOrView *>(tm->logicalTable.get());
	REQUIRE(table != nullptr);
	REQUIRE(table->tableName == "EMP2DEPT");

	// Two predicate-object maps, each with a ConstantTermMap predicate
	// and a TemplateTermMap object
	REQUIRE(tm->predicateObjectMaps.size() == 2);

	std::map<std::string, std::string> predToTemplate;
	for (auto &pom : tm->predicateObjectMaps) {
		REQUIRE(pom->predicateMaps.size() == 1);
		REQUIRE(pom->objectMaps.size() == 1);

		auto *pred = dynamic_cast<ConstantTermMap *>(pom->predicateMaps[0].get());
		REQUIRE(pred != nullptr);

		auto *obj = dynamic_cast<TemplateTermMap *>(pom->objectMaps[0].get());
		REQUIRE(obj != nullptr);

		predToTemplate[nodeUri(pred->constantValue)] = obj->templateString;
	}

	REQUIRE(predToTemplate["http://example.com/ns#employee"] == "http://data.example.com/employee/{EMPNO}");
	REQUIRE(predToTemplate["http://example.com/ns#department"] == "http://data.example.com/department/{DEPTNO}");

	REQUIRE(table->isValid());
	REQUIRE(tm->isValid());
	REQUIRE(mapping.isValid());
}

// ---------------------------------------------------------------------------
// isValidInsideOut – positive and negative tests
// ---------------------------------------------------------------------------

TEST_CASE("isValidInsideOut - empty mapping is vacuously valid") {
	R2RMLMapping m;
	REQUIRE(m.isValidInsideOut());
}

TEST_CASE("isValidInsideOut - valid: no logicalTable and no refObjectMap") {
	R2RMLParser parser;
	R2RMLMapping mapping = parser.parse(SOURCE_R2RML_DIR "inside_out_valid.ttl");

	REQUIRE(mapping.triplesMaps.size() == 1);
	TriplesMap *tm = findById(mapping, "TriplesMapIO");
	REQUIRE(tm != nullptr);

	// No logical table was specified in the TTL.
	REQUIRE(tm->logicalTable == nullptr);

	// Subject map and two predicate-object maps are present.
	REQUIRE(tm->subjectMap != nullptr);
	REQUIRE(tm->predicateObjectMaps.size() == 2);

	// Individual and aggregate checks.
	REQUIRE(tm->isValidInsideOut());
	REQUIRE(mapping.isValidInsideOut());

	// isValid() is false because there is no logicalTable.
	REQUIRE_FALSE(tm->isValid());
	REQUIRE_FALSE(mapping.isValid());
}

TEST_CASE("isValidInsideOut - invalid: has logicalTable (rr:tableName / BaseTableOrView)") {
	// example1 uses rr:tableName "EMP" -> BaseTableOrView -> should fail inside-out.
	R2RMLParser parser;
	R2RMLMapping mapping = parser.parse(SOURCE_R2RML_DIR "example1.ttl");

	REQUIRE(mapping.triplesMaps.size() == 1);
	TriplesMap *tm = findById(mapping, "TriplesMap1");
	REQUIRE(tm != nullptr);
	REQUIRE(dynamic_cast<BaseTableOrView *>(tm->logicalTable.get()) != nullptr);

	REQUIRE_FALSE(tm->isValidInsideOut());
	REQUIRE_FALSE(mapping.isValidInsideOut());
}

TEST_CASE("isValidInsideOut - invalid: has logicalTable (rr:sqlQuery / R2RMLView)") {
	// example2 uses rr:sqlQuery -> R2RMLView -> should fail inside-out.
	R2RMLParser parser;
	R2RMLMapping mapping = parser.parse(SOURCE_R2RML_DIR "example2.ttl");

	REQUIRE(mapping.triplesMaps.size() == 1);
	TriplesMap *tm = findById(mapping, "TriplesMap2");
	REQUIRE(tm != nullptr);
	REQUIRE(dynamic_cast<R2RMLView *>(tm->logicalTable.get()) != nullptr);

	REQUIRE_FALSE(tm->isValidInsideOut());
	REQUIRE_FALSE(mapping.isValidInsideOut());
}

TEST_CASE("isValidInsideOut - invalid: has refObjectMap and joinCondition") {
	// inside_out_with_rom.ttl has two TriplesMap nodes with no logicalTable,
	// but one contains a rr:refObjectMap with a rr:joinCondition.
	R2RMLParser parser;
	R2RMLMapping mapping = parser.parse(SOURCE_R2RML_DIR "inside_out_with_rom.ttl");

	REQUIRE(mapping.triplesMaps.size() == 2);

	TriplesMap *empTm = findById(mapping, "EmpTriplesMap");
	REQUIRE(empTm != nullptr);
	REQUIRE(empTm->logicalTable == nullptr);
	REQUIRE(empTm->predicateObjectMaps.size() == 1);

	PredicateObjectMap &pom = *empTm->predicateObjectMaps[0];
	REQUIRE(pom.objectMaps.size() == 1);
	auto *rom = dynamic_cast<ReferencingObjectMap *>(pom.objectMaps[0].get());
	REQUIRE(rom != nullptr);
	REQUIRE(rom->joinConditions.size() == 1);

	// The POM containing the ROM fails inside-out validation.
	REQUIRE_FALSE(pom.isValidInsideOut());
	// The TriplesMap therefore also fails.
	REQUIRE_FALSE(empTm->isValidInsideOut());
	// The whole mapping fails because at least one TriplesMap is invalid.
	REQUIRE_FALSE(mapping.isValidInsideOut());
}

TEST_CASE("isValidInsideOut - PredicateObjectMap with only non-ROM objectMaps is valid") {
	// Construct a POM programmatically with a ConstantTermMap predicate and
	// a ColumnTermMap object (no ReferencingObjectMap).
	const uint8_t predUri[] = "http://example.com/ns#name";
	SerdNode predNode = serd_node_from_string(SERD_URI, predUri);

	PredicateObjectMap pom;
	pom.predicateMaps.push_back(std::unique_ptr<ConstantTermMap>(new ConstantTermMap(predNode)));
	pom.objectMaps.push_back(std::unique_ptr<ColumnTermMap>(new ColumnTermMap("ENAME")));

	REQUIRE(pom.isValid());
	REQUIRE(pom.isValidInsideOut());
}

// ---------------------------------------------------------------------------
// Example 5 – SQL view with CASE expression mapping to a template object
// ---------------------------------------------------------------------------

TEST_CASE("Example 5 - CASE SQL view with role template object") {
	R2RMLParser parser;
	R2RMLMapping mapping = parser.parse(SOURCE_R2RML_DIR "example5.ttl");

	REQUIRE(mapping.triplesMaps.size() == 1);

	TriplesMap *tm = findById(mapping, "TriplesMap1");
	REQUIRE(tm != nullptr);

	// Logical table: R2RMLView with a CASE expression selecting ROLE
	auto *view = dynamic_cast<R2RMLView *>(tm->logicalTable.get());
	REQUIRE(view != nullptr);
	REQUIRE(view->sqlQuery.find("SELECT EMP.*") != std::string::npos);

	// Subject map has no rr:class
	REQUIRE(tm->subjectMap != nullptr);
	REQUIRE(tm->subjectMap->classIRIs.empty());

	// One predicate-object map: ex:role -> rr:template "…/roles/{ROLE}"
	REQUIRE(tm->predicateObjectMaps.size() == 1);
	PredicateObjectMap &pom = *tm->predicateObjectMaps[0];
	REQUIRE(pom.predicateMaps.size() == 1);
	REQUIRE(pom.objectMaps.size() == 1);

	auto *pred = dynamic_cast<ConstantTermMap *>(pom.predicateMaps[0].get());
	REQUIRE(pred != nullptr);
	REQUIRE(nodeUri(pred->constantValue) == "http://example.com/ns#role");

	auto *obj = dynamic_cast<TemplateTermMap *>(pom.objectMaps[0].get());
	REQUIRE(obj != nullptr);
	REQUIRE(obj->templateString == "http://data.example.com/roles/{ROLE}");

	REQUIRE(view->isValid());
	REQUIRE(pom.isValid());
	REQUIRE(tm->isValid());
	REQUIRE(mapping.isValid());
}
