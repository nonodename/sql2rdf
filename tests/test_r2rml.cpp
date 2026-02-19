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

using namespace r2rml;

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
    SerdEnv* env1 = serd_env_new(NULL);
    SerdNode out = c.generateRDFTerm(SQLRow(), *env1);
    // Use serd_node_equals to compare nodes
    REQUIRE(serd_node_equals(&node, &out));
    serd_env_free(env1);
}

TEST_CASE("Column/Template term maps return default null node") {
    ColumnTermMap ct("col");
    TemplateTermMap tt("{col}");
    SerdEnv* env2 = serd_env_new(NULL);

    SerdNode cn = ct.generateRDFTerm(SQLRow(), *env2);
    SerdNode tn = tt.generateRDFTerm(SQLRow(), *env2);

    REQUIRE(serd_node_equals(&cn, &SERD_NODE_NULL));
    REQUIRE(serd_node_equals(&tn, &SERD_NODE_NULL));
    serd_env_free(env2);
}

TEST_CASE("BaseTableOrView and R2RMLView defaults") {
    BaseTableOrView b("mytable");
    REQUIRE(b.tableName == "mytable");
    struct DummyConn : public SQLConnection {
        std::unique_ptr<SQLResultSet> execute(const std::string&) override { return nullptr; }
    } conn;
    auto rows_b = b.getRows(conn);
    REQUIRE(rows_b == nullptr);

    R2RMLView v("SELECT 1");
    REQUIRE(v.sqlQuery == "SELECT 1");
    auto rows_v = v.getRows(conn);
    REQUIRE(rows_v == nullptr);
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
    (void)c; (void)col; (void)tt;
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

static std::string nodeUri(const SerdNode& n) {
    return std::string(reinterpret_cast<const char*>(n.buf), n.n_bytes);
}

static TriplesMap* findById(R2RMLMapping& m, const std::string& fragment) {
    for (auto& tm : m.triplesMaps) {
        if (tm->id.find(fragment) != std::string::npos)
            return tm.get();
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// Example 1 – basic table mapping with template subject and column object
// ---------------------------------------------------------------------------

TEST_CASE("Example 1 - EMP table with template subject and column object") {
    R2RMLParser parser;
    R2RMLMapping mapping = parser.parse(SOURCE_R2RML_DIR "example1.ttl");

    REQUIRE(mapping.triplesMaps.size() == 1);

    TriplesMap* tm = findById(mapping, "TriplesMap1");
    REQUIRE(tm != nullptr);

    // Logical table: rr:tableName "EMP"
    auto* table = dynamic_cast<BaseTableOrView*>(tm->logicalTable.get());
    REQUIRE(table != nullptr);
    REQUIRE(table->tableName == "EMP");

    // Subject map carries rr:class ex:Employee
    REQUIRE(tm->subjectMap != nullptr);
    REQUIRE(tm->subjectMap->classIRIs.size() == 1);
    REQUIRE(tm->subjectMap->classIRIs[0] == "http://example.com/ns#Employee");

    // One predicate-object map: ex:name -> rr:column "ENAME"
    REQUIRE(tm->predicateObjectMaps.size() == 1);
    PredicateObjectMap& pom = *tm->predicateObjectMaps[0];
    REQUIRE(pom.predicateMaps.size() == 1);
    REQUIRE(pom.objectMaps.size() == 1);

    auto* pred = dynamic_cast<ConstantTermMap*>(pom.predicateMaps[0].get());
    REQUIRE(pred != nullptr);
    REQUIRE(nodeUri(pred->constantValue) == "http://example.com/ns#name");

    auto* obj = dynamic_cast<ColumnTermMap*>(pom.objectMaps[0].get());
    REQUIRE(obj != nullptr);
    REQUIRE(obj->columnName == "ENAME");
}

// ---------------------------------------------------------------------------
// Example 2 – SQL view with three predicate-object maps
// ---------------------------------------------------------------------------

TEST_CASE("Example 2 - department SQL view with three predicate-object maps") {
    R2RMLParser parser;
    R2RMLMapping mapping = parser.parse(SOURCE_R2RML_DIR "example2.ttl");

    REQUIRE(mapping.triplesMaps.size() == 1);

    TriplesMap* tm = findById(mapping, "TriplesMap2");
    REQUIRE(tm != nullptr);

    // Logical table: R2RMLView whose query selects DEPTNO
    auto* view = dynamic_cast<R2RMLView*>(tm->logicalTable.get());
    REQUIRE(view != nullptr);
    REQUIRE(view->sqlQuery.find("SELECT DEPTNO") != std::string::npos);

    // Subject map carries rr:class ex:Department
    REQUIRE(tm->subjectMap != nullptr);
    REQUIRE(tm->subjectMap->classIRIs.size() == 1);
    REQUIRE(tm->subjectMap->classIRIs[0] == "http://example.com/ns#Department");

    // Three predicate-object maps, each with one ColumnTermMap object
    REQUIRE(tm->predicateObjectMaps.size() == 3);

    std::vector<std::string> colNames;
    for (auto& pom : tm->predicateObjectMaps) {
        REQUIRE(pom->objectMaps.size() == 1);
        auto* col = dynamic_cast<ColumnTermMap*>(pom->objectMaps[0].get());
        REQUIRE(col != nullptr);
        colNames.push_back(col->columnName);
    }

    REQUIRE(std::find(colNames.begin(), colNames.end(), "DNAME") != colNames.end());
    REQUIRE(std::find(colNames.begin(), colNames.end(), "LOC")   != colNames.end());
    REQUIRE(std::find(colNames.begin(), colNames.end(), "STAFF") != colNames.end());
}

// ---------------------------------------------------------------------------
// Example 3 – referencing object map with join condition
// ---------------------------------------------------------------------------

TEST_CASE("Example 3 - referencing object map joining employee to department") {
    R2RMLParser parser;
    R2RMLMapping mapping = parser.parse(SOURCE_R2RML_DIR "example3.ttl");

    REQUIRE(mapping.triplesMaps.size() == 2);

    // TriplesMap2: the department view (same structure as example 2)
    TriplesMap* tm2 = findById(mapping, "TriplesMap2");
    REQUIRE(tm2 != nullptr);
    REQUIRE(dynamic_cast<R2RMLView*>(tm2->logicalTable.get()) != nullptr);

    // TriplesMap1: no logical table or subject map of its own;
    // one POM whose sole object is a ReferencingObjectMap back to TriplesMap2
    TriplesMap* tm1 = findById(mapping, "TriplesMap1");
    REQUIRE(tm1 != nullptr);
    REQUIRE(tm1->logicalTable == nullptr);
    REQUIRE(tm1->subjectMap == nullptr);

    REQUIRE(tm1->predicateObjectMaps.size() == 1);
    PredicateObjectMap& pom = *tm1->predicateObjectMaps[0];
    REQUIRE(pom.predicateMaps.size() == 1);
    REQUIRE(pom.objectMaps.size() == 1);

    auto* rom = dynamic_cast<ReferencingObjectMap*>(pom.objectMaps[0].get());
    REQUIRE(rom != nullptr);
    REQUIRE(rom->parentTriplesMap == tm2);
    REQUIRE(rom->joinConditions.size() == 1);
    REQUIRE(rom->joinConditions[0].childColumn  == "DEPTNO");
    REQUIRE(rom->joinConditions[0].parentColumn == "DEPTNO");
}

// ---------------------------------------------------------------------------
// Example 4 – template subjects and objects from a join table
// ---------------------------------------------------------------------------

TEST_CASE("Example 4 - EMP2DEPT table with template subject and template objects") {
    R2RMLParser parser;
    R2RMLMapping mapping = parser.parse(SOURCE_R2RML_DIR "example4.ttl");

    REQUIRE(mapping.triplesMaps.size() == 1);

    TriplesMap* tm = findById(mapping, "TriplesMap3");
    REQUIRE(tm != nullptr);

    // Logical table: rr:tableName "EMP2DEPT"
    auto* table = dynamic_cast<BaseTableOrView*>(tm->logicalTable.get());
    REQUIRE(table != nullptr);
    REQUIRE(table->tableName == "EMP2DEPT");

    // Two predicate-object maps, each with a ConstantTermMap predicate
    // and a TemplateTermMap object
    REQUIRE(tm->predicateObjectMaps.size() == 2);

    std::map<std::string, std::string> predToTemplate;
    for (auto& pom : tm->predicateObjectMaps) {
        REQUIRE(pom->predicateMaps.size() == 1);
        REQUIRE(pom->objectMaps.size() == 1);

        auto* pred = dynamic_cast<ConstantTermMap*>(pom->predicateMaps[0].get());
        REQUIRE(pred != nullptr);

        auto* obj = dynamic_cast<TemplateTermMap*>(pom->objectMaps[0].get());
        REQUIRE(obj != nullptr);

        predToTemplate[nodeUri(pred->constantValue)] = obj->templateString;
    }

    REQUIRE(predToTemplate["http://example.com/ns#employee"]
                == "http://data.example.com/employee/{EMPNO}");
    REQUIRE(predToTemplate["http://example.com/ns#department"]
                == "http://data.example.com/department/{DEPTNO}");
}

// ---------------------------------------------------------------------------
// Example 5 – SQL view with CASE expression mapping to a template object
// ---------------------------------------------------------------------------

TEST_CASE("Example 5 - CASE SQL view with role template object") {
    R2RMLParser parser;
    R2RMLMapping mapping = parser.parse(SOURCE_R2RML_DIR "example5.ttl");

    REQUIRE(mapping.triplesMaps.size() == 1);

    TriplesMap* tm = findById(mapping, "TriplesMap1");
    REQUIRE(tm != nullptr);

    // Logical table: R2RMLView with a CASE expression selecting ROLE
    auto* view = dynamic_cast<R2RMLView*>(tm->logicalTable.get());
    REQUIRE(view != nullptr);
    REQUIRE(view->sqlQuery.find("SELECT EMP.*") != std::string::npos);

    // Subject map has no rr:class
    REQUIRE(tm->subjectMap != nullptr);
    REQUIRE(tm->subjectMap->classIRIs.empty());

    // One predicate-object map: ex:role -> rr:template "…/roles/{ROLE}"
    REQUIRE(tm->predicateObjectMaps.size() == 1);
    PredicateObjectMap& pom = *tm->predicateObjectMaps[0];
    REQUIRE(pom.predicateMaps.size() == 1);
    REQUIRE(pom.objectMaps.size() == 1);

    auto* pred = dynamic_cast<ConstantTermMap*>(pom.predicateMaps[0].get());
    REQUIRE(pred != nullptr);
    REQUIRE(nodeUri(pred->constantValue) == "http://example.com/ns#role");

    auto* obj = dynamic_cast<TemplateTermMap*>(pom.objectMaps[0].get());
    REQUIRE(obj != nullptr);
    REQUIRE(obj->templateString == "http://data.example.com/roles/{ROLE}");
}
