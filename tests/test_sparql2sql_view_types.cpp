/**
 * Structural tests for typing an rr:sqlQuery logical table's columns.
 *
 * A view's columns appear in no information_schema, so a TypeCatalog built
 * only from one leaves them untyped - and since a predicate's candidate term
 * maps are combined with meet(), a single untyped view arm degrades the whole
 * annotation and makes DATATYPE() refuse an answer even when every other arm
 * is typed. sparql2sql::mappingViewSources() is what lets a catalog populator
 * close that gap: it names each view's SQL and the catalog key its columns
 * must be filed under (the same key the translator looks up).
 *
 * These tests assert the key convention and the folding it unlocks. Whether a
 * real backend's DESCRIBE fills the catalog correctly is settled in
 * tests/duckdb/.
 */

#include <catch2/catch_test_macros.hpp>

#include <set>
#include <string>
#include <vector>

#ifndef SOURCE_R2RML_DIR
#define SOURCE_R2RML_DIR ""
#endif

#include "r2rml/R2RMLMapping.h"
#include "r2rml/R2RMLParser.h"
#include "r2rml/TriplesMap.h"
#include "sparql-parser/Parser.h"
#include "sparql2sql/DuckDbDialect.h"
#include "sparql2sql/LogicalTableSource.h"
#include "sparql2sql/TranslationError.h"
#include "sparql2sql/Translator.h"
#include "sparql2sql/TypeCatalog.h"

using r2rml::R2RMLMapping;
using r2rml::R2RMLParser;
using sparql::Parser;
using sparql2sql::DuckDbDialect;
using sparql2sql::logicalTableIdentity;
using sparql2sql::mappingViewSources;
using sparql2sql::stripTrailingSemicolon;
using sparql2sql::translateQuery;
using sparql2sql::TranslationError;
using sparql2sql::TypeCatalog;
using sparql2sql::ViewSource;

namespace {

const char *const kPrefix = "PREFIX ex: <http://example.com/ns#>\n"
                            "PREFIX xsd: <http://www.w3.org/2001/XMLSchema#>\n";

const char *const kDeptView = "SELECT DEPTNO, DNAME FROM DEPT";
const char *const kNameLenView = "SELECT EMPNO, LENGTH(ENAME) AS NAMELEN FROM EMP";

const R2RMLMapping &viewMapping() {
	static R2RMLParser parser;
	static R2RMLMapping mapping = parser.parse(SOURCE_R2RML_DIR "sparql2sql_view_types.ttl");
	return mapping;
}

std::string translate(const std::string &queryBody, const TypeCatalog *catalog = nullptr) {
	Parser parser;
	auto q = parser.parseString(kPrefix + queryBody);
	DuckDbDialect dialect;
	return translateQuery(*q, viewMapping(), dialect, catalog);
}

bool contains(const std::string &haystack, const std::string &needle) {
	return haystack.find(needle) != std::string::npos;
}

// A catalog holding only what an information_schema sweep of the base tables
// would find: EMP is typed, the views are not.
TypeCatalog baseTablesOnlyCatalog() {
	TypeCatalog catalog;
	catalog.columnTypes["EMP"]["EMPNO"] = "INTEGER";
	catalog.columnTypes["EMP"]["ENAME"] = "VARCHAR";
	catalog.columnTypes["DEPT"]["DEPTNO"] = "INTEGER";
	catalog.columnTypes["DEPT"]["DNAME"] = "VARCHAR";
	return catalog;
}

const ViewSource *findView(const std::vector<ViewSource> &views, const std::string &sql) {
	for (const auto &v : views) {
		if (v.sql == sql) {
			return &v;
		}
	}
	return nullptr;
}

} // namespace

// --- The identity convention a populator has to agree with ---

TEST_CASE("mappingViewSources: names every rr:sqlQuery view and its catalog key", "[sparql2sql]") {
	std::vector<ViewSource> views = mappingViewSources(viewMapping());
	REQUIRE(views.size() == 2);

	const ViewSource *dept = findView(views, kDeptView);
	REQUIRE(dept != nullptr);
	// The key must be exactly what the translator asks the catalog for.
	CHECK(dept->identity == std::string("view:") + kDeptView);

	const ViewSource *nameLen = findView(views, kNameLenView);
	REQUIRE(nameLen != nullptr);
	CHECK(nameLen->identity == std::string("view:") + kNameLenView);
}

TEST_CASE("mappingViewSources: de-duplicates a view read by two triples maps", "[sparql2sql]") {
	// <#DeptLabel> and <#DeptCode> declare the same rr:sqlQuery, so describing
	// it twice would be wasted work (and two identical catalog writes).
	std::vector<ViewSource> views = mappingViewSources(viewMapping());
	std::set<std::string> identities;
	for (const auto &v : views) {
		CHECK(identities.insert(v.identity).second);
	}
	CHECK(identities.size() == 2);
}

TEST_CASE("mappingViewSources: an all-rr:tableName mapping offers nothing to describe", "[sparql2sql]") {
	// Base tables are read in one information_schema sweep, so the populator
	// must not be handed them again here.
	static R2RMLParser parser;
	R2RMLMapping baseTablesOnly = parser.parse(SOURCE_R2RML_DIR "example1.ttl");
	CHECK(mappingViewSources(baseTablesOnly).empty());
}

