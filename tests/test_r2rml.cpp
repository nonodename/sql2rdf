#include <catch2/catch_test_macros.hpp>

#include <serd/serd.h>

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