TEST_CASE("mappingViewSources: the identity keeps raw SQL, the describe text is stripped", "[sparql2sql]") {
	// example2.ttl's rr:sqlQuery is a multi-line block scalar ending in ";\n".
	// The identity must stay byte-identical to what the translator computes from
	// the mapping, while the SQL handed to a backend has to be embeddable - so
	// only the latter is stripped.
	static R2RMLParser parser;
	R2RMLMapping mapping = parser.parse(SOURCE_R2RML_DIR "example2.ttl");
	std::vector<ViewSource> views = mappingViewSources(mapping);
	REQUIRE(views.size() == 1);
	REQUIRE(views[0].identity.compare(0, 5, "view:") == 0);
	const std::string rawSql = views[0].identity.substr(5);
	CHECK(contains(rawSql, "FROM DEPT;"));
	CHECK(views[0].sql.compare(views[0].sql.size() - 9, 9, "FROM DEPT") == 0);
	CHECK(rawSql != views[0].sql);
	// The identity must be exactly the key the translator will look up.
	REQUIRE(mapping.triplesMaps.size() == 1);
	CHECK(logicalTableIdentity(*mapping.triplesMaps[0]->logicalTable) == views[0].identity);
}

TEST_CASE("logicalTableIdentity: a base table keys on its declared name", "[sparql2sql]") {
	const R2RMLMapping &mapping = viewMapping();
	bool sawEmp = false;
	for (const auto &tm : mapping.triplesMaps) {
		REQUIRE(tm->logicalTable);
		const std::string identity = logicalTableIdentity(*tm->logicalTable);
		REQUIRE_FALSE(identity.empty());
		if (identity == "EMP") {
			sawEmp = true;
		} else {
			// Every other source here is a view, so it must carry the prefix
			// that keeps it from ever colliding with a table name.
			CHECK(identity.compare(0, 5, "view:") == 0);
		}
	}
	CHECK(sawEmp);
}

TEST_CASE("stripTrailingSemicolon: trims one terminator and is idempotent", "[sparql2sql]") {
	CHECK(stripTrailingSemicolon("SELECT 1;") == "SELECT 1");
	CHECK(stripTrailingSemicolon("SELECT 1 ;  \n") == "SELECT 1");
	CHECK(stripTrailingSemicolon("SELECT 1") == "SELECT 1");
	CHECK(stripTrailingSemicolon(stripTrailingSemicolon("SELECT 1;")) == "SELECT 1");
	CHECK(stripTrailingSemicolon("   ").empty());
}

// --- What typing the views unlocks ---

TEST_CASE("DATATYPE over a view-backed literal has no answer without the view's types", "[sparql2sql]") {
	// Still no datatype to name - but now spelled as the type error SQL NULL,
	// which a FILTER drops, rather than a refusal to translate the query. The
	// distinction matters in practice: a query that merely *mentions* an untyped
	// arm no longer fails outright.
	const std::string query = "SELECT ?l WHERE { ?s ex:label ?l . FILTER(datatype(?l) = xsd:string) }";
	CHECK(contains(translate(query), "ELSE NULL END"));

	// The reported bug: a catalog read purely from information_schema types the
	// EMP.ENAME arm but not the view's DNAME arm. Each arm is separately
	// determined, so filter pushdown resolves them per arm - the typed arm folds
	// to its constant IRI while the untyped one yields NULL - instead of one view
	// arm poisoning the whole predicate.
	TypeCatalog baseOnly = baseTablesOnlyCatalog();
	const std::string sql = translate(query, &baseOnly);
	CHECK(contains(sql, "'http://www.w3.org/2001/XMLSchema#string'"));
	CHECK(contains(sql, "ELSE NULL END"));
}

TEST_CASE("DATATYPE over a view-backed literal folds once the view is described", "[sparql2sql]") {
	TypeCatalog catalog = baseTablesOnlyCatalog();
	// What a populator does with mappingViewSources(): file each view's
	// described result columns under the view's identity.
	for (const auto &view : mappingViewSources(viewMapping())) {
		if (view.sql == kDeptView) {
			catalog.columnTypes[view.identity]["DEPTNO"] = "INTEGER";
			catalog.columnTypes[view.identity]["DNAME"] = "VARCHAR";
		}
	}
	// Both arms are now VARCHAR, so the meet is a known xsd:string and
	// DATATYPE() folds to a constant.
	const std::string sql =
	    translate("SELECT ?l WHERE { ?s ex:label ?l . FILTER(datatype(?l) = xsd:string) }", &catalog);
	CHECK(contains(sql, "'http://www.w3.org/2001/XMLSchema#string'"));
}

TEST_CASE("DATATYPE over a computed view column folds to the described type", "[sparql2sql]") {
	// LENGTH(ENAME) exists in no base table, so this is the case a fallback to
	// information_schema could never answer: the described BIGINT is the only
	// source of the datatype.
	TypeCatalog catalog;
	catalog.columnTypes[std::string("view:") + kNameLenView]["NAMELEN"] = "BIGINT";
	const std::string sql =
	    translate("SELECT ?n WHERE { ?s ex:namelen ?n . FILTER(datatype(?n) = xsd:integer) }", &catalog);
	CHECK(contains(sql, "'http://www.w3.org/2001/XMLSchema#integer'"));

	// And the same query without the described type still has no datatype to name,
	// so the fold above really is the catalog's doing.
	CHECK(contains(translate("SELECT ?n WHERE { ?s ex:namelen ?n . FILTER(datatype(?n) = xsd:integer) }"),
	               "ELSE NULL END"));
}
